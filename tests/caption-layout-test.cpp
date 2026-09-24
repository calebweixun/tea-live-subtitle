#include "caption-layout.h"
#include "caption-display.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		std::exit(EXIT_FAILURE);
	}
}

} // namespace

static void test_box_layout()
{
	const tea_caption_layout_t fixed = tea_caption_layout_resolve(TEA_LAYOUT_MODE_FIXED, 960, 10, 120);
	expect(fixed.mode == TEA_LAYOUT_MODE_FIXED, "fixed mode must remain fixed");
	expect(fixed.outer_width == 960, "fixed outer width must be stable");
	expect(fixed.content_width == 940, "fixed content width must subtract both paddings");
	expect(fixed.force_word_wrap, "fixed mode must force native word wrapping");

	const tea_caption_layout_t fixed_again = tea_caption_layout_resolve(TEA_LAYOUT_MODE_FIXED, 960, 10, 2400);
	expect(fixed_again.outer_width == fixed.outer_width,
	       "fixed outer width must not depend on the child text width");
	expect(fixed_again.content_width == fixed.content_width,
	       "fixed content width must not depend on the child text width");

	const tea_caption_layout_t clamped = tea_caption_layout_resolve(TEA_LAYOUT_MODE_FIXED, 100, 10, 0);
	expect(clamped.outer_width == TEA_LAYOUT_DEFAULT_WIDTH_PX,
	       "invalid fixed width must use the documented default");
	expect(clamped.content_width == TEA_LAYOUT_DEFAULT_WIDTH_PX - 20,
	       "default fixed width must still account for padding");

	const tea_caption_layout_t auto_layout = tea_caption_layout_resolve(TEA_LAYOUT_MODE_AUTO, 960, 10, 640);
	expect(auto_layout.mode == TEA_LAYOUT_MODE_AUTO, "auto mode must remain auto");
	expect(auto_layout.outer_width == 660, "auto mode must preserve child width plus padding");
	expect(auto_layout.content_width == 640, "auto mode must preserve legacy child width");
	expect(!auto_layout.force_word_wrap, "auto mode must not override legacy word-wrap settings");

	const tea_caption_layout_t invalid_mode = tea_caption_layout_resolve(99, 960, 10, 640);
	expect(invalid_mode.mode == TEA_LAYOUT_MODE_AUTO, "unknown mode must fail safe to auto");

	/* Settings migration: old scene JSON has neither the new schema marker nor
	 * an explicit layout_mode and must retain the legacy auto-sized behavior.
	 * New sources and explicit user choices carry a marker/value and resolve to
	 * the requested mode. */
	expect(tea_caption_layout_mode_from_settings(TEA_LAYOUT_MODE_FIXED, false, false) == TEA_LAYOUT_MODE_AUTO,
	       "legacy scenes without a marker must remain auto-sized");
	expect(tea_caption_layout_mode_from_settings(TEA_LAYOUT_MODE_FIXED, true, false) == TEA_LAYOUT_MODE_FIXED,
	       "marked new sources must preserve fixed mode");
	expect(tea_caption_layout_mode_from_settings(TEA_LAYOUT_MODE_AUTO, true, false) == TEA_LAYOUT_MODE_AUTO,
	       "marked sources may explicitly choose auto mode");
	expect(tea_caption_layout_mode_from_settings(TEA_LAYOUT_MODE_FIXED, false, true) == TEA_LAYOUT_MODE_FIXED,
	       "an old scene may opt into fixed mode explicitly");

	expect(std::string(tea_caption_select_display_text("final transcript", true, "Waiting")) == "final transcript",
	       "transcript must always win over the placeholder");
	expect(std::string(tea_caption_select_display_text("", true, "Waiting")) == "Waiting",
	       "empty transcript may use the neutral placeholder");
	expect(std::string(tea_caption_select_display_text("", false, "Waiting")).empty(),
	       "placeholder can be disabled without changing transcript state");
	expect(std::string(tea_caption_select_display_text(nullptr, true, nullptr)).empty(),
	       "missing transcript and placeholder must render empty");
}

