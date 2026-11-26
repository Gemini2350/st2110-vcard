/* SPDX-License-Identifier: GPL-2.0-or-later
 * Playout: file -> ST 2110-20 video + ST 2110-30 audio.
 *
 * Threading:
 *   decoder thread  -- FileDecoder, fills the queues, blocks when they are full
 *   video tx thread -- st20p_tx_get_frame (BLOCK_GET) is the frame clock
 *   audio tx thread -- st30p_tx_get_frame (BLOCK_GET) is the packet clock
 *
 * Neither tx thread ever sleeps on a timer. MTL hands out a buffer exactly
 * when the next PTP epoch needs one, so playout speed is the network's, not
 * the file's or the CPU's.
 */
#pragma once

#include "core/media-buffer.h"
#include "media/decoder.h"
#include "monitor/meters.h"
#include "monitor/preview.h"

#include <mtl/st30_pipeline_api.h>
#include <mtl/st_pipeline_api.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcard {

struct PlayerConfig {
	std::string name = "player1";

	uint32_t width = 1920;
	uint32_t height = 1080;
	enum st_fps fps = ST_FPS_P50;
	bool interlaced = false;

	/* What the sender declares in the SDP (TP=2110TPN / TPW / TPNL).
	 * Empty or "auto" picks narrow on a DPDK PMD and wide on the kernel
	 * data path -- declaring narrow while MTL paces from the TSC would put
	 * a claim in the SDP that the stream does not keep. */
	std::string pacing = "auto";

	/* video transport */
	std::string video_dest_ip = "239.100.0.10";
	uint16_t video_port = 20000;
	uint8_t video_payload_type = 96;
	bool video_enabled = true;

	/* audio transport */
	bool audio_enabled = true;
	uint16_t channels = 2;
	std::string audio_dest_ip = "239.100.0.11";
	uint16_t audio_port = 30000;
	uint8_t audio_payload_type = 97;
	enum st30_ptime ptime = ST30_PTIME_1MS;

	/* buffering: how far the decoder may run ahead, in frames */
	uint32_t queue_frames = 8;

	/* Lip-sync trim in milliseconds, positive = audio later. The sender
	 * already equalises the two transmit pipelines (see start_sessions);
	 * this is for whatever a measurement on the real wire still shows. */
	int av_offset_ms = 0;
};

struct PlaylistItem {
	std::string path;
	std::string label;
};

class Player {
public:
	explicit Player(PlayerConfig config);
	~Player();

	/* Bring the ST 2110 senders up. The wire carries black/silence until
	 * something is loaded -- a real card does not disappear from the
	 * network between clips. */
	bool open();
	void close();

	bool load(const std::string &path); /* single file, replaces playlist */
	void set_playlist(std::vector<PlaylistItem> items);
	std::vector<PlaylistItem> playlist() const;

	bool play();
	void pause();
	void stop();
	bool next();
	bool previous();
	void seek(int64_t position_ns);
	void set_loop(bool loop) { loop_ = loop; }
	bool loop() const { return loop_; }

	enum class State { Idle, Playing, Paused };

	struct Status {
		State state = State::Idle;
		std::string current_file;
		int current_index = -1;
		int playlist_count = 0;
		bool loop = false;
		double position_s = 0;
		double duration_s = 0;
		uint64_t frames_sent = 0;
		uint64_t frames_repeated = 0; /* queue underrun: last frame held */
		uint64_t audio_underruns = 0;
		size_t video_queue = 0, video_queue_capacity = 0;
		size_t audio_queue_ms = 0;
		/* Epochs the transport sent nothing in because no frame was
		 * queued -- the transmit thread was late. Compensated for, but
		 * each one is a visible/audible hiccup. */
		uint64_t video_epochs_skipped = 0;
		uint64_t audio_gap_ms = 0;
		/* Audio minus video at the sender for the current item, from
		 * the PTP times MTL reports for the first frame of each.
		 * Positive = audio later. */
		bool av_skew_valid = false;
		double av_skew_ms = 0;
		bool senders_up = false;
		std::string video_sdp_dest, audio_sdp_dest;
	};
	Status status() const;

	const PlayerConfig &config() const { return config_; }
	/* ST 2110-21 pacing the video session is created with: the configured
	 * one, or what "auto" comes to on this device. This is what an SDP
	 * for the stream has to declare as TP. */
	enum st21_pacing video_pacing() const;
	/* Apply a new configuration. A change of transport only (destination,
	 * payload type, an essence switched on or off -- what an IS-05
	 * activation does) restarts the senders and nothing else: the playlist
	 * keeps running and picks up on the new sessions. A change of raster,
	 * rate or channel count stops playout; the material has to be decoded
	 * differently for it. */
	void reconfigure(const PlayerConfig &config);

