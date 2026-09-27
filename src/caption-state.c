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

#include "caption-state.h"

#include <util/threading.h>
#include <util/bmem.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TEA_MAX_FINALIZED_LINES 10
/* docs/04's max_segment_ms=14000 means a continuous session produces a new
 * segment roughly every ~14s; the M2 verification report (docs/m2-
 * verification.md, defect 6) flagged that the old value of 16 only covered
 * ~3-4 minutes of a session before the round-robin table started
 * overwriting still-relevant (already-finalized) segment tracking entries.
 * 256 covers roughly an hour (docs/08's acceptance criterion #4) at a
 * trivial ~20KB memory cost. This is still a fixed-size table, not true
 * whole-session dedup -- it just pushes the failure window from minutes to
 * well beyond what v0.1's once-only, TCP-ordered wire protocol is expected
 * to need. */
#define TEA_MAX_TRACKED_SEGMENTS 256
#define TEA_ID_BUF 64
/* Order key = (session epoch << TEA_ORDER_EPOCH_SHIFT) | segment_index, so
 * lines held over from a previous session always sort above the new
 * session's lines even though segment_index restarts at 0. */
#define TEA_ORDER_EPOCH_SHIFT 40
#define TEA_ORDER_INDEX_MASK ((UINT64_C(1) << TEA_ORDER_EPOCH_SHIFT) - 1)

typedef struct {
	char segment_id[TEA_ID_BUF];
	uint64_t revision;
	bool terminal;
	bool used;
	/* stable mode */
	uint64_t stable_revision;
	bool stable_closed; /* closing transcript.stable seen: line never changes again */
	bool had_line;      /* a line was created for it (it may since have scrolled away) */
} tea_segment_track_t;

/* One on-screen line. In partial mode these are finalized lines only (the
 * preview lives in preview_text); in stable mode each line is one segment's
 * committed text, which may still grow while `open`. */
typedef struct {
	char *text; /* never NULL */
	char segment_id[TEA_ID_BUF];
	uint64_t order;
	uint64_t key; /* display identity, see tea_caption_snapshot_line_t */
	/* stable mode: the unstable tail shown after `text` (NULL if none),
	 * taken from this segment's partials and kept until a newer valid one
	 * replaces it (see tea_hold_tail_locked()). Never copied into `text`. */
	char *partial;
	uint64_t activity; /* bumped by every transcript event of this segment */
	bool open;
	/* The shown text (committed + tail) is what the line ends with: nothing
	 * may change it any more. Set when the segment closes. */
	bool settled;
	bool order_unknown; /* created before its segment_index was known */
} tea_caption_line_t;

struct tea_caption_state {
	pthread_mutex_t lock;

	int total_lines; /* includes the current/preview line */

	char current_session_id[TEA_ID_BUF];
	bool have_session;
	/* Set by hold_for_reconnect(): the next session keeps the frozen lines. */
	bool carry_over;
	uint64_t epoch;

	bool stable_mode;
	uint64_t stable_mismatches;

	tea_segment_track_t segments[TEA_MAX_TRACKED_SEGMENTS];
	size_t next_segment_slot; /* round-robin eviction */

	tea_caption_line_t lines[TEA_MAX_FINALIZED_LINES];
	int line_count;

	char *preview_text; /* NULL when no open preview (partial mode only) */
	char preview_segment_id[TEA_ID_BUF];
	uint64_t preview_key;

	uint64_t next_key;
	uint64_t revision;
	uint64_t activity; /* bumped by every transcript event of the session */
	bool stable_tail_lines;
};

tea_caption_state_t *tea_caption_state_create(void)
{
	tea_caption_state_t *st = bzalloc(sizeof(tea_caption_state_t));
	pthread_mutex_init(&st->lock, NULL);
	st->total_lines = 2;
	return st;
}

static uint64_t tea_new_key_locked(tea_caption_state_t *st)
{
	return ++st->next_key;
}

static void tea_drop_oldest_line_locked(tea_caption_state_t *st)
{
	if (st->line_count <= 0)
		return;
	bfree(st->lines[0].text);
	bfree(st->lines[0].partial);
	memmove(&st->lines[0], &st->lines[1], sizeof(tea_caption_line_t) * (size_t)(st->line_count - 1));
	st->line_count--;
	memset(&st->lines[st->line_count], 0, sizeof(tea_caption_line_t));
}

static void tea_clear_finalized_locked(tea_caption_state_t *st)
{
	for (int i = 0; i < st->line_count; i++) {
		bfree(st->lines[i].text);
		bfree(st->lines[i].partial);
		st->lines[i].text = NULL;
		st->lines[i].partial = NULL;
	}
	memset(st->lines, 0, sizeof(st->lines));
	st->line_count = 0;
}

