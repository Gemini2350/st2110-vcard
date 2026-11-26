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
#include <vector>

namespace vcard_nmos {

enum class EssenceKind { VideoRaw, AudioL24 };

struct VideoFormat {
	uint32_t width = 1920;
	uint32_t height = 1080;
	/* The MTL session rate: frames per second, or *fields* per second
	 * when interlaced (1080i50 is 50/1 here, as in MTL). The node turns
	 * that into the frame rate IS-04 and the SDP ask for. */
	uint32_t fps_num = 50;
	uint32_t fps_den = 1;
	bool interlaced = false;
	int bit_depth = 10; /* 8 or 10 */
	/* ST 2110-21 sender type the session is paced as; declared in the
	 * SDP as TP=2110TPN / TPNL / TPW. Senders only. */
	enum class Pacing { Narrow, NarrowLinear, Wide };
	Pacing pacing = Pacing::Narrow;
};

struct AudioFormat {
	uint16_t channels = 2;
	uint32_t sample_rate = 48000;
	int bit_depth = 24;
	/* packet time in microseconds (1000 = 1ms, 125 = 125us) */
	uint32_t ptime_us = 1000;
};

/* Subset of IS-05 v1.1 transport params relevant for st2110 over RTP.
 *
 * On an ST 2022-7 endpoint IS-05 has one set of these per leg. The sessions
 * in this daemon use one destination for both legs (same group and port on
 * either media port), so an activation hands over the primary leg's values;
 * the node logs a warning when the controller asked for something else on
 * the secondary leg. */
struct TransportParams {
	std::string source_ip;   /* rx: source filter. Not used for senders,
				  * their source is the media port's own IP */
	std::string dest_ip;     /* multicast group or unicast destination */
	uint16_t dest_port = 0;
	/* RTP payload type; 0 = not signalled.
	 * Receiver activation: read from the SDP the controller sent as
	 * transport_file. An activation by transport params alone carries no
	 * payload type -- keep the configured one then.
	 * Sender: only meaningful in add_sender()'s initial_params, where it
	 * is the configured payload type the SDP advertises (0 = 96 video /
	 * 97 audio). IS-05 cannot change it, so activations leave it 0. */
	uint8_t payload_type = 0;
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
	 *
	 * interface_ips: local IP of each media port the session runs on,
	 * primary first. One entry is a plain endpoint; two make it an
	 * ST 2022-7 endpoint with two IS-05 legs (and two m= sections in a
	 * sender's SDP). They are what "auto" resolves to for a sender's
	 * source_ip and a receiver's interface_ip.
	 *
	 * initial_params for senders seeds the advertised transport params
	 * (destination, same on every leg) and the SDP payload type. */
	std::string add_receiver(EssenceKind kind, const std::string &label,
				 const VideoFormat &vfmt, const AudioFormat &afmt,
				 const std::vector<std::string> &interface_ips,
				 ActivationCb cb);
	std::string add_sender(EssenceKind kind, const std::string &label,
			       const VideoFormat &vfmt, const AudioFormat &afmt,
			       const std::vector<std::string> &interface_ips,
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
