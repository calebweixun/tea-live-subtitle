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
 *   - per-line bookkeeping: appended chunks (for per-chunk fade-in), rows
 *     removed by the row limit or by fading out, speech activity,
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
	tea_glyph_slot_t entries[TEA_GLYPH_CACHE_SLOTS];
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
		const tea_glyph_slot_t *slot = &cache->entries[i];
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
	while (cache->entries[i].cp != 0 && cache->entries[i].cp != cp)
		i = (i + 1u) & (TEA_GLYPH_CACHE_SLOTS - 1u);
	tea_glyph_slot_t *slot = &cache->entries[i];
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
	uint64_t changed_ns;  /* last time the displayed text changed */
	uint64_t activity_ns; /* last transcript event of this line's segment, or its close */
	uint64_t activity;    /* tea_caption_snapshot_line_t.activity last seen */
	bool open;            /* the server segment may still grow */
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
	line->activity_ns = now_ns;
	if (len > 0) {
		line->chunks[0].start = 0;
		line->chunks[0].born_ns = now_ns;
		line->chunk_count = 1;
	}
}

/*
 * The displayed text of a line changed from old_text to new_text (committed
 * text plus unstable tail). Keeps the timing of the unchanged prefix and starts
 * a new chunk for what follows it. Text removed from the end simply ends the
 * chunks there. Returns false when nothing changed.
 *
 * Line breaks between sentences come from the server alone: every server
 * segment is its own line, and the silence that ends a segment is the
 * server's VAD setting (session.start.segmentation.end_silence_ms). Gaps
 * between text arrivals are quantised by the server's preview cadence
 * (~800 ms) and say nothing reliable about pauses in the audio.
 */
static inline bool tea_display_line_observe(tea_display_line_t *line, const char *old_text, size_t old_len,
					    const char *new_text, size_t new_len, size_t locked_len, uint64_t now_ns)
{
	line->locked_len = locked_len < new_len ? locked_len : new_len;
	if (old_len == new_len && (old_len == 0 || memcmp(old_text, new_text, old_len) == 0))
		return false;

	size_t common = tea_utf8_common_prefix(old_text, old_len, new_text, new_len);

	while (line->chunk_count > 0 && line->chunks[line->chunk_count - 1].start >= common && common < new_len)
		line->chunk_count--;
	while (line->chunk_count > 0 && line->chunks[line->chunk_count - 1].start >= new_len)
		line->chunk_count--;

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
	}
	if (line->visible_from > common)
		line->visible_from = common;
	line->len = new_len;
	line->changed_ns = now_ns;
	if (line->activity_ns < now_ns)
		line->activity_ns = now_ns;
	return true;
}

/*
 * Speech activity of a line's segment, from the caption state's snapshot:
 * `activity` changes on every transcript event of the segment (a partial
 * that changes nothing on screen included), `open` says the segment may
 * still grow. Closing the segment counts as activity too, so a line that
 * just closed gets the full fade-out delay from that moment.
 */
static inline void tea_display_line_note_activity(tea_display_line_t *line, uint64_t activity, bool open,
						  uint64_t now_ns)
{
	if (activity != line->activity || open != line->open) {
		line->activity = activity;
		if (line->activity_ns < now_ns)
			line->activity_ns = now_ns;
	}
	line->open = open;
}

/*
 * A line whose server segment is still open stays up this long after the
 * last sign of life -- an event of its segment, any transcript event of the
 * session, or audio.ack progress -- before it may fade: a safety net for a
 * segment the server never closes. Not a user setting.
 */
#define TEA_OPEN_LINE_TIMEOUT_MS 10000

/*
 * The moment a line's fade-out delay (`fade_delay_ms`, the user's setting)
 * counts from.
 *
 * A closed line fades `delay` after its last change (closing counts as a
 * change: tea_display_line_note_activity()).
 *
 * A line whose server segment is still open never fades while the server
 * shows it is alive. The text of an open segment can stand still for many
 * seconds while the speaker is talking, pausing, singing, or the VAD holds
 * speech over music: the server does not resend an identical preview, and
 * stable commits need agreeing previews. So an open line only fades
 * `open_timeout_ms` after the latest of its own activity, the session's
 * transcript activity (session_activity_ns) and audio.ack progress
 * (server_progress_ns). The result is shifted so that the usual
 * `ref + fade_delay_ms` arithmetic gives that moment.
 */
