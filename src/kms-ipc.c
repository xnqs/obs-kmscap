/*
 * obs-kmscap - Zero-copy KMS/DRM screen capture plugin for OBS Studio
 * Copyright (C) 2026 Robert
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE
#include "kms-ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef KMSCAP_HELPER_DAEMON
#include <stdio.h>
#define LOG_ERROR 1
#define LOG_WARNING 2
#define LOG_INFO 3
#define LOG_DEBUG 4
#define blog(level, format, ...) fprintf(stderr, format "\n", ##__VA_ARGS__)
#else
#include <obs-module.h>
#endif

#ifndef CMAKE_INSTALL_PREFIX
#define CMAKE_INSTALL_PREFIX "/usr/local"
#endif

/* ---------------------------------------------------------------------- */
/* Helper Process Management                                                */
/* ---------------------------------------------------------------------- */

int ipc_kms_open_device(const char *path, pid_t *helper_pid)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		blog(LOG_ERROR, "[kmscap] socketpair failed: %s", strerror(errno));
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		blog(LOG_ERROR, "[kmscap] fork failed: %s", strerror(errno));
		close(sv[0]);
		close(sv[1]);
		return -1;
	}

	if (pid == 0) {
		/* Child process (Helper) */
		close(sv[0]);
		
		/* Remove CLOEXEC from sv[1] so the exec'd process can use it */
		int flags = fcntl(sv[1], F_GETFD);
		if (flags >= 0) {
			fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);
		}

		char fd_str[16];
		snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);

		/* We check several locations for the helper binary */
		const char *paths[] = {
			CMAKE_INSTALL_PREFIX "/bin/obs-kmscap-helper",
			"/usr/local/bin/obs-kmscap-helper",
			"/usr/bin/obs-kmscap-helper",
			"./obs-kmscap-helper",
			NULL
		};

		for (int i = 0; paths[i] != NULL; i++) {
			execl(paths[i], "obs-kmscap-helper", fd_str, NULL);
		}

		/* If we get here, execl failed for all paths */
		fprintf(stderr, "[kmscap-helper] Failed to execute helper daemon: %s\n", strerror(errno));
		exit(1);
	}

	/* Parent process (Plugin) */
	close(sv[1]);
	*helper_pid = pid;

	/* Send the OPEN command */
	struct kms_req req = { .magic = KMS_IPC_MAGIC, .type = KMS_REQ_OPEN };
	strncpy(req.device_path, path, sizeof(req.device_path) - 1);
	req.device_path[sizeof(req.device_path) - 1] = '\0';

	if (!ipc_send_req(sv[0], &req)) {
		ipc_kms_close_device(sv[0], pid);
		return -1;
	}

	struct kms_resp resp;
	if (!ipc_recv_resp(sv[0], &resp, NULL, 0) || resp.status < 0) {
		blog(LOG_ERROR, "[kmscap] Helper failed to open device %s", path);
		ipc_kms_close_device(sv[0], pid);
		return -1;
	}

	return sv[0];
}

void ipc_kms_close_device(int ipc_fd, pid_t helper_pid)
{
	if (ipc_fd >= 0) {
		struct kms_req req = { .magic = KMS_IPC_MAGIC, .type = KMS_REQ_QUIT };
		ipc_send_req(ipc_fd, &req);
		close(ipc_fd);
	}

	if (helper_pid > 0) {
		waitpid(helper_pid, NULL, 0);
	}
}

/* ---------------------------------------------------------------------- */
/* Client-side wrappers                                                     */
/* ---------------------------------------------------------------------- */

int ipc_kms_enumerate_crtcs(int ipc_fd, kms_crtc_info_t *out, int max_out)
{
	if (ipc_fd < 0 || !out) return -1;

	struct kms_req req = { .magic = KMS_IPC_MAGIC, .type = KMS_REQ_ENUM_CRTCS };
	if (!ipc_send_req(ipc_fd, &req)) return -1;

	struct kms_resp resp;
	if (!ipc_recv_resp(ipc_fd, &resp, NULL, 0) || resp.status < 0) return -1;

	int count = resp.enum_crtcs.count;
	if (count > max_out) count = max_out;

	memcpy(out, resp.enum_crtcs.crtcs, count * sizeof(kms_crtc_info_t));
	return count;
}

uint32_t ipc_kms_get_current_fb_id(int ipc_fd, uint32_t crtc_id)
{
	if (ipc_fd < 0) return 0;

	struct kms_req req = { .magic = KMS_IPC_MAGIC, .type = KMS_REQ_GET_CURRENT_FB_ID, .crtc_id = crtc_id };
	if (!ipc_send_req(ipc_fd, &req)) return 0;

	struct kms_resp resp;
	if (!ipc_recv_resp(ipc_fd, &resp, NULL, 0) || resp.status < 0) return 0;

	return resp.fb_id;
}

uint32_t ipc_kms_find_cursor_plane(int ipc_fd, uint32_t crtc_id)
{
	if (ipc_fd < 0) return 0;

	struct kms_req req = { .magic = KMS_IPC_MAGIC, .type = KMS_REQ_FIND_CURSOR, .crtc_id = crtc_id };
	if (!ipc_send_req(ipc_fd, &req)) return 0;

	struct kms_resp resp;
	if (!ipc_recv_resp(ipc_fd, &resp, NULL, 0) || resp.status < 0) return 0;

	return resp.plane_id;
}

