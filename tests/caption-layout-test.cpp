#include "caption-layout.h"
#include "caption-display.h"

#include <cctype>
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
				   std::vector<tea_wrap_row_t> *raw = nullptr, const tea_punct_break_t *punct = nullptr)
{
	tea_wrap_row_t ring[32];
	tea_wrap_row_t ordered[32];
	int total = tea_wrap_line_ex(&line, text.c_str(), max_width, extra, word_wrap, punct, cache, ring, 32);
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
					 t0 + i * 100 * kMs);
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
				 50 * kMs);
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
		tea_display_line_observe(&line, text.c_str(), text.size(), next.c_str(), next.size(), next.size(), t);
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

static void test_big_append_keeps_rows()
{
	/* The real model can commit 20 characters at once. Width 1800 at 48 px:
	 * the burst overflows the row, earlier text stays where it was, the
	 * burst is one chunk (one fade-in) split over the rows it lands on. */
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	const std::string first = "今天我們要討論的是字幕的顯示方式還有換行對齊淡出"; /* 24 */
	const std::string burst = "的效果以及真實直播時觀眾看到的樣子是否穩定好讀";   /* 22 */
	learn(&cache, first + burst);
	std::string text;
	tea_display_line_t line = line_from_chunks({first}, &text);
	const uint32_t width = 36 * 20; /* 36 characters per row in the fake font */
	auto before = wrap_rows(line, text, width, 0, true, &cache);
	expect(before.size() == 1 && before[0] == first, "the first part fits one row");
	std::string grown = text + burst;
	tea_display_line_observe(&line, text.c_str(), text.size(), grown.c_str(), grown.size(), grown.size(),
				 900 * kMs);
	auto after = wrap_rows(line, grown, width, 0, true, &cache);
	expect(after.size() == 2 && after[0].compare(0, first.size(), first) == 0,
	       "a 20+ character burst never moves the text already on the row");
	expect(after[0] + after[1] == grown, "no character is lost or duplicated across the break");
	expect(line.chunk_count == 2 && line.chunks[1].start == first.size() && line.chunks[1].born_ns == 900 * kMs,
	       "the burst is one chunk: it fades in once, on both rows it lands on");
}

namespace {

std::vector<std::string> punct_rows(const std::string &text, int mode, int comma_min, uint32_t width = 2000,
				    std::vector<tea_wrap_row_t> *raw = nullptr)
{
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	learn(&cache, text);
	std::string full;
	tea_display_line_t line = line_from_chunks({text}, &full);
	tea_punct_break_t punct;
	punct.mode = mode;
	punct.comma_min_chars = comma_min;
	return wrap_rows(line, full, width, 0, true, &cache, raw, &punct);
}

bool rows_are(const std::vector<std::string> &rows, std::initializer_list<const char *> want)
{
	if (rows.size() != want.size())
		return false;
	size_t i = 0;
	for (const char *w : want)
		if (rows[i++] != w)
			return false;
	return true;
}

} // namespace

