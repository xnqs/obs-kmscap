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

#include "kms-utils.h"

#ifdef KMSCAP_HELPER_DAEMON
#include <stdio.h>
#define LOG_ERROR 1
#define LOG_WARNING 2
#define LOG_INFO 3
#define LOG_DEBUG 4
#define blog(level, format, ...) fprintf(stderr, format "\n", ##__VA_ARGS__)
#else
#include <obs-module.h>
#include <graphics/graphics.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>

/* ---------------------------------------------------------------------- */
/* Device open/close                                                        */
/* ---------------------------------------------------------------------- */

int kms_open_device(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		blog(LOG_WARNING, "[kmscap] Cannot open DRI device %s: %s",
		     path, strerror(errno));
		return -1;
	}

	/* Allow driver to expose all planes, not just the "primary" ones. */
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
		blog(LOG_WARNING,
		     "[kmscap] Cannot set DRM_CLIENT_CAP_UNIVERSAL_PLANES on %s: %s",
		     path, strerror(errno));
		/* Non-fatal — we may still get primary plane FBs. */
	}

	/* Opt-in to atomic properties to ensure CRTC_X and CRTC_Y map accurately */
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		blog(LOG_DEBUG, "[kmscap] DRM_CLIENT_CAP_ATOMIC not supported on %s", path);
	}

	return fd;
}

void kms_close_device(int fd)
{
	if (fd >= 0)
		close(fd);
}

/* ---------------------------------------------------------------------- */
/* CRTC / connector enumeration                                             */
/* ---------------------------------------------------------------------- */

int kms_enumerate_crtcs(int fd, kms_crtc_info_t *out, int max_out)
{
	if (fd < 0 || !out || max_out <= 0)
		return -1;

	drmModeResPtr res = drmModeGetResources(fd);
	if (!res) {
		blog(LOG_WARNING, "[kmscap] drmModeGetResources failed: %s",
		     strerror(errno));
		return -1;
	}

	int found = 0;

	for (int ci = 0; ci < res->count_connectors && found < max_out; ci++) {
		drmModeConnectorPtr conn =
			drmModeGetConnector(fd, res->connectors[ci]);
		if (!conn)
			continue;

		/* Skip disconnected or off connectors. */
		if (conn->connection != DRM_MODE_CONNECTED ||
		    conn->count_modes == 0 || conn->encoder_id == 0) {
			drmModeFreeConnector(conn);
			continue;
		}

		drmModeEncoderPtr enc =
			drmModeGetEncoder(fd, conn->encoder_id);
		if (!enc) {
			drmModeFreeConnector(conn);
			continue;
		}

		if (enc->crtc_id == 0) {
			drmModeFreeEncoder(enc);
			drmModeFreeConnector(conn);
			continue;
		}

		drmModeCrtcPtr crtc = drmModeGetCrtc(fd, enc->crtc_id);
		if (!crtc) {
			drmModeFreeEncoder(enc);
			drmModeFreeConnector(conn);
			continue;
		}

		kms_crtc_info_t *info = &out[found];
		memset(info, 0, sizeof(*info));
		info->crtc_id      = enc->crtc_id;
		info->connector_id = res->connectors[ci];
		info->fb_id        = crtc->buffer_id;
		info->x            = crtc->x;
		info->y            = crtc->y;

		if (crtc->mode_valid) {
			info->width  = crtc->mode.hdisplay;
			info->height = crtc->mode.vdisplay;
		}

		/* Build human-readable name: connector name + resolution. */
		const char *type_name = drmModeGetConnectorTypeName(
			conn->connector_type);
		snprintf(info->name, sizeof(info->name),
			 "%s-%u %dx%d",
			 type_name ? type_name : "Unknown",
			 conn->connector_type_id,
			 info->width, info->height);

		found++;

		drmModeFreeCrtc(crtc);
		drmModeFreeEncoder(enc);
		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);

	blog(LOG_INFO, "[kmscap] Found %d active CRTC(s)", found);
	return found;
}

/* ---------------------------------------------------------------------- */
/* DRM Plane helpers                                                        */
/* ---------------------------------------------------------------------- */

