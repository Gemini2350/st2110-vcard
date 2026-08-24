/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>

namespace vcard {

namespace {
LogLevel g_level = LogLevel::Info;
std::mutex g_mutex;
std::deque<std::string> g_ring;
constexpr size_t kRingMax = 500;

const char *level_name(LogLevel l)
{
	switch (l) {
	case LogLevel::Debug: return "DBG";
	case LogLevel::Info: return "INF";
	case LogLevel::Warn: return "WRN";
	default: return "ERR";
	}
}
} // namespace

void log_set_level(LogLevel level) { g_level = level; }

void log_write(LogLevel level, const char *fmt, ...)
{
	if (level < g_level)
		return;

	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);

	char line[1200];
	snprintf(line, sizeof(line), "%02d:%02d:%02d.%03ld %s %s", tm.tm_hour,
		 tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, level_name(level), msg);

	std::lock_guard<std::mutex> lock(g_mutex);
	fprintf(stderr, "%s\n", line);
	g_ring.emplace_back(line);
	while (g_ring.size() > kRingMax)
		g_ring.pop_front();
}

std::vector<std::string> log_tail(size_t max_lines)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	size_t n = std::min(max_lines, g_ring.size());
	return std::vector<std::string>(g_ring.end() - n, g_ring.end());
}

} // namespace vcard
