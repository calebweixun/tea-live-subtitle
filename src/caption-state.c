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
	bool open;
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
};

tea_caption_state_t *tea_caption_state_create(void)
{
	tea_caption_state_t *st = bzalloc(sizeof(tea_caption_state_t));
	pthread_mutex_init(&st->lock, NULL);
	st->total_lines = 2;
	return st;
}

static void tea_drop_oldest_line_locked(tea_caption_state_t *st)
{
	if (st->line_count <= 0)
		return;
	bfree(st->lines[0].text);
	memmove(&st->lines[0], &st->lines[1], sizeof(tea_caption_line_t) * (size_t)(st->line_count - 1));
	st->line_count--;
	memset(&st->lines[st->line_count], 0, sizeof(tea_caption_line_t));
}

static void tea_clear_finalized_locked(tea_caption_state_t *st)
{
	for (int i = 0; i < st->line_count; i++) {
		bfree(st->lines[i].text);
		st->lines[i].text = NULL;
	}
	memset(st->lines, 0, sizeof(st->lines));
	st->line_count = 0;
}

static void tea_clear_preview_locked(tea_caption_state_t *st)
{
	bfree(st->preview_text);
	st->preview_text = NULL;
	st->preview_segment_id[0] = '\0';
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
		state->lines[i].open = false;
	memset(state->segments, 0, sizeof(state->segments));
	state->next_segment_slot = 0;
	state->have_session = false;
	state->current_session_id[0] = '\0';
	state->carry_over = true;
	state->epoch++;
	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_set_stable_mode(tea_caption_state_t *state, bool enabled)
{
	pthread_mutex_lock(&state->lock);
	if (state->stable_mode != enabled) {
		tea_clear_preview_locked(state);
		state->stable_mode = enabled;
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

static void tea_push_finalized_locked(tea_caption_state_t *st, const char *text)
{
	int keep = tea_visible_lines_locked(st);
	while (st->line_count >= keep || st->line_count >= TEA_MAX_FINALIZED_LINES)
		tea_drop_oldest_line_locked(st);
	tea_caption_line_t *line = &st->lines[st->line_count++];
	memset(line, 0, sizeof(*line));
	line->text = bstrdup(text ? text : "");
	line->order = st->line_count > 1 ? st->lines[st->line_count - 2].order : 0;
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
	line->open = true;
	seg->had_line = true;
	return line;
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
		/* Closed, duplicate or out of order: never an error, never a change. */
		pthread_mutex_unlock(&state->lock);
		return;
	}
	seg->stable_revision = stable_revision;

	tea_caption_line_t *line = tea_stable_line_locked(state, seg, segment_index);
	if (line && !tea_stable_extend_locked(line, text))
		state->stable_mismatches++;

	if (tea_stable_state_is_closing(stable_state)) {
		seg->stable_closed = true;
		seg->terminal = true;
		if (line)
			line->open = false;
	}
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
		pthread_mutex_unlock(&state->lock);
		return;
	}
	seg->revision = revision;

	if (state->stable_mode) {
		/* A partial may still rewrite any of its characters, and text_ft2
		 * cannot style half a line differently: stable mode shows only
		 * committed transcript.stable text. */
		pthread_mutex_unlock(&state->lock);
		return;
	}

	bfree(state->preview_text);
	state->preview_text = bstrdup(text ? text : "");
	snprintf(state->preview_segment_id, TEA_ID_BUF, "%s", segment_id);

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
		/* Show the final as soon as it arrives when it extends what is on
		 * screen (the closing "final" stable that follows is then a no-op).
		 * When it does not, keep the committed text and let the closing
		 * "diverged" stable append the final's tail. */
		tea_caption_line_t *line = seg->stable_closed ? NULL
							      : tea_stable_line_locked(state, seg, segment_index);
		if (line && tea_stable_extend_locked(line, text))
			line->open = false; /* line == final now */
		pthread_mutex_unlock(&state->lock);
		return;
	}

	if (state->preview_text && strncmp(state->preview_segment_id, segment_id, TEA_ID_BUF) == 0)
		tea_clear_preview_locked(state);

	tea_push_finalized_locked(state, text);

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
		/* Committed text already on screen stays; the "abandoned" stable
		 * that follows (if any) closes the line with that same text. */
		tea_caption_line_t *line = tea_find_line_locked(state, segment_id);
		if (line)
			line->open = false;
	}

	if (state->preview_text && strncmp(state->preview_segment_id, segment_id, TEA_ID_BUF) == 0)
		tea_clear_preview_locked(state);
	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_on_session_cancelled(tea_caption_state_t *state, const char *session_id)
{
	pthread_mutex_lock(&state->lock);
	if (state->have_session && session_id && session_id[0] &&
	    strncmp(state->current_session_id, session_id, TEA_ID_BUF) == 0) {
		tea_clear_preview_locked(state);
		/* Nothing more is sent after a cancel: freeze committed lines. */
		for (int i = 0; i < state->line_count; i++)
			state->lines[i].open = false;
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
