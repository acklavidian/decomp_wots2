// The face: eyes that blink, a mouth that makes vowel shapes, expressions —
// and the reader of the lip data story scripts give with each spoken line.
//
// Decompiled from 0x26A2F0–0x26A900 and 0x26B600–0x26BF40 (the part at
// Chara+0x190 that older notes and the function names call the "voice").
// See "Misnamed parts of the character" in the project notes.
//
// Seven bones of the head belong to it (21–27: eyelids, the corners of the
// mouth, the jaw; motions leave them alone).  Three layers each ease those
// bones toward one of a few poses: the mouth (six shapes: closed, a, i, u, e,
// o), the expression (six) and the eyes (open, half, shut).  The poses are
// the rest pose with a few numbers changed, built when the model is in.

#include "engine/motion.h"
#include "game/chara.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" {
    void* MemAlloc(u32 size);                               // 0x100820
    void  MemFree(void* p);                                 // 0x100650
    int   Rand_Next();                                      // 0x1200F0
}

namespace {

template <typename T = void> inline T* P(u32 a) { return guest::ptr<T>(a); }
s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);
const s32* const kFaceBones = guest::ptr<s32>(0x2DAFD0u);  // 21 … 27
alignas(16) const float kAxisX[4] = {1, 0, 0, 0};          // 0x2D1A80
alignas(16) const float kAxisY[4] = {0, 1, 0, 0};          // 0x2D1A90

struct FaceBone {           // 0x30
    float pos[4];
    float quat[4];
    u32 flag;               // 1: this pose says where the bone is
    u32 _24[3];
};
struct FaceLayer {          // 0x1C
    s32 pose;               // +0x00  which pose it is easing toward
    s32 mode;               // +0x04  2: by itself (the mouth chatters, the eyes blink)
    s32 timer;              // +0x08
    float blend;            // +0x0C  share of the way a frame
    u32 vtable;             // +0x10  +0x08 tick, +0x0C dtor, +0x10 init, +0x14 apply, +0x18 set mode
    u32 poses;              // +0x14  FaceBone[count][7]
    s32 count;              // +0x18
};
struct Face {               // 0x1D0
    u32 chara;              // +0x000
    s32 state;              // +0x004  2 = set up
    FaceLayer mouth;        // +0x008
    FaceLayer expr;         // +0x024
    FaceLayer eyes;         // +0x040
    s32 eyeRest;            // +0x05C  the eyes' pose between blinks
    FaceBone bones[7];      // +0x060
    u32 lips;               // +0x1B0  the lip data being read (a string object in the game; here a copy)
    u32 _1b4[3];
    s32 lipPos;             // +0x1C0
    s32 wait;               // +0x1C4  the frame the next event is due at
    s32 frame;              // +0x1C8
    u32 _1cc;
};
static_assert(sizeof(FaceBone) == 0x30 && sizeof(FaceLayer) == 0x1C && sizeof(Face) == 0x1D0, "Face");
static_assert(offsetof(Face, bones) == 0x60 && offsetof(Face, lips) == 0x1B0, "Face");
enum : u32 { kVtLayer = 0x2ECD70u, kVtMouth = 0x2ECD50u, kVtExpr = 0x2ECD30u, kVtEyes = 0x2ECD00u };

inline int RandRange(int low, int high) { return low + Rand_Next() % (high - low + 1); }   // 0x246090
inline float& F(u8* p) { return *reinterpret_cast<float*>(p); }
inline s32& I(u8* p) { return *reinterpret_cast<s32*>(p); }
inline u8* SkeletonBone(const Face* self, int bone) {
    const Chara* chara = P<Chara>(self->chara);
    const u32 skeleton = P<CharaParams>(chara->params)->link[1];
    return P<u8>(*P<u32>(skeleton + 4) + (u32)bone * 0xA0);
}

