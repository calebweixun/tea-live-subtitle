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
 * Registers a video source type that draws captions line by line.
 *
 * Glyph rasterisation is still OBS's own text-freetype2 source -- the plugin
 * does not ship a font engine. What changed is that the plugin no longer
 * pushes every caption line into one text_ft2 child (which can only draw a
 * left-aligned block, has no text background and no per-line opacity):
 *
 *   - A small pool of private text_ft2 "workers" (TEA_WORKER_COUNT) is used
 *     as a measuring and rasterising service. Each text_ft2 instance owns a
 *     2048x2048 glyph atlas (~8 MB CPU+GPU), so one child per visible row
 *     would cost tens of MB; three workers cost the same no matter how many
 *     rows are on screen.
 *   - Measuring: text_ft2's width is the plain sum of glyph advances (no
 *     kerning) plus a constant outline margin, so one measurement per code
 *     point fills a glyph-advance cache (caption-display.h) and after that
 *     every wrap/alignment decision is synchronous arithmetic.
 *   - Rasterising: each "piece" of a row (text that appeared at one time; the
 *     unstable tail) is rendered once by a worker into its own texture and
 *     cached. Committed text never changes, so a piece is rendered exactly
 *     once and then only drawn -- with its own opacity for fade in/out.
 *   - A new layout is shown only after all of its textures exist; until then
 *     the previous layout stays on screen, so nothing ever flickers.
 *
 * All geometry and timing decisions (wrapping, alignment, row limit, pause
 * line breaks, fades) are pure functions in caption-display.h and tested
 * without OBS in tests/caption-layout-test.cpp. This file only moves data
 * between the caption state, the workers and the GPU.
 *
 * M2: each source instance owns its own audio tap + WebSocket client +
 * caption state machine (audio-tap.h/caption-state.h/asr-client.h). This is
 * a deliberate simplification versus docs/08-obs-plugin.md's sketch of a
 * single *global* connection managed from the Tools-menu settings dialog:
 * doing it per-source keeps ownership/lifetime trivial (no shared global to
 * synchronize against scene collection load/unload, multiple independent
 * captions sources "just work" with different audio sources or even
 * different servers) at the cost of one WS connection per source instance
 * instead of one for the whole app. Revisit if that connection-count cost
 * turns out to matter in practice.
 */

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <string.h>

#include "captions-source.h"
#include "audio-tap.h"
#include "caption-state.h"
#include "caption-layout.h"
#include "caption-display.h"
#include "asr-client.h"
#include "font-default-policy.h"
#include "plugin-support.h"

/* The freetype2 text source id changed across OBS releases; on this
 * machine's OBS 32.2.1 it is "text_ft2_source", but some builds register it
 * as "text_ft2_source_v2". Try the newer id first, then fall back, and if
 * neither is registered fail loudly instead of silently drawing nothing. */
static const char *const k_text_ft2_ids[] = {
	"text_ft2_source_v2",
	"text_ft2_source",
};

/* ---- settings keys added by the per-line renderer ---- */
#define TEA_KEY_PAUSE_MS "line_break_pause_ms"
#define TEA_KEY_FADE_IN "fade_in_enabled"
#define TEA_KEY_FADE_OUT "fade_out_enabled"
#define TEA_KEY_FADE_MS "fade_ms"
#define TEA_KEY_FADE_DELAY_MS "fade_out_delay_ms"
#define TEA_KEY_MAX_ROWS "max_visible_rows"
#define TEA_KEY_BG "text_bg_enabled"
#define TEA_KEY_BG_COLOR "text_bg_color"
#define TEA_KEY_BG_PADDING "text_bg_padding"
#define TEA_KEY_TAIL "show_unstable_tail"
#define TEA_KEY_TAIL_OPACITY "unstable_tail_opacity"

/* Sources created after the per-line renderer shipped get the recommended
 * live-subtitle behaviour (fade in/out, pause breaks, a two-row screen
 * limit) as explicit values. Existing scenes have no marker and keep the
 * neutral defaults, so they look the same after the upgrade. */
#define TEA_RENDER_SCHEMA_KEY "render_schema_version"
#define TEA_RENDER_SCHEMA_VERSION 1

#define TEA_DEFAULT_PAUSE_MS 0
#define TEA_NEW_SOURCE_PAUSE_MS 3000
#define TEA_DEFAULT_FADE_MS 400
#define TEA_DEFAULT_FADE_DELAY_MS 6000
#define TEA_NEW_SOURCE_MAX_ROWS 2
#define TEA_DEFAULT_BG_COLOR 0xA0000000 /* ABGR: black, ~63% opaque */
#define TEA_DEFAULT_BG_PADDING 8
#define TEA_DEFAULT_TAIL_OPACITY 50

#define TEA_WORKER_COUNT 3
#define TEA_MAX_DRAW_ROWS 20
#define TEA_MAX_DRAW_PIECES 256
#define TEA_TEX_CACHE_SIZE 320
#define TEA_WRAP_RING 32
#define TEA_PROBE_QUEUE 64
#define TEA_MAX_LINES (TEA_CAPTION_SNAPSHOT_MAX_LINES + 1) /* + placeholder */
#define TEA_PLACEHOLDER_KEY (UINT64_MAX - 1)
/* Every worker first caches these glyphs, so all workers put the baseline at
 * the same height (text_ft2 places it at the tallest glyph cached so far). */
#define TEA_PRIME_TEXT "\xe5\x9c\x8b\xe3\x80\x8c\xe3\x80\x8d\xef\xbc\x88\xef\xbc\x89" "Agjpqy|" /* 國「」（）Agjpqy| */
#define TEA_CAL_ONE "\xe5\x9c\x8b"                                                  /* 國 */
#define TEA_CAL_TWO "\xe5\x9c\x8b\xe5\x9c\x8b"                                      /* 國國 */
#define TEA_ATLAS_SIDE 2048 /* text_ft2's glyph atlas, text-freetype2.c texbuf_w/h */

enum tea_job {
	TEA_JOB_NONE,
	TEA_JOB_PRIME,
	TEA_JOB_CAL_ONE,
	TEA_JOB_CAL_TWO,
	TEA_JOB_PROBE,
	TEA_JOB_RASTER,
};

/* Everything update() hands to the render thread. Built on the UI thread,
 * swapped in by video_tick(), never shared afterwards. */
struct tea_render_config {
	int align;
	int padding;
	int layout_mode;
	int caption_width;
	int legacy_custom_width;
	bool legacy_word_wrap;
	bool show_placeholder;
	uint32_t pause_ms;
	bool fade_in;
	bool fade_out;
	uint32_t fade_ms;
	uint32_t fade_delay_ms;
	int max_rows;
	bool bg;
	uint32_t bg_color;
	int bg_padding;
	bool tail;
	float tail_opacity;
	int font_size;
	obs_data_t *child; /* text_ft2 appearance settings (no "text") */
	char *metrics_sig; /* anything that changes glyph advances / heights */
	char *look_sig;    /* metrics + colours: anything that changes pixels */
};

struct tea_worker {
	obs_source_t *src;
	volatile long requested; /* update sequence we asked for */
	volatile long applied;   /* update sequence text_ft2 has processed */
	uint64_t look_rev;       /* appearance applied to src */
	uint64_t metrics_rev;
	uint64_t primed_rev; /* metrics_rev the baseline was primed for */
	enum tea_job job;
	uint64_t job_look_rev;
	uint64_t job_metrics_rev;
	uint32_t cp;
	int tex;
	uint32_t glyphs; /* glyphs in this worker's atlas (approximate) */
};

/* One cached texture: text[start, end) of line `key`, rendered by a worker. */
struct tea_tex {
	bool used;
	uint64_t key;
	size_t start, end;
	uint64_t look_rev;
	char *text;
	gs_texrender_t *tr;
	uint32_t w, h;
	bool ready;
	int worker; /* -1 unless being rendered */
	uint64_t stamp;
};

struct tea_rline {
	bool used;
	tea_display_line_t meta;
	char *text; /* displayed text: committed + tail */
};

struct tea_draw_row {
	int32_t x, y;
	uint32_t w, h;
	uint64_t born;    /* first appearance of the row's oldest piece */
	uint64_t changed; /* line's last change: drives fade-out */
	bool persistent;
};

struct tea_draw_piece {
	int tex;
	int row;
	int32_t x;
	uint64_t born;
	bool tail;
};

struct tea_frame {
	struct tea_draw_row rows[TEA_MAX_DRAW_ROWS];
	int row_count;
	struct tea_draw_piece pieces[TEA_MAX_DRAW_PIECES];
	int piece_count;
	uint32_t width, height;
};

struct tea_captions_source {
	obs_source_t *source;
	const char *text_ft2_id; /* the registered text_ft2 id, NULL if none */

	int max_lines;
	int caption_align; /* 0 = left, 1 = center, 2 = right */
	int padding;
	int layout_mode;      /* TEA_LAYOUT_MODE_AUTO or TEA_LAYOUT_MODE_FIXED */
	int caption_width_px; /* fixed outer width, including horizontal padding */
	bool show_placeholder;

	tea_audio_tap_t *tap;
	tea_caption_state_t *captions;
	tea_asr_client_t *client;

	/* Cache of the connection settings we last applied, so update() (which
	 * OBS calls ~500 ms after every edit in the Properties window) only
	 * tears down and reconnects the WebSocket when something that reaches
	 * the server actually changed -- and, while a Properties window is
	 * open, only when the user presses "Apply connection settings" or
	 * closes the window (see tea_connection_decide()). */
	pthread_mutex_t conn_lock;
	char *applied_audio_source_name;
	char *applied_server_host;
	int applied_server_port;
	char *applied_token_path;
	int applied_stable_captions; /* -1 = never applied, else 0/1 */
	bool connection_pending;
	volatile long properties_open;

