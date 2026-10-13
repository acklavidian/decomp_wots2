// Skeletons, motion sampling and the motion player — decompiled from
// 0x1361F0–0x136BF0, 0x1376E0–0x138D70 and 0x1391C0–0x1395F0.

#include "engine/motion.h"
#include "native/guest.h"

#include <cmath>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  Matrix_Multiply(void* dst, const void* a, const void* b);     // 0x107130  dst = b × a
}

// ── 0x136AE0 ──────────────────────────────────────────────────────────────
void Vec4_FromFixed12(float out[4], const s16 in[4]) {
    for (int i = 0; i < 4; i++)
        out[i] = (float)in[i] / 4096.0f;
}

// ── 0x136B70 ──────────────────────────────────────────────────────────────
float Vec4_Dot(const float a[4], const float b[4]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

// ── 0x136370 ──────────────────────────────────────────────────────────────
float* Quat_Normalize(float q[4]) {
    float length = sqrtf(Vec4_Dot(q, q));       // 0x136BF0
    if (length == 0.0f) {
        q[3] = 1.0f;
    } else {
        for (int i = 0; i < 4; i++)
            q[i] /= length;
    }
    return q;
}

// The shorter way round: `to`, or its negation when the two point apart.
static float Nearest(const float q[4], const float to[4], float out[4]) {
    float dot = Vec4_Dot(q, to);
    for (int i = 0; i < 4; i++)
        out[i] = dot >= 0.0f ? to[i] : -to[i];
    return dot;
}

// ── 0x1369D0 ──────────────────────────────────────────────────────────────
float* Quat_Lerp(float t, float q[4], const float to[4]) {
    float target[4];
    Nearest(q, to, target);
    for (int i = 0; i < 4; i++)
        q[i] = target[i] * t + q[i] * (1.0f - t);
    return Quat_Normalize(q);
}

// ── 0x136890 ──────────────────────────────────────────────────────────────
float* Quat_Slerp(float t, float q[4], const float to[4]) {
    float target[4];
    float dot = Nearest(q, to, target);
    if (dot < 0.0f)
        dot = -dot;
    if (dot == 1.0f) {
        memcpy(q, target, sizeof(target));      // as the original: the end value, whatever t is
        return q;
    }
    // Native: two all-but-equal rotations can give a dot a hair over 1 (or an
    // angle whose sine is 0).  The PS2's FPU has no NaN, so the original gets
    // away with it; here the NaN would stay in the bone for good (it took a
    // character's jaw off).  Such a pair counts as equal.
    if (dot > 1.0f || sinf(acosf(dot)) == 0.0f) {
        memcpy(q, target, sizeof(target));
        return q;
    }
    const float angle = acosf(dot);
    const float scale = 1.0f / sinf(angle);
    const float from = scale * sinf(angle * (1.0f - t));
    const float toward = scale * sinf(angle * t);
    for (int i = 0; i < 4; i++)
        q[i] = q[i] * from + target[i] * toward;
    return Quat_Normalize(q);
}

// ── 0x1365F0 ──────────────────────────────────────────────────────────────
// (VU0 code in the original; this is the matrix it produces.)
void Quat_ToMatrix(const float q[4], float m[16]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);      m[3] = 0;
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);      m[7] = 0;
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
    m[15] = 1;      // the translation (m[12..14]) is left as it was
}

static inline int KeyCount(const u8* track) {
    s16 count;
    memcpy(&count, track + 2, 2);
    return count;
}

