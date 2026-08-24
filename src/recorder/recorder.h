/* SPDX-License-Identifier: GPL-2.0-or-later
 * Capture: ST 2110-20 video + ST 2110-30 audio -> preview and/or file.
 *
 * The receiver runs as soon as the recorder is opened, whether or not
 * anything is being written to disk. That is deliberate: confidence
 * monitoring of an incoming stream is the thing you need most often, and it
 * should not require arming a recording first.
 *
 * The wire is never held up by the disk. If the encoder falls behind, frames
 * are dropped into a counter rather than backpressured into MTL's receive
 * path, where they would turn into packet loss for everyone.
 */
#pragma once

#include "core/media-buffer.h"
#include "media/encoder.h"
#include "monitor/meters.h"
#include "monitor/preview.h"

#include <mtl/st30_pipeline_api.h>
#include <mtl/st_pipeline_api.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vcard {

struct RecorderConfig {
	std::string name = "recorder1";

	bool video_enabled = true;
	/* Format of the expected stream. With auto_detect the width/height/fps
	 * below are only the fallback until the first packets arrive. */
	bool auto_detect = true;
	uint32_t width = 1920;
	uint32_t height = 1080;
	enum st_fps fps = ST_FPS_P50;
	bool interlaced = false;

	std::string video_source_ip;  /* IGMPv3 source filter, empty = ASM */
	std::string video_multicast_ip = "239.100.0.20";
	uint16_t video_port = 20000;
	uint8_t video_payload_type = 96;

	bool audio_enabled = true;
	uint16_t channels = 2;
	std::string audio_source_ip;
	std::string audio_multicast_ip = "239.100.0.21";
	uint16_t audio_port = 30000;
	uint8_t audio_payload_type = 97;
	enum st30_ptime ptime = ST30_PTIME_1MS;

	/* recording */
	std::string output_dir = "/var/lib/st2110-vcard/recordings";
	RecordCodec codec = RecordCodec::ProRes422HQ;
	uint32_t queue_frames = 12;
};

class Recorder {
public:
	explicit Recorder(RecorderConfig config);
	~Recorder();

	/* Join the groups and start receiving + monitoring. */
	bool open();
	void close();

	/* filename empty = auto (name + timestamp + codec extension) */
	bool start_recording(const std::string &filename);
	void stop_recording();
	bool recording() const { return recording_; }

	struct Status {
		bool receivers_up = false;
		bool signal_present = false;
		std::string detected_format;    /* "1920x1080p50" once locked */
		uint32_t width = 0, height = 0;
		double fps = 0;
		uint64_t frames_received = 0;
		uint64_t frames_incomplete = 0; /* packet loss on the wire */
		uint64_t frames_dropped = 0;    /* encoder could not keep up */
		uint64_t audio_frames_received = 0;
		int64_t last_frame_age_ms = -1;
		bool recording = false;
		std::string recording_path;
		uint64_t recorded_frames = 0;
		double recorded_seconds = 0;
		double recorded_mib = 0;
		std::string last_error;
		std::string video_source, audio_source;
	};
	Status status() const;

	const RecorderConfig &config() const { return config_; }
	void reconfigure(const RecorderConfig &config);

	Preview *preview() { return &preview_; }
	const AudioMeters *meters() const { return &meters_; }

private:
	bool start_sessions();
	void stop_sessions();
	void video_rx_loop();
	void audio_rx_loop();
	static int on_detected(void *priv, const struct st20_detect_meta *meta,
			       struct st20_detect_reply *reply);

	RecorderConfig config_;
	mutable std::mutex mutex_;

	st20p_rx_handle v_handle_ = nullptr;
	st30p_rx_handle a_handle_ = nullptr;
	std::string v_session_name_, a_session_name_;

	BoundedQueue<VideoFrame> vq_;
	AudioFifo afifo_;
	std::unique_ptr<FileEncoder> encoder_;
	std::string recording_path_;

	std::thread v_thread_, a_thread_;
	std::atomic<bool> rx_running_{false};
	std::atomic<bool> recording_{false};

	std::atomic<uint32_t> det_width_{0}, det_height_{0};
	std::atomic<int> det_fps_{-1};
	std::atomic<bool> det_interlaced_{false};

	std::atomic<uint64_t> frames_received_{0};
	std::atomic<uint64_t> frames_incomplete_{0};
	std::atomic<uint64_t> frames_dropped_{0};
	std::atomic<uint64_t> audio_frames_received_{0};
	std::atomic<uint64_t> last_frame_ns_{0};

	Preview preview_;
	AudioMeters meters_;
};

} // namespace vcard
