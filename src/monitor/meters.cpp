/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "monitor/meters.h"

#include "core/formats.h"

#include <algorithm>
#include <cmath>

namespace vcard {

namespace {
constexpr float kSilenceDb = -99.0f;
/* 20 dB per second, applied per feed call */
constexpr float kDecayDbPerSecond = 20.0f;
} // namespace

void AudioMeters::configure(uint16_t channels)
{
	channels_ = std::min<uint16_t>(channels, kMaxMeterChannels);
	for (size_t i = 0; i < kMaxMeterChannels; i++) {
		peak_[i].store(kSilenceDb, std::memory_order_relaxed);
		over_[i].store(false, std::memory_order_relaxed);
	}
}

void AudioMeters::feed(const uint8_t *l24be, size_t bytes, uint16_t channels)
{
	uint16_t ch = std::min<uint16_t>(channels, kMaxMeterChannels);
	if (!ch || bytes < (size_t)ch * 3)
		return;

	size_t frames = bytes / (3 * (size_t)channels);
	float block_peak[kMaxMeterChannels] = {0};

	for (size_t f = 0; f < frames; f++) {
		const uint8_t *p = l24be + f * 3 * channels;
		for (uint16_t c = 0; c < ch; c++) {
			int32_t v = ((int32_t)(int8_t)p[c * 3] << 16) |
				    ((int32_t)p[c * 3 + 1] << 8) |
				    (int32_t)p[c * 3 + 2];
			float a = std::fabs((float)v) / 8388608.0f; /* 2^23 */
			if (a > block_peak[c])
				block_peak[c] = a;
		}
	}

	uint64_t now = monotonic_ns();
	uint64_t last = last_feed_ns_.exchange(now);
	float elapsed_s = last ? (float)(now - last) / 1e9f : 0.0f;
	float decay = kDecayDbPerSecond * std::min(elapsed_s, 1.0f);

	for (uint16_t c = 0; c < ch; c++) {
		float db = block_peak[c] > 0.0f
				   ? 20.0f * std::log10(block_peak[c])
				   : kSilenceDb;
		db = std::max(db, kSilenceDb);
		float prev = peak_[c].load(std::memory_order_relaxed) - decay;
		peak_[c].store(std::max(db, std::max(prev, kSilenceDb)),
			       std::memory_order_relaxed);
		if (block_peak[c] >= 0.999f)
			over_[c].store(true, std::memory_order_relaxed);
	}
}

AudioMeters::Levels AudioMeters::levels() const
{
	Levels l;
	l.channels = channels_.load(std::memory_order_relaxed);
	for (size_t i = 0; i < kMaxMeterChannels; i++) {
		l.peak_dbfs[i] = peak_[i].load(std::memory_order_relaxed);
		l.over[i] = over_[i].load(std::memory_order_relaxed);
	}
	return l;
}

void AudioMeters::clear_overs()
{
	for (size_t i = 0; i < kMaxMeterChannels; i++)
		over_[i].store(false, std::memory_order_relaxed);
}

} // namespace vcard