// ── The layers ────────────────────────────────────────────────────────────
void Mouth_Tick(FaceLayer* l) {                             // 0x26BA20: chattering
    if (l->mode == 2 && l->timer-- < 1) {
        l->timer = RandRange(1, 4);
        l->pose = RandRange(0, 5);
    }
}
void Expr_Tick(FaceLayer*) {}                               // 0x26BA10
void Eyes_Tick(FaceLayer* l) {                              // 0x26B990: blinking
    if (l->mode != 2)
        return;
    if (l->pose != 2) {
        if (l->timer-- < 1) {
            l->timer = 3;
            l->pose = 2;
        }
    } else if (l->timer-- < 1) {
        l->timer = RandRange(0x3C, 300);
        l->pose = reinterpret_cast<s32*>(l)[7];             // (the word after the layer: Face::eyeRest)
    }
}
void Layer_Set(FaceLayer* l, s32 mode, int, int) {          // 0x26BA80
    l->mode = mode;
    l->timer = 0;
}
// 0x26BB60: `count` poses, each the head as it is now.
void Layer_Init(float blend, FaceLayer* l, u32 skeleton, int count, u32 flag) {
    if (l->poses != 0)
        MemFree(P(l->poses));
    l->count = count;
    FaceBone* poses = static_cast<FaceBone*>(MemAlloc((u32)count * 0x150 + 0x10));
    l->poses = guest::addr(poses);
    l->pose = 0;
    for (int p = 0; p < count; p++)
        for (int i = 0; i < 7; i++) {
            const u8* bone = P<u8>(*P<u32>(skeleton + 4) + (u32)kFaceBones[i] * 0xA0);
            FaceBone& b = poses[p * 7 + i];
            memcpy(b.quat, bone + 0x90, 16);
            memcpy(b.pos, bone + 0x70, 16);
            b.flag = flag;
        }
    l->blend = blend;
}
// 0x26BA90: one frame toward the layer's pose.
void Layer_Apply(FaceLayer* l, FaceBone* bones) {
    VirtualCall<void, FaceLayer*>(l->vtable, 0x08, l);
    const FaceBone* pose = P<FaceBone>(l->poses) + l->pose * 7;
    for (int i = 0; i < 7; i++) {
        if (pose[i].flag & 1) {
            Quat_Slerp(l->blend, bones[i].quat, pose[i].quat);
            for (int k = 0; k < 4; k++)
                bones[i].pos[k] = pose[i].pos[k] * l->blend + bones[i].pos[k] * (1.0f - l->blend);
        }
        bones[i].flag = pose[i].flag;
    }
}
void* Layer_Dtor(FaceLayer* l, s16) {                       // 0x26BD40, 0x26B8B0, 0x26B820, 0x26A1A0
    if (l != nullptr && l->poses != 0) {
        MemFree(P(l->poses));
        l->poses = 0;
    }
    return l;
}

const struct FaceBinder {
    FaceBinder() {
        const auto bind = [](u32 address, auto fn, const char* name) {
            guest::bind(address, fn, name);
        };
        bind(0x26BA20, Mouth_Tick, "Mouth_Tick");
        bind(0x26BA10, Expr_Tick, "Expr_Tick");
        bind(0x26B990, Eyes_Tick, "Eyes_Tick");
        bind(0x26BB50, Expr_Tick, "Layer_Tick");
        for (u32 dtor : {0x26BD40u, 0x26B8B0u, 0x26B820u, 0x26A1A0u})
            bind(dtor, Layer_Dtor, "Layer_Dtor");
        bind(0x26BB60, Layer_Init, "Layer_Init");
        bind(0x26BA90, Layer_Apply, "Layer_Apply");
        bind(0x26BA80, Layer_Set, "Layer_Set");
    }
} sFaceBinder;

