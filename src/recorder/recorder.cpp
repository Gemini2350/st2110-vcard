/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "recorder/recorder.h"

#include "core/formats.h"
#include "core/log.h"
#include "core/mtl-device.h"

#include <cstring>
#include <ctime>
#include <filesystem>
#include <pthread.h>

namespace vcard {

namespace {

std::string timestamp_slug()
{
	time_t t = time(nullptr);
	struct tm tm;
	localtime_r(&t, &tm);
	char buf[32];
	strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
	return buf;
}

} // namespace

Recorder::Recorder(RecorderConfig config)
	: config_(std::move(config)),
	  vq_(config_.queue_frames),
	  afifo_(audio_bytes_per_second(config_.channels) / 2)
{
	preview_.configure(480, 10.0);
	meters_.configure(config_.channels);
}

Recorder::~Recorder() { close(); }

void Recorder::reconfigure(const RecorderConfig &config)
{
	bool was_up = rx_running_;
	if (was_up)
		close();
	config_ = config;
	meters_.configure(config_.channels);
	if (was_up)
		open();
}

int Recorder::on_detected(void *priv, const struct st20_detect_meta *meta,
			  struct st20_detect_reply *reply)
{
	/* Runs on an MTL lcore tasklet -- store and return, nothing else. */
	(void)reply;
	auto *r = (Recorder *)priv;
	r->det_width_ = (uint32_t)meta->width;
	r->det_height_ = (uint32_t)meta->height;
	r->det_fps_ = (int)meta->fps;
	r->det_interlaced_ = meta->interlaced;
	return 0;
}

bool Recorder::open()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (rx_running_)
		return true;
	if (!start_sessions())
		return false;

	preview_.start();
	rx_running_ = true;
	if (config_.video_enabled)
		v_thread_ = std::thread(&Recorder::video_rx_loop, this);
	if (config_.audio_enabled)
		a_thread_ = std::thread(&Recorder::audio_rx_loop, this);
	return true;
}

void Recorder::close()
{
	stop_recording();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!rx_running_)
			return;
		rx_running_ = false;
	}
	if (v_handle_)
		st20p_rx_wake_block(v_handle_);
	if (a_handle_)
		st30p_rx_wake_block(a_handle_);
	if (v_thread_.joinable())
		v_thread_.join();
	if (a_thread_.joinable())
		a_thread_.join();
	preview_.stop();

	std::lock_guard<std::mutex> lock(mutex_);
	stop_sessions();
}