	/* ---- render thread (video_tick / video_render) ---- */
	pthread_mutex_t config_lock;
	struct tea_render_config *pending_config; /* guarded by config_lock */
	struct tea_render_config *config;
	uint64_t metrics_rev;
	uint64_t look_rev;

	struct tea_worker workers[TEA_WORKER_COUNT];
	tea_glyph_cache_t *glyphs;
	bool have_cal_one, have_cal_two;
	uint32_t cal_one, cal_two;
	uint32_t outline_extra;
	uint32_t row_height;
	uint32_t probe_queue[TEA_PROBE_QUEUE];
	int probe_count;

	struct tea_rline lines[TEA_MAX_LINES];
	int order[TEA_MAX_LINES]; /* line indices, oldest first */
	int order_count;
	uint64_t caption_rev_seen;
	bool force_snapshot;
	bool layout_dirty;

	struct tea_tex texes[TEA_TEX_CACHE_SIZE];
	struct tea_frame committed;
	bool have_committed;
	struct tea_frame pending;
	bool have_pending;
	uint64_t frame_no;

	gs_effect_t *fade_effect;
	gs_eparam_t *fade_image;
	gs_eparam_t *fade_opacity;

	volatile long out_width;
	volatile long out_height;

	/* Intrusive singly-linked list node for g_registry_head below, so the
	 * Tools-menu settings dialog can enumerate every live instance (it has
	 * no other way to reach per-source state: this plugin has no global
	 * service object, see the architecture comment at the top of this
	 * file). Guarded by g_registry_lock, not ctx-specific. */
	struct tea_captions_source *registry_next;
};

/* Draws a cached text texture with a uniform opacity. The texture holds
 * premultiplied colour (text_ft2 drew it with (SRCALPHA, INVSRCALPHA, ONE,
 * INVSRCALPHA) into a cleared target), so multiplying every channel by the
 * opacity and blending (ONE, INVSRCALPHA) equals drawing the text directly
 * at that opacity. */
static const char k_fade_effect[] = "uniform float4x4 ViewProj;\n"
				    "uniform texture2d image;\n"
				    "uniform float opacity;\n"
				    "sampler_state def_sampler {\n"
				    "	Filter   = Point;\n"
				    "	AddressU = Clamp;\n"
				    "	AddressV = Clamp;\n"
				    "};\n"
				    "struct VertInOut {\n"
				    "	float4 pos : POSITION;\n"
				    "	float2 uv  : TEXCOORD0;\n"
				    "};\n"
				    "VertInOut VSDefault(VertInOut vert_in)\n"
				    "{\n"
				    "	VertInOut vert_out;\n"
				    "	vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);\n"
				    "	vert_out.uv  = vert_in.uv;\n"
				    "	return vert_out;\n"
				    "}\n"
				    "float4 PSDraw(VertInOut vert_in) : TARGET\n"
				    "{\n"
				    "	return image.Sample(def_sampler, vert_in.uv) * opacity;\n"
				    "}\n"
				    "technique Draw\n"
				    "{\n"
				    "	pass\n"
				    "	{\n"
				    "		vertex_shader = VSDefault(vert_in);\n"
				    "		pixel_shader  = PSDraw(vert_in);\n"
				    "	}\n"
				    "}\n";

/* --- registry of live instances, for the Tools-menu settings dialog --- */

static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct tea_captions_source *g_registry_head = NULL;

static void tea_registry_add(struct tea_captions_source *ctx)
{
	pthread_mutex_lock(&g_registry_lock);
	ctx->registry_next = g_registry_head;
	g_registry_head = ctx;
	pthread_mutex_unlock(&g_registry_lock);
}

static void tea_registry_remove(struct tea_captions_source *ctx)
{
	pthread_mutex_lock(&g_registry_lock);
	struct tea_captions_source **link = &g_registry_head;
	while (*link) {
		if (*link == ctx) {
			*link = ctx->registry_next;
			break;
		}
		link = &(*link)->registry_next;
	}
	pthread_mutex_unlock(&g_registry_lock);
}

void tea_captions_source_for_each(void (*cb)(const tea_captions_source_info_t *info, void *user), void *user)
{
	pthread_mutex_lock(&g_registry_lock);
	for (struct tea_captions_source *ctx = g_registry_head; ctx; ctx = ctx->registry_next) {
		if (!ctx->client)
			continue;
		char *status_text = tea_asr_client_status_text(ctx->client);
		tea_captions_source_info_t info = {
			.source_name = ctx->source ? obs_source_get_name(ctx->source) : NULL,
			.status_text = status_text,
			.dropped_frames = tea_asr_client_dropped_audio_frames(ctx->client),
			.connected = tea_asr_client_is_connected(ctx->client),
			.capabilities_known = tea_asr_client_capabilities_known(ctx->client),
			.supports_partial_transcripts = tea_asr_client_supports_partial_transcripts(ctx->client),
			.supports_stable_transcripts = tea_asr_client_supports_stable_transcripts(ctx->client),
			.stable_captions_enabled = ctx->applied_stable_captions != 0,
			.stable_captions_active = tea_asr_client_stable_captions_active(ctx->client),
			.stable_mismatches = ctx->captions ? tea_caption_state_stable_mismatches(ctx->captions) : 0,
		};
		cb(&info, user);
		bfree(status_text);
	}
	pthread_mutex_unlock(&g_registry_lock);
}

void tea_captions_source_reconnect_all(void)
{
	pthread_mutex_lock(&g_registry_lock);
	for (struct tea_captions_source *ctx = g_registry_head; ctx; ctx = ctx->registry_next) {
		if (ctx->client)
			tea_asr_client_start(ctx->client);
	}
	pthread_mutex_unlock(&g_registry_lock);
}

static const char *tea_captions_source_get_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("TeaLiveSubtitle.SourceName");
}

/* obs_module_text() returns the key itself when a locale has no entry for it.
 * Fall back to a readable built-in label instead of showing the raw key. */
static const char *tea_text_or(const char *key, const char *fallback)
{
	const char *text = obs_module_text(key);
	return (text && strcmp(text, key) != 0) ? text : fallback;
}

static bool tea_str_eq(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	return strcmp(a, b) == 0;
}

/* ------------------------------------------------------------------------ */
/* Connection settings                                                       */
/* ------------------------------------------------------------------------ */

/* Applies the server connection settings found in `settings`, following
 * tea_connection_decide(): immediately when nothing is being edited, only on
 * Apply / window close while a Properties window is open. Appearance
 * settings never come through here, so changing them never reconnects. */
static void tea_apply_connection(struct tea_captions_source *ctx, obs_data_t *settings, bool force)
{
	if (!ctx->client)
		return;
	const char *server_host = obs_data_get_string(settings, "server_host");
	int server_port = (int)obs_data_get_int(settings, "server_port");
	const char *token_path = obs_data_get_string(settings, "token_path");
	const bool stable_captions = obs_data_get_bool(settings, "stable_captions");

	pthread_mutex_lock(&ctx->conn_lock);
	tea_connection_settings_t applied = {
		.host = ctx->applied_server_host,
		.port = ctx->applied_server_port,
		.token_path = ctx->applied_token_path,
		.stable = ctx->applied_stable_captions == 1,
	};
	tea_connection_settings_t incoming = {
		.host = server_host,
		.port = server_port,
		.token_path = token_path,
		.stable = stable_captions,
	};
	const bool interactive = os_atomic_load_long(&ctx->properties_open) > 0;
	const int action =
		tea_connection_decide(&applied, &incoming, ctx->applied_stable_captions >= 0, interactive, force);
	ctx->connection_pending = action == TEA_CONNECTION_DEFER;
	if (action == TEA_CONNECTION_RECONNECT) {
		tea_asr_client_set_server(ctx->client, server_host, server_port);
		tea_asr_client_set_token_path(ctx->client, token_path);
		/* session.start carries `stable`, so a toggle needs a new session. */
		tea_asr_client_set_stable_captions(ctx->client, stable_captions);
		tea_asr_client_start(ctx->client); /* idempotent restart with the new settings */

		bfree(ctx->applied_server_host);
		ctx->applied_server_host = bstrdup(server_host);
		ctx->applied_server_port = server_port;
		bfree(ctx->applied_token_path);
		ctx->applied_token_path = bstrdup(token_path);
		ctx->applied_stable_captions = stable_captions ? 1 : 0;
	}
	pthread_mutex_unlock(&ctx->conn_lock);
}

static bool tea_apply_connection_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)props;
	(void)property;
	struct tea_captions_source *ctx = data;
	if (!ctx || !ctx->source)
		return false;
	/* The Properties window edits the source's own settings object, so it
	 * already holds what the user typed even before OBS's 500 ms update. */
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	tea_apply_connection(ctx, settings, true);
	obs_data_release(settings);
	return false;
}

/* obs_properties_set_param() destroy callback: runs when the Properties
 * window that asked for these properties goes away (OK, Cancel, close). OK
 * and close keep the edited values, Cancel has already restored the old
 * ones through update(); either way the settings object now holds what the
 * user wants, so apply anything still pending. */
static void tea_properties_closed(void *param)
{
	obs_weak_source_t *weak = param;
	obs_source_t *source = obs_weak_source_get_source(weak);
	if (source) {
		struct tea_captions_source *ctx = obs_obj_get_data(source);
		if (ctx && os_atomic_dec_long(&ctx->properties_open) <= 0) {
			os_atomic_set_long(&ctx->properties_open, 0);
			if (ctx->connection_pending) {
				obs_data_t *settings = obs_source_get_settings(source);
				tea_apply_connection(ctx, settings, true);
				obs_data_release(settings);
			}
		}
		obs_source_release(source);
	}
	obs_weak_source_release(weak);
}

/* ------------------------------------------------------------------------ */
/* Render configuration                                                      */
/* ------------------------------------------------------------------------ */

static void tea_config_free(struct tea_render_config *cfg)
{
	if (!cfg)
		return;
	obs_data_release(cfg->child);
	bfree(cfg->metrics_sig);
	bfree(cfg->look_sig);
	bfree(cfg);
}

