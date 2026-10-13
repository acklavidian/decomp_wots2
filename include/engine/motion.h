#pragma once
#include "hal/ps2_types.h"

// Skeletons and motion sampling — decompiled from 0x136230–0x136BF0,
// 0x138A00–0x138D70 and 0x1391C0–0x1395F0.  The motion file layout is in
// the project notes ("Motion packs").

// One bone of a model or skeleton (0xA0 bytes).  Matrices are 4×4 floats in
// the row-vector convention.
struct Bone {
    float world[16];    // +0x00  model space
    float local[16];    // +0x40  relative to the parent
    u32   parent;       // +0x80  Bone*, 0 for the root (an index in the file)
    u32   flags;        // +0x84
    float _88[2];
    float rotation[4];  // +0x90  quaternion
};

// A character's own copy of its model's bones.
struct Skeleton {
    s32 count;
    u32 bones;          // Bone[]
};

// Rotation key: quaternion in 4.12 fixed point; `span` is the distance to the
// next key in frames; mode 0 interpolates linearly, anything else spherically.
struct RotationKey {
    s16 q[4];
    u8  _08[8];
    s16 frame, span, mode, _16;
};

// Position key: Hermite, values and tangents in 4.12 fixed point.
struct PositionKey {
    s16 p[3];
    s16 frame;
    s16 tangent[3];
    s16 span;
};

// Plays one motion on a skeleton (0x170 bytes; 0x1376E0–0x138870).  A motion
// is {s16 ?, boneCount, loopStart (−1 = plays once), frameCount, tracks…}:
// track 0 the distance travelled, 1 the root bone's position, 2 how far the
// character has turned, 3 + i the rotation of bone i.
struct MotionPlayer {
    float frame;            // +0x00
    float speed;            // +0x04  frames per step
    float lastFrame;        // +0x08  the frame the pose is of
    u32   motion;           // +0x0C  u32* → the motion
    u32   previous;         // +0x10
    u32   _14, _18, _1c;
    s16   firstBone, lastBone;  // +0x20  the bones it poses
    float blend;            // +0x24  frames left of easing in from the old pose
    u32   flags;            // +0x28  MotionFlag
    s32   travelHint, rootHint, turnHint;   // +0x2C  key indices from the last sample
    s32   boneHint[0x40];   // +0x38
    u8    _138[8];
    float travel[4];        // +0x140 track 0 at the last sample
    float turn[4];          // +0x150 track 2 at the last sample
    s16   loops;            // +0x160 times it has wrapped round
    u32   vtable;           // +0x164 0x2EA9C0: +0x08 destroy, +0x0C step
    u8    _168[8];
};
enum MotionFlag : u32 { kMotionAdvance = 1, kMotionLoops = 2, kMotionRoot = 4 };
enum : u32 { kVtMotionPlayer = 0x2EA9C0u };

extern "C" {
    float* Quat_Mul(float q[4], const float p[4]);                          // 0x1362C0  q = q ⊗ p
    float* Quat_MulLeft(float q[4], const float p[4]);                      // 0x136250  q = p ⊗ q
    float* Quat_Conjugate(float q[4]);                                      // 0x136330
    float* Quat_Turn(float angle, float q[4], const float axis[4]);         // 0x136680

    MotionPlayer* MotionPlayer_Ctor(MotionPlayer* self);                    // 0x1376E0
    MotionPlayer* MotionPlayer_Dtor(MotionPlayer* self, s16 flag);          // 0x1377F0
    // → the new frame, −1 at the end of a motion that does not loop, −2 with no motion.
    float MotionPlayer_Step(MotionPlayer* self, Skeleton* skeleton, float* travelled, int pose);   // 0x137860
    void  MotionPlayer_Apply(MotionPlayer* self, Skeleton* skeleton, float* travelled, int pose);  // 0x137F10
    void  MotionPlayer_SampleTurn(MotionPlayer* self);                      // 0x137C80
    void  MotionPlayer_SampleTravel(MotionPlayer* self);                    // 0x137D70
    void  MotionPlayer_ResetTravel(MotionPlayer* self);                     // 0x137E70
    u32   MotionPlayer_SetFlags(MotionPlayer* self, u32 flags);             // 0x1383C0  → the old flags
    u32   MotionPlayer_AddFlags(MotionPlayer* self, u32 flags);             // 0x138400
    u32   MotionPlayer_ClearFlags(MotionPlayer* self, u32 flags);           // 0x138440
    // Starts `motion` (a u32* → motion) at frame `start`, easing in over `blend` frames (< 0: as set).
    void  MotionPlayer_Play(float start, float blend, MotionPlayer* self, Skeleton* skeleton,
                            float* matrix, u32 motion);                     // 0x138490
    void  MotionPlayer_SetFrame(float frame, MotionPlayer* self, int keepTravel);   // 0x138600
    void  MotionPlayer_SetRange(MotionPlayer* self, s16 first, s16 last);   // 0x1386C0
    float MotionPlayer_SetBlend(float frames, MotionPlayer* self);          // 0x1387C0
    s16   MotionPlayer_LoopStart(MotionPlayer* self);                       // 0x138800
    void  MotionPlayer_BakeTurn(MotionPlayer* self, Skeleton* skeleton, float* matrix);    // 0x138870

    void  Vec4_FromFixed12(float out[4], const s16 in[4]);                  // 0x136AE0
    float Vec4_Dot(const float a[4], const float b[4]);                     // 0x136B70
    float* Quat_Normalize(float q[4]);                                      // 0x136370
    // Both leave the result in `q`.
    float* Quat_Lerp(float t, float q[4], const float to[4]);               // 0x1369D0
    float* Quat_Slerp(float t, float q[4], const float to[4]);              // 0x136890
    void  Quat_ToMatrix(const float q[4], float m[16]);                     // 0x1365F0

    // Sample a track at `frame`.  `hint` is the key index the previous call
    // returned (1 to start); the new one is returned.
    int Motion_SampleRotation(float frame, void* player, float out[4], const u8* track, int hint);  // 0x138A00
    int Motion_SamplePosition(float frame, void* player, float out[4], const u8* track, int hint);  // 0x138D70
    // Turns a motion's track offsets into addresses.
    void Motion_Relocate(u8* motion);                                       // 0x1391C0

    Skeleton* Skeleton_Ctor(Skeleton* self, int count, const Bone* source); // 0x139250
    void Skeleton_UpdateWorld(Skeleton* self, const float* root);           // 0x1393A0
    // The swing of hair and cloth: moves the tips and turns their bones after them.
    void Skeleton_Swing(Skeleton* self, const float* wind);                 // 0x139630
    void Skeleton_Update(Skeleton* self, const float* root);                // 0x1394B0
}
