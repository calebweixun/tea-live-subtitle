/*
tea-live-subtitle
Copyright (C) 2026 Caleb Weixun

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "audio-tap.h"
#include "ring-buffer.h"

#include <obs.h>
#include <media-io/audio-resampler.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <string.h>

#include "plugin-support.h"

/* ~1s of mono float audio at a generous 96kHz native rate is plenty of
 * headroom between the audio callback and the worker thread pulling it. */
#define TEA_RING_CAPACITY_SAMPLES (96000)

/* Downmix scratch chunk size for a single pull; 4096 samples at typical
 * native rates (44.1/48/96 kHz) is well under 100ms, so latency added by
 * batching stays small. */
#define TEA_PULL_CHUNK_SAMPLES (4096)

struct tea_audio_tap {
	pthread_mutex_t attach_lock; /* guards source/callback attach-detach only */
	obs_source_t *attached_source;

	tea_ring_buffer_t ring;

	/* Native format the audio callback is delivering, latched the first
	 * time the callback fires after (re)attaching. 0 == not known yet. */
	uint32_t native_rate;
	uint32_t native_channels;

	audio_resampler_t *resampler; /* created lazily once native_rate is known */
	float pull_scratch[TEA_PULL_CHUNK_SAMPLES];
};

static void tea_on_audio_capture(void *param, obs_source_t *src, const struct audio_data *data, bool muted)
{
	(void)src;
	tea_audio_tap_t *tap = param;

	if (tap->native_rate == 0) {
		struct obs_audio_info oai;
		if (obs_get_audio_info(&oai)) {
			tap->native_rate = oai.samples_per_sec;
			tap->native_channels = (uint32_t)get_audio_channels(oai.speakers);
		} else {
			/* Shouldn't happen once obs core audio is up, but never
			 * divide by zero below if it somehow does. */
			tap->native_rate = 48000;
			tap->native_channels = 2;
		}
	}

	uint32_t channels = tap->native_channels ? tap->native_channels : 1;
	uint32_t frames = data->frames;

	if (muted) {
		/* Still advance the sample clock: the server's VAD relies on a
		 * continuous timeline, a gap here would desync it. */
		tea_ring_buffer_write_silence(&tap->ring, frames);
		return;
	}

	/* OBS delivers planar float: data->data[c] is a contiguous float
	 * array for channel c. Downmix to mono with plain arithmetic --
	 * cheap, no allocation, no locks. */
	const float *planes[MAX_AV_PLANES];
	uint32_t usable_channels = 0;
	for (uint32_t c = 0; c < channels && c < MAX_AV_PLANES; c++) {
		if (!data->data[c])
			break;
		planes[c] = (const float *)data->data[c];
		usable_channels++;
	}
	if (usable_channels == 0) {
		tea_ring_buffer_write_silence(&tap->ring, frames);
		return;
	}

	/* Downmix directly into fixed-size stack chunks so we never need a
	 * heap buffer sized to `frames` on the audio thread. */
	float mono_chunk[512];
	uint32_t offset = 0;
	while (offset < frames) {
		uint32_t n = frames - offset;
		if (n > 512)
			n = 512;
		for (uint32_t i = 0; i < n; i++) {
			float sum = 0.0f;
			for (uint32_t c = 0; c < usable_channels; c++)
				sum += planes[c][offset + i];
			mono_chunk[i] = sum / (float)usable_channels;
		}
		tea_ring_buffer_write(&tap->ring, mono_chunk, n);
		offset += n;
	}
}

tea_audio_tap_t *tea_audio_tap_create(void)
{
	tea_audio_tap_t *tap = bzalloc(sizeof(tea_audio_tap_t));
	pthread_mutex_init(&tap->attach_lock, NULL);
	if (tea_ring_buffer_init(&tap->ring, TEA_RING_CAPACITY_SAMPLES) != 0) {
		obs_log(LOG_ERROR, "audio-tap: failed to allocate ring buffer");
	}
	return tap;
}

void tea_audio_tap_destroy(tea_audio_tap_t *tap)
{
	if (!tap)
		return;
	tea_audio_tap_set_source(tap, NULL);
	if (tap->resampler)
		audio_resampler_destroy(tap->resampler);
	tea_ring_buffer_free(&tap->ring);
	pthread_mutex_destroy(&tap->attach_lock);
	bfree(tap);
}

void tea_audio_tap_set_source(tea_audio_tap_t *tap, obs_source_t *source)
{
	pthread_mutex_lock(&tap->attach_lock);
	if (tap->attached_source) {
		obs_source_remove_audio_capture_callback(tap->attached_source, tea_on_audio_capture, tap);
		obs_source_release(tap->attached_source);
		tap->attached_source = NULL;
	}
	if (source) {
		tap->attached_source = obs_source_get_ref(source);
		if (tap->attached_source)
			obs_source_add_audio_capture_callback(tap->attached_source, tea_on_audio_capture, tap);
	}
	pthread_mutex_unlock(&tap->attach_lock);
}

bool tea_audio_tap_has_source(tea_audio_tap_t *tap)
{
	if (!tap)
		return false;
	pthread_mutex_lock(&tap->attach_lock);
	bool attached = tap->attached_source != NULL;
	pthread_mutex_unlock(&tap->attach_lock);
	return attached;
}

size_t tea_audio_tap_pull_pcm16(tea_audio_tap_t *tap, int16_t *out, size_t max_samples)
{
	if (tap->native_rate == 0)
		return 0; /* haven't seen a single audio callback yet */

	size_t want = TEA_PULL_CHUNK_SAMPLES;
	size_t got = tea_ring_buffer_read(&tap->ring, tap->pull_scratch, want);
	if (got == 0)
		return 0;

	if (!tap->resampler) {
		struct resample_info src = {
			.samples_per_sec = tap->native_rate,
			.format = AUDIO_FORMAT_FLOAT_PLANAR,
			.speakers = SPEAKERS_MONO,
		};
		struct resample_info dst = {
			.samples_per_sec = 16000,
			.format = AUDIO_FORMAT_16BIT,
			.speakers = SPEAKERS_MONO,
		};
		tap->resampler = audio_resampler_create(&dst, &src);
		if (!tap->resampler) {
			obs_log(LOG_ERROR, "audio-tap: audio_resampler_create failed");
			return 0;
		}
	}

	uint8_t *output[MAX_AV_PLANES] = {0};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	const uint8_t *input[MAX_AV_PLANES] = {0};
	input[0] = (const uint8_t *)tap->pull_scratch;

	bool ok = audio_resampler_resample(tap->resampler, output, &out_frames, &ts_offset, input, (uint32_t)got);
	if (!ok || !output[0] || out_frames == 0)
		return 0;

	size_t n = out_frames;
	if (n > max_samples)
		n = max_samples;
	memcpy(out, output[0], n * sizeof(int16_t));
	return n;
}

uint64_t tea_audio_tap_dropped_samples(tea_audio_tap_t *tap)
{
	return tea_ring_buffer_dropped_samples(&tap->ring);
}
