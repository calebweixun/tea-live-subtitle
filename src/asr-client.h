#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio-tap.h"
#include "caption-state.h"
#include "recognition-hints.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Plain-C facade over the Qt/C++ WebSocket client (asr-client.cpp) so
 * plugin-main.c / captions-source.c never need to include Qt headers.
 *
 * NOTE on architecture (see captions-source.c's own top-of-file comment for
 * the full rationale/tradeoff writeup): docs/08-obs-plugin.md sketches a
 * single *global* connection shared by every caption source ("global
 * connection settings, per-source appearance only"). This codebase does NOT
 * do that -- there is one `tea_asr_client_t` per `tea_live_subtitle_source`
 * instance, each opening its own TCP+WebSocket connection to the server.
 * There is no `tea-service.c` file; do not assume one exists.
 *
 * NOTE on `max_continuous_sessions`: the server now enforces this advertised
 * capability (default 2) when processing `session.start`. An excess source
 * receives an `error` event with `code="concurrent_session_limit"`,
 * `retryable=true`, followed by the dedicated WebSocket close code 4029.
 * The client shows the server's reason (or an actionable fallback derived
 * from 4029) and keeps retrying because a slot may become available. Do not
 * confuse that admission error with `session_limit` (this session's own
 * pending-segment queue filled up, or -- rejected before accept() -- the
 * server's max_total_connections cap), or with close code 1013, which is
 * shared by `queue_full`, `session_limit`, `slow_client` and `rate_limited`.
 *
 * Every attempt starts with an authenticated GET /v1/capabilities preflight
 * and only opens the WebSocket if that succeeds: the server rejects
 * unauthenticated / rate_limited / forbidden_origin / session_limit upgrades
 * before accept(), which a real client only ever sees as a reason-less HTTP
 * 403. Reconnect backoff and the auth-failure budget live in
 * asr-error-policy.hpp (ReconnectBackoff).
 *
 * The client itself lives on its own Qt worker thread: none of these calls
 * do blocking network I/O, they just hand off to that thread.
 */
typedef struct tea_asr_client tea_asr_client_t;

/* `tap` and `captions` must outlive the client. */
tea_asr_client_t *tea_asr_client_create(tea_audio_tap_t *tap, tea_caption_state_t *captions);
void tea_asr_client_destroy(tea_asr_client_t *client);

void tea_asr_client_set_server(tea_asr_client_t *client, const char *host, int port);
void tea_asr_client_set_token_path(tea_asr_client_t *client, const char *token_path);

/* Starts (or restarts, if server/token changed) the connect-handshake-
 * stream loop. Safe to call repeatedly. */
void tea_asr_client_start(tea_asr_client_t *client);
void tea_asr_client_stop(tea_asr_client_t *client);

bool tea_asr_client_is_connected(tea_asr_client_t *client);

/* Caller owns the returned buffer and must bfree() it. Human-readable,
 * already-localized-enough-for-a-diagnostics-label status such as
 * "connecting", "model_loading", "session active", "disconnected: <reason>". */
char *tea_asr_client_status_text(tea_asr_client_t *client);

uint64_t tea_asr_client_dropped_audio_frames(tea_asr_client_t *client);

/* True once the HTTP GET /v1/capabilities probe (or its failure/timeout
 * fallback) has completed, i.e. it's safe to trust
 * tea_asr_client_supports_partial_transcripts()'s answer for this
 * connection attempt. */
bool tea_asr_client_capabilities_known(tea_asr_client_t *client);
bool tea_asr_client_supports_partial_transcripts(tea_asr_client_t *client);

/* "Stable captions (append-only)" source setting, default on. When on and the
 * server advertises capabilities.features.stable_transcripts, session.start
 * asks for `"stable":{"agreement":2}` and the caption state only shows
 * committed transcript.stable text. When the server does not offer it (or
 * rejects the request), the client silently falls back to the legacy
 * partial-replace preview; it never fails to connect because of it. Takes
 * effect on the next tea_asr_client_start(). */
void tea_asr_client_set_stable_captions(tea_asr_client_t *client, bool enabled);
/* Only meaningful once capabilities are known. */
bool tea_asr_client_supports_stable_transcripts(tea_asr_client_t *client);
/* True while the current session was started with `stable`. */
bool tea_asr_client_stable_captions_active(tea_asr_client_t *client);

/* "Sentence break" source setting: the server's end-of-segment silence in ms
 * (0 = server default). Sent as session.start.segmentation.end_silence_ms,
 * clamped to the advertised range, only when the server advertises
 * capabilities.features.segmentation_control; otherwise left out. Takes
 * effect on the next tea_asr_client_start(). */
void tea_asr_client_set_end_silence_ms(tea_asr_client_t *client, int ms);
/* Only meaningful once capabilities are known; the out-params may be NULL. */
bool tea_asr_client_supports_segmentation(tea_asr_client_t *client, int *min_ms, int *max_ms, int *default_ms);
/* The value the running session uses (session.started.preview_policy.endpoint_silence_ms), -1 if unknown. */
int tea_asr_client_effective_end_silence_ms(tea_asr_client_t *client);

/* ---- diagnostics: "why are there no captions?" (docs/diagnostics.md) ---- */

#define TEA_CONN_STOPPED 0
#define TEA_CONN_WAITING_AUDIO 1 /* no audio source selected */
#define TEA_CONN_WAITING_TOKEN 2 /* token file missing / rejected */
#define TEA_CONN_CONNECTING 3
#define TEA_CONN_CONNECTED 4 /* WebSocket up, no session yet */
#define TEA_CONN_ACTIVE 5    /* session running, audio flowing */
#define TEA_CONN_RECONNECTING 6

typedef struct {
	int connection;        /* TEA_CONN_* */
	int speech;            /* TEA_SPEECH_* (asr-diagnostics.h): 0 listening, 1 speech, 2 processing */
	double input_dbfs;     /* RMS of the last ~0.5 s pulled from the audio source, -120 = silence */
	bool input_recent;     /* audio arrived from the source in the last second */
	int64_t ms_since_text; /* since the last partial / stable / final, -1 = none this session */
} tea_asr_client_diag_t;

/* Safe from any thread. */
void tea_asr_client_get_diag(tea_asr_client_t *client, tea_asr_client_diag_t *out);

/* "Record recognition events (debugging)": when `dir` is non-empty, every
 * session writes its server events to <dir>/tea-trace-<time>-<session>.jsonl
 * in the {"t_ms", "event"} format tests/replay reads (size-capped). NULL or ""
 * turns it off. Takes effect on the next session. */
void tea_asr_client_set_trace_dir(tea_asr_client_t *client, const char *dir);

/* ---- recognition hints (辨識提示, docs/recognition-hints.md) ---- */

/* The source's hint settings, raw as typed: the dictionary profile (blank or
 * the "none" entry = none), the domain description, the hotwords and
 * replacement fields, and an optional hints file re-read at every session
 * start. Sent as session.start.context only when the server advertises
 * features.context_biasing. NULLs are empty. Takes effect on the next
 * tea_asr_client_start(). */
void tea_asr_client_set_hints(tea_asr_client_t *client, const char *profile, const char *domain, const char *hotwords,
			      const char *replacements, const char *file_path);

#define TEA_HINTS_REASON_MAX 192
#define TEA_HINTS_NAME_MAX 64

typedef struct {
	int capability;            /* TEA_HINTS_CAP_* (recognition-hints.h) */
	tea_hints_limits_t limits; /* capabilities.features.context_limits */
	bool sent;                 /* the last session.start carried a context */
	bool rejected;             /* the server refused it; the session runs without */
	char reject_reason[TEA_HINTS_REASON_MAX];
	bool applied;     /* session.started echoed a context */
	bool unconfirmed; /* the session started but did not echo the context it was sent */
	char applied_profile[TEA_HINTS_NAME_MAX];
	int applied_domain_chars;
	int applied_hotwords;
	int applied_replacements;
	int applied_prompt_tokens; /* -1 = not reported */
	/* the server put the domain and hotwords into the model prompt
	 * (TEA_ASR_CONTEXT_PROMPT); replacements apply either way */
	bool prompt_applied;
	/* what was cut or refused when the last context was built */
	tea_hints_report_t report;
	int invalid_lines;
	char invalid[TEA_HINTS_ISSUES_MAX];
	bool file_error; /* the hints file is set but could not be read */
} tea_asr_client_hints_status_t;

/* Safe from any thread. */
void tea_asr_client_get_hints_status(tea_asr_client_t *client, tea_asr_client_hints_status_t *out);

/* GET /v1/dictionaries: the server's dictionary files a profile can name. */
#define TEA_DICT_UNKNOWN 0  /* never fetched */
#define TEA_DICT_FETCHING 1 /* request in flight */
#define TEA_DICT_OK 2
#define TEA_DICT_FAILED 3
#define TEA_DICT_MAX 64

typedef struct {
	char name[TEA_HINTS_NAME_MAX];
	int hotwords_count;
	int replacements_count;
} tea_dictionary_entry_t;

/* `done(param)` runs on the client's thread once the request finished
 * (whatever the outcome) or was dropped (client destroyed, newer request);
 * it is called exactly once. Uses the server and token of the last start.
 * Never blocks. */
typedef void (*tea_asr_dictionaries_done_t)(void *param);
void tea_asr_client_fetch_dictionaries(tea_asr_client_t *client, tea_asr_dictionaries_done_t done, void *param);
/* State (TEA_DICT_*); fills up to `max` entries (*count), and `error` (may be
 * NULL) with why a fetch failed. `age_ms` (may be NULL) gets the time since
 * the last completed fetch, -1 if none. Safe from any thread. */
int tea_asr_client_dictionaries(tea_asr_client_t *client, tea_dictionary_entry_t *out, int max, int *count, char *error,
				size_t error_size, int64_t *age_ms);

#ifdef __cplusplus
}
#endif
