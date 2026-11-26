/* SPDX-License-Identifier: GPL-2.0-or-later
 * Owns the device, the sessions and the NMOS node, and is the single place
 * the HTTP API and the NMOS callbacks act on. Everything below it is
 * deliberately unaware that a GUI or a broadcast controller exists.
 */
#pragma once

#include "config.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vcard {

struct MediaFile {
	std::string name;
	std::string path;
	uint64_t size_bytes = 0;
	double duration_s = 0;
	std::string format; /* "1920x1080 @ 50 prores" */
	bool playable = false;
	std::string note;
};

class Engine {
public:
	explicit Engine(Config config);
	~Engine();

	bool start();
	void stop();

	const Config &config() const { return config_; }

	Player *player(const std::string &name);
	Recorder *recorder(const std::string &name);
	std::vector<Player *> players();
	std::vector<Recorder *> recorders();

	/* Files in media_dir, probed. */
	std::vector<MediaFile> media_files() const;
	/* Recordings on disk, newest first. */
	std::vector<MediaFile> recordings() const;

	/* Resolve a user-supplied media name to a path inside media_dir.
	 * Rejects anything that escapes the directory. */
	bool resolve_media(const std::string &name, std::string *out_path) const;

	struct DeviceStatus {
		bool up = false;
		std::string port, local_ip;
		bool redundant = false;
		std::string clock_source;
		bool ptp_locked = false;
		bool ptp_have_offset = false;
		int64_t ptp_offset_ns = 0;
		int64_t ptp_last_sync_age_ms = -1;
		uint64_t ptp_tai_ns = 0;
		std::string tai_utc;
		bool nmos_up = false;
	};
	DeviceStatus device_status() const;

private:
	void register_nmos();

	struct ProbeEntry {
		uint64_t size = 0;
		int64_t mtime = 0;
		MediaFile file;
	};
	mutable std::mutex probe_mutex_;
	mutable std::map<std::string, ProbeEntry> probe_cache_;

	Config config_;
	std::vector<std::unique_ptr<Player>> players_;
	std::vector<std::unique_ptr<Recorder>> recorders_;
	bool nmos_up_ = false;
	bool started_ = false;
};

} // namespace vcard
