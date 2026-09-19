/*
tea-live-subtitle
Copyright (C) 2026 Caleb Weixun

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Phase 1 skeleton: registers a video source type that draws captions by
 * driving a *private* text-freetype2 child source. This deliberately does
 * NOT reimplement font rasterization/layout -- OBS already ships that in
 * the text-freetype2 plugin, and duplicating it would fight OBS's own text
 * rendering instead of feeling native.
 *
 * No audio capture, no WebSocket client and no caption state machine live
 * here yet; that is phase 2. For now the child text source always shows a
 * placeholder string so the source is visibly alive as soon as it is added
 * to a scene.
 */

#include <obs-module.h>
#include <util/bmem.h>
#include <graphics/graphics.h>

#include "captions-source.h"
#include "plugin-support.h"

/* The freetype2 text source id changed across OBS releases; on this
 * machine's OBS 32.2.1 it is "text_ft2_source", but some builds register it
 * as "text_ft2_source_v2". Try the newer id first, then fall back, and if
 * neither is registered fail loudly instead of silently drawing nothing. */
static const char *const k_text_ft2_ids[] = {
	"text_ft2_source_v2",
	"text_ft2_source",
};

struct tea_captions_source {
	obs_source_t *source;
	obs_source_t *text_source; /* private text_ft2_source(_v2) child, may be NULL */

	int max_lines;
	int caption_align; /* 0 = left, 1 = center, 2 = right */
	int padding;
};

/* There is no portable public API to just ask "is source id X registered"
 * across the OBS versions this plugin targets, so we try to actually create
 * a private instance of each candidate id in turn. obs_source_create_private()
 * returns NULL (and logs its own warning) when the id isn't registered, which
 * is exactly the fallback signal we need. */
static obs_source_t *tea_create_text_child(void)
{
	for (size_t i = 0; i < sizeof(k_text_ft2_ids) / sizeof(k_text_ft2_ids[0]); i++) {
		obs_source_t *child = obs_source_create_private(k_text_ft2_ids[i], "tea-live-subtitle-text", NULL);
		if (child)
			return child;
	}
	return NULL;
}

static const char *tea_captions_source_get_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("TeaLiveSubtitle.SourceName");
}

static void tea_captions_source_update(void *data, obs_data_t *settings)
{
	struct tea_captions_source *ctx = data;

	ctx->max_lines = (int)obs_data_get_int(settings, "max_lines");
	ctx->caption_align = (int)obs_data_get_int(settings, "caption_align");
	ctx->padding = (int)obs_data_get_int(settings, "padding");
	if (ctx->padding < 0)
		ctx->padding = 0;

	if (!ctx->text_source)
		return;

	/* Forward the freetype2 appearance keys (font/color1/color2/outline/
	 * drop_shadow/word_wrap/custom_width) byte-for-byte to the child.
	 * obs_data_apply() copies every key from `settings`, so our own extra
	 * keys (max_lines/caption_align/padding) simply ride along and are
	 * ignored by text_ft2_source. */
	obs_data_t *child_settings = obs_data_create();
	obs_data_apply(child_settings, settings);

	/* Phase 1 has no ASR client yet, so always show a placeholder string
	 * rather than leaving the child source empty/invisible. */
	obs_data_set_string(child_settings, "text", obs_module_text("TeaLiveSubtitle.PlaceholderText"));

	obs_source_update(ctx->text_source, child_settings);
	obs_data_release(child_settings);
}

static void *tea_captions_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct tea_captions_source *ctx = bzalloc(sizeof(struct tea_captions_source));
	ctx->source = source;

	ctx->text_source = tea_create_text_child();
	if (!ctx->text_source) {
		obs_log(LOG_ERROR, "neither 'text_ft2_source_v2' nor 'text_ft2_source' could be created; "
				   "is the text-freetype2 plugin missing? Captions will not render.");
	}

	tea_captions_source_update(ctx, settings);
	return ctx;
}

static void tea_captions_source_destroy(void *data)
{
	struct tea_captions_source *ctx = data;
	if (ctx->text_source) {
		obs_source_release(ctx->text_source);
		ctx->text_source = NULL;
	}
	bfree(ctx);
}

