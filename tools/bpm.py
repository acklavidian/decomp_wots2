#!/usr/bin/env python3
"""
Decode a .bpm sprite sheet (or a bare .Pim texture) from Way of the Samurai 2.

BPM file (little-endian), from SpriteSet_Parse (0x17C230) and SpriteSet_Draw (0x17B780):

  +0x00 u32 type (2)      +0x04 u32 sprite_count      +0x08 "BPMFile "
  +0x10 sprite_count x 0x20-byte sprite records:
        +0x00 char name[8]
        +0x08 s16 u, v, w, h            pixels in the texture
        +0x10 u16 u0, v0, u1, v1        the same rectangle as 12.4 fixed + half a texel
        +0x18 u8  clut_offset           added to the texture's CLUT base (4 per palette)
        +0x19 u8  texture_index
  then one or more 0x30-byte texture descriptors (the ".Pim" format), each followed
  somewhere by its pixel and palette data:
        +0x02 u8  psm (0x13 = 8-bit indexed, 0x14 = 4-bit indexed)
        +0x06 u16 buffer width / 64     +0x08 u16 log2 width     +0x0A u16 log2 height
        +0x14 u32 pixel bytes           +0x18 u32 pixel offset (from the first descriptor)
        +0x1C u16 palette count         +0x20 u32 palette bytes  +0x24 u32 palette offset
        +0x28 u16 descriptors remaining (1 = last)
        A descriptor with pixel bytes == 0 shares the previous descriptor's pixels.
        +0x2E u16 upload buffer width / 64 (non-zero: uploaded as 32-bit, see below)

The game uploads 8-bit pixel data to the GS as a 32-bit image a quarter of the size and
then samples it as 8-bit, so the bytes in the file are in GS 32-bit block order.
`unswizzle8` undoes that.  8-bit palettes are stored with the usual GS CSM1 shuffle.

  tools/bpm.py sheet  in.bpm out.png [--clut N]     whole texture with one palette
  tools/bpm.py sprites in.bpm outdir/               one PNG per sprite, own palette
  tools/bpm.py info   in.bpm
"""
import argparse
import struct
import sys
from pathlib import Path

import zlib

import numpy as np


def save_png(path, rgba: np.ndarray) -> None:
    """Minimal RGBA PNG writer (avoids a Pillow dependency)."""
    h, w, _ = rgba.shape
    raw = np.zeros((h, 1 + w * 4), dtype=np.uint8)
    raw[:, 1:] = rgba.reshape(h, w * 4)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"
                           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw.tobytes(), 6))
                           + chunk(b"IEND", b""))

BLOCK32 = np.array([[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23],
                    [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]])
COLUMN32 = np.array([[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15],
                     [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
                     [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47],
                     [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]])
BLOCK8 = BLOCK32
COLUMN8 = np.array([
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255]])


