#!/usr/bin/env python3
"""Decode a motion pack (.mtp) from Way of the Samurai 2 and pose a model with it.

Pack (from the loader at 0x1CF030):
  u32 version (>= 5), u32 count,
  count x { u32 motionId, u32 size, data[size] },      motionId 0xFFFFFFFF = skip
  u32 tableSize, table[tableSize]                      (which motion plays for which action)

Motion data: u32 8, u32 size, then the motion proper (track offsets are relative to it):
  s16 ?, s16 boneCount, s16 ?, s16 frameCount, u32 track[boneCount + 3]   (0 = no track)
  track 0         position: how far the character travels over the move (x, z)
  track 1         position: the root bone (hips) relative to the character
  track 2         position: unused in the packs looked at
  track 3 + i     rotation track of bone i

Rotation track (sampler 0x138A00): s16 ?, s16 keyCount, 4 bytes, then 0x18-byte keys
  s16 quaternion[4] (/4096), 8 bytes, s16 frame, s16 span, s16 mode (0 lerp, else slerp), s16
Position track (sampler 0x138D70): s16 ?, s16 keyCount, 4 bytes, then 0x10-byte keys
  s16 xyz (/4096), s16 frame, s16 tangent xyz (/4096), s16 span        (Hermite)

  tools/mtp.py info pack.mtp
  tools/mtp.py pose out.png pack.mtp <motion index> <frame> body.mdl [head.mdl] [textures.pim]
"""
import os, struct, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


class Motion:
    def __init__(self, blob, motion_id):
        self.id = motion_id
        self.d = d = blob[8:]
        _, self.bone_count, _, self.frames = struct.unpack_from('<4h', d, 0)
        self.tracks = struct.unpack_from('<%dI' % (self.bone_count + 3), d, 8)

    def _keys(self, track, stride):
        count, = struct.unpack_from('<h', self.d, track + 2)
        return [track + 8 + i * stride for i in range(count)]

    def rotation(self, bone, frame):
        """Quaternion (x, y, z, w) of a bone at `frame`, or None if it has no track."""
        track = self.tracks[3 + bone]
        if not track:
            return None
        d = self.d
        keys = self._keys(track, 0x18)
        q = lambda k: np.array(struct.unpack_from('<4h', d, k), np.float64) / 4096.0
        when = lambda k: struct.unpack_from('<h', d, k + 0x10)[0]
        if frame <= when(keys[0]):
            return q(keys[0])
        if frame >= when(keys[-1]):
            return q(keys[-1])
        i = next(i for i in range(1, len(keys)) if when(keys[i]) >= frame)
        a, b = keys[i - 1], keys[i]
        span, mode = struct.unpack_from('<hh', d, a + 0x12)
        t = (frame - when(a)) / span if span else 0.0
        qa, qb = q(a), q(b)
        if np.dot(qa, qb) < 0:
            qb = -qb
        if mode == 0:
            out = qa + (qb - qa) * t
        else:
            c = float(np.clip(np.dot(qa, qb), -1, 1))
            angle = np.arccos(c)
            if angle < 1e-4:
                out = qa
            else:
                out = (np.sin((1 - t) * angle) * qa + np.sin(t * angle) * qb) / np.sin(angle)
        return out / np.linalg.norm(out)

    def position(self, index, frame):
        track = self.tracks[index]
        if not track:
            return None
        d = self.d
        keys = self._keys(track, 0x10)
        p = lambda k: np.array(struct.unpack_from('<3h', d, k), np.float64) / 4096.0
        m = lambda k: np.array(struct.unpack_from('<3h', d, k + 8), np.float64) / 4096.0
        when = lambda k: struct.unpack_from('<h', d, k + 6)[0]
        if frame <= when(keys[0]):
            return p(keys[0])
        if frame >= when(keys[-1]):
            return p(keys[-1])
        i = next(i for i in range(1, len(keys)) if when(keys[i]) >= frame)
        a, b = keys[i - 1], keys[i]
        span, = struct.unpack_from('<h', d, a + 0xE)
        t = (frame - when(a)) / span if span else 0.0
        t2, t3 = t * t, t * t * t
        return ((2 * t3 - 3 * t2 + 1) * p(a) + (3 * t2 - 2 * t3) * p(b)
                + (t3 - 2 * t2 + t) * m(a) + (t3 - t2) * m(b))


class Pack:
    def __init__(self, blob):
        self.version, count = struct.unpack_from('<II', blob, 0)
        self.motions = []
        p = 8
        for _ in range(count):
            motion_id, size = struct.unpack_from('<II', blob, p)
            if motion_id != 0xFFFFFFFF and size >= 16:
                self.motions.append(Motion(blob[p + 8:p + 8 + size], motion_id))
            p += 8 + size
        table_size, = struct.unpack_from('<I', blob, p)
        self.table = blob[p + 4:p + 4 + table_size]


def quat_matrix(q):
    """Rotation matrix for row vectors (v' = v @ M)."""
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
                     [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
                     [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]])


def pose(model, motion, frame):
    """Model-space matrices for every bone of `model` at `frame`."""
    import mdl  # noqa: F401
    d = model.d
    bones_off = struct.unpack_from('<I', d, 8 if model.type == 1 else 0xC)[0]
    out = []
    for i in range(model.bone_count):
        local = np.frombuffer(d, '<f4', 16, bones_off + i * 0xA0 + 0x40).reshape(4, 4).astype(np.float64).copy()
        if i < motion.bone_count:
            q = motion.rotation(i, frame)
            if q is not None:
                local[:3, :3] = quat_matrix(q)
            if i == 0:
                root = motion.position(1, frame)
                if root is not None:
                    local[3, :3] = root
        parent = model.parent[i]
        out.append(local if parent < 0 else local @ out[parent])
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if sys.argv[1] == 'info':
        pack = Pack(open(sys.argv[2], 'rb').read())
        print('version %d, %d motions, table %d bytes' % (pack.version, len(pack.motions), len(pack.table)))
        for i, m in enumerate(pack.motions):
            tracks = sum(1 for t in m.tracks[3:] if t)
            print('  %3d id %5d  bones %d  frames %3d  rotation tracks %d  position tracks %s' %
                  (i, m.id, m.bone_count, m.frames, tracks, [bool(t) for t in m.tracks[:3]]))
    elif sys.argv[1] == 'pose':
        import mdl
        out, pack_path, index, frame = sys.argv[2], sys.argv[3], int(sys.argv[4]), float(sys.argv[5])
        files = sys.argv[6:]
        pim = next((f for f in files if f.lower().endswith('.pim')), None)
        models = [mdl.Model(open(f, 'rb').read()) for f in files if f != pim]
        motion = Pack(open(pack_path, 'rb').read()).motions[index]
        for m in models:
            m.rest = [r.astype(np.float32) for r in pose(m, motion, frame)]
        mdl.view(models, out, pim_path=pim)


if __name__ == '__main__':
    main()