static void tea_clear_partial(tea_caption_line_t *line)
{
	if (!line)
		return;
	bfree(line->partial);
	line->partial = NULL;
}

static char *tea_strdup_n(const char *text, size_t len)
{
	char *out = bmalloc(len + 1);
	memcpy(out, text, len);
	out[len] = '\0';
	return out;
}

/* The recogniser closes every preview hypothesis with sentence punctuation
 * ("遲遲。", then "遲遲未定的原。"). At the unstable edge that mark is
 * replaced by more words on every preview, which on screen looks like text
 * being taken back; the final (or a commit) brings the real one. Returns the
 * length without trailing sentence punctuation. */
static size_t tea_trim_hypothesis_end(const char *text, size_t len)
{
	static const char *const marks[] = {"\xe3\x80\x82", /* 。 */
					    "\xef\xbc\x8e", /* ． */
					    "\xef\xbc\x81", /* ！ */
					    "\xef\xbc\x9f", /* ？ */
					    "\xef\xbc\x8c", /* ， */
					    "\xe3\x80\x81", /* 、 */
					    "\xef\xbc\x9b", /* ； */
					    "\xef\xbc\x9a", /* ： */
					    "\xe2\x80\xa6", /* … */
					    ".",
					    "!",
					    "?",
					    ",",
					    ";",
					    ":"};
	bool trimmed = true;
	while (trimmed && len > 0) {
		trimmed = false;
		for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
			size_t n = strlen(marks[i]);
			if (len >= n && memcmp(text + len - n, marks[i], n) == 0) {
				len -= n;
				trimmed = true;
				break;
			}
		}
	}
	return len;
}

static size_t tea_utf8_count(const char *text, size_t len)
{
	size_t n = 0;
	for (size_t i = 0; i < len; i++)
		if (((unsigned char)text[i] & 0xC0) != 0x80)
			n++;
	return n;
}

/* Byte offset of the code point after the first `chars` code points. */
static size_t tea_utf8_skip(const char *text, size_t len, size_t chars)
{
	size_t i = 0;
	while (i < len && chars > 0) {
		i++;
		while (i < len && ((unsigned char)text[i] & 0xC0) == 0x80)
			i++;
		chars--;
	}
	return chars == 0 ? i : len;
}

static size_t tea_utf8_next(const char *text, size_t len, size_t i)
{
	i++;
	while (i < len && ((unsigned char)text[i] & 0xC0) == 0x80)
		i++;
	return i;
}

/* Byte length of leading sentence punctuation (see tea_trim_hypothesis_end()). */
static size_t tea_leading_marks(const char *text, size_t len)
{
	size_t i = 0;
	while (i < len) {
		size_t next = tea_utf8_next(text, len, i);
		if (tea_trim_hypothesis_end(text + i, next - i) != 0)
			break;
		i = next;
	}
	return i;
}

/*
 * Where the text after the committed text starts inside a partial:
 *   1. the partial starts with the committed text: right after it;
 *   2. the partial starts with the end of the committed text (at least two
 *      characters): after that overlap -- the preview window moved on;
 *   3. the partial restates the committed text with some characters changed
 *      (at least half of the first characters agree): after as many
 *      characters as the committed text has -- how the server itself closes
 *      a diverged segment;
 *   4. otherwise the partial is new speech (the real model often previews
 *      only the latest phrase of a long segment): all of it.
 */
static size_t tea_tail_start(const char *committed, size_t committed_len, const char *partial, size_t partial_len)
{
	if (partial_len >= committed_len && memcmp(partial, committed, committed_len) == 0)
		return committed_len;
	for (size_t s = committed_len > 0 ? tea_utf8_next(committed, committed_len, 0) : 0; s < committed_len;
	     s = tea_utf8_next(committed, committed_len, s)) {
		size_t overlap = committed_len - s;
		if (tea_utf8_count(committed + s, overlap) < 2)
			break;
		if (partial_len >= overlap && memcmp(partial, committed + s, overlap) == 0)
			return overlap;
	}
	size_t chars = tea_utf8_count(committed, committed_len);
	size_t same = 0, ci = 0, pi = 0;
	for (size_t n = 0; n < chars && ci < committed_len && pi < partial_len; n++) {
		size_t cn = tea_utf8_next(committed, committed_len, ci);
		size_t pn = tea_utf8_next(partial, partial_len, pi);
		if (cn - ci == pn - pi && memcmp(committed + ci, partial + pi, cn - ci) == 0)
			same++;
		ci = cn;
		pi = pn;
	}
	if (chars > 0 && same * 2 >= chars)
		return tea_utf8_skip(partial, partial_len, chars);
	return 0;
}