static inline uint64_t tea_display_line_fade_ref(const tea_display_line_t *line, uint64_t session_activity_ns,
						 uint64_t server_progress_ns, uint32_t fade_delay_ms,
						 uint32_t open_timeout_ms)
{
	uint64_t ref = line->changed_ns > line->activity_ns ? line->changed_ns : line->activity_ns;
	if (!line->open)
		return ref;
	if (session_activity_ns > ref)
		ref = session_activity_ns;
	if (server_progress_ns > ref)
		ref = server_progress_ns;
	if (open_timeout_ms > fade_delay_ms)
		ref += (uint64_t)(open_timeout_ms - fade_delay_ms) * 1000000ULL;
	return ref;
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
	size_t start;     /* first byte shown on the row */
	size_t end;       /* one past the last byte shown (trailing spaces trimmed) */
	size_t next;      /* where the following row starts; everything before it belongs to this row or earlier */
	uint32_t width;   /* sum of advances of [start, end) + outline extra: text_ft2's width for the row */
	bool punct_break; /* the row ends because of a punctuation line break */
} tea_wrap_row_t;

#define TEA_WRAP_MISSING_GLYPH (-1)

/* ------------------------------------------------------------------------ */
/* Punctuation line breaks                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Fast speakers rarely pause long enough for the server to end a segment, so
 * one segment can hold several clauses. With punctuation breaks a row also
 * ends right after sentence punctuation (and, optionally, a comma); the
 * punctuation stays at the end of the row it closes and the next row starts
 * at the next non-space character.
 *
 * Nothing on screen ever moves because of it: the decision for a mark is
 * made from the text up to the first character after the mark (and the row
 * it is on), so it is known before any character after the mark is shown and
 * never changes while the text only grows. The server's preview hypothesis
 * ends with a "。" that is not shown (caption-state.c trims it), so a break
 * only follows punctuation the viewer can see.
 */
#define TEA_PUNCT_BREAK_OFF 0
#define TEA_PUNCT_BREAK_SENTENCE 1 /* 。？！…；  and . ? ! ; */
#define TEA_PUNCT_BREAK_COMMA 2    /* the above plus ，、 and , */
#define TEA_PUNCT_COMMA_MIN_DEFAULT 8
#define TEA_PUNCT_COMMA_MIN_MAX 40

typedef struct {
	int mode;            /* TEA_PUNCT_BREAK_* */
	int comma_min_chars; /* a comma breaks only after this many visible characters on the row */
} tea_punct_break_t;

static inline bool tea_cp_is_sentence_end(uint32_t c)
{
	switch (c) {
	case '.':
	case '!':
	case '?':
	case ';':
	case 0x2026: /* … */
	case 0x3002: /* 。 */
	case 0xFF0E: /* ． */
	case 0xFF01: /* ！ */
	case 0xFF1F: /* ？ */
	case 0xFF1B: /* ； */
	case 0xFF61: /* ｡ */
		return true;
	default:
		return false;
	}
}

static inline bool tea_cp_is_comma(uint32_t c)
{
	return c == ',' || c == 0xFF0C /* ， */ || c == 0x3001 /* 、 */ || c == 0xFE50 /* ﹐ */ ||
	       c == 0xFE51 /* ﹑ */ || c == 0xFF64 /* ､ */;
}