bool ipc_kms_get_plane_fb(int ipc_fd, uint32_t plane_id, uint32_t *fb_id, int *x, int *y)
{
	if (ipc_fd < 0) return false;

	struct kms_req req = { .magic = KMS_IPC_MAGIC, .type = KMS_REQ_GET_PLANE_FB, .plane_id = plane_id };
	if (!ipc_send_req(ipc_fd, &req)) return false;

	struct kms_resp resp;
	if (!ipc_recv_resp(ipc_fd, &resp, NULL, 0) || resp.status < 0) return false;

	if (fb_id) *fb_id = resp.plane_fb.fb_id;
	if (x) *x = resp.plane_fb.x;
	if (y) *y = resp.plane_fb.y;
	return true;
}

bool ipc_kms_get_fb2_and_fds(int ipc_fd, uint32_t fb_id, kms_fb_t *out)
{
	if (ipc_fd < 0 || !out) return false;

	struct kms_req req = { .magic = KMS_IPC_MAGIC, .type = KMS_REQ_GET_FB_INFO_AND_FDS, .fb_id = fb_id };
	if (!ipc_send_req(ipc_fd, &req)) return false;

	struct kms_resp resp;
	int fds[KMS_MAX_PLANES];
	for (int i = 0; i < KMS_MAX_PLANES; i++) fds[i] = -1;

	if (!ipc_recv_resp(ipc_fd, &resp, fds, KMS_MAX_PLANES)) return false;
	if (resp.status < 0) return false;

	*out = resp.fb_info;
	/* The fds are received via SCM_RIGHTS, copy them into the struct */
	for (uint32_t i = 0; i < KMS_MAX_PLANES; i++) {
		out->fds[i] = fds[i];
	}

	return true;
}

/* ---------------------------------------------------------------------- */
/* Low-level socket I/O                                                     */
/* ---------------------------------------------------------------------- */

bool ipc_send_req(int ipc_fd, const struct kms_req *req)
{
	ssize_t ret;
	do {
		ret = send(ipc_fd, req, sizeof(*req), MSG_NOSIGNAL);
	} while (ret < 0 && errno == EINTR);
	return ret == sizeof(*req);
}

bool ipc_recv_req(int ipc_fd, struct kms_req *req)
{
	ssize_t ret;
	do {
		ret = recv(ipc_fd, req, sizeof(*req), 0);
	} while (ret < 0 && errno == EINTR);
	return (ret == sizeof(*req) && req->magic == KMS_IPC_MAGIC);
}

bool ipc_send_resp(int ipc_fd, const struct kms_resp *resp, const int *fds, int num_fds)
{
	struct msghdr msg = {0};
	struct iovec iov[1];

	iov[0].iov_base = (void *)resp;
	iov[0].iov_len  = sizeof(*resp);
	msg.msg_iov    = iov;
	msg.msg_iovlen = 1;

	union {
		struct cmsghdr cmsghdr;
		char control[CMSG_SPACE(KMS_MAX_PLANES * sizeof(int))];
	} cmsgu;

	if (fds && num_fds > 0) {
		msg.msg_control = cmsgu.control;
		msg.msg_controllen = CMSG_SPACE(num_fds * sizeof(int));

		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type  = SCM_RIGHTS;
		cmsg->cmsg_len   = CMSG_LEN(num_fds * sizeof(int));

		memcpy(CMSG_DATA(cmsg), fds, num_fds * sizeof(int));
	}

	ssize_t ret;
	do {
		ret = sendmsg(ipc_fd, &msg, MSG_NOSIGNAL);
	} while (ret < 0 && errno == EINTR);

	return ret == sizeof(*resp);
}

bool ipc_recv_resp(int ipc_fd, struct kms_resp *resp, int *fds, int max_fds)
{
	struct msghdr msg = {0};
	struct iovec iov[1];

	iov[0].iov_base = resp;
	iov[0].iov_len  = sizeof(*resp);
	msg.msg_iov    = iov;
	msg.msg_iovlen = 1;

	union {
		struct cmsghdr cmsghdr;
		char control[CMSG_SPACE(KMS_MAX_PLANES * sizeof(int))];
	} cmsgu;

	msg.msg_control = cmsgu.control;
	msg.msg_controllen = sizeof(cmsgu.control);

	ssize_t ret;
	do {
		ret = recvmsg(ipc_fd, &msg, 0);
	} while (ret < 0 && errno == EINTR);

	if (ret != sizeof(*resp) || resp->magic != KMS_IPC_MAGIC)
		return false;

	/* Extract FDs if requested */
	if (fds && max_fds > 0) {
		for (int i = 0; i < max_fds; i++) fds[i] = -1;

		struct cmsghdr *cmsg;
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
				int fd_count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
				if (fd_count > max_fds) fd_count = max_fds;
				memcpy(fds, CMSG_DATA(cmsg), fd_count * sizeof(int));
				break;
			}
		}
	} else if (msg.msg_controllen > 0) {
		/* We got FDs but didn't ask for them; close them to prevent leak */
		struct cmsghdr *cmsg;
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
				int fd_count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
				int *rcv_fds = (int *)CMSG_DATA(cmsg);
				for (int i = 0; i < fd_count; i++) close(rcv_fds[i]);
			}
		}
	}

	return true;
}
