/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"

#include "core/formats.h"
#include "core/log.h"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace vcard {

namespace {

enum st_fps fps_from_string(const std::string &s)
{
	double v = atof(s.c_str());
	/* the /1001 rates are written the way people say them */
	if (s == "23.98" || s == "23.976") return ST_FPS_P23_98;
	if (s == "29.97") return ST_FPS_P29_97;
	if (s == "59.94") return ST_FPS_P59_94;
	if (s == "119.88") return ST_FPS_P119_88;
	return rational_to_mtl_fps((uint32_t)(v * 1000), 1000);
}

template <typename T> T get_or(const json &j, const char *key, T fallback)
{
	auto it = j.find(key);
	if (it == j.end() || it->is_null())
		return fallback;
	try {
		return it->get<T>();
	} catch (...) {
		return fallback;
	}
}

PlayerConfig parse_player(const json &j)
{
	PlayerConfig p;
	p.name = get_or<std::string>(j, "name", p.name);
	p.width = get_or<uint32_t>(j, "width", p.width);
	p.height = get_or<uint32_t>(j, "height", p.height);
	p.fps = fps_from_string(get_or<std::string>(j, "fps", "50"));
	p.interlaced = get_or<bool>(j, "interlaced", p.interlaced);
	p.video_enabled = get_or<bool>(j, "video_enabled", p.video_enabled);
	p.video_dest_ip = get_or<std::string>(j, "video_dest_ip", p.video_dest_ip);
	p.video_port = get_or<uint16_t>(j, "video_port", p.video_port);
	p.video_payload_type =
		get_or<uint8_t>(j, "video_payload_type", p.video_payload_type);
	p.audio_enabled = get_or<bool>(j, "audio_enabled", p.audio_enabled);
	p.channels = get_or<uint16_t>(j, "channels", p.channels);
	p.audio_dest_ip = get_or<std::string>(j, "audio_dest_ip", p.audio_dest_ip);
	p.audio_port = get_or<uint16_t>(j, "audio_port", p.audio_port);
	p.audio_payload_type =
		get_or<uint8_t>(j, "audio_payload_type", p.audio_payload_type);
	p.ptime = parse_ptime(get_or<std::string>(j, "ptime", "1ms"));
	p.queue_frames = get_or<uint32_t>(j, "queue_frames", p.queue_frames);
	return p;
}

json dump_player(const PlayerConfig &p)
{
	return json{{"name", p.name},
		    {"width", p.width},
		    {"height", p.height},
		    {"fps", mtl_fps_name(p.fps)},
		    {"interlaced", p.interlaced},
		    {"video_enabled", p.video_enabled},
		    {"video_dest_ip", p.video_dest_ip},
		    {"video_port", p.video_port},
		    {"video_payload_type", p.video_payload_type},
		    {"audio_enabled", p.audio_enabled},
		    {"channels", p.channels},
		    {"audio_dest_ip", p.audio_dest_ip},
		    {"audio_port", p.audio_port},
		    {"audio_payload_type", p.audio_payload_type},
		    {"ptime", ptime_name(p.ptime)},
		    {"queue_frames", p.queue_frames}};
}

RecorderConfig parse_recorder(const json &j)
{
	RecorderConfig r;
	r.name = get_or<std::string>(j, "name", r.name);
	r.video_enabled = get_or<bool>(j, "video_enabled", r.video_enabled);
	r.auto_detect = get_or<bool>(j, "auto_detect", r.auto_detect);
	r.width = get_or<uint32_t>(j, "width", r.width);
	r.height = get_or<uint32_t>(j, "height", r.height);
	r.fps = fps_from_string(get_or<std::string>(j, "fps", "50"));
	r.interlaced = get_or<bool>(j, "interlaced", r.interlaced);
	r.video_source_ip = get_or<std::string>(j, "video_source_ip", r.video_source_ip);
	r.video_multicast_ip =
		get_or<std::string>(j, "video_multicast_ip", r.video_multicast_ip);
	r.video_port = get_or<uint16_t>(j, "video_port", r.video_port);
	r.video_payload_type =
		get_or<uint8_t>(j, "video_payload_type", r.video_payload_type);
	r.audio_enabled = get_or<bool>(j, "audio_enabled", r.audio_enabled);
	r.channels = get_or<uint16_t>(j, "channels", r.channels);
	r.audio_source_ip = get_or<std::string>(j, "audio_source_ip", r.audio_source_ip);
	r.audio_multicast_ip =
		get_or<std::string>(j, "audio_multicast_ip", r.audio_multicast_ip);
	r.audio_port = get_or<uint16_t>(j, "audio_port", r.audio_port);
	r.audio_payload_type =
		get_or<uint8_t>(j, "audio_payload_type", r.audio_payload_type);
	r.ptime = parse_ptime(get_or<std::string>(j, "ptime", "1ms"));
	r.output_dir = get_or<std::string>(j, "output_dir", r.output_dir);
	r.codec = parse_record_codec(get_or<std::string>(j, "codec", "prores422hq"));
	r.queue_frames = get_or<uint32_t>(j, "queue_frames", r.queue_frames);
	return r;
}

json dump_recorder(const RecorderConfig &r)
{
	return json{{"name", r.name},
		    {"video_enabled", r.video_enabled},
		    {"auto_detect", r.auto_detect},
		    {"width", r.width},
		    {"height", r.height},
		    {"fps", mtl_fps_name(r.fps)},
		    {"interlaced", r.interlaced},
		    {"video_source_ip", r.video_source_ip},
		    {"video_multicast_ip", r.video_multicast_ip},
		    {"video_port", r.video_port},
		    {"video_payload_type", r.video_payload_type},
		    {"audio_enabled", r.audio_enabled},
		    {"channels", r.channels},
		    {"audio_source_ip", r.audio_source_ip},
		    {"audio_multicast_ip", r.audio_multicast_ip},
		    {"audio_port", r.audio_port},
		    {"audio_payload_type", r.audio_payload_type},
		    {"ptime", ptime_name(r.ptime)},
		    {"output_dir", r.output_dir},
		    {"codec", record_codec_name(r.codec)},
		    {"queue_frames", r.queue_frames}};
}

} // namespace

