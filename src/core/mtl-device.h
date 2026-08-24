/* SPDX-License-Identifier: GPL-2.0-or-later
 * One MTL instance (mtl_handle) per process; every player and recorder
 * session shares it. mtl_init/mtl_uninit are expensive (DPDK EAL init), so the
 * handle is created once at startup and torn down at shutdown.
 */
#pragma once

#include <mtl/mtl_api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vcard {

class MtlDevice {
public:
	struct Params {
		/* PCIe BDF (0000:4b:01.0) for the DPDK PMD, or "kernel:eth0" /
		 * "native_af_xdp:eth0" for the socket-based backends. The
		 * kernel backends are for bring-up only -- they will not hold
		 * ST 2110-21 narrow pacing at full rate. */
		std::string port;
		std::string local_ip;
		std::string netmask;
		std::string gateway;
		/* Redundant second port for ST 2022-7. Empty = single path. */
		std::string port_r;
		std::string local_ip_r;

		std::string lcores;   /* e.g. "4,5,6,7"; empty = MTL picks */
		int log_level = MTL_LOG_LEVEL_NOTICE;
		/* Clock source. If phc_device is set (e.g. "/dev/ptp0") MTL
		 * takes its time from the NIC PHC that ptp4l disciplines;
		 * otherwise MTL's own PTP client is used when ptp is true.
		 * With neither, timing is free-running -- fine for a bench
		 * loopback, never for a real facility. */
		bool ptp = true;
		std::string phc_device;
		uint16_t tx_queues = 16;
		uint16_t rx_queues = 16;
	};

	static bool init(const Params &params);
	static void shutdown();
	static MtlDevice *instance();

	mtl_handle handle() const { return handle_; }
	const Params &params() const { return params_; }
	bool redundant() const { return !params_.port_r.empty(); }
	int num_ports() const { return redundant() ? 2 : 1; }

	/* True on the kernel socket / AF_XDP backends. Those have no hardware
	 * rate limiter, so MTL paces from the TSC and the stream cannot honestly
	 * be declared narrow (ST 2110-21 TP=2110TPN). */
	bool kernel_data_path() const { return kernel_data_path_; }

	/* Current PTP time in nanoseconds (TAI). 0 if unavailable. */
	uint64_t ptp_time_ns() const;

	struct PtpStatus {
		bool locked = false;
		bool have_offset = false;
		int64_t offset_ns = 0;      /* servo error of the last sync */
		int64_t last_sync_age_ms = -1;
		uint64_t tai_ns = 0;
		std::string source;
	};
	PtpStatus ptp_status() const;
	std::string clock_source_name() const;

	~MtlDevice();

private:
	explicit MtlDevice(const Params &params);

	mtl_handle handle_ = nullptr;
	Params params_;
	bool kernel_data_path_ = false;
};

} // namespace vcard