/*
 * Must hold state->lock. A new transcript.partial for an open stable line
 * offers a new unstable tail: the text it has after the committed text (see
 * tea_tail_start()). Shown text must never go backwards, so:
 *   - trailing sentence punctuation of the hypothesis is not shown (see
 *     tea_trim_hypothesis_end());
 *   - an offer that, ignoring leading punctuation, continues the tail on
 *     screen only appends to it;
 *   - an offer shorter (in characters) than the tail on screen is ignored:
 *     it would retract visible text;
 *   - any other offer replaces it -- unconfirmed words may be corrected,
 *     never removed.
 * The committed `text` is never touched.
 */
static void tea_hold_tail_locked(tea_caption_line_t *line, const char *partial)
{
	if (!partial)
		return;
	size_t shown = strlen(line->text);
	size_t incoming = strlen(partial);
	size_t from = tea_tail_start(line->text, shown, partial, incoming);
	if (from >= incoming)
		return;
	const char *candidate = partial + from;
	size_t candidate_len = tea_trim_hypothesis_end(candidate, incoming - from);
	if (candidate_len == 0)
		return;
	if (line->partial) {
		size_t held = strlen(line->partial);
		size_t lead = tea_leading_marks(candidate, candidate_len);
		if (candidate_len - lead >= held && memcmp(candidate + lead, line->partial, held) == 0) {
			candidate += lead; /* continues the shown tail: keep it, append */
			candidate_len -= lead;
		} else if (tea_utf8_count(candidate, candidate_len) < tea_utf8_count(line->partial, held)) {
			return;
		}
		if (candidate_len == held && memcmp(line->partial, candidate, held) == 0)
			return;
	}
	bfree(line->partial);
	line->partial = tea_strdup_n(candidate, candidate_len);
}

/*
 * Must hold state->lock. The committed text of `line` grew from
 * `old_committed_len` bytes (the tail was shown after it). What was on screen
 * was old committed + tail; whatever part of the tail the new committed text
 * confirmed is now committed, and the rest stays as the tail. If the new
 * committed text disagrees with the tail, the tail is dropped (committed
 * text wins; it never changes).
 */
static void tea_rebase_tail_locked(tea_caption_line_t *line, size_t old_committed_len)
{
	if (!line->partial)
		return;
	size_t committed = strlen(line->text);
	size_t tail = strlen(line->partial);
	size_t grown = committed - old_committed_len; /* bytes of the tail that are now committed */
	if (grown == 0)
		return;
	if (grown < tail && memcmp(line->partial, line->text + old_committed_len, grown) == 0) {
		char *rest = tea_strdup_n(line->partial + grown, tail - grown);
		bfree(line->partial);
		line->partial = rest;
		return;
	}
	tea_clear_partial(line);
}

/* Must hold state->lock. Does `text` start with everything the line shows
 * (committed text, plus the tail when tails are shown)? */
static bool tea_extends_shown_locked(const tea_caption_state_t *st, const tea_caption_line_t *line, const char *text)
{
	if (!text)
		return false;
	size_t committed = strlen(line->text);
	size_t tail = (st->stable_tail_lines && line->partial) ? strlen(line->partial) : 0;
	size_t incoming = strlen(text);
	if (incoming < committed + tail || memcmp(text, line->text, committed) != 0)
		return false;
	return tail == 0 || memcmp(text + committed, line->partial, tail) == 0;
}

/*
 * Must hold state->lock. The segment is over: whatever the line shows now is
 * what it ends with. Shown text never shrinks or changes once a viewer may
 * have read it, so a tail on screen stays (in its fainter style: the server
 * never confirmed it) instead of being taken back. Real-model traces show why
 * this matters: when one segment holds two phrases the final often drops a
 * whole phrase, and the tail is then the only place those words exist.
 */
static void tea_settle_line_locked(tea_caption_state_t *st, tea_caption_line_t *line)
{
	line->open = false;
	line->settled = true;
	if (!st->stable_tail_lines)
		tea_clear_partial(line); /* not shown: nothing to keep */
}

/* Must hold state->lock. Any transcript event for a segment is activity:
 * the line's speaker is still talking even when the text did not change. */
static void tea_touch_locked(tea_caption_state_t *st, tea_caption_line_t *line)
{
	st->activity++;
	if (line)
		line->activity++;
	st->revision++;
}

static void tea_clear_preview_locked(tea_caption_state_t *st)
{
	bfree(st->preview_text);
	st->preview_text = NULL;
	st->preview_segment_id[0] = '\0';
	st->preview_key = 0;
}

