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

#include "kmscap-source.h"
#include "kms-utils.h"
#include "kms-ipc.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/math-defs.h>
#include <graphics/matrix4.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drmMode.h>

/* ---------------------------------------------------------------------- */
/* Source state                                                              */
/* ---------------------------------------------------------------------- */

typedef struct {
	obs_source_t *source;

	/* Settings */
	char    dri_path[64]; /* e.g. /dev/dri/card1              */
	uint32_t crtc_id;     /* selected CRTC (0 = first active) */

	/* IPC to helper daemon */
	int ipc_fd;
	pid_t helper_pid;

	/* Current capture state */
	uint32_t     fb_id;    /* last known scanout FB id          */
	kms_fb_t     fb;       /* metadata + DMA-BUF fds            */
	gs_texture_t *texture; /* imported EGL texture (Y or RGB)   */
	gs_texture_t *texture_uv; /* UV plane for NV12              */

	/* NV12 shader */
	gs_effect_t  *nv12_effect;
	int           yuv_color_space; /* 0 = BT.709, 1 = BT.601    */

	/* Cursor state */
	bool         capture_cursor;
	uint32_t     cursor_plane_id;
	uint32_t     cursor_fb_id;
	kms_fb_t     cursor_fb;
	gs_texture_t *cursor_texture;
	int          cursor_x;
	int          cursor_y;
} kmscap_ctx_t;

/* ---------------------------------------------------------------------- */
/* Settings property names                                                  */
/* ---------------------------------------------------------------------- */

#define PROP_DRI_CARD       "dri_card"
#define PROP_CRTC_ID        "crtc_id"
#define PROP_CAPTURE_CURSOR "capture_cursor"
#define PROP_COLOR_SPACE    "yuv_color_space"

#define COLOR_SPACE_BT709 0
#define COLOR_SPACE_BT601 1

/* ---------------------------------------------------------------------- */
/* Forward declarations                                                     */
/* ---------------------------------------------------------------------- */

static void kmscap_close(kmscap_ctx_t *ctx);
static bool kmscap_open(kmscap_ctx_t *ctx);

/* ---------------------------------------------------------------------- */
/* Internal helpers                                                         */
/* ---------------------------------------------------------------------- */

/* Destroy the current EGL texture(s) and close DMA-BUF fds. */
static void kmscap_destroy_texture(kmscap_ctx_t *ctx)
{
	obs_enter_graphics();
	if (ctx->texture) {
		gs_texture_destroy(ctx->texture);
		ctx->texture = NULL;
	}
	if (ctx->texture_uv) {
		gs_texture_destroy(ctx->texture_uv);
		ctx->texture_uv = NULL;
	}
	obs_leave_graphics();
	kms_release_fb_fds(&ctx->fb);
	memset(&ctx->fb, 0, sizeof(ctx->fb));
	for (int i = 0; i < KMS_MAX_PLANES; i++)
		ctx->fb.fds[i] = -1;
	ctx->fb_id = 0;

	if (ctx->cursor_texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->cursor_texture);
		obs_leave_graphics();
		ctx->cursor_texture = NULL;
	}
	kms_release_fb_fds(&ctx->cursor_fb);
	memset(&ctx->cursor_fb, 0, sizeof(ctx->cursor_fb));
	for (int i = 0; i < KMS_MAX_PLANES; i++)
		ctx->cursor_fb.fds[i] = -1;
	ctx->cursor_fb_id = 0;
}

/*
 * Build a column-major 4x4 color matrix (matching OBS gs_matrix convention)
 * mapping [Y, Cb, Cr, 1] → [R, G, B, A] for NV12 limited-range input.
 *
 * BT.709:
 *   Y  range [16,235], Cb/Cr range [16,240], centre 128
 * BT.601:
 *   Same range definitions, different coefficients
 *
 * The matrix is laid out so that OBS can upload it directly via
 * gs_effect_set_matrix4.
 */
