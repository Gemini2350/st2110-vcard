/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal leveled logger. Everything goes to stderr; systemd captures it.
 * The last N lines are also kept in a ring so the web GUI can show them.
 */
#pragma once

#include <string>
#include <vector>

namespace vcard {

enum class LogLevel { Debug = 0, Info, Warn, Error };

void log_set_level(LogLevel level);
void log_write(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Newest last. Used by GET /api/log. */
std::vector<std::string> log_tail(size_t max_lines);

} // namespace vcard

#define VLOG_D(...) ::vcard::log_write(::vcard::LogLevel::Debug, __VA_ARGS__)
#define VLOG_I(...) ::vcard::log_write(::vcard::LogLevel::Info, __VA_ARGS__)
#define VLOG_W(...) ::vcard::log_write(::vcard::LogLevel::Warn, __VA_ARGS__)
#define VLOG_E(...) ::vcard::log_write(::vcard::LogLevel::Error, __VA_ARGS__)
