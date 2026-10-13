#!/usr/bin/env python3
"""Convert the 640x448 PPM frames written by WOTS2_SHOTS to PNG."""
import struct, sys, zlib
import numpy as np

def main():
    half = "--half" in sys.argv
    for path in [a for a in sys.argv[1:] if not a.startswith("--")]:
        data = open(path, "rb").read()
        parts = data.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        img = np.frombuffer(parts[3], np.uint8).reshape(h, w, 3)
        if half:
            img = img[::2, ::2]
        h, w, _ = img.shape
        raw = np.zeros((h, 1 + w * 3), np.uint8)
        raw[:, 1:] = img.reshape(h, w * 3)
        def chunk(tag, d):
            return struct.pack(">I", len(d)) + tag + d + struct.pack(">I", zlib.crc32(tag + d))
        out = path.rsplit(".", 1)[0] + ".png"
        open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw.tobytes(), 6)) + chunk(b"IEND", b""))
        print(out)

if __name__ == "__main__":
    main()
