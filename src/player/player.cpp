/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "player/player.h"

#include "core/formats.h"
#include "core/log.h"
#include "core/mtl-device.h"

#include <cstring>
#include <pthread.h>

namespace vcard {

namespace {

/* ST 2110-20 is narrow range. Black is Y'=64, Cb=Cr=512 at 10 bit. */
void fill_black(VideoFrame &f)
{
	uint16_t *y = (uint16_t *)f.plane(0);
	for (size_t i = 0; i < (size_t)f.width * f.height; i++)
		y[i] = 64;
	for (int p = 1; p <= 2; p++) {
		uint16_t *c = (uint16_t *)f.plane(p);
		for (size_t i = 0; i < (size_t)(f.width / 2) * f.height; i++)
			c[i] = 512;
	}
}

void copy_to_st_frame(const VideoFrame &src, struct st_frame *dst)
{
	for (int p = 0; p < 3; p++) {
		const uint8_t *s = src.plane(p);
		uint8_t *d = (uint8_t *)dst->addr[p];
		size_t bytes = src.stride[p];
		if (bytes == dst->linesize[p]) {
			memcpy(d, s, bytes * src.height);
			continue;
		}
		for (uint32_t h = 0; h < src.height; h++) {
			memcpy(d, s, bytes);
			s += src.stride[p];
			d += dst->linesize[p];
		}
	}
}

} // namespace

Player::Player(PlayerConfig config)
	: config_(std::move(config)),
	  vq_(config_.queue_frames),
	  afifo_(audio_bytes_per_second(config_.channels) / 2 /* 500 ms */)
{
	preview_.configure(480, 10.0);
	meters_.configure(config_.channels);
}

Player::~Player() { close(); }

void Player::reconfigure(const PlayerConfig &config)
{
	bool was_up = tx_running_;
	if (was_up)
		close();
	config_ = config;
	meters_.configure(config_.channels);
	if (was_up)
		open();
}

bool Player::open()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (tx_running_)
		return true;
	if (!start_sessions())
		return false;

	held_frame_.alloc(config_.width, config_.height);
	fill_black(held_frame_);
	have_held_ = true;

	preview_.start();
	tx_running_ = true;
	if (config_.video_enabled)
		v_thread_ = std::thread(&Player::video_tx_loop, this);
	if (config_.audio_enabled)
		a_thread_ = std::thread(&Player::audio_tx_loop, this);
	return true;
}

void Player::close()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!tx_running_)
			return;
		tx_running_ = false;
	}
	/* Unblock the tx threads sitting in get_frame() and join them BEFORE
	 * touching the decoder: the video tx thread calls advance(), which may
	 * replace decoder_, so stopping it from here first would race. */
	if (v_handle_)
		st20p_tx_wake_block(v_handle_);
	if (a_handle_)
		st30p_tx_wake_block(a_handle_);
	if (v_thread_.joinable())
		v_thread_.join();
	if (a_thread_.joinable())
		a_thread_.join();
	if (decoder_)
		decoder_->stop();
	preview_.stop();

	std::lock_guard<std::mutex> lock(mutex_);
	stop_sessions();
	state_ = State::Idle;
}