static void kmscap_build_color_matrix(int color_space, struct matrix4 *m)
{
	/* Scaling factors for limited range:
	 *   Y:     255 / (235 - 16)  = 255/219
	 *   CbCr:  255 / (240 - 16)  = 255/224
	 */
	const float sy  = 255.0f / 219.0f;
	const float sc  = 255.0f / 224.0f;
	const float oy  = -16.0f  / 219.0f; /* Y  offset */
	const float oc  = -128.0f / 224.0f; /* CbCr offset (centres them at 0) */

	float kr, kb;
	if (color_space == COLOR_SPACE_BT601) {
		/* BT.601 */
		kr = 0.299f;
		kb = 0.114f;
	} else {
		/* BT.709 (default) */
		kr = 0.2126f;
		kb = 0.0722f;
	}
	const float kg = 1.0f - kr - kb;

	/* Conversion from normalised (after offset/scale) Y, Cb, Cr:
	 *   R = Y                      + (2-2*Kr)*Cr
	 *   G = Y - Kb*(2-2*Kb)/Kg*Cb - Kr*(2-2*Kr)/Kg*Cr
	 *   B = Y + (2-2*Kb)*Cb
	 */
	const float r_cr = 2.0f * (1.0f - kr);
	const float b_cb = 2.0f * (1.0f - kb);
	const float g_cb = -kb * b_cb / kg;
	const float g_cr = -kr * r_cr / kg;

	/* matrix4 is row-major in OBS; rows = output channels RGBA */
	/* Row 0 = R coefficients for [Y, Cb, Cr, bias] */
	m->x.x = sy;           /* Y  → R */
	m->x.y = 0.0f;         /* Cb → R */
	m->x.z = sc * r_cr;    /* Cr → R */
	m->x.w = oy + sc * r_cr * oc; /* bias */

	/* Row 1 = G */
	m->y.x = sy;
	m->y.y = sc * g_cb;
	m->y.z = sc * g_cr;
	m->y.w = oy + sc * (g_cb + g_cr) * oc;

	/* Row 2 = B */
	m->z.x = sy;
	m->z.y = sc * b_cb;
	m->z.z = 0.0f;
	m->z.w = oy + sc * b_cb * oc;

	/* Row 3 = A (always 1) */
	m->t.x = 0.0f;
	m->t.y = 0.0f;
	m->t.z = 0.0f;
	m->t.w = 1.0f;
}

static uint32_t kmscap_get_current_fb_id(kmscap_ctx_t *ctx)
{
	if (ctx->ipc_fd < 0 || ctx->crtc_id == 0)
		return 0;

	return ipc_kms_get_current_fb_id(ctx->ipc_fd, ctx->crtc_id);
}

/**
 * (Re-)import the current CRTC scanout framebuffer as an EGL texture.
 * For NV12, imports the Y plane (GS_R8) and UV plane (GS_R8G8) separately.
 * Returns true on success.
 */
