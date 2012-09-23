/*
 * KMS/EGL backend for vo_gl.
 *
 * Copyright (C) 2012 Hans-Kristian Arntzen <maister@archlinux.us>
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "kms_common.h"
#include "video_out.h"
#include "aspect.h"
#include "../options.h"

/* We can only have one context alive.
 * Also, as the VO state and GL context is tied together,
 * we need something global as struct vo doesn't directly expose the MPGL context. */
static struct kms_context *kms_context;

static void drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
    struct drm_fb *fb = data;

    if (fb->fb_id)
        drmModeRmFB(kms_context->drm_fd, fb->fb_id);

    talloc_free(fb);
}

static struct drm_fb *drm_fb_get_from_bo(struct gbm_bo *bo)
{
   struct drm_fb *fb = gbm_bo_get_user_data(bo);
   if (fb)
      return fb;

   fb = talloc_zero(NULL, struct drm_fb);
   if (!fb)
       return NULL;

   fb->bo = bo;

   int width       = gbm_bo_get_width(bo);
   int height      = gbm_bo_get_height(bo);
   int stride      = gbm_bo_get_stride(bo);
   uint32_t handle = gbm_bo_get_handle(bo).u32;

   int ret = drmModeAddFB(kms_context->drm_fd,
           width, height, 24, 32, stride, handle, &fb->fb_id);

   if (ret < 0) {
       talloc_free(fb);
       return NULL;
   }

   gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);
   return fb;
}

int create_context_kms(MPGLContext *ctx)
{
    struct kms_context *kms = ctx->priv;
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        1,
        EGL_GREEN_SIZE,      1,
        EGL_BLUE_SIZE,       1,
        EGL_ALPHA_SIZE,      0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    kms->dpy = eglGetDisplay((EGLNativeDisplayType)kms->gbm_dev);
    if (!kms->dpy) {
        return -1;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        return -1;
    }

    EGLint major, minor;
    if (!eglInitialize(kms->dpy, &major, &minor)) {
        return -1;
    }

    EGLint n;
    if (!eglChooseConfig(kms->dpy, config_attribs, &kms->config, 1, &n) || n != 1) {
        return -1;
    }

    kms->ctx = eglCreateContext(kms->dpy, kms->config, EGL_NO_CONTEXT, context_attribs);
    if (!kms->ctx) {
        return -1;
    }

    kms->surf = eglCreateWindowSurface(kms->dpy, kms->config,
            (EGLNativeWindowType)kms->gbm_surface, NULL);
    if (!kms->surf) {
        return -1;
    }

    if (!eglMakeCurrent(kms->dpy, kms->surf, kms->surf, kms->ctx)) {
        return -1;
    }

    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(kms->dpy, kms->surf);

    kms->bo           = gbm_surface_lock_front_buffer(kms->gbm_surface);
    struct drm_fb *fb = drm_fb_get_from_bo(kms->bo);

    /* Sets a FB directly without any sync.
     * Starts up with a black screen rather than random garbage. */
    if (drmModeSetCrtc(kms->drm_fd, kms->crtc_id, fb->fb_id,
                0, 0, &kms->connector_id, 1, kms->drm_mode) < 0) {
        return -1;
    }

    return 0;
}

void vo_kms_uninit(struct vo *vo)
{
    (void)vo;
    struct kms_context *kms = kms_context;

    /* Restore original CRTC, or user will only see a black screen.
     * Just changing the VT will restore it anyways, however. */
    if (kms->orig_crtc) {
        drmModeSetCrtc(kms->drm_fd, kms->orig_crtc->crtc_id,
                kms->orig_crtc->buffer_id,
                kms->orig_crtc->x,
                kms->orig_crtc->y,
                &kms->connector_id, 1, &kms->orig_crtc->mode);

        drmModeFreeCrtc(kms->orig_crtc);
    }

    if (kms->gbm_surface)
        gbm_surface_destroy(kms->gbm_surface);

    if (kms->gbm_dev)
        gbm_device_destroy(kms->gbm_dev);

    if (kms->encoder)
        drmModeFreeEncoder(kms->encoder);

    if (kms->connector)
        drmModeFreeConnector(kms->connector);

    if (kms->resources)
        drmModeFreeResources(kms->resources);

    if (kms->drm_fd >= 0)
        drmClose(kms->drm_fd);

    memset(kms, 0, sizeof(*kms));
    kms->drm_fd = -1;
    kms_context = NULL;
}

void update_xinerama_info_kms(struct vo *vo)
{
    if (kms_context) {
        vo->opts->vo_screenwidth  = kms_context->fb_width;
        vo->opts->vo_screenheight = kms_context->fb_height;
    } else {
        vo->opts->vo_screenwidth  = 0;
        vo->opts->vo_screenheight = 0;
    }

    aspect_save_screenres(vo, vo->opts->vo_screenwidth, vo->opts->vo_screenheight);
}

