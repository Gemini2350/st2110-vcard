/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/formats.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace vcard {

bool parse_ip(const std::string &str, uint8_t out[MTL_IP_ADDR_LEN])
{
	struct in_addr addr;
	if (inet_pton(AF_INET, str.c_str(), &addr) != 1)
		return false;
	memcpy(out, &addr.s_addr, MTL_IP_ADDR_LEN);
	return true;
}

std::string ip_to_string(const uint8_t ip[MTL_IP_ADDR_LEN])
{
	char buf[INET_ADDRSTRLEN] = {0};
	inet_ntop(AF_INET, ip, buf, sizeof(buf));
	return buf;
}

bool is_multicast(const std::string &ip)
{
	uint8_t a[MTL_IP_ADDR_LEN];
	if (!parse_ip(ip, a))
		return false;
	return a[0] >= 224 && a[0] <= 239;
}

enum st_fps rational_to_mtl_fps(uint32_t num, uint32_t den)
{
	double fps = den ? (double)num / den : 0.0;
	if (fps > 23.9 && fps < 23.99) return ST_FPS_P23_98;
	if (fps > 23.99 && fps < 24.1) return ST_FPS_P24;
	if (fps > 24.9 && fps < 25.1) return ST_FPS_P25;
	if (fps > 29.9 && fps < 29.98) return ST_FPS_P29_97;
	if (fps > 29.98 && fps < 30.1) return ST_FPS_P30;
	if (fps > 49.9 && fps < 50.1) return ST_FPS_P50;
	if (fps > 59.9 && fps < 59.95) return ST_FPS_P59_94;
	if (fps > 59.95 && fps < 60.1) return ST_FPS_P60;
	if (fps > 99.9 && fps < 100.1) return ST_FPS_P100;
	if (fps > 119.8 && fps < 119.9) return ST_FPS_P119_88;
	if (fps > 119.9 && fps < 120.1) return ST_FPS_P120;
	return ST_FPS_MAX;
}

void mtl_fps_to_rational(enum st_fps fps, uint32_t *num, uint32_t *den)
{
	switch (fps) {
	case ST_FPS_P23_98: *num = 24000; *den = 1001; break;
	case ST_FPS_P24: *num = 24; *den = 1; break;
	case ST_FPS_P25: *num = 25; *den = 1; break;
	case ST_FPS_P29_97: *num = 30000; *den = 1001; break;
	case ST_FPS_P30: *num = 30; *den = 1; break;
	case ST_FPS_P50: *num = 50; *den = 1; break;
	case ST_FPS_P59_94: *num = 60000; *den = 1001; break;
	case ST_FPS_P60: *num = 60; *den = 1; break;
	case ST_FPS_P100: *num = 100; *den = 1; break;
	case ST_FPS_P119_88: *num = 120000; *den = 1001; break;
	case ST_FPS_P120: *num = 120; *den = 1; break;
	default: *num = 0; *den = 1; break;
	}
}

double mtl_fps_to_double(enum st_fps fps)
{
	uint32_t n, d;
	mtl_fps_to_rational(fps, &n, &d);
	return d ? (double)n / d : 0.0;
}

const char *mtl_fps_name(enum st_fps fps)
{
	switch (fps) {
	case ST_FPS_P23_98: return "23.98";
	case ST_FPS_P24: return "24";
	case ST_FPS_P25: return "25";
	case ST_FPS_P29_97: return "29.97";
	case ST_FPS_P30: return "30";
	case ST_FPS_P50: return "50";
	case ST_FPS_P59_94: return "59.94";
	case ST_FPS_P60: return "60";
	case ST_FPS_P100: return "100";
	case ST_FPS_P119_88: return "119.88";
	case ST_FPS_P120: return "120";
	default: return "?";
	}
}

uint64_t mtl_fps_frame_ns(enum st_fps fps)
{
	uint32_t n, d;
	mtl_fps_to_rational(fps, &n, &d);
	if (!n)
		return 0;
	return (uint64_t)1000000000ull * d / n;
}

enum st30_ptime parse_ptime(const std::string &s)
{
	if (s == "125us" || s == "125") return ST30_PTIME_125US;
	if (s == "250us" || s == "250") return ST30_PTIME_250US;
	if (s == "333us" || s == "333") return ST30_PTIME_333US;
	if (s == "4ms") return ST30_PTIME_4MS;
	return ST30_PTIME_1MS;
}

const char *ptime_name(enum st30_ptime p)
{
	switch (p) {
	case ST30_PTIME_125US: return "125us";
	case ST30_PTIME_250US: return "250us";
	case ST30_PTIME_333US: return "333us";
	case ST30_PTIME_4MS: return "4ms";
	default: return "1ms";
	}
}

uint32_t ptime_us(enum st30_ptime p)
{
	switch (p) {
	case ST30_PTIME_125US: return 125;
	case ST30_PTIME_250US: return 250;
	case ST30_PTIME_333US: return 333;
	case ST30_PTIME_4MS: return 4000;
	default: return 1000;
	}
}

void s32_to_l24be(const int32_t *src, uint8_t *dst, size_t total_samples)
{
	for (size_t i = 0; i < total_samples; i++) {
		uint32_t v = (uint32_t)src[i];
		dst[3 * i + 0] = (uint8_t)(v >> 24);
		dst[3 * i + 1] = (uint8_t)(v >> 16);
		dst[3 * i + 2] = (uint8_t)(v >> 8);
	}
}

void l24be_to_s32(const uint8_t *src, int32_t *dst, size_t total_samples)
{
	for (size_t i = 0; i < total_samples; i++) {
		uint32_t v = ((uint32_t)src[3 * i + 0] << 24) |
			     ((uint32_t)src[3 * i + 1] << 16) |
			     ((uint32_t)src[3 * i + 2] << 8);
		dst[i] = (int32_t)v;
	}
}

size_t audio_bytes_per_second(uint16_t channels)
{
	return (size_t)48000 * channels * 3;
}

uint64_t monotonic_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

uint64_t rx_timestamp_to_monotonic(enum st10_timestamp_fmt tfmt, uint64_t timestamp,
				   uint64_t receive_timestamp)
{
	/* prefer the media timestamp (sampling epoch, common PTP timebase for
	 * video and audio); fall back to the packet arrival TAI time */
	uint64_t tai = 0;
	if (tfmt == ST10_TIMESTAMP_FMT_TAI && timestamp)
		tai = timestamp;
	else if (receive_timestamp)
		tai = receive_timestamp;

	uint64_t now = monotonic_ns();
	if (!tai)
		return now;

	static std::atomic<int64_t> offset_ns{0};
	static std::atomic<bool> valid{false};

	int64_t candidate = (int64_t)now - (int64_t)tai;
	int64_t cur = offset_ns.load(std::memory_order_relaxed);

	if (!valid.load(std::memory_order_relaxed) ||
	    llabs(candidate - cur) > 500000000ll /* 500 ms: clock step or PTP relock */) {
		offset_ns.store(candidate, std::memory_order_relaxed);
		valid.store(true, std::memory_order_relaxed);
		cur = candidate;
	} else {
		/* alpha = 1/1024; converges in a few seconds, then drifts with
		 * the (phc2sys-disciplined) clocks */
		cur += (candidate - cur) / 1024;
		offset_ns.store(cur, std::memory_order_relaxed);
	}

	return (uint64_t)((int64_t)tai + cur);
}

} // namespace vcard
