#!/usr/bin/env python3
"""
VOLUME.DAT (FADEBABE archive) reader for Way of the Samurai 2.

Layout (all big-endian), recovered from the lookup code at 0x141530 / 0x140FE0:

  Header (20 bytes):  magic FA DE BA BE, entry_count, count2, data_start, total_size
  Entry  (24 bytes):  name_hash, data_offset, size, flags, stored_end, name_len
    stored bytes = data_offset .. stored_end, both relative to data_start;
    `size` is the uncompressed size
    flags & 2 -> LZO1X compressed      flags & 4 -> LZSS (4 KB window) compressed
    flags & 1 -> hash collision; the game compares names stored in the archive

  name_hash: uppercase the path, '/' -> '\\', then h = c + h * 19 (32-bit, signed chars)

Paths come from the game's asset table (config/asset_paths.json).  Anything not
under "Common/" gets the language prefix, "Us/" for this release.

  tools/volume.py list
  tools/volume.py extract menu/TitleScreen/ImgA.bpm -o out.bin
  tools/volume.py extract 0x52 -o out.bin          # by asset id
"""
import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VOLUME_PATH = ROOT / "extracted" / "VOLUME.DAT"
ASSET_PATHS = ROOT / "config" / "asset_paths.json"
LANG_PREFIX = "Us/"


def name_hash(path: str) -> int:
    h = 0
    for ch in path.upper().replace("/", "\\").encode("latin1"):
        if ch > 127:
            ch -= 256
        h = (ch + h * 19) & 0xFFFFFFFF
    return h


def lzss_decompress(src: bytes, size: int) -> bytes:
    """Okumura LZSS: 4096-byte zero-filled window, write position 0xFEE."""
    window = bytearray(4096)
    wpos = 0xFEE
    out = bytearray()
    ip = 0
    flags = 0
    n = len(src)
    while len(out) < size:
        flags >>= 1
        if not flags & 0x100:
            if ip >= n:
                break
            flags = src[ip] | 0xFF00
            ip += 1
        if flags & 1:
            if ip >= n:
                break
            c = src[ip]
            ip += 1
            out.append(c)
            window[wpos] = c
            wpos = (wpos + 1) & 0xFFF
        else:
            if ip + 1 >= n:
                break
            lo, hi = src[ip], src[ip + 1]
            ip += 2
            pos = lo | ((hi & 0xF0) << 4)
            for i in range((hi & 0x0F) + 3):
                c = window[(pos + i) & 0xFFF]
                out.append(c)
                window[wpos] = c
                wpos = (wpos + 1) & 0xFFF
    return bytes(out[:size])


