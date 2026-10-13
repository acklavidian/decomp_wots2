// Head turning and foot placement — decompiled from 0x200ED0–0x201E90.
//
// The part at Chara+0x18C (0x40 bytes).  While a character has somebody (or
// some place) to look at, its head bone (20) is turned toward it a little
// each frame, within limits; afterwards the head eases back over 20 frames.
// The same part lifts a foot onto ground that is higher than where the
// character stands (steps, slopes) by bending the leg: two-bone IK over the
// thigh, shin and foot, written straight into the bones' world matrices.

#include "engine/motion.h"
#include "game/map_collision.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstring>

extern "C" {
    void  Matrix_Multiply(void* dst, const void* a, const void* b);     // 0x107130  dst = b × a
}

namespace {

struct Look {
    s16   state;        // +0x00  0 idle, 1 looking, 2 coming back
    s16   timer;        // +0x02  frames left of coming back (20)
    u8    _04[0x0C];
    float target[4];    // +0x10  the point looked at
    float rotation[4];  // +0x20  the head's rotation as this part left it
    float step;         // +0x30  the most the head turns in a frame (half angle)
    u8    _34[0x0C];
};
static_assert(sizeof(Look) == 0x40 && offsetof(Look, rotation) == 0x20 && offsetof(Look, step) == 0x30, "Look");

constexpr int kHead = 20, kChest = 9;
constexpr int kLeftFoot = 3, kRightFoot = 6;        // (which is which is a guess)

inline Bone* BonesOf(Skeleton* s) { return guest::ptr<Bone>(s->bones); }
inline void Copy4(float* d, const float* s) { memcpy(d, s, 16); }
inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float Length3(const float* a) { return sqrtf(Dot3(a, a)); }
// sceVu0Normalize: x, y, z only.
inline void Normalize3(float* d, const float* s) {
    const float len = Length3(s);
    const float k = len != 0.0f ? 1.0f / len : 0.0f;
    d[0] = s[0] * k; d[1] = s[1] * k; d[2] = s[2] * k; d[3] = s[3];
}
inline void Sub4(float* d, const float* a, const float* b) { for (int i = 0; i < 4; i++) d[i] = a[i] - b[i]; }

// 0x136350
inline void ConjugateTo(float* d, const float* s) { d[0] = -s[0]; d[1] = -s[1]; d[2] = -s[2]; d[3] = s[3]; }

// 0x136210: the rotation of a matrix.
void FromMatrix(float q[4], const float* m) {
    const float a = m[0], b = m[5], c = m[10];
    const float trace = a + b + c;
    if (trace >= 0.0f) {
        const float s = sqrtf(trace + 1.0f);
        const float f = 0.5f / s;
        q[3] = s * 0.5f;
        q[0] = (m[6] - m[9]) * f;
        q[1] = (m[8] - m[2]) * f;
        q[2] = (m[1] - m[4]) * f;
    } else {
        int which = a < b ? 1 : 0;
        if ((a < b ? b : a) < c)
            which = 2;
        if (which == 0) {
            const float s = sqrtf(a - b - c + 1.0f);
            const float f = 0.5f / s;
            q[0] = s * 0.5f;
            q[1] = (m[4] + m[1]) * f;
            q[2] = (m[2] + m[8]) * f;
            q[3] = (m[6] - m[9]) * f;
        } else if (which == 1) {
            const float s = sqrtf(b - c - a + 1.0f);
            const float f = 0.5f / s;
            q[1] = s * 0.5f;
            q[2] = (m[9] + m[6]) * f;
            q[0] = (m[4] + m[1]) * f;
            q[3] = (m[8] - m[2]) * f;
        } else {
            const float s = sqrtf(c - a - b + 1.0f);
            const float f = 0.5f / s;
            q[2] = s * 0.5f;
            q[0] = (m[2] + m[8]) * f;
            q[1] = (m[9] + m[6]) * f;
            q[3] = (m[1] - m[4]) * f;
        }
    }
    Quat_Normalize(q);
}

// 0x136700: the rotation that takes direction `a` to direction `b`.
void Between(float out[4], const float* a, const float* b) {
    const float d = Dot3(a, b);
    float q[4];
    if (d > 0.999999f) {
        q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f;
    } else if (d < -0.999999f) {
        // Opposite: half a turn about something across `a`.
        q[0] = 0.0f; q[1] = a[0]; q[2] = -a[1];
        float len = Length3(q);
        if (len < 1e-6f) {
            q[0] = -a[2]; q[1] = 0.0f; q[2] = a[0];
            len = Length3(q);
        }
        if (len != 0.0f) { q[0] /= len; q[1] /= len; q[2] /= len; }
        q[3] = 0.0f;
    } else {
        q[0] = a[1] * b[2] - a[2] * b[1];
        q[1] = a[2] * b[0] - a[0] * b[2];
        q[2] = a[0] * b[1] - a[1] * b[0];
        const float len = Length3(q);
        const float s = sqrtf((1.0f - d) * 0.5f) / (len != 0.0f ? len : 1.0f);
        q[0] *= s; q[1] *= s; q[2] *= s;
        q[3] = sqrtf((d + 1.0f) * 0.5f);
    }
    Copy4(out, q);
}

// out = q ⊗ v ⊗ q*: `v` turned by `q`.
void Rotate(float out[4], const float* q, const float* v) {
    float r[4], c[4];
    Copy4(r, q);
    ConjugateTo(c, q);
    Quat_Mul(r, v);
    Quat_Mul(r, c);
    Copy4(out, r);
}

// No more than `limit` (a half angle) of the turn `q`.
void Limit(float q[4], float limit) {
    float axis[4];
    Normalize3(axis, q);
    const float s = sinf(limit);
    q[0] = axis[0] * s; q[1] = axis[1] * s; q[2] = axis[2] * s;
    q[3] = cosf(limit);
}

inline float Acos(float c) { return acosf(c < -1.0f ? -1.0f : c); }

// ── 0x201060 ──────────────────────────────────────────────────────────────
// Bends a leg so that its foot comes to `target`.
int PlaceFoot(Skeleton* skeleton, int index, const float* target) {
    float kneeAxis[4] = {0, 0, 0, 0};
    float hipAxis[4] = {0, 0, 0, 0};
    float kneeBase[4] = {0.0f, 0.0f, 0.70710677f, 0.70710677f};        // 0x2D9650
    if (index == kRightFoot) {
        hipAxis[1] = -1.0f;
        kneeBase[2] = -0.70710677f;
    } else if (index == kLeftFoot) {
        hipAxis[1] = 1.0f;
    } else {
        return -1;
    }
    kneeAxis[0] = 1.0f;

    Bone* foot = BonesOf(skeleton) + index;
    Bone* shin = guest::ptr<Bone>(foot->parent);
    Bone* thigh = guest::ptr<Bone>(shin->parent);
    const float footLen = foot->_88[0];
    const float shinLen = shin->_88[0];
    const float reachMax = footLen + shinLen - 0.005f;

    float shinDir[4];       // where the knee lies, in the thigh's own space
    Copy4(shinDir, &shin->local[12]);
    for (int i = 0; i < 4; i++) shinDir[i] /= shinLen;

    float hip[4], toFoot[4], toKnee[4], toTarget[4], aim[4];
    Copy4(hip, &thigh->world[12]);
    Sub4(toFoot, &foot->world[12], hip);
    Normalize3(toFoot, toFoot);
    Sub4(toKnee, &shin->world[12], hip);
    Normalize3(toKnee, toKnee);
    Sub4(toTarget, target, hip);
    float dist = Dot3(toTarget, toTarget);
    dist = dist == 0.0f ? 1e-6f : sqrtf(dist);
    for (int i = 0; i < 4; i++) aim[i] = toTarget[i] / dist;
    const float reach = dist <= reachMax ? dist : reachMax;

    // The triangle hip – knee – foot with the two bones' lengths.
    const float a2 = shinLen * shinLen, b2 = footLen * footLen, c2 = reach * reach;
    float half = Acos((c2 - a2 - b2) / (shinLen * 2.0f * footLen)) * 0.5f;
    float knee[4] = {kneeAxis[0] * sinf(half), kneeAxis[1] * sinf(half), kneeAxis[2] * sinf(half), cosf(half)};
    half = Acos((b2 - a2 - c2) / (shinLen * -2.0f * reach)) * 0.5f;
    float thighRot[4] = {hipAxis[0] * -sinf(half), hipAxis[1] * -sinf(half), hipAxis[2] * -sinf(half), cosf(half)};

    float swing[4], inv[4], e[4], t[4], parent[4];
    Between(swing, toFoot, aim);
    Rotate(swing, swing, toKnee);           // the knee's direction, swung with the leg
    ConjugateTo(inv, swing);                // (kept as the original leaves it)
    Between(swing, swing, aim);
    FromMatrix(parent, guest::ptr<Bone>(thigh->parent)->world);

    if (swing[3] == 1.0f || swing[3] == 0.0f) {
        Rotate(e, parent, shinDir);
        ConjugateTo(inv, parent);
        Between(e, e, aim);
        Quat_Mul(inv, e);
        Quat_Mul(inv, parent);
        Quat_Mul(inv, thighRot);
        Copy4(thighRot, inv);
    } else {
        float twist[4];
        Normalize3(swing, swing);
        Rotate(e, parent, hipAxis);
        ConjugateTo(inv, parent);
        Between(t, e, swing);
        Quat_MulLeft(t, inv);
        Quat_Mul(t, parent);
        Copy4(twist, t);
        Quat_Mul(parent, twist);
        Rotate(e, parent, shinDir);
        ConjugateTo(inv, parent);
        Between(t, e, aim);
        if (t[3] == 0.0f)
            Copy4(t, swing);
        Quat_MulLeft(t, inv);
        Quat_Mul(t, parent);
        Quat_MulLeft(t, twist);
        Quat_Mul(t, thighRot);
        Copy4(thighRot, t);
    }

    alignas(16) float m[16] = {};
    Quat_ToMatrix(thighRot, m);
    Copy4(&m[12], &thigh->local[12]);
    Matrix_Multiply(thigh->world, guest::ptr<Bone>(thigh->parent)->world, m);
    Quat_MulLeft(knee, kneeBase);
    Quat_ToMatrix(knee, m);
    Copy4(&m[12], &shin->local[12]);
    Matrix_Multiply(shin->world, thigh->world, m);
    // sceVu0ApplyMatrix: the foot's place only.
    const float* w = shin->world;
    const float* v = &foot->local[12];
    float pos[4];
    for (int i = 0; i < 4; i++)
        pos[i] = w[i] * v[0] + w[4 + i] * v[1] + w[8 + i] * v[2] + w[12 + i] * v[3];
    Copy4(&foot->world[12], pos);
    thigh->flags |= 0x1000000;      // "leave my world matrix alone once"
    shin->flags |= 0x1000000;
    foot->flags |= 0x1000000;
    return 0;
}

}  // namespace

