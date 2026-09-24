#pragma once

/*
 * OBS-free geometry and timing policy for the per-line caption renderer.
 *
 * captions-source.c draws every caption line itself (see the architecture
 * comment there). Everything that decides *where* and *how visible* a piece
 * of text is lives in this header as plain functions over plain data, so it
 * can be unit tested without libobs (tests/caption-layout-test.cpp):
 *
 *   - UTF-8 decoding and line-break classes (CJK breaks between characters,
 *     Latin breaks at spaces, closing punctuation hangs at the row end),
 *   - a glyph-advance cache fed by measurements of the real text_ft2 source,
 *   - greedy wrapping that never moves text that is already on screen,
 *   - per-line bookkeeping: appended chunks (for per-chunk fade-in), pause
 *     line breaks, rows removed by the row limit or by fading out,
 *   - fade-in / fade-out alpha over time,
 *   - horizontal alignment and the outer box geometry.
 *
 * No allocation, no OBS types; every buffer is fixed-size and owned by the
 * caller. Offsets are byte offsets into UTF-8 text and always sit on a
 * character boundary.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "caption-layout.h"

#define TEA_ALIGN_LEFT 0
#define TEA_ALIGN_CENTER 1
#define TEA_ALIGN_RIGHT 2

/* ------------------------------------------------------------------------ */
/* UTF-8 and break classes                                                   */
/* ------------------------------------------------------------------------ */

/* Decodes one code point at *pos and advances *pos. Malformed input yields
 * U+FFFD and advances by one byte, so the loop always terminates. */
static inline uint32_t tea_utf8_decode(const char *text, size_t len, size_t *pos)
{
	const unsigned char *p = (const unsigned char *)text + *pos;
	size_t left = len - *pos;
	uint32_t c = p[0];
	size_t n;
	if (c < 0x80) {
		*pos += 1;
		return c;
	}
	if ((c & 0xE0) == 0xC0) {
		n = 2;
		c &= 0x1F;
	} else if ((c & 0xF0) == 0xE0) {
		n = 3;
		c &= 0x0F;
	} else if ((c & 0xF8) == 0xF0) {
		n = 4;
		c &= 0x07;
	} else {
		*pos += 1;
		return 0xFFFD;
	}
	if (left < n) {
		*pos += 1;
		return 0xFFFD;
	}
	for (size_t i = 1; i < n; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			*pos += 1;
			return 0xFFFD;
		}
		c = (c << 6) | (uint32_t)(p[i] & 0x3F);
	}
	*pos += n;
	return c;
}

/* Encodes one code point; returns the byte count (1-4). `out` needs 5 bytes
 * and is NUL-terminated. */
static inline size_t tea_utf8_encode(uint32_t cp, char *out)
{
	size_t n;
	if (cp < 0x80) {
		out[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	}
	out[n] = '\0';
	return n;
}

static inline bool tea_cp_is_space(uint32_t c)
{
	return c == 0x20 || c == 0x09 || c == 0x3000;
}

/* Combining marks, joiners, variation selectors and emoji modifiers: never
 * separated from the character before them. */
static inline bool tea_cp_is_extend(uint32_t c)
{
	return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
	       c == 0x200C || c == 0x200D || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) ||
	       (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F);
}

/* Characters that may be broken around without a space: CJK ideographs,
 * kana, hangul, fullwidth forms and emoji. */
