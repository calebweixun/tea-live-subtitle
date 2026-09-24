/*
 * OBS-free stand-in for src/audio-tap.c used by the end-to-end client test.
 *
 * It paces PCM at real time (16 kHz mono s16le, the wire format) so the real
 * TeaAsrClient sees the same cadence it would get from an OBS audio source.
 * Modes (see fake_audio_tap_set_mode):
 *   "speech": 3.0 s of a 440 Hz tone, then 1.5 s of silence, repeated. The
 *             server test FakeVad treats any window above 0.05 peak as speech,
 *             so every cycle yields one segment -> partials + one final.
 *   "dead":   a source is attached but never produces audio (e.g. a capture
 *             device that stopped). Exercises the server's idle_timeout.
 *   "none":   no audio source selected at all.
 */
#include "audio-tap.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAKE_RATE 16000
#define FAKE_SPEECH_SAMPLES (3 * FAKE_RATE)
#define FAKE_CYCLE_SAMPLES (FAKE_SPEECH_SAMPLES + FAKE_RATE * 3 / 2)

enum fake_mode { FAKE_SPEECH, FAKE_DEAD, FAKE_NONE };

struct tea_audio_tap {
	enum fake_mode mode;
	double start_s;
	uint64_t produced;
};

static enum fake_mode g_default_mode = FAKE_SPEECH;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void fake_audio_tap_set_default_mode(const char *mode)
{
	if (strcmp(mode, "dead") == 0)
		g_default_mode = FAKE_DEAD;
	else if (strcmp(mode, "none") == 0)
		g_default_mode = FAKE_NONE;
	else
		g_default_mode = FAKE_SPEECH;
}

tea_audio_tap_t *tea_audio_tap_create(void)
{
	tea_audio_tap_t *tap = calloc(1, sizeof(*tap));
	tap->mode = g_default_mode;
	tap->start_s = now_s();
	return tap;
}

void tea_audio_tap_destroy(tea_audio_tap_t *tap)
{
	free(tap);
}

void tea_audio_tap_set_source(tea_audio_tap_t *tap, obs_source_t *source)
{
	(void)tap;
	(void)source;
}

bool tea_audio_tap_has_source(tea_audio_tap_t *tap)
{
	return tap && tap->mode != FAKE_NONE;
}

size_t tea_audio_tap_pull_pcm16(tea_audio_tap_t *tap, int16_t *out, size_t max_samples)
{
	if (tap->mode != FAKE_SPEECH)
		return 0;
	uint64_t due = (uint64_t)((now_s() - tap->start_s) * FAKE_RATE);
	if (due <= tap->produced)
		return 0;
	size_t n = (size_t)(due - tap->produced);
	if (n > max_samples)
		n = max_samples;
	for (size_t i = 0; i < n; i++) {
		uint64_t s = tap->produced + i;
		uint64_t phase = s % FAKE_CYCLE_SAMPLES;
		double v = 0.0;
		if (phase < FAKE_SPEECH_SAMPLES)
			v = 0.3 * sin(2.0 * M_PI * 440.0 * (double)s / FAKE_RATE);
		out[i] = (int16_t)(v * 32767.0);
	}
	tap->produced += n;
	return n;
}

uint64_t tea_audio_tap_dropped_samples(tea_audio_tap_t *tap)
{
	(void)tap;
	return 0;
}
