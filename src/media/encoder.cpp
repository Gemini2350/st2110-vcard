/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "media/encoder.h"

#include "core/formats.h"
#include "core/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <future>
#include <pthread.h>
#include <vector>

namespace vcard {

namespace {

std::string av_err(int err)
{
	char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

struct CodecPlan {
	const char *encoder;
	AVPixelFormat pix_fmt;
	const char *format;    /* libavformat muxer name */
	const char *extension; /* what the file is called; not always the same */
};

CodecPlan plan_for(RecordCodec c)
{
	switch (c) {
	case RecordCodec::ProRes4444:
		return {"prores_ks", AV_PIX_FMT_YUV444P10LE, "mov", "mov"};
	case RecordCodec::FFV1:
		return {"ffv1", AV_PIX_FMT_YUV422P10LE, "matroska", "mkv"};
	case RecordCodec::V210:
		return {"v210", AV_PIX_FMT_YUV422P10LE, "mov", "mov"};
	case RecordCodec::H264:
		/* mov, not mp4: the audio is PCM, which mp4 only learned recently */
		return {"libx264", AV_PIX_FMT_YUV420P, "mov", "mov"};
	default:
		return {"prores_ks", AV_PIX_FMT_YUV422P10LE, "mov", "mov"};
	}
}

} // namespace

RecordCodec parse_record_codec(const std::string &s)
{
	if (s == "prores4444") return RecordCodec::ProRes4444;
	if (s == "ffv1") return RecordCodec::FFV1;
	if (s == "v210" || s == "uncompressed") return RecordCodec::V210;
	if (s == "h264") return RecordCodec::H264;
	return RecordCodec::ProRes422HQ;
}

const char *record_codec_name(RecordCodec c)
{
	switch (c) {
	case RecordCodec::ProRes4444: return "prores4444";
	case RecordCodec::FFV1: return "ffv1";
	case RecordCodec::V210: return "v210";
	case RecordCodec::H264: return "h264";
	default: return "prores422hq";
	}
}

const char *record_codec_extension(RecordCodec c) { return plan_for(c).extension; }

FileEncoder::FileEncoder(BoundedQueue<VideoFrame> &vq, AudioFifo &afifo,
			 EncoderTarget target)
	: vq_(vq), afifo_(afifo), target_(target)
{
}

FileEncoder::~FileEncoder() { stop(); }

bool FileEncoder::start(const std::string &path)
{
	stop();
	path_ = path;
	last_error_.clear();
	frames_written_ = 0;
	bytes_written_ = 0;
	running_ = true;
	opened_ = std::promise<bool>();
	auto opened = opened_.get_future();
	thread_ = std::thread(&FileEncoder::run, this);

	/* Wait until the file is really open. Everything that can go wrong
	 * with a recording goes wrong here -- encoder missing from this ffmpeg
	 * build, directory not writable, disk full -- and "the REC light is on
	 * but nothing is being written" is the one failure a recorder must not
	 * have. */
	if (!opened.get()) {
		if (thread_.joinable())
			thread_.join();
		running_ = false;
		return false;
	}
	return true;
}

void FileEncoder::stop()
{
	if (!running_ && !thread_.joinable())
		return;
	running_ = false;
	vq_.stop();
	afifo_.stop();
	if (thread_.joinable())
		thread_.join();
	vq_.restart();
	afifo_.restart();
}

void FileEncoder::run()
{
	pthread_setname_np(pthread_self(), "vcard-enc");

	CodecPlan plan = plan_for(target_.codec);
	const char *container = target_.container.empty() ? plan.format
							  : target_.container.c_str();

	AVFormatContext *ofmt = nullptr;
	int err = avformat_alloc_output_context2(&ofmt, nullptr, container,
						 path_.c_str());
	if (err < 0 || !ofmt) {
		last_error_ = std::string("no muxer for '") + container +
			      "': " + av_err(err);
		VLOG_E("[enc] %s", last_error_.c_str());
		running_ = false;
		opened_.set_value(false);
		return;
	}

	AVCodecContext *venc = nullptr, *aenc = nullptr;
	AVStream *vst = nullptr, *ast = nullptr;
	SwsContext *sws = nullptr;
	AVFrame *vframe = nullptr, *aframe = nullptr;
	AVPacket *pkt = av_packet_alloc();
	std::vector<uint8_t> l24;
	std::vector<int32_t> s32;

	/* only ever called before the header is written, i.e. while start()
	 * is still waiting for the verdict */
	auto fail = [&](const std::string &msg) {
		last_error_ = msg;
		VLOG_E("[enc] %s", msg.c_str());
		opened_.set_value(false);
	};

	auto cleanup = [&]() {
		if (venc) avcodec_free_context(&venc);
		if (aenc) avcodec_free_context(&aenc);
		if (sws) sws_freeContext(sws);
		if (vframe) av_frame_free(&vframe);
		if (aframe) av_frame_free(&aframe);
		if (pkt) av_packet_free(&pkt);
		if (ofmt) {
			if (ofmt->pb && !(ofmt->oformat->flags & AVFMT_NOFILE))
				avio_closep(&ofmt->pb);
			avformat_free_context(ofmt);
		}
		running_ = false;
	};

	/* --- video encoder -------------------------------------------------- */
	const AVCodec *vcodec = avcodec_find_encoder_by_name(plan.encoder);
	if (!vcodec) {
		fail(std::string("encoder '") + plan.encoder +
		     "' not available in this ffmpeg build");
		cleanup();
		return;
	}
	vst = avformat_new_stream(ofmt, nullptr);
	venc = avcodec_alloc_context3(vcodec);
	venc->width = target_.width;
	venc->height = target_.height;
	venc->pix_fmt = plan.pix_fmt;
	venc->time_base = AVRational{(int)target_.fps_den, (int)target_.fps_num};
	venc->framerate = AVRational{(int)target_.fps_num, (int)target_.fps_den};
	/* Tagged BT.709 regardless of raster. The stream's real colorimetry is
	 * in its SDP, which this recorder does not read yet; guessing BT.2020
	 * from the frame size would mislabel every UHD SDR source. */
	venc->colorspace = AVCOL_SPC_BT709;
	venc->color_primaries = AVCOL_PRI_BT709;
	venc->color_trc = AVCOL_TRC_BT709;
	venc->color_range = AVCOL_RANGE_MPEG; /* ST 2110-20 is narrow range */
	venc->thread_count = 0;
	if (target_.codec == RecordCodec::ProRes422HQ)
		av_opt_set(venc->priv_data, "profile", "3", 0); /* 422 HQ */
	else if (target_.codec == RecordCodec::ProRes4444)
		av_opt_set(venc->priv_data, "profile", "4", 0);
	else if (target_.codec == RecordCodec::H264) {
		av_opt_set(venc->priv_data, "preset", "veryfast", 0);
		av_opt_set(venc->priv_data, "tune", "zerolatency", 0);
	}
	if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
		venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	err = avcodec_open2(venc, vcodec, nullptr);
	if (err < 0) {
		fail("video encoder open failed: " + av_err(err));
		cleanup();
		return;
	}
	avcodec_parameters_from_context(vst->codecpar, venc);
	vst->time_base = venc->time_base;

	/* --- audio: always PCM 24 bit, this is a broadcast recorder --------- */
	const AVCodec *acodec = avcodec_find_encoder(AV_CODEC_ID_PCM_S24LE);
	if (acodec && target_.channels > 0) {
		ast = avformat_new_stream(ofmt, nullptr);
		aenc = avcodec_alloc_context3(acodec);
		av_channel_layout_default(&aenc->ch_layout, target_.channels);
		aenc->sample_rate = target_.sample_rate;
		aenc->sample_fmt = AV_SAMPLE_FMT_S32;
		aenc->time_base = AVRational{1, (int)target_.sample_rate};
		if (ofmt->oformat->flags & AVFMT_GLOBALHEADER)
			aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		if (avcodec_open2(aenc, acodec, nullptr) < 0) {
			VLOG_W("[enc] audio encoder unavailable; recording video only");
			avcodec_free_context(&aenc);
			ast = nullptr;
		} else {
			avcodec_parameters_from_context(ast->codecpar, aenc);
			ast->time_base = aenc->time_base;
		}
	}

	if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
		err = avio_open(&ofmt->pb, path_.c_str(), AVIO_FLAG_WRITE);
		if (err < 0) {
			fail("cannot open " + path_ + ": " + av_err(err));
			cleanup();
			return;
		}
	}
	err = avformat_write_header(ofmt, nullptr);
	if (err < 0) {
		fail("write header failed: " + av_err(err));
		cleanup();
		return;
	}