bool Recorder::start_sessions()
{
	auto *dev = MtlDevice::instance();
	if (!dev) {
		VLOG_E("[rec:%s] MTL not initialized", config_.name.c_str());
		return false;
	}

	if (config_.video_enabled) {
		struct st20p_rx_ops vops;
		memset(&vops, 0, sizeof(vops));
		v_session_name_ = config_.name + "-st20p-rx";
		vops.name = v_session_name_.c_str();
		vops.priv = this;
		vops.port.num_port = dev->num_ports();
		if (!parse_ip(config_.video_multicast_ip,
			      vops.port.ip_addr[MTL_SESSION_PORT_P])) {
			VLOG_E("[rec:%s] invalid video group '%s'", config_.name.c_str(),
			       config_.video_multicast_ip.c_str());
			return false;
		}
		/* Source-specific join (IGMPv3) when a source IP is configured:
		 * that is what BCP-002/SSM deployments expect, and it keeps
		 * a rogue sender on the same group out of the receiver. */
		if (!config_.video_source_ip.empty())
			parse_ip(config_.video_source_ip,
				 vops.port.mcast_sip_addr[MTL_SESSION_PORT_P]);
		snprintf(vops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
			 dev->params().port.c_str());
		vops.port.udp_port[MTL_SESSION_PORT_P] = config_.video_port;
		if (dev->redundant()) {
			parse_ip(config_.video_multicast_ip,
				 vops.port.ip_addr[MTL_SESSION_PORT_R]);
			if (!config_.video_source_ip.empty())
				parse_ip(config_.video_source_ip,
					 vops.port.mcast_sip_addr[MTL_SESSION_PORT_R]);
			snprintf(vops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN,
				 "%s", dev->params().port_r.c_str());
			vops.port.udp_port[MTL_SESSION_PORT_R] = config_.video_port;
		}
		vops.port.payload_type = config_.video_payload_type;
		vops.width = config_.width;
		vops.height = config_.height;
		vops.fps = config_.fps;
		vops.interlaced = config_.interlaced;
		vops.transport_fmt = ST20_FMT_YUV_422_10BIT;
		vops.output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
		vops.device = ST_PLUGIN_DEVICE_AUTO;
		vops.framebuff_cnt = 3;
		vops.flags = ST20P_RX_FLAG_BLOCK_GET |
			     ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
		if (config_.auto_detect) {
			vops.flags |= ST20P_RX_FLAG_AUTO_DETECT;
			vops.notify_detected = Recorder::on_detected;
		}

		v_handle_ = st20p_rx_create(dev->handle(), &vops);
		if (!v_handle_) {
			VLOG_E("[rec:%s] st20p_rx_create failed", config_.name.c_str());
			return false;
		}
	}

	if (config_.audio_enabled) {
		struct st30p_rx_ops aops;
		memset(&aops, 0, sizeof(aops));
		a_session_name_ = config_.name + "-st30p-rx";
		aops.name = a_session_name_.c_str();
		aops.priv = this;
		aops.port.num_port = dev->num_ports();
		if (!parse_ip(config_.audio_multicast_ip,
			      aops.port.ip_addr[MTL_SESSION_PORT_P])) {
			VLOG_E("[rec:%s] invalid audio group '%s'", config_.name.c_str(),
			       config_.audio_multicast_ip.c_str());
			stop_sessions();
			return false;
		}
		if (!config_.audio_source_ip.empty())
			parse_ip(config_.audio_source_ip,
				 aops.port.mcast_sip_addr[MTL_SESSION_PORT_P]);
		snprintf(aops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
			 dev->params().port.c_str());
		aops.port.udp_port[MTL_SESSION_PORT_P] = config_.audio_port;
		if (dev->redundant()) {
			parse_ip(config_.audio_multicast_ip,
				 aops.port.ip_addr[MTL_SESSION_PORT_R]);
			if (!config_.audio_source_ip.empty())
				parse_ip(config_.audio_source_ip,
					 aops.port.mcast_sip_addr[MTL_SESSION_PORT_R]);
			snprintf(aops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN,
				 "%s", dev->params().port_r.c_str());
			aops.port.udp_port[MTL_SESSION_PORT_R] = config_.audio_port;
		}
		aops.port.payload_type = config_.audio_payload_type;
		aops.fmt = ST30_FMT_PCM24;
		aops.channel = config_.channels;
		aops.sampling = ST30_SAMPLING_48K;
		aops.ptime = config_.ptime;
		aops.framebuff_cnt = 4;
		int fb_size = st30_calculate_framebuff_size(
			aops.fmt, aops.ptime, aops.sampling, aops.channel,
			10 * 1000 * 1000, nullptr);
		if (fb_size <= 0) {
			VLOG_E("[rec:%s] bad audio framebuff size", config_.name.c_str());
			stop_sessions();
			return false;
		}
		aops.framebuff_size = (uint32_t)fb_size;
		aops.flags = ST30P_RX_FLAG_BLOCK_GET;

		a_handle_ = st30p_rx_create(dev->handle(), &aops);
		if (!a_handle_) {
			VLOG_E("[rec:%s] st30p_rx_create failed", config_.name.c_str());
			stop_sessions();
			return false;
		}
	}

	VLOG_I("[rec:%s] receivers up: video %s%s:%u pt%u, audio %s:%u pt%u %uch",
	       config_.name.c_str(),
	       config_.video_source_ip.empty()
		       ? ""
		       : (config_.video_source_ip + " -> ").c_str(),
	       config_.video_multicast_ip.c_str(), config_.video_port,
	       config_.video_payload_type, config_.audio_multicast_ip.c_str(),
	       config_.audio_port, config_.audio_payload_type, config_.channels);
	return true;
}

void Recorder::stop_sessions()
{
	if (v_handle_) {
		st20p_rx_free(v_handle_);
		v_handle_ = nullptr;
	}
	if (a_handle_) {
		st30p_rx_free(a_handle_);
		a_handle_ = nullptr;
	}
}

void Recorder::video_rx_loop()
{
	pthread_setname_np(pthread_self(), "vcard-vrx");

	while (rx_running_) {
		struct st_frame *frame = st20p_rx_get_frame(v_handle_);
		if (!frame)
			continue;

		frames_received_++;
		last_frame_ns_ = monotonic_ns();
		if (frame->status != ST_FRAME_STATUS_COMPLETE)
			frames_incomplete_++;

		const uint8_t *planes[3] = {(const uint8_t *)frame->addr[0],
					    (const uint8_t *)frame->addr[1],
					    (const uint8_t *)frame->addr[2]};
		size_t strides[3] = {frame->linesize[0], frame->linesize[1],
				     frame->linesize[2]};
		preview_.submit(planes, strides, frame->width, frame->height);

		if (recording_) {
			VideoFrame vf;
			vf.alloc(frame->width, frame->height);
			for (int p = 0; p < 3; p++) {
				const uint8_t *s = planes[p];
				uint8_t *d = vf.plane(p);
				for (uint32_t y = 0; y < frame->height; y++) {
					memcpy(d, s, vf.stride[p]);
					s += strides[p];
					d += vf.stride[p];
				}
			}
			/* try_push, never push: a full queue means the encoder
			 * is behind, and blocking here would stall MTL's
			 * receive path and turn a disk problem into packet
			 * loss on the network. */
			if (!vq_.try_push(std::move(vf)))
				frames_dropped_++;
		}

		st20p_rx_put_frame(v_handle_, frame);
	}
	VLOG_D("[rec:%s] video rx thread out", config_.name.c_str());
}

