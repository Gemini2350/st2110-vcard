/* SPDX-License-Identifier: GPL-2.0-or-later
 * Embedded NMOS IS-04 (registration) / IS-05 (connection management) node.
 *
 * One node per daemon. Recorders register NMOS *receivers*, players register
 * NMOS *senders*. IS-05 activations from a broadcast controller arrive on
 * nmos-cpp's server threads and are forwarded to the owning session through
 * the ActivationCb, which restarts the MTL session with the new transport
 * parameters -- so the card can be patched from a controller exactly like a
 * hardware gateway.
 *
 * Ported from obs-mtl-st2110; both projects should eventually share this file
 * as one library rather than keeping two copies in step by hand.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace vcard_nmos {

enum class EssenceKind { VideoRaw, AudioL24 };

struct VideoFormat {
	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t fps_num = 50;
	uint32_t fps_den = 1;
	bool interlaced = false;
	int bit_depth = 10; /* 8 or 10 */
};

struct AudioFormat {
	uint16_t channels = 2;
	uint32_t sample_rate = 48000;
	int bit_depth = 24;
	/* packet time in microseconds (1000 = 1ms, 125 = 125us) */
	uint32_t ptime_us = 1000;
};

/* Subset of IS-05 v1.1 transport params relevant for st2110 over RTP */
struct TransportParams {
	std::string source_ip;   /* rx: source filter; tx: our interface ip */
	std::string dest_ip;     /* multicast group or unicast destination */
	uint16_t dest_port = 0;
	uint8_t payload_type = 96;
	bool rtp_enabled = false;
};

/* Called on IS-05 activation. Runs on an nmos-cpp thread — implementations
 * must marshal to their own thread before touching OBS/MTL state that is not
 * thread-safe. */
using ActivationCb = std::function<void(const TransportParams &params)>;

struct NodeConfig {
	std::string node_label = "ST 2110 Virtual Card";
	std::string description = "File playout and capture";
	/* 0 = pick defaults (IS-04 node API 3212, IS-05 connection API 3215) */
	int http_port = 0;
	/* empty = discover registry via mDNS/DNS-SD */
	std::string registry_address;
	int registry_port = 0;
	/* Address advertised in IS-04. This is the management NIC: once the
	 * media port is bound to vfio-pci the kernel has no interface for it,
	 * so its IP cannot serve the control plane. */
	std::string host_address;
	int logging_level = 0;
};

class NmosNode {
public:
	/* Start the embedded node. Returns nullptr on failure; the daemon
	 * carries on without NMOS in that case. */
	static NmosNode *start(const NodeConfig &config);
	static NmosNode *instance();
	static void stop();

	/* Register endpoints. Returns the NMOS resource id (uuid string).
	 * initial_params for senders seeds the advertised transport params /
	 * SDP; for receivers it seeds the staged params. */
	std::string add_receiver(EssenceKind kind, const std::string &label,
				 const VideoFormat &vfmt, const AudioFormat &afmt,
				 ActivationCb cb);
	std::string add_sender(EssenceKind kind, const std::string &label,
			       const VideoFormat &vfmt, const AudioFormat &afmt,
			       const TransportParams &initial_params,
			       ActivationCb cb);
	void remove_resource(const std::string &id);

	/* Update sender status (active/inactive) shown in IS-04 */
	void set_sender_active(const std::string &id, bool active);

	~NmosNode();

private:
	NmosNode() = default;
	struct Impl;
	Impl *impl_ = nullptr;
};

} // namespace vcard_nmos
