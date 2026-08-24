/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/mtl-device.h"

#include "core/formats.h"
#include "core/log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

namespace vcard {

namespace {
std::mutex g_mutex;
std::unique_ptr<MtlDevice> g_instance;
/* mtl_init_params keeps the pointer, so the string has to outlive the call */
std::string g_lcores_store;

/* --- PHC clock source (ptp4l/phc2sys discipline the NIC clock) ------------- */
int g_phc_fd = -1;

/* Turn a /dev/ptpN fd into a POSIX dynamic clock id (see linux/ptp_clock.h) */
inline clockid_t phc_clockid(int fd) { return (clockid_t)((~(int)fd << 3) | 3); }

uint64_t phc_get_time(void * /*priv*/)
{
	if (g_phc_fd < 0)
		return 0;
	struct timespec ts;
	if (clock_gettime(phc_clockid(g_phc_fd), &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* --- built-in PTP client status -------------------------------------------- */
std::atomic<int64_t> g_ptp_delta{0};
std::atomic<uint64_t> g_ptp_last_sync{0};

void ptp_sync_notify(void * /*priv*/, struct mtl_ptp_sync_notify_meta *meta)
{
	if (!meta)
		return;
	g_ptp_delta.store(meta->delta, std::memory_order_relaxed);
	g_ptp_last_sync.store(monotonic_ns(), std::memory_order_relaxed);
}
} // namespace

MtlDevice::MtlDevice(const Params &params) : params_(params)
{
	struct mtl_init_params p;
	memset(&p, 0, sizeof(p));

	snprintf(p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", params.port.c_str());
	p.pmd[MTL_PORT_P] = mtl_pmd_by_port_name(p.port[MTL_PORT_P]);
	p.net_proto[MTL_PORT_P] = MTL_PROTO_STATIC;

	/* On the kernel and AF_XDP backends the interface still belongs to the
	 * kernel, so MTL reads the address off it -- configuring one here would
	 * only be a second place to get it wrong. Those backends also give up
	 * hardware pacing, which is the whole point of ST 2110-21, so say so
	 * loudly rather than let someone ship a facility on it. */
	const bool kernel_path = p.pmd[MTL_PORT_P] == MTL_PMD_KERNEL_SOCKET ||
				 p.pmd[MTL_PORT_P] == MTL_PMD_NATIVE_AF_XDP;
	if (kernel_path) {
		VLOG_W("[mtl] '%s' uses the kernel data path: no ST 2110-21 narrow "
		       "pacing, limited bandwidth. Fine for bring-up and 1080p "
		       "tests, not for a facility.", params.port.c_str());
		if (!params.local_ip.empty())
			VLOG_I("[mtl] ignoring local_ip '%s' -- the address comes "
			       "from the interface on this backend",
			       params.local_ip.c_str());
	} else {
		if (!parse_ip(params.local_ip, p.sip_addr[MTL_PORT_P])) {
			VLOG_E("[mtl] invalid local IP '%s'", params.local_ip.c_str());
			return;
		}
		if (!params.netmask.empty())
			parse_ip(params.netmask, p.netmask[MTL_PORT_P]);
		if (!params.gateway.empty())
			parse_ip(params.gateway, p.gateway[MTL_PORT_P]);
	}
	p.tx_queues_cnt[MTL_PORT_P] = params.tx_queues;
	p.rx_queues_cnt[MTL_PORT_P] = params.rx_queues;
	p.num_ports = 1;

	if (!params.port_r.empty()) {
		snprintf(p.port[MTL_PORT_R], MTL_PORT_MAX_LEN, "%s",
			 params.port_r.c_str());
		p.pmd[MTL_PORT_R] = mtl_pmd_by_port_name(p.port[MTL_PORT_R]);
		p.net_proto[MTL_PORT_R] = MTL_PROTO_STATIC;
		if (p.pmd[MTL_PORT_R] != MTL_PMD_KERNEL_SOCKET &&
		    p.pmd[MTL_PORT_R] != MTL_PMD_NATIVE_AF_XDP) {
			if (!parse_ip(params.local_ip_r, p.sip_addr[MTL_PORT_R])) {
				VLOG_E("[mtl] invalid redundant local IP '%s'",
				       params.local_ip_r.c_str());
				return;
			}
			if (!params.netmask.empty())
				parse_ip(params.netmask, p.netmask[MTL_PORT_R]);
		}
		p.tx_queues_cnt[MTL_PORT_R] = params.tx_queues;
		p.rx_queues_cnt[MTL_PORT_R] = params.rx_queues;
		p.num_ports = 2;
	}

	p.log_level = (enum mtl_log_level)params.log_level;
	p.flags = MTL_FLAG_BIND_NUMA;

	/* Two ways to get PTP time, and they are mutually exclusive:
	 *
	 *  a) MTL's built-in PTP client (MTL_FLAG_PTP_ENABLE). It speaks
	 *     PTP itself on the media port and disciplines its own clock.
	 *     No ptp4l needed, but it also does not feed the system clock.
	 *  b) ptp4l + phc2sys own the NIC PHC and we hand MTL a callback that
	 *     reads it. Preferred when the box already runs a PTP stack, or
	 *     when other software on the machine must share the same clock. */
	if (!params.phc_device.empty()) {
		g_phc_fd = open(params.phc_device.c_str(), O_RDONLY);
		if (g_phc_fd < 0) {
			VLOG_E("[mtl] cannot open PHC '%s' -- is ptp4l running "
			       "on this NIC?", params.phc_device.c_str());
			return;
		}
		if (!phc_get_time(nullptr)) {
			VLOG_E("[mtl] PHC '%s' does not return a time",
			       params.phc_device.c_str());
			return;
		}
		p.ptp_get_time_fn = phc_get_time;
	} else if (params.ptp) {
		p.flags |= MTL_FLAG_PTP_ENABLE;
		p.flags |= MTL_FLAG_PTP_PI; /* PI servo, much better holdover */
		p.ptp_sync_notify = ptp_sync_notify;
	}

	if (!params.lcores.empty()) {
		g_lcores_store = params.lcores;
		p.lcores = g_lcores_store.data();
	}

	handle_ = mtl_init(&p);
	if (!handle_) {
		VLOG_E("[mtl] mtl_init failed for port %s -- check hugepages, "
		       "vfio binding and that no other process holds the NIC",
		       params.port.c_str());
		return;
	}
	VLOG_I("[mtl] up on %s (%s%s), clock source: %s", params.port.c_str(),
	       kernel_path ? "kernel data path" : params.local_ip.c_str(),
	       params.port_r.empty() ? "" : ", 2022-7 redundant",
	       clock_source_name().c_str());
}

MtlDevice::~MtlDevice()
{
	if (handle_) {
		mtl_uninit(handle_);
		VLOG_I("[mtl] device released");
	}
	if (g_phc_fd >= 0) {
		close(g_phc_fd);
		g_phc_fd = -1;
	}
}

bool MtlDevice::init(const Params &params)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_instance)
		return true;
	auto dev = std::unique_ptr<MtlDevice>(new MtlDevice(params));
	if (!dev->handle())
		return false;
	g_instance = std::move(dev);
	return true;
}

void MtlDevice::shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_instance.reset();
}

MtlDevice *MtlDevice::instance() { return g_instance.get(); }

std::string MtlDevice::clock_source_name() const
{
	if (!params_.phc_device.empty())
		return "NIC PHC " + params_.phc_device + " (ptp4l)";
	if (params_.ptp)
		return "MTL built-in PTP client";
	return "system TSC (NOT PTP locked)";
}

uint64_t MtlDevice::ptp_time_ns() const
{
	if (!handle_)
		return 0;
	return mtl_ptp_read_time(handle_);
}

MtlDevice::PtpStatus MtlDevice::ptp_status() const
{
	PtpStatus s;
	if (!handle_)
		return s;
	s.tai_ns = mtl_ptp_read_time(handle_);
	s.source = clock_source_name();

	if (!params_.phc_device.empty()) {
		/* ptp4l owns the discipline; we only report that the clock is
		 * advancing. Real PTP health belongs in ptp4l's own telemetry. */
		s.locked = s.tai_ns != 0;
		s.offset_ns = 0;
		s.have_offset = false;
		return s;
	}

	/* Built-in client: the last sync's phc delta is the servo error. No
	 * sync for 5 s means the grandmaster is gone (or IGMP for the PTP
	 * group was never joined) -- report unlocked rather than stale. */
	uint64_t last = g_ptp_last_sync.load(std::memory_order_relaxed);
	uint64_t age = last ? (monotonic_ns() - last) : UINT64_MAX;
	s.offset_ns = g_ptp_delta.load(std::memory_order_relaxed);
	s.have_offset = last != 0;
	s.last_sync_age_ms = age == UINT64_MAX ? -1 : (int64_t)(age / 1000000);
	s.locked = age < 5000000000ull && llabs(s.offset_ns) < 1000000ll; /* < 1 ms */
	return s;
}

} // namespace vcard