/* ---------------- per-line renderer policy (caption-display.h) ---------------- */

namespace {

constexpr uint64_t kMs = TEA_NS_PER_MS;

/* A fake text_ft2 font: ASCII 10 px, space 5 px, everything else 20 px. */
uint32_t fake_advance(uint32_t cp)
{
	if (cp == ' ')
		return 5;
	if (cp < 0x80)
		return 10;
	return 20;
}

void learn(tea_glyph_cache_t *cache, const std::string &text)
{
	size_t pos = 0;
	while (pos < text.size()) {
		uint32_t cp = tea_utf8_decode(text.data(), text.size(), &pos);
		tea_glyph_cache_store(cache, cp, fake_advance(cp));
	}
}

std::string row_text(const std::string &text, const tea_wrap_row_t &row)
{
	return text.substr(row.start, row.end - row.start);
}

std::vector<std::string> wrap_rows(const tea_display_line_t &line, const std::string &text, uint32_t max_width,
				   uint32_t extra, bool word_wrap, const tea_glyph_cache_t *cache,
				   std::vector<tea_wrap_row_t> *raw = nullptr)
{
	tea_wrap_row_t ring[32];
	tea_wrap_row_t ordered[32];
	int total = tea_wrap_line(&line, text.c_str(), max_width, extra, word_wrap, cache, ring, 32);
	expect(total >= 0, "wrap must not report missing glyphs for a learned font");
	int kept = tea_wrap_rows_in_order(ring, total, 32, ordered);
	std::vector<std::string> out;
	for (int i = 0; i < kept; i++) {
		out.push_back(row_text(text, ordered[i]));
		if (raw)
			raw->push_back(ordered[i]);
	}
	return out;
}

bool near(float a, float b)
{
	return std::fabs(a - b) < 0.02f;
}

/* Builds a line whose text arrived in the given pieces, one chunk each. */
tea_display_line_t line_from_chunks(const std::vector<std::string> &chunks, std::string *text, uint64_t t0 = 0)
{
	tea_display_line_t line;
	std::string acc = chunks.empty() ? std::string() : chunks[0];
	tea_display_line_init(&line, 1, acc.size(), acc.size(), t0);
	for (size_t i = 1; i < chunks.size(); i++) {
		std::string next = acc + chunks[i];
		tea_display_line_observe(&line, acc.c_str(), acc.size(), next.c_str(), next.size(), next.size(),
					 t0 + i * 100 * kMs, 0);
		acc = next;
	}
	*text = acc;
	return line;
}

} // namespace

static void test_alignment()
{
	expect(tea_row_offset_x(TEA_ALIGN_LEFT, 940, 300) == 0, "left alignment starts at the left edge");
	expect(tea_row_offset_x(TEA_ALIGN_CENTER, 940, 300) == 320, "center alignment splits the slack");
	expect(tea_row_offset_x(TEA_ALIGN_RIGHT, 940, 300) == 640, "right alignment ends at the right edge");
	expect(tea_row_offset_x(TEA_ALIGN_CENTER, 940, 301) == 319, "center offsets stay on whole pixels");
	expect(tea_row_offset_x(TEA_ALIGN_RIGHT, 100, 300) == 0,
	       "a row wider than the box never gets a negative offset");
	/* every row is aligned on its own width */
	expect(tea_row_offset_x(TEA_ALIGN_CENTER, 500, 100) != tea_row_offset_x(TEA_ALIGN_CENTER, 500, 400),
	       "rows of different width get different offsets");
}

