/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "api/http-server.h"

#include "core/formats.h"
#include "core/log.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>

using json = nlohmann::json;

namespace vcard {

namespace {

const char *player_state_name(Player::State s)
{
	switch (s) {
	case Player::State::Playing: return "playing";
	case Player::State::Paused: return "paused";
	default: return "idle";
	}
}

json meters_json(const AudioMeters *m)
{
	auto l = m->levels();
	json arr = json::array();
	for (uint16_t c = 0; c < l.channels; c++)
		arr.push_back({{"peak_dbfs", l.peak_dbfs[c]}, {"over", l.over[c]}});
	return arr;
}

json player_json(Player *p)
{
	auto s = p->status();
	json j;
	j["name"] = p->config().name;
	j["state"] = player_state_name(s.state);
	j["file"] = s.current_file;
	j["index"] = s.current_index;
	j["playlist_count"] = s.playlist_count;
	j["loop"] = s.loop;
	j["position_s"] = s.position_s;
	j["duration_s"] = s.duration_s;
	j["frames_sent"] = s.frames_sent;
	j["frames_repeated"] = s.frames_repeated;
	j["audio_underruns"] = s.audio_underruns;
	j["video_queue"] = s.video_queue;
	j["video_queue_capacity"] = s.video_queue_capacity;
	j["audio_queue_ms"] = s.audio_queue_ms;
	j["video_epochs_skipped"] = s.video_epochs_skipped;
	j["audio_gap_ms"] = s.audio_gap_ms;
	j["av_skew_ms"] = s.av_skew_valid ? json(s.av_skew_ms) : json(nullptr);
	j["senders_up"] = s.senders_up;
	j["video_dest"] = s.video_sdp_dest;
	j["audio_dest"] = s.audio_sdp_dest;
	j["format"] = std::to_string(p->config().width) + "x" +
		      std::to_string(p->config().height) +
		      (p->config().interlaced ? "i" : "p") +
		      mtl_fps_name(p->config().fps);
	j["channels"] = p->config().channels;
	j["meters"] = meters_json(p->meters());

	json pl = json::array();
	for (const auto &item : p->playlist())
		pl.push_back({{"path", item.path}, {"label", item.label}});
	j["playlist"] = pl;
	return j;
}

json recorder_json(Recorder *r)
{
	auto s = r->status();
	json j;
	j["name"] = r->config().name;
	j["receivers_up"] = s.receivers_up;
	j["signal_present"] = s.signal_present;
	j["detected_format"] = s.detected_format;
	j["width"] = s.width;
	j["height"] = s.height;
	j["fps"] = s.fps;
	j["frames_received"] = s.frames_received;
	j["frames_incomplete"] = s.frames_incomplete;
	j["frames_dropped"] = s.frames_dropped;
	j["audio_frames_received"] = s.audio_frames_received;
	j["audio_gap_ms"] = s.audio_gap_ms;
	j["last_frame_age_ms"] = s.last_frame_age_ms;
	j["recording"] = s.recording;
	j["recording_path"] = s.recording_path;
	j["recorded_frames"] = s.recorded_frames;
	j["recorded_seconds"] = s.recorded_seconds;
	j["recorded_mib"] = s.recorded_mib;
	j["last_error"] = s.last_error;
	j["video_source"] = s.video_source;
	j["audio_source"] = s.audio_source;
	j["codec"] = record_codec_name(r->config().codec);
	j["channels"] = r->config().channels;
	j["meters"] = meters_json(r->meters());
	return j;
}

json media_json(const std::vector<MediaFile> &files)
{
	json arr = json::array();
	for (const auto &f : files)
		arr.push_back({{"name", f.name},
			       {"path", f.path},
			       {"size_bytes", f.size_bytes},
			       {"duration_s", f.duration_s},
			       {"format", f.format},
			       {"playable", f.playable},
			       {"note", f.note}});
	return arr;
}

void send_json(httplib::Response &res, const json &j, int status = 200)
{
	res.status = status;
	res.set_content(j.dump(), "application/json");
}

void send_error(httplib::Response &res, int status, const std::string &msg)
{
	send_json(res, json{{"error", msg}}, status);
}

json body_json(const httplib::Request &req)
{
	if (req.body.empty())
		return json::object();
	try {
		return json::parse(req.body);
	} catch (...) {
		return json::object();
	}
}

/* Streams the preview as multipart/x-mixed-replace. One connection per
 * viewer; the encode itself happens once in Preview regardless of how many
 * people are watching. */
void stream_mjpeg(httplib::Response &res, Preview *preview)
{
	auto last_gen = std::make_shared<uint64_t>(0);
	res.set_chunked_content_provider(
		"multipart/x-mixed-replace; boundary=vcardframe",
		[preview, last_gen](size_t /*offset*/, httplib::DataSink &sink) {
			/* Wait for a new picture, but wake often enough to
			 * notice a disconnected client. */
			for (int i = 0; i < 100; i++) {
				if (preview->generation() != *last_gen)
					break;
				std::this_thread::sleep_for(
					std::chrono::milliseconds(20));
			}
			auto jpeg = preview->latest_jpeg();
			if (jpeg.empty()) {
				std::this_thread::sleep_for(
					std::chrono::milliseconds(100));
				return true;
			}
			*last_gen = preview->generation();

			std::string header = "--vcardframe\r\nContent-Type: image/jpeg\r\n"
					     "Content-Length: " +
					     std::to_string(jpeg.size()) + "\r\n\r\n";
			if (!sink.write(header.data(), header.size()))
				return false;
			if (!sink.write((const char *)jpeg.data(), jpeg.size()))
				return false;
			return sink.write("\r\n", 2);
		});
}

} // namespace

HttpServer::HttpServer(Engine &engine, HttpConfig config)
	: engine_(engine), config_(std::move(config)),
	  server_(std::make_unique<httplib::Server>())
{
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start()
{
	install_routes();

	/* A request with neither Content-Length nor chunked encoding has no
	 * body (RFC 9112 6.3). cpp-httplib instead reads until the connection
	 * closes, so a bare `curl -X POST .../stop` sits in the read timeout
	 * for seconds before its handler runs. State the length for it. */
	server_->set_pre_routing_handler(
		[](const httplib::Request &req, httplib::Response &) {
			if (!req.has_header("Content-Length") &&
			    !req.has_header("Transfer-Encoding"))
				const_cast<httplib::Request &>(req).set_header(
					"Content-Length", "0");
			return httplib::Server::HandlerResponse::Unhandled;
		});

	if (!server_->set_mount_point("/", config_.web_root))
		VLOG_W("[http] web root '%s' not found -- API still available",
		       config_.web_root.c_str());

	running_ = true;
	thread_ = std::thread([this] {
		if (!server_->listen(config_.bind.c_str(), config_.port)) {
			VLOG_E("[http] cannot bind %s:%d", config_.bind.c_str(),
			       config_.port);
			running_ = false;
		}
	});
	/* listen() fails asynchronously; give it a moment so start() can
	 * report a bind failure instead of pretending everything is fine. */
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	if (!running_)
		return false;
	VLOG_I("[http] listening on http://%s:%d", config_.bind.c_str(), config_.port);
	return true;
}

void HttpServer::stop()
{
	if (!running_ && !thread_.joinable())
		return;
	running_ = false;
	server_->stop();
	if (thread_.joinable())
		thread_.join();
}

void HttpServer::install_routes()
{
	auto &srv = *server_;

	srv.Get("/api/status", [this](const httplib::Request &, httplib::Response &res) {
		auto d = engine_.device_status();
		json j;
		j["device"] = {{"up", d.up},
			       {"port", d.port},
			       {"local_ip", d.local_ip},
			       {"redundant", d.redundant},
			       {"clock_source", d.clock_source},
			       {"ptp_locked", d.ptp_locked},
			       {"ptp_have_offset", d.ptp_have_offset},
			       {"ptp_offset_ns", d.ptp_offset_ns},
			       {"ptp_last_sync_age_ms", d.ptp_last_sync_age_ms},
			       {"tai_utc", d.tai_utc},
			       {"nmos_up", d.nmos_up}};
		json ps = json::array();
		for (auto *p : engine_.players())
			ps.push_back(player_json(p));
		j["players"] = ps;
		json rs = json::array();
		for (auto *r : engine_.recorders())
			rs.push_back(recorder_json(r));
		j["recorders"] = rs;
		send_json(res, j);
	});

	srv.Get("/api/media", [this](const httplib::Request &, httplib::Response &res) {
		send_json(res, media_json(engine_.media_files()));
	});

	srv.Get("/api/recordings", [this](const httplib::Request &, httplib::Response &res) {
		send_json(res, media_json(engine_.recordings()));
	});

	srv.Get("/api/log", [](const httplib::Request &req, httplib::Response &res) {
		size_t n = 200;
		if (req.has_param("lines"))
			n = (size_t)atoi(req.get_param_value("lines").c_str());
		send_json(res, json(log_tail(n)));
	});

	/* --- player transport ---------------------------------------------- */

	auto with_player = [this](const httplib::Request &req, httplib::Response &res,
				  const std::function<void(Player *, const json &)> &fn) {
		Player *p = engine_.player(req.path_params.at("name"));
		if (!p) {
			send_error(res, 404, "no such player");
			return;
		}
		fn(p, body_json(req));
	};

	srv.Post("/api/player/:name/load", [this, with_player](const httplib::Request &req,
							       httplib::Response &res) {
		with_player(req, res, [&](Player *p, const json &b) {
			std::string file = b.value("file", "");
			std::string path;
			if (!engine_.resolve_media(file, &path)) {
				send_error(res, 400, "file not found in media dir: " + file);
				return;
			}
			if (!p->load(path)) {
				send_error(res, 500, "cannot play " + file);
				return;
			}
			send_json(res, player_json(p));
		});
	});

	srv.Post("/api/player/:name/playlist",
		 [this, with_player](const httplib::Request &req, httplib::Response &res) {
			 with_player(req, res, [&](Player *p, const json &b) {
				 std::vector<PlaylistItem> items;
				 std::vector<std::string> missing;
				 for (const auto &e : b.value("items", json::array())) {
					 std::string name =
						 e.is_string() ? e.get<std::string>()
							       : e.value("file", "");
					 std::string path;
					 if (!engine_.resolve_media(name, &path)) {
						 missing.push_back(name);
						 continue;
					 }
					 items.push_back({path, name});
				 }
				 if (!missing.empty()) {
					 send_error(res, 400,
						    "not found: " + missing.front());
					 return;
				 }
				 p->set_playlist(std::move(items));
				 send_json(res, player_json(p));
			 });
		 });

	srv.Post("/api/player/:name/play", [with_player](const httplib::Request &req,
							 httplib::Response &res) {
		with_player(req, res, [&](Player *p, const json &) {
			if (!p->play()) {
				send_error(res, 409, "nothing loaded");
				return;
			}
			send_json(res, player_json(p));
		});
	});

	srv.Post("/api/player/:name/pause", [with_player](const httplib::Request &req,
							  httplib::Response &res) {
		with_player(req, res, [&](Player *p, const json &) {
			p->pause();
			send_json(res, player_json(p));
		});
	});

	srv.Post("/api/player/:name/stop", [with_player](const httplib::Request &req,
							 httplib::Response &res) {
		with_player(req, res, [&](Player *p, const json &) {
			p->stop();
			send_json(res, player_json(p));
		});
	});

	srv.Post("/api/player/:name/next", [with_player](const httplib::Request &req,
							 httplib::Response &res) {
		with_player(req, res, [&](Player *p, const json &) {
			p->next();
			send_json(res, player_json(p));
		});
	});

	srv.Post("/api/player/:name/previous", [with_player](const httplib::Request &req,
							     httplib::Response &res) {
		with_player(req, res, [&](Player *p, const json &) {
			p->previous();
			send_json(res, player_json(p));
		});
	});

	srv.Post("/api/player/:name/seek", [with_player](const httplib::Request &req,
							 httplib::Response &res) {
		with_player(req, res, [&](Player *p, const json &b) {
			double sec = b.value("seconds", 0.0);
			p->seek((int64_t)(sec * 1e9));
			send_json(res, player_json(p));
		});
	});

	srv.Post("/api/player/:name/loop", [with_player](const httplib::Request &req,
							 httplib::Response &res) {
		with_player(req, res, [&](Player *p, const json &b) {
			p->set_loop(b.value("enabled", !p->loop()));
			send_json(res, player_json(p));
		});
	});

	/* --- recorder ------------------------------------------------------- */

	srv.Post("/api/recorder/:name/start",
		 [this](const httplib::Request &req, httplib::Response &res) {
			 Recorder *r = engine_.recorder(req.path_params.at("name"));
			 if (!r) {
				 send_error(res, 404, "no such recorder");
				 return;
			 }
			 auto b = body_json(req);
			 std::string filename = b.value("filename", "");
			 /* keep the name a plain basename: it is joined onto
			  * output_dir and must not steer the write elsewhere */
			 if (filename.find('/') != std::string::npos) {
				 send_error(res, 400, "filename must not contain '/'");
				 return;
			 }
			 if (r->recording()) {
				 send_error(res, 409, "already recording");
				 return;
			 }
			 if (!r->start_recording(filename)) {
				 std::string why = r->status().last_error;
				 send_error(res, 500,
					    "cannot start recording: " +
						    (why.empty() ? "receivers are down" : why));
				 return;
			 }
			 send_json(res, recorder_json(r));
		 });

	srv.Post("/api/recorder/:name/stop",
		 [this](const httplib::Request &req, httplib::Response &res) {
			 Recorder *r = engine_.recorder(req.path_params.at("name"));
			 if (!r) {
				 send_error(res, 404, "no such recorder");
				 return;
			 }
			 r->stop_recording();
			 send_json(res, recorder_json(r));
		 });

	/* --- confidence monitoring ------------------------------------------ */

	srv.Get("/api/preview/recorder/:name/mjpeg",
		[this](const httplib::Request &req, httplib::Response &res) {
			Recorder *r = engine_.recorder(req.path_params.at("name"));
			if (!r) {
				send_error(res, 404, "no such recorder");
				return;
			}
			stream_mjpeg(res, r->preview());
		});

	srv.Get("/api/preview/player/:name/mjpeg",
		[this](const httplib::Request &req, httplib::Response &res) {
			Player *p = engine_.player(req.path_params.at("name"));
			if (!p) {
				send_error(res, 404, "no such player");
				return;
			}
			stream_mjpeg(res, p->preview());
		});

	/* Single still, for thumbnails and for scripting a quick look. */
	srv.Get("/api/preview/recorder/:name/jpeg",
		[this](const httplib::Request &req, httplib::Response &res) {
			Recorder *r = engine_.recorder(req.path_params.at("name"));
			if (!r) {
				send_error(res, 404, "no such recorder");
				return;
			}
			auto jpeg = r->preview()->latest_jpeg();
			if (jpeg.empty()) {
				send_error(res, 503, "no picture yet");
				return;
			}
			res.set_content((const char *)jpeg.data(), jpeg.size(),
					"image/jpeg");
		});

	srv.Get("/api/preview/player/:name/jpeg",
		[this](const httplib::Request &req, httplib::Response &res) {
			Player *p = engine_.player(req.path_params.at("name"));
			if (!p) {
				send_error(res, 404, "no such player");
				return;
			}
			auto jpeg = p->preview()->latest_jpeg();
			if (jpeg.empty()) {
				send_error(res, 503, "no picture yet");
				return;
			}
			res.set_content((const char *)jpeg.data(), jpeg.size(),
					"image/jpeg");
		});
}

} // namespace vcard
