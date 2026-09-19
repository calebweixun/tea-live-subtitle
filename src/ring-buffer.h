#pragma once

/*
 * Single-producer / single-consumer lock-free ring buffer of mono float
 * samples.
 *
 * Producer (the OBS audio capture callback, on OBS's audio thread) only
 * ever writes `write_index`. It never blocks, never allocates and never
 * touches `read_index`. When the consumer falls behind, old samples are
 * simply overwritten in place -- this is what gives us "drop oldest on
 * overflow" without the producer ever needing to coordinate with the
 * consumer.
 *
 * Consumer (the resample/network worker thread) only ever writes
 * `read_index`. On each pull it compares `write_index - read_index` against
 * capacity; if the producer has lapped it, it fast-forwards `read_index` to
 * `write_index - capacity` and accounts the skipped samples in
 * `dropped_samples` so the settings dialog can surface a non-zero drop
 * counter instead of silently losing audio.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
	float *buf;           /* capacity samples, owned by this struct */
	size_t capacity;      /* power of two */
	size_t capacity_mask; /* capacity - 1 */

	_Atomic size_t write_index; /* total samples ever produced */
	_Atomic size_t read_index;  /* total samples ever consumed */

	_Atomic uint_fast64_t dropped_samples;
} tea_ring_buffer_t;

static inline size_t tea_ring_buffer_round_pow2(size_t v)
{
	size_t p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

/* Not real-time safe; call during source create/destroy only. */
static inline int tea_ring_buffer_init(tea_ring_buffer_t *rb, size_t min_capacity_samples)
{
	rb->capacity = tea_ring_buffer_round_pow2(min_capacity_samples);
	rb->capacity_mask = rb->capacity - 1;
	rb->buf = (float *)calloc(rb->capacity, sizeof(float));
	atomic_init(&rb->write_index, 0);
	atomic_init(&rb->read_index, 0);
	atomic_init(&rb->dropped_samples, 0);
	return rb->buf ? 0 : -1;
}

static inline void tea_ring_buffer_free(tea_ring_buffer_t *rb)
{
	free(rb->buf);
	rb->buf = NULL;
}

/* Producer side. Real-time safe: no locks, no allocation, no syscalls. */
static inline void tea_ring_buffer_write(tea_ring_buffer_t *rb, const float *samples, size_t count)
{
	size_t w = atomic_load_explicit(&rb->write_index, memory_order_relaxed);
	for (size_t i = 0; i < count; i++)
		rb->buf[(w + i) & rb->capacity_mask] = samples[i];
	atomic_store_explicit(&rb->write_index, w + count, memory_order_release);
}

/* Producer side helper for the `muted` case: push `count` zero samples so
 * the sample clock the server sees stays continuous even while muted. */
static inline void tea_ring_buffer_write_silence(tea_ring_buffer_t *rb, size_t count)
{
	size_t w = atomic_load_explicit(&rb->write_index, memory_order_relaxed);
	for (size_t i = 0; i < count; i++)
		rb->buf[(w + i) & rb->capacity_mask] = 0.0f;
	atomic_store_explicit(&rb->write_index, w + count, memory_order_release);
}

/* Consumer side. Reads up to `max_count` samples into `out`, returns how
 * many were actually read (may be less than max_count if not enough data
 * has been produced yet). Not safe to call from more than one consumer
 * thread at a time. */
static inline size_t tea_ring_buffer_read(tea_ring_buffer_t *rb, float *out, size_t max_count)
{
	size_t w = atomic_load_explicit(&rb->write_index, memory_order_acquire);
	size_t r = atomic_load_explicit(&rb->read_index, memory_order_relaxed);

	size_t available = w - r;
	if (available > rb->capacity) {
		/* Producer lapped us: the oldest (available - capacity) samples
		 * we thought were still unread have already been overwritten. */
		size_t skipped = available - rb->capacity;
		atomic_fetch_add_explicit(&rb->dropped_samples, skipped, memory_order_relaxed);
		r = w - rb->capacity;
		available = rb->capacity;
	}

	size_t to_read = available < max_count ? available : max_count;
	for (size_t i = 0; i < to_read; i++)
		out[i] = rb->buf[(r + i) & rb->capacity_mask];

	atomic_store_explicit(&rb->read_index, r + to_read, memory_order_relaxed);
	return to_read;
}

static inline uint64_t tea_ring_buffer_dropped_samples(const tea_ring_buffer_t *rb)
{
	return (uint64_t)atomic_load_explicit((_Atomic uint_fast64_t *)&rb->dropped_samples, memory_order_relaxed);
}
