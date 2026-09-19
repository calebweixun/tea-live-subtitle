#pragma once

/*
 * Single-producer / single-consumer ring buffer of mono float samples.
 *
 * This deliberately does NOT use C11 <stdatomic.h>: MSVC only exposes it
 * behind a compiler mode this project's CMake presets don't enable (CI
 * confirmed this fails with "C atomic support is not enabled" on the
 * Windows build), and hand-rolling per-platform atomics for something this
 * small isn't worth the portability risk. Instead the whole ring is
 * guarded by a plain pthread_mutex_t (OBS's own util/threading.h already
 * provides this cross-platform, including on Windows).
 *
 * The audio thread (producer) still never blocks: it uses
 * pthread_mutex_trylock() and, if the lock is contended (vanishingly rare
 * -- the consumer only ever holds it for a fixed-size memcpy), drops the
 * incoming chunk immediately rather than waiting. The consumer (resample/
 * network worker thread) uses a normal blocking lock since briefly waiting
 * on a lock that's only ever held for a memcpy is fine off the realtime
 * thread. Either way, overflow (buffer full) drops the *oldest* buffered
 * samples and accounts them in `dropped_samples`, so audio loss is always
 * visible instead of silent.
 */

#include <util/threading.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	float *buf;
	size_t capacity;

	size_t write_pos; /* next write index, protected by lock */
	size_t read_pos;  /* next read index, protected by lock */
	size_t fill;      /* samples currently buffered, protected by lock */

	pthread_mutex_t lock;

	uint64_t dropped_samples;          /* protected by lock */
	uint64_t pending_contention_drops; /* producer-thread-only, never touched by the consumer */
} tea_ring_buffer_t;

/* Not real-time safe; call during source create/destroy only. */
static inline int tea_ring_buffer_init(tea_ring_buffer_t *rb, size_t capacity_samples)
{
	memset(rb, 0, sizeof(*rb));
	rb->capacity = capacity_samples;
	rb->buf = (float *)calloc(capacity_samples, sizeof(float));
	pthread_mutex_init(&rb->lock, NULL);
	return rb->buf ? 0 : -1;
}

static inline void tea_ring_buffer_free(tea_ring_buffer_t *rb)
{
	pthread_mutex_destroy(&rb->lock);
	free(rb->buf);
	rb->buf = NULL;
}

/* Must hold rb->lock. Writes `count` samples (already guaranteed <=
 * capacity by the caller), dropping the oldest buffered samples first if
 * there isn't enough free space. */
static inline void tea_ring_buffer_write_locked(tea_ring_buffer_t *rb, const float *samples, size_t count)
{
	size_t free_space = rb->capacity - rb->fill;
	if (count > free_space) {
		size_t overflow = count - free_space;
		rb->read_pos = (rb->read_pos + overflow) % rb->capacity;
		rb->fill -= overflow;
		rb->dropped_samples += overflow;
	}

	for (size_t i = 0; i < count; i++)
		rb->buf[(rb->write_pos + i) % rb->capacity] = samples[i];
	rb->write_pos = (rb->write_pos + count) % rb->capacity;
	rb->fill += count;
}

/* Producer side. Real-time safe: never blocks. If `count` is larger than
 * the whole buffer it is clamped (keeping only the newest samples) so a
 * single pathological call can never write out of bounds. */
static inline void tea_ring_buffer_write(tea_ring_buffer_t *rb, const float *samples, size_t count)
{
	if (count > rb->capacity) {
		samples += (count - rb->capacity);
		count = rb->capacity;
	}

	if (pthread_mutex_trylock(&rb->lock) != 0) {
		/* Lock contended: drop rather than wait. Only the producer
		 * thread ever touches pending_contention_drops, so this is
		 * safe without synchronization. */
		rb->pending_contention_drops += count;
		return;
	}

	if (rb->pending_contention_drops) {
		rb->dropped_samples += rb->pending_contention_drops;
		rb->pending_contention_drops = 0;
	}

	tea_ring_buffer_write_locked(rb, samples, count);
	pthread_mutex_unlock(&rb->lock);
}

/* Producer side helper for the `muted` case: push `count` zero samples so
 * the sample clock the server sees stays continuous even while muted. */
static inline void tea_ring_buffer_write_silence(tea_ring_buffer_t *rb, size_t count)
{
	static const float k_silence_chunk[256] = {0};
	while (count > 0) {
		size_t n = count < 256 ? count : 256;
		tea_ring_buffer_write(rb, k_silence_chunk, n);
		count -= n;
	}
}

/* Consumer side. Reads up to `max_count` samples into `out`, returns how
 * many were actually read (may be less than max_count if not enough data
 * has been produced yet). Not safe to call from more than one consumer
 * thread at a time. May briefly block on the lock, which is fine off the
 * realtime audio thread (the producer only ever holds it for a memcpy). */
static inline size_t tea_ring_buffer_read(tea_ring_buffer_t *rb, float *out, size_t max_count)
{
	pthread_mutex_lock(&rb->lock);

	size_t to_read = rb->fill < max_count ? rb->fill : max_count;
	for (size_t i = 0; i < to_read; i++)
		out[i] = rb->buf[(rb->read_pos + i) % rb->capacity];
	rb->read_pos = (rb->read_pos + to_read) % rb->capacity;
	rb->fill -= to_read;

	pthread_mutex_unlock(&rb->lock);
	return to_read;
}

static inline uint64_t tea_ring_buffer_dropped_samples(tea_ring_buffer_t *rb)
{
	pthread_mutex_lock(&rb->lock);
	uint64_t v = rb->dropped_samples;
	pthread_mutex_unlock(&rb->lock);
	return v;
}
