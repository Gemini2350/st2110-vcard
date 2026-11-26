/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Embedded NMOS node built on sony/nmos-cpp (Apache-2.0).
 *
 * NOTE ON API STABILITY: this file is modeled on nmos-cpp's own nmos-cpp-node
 * example (Development/nmos-cpp-node/node_implementation.cpp) and was last
 * checked against nmos-cpp master 16a03fc (2026-11-19): it passes a
 * syntax-only compile against those headers, but has not been linked or run.
 * nmos-cpp's helper names occasionally change between tags; this is the one
 * file expected to need small compile fixes when the pinned tag moves.
 *
 * Structure per endpoint:
 *  - sender:   source -> flow -> sender (+ connection sender), SDP served as
 *              the IS-05 transport file
 *  - receiver: receiver (+ connection receiver)
 * An endpoint on a redundant device (two media ports, ST 2022-7) has two
 * IS-05 legs; a sender's SDP then carries two m= sections grouped as DUP.
 * IS-05 staged/active handling, activation scheduling, registration and
 * heartbeat (IS-04) all come from nmos-cpp's node_server.
 */
#include "nmos-node.h"
#include "sdp-util.h"

#include "core/log.h"

#include <map>
#include <mutex>

#include <nmos/channels.h>
#include <nmos/clock_name.h>
#include <nmos/colorspace.h>
#include <nmos/format.h>
#include <nmos/id.h>
#include <nmos/log_gate.h>
#include <nmos/model.h>
#include <nmos/node_resource.h>
#include <nmos/node_resources.h>
#include <nmos/node_server.h>
#include <nmos/connection_resources.h>
#include <nmos/connection_api.h>
#include <nmos/interlace_mode.h>
#include <nmos/mdns.h>
#include <nmos/media_type.h>
#include <nmos/rational.h>
#include <nmos/sdp_utils.h>
#include <nmos/server.h>
#include <nmos/settings.h>
#include <nmos/transfer_characteristic.h>
#include <nmos/transport.h>
#include <sdp/json.h>
#include <sdp/sdp.h>

namespace vcard_nmos {

struct NmosNode::Impl {
	nmos::node_model model;
	nmos::experimental::log_model log_model;
	std::unique_ptr<nmos::experimental::log_gate> gate;
	std::unique_ptr<nmos::server> server;

	nmos::id node_id;
	nmos::id device_id;

	std::mutex mutex;
	std::map<nmos::id, ActivationCb> callbacks;
	/* essence of each receiver, to pick the right m= section of its SDP */
	std::map<nmos::id, EssenceKind> receiver_kinds;
	/* what IS-05 "auto" stands for, per sender/receiver */
	struct AutoDefaults {
		/* per leg: sender source_ip / receiver interface_ip */
		std::vector<std::string> interface_ips;
		std::string dest_ip; /* sender only, same on every leg */
		uint16_t dest_port = 0;
	};
	std::map<nmos::id, AutoDefaults> auto_defaults;
	/* per-sender SDP parameters used by set_transportfile */
	std::map<nmos::id, nmos::sdp_parameters> sdp_params;

