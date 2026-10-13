#!/usr/bin/env python3
"""Decode an A2MF character model (.mdl) from Way of the Samurai 2.

File: "A2MF", u8 type @+4, …; everything below is relative to file + 0x10.

Type 1 (heads, the two heroes; Model_Relocate 0x133390):

  +0x02 s16 boneCount     +0x04 s16 meshCountA    +0x06 s16 meshCountB
  +0x08 u32 bones         boneCount x 0xA0: float[16] rest pose in model space, float[16] local (to parent),
                          s32 parent @+0x80 (-1 = root)
  +0x10 u32 textures      .Pim descriptors (0 if the textures are in a separate .pim)
  +0x14 u32 extra block
  +0x18 u32 mesh[meshCountA + meshCountB]

  mesh: u8 bone, u8 bone2, u8 ?, u8 flags; u32 sizeInQwords; u32 dataOffset; u32 materialOffset

Type 0 (ordinary bodies; relocation at 0x12CF40) differs only in the headers:

  +0x02 s16 boneCount     +0x04 s16 meshCount[4]
  +0x0C u32 bones   +0x14 u32 textures   +0x18 u32 extra block   +0x1C u32 mesh[sum of counts]
  mesh: u8 bone, u8 bone2, u8 ?, u8 flags; u32 materialCount; u32 sizeInQwords; u32 dataOffset;
        u32 material[materialCount]   (offsets of the TEX0 qwords inside the data)
  data: a VIF stream for VU1.  A DIRECT packet sets TEX0 (the qword's upper half holds
        u16 textureIndex, u16 clutOffset), then one batch per MSCNT:
          addr 0      2 GIF tags (NLOOP = vertex count n; triangle strip)
          addr 2      n x float xyzw   positions in the bone's space
          then        n x s16[4]       normals   / 4096; on joint meshes (bone != bone2) w
                                       selects the bone the vertex follows
          then        n x u8[4]        colours   (0x80 = 1.0)
          then        n x s16[4]       s, t, q   / 4096

  tools/mdl.py info model.mdl
  tools/mdl.py obj model.mdl out.obj
  tools/mdl.py view out.png body.mdl [head.mdl] [textures.pim]   # front/side preview
"""
import struct, sys, zlib
import numpy as np


def vif_batches(d, p, end):
    """Yield (count, positions, normals, colours, uvs, fourth word of each position, TEX0 material) per MSCNT."""
    bufs = {}
    material = (0, 0)
    while p < end:
        w, = struct.unpack_from('<I', d, p); p += 4
        cmd = (w >> 24) & 0x7F; num = (w >> 16) & 0xFF; imm = w & 0xFFFF
        if cmd in (0x50, 0x51):
            q = d[p:p + imm * 16]
            if imm >= 2:
                material = struct.unpack_from('<HH', q, 24)
            p += imm * 16
        elif cmd == 0x17 or cmd == 0x14:
            if 0 in bufs:
                n = struct.unpack_from('<I', bufs[0][1])[0] & 0x7FFF
                pos = np.frombuffer(bufs[2][1], '<f4').reshape(-1, 4)[:n, :3]
                kick = np.frombuffer(bufs[2][1], '<u4').reshape(-1, 4)[:n, 3]
                if 2 + n in bufs:
                    nrm = np.frombuffer(bufs[2 + n][1], '<i2').reshape(-1, 4)[:n] / 4096.0
                else:                       # map meshes: lighting is baked into the colours
                    nrm = np.zeros((n, 4))
                if 2 + 2 * n in bufs:
                    col = np.frombuffer(bufs[2 + 2 * n][1], 'u1').reshape(-1, 4)[:n]
                else:
                    col = np.full((n, 4), 0x80, np.uint8)
                if 2 + 3 * n in bufs:
                    uv = np.frombuffer(bufs[2 + 3 * n][1], '<i2').reshape(-1, 4)[:n, :2] / 4096.0
                else:                       # untextured batch
                    uv = np.zeros((n, 2))
                yield n, pos, nrm, col, uv, kick, material
            bufs = {}
        elif cmd & 0x60 == 0x60:
            vn = (cmd >> 2) & 3; vl = cmd & 3; n = num or 256
            size = ((vn + 1) * (4, 2, 1, 2)[vl] * n + 3) & ~3
            bufs[imm & 0x3FF] = (cmd, d[p:p + size])
            p += size
        elif cmd == 0x20: p += 4
        elif cmd in (0x30, 0x31): p += 16