static inline bool tea_cp_is_wide(uint32_t c)
{
	return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
	       (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
	       (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
	       (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) ||
	       (c >= 0x20000 && c <= 0x3FFFD);
}

/* Closing punctuation: never starts a row. When it would overflow the row it
 * hangs at the end of the row instead, so no already-shown character has to
 * move to the next row to keep it company. */
static inline bool tea_cp_is_closing(uint32_t c)
{
	switch (c) {
	case '!':
	case ')':
	case ',':
	case '.':
	case ':':
	case ';':
	case '?':
	case ']':
	case '}':
	case '%':
	case 0x2019: /* ’ */
	case 0x201D: /* ” */
	case 0x2025: /* ‥ */
	case 0x2026: /* … */
	case 0x3001: /* 、 */
	case 0x3002: /* 。 */
	case 0x3009: /* 〉 */
	case 0x300B: /* 》 */
	case 0x300D: /* 」 */
	case 0x300F: /* 』 */
	case 0x3011: /* 】 */
	case 0x3015: /* 〕 */
	case 0x3017: /* 〗 */
	case 0x301E:
	case 0x301F:
	case 0x30FC: /* ー */
	case 0xFF01: /* ！ */
	case 0xFF09: /* ） */
	case 0xFF0C: /* ， */
	case 0xFF0E: /* ． */
	case 0xFF1A: /* ： */
	case 0xFF1B: /* ； */
	case 0xFF1F: /* ？ */
	case 0xFF3D: /* ］ */
	case 0xFF5D: /* ｝ */
		return true;
	default:
		return false;
	}
}

/* Opening punctuation: never ends a row. */
static inline bool tea_cp_is_opening(uint32_t c)
{
	switch (c) {
	case '(':
	case '[':
	case '{':
	case 0x2018: /* ‘ */
	case 0x201C: /* “ */
	case 0x3008: /* 〈 */
	case 0x300A: /* 《 */
	case 0x300C: /* 「 */
	case 0x300E: /* 『 */
	case 0x3010: /* 【 */
	case 0x3014: /* 〔 */
	case 0x3016: /* 〖 */
	case 0xFF08: /* （ */
	case 0xFF3B: /* ［ */
	case 0xFF5B: /* ｛ */
		return true;
	default:
		return false;
	}
}

/* May a row end between `prev` and `next`? */
static inline bool tea_can_break_between(uint32_t prev, uint32_t next)
{
	if (tea_cp_is_extend(next) || prev == 0x200D)
		return false;
	if (tea_cp_is_space(next) || tea_cp_is_space(prev))
		return true;
	if (tea_cp_is_closing(next) || tea_cp_is_opening(prev))
		return false;
	return tea_cp_is_wide(prev) || tea_cp_is_wide(next);
}

/* Length of the longest common byte prefix of a and b that ends on a
 * character boundary of both. */
static inline size_t tea_utf8_common_prefix(const char *a, size_t a_len, const char *b, size_t b_len)
{
	size_t n = 0;
	size_t max = a_len < b_len ? a_len : b_len;
	while (n < max && a[n] == b[n])
		n++;
	while (n > 0 && ((n < a_len && ((unsigned char)a[n] & 0xC0) == 0x80) ||
			 (n < b_len && ((unsigned char)b[n] & 0xC0) == 0x80)))
		n--;
	return n;
}

/* ------------------------------------------------------------------------ */
/* Glyph advance cache                                                       */
/* ------------------------------------------------------------------------ */

/*
 * text_ft2 positions glyphs by summing each glyph's advance (no kerning), and
 * reports width = sum(advances) + outline_extra. So the width of any run of
 * text is known exactly once every code point in it has been measured once.
 * The source measures unknown code points with a private text_ft2 child and
 * stores the advance here.
 */
#define TEA_GLYPH_CACHE_SLOTS 4096u
#define TEA_GLYPH_CACHE_LIMIT (TEA_GLYPH_CACHE_SLOTS * 3u / 4u)

typedef struct {
	uint32_t cp; /* 0 = empty slot (NUL never reaches the renderer) */
	uint16_t advance;
	uint8_t seen_by; /* bitmask of measuring workers that already hold the glyph */
	uint8_t pad;
} tea_glyph_slot_t;

typedef struct {
	tea_glyph_slot_t slots[TEA_GLYPH_CACHE_SLOTS];
	uint32_t count;
} tea_glyph_cache_t;

static inline void tea_glyph_cache_clear(tea_glyph_cache_t *cache)
{
	memset(cache, 0, sizeof(*cache));
}

static inline uint32_t tea_glyph_cache_hash(uint32_t cp)
{
	return (cp * 2654435761u) & (TEA_GLYPH_CACHE_SLOTS - 1u);
}

static inline const tea_glyph_slot_t *tea_glyph_cache_find(const tea_glyph_cache_t *cache, uint32_t cp)
{
	if (cp == 0)
		return NULL;
	uint32_t i = tea_glyph_cache_hash(cp);
	for (uint32_t probes = 0; probes < TEA_GLYPH_CACHE_SLOTS; probes++) {
		const tea_glyph_slot_t *slot = &cache->slots[i];
		if (slot->cp == cp)
			return slot;
		if (slot->cp == 0)
			return NULL;
		i = (i + 1u) & (TEA_GLYPH_CACHE_SLOTS - 1u);
	}
	return NULL;
}

static inline bool tea_glyph_cache_lookup(const tea_glyph_cache_t *cache, uint32_t cp, uint32_t *advance)
{
	const tea_glyph_slot_t *slot = tea_glyph_cache_find(cache, cp);
	if (!slot)
		return false;
	if (advance)
		*advance = slot->advance;
	return true;
}

/* Stores (or overwrites) an advance. A full cache is cleared first; the
 * caller re-measures what it needs. Returns the slot. */
static inline tea_glyph_slot_t *tea_glyph_cache_store(tea_glyph_cache_t *cache, uint32_t cp, uint32_t advance)
{
	if (cp == 0)
		return NULL;
	tea_glyph_slot_t *existing = (tea_glyph_slot_t *)tea_glyph_cache_find(cache, cp);
	if (!existing && cache->count >= TEA_GLYPH_CACHE_LIMIT)
		tea_glyph_cache_clear(cache);
	uint32_t i = tea_glyph_cache_hash(cp);
	while (cache->slots[i].cp != 0 && cache->slots[i].cp != cp)
		i = (i + 1u) & (TEA_GLYPH_CACHE_SLOTS - 1u);
	tea_glyph_slot_t *slot = &cache->slots[i];
	if (slot->cp == 0) {
		slot->cp = cp;
		slot->seen_by = 0;
		cache->count++;
	}
	slot->advance = (uint16_t)(advance > 0xFFFFu ? 0xFFFFu : advance);
	return slot;
}

/* The advance of one code point from a text_ft2 measurement of a string
 * holding just that code point: width = advance + outline_extra. */
static inline uint32_t tea_glyph_advance_from_measure(uint32_t measured_width, uint32_t outline_extra)
{
	return measured_width > outline_extra ? measured_width - outline_extra : 0;
}

/* text_ft2's constant extra width (outline / drop shadow margin), derived
 * from measuring "X" and "XX": w1 = a + e, w2 = 2a + e. */
static inline uint32_t tea_outline_extra_from_measures(uint32_t width_one, uint32_t width_two)
{
	uint64_t twice = (uint64_t)width_one * 2u;
	return twice > width_two ? (uint32_t)(twice - width_two) : 0;
}

/* Collects up to `cap` distinct code points of text[from, to) that the cache
 * does not know yet. Returns how many were written. */
static inline int tea_glyph_cache_missing(const tea_glyph_cache_t *cache, const char *text, size_t from, size_t to,
					  uint32_t *out, int cap)
{
	int n = 0;
	size_t pos = from;
	while (pos < to && n < cap) {
		uint32_t cp = tea_utf8_decode(text, to, &pos);
		if (cp == 0 || tea_glyph_cache_lookup(cache, cp, NULL))
			continue;
		bool dup = false;
		for (int i = 0; i < n; i++) {
			if (out[i] == cp) {
				dup = true;
				break;
			}
		}
		if (!dup)
			out[n++] = cp;
	}
	return n;
}

/* Sum of advances of text[from, to); false when a glyph is unknown. */
static inline bool tea_text_advance(const tea_glyph_cache_t *cache, const char *text, size_t from, size_t to,
				    uint32_t *out)
{
	uint32_t sum = 0;
	size_t pos = from;
	while (pos < to) {
		uint32_t adv = 0;
		uint32_t cp = tea_utf8_decode(text, to, &pos);
		if (!tea_glyph_cache_lookup(cache, cp, &adv))
			return false;
		sum += adv;
	}
	*out = sum;
	return true;
}

/* ------------------------------------------------------------------------ */
/* Per-line bookkeeping                                                      */
/* ------------------------------------------------------------------------ */

#define TEA_DISPLAY_MAX_CHUNKS 48
#define TEA_DISPLAY_MAX_HARD_BREAKS 16
#define TEA_NS_PER_MS UINT64_C(1000000)

/* A run of text that appeared at one time. Chunk i covers
 * [chunks[i].start, chunks[i + 1].start) (the last one up to len). */
typedef struct {
	size_t start;
	uint64_t born_ns;
} tea_display_chunk_t;

typedef struct {
	uint64_t key;
	size_t len;          /* bytes of the displayed text (committed + tail) */
	size_t locked_len;   /* bytes of committed text; the rest is the unstable tail */
	size_t visible_from; /* text before this offset has left the screen for good */
	tea_display_chunk_t chunks[TEA_DISPLAY_MAX_CHUNKS];
	int chunk_count;
	size_t hard_breaks[TEA_DISPLAY_MAX_HARD_BREAKS]; /* pause breaks, ascending */
	int hard_break_count;
	uint64_t changed_ns; /* last time the displayed text changed */
	bool used;
	bool persistent; /* never fades out (the waiting placeholder) */
} tea_display_line_t;

static inline void tea_display_line_init(tea_display_line_t *line, uint64_t key, size_t len, size_t locked_len,
					 uint64_t now_ns)
{
	memset(line, 0, sizeof(*line));
	line->used = true;
	line->key = key;
	line->len = len;
	line->locked_len = locked_len < len ? locked_len : len;
	line->changed_ns = now_ns;
	if (len > 0) {
		line->chunks[0].start = 0;
		line->chunks[0].born_ns = now_ns;
		line->chunk_count = 1;
	}
}

static inline void tea_display_line_add_hard_break(tea_display_line_t *line, size_t at)
{
	for (int i = 0; i < line->hard_break_count; i++) {
		if (line->hard_breaks[i] == at)
			return;
	}
	if (line->hard_break_count == TEA_DISPLAY_MAX_HARD_BREAKS) {
		memmove(&line->hard_breaks[0], &line->hard_breaks[1],
			sizeof(line->hard_breaks[0]) * (TEA_DISPLAY_MAX_HARD_BREAKS - 1));
		line->hard_break_count--;
	}
	int pos = line->hard_break_count;
	while (pos > 0 && line->hard_breaks[pos - 1] > at) {
		line->hard_breaks[pos] = line->hard_breaks[pos - 1];
		pos--;
	}
	line->hard_breaks[pos] = at;
	line->hard_break_count++;
}

/*
 * The displayed text of a line changed from old_text to new_text (committed
 * text plus unstable tail). Keeps the timing of the unchanged prefix, starts a
 * new chunk for what follows it, and -- when the text had not changed for at
 * least pause_ms -- starts a new row there (the "sentence break" pause).
 * Text removed from the end (a tail that went away) simply ends the chunks
 * there. Returns false when nothing changed.
 */
static inline bool tea_display_line_observe(tea_display_line_t *line, const char *old_text, size_t old_len,
					    const char *new_text, size_t new_len, size_t locked_len, uint64_t now_ns,
					    uint32_t pause_ms)
{
	line->locked_len = locked_len < new_len ? locked_len : new_len;
	if (old_len == new_len && (old_len == 0 || memcmp(old_text, new_text, old_len) == 0))
		return false;

	size_t common = tea_utf8_common_prefix(old_text, old_len, new_text, new_len);
	bool paused = pause_ms > 0 && now_ns >= line->changed_ns &&
		      now_ns - line->changed_ns >= (uint64_t)pause_ms * TEA_NS_PER_MS;

	while (line->chunk_count > 0 && line->chunks[line->chunk_count - 1].start >= common && common < new_len)
		line->chunk_count--;
	while (line->chunk_count > 0 && line->chunks[line->chunk_count - 1].start >= new_len)
		line->chunk_count--;
	while (line->hard_break_count > 0 && line->hard_breaks[line->hard_break_count - 1] > common)
		line->hard_break_count--;

	if (new_len > common) {
		if (line->chunk_count == TEA_DISPLAY_MAX_CHUNKS) {
			/* Merge the two oldest chunks; they finished fading in long ago. */
			memmove(&line->chunks[1], &line->chunks[2],
				sizeof(line->chunks[0]) * (TEA_DISPLAY_MAX_CHUNKS - 2));
			line->chunk_count--;
		}
		line->chunks[line->chunk_count].start = common;
		line->chunks[line->chunk_count].born_ns = now_ns;
		line->chunk_count++;
		if (paused && common > line->visible_from)
			tea_display_line_add_hard_break(line, common);
	}
	if (line->visible_from > common)
		line->visible_from = common;
	line->len = new_len;
	line->changed_ns = now_ns;
	return true;
}

/* Every character shown so far leaves the screen (after fading out). Text
 * that arrives later for the same line starts on a fresh row. */
static inline void tea_display_line_retire(tea_display_line_t *line)
{
	line->visible_from = line->len;
}

static inline bool tea_display_line_has_visible_text(const tea_display_line_t *line)
{
	return line->visible_from < line->len;
}

/* Index of the chunk containing byte `at` (0 when there are none). */
static inline int tea_display_chunk_at(const tea_display_line_t *line, size_t at)
{
	int idx = 0;
	for (int i = 0; i < line->chunk_count; i++) {
		if (line->chunks[i].start <= at)
			idx = i;
		else
			break;
	}
	return idx;
}

/* ------------------------------------------------------------------------ */
/* Wrapping                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
	size_t start;   /* first byte shown on the row */
	size_t end;     /* one past the last byte shown (trailing spaces trimmed) */
	size_t next;    /* where the following row starts; everything before it belongs to this row or earlier */
	uint32_t width; /* sum of advances of [start, end) + outline extra: text_ft2's width for the row */
} tea_wrap_row_t;

#define TEA_WRAP_MISSING_GLYPH (-1)

static inline size_t tea_skip_spaces(const char *text, size_t pos, size_t end)
{
	while (pos < end) {
		size_t next = pos;
		uint32_t cp = tea_utf8_decode(text, end, &next);
		if (!tea_cp_is_space(cp))
			break;
		pos = next;
	}
	return pos;
}

static inline size_t tea_trim_trailing_spaces(const char *text, size_t start, size_t end)
{
	while (end > start) {
		size_t back = end - 1;
		while (back > start && ((unsigned char)text[back] & 0xC0) == 0x80)
			back--;
		size_t probe = back;
		uint32_t cp = tea_utf8_decode(text, end, &probe);
		if (!tea_cp_is_space(cp))
			break;
		end = back;
	}
	return end;
}

/*
 * Greedy wrap of text[from, to) into rows no wider than max_width
 * (0 = unlimited). A row "fits" when its text_ft2 width (advances +
 * outline_extra) is <= max_width.
 *
 * `chunk_starts` are the offsets where later-arriving text begins. When a
 * row overflows, the break may not be placed before the start of the chunk
 * that overflowed: characters that were already on screen stay on their
 * row. The chunk start itself is always a valid break. `word_wrap=false`
 * breaks right before the overflowing character (text_ft2's own behavior
 * without word wrapping). Closing punctuation hangs past the edge instead
 * of starting a row.
 *
 * Rows go to rows[total % cap], so with more rows than `cap` the last `cap`
 * are kept. Returns the total row count, or TEA_WRAP_MISSING_GLYPH.
 */
static inline int tea_wrap_paragraph(const char *text, size_t from, size_t to, const size_t *chunk_starts,
				     int chunk_count, uint32_t max_width, uint32_t outline_extra, bool word_wrap,
				     const tea_glyph_cache_t *cache, tea_wrap_row_t *rows, int cap, int total)
{
	size_t pos = tea_skip_spaces(text, from, to);
	while (pos < to) {
		size_t row_start = pos;
		size_t cut = to;
		size_t resume = to;
		uint32_t width = outline_extra;
		uint32_t prev = 0;
		bool have_prev = false;
		size_t last_opportunity = 0;
		bool have_opportunity = false;
		size_t i = pos;
		while (i < to) {
			size_t here = i;
			uint32_t adv = 0;
			uint32_t cp = tea_utf8_decode(text, to, &i);
			if (!tea_glyph_cache_lookup(cache, cp, &adv))
				return TEA_WRAP_MISSING_GLYPH;
			if (have_prev && tea_can_break_between(prev, cp)) {
				last_opportunity = here;
				have_opportunity = true;
			}
			bool overflow = max_width > 0 && have_prev && !tea_cp_is_extend(cp) && !tea_cp_is_space(cp) &&
					(uint64_t)width + adv > max_width;
			if (overflow) {
				if (tea_cp_is_closing(cp)) {
					/* hang it: the row ends right after it */
					size_t after = i;
					while (after < to) {
						size_t peek = after;
						uint32_t next_cp = tea_utf8_decode(text, to, &peek);
						if (!tea_cp_is_extend(next_cp))
							break;
						after = peek;
					}
					cut = after;
					resume = after;
					break;
				}
				size_t floor = row_start;
				for (int c = 0; c < chunk_count; c++) {
					if (chunk_starts[c] <= here && chunk_starts[c] > floor)
						floor = chunk_starts[c];
				}
				if (!word_wrap)
					cut = here; /* at or after floor by construction */
				else if (have_opportunity && last_opportunity >= floor && last_opportunity > row_start)
					cut = last_opportunity;
				else if (floor > row_start)
					cut = floor;
				else
					cut = here; /* one word wider than the row: break inside it */
				resume = cut;
				break;
			}
			width += adv;
			prev = cp;
			have_prev = true;
		}

		size_t end = tea_trim_trailing_spaces(text, row_start, cut);
		uint32_t row_width = outline_extra;
		uint32_t sum = 0;
		if (tea_text_advance(cache, text, row_start, end, &sum))
			row_width += sum;
		resume = tea_skip_spaces(text, resume, to);
		tea_wrap_row_t *row = &rows[total % cap];
		row->start = row_start;
		row->end = end;
		row->next = resume;
		row->width = row_width;
		total++;
		pos = resume;
	}
	return total;
}

/*
 * Wraps the visible part of one line: [visible_from, len), split into
 * paragraphs at the pause breaks. Returns the row count (the last `cap` rows
 * are in `rows`, oldest first after tea_wrap_rows_in_order()) or
 * TEA_WRAP_MISSING_GLYPH.
 */
static inline int tea_wrap_line(const tea_display_line_t *line, const char *text, uint32_t max_width,
				uint32_t outline_extra, bool word_wrap, const tea_glyph_cache_t *cache,
				tea_wrap_row_t *rows, int cap)
{
	size_t starts[TEA_DISPLAY_MAX_CHUNKS + 1];
	int n = 0;
	for (int i = 0; i < line->chunk_count; i++)
		starts[n++] = line->chunks[i].start;
	if (line->locked_len > 0 && line->locked_len < line->len)
		starts[n++] = line->locked_len; /* the tail never pushes committed text around */

	int total = 0;
	size_t para = line->visible_from;
	for (int h = 0; h <= line->hard_break_count; h++) {
		size_t para_end = h < line->hard_break_count ? line->hard_breaks[h] : line->len;
		if (para_end <= para)
			continue;
		if (para_end > line->len)
			para_end = line->len;
		total = tea_wrap_paragraph(text, para, para_end, starts, n, max_width, outline_extra, word_wrap, cache,
					   rows, cap, total);
		if (total == TEA_WRAP_MISSING_GLYPH)
			return TEA_WRAP_MISSING_GLYPH;
		para = para_end;
	}
	return total;
}

/* After tea_wrap_line() returned `total` rows into a ring of `cap`, copies
 * the kept rows oldest-first into `out` (room for cap rows). Returns the
 * number copied. */
static inline int tea_wrap_rows_in_order(const tea_wrap_row_t *ring, int total, int cap, tea_wrap_row_t *out)
{
	if (total <= 0)
		return 0;
	int kept = total < cap ? total : cap;
	int first = total - kept;
	for (int i = 0; i < kept; i++)
		out[i] = ring[(first + i) % cap];
	return kept;
}

/* ------------------------------------------------------------------------ */
/* Row limit                                                                 */
/* ------------------------------------------------------------------------ */

/* How many of the oldest rows have to leave so at most max_rows remain
 * (max_rows <= 0: no limit). */
static inline int tea_rows_to_evict(int total_rows, int max_rows)
{
	if (max_rows <= 0 || total_rows <= max_rows)
		return 0;
	return total_rows - max_rows;
}

/* A whole row left the screen: the line never shows anything before
 * `row_next` again. Rows are removed whole, so no shown character of a
 * remaining row changes. */
static inline void tea_display_line_evict_through(tea_display_line_t *line, size_t row_next)
{
	if (row_next > line->visible_from)
		line->visible_from = row_next;
}

/* ------------------------------------------------------------------------ */
/* Fades                                                                     */
/* ------------------------------------------------------------------------ */

static inline float tea_smoothstep(float t)
{
	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	return t * t * (3.0f - 2.0f * t);
}

/* Opacity of text that appeared at born_ns, fading in over fade_ms
 * (0 or disabled: fully visible at once). */
static inline float tea_fade_in_alpha(bool enabled, uint64_t now_ns, uint64_t born_ns, uint32_t fade_ms)
{
	if (!enabled || fade_ms == 0)
		return 1.0f;
	if (now_ns <= born_ns)
		return 0.0f;
	double t = (double)(now_ns - born_ns) / ((double)fade_ms * (double)TEA_NS_PER_MS);
	return tea_smoothstep((float)t);
}

/* Opacity of a line whose text last changed at changed_ns: fully visible for
 * delay_ms, then fading out over fade_ms. A change resets changed_ns, which
 * cancels a fade in progress. */
static inline float tea_fade_out_alpha(bool enabled, uint64_t now_ns, uint64_t changed_ns, uint32_t delay_ms,
				       uint32_t fade_ms)
{
	if (!enabled || now_ns <= changed_ns)
		return 1.0f;
	uint64_t idle = now_ns - changed_ns;
	uint64_t delay = (uint64_t)delay_ms * TEA_NS_PER_MS;
	if (idle < delay)
		return 1.0f;
	if (fade_ms == 0)
		return 0.0f;
	double t = (double)(idle - delay) / ((double)fade_ms * (double)TEA_NS_PER_MS);
	return 1.0f - tea_smoothstep((float)t);
}

/* True once the fade-out has finished and the line's rows can be removed. */
static inline bool tea_fade_out_done(bool enabled, uint64_t now_ns, uint64_t changed_ns, uint32_t delay_ms,
				     uint32_t fade_ms)
{
	if (!enabled || now_ns <= changed_ns)
		return false;
	return now_ns - changed_ns >= ((uint64_t)delay_ms + fade_ms) * TEA_NS_PER_MS;
}

/* ------------------------------------------------------------------------ */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------ */

/* Horizontal offset of a row of row_width inside available pixels. Integer
 * pixels, so glyphs stay on the pixel grid. */
static inline uint32_t tea_row_offset_x(int align, uint32_t available, uint32_t row_width)
{
	uint32_t slack = available > row_width ? available - row_width : 0;
	if (align == TEA_ALIGN_CENTER)
		return slack / 2u;
	if (align == TEA_ALIGN_RIGHT)
		return slack;
	return 0;
}

#define TEA_TEXT_ROW_SPACING 4 /* text_ft2's own gap between lines */
#define TEA_LEGACY_MIN_CUSTOM_WIDTH 100 /* text_ft2 ignores custom_width below this */

typedef struct {
	uint32_t wrap_width; /* max text_ft2 row width; 0 = unlimited */
	bool word_wrap;      /* prefer breaking at spaces */
	uint32_t inset_x;    /* padding + background padding */
	uint32_t inset_y;
	uint32_t row_pitch; /* distance between row tops */
	uint32_t row_height;
} tea_caption_frame_t;

/*
 * Where rows may go.
 *
 *  Fixed: the outer box is caption_width wide; rows wrap at the inner width.
 *  Auto (legacy scenes): text_ft2 wrapped at custom_width when it was >= 100
 *  (and ignored word_wrap for CJK anyway); below that it never wrapped and
 *  the box grew with the text. Both are preserved.
 *
 * With the background box enabled its padding is added inside the source,
 * so the box never draws outside the source's bounds, and rows are spaced so
 * neighbouring boxes do not overlap (overlapping translucent boxes would
 * show a darker band).
 */
static inline tea_caption_frame_t tea_caption_frame_resolve(int mode, int caption_width, int padding,
							    int legacy_custom_width, bool legacy_word_wrap,
							    uint32_t outline_extra, uint32_t row_height,
							    bool background, int background_padding)
{
	tea_caption_frame_t f;
	uint32_t pad = (uint32_t)tea_caption_layout_clamp_padding(padding);
	uint32_t bg = background && background_padding > 0 ? (uint32_t)background_padding : 0;
	f.inset_x = pad + bg;
	f.inset_y = pad + bg;
	f.row_height = row_height;
	uint32_t spacing = TEA_TEXT_ROW_SPACING;
	if (bg * 2u > spacing)
		spacing = bg * 2u;
	f.row_pitch = row_height + spacing;
	if (mode == TEA_LAYOUT_MODE_FIXED) {
		uint32_t outer = tea_caption_layout_clamp_fixed_width(caption_width);
		uint32_t insets = f.inset_x * 2u;
		f.wrap_width = outer > insets + 1u ? outer - insets : 1u;
		f.word_wrap = true;
	} else if (legacy_custom_width >= TEA_LEGACY_MIN_CUSTOM_WIDTH) {
		f.wrap_width = (uint32_t)legacy_custom_width + outline_extra;
		f.word_wrap = legacy_word_wrap;
	} else {
		f.wrap_width = 0;
		f.word_wrap = legacy_word_wrap;
	}
	return f;
}

/* Width available for aligning rows, and the outer source width. */
static inline uint32_t tea_caption_frame_content_width(const tea_caption_frame_t *f, int mode, int caption_width,
						       uint32_t widest_row)
{
	if (mode == TEA_LAYOUT_MODE_FIXED) {
		uint32_t outer = tea_caption_layout_clamp_fixed_width(caption_width);
		return outer > f->inset_x * 2u ? outer - f->inset_x * 2u : 0;
	}
	if (f->wrap_width > 0)
		return f->wrap_width;
	return widest_row;
}

static inline uint32_t tea_caption_frame_outer_width(const tea_caption_frame_t *f, int mode, int caption_width,
						     uint32_t widest_row)
{
	if (mode == TEA_LAYOUT_MODE_FIXED)
		return tea_caption_layout_clamp_fixed_width(caption_width);
	return tea_caption_frame_content_width(f, mode, caption_width, widest_row) + f->inset_x * 2u;
}

/* Outer height for `rows` rows; an empty caption keeps one row of height,
 * like the single text_ft2 child did, so the source never collapses. */
static inline uint32_t tea_caption_frame_outer_height(const tea_caption_frame_t *f, int rows)
{
	uint32_t n = rows > 0 ? (uint32_t)rows : 1u;
	return f->inset_y * 2u + (n - 1u) * f->row_pitch + f->row_height;
}

static inline uint32_t tea_caption_frame_row_y(const tea_caption_frame_t *f, int row)
{
	return f->inset_y + (uint32_t)row * f->row_pitch;
}

/* ------------------------------------------------------------------------ */
/* Connection vs. appearance settings                                        */
/* ------------------------------------------------------------------------ */

/*
 * Only these settings reach the server (session.start / the WebSocket URL /
 * the bearer token). Everything else is appearance and is applied by
 * redrawing only. `stable` is here because `stable` is a session.start field:
 * the server cannot switch it inside a running session.
 */
typedef struct {
	const char *host;
	int port;
	const char *token_path;
	bool stable;
} tea_connection_settings_t;

static inline bool tea_str_equal_or_both_empty(const char *a, const char *b)
{
	if (!a)
		a = "";
	if (!b)
		b = "";
	return strcmp(a, b) == 0;
}

static inline bool tea_connection_settings_equal(const tea_connection_settings_t *a, const tea_connection_settings_t *b)
{
	return tea_str_equal_or_both_empty(a->host, b->host) && a->port == b->port &&
	       tea_str_equal_or_both_empty(a->token_path, b->token_path) && a->stable == b->stable;
}

#define TEA_CONNECTION_KEEP 0      /* nothing to do */
#define TEA_CONNECTION_RECONNECT 1 /* apply now: new session */
#define TEA_CONNECTION_DEFER 2     /* changed while the Properties window is open: wait for Apply / close */

/*
 * Decides what an update() call does with the connection. `interactive` is
 * true while a Properties window for the source is open (OBS calls update()
 * ~500 ms after every edit there, including every keystroke in the host
 * field); `force` is the Apply button, the window closing, or the very first
 * update when the source is created/loaded.
 */
static inline int tea_connection_decide(const tea_connection_settings_t *applied,
					const tea_connection_settings_t *incoming, bool have_applied, bool interactive,
					bool force)
{
	if (have_applied && tea_connection_settings_equal(applied, incoming))
		return TEA_CONNECTION_KEEP;
	if (!have_applied || force || !interactive)
		return TEA_CONNECTION_RECONNECT;
	return TEA_CONNECTION_DEFER;
}