	vframe = av_frame_alloc();
	vframe->format = venc->pix_fmt;
	vframe->width = venc->width;
	vframe->height = venc->height;
	av_frame_get_buffer(vframe, 0);

	const int audio_frame_samples = 1024; /* per channel, per encoded chunk */
	if (aenc) {
		aframe = av_frame_alloc();
		aframe->format = aenc->sample_fmt;
		aframe->nb_samples = audio_frame_samples;
		av_channel_layout_copy(&aframe->ch_layout, &aenc->ch_layout);
		aframe->sample_rate = aenc->sample_rate;
		av_frame_get_buffer(aframe, 0);
	}

	auto write_packets = [&](AVCodecContext *enc, AVStream *st) {
		while (avcodec_receive_packet(enc, pkt) == 0) {
			pkt->stream_index = st->index;
			av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
			bytes_written_ += pkt->size;
			av_interleaved_write_frame(ofmt, pkt);
			av_packet_unref(pkt);
		}
	};

	VLOG_I("[enc] recording to %s (%s, %ux%u)", path_.c_str(),
	       record_codec_name(target_.codec), target_.width, target_.height);
	opened_.set_value(true);

	int64_t vpts = -1, apts = 0;
	const int64_t frame_ns =
		(int64_t)1000000000ll * target_.fps_den / target_.fps_num;
	const size_t sample_bytes = (size_t)target_.channels * 3;
	const size_t audio_chunk_bytes = (size_t)audio_frame_samples * sample_bytes;
	l24.resize(audio_chunk_bytes);