static void test_punctuation_breaks()
{
	const int S = TEA_PUNCT_BREAK_SENTENCE, C = TEA_PUNCT_BREAK_COMMA;
	expect(rows_are(punct_rows("今天很好。我們走吧", TEA_PUNCT_BREAK_OFF, 8), {"今天很好。我們走吧"}),
	       "off: punctuation never breaks");
	std::vector<tea_wrap_row_t> raw;
	expect(rows_are(punct_rows("今天很好。我們走吧", S, 8, 2000, &raw), {"今天很好。", "我們走吧"}),
	       "sentence-final punctuation ends the row and stays on it");
	expect(raw[0].punct_break && !raw[1].punct_break, "the row is marked as ended by punctuation");
	expect(rows_are(punct_rows("今天很好，我們走吧", S, 0), {"今天很好，我們走吧"}),
	       "sentence mode ignores commas");
	expect(rows_are(punct_rows("今天很好，我們走吧", C, 8), {"今天很好，我們走吧"}),
	       "a comma after fewer than the minimum characters does not break");
	expect(rows_are(punct_rows("今天天氣真的非常好，我們走吧", C, 8), {"今天天氣真的非常好，", "我們走吧"}),
	       "a comma after enough characters breaks");
	expect(rows_are(punct_rows("好。走", C, 8), {"好。", "走"}), "sentence-final punctuation ignores the minimum");
	expect(rows_are(punct_rows("一二三、四五六七八九十、好", C, 8), {"一二三、四五六七八九十、", "好"}),
	       "the minimum counts the characters on the current row");

	/* full-width and ASCII variants alike */
	expect(rows_are(punct_rows("好．走", S, 0), {"好．", "走"}) &&
		       rows_are(punct_rows("好！走", S, 0), {"好！", "走"}) &&
		       rows_are(punct_rows("好？走", S, 0), {"好？", "走"}) &&
		       rows_are(punct_rows("好；走", S, 0), {"好；", "走"}) &&
		       rows_are(punct_rows("好!走", S, 0), {"好!", "走"}) &&
		       rows_are(punct_rows("好?走", S, 0), {"好?", "走"}) &&
		       rows_are(punct_rows("好;走", S, 0), {"好;", "走"}) &&
		       rows_are(punct_rows("好,走", C, 0), {"好,", "走"}),
	       "full-width and ASCII marks break alike");

	/* numbers and abbreviations */
	expect(rows_are(punct_rows("It costs 3.5 dollars. Then we go", S, 0), {"It costs 3.5 dollars.", "Then we go"}),
	       "a decimal point never breaks; the next row starts at the next non-space character");
	expect(rows_are(punct_rows("1,000 people, and more", C, 0), {"1,000 people,", "and more"}),
	       "a thousands separator never breaks");
	expect(rows_are(punct_rows("價格是3.5元。好", S, 0), {"價格是3.5元。", "好"}), "numbers inside CJK text too");
	expect(rows_are(punct_rows("Mr. Smith went home. He slept", S, 0), {"Mr. Smith went home.", "He slept"}),
	       "a title abbreviation does not end a sentence");
	expect(rows_are(punct_rows("see e.g. this one", S, 0), {"see e.g. this one"}) &&
		       rows_are(punct_rows("the U.S. army", S, 0), {"the U.S. army"}) &&
		       rows_are(punct_rows("J. K. Rowling", S, 0), {"J. K. Rowling"}),
	       "e.g. / U.S. / initials do not end a sentence");
	expect(rows_are(punct_rows("www.example.com is up", S, 0), {"www.example.com is up"}),
	       "a dot followed by a letter never breaks");

	/* runs of marks and closing brackets stay together */
	expect(rows_are(punct_rows("他說：「好。」然後走了", S, 0), {"他說：「好。」", "然後走了"}),
	       "a closing bracket after the mark stays on the row it closes");
	expect(rows_are(punct_rows("真的嗎？！是的", S, 0), {"真的嗎？！", "是的"}), "?! stays together");
	expect(rows_are(punct_rows("嗯……好", S, 0), {"嗯……", "好"}) &&
		       rows_are(punct_rows("Well... ok", S, 0), {"Well...", "ok"}),
	       "an ellipsis stays together");

	/* nothing is decided before the next character: no row is ever split
	 * under text already shown */
	expect(rows_are(punct_rows("今天很好。", S, 0), {"今天很好。"}), "a mark at the end is not a break yet");
	expect(rows_are(punct_rows("It is 3.", S, 0), {"It is 3."}) &&
		       rows_are(punct_rows("It is 3.5", S, 0), {"It is 3.5"}),
	       "3. then 3.5: the dot is only decided by the character after it");

	/* a comma that overflows hangs and ends the row */
	expect(rows_are(punct_rows("一二三四五，六", C, 0, 100, &raw), {"一二三四五，", "六"}) && raw[0].punct_break,
	       "a hanging comma ends its row as a punctuation break");
}