static void test_utf8_and_breaks()
{
	const std::string mixed = "a我🍵";
	size_t pos = 0;
	expect(tea_utf8_decode(mixed.data(), mixed.size(), &pos) == 'a' && pos == 1, "ASCII decodes as one byte");
	expect(tea_utf8_decode(mixed.data(), mixed.size(), &pos) == 0x6211 && pos == 4, "CJK decodes as three bytes");
	expect(tea_utf8_decode(mixed.data(), mixed.size(), &pos) == 0x1F375 && pos == 8, "emoji decodes as four bytes");
	char buf[5];
	expect(tea_utf8_encode(0x1F375, buf) == 4 && std::string(buf) == "🍵", "emoji round-trips through encode");
	const char bad[] = "\xe6\x88";
	pos = 0;
	expect(tea_utf8_decode(bad, 2, &pos) == 0xFFFD && pos == 1, "a truncated sequence advances one byte");

	const std::string a = "我們需", b = "我們要";
	expect(tea_utf8_common_prefix(a.data(), a.size(), b.data(), b.size()) == 6,
	       "common prefix stops at a character boundary");

	expect(tea_can_break_between(0x6211, 0x5011), "CJK characters may break between each other");
	expect(!tea_can_break_between('a', 'b'), "letters of one word never break");
	expect(tea_can_break_between('a', ' ') && tea_can_break_between(' ', 'b'), "spaces are break opportunities");
	expect(!tea_can_break_between(0x6211, 0xFF0C), "a row never starts with a fullwidth comma");
	expect(!tea_can_break_between(0x300C, 0x6211), "a row never ends with an opening bracket");
	expect(!tea_can_break_between(0x1F468, 0x200D) && !tea_can_break_between(0x200D, 0x1F469),
	       "an emoji ZWJ sequence stays together");
}

static void test_glyph_cache()
{
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	uint32_t adv = 0;
	expect(!tea_glyph_cache_lookup(&cache, 0x6211, &adv), "an unmeasured glyph is unknown");
	tea_glyph_cache_store(&cache, 0x6211, 20);
	expect(tea_glyph_cache_lookup(&cache, 0x6211, &adv) && adv == 20, "a measured glyph is remembered");
	expect(tea_outline_extra_from_measures(24, 44) == 4, "outline margin = 2*w(X) - w(XX)");
	expect(tea_glyph_advance_from_measure(24, 4) == 20, "advance = measured width - outline margin");
	expect(tea_glyph_advance_from_measure(3, 4) == 0, "a glyph narrower than the margin has no advance");

	const std::string text = "我們我a";
	uint32_t missing[8];
	int n = tea_glyph_cache_missing(&cache, text.c_str(), 0, text.size(), missing, 8);
	expect(n == 2 && missing[0] == 0x5011 && missing[1] == 'a', "missing glyphs are listed once each");
	uint32_t sum = 0;
	expect(!tea_text_advance(&cache, text.c_str(), 0, text.size(), &sum), "a run with unknown glyphs has no width");
	learn(&cache, text);
	expect(tea_text_advance(&cache, text.c_str(), 0, text.size(), &sum) && sum == 70,
	       "a run's width is the sum of its advances");

	for (uint32_t cp = 0x4E00; cache.count < TEA_GLYPH_CACHE_LIMIT; cp++)
		tea_glyph_cache_store(&cache, cp, 20);
	tea_glyph_cache_store(&cache, 0x1F375, 30);
	expect(cache.count == 1 && tea_glyph_cache_lookup(&cache, 0x1F375, &adv) && adv == 30,
	       "a full cache starts over instead of overflowing");
}

