#pragma once

/*
 * OBS-free diagnostics policy for "why are there no captions?".
 *
 * Plain C (also compiled as C++17 by asr-client.cpp and the policy test): no
 * allocation, no clocks -- every function takes "now" in milliseconds of a
 * monotonic clock chosen by the caller -- so the audio level maths and the
 * warning timers are unit tested without OBS, Qt or a server
 * (tests/asr-client-policy-test.cpp).
 *
 * What it answers, from the plugin's side of the wire:
 *   - is audio leaving OBS (frames sent, longest gap between sends)?
 *   - is that audio audible (RMS / peak dBFS of exactly what is sent, i.e.
 *     after the plugin's downmix to mono and resample to 16 kHz)?
 *   - does the server hear speech in it (speech.started / transcript events)?
 *   - is the server answering at all (any event, audio.ack included)?
 * The server logs the same audio's dBFS and its VAD probability every 5 s
 * (docs/diagnostics.md lines the two up).
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Audio level                                                               */
/* ------------------------------------------------------------------------ */

/* Level reported for digital silence (log10(0) is -inf). */
#define TEA_DBFS_FLOOR (-120.0)

typedef struct {
	double sum_squares; /* of samples normalised to [-1, 1) */
	uint64_t samples;
	int32_t peak; /* largest |sample| (0..32768) */
} tea_pcm_level_t;

static inline void tea_pcm_level_reset(tea_pcm_level_t *level)
{
	memset(level, 0, sizeof(*level));
}

static inline void tea_pcm_level_add(tea_pcm_level_t *level, const int16_t *pcm, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int32_t v = pcm[i];
		int32_t a = v < 0 ? -v : v;
		if (a > level->peak)
			level->peak = a;
		double x = (double)v / 32768.0;
		level->sum_squares += x * x;
	}
	level->samples += count;
}

/* dBFS of a normalised amplitude; full scale (1.0) is 0 dBFS. */
static inline double tea_dbfs_from_amplitude(double amplitude)
{
	if (amplitude <= 0.0)
		return TEA_DBFS_FLOOR;
	double db = 20.0 * log10(amplitude);
	return db < TEA_DBFS_FLOOR ? TEA_DBFS_FLOOR : db;
}

/* RMS level in dBFS (a full-scale sine is -3.01 dBFS, full-scale square 0). */
static inline double tea_pcm_level_rms_dbfs(const tea_pcm_level_t *level)
{
	if (level->samples == 0)
		return TEA_DBFS_FLOOR;
	return tea_dbfs_from_amplitude(sqrt(level->sum_squares / (double)level->samples));
}

static inline double tea_pcm_level_peak_dbfs(const tea_pcm_level_t *level)
{
	return tea_dbfs_from_amplitude((double)level->peak / 32768.0);
}

/* ------------------------------------------------------------------------ */
/* Speech state (from speech.started / segment.queued / final)              */
/* ------------------------------------------------------------------------ */

#define TEA_SPEECH_LISTENING 0  /* connected, no speech right now */
#define TEA_SPEECH_DETECTED 1   /* the server's VAD is inside speech */
#define TEA_SPEECH_PROCESSING 2 /* a segment was closed and is being transcribed */

typedef struct {
	bool in_speech;
	int pending; /* closed segments waiting for their final / skipped / error */
} tea_speech_state_t;

static inline void tea_speech_reset(tea_speech_state_t *s)
{
	s->in_speech = false;
	s->pending = 0;
}

static inline void tea_speech_on_started(tea_speech_state_t *s)
{
	s->in_speech = true;
}

static inline void tea_speech_on_queued(tea_speech_state_t *s)
{
	s->in_speech = false;
	s->pending++;
}

/* transcript.final, segment.skipped or segment.error: one segment is done. */
static inline void tea_speech_on_segment_done(tea_speech_state_t *s)
{
	if (s->pending > 0)
		s->pending--;
}

static inline int tea_speech_state(const tea_speech_state_t *s)
{
	if (s->in_speech)
		return TEA_SPEECH_DETECTED;
	return s->pending > 0 ? TEA_SPEECH_PROCESSING : TEA_SPEECH_LISTENING;
}

