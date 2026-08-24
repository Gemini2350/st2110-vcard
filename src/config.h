/* SPDX-License-Identifier: GPL-2.0-or-later
 * JSON configuration. Everything the daemon needs to come up without a GUI,
 * so it can run headless from systemd and be provisioned by config management.
 */
#pragma once

#include "core/mtl-device.h"
#include "player/player.h"
#include "recorder/recorder.h"

#include <string>
#include <vector>

namespace vcard {

struct HttpConfig {
	std::string bind = "0.0.0.0";
	int port = 8088;
	std::string web_root = "/usr/share/st2110-vcard/web";
};

struct NmosConfig {
	bool enabled = true;
	std::string label = "ST 2110 Virtual Card";
	std::string description = "File playout and capture";
	/* Control-plane address. This is the management NIC, never the DPDK
	 * port's IP -- that interface does not exist to the kernel any more. */
	std::string host_address;
	std::string registry_address; /* empty = mDNS discovery */
	int registry_port = 0;
	int http_port = 0;            /* 0 = nmos-cpp defaults */
};

struct Config {
	MtlDevice::Params device;
	HttpConfig http;
	NmosConfig nmos;
	std::string media_dir = "/var/lib/st2110-vcard/media";
	std::string log_level = "info";
	std::vector<PlayerConfig> players;
	std::vector<RecorderConfig> recorders;

	static bool load(const std::string &path, Config *out, std::string *error);
	bool save(const std::string &path, std::string *error) const;
	static Config defaults();
};

} // namespace vcard
