/* SPDX-License-Identifier: GPL-2.0-or-later
 * Per-channel peak meters over the L24BE audio that goes to (or comes off)
 * the wire. Peak with a decay, plus a sticky over indicator -- enough to see
 * at a glance that channel 7 is silent or that something is clipping, which is
 * what you actually want from a confidence monitor.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace vcard {

constexpr size_t kMaxMeterChannels = 16;

class AudioMeters {
public:
	void configure(uint16_t channels);

	/* Called from the tx/rx audio thread with interleaved L24BE. */
	void feed(const uint8_t *l24be, size_t bytes, uint16_t channels);

	struct Levels {
		uint16_t channels = 0;
		std::array<float, kMaxMeterChannels> peak_dbfs{}; /* -inf as -99 */
		std::array<bool, kMaxMeterChannels> over{};
	};
	Levels levels() const;
	void clear_overs();

private:
	std::atomic<uint16_t> channels_{0};
	mutable std::array<std::atomic<float>, kMaxMeterChannels> peak_{};
	mutable std::array<std::atomic<bool>, kMaxMeterChannels> over_{};
	std::atomic<uint64_t> last_feed_ns_{0};
};

} // namespace vcard
