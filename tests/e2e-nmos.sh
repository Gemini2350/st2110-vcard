#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# NMOS end-to-end test: the daemon (mock MTL, real embedded nmos-cpp node)
# against Sony's nmos-cpp-registry, driven the way a broadcast controller
# drives a device -- read the sender's SDP, PATCH it to the receiver, retarget
# the sender. Needs a build with -DVCARD_MOCK_MTL=ON -DENABLE_NMOS=ON, plus
# nmos-cpp-registry, ffmpeg, curl and jq.
#
#   tests/e2e-nmos.sh build/st2110-vcard
#
# The media path is the loopback mock, so "the receiver joined the sender's
# group" is observable as signal on the recorder. Nothing here involves a real
# network or mDNS; the registry is given by address.

BIN=${1:-build/st2110-vcard}
API=http://127.0.0.1:18088
REG=http://127.0.0.1:8010
NODE=http://127.0.0.1:3212
CONN=http://127.0.0.1:3215/x-nmos/connection/v1.1/single
Q=$REG/x-nmos/query/v1.3
W=$(mktemp -d)
mkdir -p "$W/media" "$W/rec"
PASS=0; FAIL=0

ok()   { PASS=$((PASS+1)); echo "  ok    $*"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL  $*"; }
check(){ desc=$1; shift; if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi; }
rstat(){ curl -sf $API/api/status | jq -r ".recorders[0].$1"; }
pstat(){ curl -sf $API/api/status | jq -r ".players[0].$1"; }
count(){ curl -sf "$Q/$1" | jq length; }
# wait_for <seconds> <command...>: poll until the command succeeds
wait_for(){ n=$(($1*5)); shift; i=0; while [ $i -lt $n ]; do "$@" >/dev/null 2>&1 && return 0; sleep 0.2; i=$((i+1)); done; return 1; }
has()  { grep -q -- "$1" "$2"; }
# patch <url> <json-file>: prints the HTTP status, body goes to $W/patch.out
patch(){ curl -s -o "$W/patch.out" -w '%{http_code}' -X PATCH -H 'Content-Type: application/json' --data-binary "@$2" "$1"; }

cleanup(){ kill $PID $RPID 2>/dev/null; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT

echo "== starting registry and daemon"
cat > "$W/registry.json" <<JSON
{ "http_port": 8010, "host_address": "127.0.0.1", "logging_level": 10, "pri": 100 }
JSON
nmos-cpp-registry "$W/registry.json" > "$W/registry.log" 2>&1 &
RPID=$!
wait_for 20 curl -sf "$Q/nodes" || { echo "registry did not come up"; tail -20 "$W/registry.log"; exit 1; }

ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=s=320x180:r=50:d=60 \
	-f lavfi -i sine=f=1000:d=60 -c:v mpeg4 -q:v 6 -c:a pcm_s16le -ac 2 "$W/media/clip.mov"

# The recorder starts on groups nobody sends to: any signal it shows later got
# there through an IS-05 activation.
cat > "$W/config.json" <<JSON
{
  "log_level": "debug",
  "media_dir": "$W/media",
  "device": { "port": "kernel:lo", "ptp": true },
  "http": { "bind": "127.0.0.1", "port": 18088, "web_root": "$(dirname "$0")/../web" },
  "nmos": { "enabled": true, "label": "vcard under test", "host_address": "127.0.0.1",
            "registry_address": "127.0.0.1", "registry_port": 8010, "http_port": 3212,
            "seed_file": "$W/nmos-seed" },
  "players": [ { "name": "p1", "width": 320, "height": 180, "fps": "50",
      "video_dest_ip": "239.1.1.1", "video_port": 20000, "video_payload_type": 96,
      "audio_dest_ip": "239.1.1.2", "audio_port": 30000, "audio_payload_type": 97, "channels": 2 } ],
  "recorders": [ { "name": "r1", "width": 320, "height": 180, "fps": "50", "auto_detect": true,
      "video_multicast_ip": "239.9.9.1", "video_port": 20900,
      "audio_multicast_ip": "239.9.9.2", "audio_port": 30900, "channels": 2,
      "output_dir": "$W/rec", "codec": "v210" } ]
}
JSON
"$BIN" -c "$W/config.json" > "$W/daemon.log" 2>&1 &
PID=$!
wait_for 15 curl -sf $API/api/status || { echo "daemon did not come up"; tail -30 "$W/daemon.log"; exit 1; }
curl -sf -X POST -H 'Content-Type: application/json' -d '{"enabled":true}' $API/api/player/p1/loop >/dev/null
curl -sf -X POST -H 'Content-Type: application/json' -d '{"file":"clip.mov"}' $API/api/player/p1/load >/dev/null

echo "== IS-04: registration"
check "daemon reports the node as up" [ "$(curl -sf $API/api/status | jq -r .device.nmos_up)" = true ]
check "node registered with the registry" wait_for 20 sh -c "[ \"\$(curl -sf $Q/senders | jq length)\" = 2 ]"
# (the registry lists itself as a node too, so count ours by label)
n=$(curl -sf "$Q/nodes" | jq '[.[]|select(.label=="vcard under test")]|length')
check "registry has our node once ($n)" [ "$n" = 1 ]
NID=$(curl -sf "$Q/nodes" | jq -r '.[]|select(.label=="vcard under test").id')
n=$(curl -sf "$Q/devices" | jq --arg n "$NID" '[.[]|select(.node_id==$n)]|length')
check "with one device ($n)" [ "$n" = 1 ]
for kind in sources:2 flows:2 senders:2 receivers:2; do
	n=$(count "${kind%%:*}")
	check "registry has ${kind##*:} ${kind%%:*} ($n)" [ "$n" = "${kind##*:}" ]
done
check "Node API answers" curl -sf "$NODE/x-nmos/node/v1.3/self"
VS=$(curl -sf "$Q/senders" | jq -r '.[]|select(.label=="p1 video").id')
AS=$(curl -sf "$Q/senders" | jq -r '.[]|select(.label=="p1 audio").id')
VR=$(curl -sf "$Q/receivers" | jq -r '.[]|select(.label=="r1 video").id')
AR=$(curl -sf "$Q/receivers" | jq -r '.[]|select(.label=="r1 audio").id')
check "senders and receivers carry the session names" [ -n "$VS" -a -n "$AS" -a -n "$VR" -a -n "$AR" ]
vf=$(curl -sf "$Q/flows" | jq -c '.[]|select(.format=="urn:x-nmos:format:video")|[.media_type,.frame_width,.frame_height,.grain_rate.numerator,.components[0].bit_depth]')
check "video flow describes the session ($vf)" [ "$vf" = '["video/raw",320,180,50,10]' ]
af=$(curl -sf "$Q/flows" | jq -c '.[]|select(.format=="urn:x-nmos:format:audio")|[.media_type,.sample_rate.numerator,.bit_depth]')
check "audio flow describes the session ($af)" [ "$af" = '["audio/L24",48000,24]' ]
check "video receiver accepts raw video" [ "$(curl -sf "$Q/receivers/$VR" | jq -r '.caps.media_types[0]')" = video/raw ]

echo "== IS-05: a transmitting sender is an active sender"
check "video sender is active" [ "$(curl -sf "$CONN/senders/$VS/active" | jq -r .master_enable)" = true ]
check "its IS-04 subscription says so too" wait_for 5 sh -c "[ \"\$(curl -sf $Q/senders/$VS | jq -r .subscription.active)\" = true ]"
curl -sfL "$CONN/senders/$VS/transportfile" -o "$W/v.sdp"
curl -sfL "$CONN/senders/$AS/transportfile" -o "$W/a.sdp"
check "video SDP is served" [ -s "$W/v.sdp" ]
for want in "m=video 20000 RTP/AVP 96" "c=IN IP4 239.1.1.1" "a=rtpmap:96 raw/90000" \
	"sampling=YCbCr-4:2:2" "width=320" "height=180" "exactframerate=50" "depth=10" \
	"TP=2110TPW" "a=source-filter: incl IN IP4 239.1.1.1 127.0.0.1" "a=ts-refclk:" "a=mediaclk:direct=0"; do
	check "video SDP has '$want'" has "$want" "$W/v.sdp"
done
for want in "m=audio 30000 RTP/AVP 97" "c=IN IP4 239.1.1.2" "a=rtpmap:97 L24/48000/2" "a=ptime:1"; do
	check "audio SDP has '$want'" has "$want" "$W/a.sdp"
done

echo "== IS-05: connect the receiver the way a controller does"
check "recorder has no signal before the connection" [ "$(rstat signal_present)" = false ]
jq -n --arg sdp "$(cat "$W/v.sdp")" --arg s "$VS" \
	'{sender_id:$s, master_enable:true, activation:{mode:"activate_immediate"}, transport_file:{data:$sdp, type:"application/sdp"}}' > "$W/p.json"
code=$(patch "$CONN/receivers/$VR/staged" "$W/p.json")
check "PATCH video receiver with the sender's SDP (HTTP $code)" [ "$code" = 200 ]
check "recorder moved to the sender's group ($(rstat video_source))" wait_for 5 sh -c "[ \"\$(curl -sf $API/api/status | jq -r '.recorders[0].video_source')\" = 239.1.1.1:20000 ]"
check "and now has picture" wait_for 6 sh -c "[ \"\$(curl -sf $API/api/status | jq -r '.recorders[0].signal_present')\" = true ]"
check "source filter taken from the SDP ($(rstat video_source_ip))" [ "$(rstat video_source_ip)" = 127.0.0.1 ]
jq -n --arg sdp "$(cat "$W/a.sdp")" --arg s "$AS" \
	'{sender_id:$s, master_enable:true, activation:{mode:"activate_immediate"}, transport_file:{data:$sdp, type:"application/sdp"}}' > "$W/p.json"
code=$(patch "$CONN/receivers/$AR/staged" "$W/p.json")
check "PATCH audio receiver (HTTP $code)" [ "$code" = 200 ]
check "recorder hears the tone" wait_for 6 sh -c "curl -sf $API/api/status | jq -e '[.recorders[0].meters[].peak_dbfs]|max > -40'"
check "IS-04 shows the receiver subscribed to the sender" wait_for 5 sh -c "[ \"\$(curl -sf $Q/receivers/$VR | jq -r .subscription.sender_id)\" = $VS ]"
check "receiver reads back as active" [ "$(curl -sf "$CONN/receivers/$VR/active" | jq -r .master_enable)" = true ]

echo "== IS-05: payload type travels in the SDP"
sed -e 's/RTP\/AVP 96/RTP\/AVP 100/' -e 's/rtpmap:96 /rtpmap:100 /' -e 's/fmtp:96 /fmtp:100 /' "$W/v.sdp" > "$W/v100.sdp"
jq -n --arg sdp "$(cat "$W/v100.sdp")" --arg s "$VS" \
	'{sender_id:$s, master_enable:true, activation:{mode:"activate_immediate"}, transport_file:{data:$sdp, type:"application/sdp"}}' > "$W/p.json"
code=$(patch "$CONN/receivers/$VR/staged" "$W/p.json")
check "receiver takes payload type 100 from the SDP (HTTP $code, now $(rstat video_payload_type))" wait_for 3 sh -c "[ \"\$(curl -sf $API/api/status | jq -r '.recorders[0].video_payload_type')\" = 100 ]"
echo '{"master_enable":true,"activation":{"mode":"activate_immediate"},"transport_params":[{"multicast_ip":"239.1.1.1","destination_port":20000}]}' > "$W/p.json"
code=$(patch "$CONN/receivers/$VR/staged" "$W/p.json")
check "an activation without SDP leaves the payload type alone (HTTP $code, $(rstat video_payload_type))" [ "$(rstat video_payload_type)" = 100 ]

echo "== IS-05: retarget the sender"
echo '{"master_enable":true,"activation":{"mode":"activate_immediate"},"transport_params":[{"destination_ip":"239.2.2.1","destination_port":20002}]}' > "$W/p.json"
code=$(patch "$CONN/senders/$VS/staged" "$W/p.json")
check "PATCH video sender to a new group (HTTP $code)" [ "$code" = 200 ]
check "player sends to the new group ($(pstat video_dest))" wait_for 5 sh -c "[ \"\$(curl -sf $API/api/status | jq -r '.players[0].video_dest')\" = 239.2.2.1:20002 ]"
check "player keeps its payload type ($(pstat video_payload_type))" [ "$(pstat video_payload_type)" = 96 ]
check "receiver on the old group loses the picture" wait_for 6 sh -c "[ \"\$(curl -sf $API/api/status | jq -r '.recorders[0].signal_present')\" = false ]"
curl -sfL "$CONN/senders/$VS/transportfile" -o "$W/v2.sdp"
check "sender's SDP follows (c=239.2.2.1, port 20002)" sh -c "grep -q 'c=IN IP4 239.2.2.1' '$W/v2.sdp' && grep -q 'm=video 20002 ' '$W/v2.sdp'"
jq -n --arg sdp "$(cat "$W/v2.sdp")" --arg s "$VS" \
	'{sender_id:$s, master_enable:true, activation:{mode:"activate_immediate"}, transport_file:{data:$sdp, type:"application/sdp"}}' > "$W/p.json"
code=$(patch "$CONN/receivers/$VR/staged" "$W/p.json")
check "receiver follows with the new SDP and has picture again (HTTP $code)" wait_for 6 sh -c "[ \"\$(curl -sf $API/api/status | jq -r '.recorders[0].signal_present')\" = true ]"
check "playout survived the retarget (state $(pstat state))" [ "$(pstat state)" = playing ]

echo "== IS-05: disconnect"
echo '{"master_enable":false,"activation":{"mode":"activate_immediate"}}' > "$W/p.json"
code=$(patch "$CONN/receivers/$VR/staged" "$W/p.json")
check "PATCH master_enable=false on the receiver (HTTP $code)" [ "$code" = 200 ]
check "receiver stops receiving" wait_for 6 sh -c "[ \"\$(curl -sf $API/api/status | jq -r '.recorders[0].signal_present')\" = false ]"
check "IS-04 shows it inactive" wait_for 5 sh -c "[ \"\$(curl -sf $Q/receivers/$VR | jq -r .subscription.active)\" = false ]"
code=$(patch "$CONN/senders/$AS/staged" "$W/p.json")
check "PATCH master_enable=false on the audio sender (HTTP $code)" [ "$code" = 200 ]
check "audio sender goes quiet on the wire" wait_for 6 sh -c "a=\$(curl -sf $API/api/status | jq '.recorders[0].audio_frames_received'); sleep 1; b=\$(curl -sf $API/api/status | jq '.recorders[0].audio_frames_received'); [ \"\$a\" = \"\$b\" ]"
check "daemon is still healthy" [ "$(curl -sf $API/api/status | jq -r .device.up)" = true ]

echo "== identity survives a restart"
kill -TERM $PID; wait $PID 2>/dev/null
"$BIN" -c "$W/config.json" > "$W/daemon2.log" 2>&1 &
PID=$!
wait_for 15 curl -sf $API/api/status
check "node re-registered" wait_for 20 sh -c "[ \"\$(curl -sf $Q/senders | jq length)\" = 2 ]"
VS2=$(curl -sf "$Q/senders" | jq -r '.[]|select(.label=="p1 video").id')
VR2=$(curl -sf "$Q/receivers" | jq -r '.[]|select(.label=="r1 video").id')
check "sender id is the same after a restart" [ "$VS2" = "$VS" ]
check "receiver id is the same after a restart" [ "$VR2" = "$VR" ]

echo "== shutdown"
kill -TERM $PID
i=0; while kill -0 $PID 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
if kill -0 $PID 2>/dev/null; then bad "daemon hung on SIGTERM"; kill -9 $PID; else
	wait $PID; rc=$?; check "clean exit on SIGTERM with NMOS running (rc=$rc)" [ "$rc" = 0 ]; fi

echo
echo "passed $PASS, failed $FAIL"
if [ "$FAIL" -gt 0 ] || [ -n "$KEEP" ]; then
	echo "---- last PATCH response ----"; head -c 600 "$W/patch.out" 2>/dev/null; echo
	echo "---- video SDP ----"; cat "$W/v.sdp" 2>/dev/null
	echo "---- daemon log (nmos / errors) ----"
	grep -E "nmos|WRN|ERR|reconfig|senders up|receivers up" "$W/daemon.log" | grep -v "^\[mock" | tail -${LOGTAIL:-40} | cut -c1-220
fi
[ "$FAIL" = 0 ]