// ── 0x26BDB0: the next thing in the lip data — a letter, or a number of
// thirtieths of a second (answered in frames) ─────────────────────────────
const s32 kLipsEnd = (s32)0x80000000;
s32 NextLip(Face* self) {
    const u16* text = P<const u16>(self->lips);
    if (self->lips == 0 || text[self->lipPos] == 0)
        return kLipsEnd;
    if (text[self->lipPos] < '0' || text[self->lipPos] > '9')
        return text[self->lipPos++];
    s32 n = 0;
    while (text[self->lipPos] >= '0' && text[self->lipPos] <= '9')
        n = n * 10 + (text[self->lipPos++] - '0');
    return n * *gFrameRate / 30;
}
// ── 0x26A2F0: the lip data is a timeline: "o14c15…" = mouth shape o until
// frame 14, closed until 15…  Letters c a i u e o are mouth shapes; f + n j a
// h p s an expression; y + o h c the eyes (y w: one blink); w: let go. ─────
void ReadLips(Face* self) {
    static const char kMouth[] = "caiueo", kExpr[] = "njahps", kEyes[] = "ohc";
    for (;;) {
        if (self->frame < self->wait) {
            self->frame++;
            return;
        }
        const s32 token = NextLip(self);
        if (token == kLipsEnd)
            return;
        if (token == 'w') {
            Layer_Set(&self->mouth, 1, 0, 0);
            Layer_Set(&self->expr, 1, 0, 0);
            self->wait = NextLip(self);
        } else if (token == 'y') {
            const s32 how = NextLip(self);
            if (how == 'w') {
                self->eyes.pose = 2;
                self->eyes.timer = 3;
            } else if (const char* at = how > 0 && how < 128 ? strchr(kEyes, (char)how) : nullptr) {
                self->eyeRest = (s32)(at - kEyes);
                self->eyes.pose = self->eyeRest;
            }
        } else if (token == 'f') {
            self->wait = 0;
            const s32 how = NextLip(self);
            if (const char* at = how > 0 && how < 128 ? strchr(kExpr, (char)how) : nullptr) {
                Layer_Set(&self->expr, 1, 0, 0);
                self->expr.pose = (s32)(at - kExpr);
                if (*P<s32>(0x2EE4A0u) & 1) {               // (a blink with every other change)
                    self->eyes.pose = 2;
                    self->eyes.timer = 3;
                }
            }
        } else if (const char* at = token > 0 && token < 128 ? strchr(kMouth, (char)token) : nullptr) {
            Layer_Set(&self->mouth, 1, 0, 0);
            self->mouth.pose = (s32)(at - kMouth);
            self->wait = NextLip(self);
        }
    }
}

}  // namespace

