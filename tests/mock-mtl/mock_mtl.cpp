/* SPDX-License-Identifier: GPL-2.0-or-later
 * Software stand-in for libmtl, for running the daemon without a NIC.
 *
 * Implements exactly the subset of the MTL API that st2110-vcard calls, as an
 * in-process loopback: a TX session paces frames out on a timer at the session
 * rate and hands them to every RX session listening on the same ip:port. No
 * packets, no DPDK, no hugepages.
 *
 * It deliberately mirrors the MTL behaviours the daemon's design leans on:
 *   - nothing moves until the device is started (mtl_start or
 *     MTL_FLAG_DEV_AUTO_START_STOP) -- a daemon that forgets this is silent on
 *     real hardware, so it has to be silent here too;
 *   - BLOCK_GET hands out a buffer only when one is free, times out after 1 s
 *     and then returns NULL;
 *   - a TX epoch with no frame queued sends nothing;
 *   - an RX session with no free buffer drops the incoming frame;
 *   - notify_frame_done reports the PTP time the frame went out at.
 *
 * Fault injection, for tests: SIGUSR1 makes every video sender skip three
 * epochs, SIGUSR2 makes every audio sender skip four buffers -- what the real
 * transport does when the application was late with a frame.
 *
 * What it cannot tell you: anything about pacing compliance, PTP, IGMP, packet
 * loss or throughput. Those need the real library and a real network.
 */
#include <mtl/mtl_api.h>
#include <mtl/st30_pipeline_api.h>
#include <mtl/st_pipeline_api.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_started{false};
std::atomic<int> g_skip_video{0};
std::atomic<int> g_skip_audio{0};

void on_usr1(int) { g_skip_video = 3; }
void on_usr2(int) { g_skip_audio = 4; }
std::mutex g_bus_mutex;

uint64_t tai_now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ((uint64_t)ts.tv_sec + 37) * 1000000000ull + ts.tv_nsec;
}

uint32_t rtp_of(uint64_t tai_ns, uint32_t rate)
{
	return (uint32_t)((__uint128_t)tai_ns * rate / 1000000000ull);
}

std::string key_of(const uint8_t ip[MTL_IP_ADDR_LEN], uint16_t port)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);
	return buf;
}

double fps_of(enum st_fps fps)
{
	switch (fps) {
	case ST_FPS_P23_98: return 24000.0 / 1001;
	case ST_FPS_P24: return 24;
	case ST_FPS_P25: return 25;
	case ST_FPS_P29_97: return 30000.0 / 1001;
	case ST_FPS_P30: return 30;
	case ST_FPS_P50: return 50;
	case ST_FPS_P59_94: return 60000.0 / 1001;
	case ST_FPS_P60: return 60;
	default: return 50;
	}
}

/* --- wakeable blocking primitive shared by all four session kinds ---------- */
struct Blocker {
	std::mutex m;
	std::condition_variable cv;
	bool wake_pending = false;

	template <typename Pred> bool wait(std::unique_lock<std::mutex> &lock, Pred ready)
	{
		cv.wait_for(lock, std::chrono::seconds(1),
			    [&] { return wake_pending || ready(); });
		wake_pending = false;
		return ready();
	}
	void wake()
	{
		std::lock_guard<std::mutex> l(m);
		wake_pending = true;
		cv.notify_all();
	}
};

/* --- video ------------------------------------------------------------------ */

struct VideoBuf {
	std::vector<uint8_t> data;
	struct st_frame frame {};
	enum { FREE, IN_USER, READY } state = FREE;
	uint64_t seq = 0;
};

void layout_frame(VideoBuf &b, uint32_t w, uint32_t h)
{
	size_t ly = (size_t)w * 2, lc = (size_t)(w / 2) * 2;
	b.data.resize(ly * h + 2 * lc * h);
	memset(&b.frame, 0, sizeof(b.frame));
	b.frame.addr[0] = b.data.data();
	b.frame.addr[1] = b.data.data() + ly * h;
	b.frame.addr[2] = b.data.data() + ly * h + lc * h;
	b.frame.linesize[0] = ly;
	b.frame.linesize[1] = lc;
	b.frame.linesize[2] = lc;
	b.frame.fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
	b.frame.width = w;
	b.frame.height = h;
	b.frame.buffer_size = b.data.size();
	b.frame.data_size = b.data.size();
}