bool Player::start_sessions()
{
	auto *dev = MtlDevice::instance();
	if (!dev) {
		VLOG_E("[play:%s] MTL not initialized", config_.name.c_str());
		return false;
	}

	if (config_.video_enabled) {
		struct st20p_tx_ops vops;
		memset(&vops, 0, sizeof(vops));
		v_session_name_ = config_.name + "-st20p-tx";
		vops.name = v_session_name_.c_str();
		vops.priv = this;
		vops.port.num_port = dev->num_ports();
		if (!parse_ip(config_.video_dest_ip,
			      vops.port.dip_addr[MTL_SESSION_PORT_P])) {
			VLOG_E("[play:%s] invalid video dest ip '%s'",
			       config_.name.c_str(), config_.video_dest_ip.c_str());
			return false;
		}
		snprintf(vops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
			 dev->params().port.c_str());
		vops.port.udp_port[MTL_SESSION_PORT_P] = config_.video_port;
		if (dev->redundant()) {
			parse_ip(config_.video_dest_ip,
				 vops.port.dip_addr[MTL_SESSION_PORT_R]);
			snprintf(vops.port.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN,
				 "%s", dev->params().port_r.c_str());
			vops.port.udp_port[MTL_SESSION_PORT_R] = config_.video_port;
		}
		vops.port.payload_type = config_.video_payload_type;
		vops.width = config_.width;
		vops.height = config_.height;
		vops.fps = config_.fps;
		vops.interlaced = config_.interlaced;
		vops.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
		vops.transport_fmt = ST20_FMT_YUV_422_10BIT;
		vops.transport_pacing = ST21_PACING_NARROW;
		vops.transport_packing = ST20_PACKING_BPM;
		vops.device = ST_PLUGIN_DEVICE_AUTO;
		vops.framebuff_cnt = 3;
		/* BLOCK_GET is what makes MTL the clock: get_frame returns once
		 * per epoch instead of returning NULL when nothing is free. */
		vops.flags = ST20P_TX_FLAG_BLOCK_GET;

		v_handle_ = st20p_tx_create(dev->handle(), &vops);
		if (!v_handle_) {
			VLOG_E("[play:%s] st20p_tx_create failed", config_.name.c_str());
			return false;
		}
	}

	if (config_.audio_enabled) {
		struct st30p_tx_ops aops;
		memset(&aops, 0, sizeof(aops));
		a_session_name_ = config_.name + "-st30p-tx";
		aops.name = a_session_name_.c_str();
		aops.priv = this;
		aops.port.num_port = dev->num_ports();
		if (!parse_ip(config_.audio_dest_ip,
			      aops.port.dip_addr[MTL_SESSION_PORT_P])) {
			VLOG_E("[play:%s] invalid audio dest ip '%s'",
			       config_.name.c_str(), config_.audio_dest_ip.c_str());
			stop_sessions();
			return false;
		}
		snprintf(aops.port.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s",
			 dev->params().port.c_str());
		aops.port.udp_port[MTL_SESSION_PORT_P] = config_.audio_port;
		if (dev->redundant()) {
			parse_ip(config_.audio_dest_ip,
				 aops.port.dip_addr[MTL_SESSION_PORT_R]);
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
		/* 10 ms per framebuffer: 10 get_frame calls per second per
		 * buffer is cheap, and it keeps tx latency low. */
		int fb_size = st30_calculate_framebuff_size(
			aops.fmt, aops.ptime, aops.sampling, aops.channel,
			10 * 1000 * 1000, nullptr);
		if (fb_size <= 0) {
			VLOG_E("[play:%s] bad audio framebuff size", config_.name.c_str());
			stop_sessions();
			return false;
		}
		aops.framebuff_size = (uint32_t)fb_size;
		aops.flags = ST30P_TX_FLAG_BLOCK_GET;

		a_handle_ = st30p_tx_create(dev->handle(), &aops);
		if (!a_handle_) {
			VLOG_E("[play:%s] st30p_tx_create failed", config_.name.c_str());
			stop_sessions();
			return false;
		}
	}

	VLOG_I("[play:%s] senders up: video %s:%u pt%u %ux%u%s, audio %s:%u pt%u %uch %s",
	       config_.name.c_str(), config_.video_dest_ip.c_str(), config_.video_port,
	       config_.video_payload_type, config_.width, config_.height,
	       mtl_fps_name(config_.fps), config_.audio_dest_ip.c_str(),
	       config_.audio_port, config_.audio_payload_type, config_.channels,
	       ptime_name(config_.ptime));
	return true;
}

void Player::stop_sessions()
{
	if (v_handle_) {
		st20p_tx_free(v_handle_);
		v_handle_ = nullptr;
	}
	if (a_handle_) {
		st30p_tx_free(a_handle_);
		a_handle_ = nullptr;
	}
}

/* --- transmit threads ------------------------------------------------------ */

void Player::video_tx_loop()
{
	pthread_setname_np(pthread_self(), "vcard-vtx");

	while (tx_running_) {
		struct st_frame *dst = st20p_tx_get_frame(v_handle_);
		if (!dst)
			continue; /* woken for shutdown */

		if (blank_requested_.exchange(false) && have_held_)
			fill_black(held_frame_);

		bool consumed_new = false;
		if (state_ == State::Playing && vq_.size() > 0) {
			VideoFrame vf;
			if (vq_.pop(vf)) {
				if (vf.eos) {
					advance();
				} else {
					held_frame_ = std::move(vf);
					have_held_ = true;
					consumed_new = true;
				}
			}
		}
		if (state_ == State::Playing && !consumed_new)
			frames_repeated_++;

		if (have_held_)
			copy_to_st_frame(held_frame_, dst);

		st20p_tx_put_frame(v_handle_, dst);
		frames_sent_++;

		if (have_held_) {
			const uint8_t *planes[3] = {held_frame_.plane(0),
						    held_frame_.plane(1),
						    held_frame_.plane(2)};
			preview_.submit(planes, held_frame_.stride, held_frame_.width,
					held_frame_.height);
		}
	}
	VLOG_D("[play:%s] video tx thread out", config_.name.c_str());
}

void Player::audio_tx_loop()
{
	pthread_setname_np(pthread_self(), "vcard-atx");

	while (tx_running_) {
		struct st30_frame *frame = st30p_tx_get_frame(a_handle_);
		if (!frame)
			continue;

		bool underrun = false;
		uint8_t *dst = (uint8_t *)frame->addr;
		size_t len = frame->buffer_size;
		if (state_ == State::Playing) {
			afifo_.read_exact(dst, len, &underrun);
			if (underrun)
				audio_underruns_++;
		} else {
			memset(dst, 0, len); /* idle/paused: digital silence */
		}
		frame->data_size = len;

		meters_.feed(dst, len, config_.channels);
		st30p_tx_put_frame(a_handle_, frame);
	}
	VLOG_D("[play:%s] audio tx thread out", config_.name.c_str());
}

/* --- transport controls ---------------------------------------------------- */

bool Player::load(const std::string &path)
{
	set_playlist({PlaylistItem{path, path}});
	return play();
}

void Player::set_playlist(std::vector<PlaylistItem> items)
{
	stop();
	std::lock_guard<std::mutex> lock(mutex_);
	playlist_ = std::move(items);
	current_index_ = playlist_.empty() ? -1 : 0;
}

std::vector<PlaylistItem> Player::playlist() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return playlist_;
}