	Preview *preview() { return &preview_; }
	const AudioMeters *meters() const { return &meters_; }

private:
	bool start_sessions();
	void stop_sessions();
	void start_threads();
	void join_threads();
	void video_tx_loop();
	void audio_tx_loop();
	void start_current_item();
	void advance();
	void seek_locked(int64_t position_ns);
	static int on_video_done(void *priv, struct st_frame *frame);
	static int on_audio_done(void *priv, struct st30_frame *frame);

	PlayerConfig config_;
	mutable std::mutex mutex_;

	st20p_tx_handle v_handle_ = nullptr;
	st30p_tx_handle a_handle_ = nullptr;
	/* MTL keeps the name pointer, so it has to outlive the create call */
	std::string v_session_name_, a_session_name_;

	BoundedQueue<VideoFrame> vq_;
	AudioFifo afifo_;
	std::unique_ptr<FileDecoder> decoder_;

	std::thread v_thread_, a_thread_;
	std::atomic<bool> tx_running_{false};
	std::atomic<State> state_{State::Idle};
	std::atomic<bool> loop_{false};
	/* --- lip sync ----------------------------------------------------
	 * flowing_: both queues are primed and both transmit threads take from
	 * them. Set by the video thread, so sound and picture start on the same
	 * decision instead of each whenever its own queue first has data.
	 * The debts are what an underrun cost: a held frame or padded silence
	 * pushes that essence late for good unless the same amount is skipped
	 * once the decoder has caught up. */
	std::atomic<bool> flowing_{false};
	std::atomic<bool> preroll_blocked_{false}; /* a seek is being carried out */
	std::atomic<int64_t> shown_pts_ns_{0};     /* picture currently on the wire */
	std::atomic<bool> item_has_audio_{false};
	std::atomic<bool> item_audio_done_{false}; /* raised by the decoder */
	uint64_t preroll_started_ns_ = 0;
	uint32_t video_debt_ = 0;           /* video thread only */
	size_t audio_debt_bytes_ = 0;       /* audio thread only */
	std::atomic<int64_t> audio_trim_bytes_{0}; /* >0 lead-in silence, <0 skip */
	std::atomic<uint64_t> flow_start_ns_{0};   /* when the video thread released */

	/* --- what the transport reports back ------------------------------
	 * MTL calls notify_frame_done with the PTP time each frame actually
	 * went out at. Two things are read off that:
	 *  - a gap between consecutive frames means the transport had nothing
	 *    to send for an epoch (we were late). The content after the gap is
	 *    late by exactly that much, so the same amount is skipped;
	 *  - the times of the first video frame and the first audio block of
	 *    an item give the real A/V skew at the sender.
	 * The callbacks run on an MTL thread: atomics only, no locks. */
	uint64_t v_last_done_tai_ = 0, a_last_done_tai_ = 0; /* callback thread */
	std::atomic<uint32_t> video_slip_frames_{0};
	std::atomic<int64_t> audio_slip_ns_{0};
	std::atomic<uint64_t> video_epochs_skipped_{0};
	std::atomic<uint64_t> audio_gap_ns_total_{0};
	std::atomic<uint64_t> v_put_seq_{0}, v_done_seq_{0}, v_anchor_seq_{0};
	std::atomic<uint64_t> a_put_seq_{0}, a_done_seq_{0}, a_anchor_seq_{0};
	std::atomic<uint64_t> v_anchor_tai_{0}, a_anchor_tai_{0};
	std::atomic<uint64_t> a_anchor_lag_ns_{0}; /* skipped at the head of that block */
	uint64_t audio_block_ns_ = 10 * 1000 * 1000;

	std::atomic<uint64_t> frames_sent_{0};
	std::atomic<uint64_t> frames_repeated_{0};
	std::atomic<uint64_t> audio_underruns_{0};

	std::vector<PlaylistItem> playlist_;
	int current_index_ = -1;

	/* Last picture sent, held when the queue runs dry so the stream keeps
	 * its cadence instead of stuttering. */
	VideoFrame held_frame_;
	bool have_held_ = false;
	/* set by stop(), acted on by the video tx thread */
	std::atomic<bool> blank_requested_{false};

	Preview preview_;
	AudioMeters meters_;
};

} // namespace vcard