uint32_t kms_find_cursor_plane(int fd, uint32_t crtc_id)
{
	drmModeResPtr res = drmModeGetResources(fd);
	if (!res) return 0;
	
	int crtc_index = -1;
	for (int i = 0; i < res->count_crtcs; i++) {
		if (res->crtcs[i] == crtc_id) {
			crtc_index = i;
			break;
		}
	}
	drmModeFreeResources(res);
	if (crtc_index < 0) return 0;

	drmModePlaneResPtr p_res = drmModeGetPlaneResources(fd);
	if (!p_res) return 0;

	uint32_t cursor_plane = 0;

	for (uint32_t i = 0; i < p_res->count_planes; i++) {
		drmModePlanePtr p = drmModeGetPlane(fd, p_res->planes[i]);
		if (!p) continue;

		if (p->possible_crtcs & (1 << crtc_index)) {
			/* Check if it's a cursor plane */
			drmModeObjectPropertiesPtr props = 
				drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
			if (props) {
				for (uint32_t j = 0; j < props->count_props; j++) {
					drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
					if (prop) {
						if (strcmp(prop->name, "type") == 0) {
							if (props->prop_values[j] == DRM_PLANE_TYPE_CURSOR) {
								cursor_plane = p->plane_id;
							}
						}
						drmModeFreeProperty(prop);
					}
					if (cursor_plane) break;
				}
				drmModeFreeObjectProperties(props);
			}
		}
		drmModeFreePlane(p);
		if (cursor_plane) break;
	}

	drmModeFreePlaneResources(p_res);
	return cursor_plane;
}

bool kms_get_plane_fb(int fd, uint32_t plane_id, uint32_t *fb_id, int *x, int *y) {
	if (fd < 0 || plane_id == 0 || !fb_id) return false;
	
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
	bool found_x = false, found_y = false, found_fb = false;
	
	if (props) {
		for (uint32_t i = 0; i < props->count_props; i++) {
			drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
			if (!prop) continue;
			
			if (strcmp(prop->name, "CRTC_X") == 0) {
				if (x) *x = (int)(int64_t)props->prop_values[i];
				found_x = true;
			} else if (strcmp(prop->name, "CRTC_Y") == 0) {
				if (y) *y = (int)(int64_t)props->prop_values[i];
				found_y = true;
			} else if (strcmp(prop->name, "FB_ID") == 0) {
				*fb_id = (uint32_t)props->prop_values[i];
				found_fb = true;
			}
			
			drmModeFreeProperty(prop);
			if (found_x && found_y && found_fb) break;
		}
		drmModeFreeObjectProperties(props);
	}
	
	/* Fallback if atomic properties aren't available or we missed one */
	drmModePlanePtr p = drmModeGetPlane(fd, plane_id);
	if (p) {
		if (!found_fb) *fb_id = p->fb_id;
		if (!found_x && x) *x = p->crtc_x;
		if (!found_y && y) *y = p->crtc_y;
		drmModeFreePlane(p);
	} else if (!found_fb) {
		return false;
	}

	return true;
}

/* ---------------------------------------------------------------------- */
/* Framebuffer query (drmModeGetFB2)                                        */
/* ---------------------------------------------------------------------- */

bool kms_get_fb2(int fd, uint32_t fb_id, kms_fb_t *out)
{
	if (fd < 0 || fb_id == 0 || !out)
		return false;

	memset(out, 0, sizeof(*out));
	for (int i = 0; i < KMS_MAX_PLANES; i++)
		out->fds[i] = -1;

	drmModeFB2Ptr fb = drmModeGetFB2(fd, fb_id);
	if (!fb) {
		/* Fallback for older drivers / kernels that don't support GETFB2 */
		drmModeFBPtr old_fb = drmModeGetFB(fd, fb_id);
		if (!old_fb) {
			blog(LOG_WARNING,
			     "[kmscap] drmModeGetFB2 and GetFB(fb_id=%u) failed: %s",
			     fb_id, strerror(errno));
			return false;
		}

		out->fb_id   = fb_id;
		out->width   = old_fb->width;
		out->height  = old_fb->height;
		out->flags   = 0;

		/* Infer a basic fourcc since legacy GETFB only returns depth/bpp */
		if (old_fb->depth == 24 && old_fb->bpp == 32)
			out->fourcc = DRM_FORMAT_XRGB8888;
		else if (old_fb->depth == 32 && old_fb->bpp == 32)
			out->fourcc = DRM_FORMAT_ARGB8888;
		else if (old_fb->depth == 16 && old_fb->bpp == 16)
			out->fourcc = DRM_FORMAT_RGB565;
		else
			out->fourcc = DRM_FORMAT_XRGB8888; /* fallback */

		out->num_planes = 1;
		out->handles[0]   = old_fb->handle;
		out->pitches[0]   = old_fb->pitch;
		out->offsets[0]   = 0;
		out->modifiers[0] = DRM_FORMAT_MOD_INVALID;

		drmModeFreeFB(old_fb);
		return true;
	}

	out->fb_id   = fb_id;
	out->width   = fb->width;
	out->height  = fb->height;
	out->fourcc  = fb->pixel_format;
	out->flags   = fb->flags;

	/* Count non-zero plane handles and copy per-plane data. */
	out->num_planes = 0;
	for (int i = 0; i < KMS_MAX_PLANES; i++) {
		if (!fb->handles[i])
			break;
		out->handles[i]   = fb->handles[i];
		out->pitches[i]   = fb->pitches[i];
		out->offsets[i]   = fb->offsets[i];
		out->modifiers[i] = fb->modifier;
		out->num_planes++;
	}

	if (out->num_planes == 0) {
		blog(LOG_ERROR, "[kmscap] drmModeGetFB2 returned success but handles[0] is 0! Missing CAP_SYS_ADMIN or DRM_MASTER?");
	}

	drmModeFreeFB2(fb);
	return out->num_planes > 0;
}

