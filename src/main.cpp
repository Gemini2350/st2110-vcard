/* SPDX-License-Identifier: GPL-2.0-or-later
 * st2110-vcard: a virtual SMPTE ST 2110 video/audio card for Linux.
 */
#include "api/http-server.h"
#include "config.h"
#include "core/log.h"
#include "engine.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

namespace {

std::atomic<bool> g_quit{false};

void on_signal(int) { g_quit = true; }

void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-c config.json] [-v] [--write-default-config PATH]\n"
		"\n"
		"  -c PATH   configuration file (default /etc/st2110-vcard.json)\n"
		"  -v        verbose logging (repeat for MTL debug output)\n"
		"\n"
		"Needs hugepages and a NIC port bound for DPDK; see docs/setup-linux.md.\n",
		argv0);
}

vcard::LogLevel parse_log_level(const std::string &s)
{
	if (s == "debug") return vcard::LogLevel::Debug;
	if (s == "warn") return vcard::LogLevel::Warn;
	if (s == "error") return vcard::LogLevel::Error;
	return vcard::LogLevel::Info;
}

} // namespace

int main(int argc, char **argv)
{
	std::string config_path = "/etc/st2110-vcard.json";
	int verbose = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			config_path = argv[++i];
		} else if (!strcmp(argv[i], "-v")) {
			verbose++;
		} else if (!strcmp(argv[i], "--write-default-config") && i + 1 < argc) {
			std::string err;
			if (!vcard::Config::defaults().save(argv[i + 1], &err)) {
				fprintf(stderr, "%s\n", err.c_str());
				return 1;
			}
			printf("wrote %s\n", argv[i + 1]);
			return 0;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	vcard::Config config;
	std::string error;
	if (!vcard::Config::load(config_path, &config, &error)) {
		fprintf(stderr, "config: %s\n", error.c_str());
		fprintf(stderr, "hint: %s --write-default-config %s\n", argv[0],
			config_path.c_str());
		return 1;
	}

	vcard::log_set_level(verbose ? vcard::LogLevel::Debug
				     : parse_log_level(config.log_level));
	/* FFmpeg is chatty on the info level and most of it is noise here. */
	av_log_set_level(verbose > 1 ? AV_LOG_VERBOSE : AV_LOG_ERROR);
	if (verbose > 1)
		config.device.log_level = MTL_LOG_LEVEL_DEBUG;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	/* A disconnecting MJPEG viewer must not take the daemon down. */
	signal(SIGPIPE, SIG_IGN);

	VLOG_I("st2110-vcard starting (config %s)", config_path.c_str());

	vcard::Engine engine(config);
	if (!engine.start())
		return 1;

	vcard::HttpServer http(engine, config.http);
	if (!http.start()) {
		engine.stop();
		return 1;
	}

	while (!g_quit)
		std::this_thread::sleep_for(std::chrono::milliseconds(200));

	VLOG_I("shutting down");
	http.stop();
	engine.stop();
	return 0;
}