extern "C" {

// ── 0x26B600 ──────────────────────────────────────────────────────────────
void* CharaVoice_Ctor(void* mem, u32 chara) {
    Face* self = static_cast<Face*>(mem);
    memset(self, 0, sizeof *self);
    self->mouth.vtable = kVtMouth;
    self->expr.vtable = kVtExpr;
    self->eyes.vtable = kVtEyes;
    self->mouth.blend = self->expr.blend = self->eyes.blend = 0.5f;
    Layer_Set(&self->eyes, 2, 0, 0);                        // the eyes blink by themselves
    self->eyeRest = 0;
    self->eyes.pose = 0;
    self->state = 1;
    self->chara = chara;
    return self;
}
// 0x26B420
void CharaVoice_Dtor(void* mem) {
    Face* self = static_cast<Face*>(mem);
    for (FaceLayer* l : {&self->mouth, &self->expr, &self->eyes})
        Layer_Dtor(l, 0);
    if (self->lips != 0)
        guest::free(P(self->lips));
    self->lips = 0;
}

// ── 0x26A900: builds the poses once the model (its skeleton) is in ────────
void CharaVoice_Refresh(void* mem) {
    Face* self = static_cast<Face*>(mem);
    if (self == nullptr)
        return;
    const Chara* chara = P<Chara>(self->chara);
    const u32 skeleton = P<CharaParams>(chara->params)->link[1];
    if (skeleton == 0 || *P<s32>(skeleton) < 28)
        return;
    // How far the eyelids come down (two angles of the character, degrees).
    const float lid1 = *reinterpret_cast<const float*>(reinterpret_cast<const u8*>(chara) + 0xC4);
    const float lid2 = *reinterpret_cast<const float*>(reinterpret_cast<const u8*>(chara) + 0xC8);
    u8* p = nullptr;
    Layer_Init(0.30f, &self->mouth, skeleton, 6, 0);
    p = P<u8>(self->mouth.poses);
    I(p + 0x140) = 1;
    F(p + 0x274) += 0.001f;
    F(p + 0x280) += 0.13f;
    Quat_Normalize(&F(p + 0x280));
    I(p + 0x290) = 1;
    F(p + 0x3d0) += 0.05f;
    Quat_Normalize(&F(p + 0x3d0));
    I(p + 0x3e0) = 1;
    F(p + 0x514) += 0.0005f;
    F(p + 0x520) += 0.08f;
    Quat_Normalize(&F(p + 0x520));
    I(p + 0x530) = 1;
    F(p + 0x670) += 0.1f;
    Quat_Normalize(&F(p + 0x670));
    I(p + 0x680) = 1;
    F(p + 0x7b4) += 0.001f;
    F(p + 0x7c0) += 0.15f;
    Quat_Normalize(&F(p + 0x7c0));
    I(p + 0x7d0) = 1;
    Layer_Init(0.30f, &self->expr, skeleton, 6, 0);
    p = P<u8>(self->expr.poses);
    I(p + 0x80) = 1;
    I(p + 0xb0) = 1;
    I(p + 0x20) = 1;
    I(p + 0x50) = 1;
    I(p + 0xe0) = 1;
    I(p + 0x110) = 1;
    F(p + 0x1b8) += 0.003f;
    I(p + 0x1d0) = 1;
    F(p + 0x1e8) += 0.003f;
    I(p + 0x200) = 1;
    F(p + 0x158) += 0.004f;
    I(p + 0x170) = 1;
    F(p + 0x188) += 0.004f;
    I(p + 0x1a0) = 1;
    Quat_Turn(-0.043633f, &F(p + 0x220), kAxisY);
    F(p + 0x218) += 0.003f;
    F(p + 0x210) += 0.002f;
    I(p + 0x230) = 1;
    Quat_Turn(0.043633f, &F(p + 0x250), kAxisY);
    F(p + 0x248) += 0.003f;
    F(p + 0x240) -= 0.002f;
    I(p + 0x260) = 1;
    Quat_Turn(-0.026180f, &F(p + 0x310), kAxisY);
    Quat_Turn(0.043633f, &F(p + 0x310), kAxisX);
    F(p + 0x308) -= 0.003f;
    I(p + 0x320) = 1;
    Quat_Turn(0.026180f, &F(p + 0x340), kAxisY);
    Quat_Turn(0.043633f, &F(p + 0x340), kAxisX);
    F(p + 0x338) -= 0.003f;
    I(p + 0x350) = 1;
    F(p + 0x2a8) -= 0.008f;
    I(p + 0x2c0) = 1;
    F(p + 0x2d8) -= 0.008f;
    I(p + 0x2f0) = 1;
    Quat_Turn(0.043633f, &F(p + 0x370), kAxisY);
    F(p + 0x368) -= 0.002f;
    F(p + 0x360) -= 0.002f;
    I(p + 0x380) = 1;
    Quat_Turn(-0.043633f, &F(p + 0x3a0), kAxisY);
    F(p + 0x398) -= 0.002f;
    F(p + 0x390) += 0.002f;
    I(p + 0x3b0) = 1;
    Quat_Turn(0.061087f, &F(p + 0x460), kAxisY);
    F(p + 0x458) -= 0.003f;
    I(p + 0x470) = 1;
    Quat_Turn(-0.061087f, &F(p + 0x490), kAxisY);
    F(p + 0x488) -= 0.003f;
    I(p + 0x4a0) = 1;
    F(p + 0x3f8) -= 0.007f;
    I(p + 0x410) = 1;
    F(p + 0x428) -= 0.007f;
    I(p + 0x440) = 1;
    Quat_Turn(0.130900f, &F(p + 0x4c0), kAxisY);
    F(p + 0x4b8) -= 0.004f;
    F(p + 0x4b0) -= 0.002f;
    I(p + 0x4d0) = 1;
    Quat_Turn(-0.130900f, &F(p + 0x4f0), kAxisY);
    F(p + 0x4e8) -= 0.004f;
    F(p + 0x4e0) += 0.002f;
    I(p + 0x500) = 1;
    F(p + 0x5a8) += 0.005f;
    I(p + 0x5c0) = 1;
    F(p + 0x5d8) += 0.005f;
    I(p + 0x5f0) = 1;
    F(p + 0x548) += 0.01f;
    I(p + 0x560) = 1;
    F(p + 0x578) += 0.01f;
    I(p + 0x590) = 1;
    Quat_Turn(-0.087266f, &F(p + 0x610), kAxisY);
    F(p + 0x608) += 0.004f;
    F(p + 0x600) += 0.003f;
    I(p + 0x620) = 1;
    Quat_Turn(0.087266f, &F(p + 0x640), kAxisY);
    F(p + 0x638) += 0.004f;
    F(p + 0x630) -= 0.003f;
    I(p + 0x650) = 1;
    F(p + 0x6f8) += 0.005f;
    I(p + 0x710) = 1;
    F(p + 0x728) += 0.005f;
    I(p + 0x740) = 1;
    F(p + 0x698) += 0.015f;
    I(p + 0x6b0) = 1;
    F(p + 0x6c8) += 0.015f;
    I(p + 0x6e0) = 1;
    Quat_Turn(-0.087266f, &F(p + 0x760), kAxisY);
    F(p + 0x758) -= 0.002f;
    F(p + 0x750) -= 0.002f;
    I(p + 0x770) = 1;
    Quat_Turn(0.087266f, &F(p + 0x790), kAxisY);
    F(p + 0x788) -= 0.002f;
    F(p + 0x780) += 0.002f;
    I(p + 0x7a0) = 1;
    Layer_Init(0.60f, &self->eyes, skeleton, 3, 0);
    p = P<u8>(self->eyes.poses);
    I(p + 0x20) = 1;
    I(p + 0x50) = 1;
    Quat_Turn(lid1 * 0.5f * 0.5f * 0.017453292f, &F(p + 0x160), kAxisX);
    I(p + 0x170) = 1;
    Quat_Turn(lid2 * 0.5f * 0.5f * 0.017453292f, &F(p + 0x190), kAxisX);
    I(p + 0x1a0) = 1;
    Quat_Turn(lid1 * 0.5f * 0.017453292f, &F(p + 0x2b0), kAxisX);
    I(p + 0x2c0) = 1;
    Quat_Turn(lid2 * 0.5f * 0.017453292f, &F(p + 0x2e0), kAxisX);
    I(p + 0x2f0) = 1;

    for (int i = 0; i < 7; i++) {
        const u8* bone = SkeletonBone(self, kFaceBones[i]);
        memcpy(self->bones[i].quat, bone + 0x90, 16);
        memcpy(self->bones[i].pos, bone + 0x70, 16);
        self->bones[i].flag = 0;
    }
    self->state = 2;
    self->wait = 0;
    self->frame = 0;
}

// ── 0x26A7F0: once a frame, before the skeleton is worked out ─────────────
void CharaVoice_Update(void* mem) {
    Face* self = static_cast<Face*>(mem);
    if (self == nullptr || self->state != 2)
        return;
    ReadLips(self);
    for (FaceLayer* l : {&self->mouth, &self->expr, &self->eyes})
        VirtualCall<void, FaceLayer*, FaceBone*>(l->vtable, 0x14, l, self->bones);
    for (int i = 0; i < 7; i++) {
        u8* bone = SkeletonBone(self, kFaceBones[i]);
        memcpy(bone + 0x90, self->bones[i].quat, 16);
        Quat_ToMatrix(self->bones[i].quat, reinterpret_cast<float*>(bone + 0x40));
        memcpy(bone + 0x70, self->bones[i].pos, 16);
    }
    // Debug: WOTS2_TRACE_FACE=id follows one character's face.
    static const char* trace = getenv("WOTS2_TRACE_FACE");
    if (trace != nullptr && (P<Chara>(self->chara)->id == atoi(trace) || (trace[0] == 'm' && self->mouth.pose != 0))) {
        const Chara* c = P<Chara>(self->chara);
        printf("[face] %d: lids %.1f %.1f  mouth %d/%d  expr %d  eyes %d/%d t%d  lid quat %.3f %.3f %.3f %.3f\n",
               c->id, c->listParam[0], c->listParam[1], self->mouth.pose, self->mouth.mode, self->expr.pose,
               self->eyes.pose, self->eyes.mode, self->eyes.timer, self->bones[0].quat[0],
               self->bones[0].quat[1], self->bones[0].quat[2], self->bones[0].quat[3]);
    }
}

// ── 0x26A7C0: lip data to follow from now on (a spoken line begins) ───────
void CharaVoice_fn26a7c0(void* mem, u32 text, int, int) {
    Face* self = static_cast<Face*>(mem);
    if (self == nullptr)
        return;
    if (self->lips != 0)
        guest::free(P(self->lips));
    self->lips = 0;
    if (text != 0) {
        const u16* from = P<const u16>(text);
        size_t n = 0;
        while (from[n] != 0)
            n++;
        u16* copy = static_cast<u16*>(guest::alloc((u32)(n + 1) * 2));
        memcpy(copy, from, (n + 1) * 2);
        self->lips = guest::addr(copy);
    }
    self->lipPos = 0;
    self->wait = 0;
    self->frame = 0;
}

}  // extern "C"