int create_window_kms(MPGLContext *ctx, uint32_t d_width, uint32_t d_height, uint32_t flags)
{
    (void)d_width;
    (void)d_height;
    (void)flags;

    /* We can only have one context alive at a time.
     * Makes no sense to have more anyways as it takes full ownership over the display. */
    if (kms_context)
        return -1;

    kms_context = ctx->priv;
    struct kms_context *kms = ctx->priv;

    static const char *modules[] = {
        "i915", "radeon", "nouveau", "vmwgfx", "omapdrm", "exynos", NULL
    };

    for (int i = 0; modules[i]; i++) {
        kms->drm_fd = drmOpen(modules[i], NULL);
        if (kms->drm_fd >= 0)
            break;
    }

    if (kms->drm_fd < 0)
        return -1;

    kms->resources = drmModeGetResources(kms->drm_fd);
    if (!kms->resources)
        return -1;

    for (int i = 0; i < kms->resources->count_connectors; i++) {
        kms->connector = drmModeGetConnector(kms->drm_fd, kms->resources->connectors[i]);
        if (kms->connector->connection == DRM_MODE_CONNECTED)
            break;

        drmModeFreeConnector(kms->connector);
        kms->connector = NULL;
    }

    // TODO: Figure out what index for crtcs to use ...
    kms->orig_crtc = drmModeGetCrtc(kms->drm_fd, kms->resources->crtcs[0]);

    if (!kms->connector)
        return -1;

    /* Picks the mode with largest screen resolution.
     * FIXME: Be more smart about it. */
    for (int i = 0, area = 0; i < kms->connector->count_modes; i++) {
        drmModeModeInfo *current_mode = &kms->connector->modes[i];
        int current_area = current_mode->hdisplay * current_mode->vdisplay;
        if (current_area > area) {
            kms->drm_mode = current_mode;
            area          = current_area;
        }
    }

    if (!kms->drm_mode)
        return -1;

    for (int i = 0; i < kms->resources->count_encoders; i++) {
        kms->encoder = drmModeGetEncoder(kms->drm_fd, kms->resources->encoders[i]);
        if (kms->encoder->encoder_id == kms->connector->encoder_id)
            break;

        drmModeFreeEncoder(kms->encoder);
        kms->encoder = NULL;
    }

    if (!kms->encoder)
        return -1;

    kms->crtc_id      = kms->encoder->crtc_id;
    kms->connector_id = kms->connector->connector_id;

    kms->fb_width     = kms->drm_mode->hdisplay;
    kms->fb_height    = kms->drm_mode->vdisplay;

    kms->gbm_dev      = gbm_create_device(kms->drm_fd);
    kms->gbm_surface  = gbm_surface_create(kms->gbm_dev,
            kms->fb_width, kms->fb_height,
            GBM_FORMAT_XRGB8888,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

    if (!kms->gbm_surface)
        return -1;

    return 0;
}

int vo_kms_init(struct vo *vo)
{
    (void)vo;
    return 1;
}

void releaseGlContext_kms(MPGLContext *ctx)
{
    struct kms_context *kms = ctx->priv;

    if (kms->dpy) {
        if (kms->ctx) {
            eglMakeCurrent(kms->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(kms->dpy, kms->ctx);
        }

        if (kms->surf)
            eglDestroySurface(kms->dpy, kms->surf);
        eglTerminate(kms->dpy);
    }

    kms->dpy    = NULL;
    kms->ctx    = NULL;
    kms->surf   = NULL;
    kms->config = 0;
}

static void drm_page_flip_handler(int fd, unsigned frame, unsigned sec, unsigned usec, void *data)
{
    (void)fd;

    (void)frame; /* These can be used to detect exactly when the page flip really took place. */
    (void)sec;
    (void)usec;
    
    int *waiting_for_flip = data;
    *waiting_for_flip     = 0;
}

void swapGlBuffers_kms(MPGLContext *ctx)
{
    struct kms_context *kms = ctx->priv;
    /* Swapping buffers is done through page-flipping.
     * A page flipping event is sent to the kernel which will page flip in
     * an IRQ routine. When page-flipping actually takes place, an event is sent back
     * over the DRM file descriptor. */

    eglSwapBuffers(kms->dpy, kms->surf);

    /* Rendered surface is now in front buffer.
     * This surface is locked for subsequent rendering.
     * We then request the GPU to display the locked buffer next VBlank. */
    struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(kms->gbm_surface);
    struct drm_fb *fb      = drm_fb_get_from_bo(next_bo);

    int waiting_for_flip = 1;

    if (drmModePageFlip(kms->drm_fd, kms->crtc_id, fb->fb_id,
            DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip) < 0) {
        return;
    }

    /* Wait till page-flip is reported to take place.
     * We could return right away, and handle page flip next call to swapGlBuffers_kms()
     * (triple buffering), but double buffering is simple enough. */

    drmEventContext evctx = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = drm_page_flip_handler,
    };

    while (waiting_for_flip) {
        struct pollfd fds = {
            .fd     = kms->drm_fd,
            .events = POLLIN,
        };

        if (poll(&fds, 1, -1) < 0)
            break;

        if (fds.revents & (POLLHUP | POLLERR))
            break;

        if (fds.revents & POLLIN)
            drmHandleEvent(kms->drm_fd, &evctx);
        else
            break;
    }

    /* Release last locked surface, as it's no longer being displayed. */
    gbm_surface_release_buffer(kms->gbm_surface, kms->bo);
    kms->bo = next_bo;
}

/* The driver does not handle any kind of input. */
int vo_kms_check_events(struct vo *vo)
{
    if (kms_context) {
        if (vo->dwidth != kms_context->fb_width || vo->dheight != kms_context->fb_height) {
            vo->dwidth  = kms_context->fb_width;
            vo->dheight = kms_context->fb_height;
            return VO_EVENT_RESIZE;
        }
    }

    return 0;
}

/* Duplicate visible behavior from other drivers. */
void vo_kms_border(struct vo *vo)
{
    vo_border = !vo_border;
}

/* We're always fullscreen. */
void vo_kms_fullscreen(struct vo *vo)
{
   (void)vo;
}

/* We're always on top. */
void vo_kms_ontop(struct vo *vo)
{
   (void)vo;
}

