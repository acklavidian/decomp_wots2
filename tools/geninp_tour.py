#!/usr/bin/env python3
# Input for a WOTS2_TOUR run from a new game: the opening and the first fight
# as in inp_fullstory.txt, then Cross every 40 frames and Square in between —
# but nothing around the multiples of 300, when WOTS2_WARP="every 300:…" puts
# the player before its door (moved in mid-jump it stays where it was) — and,
# by the seed, Down zero to three times before a Cross, so that a question
# is not answered the same way for ever.
#   tools/geninp_tour.py <frames> <seed>
import os, random, sys
N, seed = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
here = os.path.dirname(os.path.abspath(__file__))
out = [x for x in open(os.path.join(here, "inp_fullstory.txt")).read().strip().split(",")
       if int(x.split(":")[0].split("-")[0]) < 9000]
for f in range(9000, N, 40):
    if f % 300 < 60 or f % 300 > 270:
        continue
    for i in range(rnd.choice([0, 0, 0, 1, 1, 2, 3])):
        out.append(f"{f - 16 + i * 4}:down")
    out.append(f"{f}:cross")
    out.append(f"{f + 20}:square")
print(",".join(out))