void tea_caption_state_destroy(tea_caption_state_t *state)
{
	if (!state)
		return;
	tea_clear_finalized_locked(state);
	bfree(state->preview_text);
	pthread_mutex_destroy(&state->lock);
	bfree(state);
}

/* Lines kept for display. Partial mode: finalized lines = max(total - 1, 1)
 * plus the preview line. Stable mode: every line is a segment line. */
static int tea_visible_lines_locked(const tea_caption_state_t *st)
{
	if (st->stable_mode)
		return st->total_lines;
	return st->total_lines > 1 ? st->total_lines - 1 : 1;
}

void tea_caption_state_set_max_lines(tea_caption_state_t *state, int total_lines)
{
	if (total_lines < 1)
		total_lines = 1;
	if (total_lines > TEA_MAX_FINALIZED_LINES)
		total_lines = TEA_MAX_FINALIZED_LINES;

	pthread_mutex_lock(&state->lock);
	state->revision++;
	state->total_lines = total_lines;
	if (!state->stable_mode) {
		int keep = tea_visible_lines_locked(state);
		while (state->line_count > keep)
			tea_drop_oldest_line_locked(state); /* drop the oldest to make room */
	}
	pthread_mutex_unlock(&state->lock);
}

static void tea_reset_locked(tea_caption_state_t *st)
{
	tea_clear_finalized_locked(st);
	tea_clear_preview_locked(st);
	memset(st->segments, 0, sizeof(st->segments));
	st->next_segment_slot = 0;
	st->carry_over = false;
	st->epoch++;
	st->revision++;
}

void tea_caption_state_reset(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);
	tea_reset_locked(state);
	state->have_session = false;
	state->current_session_id[0] = '\0';
	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_hold_for_reconnect(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);
	tea_clear_preview_locked(state);
	for (int i = 0; i < state->line_count; i++)
		tea_settle_line_locked(state, &state->lines[i]); /* frozen as shown */
	memset(state->segments, 0, sizeof(state->segments));
	state->next_segment_slot = 0;
	state->have_session = false;
	state->current_session_id[0] = '\0';
	state->carry_over = true;
	state->epoch++;
	state->revision++;
	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_set_stable_mode(tea_caption_state_t *state, bool enabled)
{
	pthread_mutex_lock(&state->lock);
	if (state->stable_mode != enabled) {
		tea_clear_preview_locked(state);
		state->stable_mode = enabled;
		state->revision++;
	}
	pthread_mutex_unlock(&state->lock);
}

bool tea_caption_state_stable_mode(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);
	bool result = state->stable_mode;
	pthread_mutex_unlock(&state->lock);
	return result;
}

uint64_t tea_caption_state_stable_mismatches(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);
	uint64_t result = state->stable_mismatches;
	pthread_mutex_unlock(&state->lock);
	return result;
}

/* Must hold state->lock. Ensures we're tracking `session_id` as current,
 * resetting everything if it differs from what we had (new/reconnected
 * session -- v0.1 never resumes, so a session_id change always means a
 * fresh caption context). After hold_for_reconnect() the frozen lines are
 * kept instead and the new session's lines scroll in below them. Returns
 * false if session_id is NULL/empty. */
static bool tea_ensure_session_locked(tea_caption_state_t *st, const char *session_id)
{
	if (!session_id || !session_id[0])
		return false;
	if (!st->have_session && st->carry_over) {
		st->carry_over = false;
		st->have_session = true;
		snprintf(st->current_session_id, TEA_ID_BUF, "%s", session_id);
		return true;
	}
	if (!st->have_session || strncmp(st->current_session_id, session_id, TEA_ID_BUF) != 0) {
		tea_reset_locked(st);
		st->have_session = true;
		snprintf(st->current_session_id, TEA_ID_BUF, "%s", session_id);
	}
	return true;
}

/* Must hold state->lock. Finds or allocates (round-robin evicting the
 * least-recently-added entry when full) the tracking slot for segment_id. */
static tea_segment_track_t *tea_track_segment_locked(tea_caption_state_t *st, const char *segment_id)
{
	for (size_t i = 0; i < TEA_MAX_TRACKED_SEGMENTS; i++) {
		if (st->segments[i].used && strncmp(st->segments[i].segment_id, segment_id, TEA_ID_BUF) == 0)
			return &st->segments[i];
	}
	tea_segment_track_t *slot = &st->segments[st->next_segment_slot];
	st->next_segment_slot = (st->next_segment_slot + 1) % TEA_MAX_TRACKED_SEGMENTS;
	memset(slot, 0, sizeof(*slot));
	snprintf(slot->segment_id, TEA_ID_BUF, "%s", segment_id);
	slot->used = true;
	return slot;
}