// ── 0x138A00 ──────────────────────────────────────────────────────────────
int Motion_SampleRotation(float frame, void*, float out[4], const u8* track, int hint) {
    const RotationKey* key = reinterpret_cast<const RotationKey*>(track + 8);
    const int count = KeyCount(track);
    const float whole = floorf(frame);

    if (frame <= (float)key[0].frame) {
        Vec4_FromFixed12(out, key[0].q);
        return 0;
    }
    if ((float)key[count - 1].frame <= frame) {
        Vec4_FromFixed12(out, key[count - 1].q);
        return count - 1;
    }
    if ((int)whole < key[hint - 1].frame)
        hint = 1;
    while (hint < count - 1 && key[hint].frame < (int)whole)
        hint++;

    const RotationKey& a = key[hint - 1];
    const RotationKey& b = key[hint];
    if (frame == (float)b.frame) {
        Vec4_FromFixed12(out, b.q);
    } else {
        const float t = (frame - (float)a.frame) / (float)a.span;
        float from[4], to[4];
        Vec4_FromFixed12(from, a.q);
        Vec4_FromFixed12(to, b.q);
        if (a.mode == 0)
            Quat_Lerp(t, from, to);
        else
            Quat_Slerp(t, from, to);
        memcpy(out, from, sizeof(from));
    }
    return hint;
}

// ── 0x138D70 ──────────────────────────────────────────────────────────────
int Motion_SamplePosition(float frame, void*, float out[4], const u8* track, int hint) {
    const PositionKey* key = reinterpret_cast<const PositionKey*>(track + 8);
    const int count = KeyCount(track);
    const float whole = floorf(frame);
    float value[4] = {0, 0, 0, 1.0f};

    auto plain = [&](const PositionKey& k) {
        const s16 raw[4] = {k.p[0], k.p[1], k.p[2], k.frame};
        Vec4_FromFixed12(value, raw);
    };
    if (frame <= (float)key[0].frame) {
        plain(key[0]);
        hint = 0;
    } else if ((float)key[count - 1].frame <= frame) {
        plain(key[count - 1]);
        hint = count - 1;
    } else {
        if ((int)whole < key[hint - 1].frame)
            hint = 1;
        while (hint < count && key[hint].frame < (int)whole)
            hint++;
        const PositionKey& a = key[hint - 1];
        const PositionKey& b = key[hint];
        if (frame == (float)b.frame) {
            plain(b);
        } else {
            const float t = (frame - (float)a.frame) / (float)a.span;
            const float t2 = t * t, t3 = t2 * t;
            const float basis[4] = {(t3 * 2.0f - t2 * 3.0f) + 1.0f, t2 * 3.0f - t3 * 2.0f,
                                    t3 + (t - t2 * 2.0f), t3 - t2};
            for (int axis = 0; axis < 3; axis++) {
                const s16 raw[4] = {a.p[axis], b.p[axis], a.tangent[axis], b.tangent[axis]};
                float terms[4];
                Vec4_FromFixed12(terms, raw);
                value[axis] = Vec4_Dot(basis, terms);
            }
        }
    }
    value[3] = 1.0f;
    memcpy(out, value, sizeof(value));
    return hint;
}

// ── 0x1391C0 ──────────────────────────────────────────────────────────────
void Motion_Relocate(u8* motion) {
    s16 bones;
    memcpy(&bones, motion + 2, 2);
    u32* track = reinterpret_cast<u32*>(motion + 8);
    for (int i = bones + 3; i != 0; i--, track++)
        if (*track != 0)
            *track += guest::addr(motion);
}

// ── 0x139250 ──────────────────────────────────────────────────────────────
Skeleton* Skeleton_Ctor(Skeleton* self, int count, const Bone* source) {
    Bone* bones = static_cast<Bone*>(MemAlloc((u32)count * sizeof(Bone)));
    self->bones = guest::addr(bones);
    memcpy(bones, source, (size_t)count * sizeof(Bone));
    for (int i = 0; i < count; i++)
        if (bones[i].parent != 0)
            bones[i].parent = self->bones + (bones[i].parent - guest::addr(source));
    self->count = count;
    return self;
}

// ── 0x1393A0 ──────────────────────────────────────────────────────────────
// world = local × parent's world (the root hangs off `root`).  A bone whose
// top flag byte is set is left alone once.
void Skeleton_UpdateWorld(Skeleton* self, const float* root) {
    Bone* bone = guest::ptr<Bone>(self->bones);
    for (int i = self->count; i != 0; i--, bone++) {
        if ((bone->flags & 0xFF000000u) == 0)
            Matrix_Multiply(bone->world, bone->parent ? guest::ptr<Bone>(bone->parent)->world : root,
                            bone->local);                   // 0x1389C0
        else
            bone->flags &= 0xFEFFFFFFu;
    }
}