static int tea_clamp_int(long long v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return (int)v;
}

static struct tea_render_config *tea_config_build(struct tea_captions_source *ctx, obs_data_t *settings)
{
	struct tea_render_config *cfg = bzalloc(sizeof(*cfg));
	cfg->align = tea_clamp_int(obs_data_get_int(settings, "caption_align"), 0, 2);
	cfg->padding = ctx->padding;
	cfg->layout_mode = ctx->layout_mode;
	cfg->caption_width = ctx->caption_width_px;
	cfg->legacy_custom_width = tea_clamp_int(obs_data_get_int(settings, "custom_width"), 0, 8192);
	cfg->legacy_word_wrap = obs_data_get_bool(settings, "word_wrap");
	cfg->show_placeholder = ctx->show_placeholder;
	cfg->pause_ms = (uint32_t)tea_clamp_int(obs_data_get_int(settings, TEA_KEY_PAUSE_MS), 0, 60000);
	cfg->fade_in = obs_data_get_bool(settings, TEA_KEY_FADE_IN);
	cfg->fade_out = obs_data_get_bool(settings, TEA_KEY_FADE_OUT);
	cfg->fade_ms = (uint32_t)tea_clamp_int(obs_data_get_int(settings, TEA_KEY_FADE_MS), 0, 10000);
	cfg->fade_delay_ms = (uint32_t)tea_clamp_int(obs_data_get_int(settings, TEA_KEY_FADE_DELAY_MS), 0, 600000);
	cfg->max_rows = tea_clamp_int(obs_data_get_int(settings, TEA_KEY_MAX_ROWS), 0, TEA_MAX_DRAW_ROWS);
	cfg->bg = obs_data_get_bool(settings, TEA_KEY_BG);
	cfg->bg_color = (uint32_t)obs_data_get_int(settings, TEA_KEY_BG_COLOR);
	cfg->bg_padding = tea_clamp_int(obs_data_get_int(settings, TEA_KEY_BG_PADDING), 0, 200);
	cfg->tail = obs_data_get_bool(settings, TEA_KEY_TAIL);
	cfg->tail_opacity = (float)tea_clamp_int(obs_data_get_int(settings, TEA_KEY_TAIL_OPACITY), 0, 100) / 100.0f;

	/* obs_data_apply() copies user values only. Start with the effective
	 * defaults so a platform CJK font (and all other plugin defaults) actually
	 * reaches the private text_ft2_source, then overlay the user's values.
	 * Our own keys ride along and are ignored by text_ft2_source. Workers
	 * always measure/draw a single unwrapped run: wrapping and width are
	 * decided here, not by text_ft2 (whose custom_width would make its
	 * reported width the box width instead of the text width). */
	obs_data_t *child_settings = obs_data_get_defaults(settings);
	obs_data_apply(child_settings, settings);
	obs_data_set_int(child_settings, "custom_width", 0);
	obs_data_set_bool(child_settings, "word_wrap", false);
	obs_data_erase(child_settings, "text");
	cfg->child = child_settings;

	obs_data_t *font = obs_data_get_obj(child_settings, "font");
	cfg->font_size = font ? (int)obs_data_get_int(font, "size") : 48;
	struct dstr sig = {0};
	dstr_printf(&sig, "%s|%d|%d|%d", font ? obs_data_get_json(font) : "",
		    obs_data_get_bool(child_settings, "outline") ? 1 : 0,
		    obs_data_get_bool(child_settings, "drop_shadow") ? 1 : 0,
		    obs_data_has_user_value(child_settings, "antialiasing")
			    ? (obs_data_get_bool(child_settings, "antialiasing") ? 1 : 0)
			    : -1);
	obs_data_release(font);
	cfg->metrics_sig = bstrdup(sig.array ? sig.array : "");
	dstr_catf(&sig, "|%lld|%lld", obs_data_get_int(child_settings, "color1"),
		  obs_data_get_int(child_settings, "color2"));
	cfg->look_sig = bstrdup(sig.array ? sig.array : "");
	dstr_free(&sig);
	return cfg;
}

static void tea_captions_source_update(void *data, obs_data_t *settings)
{
	struct tea_captions_source *ctx = data;

	ctx->max_lines = (int)obs_data_get_int(settings, "max_lines");
	ctx->caption_align = (int)obs_data_get_int(settings, "caption_align");
	ctx->padding = (int)obs_data_get_int(settings, "padding");
	ctx->layout_mode = (int)obs_data_get_int(settings, "layout_mode");
	ctx->caption_width_px = (int)obs_data_get_int(settings, "caption_width_px");
	ctx->show_placeholder = obs_data_get_bool(settings, "show_placeholder");
	if (ctx->padding < 0)
		ctx->padding = 0;
	if (ctx->max_lines < 1)
		ctx->max_lines = 1;
	const bool has_schema_marker = obs_data_has_user_value(settings, TEA_LAYOUT_SCHEMA_KEY);
	const bool has_explicit_layout_mode = obs_data_has_user_value(settings, "layout_mode");
	ctx->layout_mode =
		tea_caption_layout_mode_from_settings(ctx->layout_mode, has_schema_marker, has_explicit_layout_mode);
	if (has_explicit_layout_mode && !has_schema_marker)
		obs_data_set_int(settings, TEA_LAYOUT_SCHEMA_KEY, TEA_LAYOUT_SCHEMA_VERSION);

	/* --- display-only settings: redraw, never reconnect --- */
	if (ctx->captions) {
		tea_caption_state_set_max_lines(ctx->captions, ctx->max_lines);
		tea_caption_state_set_stable_tail_lines(ctx->captions, obs_data_get_bool(settings, TEA_KEY_TAIL));
	}

	/* --- audio source: re-attaches the tap, the session keeps running --- */
	const char *audio_source_name = obs_data_get_string(settings, "audio_source_name");
	if (!tea_str_eq(ctx->applied_audio_source_name, audio_source_name) && ctx->tap) {
		obs_source_t *audio_src =
			(audio_source_name && audio_source_name[0]) ? obs_get_source_by_name(audio_source_name) : NULL;
		tea_audio_tap_set_source(ctx->tap, audio_src);
		if (audio_src)
			obs_source_release(audio_src);

		bfree(ctx->applied_audio_source_name);
		ctx->applied_audio_source_name = bstrdup(audio_source_name);
	}

	/* --- server connection: see tea_apply_connection() --- */
	tea_apply_connection(ctx, settings, false);

	struct tea_render_config *cfg = tea_config_build(ctx, settings);
	pthread_mutex_lock(&ctx->config_lock);
	tea_config_free(ctx->pending_config);
	ctx->pending_config = cfg;
	pthread_mutex_unlock(&ctx->config_lock);
}

/* ------------------------------------------------------------------------ */
/* Workers                                                                   */
/* ------------------------------------------------------------------------ */

static void tea_worker_updated(void *data, calldata_t *cd)
{
	(void)cd;
	struct tea_worker *w = data;
	os_atomic_set_long(&w->applied, os_atomic_load_long(&w->requested));
}

/* There is no portable public API to just ask "is source id X registered"
 * across the OBS versions this plugin targets, so we try to actually create
 * a private instance of each candidate id in turn. obs_source_create_private()
 * returns NULL (and logs its own warning) when the id isn't registered, which
 * is exactly the fallback signal we need. */
static obs_source_t *tea_create_text_child(struct tea_captions_source *ctx)
{
	if (ctx->text_ft2_id)
		return obs_source_create_private(ctx->text_ft2_id, "tea-live-subtitle-text", NULL);
	for (size_t i = 0; i < sizeof(k_text_ft2_ids) / sizeof(k_text_ft2_ids[0]); i++) {
		obs_source_t *child = obs_source_create_private(k_text_ft2_ids[i], "tea-live-subtitle-text", NULL);
		if (child) {
			ctx->text_ft2_id = k_text_ft2_ids[i];
			return child;
		}
	}
	return NULL;
}

static void tea_worker_open(struct tea_captions_source *ctx, int index)
{
	struct tea_worker *w = &ctx->workers[index];
	memset(w, 0, sizeof(*w));
	w->tex = -1;
	w->src = tea_create_text_child(ctx);
	if (w->src)
		signal_handler_connect(obs_source_get_signal_handler(w->src), "update", tea_worker_updated, w);
}

static void tea_worker_close(struct tea_worker *w)
{
	if (w->src) {
		signal_handler_disconnect(obs_source_get_signal_handler(w->src), "update", tea_worker_updated, w);
		obs_source_release(w->src);
		w->src = NULL;
	}
}

static uint8_t tea_worker_bit(const struct tea_captions_source *ctx, const struct tea_worker *w)
{
	return (uint8_t)(1u << (unsigned)(w - ctx->workers));
}

/* Approximate number of glyphs a text_ft2 atlas holds at this font size;
 * past that text_ft2 logs "Out of space" and stops drawing new glyphs. */
static uint32_t tea_atlas_capacity(int font_size)
{
	uint32_t cell = (uint32_t)(font_size > 0 ? font_size : 48) + 2u;
	uint32_t per_side = TEA_ATLAS_SIDE / cell;
	uint32_t capacity = per_side * per_side * 7u / 10u;
	return capacity > 150u ? capacity - 100u : 50u; /* text_ft2 pre-caches ~95 ASCII glyphs */
}

static void tea_worker_count_glyphs(struct tea_captions_source *ctx, struct tea_worker *w, const char *text)
{
	uint8_t bit = tea_worker_bit(ctx, w);
	size_t len = strlen(text);
	size_t pos = 0;
	while (pos < len) {
		uint32_t cp = tea_utf8_decode(text, len, &pos);
		tea_glyph_slot_t *slot = (tea_glyph_slot_t *)tea_glyph_cache_find(ctx->glyphs, cp);
		if (!slot) {
			w->glyphs++; /* being measured right now */
		} else if (!(slot->seen_by & bit)) {
			slot->seen_by |= bit;
			w->glyphs++;
		}
	}
}