static void test_punctuation_breaks_never_move_shown_text()
{
	/* Grow a line one code point per update through commas, periods, a
	 * decimal number and an abbreviation, in the most eager mode, with a
	 * narrow box: every earlier row stays exactly as it was, the last one
	 * only grows. */
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	const std::string full =
		"今天我們要討論，字幕的顯示方式。還有換行、對齊，與淡出的效果！價格是3.5元，Mr. Smith said "
		"hi. Then 1,000 people came……真的嗎？好。";
	learn(&cache, full);
	tea_punct_break_t punct;
	punct.mode = TEA_PUNCT_BREAK_COMMA;
	punct.comma_min_chars = 3;
	tea_display_line_t line;
	size_t pos = 0;
	tea_utf8_decode(full.data(), full.size(), &pos);
	std::string text = full.substr(0, pos);
	tea_display_line_init(&line, 7, text.size(), text.size(), 0);
	std::vector<std::string> prev = wrap_rows(line, text, 150, 4, true, &cache, nullptr, &punct);
	uint64_t t = 0;
	int breaks = 0;
	while (pos < full.size()) {
		tea_utf8_decode(full.data(), full.size(), &pos);
		std::string next = full.substr(0, pos);
		t += 100 * kMs;
		tea_display_line_observe(&line, text.c_str(), text.size(), next.c_str(), next.size(), next.size(), t);
		text = next;
		std::vector<tea_wrap_row_t> raw;
		std::vector<std::string> now = wrap_rows(line, text, 150, 4, true, &cache, &raw, &punct);
		expect(now.size() >= prev.size(), "appending never removes a row");
		for (size_t i = 0; i + 1 < prev.size(); i++)
			expect(now[i] == prev[i], "a finished row never changes when text is appended");
		if (!prev.empty())
			expect(now[prev.size() - 1].compare(0, prev.back().size(), prev.back()) == 0,
			       "the growing row only gains characters at its end");
		breaks = 0;
		for (const auto &r : raw)
			breaks += r.punct_break ? 1 : 0;
		prev = now;
	}
	expect(breaks >= 6, "the text really was broken at punctuation");
}

/* ---- soft breaks: runs without punctuation ---- */

/* One layout, as captions-source.c does it: wrap with soft breaks, then keep
 * what was decided. */
std::vector<std::string> soft_layout(tea_display_line_t *line, const std::string &text, uint32_t width,
				     const tea_glyph_cache_t *cache, const tea_punct_break_t *punct,
				     const tea_soft_break_t *cfg, std::vector<tea_wrap_row_t> *raw = nullptr)
{
	tea_wrap_row_t ring[32];
	tea_wrap_row_t ordered[32];
	tea_soft_wrap_t soft;
	soft.cfg = cfg;
	int total = tea_wrap_line_soft(line, text.c_str(), width, 0, true, punct, &soft, cache, ring, 32);
	expect(total >= 0, "soft wrap: no missing glyphs");
	tea_display_line_soft_commit(line, &soft);
	int kept = tea_wrap_rows_in_order(ring, total, 32, ordered);
	std::vector<std::string> out;
	for (int i = 0; i < kept; i++) {
		out.push_back(row_text(text, ordered[i]));
		if (raw)
			raw->push_back(ordered[i]);
	}
	return out;
}

size_t count_chars(const std::string &s)
{
	size_t n = 0, pos = 0;
	while (pos < s.size()) {
		tea_utf8_decode(s.data(), s.size(), &pos);
		n++;
	}
	return n;
}

/* Feeds `pieces` (text, arrival time in ms) one by one, laying out after
 * each like the source does every tick. Checks on every step that rows
 * already on screen never change (only the last one may grow). */
std::vector<std::string> soft_feed(const std::vector<std::pair<std::string, uint64_t>> &pieces, uint32_t width,
				   const tea_punct_break_t *punct, const tea_soft_break_t *cfg, int *soft_breaks)
{
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	std::string all;
	for (const auto &p : pieces)
		all += p.first;
	learn(&cache, all);
	tea_display_line_t line;
	std::string text = pieces[0].first;
	tea_display_line_init(&line, 9, text.size(), text.size(), pieces[0].second * kMs);
	std::vector<std::string> prev = soft_layout(&line, text, width, &cache, punct, cfg);
	std::vector<tea_wrap_row_t> raw;
	for (size_t i = 1; i < pieces.size(); i++) {
		std::string next = text + pieces[i].first;
		tea_display_line_observe(&line, text.c_str(), text.size(), next.c_str(), next.size(), next.size(),
					 pieces[i].second * kMs);
		text = next;
		raw.clear();
		std::vector<std::string> now = soft_layout(&line, text, width, &cache, punct, cfg, &raw);
		expect(now.size() >= prev.size(), "soft breaks: appending never removes a row");
		for (size_t r = 0; r + 1 < prev.size(); r++)
			expect(now[r] == prev[r], "soft breaks: a finished row never changes");
		if (!prev.empty())
			expect(now[prev.size() - 1].compare(0, prev.back().size(), prev.back()) == 0,
			       "soft breaks: the growing row only gains characters at its end");
		prev = now;
	}
	*soft_breaks = 0;
	for (const auto &r : raw)
		*soft_breaks += r.soft_break ? 1 : 0;
	return prev;
}

