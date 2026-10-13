#!/bin/bash
# Run one event script alone for 6,000 frames with Cross and Square pressed in
# turn, the player kept alive and everybody else one blow from death, so that
# fights are won and what follows them runs.  Prints one line:
#   exit code | script | calls | the last built-in called | missing | scripts started
#
#   tools/smoke_one.sh CTX/TimeD/D02C1x [log directory] [binary]
#   tools/sx.py list | awk '{print $NF}' | xargs -P 8 -I{} tools/smoke_one.sh {} /tmp/smoke
cd "$(dirname "$0")/.." || exit 1
name="$1"; dir="${2:-/tmp/wots2_smoke}"; bin="${3:-build/wots2}"
mkdir -p "$dir"
log="$dir/$(echo "$name" | tr '/' '_').log"
INP="340:start,500:cross,560:cross,700:down,710:down,720:down,730:down,740:down,770:cross"
for f in $(seq 900 25 6000); do INP="$INP,$f:cross,$((f+12)):square"; done
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=offscreen WOTS2_FAST=1 WOTS2_NEW_GAME=1 WOTS2_MONEY=3000 WOTS2_TOUGH=1 WOTS2_WEAK_NPCS=1 \
  WOTS2_TRACE_CALLS=1 WOTS2_TRACE_EVENTS=1 WOTS2_START_SCRIPT="$name" WOTS2_INPUT="$INP" WOTS2_EXIT_FRAME=6000 \
  timeout 240 "$bin" extracted > "$log" 2>&1
code=$?
last=$(grep "^\[call" "$log" | tail -1 | cut -c1-60)
miss=$(grep "not decompiled yet" "$log" | sed 's/.*yet: //' | tr '\n' ' ')
next=$(grep "StartScript " "$log" | sed 's/.*StartScript //' | tr '\n' ' ')
echo "$code | $name | calls $(grep -c '^\[call' "$log") | last $last | missing: $miss | chain: $next"
