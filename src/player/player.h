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
		bool senders_up = false;
		std::string video_sdp_dest, audio_sdp_dest;
	};
	Status status() const;

	const PlayerConfig &config() const { return config_; }
	/* Applied on the next open(); a live change restarts the senders. */
	void reconfigure(const PlayerConfig &config);

	Preview *preview() { return &preview_; }
	const AudioMeters *meters() const { return &meters_; }

private:
	bool start_sessions();
	void stop_sessions();
	void video_tx_loop();
	void audio_tx_loop();
	void start_current_item();
	void advance();

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