static void test_wrapping()
{
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	learn(&cache, "我們需要一個計畫再說，「」hello world foo abcdefghijklmnop");

	std::string text;
	tea_display_line_t line = line_from_chunks({"我們需要一個計畫再說"}, &text);
	std::vector<tea_wrap_row_t> raw;
	auto rows = wrap_rows(line, text, 100, 0, true, &cache, &raw);
	expect(rows.size() == 2 && rows[0] == "我們需要一" && rows[1] == "個計畫再說", "CJK wraps between characters");
	expect(raw[0].width == 100 && raw[1].width == 100, "row width is the sum of advances");

	rows = wrap_rows(line, text, 0, 0, true, &cache);
	expect(rows.size() == 1, "an unlimited width never wraps (legacy Auto mode)");

	/* outline margin counts against the width, like text_ft2 reports it */
	rows = wrap_rows(line, text, 100, 4, true, &cache, &raw);
	expect(rows[0] == "我們需要", "the outline margin is part of the row width");

	line = line_from_chunks({"我們需要一，"}, &text);
	rows = wrap_rows(line, text, 100, 0, true, &cache);
	expect(rows.size() == 1 && rows[0] == "我們需要一，", "closing punctuation hangs instead of starting a row");

	line = line_from_chunks({"hello world foo"}, &text);
	rows = wrap_rows(line, text, 80, 0, true, &cache);
	expect(rows.size() == 3 && rows[0] == "hello" && rows[1] == "world" && rows[2] == "foo",
	       "Latin text wraps at spaces and trims them");
	rows = wrap_rows(line, text, 80, 0, false, &cache);
	expect(rows[0] == "hello wo", "without word wrap the row breaks at the overflowing character");

	line = line_from_chunks({"abcdefghijklmnop"}, &text);
	rows = wrap_rows(line, text, 80, 0, true, &cache);
	expect(rows.size() == 2 && rows[0] == "abcdefgh", "a word wider than the row is broken inside");

	/* Already shown text never moves: "hello wor" was on screen, then "ld"
	 * arrived and overflowed. The break goes to where the new chunk starts,
	 * not back to the space (which would pull "wor" off the first row). */
	line = line_from_chunks({"hello wor", "ld"}, &text);
	rows = wrap_rows(line, text, 100, 0, true, &cache);
	expect(rows.size() == 2 && rows[0] == "hello wor" && rows[1] == "ld",
	       "an overflowing append never moves characters already on screen");
	line = line_from_chunks({"hello world"}, &text);
	rows = wrap_rows(line, text, 100, 0, true, &cache);
	expect(rows[0] == "hello" && rows[1] == "world", "the same text arriving at once wraps at the space");

	/* the unstable tail never pushes committed text around */
	line = line_from_chunks({"hello wor"}, &text);
	std::string with_tail = text + "ld";
	tea_display_line_observe(&line, text.c_str(), text.size(), with_tail.c_str(), with_tail.size(), text.size(),
				 50 * kMs, 0);
	rows = wrap_rows(line, with_tail, 100, 0, true, &cache);
	expect(rows[0] == "hello wor", "a tail overflowing the row starts on the next row");
}

static void test_append_stability()
{
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	const std::string full = "今天我們要討論的是字幕的顯示方式，還有換行、對齊與淡出的效果。hello world again";
	learn(&cache, full);

	/* Grow the line one code point per update. After every update, every row
	 * that was on screen keeps its exact text, except the last one, which
	 * may only grow at its end. */
	tea_display_line_t line;
	size_t pos = 0;
	tea_utf8_decode(full.data(), full.size(), &pos);
	std::string text = full.substr(0, pos);
	tea_display_line_init(&line, 7, text.size(), text.size(), 0);
	std::vector<std::string> prev = wrap_rows(line, text, 130, 4, true, &cache);
	uint64_t t = 0;
	while (pos < full.size()) {
		tea_utf8_decode(full.data(), full.size(), &pos);
		std::string next = full.substr(0, pos);
		t += 100 * kMs;
		tea_display_line_observe(&line, text.c_str(), text.size(), next.c_str(), next.size(), next.size(), t,
					 0);
		text = next;
		std::vector<std::string> now = wrap_rows(line, text, 130, 4, true, &cache);
		expect(now.size() >= prev.size(), "appending never removes a row");
		for (size_t i = 0; i + 1 < prev.size(); i++)
			expect(now[i] == prev[i], "a full row never changes when text is appended");
		if (!prev.empty())
			expect(now[prev.size() - 1].compare(0, prev.back().size(), prev.back()) == 0,
			       "the growing row only gains characters at its end");
		prev = now;
	}
}