static void tea_worker_assign(struct tea_captions_source *ctx, struct tea_worker *w, enum tea_job job, const char *text)
{
	obs_data_t *update = obs_data_create();
	if (w->look_rev != ctx->look_rev) {
		obs_data_apply(update, ctx->config->child);
		if (w->metrics_rev != ctx->metrics_rev)
			w->glyphs = 0; /* a font change makes text_ft2 start a fresh atlas */
		w->look_rev = ctx->look_rev;
		w->metrics_rev = ctx->metrics_rev;
	}
	obs_data_set_string(update, "text", text);
	os_atomic_inc_long(&w->requested);
	obs_source_update(w->src, update);
	obs_data_release(update);
	w->job = job;
	w->job_look_rev = ctx->look_rev;
	w->job_metrics_rev = ctx->metrics_rev;
	tea_worker_count_glyphs(ctx, w, text);
}

static bool tea_worker_done(struct tea_worker *w)
{
	return w->job != TEA_JOB_NONE && os_atomic_load_long(&w->applied) >= os_atomic_load_long(&w->requested);
}

static bool tea_metrics_ready(const struct tea_captions_source *ctx)
{
	return ctx->have_cal_one && ctx->have_cal_two && ctx->row_height > 0;
}

/* Probe / calibration results are read on the tick thread. */
static void tea_collect_measurements(struct tea_captions_source *ctx)
{
	for (int i = 0; i < TEA_WORKER_COUNT; i++) {
		struct tea_worker *w = &ctx->workers[i];
		if (!w->src || w->job == TEA_JOB_RASTER || !tea_worker_done(w))
			continue;
		const bool current = w->job_metrics_rev == ctx->metrics_rev;
		const uint32_t width = obs_source_get_width(w->src);
		const uint32_t height = obs_source_get_height(w->src);
		switch (w->job) {
		case TEA_JOB_PRIME:
			if (current) {
				w->primed_rev = ctx->metrics_rev;
				if (ctx->row_height == 0)
					ctx->row_height = height > 0 ? height : (uint32_t)ctx->config->font_size;
			}
			break;
		case TEA_JOB_CAL_ONE:
			if (current) {
				ctx->cal_one = width;
				ctx->have_cal_one = true;
			}
			break;
		case TEA_JOB_CAL_TWO:
			if (current) {
				ctx->cal_two = width;
				ctx->have_cal_two = true;
			}
			break;
		case TEA_JOB_PROBE:
			if (current && ctx->have_cal_one && ctx->have_cal_two) {
				tea_glyph_slot_t *slot = tea_glyph_cache_store(
					ctx->glyphs, w->cp, tea_glyph_advance_from_measure(width, ctx->outline_extra));
				if (slot)
					slot->seen_by |= tea_worker_bit(ctx, w);
			}
			break;
		default:
			break;
		}
		if (ctx->have_cal_one && ctx->have_cal_two)
			ctx->outline_extra = tea_outline_extra_from_measures(ctx->cal_one, ctx->cal_two);
		w->job = TEA_JOB_NONE;
		ctx->layout_dirty = true;
	}
}

static bool tea_job_in_flight(const struct tea_captions_source *ctx, enum tea_job job, uint32_t cp)
{
	for (int i = 0; i < TEA_WORKER_COUNT; i++) {
		const struct tea_worker *w = &ctx->workers[i];
		if (w->job == job && w->job_metrics_rev == ctx->metrics_rev && (job != TEA_JOB_PROBE || w->cp == cp))
			return true;
	}
	return false;
}

static void tea_queue_probe(struct tea_captions_source *ctx, uint32_t cp)
{
	if (tea_job_in_flight(ctx, TEA_JOB_PROBE, cp))
		return;
	for (int i = 0; i < ctx->probe_count; i++) {
		if (ctx->probe_queue[i] == cp)
			return;
	}
	if (ctx->probe_count < TEA_PROBE_QUEUE)
		ctx->probe_queue[ctx->probe_count++] = cp;
}

static int tea_next_raster(struct tea_captions_source *ctx)
{
	for (int i = 0; i < TEA_TEX_CACHE_SIZE; i++) {
		struct tea_tex *t = &ctx->texes[i];
		if (t->used && !t->ready && t->worker < 0 && t->look_rev == ctx->look_rev && t->stamp == ctx->frame_no)
			return i;
	}
	return -1;
}

static void tea_dispatch_jobs(struct tea_captions_source *ctx)
{
	for (int i = 0; i < TEA_WORKER_COUNT; i++) {
		struct tea_worker *w = &ctx->workers[i];
		if (!w->src || w->job != TEA_JOB_NONE)
			continue;
		if (w->primed_rev != ctx->metrics_rev) {
			tea_worker_assign(ctx, w, TEA_JOB_PRIME, TEA_PRIME_TEXT);
			continue;
		}
		if (!ctx->have_cal_one && !tea_job_in_flight(ctx, TEA_JOB_CAL_ONE, 0)) {
			tea_worker_assign(ctx, w, TEA_JOB_CAL_ONE, TEA_CAL_ONE);
			continue;
		}
		if (!ctx->have_cal_two && !tea_job_in_flight(ctx, TEA_JOB_CAL_TWO, 0)) {
			tea_worker_assign(ctx, w, TEA_JOB_CAL_TWO, TEA_CAL_TWO);
			continue;
		}
		if (!ctx->have_cal_one || !ctx->have_cal_two)
			continue;
		if (ctx->probe_count > 0) {
			uint32_t cp = ctx->probe_queue[0];
			memmove(&ctx->probe_queue[0], &ctx->probe_queue[1],
				sizeof(ctx->probe_queue[0]) * (size_t)(ctx->probe_count - 1));
			ctx->probe_count--;
			char utf8[5];
			tea_utf8_encode(cp, utf8);
			w->cp = cp;
			tea_worker_assign(ctx, w, TEA_JOB_PROBE, utf8);
			continue;
		}
		int tex = tea_next_raster(ctx);
		if (tex >= 0) {
			ctx->texes[tex].worker = i;
			w->tex = tex;
			tea_worker_assign(ctx, w, TEA_JOB_RASTER, ctx->texes[tex].text);
		}
	}
}

/* A worker whose atlas is nearly full is replaced by a fresh one while idle. */
static void tea_recycle_workers(struct tea_captions_source *ctx)
{
	const uint32_t capacity = tea_atlas_capacity(ctx->config->font_size);
	for (int i = 0; i < TEA_WORKER_COUNT; i++) {
		struct tea_worker *w = &ctx->workers[i];
		if (!w->src || w->job != TEA_JOB_NONE || w->glyphs < capacity)
			continue;
		uint8_t bit = tea_worker_bit(ctx, w);
		for (uint32_t s = 0; s < TEA_GLYPH_CACHE_SLOTS; s++)
			ctx->glyphs->slots[s].seen_by &= (uint8_t)~bit;
		tea_worker_close(w);
		tea_worker_open(ctx, i);
	}
}

/* ------------------------------------------------------------------------ */
/* Texture cache                                                             */
/* ------------------------------------------------------------------------ */

static void tea_tex_release(struct tea_tex *t)
{
	if (t->tr)
		gs_texrender_destroy(t->tr); /* caller holds the graphics context */
	bfree(t->text);
	memset(t, 0, sizeof(*t));
	t->worker = -1;
}

static int tea_tex_acquire(struct tea_captions_source *ctx, uint64_t key, const char *text, size_t start, size_t end)
{
	int free_slot = -1;
	for (int i = 0; i < TEA_TEX_CACHE_SIZE; i++) {
		struct tea_tex *t = &ctx->texes[i];
		if (!t->used) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (t->key == key && t->start == start && t->end == end && t->look_rev == ctx->look_rev) {
			t->stamp = ctx->frame_no;
			return i;
		}
	}
	if (free_slot < 0)
		return -1;
	struct tea_tex *t = &ctx->texes[free_slot];
	memset(t, 0, sizeof(*t));
	t->used = true;
	t->key = key;
	t->start = start;
	t->end = end;
	t->look_rev = ctx->look_rev;
	t->worker = -1;
	t->stamp = ctx->frame_no;
	t->text = bmalloc(end - start + 1);
	memcpy(t->text, text + start, end - start);
	t->text[end - start] = '\0';
	return free_slot;
}

static void tea_stamp_frame(struct tea_captions_source *ctx, const struct tea_frame *frame)
{
	for (int i = 0; i < frame->piece_count; i++) {
		int tex = frame->pieces[i].tex;
		if (tex >= 0)
			ctx->texes[tex].stamp = ctx->frame_no;
	}
}

static void tea_gc_textures(struct tea_captions_source *ctx)
{
	if (ctx->have_committed)
		tea_stamp_frame(ctx, &ctx->committed);
	if (ctx->have_pending)
		tea_stamp_frame(ctx, &ctx->pending);
	bool entered = false;
	for (int i = 0; i < TEA_TEX_CACHE_SIZE; i++) {
		struct tea_tex *t = &ctx->texes[i];
		if (!t->used || t->worker >= 0 || t->stamp == ctx->frame_no)
			continue;
		if (!entered) {
			obs_enter_graphics();
			entered = true;
		}
		tea_tex_release(t);
	}
	if (entered)
		obs_leave_graphics();
}