static void test_soft_breaks()
{
	tea_soft_break_t cfg;
	tea_soft_break_defaults(&cfg, true, 16);
	tea_punct_break_t punct;
	punct.mode = TEA_PUNCT_BREAK_COMMA;
	punct.comma_min_chars = 8;
	const std::string cjk = "一二三四五六七八九十甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥";
	auto cjk_at = [&](size_t i) {
		size_t pos = 0, n = 0, start = 0;
		while (pos < cjk.size()) {
			start = pos;
			tea_utf8_decode(cjk.data(), cjk.size(), &pos);
			if (n++ == i % 32)
				return cjk.substr(start, pos - start);
		}
		return std::string();
	};
	int soft = 0;

	/* a pause after 18 characters: the row ends there */
	{
		std::vector<std::pair<std::string, uint64_t>> pieces;
		uint64_t t = 1000;
		for (int i = 0; i < 9; i++, t += 300)
			pieces.push_back({cjk_at(2 * i) + cjk_at(2 * i + 1), t});
		t += 500; /* 800 ms since the last text: a pause */
		for (int i = 9; i < 13; i++, t += 300)
			pieces.push_back({cjk_at(2 * i) + cjk_at(2 * i + 1), t});
		auto rows = soft_feed(pieces, 0, &punct, &cfg, &soft);
		expect(rows.size() == 2 && count_chars(rows[0]) == 18 && count_chars(rows[1]) == 8 && soft == 1,
		       "a soft break at the first pause after 16 characters");
	}
	/* a pause before 16 characters does not break */
	{
		std::vector<std::pair<std::string, uint64_t>> pieces;
		uint64_t t = 1000;
		for (int i = 0; i < 10; i++) {
			pieces.push_back({cjk_at(2 * i) + cjk_at(2 * i + 1), t});
			t += i == 4 ? 900 : 300; /* a pause after 10 characters */
		}
		auto rows = soft_feed(pieces, 0, &punct, &cfg, &soft);
		expect(rows.size() == 1 && soft == 0, "no soft break before 16 characters, pause or not");
	}
	/* no pause: the hard limit at 16 + 8 */
	{
		std::vector<std::pair<std::string, uint64_t>> pieces;
		for (int i = 0; i < 20; i++)
			pieces.push_back({cjk_at(2 * i) + cjk_at(2 * i + 1), 1000 + 300 * (uint64_t)i});
		auto rows = soft_feed(pieces, 0, &punct, &cfg, &soft);
		expect(rows.size() == 2 && count_chars(rows[0]) == 24 && count_chars(rows[1]) == 16,
		       "without a pause the row ends at 24 characters");
	}
	/* never inside an ASCII word or a number */
	{
		std::vector<std::pair<std::string, uint64_t>> pieces;
		uint64_t t = 1000;
		for (int i = 0; i < 22; i++, t += 300)
			pieces.push_back({cjk_at(i), t});
		const std::string rest = "Hello世界一二三四五六七八九十一二三四五六七八九十12345678九十";
		size_t pos = 0;
		while (pos < rest.size()) {
			size_t start = pos;
			tea_utf8_decode(rest.data(), rest.size(), &pos);
			pieces.push_back({rest.substr(start, pos - start), t});
			t += 300;
		}
		auto rows = soft_feed(pieces, 0, &punct, &cfg, &soft);
		bool whole = false;
		for (const auto &r : rows)
			whole = whole || r.find("Hello") != std::string::npos;
		for (size_t r = 0; r + 1 < rows.size(); r++) {
			const char last = rows[r].back(), first = rows[r + 1].front();
			const bool ascii_split = std::isalnum((unsigned char)last) &&
						 std::isalnum((unsigned char)first);
			expect(!ascii_split, "a soft break never splits an ASCII word or a number");
		}
		expect(rows.size() >= 3 && soft >= 2 && whole, "the long run was broken, around the word and number");
	}
	/* a punctuation break resets the count */
	{
		std::vector<std::pair<std::string, uint64_t>> pieces;
		uint64_t t = 1000;
		for (int i = 0; i < 10; i++, t += 300)
			pieces.push_back({cjk_at(i), t});
		pieces.push_back({"，", t});
		t += 300;
		for (int i = 10; i < 30; i++, t += 300)
			pieces.push_back({cjk_at(i), t});
		auto rows = soft_feed(pieces, 0, &punct, &cfg, &soft);
		expect(rows.size() == 2 && count_chars(rows[0]) == 11 && count_chars(rows[1]) == 20 && soft == 0,
		       "after a comma break the next row counts from zero (20 < 24, no pause: no soft break)");
	}
	/* off: no soft breaks */
	{
		tea_soft_break_t off;
		tea_soft_break_defaults(&off, false, 16);
		std::vector<std::pair<std::string, uint64_t>> pieces;
		for (int i = 0; i < 20; i++)
			pieces.push_back({cjk_at(2 * i) + cjk_at(2 * i + 1), 1000 + 900 * (uint64_t)i});
		auto rows = soft_feed(pieces, 0, &punct, &off, &soft);
		expect(rows.size() == 1 && soft == 0, "off: no soft breaks");
	}
	/* growing with pauses, punctuation, words, numbers and a narrow box: rows
	 * already shown never change (checked in soft_feed). Three characters
	 * per update keeps the line under TEA_DISPLAY_MAX_CHUNKS: merging chunks
	 * can move a width-wrapped row whether soft breaks are on or not. */
	{
		const std::string full =
			"今天我們要討論字幕的顯示方式還有換行對齊與淡出的效果價格是3500元Smith said hi "
			"then 1000 people came真的嗎好我們繼續看下一段沒有標點的長句子會怎麼換行呢，"
			"然後這裡有逗號。再來一段很長很長很長很長很長很長很長的句子看看";
		std::vector<std::pair<std::string, uint64_t>> pieces;
		size_t pos = 0;
		uint64_t t = 1000;
		int k = 0;
		while (pos < full.size()) {
			size_t start = pos;
			for (int c = 0; c < 3 && pos < full.size(); c++)
				tea_utf8_decode(full.data(), full.size(), &pos);
			pieces.push_back({full.substr(start, pos - start), t});
			t += (++k % 4 == 0) ? 700 : 300;
		}
		expect(pieces.size() < TEA_DISPLAY_MAX_CHUNKS, "the test stays under the chunk limit");
		auto rows = soft_feed(pieces, 300, &punct, &cfg, &soft);
		expect(rows.size() >= 6, "the mixed text was broken into rows");
		(void)soft_feed(pieces, 0, &punct, &cfg, &soft);
		expect(soft >= 3, "with no width limit, soft breaks did the work");
	}
	/* a rewritten tail drops the soft breaks decided in it */
	{
		static tea_glyph_cache_t cache;
		tea_glyph_cache_clear(&cache);
		std::string a = cjk.substr(0, 3 * 20); /* 20 characters */
		std::string b = a + cjk.substr(3 * 20, 3 * 6);
		learn(&cache, cjk);
		tea_display_line_t line;
		tea_display_line_init(&line, 4, a.size(), 3 * 10, 0);
		soft_layout(&line, a, 0, &cache, &punct, &cfg);
		tea_display_line_observe(&line, a.c_str(), a.size(), b.c_str(), b.size(), 3 * 10, 1000 * kMs);
		std::vector<tea_wrap_row_t> raw;
		soft_layout(&line, b, 0, &cache, &punct, &cfg, &raw);
		expect(line.soft_count == 1 && line.soft_breaks[0] == a.size(), "the pause after 20 characters broke");
		std::string c = cjk.substr(0, 3 * 12) + "改";
		learn(&cache, c);
		tea_display_line_observe(&line, b.c_str(), b.size(), c.c_str(), c.size(), 3 * 10, 1300 * kMs);
		expect(line.soft_count == 0 && line.soft_decided <= 3 * 12,
		       "a tail rewritten before the break forgets it and decides again");
	}
}

