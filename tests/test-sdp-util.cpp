/* SPDX-License-Identifier: GPL-2.0-or-later
 * Standalone test for src/nmos/sdp-util.h. Needs neither MTL nor nmos-cpp:
 *
 *   c++ -std=c++17 -Wall -Wextra -I src tests/test-sdp-util.cpp -o /tmp/test-sdp-util
 *   /tmp/test-sdp-util
 */
#include "nmos/sdp-util.h"

#include <cstdio>

using vcard_nmos::sdp_payload_type;

static int failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static void test_sdp()
{
	const std::string video =
		"v=0\r\n"
		"o=- 123456 11 IN IP4 192.168.100.2\r\n"
		"s=Example of a SMPTE ST2110-20 signal\r\n"
		"t=0 0\r\n"
		"m=video 50000 RTP/AVP 112\r\n"
		"c=IN IP4 239.100.9.10/32\r\n"
		"a=source-filter: incl IN IP4 239.100.9.10 192.168.100.2\r\n"
		"a=rtpmap:112 raw/90000\r\n"
		"a=fmtp:112 sampling=YCbCr-4:2:2; width=1920; height=1080; "
		"exactframerate=50; depth=10; TCS=SDR; colorimetry=BT709; PM=2110GPM; "
		"SSN=ST2110-20:2017; TP=2110TPN; \r\n"
		"a=ts-refclk:ptp=IEEE1588-2008:39-A7-94-FF-FE-07-CB-D0:37\r\n"
		"a=mediaclk:direct=0\r\n";
	CHECK(sdp_payload_type(video, "video") == 112);
	CHECK(sdp_payload_type(video, nullptr) == 112);
	CHECK(sdp_payload_type(video, "") == 112);
	CHECK(sdp_payload_type(video, "audio") == -1);

	const std::string audio =
		"v=0\n"
		"o=- 1 1 IN IP4 10.0.0.1\n"
		"s=audio\n"
		"t=0 0\n"
		"m=audio 30000 RTP/AVP 98\n"
		"c=IN IP4 239.168.85.30/64\n"
		"a=rtpmap:98 L24/48000/2\n"
		"a=ptime:1\n";
	CHECK(sdp_payload_type(audio, "audio") == 98);
	CHECK(sdp_payload_type(audio, "video") == -1);

	/* ST 2022-7: two legs, same payload type */
	const std::string dup =
		"v=0\r\n"
		"a=group:DUP PRIMARY SECONDARY\r\n"
		"m=video 20000 RTP/AVP 96\r\n"
		"a=rtpmap:96 raw/90000\r\n"
		"a=mid:PRIMARY\r\n"
		"m=video 20000 RTP/AVP 96\r\n"
		"a=rtpmap:96 raw/90000\r\n"
		"a=mid:SECONDARY\r\n";
	CHECK(sdp_payload_type(dup, "video") == 96);

	/* both essences in one file: pick by media type */
	const std::string both =
		"v=0\n"
		"m=audio 30000 RTP/AVP 97\n"
		"a=rtpmap:97 L24/48000/8\n"
		"m=video 20000 RTP/AVP 100\n"
		"a=rtpmap:100 raw/90000\n";
	CHECK(sdp_payload_type(both, "video") == 100);
	CHECK(sdp_payload_type(both, "audio") == 97);
	CHECK(sdp_payload_type(both, nullptr) == 97);

	/* no rtpmap: fall back to the m= format list */
	CHECK(sdp_payload_type("v=0\nm=video 20000 RTP/AVP 103\nc=IN IP4 239.1.1.1/32\n",
			       "video") == 103);
	/* port with a count, last line without a newline */
	CHECK(sdp_payload_type("m=audio 5004/2 RTP/AVP 97", "audio") == 97);
	/* a broken rtpmap line does not hide the m= line */
	CHECK(sdp_payload_type("m=video 20000 RTP/AVP 96\na=rtpmap:abc raw/90000\n", "video") ==
	      96);

	/* nothing usable */
	CHECK(sdp_payload_type("", "video") == -1);
	CHECK(sdp_payload_type("v=0\ns=x\n", "video") == -1);
	CHECK(sdp_payload_type("m=video 20000 RTP/AVP 300\na=rtpmap:300 raw/90000\n",
			       "video") == -1);
	CHECK(sdp_payload_type("m=video 20000 udp 33\n", "video") == -1);
	/* rtpmap outside any media section is ignored */
	CHECK(sdp_payload_type("v=0\na=rtpmap:99 raw/90000\n", nullptr) == -1);
}

int main()
{
	test_sdp();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
