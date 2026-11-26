/* SPDX-License-Identifier: GPL-2.0-or-later
 * Format mapping between FFmpeg and MTL, plus the sample conversions.
 *
 * The whole pipeline is built around one pixel format: yuv422p10le.
 * AV_PIX_FMT_YUV422P10LE and ST_FRAME_FMT_YUV422PLANAR10LE have an identical
 * memory layout, so decoded frames go to the wire (and RX frames go to the
 * encoder) without any pixel conversion at all. Never introduce one here --
 * at UHD it costs more than the decode.
 */
#pragma once

#include <mtl/mtl_api.h>
#include <mtl/st30_api.h>
#include <mtl/st_pipeline_api.h>

#include <cstdint>
#include <string>

namespace vcard {

bool parse_ip(const std::string &str, uint8_t out[MTL_IP_ADDR_LEN]);
std::string ip_to_string(const uint8_t ip[MTL_IP_ADDR_LEN]);
bool is_multicast(const std::string &ip);

enum st_fps rational_to_mtl_fps(uint32_t num, uint32_t den);
void mtl_fps_to_rational(enum st_fps fps, uint32_t *num, uint32_t *den);
double mtl_fps_to_double(enum st_fps fps);
const char *mtl_fps_name(enum st_fps fps);
/* Nanoseconds per frame, exact for the /1001 rates. */
uint64_t mtl_fps_frame_ns(enum st_fps fps);

enum st30_ptime parse_ptime(const std::string &s);
const char *ptime_name(enum st30_ptime p);
uint32_t ptime_us(enum st30_ptime p);

/* --- audio sample conversion ----------------------------------------------
 * ST 2110-30 carries L24 big-endian, channel-interleaved. FFmpeg gives us
 * S32 interleaved (AV_SAMPLE_FMT_S32); the top 24 bits carry the sample.
 * "total_samples" counts individual samples, i.e. frames * channels.
 */
void s32_to_l24be(const int32_t *src, uint8_t *dst, size_t total_samples);
void l24be_to_s32(const uint8_t *src, int32_t *dst, size_t total_samples);

/* Bytes on the wire for one second of audio. */
size_t audio_bytes_per_second(uint16_t channels);

/* --- RX timestamp mapping --------------------------------------------------
 * Maps an MTL RX frame timestamp (PTP/TAI) onto CLOCK_MONOTONIC through one
 * process-wide offset, so video and audio of every receiver keep their
 * on-wire alignment (lip sync, inter-source sync). The absolute offset only
 * shifts overall latency.
 */
uint64_t rx_timestamp_to_monotonic(enum st10_timestamp_fmt tfmt, uint64_t timestamp,
				   uint64_t receive_timestamp);

uint64_t monotonic_ns();

/* Sampling instant of an RX frame in TAI nanoseconds.
 *
 * MTL hands RX frames over with the 32-bit RTP timestamp (media clock: 90 kHz
 * for video, the sample rate for audio) plus the PTP time at which the first
 * packet arrived. The RTP value alone wraps every few hours and at a different
 * period for each essence, so it is anchored here to the arrival time: the
 * result is the point on the PTP timeline the sender stamped, which is the one
 * thing video and audio of a ST 2110 source genuinely share. Returns 0 when
 * the frame carries nothing usable. */
uint64_t rx_sample_time_ns(enum st10_timestamp_fmt tfmt, uint64_t timestamp,
			   uint64_t receive_timestamp, uint32_t clock_rate);

} // namespace vcard