static void test_chunks()
{
	static tea_glyph_cache_t cache;
	tea_glyph_cache_clear(&cache);
	learn(&cache, "我們需要一個計畫XY");

	tea_display_line_t line;
	std::string a = "我們";
	tea_display_line_init(&line, 3, a.size(), a.size(), 0);
	std::string b = "我們需要";
	expect(tea_display_line_observe(&line, a.c_str(), a.size(), b.c_str(), b.size(), b.size(), 500 * kMs),
	       "growth is a change");
	std::string c = "我們需要一個";
	tea_display_line_observe(&line, b.c_str(), b.size(), c.c_str(), c.size(), c.size(), 2500 * kMs);
	auto rows = wrap_rows(line, c, 1000, 0, true, &cache);
	expect(rows.size() == 1 && rows[0] == c,
	       "a gap between text arrivals never breaks the line (only server segments do)");
	expect(!tea_display_line_observe(&line, c.c_str(), c.size(), c.c_str(), c.size(), c.size(), 9000 * kMs),
	       "an identical snapshot is not a change");

	/* chunks: each appended run keeps its own birth time (per-run fade-in) */
	expect(line.chunk_count == 3, "each append starts a chunk");
	expect(line.chunks[1].start == a.size() && line.chunks[1].born_ns == 500 * kMs,
	       "a chunk remembers where and when it appeared");
	expect(tea_display_chunk_at(&line, 0) == 0 && tea_display_chunk_at(&line, b.size()) == 2,
	       "chunk lookup by byte offset");

	/* a rewrite (legacy preview / unstable tail) keeps the unchanged prefix's timing */
	std::string d = "我們需要XY";
	tea_display_line_observe(&line, c.c_str(), c.size(), d.c_str(), d.size(), b.size(), 3000 * kMs);
	expect(line.chunks[line.chunk_count - 1].start == b.size() &&
		       line.chunks[line.chunk_count - 1].born_ns == 3000 * kMs,
	       "rewritten text becomes a new chunk after the common prefix");
	expect(line.locked_len == b.size(), "the committed length is tracked separately from the tail");
	std::string e = "我們需要";
	tea_display_line_observe(&line, d.c_str(), d.size(), e.c_str(), e.size(), e.size(), 3100 * kMs);
	expect(line.len == e.size() && line.chunks[line.chunk_count - 1].start < e.size(),
	       "a tail that disappears simply ends the chunks there");

	/* retire (faded out): later text starts fresh */
	tea_display_line_retire(&line);
	expect(!tea_display_line_has_visible_text(&line), "a retired line shows nothing");
	std::string f = "我們需要一個計畫";
	tea_display_line_observe(&line, e.c_str(), e.size(), f.c_str(), f.size(), f.size(), 4000 * kMs);
	rows = wrap_rows(line, f, 1000, 0, true, &cache);
	expect(rows.size() == 1 && rows[0] == "一個計畫", "text arriving after a fade-out shows only the new part");
}