static void tea_push_finalized_locked(tea_caption_state_t *st, const char *text, uint64_t key)
{
	int keep = tea_visible_lines_locked(st);
	while (st->line_count >= keep || st->line_count >= TEA_MAX_FINALIZED_LINES)
		tea_drop_oldest_line_locked(st);
	tea_caption_line_t *line = &st->lines[st->line_count++];
	memset(line, 0, sizeof(*line));
	line->text = bstrdup(text ? text : "");
	line->order = st->line_count > 1 ? st->lines[st->line_count - 2].order : 0;
	line->key = key ? key : tea_new_key_locked(st);
}

/* ---------------- stable mode ---------------- */

static tea_caption_line_t *tea_find_line_locked(tea_caption_state_t *st, const char *segment_id)
{
	for (int i = 0; i < st->line_count; i++) {
		if (strncmp(st->lines[i].segment_id, segment_id, TEA_ID_BUF) == 0)
			return &st->lines[i];
	}
	return NULL;
}

/* Must hold state->lock. Returns the segment's line, creating it in
 * transcript order if this is the first time it has anything to show.
 * Returns NULL when the segment's line has already scrolled out of the
 * buffer (or would be older than everything still kept): it must never
 * reappear. */
static tea_caption_line_t *tea_stable_line_locked(tea_caption_state_t *st, tea_segment_track_t *seg,
						  uint64_t segment_index)
{
	tea_caption_line_t *line = tea_find_line_locked(st, seg->segment_id);
	if (line || seg->had_line)
		return line;

	uint64_t order;
	if (segment_index == TEA_CAPTION_SEGMENT_INDEX_UNKNOWN)
		order = st->line_count > 0 ? st->lines[st->line_count - 1].order : (st->epoch << TEA_ORDER_EPOCH_SHIFT);
	else
		order = (st->epoch << TEA_ORDER_EPOCH_SHIFT) | (segment_index & TEA_ORDER_INDEX_MASK);

	int pos = st->line_count;
	while (pos > 0 && st->lines[pos - 1].order > order)
		pos--;
	if (st->line_count >= TEA_MAX_FINALIZED_LINES) {
		if (pos == 0) {
			seg->had_line = true; /* older than everything kept: never shown */
			return NULL;
		}
		tea_drop_oldest_line_locked(st);
		pos--;
	}
	memmove(&st->lines[pos + 1], &st->lines[pos], sizeof(tea_caption_line_t) * (size_t)(st->line_count - pos));
	st->line_count++;
	line = &st->lines[pos];
	memset(line, 0, sizeof(*line));
	line->text = bstrdup("");
	snprintf(line->segment_id, TEA_ID_BUF, "%s", seg->segment_id);
	line->order = order;
	line->order_unknown = segment_index == TEA_CAPTION_SEGMENT_INDEX_UNKNOWN;
	line->key = tea_new_key_locked(st);
	line->open = true;
	seg->had_line = true;
	return line;
}

/* Must hold state->lock. A line created from a partial (no segment_index on
 * that event) is re-sorted once the segment_index is known. Such a line has
 * no committed text yet, so moving it never moves shown committed text. */
static tea_caption_line_t *tea_stable_fix_order_locked(tea_caption_state_t *st, tea_caption_line_t *line,
						       uint64_t segment_index)
{
	if (!line || !line->order_unknown || segment_index == TEA_CAPTION_SEGMENT_INDEX_UNKNOWN)
		return line;
	tea_caption_line_t moved = *line;
	moved.order = (st->epoch << TEA_ORDER_EPOCH_SHIFT) | (segment_index & TEA_ORDER_INDEX_MASK);
	moved.order_unknown = false;
	int idx = (int)(line - st->lines);
	memmove(&st->lines[idx], &st->lines[idx + 1], sizeof(tea_caption_line_t) * (size_t)(st->line_count - idx - 1));
	st->line_count--;
	int pos = st->line_count;
	while (pos > 0 && st->lines[pos - 1].order > moved.order)
		pos--;
	memmove(&st->lines[pos + 1], &st->lines[pos], sizeof(tea_caption_line_t) * (size_t)(st->line_count - pos));
	st->lines[pos] = moved;
	st->line_count++;
	return &st->lines[pos];
}

/* Must hold state->lock. Only ever appends: `text` must start with the bytes
 * already displayed (the server guarantees UTF-8 byte-prefix extension, cut
 * at a grapheme boundary). Anything else leaves the line untouched. Returns
 * false for a non-extending value. */
