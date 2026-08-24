/* SPDX-License-Identifier: GPL-2.0-or-later
 * yuv422p10le frames + L24BE audio -> file, on its own thread.
 *
 * Codec choice is the whole game on the record side. Uncompressed UHDp50 is
 * ~1.3 GB/s to disk; ProRes 422 HQ is ~90 MB/s and encodes in real time on a
 * decent CPU. The default is ProRes for that reason, with v210 available when
 * a bit-exact capture of the wire is what you actually want.
 */
#pragma once

#include "core/media-buffer.h"

#include <atomic>
#include <string>
#include <thread>

namespace vcard {

enum class RecordCodec {
	ProRes422HQ, /* prores_ks, 10 bit 4:2:2, ~real time, sane file size */
	ProRes4444,
	FFV1,        /* lossless, slower, big */
	V210,        /* uncompressed 10 bit 4:2:2 -- bit-exact, needs NVMe */
	H264,        /* proxy / review copy only */
};

RecordCodec parse_record_codec(const std::string &s);
const char *record_codec_name(RecordCodec c);
const char *record_codec_extension(RecordCodec c);

struct EncoderTarget {
	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t fps_num = 50;
	uint32_t fps_den = 1;
	uint16_t channels = 2;
	uint32_t sample_rate = 48000;
	RecordCodec codec = RecordCodec::ProRes422HQ;
	/* Container is derived from the codec unless overridden. */
	std::string container;
};

class FileEncoder {
public:
	FileEncoder(BoundedQueue<VideoFrame> &vq, AudioFifo &afifo, EncoderTarget target);
	~FileEncoder();

	bool start(const std::string &path);
	void stop();
	bool running() const { return running_; }

	const std::string &path() const { return path_; }
	uint64_t frames_written() const { return frames_written_; }
	uint64_t bytes_written() const { return bytes_written_; }
	const std::string &last_error() const { return last_error_; }

private:
	void run();

	BoundedQueue<VideoFrame> &vq_;
	AudioFifo &afifo_;
	EncoderTarget target_;
	std::string path_;
	std::string last_error_;

	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<uint64_t> frames_written_{0};
	std::atomic<uint64_t> bytes_written_{0};
};

} // namespace vcard