struct VideoRx {
	struct st20p_rx_ops ops;
	std::string key;
	Blocker blk;
	std::deque<VideoBuf *> ready;
	std::map<struct st_frame *, VideoBuf *> out; /* handed to the app */
	bool detected = false;
	std::atomic<uint64_t> dropped{0};
};

std::multimap<std::string, VideoRx *> g_video_rx;

struct VideoTx {
	struct st20p_tx_ops ops;
	std::string key;
	Blocker blk;
	std::vector<VideoBuf> bufs;
	uint64_t next_seq = 0;
	std::thread pacer;
	std::atomic<bool> run{true};

	void pace()
	{
		auto period = std::chrono::nanoseconds((int64_t)(1e9 / fps_of(ops.fps)));
		auto next = Clock::now() + period;
		/* epochs are exact multiples of the period, like MTL's -- the
		 * scheduling jitter of this thread must not leak into them */
		const uint64_t tai0 = tai_now_ns();
		uint64_t tick = 0;
		while (run) {
			std::this_thread::sleep_until(next);
			next += period;
			tick++;
			/* Epochs follow the clock. If this thread was held up (a
			 * stopped process, a starved core), the ones it slept
			 * through are lost -- MTL does not send them late in a
			 * burst either. */
			for (auto now = Clock::now(); next + period < now; next += period)
				tick++;
			const uint64_t epoch_tai = tai0 + tick * (uint64_t)period.count();
			if (!g_started)
				continue;
			if (g_skip_video > 0) {
				g_skip_video--;
				continue;
			}

			VideoBuf *send = nullptr;
			{
				std::lock_guard<std::mutex> l(blk.m);
				for (auto &b : bufs)
					if (b.state == VideoBuf::READY &&
					    (!send || b.seq < send->seq))
						send = &b;
			}
			if (!send)
				continue; /* epoch with nothing queued: nothing sent */

			{
				std::lock_guard<std::mutex> bus(g_bus_mutex);
				auto range = g_video_rx.equal_range(key);
				for (auto it = range.first; it != range.second; ++it)
					deliver(it->second, *send, epoch_tai);
			}
			if (ops.notify_frame_done) {
				send->frame.tfmt = ST10_TIMESTAMP_FMT_TAI;
				send->frame.timestamp = epoch_tai;
				send->frame.epoch = epoch_tai / (uint64_t)period.count();
				ops.notify_frame_done(ops.priv, &send->frame);
			}
			{
				std::lock_guard<std::mutex> l(blk.m);
				send->state = VideoBuf::FREE;
			}
			blk.cv.notify_all();
		}
	}

	void deliver(VideoRx *rx, const VideoBuf &src, uint64_t epoch_tai)
	{
		if (!rx->detected && (rx->ops.flags & ST20P_RX_FLAG_AUTO_DETECT) &&
		    rx->ops.notify_detected) {
			struct st20_detect_meta meta {};
			meta.width = (int)ops.width;
			meta.height = (int)ops.height;
			meta.fps = ops.fps;
			meta.interlaced = ops.interlaced;
			struct st20_detect_reply reply {};
			rx->ops.notify_detected(rx->ops.priv, &meta, &reply);
		}
		rx->detected = true;

		std::lock_guard<std::mutex> l(rx->blk.m);
		if (rx->ready.size() + rx->out.size() >= rx->ops.framebuff_cnt) {
			rx->dropped++; /* no free buffer: frame is lost */
			return;
		}
		auto *b = new VideoBuf();
		layout_frame(*b, src.frame.width, src.frame.height);
		memcpy(b->data.data(), src.data.data(), src.data.size());
		/* as MTL: 32-bit RTP timestamp plus PTP time of arrival */
		b->frame.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
		b->frame.timestamp = rtp_of(epoch_tai, 90000);
		b->frame.receive_timestamp = tai_now_ns();
		b->frame.status = ST_FRAME_STATUS_COMPLETE;
		rx->ready.push_back(b);
		rx->blk.cv.notify_all();
	}
};