static bool kmscap_import_fb(kmscap_ctx_t *ctx)
{
	uint32_t fb_id = kmscap_get_current_fb_id(ctx);
	if (fb_id == 0) {
		blog(LOG_WARNING,
		     "[kmscap] CRTC %u has no active framebuffer", ctx->crtc_id);
		return false;
	}

	/* Nothing changed — keep the existing texture. */
	if (fb_id == ctx->fb_id && ctx->texture)
		return true;

	/* Drop old texture / fds before re-importing. */
	kmscap_destroy_texture(ctx);
	ctx->fb_id = fb_id;

	if (!ipc_kms_get_fb2_and_fds(ctx->ipc_fd, fb_id, &ctx->fb)) {
		blog(LOG_ERROR,
		     "[kmscap] Failed to get FB2 and FDs for fb_id=%u via helper",
		     fb_id);
		return false;
	}

	bool has_modifiers = (ctx->fb.flags & DRM_MODE_FB_MODIFIERS) != 0;
	if (has_modifiers && ctx->fb.modifiers[0] == DRM_FORMAT_MOD_INVALID)
		has_modifiers = false;
	const uint64_t *mods = has_modifiers ? ctx->fb.modifiers : NULL;

	/* ------------------------------------------------------------------ */
	/* NV12 / NV21 — split into two single-channel plane textures.        */
	/* ------------------------------------------------------------------ */
	if (ctx->fb.fourcc == DRM_FORMAT_NV12 ||
	    ctx->fb.fourcc == DRM_FORMAT_NV21) {

		if (ctx->fb.num_planes < 2) {
			blog(LOG_ERROR,
			     "[kmscap] NV12 fb_id=%u has only %u plane(s); need 2",
			     fb_id, ctx->fb.num_planes);
			kms_release_fb_fds(&ctx->fb);
			return false;
		}

		/* Plane 0: Y luma, full resolution, single R8 channel */
		uint64_t y_mod  = has_modifiers ? ctx->fb.modifiers[0] : 0;
		uint64_t uv_mod = has_modifiers ? ctx->fb.modifiers[1] : 0;

		obs_enter_graphics();
		ctx->texture = gs_texture_create_from_dmabuf(
			ctx->fb.width, ctx->fb.height,
			DRM_FORMAT_R8, GS_R8,
			1,
			&ctx->fb.fds[0],
			&ctx->fb.pitches[0],
			&ctx->fb.offsets[0],
			has_modifiers ? &y_mod : NULL);

		/* Plane 1: UV chroma, half resolution, R8G8 (Cb in R, Cr in G) */
		if (ctx->texture) {
			ctx->texture_uv = gs_texture_create_from_dmabuf(
				ctx->fb.width / 2, ctx->fb.height / 2,
				DRM_FORMAT_GR88, GS_R8G8,
				1,
				&ctx->fb.fds[1],
				&ctx->fb.pitches[1],
				&ctx->fb.offsets[1],
				has_modifiers ? &uv_mod : NULL);
		}
		obs_leave_graphics();

		if (!ctx->texture || !ctx->texture_uv) {
			blog(LOG_ERROR,
			     "[kmscap] NV12 plane import failed "
			     "(fb_id=%u %ux%u Y=%s UV=%s)",
			     fb_id, ctx->fb.width, ctx->fb.height,
			     ctx->texture    ? "ok" : "FAIL",
			     ctx->texture_uv ? "ok" : "FAIL");
			obs_enter_graphics();
			if (ctx->texture)    { gs_texture_destroy(ctx->texture);    ctx->texture    = NULL; }
			if (ctx->texture_uv) { gs_texture_destroy(ctx->texture_uv); ctx->texture_uv = NULL; }
			obs_leave_graphics();
			kms_release_fb_fds(&ctx->fb);
			return false;
		}

		blog(LOG_DEBUG,
		     "[kmscap] Imported NV12 fb_id=%u %ux%u (Y+UV planes)",
		     fb_id, ctx->fb.width, ctx->fb.height);
		return true;
	}

	/* ------------------------------------------------------------------ */
	/* All other (single-plane RGB/RGBX) formats                           */
	/* ------------------------------------------------------------------ */
	int      gs_fmt  = kms_fourcc_to_gs_format(ctx->fb.fourcc);
	uint32_t n       = ctx->fb.num_planes;

	obs_enter_graphics();
	ctx->texture = gs_texture_create_from_dmabuf(
		ctx->fb.width, ctx->fb.height,
		ctx->fb.fourcc,
		(enum gs_color_format)gs_fmt,
		n, ctx->fb.fds, ctx->fb.pitches, ctx->fb.offsets, mods);

	/* Fallback: if explicit modifiers were rejected (common on AMD Polaris
	 * where the kernel may report modifiers that Mesa EGL can't import),
	 * retry with implicit modifiers (NULL). */
	if (!ctx->texture && mods != NULL) {
		blog(LOG_WARNING,
		     "[kmscap] Explicit modifier import failed for fb_id=%u, "
		     "retrying with implicit modifiers", fb_id);
		ctx->texture = gs_texture_create_from_dmabuf(
			ctx->fb.width, ctx->fb.height,
			ctx->fb.fourcc,
			(enum gs_color_format)gs_fmt,
			n, ctx->fb.fds, ctx->fb.pitches, ctx->fb.offsets, NULL);
	}
	obs_leave_graphics();

	if (!ctx->texture) {
		blog(LOG_ERROR,
		     "[kmscap] gs_texture_create_from_dmabuf failed "
		     "(fb_id=%u fourcc=%c%c%c%c %ux%u %u planes)",
		     fb_id,
		     (ctx->fb.fourcc >>  0) & 0xFF,
		     (ctx->fb.fourcc >>  8) & 0xFF,
		     (ctx->fb.fourcc >> 16) & 0xFF,
		     (ctx->fb.fourcc >> 24) & 0xFF,
		     ctx->fb.width, ctx->fb.height, n);
		kms_release_fb_fds(&ctx->fb);
		return false;
	}

	blog(LOG_DEBUG,
	     "[kmscap] Imported fb_id=%u %ux%u fourcc=%c%c%c%c %u plane(s)",
	     fb_id, ctx->fb.width, ctx->fb.height,
	     (ctx->fb.fourcc >>  0) & 0xFF, (ctx->fb.fourcc >>  8) & 0xFF,
	     (ctx->fb.fourcc >> 16) & 0xFF, (ctx->fb.fourcc >> 24) & 0xFF,
	     n);

	return true;
}