/* Raster jobs finish on the graphics thread inside video_render(). */
static void tea_finish_rasters(struct tea_captions_source *ctx)
{
	for (int i = 0; i < TEA_WORKER_COUNT; i++) {
		struct tea_worker *w = &ctx->workers[i];
		if (!w->src || w->job != TEA_JOB_RASTER || !tea_worker_done(w))
			continue;
		struct tea_tex *t = w->tex >= 0 ? &ctx->texes[w->tex] : NULL;
		if (t && t->used && t->worker == i) {
			t->worker = -1;
			if (w->job_look_rev == ctx->look_rev && t->look_rev == ctx->look_rev) {
				const uint32_t cx = obs_source_get_width(w->src);
				const uint32_t cy = obs_source_get_height(w->src);
				if (cx > 0 && cy > 0) {
					if (!t->tr)
						t->tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
					else
						gs_texrender_reset(t->tr);
					if (t->tr && gs_texrender_begin(t->tr, cx, cy)) {
						struct vec4 clear;
						vec4_zero(&clear);
						gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
						gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
						gs_blend_state_push();
						gs_reset_blend_state();
						obs_source_video_render(w->src);
						gs_blend_state_pop();
						gs_texrender_end(t->tr);
					}
				}
				t->w = cx;
				t->h = cy;
				t->ready = true;
			}
		}
		w->job = TEA_JOB_NONE;
		w->tex = -1;
		ctx->layout_dirty = true;
	}
}

/* ------------------------------------------------------------------------ */
/* Lines                                                                     */
/* ------------------------------------------------------------------------ */

static void tea_line_free(struct tea_rline *line)
{
	bfree(line->text);
	memset(line, 0, sizeof(*line));
}

static int tea_line_find(struct tea_captions_source *ctx, uint64_t key)
{
	for (int i = 0; i < TEA_MAX_LINES; i++) {
		if (ctx->lines[i].used && ctx->lines[i].meta.key == key)
			return i;
	}
	return -1;
}

static int tea_line_alloc(struct tea_captions_source *ctx)
{
	for (int i = 0; i < TEA_MAX_LINES; i++) {
		if (!ctx->lines[i].used)
			return i;
	}
	return -1;
}

/* Joins committed text and tail into one displayed string; newlines become
 * spaces (every row is a single text_ft2 line). */
static char *tea_display_text(const char *text, const char *tail, size_t *len_out, size_t *locked_out)
{
	size_t locked = text ? strlen(text) : 0;
	size_t extra = tail ? strlen(tail) : 0;
	char *out = bmalloc(locked + extra + 1);
	if (locked)
		memcpy(out, text, locked);
	if (extra)
		memcpy(out + locked, tail, extra);
	out[locked + extra] = '\0';
	for (size_t i = 0; i < locked + extra; i++) {
		if (out[i] == '\n' || out[i] == '\r')
			out[i] = ' ';
	}
	*len_out = locked + extra;
	*locked_out = locked;
	return out;
}

static void tea_sync_line(struct tea_captions_source *ctx, bool *keep, uint64_t key, const char *text, const char *tail,
			  bool persistent, uint64_t now)
{
	size_t len = 0, locked = 0;
	char *display = tea_display_text(text, tail, &len, &locked);
	int idx = tea_line_find(ctx, key);
	if (idx < 0) {
		idx = tea_line_alloc(ctx);
		if (idx < 0) {
			bfree(display);
			return;
		}
		struct tea_rline *line = &ctx->lines[idx];
		tea_display_line_init(&line->meta, key, len, locked, now);
		line->used = true;
		line->text = display;
	} else {
		struct tea_rline *line = &ctx->lines[idx];
		tea_display_line_observe(&line->meta, line->text, line->meta.len, display, len, locked, now,
					 persistent ? 0 : ctx->config->pause_ms);
		bfree(line->text);
		line->text = display;
	}
	ctx->lines[idx].meta.persistent = persistent;
	keep[idx] = true;
	if (ctx->order_count < TEA_MAX_LINES)
		ctx->order[ctx->order_count++] = idx;
}

static void tea_sync_lines(struct tea_captions_source *ctx, uint64_t now)
{
	if (!ctx->captions)
		return;
	uint64_t rev = tea_caption_state_revision(ctx->captions);
	if (rev == ctx->caption_rev_seen && !ctx->force_snapshot)
		return;
	ctx->force_snapshot = false;
	ctx->caption_rev_seen = rev;

	tea_caption_snapshot_t snap;
	tea_caption_state_snapshot(ctx->captions, ctx->config->tail, &snap);

	/* Lines that left the snapshot are dropped first, so their slots can be
	 * reused by lines that just arrived. */
	bool keep[TEA_MAX_LINES] = {false};
	bool any_text = false;
	for (int i = 0; i < snap.count; i++) {
		const tea_caption_snapshot_line_t *s = &snap.lines[i];
		if (s->text[0] || (s->tail && s->tail[0]))
			any_text = true;
		int idx = tea_line_find(ctx, s->key);
		if (idx >= 0)
			keep[idx] = true;
	}
	/* The source (not the state machine) owns the neutral waiting prompt;
	 * connection diagnostics never reach this path. */
	const char *placeholder = tea_caption_select_display_text(any_text ? "x" : "", ctx->config->show_placeholder,
								  obs_module_text("TeaLiveSubtitle.PlaceholderText"));
	const bool use_placeholder = !any_text && placeholder[0];
	if (use_placeholder) {
		int idx = tea_line_find(ctx, TEA_PLACEHOLDER_KEY);
		if (idx >= 0)
			keep[idx] = true;
	}
	for (int i = 0; i < TEA_MAX_LINES; i++) {
		if (ctx->lines[i].used && !keep[i])
			tea_line_free(&ctx->lines[i]);
	}

	memset(keep, 0, sizeof(keep));
	ctx->order_count = 0;
	for (int i = 0; i < snap.count; i++) {
		const tea_caption_snapshot_line_t *s = &snap.lines[i];
		tea_sync_line(ctx, keep, s->key, s->text, s->tail, false, now);
	}
	if (use_placeholder)
		tea_sync_line(ctx, keep, TEA_PLACEHOLDER_KEY, placeholder, NULL, true, now);
	tea_caption_snapshot_free(&snap);
	ctx->layout_dirty = true;
}

