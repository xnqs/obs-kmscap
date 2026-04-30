/*
 * obs-kmscap - Zero-copy KMS/DRM screen capture plugin for OBS Studio
 * Copyright (C) 2026 Robert
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "kms-utils.h"

#include <sys/types.h>

#define KMS_IPC_MAGIC 0x4B4D5343 /* "KMSC" */

enum kms_req_type {
	KMS_REQ_OPEN,
	KMS_REQ_ENUM_CRTCS,
	KMS_REQ_FIND_CURSOR,
	KMS_REQ_GET_PLANE_FB,
	KMS_REQ_GET_FB_INFO_AND_FDS,
	KMS_REQ_GET_CURRENT_FB_ID,
	KMS_REQ_QUIT
};

struct kms_req {
	uint32_t magic;
	uint32_t type;
	union {
		char device_path[64];
		uint32_t crtc_id;
		uint32_t plane_id;
		uint32_t fb_id;
	};
};

struct kms_resp {
	uint32_t magic;
	int32_t status; /* 0 = ok, <0 = error */
	union {
		struct {
			int count;
			kms_crtc_info_t crtcs[KMS_MAX_CRTCS];
		} enum_crtcs;
		uint32_t fb_id;
		uint32_t plane_id;
		struct {
			uint32_t fb_id;
			int x;
			int y;
		} plane_fb;
		kms_fb_t fb_info; /* DMA-BUF FDs will be passed via SCM_RIGHTS */
	};
};

/* Client-side (Plugin) API */

/**
 * Spawns the helper daemon and returns the IPC socket file descriptor.
 * Updates helper_pid with the child process ID.
 */
int ipc_kms_open_device(const char *path, pid_t *helper_pid);

/**
 * Sends KMS_REQ_QUIT and reaps the child process.
 */
void ipc_kms_close_device(int ipc_fd, pid_t helper_pid);

int ipc_kms_enumerate_crtcs(int ipc_fd, kms_crtc_info_t *out, int max_out);
uint32_t ipc_kms_get_current_fb_id(int ipc_fd, uint32_t crtc_id);
uint32_t ipc_kms_find_cursor_plane(int ipc_fd, uint32_t crtc_id);
bool ipc_kms_get_plane_fb(int ipc_fd, uint32_t plane_id, uint32_t *fb_id, int *x, int *y);
bool ipc_kms_get_fb2_and_fds(int ipc_fd, uint32_t fb_id, kms_fb_t *out);

/* Internal IPC messaging utilities used by both sides */

bool ipc_send_req(int ipc_fd, const struct kms_req *req);
bool ipc_recv_resp(int ipc_fd, struct kms_resp *resp, int *fds, int max_fds);

bool ipc_recv_req(int ipc_fd, struct kms_req *req);
bool ipc_send_resp(int ipc_fd, const struct kms_resp *resp, const int *fds, int num_fds);
