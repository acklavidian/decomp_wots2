#!/bin/bash
# Walk into one door of one area and see what happens.  Takes a line of
# tools/doors.py ("variant area exit script standX standZ towardX towardZ"),
# a log directory and optionally the binary; prints one line of result.
#   tools/doors.py | grep -v none | xargs -P 8 -L 1 tools/door_crawl.sh /tmp/doors
cd "$(dirname "$0")/.." || exit 1
dir="$1"; variant="$2"; area="$3"; ex="$4"; script="$5"; sx="$6"; sz="$7"; tx="$8"; tz="$9"; bin="${DOOR_BIN:-build/wots2}"
mkdir -p "$dir"; log="$dir/v${variant}_a${area}_e${ex}.log"
INP="340:start,500:cross,560:cross,700:down,710:down,720:down,730:down,740:down,770:cross"
for f in $(seq 1300 40 2400); do INP="$INP,$f:cross"; done
for f in $(seq 2440 90 3400); do INP="$INP,$f:triangle,$((f+25)):triangle,$((f+55)):cross"; done
period=1; [ "$variant" = 1 ] && period=4
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=offscreen WOTS2_FAST=1 WOTS2_NEW_GAME=1 WOTS2_SKIP_INTRO=1 WOTS2_MONEY=3000 WOTS2_AREA=$area WOTS2_PERIOD=$period \
  WOTS2_WARP="1100:$sx,$sz>$tx,$tz" WOTS2_TRACE_PLAYER=3500 WOTS2_INPUT="$INP" WOTS2_EXIT_FRAME=3500 timeout 200 "$bin" extracted > "$log" 2>&1
code=$?
maps=$(grep "^\[world\] map" "$log" | sed 's/\[world\] map \([0-9]*\):.*/\1/' | tr '\n' ' ')
started=$(grep "StartScript " "$log" | sed 's/.*StartScript //' | tr '\n' ' ')
miss=$(grep -c "not decompiled yet\|unbound" "$log")
echo "$code | v$variant area $area exit $ex | wants $script | maps $maps| started $started| missing $miss"