static void test_open_lines_do_not_fade_while_speaking()
{
	/* The reported bug: fade-out delay 1500 ms, stable commits every
	 * 1.6-2.4 s. The committed text of the open line stands still for longer
	 * than the delay while the speaker keeps talking. */
	const uint32_t delay = 1500, fade = 200;
	tea_display_line_t line;
	std::string text = "我們";
	tea_display_line_init(&line, 9, text.size(), text.size(), 0);
	tea_display_line_note_activity(&line, 1, true, 0);
	/* partials keep arriving (every 800 ms) without changing what is shown */
	uint64_t session = 0;
	for (uint64_t t = 800; t <= 2400; t += 800) {
		session = t * kMs;
		tea_display_line_note_activity(&line, 1 + t, true, t * kMs);
	}
	const uint32_t timeout = TEA_OPEN_LINE_TIMEOUT_MS;
	uint64_t now = 2500 * kMs; /* 2.5 s since the text last changed */
	uint64_t ref = tea_display_line_fade_ref(&line, session, 0, delay, timeout);
	expect(tea_fade_out_alpha(true, now, ref, delay, fade) == 1.0f,
	       "an open line with ongoing partials is fully visible even though its text is 2.5 s old");
	expect(!tea_fade_out_done(true, now, ref, delay, fade), "and is never removed mid-speech");
	expect(tea_fade_out_alpha(true, now, line.changed_ns, delay, fade) == 0.0f,
	       "(the old rule -- time since the last text change -- had already faded it out)");

	/* church soak: the segment stays open, its text and every transcript
	 * event stop for 5 s (singing, a pause, the VAD holding speech over
	 * music), audio.ack keeps coming: the line stays */
	tea_display_line_t held;
	tea_display_line_init(&held, 11, 3, 3, 0);
	tea_display_line_note_activity(&held, 1, true, 0);
	for (uint64_t t = 0; t <= 30000; t += 100) {
		const uint64_t r = tea_display_line_fade_ref(&held, 0, t * kMs, delay, timeout);
		if (tea_fade_out_alpha(true, t * kMs, r, delay, fade) != 1.0f) {
			expect(false, "an open line never fades while audio.ack progresses (30 s without any text)");
			break;
		}
	}
	/* the same line with no transcript event and no ack progress: the
	 * user's 1.5 s delay no longer applies, the safety timeout does */
	expect(tea_fade_out_alpha(true, 1800 * kMs, tea_display_line_fade_ref(&held, 0, 0, delay, timeout), delay,
				  fade) == 1.0f,
	       "an open line does not fade after the user's delay when nothing at all arrives");
	expect(tea_fade_out_alpha(true, (timeout - 1) * (uint64_t)kMs,
				  tea_display_line_fade_ref(&held, 0, 0, delay, timeout), delay, fade) == 1.0f,
	       "an open line is fully visible until the safety timeout");
	expect(tea_fade_out_done(true, (uint64_t)(timeout + fade) * kMs,
				 tea_display_line_fade_ref(&held, 0, 0, delay, timeout), delay, fade),
	       "an open line with no sign of life for the safety timeout fades out");
	expect(tea_fade_out_done(true, (uint64_t)(4000 + timeout + fade) * kMs,
				 tea_display_line_fade_ref(&held, 0, 4000 * kMs, delay, timeout), delay, fade) &&
		       !tea_fade_out_done(true, (uint64_t)(4000 + timeout) * kMs,
					  tea_display_line_fade_ref(&held, 0, 4000 * kMs, delay, timeout), delay, fade),
	       "the safety timeout counts from the last ack progress");
	/* a safety timeout shorter than the user's delay never shortens it */
	expect(tea_display_line_fade_ref(&held, 0, 0, delay, 500) == 0, "the user's delay is the minimum");

	/* activity of *another* segment also keeps an open line up: the
	 * session is still talking */
	tea_display_line_t quiet;
	tea_display_line_init(&quiet, 10, 3, 3, 0);
	tea_display_line_note_activity(&quiet, 1, true, 0);
	expect(tea_fade_out_alpha(true, 2500 * kMs, tea_display_line_fade_ref(&quiet, 2400 * kMs, 0, delay, timeout),
				  delay, fade) == 1.0f,
	       "session activity keeps every open line visible");

	/* closing counts as activity: a closed line gets the user's delay from
	 * then, whatever the session or the server does afterwards */
	tea_display_line_note_activity(&quiet, 1, false, 5000 * kMs);
	uint64_t closed_ref = tea_display_line_fade_ref(&quiet, 9000 * kMs, 9000 * kMs, delay, timeout);
	expect(closed_ref == 5000 * kMs, "a closed line ignores later session activity and ack progress");
	expect(tea_fade_out_alpha(true, 6400 * kMs, closed_ref, delay, fade) == 1.0f &&
		       tea_fade_out_done(true, 6700 * kMs, closed_ref, delay, fade),
	       "a closed line fades out `delay` after it closed");
}

