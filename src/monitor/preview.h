/* SPDX-License-Identifier: GPL-2.0-or-later
 * Confidence monitoring: a small MJPEG view of what a session is actually
 * sending or receiving, for the web GUI.
 *
 * This must never slow down the wire. Two rules follow from that:
 *   - submit() is called on the tx/rx thread and only does a decimating copy
 *     into a small 8-bit buffer (reading every Nth pixel, so a UHD frame costs
 *     a fraction of a full pass), then hands off.
 *   - the JPEG encode happens on the preview's own thread, and frames are
 *     dropped rather than queued: a monitor that lags is useless anyway.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SwsContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace vcard {

class Preview {
public:
	Preview();
	~Preview();

	/* Target width of the preview image; height follows the source aspect. */
	void configure(uint32_t target_width, double target_fps);
	void start();
	void stop();

	/* Called from the tx/rx thread with a yuv422p10le picture. Cheap. */
	void submit(const uint8_t *const planes[3], const size_t strides[3],
		    uint32_t width, uint32_t height);

	/* Latest encoded JPEG, empty if none yet. */
	std::vector<uint8_t> latest_jpeg() const;
	uint64_t generation() const { return generation_; }
	bool enabled() const { return enabled_; }
	void set_enabled(bool on) { enabled_ = on; }

private:
	void run();
	bool encode(const std::vector<uint8_t> &y, const std::vector<uint8_t> &u,
		    const std::vector<uint8_t> &v, uint32_t w, uint32_t h);

	uint32_t target_width_ = 480;
	double target_fps_ = 10.0;
	std::atomic<bool> enabled_{true};

	/* single-slot mailbox: newest wins */
	mutable std::mutex slot_mutex_;
	std::condition_variable slot_cv_;
	std::vector<uint8_t> slot_y_, slot_u_, slot_v_;
	uint32_t slot_w_ = 0, slot_h_ = 0;
	bool slot_full_ = false;

	mutable std::mutex jpeg_mutex_;
	std::vector<uint8_t> jpeg_;
	std::atomic<uint64_t> generation_{0};

	uint64_t last_submit_ns_ = 0;

	std::thread thread_;
	std::atomic<bool> running_{false};

	SwsContext *sws_ = nullptr;
	AVCodecContext *enc_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *pkt_ = nullptr;
	uint32_t enc_w_ = 0, enc_h_ = 0;
};

} // namespace vcard
