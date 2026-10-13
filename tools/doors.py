#!/usr/bin/env python3
"""Where the doors of an area's map are, from its collision file.

  tools/doors.py                 every area, day and night: one line per door
                                 "variant area exit script standX standZ towardX towardZ"
A door is a set of collision cells with flag 8 whose index is the exit's number
(-1 reads as 0).  The line gives a walkable spot beside it and a point beyond
it, for WOTS2_WARP="frame:standX,standZ>towardX,towardZ".
"""
import os, re, struct, subprocess, tempfile

def volume(path):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'map.col')
        subprocess.run([os.path.join(os.path.dirname(__file__), 'volume.py'), 'extract', path, '-o', out],
                       check=True, capture_output=True)
        return open(out, 'rb').read()

def doors(col):
    d = col
    n = struct.unpack_from('<i', d, 0)[0]; o = 4 + n * 16
    cs = struct.unpack_from('<f', d, o + 4)[0]
    ox, oz, w, cc = struct.unpack_from('<iiii', d, o + 8)
    tc = struct.unpack_from('<i', d, o + 0x20)[0]
    o += 0x24
    types = [(struct.unpack_from('<I', d, o + i * 0x3C + 4)[0], struct.unpack_from('<i', d, o + i * 0x3C + 0xC)[0]) for i in range(tc)]
    o += tc * 0x3C
    h = cc // w
    def cell(cx, cz):
        if not (0 <= cx < w and 0 <= cz < h): return None
        ty = struct.unpack_from('<h', d, o + (cz * w + cx) * 4 + 2)[0]
        return types[ty] if 0 <= ty < tc else None
    # The game finds a cell with (int)(x * 100 / size) + offset, which rounds
    # toward zero: cells on the negative side lie one further out than a
    # floor would put them (and cell 0 is twice as wide).
    def middle(v): return (v + 0.5 if v > 0 else v - 0.5 if v < 0 else 0.0) * cs / 100
    def world(cx, cz): return (middle(cx - ox), middle(cz - oz))
    def free(c): return c is not None and (c[0] & 1) and not (c[0] & (2 | 8 | 4))
    found = {}
    for cz in range(h):
        for cx in range(w):
            c = cell(cx, cz)
            if c is None or not (c[0] & 8): continue
            e = 0 if c[1] == -1 else c[1]
            explicit = c[1] != -1
            for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                # three free cells in a row on this side: room to stand
                if all(free(cell(cx + dx * k, cz + dz * k)) for k in (1, 2, 3)):
                    sx, sz = world(cx + dx * 3, cz + dz * 3)
                    tx, tz = world(cx - dx * 6, cz - dz * 6)
                    key = (e, explicit)
                    found.setdefault(key, []).append((sx, sz, tx, tz))
    result = {}
    for (e, explicit), spots in sorted(found.items(), key=lambda kv: kv[0][1]):
        result[e] = spots[len(spots) // 2]          # explicit ones (sorted last) win
    return result

def main():
    exe = os.path.join(os.path.dirname(__file__), '..', 'build', 'wots2')
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', WOTS2_DUMP_MAPS='1')
    out = subprocess.run([exe, 'extracted'], capture_output=True, text=True, env=env).stdout
    for line in out.split('\n'):
        m = re.match(r'\s*\[(\d)\] area\s+(\d+): map\s+(\d+) \S+\s+\((\S+)\) exits:(.*)', line)
        if not m: continue
        variant, area, col, exits = int(m.group(1)), int(m.group(2)), m.group(4), m.group(5)
        table = {int(e): s for e, s in re.findall(r'(\d+)→\d+\(([^)]*)\)', exits)}
        try: spots = doors(volume(col))
        except Exception as err:
            print('#', col, err); continue
        for e in sorted(set(table) | set(spots)):
            s = spots.get(e)
            print(variant, area, e, table.get(e, '(no such exit)') or '-',
                  *(('%.2f' % v for v in s) if s else ('none',)))

if __name__ == '__main__':
    main()
