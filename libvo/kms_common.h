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

#ifndef KMS_COMMON_H__
#define KMS_COMMON_H__

#include "gl_common.h"

#include <EGL/egl.h>
#include <libdrm/drm.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/poll.h>

struct kms_context {
    EGLDisplay          dpy;
    EGLContext          ctx;
    EGLSurface          surf;
    EGLConfig           config;
    struct gbm_device  *gbm_dev;
    struct gbm_surface *gbm_surface;
    int                 drm_fd;
    int                 fb_width;
    int                 fb_height;
    drmModeModeInfo    *drm_mode;
    uint32_t            crtc_id;
    uint32_t            connector_id;
    drmModeCrtcPtr      orig_crtc;
    struct gbm_bo      *bo;
    drmModeRes         *resources;
    drmModeConnector   *connector;
    drmModeEncoder     *encoder;
};

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
};

int vo_kms_init(struct vo *vo);
void vo_kms_uninit(struct vo *vo);

int create_window_kms(MPGLContext *ctx, uint32_t d_width, uint32_t d_height, uint32_t flags);
int create_context_kms(MPGLContext *ctx);

void releaseGlContext_kms(MPGLContext *ctx);

void swapGlBuffers_kms(MPGLContext *ctx);

void update_xinerama_info_kms(struct vo *vo);
int vo_kms_check_events(struct vo *vo);
void vo_kms_border(struct vo *vo);
void vo_kms_fullscreen(struct vo *vo);
void vo_kms_ontop(struct vo *vo);

#endif