class Model:
    def __init__(self, blob):
        if blob[:4] != b'A2MF':
            raise ValueError('not an A2MF file')
        self.type = blob[4]
        d = self.d = blob[0x10:]
        self.bone_count, = struct.unpack_from('<h', d, 2)
        if self.type == 1:
            count = sum(struct.unpack_from('<hh', d, 4))
            bones, _, self.textures, self.extra = struct.unpack_from('<4I', d, 8)
            table = 0x18
        elif self.type == 0:
            count = sum(struct.unpack_from('<4h', d, 4))
            bones, _, self.textures, self.extra = struct.unpack_from('<4I', d, 0xC)
            table = 0x1C
        elif self.type == 3:
            self._init_map()
            return
        else:
            raise ValueError('unknown model type %d' % self.type)
        self.rest = [np.frombuffer(d, '<f4', 16, bones + i * 0xA0).reshape(4, 4)
                     for i in range(self.bone_count)]
        self.parent = [struct.unpack_from('<i', d, bones + i * 0xA0 + 0x80)[0]
                       for i in range(self.bone_count)]
        self.meshes = []
        self.swap_joint = False
        for off in struct.unpack_from('<%dI' % count, d, table):
            bone, bone2, _, flags = struct.unpack_from('<4B', d, off)
            if self.type == 1:
                qwords, data = struct.unpack_from('<II', d, off + 4)
            else:
                qwords, data = struct.unpack_from('<II', d, off + 8)
            self.meshes.append((bone, bone2, flags, off + data, off + data + qwords * 16))

    def _init_map(self):
        """Type 3: a map, cut into a grid of cells.

          +0x00 float origin[4]     +0x10 float cellSize[3]
          +0x20 s32 nx, ny, nz      +0x2C u32 textures (.Pim descriptors)
          +0x30 cell[nx*ny*nz]: u32 offset, u32 size (0 = empty)
          cell: s16 meshCountA, meshCountB, u32 mesh[] (relative to the cell);
          meshes as in type 1, with no bones: positions are world coordinates, there are
          no normals, and the colours carry the baked lighting.
        """
        d = self.d
        self.bone_count = 1
        self.rest = [np.eye(4, dtype=np.float32)]
        self.parent = [-1]
        self.extra = 0
        self.swap_joint = False
        self.origin = struct.unpack_from('<3f', d, 0)
        self.cell_size = struct.unpack_from('<3f', d, 0x10)
        self.grid = struct.unpack_from('<3i', d, 0x20)
        self.textures, = struct.unpack_from('<I', d, 0x2C)
        self.meshes = []
        for i in range(self.grid[0] * self.grid[1] * self.grid[2]):
            off, size = struct.unpack_from('<II', d, 0x30 + i * 8)
            if not size:
                continue
            count = sum(struct.unpack_from('<hh', d, off))
            for m in struct.unpack_from('<%dI' % count, d, off + 4):
                m += off
                flags = d[m + 3]
                qwords, data = struct.unpack_from('<II', d, m + 4)
                self.meshes.append((0, 0, flags, m + data, m + data + qwords * 16))

    def skinned_triangles(self):
        """The "extra block": vertices blended between two bones on the CPU.

          +0x00 u16 positions, u16 secondInfluences      +0x08 u16 meshCountA, meshCountB
          +0x0C positions[n]  xyz + weight      +0x10 normals[n]
          +0x14 positions2[m] xyz + weight      +0x18 normals2[m]     (for vertices 0..m-1)
          +0x1C uvs: s16 s, t, q, 0 (/4096)     +0x20 colours (u8 x 4)
          +0x24 / +0x28 / +0x2C / +0x30  runs of {s16 bone, s16 count}, -1 ends, one list per array
          +0x34 mesh offsets; mesh: u64 TEX0, u16 texture, u16 clut, u16 ?, u16 stripCount,
                u32 strip[stripCount]; strip: GIF tag (16 bytes, NLOOP = n), then
                n x {u16 position, u16 normal, u16 colour, u16 uv}
        """
        d, e = self.d, self.extra
        if not e:
            return
        count, count2 = struct.unpack_from('<HH', d, e)
        na, nb = struct.unpack_from('<HH', d, e + 8)
        offs = struct.unpack_from('<10I', d, e + 0xC)

        def blend(array, runs, n):
            src = np.frombuffer(d, '<f4', n * 4, e + array).reshape(n, 4)
            out = np.zeros((n, 3))
            i, p = 0, e + runs
            while True:
                bone, run = struct.unpack_from('<hh', d, p); p += 4
                if bone < 0:
                    break
                m = self.rest[bone]
                # xyz is already scaled by the weight; w scales the translation
                out[i:i + run] = src[i:i + run, :3] @ m[:3, :3] + m[3, :3] * src[i:i + run, 3:4]
                i += run
            return out

        pos = blend(offs[0], offs[6], count)
        pos[:count2] += blend(offs[2], offs[8], count2)
        uvs = e + offs[4]
        for mo in struct.unpack_from('<%dI' % (na + nb), d, e + 0x34):
            m = e + mo
            material = struct.unpack_from('<HH', d, m + 8)
            strips, = struct.unpack_from('<H', d, m + 0xE)
            for so in struct.unpack_from('<%dI' % strips, d, m + 0x10):
                n = struct.unpack_from('<I', d, m + so)[0] & 0x3FFF
                idx = np.frombuffer(d, '<u2', n * 4, m + so + 0x10).reshape(n, 4)
                v = pos[idx[:, 0]]
                uv = np.array([struct.unpack_from('<hh', d, uvs + int(k) * 8) for k in idx[:, 3]]) / 4096.0
                for i in range(n - 2):
                    a, b, c = (i, i + 1, i + 2) if i % 2 == 0 else (i + 1, i, i + 2)
                    yield v[[a, b, c]], uv[[a, b, c]], material

    def triangles(self):
        yield from self.rigid_triangles()
        yield from self.skinned_triangles()

    def rigid_triangles(self):
        """Yield (v[3][3], uv[3][2], material) in model space (rest pose)."""
        for bone, bone2, flags, start, end in self.meshes:
            m, m2 = self.rest[bone], self.rest[bone2]
            for n, pos, nrm, col, uv, kick, material in vif_batches(self.d, start, end):
                world = pos @ m[:3, :3] + m[3, :3]
                if bone2 != bone:
                    # Joint meshes: the normal's w picks the bone for each vertex.
                    other = pos @ m2[:3, :3] + m2[3, :3]
                    pick = (nrm[:, 3] != 0) ^ self.swap_joint
                    world = np.where(pick[:, None], other, world)
                # Maps: the fourth word of a position is 1 where the vertex
                # completes a triangle with the two before it, 0 where it does not.
                if self.type != 3:
                    kick = None
                for i in range(n - 2):
                    a, b, c = (i, i + 1, i + 2) if i % 2 == 0 else (i + 1, i, i + 2)
                    if kick is not None and not kick[i + 2]:
                        continue
                    yield world[[a, b, c]], uv[[a, b, c]], material


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\0' + rgb[y].tobytes() for y in range(h))
    def chunk(t, body):
        return struct.pack('>I', len(body)) + t + body + struct.pack('>I', zlib.crc32(t + body) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def load_textures(model, pim_path=None):
    """RGBA images keyed by (textureIndex, clutOffset), from the model or a separate .pim."""
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import bpm
    if model.textures:                 # embedded textures win
        sheet = bpm.Bpm(model.d[model.textures:])
    elif pim_path:
        sheet = bpm.Bpm(open(pim_path, 'rb').read())
    else:
        return None
    cache = {}
    def get(material):
        if material not in cache:
            tex = sheet.textures[material[0]] if material[0] < len(sheet.textures) else None
            try:
                cache[material] = tex.rgba(material[1] // tex.clut_step) if tex else None
            except Exception:
                cache[material] = None
        return cache[material]
    return get


def view(models, path, size=512, pim_path=None):
    """Front and side views with a z-buffer; textured when textures are available."""
    model = models[0]
    all_tris, tri_textures = [], []
    for m in models:
        get = load_textures(m, pim_path)
        for t in m.triangles():
            all_tris.append(t)
            tri_textures.append(get)
    tris = np.array([t[0] for t in all_tris])
    lo, hi = tris.reshape(-1, 3).min(0), tris.reshape(-1, 3).max(0)
    scale = (size - 20) / max(hi - lo)
    img = np.full((size, size * 2, 3), 32, np.uint8)
    is_map = models[0].type == 3
    # (horizontal axis, vertical axis, depth axis) per panel
    panels = ((0, 2, 1), (0, 1, 2)) if is_map else ((0, 1, 2), (2, 1, 0))
    for panel, (ax, ay, depth) in enumerate(panels):
        zbuf = np.full((size, size), -1e9)
        for t, (_, uv, material), textures in zip(tris, all_tris, tri_textures):
            n = np.cross(t[1] - t[0], t[2] - t[0])
            ln = np.linalg.norm(n)
            if ln == 0:
                continue
            shade = int(60 + 180 * abs(n[depth]) / ln)
            xs = (t[:, ax] - (lo[ax] + hi[ax]) / 2) * scale + size / 2
            ys = size - 10 - (t[:, ay] - lo[ay]) * scale
            x0, x1 = int(max(xs.min(), 0)), int(min(xs.max() + 1, size - 1))
            y0, y1 = int(max(ys.min(), 0)), int(min(ys.max() + 1, size - 1))
            if x1 < x0 or y1 < y0:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(den) < 1e-9:
                continue
            w0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / den
            w1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / den
            w2 = 1 - w0 - w1
            z = w0 * t[0, depth] + w1 * t[1, depth] + w2 * t[2, depth]
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & (z > zbuf[y0:y1 + 1, x0:x1 + 1])
            zbuf[y0:y1 + 1, x0:x1 + 1][inside] = z[inside]
            tex = textures(material) if textures else None
            region = img[y0:y1 + 1, panel * size + x0:panel * size + x1 + 1]
            if tex is None:
                region[inside] = shade
            else:
                th, tw = tex.shape[:2]
                u = (w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0])[inside]
                v = (w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1])[inside]
                texel = tex[(v * th).astype(int) % th, (u * tw).astype(int) % tw]
                opaque = texel[:, 3] >= 64
                lit = (texel[:, :3] * (0.55 + 0.45 * shade / 240.0)).clip(0, 255).astype(np.uint8)
                target = region[inside]
                target[opaque] = lit[opaque]
                region[inside] = target
    write_png(path, img)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if sys.argv[1] == 'view':
        # view out.png model.mdl [more.mdl ...] [textures.pim]
        files = sys.argv[3:]
        pim = next((f for f in files if f.lower().endswith('.pim')), None)
        models = [Model(open(f, 'rb').read()) for f in files if f != pim]
        view(models, sys.argv[2], pim_path=pim)
        return
    model = Model(open(sys.argv[2], 'rb').read())
    if sys.argv[1] == 'info':
        tris = sum(1 for _ in model.triangles())
        print('type %d  bones %d  meshes %d  triangles %d  textures @%#x  extra @%#x' %
              (model.type, model.bone_count, len(model.meshes), tris, model.textures, model.extra))
        for i, (bone, bone2, flags, start, end) in enumerate(model.meshes):
            mats = {m for *_, m in vif_batches(model.d, start, end)}
            print('  mesh %2d bone %2d/%2d flags %#04x %5d bytes  tex/clut %s' %
                  (i, bone, bone2, flags, end - start, sorted(mats)))
    elif sys.argv[1] == 'obj':
        with open(sys.argv[3], 'w') as f:
            k = 1
            for v, uv, _ in model.triangles():
                for p, t in zip(v, uv):
                    f.write('v %f %f %f\nvt %f %f\n' % (*p, t[0], 1 - t[1]))
                f.write('f %d/%d %d/%d %d/%d\n' % (k, k, k + 1, k + 1, k + 2, k + 2))
                k += 3


if __name__ == '__main__':
    main()
