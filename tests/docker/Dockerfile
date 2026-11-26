# Build-and-test image for the mock-MTL end-to-end test. No NIC, no DPDK.
FROM alpine:3.24
RUN apk add --no-cache g++ make cmake pkgconf git ffmpeg ffmpeg-dev nlohmann-json linux-headers curl jq
RUN git clone -q --depth 1 --branch v0.18.3 https://github.com/yhirose/cpp-httplib.git /opt/httplib && \
    cmake -S /opt/httplib -B /tmp/hb -DHTTPLIB_REQUIRE_OPENSSL=OFF -DHTTPLIB_USE_OPENSSL_IF_AVAILABLE=OFF \
      -DHTTPLIB_USE_ZLIB_IF_AVAILABLE=OFF -DHTTPLIB_USE_BROTLI_IF_AVAILABLE=OFF >/dev/null && \
    cmake --install /tmp/hb >/dev/null && rm -rf /tmp/hb
