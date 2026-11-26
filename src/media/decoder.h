/* SPDX-License-Identifier: GPL-2.0-or-later
 * File -> yuv422p10le frames + L24BE audio, on its own thread.
 *
 * The decoder conforms whatever the file happens to be (rate, size, pixel
 * format, sample rate, channel count) to the one format the ST 2110 session
 * runs at. Frame rate is conformed by repeating/dropping against the session
 * grid, so a 25p file on a 50p session plays at the right *duration* rather
 * than at double speed.
 */
#pragma once

#include "core/media-buffer.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace vcard {

struct DecoderTarget {
	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t fps_num = 50;
	uint32_t fps_den = 1;
	uint16_t channels = 2;
	uint32_t sample_rate = 48000;
	/* false: ignore the file's audio altogether. Nobody would take it off
	 * the queue, and a full audio queue stalls the video with it. */
	bool audio = true;
};

class FileDecoder {
public:
	/* audio_done, if given, is raised once the file has delivered all the
	 * audio it has: an empty audio queue after that is the end of the
	 * item, not an underrun. */
	FileDecoder(BoundedQueue<VideoFrame> &vq, AudioFifo &afifo, DecoderTarget target,
		    std::atomic<bool> *audio_done = nullptr);
	~FileDecoder();

	/* Probe a file without starting playback. Returns false if it has no
	 * usable video stream. */
	struct FileInfo {
		bool ok = false;
		std::string error;
		uint32_t width = 0, height = 0;
		double fps = 0;
		double duration_s = 0;
		int audio_channels = 0;
		int audio_sample_rate = 0;
		std::string video_codec, audio_codec, pix_fmt;
	};
	static FileInfo probe(const std::string &path);

	bool start(const std::string &path, bool loop);
	void stop();
	bool running() const { return running_; }

	/* Position on the media timeline, in nanoseconds. */
	int64_t position_ns() const { return position_ns_; }
	int64_t duration_ns() const { return duration_ns_; }
	uint64_t frames_emitted() const { return frames_emitted_; }
	bool finished() const { return finished_; }
	/* Whether the current file carries audio we can decode. Known as soon
	 * as start() returns; the player uses it to decide whether an empty
	 * audio queue is an underrun or simply a silent file. */
	bool has_audio() const { return has_audio_; }

	/* Seek request, honoured at the next decode step. seek_serial() goes
	 * up once the queues have been flushed and decoding continues from
	 * the new position. */
	void seek(int64_t position_ns);
	uint64_t seek_serial() const { return seek_serial_; }

private:
	void run(std::string path, bool loop);
	bool decode_file(const std::string &path);

	BoundedQueue<VideoFrame> &vq_;
	AudioFifo &afifo_;
	DecoderTarget target_;
	std::atomic<bool> *audio_done_ = nullptr;

	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> finished_{false};
	std::atomic<int64_t> position_ns_{0};
	std::atomic<int64_t> duration_ns_{0};
	std::atomic<int64_t> seek_request_{-1};
	std::atomic<uint64_t> frames_emitted_{0};
	std::atomic<bool> has_audio_{false};
	std::atomic<uint64_t> seek_serial_{0};
	/* Audio samples a looped file delivered beyond its video length; taken
	 * off the head of the next pass so loops cannot drift apart. */
	int64_t loop_audio_excess_ = 0;
};

} // namespace vcard
