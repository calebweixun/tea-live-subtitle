#pragma once

/*
 * Punctuation-insensitive text alignment for the unstable tail and for
 * closing a line (caption-state.c). Plain C, static inline, no allocation.
 *
 * The recogniser restates the whole segment in every preview, and the same
 * words can come back with punctuation or spaces added or removed
 * ("對因為我我今天" vs "對，因為我我今天"). Comparing raw bytes then fails, the
 * restatement looks like new speech, and the sentence is shown twice. Every
 * comparison here works on a normalised form -- punctuation and whitespace
 * dropped, ASCII lower-cased, a few common CJK variant characters folded --
 * and maps positions back to the original bytes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TEA_NORM_MAX 1024

typedef struct {
	uint32_t cp[TEA_NORM_MAX];  /* normalised code points */
	uint32_t end[TEA_NORM_MAX]; /* byte offset just after each one in the original */
	int n;
} tea_norm_t;

static inline uint32_t tea_align_decode(const char *s, size_t len, size_t *pos)
{
	const unsigned char *p = (const unsigned char *)s + *pos;
	size_t left = len - *pos;
	uint32_t c = p[0];
	size_t n = 1;
	if (c >= 0xF0 && left >= 4) {
		c = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
		n = 4;
	} else if (c >= 0xE0 && left >= 3) {
		c = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
		n = 3;
	} else if (c >= 0xC0 && left >= 2) {
		c = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
		n = 2;
	}
	*pos += n;
	return c;
}

/* Punctuation, symbols and whitespace: ignored when comparing words. */
static inline bool tea_align_is_ignored(uint32_t c)
{
	if (c <= 0x20 || c == 0x7F)
		return true;
	if (c < 0x80)
		return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
	return (c >= 0x2000 && c <= 0x206F) || /* general punctuation: … — “ ” */
	       (c >= 0x3000 && c <= 0x303F) || /* CJK punctuation: 、。「」 */
	       (c >= 0xFE10 && c <= 0xFE1F) || (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF01 && c <= 0xFF0F) ||
	       (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65) ||
	       c == 0x00A0 || c == 0x00B7;
}

/* ASCII case and a few variant characters the recogniser alternates between. */
static inline uint32_t tea_align_fold(uint32_t c)
{
	if (c >= 'A' && c <= 'Z')
		return c + 32;
	if (c >= 0xFF21 && c <= 0xFF3A) /* fullwidth A-Z */
		return c - 0xFF21 + 'a';
	if (c >= 0xFF41 && c <= 0xFF5A) /* fullwidth a-z */
		return c - 0xFF41 + 'a';
	if (c >= 0xFF10 && c <= 0xFF19) /* fullwidth digits */
		return c - 0xFF10 + '0';
	switch (c) {
	case 0x7232:           /* 爲 */
		return 0x70BA; /* 為 */
	case 0x88CF:           /* 裏 */
		return 0x88E1; /* 裡 */
	case 0x9EBD:           /* 麽 */
		return 0x9EBC; /* 麼 */
	case 0x8846:           /* 衆 */
		return 0x773E; /* 眾 */
	case 0x7DAB:           /* 綫 */
		return 0x7DDA; /* 線 */
	case 0x7FA3:           /* 羣 */
		return 0x7FA4; /* 群 */
	case 0x5553:           /* 啓 */
		return 0x555F; /* 啟 */
	case 0x7740:           /* 着 */
		return 0x8457; /* 著 */
	default:
		return c;
	}
}

static inline void tea_norm_build(const char *text, size_t len, tea_norm_t *out)
{
	out->n = 0;
	size_t pos = 0;
	while (pos < len && out->n < TEA_NORM_MAX) {
		uint32_t c = tea_align_decode(text, len, &pos);
		if (tea_align_is_ignored(c))
			continue;
		out->cp[out->n] = tea_align_fold(c);
		out->end[out->n] = (uint32_t)pos;
		out->n++;
	}
}

