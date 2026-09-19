#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the "tea_live_subtitle_source" obs_source_info with libobs.
 * Call once from obs_module_load(). */
void tea_captions_source_register(void);

/* Snapshot of one live tea_live_subtitle_source instance's connection state,
 * for the Tools-menu settings dialog (settings-dialog.cpp). There is no
 * global connection object in this plugin (see the architecture comment at
 * the top of captions-source.c) -- each source owns its own asr-client, so
 * this is the only way to see across all of them at once. */
typedef struct {
	const char *source_name; /* borrowed, valid only during the callback */
	const char *status_text; /* borrowed, valid only during the callback; do not bfree */
	uint64_t dropped_frames;
	bool connected;
	bool capabilities_known;
	bool supports_partial_transcripts; /* only meaningful if capabilities_known */
} tea_captions_source_info_t;

/* Calls `cb` once per currently-live tea_live_subtitle_source instance, in
 * unspecified order, synchronously on the calling thread. Safe to call from
 * the OBS UI thread while sources are being created/destroyed concurrently
 * (internally lock-protected); `cb` must not block. */
void tea_captions_source_for_each(void (*cb)(const tea_captions_source_info_t *info, void *user), void *user);

/* Restarts every live instance's connection attempt, clearing any
 * "stopped, won't retry" state (e.g. after a session_limit rejection caused
 * by a second source) so it tries fresh. */
void tea_captions_source_reconnect_all(void);

#ifdef __cplusplus
}
#endif
