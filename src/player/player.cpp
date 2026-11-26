/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "player/player.h"

#include "core/formats.h"
#include "core/log.h"
#include "core/mtl-device.h"

#include <algorithm>
#include <chrono>
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

/* Narrow pacing needs the NIC's rate limiter, which in MTL means an E810/E830
 * on the DPDK PMD. Everywhere else MTL falls back to TSC pacing, which is wide
 * -- so the SDP has to say wide. */
enum st21_pacing resolve_pacing(const std::string &configured, bool kernel_path)
{
	if (configured == "narrow")
		return ST21_PACING_NARROW;
	if (configured == "wide")
		return ST21_PACING_WIDE;
	if (configured == "linear")
		return ST21_PACING_LINEAR;
	return kernel_path ? ST21_PACING_WIDE : ST21_PACING_NARROW;
}

const char *pacing_name(enum st21_pacing p)
{
	switch (p) {
	case ST21_PACING_WIDE: return "wide";
	case ST21_PACING_LINEAR: return "linear";
	default: return "narrow";
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

enum st21_pacing Player::video_pacing() const
{
	auto *dev = MtlDevice::instance();
	return resolve_pacing(config_.pacing, dev && dev->kernel_data_path());
}

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

	/* fresh sessions, fresh bookkeeping for the done callbacks */
	v_put_seq_ = v_done_seq_ = v_anchor_seq_ = 0;
	a_put_seq_ = a_done_seq_ = a_anchor_seq_ = 0;
	v_last_done_tai_ = a_last_done_tai_ = 0;
	v_anchor_tai_ = a_anchor_tai_ = 0;

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
		vops.transport_pacing = resolve_pacing(config_.pacing,
						       dev->kernel_data_path());
		vops.transport_packing = ST20_PACKING_BPM;
		vops.device = ST_PLUGIN_DEVICE_AUTO;
		vops.framebuff_cnt = 3;
		/* BLOCK_GET is what makes MTL the clock: get_frame returns once
		 * per epoch instead of returning NULL when nothing is free. */
		vops.flags = ST20P_TX_FLAG_BLOCK_GET;
		vops.notify_frame_done = Player::on_video_done;

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
		/* 10 ms per framebuffer. How many of them decides lip sync: with
		 * BLOCK_GET a buffer comes back when its frame has gone out, so
		 * what we fill now is sent `count` periods later -- 60 ms for
		 * three video buffers at 50p. Four 10 ms audio buffers would be
		 * 40 ms and the sound would lead by the difference, so the audio
		 * depth is derived from the video one. Whether real hardware
		 * agrees shows up as av_skew_ms in the status; av_offset_ms trims
		 * what is left. */
		const uint64_t audio_fb_ns = 10 * 1000 * 1000;
		uint64_t video_ahead_ns = 3 /* vops.framebuff_cnt */ *
					  mtl_fps_frame_ns(config_.fps);
		uint16_t audio_bufs = (uint16_t)((video_ahead_ns + audio_fb_ns / 2) /
						 audio_fb_ns);
		aops.framebuff_cnt = config_.video_enabled
					     ? std::clamp<uint16_t>(audio_bufs, 2, 16)
					     : 4;
		int fb_size = st30_calculate_framebuff_size(
			aops.fmt, aops.ptime, aops.sampling, aops.channel,
			audio_fb_ns, nullptr);
		if (fb_size <= 0) {
			VLOG_E("[play:%s] bad audio framebuff size", config_.name.c_str());
			stop_sessions();
			return false;
		}
		aops.framebuff_size = (uint32_t)fb_size;
		aops.flags = ST30P_TX_FLAG_BLOCK_GET;
		aops.notify_frame_done = Player::on_audio_done;
		audio_block_ns_ = (uint64_t)fb_size * 1000000000ull /
				  audio_bytes_per_second(config_.channels);

		a_handle_ = st30p_tx_create(dev->handle(), &aops);
		if (!a_handle_) {
			VLOG_E("[play:%s] st30p_tx_create failed", config_.name.c_str());
			stop_sessions();
			return false;
		}
	}

	VLOG_I("[play:%s] senders up: video %s:%u pt%u %ux%up%s %s pacing, "
	       "audio %s:%u pt%u %uch %s",
	       config_.name.c_str(), config_.video_dest_ip.c_str(), config_.video_port,
	       config_.video_payload_type, config_.width, config_.height,
	       mtl_fps_name(config_.fps),
	       pacing_name(resolve_pacing(config_.pacing, dev->kernel_data_path())),
	       config_.audio_dest_ip.c_str(), config_.audio_port,
	       config_.audio_payload_type, config_.channels, ptime_name(config_.ptime));
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

/* --- transport feedback ---------------------------------------------------- */

int Player::on_video_done(void *priv, struct st_frame *frame)
{
	auto *p = (Player *)priv;
	uint64_t seq = ++p->v_done_seq_;
	if (frame->tfmt != ST10_TIMESTAMP_FMT_TAI || !frame->timestamp)
		return 0;
	uint64_t t = frame->timestamp;
	if (seq == p->v_anchor_seq_.load())
		p->v_anchor_tai_ = t;

	uint64_t prev = p->v_last_done_tai_;
	p->v_last_done_tai_ = t;
	if (!prev || t <= prev)
		return 0;
	uint64_t frame_ns = mtl_fps_frame_ns(p->config_.fps);
	uint64_t steps = (t - prev + frame_ns / 2) / frame_ns;
	if (steps > 1 && steps < 500) {
		p->video_slip_frames_ += (uint32_t)(steps - 1);
		p->video_epochs_skipped_ += steps - 1;
	}
	return 0;
}

int Player::on_audio_done(void *priv, struct st30_frame *frame)
{
	auto *p = (Player *)priv;
	uint64_t seq = ++p->a_done_seq_;
	if (frame->tfmt != ST10_TIMESTAMP_FMT_TAI || !frame->timestamp)
		return 0;
	uint64_t t = frame->timestamp;
	if (seq == p->a_anchor_seq_.load())
		p->a_anchor_tai_ = t;

	uint64_t prev = p->a_last_done_tai_;
	p->a_last_done_tai_ = t;
	if (!prev || t <= prev)
		return 0;
	/* Audio can fall behind by single packets, not only whole buffers.
	 * Half a millisecond of deadband keeps arithmetic noise out. */
	int64_t extra = (int64_t)(t - prev) - (int64_t)p->audio_block_ns_;
	if (extra > 500000 && extra < 5000000000ll) {
		p->audio_slip_ns_ += extra;
		p->audio_gap_ns_total_ += (uint64_t)extra;
	}
	return 0;
}

/* --- transmit threads ------------------------------------------------------ */

void Player::video_tx_loop()
{
	pthread_setname_np(pthread_self(), "vcard-vtx");

	while (tx_running_) {
		struct st_frame *dst = st20p_tx_get_frame(v_handle_);
		if (!dst)
			continue; /* woken for shutdown, or the 1 s block timeout */
		const uint64_t woke_ns = monotonic_ns();
		bool first_of_flow = false;

		if (blank_requested_.exchange(false) && have_held_)
			fill_black(held_frame_);

		const bool playing = state_ == State::Playing;

		/* Preroll: hold the previous picture until the decoder has both
		 * queues primed, then release picture and sound together. The
		 * timeout keeps a file shorter than the preroll (or one whose
		 * audio starts late) from holding forever. */
		if (playing && !flowing_ && !preroll_blocked_) {
			uint64_t now = monotonic_ns();
			if (!preroll_started_ns_)
				preroll_started_ns_ = now;
			size_t want_v = std::min<size_t>(vq_.capacity() / 2, 4);
			size_t want_a = audio_bytes_per_second(config_.channels) / 20;
			bool v_ready = vq_.size() >= std::max<size_t>(want_v, 1);
			bool a_ready = !item_has_audio_ || !config_.audio_enabled ||
				       afifo_.size() >= want_a;
			if ((v_ready && a_ready) || now - preroll_started_ns_ > 500000000ull) {
				video_debt_ = 0;
				video_slip_frames_ = 0;
				v_anchor_tai_ = 0;
				a_anchor_tai_ = 0;
				first_of_flow = true;
				flow_start_ns_ = woke_ns;
				flowing_ = true;
			}
		}

		bool consumed_new = false;
		if (playing && flowing_) {
			/* Pay back earlier underruns by skipping, but never
			 * the last queued frame -- that one has to go out. */
			uint32_t slipped = video_slip_frames_.exchange(0);
			if (slipped)
				video_debt_ = std::min<uint32_t>(video_debt_ + slipped, 250);
			VideoFrame vf;
			bool got = vq_.try_pop(vf);
			while (got && !vf.eos && video_debt_ > 0 && vq_.size() > 0) {
				VideoFrame next;
				if (!vq_.try_pop(next))
					break;
				vf = std::move(next);
				video_debt_--;
			}
			if (got && vf.eos) {
				advance();
			} else if (got) {
				held_frame_ = std::move(vf);
				have_held_ = true;
				consumed_new = true;
				shown_pts_ns_ = held_frame_.pts_ns;
			} else {
				frames_repeated_++;
				if (video_debt_ < 250)
					video_debt_++;
			}
		}

		if (have_held_)
			copy_to_st_frame(held_frame_, dst);

		uint64_t seq = ++v_put_seq_;
		if (first_of_flow && consumed_new)
			v_anchor_seq_ = seq;
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
	const size_t sample_frame = (size_t)config_.channels * 3;
	bool was_flowing = false;

	while (tx_running_) {
		struct st30_frame *frame = st30p_tx_get_frame(a_handle_);
		if (!frame)
			continue;
		const uint64_t woke_ns = monotonic_ns();
		const uint64_t seq = ++a_put_seq_;

		uint8_t *dst = (uint8_t *)frame->addr;
		size_t len = frame->buffer_size;
		const bool flowing = state_ == State::Playing && flowing_ &&
				     item_has_audio_;
		if (flowing && !was_flowing) {
			audio_slip_ns_ = 0;
			a_anchor_seq_ = seq;
			/* The video thread released the item on its tick; ours
			 * comes up to one buffer later. Skip what should have
			 * gone out in between, so the first sample lines up with
			 * the first picture instead of trailing it. */
			uint64_t start = flow_start_ns_.load();
			uint64_t lag_ns = (start && woke_ns > start) ? woke_ns - start : 0;
			size_t lag = lag_ns < 50000000ull
					     ? (size_t)(lag_ns * 48 / 1000000) * sample_frame
					     : 0;
			audio_debt_bytes_ = lag;
			a_anchor_lag_ns_ = lag ? lag_ns : 0;
		}
		was_flowing = flowing;

		int64_t slip_ns = audio_slip_ns_.exchange(0);
		if (flowing && slip_ns > 0)
			audio_debt_bytes_ += (size_t)(slip_ns * 48 / 1000000) * sample_frame;

		if (flowing) {
			size_t off = 0;
			int64_t trim = audio_trim_bytes_.load();
			if (trim > 0) { /* configured lead-in: audio later */
				off = std::min<size_t>(len, (size_t)trim);
				memset(dst, 0, off);
				audio_trim_bytes_ -= (int64_t)off;
			} else if (trim < 0) { /* audio earlier */
				size_t n = afifo_.discard((size_t)-trim);
				audio_trim_bytes_ += (int64_t)n;
			}
			if (audio_debt_bytes_)
				audio_debt_bytes_ -= afifo_.discard(audio_debt_bytes_);

			bool underrun = false;
			size_t missing = 0;
			afifo_.read_exact(dst + off, len - off, &underrun, &missing);
			/* Running dry after the file has handed over its last
			 * sample is simply the end of the item. */
			if (underrun && !item_audio_done_) {
				uint64_t n = ++audio_underruns_;
				if (n <= 3 || n % 100 == 0)
					VLOG_D("[play:%s] audio underrun #%llu: %zu of %zu "
					       "bytes missing at %.2f s, video queue %zu",
					       config_.name.c_str(), (unsigned long long)n,
					       missing, len - off, shown_pts_ns_ / 1e9,
					       vq_.size());
				/* whole sample frames only, or the channels
				 * would rotate */
				missing -= missing % sample_frame;
				if (audio_debt_bytes_ < audio_bytes_per_second(config_.channels))
					audio_debt_bytes_ += missing;
			}
		} else {
			memset(dst, 0, len); /* idle, paused or silent file */
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

	decoder_ = std::make_unique<FileDecoder>(vq_, afifo_, t, &item_audio_done_);
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
	item_has_audio_ = decoder_->has_audio();
	shown_pts_ns_ = 0;
	/* bytes of whole sample frames; sign carries the direction */
	int64_t trim_samples = (int64_t)config_.av_offset_ms * 48;
	audio_trim_bytes_ = trim_samples * (int64_t)config_.channels * 3;
	preroll_started_ns_ = 0;
	flowing_ = false;
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
		flowing_ = false;
		if (decoder_)
			decoder_->stop();
		return;
	}
	flowing_ = false;
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
		/* Resume by seeking to the picture that is on screen. The two
		 * transmit threads stop on different ticks when pausing, so
		 * simply carrying on would leave sound and picture a little
		 * further apart after every pause. */
		int64_t frame_ns = (int64_t)mtl_fps_frame_ns(config_.fps);
		seek_locked(shown_pts_ns_ + frame_ns);
		state_ = State::Playing;
		return true;
	}
	if (current_index_ < 0)
		current_index_ = 0;
	flowing_ = false;
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
	flowing_ = false;
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
	flowing_ = false;
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
	flowing_ = false;
	if (decoder_)
		decoder_->stop();
	start_current_item();
	state_ = State::Playing;
	return true;
}

void Player::seek(int64_t position_ns)
{
	std::lock_guard<std::mutex> lock(mutex_);
	seek_locked(position_ns);
}

void Player::seek_locked(int64_t position_ns)
{
	if (!decoder_)
		return;
	/* Stop taking from the queues first: what is in them belongs to the
	 * old position, and picture and sound have to restart together. Then
	 * empty them so a decoder blocked on a full queue gets to see the
	 * request, and wait until it has flushed and repositioned -- otherwise
	 * the preroll would be satisfied by leftovers. */
	preroll_blocked_ = true;
	flowing_ = false;
	uint64_t before = decoder_->seek_serial();
	decoder_->seek(position_ns);
	for (int i = 0; i < 500 && decoder_->seek_serial() == before &&
			!decoder_->finished(); i++) {
		vq_.clear();
		afifo_.clear();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	shown_pts_ns_ = position_ns;
	preroll_started_ns_ = 0;
	preroll_blocked_ = false;
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
		/* what is on the wire, not how far the decoder has run ahead */
		s.position_s = shown_pts_ns_ / 1e9;
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
	s.video_epochs_skipped = video_epochs_skipped_;
	s.audio_gap_ms = audio_gap_ns_total_ / 1000000;
	uint64_t va = v_anchor_tai_, aa = a_anchor_tai_;
	if (va && aa) {
		s.av_skew_valid = true;
		/* the first block starts `lag` into the item, by design */
		s.av_skew_ms = ((double)aa - (double)a_anchor_lag_ns_.load() -
				(double)va) / 1e6;
	}
	s.senders_up = tx_running_;
	s.video_sdp_dest = config_.video_dest_ip + ":" +
			   std::to_string(config_.video_port);
	s.audio_sdp_dest = config_.audio_dest_ip + ":" +
			   std::to_string(config_.audio_port);
	return s;
}

} // namespace vcard