/**
 * (Re-)import the cursor if needed.
 */
static bool kmscap_import_cursor_fb(kmscap_ctx_t *ctx)
{
	if (!ctx->capture_cursor || ctx->cursor_plane_id == 0)
		return false;

	uint32_t fb_id = 0;
	int cx = 0, cy = 0;
	if (!ipc_kms_get_plane_fb(ctx->ipc_fd, ctx->cursor_plane_id, &fb_id, &cx, &cy))
		return false;

	ctx->cursor_x = cx;
	ctx->cursor_y = cy;

	if (fb_id == 0)
		return true; /* Hidden cursor */

	if (fb_id == ctx->cursor_fb_id && ctx->cursor_texture)
		return true;

	if (ctx->cursor_texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->cursor_texture);
		obs_leave_graphics();
		ctx->cursor_texture = NULL;
	}
	kms_release_fb_fds(&ctx->cursor_fb);
	ctx->cursor_fb_id = fb_id;

	if (!ipc_kms_get_fb2_and_fds(ctx->ipc_fd, fb_id, &ctx->cursor_fb)) {
		blog(LOG_ERROR, "[kmscap] Failed to get cursor FB and FDs");
		return false;
	}

	int      gs_fmt    = kms_fourcc_to_gs_format(ctx->cursor_fb.fourcc);
	uint32_t n         = ctx->cursor_fb.num_planes;
	int      fds[KMS_MAX_PLANES];
	uint32_t strides[KMS_MAX_PLANES];
	uint32_t offsets[KMS_MAX_PLANES];
	uint64_t modifiers[KMS_MAX_PLANES];

	for (uint32_t i = 0; i < n; i++) {
		fds[i]       = ctx->cursor_fb.fds[i];
		strides[i]   = ctx->cursor_fb.pitches[i];
		offsets[i]   = ctx->cursor_fb.offsets[i];
		modifiers[i] = ctx->cursor_fb.modifiers[i];
	}

	bool has_modifiers = (ctx->cursor_fb.flags & DRM_MODE_FB_MODIFIERS) != 0;
	if (has_modifiers && modifiers[0] == DRM_FORMAT_MOD_INVALID)
		has_modifiers = false;

	obs_enter_graphics();
	ctx->cursor_texture = gs_texture_create_from_dmabuf(
		ctx->cursor_fb.width, ctx->cursor_fb.height,
		ctx->cursor_fb.fourcc,
		(enum gs_color_format)gs_fmt,
		n, fds, strides, offsets, has_modifiers ? modifiers : NULL);
	obs_leave_graphics();

	if (!ctx->cursor_texture) {
		blog(LOG_ERROR, "[kmscap] Failed to create cursor texture (fb_id=%u)", fb_id);
		kms_release_fb_fds(&ctx->cursor_fb);
		return false;
	}

	return true;
}

/**
 * Find the CRTC id matching ctx->crtc_id from the settings, or pick the
 * first active CRTC if crtc_id == 0.
 * Returns 0 on failure.
 */
static uint32_t kmscap_resolve_crtc(kmscap_ctx_t *ctx)
{
	kms_crtc_info_t crtcs[KMS_MAX_CRTCS];
	int n = ipc_kms_enumerate_crtcs(ctx->ipc_fd, crtcs, KMS_MAX_CRTCS);
	if (n <= 0) {
		blog(LOG_ERROR, "[kmscap] No active CRTCs found on %s",
		     ctx->dri_path);
		return 0;
	}

	/* If a specific CRTC was requested, look for it. */
	if (ctx->crtc_id != 0) {
		for (int i = 0; i < n; i++) {
			if (crtcs[i].crtc_id == ctx->crtc_id)
				return ctx->crtc_id;
		}
		blog(LOG_WARNING,
		     "[kmscap] Requested CRTC %u not found, using first active",
		     ctx->crtc_id);
	}

	/* Default: use the first active CRTC. */
	return crtcs[0].crtc_id;
}

/* ---------------------------------------------------------------------- */
/* Open / close                                                              */
/* ---------------------------------------------------------------------- */