	NodeConfig config;
};

static NmosNode *g_node = nullptr;

/* --- helpers ---------------------------------------------------------------- */

static TransportParams parse_transport_params(const web::json::value &tp)
{
	using web::json::value;
	TransportParams out;
	if (tp.size() == 0)
		return out;
	const auto &p = tp.at(0);
	auto get_s = [&](const utility::string_t &f) -> std::string {
		if (p.has_field(f) && p.at(f).is_string())
			return utility::us2s(p.at(f).as_string());
		return {};
	};
	auto get_i = [&](const utility::string_t &f) -> int {
		if (p.has_field(f) && p.at(f).is_integer())
			return p.at(f).as_integer();
		return 0;
	};
	/* receiver params */
	out.source_ip = get_s(U("source_ip"));
	std::string multicast = get_s(U("multicast_ip"));
	std::string dest = get_s(U("destination_ip"));
	out.dest_ip = !multicast.empty() ? multicast : dest;
	int port = get_i(U("destination_port"));
	out.dest_port = port > 0 ? (uint16_t)port : 0;
	if (p.has_field(U("rtp_enabled")) && p.at(U("rtp_enabled")).is_boolean())
		out.rtp_enabled = p.at(U("rtp_enabled")).as_bool();
	return out;
}

/* The sessions put both ST 2022-7 legs on one destination (and a receiver
 * uses one source filter for both), and parse_transport_params() hands over
 * the primary leg. True when the secondary leg of an activation asks for
 * something else -- which the sessions cannot follow. A sender's source_ip
 * and a receiver's interface_ip differ per leg by design and are not
 * compared. */
static bool secondary_leg_differs(const web::json::value &tp, bool receiver)
{
	if (tp.size() < 2)
		return false;
	static const utility::string_t sender_keys[] = {U("destination_ip"),
							U("destination_port")};
	static const utility::string_t receiver_keys[] = {
		U("multicast_ip"), U("source_ip"), U("destination_port")};
	const auto &a = tp.at(0);
	const auto &b = tp.at(1);
	auto differs = [&](const utility::string_t &key) {
		if (a.has_field(key) != b.has_field(key))
			return true;
		return a.has_field(key) && a.at(key) != b.at(key);
	};
	if (receiver) {
		for (const auto &key : receiver_keys)
			if (differs(key))
				return true;
	} else {
		for (const auto &key : sender_keys)
			if (differs(key))
				return true;
	}
	return false;
}

/* The RTP payload type is not an IS-05 transport param. For a receiver it
 * only exists in the SDP the controller PATCHed as transport_file, which
 * nmos-cpp keeps in the active endpoint ({"data": "<sdp>", "type": ...}).
 * Returns 0 when there is none (activation by transport params only). */
static uint8_t payload_type_from_transport_file(const web::json::value &active,
						const char *media)
{
	if (!active.is_object() || !active.has_field(U("transport_file")))
		return 0;
	const auto &tf = active.at(U("transport_file"));
	if (!tf.is_object() || !tf.has_field(U("data")) || !tf.at(U("data")).is_string())
		return 0;
	int pt = sdp_payload_type(utility::us2s(tf.at(U("data")).as_string()), media);
	return pt > 0 ? (uint8_t)pt : 0;
}

/* SMPTE ST 2110-30 channel symbols for the audio source and the SDP
 * channel-order. What a played file carries beyond mono and stereo is not
 * known here, so any other channel count is declared as undefined channels
 * (U01..Unn). */
static std::vector<nmos::channel_symbol> channel_symbols_for(uint16_t channels)
{
	if (channels == 1)
		return nmos::channel_symbols::M;
	if (channels == 2)
		return nmos::channel_symbols::ST;
	std::vector<nmos::channel_symbol> symbols;
	for (unsigned int i = 1; i <= channels; i++)
		symbols.push_back(nmos::channel_symbols::Undefined(i));
	return symbols;
}

/* --- lifecycle -------------------------------------------------------------- */

NmosNode *NmosNode::start(const NodeConfig &config)
{
	if (g_node)
		return g_node;

	auto *node = new NmosNode();
	auto *impl = new Impl();
	node->impl_ = impl;
	impl->config = config;

	try {
		auto &settings = impl->model.settings;
		settings[nmos::fields::label] =
			web::json::value::string(utility::s2us(config.node_label));
		settings[nmos::fields::description] = web::json::value::string(
			utility::s2us(config.description));
		if (!config.host_address.empty())
			settings[nmos::fields::host_address] = web::json::value::string(
				utility::s2us(config.host_address));
		if (config.http_port > 0) {
			settings[nmos::fields::node_port] =
				web::json::value::number(config.http_port);
			settings[nmos::fields::connection_port] =
				web::json::value::number(config.http_port + 3);
		}
		if (!config.registry_address.empty()) {
			/* fixed registry instead of mDNS discovery */
			settings[nmos::fields::registry_address] =
				web::json::value::string(
					utility::s2us(config.registry_address));
			if (config.registry_port > 0)
				settings[nmos::fields::registration_port] =
					web::json::value::number(config.registry_port);
			settings[nmos::fields::highest_pri] =
				web::json::value::number(nmos::service_priorities::no_priority);
		}
		nmos::insert_node_default_settings(settings);

		impl->log_model.level = config.logging_level;
		impl->gate = std::make_unique<nmos::experimental::log_gate>(
			std::cerr, std::cout, impl->log_model);

		/* node + device resources */
		impl->node_id = nmos::make_id();
		impl->device_id = nmos::make_id();
		auto lock = impl->model.write_lock();
		nmos::insert_resource(
			impl->model.node_resources,
			nmos::make_node(impl->node_id, impl->model.settings));
		nmos::insert_resource(
			impl->model.node_resources,
			nmos::make_device(impl->device_id, impl->node_id, {}, {},
					  impl->model.settings));
		lock.unlock();

		/* node server with our IS-05 hooks */
		nmos::experimental::node_implementation node_impl;
		node_impl.on_connection_activated(
			[impl](const nmos::resource &resource,
			       const nmos::resource &connection_resource) {
				ActivationCb cb;
				const char *rx_media = nullptr; /* set for receivers */
				{
					std::lock_guard<std::mutex> g(impl->mutex);
					auto it = impl->callbacks.find(resource.id);
					if (it != impl->callbacks.end())
						cb = it->second;
					auto kind = impl->receiver_kinds.find(resource.id);
					if (kind != impl->receiver_kinds.end())
						rx_media = kind->second == EssenceKind::VideoRaw
								   ? "video"
								   : "audio";
				}
				if (!cb)
					return;
				const auto &active = nmos::fields::endpoint_active(
					connection_resource.data);
				const auto &legs = nmos::fields::transport_params(active);
				auto tp = parse_transport_params(legs);
				if (rx_media)
					tp.payload_type = payload_type_from_transport_file(
						active, rx_media);
				if (secondary_leg_differs(legs, rx_media != nullptr))
					VLOG_W("[nmos] %s %s: the secondary leg asks for "
					       "another %s than the primary; both media "
					       "ports use the primary leg's",
					       rx_media ? "receiver" : "sender",
					       utility::us2s(resource.id).c_str(),
					       rx_media ? "group, source or port"
							: "destination");
				cb(tp);
			});
		node_impl.on_set_transportfile(
			[impl](const nmos::resource &sender,
			       const nmos::resource &connection_sender,
			       web::json::value &endpoint_transportfile) {
				std::lock_guard<std::mutex> g(impl->mutex);
				auto it = impl->sdp_params.find(sender.id);
				if (it == impl->sdp_params.end())
					return;
				const auto &active = nmos::fields::endpoint_active(
					connection_sender.data);
				auto session = nmos::make_session_description(
					it->second,
					nmos::fields::transport_params(active));
				endpoint_transportfile =
					nmos::make_connection_rtp_sender_transportfile(
						utility::s2us(sdp::make_session_description(
							session)));
			});
		node_impl.on_resolve_auto(
			[impl](const nmos::resource &resource,
			       const nmos::resource &,
			       web::json::value &transport_params) {
				/* An activation may leave params on "auto"; the
				 * active endpoint has to show real values. Ours
				 * come from the configuration of the endpoint:
				 * each leg is one media port. */
				Impl::AutoDefaults d;
				{
					std::lock_guard<std::mutex> g(impl->mutex);
					auto it = impl->auto_defaults.find(resource.id);
					if (it != impl->auto_defaults.end())
						d = it->second;
				}
				auto str = [](const std::string &v) {
					return web::json::value::string(utility::s2us(v));
				};
				const bool is_sender = nmos::types::sender == resource.type;
				size_t n = 0;
				for (auto &leg : transport_params.as_array()) {
					const std::string local_ip =
						n < d.interface_ips.size()
							? d.interface_ips[n]
							: std::string();
					n++;
					if (is_sender) {
						if (!local_ip.empty())
							nmos::details::resolve_auto(
								leg, nmos::fields::source_ip,
								[&] { return str(local_ip); });
						if (!d.dest_ip.empty())
							nmos::details::resolve_auto(
								leg, nmos::fields::destination_ip,
								[&] { return str(d.dest_ip); });
						if (d.dest_port)
							nmos::details::resolve_auto(
								leg, nmos::fields::destination_port,
								[&] {
									return web::json::value::number(
										d.dest_port);
								});
					} else if (!local_ip.empty()) {
						nmos::details::resolve_auto(
							leg, nmos::fields::interface_ip,
							[&] { return str(local_ip); });
					}
				}
				/* spec defaults for whatever is still "auto" */
				nmos::resolve_rtp_auto(resource.type, transport_params);
			});

		impl->server = std::make_unique<nmos::server>(
			nmos::experimental::make_node_server(
				impl->model, node_impl, impl->log_model, *impl->gate));
		impl->server->open().wait();

		VLOG_I("[nmos] node started (label '%s')",
		     config.node_label.c_str());
	} catch (const std::exception &e) {
		VLOG_E("[nmos] node startup failed: %s", e.what());
		delete impl;
		delete node;
		return nullptr;
	}

	g_node = node;
	return g_node;
}

NmosNode *NmosNode::instance()
{
	return g_node;
}

void NmosNode::stop()
{
	if (!g_node)
		return;
	delete g_node;
	g_node = nullptr;
}

NmosNode::~NmosNode()
{
	if (impl_) {
		try {
			if (impl_->server)
				impl_->server->close().wait();
		} catch (...) {
		}
		delete impl_;
	}
}

/* --- resource registration --------------------------------------------------- */

std::string NmosNode::add_receiver(EssenceKind kind, const std::string &label,
				   const VideoFormat &vfmt, const AudioFormat &afmt,
				   const std::vector<std::string> &interface_ips,
				   ActivationCb cb)
{
	(void)vfmt;
	auto *impl = impl_;
	auto receiver_id = nmos::make_id();
	const bool smpte2022_7 = interface_ips.size() > 1;

	auto lock = impl->model.write_lock();

	auto format = kind == EssenceKind::VideoRaw ? nmos::formats::video
						    : nmos::formats::audio;
	std::vector<nmos::media_type> media_types{
		kind == EssenceKind::VideoRaw
			? nmos::media_types::video_raw
			: nmos::media_types::audio_L((unsigned int)afmt.bit_depth)};

	auto receiver = nmos::make_receiver(receiver_id, impl->device_id,
					    nmos::transports::rtp, {}, format,
					    media_types, impl->model.settings);
	receiver.data[nmos::fields::label] =
		web::json::value::string(utility::s2us(label));
	nmos::insert_resource(impl->model.node_resources, std::move(receiver));

	nmos::insert_resource(impl->model.connection_resources,
			      nmos::make_connection_rtp_receiver(receiver_id,
								 smpte2022_7));

	impl->model.notify();
	lock.unlock();

	{
		std::lock_guard<std::mutex> g(impl->mutex);
		impl->callbacks[receiver_id] = std::move(cb);
		impl->receiver_kinds[receiver_id] = kind;
		impl->auto_defaults[receiver_id].interface_ips = interface_ips;
	}
	VLOG_I("[nmos] registered receiver '%s' (%s)%s", label.c_str(),
	       utility::us2s(receiver_id).c_str(),
	       smpte2022_7 ? ", two legs" : "");
	return utility::us2s(receiver_id);
}

std::string NmosNode::add_sender(EssenceKind kind, const std::string &label,
				 const VideoFormat &vfmt, const AudioFormat &afmt,
				 const std::vector<std::string> &interface_ips,
				 const TransportParams &initial_params,
				 ActivationCb cb)
{
	auto *impl = impl_;
	auto source_id = nmos::make_id();
	auto flow_id = nmos::make_id();
	auto sender_id = nmos::make_id();
	const size_t legs = interface_ips.size() > 1 ? 2 : 1;

	auto lock = impl->model.write_lock();

	nmos::resource source, flow;
	nmos::sdp_parameters sdp;

	/* What the SDP says must match what the MTL session in player.cpp puts
	 * on the wire, so everything is stated here explicitly instead of
	 * being derived from the IS-04 resources. (nmos-cpp's resource-based
	 * make_sdp_parameters() is deprecated, and it needs a node clock and
	 * sender interface_bindings that this node does not model.) */
	const auto session_name = utility::s2us(label);
	const uint64_t payload_type =
		initial_params.payload_type
			? initial_params.payload_type
			: (kind == EssenceKind::VideoRaw ? 96 : 97);
	/* One m= section per leg. nmos-cpp writes a=mid and a=group:DUP only
	 * when the transport params have more than one leg, but then needs an
	 * id for each. */
	std::vector<utility::string_t> media_stream_ids{U("PRIMARY")};
	if (legs > 1)
		media_stream_ids.push_back(U("SECONDARY"));
	/* ST 2110-10 wants a=ts-refclk, per leg. The node does not know the
	 * grandmaster identity or lock state of the device's clock, so this
	 * claims a traceable PTP reference. That is wrong when the device
	 * runs free (device.ptp off and no phc_device); it should then be
	 * localmac=<MAC of the media port>. */
	const std::vector<nmos::sdp_parameters::ts_refclk_t> ts_refclk(
		legs, nmos::sdp_parameters::ts_refclk_t::ptp_traceable());

	if (kind == EssenceKind::VideoRaw) {
		/* MTL counts fields per second on an interlaced session;
		 * IS-04 grain_rate and the SDP exactframerate count frames */
		nmos::rational frame_rate(vfmt.fps_num,
					  vfmt.fps_den * (vfmt.interlaced ? 2 : 1));
		source = nmos::make_video_source(source_id, impl->device_id,
						 nmos::clock_names::clk0, frame_rate,
						 impl->model.settings);
		flow = nmos::make_raw_video_flow(
			flow_id, source_id, impl->device_id, frame_rate, vfmt.width,
			vfmt.height,
			vfmt.interlaced ? nmos::interlace_modes::interlaced_tff
					: nmos::interlace_modes::progressive,
			nmos::colorspaces::BT709, nmos::transfer_characteristics::SDR,
			sdp::samplings::YCbCr_4_2_2, vfmt.bit_depth,
			impl->model.settings);

		nmos::video_raw_parameters params;
		params.sampling = sdp::samplings::YCbCr_4_2_2;
		params.depth = (uint32_t)vfmt.bit_depth;
		params.width = vfmt.width;
		params.height = vfmt.height;
		params.exactframerate = frame_rate;
		params.interlace = vfmt.interlaced;
		params.segmented = false;
		params.tcs = sdp::transfer_characteristic_systems::SDR;
		params.colorimetry = sdp::colorimetries::BT709;
		/* RANGE left out: the player sends narrow range, the default */
		/* MTL packs in block mode, which is also a valid general-mode
		 * stream; 2110GPM is the claim that holds either way */
		params.pm = sdp::packing_modes::general;
		params.ssn = sdp::smpte_standard_numbers::ST2110_20_2017;
		/* the pacing the player's session was actually created with */
		switch (vfmt.pacing) {
		case VideoFormat::Pacing::Wide:
			params.tp = sdp::type_parameters::type_W;
			break;
		case VideoFormat::Pacing::NarrowLinear:
			params.tp = sdp::type_parameters::type_NL;
			break;
		case VideoFormat::Pacing::Narrow:
			params.tp = sdp::type_parameters::type_N;
			break;
		}
		sdp = nmos::make_video_raw_sdp_parameters(session_name, params,
							  payload_type,
							  media_stream_ids,
							  ts_refclk);
	} else {
		const auto symbols = channel_symbols_for(afmt.channels);
		std::vector<nmos::channel> channels;
		for (size_t i = 0; i < symbols.size(); i++)
			channels.push_back(
				{utility::s2us("Channel " + std::to_string(i + 1)),
				 symbols[i]});

		const uint32_t ptime_us = afmt.ptime_us ? afmt.ptime_us : 1000;
		nmos::rational sample_rate(afmt.sample_rate, 1);
		source = nmos::make_audio_source(source_id, impl->device_id,
						 nmos::clock_names::clk0,
						 nmos::rational(1000000, ptime_us), /* packets/s */
						 channels, impl->model.settings);
		flow = nmos::make_raw_audio_flow(flow_id, source_id, impl->device_id,
						 sample_rate, afmt.bit_depth,
						 impl->model.settings);

		nmos::audio_L_parameters params;
		params.channel_count = afmt.channels;
		params.bit_depth = (uint32_t)afmt.bit_depth;
		params.sample_rate = afmt.sample_rate;
		params.channel_order = nmos::make_fmtp_channel_order(symbols);
		params.packet_time = ptime_us / 1000.0; /* a=ptime is in ms */
		sdp = nmos::make_audio_L_sdp_parameters(session_name, params,
							payload_type,
							media_stream_ids, ts_refclk);
	}
	source.data[nmos::fields::label] =
		web::json::value::string(utility::s2us(label));
	flow.data[nmos::fields::label] =
		web::json::value::string(utility::s2us(label));

	/* this overload fills in manifest_href with the IS-05 transportfile URL */
	auto sender = nmos::make_sender(sender_id, flow_id, impl->device_id, {},
					impl->model.settings);
	sender.data[nmos::fields::label] =
		web::json::value::string(utility::s2us(label));

	nmos::insert_resource(impl->model.node_resources, std::move(source));
	nmos::insert_resource(impl->model.node_resources, std::move(flow));
	nmos::insert_resource(impl->model.node_resources, std::move(sender));

	auto connection_sender =
		nmos::make_connection_rtp_sender(sender_id, legs > 1);
	/* seed staged params with our configured defaults so a controller sees
	 * sensible values before the first PATCH. Both legs go to the same
	 * destination, each from its own media port -- that is what the
	 * player's session does. */
	auto &staged = connection_sender.data[nmos::fields::endpoint_staged];
	auto &staged_legs = nmos::fields::transport_params(staged);
	for (size_t leg = 0; leg < legs; leg++) {
		auto &tp = staged_legs[leg];
		tp[U("destination_ip")] = web::json::value::string(
			utility::s2us(initial_params.dest_ip));
		tp[U("destination_port")] =
			web::json::value::number(initial_params.dest_port);
		if (leg < interface_ips.size() && !interface_ips[leg].empty())
			tp[U("source_ip")] = web::json::value::string(
				utility::s2us(interface_ips[leg]));
		tp[U("rtp_enabled")] = web::json::value::boolean(true);
	}
	nmos::insert_resource(impl->model.connection_resources,
			      std::move(connection_sender));

	impl->model.notify();
	lock.unlock();

	{
		std::lock_guard<std::mutex> g(impl->mutex);
		impl->callbacks[sender_id] = std::move(cb);
		impl->sdp_params[sender_id] = sdp;
		auto &d = impl->auto_defaults[sender_id];
		d.interface_ips = interface_ips;
		d.dest_ip = initial_params.dest_ip;
		d.dest_port = initial_params.dest_port;
	}
	VLOG_I("[nmos] registered sender '%s' (%s)%s", label.c_str(),
	       utility::us2s(sender_id).c_str(), legs > 1 ? ", two legs" : "");
	return utility::us2s(sender_id);
}

void NmosNode::remove_resource(const std::string &id)
{
	auto *impl = impl_;
	nmos::id rid = utility::s2us(id);
	{
		std::lock_guard<std::mutex> g(impl->mutex);
		impl->callbacks.erase(rid);
		impl->receiver_kinds.erase(rid);
		impl->auto_defaults.erase(rid);
		impl->sdp_params.erase(rid);
	}
	auto lock = impl->model.write_lock();
	/* erasing the sender/receiver also unregisters it from the RDS on the
	 * next registration update; associated source/flow of senders are left
	 * for now (harmless, cleaned up on shutdown) */
	nmos::erase_resource(impl->model.node_resources, rid);
	nmos::erase_resource(impl->model.connection_resources, rid);
	impl->model.notify();
}

void NmosNode::set_sender_active(const std::string &id, bool active)
{
	auto *impl = impl_;
	auto lock = impl->model.write_lock();
	nmos::modify_resource(impl->model.node_resources, utility::s2us(id),
			      [active](nmos::resource &r) {
				      r.data[U("subscription")][U("active")] =
					      web::json::value::boolean(active);
			      });
	impl->model.notify();
}

} // namespace vcard_nmos