// ── 0x1394B0 ──────────────────────────────────────────────────────────────
void Skeleton_Update(Skeleton* self, const float* root) {
    alignas(16) float base[16];
    memcpy(base, root, sizeof(base));           // 0x139480
    base[13] -= 4.0f;                           // as the original: four units down
    Skeleton_UpdateWorld(self, base);

    Bone* bone = guest::ptr<Bone>(self->bones);
    for (int i = 0; i < self->count; i++, bone++) {
        // Clear components that were never written (all bits set).
        u32* raw = reinterpret_cast<u32*>(bone->local);
        for (int k = 0; k < 16; k++)
            if (raw[k] == 0xFFFFFFFFu)
                raw[k] = 0;
        if ((bone->flags & 0xE0000000u) != 0) {
            // 0x1395F0: world position = parent's world applied to the local position
            const float* m = guest::ptr<Bone>(bone->parent)->world;
            const float* v = &bone->local[12];
            float r[4];
            for (int k = 0; k < 4; k++)
                r[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k] * v[3];
            memcpy(&bone->world[12], r, sizeof(r));
        }
    }
}


// ── 0x139630 ──────────────────────────────────────────────────────────────
// Hair, sleeves, skirts.  A bone that swings has a *tip*: a further entry of
// the bone table (flag bits 0xE0000000) that is not a joint but a point —
// its "local matrix" holds limits (+0x40: four numbers), the way it last
// pointed (+0x60) and where it sits on its parent (+0x70), and its world
// position (+0x30) is carried from frame to frame, which is what makes it
// lag behind.  Each frame the tip is moved — pulled down by its weight
// (flags 0xC0000000; `length` at +0x8C), or eased back towards its rest
// place (0x20000000) — and its parent, the swinging bone, is turned so that
// its z axis points at it, within the limits.  `wind` is added to every tip.
static inline void Cross3(float* o, const float* a, const float* b) {
    const float r[4] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0.0f};
    memcpy(o, r, sizeof r);
}
static inline float Normalize3(float* v) {
    const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0.0f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
    return length;
}
static inline void ApplyRow(float* o, const float* m, const float* v) {
    float r[4];
    for (int k = 0; k < 4; k++)
        r[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k] * v[3];
    memcpy(o, r, sizeof r);
}
void Skeleton_Swing(Skeleton* self, const float* wind) {
    Bone* bone = guest::ptr<Bone>(self->bones);
    for (int i = 0; i < self->count; i++, bone++) {
        if ((bone->flags & 0xE0000000u) == 0 || bone->parent == 0)
            continue;
        Bone* parent = guest::ptr<Bone>(bone->parent);
        // The frame the parent is to take: rows up, side, dir.
        alignas(16) float m[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        float* up = &m[0];
        float* side = &m[4];
        float* dir = &m[8];
        float tip[4];
        for (int k = 0; k < 4; k++)
            tip[k] = bone->world[12 + k] + wind[k];
        if (bone->flags & 0xC0000000u) {
            const float weight = bone->_88[1] * 2.5f;
            tip[1] += weight;
            float d[4];
            for (int k = 0; k < 4; k++)
                d[k] = tip[k] - parent->world[12 + k];
            if (d[1] > 0.0f) {
                float along[4];
                for (int k = 0; k < 4; k++)
                    along[k] = parent->world[4 + k] * weight;
                for (int k = 0; k < 4; k++)
                    d[k] = along[1] < 0.0f ? d[k] + along[k] : d[k] - along[k];
            }
            Normalize3(d);
            memcpy(dir, d, sizeof d);
            const float facing = parent->world[0] * d[0] + parent->world[1] * d[1] + parent->world[2] * d[2];
            if (fabsf(facing) < 0.99f) {
                Cross3(side, dir, &parent->world[0]);
                Normalize3(side);
                Cross3(up, side, dir);
            } else {
                Cross3(up, &parent->world[4], dir);
                Normalize3(up);
                Cross3(side, dir, up);
            }
        } else {
            // Where the tip would sit if its bone kept its pose.
            alignas(16) float posed[16];
            if (parent->parent != 0)
                Matrix_Multiply(posed, guest::ptr<Bone>(parent->parent)->world, parent->local);
            else
                memcpy(posed, parent->world, sizeof posed);
            float rest[4];
            ApplyRow(rest, posed, &bone->local[12]);
            for (int k = 0; k < 4; k++)
                tip[k] = tip[k] * 0.6f + rest[k] * 0.4f;
            for (int k = 0; k < 4; k++)
                dir[k] = tip[k] - parent->world[12 + k];
            Normalize3(dir);
            Cross3(up, &parent->world[4], dir);
            Normalize3(up);
            Cross3(side, dir, up);
        }
        dir[3] = 0.0f;

        // Into the parent's own space, where the limits are.
        alignas(16) float base[16], back[16];
        memcpy(base, parent->world, sizeof base);
        base[12] = base[13] = base[14] = 0.0f;
        base[15] = 1.0f;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                back[r * 4 + c] = base[c * 4 + r];              // 0x107200: transposed
        Matrix_Multiply(m, back, m);
        const float* limitA = &bone->local[0];                  // +0x40
        const float* limitB = bone->rotation;                   // +0x90
        float v[4];
        memcpy(v, dir, sizeof v);
        v[0] = 0.0f;
        float length = Normalize3(v);
        if (v[1] > 0.0f) {
            if (v[2] - limitA[0] < 0.0f) {
                dir[2] = length * limitA[0];
                dir[1] = -length * limitA[1];
                Cross3(side, dir, up);
                Normalize3(side);
                Cross3(up, side, dir);
            }
        } else if (v[2] - limitB[0] < 0.0f) {
            dir[2] = length * limitB[0];
            dir[1] = -length * limitB[1];
            Cross3(side, dir, up);
            Normalize3(side);
            Cross3(up, side, dir);
        }
        memcpy(v, dir, sizeof v);
        v[1] = 0.0f;
        length = Normalize3(v);
        if (v[0] < 0.0f) {
            if (v[2] - limitA[2] < 0.0f) {
                dir[2] = length * limitA[2];
                dir[0] = length * limitA[3];
                Cross3(up, side, dir);
                Normalize3(up);
                Cross3(side, dir, up);
            }
        } else if (v[2] - limitB[2] < 0.0f) {
            dir[2] = length * limitB[2];
            dir[0] = length * limitB[3];
            Cross3(up, side, dir);
            Normalize3(up);
            Cross3(side, dir, up);
        }
        memcpy(&bone->local[8], dir, 4 * sizeof(float));        // +0x60: the way it points
        Matrix_Multiply(m, base, m);
        memcpy(parent->world, m, 12 * sizeof(float));           // (0x136E10: the three rows)
        ApplyRow(&bone->world[12], parent->world, &bone->local[12]);
    }
}

// ── Quaternion products (VU0 code in the original) ────────────────────────
// 0x1362C0: q = q ⊗ p
float* Quat_Mul(float q[4], const float p[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = p[3] * x + p[2] * y - p[1] * z + p[0] * w;
    q[1] = -p[2] * x + p[3] * y + p[0] * z + p[1] * w;
    q[2] = p[1] * x - p[0] * y + p[3] * z + p[2] * w;
    q[3] = -p[0] * x - p[1] * y - p[2] * z + p[3] * w;
    return q;
}
// 0x136250: q = p ⊗ q
float* Quat_MulLeft(float q[4], const float p[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = p[3] * x - p[2] * y + p[1] * z + p[0] * w;
    q[1] = p[2] * x + p[3] * y - p[0] * z + p[1] * w;
    q[2] = -p[1] * x + p[0] * y + p[3] * z + p[2] * w;
    q[3] = -p[0] * x - p[1] * y - p[2] * z + p[3] * w;
    return q;
}
// 0x136330
float* Quat_Conjugate(float q[4]) {
    q[0] = -q[0]; q[1] = -q[1]; q[2] = -q[2];
    return q;
}
// 0x136680: turns q by `angle` (as the original: sine and cosine of the
// whole angle, not half of it) about `axis`.
float* Quat_Turn(float angle, float q[4], const float axis[4]) {
    const float s = sinf(angle);
    const float turn[4] = {axis[0] * s, axis[1] * s, axis[2] * s, cosf(angle)};
    return Quat_MulLeft(q, turn);
}

// ── The motion player ─────────────────────────────────────────────────────
static_assert(sizeof(MotionPlayer) == 0x170 && offsetof(MotionPlayer, vtable) == 0x164, "MotionPlayer");
static_assert(offsetof(MotionPlayer, travel) == 0x140, "MotionPlayer");

static inline const u8* MotionOf(const MotionPlayer* self) {
    if (self->motion == 0)
        return nullptr;
    return guest::ptr<const u8>(*guest::ptr<u32>(self->motion));
}
static inline s16 MotionS16(const u8* motion, int offset) { s16 v; memcpy(&v, motion + offset, 2); return v; }
static inline const u8* MotionTrack(const u8* motion, int index) {
    u32 at;
    memcpy(&at, motion + 8 + index * 4, 4);
    return at ? guest::ptr<const u8>(at) : nullptr;
}

// ── 0x1376E0 ──────────────────────────────────────────────────────────────
MotionPlayer* MotionPlayer_Ctor(MotionPlayer* self) {
    self->vtable = kVtMotionPlayer;
    self->motion = self->previous = 0;
    self->_18 = 0; self->_14 = 0; self->_1c = 0;
    self->frame = 0.0f;
    self->lastFrame = 0.0f;
    self->speed = 1.0f;
    self->firstBone = self->lastBone = 0;
    self->flags = 7;
    self->blend = 0.0f;
    self->loops = 0;
    for (int i = 0; i < 0x40; i++)
        self->boneHint[i] = 1;
    self->travelHint = self->rootHint = self->turnHint = 1;
    return self;
}

// ── 0x1377F0 ──────────────────────────────────────────────────────────────
extern "C" void MemFree(void* p);
MotionPlayer* MotionPlayer_Dtor(MotionPlayer* self, s16 flag) {
    if (self) {
        self->vtable = kVtMotionPlayer;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
GUEST_BIND(0x1377F0, MotionPlayer_Dtor);

// ── 0x137C80 ──────────────────────────────────────────────────────────────
// Track 2: how far the motion has turned the character round.
void MotionPlayer_SampleTurn(MotionPlayer* self) {
    const u8* motion = MotionOf(self);
    if (!motion || self->firstBone != 0)
        return;
    if (const u8* track = MotionTrack(motion, 2)) {
        self->turnHint = Motion_SampleRotation(self->frame, self, self->turn, track, 1);
    } else {
        self->turn[0] = self->turn[1] = self->turn[2] = 0.0f;
        self->turn[3] = 1.0f;
    }
}

// ── 0x137D70 ──────────────────────────────────────────────────────────────
// Track 0: how far it has carried the character.
void MotionPlayer_SampleTravel(MotionPlayer* self) {
    const u8* motion = MotionOf(self);
    if (!motion || self->firstBone != 0)
        return;
    if (const u8* track = MotionTrack(motion, 0))
        self->travelHint = Motion_SamplePosition(self->frame, self, self->travel, track, 1);
    for (int i = 0; i < 0x40; i++)
        self->boneHint[i] = 1;
    self->rootHint = 1;
    self->turnHint = 1;
}

// ── 0x137E70 ──────────────────────────────────────────────────────────────
void MotionPlayer_ResetTravel(MotionPlayer* self) {
    self->travel[0] = self->travel[1] = self->travel[2] = 0.0f;
    self->travel[3] = 1.0f;
    for (int i = 0; i < 0x40; i++)
        self->boneHint[i] = 1;
    self->rootHint = 1;
    self->turnHint = 1;
}

// ── 0x137F10 ──────────────────────────────────────────────────────────────
// Poses the bones of the player's range and gives the distance travelled
// since the last call.  While `blend` runs, each bone only goes a share of
// the way from where it is.
void MotionPlayer_Apply(MotionPlayer* self, Skeleton* skeleton, float* travelled, int pose) {
    const u8* motion = MotionOf(self);
    Bone* bones = guest::ptr<Bone>(skeleton->bones);
    int first = self->firstBone, last = self->lastBone;
    if (first > skeleton->count) first = skeleton->count;
    if (last > skeleton->count) last = skeleton->count;

    if (pose) {
        for (int i = first; i < last; i++) {
            const u8* track = MotionTrack(motion, 3 + i);
            if (!track)
                continue;
            float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            self->boneHint[i] = Motion_SampleRotation(self->frame, self, q, track, self->boneHint[i]);
            if (self->blend != 0.0f)
                Quat_Lerp(1.0f - 1.0f / self->blend, q, bones[i].rotation);
            memcpy(bones[i].rotation, q, sizeof q);
            Quat_ToMatrix(q, bones[i].local);
        }
    }
    if (first != 0)
        return;
    if (const u8* track = MotionTrack(motion, 0)) {
        float at[4];
        self->travelHint = Motion_SamplePosition(self->frame, self, at, track, self->travelHint);
        for (int i = 0; i < 4; i++)
            travelled[i] = at[i] - self->travel[i];
        memcpy(self->travel, at, sizeof at);
    }
    if (const u8* track = MotionTrack(motion, 1)) {
        float at[4];
        self->rootHint = Motion_SamplePosition(self->frame, self, at, track, self->rootHint);
        u16 trackFlags;
        memcpy(&trackFlags, track, 2);
        if ((trackFlags & 1) == 0)
            at[1] *= bones[0]._88[0];           // the model's own height
        float* place = &bones[0].local[12];
        if (self->blend == 0.0f) {
            place[0] = at[0]; place[1] = at[1]; place[2] = at[2];
        } else {
            const float share = 1.0f / self->blend;
            for (int i = 0; i < 3; i++)
                place[i] = at[i] * share + place[i] * (1.0f - share);
        }
    }
}

// ── 0x137860 ──────────────────────────────────────────────────────────────
// One frame.  Returns the new frame number, −1 when the motion has run out
// and −2 when there is none.
float MotionPlayer_Step(MotionPlayer* self, Skeleton* skeleton, float* travelled, int pose) {
    const u8* motion = MotionOf(self);
    if (!motion)
        return -2.0f;
    float result = 0.0f;
    const float end = (float)MotionS16(motion, 6);
    if ((self->flags & kMotionLoops) && MotionS16(motion, 6) > 0) {
        const float loop = (float)MotionS16(motion, 4);
        if (end <= self->frame) {
            // Wrap round, keeping the travel that was still owed.
            float owed[4];
            memcpy(owed, self->travel, sizeof owed);
            MotionPlayer_SampleTravel(self);
            for (int i = 0; i < 4; i++)
                owed[i] = self->travel[i] - owed[i];
            float over = self->frame - end;
            const float span = end - loop;
            if (span == 0.0f) {
                self->frame = loop;
            } else {
                while (span < over)
                    over -= span;
                self->frame = loop + over;
            }
            MotionPlayer_SampleTravel(self);
            for (int i = 0; i < 4; i++)
                self->travel[i] -= owed[i];
            self->loops++;
        }
    }
    if (end <= self->frame)
        result = -1.0f;
    MotionPlayer_Apply(self, skeleton, travelled, pose);
    if (self->blend != 0.0f) {
        self->blend -= self->speed;
        if (self->blend < 0.0f)
            self->blend = 0.0f;
    }
    self->lastFrame = self->frame;
    if (self->flags & kMotionAdvance)
        self->frame += self->speed;
    if (result >= 0.0f)
        result = self->frame;
    return result;
}
GUEST_BIND(0x137860, MotionPlayer_Step);

// ── 0x1383C0 / 0x138400 / 0x138440 ────────────────────────────────────────
u32 MotionPlayer_SetFlags(MotionPlayer* self, u32 flags)   { const u32 old = self->flags; self->flags = flags; return old; }
u32 MotionPlayer_AddFlags(MotionPlayer* self, u32 flags)   { const u32 old = self->flags; self->flags |= flags; return old; }
u32 MotionPlayer_ClearFlags(MotionPlayer* self, u32 flags) { const u32 old = self->flags; self->flags &= ~flags; return old; }

// ── 0x1386C0 ──────────────────────────────────────────────────────────────
void MotionPlayer_SetRange(MotionPlayer* self, s16 first, s16 last) {
    const u8* motion = MotionOf(self);
    if (!motion)
        return;
    self->firstBone = first;
    self->lastBone = last;
    if (MotionS16(motion, 2) < self->lastBone)
        self->lastBone = MotionS16(motion, 2);
    if (self->lastBone <= self->firstBone)
        self->firstBone = self->lastBone - 1;
}

// ── 0x1387C0 ──────────────────────────────────────────────────────────────
float MotionPlayer_SetBlend(float frames, MotionPlayer* self) {
    const float old = self->blend;
    self->blend = frames;
    return old;
}

// ── 0x138800 ──────────────────────────────────────────────────────────────
s16 MotionPlayer_LoopStart(MotionPlayer* self) {
    const u8* motion = MotionOf(self);
    return motion ? MotionS16(motion, 4) : 0;
}

// ── 0x138870 ──────────────────────────────────────────────────────────────
// Hands the turn the motion made to the character's own matrix and takes it
// out of the root bone, so the next motion starts facing the new way.
void MotionPlayer_BakeTurn(MotionPlayer* self, Skeleton* skeleton, float* matrix) {
    const u8* motion = MotionOf(self);
    if (!motion || self->firstBone != 0 || !MotionTrack(motion, 2))
        return;
    float q[4];
    memcpy(q, self->turn, sizeof q);
    alignas(16) float turned[16];
    Quat_ToMatrix(q, turned);
    turned[12] = turned[13] = turned[14] = 0.0f;
    Matrix_Multiply(matrix, matrix, turned);
    Quat_Conjugate(q);
    Bone* root = guest::ptr<Bone>(skeleton->bones);
    Quat_Mul(q, root->rotation);
    memcpy(root->rotation, q, sizeof q);
    Quat_ToMatrix(q, root->local);
}

// ── 0x138490 ──────────────────────────────────────────────────────────────
void MotionPlayer_Play(float start, float blend, MotionPlayer* self, Skeleton* skeleton, float* matrix,
                       u32 motion) {
    MotionPlayer_SampleTurn(self);
    if (self->flags & kMotionRoot)
        MotionPlayer_BakeTurn(self, skeleton, matrix);
    self->previous = self->motion;
    self->motion = motion;
    if (blend >= 0.0f)
        MotionPlayer_SetBlend(blend, self);
    self->frame = start;
    self->lastFrame = start;
    if (self->flags & kMotionRoot)
        MotionPlayer_SampleTravel(self);
    else
        MotionPlayer_ResetTravel(self);
    const u8* data = MotionOf(self);
    MotionPlayer_SetRange(self, 0, MotionS16(data, 2));
    self->loops = 0;
    if (MotionS16(data, 4) < 0)
        self->flags &= ~(u32)kMotionLoops;
    else
        self->flags |= kMotionLoops;
}

// ── 0x138600 ──────────────────────────────────────────────────────────────
// Jump to a frame; with `keepTravel` the distance owed is carried over.
void MotionPlayer_SetFrame(float frame, MotionPlayer* self, int keepTravel) {
    if (!keepTravel) {
        self->frame = frame;
        return;
    }
    float owed[4];
    memcpy(owed, self->travel, sizeof owed);
    MotionPlayer_SampleTravel(self);
    for (int i = 0; i < 4; i++)
        owed[i] = self->travel[i] - owed[i];
    self->frame = frame;
    MotionPlayer_SampleTravel(self);
    for (int i = 0; i < 4; i++)
        self->travel[i] -= owed[i];
}