/* Lines idle for longer than delay + fade have faded out completely. */
static void tea_expire_lines(struct tea_captions_source *ctx, uint64_t now)
{
	const struct tea_render_config *cfg = ctx->config;
	if (!cfg->fade_out)
		return;
	for (int i = 0; i < TEA_MAX_LINES; i++) {
		struct tea_rline *line = &ctx->lines[i];
		if (!line->used || line->meta.persistent || !tea_display_line_has_visible_text(&line->meta))
			continue;
		if (tea_fade_out_done(true, now, line->meta.changed_ns, cfg->fade_delay_ms, cfg->fade_ms)) {
			tea_display_line_retire(&line->meta);
			ctx->layout_dirty = true;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Layout                                                                    */
/* ------------------------------------------------------------------------ */

static tea_caption_frame_t tea_frame_geometry(const struct tea_captions_source *ctx)
{
	const struct tea_render_config *cfg = ctx->config;
	uint32_t row_height = ctx->row_height > 0 ? ctx->row_height
						  : (uint32_t)(cfg->font_size > 0 ? cfg->font_size : 48);
	return tea_caption_frame_resolve(cfg->layout_mode, cfg->caption_width, cfg->padding, cfg->legacy_custom_width,
					 cfg->legacy_word_wrap, ctx->outline_extra, row_height, cfg->bg,
					 cfg->bg_padding);
}

static void tea_publish_size(struct tea_captions_source *ctx, uint32_t width, uint32_t height)
{
	os_atomic_set_long(&ctx->out_width, (long)width);
	os_atomic_set_long(&ctx->out_height, (long)height);
}

struct tea_layout_row {
	int line;
	tea_wrap_row_t row;
};

static uint64_t tea_chunk_born(const tea_display_line_t *meta, size_t at)
{
	if (meta->chunk_count == 0)
		return meta->changed_ns;
	return meta->chunks[tea_display_chunk_at(meta, at)].born_ns;
}

/* Where the piece that starts at `from` ends: the next chunk start or the
 * committed/tail boundary, whichever comes first before `row_end`. */
static size_t tea_piece_end(const tea_display_line_t *meta, size_t from, size_t row_end)
{
	size_t end = row_end;
	for (int c = 0; c < meta->chunk_count; c++) {
		if (meta->chunks[c].start > from && meta->chunks[c].start < end)
			end = meta->chunks[c].start;
	}
	if (meta->locked_len > from && meta->locked_len < end)
		end = meta->locked_len;
	return end;
}

static void tea_layout(struct tea_captions_source *ctx)
{
	const struct tea_render_config *cfg = ctx->config;
	ctx->layout_dirty = false;
	if (!tea_metrics_ready(ctx))
		return;

	/* 1. every glyph on screen must have a known advance */
	bool missing = false;
	for (int o = 0; o < ctx->order_count; o++) {
		const struct tea_rline *line = &ctx->lines[ctx->order[o]];
		uint32_t cps[TEA_PROBE_QUEUE];
		int n = tea_glyph_cache_missing(ctx->glyphs, line->text, line->meta.visible_from, line->meta.len, cps,
						TEA_PROBE_QUEUE);
		for (int i = 0; i < n; i++)
			tea_queue_probe(ctx, cps[i]);
		missing = missing || n > 0;
	}
	if (missing)
		return; /* re-run when the measurements arrive */

	/* 2. wrap */
	const tea_caption_frame_t geo = tea_frame_geometry(ctx);
	static struct tea_layout_row rows[TEA_MAX_LINES * TEA_WRAP_RING]; /* graphics thread only */
	int row_count = 0;
	for (int o = 0; o < ctx->order_count; o++) {
		struct tea_rline *line = &ctx->lines[ctx->order[o]];
		tea_wrap_row_t ring[TEA_WRAP_RING];
		tea_wrap_row_t ordered[TEA_WRAP_RING];
		int total = tea_wrap_line(&line->meta, line->text, geo.wrap_width, ctx->outline_extra, geo.word_wrap,
					  ctx->glyphs, ring, TEA_WRAP_RING);
		if (total == TEA_WRAP_MISSING_GLYPH)
			return;
		int kept = tea_wrap_rows_in_order(ring, total, TEA_WRAP_RING, ordered);
		if (total > kept && kept > 0)
			tea_display_line_evict_through(&line->meta, ordered[0].start);
		for (int r = 0; r < kept; r++) {
			rows[row_count].line = ctx->order[o];
			rows[row_count].row = ordered[r];
			row_count++;
		}
	}

	/* 3. screen row limit: whole rows leave, oldest first */
	const int limit = cfg->max_rows > 0 ? cfg->max_rows : TEA_MAX_DRAW_ROWS;
	const int evict = tea_rows_to_evict(row_count, limit);
	for (int i = 0; i < evict; i++)
		tea_display_line_evict_through(&ctx->lines[rows[i].line].meta, rows[i].row.next);
	const struct tea_layout_row *visible = rows + evict;
	const int visible_count = row_count - evict;

	/* 4. geometry */
	uint32_t widest = 0;
	for (int i = 0; i < visible_count; i++) {
		if (visible[i].row.width > widest)
			widest = visible[i].row.width;
	}
	const uint32_t content = tea_caption_frame_content_width(&geo, cfg->layout_mode, cfg->caption_width, widest);
	struct tea_frame *frame = &ctx->pending;
	memset(frame, 0, sizeof(*frame));
	frame->width = tea_caption_frame_outer_width(&geo, cfg->layout_mode, cfg->caption_width, widest);
	frame->height = tea_caption_frame_outer_height(&geo, visible_count);

	/* 5. pieces */
	bool ready = true;
	for (int i = 0; i < visible_count && frame->row_count < TEA_MAX_DRAW_ROWS; i++) {
		const struct tea_rline *line = &ctx->lines[visible[i].line];
		const tea_wrap_row_t *wr = &visible[i].row;
		struct tea_draw_row *row = &frame->rows[frame->row_count];
		row->x = (int32_t)(geo.inset_x + tea_row_offset_x(cfg->align, content, wr->width));
		row->y = (int32_t)tea_caption_frame_row_y(&geo, frame->row_count);
		row->w = wr->width;
		row->h = geo.row_height;
		row->changed = line->meta.changed_ns;
		row->persistent = line->meta.persistent;
		row->born = UINT64_MAX;
		const bool has_tail = line->meta.locked_len < line->meta.len;
		size_t from = wr->start;
		while (from < wr->end && frame->piece_count < TEA_MAX_DRAW_PIECES) {
			size_t to = tea_piece_end(&line->meta, from, wr->end);
			uint32_t advance = 0;
			tea_text_advance(ctx->glyphs, line->text, wr->start, from, &advance);
			struct tea_draw_piece *piece = &frame->pieces[frame->piece_count++];
			piece->row = frame->row_count;
			piece->x = (int32_t)advance;
			piece->born = tea_chunk_born(&line->meta, from);
			piece->tail = has_tail && from >= line->meta.locked_len;
			piece->tex = tea_tex_acquire(ctx, line->meta.key, line->text, from, to);
			/* A full texture cache only drops this piece; it never
			 * blocks the whole frame from being shown. */
			if (piece->tex >= 0 && !ctx->texes[piece->tex].ready)
				ready = false;
			if (piece->born < row->born)
				row->born = piece->born;
			from = to;
		}
		if (row->born == UINT64_MAX)
			row->born = line->meta.changed_ns;
		frame->row_count++;
	}

	/* 6. show it once every texture exists; until then the previous frame
	 *    stays on screen (no half-drawn rows) */
	if (ready) {
		ctx->committed = *frame;
		ctx->have_committed = true;
		ctx->have_pending = false;
		tea_publish_size(ctx, frame->width, frame->height);
	} else {
		ctx->have_pending = true;
	}
}

/* ------------------------------------------------------------------------ */
/* Source callbacks                                                          */
/* ------------------------------------------------------------------------ */

static void tea_take_config(struct tea_captions_source *ctx)
{
	pthread_mutex_lock(&ctx->config_lock);
	struct tea_render_config *next = ctx->pending_config;
	ctx->pending_config = NULL;
	pthread_mutex_unlock(&ctx->config_lock);
	if (!next)
		return;

	struct tea_render_config *old = ctx->config;
	const bool metrics_changed = !old || strcmp(old->metrics_sig, next->metrics_sig) != 0;
	const bool look_changed = !old || strcmp(old->look_sig, next->look_sig) != 0;
	const bool resync = !old || old->tail != next->tail || old->show_placeholder != next->show_placeholder;
	if (metrics_changed) {
		ctx->metrics_rev++;
		tea_glyph_cache_clear(ctx->glyphs);
		ctx->have_cal_one = false;
		ctx->have_cal_two = false;
		ctx->row_height = 0;
		ctx->outline_extra = 0;
		ctx->probe_count = 0;
	}
	if (look_changed)
		ctx->look_rev++;
	ctx->config = next;
	tea_config_free(old);
	ctx->force_snapshot = ctx->force_snapshot || resync;
	ctx->layout_dirty = true;
	if (!ctx->have_committed) {
		tea_caption_frame_t geo = tea_frame_geometry(ctx);
		tea_publish_size(ctx, tea_caption_frame_outer_width(&geo, next->layout_mode, next->caption_width, 0),
				 tea_caption_frame_outer_height(&geo, 0));
	}
}

static void tea_captions_source_video_tick(void *data, float seconds)
{
	(void)seconds;
	struct tea_captions_source *ctx = data;
	tea_take_config(ctx);
	if (!ctx->config)
		return;
	ctx->frame_no++;
	const uint64_t now = os_gettime_ns();

	tea_collect_measurements(ctx);
	tea_sync_lines(ctx, now);
	tea_expire_lines(ctx, now);
	if (ctx->layout_dirty)
		tea_layout(ctx);
	else if (ctx->have_pending)
		tea_stamp_frame(ctx, &ctx->pending);
	tea_dispatch_jobs(ctx);
	tea_gc_textures(ctx);
	tea_recycle_workers(ctx);
}

static float tea_row_alpha(const struct tea_render_config *cfg, const struct tea_draw_row *row, uint64_t now)
{
	if (row->persistent)
		return 1.0f;
	return tea_fade_out_alpha(cfg->fade_out, now, row->changed, cfg->fade_delay_ms, cfg->fade_ms);
}

static void tea_draw_backgrounds(struct tea_captions_source *ctx, uint64_t now)
{
	const struct tea_render_config *cfg = ctx->config;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	const float pad = (float)cfg->bg_padding;

	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	for (int i = 0; i < ctx->committed.row_count; i++) {
		const struct tea_draw_row *row = &ctx->committed.rows[i];
		float alpha =
			tea_row_alpha(cfg, row, now) * tea_fade_in_alpha(cfg->fade_in, now, row->born, cfg->fade_ms);
		if (alpha <= 0.0f || row->w == 0)
			continue;
		struct vec4 c;
		vec4_from_rgba(&c, cfg->bg_color);
		c.w *= alpha;
		gs_effect_set_vec4(color, &c);
		gs_matrix_push();
		gs_matrix_translate3f((float)row->x - pad, (float)row->y - pad, 0.0f);
		size_t passes = gs_technique_begin(tech);
		for (size_t p = 0; p < passes; p++) {
			if (gs_technique_begin_pass(tech, p)) {
				gs_draw_sprite(NULL, 0, row->w + (uint32_t)cfg->bg_padding * 2u,
					       row->h + (uint32_t)cfg->bg_padding * 2u);
				gs_technique_end_pass(tech);
			}
		}
		gs_technique_end(tech);
		gs_matrix_pop();
	}
}

static void tea_draw_pieces(struct tea_captions_source *ctx, uint64_t now)
{
	const struct tea_render_config *cfg = ctx->config;
	gs_effect_t *effect = ctx->fade_effect ? ctx->fade_effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = ctx->fade_effect ? ctx->fade_image : gs_effect_get_param_by_name(effect, "image");

	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	for (int i = 0; i < ctx->committed.piece_count; i++) {
		const struct tea_draw_piece *piece = &ctx->committed.pieces[i];
		if (piece->tex < 0)
			continue;
		const struct tea_tex *t = &ctx->texes[piece->tex];
		if (!t->used || !t->ready || !t->tr || t->w == 0 || t->h == 0)
			continue;
		const struct tea_draw_row *row = &ctx->committed.rows[piece->row];
		float alpha =
			tea_row_alpha(cfg, row, now) * tea_fade_in_alpha(cfg->fade_in, now, piece->born, cfg->fade_ms);
		if (piece->tail)
			alpha *= cfg->tail_opacity;
		if (alpha <= 0.0f)
			continue;
		if (!ctx->fade_effect && alpha < 1.0f)
			continue; /* no opacity support without our effect: never draw a half-faded row opaque */
		gs_texture_t *tex = gs_texrender_get_texture(t->tr);
		if (!tex)
			continue;
		gs_effect_set_texture(image, tex);
		if (ctx->fade_effect)
			gs_effect_set_float(ctx->fade_opacity, alpha);
		gs_matrix_push();
		gs_matrix_translate3f((float)(row->x + piece->x), (float)row->y, 0.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, t->w, t->h);
		gs_matrix_pop();
	}
}

static void tea_captions_source_video_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	struct tea_captions_source *ctx = data;
	if (!ctx->config)
		return;
	tea_finish_rasters(ctx);
	if (!ctx->have_committed)
		return;
	const uint64_t now = os_gettime_ns();
	gs_blend_state_push();
	if (ctx->config->bg)
		tea_draw_backgrounds(ctx, now);
	tea_draw_pieces(ctx, now);
	gs_blend_state_pop();
}

static uint32_t tea_captions_source_get_width(void *data)
{
	struct tea_captions_source *ctx = data;
	return (uint32_t)os_atomic_load_long(&ctx->out_width);
}

static uint32_t tea_captions_source_get_height(void *data)
{
	struct tea_captions_source *ctx = data;
	return (uint32_t)os_atomic_load_long(&ctx->out_height);
}

static void *tea_captions_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct tea_captions_source *ctx = bzalloc(sizeof(struct tea_captions_source));
	ctx->source = source;
	ctx->applied_server_port = -1;
	ctx->applied_stable_captions = -1;
	pthread_mutex_init(&ctx->conn_lock, NULL);
	pthread_mutex_init(&ctx->config_lock, NULL);
	ctx->glyphs = bzalloc(sizeof(tea_glyph_cache_t));
	for (int i = 0; i < TEA_TEX_CACHE_SIZE; i++)
		ctx->texes[i].worker = -1;

	for (int i = 0; i < TEA_WORKER_COUNT; i++)
		tea_worker_open(ctx, i);
	if (!ctx->text_ft2_id) {
		obs_log(LOG_ERROR, "neither 'text_ft2_source_v2' nor 'text_ft2_source' could be created; "
				   "is the text-freetype2 plugin missing? Captions will not render.");
	}

	obs_enter_graphics();
	char *error = NULL;
	ctx->fade_effect = gs_effect_create(k_fade_effect, "tea-live-subtitle-fade.effect", &error);
	if (ctx->fade_effect) {
		ctx->fade_image = gs_effect_get_param_by_name(ctx->fade_effect, "image");
		ctx->fade_opacity = gs_effect_get_param_by_name(ctx->fade_effect, "opacity");
	} else {
		obs_log(LOG_WARNING, "fade effect failed to compile (%s); fades are disabled",
			error ? error : "unknown error");
	}
	bfree(error);
	obs_leave_graphics();

	ctx->captions = tea_caption_state_create();
	ctx->tap = tea_audio_tap_create();
	ctx->client = tea_asr_client_create(ctx->tap, ctx->captions);

	tea_captions_source_update(ctx, settings);
	tea_registry_add(ctx);
	return ctx;
}

static void tea_captions_source_destroy(void *data)
{
	struct tea_captions_source *ctx = data;

	tea_registry_remove(ctx);

	if (ctx->client) {
		tea_asr_client_stop(ctx->client);
		tea_asr_client_destroy(ctx->client);
		ctx->client = NULL;
	}
	if (ctx->tap) {
		tea_audio_tap_destroy(ctx->tap);
		ctx->tap = NULL;
	}
	if (ctx->captions) {
		tea_caption_state_destroy(ctx->captions);
		ctx->captions = NULL;
	}

	for (int i = 0; i < TEA_WORKER_COUNT; i++)
		tea_worker_close(&ctx->workers[i]);

	obs_enter_graphics();
	for (int i = 0; i < TEA_TEX_CACHE_SIZE; i++) {
		if (ctx->texes[i].used)
			tea_tex_release(&ctx->texes[i]);
	}
	if (ctx->fade_effect)
		gs_effect_destroy(ctx->fade_effect);
	obs_leave_graphics();

	for (int i = 0; i < TEA_MAX_LINES; i++) {
		if (ctx->lines[i].used)
			tea_line_free(&ctx->lines[i]);
	}
	tea_config_free(ctx->config);
	tea_config_free(ctx->pending_config);
	bfree(ctx->glyphs);
	pthread_mutex_destroy(&ctx->conn_lock);
	pthread_mutex_destroy(&ctx->config_lock);

	bfree(ctx->applied_audio_source_name);
	bfree(ctx->applied_server_host);
	bfree(ctx->applied_token_path);
	bfree(ctx);
}

static void tea_captions_source_get_defaults(obs_data_t *settings)
{
	/* OBS calls get_defaults once while building the defaults object and again
	 * while creating a source from that object. The second call can be
	 * distinguished by the existing defaults from the first call. A loaded
	 * legacy scene has user values but no defaults at this point, so it stays
	 * unmarked and is migrated to Auto by tea_captions_source_update(). */
	const bool from_new_source_defaults = obs_data_has_default_value(settings, "max_lines") ||
					      obs_data_has_default_value(settings, "font");

	obs_data_set_default_int(settings, "max_lines", 2);
	obs_data_set_default_int(settings, "caption_align", 1);
	obs_data_set_default_int(settings, "padding", 10);
	/* New sources use a stable 960px outer box. Legacy scenes are identified by
	 * the absence of a user layout marker in update(), so their effective mode
	 * remains Auto even though this is the current UI default. */
	obs_data_set_default_int(settings, "layout_mode", TEA_LAYOUT_MODE_FIXED);
	obs_data_set_default_int(settings, "caption_width_px", TEA_LAYOUT_DEFAULT_WIDTH_PX);
	obs_data_set_default_int(settings, TEA_LAYOUT_SCHEMA_KEY, TEA_LAYOUT_SCHEMA_VERSION);
	obs_data_set_default_bool(settings, "show_placeholder", true);
	if (from_new_source_defaults && !obs_data_has_user_value(settings, TEA_LAYOUT_SCHEMA_KEY)) {
		obs_data_set_int(settings, TEA_LAYOUT_SCHEMA_KEY, TEA_LAYOUT_SCHEMA_VERSION);
		if (!obs_data_has_user_value(settings, "layout_mode"))
			obs_data_set_int(settings, "layout_mode", TEA_LAYOUT_MODE_FIXED);
	}

	/* Per-line renderer. The defaults are the "looks exactly like before"
	 * values; only a brand-new source (same detection as above) gets the
	 * recommended live-subtitle values written as its own settings. */
	obs_data_set_default_int(settings, TEA_KEY_PAUSE_MS, TEA_DEFAULT_PAUSE_MS);
	obs_data_set_default_bool(settings, TEA_KEY_FADE_IN, false);
	obs_data_set_default_bool(settings, TEA_KEY_FADE_OUT, false);
	obs_data_set_default_int(settings, TEA_KEY_FADE_MS, TEA_DEFAULT_FADE_MS);
	obs_data_set_default_int(settings, TEA_KEY_FADE_DELAY_MS, TEA_DEFAULT_FADE_DELAY_MS);
	obs_data_set_default_int(settings, TEA_KEY_MAX_ROWS, 0);
	obs_data_set_default_bool(settings, TEA_KEY_BG, false);
	obs_data_set_default_int(settings, TEA_KEY_BG_COLOR, TEA_DEFAULT_BG_COLOR);
	obs_data_set_default_int(settings, TEA_KEY_BG_PADDING, TEA_DEFAULT_BG_PADDING);
	obs_data_set_default_bool(settings, TEA_KEY_TAIL, false);
	obs_data_set_default_int(settings, TEA_KEY_TAIL_OPACITY, TEA_DEFAULT_TAIL_OPACITY);
	if (from_new_source_defaults && !obs_data_has_user_value(settings, TEA_RENDER_SCHEMA_KEY)) {
		obs_data_set_int(settings, TEA_RENDER_SCHEMA_KEY, TEA_RENDER_SCHEMA_VERSION);
		obs_data_set_bool(settings, TEA_KEY_FADE_IN, true);
		obs_data_set_bool(settings, TEA_KEY_FADE_OUT, true);
		obs_data_set_int(settings, TEA_KEY_PAUSE_MS, TEA_NEW_SOURCE_PAUSE_MS);
		obs_data_set_int(settings, TEA_KEY_MAX_ROWS, TEA_NEW_SOURCE_MAX_ROWS);
	}

	obs_data_set_default_string(settings, "audio_source_name", "");
	obs_data_set_default_string(settings, "server_host", "127.0.0.1");
	obs_data_set_default_int(settings, "server_port", 8327);
	obs_data_set_default_string(settings, "token_path", "");
	/* Append-only captions (transcript.stable) are what live subtitles need:
	 * on by default, silently unused when the server does not offer them. */
	obs_data_set_default_bool(settings, "stable_captions", true);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_default_string(font_obj, "face", TEA_DEFAULT_FONT_FACE);
	obs_data_set_default_string(font_obj, "style", TEA_DEFAULT_FONT_STYLE);
	obs_data_set_default_int(font_obj, "size", 48);
	obs_data_set_default_int(font_obj, "flags", 0);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	/* text_ft2 paints glyphs with a vertical gradient from color1 (top) to
	 * color2 (bottom); its outline and shadow are always black. The same
	 * value in both gives a solid colour. Legacy default kept: white top,
	 * black bottom. */
	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF); /* gradient top: opaque white */
	obs_data_set_default_int(settings, "color2", 0xFF000000); /* gradient bottom: opaque black */
	obs_data_set_default_bool(settings, "outline", true);
	obs_data_set_default_bool(settings, "drop_shadow", false);
	obs_data_set_default_bool(settings, "word_wrap", true);
	obs_data_set_default_int(settings, "custom_width", 0);
}