static bool kmscap_open(kmscap_ctx_t *ctx)
{
	ctx->ipc_fd = ipc_kms_open_device(ctx->dri_path, &ctx->helper_pid);
	if (ctx->ipc_fd < 0)
		return false;

	ctx->crtc_id = kmscap_resolve_crtc(ctx);
	if (ctx->crtc_id == 0) {
		ipc_kms_close_device(ctx->ipc_fd, ctx->helper_pid);
		ctx->ipc_fd = -1;
		ctx->helper_pid = 0;
		return false;
	}

	blog(LOG_INFO, "[kmscap] Opened %s, using CRTC %u",
	     ctx->dri_path, ctx->crtc_id);

	ctx->cursor_plane_id = ipc_kms_find_cursor_plane(ctx->ipc_fd, ctx->crtc_id);

	kmscap_import_cursor_fb(ctx);

	return kmscap_import_fb(ctx);
}

static void kmscap_close(kmscap_ctx_t *ctx)
{
	kmscap_destroy_texture(ctx);
	ipc_kms_close_device(ctx->ipc_fd, ctx->helper_pid);
	ctx->ipc_fd  = -1;
	ctx->helper_pid = 0;
	ctx->crtc_id = 0;
}

/* ---------------------------------------------------------------------- */
/* OBS source callbacks                                                      */
/* ---------------------------------------------------------------------- */

static const char *kmscap_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("KMSCapture.Name");
}

static void *kmscap_create(obs_data_t *settings, obs_source_t *source)
{
	kmscap_ctx_t *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	ctx->ipc_fd = -1;
	ctx->helper_pid = 0;
	for (int i = 0; i < KMS_MAX_PLANES; i++) {
		ctx->fb.fds[i] = -1;
		ctx->cursor_fb.fds[i] = -1;
	}

	/* Load the NV12 YUV→RGB conversion effect. */
	char *effect_path = obs_module_file("effects/kmscap-nv12.effect");
	if (effect_path) {
		obs_enter_graphics();
		ctx->nv12_effect = gs_effect_create_from_file(effect_path, NULL);
		obs_leave_graphics();
		bfree(effect_path);
		if (!ctx->nv12_effect)
			blog(LOG_WARNING, "[kmscap] Failed to load NV12 effect — "
			     "NV12 capture will not work");
	} else {
		blog(LOG_WARNING, "[kmscap] NV12 effect file not found");
	}

	/* Read initial settings. */
	const char *card = obs_data_get_string(settings, PROP_DRI_CARD);
	snprintf(ctx->dri_path, sizeof(ctx->dri_path), "%s",
		 (card && *card) ? card : "/dev/dri/card1");

	ctx->crtc_id       = (uint32_t)obs_data_get_int(settings, PROP_CRTC_ID);
	ctx->capture_cursor = obs_data_get_bool(settings, PROP_CAPTURE_CURSOR);
	ctx->yuv_color_space = (int)obs_data_get_int(settings, PROP_COLOR_SPACE);

	if (!kmscap_open(ctx)) {
		blog(LOG_ERROR, "[kmscap] Failed to open capture — "
		     "check DRI card and CRTC settings");
		/* Return the context anyway; the user can fix settings. */
	}

	return ctx;
}

static void kmscap_destroy(void *data)
{
	kmscap_ctx_t *ctx = data;
	kmscap_close(ctx);
	if (ctx->nv12_effect) {
		obs_enter_graphics();
		gs_effect_destroy(ctx->nv12_effect);
		obs_leave_graphics();
		ctx->nv12_effect = NULL;
	}
	bfree(ctx);
}

static uint32_t kmscap_get_width(void *data)
{
	kmscap_ctx_t *ctx = data;
	return ctx->fb.width;
}

static uint32_t kmscap_get_height(void *data)
{
	kmscap_ctx_t *ctx = data;
	return ctx->fb.height;
}

static void kmscap_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	kmscap_ctx_t *ctx = data;

	if (ctx->ipc_fd < 0)
		return;

	if (!obs_source_showing(ctx->source))
		return;

	/* Poll for FB change (compositor may flip between double buffers). */
	uint32_t current_fb = kmscap_get_current_fb_id(ctx);
	if (current_fb == 0)
		return;

	if (current_fb != ctx->fb_id) {
		blog(LOG_DEBUG, "[kmscap] FB changed %u -> %u, re-importing",
		     ctx->fb_id, current_fb);
		kmscap_import_fb(ctx);
	}

	/* Cursor update */
	if (ctx->capture_cursor && ctx->cursor_plane_id != 0) {
		kmscap_import_cursor_fb(ctx);
	}
}