Config Config::defaults()
{
	Config c;
	c.device.port = "0000:4b:01.0";
	c.device.local_ip = "192.168.96.10";
	c.device.netmask = "255.255.255.0";
	c.players.push_back(PlayerConfig{});
	c.recorders.push_back(RecorderConfig{});
	return c;
}

bool Config::load(const std::string &path, Config *out, std::string *error)
{
	std::ifstream f(path);
	if (!f) {
		*error = "cannot open " + path;
		return false;
	}
	json j;
	try {
		f >> j;
	} catch (const std::exception &e) {
		*error = std::string("JSON parse error: ") + e.what();
		return false;
	}

	Config c;
	if (j.contains("device")) {
		const auto &d = j["device"];
		c.device.port = get_or<std::string>(d, "port", c.device.port);
		c.device.local_ip = get_or<std::string>(d, "local_ip", c.device.local_ip);
		c.device.netmask = get_or<std::string>(d, "netmask", c.device.netmask);
		c.device.gateway = get_or<std::string>(d, "gateway", c.device.gateway);
		c.device.port_r = get_or<std::string>(d, "port_r", c.device.port_r);
		c.device.local_ip_r =
			get_or<std::string>(d, "local_ip_r", c.device.local_ip_r);
		c.device.lcores = get_or<std::string>(d, "lcores", c.device.lcores);
		c.device.ptp = get_or<bool>(d, "ptp", c.device.ptp);
		c.device.phc_device =
			get_or<std::string>(d, "phc_device", c.device.phc_device);
		c.device.tx_queues = get_or<uint16_t>(d, "tx_queues", c.device.tx_queues);
		c.device.rx_queues = get_or<uint16_t>(d, "rx_queues", c.device.rx_queues);
	}
	if (j.contains("http")) {
		const auto &h = j["http"];
		c.http.bind = get_or<std::string>(h, "bind", c.http.bind);
		c.http.port = get_or<int>(h, "port", c.http.port);
		c.http.web_root = get_or<std::string>(h, "web_root", c.http.web_root);
	}
	if (j.contains("nmos")) {
		const auto &n = j["nmos"];
		c.nmos.enabled = get_or<bool>(n, "enabled", c.nmos.enabled);
		c.nmos.label = get_or<std::string>(n, "label", c.nmos.label);
		c.nmos.description =
			get_or<std::string>(n, "description", c.nmos.description);
		c.nmos.host_address =
			get_or<std::string>(n, "host_address", c.nmos.host_address);
		c.nmos.registry_address =
			get_or<std::string>(n, "registry_address", c.nmos.registry_address);
		c.nmos.registry_port = get_or<int>(n, "registry_port", c.nmos.registry_port);
		c.nmos.http_port = get_or<int>(n, "http_port", c.nmos.http_port);
	}
	c.media_dir = get_or<std::string>(j, "media_dir", c.media_dir);
	c.log_level = get_or<std::string>(j, "log_level", c.log_level);

	if (j.contains("players"))
		for (const auto &p : j["players"])
			c.players.push_back(parse_player(p));
	if (j.contains("recorders"))
		for (const auto &r : j["recorders"])
			c.recorders.push_back(parse_recorder(r));

	if (c.players.empty() && c.recorders.empty()) {
		*error = "config defines neither players nor recorders";
		return false;
	}
	*out = c;
	return true;
}

bool Config::save(const std::string &path, std::string *error) const
{
	json j;
	j["device"] = {{"port", device.port},
		       {"local_ip", device.local_ip},
		       {"netmask", device.netmask},
		       {"gateway", device.gateway},
		       {"port_r", device.port_r},
		       {"local_ip_r", device.local_ip_r},
		       {"lcores", device.lcores},
		       {"ptp", device.ptp},
		       {"phc_device", device.phc_device},
		       {"tx_queues", device.tx_queues},
		       {"rx_queues", device.rx_queues}};
	j["http"] = {{"bind", http.bind}, {"port", http.port}, {"web_root", http.web_root}};
	j["nmos"] = {{"enabled", nmos.enabled},
		     {"label", nmos.label},
		     {"description", nmos.description},
		     {"host_address", nmos.host_address},
		     {"registry_address", nmos.registry_address},
		     {"registry_port", nmos.registry_port},
		     {"http_port", nmos.http_port}};
	j["media_dir"] = media_dir;
	j["log_level"] = log_level;
	j["players"] = json::array();
	for (const auto &p : players)
		j["players"].push_back(dump_player(p));
	j["recorders"] = json::array();
	for (const auto &r : recorders)
		j["recorders"].push_back(dump_recorder(r));

	std::ofstream f(path);
	if (!f) {
		*error = "cannot write " + path;
		return false;
	}
	f << j.dump(2) << "\n";
	return true;
}

} // namespace vcard