/* --- audio ------------------------------------------------------------------ */

struct AudioBuf {
	std::vector<uint8_t> data;
	struct st30_frame frame {};
	enum { FREE, IN_USER, READY } state = FREE;
	uint64_t seq = 0;
};

struct AudioRx {
	struct st30p_rx_ops ops;
	std::string key;
	Blocker blk;
	std::deque<AudioBuf *> ready;
	std::map<struct st30_frame *, AudioBuf *> out;
	std::atomic<uint64_t> dropped{0};
};

std::multimap<std::string, AudioRx *> g_audio_rx;

struct AudioTx {
	struct st30p_tx_ops ops;
	std::string key;
	Blocker blk;
	std::vector<AudioBuf> bufs;
	uint64_t next_seq = 0;
	std::thread pacer;
	std::atomic<bool> run{true};

	void pace()
	{
		double seconds = (double)ops.framebuff_size / (48000.0 * ops.channel * 3);
		auto period = std::chrono::nanoseconds((int64_t)(seconds * 1e9));
		auto next = Clock::now() + period;
		const uint64_t tai0 = tai_now_ns();
		uint64_t tick = 0;
		while (run) {
			std::this_thread::sleep_until(next);
			next += period;
			tick++;
			for (auto now = Clock::now(); next + period < now; next += period)
				tick++;
			const uint64_t epoch_tai = tai0 + tick * (uint64_t)period.count();
			if (!g_started)
				continue;
			if (g_skip_audio > 0) {
				g_skip_audio--;
				continue;
			}

			AudioBuf *send = nullptr;
			{
				std::lock_guard<std::mutex> l(blk.m);
				for (auto &b : bufs)
					if (b.state == AudioBuf::READY &&
					    (!send || b.seq < send->seq))
						send = &b;
			}
			if (!send)
				continue;

			{
				std::lock_guard<std::mutex> bus(g_bus_mutex);
				auto range = g_audio_rx.equal_range(key);
				for (auto it = range.first; it != range.second; ++it) {
					AudioRx *rx = it->second;
					std::lock_guard<std::mutex> l(rx->blk.m);
					if (rx->ready.size() + rx->out.size() >=
					    rx->ops.framebuff_cnt) {
						rx->dropped++;
						continue;
					}
					auto *b = new AudioBuf();
					b->data = send->data;
					b->frame = send->frame;
					b->frame.addr = b->data.data();
					b->frame.tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
					b->frame.timestamp = rtp_of(epoch_tai, 48000);
					b->frame.receive_timestamp = tai_now_ns();
					rx->ready.push_back(b);
					rx->blk.cv.notify_all();
				}
			}
			if (ops.notify_frame_done) {
				send->frame.tfmt = ST10_TIMESTAMP_FMT_TAI;
				send->frame.timestamp = epoch_tai;
				ops.notify_frame_done(ops.priv, &send->frame);
			}
			{
				std::lock_guard<std::mutex> l(blk.m);
				send->state = AudioBuf::FREE;
			}
			blk.cv.notify_all();
		}
	}
};

std::thread g_ptp_thread;
std::atomic<bool> g_ptp_run{false};

} // namespace

extern "C" {

/* --- device ----------------------------------------------------------------- */

mtl_handle mtl_init(struct mtl_init_params *p)
{
	fprintf(stderr, "[mock-mtl] init port=%s pmd=%d flags=0x%llx%s\n", p->port[0],
		(int)p->pmd[0], (unsigned long long)p->flags,
		(p->flags & MTL_FLAG_DEV_AUTO_START_STOP) ? " (auto start)" : "");
	g_started = (p->flags & MTL_FLAG_DEV_AUTO_START_STOP) != 0;
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);

	if ((p->flags & MTL_FLAG_PTP_ENABLE) && p->ptp_sync_notify) {
		auto notify = p->ptp_sync_notify;
		void *priv = p->priv;
		g_ptp_run = true;
		g_ptp_thread = std::thread([notify, priv] {
			while (g_ptp_run) {
				struct mtl_ptp_sync_notify_meta meta {};
				meta.master_utc_offset = 37;
				meta.delta = 120; /* ns */
				notify(priv, &meta);
				for (int i = 0; i < 10 && g_ptp_run; i++)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(100));
			}
		});
	}
	static int dummy;
	return (mtl_handle)&dummy;
}