void Recorder::audio_rx_loop()
{
	pthread_setname_np(pthread_self(), "vcard-arx");

	while (rx_running_) {
		struct st30_frame *frame = st30p_rx_get_frame(a_handle_);
		if (!frame)
			continue;

		audio_frames_received_++;
		meters_.feed((const uint8_t *)frame->addr, frame->data_size,
			     config_.channels);
		if (recording_)
			afifo_.try_write((const uint8_t *)frame->addr, frame->data_size);

		st30p_rx_put_frame(a_handle_, frame);
	}
	VLOG_D("[rec:%s] audio rx thread out", config_.name.c_str());
}

bool Recorder::start_recording(const std::string &filename)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (recording_)
		return false;
	if (!rx_running_) {
		VLOG_E("[rec:%s] receivers are not running", config_.name.c_str());
		return false;
	}

	EncoderTarget t;
	uint32_t w = det_width_.load(), h = det_height_.load();
	int fps = det_fps_.load();
	t.width = w ? w : config_.width;
	t.height = h ? h : config_.height;
	mtl_fps_to_rational(fps >= 0 ? (enum st_fps)fps : config_.fps, &t.fps_num,
			    &t.fps_den);
	t.channels = config_.audio_enabled ? config_.channels : 0;
	t.codec = config_.codec;

	std::string name = filename;
	if (name.empty())
		name = config_.name + "-" + timestamp_slug() + "." +
		       record_codec_extension(config_.codec);

	std::error_code ec;
	std::filesystem::create_directories(config_.output_dir, ec);
	std::filesystem::path full =
		std::filesystem::path(config_.output_dir) / name;
	recording_path_ = full.string();

	vq_.restart();
	afifo_.restart();
	encoder_ = std::make_unique<FileEncoder>(vq_, afifo_, t);
	if (!encoder_->start(recording_path_)) {
		encoder_.reset();
		return false;
	}
	frames_dropped_ = 0;
	recording_ = true;
	VLOG_I("[rec:%s] recording started: %s", config_.name.c_str(),
	       recording_path_.c_str());
	return true;
}

void Recorder::stop_recording()
{
	std::unique_ptr<FileEncoder> enc;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!recording_)
			return;
		recording_ = false;
		enc = std::move(encoder_);
	}
	/* Stop outside the lock: flushing the muxer can take a moment and the
	 * status endpoint should stay responsive while it happens. */
	if (enc)
		enc->stop();
	VLOG_I("[rec:%s] recording stopped", config_.name.c_str());
}

Recorder::Status Recorder::status() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	Status s;
	s.receivers_up = rx_running_;
	s.frames_received = frames_received_;
	s.frames_incomplete = frames_incomplete_;
	s.frames_dropped = frames_dropped_;
	s.audio_frames_received = audio_frames_received_;

	uint64_t last = last_frame_ns_.load();
	if (last) {
		int64_t age_ms = (int64_t)((monotonic_ns() - last) / 1000000);
		s.last_frame_age_ms = age_ms;
		s.signal_present = age_ms < 1000;
	}

	uint32_t w = det_width_.load(), h = det_height_.load();
	int fps = det_fps_.load();
	s.width = w ? w : config_.width;
	s.height = h ? h : config_.height;
	enum st_fps f = fps >= 0 ? (enum st_fps)fps : config_.fps;
	s.fps = mtl_fps_to_double(f);
	if (w && h)
		s.detected_format = std::to_string(w) + "x" + std::to_string(h) +
				    (det_interlaced_ ? "i" : "p") + mtl_fps_name(f);

	s.recording = recording_;
	s.recording_path = recording_path_;
	if (encoder_) {
		s.recorded_frames = encoder_->frames_written();
		s.recorded_mib = encoder_->bytes_written() / 1048576.0;
		s.recorded_seconds = s.fps > 0 ? s.recorded_frames / s.fps : 0;
		s.last_error = encoder_->last_error();
	}
	s.video_source = config_.video_multicast_ip + ":" +
			 std::to_string(config_.video_port);
	s.audio_source = config_.audio_multicast_ip + ":" +
			 std::to_string(config_.audio_port);
	return s;
}

} // namespace vcard