static void test_pause_breaks_and_chunks()
{
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	learn(&cache, "我們需要一個計畫XY");

	tea_display_line_t line;
	std::string a = "我們";
	tea_display_line_init(&line, 3, a.size(), a.size(), 0);
	std::string b = "我們需要";
	expect(tea_display_line_observe(&line, a.c_str(), a.size(), b.c_str(), b.size(), b.size(), 500 * kMs, 1000),
	       "growth is a change");
	expect(line.hard_break_count == 0, "a short gap keeps the row going");
	std::string c = "我們需要一個";
	tea_display_line_observe(&line, b.c_str(), b.size(), c.c_str(), c.size(), c.size(), 2500 * kMs, 1000);
	expect(line.hard_break_count == 1 && line.hard_breaks[0] == b.size(),
	       "a pause at least as long as the setting starts a new row");
	auto rows = wrap_rows(line, c, 1000, 0, true, &cache);
	expect(rows.size() == 2 && rows[0] == "我們需要" && rows[1] == "一個", "the pause break splits the rows");
	expect(!tea_display_line_observe(&line, c.c_str(), c.size(), c.c_str(), c.size(), c.size(), 9000 * kMs, 1000),
	       "an identical snapshot is not a change");

	/* chunks: each appended run keeps its own birth time (per-run fade-in) */
	expect(line.chunk_count == 3, "each append starts a chunk");
	expect(line.chunks[1].start == a.size() && line.chunks[1].born_ns == 500 * kMs,
	       "a chunk remembers where and when it appeared");
	expect(tea_display_chunk_at(&line, 0) == 0 && tea_display_chunk_at(&line, b.size()) == 2,
	       "chunk lookup by byte offset");

	/* a rewrite (legacy preview / unstable tail) keeps the unchanged prefix's timing */
	std::string d = "我們需要XY";
	tea_display_line_observe(&line, c.c_str(), c.size(), d.c_str(), d.size(), b.size(), 3000 * kMs, 0);
	expect(line.chunks[line.chunk_count - 1].start == b.size() &&
		       line.chunks[line.chunk_count - 1].born_ns == 3000 * kMs,
	       "rewritten text becomes a new chunk after the common prefix");
	expect(line.hard_break_count == 1, "a pause break inside the kept prefix survives");
	expect(line.locked_len == b.size(), "the committed length is tracked separately from the tail");
	std::string e = "我們需要";
	tea_display_line_observe(&line, d.c_str(), d.size(), e.c_str(), e.size(), e.size(), 3100 * kMs, 0);
	expect(line.len == e.size() && line.chunks[line.chunk_count - 1].start < e.size(),
	       "a tail that disappears simply ends the chunks there");

	/* retire (faded out): later text starts fresh */
	tea_display_line_retire(&line);
	expect(!tea_display_line_has_visible_text(&line), "a retired line shows nothing");
	std::string f = "我們需要一個計畫";
	tea_display_line_observe(&line, e.c_str(), e.size(), f.c_str(), f.size(), f.size(), 4000 * kMs, 0);
	rows = wrap_rows(line, f, 1000, 0, true, &cache);
	expect(rows.size() == 1 && rows[0] == "一個計畫", "text arriving after a fade-out shows only the new part");
}