static inline bool tea_norm_starts_with(const tea_norm_t *a, const tea_norm_t *prefix)
{
	if (prefix->n > a->n)
		return false;
	for (int i = 0; i < prefix->n; i++)
		if (a->cp[i] != prefix->cp[i])
			return false;
	return true;
}

/* Is `needle` a contiguous run of `hay` (normalised)? */
static inline bool tea_norm_contains(const tea_norm_t *hay, const tea_norm_t *needle)
{
	if (needle->n == 0)
		return true;
	for (int s = 0; s + needle->n <= hay->n; s++) {
		int i = 0;
		while (i < needle->n && hay->cp[s + i] == needle->cp[i])
			i++;
		if (i == needle->n)
			return true;
	}
	return false;
}

/* Longest common run of a and b; *a_end / *b_end: index just after it. */
static inline int tea_norm_longest_common(const tea_norm_t *a, const tea_norm_t *b, int *a_end, int *b_end)
{
	int prev[TEA_NORM_MAX + 1];
	int cur[TEA_NORM_MAX + 1];
	int best = 0;
	*a_end = 0;
	*b_end = 0;
	for (int j = 0; j <= b->n; j++)
		prev[j] = 0;
	for (int i = 1; i <= a->n; i++) {
		cur[0] = 0;
		for (int j = 1; j <= b->n; j++) {
			cur[j] = a->cp[i - 1] == b->cp[j - 1] ? prev[j - 1] + 1 : 0;
			if (cur[j] > best || (cur[j] == best && best > 0 && j > *b_end)) {
				best = cur[j];
				*a_end = i;
				*b_end = j;
			}
		}
		memcpy(prev, cur, sizeof(int) * (size_t)(b->n + 1));
	}
	return best;
}

/* Longest m >= min_len with a ending in b's first m characters (0 if none). */
static inline int tea_norm_suffix_prefix(const tea_norm_t *a, const tea_norm_t *b, int min_len)
{
	int most = a->n < b->n ? a->n : b->n;
	for (int m = most; m >= min_len && m > 0; m--) {
		if (memcmp(&a->cp[a->n - m], b->cp, sizeof(uint32_t) * (size_t)m) == 0)
			return m;
	}
	return 0;
}

/*
 * Longest common run of a and b that ends within the last `max_left`
 * characters of a (ties: the earliest in b). *a_end / *b_end: index just
 * after it.
 */
static inline int tea_norm_tail_run(const tea_norm_t *a, const tea_norm_t *b, int max_left, int *a_end, int *b_end)
{
	int prev[TEA_NORM_MAX + 1];
	int cur[TEA_NORM_MAX + 1];
	int best = 0;
	*a_end = 0;
	*b_end = 0;
	for (int j = 0; j <= b->n; j++)
		prev[j] = 0;
	for (int i = 1; i <= a->n; i++) {
		cur[0] = 0;
		for (int j = 1; j <= b->n; j++) {
			cur[j] = a->cp[i - 1] == b->cp[j - 1] ? prev[j - 1] + 1 : 0;
			if (i >= a->n - max_left && cur[j] > best) {
				best = cur[j];
				*a_end = i;
				*b_end = j;
			}
		}
		memcpy(prev, cur, sizeof(int) * (size_t)(b->n + 1));
	}
	return best;
}

/*
 * Semi-global edit distance: the cheapest way to align all of `a` with some
 * prefix b[0, j) of `b`. Returns the cost; *b_len gets that j (the longest j
 * among equally cheap ones).
 */
static inline int tea_norm_align_prefix(const tea_norm_t *a, const tea_norm_t *b, int *b_len)
{
	int prev[TEA_NORM_MAX + 1];
	int cur[TEA_NORM_MAX + 1];
	for (int j = 0; j <= b->n; j++)
		prev[j] = j; /* a[0, 0) vs b[0, j): j insertions */
	for (int i = 1; i <= a->n; i++) {
		cur[0] = i;
		for (int j = 1; j <= b->n; j++) {
			int sub = prev[j - 1] + (a->cp[i - 1] == b->cp[j - 1] ? 0 : 1);
			int del = prev[j] + 1;
			int ins = cur[j - 1] + 1;
			int best = sub < del ? sub : del;
			cur[j] = best < ins ? best : ins;
		}
		memcpy(prev, cur, sizeof(int) * (size_t)(b->n + 1));
	}
	int best_cost = prev[0];
	*b_len = 0;
	for (int j = 1; j <= b->n; j++) {
		if (prev[j] <= best_cost) {
			best_cost = prev[j];
			*b_len = j;
		}
	}
	return best_cost;
}

