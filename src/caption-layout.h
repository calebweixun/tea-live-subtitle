#pragma once

/*
 * OBS-free geometry policy for the captions source.
 *
 * The source still delegates glyph rasterisation and native word wrapping to
 * text_ft2_source.  This header only defines the boundary between the
 * plugin-owned outer source box and the child source's content width, which
 * makes the fixed-width contract testable without an OBS SDK.
 */

#include <stdbool.h>
#include <stdint.h>

#define TEA_LAYOUT_MODE_AUTO 0
#define TEA_LAYOUT_MODE_FIXED 1

/* Persisted settings schema for the outer caption box.  Older scene
 * collections have no layout marker; they must keep the legacy auto-sized
 * source until the user explicitly chooses a layout mode. */
#define TEA_LAYOUT_SCHEMA_VERSION 1
#define TEA_LAYOUT_SCHEMA_KEY "layout_schema_version"

#define TEA_LAYOUT_DEFAULT_WIDTH_PX 960
#define TEA_LAYOUT_MIN_WIDTH_PX 320
#define TEA_LAYOUT_MAX_WIDTH_PX 8192

typedef struct tea_caption_layout {
	int mode;
	uint32_t outer_width;
	uint32_t content_width;
	bool force_word_wrap;
} tea_caption_layout_t;

static inline int tea_caption_layout_clamp_padding(int padding)
{
	return padding < 0 ? 0 : padding;
}

static inline uint32_t tea_caption_layout_clamp_fixed_width(int width)
{
	if (width < TEA_LAYOUT_MIN_WIDTH_PX)
		return TEA_LAYOUT_DEFAULT_WIDTH_PX;
	if (width > TEA_LAYOUT_MAX_WIDTH_PX)
		return TEA_LAYOUT_MAX_WIDTH_PX;
	return (uint32_t)width;
}

/* Resolve a value read from OBS settings after the layout migration policy
 * has classified the source.  A schema marker is written for new sources;
 * an explicit layout_mode is also honoured so an old scene can opt into the
 * new behavior.  An old scene with neither marker nor explicit mode remains
 * in Auto mode even though the current default shown to new sources is Fixed.
 */
static inline int tea_caption_layout_mode_from_settings(int stored_mode, bool has_schema_marker, bool has_explicit_mode)
{
	if (!has_schema_marker && !has_explicit_mode)
		return TEA_LAYOUT_MODE_AUTO;
	return stored_mode == TEA_LAYOUT_MODE_FIXED ? TEA_LAYOUT_MODE_FIXED : TEA_LAYOUT_MODE_AUTO;
}

static inline tea_caption_layout_t tea_caption_layout_resolve(int mode, int fixed_width, int padding,
							      uint32_t child_width)
{
	tea_caption_layout_t result = {
		.mode = mode == TEA_LAYOUT_MODE_FIXED ? TEA_LAYOUT_MODE_FIXED : TEA_LAYOUT_MODE_AUTO,
		.outer_width = 0,
		.content_width = 0,
		.force_word_wrap = false,
	};

	uint32_t safe_padding = (uint32_t)tea_caption_layout_clamp_padding(padding);
	if (result.mode == TEA_LAYOUT_MODE_FIXED) {
		result.outer_width = tea_caption_layout_clamp_fixed_width(fixed_width);
		uint32_t horizontal_padding = safe_padding > UINT32_MAX / 2 ? UINT32_MAX : safe_padding * 2;
		result.content_width = result.outer_width > horizontal_padding ? result.outer_width - horizontal_padding
									       : 1;
		result.force_word_wrap = true;
		return result;
	}

	result.content_width = child_width;
	if (result.content_width > UINT32_MAX - safe_padding * 2)
		result.outer_width = UINT32_MAX;
	else
		result.outer_width = result.content_width + safe_padding * 2;
	return result;
}

/* Select only transcript text or a caller-owned neutral placeholder. This
 * intentionally has no connection/status input, so server diagnostics cannot
 * accidentally become source text. The caller owns any copy it needs. */
static inline const char *tea_caption_select_display_text(const char *transcript, bool show_placeholder,
							  const char *placeholder)
{
	if (transcript && transcript[0] != '\0')
		return transcript;
	if (show_placeholder && placeholder && placeholder[0] != '\0')
		return placeholder;
	return "";
}