static void test_row_limit()
{
	expect(tea_rows_to_evict(5, 2) == 3, "oldest rows beyond the limit leave");
	expect(tea_rows_to_evict(2, 2) == 0 && tea_rows_to_evict(9, 0) == 0,
	       "no eviction within the limit or unlimited");

	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	learn(&cache, "一二三四五六七八九十");
	std::string text;
	tea_display_line_t line = line_from_chunks({"一二三四五六七八九十"}, &text);
	std::vector<tea_wrap_row_t> raw;
	auto rows = wrap_rows(line, text, 60, 0, true, &cache, &raw);
	expect(rows.size() == 4, "ten characters in rows of three");
	int evict = tea_rows_to_evict((int)rows.size(), 2);
	for (int i = 0; i < evict; i++)
		tea_display_line_evict_through(&line, raw[(size_t)i].next);
	auto kept = wrap_rows(line, text, 60, 0, true, &cache);
	expect(kept.size() == 2 && kept[0] == rows[2] && kept[1] == rows[3],
	       "evicting removes whole rows and leaves the others untouched");
	tea_display_line_evict_through(&line, 0);
	expect(wrap_rows(line, text, 60, 0, true, &cache).size() == 2, "eviction never brings rows back");
}

static void test_fades()
{
	expect(tea_fade_in_alpha(false, 0, 0, 400) == 1.0f, "fade-in off: visible at once");
	expect(tea_fade_in_alpha(true, 100 * kMs, 100 * kMs, 0) == 1.0f, "zero duration: visible at once");
	expect(tea_fade_in_alpha(true, 100 * kMs, 100 * kMs, 400) == 0.0f, "fade-in starts transparent");
	expect(near(tea_fade_in_alpha(true, 300 * kMs, 100 * kMs, 400), 0.5f), "fade-in is half way at half time");
	expect(tea_fade_in_alpha(true, 250 * kMs, 100 * kMs, 400) < tea_fade_in_alpha(true, 260 * kMs, 100 * kMs, 400),
	       "fade-in only increases");
	expect(tea_fade_in_alpha(true, 600 * kMs, 100 * kMs, 400) == 1.0f, "fade-in ends opaque");

	const uint64_t changed = 1000 * kMs;
	expect(tea_fade_out_alpha(false, 99999 * kMs, changed, 6000, 400) == 1.0f, "fade-out off: stays forever");
	expect(tea_fade_out_alpha(true, changed + 5999 * kMs, changed, 6000, 400) == 1.0f,
	       "fully visible until the delay has passed");
	expect(near(tea_fade_out_alpha(true, changed + 6200 * kMs, changed, 6000, 400), 0.5f),
	       "fade-out is half way at half the fade time");
	expect(tea_fade_out_alpha(true, changed + 6400 * kMs, changed, 6000, 400) == 0.0f, "fade-out ends transparent");
	expect(tea_fade_out_alpha(true, changed + 6000 * kMs, changed, 6000, 0) == 0.0f,
	       "zero fade time: gone at once");
	expect(!tea_fade_out_done(true, changed + 6399 * kMs, changed, 6000, 400) &&
		       tea_fade_out_done(true, changed + 6400 * kMs, changed, 6000, 400),
	       "the line is removed exactly when the fade-out ends");
	/* new text for the line resets changed_ns: the fade is cancelled */
	const uint64_t regrow = changed + 6200 * kMs;
	expect(tea_fade_out_alpha(true, regrow + 1 * kMs, regrow, 6000, 400) == 1.0f,
	       "new content during a fade-out makes the line fully visible again");
	/* fade in and fade out share one duration setting */
	expect(near(tea_fade_in_alpha(true, 200 * kMs, 0, 400),
		    1.0f - tea_fade_out_alpha(true, 6200 * kMs, 0, 6000, 400)),
	       "fade-in and fade-out follow the same curve over the shared duration");
}