/*
 * The longest prefix a[0, *a_len) that still aligns with some prefix
 * b[0, *b_len) of `b` within one edit per `chars_per_edit` characters
 * (shorter prefixes must match exactly). Used to keep the words of a shown tail that a new
 * preview only changed further on. Returns the edit cost of that alignment.
 */
static inline int tea_norm_agreeing_prefix(const tea_norm_t *a, const tea_norm_t *b, int chars_per_edit, int *a_len,
					   int *b_len)
{
	int prev[TEA_NORM_MAX + 1];
	int cur[TEA_NORM_MAX + 1];
	for (int j = 0; j <= b->n; j++)
		prev[j] = j;
	*a_len = 0;
	*b_len = 0;
	int best_cost = 0;
	for (int i = 1; i <= a->n; i++) {
		cur[0] = i;
		int row_min = cur[0], row_j = 0;
		for (int j = 1; j <= b->n; j++) {
			int sub = prev[j - 1] + (a->cp[i - 1] == b->cp[j - 1] ? 0 : 1);
			int del = prev[j] + 1;
			int ins = cur[j - 1] + 1;
			int best = sub < del ? sub : del;
			cur[j] = best < ins ? best : ins;
			if (cur[j] <= row_min) {
				row_min = cur[j];
				row_j = j;
			}
		}
		const int allowed = i / chars_per_edit; /* short prefixes must match exactly */
		if (row_min <= allowed && row_j > 0) {
			*a_len = i;
			*b_len = row_j;
			best_cost = row_min;
		}
		memcpy(prev, cur, sizeof(int) * (size_t)(b->n + 1));
	}
	return best_cost;
}

/* Byte offset in the original text just after normalised index k-1 (0 for k=0). */
static inline size_t tea_norm_byte_after(const tea_norm_t *t, int k)
{
	return k > 0 ? t->end[k - 1] : 0;
}

#define TEA_ALIGN_PREFIX 1  /* the text starts with the committed text */
#define TEA_ALIGN_FUZZY 2   /* it restates the committed text with a few words changed */
#define TEA_ALIGN_OVERLAP 3 /* it starts with the end of the committed text */
#define TEA_ALIGN_NEW 4     /* it shares (almost) nothing with the committed text: new speech */
#define TEA_ALIGN_AFTER 5   /* final only: after its longest run shared with the committed text */
#define TEA_ALIGN_NONE 0    /* ambiguous: it shares words but cannot be placed */

/* Longest shared run up to which a text still counts as new speech. */
#define TEA_ALIGN_NEW_MAX_COMMON 4
/* Fuzzy restatement: at most one edit per this many committed characters. */
#define TEA_ALIGN_FUZZY_CHARS_PER_EDIT 4
#define TEA_ALIGN_OVERLAP_MIN 3
/* Exact committed-end / text-start overlap that counts (e.g. "…我們" / "我們…"). */
#define TEA_ALIGN_SUFFIX_MIN 2
/* A restated committed end may miss this many of its last characters. */
#define TEA_ALIGN_TAIL_RUN_LEFT 3
/* Same first characters: a restatement may differ in up to half the rest. */
#define TEA_ALIGN_RESTATE_HEAD 3

