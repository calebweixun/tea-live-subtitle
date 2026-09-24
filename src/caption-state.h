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

/* Connection lost, but keep what is on screen: drops any unconfirmed
 * preview, freezes every line (no segment of the old session may grow any
 * more), forgets the old session's segment tracking, and lets the *next*
 * session's text scroll in under the frozen lines instead of wiping them.
 * Used in stable mode so a reconnect never blanks the canvas; the caller is
 * responsible for calling tea_caption_state_reset() if the outage lasts too
 * long for the frozen text to still be meaningful. */
void tea_caption_state_hold_for_reconnect(tea_caption_state_t *state);

/* Selects how the *next* session's events are shown (the caller sets this
 * right before sending session.start):
 *   false: legacy partial mode -- one preview line that transcript.partial
 *          replaces wholesale, finals pushed below it.
 *   true:  stable mode -- only committed transcript.stable text is shown,
 *          stored per segment_id, append-only; transcript.partial is never
 *          rendered. Any preview line is dropped when switching. */
void tea_caption_state_set_stable_mode(tea_caption_state_t *state, bool enabled);
bool tea_caption_state_stable_mode(tea_caption_state_t *state);

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
 * segment_id are rejected.
 *
 * Stable mode: the final only ever *extends* the segment's line. If it does
 * not start with the committed text already on screen, the line is left
 * alone and the closing transcript.stable (state "diverged") that the server
 * sends right after decides how it ends. */
void tea_caption_state_on_final(tea_caption_state_t *state, const char *session_id, const char *segment_id,
				uint64_t revision, const char *text);

/* Same as tea_caption_state_on_final(), plus the segment_index so stable mode
 * can keep lines in transcript order when a segment shows up late. */
#define TEA_CAPTION_SEGMENT_INDEX_UNKNOWN UINT64_MAX
void tea_caption_state_on_final_indexed(tea_caption_state_t *state, const char *session_id, const char *segment_id,
					uint64_t segment_index, uint64_t revision, const char *text);

/* transcript.stable (stable mode only; ignored otherwise). `text` is the
 * segment's whole committed text; by contract it starts with the previous
 * value byte-for-byte, so only the new tail is appended. Defensive rule: a
 * value that does not start with what is already displayed never changes the
 * displayed text -- it is dropped and counted (see
 * tea_caption_state_stable_mismatches()). `stable_state` is "open", or one of
 * "final" / "diverged" / "abandoned", which close the segment's line.
 * Lines are kept per segment_id, so a later segment's stable text arriving
 * before an earlier segment's final never touches the earlier line. */
void tea_caption_state_on_stable(tea_caption_state_t *state, const char *session_id, const char *segment_id,
				 uint64_t segment_index, uint64_t stable_revision, const char *text,
				 const char *stable_state);

/* Number of transcript.stable values dropped because they did not extend the
 * displayed text (contract violation / old or buggy server). Cumulative for
 * the lifetime of the state object; diagnostics only (Tools dialog). */
uint64_t tea_caption_state_stable_mismatches(tea_caption_state_t *state);

/* segment.skipped / segment.error: drop any preview for this segment
 * without ever having shown finalized text for it. In stable mode, committed
 * text that is already on screen stays (it was promised never to change);
 * the line simply stops growing. */
void tea_caption_state_on_segment_dropped(tea_caption_state_t *state, const char *session_id, const char *segment_id);

/* session.cancelled: clears all not-yet-final previews; finalized lines
 * already shown are left alone (they're already immutable text). In stable
 * mode committed lines are likewise kept and frozen. */
void tea_caption_state_on_session_cancelled(tea_caption_state_t *state, const char *session_id);

/* Returns a newly bfree()-able UTF-8 string containing transcript text only,
 * ready to hand to text_ft2_source's "text" setting. Connectivity, model,
 * token, reconnect and server-error text deliberately never enters this
 * state machine; those diagnostics belong in the Tools/source status UI.
 * Never returns NULL (returns an empty string when there is no transcript). */
char *tea_caption_state_render(tea_caption_state_t *state);

/* True while the last-rendered line is a partial (not yet final), so the
 * caller can render it with reduced opacity / a different color per
 * docs/08. In stable mode: the last line is committed text of a segment
 * that may still grow (no closing transcript.stable yet). */
bool tea_caption_state_last_line_is_partial(tea_caption_state_t *state);

/* ---------------- per-line snapshot for the line renderer ----------------
 *
 * tea_caption_state_render() joins everything into one string, which is what
 * the e2e driver and older callers want. The OBS source renders every line
 * separately (alignment, wrapping, background box and fades are per line), so
 * it needs the same visible lines as a list, each with an identity that stays
 * put while the line grows.
 *
 * Unstable tail (stable mode only): the newest transcript.partial of a
 * segment is kept next to its committed line. When -- and only when -- that
 * partial starts with the committed text byte-for-byte, the bytes after it
 * are reported as `tail`. The committed `text` is never derived from a
 * partial, so showing the tail can never rewrite a committed character; the
 * tail disappears as soon as the segment closes (final / closing stable /
 * skipped / cancelled / reconnect). */
#define TEA_CAPTION_SNAPSHOT_MAX_LINES 12

typedef struct {
	uint64_t key; /* stable identity of the on-screen line, never 0 */
	char *text;   /* committed text (partial mode: final or preview), bfree()-able, never NULL */
	char *tail;   /* unstable tail after `text`, or NULL; bfree()-able */
	bool open;    /* stable mode: segment may still grow; partial mode: preview line */
} tea_caption_snapshot_line_t;

typedef struct {
	tea_caption_snapshot_line_t lines[TEA_CAPTION_SNAPSHOT_MAX_LINES];
	int count;
	uint64_t revision; /* tea_caption_state_revision() at the time of the snapshot */
} tea_caption_snapshot_t;

/* Increases on every change that may alter a snapshot or render; lets the
 * render thread skip snapshots when nothing happened. */
uint64_t tea_caption_state_revision(tea_caption_state_t *state);

/* Stable mode: also give a segment a line as soon as its first partial
 * arrives (tail only, no committed text yet), so the tail shows before the
 * first transcript.stable. Off by default; purely a display choice -- it never
 * changes what is sent to the server. */
void tea_caption_state_set_stable_tail_lines(tea_caption_state_t *state, bool enabled);

/* Fills `out` with copies of the visible lines, oldest first. With
 * include_tail=false every `tail` is NULL and tail-only lines are skipped,
 * which makes the list exactly the lines of tea_caption_state_render(). */
void tea_caption_state_snapshot(tea_caption_state_t *state, bool include_tail, tea_caption_snapshot_t *out);
void tea_caption_snapshot_free(tea_caption_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