static void tea_captions_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "max_lines", 2);
	obs_data_set_default_int(settings, "caption_align", 1);
	obs_data_set_default_int(settings, "padding", 10);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_string(font_obj, "face", "Arial");
	obs_data_set_int(font_obj, "size", 48);
	obs_data_set_int(font_obj, "flags", 0);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF); /* opaque white */
	obs_data_set_default_int(settings, "color2", 0xFF000000); /* opaque black outline */
	obs_data_set_default_bool(settings, "outline", true);
	obs_data_set_default_bool(settings, "drop_shadow", false);
	obs_data_set_default_bool(settings, "word_wrap", true);
	obs_data_set_default_int(settings, "custom_width", 0);
}

static obs_properties_t *tea_captions_source_get_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();

	/* text_ft2_source's own appearance settings, exposed as-is. */
	obs_properties_add_font(props, "font", obs_module_text("TeaLiveSubtitle.Prop.Font"));
	obs_properties_add_color(props, "color1", obs_module_text("TeaLiveSubtitle.Prop.Color1"));
	obs_properties_add_color(props, "color2", obs_module_text("TeaLiveSubtitle.Prop.Color2"));
	obs_properties_add_bool(props, "outline", obs_module_text("TeaLiveSubtitle.Prop.Outline"));
	obs_properties_add_bool(props, "drop_shadow", obs_module_text("TeaLiveSubtitle.Prop.DropShadow"));
	obs_properties_add_bool(props, "word_wrap", obs_module_text("TeaLiveSubtitle.Prop.WordWrap"));
	obs_properties_add_int(props, "custom_width", obs_module_text("TeaLiveSubtitle.Prop.CustomWidth"), 0, 8192, 1);

	/* This plugin's own settings. Deliberately no x/y/position controls --
	 * that is left to the scene item transform, like a native source. */
	obs_properties_add_int(props, "max_lines", obs_module_text("TeaLiveSubtitle.Prop.MaxLines"), 1, 10, 1);

	obs_property_t *align_list = obs_properties_add_list(props, "caption_align",
							     obs_module_text("TeaLiveSubtitle.Prop.Align"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(align_list, obs_module_text("TeaLiveSubtitle.Prop.Align.Left"), 0);
	obs_property_list_add_int(align_list, obs_module_text("TeaLiveSubtitle.Prop.Align.Center"), 1);
	obs_property_list_add_int(align_list, obs_module_text("TeaLiveSubtitle.Prop.Align.Right"), 2);

	obs_properties_add_int(props, "padding", obs_module_text("TeaLiveSubtitle.Prop.Padding"), 0, 200, 1);

	return props;
}

static uint32_t tea_captions_source_get_width(void *data)
{
	struct tea_captions_source *ctx = data;
	uint32_t child_w = ctx->text_source ? obs_source_get_width(ctx->text_source) : 0;
	return child_w + (uint32_t)(ctx->padding * 2);
}

static uint32_t tea_captions_source_get_height(void *data)
{
	struct tea_captions_source *ctx = data;
	uint32_t child_h = ctx->text_source ? obs_source_get_height(ctx->text_source) : 0;
	return child_h + (uint32_t)(ctx->padding * 2);
}

static void tea_captions_source_video_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	struct tea_captions_source *ctx = data;
	if (!ctx->text_source)
		return;

	/* Our own bounding box always hugs the child source plus padding, so
	 * left/center/right currently collapse to the same padding offset.
	 * Alignment becomes meaningful once phase 2 gives the source a fixed
	 * canvas width independent of the current caption length. */
	uint32_t own_w = tea_captions_source_get_width(data);
	uint32_t child_w = obs_source_get_width(ctx->text_source);
	float offset_x = (float)ctx->padding;
	if (ctx->caption_align == 1) {
		offset_x = (float)(own_w - child_w) / 2.0f;
	} else if (ctx->caption_align == 2) {
		offset_x = (float)(own_w - child_w) - (float)ctx->padding;
	}

	gs_matrix_push();
	gs_matrix_translate3f(offset_x, (float)ctx->padding, 0.0f);
	obs_source_video_render(ctx->text_source);
	gs_matrix_pop();
}

static struct obs_source_info tea_captions_source_info = {
	.id = "tea_live_subtitle_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = tea_captions_source_get_name,
	.create = tea_captions_source_create,
	.destroy = tea_captions_source_destroy,
	.update = tea_captions_source_update,
	.get_defaults = tea_captions_source_get_defaults,
	.get_properties = tea_captions_source_get_properties,
	.get_width = tea_captions_source_get_width,
	.get_height = tea_captions_source_get_height,
	.video_render = tea_captions_source_video_render,
};

void tea_captions_source_register(void)
{
	obs_register_source(&tea_captions_source_info);
}
