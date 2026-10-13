#!/bin/bash
# Scripted test: a whole game from a new game with the WOTS2_TOUR autopilot —
# every area in turn, home to rest in between, questions answered at random.
# Writes <log dir>/t<seed>.log (the built-in calls left out).
#   tools/tour.sh <seed> [log dir] [frames] [binary]
cd "$(dirname "$0")/.." || exit 1
seed="$1"; dir="${2:-/tmp/wots2_tour}"; frames="${3:-400000}"; bin="${4:-build/wots2}"
mkdir -p "$dir"
tools/geninp_tour.py "$frames" "$seed" > "$dir/inp$seed.txt"
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=offscreen WOTS2_FAST=1 WOTS2_NEW_GAME=1 WOTS2_TOUR="$seed" WOTS2_WARP_AREA=6 \
  WOTS2_WARP="every 300:-2.4,-5.6>-2.4,-7.5" WOTS2_TOUGH=1 WOTS2_WEAK_NPCS=9000 WOTS2_TRACE_CALLS=1 \
  WOTS2_INPUT="@$dir/inp$seed.txt" WOTS2_EXIT_FRAME="$frames" "$bin" extracted 2>&1 \
  | grep -a -v '^\[call\|^\[chara\]\|^\[sprites\]\|^\[world\]' > "$dir/t$seed.log"
echo "exit ${PIPESTATUS[0]}" >> "$dir/t$seed.log"
