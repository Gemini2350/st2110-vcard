/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Embedded NMOS node built on sony/nmos-cpp (Apache-2.0).
 *
 * NOTE ON API STABILITY: this file is written against the nmos-cpp tag pinned
 * in docs/setup-linux.md and is modeled on nmos-cpp's own nmos-cpp-node
 * example (node_implementation.cpp).
 * nmos-cpp's helper names occasionally change between tags; this is the one
 * file expected to need small compile fixes when the pinned tag moves.
 *
 * Structure per endpoint:
 *  - sender:   source -> flow -> sender (+ connection sender), SDP served as
 *              the IS-05 transport file
 *  - receiver: receiver (+ connection receiver)
 * IS-05 staged/active handling, activation scheduling, registration and
 * heartbeat (IS-04) all come from nmos-cpp's node_server.
 */
#include "nmos-node.h"

#include "core/log.h"

#include <map>
#include <mutex>

#include <nmos/id.h>
#include <nmos/log_gate.h>
#include <nmos/model.h>
#include <nmos/node_resource.h>
#include <nmos/node_resources.h>
#include <nmos/node_server.h>
#include <nmos/connection_resources.h>
#include <nmos/connection_api.h>
#include <nmos/interlace_mode.h>
#include <nmos/media_type.h>
#include <nmos/rational.h>
#include <nmos/sdp_utils.h>
#include <nmos/server.h>
#include <nmos/settings.h>
#include <nmos/transport.h>
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
				{
					std::lock_guard<std::mutex> g(impl->mutex);
					auto it = impl->callbacks.find(resource.id);
					if (it != impl->callbacks.end())
						cb = it->second;
				}
				if (!cb)
					return;
				const auto &active = nmos::fields::endpoint_active(
					connection_resource.data);
				auto tp = parse_transport_params(
					nmos::fields::transport_params(active));
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
		node_impl.on_resolve_auto(&nmos::experimental::resolve_auto);

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
				   ActivationCb cb)
{
	(void)vfmt;
	(void)afmt;
	auto *impl = impl_;
	auto receiver_id = nmos::make_id();

	auto lock = impl->model.write_lock();

	auto format = kind == EssenceKind::VideoRaw ? nmos::formats::video
						    : nmos::formats::audio;
	std::vector<utility::string_t> media_types =
		kind == EssenceKind::VideoRaw
			? std::vector<utility::string_t>{nmos::media_types::video_raw.name}
			: std::vector<utility::string_t>{
				  nmos::media_types::audio_L(afmt.bit_depth).name};

	auto receiver = nmos::make_receiver(receiver_id, impl->device_id,
					    nmos::transports::rtp, {}, format,
					    media_types, impl->model.settings);
	receiver.data[nmos::fields::label] =
		web::json::value::string(utility::s2us(label));
	nmos::insert_resource(impl->model.node_resources, std::move(receiver));

	nmos::insert_resource(impl->model.connection_resources,
			      nmos::make_connection_rtp_receiver(receiver_id, false));

	impl->model.notify();
	lock.unlock();

	{
		std::lock_guard<std::mutex> g(impl->mutex);
		impl->callbacks[receiver_id] = std::move(cb);
	}
	VLOG_I("[nmos] registered receiver '%s' (%s)", label.c_str(),
	     utility::us2s(receiver_id).c_str());
	return utility::us2s(receiver_id);
}

std::string NmosNode::add_sender(EssenceKind kind, const std::string &label,
				 const VideoFormat &vfmt, const AudioFormat &afmt,
				 const TransportParams &initial_params,
				 ActivationCb cb)
{
	auto *impl = impl_;
	auto source_id = nmos::make_id();
	auto flow_id = nmos::make_id();
	auto sender_id = nmos::make_id();

	auto lock = impl->model.write_lock();

	nmos::resource source, flow;
	nmos::sdp_parameters sdp;

	if (kind == EssenceKind::VideoRaw) {
		nmos::rational grain_rate(vfmt.fps_num, vfmt.fps_den);
		source = nmos::make_video_source(source_id, impl->device_id,
						 nmos::clock_names::clk0, grain_rate,
						 impl->model.settings);
		flow = nmos::make_raw_video_flow(
			flow_id, source_id, impl->device_id, grain_rate, vfmt.width,
			vfmt.height,
			vfmt.interlaced ? nmos::interlace_modes::interlaced_tff
					: nmos::interlace_modes::progressive,
			nmos::colorspaces::BT709, nmos::transfer_characteristics::SDR,
			sdp::samplings::YCbCr_4_2_2, vfmt.bit_depth,
			impl->model.settings);
	} else {
		nmos::rational sample_rate(afmt.sample_rate, 1);
		source = nmos::make_audio_source(source_id, impl->device_id,
						 nmos::clock_names::clk0,
						 nmos::rational(1000, afmt.ptime_us),
						 afmt.channels, impl->model.settings);
		flow = nmos::make_raw_audio_flow(flow_id, source_id, impl->device_id,
						 afmt.sample_rate, afmt.bit_depth,
						 impl->model.settings);
	}
	source.data[nmos::fields::label] =
		web::json::value::string(utility::s2us(label));
	flow.data[nmos::fields::label] =
		web::json::value::string(utility::s2us(label));

	auto sender = nmos::make_sender(sender_id, flow_id, nmos::transports::rtp,
					impl->device_id, {}, {},
					impl->model.settings);
	sender.data[nmos::fields::label] =
		web::json::value::string(utility::s2us(label));

	/* SDP parameters derived from source/flow/sender; the transport section
	 * is filled in per-activation from IS-05 transport params */
	sdp = nmos::make_sdp_parameters(source.data, flow.data, sender.data,
					{utility::s2us(std::string("PRIMARY"))});

	nmos::insert_resource(impl->model.node_resources, std::move(source));
	nmos::insert_resource(impl->model.node_resources, std::move(flow));
	nmos::insert_resource(impl->model.node_resources, std::move(sender));

	auto connection_sender = nmos::make_connection_rtp_sender(sender_id, false);
	/* seed staged params with our configured defaults so a controller sees
	 * sensible values before the first PATCH */
	auto &staged = connection_sender.data[nmos::fields::endpoint_staged];
	auto &tp0 = nmos::fields::transport_params(staged)[0];
	tp0[U("destination_ip")] = web::json::value::string(
		utility::s2us(initial_params.dest_ip));
	tp0[U("destination_port")] =
		web::json::value::number(initial_params.dest_port);
	tp0[U("source_ip")] = web::json::value::string(
		utility::s2us(initial_params.source_ip));
	tp0[U("rtp_enabled")] = web::json::value::boolean(true);
	nmos::insert_resource(impl->model.connection_resources,
			      std::move(connection_sender));

	impl->model.notify();
	lock.unlock();

	{
		std::lock_guard<std::mutex> g(impl->mutex);
		impl->callbacks[sender_id] = std::move(cb);
		impl->sdp_params[sender_id] = sdp;
	}
	VLOG_I("[nmos] registered sender '%s' (%s)", label.c_str(),
	     utility::us2s(sender_id).c_str());
	return utility::us2s(sender_id);
}

void NmosNode::remove_resource(const std::string &id)
{
	auto *impl = impl_;
	nmos::id rid = utility::s2us(id);
	{
		std::lock_guard<std::mutex> g(impl->mutex);
		impl->callbacks.erase(rid);
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
