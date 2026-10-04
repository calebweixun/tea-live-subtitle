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
 *   "silence": a source that delivers audio at real time, all zeros (a muted
 *             or silent source).
 *   "none":   no audio source selected at all.
 *   "gap:ON_MS:OFF_MS": "speech" for ON_MS, then no audio at all for OFF_MS
 *             (a media source that finished: the capture delivers nothing),
 *             then "speech" again. Exercises the lazy reconnect.
 *   "file:PATH": a 16 kHz mono s16le WAV file, looped, paced at real time
 *             (a real recording, e.g. congregational singing, so the
 *             server's real singing detector has something to label).
 */
#include "audio-tap.h"

#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAKE_RATE 16000
#define FAKE_SPEECH_SAMPLES (3 * FAKE_RATE)
#define FAKE_CYCLE_SAMPLES (FAKE_SPEECH_SAMPLES + FAKE_RATE * 3 / 2)

enum fake_mode { FAKE_SPEECH, FAKE_DEAD, FAKE_SILENCE, FAKE_NONE, FAKE_FILE, FAKE_GAP };

struct tea_audio_tap {
	enum fake_mode mode;
	double start_s;
	uint64_t produced;
};

static enum fake_mode g_default_mode = FAKE_SPEECH;
static uint64_t g_gap_from = 0, g_gap_to = 0; /* FAKE_GAP: samples [from, to) never arrive */
static int16_t *g_file_pcm = NULL;            /* FAKE_FILE: the WAV's samples */
static size_t g_file_samples = 0;

/* The samples of a 16 kHz mono 16-bit PCM WAV, or false. */
static bool load_wav(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	unsigned char head[12];
	bool ok = fread(head, 1, 12, f) == 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0;
	while (ok) {
		unsigned char chunk[8];
		if (fread(chunk, 1, 8, f) != 8) {
			ok = false;
			break;
		}
		const uint32_t size = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) | ((uint32_t)chunk[6] << 16) |
				      ((uint32_t)chunk[7] << 24);
		if (memcmp(chunk, "fmt ", 4) == 0) {
			unsigned char fmt[16];
			ok = size >= 16 && fread(fmt, 1, 16, f) == 16 && fmt[0] == 1 && fmt[2] == 1 &&
			     (fmt[4] | (fmt[5] << 8)) == FAKE_RATE && fmt[14] == 16 &&
			     fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR) == 0;
		} else if (memcmp(chunk, "data", 4) == 0) {
			g_file_samples = size / 2;
			g_file_pcm = malloc(g_file_samples * sizeof(int16_t));
			ok = g_file_pcm && fread(g_file_pcm, 2, g_file_samples, f) == g_file_samples &&
			     g_file_samples > 0;
			break;
		} else {
			ok = fseek(f, (long)(size + (size & 1)), SEEK_CUR) == 0;
		}
	}
	fclose(f);
	return ok;
}

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
	else if (strcmp(mode, "silence") == 0)
		g_default_mode = FAKE_SILENCE;
	else if (strncmp(mode, "gap:", 4) == 0) {
		const long on_ms = strtol(mode + 4, NULL, 10);
		const char *off = strchr(mode + 4, ':');
		const long off_ms = off ? strtol(off + 1, NULL, 10) : 0;
		g_gap_from = (uint64_t)on_ms * FAKE_RATE / 1000;
		g_gap_to = g_gap_from + (uint64_t)off_ms * FAKE_RATE / 1000;
		g_default_mode = FAKE_GAP;
	} else if (strncmp(mode, "file:", 5) == 0) {
		if (!load_wav(mode + 5)) {
			fprintf(stderr, "cannot read %s as 16 kHz mono 16-bit PCM WAV\n", mode + 5);
			exit(2);
		}
		g_default_mode = FAKE_FILE;
	} else
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
	if (tap->mode != FAKE_SPEECH && tap->mode != FAKE_SILENCE && tap->mode != FAKE_FILE && tap->mode != FAKE_GAP)
		return 0;
	uint64_t due = (uint64_t)((now_s() - tap->start_s) * FAKE_RATE);
	if (tap->mode == FAKE_GAP && due > g_gap_from && tap->produced < g_gap_to) {
		/* inside the gap nothing arrives; afterwards audio resumes at real
		 * time, without the gap's backlog */
		if (tap->produced < g_gap_from) {
			due = g_gap_from;
		} else if (due < g_gap_to) {
			return 0;
		} else {
			tap->produced = g_gap_to;
		}
	}
	if (due <= tap->produced)
		return 0;
	size_t n = (size_t)(due - tap->produced);
	if (n > max_samples)
		n = max_samples;
	for (size_t i = 0; i < n; i++) {
		uint64_t s = tap->produced + i;
		if (tap->mode == FAKE_FILE) {
			out[i] = g_file_pcm[s % g_file_samples];
			continue;
		}
		uint64_t phase = s % FAKE_CYCLE_SAMPLES;
		double v = 0.0;
		if ((tap->mode == FAKE_SPEECH || tap->mode == FAKE_GAP) && phase < FAKE_SPEECH_SAMPLES)
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