static void kmscap_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	kmscap_ctx_t *ctx = data;

	if (!ctx->texture)
		return;

	/* ------------------------------------------------------------------ */
	/* NV12: use the YUV→RGB conversion shader                             */
	/* ------------------------------------------------------------------ */
	if (ctx->texture_uv && ctx->nv12_effect) {
		struct matrix4 cmat;
		kmscap_build_color_matrix(ctx->yuv_color_space, &cmat);

		gs_eparam_t *p_y    = gs_effect_get_param_by_name(ctx->nv12_effect, "image");
		gs_eparam_t *p_uv   = gs_effect_get_param_by_name(ctx->nv12_effect, "image_uv");
		gs_eparam_t *p_cmat = gs_effect_get_param_by_name(ctx->nv12_effect, "color_matrix");

		gs_effect_set_texture(p_y,  ctx->texture);
		gs_effect_set_texture(p_uv, ctx->texture_uv);
		gs_effect_set_matrix4(p_cmat, &cmat);

		while (gs_effect_loop(ctx->nv12_effect, "Draw"))
			gs_draw_sprite(ctx->texture, 0, ctx->fb.width, ctx->fb.height);

		/* Cursor is always ARGB — draw it with the default effect on top */
		if (ctx->capture_cursor && ctx->cursor_texture && ctx->cursor_fb_id != 0) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
			gs_effect_set_texture(img, ctx->cursor_texture);
			while (gs_effect_loop(def, "Draw")) {
				gs_matrix_push();
				gs_matrix_translate3f((float)ctx->cursor_x, (float)ctx->cursor_y, 0.0f);
				gs_draw_sprite(ctx->cursor_texture, 0, 0, 0);
				gs_matrix_pop();
			}
		}
		return;
	}

	/* ------------------------------------------------------------------ */
	/* Standard RGB path                                                    */
	/* ------------------------------------------------------------------ */
	effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, ctx->texture);

	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(ctx->texture, 0, 0, 0);

		if (ctx->capture_cursor && ctx->cursor_texture && ctx->cursor_fb_id != 0) {
			gs_matrix_push();
			gs_matrix_translate3f((float)ctx->cursor_x, (float)ctx->cursor_y, 0.0f);

			gs_eparam_t *cursor_image = gs_effect_get_param_by_name(effect, "image");
			gs_effect_set_texture(cursor_image, ctx->cursor_texture);
			gs_draw_sprite(ctx->cursor_texture, 0, 0, 0);

			/* Restore base texture for next pass if any */
			gs_effect_set_texture(image, ctx->texture);
			gs_matrix_pop();
		}
	}
}

static void kmscap_update(void *data, obs_data_t *settings)
{
	kmscap_ctx_t *ctx = data;

	const char *card = obs_data_get_string(settings, PROP_DRI_CARD);
	uint32_t    crtc = (uint32_t)obs_data_get_int(settings, PROP_CRTC_ID);

	/* Only re-open if something actually changed. */
	bool card_changed = strncmp(ctx->dri_path,
				    (card && *card) ? card : "/dev/dri/card1",
				    sizeof(ctx->dri_path)) != 0;
	bool crtc_changed = (crtc != ctx->crtc_id && crtc != 0);

	ctx->capture_cursor  = obs_data_get_bool(settings, PROP_CAPTURE_CURSOR);
	ctx->yuv_color_space = (int)obs_data_get_int(settings, PROP_COLOR_SPACE);

	if (card_changed || crtc_changed || ctx->ipc_fd < 0) {
		kmscap_close(ctx);
		snprintf(ctx->dri_path, sizeof(ctx->dri_path), "%s",
			 (card && *card) ? card : "/dev/dri/card1");
		ctx->crtc_id = crtc;
		kmscap_open(ctx);
	}
}

static void kmscap_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, PROP_DRI_CARD, "/dev/dri/card1");
	obs_data_set_default_int(settings, PROP_CRTC_ID, 0);
	obs_data_set_default_bool(settings, PROP_CAPTURE_CURSOR, true);
	obs_data_set_default_int(settings, PROP_COLOR_SPACE, COLOR_SPACE_BT709);
}