/*
 * Where, in `text` (a partial, or the final when `final_mode`), the part
 * after the committed text begins; *start gets the byte offset. The rules,
 * all on normalised text:
 *   PREFIX   the text starts with the committed text;
 *   FUZZY    it aligns with the whole committed text within one edit per
 *            four characters (a word changed, a variant character); two
 *            or three committed characters may differ in one, one must match;
 *            a restatement that starts with the same three characters may
 *            differ in up to half of them;
 *   OVERLAP  it starts with the end of the committed text (3+ characters):
 *            the preview window moved on;
 *   NEW      it shares no run longer than 4 characters with the committed
 *            text and its head does not fuzzily match the committed tail:
 *            the model previewed only the newest phrase -- all of it follows;
 *   AFTER    (final only) after the end of its longest run shared with the
 *            committed text, so nothing already committed repeats;
 *   NONE     otherwise: it restates part of the committed text in a way that
 *            cannot be placed; offer nothing rather than show it twice.
 */
/* Past punctuation / spaces at text[pos]: used when the committed text already
 * ends with a mark, so the same mark in the text is not shown again. */
static inline size_t tea_align_skip_ignored(const char *text, size_t len, size_t pos)
{
	while (pos < len) {
		size_t next = pos;
		uint32_t c = tea_align_decode(text, len, &next);
		if (!tea_align_is_ignored(c))
			break;
		pos = next;
	}
	return pos;
}

static inline int tea_align_after_committed_raw(const char *committed, size_t committed_len, const char *text,
						size_t text_len, bool final_mode, size_t *start);

static inline int tea_align_after_committed(const char *committed, size_t committed_len, const char *text,
					    size_t text_len, bool final_mode, size_t *start)
{
	int kind = tea_align_after_committed_raw(committed, committed_len, text, text_len, final_mode, start);
	if (kind != TEA_ALIGN_NEW && kind != TEA_ALIGN_NONE && *start > 0 && committed_len > 0) {
		size_t pos = 0;
		uint32_t last = 0;
		while (pos < committed_len) /* last code point of the committed text */
			last = tea_align_decode(committed, committed_len, &pos);
		if (tea_align_is_ignored(last))
			*start = tea_align_skip_ignored(text, text_len, *start);
	}
	return kind;
}

/* The first k characters of t match c[s, s + k): exactly, or but for one
 * from three characters on. */
static inline bool tea_align_head_at(const tea_norm_t *c, const tea_norm_t *t, int s, int k)
{
	int diff = 0;
	for (int i = 0; i < k; i++)
		diff += c->cp[s + i] != t->cp[i];
	return diff == 0 || (k >= 3 && diff <= 1);
}

