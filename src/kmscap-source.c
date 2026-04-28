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

#include <obs-module.h>
#include <graphics/graphics.h>

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

	/* DRM device */
	int drm_fd;

	/* Current capture state */
	uint32_t     fb_id;   /* last known scanout FB id         */
	kms_fb_t     fb;      /* metadata + DMA-BUF fds           */
	gs_texture_t *texture;/* imported EGL texture             */

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

#define PROP_DRI_CARD "dri_card"
#define PROP_CRTC_ID  "crtc_id"
#define PROP_CAPTURE_CURSOR "capture_cursor"

/* ---------------------------------------------------------------------- */
/* Forward declarations                                                     */
/* ---------------------------------------------------------------------- */

static void kmscap_close(kmscap_ctx_t *ctx);
static bool kmscap_open(kmscap_ctx_t *ctx);

/* ---------------------------------------------------------------------- */
/* Internal helpers                                                         */
/* ---------------------------------------------------------------------- */

/* Destroy the current EGL texture and close DMA-BUF fds. */
static void kmscap_destroy_texture(kmscap_ctx_t *ctx)
{
	if (ctx->texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->texture);
		obs_leave_graphics();
		ctx->texture = NULL;
	}
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

/**
 * Query the CRTC for its current scanout FB id.
 * Returns 0 on failure or if the CRTC has no active framebuffer.
 */
static uint32_t kmscap_get_current_fb_id(kmscap_ctx_t *ctx)
{
	if (ctx->drm_fd < 0 || ctx->crtc_id == 0)
		return 0;

	drmModeCrtcPtr crtc = drmModeGetCrtc(ctx->drm_fd, ctx->crtc_id);
	if (!crtc)
		return 0;

	uint32_t fb_id = crtc->buffer_id;
	drmModeFreeCrtc(crtc);
	return fb_id;
}

