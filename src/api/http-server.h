/* SPDX-License-Identifier: GPL-2.0-or-later
 * REST API + static web GUI + MJPEG confidence streams.
 */
#pragma once

#include "engine.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace vcard {

class HttpServer {
public:
	HttpServer(Engine &engine, HttpConfig config);
	~HttpServer();

	bool start();
	void stop();

private:
	void install_routes();

	Engine &engine_;
	HttpConfig config_;
	std::unique_ptr<httplib::Server> server_;
	std::thread thread_;
	std::atomic<bool> running_{false};
};

} // namespace vcard