static inline bool tea_cp_is_ascii_alnum(uint32_t c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* ASCII word (letters/dots) that ends at byte `end`, lower-cased, into buf. */
static inline size_t tea_ascii_word_before(const char *text, size_t start, size_t end, char *buf, size_t cap)
{
	size_t b = end;
	while (b > start) {
		char c = text[b - 1];
		bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.';
		if (!letter)
			break;
		b--;
	}
	size_t n = end - b;
	if (n >= cap)
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = text[b + i];
		buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	buf[n] = '\0';
	return n;
}

/* "Mr." "e.g." "U.S." "J." ...: a period that does not end a sentence. */
static inline bool tea_is_abbreviation(const char *word, size_t len)
{
	static const char *const known[] = {"mr", "mrs", "ms",  "dr", "prof", "st",  "jr",  "sr",  "vs",     "etc",
					    "no", "inc", "ltd", "co", "mt",   "e.g", "i.e", "fig", "approx", "dept"};
	if (len == 1)
		return true; /* an initial */
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		if (strcmp(word, known[i]) == 0)
			return true;
	}
	/* dotted acronyms: "u.s", "a.m" -- a dot inside the word */
	return memchr(word, '.', len) != NULL;
}

/*
 * The punctuation mark `cp` occupies text[here, after) on a row that starts
 * at row_start. Returns the byte offset where the row ends because of it
 * (after the mark and any punctuation / closing brackets right behind it),
 * or 0 for no break. No break is decided while nothing follows the mark yet.
 */
static inline size_t tea_punct_break_end(const char *text, size_t to, size_t row_start, size_t here, size_t after,
					 uint32_t cp, const tea_punct_break_t *punct)
{
	if (!punct || punct->mode == TEA_PUNCT_BREAK_OFF)
		return 0;
	bool sentence = tea_cp_is_sentence_end(cp);
	bool comma = punct->mode >= TEA_PUNCT_BREAK_COMMA && tea_cp_is_comma(cp);
	if (!sentence && !comma)
		return 0;

	/* the whole run of marks: "……" "。」" "?!" */
	size_t j = after;
	while (j < to) {
		size_t peek = j;
		uint32_t c = tea_utf8_decode(text, to, &peek);
		if (tea_cp_is_sentence_end(c)) {
			sentence = true;
		} else if (!(tea_cp_is_comma(c) || tea_cp_is_closing(c) || tea_cp_is_extend(c))) {
			break;
		}
		j = peek;
	}
	if (j >= to)
		return 0; /* nothing after it yet: decided when the next character arrives */
	size_t peek = j;
	const uint32_t next = tea_utf8_decode(text, to, &peek);

	if ((cp == '.' || cp == ',') && j == after) {
		/* ASCII: "3.5", "1,000", "e.g.x", "U.S.A" never break */
		if (tea_cp_is_ascii_alnum(next))
			return 0;
		if (cp == '.') {
			char word[16];
			size_t n = tea_ascii_word_before(text, row_start, here, word, sizeof(word));
			if (n > 0 && tea_is_abbreviation(word, n))
				return 0;
		}
	}
	if (!sentence) {
		/* comma: only when the row already holds enough to be worth it */
		size_t chars = 0, pos = row_start;
		while (pos < here) {
			uint32_t c = tea_utf8_decode(text, here, &pos);
			if (!tea_cp_is_space(c) && !tea_cp_is_extend(c))
				chars++;
		}
		if (chars < (size_t)(punct->comma_min_chars > 0 ? punct->comma_min_chars : 0))
			return 0;
	}
	return j;
}

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
				     const tea_punct_break_t *punct, const tea_glyph_cache_t *cache,
				     tea_wrap_row_t *rows, int cap, int total)
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
		bool punct_break = false;
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
					punct_break = punct && punct->mode != TEA_PUNCT_BREAK_OFF &&
						      (tea_cp_is_sentence_end(cp) ||
						       (punct->mode >= TEA_PUNCT_BREAK_COMMA && tea_cp_is_comma(cp)));
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
			const size_t stop = tea_punct_break_end(text, to, row_start, here, i, cp, punct);
			if (stop > 0) {
				cut = stop;
				resume = stop;
				punct_break = true;
				break;
			}
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
		row->punct_break = punct_break;
		total++;
		pos = resume;
	}
	return total;
}

/*
 * Wraps the visible part of one line: [visible_from, len). Returns the row
 * count (the last `cap` rows are in `rows`, oldest first after
 * tea_wrap_rows_in_order()) or TEA_WRAP_MISSING_GLYPH.
 */
