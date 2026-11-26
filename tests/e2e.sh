#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# End-to-end test against the mock MTL loopback: the player sends to a
# multicast group, the recorder listens on the same group, and everything in
# between is the real daemon -- decoder, transport threads, preview, encoder,
# REST API. Needs: a build with -DVCARD_MOCK_MTL=ON, ffmpeg, ffprobe, curl, jq.
#
#   tests/e2e.sh build/st2110-vcard
#
# Says nothing about the network side (pacing, PTP, IGMP, loss). See
# tests/mock-mtl/mock_mtl.cpp for what the mock does and does not model.

BIN=${1:-build/st2110-vcard}
PORT=18088
API=http://127.0.0.1:$PORT
W=$(mktemp -d)
mkdir -p "$W/media" "$W/rec"
PASS=0; FAIL=0

ok()   { PASS=$((PASS+1)); echo "  ok    $*"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL  $*"; }
check(){ desc=$1; shift; if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi; }
# numeric range check: in_range <value> <min> <max>
in_range(){ awk -v v="$1" -v a="$2" -v b="$3" 'BEGIN{exit !(v+0>=a && v+0<=b)}'; }
# every GET leaves its duration behind, so a slow answer shows up by name
get()  { curl -sf -w "%{time_total} $1\n" -o "$W/get.out" "$API$1" >> "$W/calls.log"; rc=$?; cat "$W/get.out" 2>/dev/null; return $rc; }
post() { curl -sf -w "%{time_total} POST $1\n" -o "$W/post.out" -X POST -H 'Content-Type: application/json' -d "${2:-{\}}" "$API$1" >> "$W/calls.log"; rc=$?; cat "$W/post.out" 2>/dev/null; return $rc; }
fetch(){ curl -sf -w "%{time_total} $1\n" -o "$2" "$API$1" >> "$W/calls.log"; }
pstat(){ get /api/status | jq -r ".players[0].$1"; }
rstat(){ get /api/status | jq -r ".recorders[0].$1"; }
# Not `date +%s.%N`: BusyBox date has no %N, which silently turns every
# measurement into whole seconds -- and a 6.0 s clip into "6 or 7".
now()  { cut -d' ' -f1 /proc/uptime; }
since(){ awk -v a="$1" -v b="$(now)" 'BEGIN{printf "%.2f", b-a}'; }
wait_idle(){ # wait_idle <timeout_s>
	i=0
	while [ $i -lt $(($1*20)) ]; do
		[ "$(pstat state)" = idle ] && return 0
		sleep 0.05; i=$((i+1))
	done
	return 1
}
# detect <file>: flash and beep onsets of a file into $W/flash.txt / $W/beep.txt
detect(){
	ffmpeg -hide_banner -nostats -i "$1" -vf blackdetect=d=0.05:pix_th=0.2 \
		-af silencedetect=n=-40dB:d=0.05 -f null - 2> "$W/detect.log"
	grep -o 'black_end:[0-9.]*' "$W/detect.log" | cut -d: -f2 > "$W/flash.txt"
	grep -o 'black_start:[0-9.]*' "$W/detect.log" | cut -d: -f2 > "$W/flashend.txt"
	grep -o 'silence_end: [0-9.]*' "$W/detect.log" | awk '{print $2}' > "$W/beep.txt"
}
nth()  { sed -n "${2}p" "$W/$1.txt"; }
ms()   { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.0f", (b-a)*1000}'; }

echo "== generating test media"
FF="ffmpeg -hide_banner -loglevel error -y"
# 25p material for a 50p session: must play for its duration, not at 2x
$FF -f lavfi -i testsrc2=s=320x180:r=25:d=6 -f lavfi -i sine=f=1000:d=6 \
	-c:v mpeg4 -q:v 4 -c:a pcm_s16le -ac 2 "$W/media/clip25.mov"
# timestamps that do not start at zero (camera files, MXF with timecode)
$FF -f lavfi -i testsrc2=s=320x180:r=50:d=4 -f lavfi -i sine=f=440:d=4 \
	-c:v mpeg4 -q:v 4 -c:a pcm_s16le -ac 2 -output_ts_offset 3600 -f mpegts "$W/media/offset.ts"
# lip-sync probe: a white flash and a beep, both exactly from 2.0 s to 2.5 s
# (aevalsrc, not a gated sine: filter enables only switch on frame boundaries)
$FF -f lavfi -i color=c=black:s=320x180:r=50:d=5 \
	-f lavfi -i "aevalsrc='if(between(t,2,2.5),0.5*sin(2*PI*1000*t),0)':s=48000:d=5" \
	-vf "drawbox=enable='between(t,2,2.5)':color=white:t=fill" \
	-c:v mpeg4 -q:v 2 -g 1 -c:a pcm_s16le -ac 2 "$W/media/sync.mov"
# video only
$FF -f lavfi -i testsrc2=s=320x180:r=50:d=2 -c:v mpeg4 -q:v 4 "$W/media/silent.mov"

cat > "$W/config.json" <<JSON
{
  "log_level": "debug",
  "media_dir": "$W/media",
  "device": { "port": "kernel:lo", "ptp": true },
  "http": { "bind": "127.0.0.1", "port": $PORT, "web_root": "$(dirname "$0")/../web" },
  "nmos": { "enabled": false },
  "players": [ { "name": "p1", "width": 320, "height": 180, "fps": "50",
      "video_dest_ip": "239.1.1.1", "video_port": 20000,
      "audio_dest_ip": "239.1.1.2", "audio_port": 30000, "channels": 2 } ],
  "recorders": [ { "name": "r1", "width": 320, "height": 180, "fps": "50", "auto_detect": true,
      "video_multicast_ip": "239.1.1.1", "video_port": 20000,
      "audio_multicast_ip": "239.1.1.2", "audio_port": 30000, "channels": 2,
      "output_dir": "$W/rec", "codec": "${CODEC:-v210}" } ]
}
JSON

echo "== starting daemon"
"$BIN" -c "$W/config.json" > "$W/daemon.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 50 ]; do get /api/status >/dev/null 2>&1 && break; sleep 0.2; i=$((i+1)); done
if ! get /api/status >/dev/null 2>&1; then
	echo "daemon did not come up:"; tail -20 "$W/daemon.log"; exit 1
fi

echo "== bring-up"
check "device up"            [ "$(get /api/status | jq -r .device.up)" = true ]
check "senders up"           [ "$(pstat senders_up)" = true ]
check "receivers up"         [ "$(rstat receivers_up)" = true ]
check "web GUI served"       sh -c "curl -sf $API/ | grep -q 'ST 2110 Virtual Card'"
sleep 2
check "PTP reported locked"  [ "$(get /api/status | jq -r .device.ptp_locked)" = true ]
n=$(rstat frames_received)
check "idle player still on the wire (black): $n frames in 2 s" in_range "$n" 60 130
check "signal present"       [ "$(rstat signal_present)" = true ]
check "format auto-detected: $(rstat detected_format)" [ "$(rstat detected_format)" = 320x180p50 ]
n=$(get /api/media | jq '[.[]|select(.playable)]|length')
check "media listed and probed ($n files)" [ "$n" = 4 ]

echo "== playout: 25p file on a 50p session"
t0=$(now)
post /api/player/p1/load '{"file":"clip25.mov"}' >/dev/null
sleep 2.5
check "state playing"        [ "$(pstat state)" = playing ]
pos=$(pstat position_s)
check "position advances in real time ($pos s after 2.5 s)" in_range "$pos" 1.8 3.2
fetch "/api/preview/player/p1/jpeg" "$W/p.jpg"
fetch "/api/preview/recorder/r1/jpeg" "$W/r1.jpg"
check "player preview is a JPEG"   sh -c "head -c 3 '$W/p.jpg' | od -An -tx1 | grep -q 'ff d8 ff'"
check "recorder preview is a JPEG" sh -c "head -c 3 '$W/r1.jpg' | od -An -tx1 | grep -q 'ff d8 ff'"
sz=$(wc -c < "$W/r1.jpg")
check "recorder preview shows picture, not black ($sz bytes)" in_range "$sz" 3000 400000
sleep 0.6
fetch "/api/preview/recorder/r1/jpeg" "$W/r2.jpg"
check "recorder preview is live (changes over time)" sh -c "! cmp -s '$W/r1.jpg' '$W/r2.jpg'"
pk=$(get /api/status | jq '[.recorders[0].meters[].peak_dbfs]|max')
check "receive meters show the tone ($pk dBFS)" in_range "$pk" -30 0
wait_idle 12; el=$(since "$t0")
check "6 s of 25p plays for ~6 s, not 3 s (took $el s)" in_range "$el" 5.9 6.5
rep=$(pstat frames_repeated); und=$(pstat audio_underruns)
check "no repeated frames during playout ($rep)" [ "$rep" = 0 ]
check "no audio underruns during playout ($und)" [ "$und" = 0 ]

echo "== playout: file whose timestamps start at 1 h"
t0=$(now)
post /api/player/p1/load '{"file":"offset.ts"}' >/dev/null
sleep 1.5
fetch "/api/preview/recorder/r1/jpeg" "$W/o1.jpg"; sleep 0.6
fetch "/api/preview/recorder/r1/jpeg" "$W/o2.jpg"
check "picture is moving, not stuck on the first frame" sh -c "! cmp -s '$W/o1.jpg' '$W/o2.jpg'"
pos=$(pstat position_s)
check "position is relative to file start ($pos s)" in_range "$pos" 1.2 3.2
wait_idle 10; el=$(since "$t0")
check "4 s file ends after ~4 s (took $el s)" in_range "$el" 3.9 4.5

echo "== record + lip sync through the whole chain"
EXT=mov; [ "${CODEC:-v210}" = ffv1 ] && EXT=mkv
post /api/recorder/r1/start "{\"filename\":\"sync-take.$EXT\"}" >/dev/null
check "recording flag set"   [ "$(rstat recording)" = true ]
sleep 0.5
post /api/player/p1/load '{"file":"sync.mov"}' >/dev/null
wait_idle 10
sleep 0.5
post /api/recorder/r1/stop >/dev/null
check "recording flag cleared" [ "$(rstat recording)" = false ]
check "no frames dropped by the encoder ($(rstat frames_dropped))" [ "$(rstat frames_dropped)" = 0 ]
REC="$W/rec/sync-take.$EXT"
check "recording exists" [ -s "$REC" ]
nf=$(ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$REC" 2>/dev/null)
check "recording has ~6 s of video ($nf frames)" in_range "${nf:-0}" 270 340
na=$(ffprobe -v error -select_streams a -show_entries stream=codec_name -of csv=p=0 "$REC" 2>/dev/null | head -1)
check "recording has PCM audio ($na)" [ "$na" = pcm_s24le ]
pf=$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$REC" 2>/dev/null)
case "${CODEC:-v210}" in
h264) check "recording is 8 bit 4:2:0, as H.264 proxies are ($pf)" [ "$pf" = yuv420p ] ;;
*)    check "recording is 10 bit 4:2:2 ($pf)" [ "$pf" = yuv422p10le ] ;;
esac
vd=$(ffprobe -v error -select_streams v:0 -show_entries stream=duration -of csv=p=0 "$REC")
ad=$(ffprobe -v error -select_streams a:0 -show_entries stream=duration -of csv=p=0 "$REC")
dd=$(awk -v a="$vd" -v b="$ad" 'BEGIN{d=a-b; if(d<0)d=-d; print d}')
check "audio and video tracks are the same length (v=$vd a=$ad)" in_range "$dd" 0 0.15
detect "$W/media/sync.mov"; src_off=$(ms "$(nth flash 1)" "$(nth beep 1)")
detect "$REC"; flash=$(nth flash 1); beep=$(nth beep 1)
check "flash found in recording (t=$flash)" [ -n "$flash" ]
check "beep found in recording (t=$beep)"   [ -n "$beep" ]
err=$(( $(ms "${flash:-0}" "${beep:-9}") - src_off ))
check "lip sync: error vs source clip $err ms (limit +-25)" in_range "$err" -25 25
flen=$(awk -v a="${flash:-0}" -v b="$(nth flashend 2)" 'BEGIN{printf "%.2f", b-a}')
check "flash lasts 0.5 s, i.e. no frames lost or doubled ($flen s)" in_range "$flen" 0.48 0.52

echo "== loop: sync must hold across passes"
post /api/player/p1/stop >/dev/null
post /api/recorder/r1/start "{\"filename\":\"loop-take.$EXT\"}" >/dev/null
sleep 0.3
post /api/player/p1/loop '{"enabled":true}' >/dev/null
post /api/player/p1/load '{"file":"sync.mov"}' >/dev/null
sleep 1
skew=$(pstat av_skew_ms)
# a few ms here is scheduling jitter of the two transmit threads on a shared
# test VM; the recording-based checks below are the ones that judge lip sync
check "sender-side A/V skew as reported by the transport ($skew ms, limit +-8)" in_range "$skew" -8 8
sleep 5;   kill -USR1 $PID                  # pass 2: transport skips 3 video epochs
sleep 1.5; kill -USR2 $PID                  # pass 2: transport skips 4 audio buffers
sleep 4;   kill -STOP $PID; sleep 0.7; kill -CONT $PID   # pass 3: whole process frozen
sleep 6.3
ve=$(pstat video_epochs_skipped); ag=$(pstat audio_gap_ms)
check "lost video epochs were noticed ($ve, injected 3 + ~35)" in_range "$ve" 30 100
check "audio gaps were noticed ($ag ms, injected 40 + ~700)" in_range "$ag" 600 2000
post /api/player/p1/stop >/dev/null
post /api/player/p1/loop '{"enabled":false}' >/dev/null
sleep 0.3
post /api/recorder/r1/stop >/dev/null
detect "$W/rec/loop-take.$EXT"
echo "        flashes: $(tr '\n' ' ' < "$W/flash.txt")"
echo "        beeps:   $(tr '\n' ' ' < "$W/beep.txt")"
check "recorder saw the audio holes ($(rstat audio_gap_ms) ms)" in_range "$(rstat audio_gap_ms)" 600 2000
# Judge the last complete pass: its beep, and whichever flash is nearest to it.
# (The detectors also report the end of the file as an "end", and a hole can
# split an event, so positions in the list mean nothing.)
beep=$(awk '$1 > 16 && $1 < 18.5' "$W/beep.txt" | tail -1)
flash=$(awk -v b="${beep:-0}" '{d=$1-b; if(d<0)d=-d; if(best==""||d<bd){bd=d;best=$1}} END{print best}' "$W/flash.txt")
check "4th pass is in the recording (beep at $beep s)" [ -n "$beep" ]
err=$(( $(ms "${flash:-0}" "${beep:-9}") - src_off ))
check "lip sync is back after all three faults: error $err ms (limit +-25)" in_range "$err" -25 25
first=$(sed -n 1p "$W/flash.txt")
drift=$(awk -v a="$first" -v b="${flash:-0}" 'BEGIN{printf "%.0f", (b-a-15)*1000}')
check "picture is still on the original timeline: 4th flash $drift ms off (limit +-25)" in_range "$drift" -25 25

echo "== transport controls"
post /api/player/p1/playlist '{"items":["clip25.mov","silent.mov"]}' >/dev/null
check "playlist of 2 accepted" [ "$(pstat playlist_count)" = 2 ]
post /api/player/p1/play >/dev/null; sleep 1.5
post /api/player/p1/pause >/dev/null; sleep 0.3
a=$(pstat position_s); sleep 1; b=$(pstat position_s)
check "pause holds position ($a -> $b)" [ "$a" = "$b" ]
n1=$(rstat frames_received); sleep 1; n2=$(rstat frames_received)
check "paused player stays on the wire ($((n2-n1)) frames/s)" in_range "$((n2-n1))" 40 60
post /api/player/p1/play >/dev/null; sleep 1
c=$(pstat position_s)
check "play resumes ($b -> $c)" in_range "$(awk -v a="$b" -v c="$c" 'BEGIN{print c-a}')" 0.5 1.6
post /api/player/p1/seek '{"seconds":5}' >/dev/null; sleep 0.8
d=$(pstat position_s)
check "seek to 5 s lands there ($d)" in_range "$d" 4.8 6.1
post /api/player/p1/next >/dev/null; sleep 0.7
check "next moves to item 2" [ "$(pstat index)" = 1 ]
u0=$(pstat audio_underruns); sleep 1; u1=$(pstat audio_underruns)
check "video-only file does not count as audio underrun ($((u1-u0)))" [ "$((u1-u0))" = 0 ]
post /api/player/p1/stop >/dev/null; sleep 0.5
check "stop goes idle" [ "$(pstat state)" = idle ]
n1=$(rstat frames_received); sleep 1; n2=$(rstat frames_received)
check "stopped player stays on the wire ($((n2-n1)) frames/s)" in_range "$((n2-n1))" 40 60
check "path traversal rejected" [ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d '{"file":"../config.json"}' $API/api/player/p1/load)" = 400 ]
check "unknown player is 404" [ "$(curl -s -o /dev/null -w '%{http_code}' -X POST $API/api/player/nope/play)" = 404 ]
t0=$(now); curl -s -o /dev/null -X POST $API/api/player/p1/stop; el=$(since "$t0")
check "POST without a body answers promptly ($el s)" in_range "$el" 0 0.3

echo "== failure reporting"
check "auto-named recording gets the codec's extension" sh -c "post(){ curl -sf -X POST -H 'Content-Type: application/json' -d '{}' $API\$1; }; post /api/recorder/r1/start | jq -r .recording_path | grep -q '\\.'$EXT'\$'"
post /api/recorder/r1/stop >/dev/null
# a directory where the file should go: fails for root too, unlike chmod
mkdir "$W/rec/blocked.$EXT"
code=$(curl -s -o "$W/err.json" -w '%{http_code}' -X POST -H 'Content-Type: application/json' -d "{\"filename\":\"blocked.$EXT\"}" $API/api/recorder/r1/start)
check "recording that cannot open its file is refused (HTTP $code: $(jq -r .error "$W/err.json" | cut -c1-70))" [ "$code" = 500 ]
check "and the recorder is not left in REC" [ "$(rstat recording)" = false ]

slow=$(sort -rn "$W/calls.log" | head -1)
check "no API call took long (slowest: $slow s)" in_range "${slow%% *}" 0 0.25

echo "== shutdown"
kill -TERM $PID
i=0; while kill -0 $PID 2>/dev/null && [ $i -lt 80 ]; do sleep 0.1; i=$((i+1)); done
if kill -0 $PID 2>/dev/null; then bad "daemon hung on SIGTERM"; kill -9 $PID; else
	wait $PID; rc=$?; check "clean exit on SIGTERM (rc=$rc, $((i/10)).$((i%10)) s)" [ "$rc" = 0 ]; fi

grep -E "underrun|behind" "$W/daemon.log" | sed 's/^/        /' | head -8
[ "$FAIL" -gt 0 ] && sort -rn "$W/calls.log" | head -3 | sed 's/^/        slow call: /'
[ "$FAIL" -gt 0 ] && grep -E "playing \[|finished|recording st|preroll|seek" "$W/daemon.log" | sed 's/^/        /' | head -40
echo
echo "passed $PASS, failed $FAIL"
if [ "$FAIL" -gt 0 ] || [ -n "$KEEP" ]; then
	echo "---- daemon log (tail) ----"; grep -v "^\[mock-mtl\]" "$W/daemon.log" | tail -${LOGTAIL:-25}
fi
rm -rf "$W"
[ "$FAIL" = 0 ]
