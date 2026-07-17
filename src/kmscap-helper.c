/*
 * obs-kmscap - Zero-copy KMS/DRM screen capture plugin for OBS Studio
 * Copyright (C) 2026 Robert
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "kms-ipc.h"
#include "kms-utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static int g_drm_fd = -1;

static void handle_request(int ipc_fd, const struct kms_req *req)
{
	struct kms_resp resp = { .magic = KMS_IPC_MAGIC, .status = 0 };

	switch (req->type) {
	case KMS_REQ_OPEN:
		if (g_drm_fd >= 0) {
			kms_close_device(g_drm_fd);
			g_drm_fd = -1;
		}
		g_drm_fd = kms_open_device(req->device_path);
		resp.status = (g_drm_fd >= 0) ? 0 : -1;
		ipc_send_resp(ipc_fd, &resp, NULL, 0);
		break;

	case KMS_REQ_ENUM_CRTCS:
		if (g_drm_fd < 0) {
			resp.status = -1;
		} else {
			int count = kms_enumerate_crtcs(g_drm_fd, resp.enum_crtcs.crtcs, KMS_MAX_CRTCS);
			if (count < 0) {
				resp.status = -1;
			} else {
				resp.enum_crtcs.count = count;
			}
		}
		ipc_send_resp(ipc_fd, &resp, NULL, 0);
		break;

	case KMS_REQ_GET_CURRENT_FB_ID:
		if (g_drm_fd < 0) {
			resp.status = -1;
		} else {
			uint32_t primary_plane = kms_find_primary_plane(g_drm_fd, req->crtc_id);
			uint32_t fb_id = 0;
			if (primary_plane && kms_get_plane_fb(g_drm_fd, primary_plane, &fb_id, NULL, NULL) && fb_id != 0) {
				resp.fb_id = fb_id;
			} else {
				drmModeCrtcPtr crtc = drmModeGetCrtc(g_drm_fd, req->crtc_id);
				if (crtc) {
					resp.fb_id = crtc->buffer_id;
					drmModeFreeCrtc(crtc);
				} else {
					resp.fb_id = 0;
					resp.status = -1;
				}
			}
		}
		ipc_send_resp(ipc_fd, &resp, NULL, 0);
		break;

	case KMS_REQ_FIND_CURSOR:
		if (g_drm_fd < 0) {
			resp.status = -1;
		} else {
			resp.plane_id = kms_find_cursor_plane(g_drm_fd, req->crtc_id);
		}
		ipc_send_resp(ipc_fd, &resp, NULL, 0);
		break;

	case KMS_REQ_GET_PLANE_FB:
		if (g_drm_fd < 0) {
			resp.status = -1;
		} else {
			uint32_t fb_id = 0;
			int x = 0, y = 0;
			if (kms_get_plane_fb(g_drm_fd, req->plane_id, &fb_id, &x, &y)) {
				resp.plane_fb.fb_id = fb_id;
				resp.plane_fb.x = x;
				resp.plane_fb.y = y;
			} else {
				resp.status = -1;
			}
		}
		ipc_send_resp(ipc_fd, &resp, NULL, 0);
		break;

	case KMS_REQ_GET_FB_INFO_AND_FDS:
		if (g_drm_fd < 0) {
			resp.status = -1;
			ipc_send_resp(ipc_fd, &resp, NULL, 0);
		} else {
			kms_fb_t fb;
			if (kms_get_fb2(g_drm_fd, req->fb_id, &fb)) {
				if (kms_export_dmabuf_fds(g_drm_fd, &fb)) {
					resp.fb_info = fb;
					/* Collect valid FDs to send */
					int fds_to_send[KMS_MAX_PLANES];
					int num_fds = fb.num_planes;
					for (uint32_t i = 0; i < fb.num_planes; i++) {
						fds_to_send[i] = fb.fds[i];
					}
					ipc_send_resp(ipc_fd, &resp, fds_to_send, num_fds);
					
					/* We must close our local FDs since we sent them */
					kms_release_fb_fds(&fb);
				} else {
					kms_release_fb_fds(&fb);
					resp.status = -1;
					ipc_send_resp(ipc_fd, &resp, NULL, 0);
				}
			} else {
				resp.status = -1;
				ipc_send_resp(ipc_fd, &resp, NULL, 0);
			}
		}
		break;

	case KMS_REQ_QUIT:
		/* We just exit naturally */
		exit(0);
		break;

	default:
		resp.status = -1;
		ipc_send_resp(ipc_fd, &resp, NULL, 0);
		break;
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <ipc_fd>\n", argv[0]);
		return 1;
	}

	int ipc_fd = atoi(argv[1]);
	if (ipc_fd < 0) {
		fprintf(stderr, "Invalid ipc_fd\n");
		return 1;
	}

	/* Ignore SIGPIPE so we don't crash if the plugin dies */
	signal(SIGPIPE, SIG_IGN);

	struct kms_req req;
	while (ipc_recv_req(ipc_fd, &req)) {
		handle_request(ipc_fd, &req);
	}

	if (g_drm_fd >= 0) {
		kms_close_device(g_drm_fd);
	}
	close(ipc_fd);
	return 0;
}