static inline int tea_wrap_line_ex(const tea_display_line_t *line, const char *text, uint32_t max_width,
				   uint32_t outline_extra, bool word_wrap, const tea_punct_break_t *punct,
				   const tea_glyph_cache_t *cache, tea_wrap_row_t *rows, int cap)
{
	size_t starts[TEA_DISPLAY_MAX_CHUNKS + 1];
	int n = 0;
	for (int i = 0; i < line->chunk_count; i++)
		starts[n++] = line->chunks[i].start;
	if (line->locked_len > 0 && line->locked_len < line->len)
		starts[n++] = line->locked_len; /* the tail never pushes committed text around */

	if (line->visible_from >= line->len)
		return 0;
	return tea_wrap_paragraph(text, line->visible_from, line->len, starts, n, max_width, outline_extra, word_wrap,
				  punct, cache, rows, cap, 0);
}

/* tea_wrap_line_ex() without punctuation breaks. */
static inline int tea_wrap_line(const tea_display_line_t *line, const char *text, uint32_t max_width,
				uint32_t outline_extra, bool word_wrap, const tea_glyph_cache_t *cache,
				tea_wrap_row_t *rows, int cap)
{
	return tea_wrap_line_ex(line, text, max_width, outline_extra, word_wrap, NULL, cache, rows, cap);
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
 * redrawing only. `stable`, `end_silence_ms` and the recognition hints are
 * here because they are session.start fields: the server cannot switch them
 * inside a running session.
 */
typedef struct {
	const char *host;
	int port;
	const char *token_path;
	bool stable;
	int end_silence_ms; /* sentence break silence; 0 = server default */
	bool trace;         /* record the session's events to a file (per session) */
	/* recognition hints (session.start.context): every hint setting joined
	 * into one string (tea_hints_signature() in captions-source.c) */
	const char *hints;
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
	       tea_str_equal_or_both_empty(a->token_path, b->token_path) && a->stable == b->stable &&
	       a->end_silence_ms == b->end_silence_ms && a->trace == b->trace &&
	       tea_str_equal_or_both_empty(a->hints, b->hints);
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

/* ------------------------------------------------------------------------ */
/* Sentence break: the server's segmentation silence                         */
/* ------------------------------------------------------------------------ */

/* Bounds of session.start.segmentation.end_silence_ms (docs/04 「切段控制」).
 * The server advertises the real ones in
 * capabilities.features.segmentation_control.end_silence_ms. */
#define TEA_END_SILENCE_MIN_MS 300
#define TEA_END_SILENCE_MAX_MS 3000

/*
 * The value to put in session.start.segmentation.end_silence_ms, or 0 to
 * leave the field out. It is sent only when the server advertised
 * segmentation_control (an older server rejects unknown session.start
 * fields), only for a non-zero setting (0 = server default), and clamped to
 * the range the server advertised.
 */
static inline int tea_end_silence_request(int preferred_ms, bool server_supports, int server_min, int server_max)
{
	if (!server_supports || preferred_ms <= 0)
		return 0;
	if (server_min <= 0 || server_max < server_min) {
		server_min = TEA_END_SILENCE_MIN_MS;
		server_max = TEA_END_SILENCE_MAX_MS;
	}
	if (preferred_ms < server_min)
		return server_min;
	if (preferred_ms > server_max)
		return server_max;
	return preferred_ms;
}

/*
 * Settings migration: the old client-side "sentence break pause"
 * (line_break_pause_ms) measured gaps between text arrivals; the setting now
 * means the server's end-of-segment silence. 0 (off) becomes "server
 * default"; anything else is kept, clamped to the protocol range.
 */
static inline int tea_end_silence_from_legacy_pause(long long pause_ms)
{
	if (pause_ms <= 0)
		return 0;
	if (pause_ms < TEA_END_SILENCE_MIN_MS)
		return TEA_END_SILENCE_MIN_MS;
	if (pause_ms > TEA_END_SILENCE_MAX_MS)
		return TEA_END_SILENCE_MAX_MS;
	return (int)pause_ms;
}