/* ---------------------------------------------------------------------- */
/* DMA-BUF export                                                           */
/* ---------------------------------------------------------------------- */

bool kms_export_dmabuf_fds(int fd, kms_fb_t *fb)
{
	if (fd < 0 || !fb || fb->num_planes == 0)
		return false;

	bool any_ok = false;

	for (uint32_t i = 0; i < fb->num_planes; i++) {
		/* Deduplicate: same handle may be shared across planes
		 * (e.g. semi-planar NV12 stored in a single allocation). */
		bool already_exported = false;
		for (uint32_t j = 0; j < i; j++) {
			if (fb->handles[j] == fb->handles[i]) {
				fb->fds[i] = fb->fds[j];
				already_exported = true;
				break;
			}
		}
		if (already_exported) {
			any_ok = true;
			continue;
		}

		int dmabuf_fd = -1;
		int ret = drmPrimeHandleToFD(fd, fb->handles[i],
					     DRM_CLOEXEC | DRM_RDWR,
					     &dmabuf_fd);
		if (ret != 0 || dmabuf_fd < 0) {
			blog(LOG_ERROR,
			     "[kmscap] drmPrimeHandleToFD plane %u failed: %s",
			     i, strerror(errno));
			fb->fds[i] = -1;
		} else {
			fb->fds[i] = dmabuf_fd;
			any_ok = true;
		}
	}

	return any_ok;
}

void kms_release_fb_fds(kms_fb_t *fb)
{
	if (!fb)
		return;

	/* Track which fds we've already closed (shared-handle dedup). */
	int closed[KMS_MAX_PLANES];
	int n_closed = 0;

	for (int i = 0; i < KMS_MAX_PLANES; i++) {
		if (fb->fds[i] < 0)
			continue;

		bool already = false;
		for (int j = 0; j < n_closed; j++) {
			if (closed[j] == fb->fds[i]) {
				already = true;
				break;
			}
		}

		if (!already) {
			closed[n_closed++] = fb->fds[i];
			close(fb->fds[i]);
		}

		fb->fds[i] = -1;
	}
}

/* ---------------------------------------------------------------------- */
/* Format mapping                                                            */
/* ---------------------------------------------------------------------- */

#ifndef KMSCAP_HELPER_DAEMON
int kms_fourcc_to_gs_format(uint32_t fourcc)
{
	/*
	 * DRM uses little-endian component ordering in the fourcc name,
	 * but stores pixels LSB-first in memory, which matches OBS's
	 * GS_BGRA / GS_BGRX convention on little-endian (x86) systems.
	 */
	switch (fourcc) {
	case DRM_FORMAT_ARGB8888:
		return GS_BGRA;
	case DRM_FORMAT_XRGB8888:
		return GS_BGRX;
	case DRM_FORMAT_ABGR8888:
		return GS_RGBA;
	case DRM_FORMAT_XBGR8888:
		return GS_RGBA; /* closest available */
	default:
		/* RGB565 and other formats not in gs_color_format — pass the
		 * DRM fourcc directly to EGL and use GS_BGRA as a hint. */
		blog(LOG_WARNING,
		     "[kmscap] Unrecognised fourcc %c%c%c%c (0x%08x), "
		     "falling back to GS_BGRA",
		     (fourcc >> 0) & 0xFF, (fourcc >> 8) & 0xFF,
		     (fourcc >> 16) & 0xFF, (fourcc >> 24) & 0xFF, fourcc);
		return GS_BGRA;
	}
}
#endif