static bool tea_stable_extend_locked(tea_caption_line_t *line, const char *text)
{
	if (!text)
		return false;
	size_t shown = strlen(line->text);
	size_t incoming = strlen(text);
	if (incoming < shown || memcmp(text, line->text, shown) != 0)
		return false;
	if (incoming == shown)
		return true;
	char *grown = bmalloc(incoming + 1);
	memcpy(grown, line->text, shown);
	memcpy(grown + shown, text + shown, incoming - shown + 1);
	bfree(line->text);
	line->text = grown;
	return true;
}

static bool tea_stable_state_is_closing(const char *stable_state)
{
	return stable_state && (strcmp(stable_state, "final") == 0 || strcmp(stable_state, "diverged") == 0 ||
				strcmp(stable_state, "abandoned") == 0);
}

void tea_caption_state_on_stable(tea_caption_state_t *state, const char *session_id, const char *segment_id,
				 uint64_t segment_index, uint64_t stable_revision, const char *text,
				 const char *stable_state)
{
	if (!segment_id || !segment_id[0])
		return;
	pthread_mutex_lock(&state->lock);
	if (!state->stable_mode || !tea_ensure_session_locked(state, session_id)) {
		pthread_mutex_unlock(&state->lock);
		return;
	}

	tea_segment_track_t *seg = tea_track_segment_locked(state, segment_id);
	if (seg->stable_closed || (seg->stable_revision != 0 && stable_revision <= seg->stable_revision)) {
		/* Closed, duplicate or out of order: never an error, never a change
		 * of text -- but still a sign the session is active. */
		tea_touch_locked(state, seg->stable_closed ? NULL : tea_find_line_locked(state, segment_id));
		pthread_mutex_unlock(&state->lock);
		return;
	}
	seg->stable_revision = stable_revision;

	const bool closing = tea_stable_state_is_closing(stable_state);
	tea_caption_line_t *line = tea_stable_line_locked(state, seg, segment_index);
	line = tea_stable_fix_order_locked(state, line, segment_index);
	if (line && line->settled) {
		/* already ended (final / skipped / cancelled): nothing changes */
	} else if (line && closing && !tea_extends_shown_locked(state, line, text)) {
		/* A closing value ("diverged": committed + the final's text after
		 * that length) that does not extend what is shown would rewrite
		 * text a viewer has read: the line ends as shown. */
	} else if (line) {
		size_t before = strlen(line->text);
		if (!tea_stable_extend_locked(line, text))
			state->stable_mismatches++;
		else
			tea_rebase_tail_locked(line, before);
	}
	tea_touch_locked(state, line);

	if (closing) {
		seg->stable_closed = true;
		seg->terminal = true;
		if (line && !line->settled) {
			if (tea_extends_shown_locked(state, line, text))
				tea_clear_partial(line); /* the committed text now covers the tail */
			tea_settle_line_locked(state, line);
		}
	}
	state->revision++;
	pthread_mutex_unlock(&state->lock);
}

/* ---------------- partial / final ---------------- */