void Player::start_current_item()
{
	/* caller holds mutex_ */
	if (current_index_ < 0 || current_index_ >= (int)playlist_.size())
		return;

	DecoderTarget t;
	t.width = config_.width;
	t.height = config_.height;
	mtl_fps_to_rational(config_.fps, &t.fps_num, &t.fps_den);
	t.channels = config_.channels;

	decoder_ = std::make_unique<FileDecoder>(vq_, afifo_, t);
	/* Looping a single item is the decoder's job; looping a playlist is
	 * ours (see advance()). */
	bool single_loop = loop_ && playlist_.size() == 1;
	if (!decoder_->start(playlist_[current_index_].path, single_loop)) {
		VLOG_E("[play:%s] cannot start %s", config_.name.c_str(),
		       playlist_[current_index_].path.c_str());
		decoder_.reset();
		state_ = State::Idle;
		return;
	}
	VLOG_I("[play:%s] playing [%d/%zu] %s", config_.name.c_str(),
	       current_index_ + 1, playlist_.size(),
	       playlist_[current_index_].path.c_str());
}

void Player::advance()
{
	/* called from the video tx thread on end of item */
	std::lock_guard<std::mutex> lock(mutex_);
	if (playlist_.empty()) {
		state_ = State::Idle;
		return;
	}
	if (current_index_ + 1 < (int)playlist_.size()) {
		current_index_++;
	} else if (loop_) {
		current_index_ = 0;
	} else {
		VLOG_I("[play:%s] playlist finished", config_.name.c_str());
		state_ = State::Idle;
		if (decoder_)
			decoder_->stop();
		return;
	}
	if (decoder_)
		decoder_->stop();
	start_current_item();
}

bool Player::play()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (playlist_.empty())
		return false;
	if (state_ == State::Paused) {
		state_ = State::Playing;
		return true;
	}
	if (current_index_ < 0)
		current_index_ = 0;
	start_current_item();
	if (!decoder_)
		return false;
	state_ = State::Playing;
	return true;
}

void Player::pause()
{
	/* The senders keep running and freeze on the last picture: from the
	 * network's point of view the card is still there. */
	if (state_ == State::Playing)
		state_ = State::Paused;
}

void Player::stop()
{
	std::lock_guard<std::mutex> lock(mutex_);
	state_ = State::Idle;
	if (decoder_) {
		decoder_->stop();
		decoder_.reset();
	}
	/* held_frame_ belongs to the video tx thread; ask it to blank rather
	 * than overwriting the buffer it may be copying out right now. */
	blank_requested_ = true;
}

bool Player::next()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (current_index_ + 1 >= (int)playlist_.size())
		return false;
	current_index_++;
	if (decoder_)
		decoder_->stop();
	start_current_item();
	state_ = State::Playing;
	return true;
}

bool Player::previous()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (current_index_ <= 0)
		return false;
	current_index_--;
	if (decoder_)
		decoder_->stop();
	start_current_item();
	state_ = State::Playing;
	return true;
}

void Player::seek(int64_t position_ns)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (decoder_)
		decoder_->seek(position_ns);
}

Player::Status Player::status() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	Status s;
	s.state = state_;
	s.current_index = current_index_;
	s.playlist_count = (int)playlist_.size();
	s.loop = loop_;
	if (current_index_ >= 0 && current_index_ < (int)playlist_.size())
		s.current_file = playlist_[current_index_].path;
	if (decoder_) {
		s.position_s = decoder_->position_ns() / 1e9;
		s.duration_s = decoder_->duration_ns() / 1e9;
	}
	s.frames_sent = frames_sent_;
	s.frames_repeated = frames_repeated_;
	s.audio_underruns = audio_underruns_;
	s.video_queue = vq_.size();
	s.video_queue_capacity = vq_.capacity();
	s.audio_queue_ms = config_.channels
				   ? afifo_.size() * 1000 /
					     audio_bytes_per_second(config_.channels)
				   : 0;
	s.senders_up = tx_running_;
	s.video_sdp_dest = config_.video_dest_ip + ":" +
			   std::to_string(config_.video_port);
	s.audio_sdp_dest = config_.audio_dest_ip + ":" +
			   std::to_string(config_.audio_port);
	return s;
}

} // namespace vcard