static void test_frame_geometry()
{
	tea_caption_frame_t fixed = tea_caption_frame_resolve(TEA_LAYOUT_MODE_FIXED, 960, 10, 0, true, 4, 60, false, 8);
	expect(fixed.wrap_width == 940 && fixed.word_wrap, "fixed mode wraps at the inner width");
	expect(tea_caption_frame_outer_width(&fixed, TEA_LAYOUT_MODE_FIXED, 960, 3000) == 960,
	       "fixed mode never grows sideways");
	expect(tea_caption_frame_content_width(&fixed, TEA_LAYOUT_MODE_FIXED, 960, 100) == 940,
	       "fixed mode aligns inside the inner width");
	expect(fixed.row_pitch == 64 && tea_caption_frame_outer_height(&fixed, 2) == 20 + 64 + 60,
	       "rows are spaced like text_ft2 lines");
	expect(tea_caption_frame_outer_height(&fixed, 0) == 80, "an empty caption keeps one row of height");

	tea_caption_frame_t legacy =
		tea_caption_frame_resolve(TEA_LAYOUT_MODE_AUTO, 960, 10, 500, false, 4, 60, false, 0);
	expect(legacy.wrap_width == 504 && !legacy.word_wrap,
	       "legacy Auto with custom_width keeps wrapping where text_ft2 did");
	expect(tea_caption_frame_outer_width(&legacy, TEA_LAYOUT_MODE_AUTO, 960, 100) == 524,
	       "legacy Auto with custom_width keeps its old outer width");

	tea_caption_frame_t unbounded =
		tea_caption_frame_resolve(TEA_LAYOUT_MODE_AUTO, 960, 10, 0, true, 4, 60, false, 0);
	expect(unbounded.wrap_width == 0, "legacy Auto without custom_width is not silently changed to wrap");
	expect(tea_caption_frame_outer_width(&unbounded, TEA_LAYOUT_MODE_AUTO, 960, 700) == 720,
	       "legacy Auto grows with its widest row, as before");

	tea_caption_frame_t boxed = tea_caption_frame_resolve(TEA_LAYOUT_MODE_FIXED, 960, 10, 0, true, 4, 60, true, 8);
	expect(boxed.inset_x == 18 && boxed.wrap_width == 924, "the background box stays inside the source");
	expect(boxed.row_pitch == 76, "background boxes of neighbouring rows do not overlap");
	expect(tea_caption_frame_row_y(&boxed, 1) == 18 + 76, "row y follows the pitch");
}

static void test_connection_policy()
{
	tea_connection_settings_t applied = {"127.0.0.1", 8327, "", true};
	tea_connection_settings_t same = {"127.0.0.1", 8327, nullptr, true};
	expect(tea_connection_decide(&applied, &same, true, true, false) == TEA_CONNECTION_KEEP,
	       "an appearance-only update never reconnects, even while editing");
	expect(tea_connection_decide(&applied, &same, true, false, true) == TEA_CONNECTION_KEEP,
	       "Apply without connection changes does not reconnect");
	tea_connection_settings_t typing = {"127.0.0.", 8327, "", true};
	expect(tea_connection_decide(&applied, &typing, true, true, false) == TEA_CONNECTION_DEFER,
	       "a host being typed in the Properties window waits");
	expect(tea_connection_decide(&applied, &typing, true, true, true) == TEA_CONNECTION_RECONNECT,
	       "Apply / closing the window reconnects with the edited host");
	expect(tea_connection_decide(&applied, &typing, true, false, false) == TEA_CONNECTION_RECONNECT,
	       "a non-interactive update (undo, script) applies at once");
	tea_connection_settings_t stable_off = {"127.0.0.1", 8327, "", false};
	expect(tea_connection_decide(&applied, &stable_off, true, true, false) == TEA_CONNECTION_DEFER,
	       "stable captions is a session.start field: treated as a connection setting");
	expect(tea_connection_decide(&applied, &same, false, true, false) == TEA_CONNECTION_RECONNECT,
	       "the first update of a new source connects");
}

int main()
{
	test_box_layout();
	test_alignment();
	test_utf8_and_breaks();
	test_glyph_cache();
	test_wrapping();
	test_append_stability();
	test_pause_breaks_and_chunks();
	test_row_limit();
	test_fades();
	test_frame_geometry();
	test_connection_policy();
	std::cout << "caption layout tests passed\n";
	return EXIT_SUCCESS;
}
