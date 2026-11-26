/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "media/decoder.h"

#include "core/formats.h"
#include "core/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
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

/* RAII wrappers keep the error paths readable; there are a lot of them. */
struct FmtCtx {
	AVFormatContext *p = nullptr;
	~FmtCtx() { if (p) avformat_close_input(&p); }
};
struct CodecCtx {
	AVCodecContext *p = nullptr;
	~CodecCtx() { if (p) avcodec_free_context(&p); }
};
struct Frame {
	AVFrame *p = av_frame_alloc();
	~Frame() { if (p) av_frame_free(&p); }
};
struct Packet {
	AVPacket *p = av_packet_alloc();
	~Packet() { if (p) av_packet_free(&p); }
};

AVCodecContext *open_stream(AVFormatContext *fmt, int index)
{
	AVStream *st = fmt->streams[index];
	const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
	if (!codec)
		return nullptr;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return nullptr;
	if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
		avcodec_free_context(&ctx);
		return nullptr;
	}
	/* Decoding uncompressed or intra codecs at UHD is the single most
	 * expensive thing this daemon does -- give it every core it can use. */
	ctx->thread_count = 0;
	ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		return nullptr;
	}
	return ctx;
}

} // namespace

FileDecoder::FileDecoder(BoundedQueue<VideoFrame> &vq, AudioFifo &afifo,
			 DecoderTarget target, std::atomic<bool> *audio_done)
	: vq_(vq), afifo_(afifo), target_(target), audio_done_(audio_done)
{
}

FileDecoder::~FileDecoder() { stop(); }

