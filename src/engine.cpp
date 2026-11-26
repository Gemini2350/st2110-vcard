/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine.h"

#include "core/formats.h"
#include "core/log.h"
#include "media/decoder.h"

#include <algorithm>
#include <ctime>
#include <filesystem>

#ifdef HAVE_NMOS
#include "nmos/nmos-node.h"
#endif

namespace fs = std::filesystem;

namespace vcard {

namespace {

bool has_media_extension(const fs::path &p)
{
	static const char *exts[] = {".mov", ".mxf", ".mp4", ".mkv", ".avi",
				     ".ts",  ".m2t", ".y4m", ".yuv", ".webm"};
	std::string e = p.extension().string();
	std::transform(e.begin(), e.end(), e.begin(), ::tolower);
	for (const char *x : exts)
		if (e == x)
			return true;
	return false;
}

MediaFile describe(const fs::path &p)
{
	MediaFile m;
	m.name = p.filename().string();
	m.path = p.string();
	std::error_code ec;
	m.size_bytes = fs::file_size(p, ec);

	auto info = FileDecoder::probe(p.string());
	m.playable = info.ok;
	if (!info.ok) {
		m.note = info.error;
		return m;
	}
	m.duration_s = info.duration_s;
	std::string audio;
	if (info.audio_channels)
		audio = " / " + std::to_string(info.audio_channels) + "ch " +
			std::to_string(info.audio_sample_rate / 1000) + "k " +
			info.audio_codec;
	char buf[160];
	snprintf(buf, sizeof(buf), "%ux%u @ %.4g %s %s%s", info.width, info.height,
		 info.fps, info.video_codec.c_str(), info.pix_fmt.c_str(),
		 audio.c_str());
	m.format = buf;
	return m;
}

} // namespace

Engine::Engine(Config config) : config_(std::move(config)) {}

Engine::~Engine() { stop(); }

bool Engine::start()
{
	if (started_)
		return true;

	if (!MtlDevice::init(config_.device)) {
		VLOG_E("[engine] MTL device did not come up -- nothing else can work");
		return false;
	}

	std::error_code ec;
	fs::create_directories(config_.media_dir, ec);

	for (const auto &pc : config_.players) {
		auto p = std::make_unique<Player>(pc);
		if (!p->open()) {
			VLOG_E("[engine] player '%s' failed to open", pc.name.c_str());
			continue;
		}
		players_.push_back(std::move(p));
	}
	for (const auto &rc : config_.recorders) {
		auto r = std::make_unique<Recorder>(rc);
		if (!r->open()) {
			VLOG_E("[engine] recorder '%s' failed to open", rc.name.c_str());
			continue;
		}
		recorders_.push_back(std::move(r));
	}

	if (players_.empty() && recorders_.empty()) {
		VLOG_E("[engine] no session came up");
		MtlDevice::shutdown();
		return false;
	}

	if (config_.nmos.enabled)
		register_nmos();

	started_ = true;
	VLOG_I("[engine] running: %zu player(s), %zu recorder(s)", players_.size(),
	       recorders_.size());
	return true;
}

void Engine::stop()
{
	if (!started_)
		return;
	started_ = false;

#ifdef HAVE_NMOS
	if (nmos_up_) {
		vcard_nmos::NmosNode::stop();
		nmos_up_ = false;
	}
#endif
	for (auto &r : recorders_)
		r->close();
	for (auto &p : players_)
		p->close();
	recorders_.clear();
	players_.clear();
	MtlDevice::shutdown();
	VLOG_I("[engine] stopped");
}

void Engine::register_nmos()
{
#ifdef HAVE_NMOS
	vcard_nmos::NodeConfig nc;
	nc.node_label = config_.nmos.label;
	nc.description = config_.nmos.description;
	nc.host_address = config_.nmos.host_address;
	nc.registry_address = config_.nmos.registry_address;
	nc.registry_port = config_.nmos.registry_port;
	nc.http_port = config_.nmos.http_port;

	/* An activation only retargets a session when it carries a complete
	 * destination. Deactivations (master_enable false) and unicast setups
	 * arrive with the address empty or null; restarting a session onto
	 * that would just kill it. */
	auto usable = [](const vcard_nmos::TransportParams &np, const char *what) {
		if (np.dest_ip.empty() || !np.dest_port) {
			VLOG_W("[nmos] %s activation without destination ignored "
			       "(deactivate / unicast are not handled yet)", what);
			return false;
		}
		return true;
	};

	auto *node = vcard_nmos::NmosNode::start(nc);
	if (!node) {
		VLOG_W("[engine] NMOS node did not start; continuing without it");
		return;
	}
	nmos_up_ = true;

	/* Local IP of each media port, primary first. With a redundant port
	 * every endpoint has two IS-05 legs, one per port (ST 2022-7). */
	std::vector<std::string> media_ips{config_.device.local_ip};
	if (!config_.device.port_r.empty())
		media_ips.push_back(config_.device.local_ip_r);

	/* Players are senders; the IS-05 activation retargets the ST 2110
	 * session, which is exactly what a controller expects to be able to
	 * do to a playout device. */
	for (auto &p : players_) {
		const auto &c = p->config();
		vcard_nmos::VideoFormat vf;
		vf.width = c.width;
		vf.height = c.height;
		mtl_fps_to_rational(c.fps, &vf.fps_num, &vf.fps_den);
		vf.interlaced = c.interlaced;
		/* the SDP declares the pacing the session really runs with */
		switch (p->video_pacing()) {
		case ST21_PACING_WIDE:
			vf.pacing = vcard_nmos::VideoFormat::Pacing::Wide;
			break;
		case ST21_PACING_LINEAR:
			vf.pacing = vcard_nmos::VideoFormat::Pacing::NarrowLinear;
			break;
		default:
			vf.pacing = vcard_nmos::VideoFormat::Pacing::Narrow;
			break;
		}
		vcard_nmos::AudioFormat af;
		af.channels = c.channels;
		af.ptime_us = ptime_us(c.ptime);

		/* A sender's payload type is ours to choose: it goes into the
		 * SDP below and IS-05 has no way to change it, so the
		 * activations leave the configured one alone. */
		Player *pp = p.get();
		if (c.video_enabled) {
			vcard_nmos::TransportParams tp;
			tp.dest_ip = c.video_dest_ip;
			tp.dest_port = c.video_port;
			tp.payload_type = c.video_payload_type;
			tp.rtp_enabled = true;
			node->add_sender(vcard_nmos::EssenceKind::VideoRaw,
					 c.name + " video", vf, af, media_ips, tp,
					 [pp, usable](const vcard_nmos::TransportParams &np) {
						 if (!usable(np, "video sender"))
							 return;
						 PlayerConfig nc2 = pp->config();
						 nc2.video_dest_ip = np.dest_ip;
						 nc2.video_port = np.dest_port;
						 pp->reconfigure(nc2);
					 });
		}
		if (c.audio_enabled) {
			vcard_nmos::TransportParams tp;
			tp.dest_ip = c.audio_dest_ip;
			tp.dest_port = c.audio_port;
			tp.payload_type = c.audio_payload_type;
			tp.rtp_enabled = true;
			node->add_sender(vcard_nmos::EssenceKind::AudioL24,
					 c.name + " audio", vf, af, media_ips, tp,
					 [pp, usable](const vcard_nmos::TransportParams &np) {
						 if (!usable(np, "audio sender"))
							 return;
						 PlayerConfig nc2 = pp->config();
						 nc2.audio_dest_ip = np.dest_ip;
						 nc2.audio_port = np.dest_port;
						 pp->reconfigure(nc2);
					 });
		}
	}

	/* Recorders are receivers: a controller points them at a sender and
	 * the multicast join follows. The payload type comes from the SDP of
	 * the activation; an activation by transport params alone carries
	 * none (0), and the configured one stays. */
	for (auto &r : recorders_) {
		const auto &c = r->config();
		vcard_nmos::VideoFormat vf;
		vf.width = c.width;
		vf.height = c.height;
		mtl_fps_to_rational(c.fps, &vf.fps_num, &vf.fps_den);
		vf.interlaced = c.interlaced;
		vcard_nmos::AudioFormat af;
		af.channels = c.channels;
		af.ptime_us = ptime_us(c.ptime);

		Recorder *rr = r.get();
		if (c.video_enabled)
			node->add_receiver(vcard_nmos::EssenceKind::VideoRaw,
					   c.name + " video", vf, af, media_ips,
					   [rr, usable](const vcard_nmos::TransportParams &np) {
						   if (!usable(np, "video receiver"))
							   return;
						   RecorderConfig nc2 = rr->config();
						   nc2.video_multicast_ip = np.dest_ip;
						   nc2.video_port = np.dest_port;
						   nc2.video_source_ip = np.source_ip;
						   if (np.payload_type)
							   nc2.video_payload_type =
								   np.payload_type;
						   rr->reconfigure(nc2);
					   });
		if (c.audio_enabled)
			node->add_receiver(vcard_nmos::EssenceKind::AudioL24,
					   c.name + " audio", vf, af, media_ips,
					   [rr, usable](const vcard_nmos::TransportParams &np) {
						   if (!usable(np, "audio receiver"))
							   return;
						   RecorderConfig nc2 = rr->config();
						   nc2.audio_multicast_ip = np.dest_ip;
						   nc2.audio_port = np.dest_port;
						   nc2.audio_source_ip = np.source_ip;
						   if (np.payload_type)
							   nc2.audio_payload_type =
								   np.payload_type;
						   rr->reconfigure(nc2);
					   });
	}
	VLOG_I("[engine] NMOS node registered");
#endif
}

Player *Engine::player(const std::string &name)
{
	for (auto &p : players_)
		if (p->config().name == name)
			return p.get();
	return nullptr;
}

Recorder *Engine::recorder(const std::string &name)
{
	for (auto &r : recorders_)
		if (r->config().name == name)
			return r.get();
	return nullptr;
}

std::vector<Player *> Engine::players()
{
	std::vector<Player *> v;
	for (auto &p : players_)
		v.push_back(p.get());
	return v;
}

std::vector<Recorder *> Engine::recorders()
{
	std::vector<Recorder *> v;
	for (auto &r : recorders_)
		v.push_back(r.get());
	return v;
}

std::vector<MediaFile> Engine::media_files() const
{
	/* The GUI asks for this list every few seconds. Probing opens every
	 * file and reads into it, so results are kept until size or mtime of
	 * the file change. */
	std::lock_guard<std::mutex> lock(probe_mutex_);
	std::vector<MediaFile> out;
	std::map<std::string, ProbeEntry> fresh;
	std::error_code ec;
	for (const auto &e : fs::directory_iterator(config_.media_dir, ec)) {
		if (!e.is_regular_file(ec) || !has_media_extension(e.path()))
			continue;
		const std::string key = e.path().string();
		uint64_t size = fs::file_size(e.path(), ec);
		int64_t mtime = (int64_t)fs::last_write_time(e.path(), ec)
					.time_since_epoch()
					.count();
		auto it = probe_cache_.find(key);
		if (it != probe_cache_.end() && it->second.size == size &&
		    it->second.mtime == mtime) {
			fresh.emplace(key, it->second);
		} else {
			fresh.emplace(key, ProbeEntry{size, mtime, describe(e.path())});
		}
		out.push_back(fresh[key].file);
	}
	probe_cache_.swap(fresh);
	std::sort(out.begin(), out.end(),
		  [](const MediaFile &a, const MediaFile &b) { return a.name < b.name; });
	return out;
}

std::vector<MediaFile> Engine::recordings() const
{
	std::vector<MediaFile> out;
	std::error_code ec;
	for (const auto &rc : config_.recorders) {
		for (const auto &e : fs::directory_iterator(rc.output_dir, ec)) {
			if (!e.is_regular_file(ec))
				continue;
			MediaFile m;
			m.name = e.path().filename().string();
			m.path = e.path().string();
			m.size_bytes = fs::file_size(e.path(), ec);
			m.playable = true;
			out.push_back(std::move(m));
		}
	}
	std::sort(out.begin(), out.end(),
		  [](const MediaFile &a, const MediaFile &b) { return a.name > b.name; });
	return out;
}

bool Engine::resolve_media(const std::string &name, std::string *out_path) const
{
	std::error_code ec;
	fs::path root = fs::weakly_canonical(config_.media_dir, ec);
	fs::path candidate = fs::weakly_canonical(root / name, ec);
	if (ec)
		return false;
	/* Reject any name that climbs out of media_dir. The web GUI is on the
	 * house network, not the open internet, but a filename is still user
	 * input and this is one line. */
	auto rel = candidate.lexically_relative(root);
	if (rel.empty() || rel.native().rfind("..", 0) == 0)
		return false;
	if (!fs::is_regular_file(candidate, ec))
		return false;
	*out_path = candidate.string();
	return true;
}

Engine::DeviceStatus Engine::device_status() const
{
	DeviceStatus s;
	auto *dev = MtlDevice::instance();
	if (!dev)
		return s;
	s.up = true;
	s.port = dev->params().port;
	s.local_ip = dev->params().local_ip;
	s.redundant = dev->redundant();
	auto ptp = dev->ptp_status();
	s.clock_source = ptp.source;
	s.ptp_locked = ptp.locked;
	s.ptp_have_offset = ptp.have_offset;
	s.ptp_offset_ns = ptp.offset_ns;
	s.ptp_last_sync_age_ms = ptp.last_sync_age_ms;
	s.ptp_tai_ns = ptp.tai_ns;

	if (ptp.tai_ns) {
		/* TAI is 37 s ahead of UTC as of 2017-01-01 and there has been
		 * no leap second since. Shown for eyeballing against a clock,
		 * not used for anything on the media path. */
		time_t utc = (time_t)(ptp.tai_ns / 1000000000ull) - 37;
		struct tm tm;
		gmtime_r(&utc, &tm);
		char buf[32];
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
		s.tai_utc = buf;
	}
	s.nmos_up = nmos_up_;
	return s;
}

} // namespace vcard
