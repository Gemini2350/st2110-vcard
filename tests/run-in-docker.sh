#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the daemon against the mock MTL and run the end-to-end test in a
# container. Works on any machine with Docker, including a Mac -- no NIC, no
# DPDK, no hugepages.
#
#   tests/run-in-docker.sh [codec]        codec: v210 (default) | prores422hq | ffv1 | h264
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

docker build -q -t st2110-vcard-test "$here/tests/docker" >/dev/null

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/proj" "$stage/mtl"
cp -R "$here"/{src,web,config,tests,CMakeLists.txt} "$stage/proj/"
cp -R "$MTL_DIR/include" "$stage/mtl/include"

COPYFILE_DISABLE=1 tar -C "$stage" -cf - . | docker run --rm -i -e CODEC="${1:-v210}" st2110-vcard-test sh -c '
	mkdir /w && tar -C /w -xf - 2>/dev/null && cd /w/proj &&
	cmake -B /b -DVCARD_MOCK_MTL=ON -DMTL_SOURCE_DIR=/w/mtl -DENABLE_NMOS=OFF \
	      -DCMAKE_BUILD_TYPE=RelWithDebInfo >/b.log 2>&1 || { tail -30 /b.log; exit 1; }
	cmake --build /b -j"$(nproc)" 2>&1 | grep -E "error|warning: " ; 
	[ -x /b/st2110-vcard ] || { echo "build failed"; exit 1; }
	sh tests/e2e.sh /b/st2110-vcard'
