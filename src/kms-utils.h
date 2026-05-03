/*
 * obs-kmscap - Zero-copy KMS/DRM screen capture plugin for OBS Studio
 * Copyright (C) 2026 Robert
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

#ifndef DRM_MODE_FB_MODIFIERS
#define DRM_MODE_FB_MODIFIERS 2
#endif

/* Maximum number of planes in a framebuffer (DRM_FORMAT_MAX_PLANES = 4) */
#define KMS_MAX_PLANES 4

/* Maximum number of CRTCs / outputs to enumerate */
#define KMS_MAX_CRTCS 16

/**
 * Parsed information from drmModeGetFB2, ready for EGL import.
 */
typedef struct {
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint32_t fourcc; /* DRM_FORMAT_* */
	uint32_t flags;
	uint32_t num_planes;

	/* Per-plane data */
	uint32_t handles[KMS_MAX_PLANES];
	uint32_t pitches[KMS_MAX_PLANES];  /* byte stride */
	uint32_t offsets[KMS_MAX_PLANES];
	uint64_t modifiers[KMS_MAX_PLANES];

	/* DMA-BUF file descriptors, one per plane (or -1 if not exported) */
	int fds[KMS_MAX_PLANES];
} kms_fb_t;

/**
 * Info about a single CRTC/connector pair (one logical display output).
 */
typedef struct {
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t fb_id; /* current scanout framebuffer (0 if idle) */
	int      x, y;
	int      width, height; /* mode resolution */
	char     name[64]; /* e.g. "HDMI-A-1 1920x1080" */
} kms_crtc_info_t;

/**
 * Open a DRI render or primary node (e.g. /dev/dri/card0).
 *
 * @param  path  Path to the DRI device.
 * @return       File descriptor on success, -1 on failure.
 */
int kms_open_device(const char *path);

/**
 * Close a DRI device fd.
 */
void kms_close_device(int fd);

/**
 * Enumerate active CRTCs (those with a connected display and active mode).
 *
 * @param  fd       Open DRM device fd.
 * @param  out      Array to write kms_crtc_info_t entries to.
 * @param  max_out  Size of out array.
 * @return          Number of active CRTCs found, or -1 on error.
 */
int kms_enumerate_crtcs(int fd, kms_crtc_info_t *out, int max_out);

/**
 * Find the Plane ID of the cursor plane associated with this CRTC.
 * 
 * @param fd      Open DRM device fd.
 * @param crtc_id Target CRTC ID to map to.
 * @return        Plane ID of the cursor plane, or 0 if not found.
 */
uint32_t kms_find_cursor_plane(int fd, uint32_t crtc_id);

/**
 * Retrieve current framebuffer ID and position of a specific plane.
 * 
 * @param fd       Open DRM device fd.
 * @param plane_id Target Plane ID.
 * @param fb_id    Pointer to store the returned framebuffer ID.
 * @param x        Pointer to store the CRTC X offset.
 * @param y        Pointer to store the CRTC Y offset.
 * @return         true if successfully retrieved.
 */
bool kms_get_plane_fb(int fd, uint32_t plane_id, uint32_t *fb_id, int *x, int *y);

/**
 * Retrieve framebuffer metadata using drmModeGetFB2.
 * Requires kernel >= 5.7.
 *
 * @param  fd    Open DRM device fd.
 * @param  fb_id Framebuffer ID to query.
 * @param  out   Output struct; fds[] are all initialised to -1.
 * @return       true on success.
 */
bool kms_get_fb2(int fd, uint32_t fb_id, kms_fb_t *out);

/**
 * Export DMA-BUF file descriptors for all planes of a framebuffer.
 * Fills in fb->fds[]. The caller must close them when done.
 *
 * @param  fd  Open DRM device fd.
 * @param  fb  Framebuffer info (must have been populated via kms_get_fb2).
 * @return     true if at least one plane was exported successfully.
 */
bool kms_export_dmabuf_fds(int fd, kms_fb_t *fb);

/**
 * Release DMA-BUF fds held in a kms_fb_t (closes fds, sets them to -1).
 */
void kms_release_fb_fds(kms_fb_t *fb);

/**
 * Map a DRM fourcc format code to the corresponding OBS gs_color_format.
 * Returns GS_UNKNOWN if the format is not supported.
 *
 * Supported:
 *   DRM_FORMAT_XRGB8888  -> GS_BGRX
 *   DRM_FORMAT_ARGB8888  -> GS_BGRA
 *   DRM_FORMAT_XBGR8888  -> GS_RGBX (if available)
 *   DRM_FORMAT_ABGR8888  -> GS_RGBA
 *   DRM_FORMAT_RGB565    -> GS_R5G6B5
 */
int kms_fourcc_to_gs_format(uint32_t fourcc);