static bool tea_enum_audio_sources_cb(void *param, obs_source_t *source)
{
	obs_property_t *list = param;
	uint32_t flags = obs_source_get_output_flags(source);
	if (flags & OBS_SOURCE_AUDIO)
		obs_property_list_add_string(list, obs_source_get_name(source), obs_source_get_name(source));
	return true;
}

/* Shows the "Auto mode never wraps" hint only where it applies, and only the
 * width settings that the selected mode actually uses. */
static bool tea_layout_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)property;
	const bool has_schema_marker = obs_data_has_user_value(settings, TEA_LAYOUT_SCHEMA_KEY);
	const bool has_explicit_layout_mode = obs_data_has_user_value(settings, "layout_mode");
	const int mode = tea_caption_layout_mode_from_settings((int)obs_data_get_int(settings, "layout_mode"),
							       has_schema_marker, has_explicit_layout_mode);
	const bool unbounded = mode == TEA_LAYOUT_MODE_AUTO &&
			       obs_data_get_int(settings, "custom_width") < TEA_LEGACY_MIN_CUSTOM_WIDTH;
	obs_property_set_visible(obs_properties_get(props, "auto_mode_hint"), unbounded);
	obs_property_set_visible(obs_properties_get(props, "custom_width"), mode == TEA_LAYOUT_MODE_AUTO);
	obs_property_set_visible(obs_properties_get(props, "word_wrap"), mode == TEA_LAYOUT_MODE_AUTO);
	obs_property_set_visible(obs_properties_get(props, "caption_width_px"), mode == TEA_LAYOUT_MODE_FIXED);
	return true;
}