/* ------------------------------------------------------------------------ */
/* Warnings                                                                  */
/* ------------------------------------------------------------------------ */

#define TEA_WARN_NO_AUDIO 0x1  /* connected, nothing sent for > 3 s */
#define TEA_WARN_QUIET 0x2     /* sending, but below -60 dBFS for > 10 s */
#define TEA_WARN_NO_SPEECH 0x4 /* audible audio for > 15 s, the server reports no speech */
#define TEA_WARN_STALLED 0x8   /* sending, but no server event at all for > 5 s */
#define TEA_WARN_COUNT 4

#define TEA_WARN_NO_AUDIO_MS 3000
#define TEA_WARN_QUIET_MS 10000
#define TEA_WARN_NO_SPEECH_MS 15000
#define TEA_WARN_STALLED_MS 5000
#define TEA_WARN_REPEAT_MS 30000 /* each warning at most once per 30 s */
#define TEA_WARN_QUIET_DBFS (-60.0)
/* "Sending" for the quiet / stalled warnings: something was sent this recently. */
#define TEA_WARN_SENDING_MS 1000

typedef struct {
	bool session; /* a session is running (audio may be sent) */
	int64_t session_start_ms;
	int64_t last_send_ms;                 /* -1: nothing sent this session */
	int64_t last_audible_ms;              /* last frame at or above TEA_WARN_QUIET_DBFS */
	int64_t quiet_since_ms;               /* start of the current below-threshold stretch, -1 if audible */
	int64_t last_server_ms;               /* last event of any kind from the server */
	int64_t audible_without_speech_ms;    /* audible audio sent since the server last reported speech */
	int64_t last_warn_ms[TEA_WARN_COUNT]; /* -1: never */
} tea_warn_state_t;

static inline void tea_warn_session_start(tea_warn_state_t *w, int64_t now_ms)
{
	memset(w, 0, sizeof(*w));
	w->session = true;
	w->session_start_ms = now_ms;
	w->last_send_ms = -1;
	w->last_audible_ms = -1;
	w->quiet_since_ms = -1;
	w->last_server_ms = now_ms;
	for (int i = 0; i < TEA_WARN_COUNT; i++)
		w->last_warn_ms[i] = -1;
}

static inline void tea_warn_session_end(tea_warn_state_t *w)
{
	w->session = false;
}

/* One frame of `duration_ms` audio at `rms_dbfs` was sent. */
static inline void tea_warn_on_send(tea_warn_state_t *w, int64_t now_ms, int64_t duration_ms, double rms_dbfs)
{
	w->last_send_ms = now_ms;
	if (rms_dbfs >= TEA_WARN_QUIET_DBFS) {
		w->last_audible_ms = now_ms;
		w->quiet_since_ms = -1;
		w->audible_without_speech_ms += duration_ms;
	} else if (w->quiet_since_ms < 0) {
		w->quiet_since_ms = now_ms;
	}
}

static inline void tea_warn_on_server_event(tea_warn_state_t *w, int64_t now_ms)
{
	w->last_server_ms = now_ms;
}

/* speech.started or any transcript event: the server hears speech. */
static inline void tea_warn_on_speech(tea_warn_state_t *w)
{
	w->audible_without_speech_ms = 0;
}

static inline int tea_warn_index(int flag)
{
	int i = 0;
	while (flag > 1) {
		flag >>= 1;
		i++;
	}
	return i;
}

/* Which warnings hold right now, before rate limiting. */
static inline int tea_warn_conditions(const tea_warn_state_t *w, int64_t now_ms)
{
	if (!w->session)
		return 0;
	int flags = 0;
	const int64_t last_send = w->last_send_ms >= 0 ? w->last_send_ms : w->session_start_ms;
	const bool sending = w->last_send_ms >= 0 && now_ms - w->last_send_ms <= TEA_WARN_SENDING_MS;
	if (now_ms - last_send > TEA_WARN_NO_AUDIO_MS)
		flags |= TEA_WARN_NO_AUDIO;
	if (sending && w->quiet_since_ms >= 0 && now_ms - w->quiet_since_ms > TEA_WARN_QUIET_MS)
		flags |= TEA_WARN_QUIET;
	if (sending && w->audible_without_speech_ms > TEA_WARN_NO_SPEECH_MS)
		flags |= TEA_WARN_NO_SPEECH;
	if (sending && now_ms - w->last_server_ms > TEA_WARN_STALLED_MS)
		flags |= TEA_WARN_STALLED;
	return flags;
}

