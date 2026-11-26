/* SPDX-License-Identifier: GPL-2.0-or-later
 * SDP helper for the NMOS node.
 *
 * Deliberately free of MTL and nmos-cpp includes so the logic can be built
 * and tested on any machine (see tests/test-sdp-util.cpp). Same function as
 * sdp_payload_type() in obs-mtl-st2110's src/rtp-util.h.
 */
#pragma once

#include <cstdlib>
#include <string>

namespace vcard_nmos {

/* RTP payload type of the first stream of the given media type ("video",
 * "audio"; nullptr or "" = any) in an SDP. Taken from a=rtpmap, or from the
 * format list of the m= line if the section has no rtpmap. -1 if not found.
 *
 * With ST 2022-7 the SDP carries one m= section per leg; both use the same
 * payload type, so the first match is enough. */
inline int sdp_payload_type(const std::string &sdp, const char *media)
{
	auto parse_pt = [](const char *s) -> int {
		char *end = nullptr;
		long v = strtol(s, &end, 10);
		if (end == s || v < 0 || v > 127)
			return -1;
		if (*end != '\0' && *end != ' ' && *end != '\t')
			return -1;
		return (int)v;
	};

	const std::string want = media ? media : "";
	bool in_section = false;
	int from_m_line = -1;

	size_t pos = 0;
	while (pos < sdp.size()) {
		size_t eol = sdp.find('\n', pos);
		if (eol == std::string::npos)
			eol = sdp.size();
		std::string line = sdp.substr(pos, eol - pos);
		pos = eol + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		if (line.compare(0, 2, "m=") == 0) {
			/* m=<media> <port>[/<n>] <proto> <fmt> ... */
			size_t sp1 = line.find(' ', 2);
			std::string m = line.substr(2, sp1 == std::string::npos ? std::string::npos
										  : sp1 - 2);
			in_section = want.empty() || m == want;
			if (!in_section || from_m_line >= 0 || sp1 == std::string::npos)
				continue;
			size_t sp2 = line.find(' ', sp1 + 1);
			if (sp2 == std::string::npos)
				continue;
			size_t sp3 = line.find(' ', sp2 + 1);
			if (sp3 == std::string::npos)
				continue;
			if (line.compare(sp2 + 1, 4, "RTP/") == 0)
				from_m_line = parse_pt(line.c_str() + sp3 + 1);
		} else if (in_section && line.compare(0, 9, "a=rtpmap:") == 0) {
			int pt = parse_pt(line.c_str() + 9);
			if (pt >= 0)
				return pt;
		}
	}
	return from_m_line;
}

} // namespace vcard_nmos
