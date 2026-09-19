#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Subtitle state machine described in docs/08-obs-plugin.md section 3.
 *
 * Thread-safety: every entry point takes an internal mutex. Events come in
 * from the asr-client worker thread (on_partial/on_final/...), the
 * rendered string is pulled from the graphics/update thread via
 * tea_caption_state_render(). Neither side ever blocks on network or does
 * more than a memcpy-sized amount of work while holding the lock.
 */
typedef struct tea_caption_state tea_caption_state_t;

tea_caption_state_t *tea_caption_state_create(void);
void tea_caption_state_destroy(tea_caption_state_t *state);

/* Total on-screen lines including the current (possibly partial) line.
 * Finalized lines kept = max(total_lines - 1, 1). */
void tea_caption_state_set_max_lines(tea_caption_state_t *state, int total_lines);

/* Clears all finalized lines and any in-flight preview. Call when a brand
 * new session starts (including after reconnect -- v0.1 has no resume) or
 * when the connection is lost, so stale text never lingers pretending the
 * session is still alive. */
void tea_caption_state_reset(tea_caption_state_t *state);

/* transcript.partial: replaces the full text of segment_id, only if
 * revision is strictly greater than what we've already shown and the
 * segment has not already reached a terminal state. Ignored (not an error)
 * if `enable_partial` is false -- callers should only invoke this once
 * hello/capabilities has confirmed the server offers previews. */
void tea_caption_state_on_partial(tea_caption_state_t *state, const char *session_id, const char *segment_id,
				  uint64_t revision, const char *text);

/* transcript.final: the segment's terminal, immutable text. Always wins
 * over any partial for the same segment_id regardless of revision
 * ordering glitches, and after this call further partials for that
 * segment_id are rejected. */
void tea_caption_state_on_final(tea_caption_state_t *state, const char *session_id, const char *segment_id,
				uint64_t revision, const char *text);

/* segment.skipped / segment.error: drop any preview for this segment
 * without ever having shown finalized text for it. */
void tea_caption_state_on_segment_dropped(tea_caption_state_t *state, const char *session_id, const char *segment_id);

/* session.cancelled: clears all not-yet-final previews; finalized lines
 * already shown are left alone (they're already immutable text). */
void tea_caption_state_on_session_cancelled(tea_caption_state_t *state, const char *session_id);

/* Returns a newly bfree()-able UTF-8 string containing transcript text only,
 * ready to hand to text_ft2_source's "text" setting. Connectivity, model,
 * token, reconnect and server-error text deliberately never enters this
 * state machine; those diagnostics belong in the Tools/source status UI.
 * Never returns NULL (returns an empty string when there is no transcript). */
char *tea_caption_state_render(tea_caption_state_t *state);

/* True while the last-rendered line is a partial (not yet final), so the
 * caller can render it with reduced opacity / a different color per
 * docs/08. */
bool tea_caption_state_last_line_is_partial(tea_caption_state_t *state);

#ifdef __cplusplus
}
#endif