static obs_properties_t *tea_captions_source_get_properties(void *data)
{
	struct tea_captions_source *ctx = data;
	obs_properties_t *props = obs_properties_create();

	/* A live Properties window: connection edits wait for Apply / close. */
	if (ctx && ctx->source) {
		os_atomic_inc_long(&ctx->properties_open);
		obs_properties_set_param(props, obs_source_get_weak_source(ctx->source), tea_properties_closed);
	}

	/* --- connection settings (applied with the button or when the window closes) --- */
	obs_property_t *audio_list = obs_properties_add_list(props, "audio_source_name",
							     obs_module_text("TeaLiveSubtitle.Prop.AudioSource"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_list, obs_module_text("TeaLiveSubtitle.Prop.AudioSource.None"), "");
	obs_enum_sources(tea_enum_audio_sources_cb, audio_list);

	obs_properties_add_text(props, "server_host", obs_module_text("TeaLiveSubtitle.Prop.ServerHost"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "server_port", obs_module_text("TeaLiveSubtitle.Prop.ServerPort"), 1, 65535, 1);
	obs_properties_add_text(props, "token_path", obs_module_text("TeaLiveSubtitle.Prop.TokenPath"),
				OBS_TEXT_DEFAULT);
	obs_property_t *stable_prop = obs_properties_add_bool(
		props, "stable_captions",
		tea_text_or("TeaLiveSubtitle.Prop.StableCaptions", "Stable captions (append-only, no rewrites)"));
	obs_property_set_long_description(
		stable_prop, tea_text_or("TeaLiveSubtitle.Prop.StableCaptions.Tooltip",
					 "Show only text the server has committed (transcript.stable), so shown "
					 "characters are never rewritten. Off: show the live partial preview, which "
					 "may change. Used only when the server supports it."));
	obs_properties_add_text(props, "connection_hint",
				tea_text_or("TeaLiveSubtitle.Prop.ConnectionHint",
					    "Server, port, token and stable-caption changes take effect when you "
					    "press \"Apply connection settings\" or close this window. Everything "
					    "else applies immediately."),
				OBS_TEXT_INFO);
	obs_properties_add_button(props, "apply_connection",
				  tea_text_or("TeaLiveSubtitle.Prop.ApplyConnection", "Apply connection settings"),
				  tea_apply_connection_clicked);

	/* --- text appearance (text_ft2's own settings, described as they behave) --- */
	obs_properties_add_font(props, "font", obs_module_text("TeaLiveSubtitle.Prop.Font"));
	obs_properties_add_color(props, "color1",
				 tea_text_or("TeaLiveSubtitle.Prop.Color1", "Text gradient: top color"));
	obs_properties_add_color(props, "color2",
				 tea_text_or("TeaLiveSubtitle.Prop.Color2", "Text gradient: bottom color"));
	obs_property_t *outline_prop =
		obs_properties_add_bool(props, "outline", tea_text_or("TeaLiveSubtitle.Prop.Outline", "Black outline"));
	obs_property_set_long_description(
		outline_prop, tea_text_or("TeaLiveSubtitle.Prop.Outline.Tooltip",
					  "OBS's text engine always draws the outline in black; it has no outline "
					  "color setting."));
	obs_properties_add_bool(props, "drop_shadow",
				tea_text_or("TeaLiveSubtitle.Prop.DropShadow", "Black drop shadow"));

	obs_properties_add_bool(props, TEA_KEY_BG,
				tea_text_or("TeaLiveSubtitle.Prop.TextBackground", "Text background box"));
	obs_properties_add_color_alpha(props, TEA_KEY_BG_COLOR,
				       tea_text_or("TeaLiveSubtitle.Prop.TextBackgroundColor",
						   "Text background color"));
	obs_properties_add_int(props, TEA_KEY_BG_PADDING,
			       tea_text_or("TeaLiveSubtitle.Prop.TextBackgroundPadding",
					   "Text background padding (pixels)"),
			       0, 200, 1);

	/* --- layout --- */
	obs_property_t *layout_list = obs_properties_add_list(props, "layout_mode",
							      obs_module_text("TeaLiveSubtitle.Prop.LayoutMode"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(layout_list, obs_module_text("TeaLiveSubtitle.Prop.LayoutMode.Auto"),
				  TEA_LAYOUT_MODE_AUTO);
	obs_property_list_add_int(layout_list, obs_module_text("TeaLiveSubtitle.Prop.LayoutMode.Fixed"),
				  TEA_LAYOUT_MODE_FIXED);
	obs_property_set_modified_callback(layout_list, tea_layout_mode_modified);
	obs_properties_add_text(props, "auto_mode_hint",
				tea_text_or("TeaLiveSubtitle.Prop.AutoModeHint",
					    "Auto mode without a legacy content width never wraps: long sentences keep "
					    "growing sideways. Switch to \"Fixed Width\" to wrap at the box width."),
				OBS_TEXT_INFO);
	obs_properties_add_int(props, "caption_width_px", obs_module_text("TeaLiveSubtitle.Prop.CaptionWidth"),
			       TEA_LAYOUT_MIN_WIDTH_PX, TEA_LAYOUT_MAX_WIDTH_PX, 1);
	obs_property_t *custom_width = obs_properties_add_int(
		props, "custom_width", obs_module_text("TeaLiveSubtitle.Prop.CustomWidth"), 0, 8192, 1);
	obs_property_set_modified_callback(custom_width, tea_layout_mode_modified);
	obs_properties_add_bool(props, "word_wrap", tea_text_or("TeaLiveSubtitle.Prop.WordWrap", "Word Wrap"));

	obs_property_t *align_list = obs_properties_add_list(props, "caption_align",
							     obs_module_text("TeaLiveSubtitle.Prop.Align"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(align_list, obs_module_text("TeaLiveSubtitle.Prop.Align.Left"), 0);
	obs_property_list_add_int(align_list, obs_module_text("TeaLiveSubtitle.Prop.Align.Center"), 1);
	obs_property_list_add_int(align_list, obs_module_text("TeaLiveSubtitle.Prop.Align.Right"), 2);
	obs_properties_add_int(props, "padding", obs_module_text("TeaLiveSubtitle.Prop.Padding"), 0, 200, 1);

	/* This plugin's own settings. Deliberately no x/y/position controls --
	 * that is left to the scene item transform, like a native source. */
	obs_properties_add_int(props, "max_lines", obs_module_text("TeaLiveSubtitle.Prop.MaxLines"), 1, 10, 1);
	obs_property_t *rows_prop = obs_properties_add_int(props, TEA_KEY_MAX_ROWS,
							   tea_text_or("TeaLiveSubtitle.Prop.MaxVisibleRows",
								       "Max rows on screen (0 = no limit)"),
							   0, TEA_MAX_DRAW_ROWS, 1);
	obs_property_set_long_description(
		rows_prop, tea_text_or("TeaLiveSubtitle.Prop.MaxVisibleRows.Tooltip",
				       "Counts wrapped rows. When a new row would exceed the limit, the oldest "
				       "row leaves the screen as a whole; text inside a row never changes."));
	obs_property_t *pause_prop = obs_properties_add_int(props, TEA_KEY_PAUSE_MS,
							    tea_text_or("TeaLiveSubtitle.Prop.LineBreakPause",
									"Sentence break pause (ms, 0 = off)"),
							    0, 60000, 100);
	obs_property_set_long_description(
		pause_prop, tea_text_or("TeaLiveSubtitle.Prop.LineBreakPause.Tooltip",
					"Start a new row when the caption has not changed for this long. Every new "
					"server segment (the server cuts after ~0.9 s of silence) already starts a "
					"new row; this adds breaks for long pauses inside one segment."));
	obs_properties_add_bool(props, "show_placeholder", obs_module_text("TeaLiveSubtitle.Prop.ShowPlaceholder"));

	/* --- fades --- */
	obs_properties_add_bool(props, TEA_KEY_FADE_IN, tea_text_or("TeaLiveSubtitle.Prop.FadeIn", "Fade in new text"));
	obs_properties_add_bool(props, TEA_KEY_FADE_OUT,
				tea_text_or("TeaLiveSubtitle.Prop.FadeOut", "Fade out old captions"));
	obs_properties_add_int(props, TEA_KEY_FADE_DELAY_MS,
			       tea_text_or("TeaLiveSubtitle.Prop.FadeOutDelay", "Fade out after (ms without changes)"),
			       500, 600000, 100);
	obs_properties_add_int(props, TEA_KEY_FADE_MS,
			       tea_text_or("TeaLiveSubtitle.Prop.FadeDuration", "Fade in/out duration (ms)"), 0, 10000,
			       50);

	/* --- unstable tail (stable captions only) --- */
	obs_property_t *tail_prop = obs_properties_add_bool(props, TEA_KEY_TAIL,
							    tea_text_or("TeaLiveSubtitle.Prop.UnstableTail",
									"Show not-yet-confirmed text (fainter)"));
	obs_property_set_long_description(
		tail_prop, tea_text_or("TeaLiveSubtitle.Prop.UnstableTail.Tooltip",
				       "Stable captions only. After the confirmed text, show the part of the newest "
				       "preview the server has not confirmed yet. Confirmed text never changes; "
				       "only this fainter tail may."));
	obs_properties_add_int_slider(props, TEA_KEY_TAIL_OPACITY,
				      tea_text_or("TeaLiveSubtitle.Prop.UnstableTailOpacity",
						  "Not-yet-confirmed text opacity (%)"),
				      10, 100, 5);
	return props;
}

static struct obs_source_info tea_captions_source_info = {
	.id = "tea_live_subtitle_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = tea_captions_source_get_name,
	.create = tea_captions_source_create,
	.destroy = tea_captions_source_destroy,
	.update = tea_captions_source_update,
	.video_tick = tea_captions_source_video_tick,
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
