/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "monitor/preview.h"

#include "core/formats.h"
#include "core/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <pthread.h>

namespace vcard {

Preview::Preview() = default;

Preview::~Preview() { stop(); }

void Preview::configure(uint32_t target_width, double target_fps)
{
	target_width_ = std::max<uint32_t>(160, target_width);
	target_fps_ = target_fps > 0 ? target_fps : 10.0;
}

void Preview::start()
{
	if (running_)
		return;
	running_ = true;
	thread_ = std::thread(&Preview::run, this);
}

void Preview::stop()
{
	if (!running_ && !thread_.joinable())
		return;
	running_ = false;
	slot_cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void Preview::submit(const uint8_t *const planes[3], const size_t strides[3],
		     uint32_t width, uint32_t height)
{
	if (!enabled_ || !running_ || !width || !height)
		return;

	/* rate limit before doing any work at all */
	uint64_t now = monotonic_ns();
	uint64_t min_gap = (uint64_t)(1e9 / target_fps_);
	if (last_submit_ns_ && now - last_submit_ns_ < min_gap)
		return;
	last_submit_ns_ = now;

	/* Integer decimation factor. Luma and chroma use the same vertical
	 * step; horizontally chroma is already half rate in 4:2:2. */
	uint32_t step = std::max<uint32_t>(1, width / target_width_);
	uint32_t ow = width / step;
	uint32_t oh = height / step;
	uint32_t ocw = ow / 2;

	std::unique_lock<std::mutex> lock(slot_mutex_);
	slot_y_.resize((size_t)ow * oh);
	slot_u_.resize((size_t)ocw * oh);
	slot_v_.resize((size_t)ocw * oh);

	for (uint32_t y = 0; y < oh; y++) {
		const uint16_t *sy =
			(const uint16_t *)(planes[0] + (size_t)y * step * strides[0]);
		uint8_t *dy = slot_y_.data() + (size_t)y * ow;
		for (uint32_t x = 0; x < ow; x++)
			dy[x] = (uint8_t)(sy[x * step] >> 2); /* 10 -> 8 bit */

		const uint16_t *su =
			(const uint16_t *)(planes[1] + (size_t)y * step * strides[1]);
		const uint16_t *sv =
			(const uint16_t *)(planes[2] + (size_t)y * step * strides[2]);
		uint8_t *du = slot_u_.data() + (size_t)y * ocw;
		uint8_t *dv = slot_v_.data() + (size_t)y * ocw;
		for (uint32_t x = 0; x < ocw; x++) {
			du[x] = (uint8_t)(su[x * step] >> 2);
			dv[x] = (uint8_t)(sv[x * step] >> 2);
		}
	}
	slot_w_ = ow;
	slot_h_ = oh;
	slot_full_ = true;
	lock.unlock();
	slot_cv_.notify_one();
}

void Preview::run()
{
	pthread_setname_np(pthread_self(), "vcard-preview");
	std::vector<uint8_t> y, u, v;
	uint32_t w = 0, h = 0;

	while (running_) {
		{
			std::unique_lock<std::mutex> lock(slot_mutex_);
			slot_cv_.wait(lock, [&] { return !running_ || slot_full_; });
			if (!running_)
				break;
			y.swap(slot_y_);
			u.swap(slot_u_);
			v.swap(slot_v_);
			w = slot_w_;
			h = slot_h_;
			slot_full_ = false;
		}
		encode(y, u, v, w, h);
	}

	if (sws_) sws_freeContext(sws_);
	if (enc_) avcodec_free_context(&enc_);
	if (frame_) av_frame_free(&frame_);
	if (pkt_) av_packet_free(&pkt_);
	sws_ = nullptr;
	enc_ = nullptr;
	frame_ = nullptr;
	pkt_ = nullptr;
}

bool Preview::encode(const std::vector<uint8_t> &y, const std::vector<uint8_t> &u,
		     const std::vector<uint8_t> &v, uint32_t w, uint32_t h)
{
	if (!w || !h)
		return false;

	/* MJPEG wants even dimensions for 4:2:0 subsampling */
	uint32_t ew = w & ~1u, eh = h & ~1u;
	if (!enc_ || ew != enc_w_ || eh != enc_h_) {
		if (enc_) avcodec_free_context(&enc_);
		if (frame_) av_frame_free(&frame_);
		if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }

		const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
		if (!codec) {
			VLOG_E("[preview] no mjpeg encoder in this ffmpeg build");
			enabled_ = false;
			return false;
		}
		enc_ = avcodec_alloc_context3(codec);
		enc_->width = ew;
		enc_->height = eh;
		enc_->pix_fmt = AV_PIX_FMT_YUVJ420P;
		enc_->time_base = AVRational{1, 25};
		enc_->color_range = AVCOL_RANGE_JPEG;
		enc_->flags |= AV_CODEC_FLAG_QSCALE;
		enc_->global_quality = FF_QP2LAMBDA * 6;
		if (avcodec_open2(enc_, codec, nullptr) < 0) {
			VLOG_E("[preview] mjpeg encoder open failed");
			avcodec_free_context(&enc_);
			enabled_ = false;
			return false;
		}
		frame_ = av_frame_alloc();
		frame_->format = enc_->pix_fmt;
		frame_->width = ew;
		frame_->height = eh;
		av_frame_get_buffer(frame_, 0);
		enc_w_ = ew;
		enc_h_ = eh;
	}
	if (!pkt_)
		pkt_ = av_packet_alloc();

	/* yuv422p8 (our decimated buffer) -> yuvj420p for the JPEG */
	sws_ = sws_getCachedContext(sws_, w, h, AV_PIX_FMT_YUV422P, ew, eh,
				    AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr,
				    nullptr, nullptr);
	if (!sws_)
		return false;

	const uint8_t *src[3] = {y.data(), u.data(), v.data()};
	int src_stride[3] = {(int)w, (int)(w / 2), (int)(w / 2)};
	av_frame_make_writable(frame_);
	sws_scale(sws_, src, src_stride, 0, h, frame_->data, frame_->linesize);
	frame_->quality = enc_->global_quality;
	frame_->pts = (int64_t)generation_.load();

	if (avcodec_send_frame(enc_, frame_) != 0)
		return false;
	while (avcodec_receive_packet(enc_, pkt_) == 0) {
		{
			std::lock_guard<std::mutex> lock(jpeg_mutex_);
			jpeg_.assign(pkt_->data, pkt_->data + pkt_->size);
		}
		generation_++;
		av_packet_unref(pkt_);
	}
	return true;
}

std::vector<uint8_t> Preview::latest_jpeg() const
{
	std::lock_guard<std::mutex> lock(jpeg_mutex_);
	return jpeg_;
}

} // namespace vcard