	auto encode_audio = [&](int samples) {
		size_t total = (size_t)samples * target_.channels;
		bool underrun = false;
		afifo_.read_exact(l24.data(), (size_t)samples * sample_bytes, &underrun);
		s32.resize(total);
		l24be_to_s32(l24.data(), s32.data(), total);
		av_frame_make_writable(aframe);
		aframe->nb_samples = samples;
		memcpy(aframe->data[0], s32.data(), total * 4);
		aframe->pts = apts;
		apts += samples;
		if (avcodec_send_frame(aenc, aframe) == 0)
			write_packets(aenc, ast);
	};

	/* No running_ check: stop() closes the queue, and pop() keeps handing
	 * out what is still in it before it reports the end. Otherwise the
	 * last third of a second of every take would be thrown away. */
	for (;;) {
		VideoFrame vf;
		if (!vq_.pop(vf))
			break;
		if (vf.eos)
			break;

		av_frame_make_writable(vframe);
		if (plan.pix_fmt == AV_PIX_FMT_YUV422P10LE &&
		    (int)vf.width == venc->width && (int)vf.height == venc->height) {
			/* identical layout: straight line copies */
			for (int p = 0; p < 3; p++) {
				const uint8_t *src = vf.plane(p);
				uint8_t *dst = vframe->data[p];
				size_t bytes = vf.stride[p];
				for (uint32_t y = 0; y < vf.height; y++) {
					memcpy(dst, src, bytes);
					src += vf.stride[p];
					dst += vframe->linesize[p];
				}
			}
		} else {
			/* other pixel format, or the source changed raster
			 * mid-take: the file keeps the size it was opened with */
			const uint8_t *src[3] = {vf.plane(0), vf.plane(1), vf.plane(2)};
			int src_stride[3] = {(int)vf.stride[0], (int)vf.stride[1],
					     (int)vf.stride[2]};
			sws = sws_getCachedContext(sws, vf.width, vf.height,
						   AV_PIX_FMT_YUV422P10LE,
						   venc->width, venc->height,
						   venc->pix_fmt, SWS_BICUBIC,
						   nullptr, nullptr, nullptr);
			if (!sws)
				continue;
			sws_scale(sws, src, src_stride, 0, vf.height, vframe->data,
				  vframe->linesize);
		}

		/* The frame's place in the file comes from when it was sampled,
		 * not from how many we happened to get: a frame lost on the
		 * wire or dropped under load leaves a gap of the right length,
		 * and the sound stays where it belongs. pts_ns < 0 means the
		 * source gave us no usable timestamp; then we can only count. */
		int64_t slot = vf.pts_ns >= 0 ? (vf.pts_ns + frame_ns / 2) / frame_ns
					      : vpts + 1;
		vpts = std::max(slot, vpts + 1);
		vframe->pts = vpts;
		if (avcodec_send_frame(venc, vframe) == 0)
			write_packets(venc, vst);
		frames_written_++;

		/* Video is the pump: one frame in, whatever whole audio chunks
		 * have accumulated out. */
		if (aenc)
			while (afifo_.size() >= audio_chunk_bytes)
				encode_audio(audio_frame_samples);
	}

	/* Tail: write the audio that belongs to the video we have, and no
	 * more, so both tracks end together. */
	if (aenc && vpts >= 0) {
		int64_t want = av_rescale((vpts + 1) * frame_ns, target_.sample_rate,
					  1000000000);
		while (apts < want) {
			int64_t have = (int64_t)(afifo_.size() / sample_bytes);
			int n = (int)std::min<int64_t>(
				{want - apts, have, (int64_t)audio_frame_samples});
			if (n <= 0)
				break;
			encode_audio(n);
		}
	}

	/* flush */
	avcodec_send_frame(venc, nullptr);
	write_packets(venc, vst);
	if (aenc) {
		avcodec_send_frame(aenc, nullptr);
		write_packets(aenc, ast);
	}
	av_write_trailer(ofmt);
	VLOG_I("[enc] closed %s (%llu frames, %.1f MiB)", path_.c_str(),
	       (unsigned long long)frames_written_.load(),
	       bytes_written_.load() / 1048576.0);
	cleanup();
}

} // namespace vcard