int mtl_uninit(mtl_handle)
{
	g_started = false;
	g_ptp_run = false;
	if (g_ptp_thread.joinable())
		g_ptp_thread.join();
	return 0;
}

int mtl_start(mtl_handle)
{
	g_started = true;
	return 0;
}

int mtl_stop(mtl_handle)
{
	g_started = false;
	return 0;
}

uint64_t mtl_ptp_read_time(mtl_handle) { return tai_now_ns(); }
uint64_t mtl_ptp_read_time_raw(mtl_handle) { return tai_now_ns(); }

enum mtl_pmd_type mtl_pmd_by_port_name(const char *port)
{
	if (!strncmp(port, "kernel:", 7)) return MTL_PMD_KERNEL_SOCKET;
	if (!strncmp(port, "native_af_xdp:", 14)) return MTL_PMD_NATIVE_AF_XDP;
	if (!strncmp(port, "dpdk_af_xdp:", 12)) return MTL_PMD_DPDK_AF_XDP;
	return MTL_PMD_DPDK_USER;
}

int st30_calculate_framebuff_size(enum st30_fmt, enum st30_ptime ptime,
				  enum st30_sampling, uint16_t channel,
				  uint64_t desired_frame_time_ns, double *fps)
{
	uint64_t ptime_ns = 1000000;
	switch (ptime) {
	case ST30_PTIME_125US: ptime_ns = 125000; break;
	case ST30_PTIME_250US: ptime_ns = 250000; break;
	case ST30_PTIME_333US: ptime_ns = 333333; break;
	case ST30_PTIME_4MS: ptime_ns = 4000000; break;
	default: break;
	}
	uint64_t pkts = desired_frame_time_ns / ptime_ns;
	if (!pkts)
		return -1;
	uint64_t samples_per_pkt = 48000ull * ptime_ns / 1000000000ull;
	if (fps)
		*fps = 1e9 / (double)(pkts * ptime_ns);
	return (int)(pkts * samples_per_pkt * channel * 3);
}

/* --- st20p tx ---------------------------------------------------------------- */

st20p_tx_handle st20p_tx_create(mtl_handle, struct st20p_tx_ops *ops)
{
	auto *tx = new VideoTx();
	tx->ops = *ops;
	tx->key = key_of(ops->port.dip_addr[MTL_SESSION_PORT_P],
			 ops->port.udp_port[MTL_SESSION_PORT_P]);
	tx->bufs.resize(ops->framebuff_cnt);
	for (auto &b : tx->bufs)
		layout_frame(b, ops->width, ops->height);
	tx->pacer = std::thread(&VideoTx::pace, tx);
	fprintf(stderr, "[mock-mtl] st20p tx %s -> %s %ux%u\n", ops->name,
		tx->key.c_str(), ops->width, ops->height);
	return (st20p_tx_handle)tx;
}

int st20p_tx_free(st20p_tx_handle handle)
{
	auto *tx = (VideoTx *)handle;
	tx->run = false;
	tx->pacer.join();
	delete tx;
	return 0;
}

struct st_frame *st20p_tx_get_frame(st20p_tx_handle handle)
{
	auto *tx = (VideoTx *)handle;
	std::unique_lock<std::mutex> lock(tx->blk.m);
	auto find_free = [&]() -> VideoBuf * {
		for (auto &b : tx->bufs)
			if (b.state == VideoBuf::FREE)
				return &b;
		return nullptr;
	};
	VideoBuf *b = find_free();
	if (!b && (tx->ops.flags & ST20P_TX_FLAG_BLOCK_GET)) {
		tx->blk.wait(lock, [&] { return find_free() != nullptr; });
		b = find_free();
	}
	if (!b)
		return nullptr;
	b->state = VideoBuf::IN_USER;
	return &b->frame;
}

