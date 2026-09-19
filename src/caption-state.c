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

typedef struct {
	char segment_id[TEA_ID_BUF];
	uint64_t revision;
	bool terminal;
	bool used;
} tea_segment_track_t;

struct tea_caption_state {
	pthread_mutex_t lock;

	int total_lines; /* includes the current/preview line */

	char current_session_id[TEA_ID_BUF];
	bool have_session;

	tea_segment_track_t segments[TEA_MAX_TRACKED_SEGMENTS];
	size_t next_segment_slot; /* round-robin eviction */

	char *finalized_lines[TEA_MAX_FINALIZED_LINES];
	int finalized_count;

	char *preview_text; /* NULL when no open preview */
	char preview_segment_id[TEA_ID_BUF];
};

tea_caption_state_t *tea_caption_state_create(void)
{
	tea_caption_state_t *st = bzalloc(sizeof(tea_caption_state_t));
	pthread_mutex_init(&st->lock, NULL);
	st->total_lines = 2;
	return st;
}

static void tea_clear_finalized_locked(tea_caption_state_t *st)
{
	for (int i = 0; i < st->finalized_count; i++) {
		bfree(st->finalized_lines[i]);
		st->finalized_lines[i] = NULL;
	}
	st->finalized_count = 0;
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

void tea_caption_state_set_max_lines(tea_caption_state_t *state, int total_lines)
{
	if (total_lines < 1)
		total_lines = 1;
	if (total_lines > TEA_MAX_FINALIZED_LINES)
		total_lines = TEA_MAX_FINALIZED_LINES;

	pthread_mutex_lock(&state->lock);
	state->total_lines = total_lines;
	int keep = total_lines > 1 ? total_lines - 1 : 1;
	while (state->finalized_count > keep) {
		/* Drop the oldest to make room, shifting the rest down. */
		bfree(state->finalized_lines[0]);
		memmove(&state->finalized_lines[0], &state->finalized_lines[1],
			sizeof(char *) * (size_t)(state->finalized_count - 1));
		state->finalized_count--;
	}
	pthread_mutex_unlock(&state->lock);
}

static void tea_reset_locked(tea_caption_state_t *st)
{
	tea_clear_finalized_locked(st);
	bfree(st->preview_text);
	st->preview_text = NULL;
	st->preview_segment_id[0] = '\0';
	memset(st->segments, 0, sizeof(st->segments));
	st->next_segment_slot = 0;
}

void tea_caption_state_reset(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);
	tea_reset_locked(state);
	state->have_session = false;
	state->current_session_id[0] = '\0';
	pthread_mutex_unlock(&state->lock);
}

/* Must hold state->lock. Ensures we're tracking `session_id` as current,
 * resetting everything if it differs from what we had (new/reconnected
 * session -- v0.1 never resumes, so a session_id change always means a
 * fresh caption context). Returns false if session_id is NULL/empty. */
static bool tea_ensure_session_locked(tea_caption_state_t *st, const char *session_id)
{
	if (!session_id || !session_id[0])
		return false;
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
	int keep = st->total_lines > 1 ? st->total_lines - 1 : 1;
	if (st->finalized_count >= keep) {
		bfree(st->finalized_lines[0]);
		memmove(&st->finalized_lines[0], &st->finalized_lines[1],
			sizeof(char *) * (size_t)(st->finalized_count - 1));
		st->finalized_count--;
	}
	st->finalized_lines[st->finalized_count++] = bstrdup(text ? text : "");
}

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

	bfree(state->preview_text);
	state->preview_text = bstrdup(text ? text : "");
	snprintf(state->preview_segment_id, TEA_ID_BUF, "%s", segment_id);

	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_on_final(tea_caption_state_t *state, const char *session_id, const char *segment_id,
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
		/* Terminal state already reached for this segment_id; final is
		 * only ever sent once per docs/04, ignore a duplicate/replay. */
		pthread_mutex_unlock(&state->lock);
		return;
	}
	seg->terminal = true;
	seg->revision = revision;

	if (state->preview_text && strncmp(state->preview_segment_id, segment_id, TEA_ID_BUF) == 0) {
		bfree(state->preview_text);
		state->preview_text = NULL;
		state->preview_segment_id[0] = '\0';
	}

	tea_push_finalized_locked(state, text);

	pthread_mutex_unlock(&state->lock);
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

	if (state->preview_text && strncmp(state->preview_segment_id, segment_id, TEA_ID_BUF) == 0) {
		bfree(state->preview_text);
		state->preview_text = NULL;
		state->preview_segment_id[0] = '\0';
	}
	pthread_mutex_unlock(&state->lock);
}

void tea_caption_state_on_session_cancelled(tea_caption_state_t *state, const char *session_id)
{
	pthread_mutex_lock(&state->lock);
	if (state->have_session && session_id && session_id[0] &&
	    strncmp(state->current_session_id, session_id, TEA_ID_BUF) == 0) {
		bfree(state->preview_text);
		state->preview_text = NULL;
		state->preview_segment_id[0] = '\0';
	}
	pthread_mutex_unlock(&state->lock);
}

char *tea_caption_state_render(tea_caption_state_t *state)
{
	pthread_mutex_lock(&state->lock);

	if (state->finalized_count == 0 && !state->preview_text) {
		/* The caller may choose a neutral placeholder for this empty state,
		 * but connection diagnostics must never be rendered as captions. */
		char *out = bstrdup("");
		pthread_mutex_unlock(&state->lock);
		return out;
	}

	size_t total_len = 1;
	for (int i = 0; i < state->finalized_count; i++)
		total_len += strlen(state->finalized_lines[i]) + 1;
	if (state->preview_text)
		total_len += strlen(state->preview_text) + 1;

	char *out = bmalloc(total_len);
	out[0] = '\0';
	for (int i = 0; i < state->finalized_count; i++) {
		strcat(out, state->finalized_lines[i]);
		strcat(out, "\n");
	}
	if (state->preview_text)
		strcat(out, state->preview_text);
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
	pthread_mutex_unlock(&state->lock);
	return result;
}