/* Callback fired when the user picks a different DRI card in Properties. */
static bool kmscap_card_changed(obs_properties_t *props, obs_property_t *p,
				obs_data_t *settings)
{
	UNUSED_PARAMETER(p);

	const char *card = obs_data_get_string(settings, PROP_DRI_CARD);
	obs_property_t *crtc_list = obs_properties_get(props, PROP_CRTC_ID);
	obs_property_list_clear(crtc_list);

	pid_t hpid = 0;
	int fd = ipc_kms_open_device((card && *card) ? card : "/dev/dri/card1", &hpid);
	if (fd < 0) {
		obs_property_set_enabled(crtc_list, false);
		return true;
	}

	kms_crtc_info_t crtcs[KMS_MAX_CRTCS];
	int n = ipc_kms_enumerate_crtcs(fd, crtcs, KMS_MAX_CRTCS);
	ipc_kms_close_device(fd, hpid);

	obs_property_set_enabled(crtc_list, n > 0);
	for (int i = 0; i < n; i++) {
		char label[128];
		snprintf(label, sizeof(label), "[CRTC %u] %s",
			 crtcs[i].crtc_id, crtcs[i].name);
		obs_property_list_add_int(crtc_list, label,
					  (long long)crtcs[i].crtc_id);
	}

	return true; /* redraw properties */
}

static obs_properties_t *kmscap_get_properties(void *data)
{
	kmscap_ctx_t *ctx = data;

	obs_properties_t *props = obs_properties_create();

	/* DRI card list */
	obs_property_t *card_list = obs_properties_add_list(
		props, PROP_DRI_CARD,
		obs_module_text("KMSCapture.DRICard"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	for (int i = 0; i < 16; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		if (access(path, F_OK) == 0)
			obs_property_list_add_string(card_list, path, path);
	}

	obs_property_set_modified_callback(card_list, kmscap_card_changed);

	/* CRTC / output list */
	obs_property_t *crtc_list = obs_properties_add_list(
		props, PROP_CRTC_ID,
		obs_module_text("KMSCapture.CRTC"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	
	/* Cursor capture toggle */
	obs_properties_add_bool(props, PROP_CAPTURE_CURSOR,
		obs_module_text("KMSCapture.CaptureCursor"));

	/* YUV color space (only relevant when NV12 is scanned out) */
	obs_property_t *cs_list = obs_properties_add_list(
		props, PROP_COLOR_SPACE,
		obs_module_text("KMSCapture.ColorSpace"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cs_list,
		obs_module_text("KMSCapture.ColorSpace.BT709"), COLOR_SPACE_BT709);
	obs_property_list_add_int(cs_list,
		obs_module_text("KMSCapture.ColorSpace.BT601"), COLOR_SPACE_BT601);

	const char *cur_card = ctx ? ctx->dri_path : "/dev/dri/card1";
	pid_t hpid = 0;
	int dfd = ipc_kms_open_device(cur_card, &hpid);
	if (dfd >= 0) {
		kms_crtc_info_t crtcs[KMS_MAX_CRTCS];
		int n = ipc_kms_enumerate_crtcs(dfd, crtcs, KMS_MAX_CRTCS);
		ipc_kms_close_device(dfd, hpid);

		if (n <= 0) {
			obs_property_list_add_int(
				crtc_list,
				obs_module_text("KMSCapture.NoCRTC"), 0);
			obs_property_set_enabled(crtc_list, false);
		} else {
			for (int i = 0; i < n; i++) {
				char label[128];
				snprintf(label, sizeof(label),
					 "[CRTC %u] %s",
					 crtcs[i].crtc_id, crtcs[i].name);
				obs_property_list_add_int(
					crtc_list, label,
					(long long)crtcs[i].crtc_id);
			}
		}
	} else {
		obs_property_set_enabled(crtc_list, false);
	}

	return props;
}

/* ---------------------------------------------------------------------- */
/* Source info registration                                                  */
/* ---------------------------------------------------------------------- */

struct obs_source_info kmscap_source_info = {
	.id             = "kmscap_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
	                  OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = kmscap_get_name,
	.create         = kmscap_create,
	.destroy        = kmscap_destroy,
	.get_width      = kmscap_get_width,
	.get_height     = kmscap_get_height,
	.video_tick     = kmscap_video_tick,
	.video_render   = kmscap_video_render,
	.update         = kmscap_update,
	.get_defaults   = kmscap_get_defaults,
	.get_properties = kmscap_get_properties,
	.icon_type      = OBS_ICON_TYPE_DESKTOP_CAPTURE,
};
