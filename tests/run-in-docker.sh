#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the daemon against the mock MTL and run the end-to-end tests in a
# container. Works on any machine with Docker, including a Mac -- no NIC, no
# DPDK, no hugepages.
#
#   tests/run-in-docker.sh [codec]     media path only; codec: v210 (default),
#                                      prores422hq, ffv1, h264
#   tests/run-in-docker.sh nmos        also builds nmos-cpp into the image (slow
#                                      the first time), links the NMOS node and
#                                      runs tests/e2e-nmos.sh against Sony's
#                                      nmos-cpp-registry
#
# Needs a Media-Transport-Library checkout for its headers (not built):
#   MTL_DIR=/path/to/Media-Transport-Library tests/run-in-docker.sh
#
# The sources are streamed into the container rather than bind-mounted: mounts
# of cloud-synced folders (iCloud Documents on macOS) fail in odd ways from
# inside a Linux VM.

set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
MTL_DIR=${MTL_DIR:-$here/../Media-Transport-Library}
[ -f "$MTL_DIR/include/mtl_api.h" ] || { echo "MTL headers not found in $MTL_DIR/include (set MTL_DIR)"; exit 1; }

mode=${1:-v210}
if [ "$mode" = nmos ]; then
	image=st2110-vcard-test-nmos
	docker build -q -t $image -f "$here/tests/docker/Dockerfile.nmos" "$here/tests/docker" >/dev/null
	nmos=ON; codec=v210
else
	image=st2110-vcard-test
	docker build -q -t $image "$here/tests/docker" >/dev/null
	nmos=OFF; codec=$mode
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/proj" "$stage/mtl"
cp -R "$here"/{src,web,config,tests,CMakeLists.txt} "$stage/proj/"
cp -R "$MTL_DIR/include" "$stage/mtl/include"

COPYFILE_DISABLE=1 tar -C "$stage" -cf - . | docker run --rm -i -e CODEC="$codec" -e NMOS="$nmos" \
	-e KEEP="${KEEP:-}" -e LOGTAIL="${LOGTAIL:-}" $image sh -c '
	mkdir /w && tar -C /w -xf - 2>/dev/null && cd /w/proj &&
	cmake -B /b -DVCARD_MOCK_MTL=ON -DMTL_SOURCE_DIR=/w/mtl -DENABLE_NMOS=$NMOS \
	      -DCMAKE_BUILD_TYPE=RelWithDebInfo >/b.log 2>&1 || { tail -30 /b.log; exit 1; }
	grep -E "NMOS node|nmos-cpp not found" /b.log
	cmake --build /b -j"$(nproc)" 2>&1 | grep -E "error|warning: |undefined reference" | head -40
	[ -x /b/st2110-vcard ] || { echo "build failed"; exit 1; }
	rc=0
	sh tests/e2e.sh /b/st2110-vcard || rc=1
	if [ "$NMOS" = ON ]; then sh tests/e2e-nmos.sh /b/st2110-vcard || rc=1; fi
	exit $rc'