int st20p_tx_put_frame(st20p_tx_handle handle, struct st_frame *frame)
{
	auto *tx = (VideoTx *)handle;
	std::lock_guard<std::mutex> lock(tx->blk.m);
	for (auto &b : tx->bufs)
		if (&b.frame == frame) {
			b.state = VideoBuf::READY;
			b.seq = tx->next_seq++;
			return 0;
		}
	return -1;
}

int st20p_tx_wake_block(st20p_tx_handle handle)
{
	((VideoTx *)handle)->blk.wake();
	return 0;
}

/* --- st20p rx ---------------------------------------------------------------- */

st20p_rx_handle st20p_rx_create(mtl_handle, struct st20p_rx_ops *ops)
{
	auto *rx = new VideoRx();
	rx->ops = *ops;
	rx->key = key_of(ops->port.ip_addr[MTL_SESSION_PORT_P],
			 ops->port.udp_port[MTL_SESSION_PORT_P]);
	std::lock_guard<std::mutex> bus(g_bus_mutex);
	g_video_rx.emplace(rx->key, rx);
	fprintf(stderr, "[mock-mtl] st20p rx %s <- %s\n", ops->name, rx->key.c_str());
	return (st20p_rx_handle)rx;
}

int st20p_rx_free(st20p_rx_handle handle)
{
	auto *rx = (VideoRx *)handle;
	{
		std::lock_guard<std::mutex> bus(g_bus_mutex);
		for (auto it = g_video_rx.begin(); it != g_video_rx.end(); ++it)
			if (it->second == rx) {
				g_video_rx.erase(it);
				break;
			}
	}
	for (auto *b : rx->ready)
		delete b;
	delete rx;
	return 0;
}

struct st_frame *st20p_rx_get_frame(st20p_rx_handle handle)
{
	auto *rx = (VideoRx *)handle;
	std::unique_lock<std::mutex> lock(rx->blk.m);
	if (rx->ready.empty() && (rx->ops.flags & ST20P_RX_FLAG_BLOCK_GET))
		rx->blk.wait(lock, [&] { return !rx->ready.empty(); });
	if (rx->ready.empty())
		return nullptr;
	VideoBuf *b = rx->ready.front();
	rx->ready.pop_front();
	rx->out[&b->frame] = b;
	return &b->frame;
}

int st20p_rx_put_frame(st20p_rx_handle handle, struct st_frame *frame)
{
	auto *rx = (VideoRx *)handle;
	VideoBuf *b = nullptr;
	{
		std::lock_guard<std::mutex> lock(rx->blk.m);
		auto it = rx->out.find(frame);
		if (it == rx->out.end())
			return -1;
		b = it->second;
		rx->out.erase(it);
	}
	delete b;
	return 0;
}

int st20p_rx_wake_block(st20p_rx_handle handle)
{
	((VideoRx *)handle)->blk.wake();
	return 0;
}

/* --- st30p tx ---------------------------------------------------------------- */

st30p_tx_handle st30p_tx_create(mtl_handle, struct st30p_tx_ops *ops)
{
	auto *tx = new AudioTx();
	tx->ops = *ops;
	tx->key = key_of(ops->port.dip_addr[MTL_SESSION_PORT_P],
			 ops->port.udp_port[MTL_SESSION_PORT_P]);
	tx->bufs.resize(ops->framebuff_cnt);
	for (auto &b : tx->bufs) {
		b.data.resize(ops->framebuff_size);
		b.frame.addr = b.data.data();
		b.frame.fmt = ops->fmt;
		b.frame.channel = ops->channel;
		b.frame.sampling = ops->sampling;
		b.frame.ptime = ops->ptime;
		b.frame.buffer_size = ops->framebuff_size;
		b.frame.data_size = ops->framebuff_size;
	}
	tx->pacer = std::thread(&AudioTx::pace, tx);
	fprintf(stderr, "[mock-mtl] st30p tx %s -> %s %uch fb=%u\n", ops->name,
		tx->key.c_str(), ops->channel, ops->framebuff_size);
	return (st30p_tx_handle)tx;
}