static inline int tea_align_after_committed_raw(const char *committed, size_t committed_len, const char *text,
						size_t text_len, bool final_mode, size_t *start)
{
	tea_norm_t c, t; /* 8 KB each: fine on any thread's stack */
	tea_norm_build(committed, committed_len, &c);
	tea_norm_build(text, text_len, &t);
	*start = 0;
	if (c.n == 0)
		return TEA_ALIGN_PREFIX;
	if (tea_norm_starts_with(&t, &c)) {
		*start = tea_norm_byte_after(&t, c.n);
		return TEA_ALIGN_PREFIX;
	}
	int j = 0;
	int cost = tea_norm_align_prefix(&c, &t, &j);
	/* one edit per four characters; a two- or three-character commit may
	 * differ in one (我們 / 你們), a single character must match */
	const int max_edits = c.n >= TEA_ALIGN_FUZZY_CHARS_PER_EDIT ? c.n / TEA_ALIGN_FUZZY_CHARS_PER_EDIT
								    : (c.n >= 2 ? 1 : 0);
	if (j > 0 && cost <= max_edits) {
		*start = tea_norm_byte_after(&t, j);
		return TEA_ALIGN_FUZZY;
	}
	/* A restatement that begins exactly like the committed text but differs
	 * in more words (the commit was an early mishearing): still the same
	 * sentence, so what follows it comes after the aligned position, never
	 * the whole restatement again. */
	if (j > 0 && c.n >= TEA_ALIGN_RESTATE_HEAD && t.n >= TEA_ALIGN_RESTATE_HEAD &&
	    memcmp(c.cp, t.cp, sizeof(uint32_t) * TEA_ALIGN_RESTATE_HEAD) == 0 && cost * 2 <= c.n) {
		*start = tea_norm_byte_after(&t, j);
		return TEA_ALIGN_FUZZY;
	}
	/* The preview window moved on: the text starts with the committed end. */
	const int overlap = tea_norm_suffix_prefix(&c, &t, TEA_ALIGN_SUFFIX_MIN);
	if (overlap > 0) {
		*start = tea_norm_byte_after(&t, overlap);
		return TEA_ALIGN_OVERLAP;
	}
	int c_end = 0, t_end = 0;
	int common = tea_norm_longest_common(&c, &t, &c_end, &t_end);
	if (common >= TEA_ALIGN_OVERLAP_MIN && c_end >= c.n - 1 && t_end - common <= 1) {
		*start = tea_norm_byte_after(&t, t_end);
		return TEA_ALIGN_OVERLAP;
	}
	/*
	 * The text restates the end of the committed text, heard slightly
	 * differently: a run of it that ends at most three characters before the
	 * committed end, found no later in the text than it could be if what
	 * precedes it re-hears the committed words before that run. What follows
	 * starts after the run plus the committed characters left after it.
	 */
	{
		int rc_end = 0, rt_end = 0;
		const int run = tea_norm_tail_run(&c, &t, TEA_ALIGN_TAIL_RUN_LEFT, &rc_end, &rt_end);
		const int left = c.n - rc_end;
		if (run >= TEA_ALIGN_OVERLAP_MIN && left < run && rt_end - run <= rc_end - run + 2) {
			const int after = rt_end + left < t.n ? rt_end + left : t.n;
			*start = tea_norm_byte_after(&t, after);
			return TEA_ALIGN_OVERLAP;
		}
	}
	/* head of the text vs. the committed tail, one edit allowed from 3 characters */
	bool head_matches = false;
	int k = c.n < 4 ? c.n : 4;
	if (k >= 2 && t.n >= k) {
		/* the committed start (a restatement of the whole committed text
		 * that begins like it, then differs too much to be placed, is not
		 * new speech), then windows near the committed end */
		head_matches = tea_align_head_at(&c, &t, 0, k);
		for (int s = c.n - k - 4 < 1 ? 1 : c.n - k - 4; s + k <= c.n && !head_matches; s++)
			head_matches = tea_align_head_at(&c, &t, s, k);
	}
	/* A shared run at about the same place in both is a restatement heard
	 * differently around it, not new speech that happens to share words. */
	const int shift = (t_end - common) - (c_end - common);
	const bool same_place = common >= TEA_ALIGN_OVERLAP_MIN && shift >= -2 && shift <= 2;
	if (common <= TEA_ALIGN_NEW_MAX_COMMON && !head_matches && !same_place)
		return TEA_ALIGN_NEW;
	if (final_mode) {
		*start = tea_norm_byte_after(&t, t_end);
		return TEA_ALIGN_AFTER;
	}
	return TEA_ALIGN_NONE;
}

/*
 * Would `display` show a normalised run of `k` characters more often than
 * `ref` has it? The guard against showing a sentence twice: the server's own
 * text (the partial being shown, or the final) is the reference, so a
 * speaker who really repeats themselves is still shown as such.
 */
static inline bool tea_norm_has_new_repeat(const tea_norm_t *display, const tea_norm_t *ref, int k)
{
	for (int s = 0; s + k <= display->n; s++) {
		int count = 0;
		for (int u = 0; u + k <= display->n; u++) {
			if (memcmp(&display->cp[u], &display->cp[s], sizeof(uint32_t) * (size_t)k) == 0) {
				count++;
				u += k - 1; /* non-overlapping */
			}
		}
		if (count < 2)
			continue;
		int ref_count = 0;
		for (int u = 0; u + k <= ref->n; u++) {
			if (memcmp(&ref->cp[u], &display->cp[s], sizeof(uint32_t) * (size_t)k) == 0) {
				ref_count++;
				u += k - 1;
			}
		}
		if (ref_count < count)
			return true;
	}
	return false;
}