/* The warnings to log now: conditions that hold and were not logged in the
 * last TEA_WARN_REPEAT_MS. Marks them as logged. */
static inline int tea_warn_due(tea_warn_state_t *w, int64_t now_ms)
{
	int holding = tea_warn_conditions(w, now_ms);
	int due = 0;
	for (int flag = 1; flag < (1 << TEA_WARN_COUNT); flag <<= 1) {
		if (!(holding & flag))
			continue;
		int64_t *last = &w->last_warn_ms[tea_warn_index(flag)];
		if (*last >= 0 && now_ms - *last < TEA_WARN_REPEAT_MS)
			continue;
		*last = now_ms;
		due |= flag;
	}
	return due;
}

/* ------------------------------------------------------------------------ */
/* Heartbeat window                                                          */
/* ------------------------------------------------------------------------ */

#define TEA_HEARTBEAT_MS 10000

#define TEA_EV_PARTIAL 0
#define TEA_EV_STABLE 1
#define TEA_EV_FINAL 2
#define TEA_EV_SPEECH_STARTED 3
#define TEA_EV_SEGMENT_QUEUED 4
#define TEA_EV_AUDIO_ACK 5
#define TEA_EV_OTHER 6
#define TEA_EV_COUNT 7

typedef struct {
	int64_t start_ms;
	uint64_t frames;
	uint64_t samples; /* 16 kHz mono */
	int64_t last_send_ms;
	int64_t max_gap_ms; /* longest time between two sends (or window start and the first send) */
	tea_pcm_level_t level;
	uint32_t events[TEA_EV_COUNT];
} tea_heartbeat_t;

static inline void tea_heartbeat_reset(tea_heartbeat_t *h, int64_t now_ms)
{
	memset(h, 0, sizeof(*h));
	h->start_ms = now_ms;
	h->last_send_ms = now_ms;
}

static inline void tea_heartbeat_on_send(tea_heartbeat_t *h, int64_t now_ms, const int16_t *pcm, size_t samples)
{
	int64_t gap = now_ms - h->last_send_ms;
	if (gap > h->max_gap_ms)
		h->max_gap_ms = gap;
	h->last_send_ms = now_ms;
	h->frames++;
	h->samples += samples;
	tea_pcm_level_add(&h->level, pcm, samples);
}

/* The longest gap, counting a send-free stretch that is still going on. */
static inline int64_t tea_heartbeat_max_gap(const tea_heartbeat_t *h, int64_t now_ms)
{
	int64_t open_gap = now_ms - h->last_send_ms;
	return open_gap > h->max_gap_ms ? open_gap : h->max_gap_ms;
}

static inline bool tea_heartbeat_due(const tea_heartbeat_t *h, int64_t now_ms)
{
	return now_ms - h->start_ms >= TEA_HEARTBEAT_MS;
}

static inline int tea_event_kind(const char *type)
{
	if (!type)
		return TEA_EV_OTHER;
	if (strcmp(type, "transcript.partial") == 0)
		return TEA_EV_PARTIAL;
	if (strcmp(type, "transcript.stable") == 0)
		return TEA_EV_STABLE;
	if (strcmp(type, "transcript.final") == 0)
		return TEA_EV_FINAL;
	if (strcmp(type, "speech.started") == 0)
		return TEA_EV_SPEECH_STARTED;
	if (strcmp(type, "segment.queued") == 0)
		return TEA_EV_SEGMENT_QUEUED;
	if (strcmp(type, "audio.ack") == 0)
		return TEA_EV_AUDIO_ACK;
	return TEA_EV_OTHER;
}