static void test_end_silence_setting()
{
	expect(tea_end_silence_request(870, true, 300, 3000) == 870, "a value inside the range is sent as-is");
	expect(tea_end_silence_request(100, true, 300, 3000) == 300, "values are clamped to the server's minimum");
	expect(tea_end_silence_request(5000, true, 300, 3000) == 3000, "and to the server's maximum");
	expect(tea_end_silence_request(870, true, 400, 2000) == 870 &&
		       tea_end_silence_request(350, true, 400, 2000) == 400,
	       "the advertised range wins over the protocol bounds");
	expect(tea_end_silence_request(870, false, 300, 3000) == 0,
	       "a server that does not advertise segmentation_control gets no field at all");
	expect(tea_end_silence_request(0, true, 300, 3000) == 0, "0 = server default: no field");
	expect(tea_end_silence_from_legacy_pause(0) == 0, "an old pause of 0 (off) means server default");
	expect(tea_end_silence_from_legacy_pause(870) == 870, "an old pause value carries over");
	expect(tea_end_silence_from_legacy_pause(100) == 300 && tea_end_silence_from_legacy_pause(9000) == 3000,
	       "old values are clamped into the protocol range");
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
	tea_connection_settings_t applied = {"127.0.0.1", 8327, "", true, 0, false, ""};
	tea_connection_settings_t same = {"127.0.0.1", 8327, nullptr, true, 0, false, ""};
	expect(tea_connection_decide(&applied, &same, true, true, false) == TEA_CONNECTION_KEEP,
	       "an appearance-only update never reconnects, even while editing");
	expect(tea_connection_decide(&applied, &same, true, false, true) == TEA_CONNECTION_KEEP,
	       "Apply without connection changes does not reconnect");
	tea_connection_settings_t typing = {"127.0.0.", 8327, "", true, 0, false, ""};
	expect(tea_connection_decide(&applied, &typing, true, true, false) == TEA_CONNECTION_DEFER,
	       "a host being typed in the Properties window waits");
	expect(tea_connection_decide(&applied, &typing, true, true, true) == TEA_CONNECTION_RECONNECT,
	       "Apply / closing the window reconnects with the edited host");
	expect(tea_connection_decide(&applied, &typing, true, false, false) == TEA_CONNECTION_RECONNECT,
	       "a non-interactive update (undo, script) applies at once");
	tea_connection_settings_t stable_off = {"127.0.0.1", 8327, "", false, 0, false, ""};
	expect(tea_connection_decide(&applied, &stable_off, true, true, false) == TEA_CONNECTION_DEFER,
	       "stable captions is a session.start field: treated as a connection setting");
	tea_connection_settings_t silence = {"127.0.0.1", 8327, "", true, 870, false, ""};
	expect(tea_connection_decide(&applied, &silence, true, true, false) == TEA_CONNECTION_DEFER,
	       "the sentence break silence is a session.start field: a connection setting");
	tea_connection_settings_t tracing = {"127.0.0.1", 8327, "", true, 0, true, ""};
	expect(tea_connection_decide(&applied, &tracing, true, true, false) == TEA_CONNECTION_DEFER,
	       "the event trace applies per session: a connection setting");
	tea_connection_settings_t hints = {"127.0.0.1", 8327, "", true, 0, false, "church\x1f\x1f\x1f\x1f"};
	expect(tea_connection_decide(&applied, &hints, true, true, false) == TEA_CONNECTION_DEFER,
	       "recognition hints go out in session.start: a connection setting, never live");
	tea_connection_settings_t no_hints = {"127.0.0.1", 8327, "", true, 0, false, nullptr};
	expect(tea_connection_decide(&applied, &no_hints, true, true, false) == TEA_CONNECTION_KEEP,
	       "no hints equals empty hints");
	expect(tea_connection_decide(&applied, &same, false, true, false) == TEA_CONNECTION_RECONNECT,
	       "the first update of a new source connects");
}