def lzo1x_decompress(src: bytes, size: int) -> bytes:
    out = bytearray()
    ip = 0

    def copy_match(dist, length):
        start = len(out) - dist
        if start < 0:
            raise ValueError("LZO: match before start of output")
        for i in range(length):
            out.append(out[start + i])

    def literals(count):
        nonlocal ip
        out.extend(src[ip:ip + count])
        ip += count

    def extended(t, base):
        nonlocal ip
        while src[ip] == 0:
            t += 255
            ip += 1
        t += base + src[ip]
        ip += 1
        return t

    state = "start"
    t = src[ip]
    if t > 17:
        ip += 1
        t -= 17
        if t < 4:
            state = "match_next"
        else:
            literals(t)
            state = "first_literal_run"

    while True:
        if state == "start":
            t = src[ip]; ip += 1
            if t >= 16:
                state = "match"
                continue
            if t == 0:
                t = extended(t, 15)
            literals(t + 3)
            state = "first_literal_run"

        if state == "first_literal_run":
            t = src[ip]; ip += 1
            if t >= 16:
                state = "match"
            else:
                dist = 1 + 0x800 + (t >> 2) + (src[ip] << 2); ip += 1
                copy_match(dist, 3)
                state = "match_done"

        if state == "match":
            if t >= 64:
                dist = 1 + ((t >> 2) & 7) + (src[ip] << 3); ip += 1
                copy_match(dist, (t >> 5) - 1 + 2)
            elif t >= 32:
                t &= 31
                if t == 0:
                    t = extended(t, 31)
                dist = 1 + (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2
                copy_match(dist, t + 2)
            elif t >= 16:
                dist = (t & 8) << 11
                t &= 7
                if t == 0:
                    t = extended(t, 7)
                dist += (src[ip] >> 2) + (src[ip + 1] << 6); ip += 2
                if dist == 0:
                    break  # end-of-stream marker
                copy_match(dist + 0x4000, t + 2)
            else:
                dist = 1 + (t >> 2) + (src[ip] << 2); ip += 1
                copy_match(dist, 2)
            state = "match_done"

        if state == "match_done":
            t = src[ip - 2] & 3
            if t == 0:
                state = "start"
                continue
            state = "match_next"

        if state == "match_next":
            literals(t)
            t = src[ip]; ip += 1
            state = "match"

    if len(out) != size:
        raise ValueError(f"LZO: got {len(out)} bytes, expected {size}")
    return bytes(out)


class Volume:
    def __init__(self, path: Path = VOLUME_PATH):
        self.f = open(path, "rb")
        magic, self.count, self.count2, self.data_start, self.total = struct.unpack(
            ">4sIIII", self.f.read(20))
        if magic != b"\xfa\xde\xba\xbe":
            raise ValueError("not a FADEBABE archive")
        raw = self.f.read(self.count * 24)
        self.entries = [struct.unpack_from(">6I", raw, i * 24) for i in range(self.count)]
        self.by_hash = {}
        for e in self.entries:
            self.by_hash.setdefault(e[0], []).append(e)

    def find(self, path: str):
        """Look up a path as the game would: add the language prefix unless under Common/."""
        if not path.upper().startswith(("COMMON/", "US/")):
            path = LANG_PREFIX + path
        hits = self.by_hash.get(name_hash(path))
        if not hits:
            return None
        if len(hits) > 1:
            print(f"warning: {len(hits)} entries share the hash of {path}; using the first",
                  file=sys.stderr)
        return hits[0]

    def read(self, entry) -> bytes:
        _, offset, size, flags, stored_end, _ = entry
        self.f.seek(self.data_start + offset)
        data = self.f.read(stored_end - offset)
        if flags & 2:
            return lzo1x_decompress(data, size)
        if flags & 4:
            return lzss_decompress(data, size)
        return data[:size]

    def read_path(self, path: str) -> bytes:
        entry = self.find(path)
        if entry is None:
            raise FileNotFoundError(path)
        return self.read(entry)


def asset_path(arg: str) -> str:
    """Accept either a path or a numeric asset id (index into the game's path table)."""
    try:
        idx = int(arg, 0)
    except ValueError:
        return arg
    return json.loads(ASSET_PATHS.read_text())["paths"][idx]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="list the known asset paths and whether the archive has them")
    ex = sub.add_parser("extract", help="extract one file by path or asset id")
    ex.add_argument("asset")
    ex.add_argument("-o", "--out", type=Path, required=True)
    args = ap.parse_args()

    vol = Volume()
    if args.cmd == "list":
        paths = json.loads(ASSET_PATHS.read_text())["paths"]
        print(f"{vol.count} entries in archive")
        for i, p in enumerate(paths):
            if p.startswith(("modules/", "cdrom0:")):
                continue
            e = vol.find(p)
            info = f"{e[2]:>9,} bytes  flags={e[3]}" if e else "(not in archive)"
            print(f"{i:#05x}  {p:<44} {info}")
    elif args.cmd == "extract":
        data = vol.read_path(asset_path(args.asset))
        args.out.write_bytes(data)
        print(f"{args.out}: {len(data):,} bytes")


if __name__ == "__main__":
    main()