def _column4(x, y):
    """Nibble index inside a 32x16 block for PSMT4 (same construction as COLUMN8)."""
    c, yy = y // 4, y % 4
    g = (x % 8) // 2
    if (yy >= 2) != (c % 2 == 1):
        g = (g + 2) % 4
    return c * 128 + g * 32 + (x % 2) * 8 + (yy % 2) * 16 + (x // 8) * 2 + (yy // 2)


COLUMN4 = np.array([[_column4(x, y) for x in range(32)] for y in range(16)])
BLOCK4 = np.array([[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15],
                   [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]])


def _gs_memory_from_ct32(data: bytes, upload_bw: int, up_w: int) -> np.ndarray:
    """GS memory image after uploading `data` as a PSMCT32 image up_w pixels wide
    into a buffer upload_bw*64 wide.  (The image is half as wide as the indexed
    texture it becomes, which for small textures is less than the buffer width.)"""
    src = np.frombuffer(data, dtype=np.uint8)
    up_h = len(src) // 4 // up_w
    y, x = np.mgrid[0:up_h, 0:up_w]
    addr32 = ((y // 32) * upload_bw + x // 64) * 8192 \
        + BLOCK32[(y // 8) % 4, (x // 8) % 8] * 256 + COLUMN32[y % 8, x % 8] * 4
    mem = np.zeros(int(addr32.max()) + 4, dtype=np.uint8)
    flat = addr32.reshape(-1)
    px = src[:up_w * up_h * 4].reshape(-1, 4)
    for b in range(4):
        mem[flat + b] = px[:, b]
    return mem


def unswizzle4(data: bytes, width: int, height: int, upload_bw: int, up_w: int = 0,
               mem_bw: int = 0, start: int = 0) -> np.ndarray:
    """As unswizzle8, for PSMT4: pages are 128x128, blocks 32x16, addresses in nibbles.
    `mem_bw` / `start`: the upload was another texture's (its buffer width) and this
    one begins `start` bytes into it."""
    mem = _gs_memory_from_ct32(data, mem_bw or upload_bw, up_w or width // 2)[start:]
    y, x = np.mgrid[0:height, 0:width]
    nib = ((y // 128) * upload_bw + x // 128) * 16384 \
        + BLOCK4[(y // 16) % 8, (x // 32) % 4] * 512 + COLUMN4[y % 16, x % 32]
    byte = mem[np.minimum(nib // 2, len(mem) - 1)]
    return np.where(nib & 1, byte >> 4, byte & 0x0F).astype(np.uint8)


def unswizzle8(data: bytes, width: int, height: int, upload_bw: int, up_w: int = 0,
               mem_bw: int = 0, start: int = 0) -> np.ndarray:
    """Bytes uploaded as PSMCT32 (buffer width upload_bw*64) -> PSMT8 image width x height."""
    mem = _gs_memory_from_ct32(data, mem_bw or upload_bw, up_w or width // 2)[start:]
    # Read it back as 8-bit: pages are 128x64, blocks 16x16
    y, x = np.mgrid[0:height, 0:width]
    addr8 = ((y // 64) * upload_bw + x // 128) * 8192 \
        + BLOCK8[(y // 16) % 4, (x // 16) % 8] * 256 + COLUMN8[y % 16, x % 16]
    return mem[np.minimum(addr8, len(mem) - 1)]


def read_clut4(raw: bytes) -> np.ndarray:
    """16 RGBA entries (no shuffle), alpha scaled from 0x80 to 0xFF."""
    pal = np.frombuffer(raw[:64], dtype=np.uint8).reshape(16, 4).copy()
    pal[:, 3] = np.minimum(pal[:, 3].astype(np.uint16) * 255 // 128, 255).astype(np.uint8)
    return pal


def read_clut8(raw: bytes) -> np.ndarray:
    """256 RGBA entries, un-shuffled from CSM1 order, alpha scaled from 0x80 to 0xFF."""
    pal = np.frombuffer(raw[:1024], dtype=np.uint8).reshape(256, 4).copy()
    idx = np.arange(256)
    swap = (idx & 0x18) == 0x08
    swap2 = (idx & 0x18) == 0x10
    order = idx.copy()
    order[swap] += 8
    order[swap2] -= 8
    pal = pal[order]
    pal[:, 3] = np.minimum(pal[:, 3].astype(np.uint16) * 255 // 128, 255).astype(np.uint8)
    return pal


class Texture:
    def __init__(self, blob: bytes, base: int, origin: int):
        # Data offsets are relative to the first descriptor (`origin`), not this one.
        d = blob[base:base + 0x30]
        self.psm = d[2]
        self.tbw, self.tw, self.th = struct.unpack_from("<HHH", d, 6)
        self.pix_size, self.pix_off = struct.unpack_from("<II", d, 0x14)
        self.clut_count, = struct.unpack_from("<H", d, 0x1C)
        self.clut_size, self.clut_off = struct.unpack_from("<II", d, 0x20)
        self.remaining, = struct.unpack_from("<H", d, 0x28)
        # For a texture inside another's upload: where it starts, in 256-byte GS blocks.
        self.block, = struct.unpack_from("<H", d, 0x2A)
        self.mem_bw = 0
        self.upload_bw, = struct.unpack_from("<H", d, 0x2E)
        # Size of the 32-bit image that is uploaded.  Normally half the texture's
        # width; less when the picture does not fill its power-of-two texture.
        self.up_w, self.up_h = struct.unpack_from("<HH", d, 0x10)
        self.width, self.height = 1 << self.tw, 1 << self.th
        self.pixels = blob[origin + self.pix_off: origin + self.pix_off + self.pix_size]
        self.clut_raw = blob[origin + self.clut_off: origin + self.clut_off + self.clut_size]
        self._indices = None
        self.start = 0

    def indices(self) -> np.ndarray:
        if self._indices is None:
            if self.psm == 0x13 and self.up_w and self.up_w * self.up_h == len(self.pixels):
                # uploaded as it is: an up_w x up_h rectangle of 8-bit pixels
                img = np.zeros((self.height, self.width), np.uint8)
                rect = np.frombuffer(self.pixels, np.uint8).reshape(self.up_h, self.up_w)
                img[:min(self.up_h, self.height), :min(self.up_w, self.width)] = \
                    rect[:self.height, :self.width]
                self._indices = img
            elif self.psm == 0x13 and self.upload_bw:
                self._indices = unswizzle8(self.pixels, self.width, self.height, self.read_bw(), self.up_w,
                                           self.mem_bw or self.upload_bw, self.start)
            elif self.psm == 0x14 and self.upload_bw:
                self._indices = unswizzle4(self.pixels, self.width, self.height, self.read_bw(), self.up_w,
                                           self.mem_bw or self.upload_bw, self.start)
            elif self.psm == 0x13:
                self._indices = np.frombuffer(self.pixels, np.uint8)[:self.width * self.height] \
                    .reshape(self.height, self.width)
            else:
                raise NotImplementedError(f"pixel format {self.psm:#x}")
        return self._indices

    def read_bw(self) -> int:
        """Pages per row when read: the texture's own buffer width (TEX0 TBW, +0x06) over the
        indexed page's 128 texels -- not the upload's (a 4-bit 128x256 texture is uploaded two
        32-bit pages wide but read one page wide, two down)."""
        if self.psm in (0x13, 0x14) and self.tbw >= 2 and self.tbw * 64 >= self.width:
            return self.tbw // 2
        return self.upload_bw

    @property
    def clut_step(self) -> int:
        """How far the sprite record's clut_offset advances per palette."""
        return 4 if self.psm == 0x13 else 1

    def clut(self, n: int) -> np.ndarray:
        if self.psm == 0x13:
            return read_clut8(self.clut_raw[n * 1024:(n + 1) * 1024])
        return read_clut4(self.clut_raw[n * 64:(n + 1) * 64])

    def rgba(self, clut: int) -> np.ndarray:
        if self.psm == 0:
            # 32-bit colour, no palette: the bytes are the pixels.
            w = self.up_w or self.width
            px = np.frombuffer(self.pixels, np.uint8)
            h = len(px) // 4 // w
            out = np.zeros((self.height, self.width, 4), np.uint8)
            out[:h, :w] = px[:h * w * 4].reshape(h, w, 4)[:self.height, :self.width]
            out[..., 3] = np.minimum(out[..., 3].astype(int) * 255 // 128, 255)
            return out
        return self.clut(clut)[self.indices()]


class Bpm:
    def __init__(self, blob: bytes):
        self.sprites = []
        base = 0
        if blob[8:15] == b"BPMFile":
            count, = struct.unpack_from("<I", blob, 4)
            for i in range(count):
                r = blob[0x10 + i * 0x20: 0x30 + i * 0x20]
                u, v, w, h = struct.unpack_from("<hhhh", r, 8)
                self.sprites.append(dict(index=i, name=r[:8].rstrip(b"\0 ").decode("latin1"),
                                         u=u, v=v, w=w, h=h, clut_offset=r[0x18], tex=r[0x19]))
            base = 0x10 + count * 0x20
        self.textures = []
        origin = base
        while True:
            t = Texture(blob, base, origin)
            if t.pix_size == 0 and self.textures:
                # No pixels of its own: it lies in the previous texture's upload (from
                # GS block `block` on), read in its own pixel format.
                prev = self.textures[-1]
                t.pixels = prev.pixels
                t.mem_bw = prev.mem_bw or prev.upload_bw
                t.upload_bw = t.upload_bw or prev.upload_bw
                t.up_w = prev.up_w
                t.start = prev.start + t.block * 256
            self.textures.append(t)
            if t.remaining < 2:
                break
            base += 0x30
        for sp in self.sprites:
            sp["clut"] = sp["clut_offset"] // self.textures[sp["tex"]].clut_step


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("cmd", choices=["info", "sheet", "sprites"])
    ap.add_argument("src", type=Path)
    ap.add_argument("out", type=Path, nargs="?")
    ap.add_argument("--clut", type=int, default=0)
    ap.add_argument("--tex", type=int, default=0)
    args = ap.parse_args()

    bpm = Bpm(args.src.read_bytes())
    if args.cmd == "info":
        for i, t in enumerate(bpm.textures):
            print(f"texture {i}: {t.width}x{t.height} psm={t.psm:#x} cluts={t.clut_count} "
                  f"upload_bw={t.upload_bw}")
        for s in bpm.sprites:
            print("{index:3d} {name:<8} u={u:4d} v={v:4d} w={w:4d} h={h:4d} clut={clut:2d} tex={tex}"
                  .format(**s))
    elif args.cmd == "sheet":
        save_png(args.out, bpm.textures[args.tex].rgba(args.clut))
        print(args.out)
    elif args.cmd == "sprites":
        args.out.mkdir(parents=True, exist_ok=True)
        for s in bpm.sprites:
            img = bpm.textures[s["tex"]].rgba(s["clut"])[s["v"]:s["v"] + s["h"], s["u"]:s["u"] + s["w"]]
            if img.size:
                save_png(args.out / f"{s['index']:03d}_{s['name']}.png", np.ascontiguousarray(img))
        print(f"{len(bpm.sprites)} sprites -> {args.out}")


if __name__ == "__main__":
    main()