FileDecoder::FileInfo FileDecoder::probe(const std::string &path)
{
	FileInfo info;
	FmtCtx fmt;
	int err = avformat_open_input(&fmt.p, path.c_str(), nullptr, nullptr);
	if (err < 0) {
		info.error = "open failed: " + av_err(err);
		return info;
	}
	if (avformat_find_stream_info(fmt.p, nullptr) < 0) {
		info.error = "no stream info";
		return info;
	}

	int vidx = av_find_best_stream(fmt.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (vidx < 0) {
		info.error = "file has no video stream";
		return info;
	}
	AVStream *vs = fmt.p->streams[vidx];
	info.width = vs->codecpar->width;
	info.height = vs->codecpar->height;
	AVRational fr = av_guess_frame_rate(fmt.p, vs, nullptr);
	info.fps = fr.den ? (double)fr.num / fr.den : 0.0;
	const AVCodecDescriptor *vd = avcodec_descriptor_get(vs->codecpar->codec_id);
	info.video_codec = vd ? vd->name : "?";
	const char *pf = av_get_pix_fmt_name((AVPixelFormat)vs->codecpar->format);
	info.pix_fmt = pf ? pf : "?";

	int aidx = av_find_best_stream(fmt.p, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (aidx >= 0) {
		AVStream *as = fmt.p->streams[aidx];
		info.audio_channels = as->codecpar->ch_layout.nb_channels;
		info.audio_sample_rate = as->codecpar->sample_rate;
		const AVCodecDescriptor *ad =
			avcodec_descriptor_get(as->codecpar->codec_id);
		info.audio_codec = ad ? ad->name : "?";
	}

	if (fmt.p->duration != AV_NOPTS_VALUE)
		info.duration_s = (double)fmt.p->duration / AV_TIME_BASE;
	info.ok = true;
	return info;
}

bool FileDecoder::start(const std::string &path, bool loop)
{
	stop();
	auto info = probe(path);
	if (!info.ok) {
		VLOG_E("[dec] %s: %s", path.c_str(), info.error.c_str());
		return false;
	}
	running_ = true;
	finished_ = false;
	position_ns_ = 0;
	frames_emitted_ = 0;
	has_audio_ = target_.audio && info.audio_channels > 0;
	loop_audio_excess_ = 0;
	seek_request_ = -1;
	if (audio_done_)
		*audio_done_ = false;
	thread_ = std::thread(&FileDecoder::run, this, path, loop);
	return true;
}

void FileDecoder::stop()
{
	running_ = false;
	vq_.stop();
	afifo_.stop();
	if (thread_.joinable())
		thread_.join();
	vq_.restart();
	afifo_.restart();
}

void FileDecoder::seek(int64_t position_ns) { seek_request_ = position_ns; }

void FileDecoder::run(std::string path, bool loop)
{
	pthread_setname_np(pthread_self(), "vcard-dec");
	do {
		if (!decode_file(path))
			break;
		if (loop && running_)
			VLOG_D("[dec] loop restart: %s", path.c_str());
	} while (loop && running_);

	finished_ = true;
	if (audio_done_)
		*audio_done_ = true;
	/* Sentinel so the player knows the item ended rather than stalled. */
	VideoFrame eos;
	eos.eos = true;
	vq_.push(std::move(eos));
	VLOG_I("[dec] finished %s (%llu frames)", path.c_str(),
	       (unsigned long long)frames_emitted_.load());
}

bool FileDecoder::decode_file(const std::string &path)
{
	FmtCtx fmt;
	int err = avformat_open_input(&fmt.p, path.c_str(), nullptr, nullptr);
	if (err < 0) {
		VLOG_E("[dec] open %s: %s", path.c_str(), av_err(err).c_str());
		return false;
	}
	if (avformat_find_stream_info(fmt.p, nullptr) < 0) {
		VLOG_E("[dec] no stream info for %s", path.c_str());
		return false;
	}
	/* AVFormatContext::duration is in AV_TIME_BASE (microsecond) units */
	duration_ns_ = fmt.p->duration != AV_NOPTS_VALUE ? fmt.p->duration * 1000ll : 0;

	int vidx = av_find_best_stream(fmt.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	int aidx = av_find_best_stream(fmt.p, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (vidx < 0)
		return false;
	if (!target_.audio)
		aidx = -1;

	CodecCtx vdec, adec;
	vdec.p = open_stream(fmt.p, vidx);
	if (!vdec.p) {
		VLOG_E("[dec] cannot open video decoder for %s", path.c_str());
		return false;
	}
	if (aidx >= 0) {
		adec.p = open_stream(fmt.p, aidx);
		if (!adec.p)
			VLOG_W("[dec] audio stream present but not decodable; "
			       "playing silence");
	}
	has_audio_ = adec.p != nullptr;

	SwsContext *sws = nullptr;
	SwrContext *swr = nullptr;
	struct Cleanup {
		SwsContext *&sws;
		SwrContext *&swr;
		~Cleanup()
		{
			if (sws) sws_freeContext(sws);
			if (swr) swr_free(&swr);
		}
	} cleanup{sws, swr};

	std::vector<uint8_t> l24;
	std::vector<int32_t> s32;
	const size_t bytes_per_frame = (size_t)target_.channels * 3;
	const AVRational ns_base{1, 1000000000};

	/* --- timeline ------------------------------------------------------
	 * Everything below runs on one timeline that starts at zero at the
	 * beginning of the file. A container's own timestamps do not: camera
	 * files and transport streams routinely start at an hour or at some
	 * arbitrary PCR value, and audio and video rarely start on the same
	 * tick. The container's start_time is the origin both essences are
	 * measured from, which keeps their offset to each other intact. */
	int64_t origin_ns = fmt.p->start_time != AV_NOPTS_VALUE
				    ? fmt.p->start_time * 1000ll
				    : AV_NOPTS_VALUE;

	/* Session grid: one slot per transmitted frame. */
	const int64_t frame_ns =
		(int64_t)1000000000ll * target_.fps_den / target_.fps_num;
	/* How long one picture of *this file* is on screen. Decides how many
	 * session slots it fills: two for 25p on 50p, sometimes none for 60p
	 * on 50p. */
	AVRational src_rate = av_guess_frame_rate(fmt.p, fmt.p->streams[vidx], nullptr);
	const int64_t src_frame_ns = (src_rate.num > 0 && src_rate.den > 0)
					     ? (int64_t)1000000000ll * src_rate.den / src_rate.num
					     : frame_ns;

	int64_t out_index = 0;   /* next slot on the session grid */
	VideoFrame pending;      /* most recent decoded picture */
	bool have_pending = false;

	/* Audio is placed on the same timeline once, at the start and after
	 * every seek, and then simply appended: gate_ns is the timeline
	 * position the next written sample has to land on. */
	int64_t gate_ns = 0;
	bool audio_placed = false;
	int64_t audio_drop = loop_audio_excess_; /* samples to discard first */
	int64_t audio_written = 0;               /* samples since gate_ns */
	loop_audio_excess_ = 0;

	auto write_silence = [&](int64_t samples) {
		static const uint8_t zeros[4800 * 3] = {0};
		size_t bytes = (size_t)samples * bytes_per_frame;
		while (bytes) {
			size_t n = std::min(bytes, sizeof(zeros) - sizeof(zeros) % bytes_per_frame);
			if (!afifo_.write(zeros, n))
				return false;
			bytes -= n;
		}
		audio_written += samples;
		return true;
	};

	Packet pkt;
	Frame frame;
	bool eof = false;

	auto emit_until = [&](int64_t covered_until_ns) {
		/* Fill every session slot the held picture covers. The last
		 * slot moves the buffer instead of copying it, so the 1:1 case
		 * -- a file authored at the session rate -- costs no memcpy.
		 * At UHD that copy would be 12 MB per frame. */
		while (have_pending && out_index * frame_ns < covered_until_ns) {
			bool last = (out_index + 1) * frame_ns >= covered_until_ns;
			VideoFrame out = last ? std::move(pending) : pending;
			out.pts_ns = out_index * frame_ns;
			position_ns_ = out.pts_ns;
			if (last)
				have_pending = false;
			if (!vq_.push(std::move(out)))
				return false;
			frames_emitted_++;
			out_index++;
		}
		return true;
	};

	while (running_) {
		int64_t seek_to = seek_request_.exchange(-1);
		if (seek_to >= 0) {
			int64_t abs_ns = seek_to + (origin_ns == AV_NOPTS_VALUE ? 0 : origin_ns);
			int64_t ts = av_rescale_q(abs_ns, ns_base, AV_TIME_BASE_Q);
			if (av_seek_frame(fmt.p, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
				avcodec_flush_buffers(vdec.p);
				if (adec.p)
					avcodec_flush_buffers(adec.p);
				if (swr)
					swr_free(&swr); /* drops its buffered tail */
				/* Both queues, or the old audio keeps playing
				 * under the new picture. */
				vq_.clear();
				afifo_.clear();
				out_index = seek_to / frame_ns;
				have_pending = false;
				gate_ns = out_index * frame_ns;
				audio_placed = false;
				audio_drop = 0;
				audio_written = 0;
				position_ns_ = gate_ns;
				eof = false;
			}
			seek_serial_++; /* also on failure: nobody should wait on it */
		}

		AVCodecContext *dec = nullptr;
		bool is_video = false;
		if (!eof) {
			err = av_read_frame(fmt.p, pkt.p);
			if (err == AVERROR_EOF) {
				eof = true;
				avcodec_send_packet(vdec.p, nullptr);
				if (adec.p)
					avcodec_send_packet(adec.p, nullptr);
			} else if (err < 0) {
				VLOG_E("[dec] read error: %s", av_err(err).c_str());
				break;
			} else {
				if (pkt.p->stream_index == vidx) {
					dec = vdec.p;
					is_video = true;
				} else if (aidx >= 0 && pkt.p->stream_index == aidx) {
					dec = adec.p;
				}
				if (dec)
					avcodec_send_packet(dec, pkt.p);
				av_packet_unref(pkt.p);
				if (!dec)
					continue;
			}
		}

		/* On EOF drain both decoders; otherwise only the one we fed. */
		for (int pass = 0; pass < 2; pass++) {
			AVCodecContext *d = eof ? (pass == 0 ? vdec.p : adec.p) : dec;
			bool video = eof ? (pass == 0) : is_video;
			if (!d) {
				if (!eof)
					break;
				continue;
			}

			while (avcodec_receive_frame(d, frame.p) == 0) {
				int64_t pts = frame.p->best_effort_timestamp;
				AVRational tb = fmt.p->streams[video ? vidx : aidx]->time_base;
				int64_t pts_ns = pts == AV_NOPTS_VALUE
							 ? AV_NOPTS_VALUE
							 : av_rescale_q(pts, tb, ns_base);
				if (origin_ns == AV_NOPTS_VALUE && pts_ns != AV_NOPTS_VALUE)
					origin_ns = pts_ns;

				if (video) {
					sws = sws_getCachedContext(
						sws, frame.p->width, frame.p->height,
						(AVPixelFormat)frame.p->format,
						target_.width, target_.height,
						AV_PIX_FMT_YUV422P10LE, SWS_BICUBIC,
						nullptr, nullptr, nullptr);
					if (!sws) {
						VLOG_E("[dec] no scaler for %dx%d fmt %d",
						       frame.p->width, frame.p->height,
						       frame.p->format);
						return false;
					}
					int64_t rel = pts_ns == AV_NOPTS_VALUE
							      ? out_index * frame_ns
							      : pts_ns - origin_ns;
					/* Pictures entirely before the next slot
					 * (the run-up after a seek) need no
					 * conversion at all. */
					if (rel + src_frame_ns <= out_index * frame_ns) {
						av_frame_unref(frame.p);
						continue;
					}
					VideoFrame vf;
					vf.alloc(target_.width, target_.height);
					uint8_t *dst[3] = {vf.plane(0), vf.plane(1),
							   vf.plane(2)};
					int dst_stride[3] = {(int)vf.stride[0],
							     (int)vf.stride[1],
							     (int)vf.stride[2]};
					sws_scale(sws, frame.p->data, frame.p->linesize, 0,
						  frame.p->height, dst, dst_stride);
					pending = std::move(vf);
					have_pending = true;
					if (!emit_until(rel + src_frame_ns))
						return false;
				} else {
					if (!swr) {
						AVChannelLayout out_layout;
						av_channel_layout_default(
							&out_layout, target_.channels);
						if (swr_alloc_set_opts2(
							    &swr, &out_layout,
							    AV_SAMPLE_FMT_S32,
							    target_.sample_rate,
							    &frame.p->ch_layout,
							    (AVSampleFormat)frame.p->format,
							    frame.p->sample_rate, 0,
							    nullptr) < 0 ||
						    swr_init(swr) < 0) {
							VLOG_E("[dec] resampler init failed");
							return false;
						}
					}
					if (!audio_placed && pts_ns != AV_NOPTS_VALUE) {
						/* Where does this block sit relative
						 * to where audio has to start? Late:
						 * pad with silence. Early (always the
						 * case after a seek, which lands on a
						 * keyframe before the target): cut. */
						int64_t lead_ns = (pts_ns - origin_ns) - gate_ns;
						int64_t lead = av_rescale(lead_ns,
									  target_.sample_rate,
									  1000000000);
						if (lead > 0) {
							/* cap: a broken timestamp must
							 * not buy minutes of silence */
							if (!write_silence(std::min<int64_t>(
								    lead, target_.sample_rate * 10)))
								return false;
						} else {
							audio_drop += -lead;
						}
						audio_placed = true;
					}
					int max_out = swr_get_out_samples(
						swr, frame.p->nb_samples);
					s32.resize((size_t)max_out * target_.channels);
					uint8_t *out = (uint8_t *)s32.data();
					int got = swr_convert(swr, &out, max_out,
							      (const uint8_t **)frame.p->extended_data,
							      frame.p->nb_samples);
					int skip = (int)std::min<int64_t>(audio_drop,
									  got > 0 ? got : 0);
					audio_drop -= skip;
					if (got - skip > 0) {
						size_t total = (size_t)(got - skip) *
							       target_.channels;
						l24.resize(total * 3);
						s32_to_l24be(s32.data() +
								     (size_t)skip * target_.channels,
							     l24.data(), total);
						if (!afifo_.write(l24.data(), l24.size()))
							return false;
						audio_written += got - skip;
					}
				}
				av_frame_unref(frame.p);
				if (!running_)
					return false;
			}
			if (!eof)
				break;
		}

		if (eof) {
			/* A seek that arrives while we sit at the end is still
			 * a seek. */
			if (seek_request_.load() >= 0)
				continue;
			break;
		}
	}
	if (!running_)
		return false;

	/* The last picture stays up for its full duration. */
	if (have_pending)
		emit_until((out_index + 1) * frame_ns);

	/* The resampler holds back a few samples for its filter; get them out. */
	if (swr && has_audio_) {
		int tail = swr_get_out_samples(swr, 0);
		if (tail > 0) {
			s32.resize((size_t)tail * target_.channels);
			uint8_t *out = (uint8_t *)s32.data();
			int got = swr_convert(swr, &out, tail, nullptr, 0);
			int skip = (int)std::min<int64_t>(audio_drop, got > 0 ? got : 0);
			if (got - skip > 0) {
				size_t total = (size_t)(got - skip) * target_.channels;
				l24.resize(total * 3);
				s32_to_l24be(s32.data() + (size_t)skip * target_.channels,
					     l24.data(), total);
				if (!afifo_.write(l24.data(), l24.size()))
					return false;
				audio_written += got - skip;
			}
		}
	}

	/* Make the audio exactly as long as the video that went out. Files are
	 * rarely cut that precisely; left alone, every pass of a loop would
	 * shift sound against picture by the difference. */
	if (has_audio_) {
		int64_t video_ns = out_index * frame_ns - gate_ns;
		int64_t want = av_rescale(video_ns, target_.sample_rate, 1000000000);
		if (audio_written < want) {
			if (!write_silence(want - audio_written))
				return false;
		} else {
			loop_audio_excess_ = audio_written - want;
		}
	}
	return running_;
}

} // namespace vcard