void tea_caption_state_on_partial(tea_caption_state_t *state, const char *session_id, const char *segment_id,
				  uint64_t revision, const char *text)
{
	if (!segment_id)
		return;
	pthread_mutex_lock(&state->lock);
	if (!tea_ensure_session_locked(state, session_id)) {
		pthread_mutex_unlock(&state->lock);
		return;
	}

	tea_segment_track_t *seg = tea_track_segment_locked(state, segment_id);
	if (seg->terminal) {
		/* "terminal 之後拒絕 partial" */
		pthread_mutex_unlock(&state->lock);
		return;
	}
	if (revision <= seg->revision && seg->revision != 0) {
		/* Only apply strictly higher revisions; out-of-order/duplicate
		 * partials are dropped, not an error. */
		tea_touch_locked(state, NULL);
		pthread_mutex_unlock(&state->lock);
		return;
	}
	seg->revision = revision;

	if (state->stable_mode) {
		/* A partial may still rewrite any of its characters, so it never
		 * becomes committed text. It is only remembered as the source of
		 * the optional unstable tail (see tea_caption_state_snapshot()). */
		tea_caption_line_t *line = seg->stable_closed ? NULL : tea_find_line_locked(state, segment_id);
		if (!line && state->stable_tail_lines && !seg->stable_closed && !seg->had_line)
			line = tea_stable_line_locked(state, seg, TEA_CAPTION_SEGMENT_INDEX_UNKNOWN);
		if (line && line->open)
			tea_hold_tail_locked(line, text);
		tea_touch_locked(state, line);
		pthread_mutex_unlock(&state->lock);
		return;
	}

	if (!state->preview_text || strncmp(state->preview_segment_id, segment_id, TEA_ID_BUF) != 0)
		state->preview_key = tea_new_key_locked(state);
	bfree(state->preview_text);
	state->preview_text = bstrdup(text ? text : "");
	snprintf(state->preview_segment_id, TEA_ID_BUF, "%s", segment_id);
	tea_touch_locked(state, NULL);

	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_on_final_indexed(tea_caption_state_t *state, const char *session_id, const char *segment_id,
					uint64_t segment_index, uint64_t revision, const char *text)
{
	if (!segment_id)
		return;
	pthread_mutex_lock(&state->lock);
	if (!tea_ensure_session_locked(state, session_id)) {
		pthread_mutex_unlock(&state->lock);
		return;
	}

	tea_segment_track_t *seg = tea_track_segment_locked(state, segment_id);
	if (seg->terminal) {
		/* Terminal state already reached for this segment_id; final is
		 * only ever sent once per docs/04, ignore a duplicate/replay. */
		pthread_mutex_unlock(&state->lock);
		return;
	}
	seg->terminal = true;
	seg->revision = revision;

	if (state->stable_mode) {
		/* A final that extends what is shown (committed text + tail) is shown
		 * at once and ends the line; the closing "final" stable that follows
		 * is then a no-op. A final that does not -- shorter, or different
		 * (the real model's final often drops a whole phrase of a two-phrase
		 * segment) -- is not allowed to take back or rewrite text a viewer
		 * has read: the line closes (for fading) but keeps what it shows.
		 * With tails hidden, the closing "diverged" stable that follows may
		 * still append the final's text after the committed length, as it
		 * extends what is shown. Transcripts/exports use transcript.final;
		 * this is display only. */
		tea_caption_line_t *line = seg->stable_closed ? NULL
							      : tea_stable_line_locked(state, seg, segment_index);
		line = tea_stable_fix_order_locked(state, line, segment_index);
		if (line && !line->settled) {
			if (tea_extends_shown_locked(state, line, text) && tea_stable_extend_locked(line, text)) {
				tea_clear_partial(line);
				tea_settle_line_locked(state, line);
			} else {
				line->open = false; /* closed for fading; the closing stable settles it */
			}
		}
		tea_touch_locked(state, line ? line : tea_find_line_locked(state, segment_id));
		pthread_mutex_unlock(&state->lock);
		return;
	}

	uint64_t key = 0;
	if (state->preview_text && strncmp(state->preview_segment_id, segment_id, TEA_ID_BUF) == 0) {
		key = state->preview_key; /* the preview line turns into this final line */
		tea_clear_preview_locked(state);
	}

	tea_push_finalized_locked(state, text, key);
	tea_touch_locked(state, NULL);

	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_on_final(tea_caption_state_t *state, const char *session_id, const char *segment_id,
				uint64_t revision, const char *text)
{
	tea_caption_state_on_final_indexed(state, session_id, segment_id, TEA_CAPTION_SEGMENT_INDEX_UNKNOWN, revision,
					   text);
}

void tea_caption_state_on_segment_dropped(tea_caption_state_t *state, const char *session_id, const char *segment_id)
{
	if (!segment_id)
		return;
	pthread_mutex_lock(&state->lock);
	if (!tea_ensure_session_locked(state, session_id)) {
		pthread_mutex_unlock(&state->lock);
		return;
	}

	tea_segment_track_t *seg = tea_track_segment_locked(state, segment_id);
	seg->terminal = true;

	if (state->stable_mode) {
		/* Shown text stays (committed text and any tail); the "abandoned"
		 * stable that follows (if any) changes nothing. */
		tea_caption_line_t *line = tea_find_line_locked(state, segment_id);
		if (line && !line->settled)
			tea_settle_line_locked(state, line);
	}

	if (state->preview_text && strncmp(state->preview_segment_id, segment_id, TEA_ID_BUF) == 0)
		tea_clear_preview_locked(state);
	state->revision++;
	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_on_session_cancelled(tea_caption_state_t *state, const char *session_id)
{
	pthread_mutex_lock(&state->lock);
	if (state->have_session && session_id && session_id[0] &&
	    strncmp(state->current_session_id, session_id, TEA_ID_BUF) == 0) {
		tea_clear_preview_locked(state);
		/* Nothing more is sent after a cancel: freeze what is shown. */
		for (int i = 0; i < state->line_count; i++)
			tea_settle_line_locked(state, &state->lines[i]);
		state->revision++;
	}
	pthread_mutex_unlock(&state->lock);
}

/* ---------------- render ---------------- */

/* Must hold state->lock. Index of the first line to render. Stable mode
 * skips empty lines (a segment whose first event carried no text yet). */
static int tea_first_visible_line_locked(const tea_caption_state_t *st)
{
	int want = tea_visible_lines_locked(st);
	int first = st->line_count;
	int shown = 0;
	while (first > 0 && shown < want) {
		first--;
		if (!st->stable_mode || st->lines[first].text[0] != '\0')
			shown++;
	}
	return first;
}

char *tea_caption_state_render(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);

	int first = tea_first_visible_line_locked(state);
	size_t total_len = 1;
	for (int i = first; i < state->line_count; i++)
		total_len += strlen(state->lines[i].text) + 1;
	const char *preview = state->stable_mode ? NULL : state->preview_text;
	if (preview)
		total_len += strlen(preview) + 1;

	if (total_len == 1 || (state->line_count == 0 && !preview)) {
		/* The caller may choose a neutral placeholder for this empty state,
		 * but connection diagnostics must never be rendered as captions. */
		char *out = bstrdup("");
		pthread_mutex_unlock(&state->lock);
		return out;
	}

	char *out = bmalloc(total_len);
	out[0] = '\0';
	for (int i = first; i < state->line_count; i++) {
		if (state->stable_mode && state->lines[i].text[0] == '\0')
			continue;
		strcat(out, state->lines[i].text);
		strcat(out, "\n");
	}
	if (preview)
		strcat(out, preview);
	else if (out[0] != '\0') {
		/* Trim the trailing newline after the last finalized line when
		 * there's no preview to follow it. */
		size_t len = strlen(out);
		if (len > 0 && out[len - 1] == '\n')
			out[len - 1] = '\0';
	}

	pthread_mutex_unlock(&state->lock);
	return out;
}

bool tea_caption_state_last_line_is_partial(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);
	bool result = state->preview_text != NULL;
	if (state->stable_mode) {
		result = false;
		for (int i = state->line_count - 1; i >= 0; i--) {
			if (state->lines[i].text[0] != '\0') {
				result = state->lines[i].open;
				break;
			}
		}
	}
	pthread_mutex_unlock(&state->lock);
	return result;
}

/* ---------------- snapshot ---------------- */

uint64_t tea_caption_state_revision(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);
	uint64_t result = state->revision;
	pthread_mutex_unlock(&state->lock);
	return result;
}