/* A line on screen that the caption state hides (classified singing after
 * its first words showed, or a pause) fades out with the usual fade, from
 * that moment, whatever the open-line rules say; shown again, the caller
 * starts it over as a new line. */
static void test_hidden_lines_fade()
{
	const uint32_t delay = 1500, fade = 200;
	tea_display_line_t line;
	tea_display_line_init(&line, 7, 6, 6, 0);
	tea_display_line_note_activity(&line, 1, true, 0);
	/* open and talking: would never fade */
	const uint64_t talking =
		tea_display_line_fade_ref(&line, 10900 * kMs, 10900 * kMs, delay, TEA_OPEN_LINE_TIMEOUT_MS);
	expect(tea_fade_out_alpha(true, 11000 * kMs, talking, delay, fade) == 1.0f, "an open line is up");

	expect(tea_display_line_note_hidden(&line, true, 11000 * kMs) == TEA_HIDE_STARTED, "hidden at 11.0 s");
	expect(tea_display_line_note_hidden(&line, true, 11100 * kMs) == TEA_HIDE_KEEP, "hiding twice changes nothing");
	const uint64_t ref =
		tea_display_line_fade_ref(&line, 11150 * kMs, 11150 * kMs, delay, TEA_OPEN_LINE_TIMEOUT_MS);
	expect(tea_fade_out_alpha(true, 11000 * kMs, ref, delay, fade) == 1.0f,
	       "fully visible at the moment it is hidden");
	const float half = tea_fade_out_alpha(true, 11100 * kMs, ref, delay, fade);
	expect(half > 0.4f && half < 0.6f, "half way through the usual fade 100 ms later");
	expect(tea_fade_out_done(true, 11200 * kMs, ref, delay, fade),
	       "gone after the fade duration, although the segment is open and the session talking");
	expect(tea_display_line_note_hidden(&line, false, 11300 * kMs) == TEA_HIDE_ENDED,
	       "shown again: the caller starts it over");
	expect(!line.hidden, "no longer hidden");
}

int main()
{
	test_soft_breaks();
	test_hidden_lines_fade();
	test_box_layout();
	test_alignment();
	test_utf8_and_breaks();
	test_glyph_cache();
	test_wrapping();
	test_append_stability();
	test_chunks();
	test_punctuation_breaks();
	test_punctuation_breaks_never_move_shown_text();
	test_big_append_keeps_rows();
	test_open_lines_do_not_fade_while_speaking();
	test_end_silence_setting();
	test_row_limit();
	test_fades();
	test_frame_geometry();
	test_connection_policy();
	std::cout << "caption layout tests passed\n";
	return EXIT_SUCCESS;
}
