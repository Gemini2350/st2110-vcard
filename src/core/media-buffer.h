/* SPDX-License-Identifier: GPL-2.0-or-later
 * The buffers between the file layer and the wire layer.
 *
 * Design note -- who is the clock:
 *   MTL is. st20p_tx_get_frame with ST20P_TX_FLAG_BLOCK_GET returns exactly
 *   once per frame epoch, and st30p_tx_get_frame once per audio frame period,
 *   both derived from PTP. So the output threads are paced by the network,
 *   never by a sleep() and never by the file's own timebase. The decoder runs
 *   ahead into these queues and blocks when they are full. That is the whole
 *   trick: a file has no clock, PTP does, and the queue absorbs the difference.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace vcard {

/* One decoded picture in yuv422p10le -- identical layout to
 * ST_FRAME_FMT_YUV422PLANAR10LE, so it goes to the wire with a plain memcpy
 * per line (and no conversion at all when the strides match). */
struct VideoFrame {
	std::vector<uint8_t> data;
	size_t offset[3] = {0, 0, 0};
	size_t stride[3] = {0, 0, 0};
	uint32_t width = 0;
	uint32_t height = 0;
	int64_t pts_ns = 0;   /* position on the media timeline */
	bool eos = false;     /* sentinel: end of the current item */

	void alloc(uint32_t w, uint32_t h)
	{
		width = w;
		height = h;
		stride[0] = (size_t)w * 2;      /* Y : 16-bit container */
		stride[1] = (size_t)(w / 2) * 2; /* Cb: 4:2:2, half width */
		stride[2] = stride[1];           /* Cr */
		offset[0] = 0;
		offset[1] = stride[0] * h;
		offset[2] = offset[1] + stride[1] * h;
		data.resize(offset[2] + stride[2] * h);
	}
	uint8_t *plane(int i) { return data.data() + offset[i]; }
	const uint8_t *plane(int i) const { return data.data() + offset[i]; }
};

/* Bounded queue with backpressure. push() blocks while full, pop() blocks
 * while empty; both return false once stopped. */
template <typename T> class BoundedQueue {
public:
	explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

	bool push(T &&item)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		not_full_.wait(lock, [&] { return stopped_ || q_.size() < capacity_; });
		if (stopped_)
			return false;
		q_.push_back(std::move(item));
		not_empty_.notify_one();
		return true;
	}

	/* Non-blocking: returns false instead of waiting for room. */
	bool try_push(T &&item)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopped_ || q_.size() >= capacity_)
			return false;
		q_.push_back(std::move(item));
		not_empty_.notify_one();
		return true;
	}

	bool pop(T &out)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		not_empty_.wait(lock, [&] { return stopped_ || !q_.empty(); });
		if (q_.empty())
			return false;
		out = std::move(q_.front());
		q_.pop_front();
		not_full_.notify_one();
		return true;
	}

	/* Non-blocking: returns false when there is nothing to take. The
	 * transmit threads use only this -- a paced thread that waits on the
	 * queue is a paced thread that misses its epoch. */
	bool try_pop(T &out)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (q_.empty())
			return false;
		out = std::move(q_.front());
		q_.pop_front();
		not_full_.notify_one();
		return true;
	}

	/* Non-blocking peek at fill level, for the status API. */
	size_t size() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return q_.size();
	}
	size_t capacity() const { return capacity_; }

	void stop()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopped_ = true;
		not_empty_.notify_all();
		not_full_.notify_all();
	}

	void restart()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopped_ = false;
		q_.clear();
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		q_.clear();
		not_full_.notify_all();
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable not_empty_, not_full_;
	std::deque<T> q_;
	size_t capacity_;
	bool stopped_ = false;
};

/* Byte FIFO for audio. ST 2110-30 frames are a fixed number of packet times,
 * which never lines up with the decoder's frame sizes, so audio is handled as
 * a stream of L24BE bytes rather than as discrete blocks. Fixed-size ring:
 * no allocation on the audio path. */
class AudioFifo {
public:
	explicit AudioFifo(size_t capacity_bytes)
		: buf_(capacity_bytes + 1), capacity_(capacity_bytes)
	{
	}

	bool write(const uint8_t *src, size_t len)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		while (len) {
			not_full_.wait(lock, [&] { return stopped_ || used_ < capacity_; });
			if (stopped_)
				return false;
			size_t chunk = std::min(len, capacity_ - used_);
			for (size_t i = 0; i < chunk; i++) {
				buf_[head_] = src[i];
				head_ = (head_ + 1) % buf_.size();
			}
			used_ += chunk;
			src += chunk;
			len -= chunk;
			not_empty_.notify_one();
		}
		return true;
	}

	/* Non-blocking write: drops the block if it does not fit. Used on the
	 * RX path, where the wire must never be held up by a slow disk. */
	bool try_write(const uint8_t *src, size_t len)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopped_ || capacity_ - used_ < len)
			return false;
		for (size_t i = 0; i < len; i++) {
			buf_[head_] = src[i];
			head_ = (head_ + 1) % buf_.size();
		}
		used_ += len;
		not_empty_.notify_one();
		return true;
	}

	/* Fills exactly len bytes, padding with digital silence on underrun so
	 * the ST 2110-30 stream never gets a gap -- a hole in the RTP sequence
	 * is far worse for a downstream receiver than a few silent samples.
	 *
	 * Never blocks. It is called from the audio tx thread, which MTL has
	 * already paced; waiting here for a slow decoder would push the whole
	 * stream late instead of dropping the one thing that is missing. */
	bool read_exact(uint8_t *dst, size_t len, bool *underrun,
			size_t *missing = nullptr)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		size_t have = std::min(len, used_);
		if (missing)
			*missing = len - have;
		for (size_t i = 0; i < have; i++) {
			dst[i] = buf_[tail_];
			tail_ = (tail_ + 1) % buf_.size();
		}
		used_ -= have;
		if (have < len) {
			std::memset(dst + have, 0, len - have);
			if (underrun)
				*underrun = true;
		}
		not_full_.notify_all();
		return true;
	}

	/* Throws away up to len bytes from the read side; returns how many. */
	size_t discard(size_t len)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		size_t n = std::min(len, used_);
		tail_ = (tail_ + n) % buf_.size();
		used_ -= n;
		if (n)
			not_full_.notify_all();
		return n;
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		head_ = tail_ = used_ = 0;
		not_full_.notify_all();
	}

	size_t size() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return used_;
	}
	size_t capacity() const { return capacity_; }
	size_t free_space() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return capacity_ - used_;
	}

	void stop()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopped_ = true;
		not_empty_.notify_all();
		not_full_.notify_all();
	}
	void restart()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopped_ = false;
		head_ = tail_ = used_ = 0;
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable not_empty_, not_full_;
	std::vector<uint8_t> buf_;
	size_t capacity_;
	size_t head_ = 0, tail_ = 0, used_ = 0;
	bool stopped_ = false;
};

} // namespace vcard