void tea_caption_state_set_stable_tail_lines(tea_caption_state_t *state, bool enabled)
{
	pthread_mutex_lock(&state->lock);
	if (state->stable_tail_lines != enabled) {
		state->stable_tail_lines = enabled;
		state->revision++;
	}
	pthread_mutex_unlock(&state->lock);
}

/* The held unstable tail (see tea_hold_tail_locked()), or NULL. */
static const char *tea_unstable_tail(const tea_caption_line_t *line)
{
	return line->partial && line->partial[0] ? line->partial : NULL;
}

void tea_caption_state_snapshot(tea_caption_state_t *state, bool include_tail, tea_caption_snapshot_t *out)
{
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&state->lock);
	out->revision = state->revision;
	out->activity = state->activity;

	/* Same selection as tea_caption_state_render(): the newest `want` lines
	 * with committed text. Tail-only lines (no committed text yet) never
	 * count against that budget, so showing a tail never pushes an older
	 * committed line off the list (which would bring it back later). */
	int want = tea_visible_lines_locked(state);
	int first = state->line_count;
	int shown = 0;
	while (first > 0 && shown < want) {
		first--;
		if (!state->stable_mode || state->lines[first].text[0] != '\0')
			shown++;
	}
	const int cap = TEA_CAPTION_SNAPSHOT_MAX_LINES - 1; /* room for the preview */
	for (int i = first; i < state->line_count && out->count < cap; i++) {
		const tea_caption_line_t *line = &state->lines[i];
		const char *tail = (state->stable_mode && include_tail) ? tea_unstable_tail(line) : NULL;
		if (state->stable_mode && line->text[0] == '\0' && !tail)
			continue;
		tea_caption_snapshot_line_t *dst = &out->lines[out->count++];
		dst->key = line->key;
		dst->text = bstrdup(line->text);
		dst->tail = tail ? bstrdup(tail) : NULL;
		dst->open = line->open;
		dst->activity = line->activity;
	}
	if (!state->stable_mode && state->preview_text) {
		tea_caption_snapshot_line_t *dst = &out->lines[out->count++];
		dst->key = state->preview_key ? state->preview_key : UINT64_MAX;
		dst->text = bstrdup(state->preview_text);
		dst->tail = NULL;
		dst->open = true;
		dst->activity = state->activity;
	}
	pthread_mutex_unlock(&state->lock);
}

void tea_caption_snapshot_free(tea_caption_snapshot_t *snapshot)
{
	if (!snapshot)
		return;
	for (int i = 0; i < snapshot->count; i++) {
		bfree(snapshot->lines[i].text);
		bfree(snapshot->lines[i].tail);
	}
	memset(snapshot, 0, sizeof(*snapshot));
}