/**
 * (Re-)import the current CRTC scanout framebuffer as an EGL texture.
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

	if (!kms_get_fb2(ctx->drm_fd, fb_id, &ctx->fb)) {
		blog(LOG_ERROR,
		     "[kmscap] Failed to get FB2 for fb_id=%u", fb_id);
		return false;
	}

	if (!kms_export_dmabuf_fds(ctx->drm_fd, &ctx->fb)) {
		blog(LOG_ERROR,
		     "[kmscap] Failed to export DMA-BUF fds for fb_id=%u",
		     fb_id);
		kms_release_fb_fds(&ctx->fb);
		return false;
	}

	/* Build the arrays that gs_texture_create_from_dmabuf expects. */
	int      gs_fmt    = kms_fourcc_to_gs_format(ctx->fb.fourcc);
	uint32_t n         = ctx->fb.num_planes;
	int      fds[KMS_MAX_PLANES];
	uint32_t strides[KMS_MAX_PLANES];
	uint32_t offsets[KMS_MAX_PLANES];
	uint64_t modifiers[KMS_MAX_PLANES];

	for (uint32_t i = 0; i < n; i++) {
		fds[i]       = ctx->fb.fds[i];
		strides[i]   = ctx->fb.pitches[i];
		offsets[i]   = ctx->fb.offsets[i];
		modifiers[i] = ctx->fb.modifiers[i];
	}

	obs_enter_graphics();
	ctx->texture = gs_texture_create_from_dmabuf(
		ctx->fb.width, ctx->fb.height,
		ctx->fb.fourcc,
		(enum gs_color_format)gs_fmt,
		n, fds, strides, offsets, modifiers);
	obs_leave_graphics();

	if (!ctx->texture) {
		blog(LOG_ERROR,
		     "[kmscap] gs_texture_create_from_dmabuf failed "
		     "(fb_id=%u fourcc=%c%c%c%c %ux%u %u planes)",
		     fb_id,
		     (ctx->fb.fourcc >> 0) & 0xFF,
		     (ctx->fb.fourcc >> 8) & 0xFF,
		     (ctx->fb.fourcc >> 16) & 0xFF,
		     (ctx->fb.fourcc >> 24) & 0xFF,
		     ctx->fb.width, ctx->fb.height, n);
		kms_release_fb_fds(&ctx->fb);
		return false;
	}

	blog(LOG_DEBUG,
	     "[kmscap] Imported fb_id=%u %ux%u fourcc=%c%c%c%c %u plane(s)",
	     fb_id, ctx->fb.width, ctx->fb.height,
	     (ctx->fb.fourcc >> 0) & 0xFF, (ctx->fb.fourcc >> 8) & 0xFF,
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
	if (!kms_get_plane_fb(ctx->drm_fd, ctx->cursor_plane_id, &fb_id, &cx, &cy))
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

	if (!kms_get_fb2(ctx->drm_fd, fb_id, &ctx->cursor_fb))
		return false;

	if (!kms_export_dmabuf_fds(ctx->drm_fd, &ctx->cursor_fb)) {
		kms_release_fb_fds(&ctx->cursor_fb);
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

	obs_enter_graphics();
	ctx->cursor_texture = gs_texture_create_from_dmabuf(
		ctx->cursor_fb.width, ctx->cursor_fb.height,
		ctx->cursor_fb.fourcc,
		(enum gs_color_format)gs_fmt,
		n, fds, strides, offsets, modifiers);
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
	int n = kms_enumerate_crtcs(ctx->drm_fd, crtcs, KMS_MAX_CRTCS);
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
	ctx->drm_fd = kms_open_device(ctx->dri_path);
	if (ctx->drm_fd < 0)
		return false;

	ctx->crtc_id = kmscap_resolve_crtc(ctx);
	if (ctx->crtc_id == 0) {
		kms_close_device(ctx->drm_fd);
		ctx->drm_fd = -1;
		return false;
	}

	blog(LOG_INFO, "[kmscap] Opened %s, using CRTC %u",
	     ctx->dri_path, ctx->crtc_id);

	ctx->cursor_plane_id = kms_find_cursor_plane(ctx->drm_fd, ctx->crtc_id);

	kmscap_import_cursor_fb(ctx);

	return kmscap_import_fb(ctx);
}

static void kmscap_close(kmscap_ctx_t *ctx)
{
	kmscap_destroy_texture(ctx);
	kms_close_device(ctx->drm_fd);
	ctx->drm_fd  = -1;
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
	ctx->drm_fd = -1;
	for (int i = 0; i < KMS_MAX_PLANES; i++) {
		ctx->fb.fds[i] = -1;
		ctx->cursor_fb.fds[i] = -1;
	}

	/* Read initial settings. */
	const char *card = obs_data_get_string(settings, PROP_DRI_CARD);
	snprintf(ctx->dri_path, sizeof(ctx->dri_path), "%s",
		 (card && *card) ? card : "/dev/dri/card1");

	ctx->crtc_id = (uint32_t)obs_data_get_int(settings, PROP_CRTC_ID);
	ctx->capture_cursor = obs_data_get_bool(settings, PROP_CAPTURE_CURSOR);

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

	if (ctx->drm_fd < 0)
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

	ctx->capture_cursor = obs_data_get_bool(settings, PROP_CAPTURE_CURSOR);

	if (card_changed || crtc_changed || ctx->drm_fd < 0) {
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
}

/* Callback fired when the user picks a different DRI card in Properties. */
static bool kmscap_card_changed(obs_properties_t *props, obs_property_t *p,
				obs_data_t *settings)
{
	UNUSED_PARAMETER(p);

	const char *card = obs_data_get_string(settings, PROP_DRI_CARD);
	obs_property_t *crtc_list = obs_properties_get(props, PROP_CRTC_ID);
	obs_property_list_clear(crtc_list);

	int fd = kms_open_device((card && *card) ? card : "/dev/dri/card1");
	if (fd < 0) {
		obs_property_set_enabled(crtc_list, false);
		return true;
	}

	kms_crtc_info_t crtcs[KMS_MAX_CRTCS];
	int n = kms_enumerate_crtcs(fd, crtcs, KMS_MAX_CRTCS);
	kms_close_device(fd);

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

	for (int i = 0;; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		if (access(path, F_OK) == 0)
			obs_property_list_add_string(card_list, path, path);
		else
			break;
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

	/* Populate CRTC list for the currently selected (or default) card. */
	const char *cur_card = ctx ? ctx->dri_path : "/dev/dri/card1";
	int dfd = kms_open_device(cur_card);
	if (dfd >= 0) {
		kms_crtc_info_t crtcs[KMS_MAX_CRTCS];
		int n = kms_enumerate_crtcs(dfd, crtcs, KMS_MAX_CRTCS);
		kms_close_device(dfd);

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