extern "C" {

// ── 0x201E90 ──────────────────────────────────────────────────────────────
void* CharaState_Ctor(void* mem) {
    Look* self = static_cast<Look*>(mem);
    const float* zero = guest::ptr<float>(0x306900);
    self->state = 0;
    self->step = 0.0436333f;        // 2.5°
    Copy4(self->target, zero);
    Copy4(self->rotation, zero);
    self->rotation[3] = 0.0f;
    return self;
}

// ── 0x201790 ──────────────────────────────────────────────────────────────
s16 CharaLook_State(void* look) { return static_cast<Look*>(look)->state; }

// ── 0x201E10 ──────────────────────────────────────────────────────────────
void CharaLook_Begin(void* look, Skeleton* skeleton) {
    Look* self = static_cast<Look*>(look);
    self->state = 1;
    Copy4(self->rotation, BonesOf(skeleton)[kHead].rotation);
}

// ── 0x201E30 ──────────────────────────────────────────────────────────────
void CharaLook_SetTarget(void* look, const float* point) {
    Copy4(static_cast<Look*>(look)->target, point);
}

// ── 0x201DF0 ──────────────────────────────────────────────────────────────
void CharaLook_End(void* look) {
    Look* self = static_cast<Look*>(look);
    self->state = 2;
    self->timer = 20;
}

// ── 0x2017A0 ──────────────────────────────────────────────────────────────
void CharaLook_Apply(void* look, Skeleton* skeleton, const float* matrix) {
    Look* self = static_cast<Look*>(look);
    if (self->state == 0)
        return;
    Bone* bones = BonesOf(skeleton);
    Bone* head = &bones[kHead];

    if (self->state == 2) {
        // Coming back: the target is straight ahead, and the head's rotation
        // is eased toward the one the motion gives it.
        if (self->timer == 20)
            Copy4(self->rotation, head->rotation);
        for (int i = 0; i < 4; i++)
            self->target[i] = head->world[12 + i] + matrix[8 + i];
        const float t = self->timer == 0 ? 0.0f : (float)self->timer / 20.0f;
        float q[4];
        Copy4(q, head->rotation);
        Quat_Lerp(t, q, self->rotation);
        Copy4(head->rotation, q);
        Copy4(self->rotation, q);
        if (--self->timer < 1) {
            if (Dot3(&bones[kChest].world[8], &head->world[8]) < 0.98f) {
                self->state = 2;
                self->timer = 20;
            } else {
                self->timer = 0;
                self->state = 0;
            }
        }
    }

    const float kFront[4] = {0.0f, -1.0f, 0.0f, 0.0f};      // 0x2D9630: the way a head bone faces
    const float kSide[4] = {0.0f, 0.0f, 1.0f, 0.0f};        // 0x2D9640

    float want[4];
    Sub4(want, self->target, &head->world[12]);
    const float flat = sqrtf(want[0] * want[0] + want[2] * want[2]);
    if (flat >= 0.0001f && want[1] / flat <= -0.5f) {
        // Not steeply down: flatten the direction out.
        float level[4];
        Copy4(level, want);
        level[1] = 0.0f;
        Normalize3(level, level);
        want[0] = level[0] * -want[1] * 2.0f;
        want[2] = level[2] * -want[1] * 2.0f;
    }
    Normalize3(want, want);

    float neck[4], front[4], q[4], inv[4];
    FromMatrix(neck, guest::ptr<Bone>(head->parent)->world);
    Quat_Normalize(neck);
    Rotate(front, neck, kFront);
    // No further than 27.5° (as a half angle) from the way the neck faces,
    // and less the further behind the target is.
    Between(q, front, want);
    const float angle = acosf(q[3]);
    float limit = 0.47996554f;
    if (angle >= 0.7853982f) {
        limit = 1.5707964f - angle;
        if (limit >= 0.47996554f)
            limit = 0.47996554f;
    }
    if (limit < angle)
        Limit(q, limit);
    Rotate(q, q, front);
    Normalize3(q, q);
    Copy4(want, q);

    float neckSide[4], now[4], side[4], turn[4];
    Rotate(neckSide, neck, kSide);
    Copy4(now, neck);
    Quat_Mul(now, self->rotation);          // the head as it was left, in the world
    Rotate(side, now, kSide);
    Between(turn, side, neckSide);          // level with the neck first…
    Copy4(q, now);
    Quat_MulLeft(q, turn);
    Rotate(front, q, kFront);
    Between(q, front, want);                // …then round to the target
    Quat_Mul(turn, q);
    if (self->step < acosf(turn[3]))
        Limit(turn, self->step);

    ConjugateTo(inv, now);
    Quat_Mul(inv, turn);
    Quat_Mul(inv, now);
    Quat_MulLeft(inv, self->rotation);
    Quat_Normalize(inv);
    Copy4(head->rotation, inv);
    Copy4(self->rotation, inv);
    Quat_ToMatrix(inv, head->local);
    Skeleton_UpdateWorld(skeleton, matrix);
}

// ── 0x200ED0 ──────────────────────────────────────────────────────────────
// A foot over ground higher than where the character stands is put on it.
void CharaLook_Feet(void*, Skeleton* skeleton, void* map, const float* matrix) {
    static const int kFeet[2] = {kLeftFoot, kRightFoot};    // 0x2EDFC0
    Bone* bones = BonesOf(skeleton);
    const float top = matrix[13] + 0.5f;
    for (int index : kFeet) {
        const float* at = &bones[index].world[12];
        const ColCellType* cell = Map_CellAt(static_cast<MapCollision*>(map), at);
        if (cell == nullptr || (cell->flags & 1) == 0)
            continue;
        float ground = cell->height;
        if (ground > top) {
            if (ground > top + 0.22f)
                continue;
            ground = top;
        }
        if (matrix[13] < ground) {
            float target[4];
            Copy4(target, at);
            target[1] = ground + 0.1f;
            PlaceFoot(skeleton, index, target);
        }
    }
}

}  // extern "C"
