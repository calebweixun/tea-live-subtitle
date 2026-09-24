#pragma once

#include <obs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Taps one OBS audio source via obs_source_add_audio_capture_callback(),
 * downmixes to mono float in the audio callback (cheap arithmetic only --
 * no allocation, no locks, no resampling on that thread) and buffers it in
 * a lock-free ring buffer at the source's native sample rate.
 *
 * A worker thread (owned by asr-client, not by this file) periodically
 * calls tea_audio_tap_pull_pcm16() to drain the ring buffer through
 * libobs's own audio_resampler to 16 kHz mono 16-bit PCM, which is the
 * exact wire format docs/04 requires.
 */
typedef struct tea_audio_tap tea_audio_tap_t;

tea_audio_tap_t *tea_audio_tap_create(void);
void tea_audio_tap_destroy(tea_audio_tap_t *tap);

/* Detaches from any previously attached source, then attaches to `source`
 * (may be NULL to just detach). Safe to call repeatedly, e.g. whenever the
 * user picks a different audio source in the settings dialog. */
void tea_audio_tap_set_source(tea_audio_tap_t *tap, obs_source_t *source);

/* True while an OBS audio source is attached. Any thread. The ASR client
 * uses it to avoid holding a server connection slot for a caption source
 * that has no audio to send. */
bool tea_audio_tap_has_source(tea_audio_tap_t *tap);

/* Consumer side, call only from a single worker thread. Resamples whatever
 * is currently available in the ring buffer to 16 kHz mono PCM16 and writes
 * up to `max_samples` int16 samples into `out`. Returns the number of
 * samples actually written (0 if nothing new is buffered yet). */
size_t tea_audio_tap_pull_pcm16(tea_audio_tap_t *tap, int16_t *out, size_t max_samples);

/* Samples dropped because the worker thread could not keep up with the
 * audio callback. Surfaced in the settings dialog; never reset silently. */
uint64_t tea_audio_tap_dropped_samples(tea_audio_tap_t *tap);

#ifdef __cplusplus
}
#endif