int st30p_tx_free(st30p_tx_handle handle)
{
	auto *tx = (AudioTx *)handle;
	tx->run = false;
	tx->pacer.join();
	delete tx;
	return 0;
}

struct st30_frame *st30p_tx_get_frame(st30p_tx_handle handle)
{
	auto *tx = (AudioTx *)handle;
	std::unique_lock<std::mutex> lock(tx->blk.m);
	auto find_free = [&]() -> AudioBuf * {
		for (auto &b : tx->bufs)
			if (b.state == AudioBuf::FREE)
				return &b;
		return nullptr;
	};
	AudioBuf *b = find_free();
	if (!b && (tx->ops.flags & ST30P_TX_FLAG_BLOCK_GET)) {
		tx->blk.wait(lock, [&] { return find_free() != nullptr; });
		b = find_free();
	}
	if (!b)
		return nullptr;
	b->state = AudioBuf::IN_USER;
	return &b->frame;
}

int st30p_tx_put_frame(st30p_tx_handle handle, struct st30_frame *frame)
{
	auto *tx = (AudioTx *)handle;
	std::lock_guard<std::mutex> lock(tx->blk.m);
	for (auto &b : tx->bufs)
		if (&b.frame == frame) {
			b.state = AudioBuf::READY;
			b.seq = tx->next_seq++;
			return 0;
		}
	return -1;
}

int st30p_tx_wake_block(st30p_tx_handle handle)
{
	((AudioTx *)handle)->blk.wake();
	return 0;
}

/* --- st30p rx ---------------------------------------------------------------- */

st30p_rx_handle st30p_rx_create(mtl_handle, struct st30p_rx_ops *ops)
{
	auto *rx = new AudioRx();
	rx->ops = *ops;
	rx->key = key_of(ops->port.ip_addr[MTL_SESSION_PORT_P],
			 ops->port.udp_port[MTL_SESSION_PORT_P]);
	std::lock_guard<std::mutex> bus(g_bus_mutex);
	g_audio_rx.emplace(rx->key, rx);
	fprintf(stderr, "[mock-mtl] st30p rx %s <- %s\n", ops->name, rx->key.c_str());
	return (st30p_rx_handle)rx;
}

int st30p_rx_free(st30p_rx_handle handle)
{
	auto *rx = (AudioRx *)handle;
	{
		std::lock_guard<std::mutex> bus(g_bus_mutex);
		for (auto it = g_audio_rx.begin(); it != g_audio_rx.end(); ++it)
			if (it->second == rx) {
				g_audio_rx.erase(it);
				break;
			}
	}
	for (auto *b : rx->ready)
		delete b;
	delete rx;
	return 0;
}

struct st30_frame *st30p_rx_get_frame(st30p_rx_handle handle)
{
	auto *rx = (AudioRx *)handle;
	std::unique_lock<std::mutex> lock(rx->blk.m);
	if (rx->ready.empty() && (rx->ops.flags & ST30P_RX_FLAG_BLOCK_GET))
		rx->blk.wait(lock, [&] { return !rx->ready.empty(); });
	if (rx->ready.empty())
		return nullptr;
	AudioBuf *b = rx->ready.front();
	rx->ready.pop_front();
	rx->out[&b->frame] = b;
	return &b->frame;
}

int st30p_rx_put_frame(st30p_rx_handle handle, struct st30_frame *frame)
{
	auto *rx = (AudioRx *)handle;
	AudioBuf *b = nullptr;
	{
		std::lock_guard<std::mutex> lock(rx->blk.m);
		auto it = rx->out.find(frame);
		if (it == rx->out.end())
			return -1;
		b = it->second;
		rx->out.erase(it);
	}
	delete b;
	return 0;
}

int st30p_rx_wake_block(st30p_rx_handle handle)
{
	((AudioRx *)handle)->blk.wake();
	return 0;
}

} /* extern "C" */
