#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio-tap.h"
#include "caption-state.h"

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
 * This matters operationally: docs/04's capabilities response advertises
 * `max_continuous_sessions=1`, so a *second* concurrently-active caption
 * source will have its `session.start` rejected (or its connection closed)
 * by the server. The client detects the server's `session_limit` error code
 * and WS close code 1013 (over-capacity) and reports a clear, non-retrying
 * status instead of retrying forever -- see TeaAsrClient::handleJsonMessage
 * and TeaAsrClient::scheduleReconnect in asr-client.cpp.
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

#ifdef __cplusplus
}
#endif
