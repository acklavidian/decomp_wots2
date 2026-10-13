// The render manager's world effects (0x14E920–0x14FC00, the half of it
// that is decompiled): what is drawn into the world on top of the map and
// the characters — shadows, and (to come) blood, dust and sparks.
//
// The manager (0x70 bytes, *0x2EE684 and *0x2EE520) keeps two lists:
//   +0x24  world effects, made by RenderMgr_TriggerEffect for types >= 14
//          (0x14ECE0), stepped by RenderMgr_SetMap once a frame and drawn by
//          RenderMgr_Draw with the depth test on
//   +0x30  screen effects (types below 14: letterbox, fade, text) — still
//          the stand-in in src/native/hle.cpp
// and the effect sprites (+0x3C, Common/Effect/Effect.BPM, also *0x2EE51C)
// and particle orders (+0x40, Common/Effect/Effect.ord).
//
// An effect starts {type, _, vtable}: +0x08 destroy(flag), +0x0C start(a, b),
// +0x10 update → 0 = finished, +0x14 draw.  An effect leaves by having its
// type set to 0 (0x172090).
//
// Not reproduced: effects come from the manager's own 128 KB arena (its
// allocator object at +0x04); here they are ordinary heap blocks.

#include "engine/containers.h"
#include "game/chara.h"
#include "game/chara_resources.h"
#include "game/map_collision.h"
#include "hal/ps2_types.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

extern "C" void Lights_Add(const float* pos, const float* colour, u32 flags, s32 frames);   // 0x165FD0 + 0x166B80 (point_lights.cpp)

struct SpriteSet;
extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    SpriteSet* SpriteSet_Ctor(void* mem);                               // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    int   SpriteSet_Bind(SpriteSet* set);                               // 0x17BFD0
    void* SpriteSet_GetSprite(SpriteSet* set, int index);               // 0x17BDF0
    void* AssetRef_GetData(void* ref);                                  // 0x174A00
    void  GsDevice_DrawTexturedPrim(void* dev, const void* packet, int count);  // 0x12FAF0
    void  GsDevice_SetBlend(void* dev, u64 alpha);                      // 0x130C50
    void  GsDevice_WriteRegister(void* dev, u32 reg, u64 value);        // 0x130940
    int   GsDevice_Project(void* dev, s32* out, const float* world);    // 0x12F7E0
    void  GsDevice_DrawPrim(void* dev, const void* packet, int count);  // 0x12FC30
    int   SpriteSet_DrawEx(void* set, int index, void* params);         // 0x17ADF0
    void  SpriteSet_DrawRotated(void* set, int index, const void* params);     // 0x179A40
    int   Rand_Next();                                                  // 0x1200F0
    float Sound_ApplyPan(float nearDist, float farDist, int* left, int* right, int positional);  // 0x180C70
    void  SeDriver_Play(void* driver, int id, u32 pitch, int volumeL, int volumeR);             // 0x15B290
    u32   Screen_X(int v, int align);                                   // 0x12E5E0
    u32   Screen_Y(int v, int align);                                   // 0x12E550
    void  Matrix_Identity(void* m);                                     // 0x107428
    void  Matrix_Multiply(void* dst, const void* a, const void* b);     // 0x107130
    void  CamList_Shake(float amount, void* list, int frames);          // 0x1503A0
    void* ArchiveFile_Create();
    void  ArchiveFile_Open(void* file, int assetId);                    // 0x142AA0
    u32   ArchiveFile_GetSize(void* file);
    void  ArchiveFile_Read(void* file, void* dst, u32 size);
    // 0x172160: the view's rotation turned back (camera → world) at 0x309E50,
    // which is what turns a flat sprite to face the viewer (native camera).
    void  Fx_PrepareView();
    // Native: draws the model file `path` at a world matrix.
    void  hle_object_draw_with(const char* path, const float* matrix, const char* textures);
    void* RenderMgr_AddWorldEffect(void* mgr, int type, u32 a, u32 b);
    void  Sound_PlaySE(int id, int positional, int volume);              // 0x180E70
    int   RenderMgr_AddEnvSound(void* mgr, int id, int volume, int interval);  // 0x14EA50
    int   RenderMgr_RemoveEnvSound(void* mgr, int handle);                      // 0x14E920
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<void> gGsDevice{0x2EE670u};
u32* const gEffectSprites = guest::ptr<u32>(0x2EE51Cu);
u32* const gEffectMgr     = guest::ptr<u32>(0x2EE520u);
u32* const gEffectMap     = guest::ptr<u32>(0x2EE524u);
s32* const gFxBlend       = guest::ptr<s32>(0x2EE5BCu);

struct RenderMgr {
    u8  _00[0x24];
    GuestList world;        // +0x24
    GuestList screen;       // +0x30  (not used by the stand-in)
    u32 sprites;            // +0x3C
    u32 orders;             // +0x40  {asset ref, 1, count, records}
    u8  _44[0x0C];
    float flash[4];         // +0x50
    u32 boxModels[2];       // +0x60
    u32 _68, _6c;
};

struct Effect {
    s32 type;       // +0x00
    s32 life;       // +0x04  frames left, for the classes that count
    u32 vtable;     // +0x08
};

inline void FxDestroy(Effect* e) { VirtualCall<void*, Effect*, int>(e->vtable, 0x08, e, -1); }
inline int  FxStart(Effect* e, u32 a, u32 b) { return VirtualCall<int, Effect*, u32, u32>(e->vtable, 0x0C, e, a, b); }
inline int  FxUpdate(Effect* e) { return VirtualCall<int, Effect*>(e->vtable, 0x10, e); }
inline int  FxDraw(Effect* e) { return VirtualCall<int, Effect*>(e->vtable, 0x14, e); }

// ── 0x1720C0 / 0x1720A0: the effects' blend mode, remembered ──────────────
void Fx_SetBlend(int mode) {
    if (*gFxBlend == mode)
        return;
    *gFxBlend = mode;
    if (mode == 2)
        GsDevice_SetBlend(*gGsDevice, 0x8000000044ull);
    else if (mode == 1)
        GsDevice_SetBlend(*gGsDevice, 0x42);
    else if (mode == 0)
        GsDevice_SetBlend(*gGsDevice, 0x8000000048ull);
}
void Fx_ResetBlend() {
    *gFxBlend = 2;
    GsDevice_SetBlend(*gGsDevice, 0x8000000044ull);
}

// ── 0x1721C0 / 0x14F860: the base class (vtable 0x2EAC20) ─────────────────
Effect* Effect_Ctor(Effect* self) {
    self->vtable = 0x2EAC20u;
    self->type = 1;
    return self;
}
void* Effect_Dtor(Effect* self, int flag) {
    if (self != nullptr) {
        self->vtable = 0x2EAC20u;
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}
void Effect_Nothing(Effect*) {}             // 0x14F8B0
GUEST_BIND(0x14F860, Effect_Dtor);
GUEST_BIND(0x14F8B0, Effect_Nothing);

// ── 0x17BED0: a sprite of a set by its eight-character name ───────────────
int SpriteSet_FindByName(u32 set, const char* name) {
    u32* handle = P<u32>(set);
    if (handle[3] == 0) {
        if (handle[1] == 0)
            return -1;
        const s32* file = static_cast<const s32*>(AssetRef_GetData(P(handle[1])));
        if (file == nullptr)
            return -1;
        handle[4] = (u32)file[1];
        handle[3] = guest::addr(file + 4);
    }
    const u8* record = P<u8>(handle[3]);
    for (s32 i = 0; i < (s32)handle[4]; i++, record += 0x20)
        if (memcmp(record, name, 8) == 0)
            return i;
    return -1;
}

// ── 0x17AA80: a sprite stretched over four (or three) projected points ────
struct QuadParams {
    u32 flags;              // 1 keep inside the texels' centres, 2 a triangle
    u32 _04[3];
    s32 at[4][4];           // +0x10  what GsDevice_Project gave: x, y (1/16 px), z × 16, w
    u32 rgba;               // +0x50
};
int SpriteSet_DrawQuad(u32 set, int index, const QuadParams* q) {
    const u32* handle = P<u32>(set);
    u32 desc = handle[2];
    if (desc == 0 || handle[3] == 0 || index < 0 || (s32)handle[4] < index)
        return 0;
    const u8* sprite = P<u8>(handle[3] + (u32)index * 0x20);
    if (sprite[0x19] != 0)
        desc += (u32)sprite[0x19] * 0x30;
    const u8* d = P<u8>(desc);
    const auto u16at = [](const u8* p) { u16 v; memcpy(&v, p, 2); return v; };
    const u64 tex0 = (u64)u16at(d + 10) << 30 | (u64)u16at(d + 8) << 26 | (u64)d[2] << 20 | (u64)u16at(d + 4)
                   | (u64)u16at(d + 6) << 14 | 0x400000000ull
                   | (u64)(s64)(s32)((u32)u16at(d + 0x1E) + (u32)sprite[0x18]) << 37 | 0x2000000000000000ull;
    const s16 lo = (q->flags & 1) ? 8 : 0, hi = (q->flags & 1) ? -0x10 : 0;
    const s16 u0 = (s16)(u16at(sprite + 8) * 0x10 + lo), v0 = (s16)(u16at(sprite + 10) * 0x10 + lo);
    const s16 u1 = (s16)((u16at(sprite + 8) + u16at(sprite + 0xC)) * 0x10 + hi);
    const s16 v1 = (s16)((u16at(sprite + 10) + u16at(sprite + 0xE)) * 0x10 + hi);
    const s16 us[4] = {u0, u0, u1, u1}, vs[4] = {v0, v1, v0, v1};

    struct Vertex { s16 u, v; u32 _04; u32 rgba; u32 q; u16 x, y; u32 z; };
    struct { u64 tex0, prim; Vertex v[4]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.tex0 = tex0;
    packet.prim = 0x15C;
    const int count = (q->flags & 2) ? 3 : 4;
    for (int i = 0; i < count; i++) {
        Vertex& v = packet.v[i];
        v.u = us[i];
        v.v = vs[i];
        v.rgba = q->rgba;
        v.x = (u16)(q->at[i][0] & 0xFFF0);
        v.y = (u16)(q->at[i][1] & 0xFFF0);
        v.z = (u32)(q->at[i][2] >> 4);
    }
    GsDevice_DrawTexturedPrim(*gGsDevice, &packet, count);
    return 1;
}

// ═══ Type 0x0F: a character's shadow (0x18 bytes, vtable 0x2EAE80) ════════
// A dark blot under the feet: a quad from one foot to the other, laid on the
// ground of the cell under each.
struct ShadowEffect {
    Effect base;
    u32 params;         // +0x0C  the character's CharaParams
    s32 off;            // +0x10
    s32 sprite;         // +0x14  "shadow  " of the effect sprites
};

// 0x1674B0
ShadowEffect* Shadow_Ctor(ShadowEffect* self) {
    Effect_Ctor(&self->base);
    self->base.vtable = 0x2EAE80u;
    self->base.type = 0x0F;
    self->off = 0;
    self->sprite = 0;
    return self;
}
// 0x167450
void* Shadow_Dtor(ShadowEffect* self, int flag) {
    if (self != nullptr) {
        self->base.vtable = 0x2EAC20u;
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}
// 0x167400
int Shadow_Start(ShadowEffect* self, u32 params, u32) {
    self->sprite = SpriteSet_FindByName(*gEffectSprites, P<const char>(*P<u32>(0x2EDA94u)));
    self->params = params;
    return 0;
}
// 0x1673E0
int Shadow_Update(ShadowEffect* self) { return self->base.type != 0; }

inline void Sub(float* o, const float* a, const float* b) { for (int i = 0; i < 4; i++) o[i] = a[i] - b[i]; }
inline void Add(float* o, const float* a, const float* b) { for (int i = 0; i < 4; i++) o[i] = a[i] + b[i]; }
inline void Normalize(float* o, const float* a) {
    const float n = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float k = n != 0.0f ? 1.0f / n : 0.0f;
    o[0] = a[0] * k; o[1] = a[1] * k; o[2] = a[2] * k; o[3] = a[3];
}
inline void Scale3(float* v, float k) { v[0] *= k; v[1] *= k; v[2] *= k; }

// 0x167160
int Shadow_Draw(ShadowEffect* self) {
    if (self->base.type == 0)
        return 0;
    if (self->off != 0)
        return 1;
    const u32 model = P<CharaParams>(self->params)->link[1];
    if (model == 0)
        return -1;
    const u32 points = P<u32>(model)[1];
    if (points == 0)
        return -1;
    alignas(16) float a[4], b[4];
    memcpy(a, P<float>(points + 0x210), 16);
    memcpy(b, P<float>(points + 0x3F0), 16);
    if (*gEffectMap != 0) {
        MapCollision* map = P<MapCollision>(*gEffectMap);
        const ColCellType* ca = Map_CellAt(map, a);
        const ColCellType* cb = Map_CellAt(map, b);
        a[1] = ca->height + 0.03f;
        b[1] = cb->height + 0.03f;
    }
    const float* up = P<float>(0x2D2010u);
    alignas(16) float along[4], reach[4], side[4];
    Sub(along, a, b);
    Normalize(reach, along);
    Scale3(reach, 0.18f);
    side[0] = along[1] * up[2] - along[2] * up[1];
    side[1] = along[2] * up[0] - along[0] * up[2];
    side[2] = along[0] * up[1] - along[1] * up[0];
    side[3] = 0.0f;
    Normalize(side, side);
    Scale3(side, 0.22f);
    Sub(b, b, reach);
    Add(a, a, reach);
    alignas(16) float corner[4][4];
    Add(corner[0], a, side);
    Add(corner[1], b, side);
    Sub(corner[2], a, side);
    Sub(corner[3], b, side);

    QuadParams quad;
    memset(&quad, 0, sizeof quad);
    quad.flags = 1;
    quad.rgba = 0x80808080u;
    int outside = 0;
    for (int i = 0; i < 4; i++)
        if (GsDevice_Project(*gGsDevice, quad.at[i], corner[i]) == 0)
            outside++;
    if (outside < 4) {
        Fx_SetBlend(1);
        SpriteSet_DrawQuad(*gEffectSprites, self->sprite, &quad);
    }
    return 1;
}
GUEST_BIND(0x167450, Shadow_Dtor);
GUEST_BIND(0x167400, Shadow_Start);
GUEST_BIND(0x1673E0, Shadow_Update);
GUEST_BIND(0x167160, Shadow_Draw);


// ── Small vector helpers (libvu0's, on plain floats) ──────────────────────
inline void Copy4(float* o, const float* a) { memcpy(o, a, 16); }
inline void ScaleXYZ(float* o, const float* a, float k) { o[0] = a[0] * k; o[1] = a[1] * k; o[2] = a[2] * k; o[3] = a[3]; }
inline void Scale4(float* o, const float* a, float k) { for (int i = 0; i < 4; i++) o[i] = a[i] * k; }
// sceVu0ApplyMatrix: a row vector through a matrix.
inline void Apply(float* o, const float* m, const float* v) {
    float r[4];
    for (int j = 0; j < 4; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(o, r, 16);
}
// 0x171F90 / 0x2215C0: a random direction.
inline void RandomUnit(float* o) {
    float v[4] = {0, 0, 0, 0};
    v[0] = (float)(Rand_Next() % 200 - 100) * 0.01f;
    v[1] = (float)(Rand_Next() % 200 - 100) * 0.01f;
    v[2] = (float)(Rand_Next() % 200 - 100) * 0.01f;
    Normalize(o, v);
}
const float* InverseView() { return P<float>(0x309E50u); }
RenderMgr* Mgr() { return P<RenderMgr>(*gEffectMgr); }
void AddToWorld(Effect* e) {
    RenderMgr* mgr = Mgr();
    GuestList_Insert(&mgr->world, GuestList_End(&mgr->world), guest::addr(e));
}
inline bool BloodOn() { return *P<s32>(*P<u32>(0x2EE68Cu) + 0x0C) != 0; }    // the Blood setting

// ── 0x17BC10: a sprite by name, with the TEX0 that selects its texture ────
const u8* Fx_Sprite(const char* name, u64* tex0) {
    const u32 set = *gEffectSprites;
    if (set == 0)
        return nullptr;
    const u32* handle = P<u32>(set);
    u32 desc = handle[2];
    if (desc == 0)
        return nullptr;
    const int index = SpriteSet_FindByName(set, name);
    if (index < 0)
        return nullptr;
    const u8* sprite = P<u8>(handle[3] + (u32)index * 0x20);
    if (sprite[0x19] != 0)
        desc += (u32)sprite[0x19] * 0x30;
    const u8* d = P<u8>(desc);
    const auto u16at = [](const u8* p) { u16 v; memcpy(&v, p, 2); return v; };
    *tex0 = (u64)u16at(d + 10) << 30 | (u64)u16at(d + 8) << 26 | (u64)d[2] << 20 | (u64)u16at(d + 4)
          | (u64)u16at(d + 6) << 14 | 0x400000000ull
          | (u64)(s64)(s32)((u32)u16at(d + 0x1E) + (u32)sprite[0x18]) << 37 | 0x2000000000000000ull;
    return sprite;
}

// A textured strip / fan of projected points: what the draw methods below
// write into the scratchpad by hand.  `corner` picks each vertex's texture
// corner: bit 0 = right edge, bit 1 = bottom edge.
void DrawProjected(u64 tex0, u32 prim, const u8* sprite, const s32 (*at)[4], const u8* corner, int count, u32 rgba) {
    const auto s16at = [](const u8* p) { s16 v; memcpy(&v, p, 2); return v; };
    const s16 u0 = (s16)(s16at(sprite + 8) * 0x10 + 8), v0 = (s16)(s16at(sprite + 10) * 0x10 + 8);
    const s16 u1 = (s16)(u0 + s16at(sprite + 0xC) * 0x10 - 0x10), v1 = (s16)(v0 + s16at(sprite + 0xE) * 0x10 - 0x10);
    struct Vertex { s16 u, v; u32 _04; u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 tex0, prim; Vertex v[10]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.tex0 = tex0;
    packet.prim = prim;
    for (int i = 0; i < count; i++) {
        Vertex& v = packet.v[i];
        v.u = (corner[i] & 1) ? u1 : u0;
        v.v = (corner[i] & 2) ? v1 : v0;
        v.rgba = rgba;
        v.q = 1.0f;
        v.x = (u16)at[i][0];
        v.y = (u16)at[i][1];
        v.z = (u32)(at[i][2] >> 4);
    }
    GsDevice_DrawTexturedPrim(*gGsDevice, &packet, count);
}
// The four corners every flat sprite here uses: (u0, v1), (u0, v0), (u1, v1), (u1, v0).
const u8 kQuadCorners[4] = {2, 0, 3, 1};

// ═══ Type 0x19: a particle (0x80 bytes) ════════════════════════════════════
// What it looks like and how it moves comes from an *order*, one of the 30
// records of Common/Effect/Effect.ord (dust, spray, leaves, flames…).
struct FxOrder {            // 0x40 bytes
    float spin[4];          // +0x00  turned by this much a frame (angles about x, y, z)
    char  sprite[8];        // +0x10
    float grow;             // +0x18  size × this a frame
    float drag;             // +0x1C  speed × this a frame
    float aim;              // +0x20  0 = flies off anywhere … 1 = straight along the direction given
    float speed;            // +0x24
    float scatter;          // +0x28  how far from the place given it starts
    float size;             // +0x2C
    float gravity;          // +0x30  taken off the upward speed a frame
    u32   rgba;             // +0x34
    u8    _38, life, _3a, _3b;
    u8    _3c, mode;        // +0x3D  low 2 bits: blend (0 add, 1 subtract, 2 normal); 0x10 fades out, 0x20 in, 0x30 in and out
    u8    kind;             // +0x3E  0 a streak, 1 a triangle, 3 a ring on the ground, else a square
    u8    _3f;
};
struct Particle {
    Effect base;            // life at +0x04
    u32 _0c;
    float turn[16];         // +0x10
    float pos[4];           // +0x50
    float vel[4];           // +0x60
    float size;             // +0x70
    s32 _74;
    s32 total;              // +0x78
    u32 order;              // +0x7C
};
struct FxSource { float pos[4]; float dir[4]; };

// sceVu0RotMatrix (0x1076C0) on an identity: about z, then y, then x.
void SpinMatrix(float* m, const float* angles) {
    Matrix_Identity(m);
    const auto turn = [&](int a, int b, float angle) {
        const float c = std::cos(angle), s = std::sin(angle);
        for (int row = 0; row < 4; row++) {
            const float x = m[row * 4 + a], y = m[row * 4 + b];
            m[row * 4 + a] = x * c - y * s;
            m[row * 4 + b] = x * s + y * c;
        }
    };
    turn(0, 1, angles[2]);
    turn(2, 0, angles[1]);
    turn(1, 2, angles[0]);
}

// 0x171AC0
int Particle_Start(Particle* self, const FxSource* from, u32 orderAddr) {
    self->order = orderAddr;
    if (orderAddr == 0)
        return 0;
    const FxOrder* order = P<FxOrder>(orderAddr);
    alignas(16) float any[4], along[4];
    RandomUnit(any);
    ScaleXYZ(any, any, 1.0f - order->aim);
    ScaleXYZ(along, from->dir, order->aim);
    Add(self->vel, any, along);
    Normalize(self->vel, self->vel);
    ScaleXYZ(self->vel, self->vel, order->speed);
    Matrix_Identity(self->turn);
    Copy4(self->pos, from->pos);
    RandomUnit(any);
    ScaleXYZ(any, any, order->scatter);
    Add(self->pos, self->pos, any);
    self->pos[3] = 0.0f;
    self->size = order->size;
    self->_74 = 0;
    self->base.life = order->life;
    self->total = self->base.life;
    return 1;
}
// 0x1719F0
int Particle_Update(Particle* self) {
    if (self->base.life == 0)
        return 0;
    self->base.life--;
    Add(self->pos, self->pos, self->vel);
    if (self->order != 0) {
        const FxOrder* order = P<FxOrder>(self->order);
        self->vel[1] -= order->gravity;
        ScaleXYZ(self->vel, self->vel, order->drag);
        alignas(16) float step[16];
        SpinMatrix(step, order->spin);
        Matrix_Multiply(self->turn, self->turn, step);
        self->size *= order->grow;
    }
    return 1;
}
// 0x171D80: its colour now (and its blend mode set).
u32 Particle_Colour(const Particle* self) {
    const FxOrder* order = P<FxOrder>(self->order);
    u32 colour = (order->rgba & 0xFEFEFEFEu) >> 1;
    const s32 alpha = (s32)(order->rgba >> 25);
    Fx_SetBlend(order->mode & 3);
    const s32 life = self->base.life, total = self->total;
    switch (order->mode & 0x30) {
    case 0x30: {
        const s32 half = total / 2;
        if (half < life)
            colour = (colour & 0xFFFFFF) | (u32)(0x80 - alpha * (life - half) / (half != 0 ? half : 1)) << 24;
        else
            colour = (colour & 0xFFFFFF) | (u32)(alpha * life / ((total >> 1) != 0 ? (total >> 1) : 1)) << 24;
        break;
    }
    case 0x20:
        colour = (colour & 0xFFFFFF) | (u32)(0x80 - alpha * life / (total != 0 ? total : 1)) << 24;
        break;
    case 0x10:
        colour = (colour & 0xFFFFFF) | (u32)(alpha * life / (total != 0 ? total : 1)) << 24;
        break;
    }
    return colour;
}
// A particle's corner in the world: scaled, turned its own way, and (flat
// sprites) turned to face the viewer.
void ParticleCorner(const Particle* self, const float* shape, bool faceViewer, float* out) {
    ScaleXYZ(out, shape, self->size);
    Apply(out, self->turn, out);
    if (faceViewer)
        Apply(out, InverseView(), out);
    Add(out, out, self->pos);
}
// 0x171710 (vtable 0x2EAFA0): a square facing the viewer
int Particle_DrawSquare(Particle* self) {
    if (self->order == 0)
        return 0;
    const float* shape = P<float>(0x2D20D0u);
    s32 at[4][4];
    for (int i = 0; i < 4; i++) {
        alignas(16) float p[4];
        ParticleCorner(self, shape + i * 4, true, p);
        if (GsDevice_Project(*gGsDevice, at[i], p) == 0)
            return 1;
    }
    u64 tex0;
    const u8* sprite = Fx_Sprite(P<FxOrder>(self->order)->sprite, &tex0);
    if (sprite != nullptr)
        DrawProjected(tex0, 0x15C, sprite, at, kQuadCorners, 4, Particle_Colour(self));
    return 0;
}
// 0x170D90 (vtable 0x2EAF40): a triangle facing the viewer
int Particle_DrawTriangle(Particle* self) {
    if (self->order == 0)
        return 0;
    const float* shape = P<float>(0x2D2210u);
    s32 at[3][4];
    for (int i = 0; i < 3; i++) {
        alignas(16) float p[4];
        ParticleCorner(self, shape + i * 4, true, p);
        if (GsDevice_Project(*gGsDevice, at[i], p) == 0)
            return 1;
    }
    u64 tex0;
    const u8* sprite = Fx_Sprite(P<FxOrder>(self->order)->sprite, &tex0);
    if (sprite != nullptr)
        DrawProjected(tex0, 0x15C, sprite, at, kQuadCorners, 3, Particle_Colour(self));
    return 0;
}
// 0x1710F0 (vtable 0x2EAF60): a ring lying in the world — a fan of ten points
int Particle_DrawRing(Particle* self) {
    if (self->order == 0)
        return 0;
    const float* shape = P<float>(0x2D2110u);
    s32 at[10][4];
    int outside = 0;
    for (int i = 0; i < 10; i++) {
        alignas(16) float p[4];
        ParticleCorner(self, shape + i * 4, false, p);
        p[3] = 1.0f;
        if (GsDevice_Project(*gGsDevice, at[i], p) == 0)
            outside++;
    }
    if (outside >= 6)
        return 1;
    u64 tex0;
    const u8* sprite = Fx_Sprite(P<FxOrder>(self->order)->sprite, &tex0);
    if (sprite == nullptr)
        return 0;
    const s32* right = P<s32>(0x2D21B0u);
    const s32* bottom = P<s32>(0x2D21E0u);
    u8 corner[10];
    for (int i = 0; i < 10; i++)
        corner[i] = (u8)((right[i] ? 1 : 0) | (bottom[i] ? 2 : 0));
    const u32 colour = Particle_Colour(self);
    DrawProjected(tex0, 0x15D, sprite, at, corner, 10, colour);
    return (int)colour;
}
// 0x1714D0 (vtable 0x2EAF80): a streak — a line from where it is to where it
// will be in two frames
int Particle_DrawStreak(Particle* self) {
    if (self->order == 0)
        return 0;
    alignas(16) float ends[2][4];
    Copy4(ends[0], self->pos);
    Add(ends[1], self->pos, self->vel);
    Add(ends[1], ends[1], self->vel);
    ends[0][3] = ends[1][3] = 1.0f;
    s32 at[2][4];
    for (int i = 0; i < 2; i++)
        if (GsDevice_Project(*gGsDevice, at[i], ends[i]) == 0)
            return 1;
    struct V { u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 prim; V v[2]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.prim = 0xE9;
    const u32 colour = Particle_Colour(self);
    for (int i = 0; i < 2; i++)
        packet.v[i] = {colour, 1.0f, (u16)at[i][0], (u16)at[i][1], (u32)(at[i][2] >> 4)};
    GsDevice_DrawPrim(*gGsDevice, &packet, 2);
    return 0;
}
void* Particle_Dtor(Particle* self, int flag) {         // 0x171EF0 / 0x171650 / 0x171410 / 0x171030
    if (self != nullptr) {
        self->base.vtable = 0x2EAC20u;
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}
// 0x171F50 / 0x1716C0 / 0x171480 / 0x1710A0
Particle* Particle_Ctor(Particle* self, u32 vtable) {
    Effect_Ctor(&self->base);
    self->base.type = 0x19;
    self->base.vtable = vtable;
    return self;
}
GUEST_BIND(0x171EF0, Particle_Dtor);
static const guest::Binder guest_bind_Particle_Dtor_1{0x171650, reinterpret_cast<void*>(Particle_Dtor), "Particle_Dtor"};
static const guest::Binder guest_bind_Particle_Dtor_2{0x171410, reinterpret_cast<void*>(Particle_Dtor), "Particle_Dtor"};
static const guest::Binder guest_bind_Particle_Dtor_3{0x171030, reinterpret_cast<void*>(Particle_Dtor), "Particle_Dtor"};
GUEST_BIND(0x171AC0, Particle_Start);
GUEST_BIND(0x1719F0, Particle_Update);
GUEST_BIND(0x171710, Particle_DrawSquare);
GUEST_BIND(0x170D90, Particle_DrawTriangle);
GUEST_BIND(0x1710F0, Particle_DrawRing);
GUEST_BIND(0x1714D0, Particle_DrawStreak);

// ═══ Type 0x1A: an emitter (0x40 bytes, vtable 0x2EAFC0) ═══════════════════
// Throws out `count` particles of one order a frame for `life` frames.
struct EmitEntry { u32 pos, dir; s16 order, life, count, flags; };     // flags: 1 for ever, 2 stays where it began
struct Emitter {
    Effect base;
    u32 _0c;
    float pos[4];           // +0x10
    float dir[4];           // +0x20
    EmitEntry entry;        // +0x30
};
int Emitter_Start(Emitter* self, const EmitEntry* entry, u32) {         // 0x17C780
    self->entry = *entry;
    self->base.life = (u16)self->entry.life;
    Copy4(self->pos, P<float>(self->entry.pos));
    Copy4(self->dir, P<float>(self->entry.dir));
    return 1;
}
int Emitter_Update(Emitter* self) {                                     // 0x17C660
    static u32 scratch = 0;         // (the original's is on its stack, which is guest memory)
    if (scratch == 0)
        scratch = guest::addr(guest::alloc(sizeof(FxSource)));
    FxSource* from = P<FxSource>(scratch);
    for (u32 n = (u16)self->entry.count; n != 0; n--) {
        if ((self->entry.flags & 2) == 0) {
            Copy4(from->pos, P<float>(self->entry.pos));
            Copy4(from->dir, P<float>(self->entry.dir));
        } else {
            Copy4(from->pos, self->pos);
            Copy4(from->dir, self->dir);
        }
        RenderMgr_AddWorldEffect(Mgr(), 0x19, scratch, (u32)(u16)self->entry.order);
    }
    if (self->entry.flags & 1)
        return 1;
    self->base.life--;
    return self->base.life != 0;
}
int Emitter_Draw(Emitter*) { return 1; }                                // 0x17C650
void* Emitter_Dtor(Emitter* self, int flag) {                           // 0x17C800
    if (self != nullptr) {
        self->base.vtable = 0x2EAC20u;
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}
GUEST_BIND(0x17C780, Emitter_Start);
GUEST_BIND(0x17C660, Emitter_Update);
GUEST_BIND(0x17C650, Emitter_Draw);
GUEST_BIND(0x17C800, Emitter_Dtor);

// ── 0x17EDF0 (→ 0x17EC90): the stock bursts ───────────────────────────────
// 0–2 a puff of dust for 2 / 3 / 5 frames, 3 order 5 for 2, 4–6 three of
// order 7 a frame for 2 / 6 / 8, 7 an explosion (order 13 and ten of 15).
void Fx_Burst(int kind, const float* pos, const float* dir) {
    static const s16 kTable[7][4] = {      // order, frames, a frame, flags  (0x17EE80)
        {0, 2, 1, 2}, {0, 3, 1, 2}, {0, 5, 1, 2}, {5, 2, 1, 2}, {7, 2, 3, 2}, {7, 6, 3, 2}, {7, 8, 3, 2},
    };
    static u32 scratch = 0;
    if (scratch == 0)
        scratch = guest::addr(guest::alloc(sizeof(FxSource)));
    FxSource* from = P<FxSource>(scratch);
    Copy4(from->pos, pos);
    Copy4(from->dir, dir);
    if (kind == 7) {
        RenderMgr_AddWorldEffect(Mgr(), 0x19, scratch, 0x0D);
        for (int i = 0; i < 10; i++)
            RenderMgr_AddWorldEffect(Mgr(), 0x19, scratch, 0x0F);
        return;
    }
    if (kind < 0 || kind > 6)
        return;
    EmitEntry entry{scratch, scratch + 0x10, kTable[kind][0], kTable[kind][1], kTable[kind][2], kTable[kind][3]};
    Emitter* e = static_cast<Emitter*>(MemAlloc(0x40));
    Effect_Ctor(&e->base);
    e->base.vtable = 0x2EAFC0u;
    e->base.type = 0x1A;
    Emitter_Start(e, &entry, 0);
    AddToWorld(&e->base);
}

// ═══ Blood ═════════════════════════════════════════════════════════════════
// A cut makes a *spray* (type 0x17) at the victim's chest that, for a few
// frames, throws out *puffs* (a red cloud that swells and fades) and *drops*
// (which fall and lie on the ground).  All of it only with the Blood setting on.
struct Mote {               // 0x40 bytes: the puff, the drop, the pool and the spark
    Effect base;            // life at +0x04
    u32 _0c;
    float pos[4];           // +0x10
    float vel[4];           // +0x20
    union { float size; s32 sparkTotal; };  // +0x30
    union { s32 total; s32 sparkKind; };    // +0x34
    s32 state;              // +0x38  drops: 0 falling, 1 lying, 2 ran down a wall
    u32 _3c;
};
void* Mote_Dtor(Mote* self, int flag) {     // 0x221520, 0x22F290, 0x1879A0, 0x239CF0, 0x2399A0, 0x222550
    if (self != nullptr) {
        self->base.vtable = 0x2EAC20u;
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}
inline u32 FadeColour(const Mote* self, u32 rgb) {
    return (u32)((self->base.life << 7) / (self->total != 0 ? self->total : 1)) << 24 | rgb;
}

// ── The puff (0x221580, vtable 0x2EC4D0) ──────────────────────────────────
int Puff_Start(Mote* self, const float* pos, const float* vel) {            // 0x2212B0
    Copy4(self->pos, pos);
    Copy4(self->vel, vel);
    alignas(16) float any[4];
    RandomUnit(any);
    Scale4(any, any, 0.05f);
    Add(self->pos, self->pos, any);
    RandomUnit(any);
    Scale4(any, any, 0.07f);
    Add(self->vel, self->vel, any);
    Add(self->pos, self->pos, self->vel);
    self->size = 0.25f;
    self->pos[3] = 1.0f;
    self->base.life = self->total = (Rand_Next() & 7) + 3;
    return 1;
}
int Puff_Update(Mote* self) {                                               // 0x221200
    Add(self->pos, self->pos, self->vel);
    self->vel[1] -= 0.0245f;
    Scale4(self->vel, self->vel, 0.9f);
    self->size += 0.005f;
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Puff_Draw(Mote* self) {                                                 // 0x220F20
    const float* shape = P<float>(0x2D99E0u);
    s32 at[4][4];
    int outside = 0;
    for (int i = 0; i < 4; i++) {
        alignas(16) float p[4];
        ScaleXYZ(p, shape + i * 4, self->size);
        Apply(p, InverseView(), p);
        Add(p, p, self->pos);
        if (GsDevice_Project(*gGsDevice, at[i], p) == 0)
            outside++;
    }
    if (outside == 4)
        return 1;
    u64 tex0;
    const u8* sprite = Fx_Sprite(P<const char>(*P<u32>(0x2EE1DCu)), &tex0);     // "blood3"
    if (sprite == nullptr)
        return 0;
    Fx_SetBlend(2);
    DrawProjected(tex0, 0x15C, sprite, at, kQuadCorners, 4, FadeColour(self, 0x808080));
    return 1;
}
GUEST_BIND(0x221520, Mote_Dtor);
GUEST_BIND(0x2212B0, Puff_Start);
GUEST_BIND(0x221200, Puff_Update);
GUEST_BIND(0x220F20, Puff_Draw);

// ── The drop (0x22F2F0, vtable 0x2EC640) ──────────────────────────────────
int Drop_Start(Mote* self, const float* pos, const float* vel) {            // 0x22F190
    self->base.life = self->total = (Rand_Next() & 0x1F) + 10;
    Copy4(self->pos, pos);
    Copy4(self->vel, vel);
    alignas(16) float any[4];
    RandomUnit(any);
    Scale4(any, any, 0.1f);
    Add(self->pos, self->pos, any);
    RandomUnit(any);
    Scale4(any, any, 0.1f);
    any[1] *= 0.1f;
    Add(self->vel, self->vel, any);
    Add(self->pos, self->pos, self->vel);
    self->size = 0.01f;
    return 1;
}
int Drop_Update(Mote* self) {                                               // 0x22EFD0
    if (self->state == 0) {
        Add(self->pos, self->pos, self->vel);
        Scale4(self->vel, self->vel, 0.85f);
        self->vel[1] -= 0.0245f;
        if (*gEffectMap != 0) {
            const ColCellType* cell = Map_CellAt(P<MapCollision>(*gEffectMap), self->pos);
            if ((cell->flags & 2) == 0) {
                if (self->pos[1] < cell->height) {
                    self->state = 1;
                    self->pos[1] = cell->height + 0.03f;
                }
            } else {
                // Against a wall: back out of it and run down.
                self->state = 2;
                self->vel[1] = 0.0f;
                Normalize(self->vel, self->vel);
                Scale4(self->vel, self->vel, 0.5f);
                Add(self->pos, self->pos, self->vel);
                memset(self->vel, 0, 16);
            }
        }
    }
    if (self->state == 2) {
        Add(self->pos, self->pos, self->vel);
        self->vel[1] -= 0.0245f;
        Scale4(self->vel, self->vel, 0.1f);
    }
    self->size += 0.001f;
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Drop_Draw(Mote* self) {                                                 // 0x22EDA0
    Fx_SetBlend(2);
    QuadParams quad;
    memset(&quad, 0, sizeof quad);
    quad.rgba = FadeColour(self, 0x808080);
    int outside = 0;
    const bool upright = self->state == 0 || self->state == 2;
    const float* shape = P<float>(upright ? 0x2DA020u : 0x2DA060u);
    for (int i = 0; i < 4; i++) {
        alignas(16) float p[4];
        ScaleXYZ(p, shape + i * 4, self->size);
        if (upright)
            Apply(p, InverseView(), p);
        Add(p, p, self->pos);
        if (GsDevice_Project(*gGsDevice, quad.at[i], p) == 0)
            outside++;
    }
    if (outside < 3)
        SpriteSet_DrawQuad(*gEffectSprites,
                           SpriteSet_FindByName(*gEffectSprites, P<const char>(*P<u32>(0x2EE1E4u))), &quad);   // "blood"
    return 1;
}
static const guest::Binder guest_bind_Mote_Dtor_1{0x22F290, reinterpret_cast<void*>(Mote_Dtor), "Mote_Dtor"};
GUEST_BIND(0x22F190, Drop_Start);
GUEST_BIND(0x22EFD0, Drop_Update);
GUEST_BIND(0x22EDA0, Drop_Draw);

Mote* NewMote(u32 vtable, int type) {
    Mote* m = static_cast<Mote*>(MemAlloc(0x40));
    memset(m, 0, 0x40);
    Effect_Ctor(&m->base);
    m->base.vtable = vtable;
    m->base.type = type;
    return m;
}
void ThrowPuff(const float* pos, const float* vel) {
    Mote* m = NewMote(0x2EC4D0u, 0x16);
    Puff_Start(m, pos, vel);
    AddToWorld(&m->base);
}
void ThrowDrop(const float* pos, const float* vel) {
    Mote* m = NewMote(0x2EC640u, 0x16);
    Drop_Start(m, pos, vel);
    AddToWorld(&m->base);
}

// ── The pool (0x187A00, vtable 0x2EB0F0): blood spreading on the ground ───
int Pool_Start(Mote* self, const float* pos, const float* dir) {            // 0x187920
    if (!BloodOn()) {
        self->base.life = 0;
        return 0;
    }
    self->base.life = self->total = 0x78;
    self->size = 0.2f;
    Copy4(self->pos, pos);
    Copy4(self->vel, dir);
    return 1;
}
int Pool_Update(Mote* self) {                                               // 0x1878A0
    const s32 life = self->base.life;
    if (life == 0)
        return 0;
    if (life > 0x3C)
        self->size += ((float)((0x3C - life) * (0x3C - life)) / 3600.0f) * 0.03f;
    self->base.life--;
    return 1;
}
int Pool_Draw(Mote* self) {                                                 // 0x187590
    u64 tex0;
    const u8* sprite = Fx_Sprite(P<const char>(*P<u32>(0x2EDB00u)), &tex0);     // "blood"
    if (sprite == nullptr)
        return 0;
    Fx_SetBlend(2);
    const float* shape = P<float>(0x2D8080u);
    s32 at[4][4];
    int outside = 0;
    for (int i = 0; i < 4; i++) {
        alignas(16) float p[4];
        ScaleXYZ(p, shape + i * 4, self->size);
        Add(p, p, self->pos);
        if (GsDevice_Project(*gGsDevice, at[i], p) == 0)
            outside++;
    }
    if (outside >= 3)
        return 1;
    const s32 life = self->base.life;
    const u32 alpha = life < 0x6F ? (u32)((life << 7) / 0x6E) : 0x80u;
    DrawProjected(tex0, 0x15C, sprite, at, kQuadCorners, 4, alpha << 24 | 0x808080);
    return 1;
}
static const guest::Binder guest_bind_Mote_Dtor_2{0x1879A0, reinterpret_cast<void*>(Mote_Dtor), "Mote_Dtor"};
GUEST_BIND(0x187920, Pool_Start);
GUEST_BIND(0x1878A0, Pool_Update);
GUEST_BIND(0x187590, Pool_Draw);

// ── The spray (0x2225B0, type 0x17, vtable 0x2EC510) ──────────────────────
struct Spray {
    Effect base;            // life at +0x04
    u32 _0c;
    float pos[4];           // +0x10
    float dir[4];           // +0x20  in the chest's own space, so it turns with the body
    u32 params;             // +0x30  whose blood
    s32 level;              // +0x34  0 a nick … 2 a deep cut, 3 the last one (three seconds of it)
    u32 _38, _3c;
};
struct SprayParams { float pos[4]; float dir[4]; u32 params; s32 level; };
const float* ChestOf(u32 params) {
    const u32 model = params != 0 ? P<CharaParams>(params)->link[1] : 0;
    return model != 0 && P<u32>(model)[1] != 0 ? P<float>(P<u32>(model)[1] + 0x460) : nullptr;
}
int Spray_Update(Spray* self) {                                             // 0x221DF0
    if (self->base.life == 0)
        return 0;
    self->base.life--;
    alignas(16) float out[4], down[4];
    Copy4(out, self->dir);
    if (const float* chest = ChestOf(self->params)) {
        Copy4(self->pos, chest + 12);
        Apply(out, chest, out);
    }
    Copy4(down, out);
    down[1] = -0.0327f;
    if (self->level != 0) {
        if (self->level != 1 && self->level != 2) {
            if (self->level != 3)
                return 1;
            ThrowPuff(self->pos, out);
            ThrowDrop(self->pos, down);
        }
        ThrowPuff(self->pos, out);
        ThrowDrop(self->pos, down);
    }
    if (self->base.life & 1)
        ThrowDrop(self->pos, down);
    ThrowPuff(self->pos, out);
    ThrowPuff(self->pos, out);
    return 1;
}
// `shake`: the player is in it, so the camera feels it.
int Spray_Start(Spray* self, const SprayParams* from, u32 shake) {          // 0x222260
    if (!BloodOn()) {
        self->base.life = 0;
        return 0;
    }
    self->base.life = 0;
    Copy4(self->pos, from->pos);
    Copy4(self->dir, from->dir);
    self->params = from->params;
    self->level = from->level;
    Normalize(self->dir, self->dir);
    ScaleXYZ(self->dir, self->dir, 0.2f);
    self->dir[1] += 0.043555558f;
    if (const float* chest = ChestOf(self->params)) {
        Copy4(self->pos, chest + 12);
        alignas(16) float any[4];
        RandomUnit(any);
        ScaleXYZ(any, any, 0.1f);
        Add(self->pos, self->pos, any);
        // Into the chest's space: its rotation turned back.
        alignas(16) float back[16];
        memset(back, 0, sizeof back);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                back[i * 4 + j] = chest[j * 4 + i];
        back[15] = 1.0f;
        Apply(self->dir, back, self->dir);
    }
    void* cameras = P(*P<u32>(0x2EE528u));
    // A red light on whoever is near, for a few frames.
    static const float kRed[4] = {1.0f, 0.0f, 0.0f, 1.0f};                 // 0x2D9A20
    if (self->level >= 0 && self->level <= 3)
        Lights_Add(self->pos, kRed, 9, 3 + 3 * self->level);
    switch (self->level) {
    case 3:
        self->base.life = 0x5A;
        break;
    case 2:
        self->base.life = 9;
        if (cameras != nullptr && shake != 0)
            CamList_Shake(0.02f, cameras, 5);
        break;
    case 1:
        self->base.life = 6;
        if (cameras != nullptr && shake != 0)
            CamList_Shake(0.01f, cameras, 5);
        break;
    case 0:
        self->base.life = 3;
        break;
    }
    Spray_Update(self);
    return 1;
}
int Spray_Draw(Spray*) { return 1; }                                        // 0x221DE0
static const guest::Binder guest_bind_Mote_Dtor_3{0x222550, reinterpret_cast<void*>(Mote_Dtor), "Mote_Dtor"};
GUEST_BIND(0x222260, Spray_Start);
GUEST_BIND(0x221DF0, Spray_Update);
GUEST_BIND(0x221DE0, Spray_Draw);

// ═══ A blunt blow (0x239A00, vtable 0x2EC6D0) and its sparks (0x2EC6F0) ════
// For three to five frames it throws out white dots: quick ones that slow
// down, and (harder blows) ones that arc and bounce on the ground.
struct SparkSource { float pos[4]; float dir[4]; s32 kind; };
int Spark_Start(Mote* self, const SparkSource* from, u32) {                 // 0x239C10
    Copy4(self->pos, from->pos);
    Copy4(self->vel, from->dir);
    alignas(16) float any[4];
    RandomUnit(any);
    Scale4(any, any, 0.1f);
    Add(self->vel, self->vel, any);
    self->sparkKind = from->kind;
    if (self->sparkKind == 1)
        self->base.life = self->sparkTotal = (Rand_Next() & 0xF) + 0xF;
    else if (self->sparkKind == 0)
        self->base.life = self->sparkTotal = (Rand_Next() & 3) + 7;
    return 1;
}
int Spark_Update(Mote* self) {                                              // 0x239B20
    Add(self->pos, self->pos, self->vel);
    if (self->sparkKind == 1) {
        self->vel[1] -= 0.03266667f;
        if (*gEffectMap != 0 && self->pos[1] < Map_CellAt(P<MapCollision>(*gEffectMap), self->pos)->height)
            self->vel[1] = -self->vel[1] * 0.5f;
    } else if (self->sparkKind == 0) {
        Scale4(self->vel, self->vel, 0.8f);
    }
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Spark_Draw(Mote* self) {                                                // 0x239A40
    Fx_SetBlend(2);
    s32 at[4];
    alignas(16) float p[4];
    Copy4(p, self->pos);
    if (GsDevice_Project(*gGsDevice, at, p) == 0)
        return 0;
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } draw;
    draw.flags = 1;
    draw.x = at[0];
    draw.y = at[1];
    draw.z = (u32)(at[2] >> 4);
    draw.w = draw.h = -1;
    draw.rgba = (u32)((self->base.life << 7) / (self->sparkTotal != 0 ? self->sparkTotal : 1)) << 24 | 0x808090;
    SpriteSet_DrawEx(P(*gEffectSprites), SpriteSet_FindByName(*gEffectSprites, P<const char>(*P<u32>(0x2EE1E8u))),
                     &draw);       // "dot"
    return 1;
}
static const guest::Binder guest_bind_Mote_Dtor_4{0x239CF0, reinterpret_cast<void*>(Mote_Dtor), "Mote_Dtor"};
GUEST_BIND(0x239C10, Spark_Start);
GUEST_BIND(0x239B20, Spark_Update);
GUEST_BIND(0x239A40, Spark_Draw);

struct Impact {
    Effect base;            // life at +0x04
    u32 _0c;
    float pos[4];           // +0x10
    float dir[4];           // +0x20
    s32 level;              // +0x30
    u32 _34[3];
};
void ThrowSparks(const Impact* self, int kind, int count) {
    SparkSource from;
    Copy4(from.pos, self->pos);
    Copy4(from.dir, self->dir);
    from.kind = kind;
    for (int i = 0; i < count; i++) {
        Mote* m = NewMote(0x2EC6F0u, 1);
        Spark_Start(m, &from, 0);
        AddToWorld(&m->base);
    }
}
int Impact_Update(Impact* self) {                                           // 0x2394A0
    const s32 level = self->level;
    if (level >= 0 && level <= 3) {
        if (level >= 2) {
            ThrowSparks(self, 1, 4);
            ThrowSparks(self, 0, 1);
        }
        if (level >= 1) {
            ThrowSparks(self, 1, 4);
            ThrowSparks(self, 0, 1);
        }
        ThrowSparks(self, 0, 8);
    }
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
// (As with blood, the original lights the place for those frames.)
int Impact_Start(Impact* self, const SparkSource* from, u32) {              // 0x239860
    Copy4(self->pos, from->pos);
    Copy4(self->dir, from->dir);
    self->level = from->kind;
    self->base.life = self->level == 3 || self->level == 2 ? 5 : self->level == 1 ? 4 : 3;
    static const float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};               // 0x2DA180
    Lights_Add(self->pos, kWhite, 9, self->base.life);
    Impact_Update(self);
    return 1;
}
int Impact_Draw(Impact*) { return 1; }                                      // 0x239490
static const guest::Binder guest_bind_Mote_Dtor_5{0x2399A0, reinterpret_cast<void*>(Mote_Dtor), "Mote_Dtor"};
GUEST_BIND(0x239860, Impact_Start);
GUEST_BIND(0x2394A0, Impact_Update);
GUEST_BIND(0x239490, Impact_Draw);

// ═══ A ring spreading from a blow (0x2BB980, 0x30 bytes, vtable 0x2ED900) ══
// The sprite "hamon" — a quarter of a ring — as a fan of nine points round
// its middle, turned to the viewer, growing for six frames as it fades;
// bigger for a harder blow (level 0–2), with a yellow point light.
struct Ring {
    Effect base;            // life at +0x04
    u32 _0c;
    float pos[4];           // +0x10
    s32 level;              // +0x20
    u32 _24[3];
};
int Ring_Start(Ring* self, const float* pos, s32 level) {                   // 0x2BB7F0
    self->base.life = 6;
    Copy4(self->pos, pos);
    self->level = level < 0 ? 0 : level > 2 ? 2 : level;
    static const float kYellow[4] = {1.0f, 1.0f, 0.0f, 0.0f};              // 0x2DB970
    if (level >= 0 && level <= 2)
        Lights_Add(pos, kYellow, 9, 6 + 2 * level);
    return 1;
}
int Ring_Update(Ring* self) {                                               // 0x2BB7C0
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Ring_Draw(Ring* self) {                                                 // 0x2BB7A0 → 0x2BB3B0
    static const float kGrow[3] = {7.0f, 8.0f, 10.0f};
    const float size = ((float)(6 - self->base.life) / 6.0f * kGrow[self->level] + 8.0f) * 0.04f;
    u64 tex0;
    const u8* sprite = Fx_Sprite(P<const char>(0x2E8C60u), &tex0);          // "hamon"
    if (sprite == nullptr)
        return 0;
    const float* shape = P<float>(0x2DB980u);       // the middle, then eight points round it and the first again
    static const u8 kCorner[10] = {3, 2, 0, 1, 0, 2, 0, 1, 0, 2};
    s32 at[10][4];
    int seen = 10;
    for (int i = 0; i < 10; i++) {
        alignas(16) float p[4];
        ScaleXYZ(p, shape + i * 4, size);
        Apply(p, InverseView(), p);
        Add(p, self->pos, p);
        p[3] = 1.0f;
        if (GsDevice_Project(*gGsDevice, at[i], p) == 0)
            seen--;
        at[i][2] = (s32)(0xFFF000u << 4);           // in front of everything
    }
    if (seen != 0) {
        Fx_SetBlend(0);
        DrawProjected(tex0, 0x15D, sprite, at, kCorner, 10, (u32)((self->base.life << 7) / 6) << 24 | 0x808080);
    }
    return 1;
}
void* Ring_Dtor(Ring* self, int flag) {                                     // 0x2BB920
    if (self != nullptr && flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x2BB920, Ring_Dtor);
GUEST_BIND(0x2BB7F0, Ring_Start);
GUEST_BIND(0x2BB7C0, Ring_Update);
GUEST_BIND(0x2BB7A0, Ring_Draw);

// ═══ Type 0x12: a blow stopped (0x268FF0, 0x30 bytes, vtable 0x2ECC20) ════
// A bright flash (the "guard" sprite, additive) where the blades met, and
// ten streaks flying out of it (vtable 0x2ECC00).  Both are drawn in front
// of everything.
struct Flash {
    Effect base;            // life at +0x04
    u32 _0c;
    float pos[4];           // +0x10
    s32 total;              // +0x20
    float size;             // +0x24
    u32 rgb;                // +0x28
    u32 _2c;
};
struct Streak {
    Effect base;
    u32 _0c;
    float pos[4];           // +0x10
    float vel[4];           // +0x20
    s32 total;              // +0x30
    u32 rgb;                // +0x34
    u32 _38, _3c;
};
int Streak_Start(Streak* self, const float* pos, u32 rgb) {                 // 0x268AA0
    Copy4(self->pos, pos);
    self->rgb = rgb;
    RandomUnit(self->vel);
    Scale4(self->vel, self->vel, 0.45f);
    self->base.life = self->total = (Rand_Next() & 3) + 3;
    return 1;
}
int Streak_Update(Streak* self) {                                           // 0x268A50
    Add(self->pos, self->pos, self->vel);
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Streak_Draw(Streak* self) {                                             // 0x2688F0
    alignas(16) float ends[2][4];
    Copy4(ends[0], self->pos);
    Add(ends[1], self->pos, self->vel);
    Add(ends[1], ends[1], self->vel);
    s32 at[2][4];
    int outside = 0;
    for (int i = 0; i < 2; i++)
        if (GsDevice_Project(*gGsDevice, at[i], ends[i]) == 0)
            outside++;
    if (outside == 2)
        return 1;
    Fx_SetBlend(2);
    struct V { u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 prim; V v[2]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.prim = 0x49;
    const u32 colour = self->rgb | (u32)((self->base.life << 7) / (self->total != 0 ? self->total : 1)) << 24;
    for (int i = 0; i < 2; i++)
        packet.v[i] = {colour, 0.0f, (u16)at[i][0], (u16)at[i][1], 0xFFF000u};
    GsDevice_DrawPrim(*gGsDevice, &packet, 2);
    return 1;
}
GUEST_BIND(0x268AA0, Streak_Start);
GUEST_BIND(0x268A50, Streak_Update);
GUEST_BIND(0x2688F0, Streak_Draw);
static const guest::Binder guest_bind_Streak_Dtor{0x268B20, reinterpret_cast<void*>(Mote_Dtor), "Streak_Dtor"};

int Flash_Start(Flash* self, const float* pos, u32 rgb) {                   // 0x268DB0
    Copy4(self->pos, pos);
    self->rgb = (rgb != 0 ? rgb : 0x808080u) & 0xFFFFFFu;
    self->size = 0.4f;
    self->total = 4;
    self->base.life = 7;
    {   // its colour as a light, for as long as it lasts
        const float colour[4] = {(self->rgb & 0xFF) / 255.0f, ((self->rgb >> 8) & 0xFF) / 255.0f,
                                 ((self->rgb >> 16) & 0xFF) / 255.0f, 0.0f};
        Lights_Add(self->pos, colour, 9, self->base.life);
    }
    for (int i = 0; i < 10; i++) {
        Streak* streak = static_cast<Streak*>(MemAlloc(0x40));
        memset(streak, 0, 0x40);
        Effect_Ctor(&streak->base);
        streak->base.vtable = 0x2ECC00u;
        streak->base.type = 0x12;
        Streak_Start(streak, self->pos, self->rgb);
        AddToWorld(&streak->base);
    }
    return 1;
}
int Flash_Update(Flash* self) {                                             // 0x268D50
    if (self->base.life < self->total)
        self->size *= 0.98f;
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Flash_Draw(Flash* self) {                                               // 0x268BC0
    QuadParams quad;
    memset(&quad, 0, sizeof quad);
    quad.flags = 1;
    quad.rgba = self->base.life < self->total ? self->rgb | (u32)((self->base.life << 7) / self->total) << 24
                                              : self->rgb | 0x80000000u;
    const float* shape = P<float>(0x2DAF40u);
    int outside = 0;
    for (int i = 0; i < 4; i++) {
        alignas(16) float p[4];
        ScaleXYZ(p, shape + i * 4, self->size);
        Apply(p, InverseView(), p);
        Add(p, self->pos, p);
        if (GsDevice_Project(*gGsDevice, quad.at[i], p) == 0)
            outside++;
        quad.at[i][2] = 0xFFF000;
    }
    if (outside < 4) {
        Fx_SetBlend(0);
        SpriteSet_DrawQuad(*gEffectSprites,
                           SpriteSet_FindByName(*gEffectSprites, P<const char>(*P<u32>(0x2EE378u))), &quad);   // "guard"
    }
    return 1;
}
GUEST_BIND(0x268DB0, Flash_Start);
GUEST_BIND(0x268D50, Flash_Update);
GUEST_BIND(0x268BC0, Flash_Draw);
static const guest::Binder guest_bind_Flash_Dtor{0x268F90, reinterpret_cast<void*>(Mote_Dtor), "Flash_Dtor"};

// ── 0x243460: blood from a character ──────────────────────────────────────
void Chara_Bleed(Chara* chara, const float* at, int level) {
    const u32 paramsAddr = chara->params;
    const CharaParams* params = P<CharaParams>(paramsAddr);
    if (*P<u32>(paramsAddr + 0x10) != 0)            // held by someone
        return;
    const u32 flagsAt = *P<u32>(paramsAddr + 0xC8);
    if (((flagsAt != 0 ? *P<u32>(flagsAt) : 0) & 0x10) != 0)
        return;
    (void)params;
    SprayParams from;
    Copy4(from.pos, at);
    if (chara->flagHandle != 0)
        Copy4(from.dir, P<float>(chara->flagHandle + 0x1A0));       // the way the blow went
    else
        memset(from.dir, 0, 16);
    from.params = paramsAddr;
    from.level = level;
    // The camera shakes when the player is the one cut, or the one cutting.
    const u32 by = chara->_1dc;
    const u32 shake = chara->id < 2 || (by != 0 && *P<s32>(by) < 2) ? 1 : 0;
    Spray* spray = static_cast<Spray*>(MemAlloc(0x40));
    memset(spray, 0, 0x40);
    Effect_Ctor(&spray->base);
    spray->base.vtable = 0x2EC510u;
    spray->base.type = 0x17;
    spray->base.life = 10;
    Spray_Start(spray, &from, shake);
    AddToWorld(&spray->base);
}


// ═══ Type 0x13: the glare of a broken guard (0x22F8E0, 0x30 bytes, vtable
// 0x2EC660) ═════════════════════════════════════════════════════════════════
// For seven frames the picture itself — the frame buffer as a texture, each
// point taken from where it is drawn — is added to itself over a patch that
// faces the viewer: about half strength in the middle, swelling to three
// quarters and back, nothing at the rim.
struct Glare {
    Effect base;            // life at +0x04
    s32 total;              // +0x0C
    u32 _10[4];
    float pos[4];           // +0x20
};
static_assert(sizeof(Glare) == 0x30, "Glare");
int Glare_Start(Glare* self, const float* pos) {                            // 0x22F850
    self->base.life = self->total = 7;
    Copy4(self->pos, pos);
    return 1;
}
int Glare_Update(Glare* self) {                                             // 0x22F820
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Glare_Draw(Glare* self) {                                               // 0x22F330
    const float* shape = P<float>(0x2DA0A0u);       // the middle, then seven points round it
    s32 at[8][4];
    int outside = 0;
    for (int i = 0; i < 8; i++) {
        alignas(16) float p[4];
        ScaleXYZ(p, shape + i * 4, 0.5f);
        Apply(p, InverseView(), p);
        Add(p, p, self->pos);
        p[3] = 1.0f;
        if (GsDevice_Project(*gGsDevice, at[i], p) == 0)
            outside++;
    }
    if (outside >= 6)
        return 1;
    const s32 width = *P<s32>(0x2EE4C0u), height = *P<s32>(0x2EE4C4u);
    const s32 midX = (s32)(Screen_X(0, 0) & 0xFFFF), midY = (s32)(Screen_Y(0, 0) & 0xFFFF);
    const float swell = std::sin((float)(self->base.life * 0xB4 / self->total) * 3.1415927f / 180.0f) * 30.0f + 65.0f;
    const u32 grey = (u32)(s32)swell & 0xFF;
    struct Vertex { s16 u, v; u32 _04; u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 tex0, prim; Vertex v[8]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.tex0 = (u64)(width >> 6) << 14 | 0x268100000ull;
    packet.prim = 0x15D;
    for (int i = 0; i < 8; i++) {
        Vertex& v = packet.v[i];
        v.x = (u16)at[i][0];
        v.y = (u16)at[i][1];
        v.z = 0xFFF000;
        v.q = 1.0f;
        v.u = (s16)((width << 3) + ((s16)at[i][0] - (s16)midX));
        v.v = (s16)((height << 3) + ((s16)at[i][1] - (s16)midY));
        v.rgba = i == 0 ? (grey | grey << 8 | grey << 16 | 0x80000000u) : 0x80000000u;
    }
    Fx_SetBlend(0);
    GsDevice_DrawTexturedPrim(*gGsDevice, &packet, 8);
    return 1;
}
static const guest::Binder guest_bind_Glare_Dtor{0x22F880, reinterpret_cast<void*>(Mote_Dtor), "Glare_Dtor"};
GUEST_BIND(0x22F850, Glare_Start);
GUEST_BIND(0x22F820, Glare_Update);
GUEST_BIND(0x22F330, Glare_Draw);

// ═══ A crate or a pot breaking (0x29B020, vtable 0x2ED220) ═════════════════
// The effect itself only starts the others and is gone the next frame: ten
// puffs of dust (0x40 bytes, vtable 0x2ED200) and ten splinters (0x90 bytes,
// vtable 0x2ED240) — small models tumbling away and bouncing off the ground.
// The splinter models are the manager's two at +0x60 (0x29B480, made by
// RenderMgr_SetMap on first use): drawn natively from the same two files.
int Dust_Start(Mote* self, const float* pos, const float* size) {           // 0x29ACC0
    Copy4(self->pos, pos);
    self->size = *size * 0.5f;
    RandomUnit(self->vel);
    Scale4(self->vel, self->vel, self->size * 0.5f);
    self->vel[1] = 0.0f;
    Add(self->pos, self->pos, self->vel);
    Scale4(self->vel, self->vel, 0.3f);
    return 1;
}
int Dust_Update(Mote* self) {                                               // 0x29AC10
    Add(self->pos, self->pos, self->vel);
    Scale4(self->vel, self->vel, 0.8f);
    self->vel[1] += 0.005f;
    self->size += self->size * 0.02f;
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Dust_Draw(Mote* self) {                                                 // 0x29AAA0
    Fx_SetBlend(2);
    QuadParams quad;
    memset(&quad, 0, sizeof quad);
    quad.rgba = (u32)((self->base.life << 7) / self->total) << 24 | 0x808080;
    const float* shape = P<float>(0x2DB360u);
    int outside = 0;
    for (int i = 0; i < 4; i++) {
        alignas(16) float p[4];
        ScaleXYZ(p, shape + i * 4, self->size);
        Apply(p, InverseView(), p);
        Add(p, p, self->pos);
        if (GsDevice_Project(*gGsDevice, quad.at[i], p) == 0)
            outside++;
    }
    if (outside < 3)
        SpriteSet_DrawQuad(*gEffectSprites, SpriteSet_FindByName(*gEffectSprites, P<const char>(0x2E8540u)), &quad);  // "hokori"
    return 1;
}
static const guest::Binder guest_bind_Dust_Dtor{0x29AD60, reinterpret_cast<void*>(Mote_Dtor), "Dust_Dtor"};
GUEST_BIND(0x29ACC0, Dust_Start);
GUEST_BIND(0x29AC10, Dust_Update);
GUEST_BIND(0x29AAA0, Dust_Draw);

struct Splinter {
    Effect base;            // life at +0x04
    u32 _0c;
    float matrix[16];       // +0x10  its size and how it has turned
    float pos[4];           // +0x50
    float vel[4];           // +0x60
    float spin[4];          // +0x70  angles a frame
    u32 kind;               // +0x80  0 wood, 1 pottery
    u32 _84[3];
};
static_assert(sizeof(Splinter) == 0x90, "Splinter");
// sceVu0RotMatrix (0x1076C0): turned about z, then y, then x.
void RotateBy(float* m, const float* angles) {
    const auto turn = [&](int a, int b, float angle) {
        const float c = std::cos(angle), s = std::sin(angle);
        for (int row = 0; row < 4; row++) {
            const float x = m[row * 4 + a], y = m[row * 4 + b];
            m[row * 4 + a] = x * c - y * s;
            m[row * 4 + b] = x * s + y * c;
        }
    };
    turn(0, 1, angles[2]);
    turn(2, 0, angles[1]);
    turn(1, 2, angles[0]);
}
inline float RandomUnitNumber() { return (float)(Rand_Next() % 200 - 100) * 0.01f; }   // 0x2216C0
int Splinter_Start(Splinter* self, const float* pos, u32 kind) {            // 0x29B200
    Matrix_Identity(self->matrix);
    Copy4(self->pos, pos);
    self->kind = kind;
    self->matrix[0] = RandomUnitNumber() + 1.5f;
    self->matrix[5] = RandomUnitNumber() + 1.5f;
    self->matrix[10] = RandomUnitNumber() + 1.5f;
    RandomUnit(self->spin);
    Scale4(self->spin, self->spin, 0.18f);
    RandomUnit(self->vel);
    self->vel[1] += 4.0f;
    Scale4(self->vel, self->vel, 0.05f);
    return 1;
}
int Splinter_Update(Splinter* self) {                                       // 0x29B120
    Add(self->pos, self->pos, self->vel);
    self->vel[1] -= 0.024500001f;
    if (*gEffectMap != 0) {
        const ColCellType* cell = Map_CellAt(P<MapCollision>(*gEffectMap), self->pos);
        if (cell != nullptr && self->pos[1] < cell->height && self->vel[1] < 0.0f)
            self->vel[1] *= -0.5f;
    }
    RotateBy(self->matrix, self->spin);
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Splinter_Draw(Splinter* self) {                                         // 0x29B060 (→ 0x29B350: the VU1 boundary)
    static const char* const kModels[2] = {"Common/effect/kibako01.mdl", "Common/effect/tubo01.mdl"};
    if (self->kind >= 2)
        return 1;
    alignas(16) float m[16];
    memcpy(m, self->matrix, sizeof m);
    Copy4(&m[12], self->pos);
    Fx_SetBlend(2);     // (native: the 3D layer draws with the blend the sprites left)
    hle_object_draw_with(kModels[self->kind], m, "Common/Effect/Effect.BPM");    // (bound to the effect sprites' textures)
    return 1;
}
void* Splinter_Dtor(Splinter* self, int flag) {                             // 0x29B2F0
    if (self != nullptr) {
        self->base.vtable = 0x2EAC20u;
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}
GUEST_BIND(0x29B2F0, Splinter_Dtor);
GUEST_BIND(0x29B200, Splinter_Start);
GUEST_BIND(0x29B120, Splinter_Update);
GUEST_BIND(0x29B060, Splinter_Draw);

}  // namespace

extern "C" {

// ── 0x29AE00 (the start of effect class 0x29B020): something breaks at
// `pos` — `size` is how big it was, `kind` 0 wood / 1 pottery. ─────────────
void Fx_BreakBox(const float* pos, float size, int kind) {
    alignas(16) float at[4];
    Copy4(at, pos);
    for (int i = 0; i < 10; i++) {
        Mote* dust = NewMote(0x2ED200u, 1);
        dust->base.life = dust->total = (Rand_Next() & 7) + 7;
        Dust_Start(dust, at, &size);
        AddToWorld(&dust->base);
        Splinter* s = static_cast<Splinter*>(MemAlloc(sizeof(Splinter)));
        memset(s, 0, sizeof *s);
        Effect_Ctor(&s->base);
        s->base.vtable = 0x2ED240u;
        s->base.life = (Rand_Next() & 0x1F) + 0xF;
        Splinter_Start(s, at, (u32)kind);
        AddToWorld(&s->base);
    }
}

// ═══ Type 0x10: the trail of a blade (0x17CF40–0x17E520, 0x8E0 bytes, vtable
// 0x2EAFE0) ═════════════════════════════════════════════════════════════════
// A ribbon between where the blade's tip and base have been: a ring of 64
// pairs of points, each with a strength that falls 0x20 a frame from 0x80.
// The sword holder feeds it the blade's two ends every frame of an attack's
// hit frames (src/game/sword_holder.cpp); between the places it is given,
// in-between pairs are worked in so that a fast swing still draws an arc.
struct Trail {
    Effect base;
    u32 _0c;
    float ring[64][2][4];   // +0x010  {tip, base}
    float recent[4][2][4];  // +0x810  the last four places as they were given
    u8  strength[64];       // +0x890
    u8  oldest;             // +0x8D0
    u8  next;               // +0x8D1  where the next pair goes
    u8  recentAt;           // +0x8D2
    u8  style;              // +0x8D3  0 plain, 1 / 2 the sprites zanzo1 / zanzo2, 3 lines
    u8  hidden;             // +0x8D4
    u8  flash;              // +0x8D5
    u8  _8d6[10];
};
static_assert(sizeof(Trail) == 0x8E0 && offsetof(Trail, strength) == 0x890, "Trail");
std::set<u32> sTrails;      // the live ones (their holders ask before touching one)

void Trail_Push(Trail* t, const float* tip, const float* base) {
    t->strength[t->next] = 0x80;
    Copy4(t->ring[t->next][0], tip);
    Copy4(t->ring[t->next][1], base);
    t->next = (u8)((t->next + 1) & 0x3F);
    if (t->next == t->oldest)
        t->oldest = (u8)((t->oldest + 1) & 0x3F);
}
// 0x17E580: a point of the smooth curve through four points, between the second and the third.
void Trail_Curve(float t, float* out, const float (*p)[4]) {
    const float t2 = t * t, t3 = t2 * t;
    for (int k = 0; k < 4; k++) {
        const float in = ((p[1][k] - p[0][k]) + (p[2][k] - p[1][k])) * 0.5f;
        const float leave = ((p[2][k] - p[1][k]) + (p[3][k] - p[2][k])) * 0.5f;
        out[k] = p[1][k] * ((t3 * 2.0f - t2 * 3.0f) + 1.0f) + in * (t + (t3 - t2 * 2.0f)) + leave * (t3 - t2)
               + p[2][k] * (t3 * -2.0f + t2 * 3.0f);
    }
}
inline void Cross4(float* o, const float* a, const float* b) {
    const float r[4] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0.0f};
    memcpy(o, r, sizeof r);
}
// 0x17D6E0: with three places behind it, the stretch before the last is
// rounded off now that the next place is known.
void Trail_Smooth(Trail* t, const float* tip, const float* base) {
    float d[4];
    Sub(d, base, tip);
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    t->flash = 0;
    const u32 last = (t->next + 0x3Fu) & 0x3F, before = (t->next + 0x3Eu) & 0x3F, first = (t->next + 0x3Du) & 0x3F;
    Sub(d, t->ring[last][1], tip);
    if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) < 0.1f)
        return;
    if (t->oldest == first || t->oldest == before || t->oldest == last) {
        Trail_Push(t, tip, base);
        return;
    }
    float lastTip[4], lastBase[4];
    Copy4(lastTip, t->ring[last][0]);
    Copy4(lastBase, t->ring[last][1]);
    float through[4][4], tips[2][4], bases[2][4];
    Copy4(through[0], t->ring[first][0]);
    Copy4(through[1], t->ring[before][0]);
    Copy4(through[2], t->ring[last][0]);
    Copy4(through[3], tip);
    Trail_Curve(0.25f, t->ring[last][0], through);
    Trail_Curve(0.5f, tips[0], through);
    Trail_Curve(0.75f, tips[1], through);
    Copy4(through[0], t->ring[first][1]);
    Copy4(through[1], t->ring[before][1]);
    Copy4(through[2], t->ring[last][1]);
    Copy4(through[3], base);
    Trail_Curve(0.25f, t->ring[last][1], through);
    Trail_Curve(0.5f, bases[0], through);
    Trail_Curve(0.75f, bases[1], through);
    for (int i = 0; i < 2; i++) {                   // the blade keeps its length
        Sub(d, tips[i], bases[i]);
        Normalize(d, d);
        Scale4(d, d, length);
        Add(tips[i], bases[i], d);
    }
    Trail_Push(t, tips[0], bases[0]);
    Trail_Push(t, tips[1], bases[1]);
    Trail_Push(t, lastTip, lastBase);
    Trail_Push(t, tip, base);
}
// 0x17CF40: the blade is here now (its two ends, in the part's own space).
int Trail_Add(Trail* t, const float* matrix, const float* tipIn, const float* baseIn) {
    alignas(16) float tip[4], base[4];
    Apply(tip, matrix, tipIn);
    Apply(base, matrix, baseIn);
    const u32 last = (t->next + 0x3Fu) & 0x3F;
    if (t->oldest == last) {
        Trail_Push(t, tip, base);
    } else {
        float d[3] = {t->ring[last][0][0] - tip[0], t->ring[last][0][1] - tip[1], t->ring[last][0][2] - tip[2]};
        if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) < 0.1f)
            return 1;
        if (t->oldest != ((t->next + 0x3Eu) & 0x3F) && t->oldest != ((t->next + 0x3Du) & 0x3F)) {
            Trail_Smooth(t, tip, base);
            return 1;
        }
        // Only one or two places so far: three in-betweens on a curve that
        // leaves each blade at right angles to itself.
        const float (*was)[4] = t->recent[(t->recentAt + 3) & 3];
        float along[4], alongWas[4], normal[4], lean[4], leanWas[4];
        Sub(along, tip, base);
        Sub(alongWas, was[0], was[1]);
        Cross4(normal, along, alongWas);
        Cross4(leanWas, normal, alongWas);
        ScaleXYZ(normal, normal, -1.0f);
        Cross4(lean, normal, along);
        static const float kWeights[3][4] = {{0.421875f, 0.421875f, 0.140625f, 0.015625f},
                                             {0.125f, 0.375f, 0.375f, 0.125f},
                                             {0.015625f, 0.140625f, 0.421875f, 0.421875f}};
        float made[3][2][4];
        for (int end = 0; end < 2; end++) {
            const float* now = end == 0 ? tip : base;
            float control[4][4];
            Copy4(control[0], now);
            Add(control[1], lean, now);
            Add(control[2], leanWas, was[end]);
            Copy4(control[3], was[end]);
            for (int i = 0; i < 3; i++)
                for (int k = 0; k < 4; k++)
                    made[i][end][k] = k == 3 ? control[0][3] * kWeights[i][0] + control[1][3] * kWeights[i][1] +
                                               control[2][3] * kWeights[i][2] + control[3][3] * kWeights[i][3]
                                             : control[0][k] * kWeights[i][0] + control[1][k] * kWeights[i][1] +
                                               control[2][k] * kWeights[i][2] + control[3][k] * kWeights[i][3];
        }
        for (int i = 2; i >= 0; i--) {
            made[i][0][3] = made[i][1][3] = 1.0f;
            Trail_Push(t, made[i][0], made[i][1]);
        }
    }
    Copy4(t->recent[t->recentAt][0], tip);
    Copy4(t->recent[t->recentAt][1], base);
    t->recentAt = (u8)((t->recentAt + 1) & 3);
    return 1;
}
int Trail_Start(Trail* t, u32 style, u32 hidden) {                          // 0x17E4B0
    t->style = (u8)style;
    t->hidden = (u8)hidden;
    return 1;
}
int Trail_Update(Trail* t) {                                                // 0x17E430
    if (t->base.type == 0)
        return 0;
    for (u32 i = t->oldest; i != t->next; i = (i + 1) & 0x3F) {
        if (t->strength[i] == 0)
            t->oldest = (u8)i;
        else
            t->strength[i] = (u8)(t->strength[i] - 0x20);
    }
    return 1;
}
// 0x17DBB0 / 0x17DE60: the ribbon — clear along the tips, as strong as each
// pair still is along the bases; a sprite stretched over each stretch, or plain.
void Trail_DrawRibbon(const Trail* t, const char* name) {
    struct TexVertex { s16 u, v; u32 _04; u32 rgba; float q; u16 x, y; u32 z; };
    struct FlatVertex { u32 rgba, q; u16 x, y; u32 z; };
    static struct { u64 tex0, prim; TexVertex v[128]; } textured;
    static struct { u64 prim; FlatVertex v[128]; } flat;
    u64 tex0 = 0;
    const u8* sprite = name != nullptr ? Fx_Sprite(name, &tex0) : nullptr;
    if (name != nullptr && sprite == nullptr)
        return;
    s16 us[2] = {0, 0}, vs[2] = {0, 0};
    if (sprite != nullptr) {
        const auto s16at = [](const u8* p) { s16 v; memcpy(&v, p, 2); return v; };
        us[0] = (s16)((s16at(sprite + 8) + s16at(sprite + 0xC) - 1) * 0x10);
        us[1] = (s16)(s16at(sprite + 8) * 0x10 + 8);
        vs[0] = (s16)((s16at(sprite + 10) + s16at(sprite + 0xE) - 1) * 0x10);
        vs[1] = (s16)(s16at(sprite + 10) * 0x10 + 8);
    }
    int n = 0;
    for (u32 raw = t->oldest + 1u;; raw++) {
        const u32 i = raw & 0x3F;
        if (i == t->next)
            break;
        for (int end = 0; end < 2; end++, n++) {
            s32 at[4];
            if (GsDevice_Project(*gGsDevice, at, t->ring[i][end]) == 0)
                return;                             // any point out of view: none of it is drawn
            const u32 rgba = end == 0 ? 0x808080u : (u32)t->strength[i] << 24 | 0x808080;
            if (sprite != nullptr) {
                TexVertex& v = textured.v[n];
                v = TexVertex{us[raw & 1], vs[end], 0, rgba, 1.0f, (u16)at[0], (u16)at[1], (u32)(at[2] >> 4)};
            } else {
                flat.v[n] = FlatVertex{rgba, 0, (u16)at[0], (u16)at[1], (u32)(at[2] >> 4)};
            }
        }
    }
    if (n < 4)
        return;
    Fx_SetBlend(0);
    if (sprite != nullptr) {
        textured.tex0 = tex0;
        textured.prim = 0x15C;
        GsDevice_DrawTexturedPrim(*gGsDevice, &textured, n);
    } else {
        flat.prim = 0x14C;
        GsDevice_DrawPrim(*gGsDevice, &flat, n);
    }
}
int Trail_Draw(Trail* t) {                                                  // 0x17DFD0
    if (t->hidden || t->oldest == ((t->next + 0x3Fu) & 0x3F))
        return 1;
    if (t->style == 2)
        Trail_DrawRibbon(t, P<const char>(0x2E0820u));          // "zanzo2"
    else if (t->style == 1)
        Trail_DrawRibbon(t, P<const char>(0x2E0830u));          // "zanzo1"
    else if (t->style == 0)
        Trail_DrawRibbon(t, nullptr);
    // (Style 3 draws each pair as a line, 0x12E730; and 0x17E150, a flash
    // "zanzo3" ahead of the newest pair when +0x8D5 is set — nothing here
    // sets it.)
    t->flash = 0;
    return 1;
}
void* Trail_Dtor(Trail* t, int flag) {                                      // 0x17E4C0
    if (t != nullptr) {
        sTrails.erase(guest::addr(t));
        t->base.vtable = 0x2EAC20u;
        if ((s16)flag > 0)
            MemFree(t);
    }
    return t;
}
GUEST_BIND(0x17E4C0, Trail_Dtor);
GUEST_BIND(0x17E4B0, Trail_Start);
GUEST_BIND(0x17E430, Trail_Update);
GUEST_BIND(0x17DFD0, Trail_Draw);

// ── The blade trails, for the sword holder (0x244940) ─────────────────────
void* Fx_NewTrail() {                                           // 0x17E520, started and listed
    Trail* t = static_cast<Trail*>(MemAlloc(sizeof(Trail)));
    if (t == nullptr)
        return nullptr;
    memset(t, 0, sizeof *t);
    Effect_Ctor(&t->base);
    t->base.vtable = 0x2EAFE0u;
    t->base.type = 0x10;
    t->next = 1;
    Trail_Start(t, 0, 0);
    AddToWorld(&t->base);
    sTrails.insert(guest::addr(t));
    return t;
}
int  Fx_TrailAlive(u32 trail) { return sTrails.count(trail) != 0; }
void Fx_TrailStyle(void* trail, int style) { Trail_Start(static_cast<Trail*>(trail), (u32)style, 0); }
void Fx_TrailAdd(void* trail, const float* matrix, const float* tip, const float* base) {
    Trail_Add(static_cast<Trail*>(trail), matrix, tip, base);
}
void Fx_TrailRelease(u32 trail) {                               // 0x172090
    if (sTrails.count(trail) != 0)
        P<Effect>(trail)->type = 0;
}

// ═══ What some accessories add to their wearer (0x2AC6F0–0x2AD3C0) ════════
// Drawn with the world (by the accessory object, src/game/accessories.cpp),
// depth-tested and additive, from the effect sprites.
static void Acc_Begin() {
    GsDevice_WriteRegister(*gGsDevice, 0x47, 0x53001);
    GsDevice_SetBlend(*gGsDevice, 0x8000000048ull);
}
static void Acc_End() {
    GsDevice_SetBlend(*gGsDevice, 0x8000000044ull);
    GsDevice_WriteRegister(*gGsDevice, 0x47, 0);        // (the original: the device's standing TEST value)
}
// 0x2AC9F0 (class 0x2ED6B0): a halo — ten faint rings stacked over the head.
void Fx_AccHalo(const float* head) {
    Acc_Begin();
    if (SpriteSet_Bind(P<SpriteSet>(*gEffectSprites)) != 0) {
        u64 tex0;
        const u8* sprite = Fx_Sprite(P<const char>(0x2E8760u), &tex0);     // "Halo"
        alignas(16) float at[4] = {0.0f, 0.0f, 0.3f, 1.0f};                 // 0x2DB610
        const float* ring = P<float>(0x2DB620u);        // the middle, then eight points round it and the first again
        const s32* us = P<s32>(0x2DB6C0u);
        const s32* vs = P<s32>(0x2DB6F0u);
        for (int i = 0; i < 10 && sprite != nullptr; i++, at[2] += 0.002f) {
            alignas(16) float centre[4];
            Apply(centre, head, at);
            s32 points[10][4];
            u8 corner[10];
            int outside = 0;
            for (int k = 0; k < 10; k++) {
                alignas(16) float p[4];
                ScaleXYZ(p, ring + k * 4, 0.15f);
                Add(p, centre, p);
                if (GsDevice_Project(*gGsDevice, points[k], p) == 0)
                    outside++;
                corner[k] = (u8)((us[k] & 1) | (vs[k] & 1) << 1);
            }
            if (outside < 10)
                DrawProjected(tex0, 0x15D, sprite, points, corner, 10, 0x30408080u);
        }
    }
    Acc_End();
}
// A flat sprite of the effect sheet over four points round `pos`.
static void Acc_Quad(const char* name, const float corners[4][4], const float* frame, const float* pos, u32 rgba) {
    QuadParams quad;
    memset(&quad, 0, sizeof quad);
    quad.flags = 1;
    quad.rgba = rgba;
    int outside = 0;
    for (int i = 0; i < 4; i++) {
        alignas(16) float p[4];
        Apply(p, frame, corners[i]);
        Add(p, pos, p);
        if (GsDevice_Project(*gGsDevice, quad.at[i], p) == 0)
            outside++;
    }
    if (outside < 4)
        SpriteSet_DrawQuad(*gEffectSprites, SpriteSet_FindByName(*gEffectSprites, name), &quad);
}
// 0x2AD180 (class 0x2ED6F0): two candle flames at the temples, turned to
// the camera about the upright; their heights and strengths flicker.
void Fx_AccFlames(const float* head, const float* heights, const u32* colours) {
    static const float kAt[2][4] = {{0.06f, -0.08f, 0.28f, 1.0f}, {-0.06f, -0.08f, 0.28f, 1.0f}};   // 0x2DB550
    Acc_Begin();
    if (SpriteSet_Bind(P<SpriteSet>(*gEffectSprites)) != 0)
        for (int i = 0; i < 2; i++) {
            alignas(16) float pos[4], eye[4], to[4];
            Apply(pos, head, kAt[i]);
            Copy4(eye, P<float>(*P<u32>(0x2EE528u) + 0x40));           // the camera's eye
            eye[1] = pos[1];
            // 0x2B2E40: a frame that looks from the flame to the eye.
            alignas(16) float frame[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            Sub(to, eye, pos);
            Normalize(to, to);
            const float up[4] = {0.0f, 1.0f, 0.0f, 0.0f};
            frame[0] = up[1] * to[2] - up[2] * to[1];
            frame[1] = up[2] * to[0] - up[0] * to[2];
            frame[2] = up[0] * to[1] - up[1] * to[0];
            frame[4] = to[1] * frame[2] - to[2] * frame[1];
            frame[5] = to[2] * frame[0] - to[0] * frame[2];
            frame[6] = to[0] * frame[1] - to[1] * frame[0];
            Normalize(&frame[4], &frame[4]);
            frame[8] = to[0];
            frame[9] = to[1];
            frame[10] = to[2];
            const float corners[4][4] = {{0.02f, heights[i], 0, 0}, {0.02f, -0.02f, 0, 0},     // 0x2DB570
                                         {-0.02f, heights[i], 0, 0}, {-0.02f, -0.02f, 0, 0}};
            Acc_Quad(P<const char>(0x2E8780u), corners, frame, pos, colours[i]);                // "flame"
        }
    Acc_End();
}
// ═══ A candle's flame on the map (0x20 bytes, vtable 0x2ED770, 0x2B2AC0–
// 0x2B2E00): object type 8 with a = 1 ═══════════════════════════════════════
// The accessory's flame, larger: the sprite `flame` standing at its place,
// turned to the camera about the upright, its height and strength new
// every frame.
struct CandleFlame {
    Effect base;
    u32 _c;
    float pos[4];           // +0x10
};
void* CandleFlame_Dtor(CandleFlame* self, int flag) {           // 0x2B2DA0
    if (self != nullptr && flag > 0)
        MemFree(self);
    return self;
}
int CandleFlame_Start(CandleFlame* self, const float* pos, u32) {       // 0x2B2D80
    Copy4(self->pos, pos);
    return 1;
}
int CandleFlame_Update(CandleFlame*) { return 1; }              // 0x2B2D70
int CandleFlame_Draw(CandleFlame* self) {                       // 0x2B2AC0
    const float height = (float)(Rand_Next() % 30 + 70) * 0.002f;
    const float corners[4][4] = {{0.05f, height, 0, 0}, {0.05f, 0, 0, 0},      // 0x2DB850
                                 {-0.05f, height, 0, 0}, {-0.05f, 0, 0, 0}};
    alignas(16) float eye[4], to[4];
    Copy4(eye, P<float>(*P<u32>(0x2EE528u) + 0x40));
    eye[1] = self->pos[1];
    alignas(16) float frame[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    Sub(to, eye, self->pos);
    Normalize(to, to);
    frame[0] = to[2];                                           // up × to
    frame[2] = -to[0];
    frame[4] = to[1] * frame[2] - to[2] * frame[1];
    frame[5] = to[2] * frame[0] - to[0] * frame[2];
    frame[6] = to[0] * frame[1] - to[1] * frame[0];
    Normalize(&frame[4], &frame[4]);
    frame[8] = to[0];
    frame[9] = to[1];
    frame[10] = to[2];
    const u32 strength = (u32)(Rand_Next() % 30 + 70) * 0x80 / 100;
    Fx_SetBlend(0);
    Acc_Quad(P<const char>(0x2E8900u), corners, frame, self->pos, strength << 24 | 0x808080);  // "flame"
    return 1;
}
GUEST_BIND(0x2B2DA0, CandleFlame_Dtor);
GUEST_BIND(0x2B2D80, CandleFlame_Start);
GUEST_BIND(0x2B2D70, CandleFlame_Update);
GUEST_BIND(0x2B2AC0, CandleFlame_Draw);

// ═══ Type 0x18: a technique learnt (0x186FE0–0x187550, 0x60 bytes, vtable
// 0x2EB0D0) ═══════════════════════════════════════════════════════════════
// A glint at the eye of whoever's sword has just learnt something: the
// sprite `mikiru`, additive, twice at right angles — a cross — facing the
// viewer and spinning 0.3 a frame; it opens to 0.3 over the first frames
// and closes again from the sixteenth-last.  It rides bone 21 of the
// skeleton (the first of the face's) and lights what is near.
struct Glint {
    Effect base;            // type 0x18; life = frames left (30)
    u32 _0c;
    float spin[16];         // +0x10
    float size;             // +0x50
    u32 owner;              // +0x54  CharaParams*
    u32 light;              // +0x58
    u32 _5c;
};
static void TurnZ(float* m, float angle) {          // sceVu0RotMatrixZ (0x1074C8)
    const float c = std::cos(angle), s = std::sin(angle);
    for (int row = 0; row < 4; row++) {
        const float x = m[row * 4 + 0], y = m[row * 4 + 1];
        m[row * 4 + 0] = x * c - y * s;
        m[row * 4 + 1] = x * s + y * c;
    }
}
int Glint_Start(Glint* self, u32 owner, u32) {                  // 0x187450
    Matrix_Identity(self->spin);
    self->base.life = 30;
    self->size = 0.01f;
    self->owner = owner;
    self->light = 0;
    return 1;
}
int Glint_Update(Glint* self) {                                 // 0x187380
    TurnZ(self->spin, 0.3f);
    const s32 was = self->base.life--;
    if (was < 0x10) {
        if (self->size <= 0.0f)
            return 0;
        self->size -= 0.04f;
    } else if (self->size < 0.3f) {
        self->size += 0.04f;
    }
    return 1;
}
int Glint_Draw(Glint* self) {                                   // 0x186FE0
    alignas(16) float pos[4] = {0.0f, 0.0f, 0.0f, 1.0f};        // 0x2D8010
    if (self->owner != 0) {
        const float kAt[4] = {0.0f, 0.0f, 0.05f, 1.0f};         // 0x2D8020
        const u32 model = P<CharaParams>(self->owner)->link[1];
        if (model == 0 || P<u32>(model)[1] == 0)
            return 1;
        Apply(pos, P<float>(P<u32>(model)[1] + 0xD20), kAt);
    }
    {   // (the original keeps one light and moves it: 0x166B80, flags 5)
        const float colour[4] = {3.3f * self->size, 3.3f * self->size, 3.3f * self->size, 0.0f};    // 0x2D8030
        Lights_Add(pos, colour, 1, 1);
    }
    static const float kCorners[4][4] = {{1.0f, -0.25f, 0, 1}, {1.0f, 0.25f, 0, 1},     // 0x2D8040
                                         {-1.0f, -0.25f, 0, 1}, {-1.0f, 0.25f, 0, 1}};
    Fx_SetBlend(0);
    alignas(16) float turned[16];
    memcpy(turned, self->spin, sizeof turned);
    for (int arm = 0; arm < 2; arm++) {
        TurnZ(turned, 1.5707964f);
        QuadParams quad;
        memset(&quad, 0, sizeof quad);
        quad.flags = 1;
        quad.rgba = 0x80808080u;
        for (int i = 0; i < 4; i++) {
            alignas(16) float p[4], c[4];
            Scale4(c, kCorners[i], self->size);
            c[3] = 1.0f;
            Apply(p, turned, c);
            Apply(p, InverseView(), p);
            Add(p, p, pos);
            p[3] = 1.0f;
            if (GsDevice_Project(*gGsDevice, quad.at[i], p) == 0)
                return 1;
        }
        SpriteSet_DrawQuad(*gEffectSprites, SpriteSet_FindByName(*gEffectSprites, "mikiru  "), &quad);
    }
    return 1;
}
GUEST_BIND(0x187450, Glint_Start);
GUEST_BIND(0x187380, Glint_Update);
GUEST_BIND(0x186FE0, Glint_Draw);
static const guest::Binder guest_bind_Glint_Dtor{0x1874D0, reinterpret_cast<void*>(Mote_Dtor), "Glint_Dtor"};
// What the sword holder does when a technique is learnt (0x244F90, 0x245030).
void Fx_TechniqueGlint(CharaParams* owner) {
    if (owner == nullptr)
        return;
    Glint* glint = static_cast<Glint*>(MemAlloc(0x60));
    memset(glint, 0, 0x60);
    Effect_Ctor(&glint->base);
    glint->base.vtable = 0x2EB0D0u;
    glint->base.type = 0x18;
    Glint_Start(glint, guest::addr(owner), 0);
    AddToWorld(&glint->base);
}

// ═══ A sword worn down a point (0x24C1B0–0x24C640): thirty shards of steel
// from each hand that holds a blade ═══════════════════════════════════════
// The maker (0x20 bytes, vtable 0x2EC8C0) is gone the next frame; a shard
// (0x80 bytes, vtable 0x2EC8E0) is a triangle of the sprite `katana`,
// 0.04 across, thrown out and up, falling, tumbling, for 17–20 frames, its
// brightness running 7 … 0 again and again.
struct Shard {
    Effect base;            // life = frames left
    u32 _0c;
    float pos[4];           // +0x10
    float vel[4];           // +0x20
    float turn[16];         // +0x30
    float size;             // +0x70
    u32 _74[3];
};
struct Shards {
    Effect base;
    u32 _0c;
    float pos[4];           // +0x10
};
int Shard_Start(Shard* self, const float* pos, u32) {           // 0x24C580
    self->base.life = 0x14 - (s32)((u32)Rand_Next() & 3);
    Copy4(self->pos, pos);
    RandomUnit(self->vel);
    const float speed = RandomUnitNumber() * 0.05f + 0.1f;
    for (int k = 0; k < 3; k++)
        self->vel[k] *= speed;
    self->vel[1] += 0.2f;
    self->size = 0.04f;
    Matrix_Identity(self->turn);
    return 1;
}
int Shard_Update(Shard* self) {                                 // 0x24C4E0
    self->vel[1] -= 0.0245f;
    Add(self->pos, self->pos, self->vel);
    Scale4(self->vel, self->vel, 0.98f);
    const float kSpin[4] = {0.4f, 0.6f, 0.0f, 0.0f};            // 0x2DA440
    RotateBy(self->turn, kSpin);
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Shard_Draw(Shard* self) {                                   // 0x24C380
    static const float kCorners[3][4] = {{1, -1, 0, 1}, {1, 1, 0, 1}, {-1, -1, 0, 1}};      // 0x2DA450
    QuadParams quad;
    memset(&quad, 0, sizeof quad);
    quad.flags = 3;
    const u32 step = (u32)(self->base.life % 8);
    quad.rgba = step << 4 | step << 20 | step << 12 | 0x80000000u;
    int outside = 0;
    for (int i = 0; i < 3; i++) {
        alignas(16) float p[4], c[4];
        Scale4(c, kCorners[i], self->size);
        c[3] = 1.0f;
        Apply(p, self->turn, c);
        Add(p, p, self->pos);
        p[3] = 1.0f;
        if (GsDevice_Project(*gGsDevice, quad.at[i], p) == 0)
            outside++;
    }
    if (outside < 3) {
        Fx_SetBlend(2);
        SpriteSet_DrawQuad(*gEffectSprites, SpriteSet_FindByName(*gEffectSprites, "katana  "), &quad);
    }
    return 1;
}
GUEST_BIND(0x24C580, Shard_Start);
GUEST_BIND(0x24C4E0, Shard_Update);
GUEST_BIND(0x24C380, Shard_Draw);
static const guest::Binder guest_bind_Shard_Dtor{0x24C640, reinterpret_cast<void*>(Mote_Dtor), "Shard_Dtor"};
int Shards_Start(Shards* self, const float* pos, u32) {         // 0x24C1F0
    self->base.life = 0;
    Copy4(self->pos, pos);
    for (int i = 0; i < 30; i++) {
        Shard* shard = static_cast<Shard*>(MemAlloc(0x80));
        memset(shard, 0, 0x80);
        Effect_Ctor(&shard->base);
        shard->base.vtable = 0x2EC8E0u;
        Shard_Start(shard, self->pos, 0);
        AddToWorld(&shard->base);
    }
    return 1;
}
int Shards_Update(Shards* self) {                               // 0x24C1C0
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Shards_Draw(Shards*) { return 1; }                          // 0x24C1B0
GUEST_BIND(0x24C1F0, Shards_Start);
GUEST_BIND(0x24C1C0, Shards_Update);
GUEST_BIND(0x24C1B0, Shards_Draw);
static const guest::Binder guest_bind_Shards_Dtor{0x24C2E0, reinterpret_cast<void*>(Mote_Dtor), "Shards_Dtor"};
// What the sword holder does when the sword in hand loses a point (0x2452F0).
void Fx_SwordShards(const float* pos) {
    alignas(16) float at[4];
    memcpy(at, pos, 16);
    Shards* maker = static_cast<Shards*>(MemAlloc(0x20));
    memset(maker, 0, 0x20);
    Effect_Ctor(&maker->base);
    maker->base.vtable = 0x2EC8C0u;
    Shards_Start(maker, at, 0);
    AddToWorld(&maker->base);
}

// ═══ Steam, smoke and fire on the map (0x23F6A0–0x241FE0): object type 8
// with a = 0; `b` picks which ═══════════════════════════════════════════════
// An emitter (0x30 bytes, vtable 0x2EC770) at the place throws out puffs: a
// square of the sprite `kemuri` (smoke) or `particle`, facing the viewer,
// that drifts with the wind, rises, swells and fades.  Five classes of puff
// over one layout: smoke (0x2EC810, 0x50 bytes), light steam (0x2EC7F0),
// heavy smoke (0x2EC7D0), the general one whose numbers the emitter gives
// (0x2EC7B0, 0x60 bytes: flames, hot smoke, steam) and a burst (0x2EC790);
// and a spark (0x2EC750, 0x40 bytes).
struct Smoke {
    Effect base;            // life = frames left
    u32 _0c;
    float pos[4];           // +0x10
    float vel[4];           // +0x20
    float size;             // +0x30
    float lift;             // +0x34  (0x2EC7B0) added to the rise every frame
    s32 total;              // +0x38
    s32 blend;              // +0x3C
    u32 colour;             // +0x40
    u32 _44[3];
    s32 sprite;             // +0x50  (0x2EC7B0)
    u32 _54[3];
};
struct SmokeSpark {
    Effect base;
    u32 _0c;
    float pos[4];           // +0x10
    float vel[4];           // +0x20
    float size;             // +0x30
    s32 total;              // +0x34
    s32 blend;              // +0x38
    u32 colour;             // +0x3C
};
struct PuffOrder {          // what an emitter hands the general puff
    const float* pos;
    float size, rise, lift, scatter;
    s32 kind;               // 1: the sprite `particle`, else `kemuri`
    u32 colour;
    s32 blend, life;
};
void* Smoke_Dtor(Effect* self, int flag) {      // 0x241CA0, 0x241FE0, 0x241F70, 0x240EE0, 0x241D00, 0x23FA50, 0x241010
    if (self != nullptr && flag > 0)
        MemFree(self);
    return self;
}
inline void Smoke_Scatter(Smoke* self, float place, float speed, float rise) {
    alignas(16) float r[4];
    RandomUnit(r);
    Scale4(r, r, place);
    Add(self->pos, self->pos, r);
    RandomUnit(r);
    Scale4(r, r, speed);
    r[1] += rise;
    Add(self->vel, self->vel, r);
}
void Smoke_Start(Smoke* self, const float* pos, s32 blend) {    // 0x241BC0
    self->blend = blend;
    Copy4(self->pos, pos);
    memset(self->vel, 0, 16);
    self->base.life = self->total = (Rand_Next() & 0xF) + 30;
    self->size = 0.3f;
    Smoke_Scatter(self, 0.05f, 0.005f, 0.02f);
}
void Steam_Start(Smoke* self, const float* pos, float size, s32 blend) {    // 0x2417A0
    self->blend = blend;
    Copy4(self->pos, pos);
    memset(self->vel, 0, 16);
    self->base.life = self->total = (Rand_Next() & 0xF) + 30;
    self->size = size;
    Smoke_Scatter(self, size * 0.16f, size * 0.016f, size * 0.6f);
}
void HeavySmoke_Start(Smoke* self, const float* pos, float size) {          // 0x241570
    Copy4(self->pos, pos);
    memset(self->vel, 0, 16);
    self->size = size;
    self->base.life = self->total = (Rand_Next() & 0x3F) + 0x78;
    self->blend = 2;
    self->colour = 0;
    Smoke_Scatter(self, size * 0.16f, size * 0.016f, 0.02f);
}
void Puff_StartOrder(Smoke* self, const PuffOrder& o) {                     // 0x241350
    Copy4(self->pos, o.pos);
    memset(self->vel, 0, 16);
    self->base.life = self->total = o.life + (Rand_Next() & 0xF);
    self->size = o.size;
    self->blend = o.blend;
    self->colour = o.colour;
    self->lift = o.lift;
    Smoke_Scatter(self, o.size * o.scatter, o.rise * 0.4f, o.rise);
    self->sprite = SpriteSet_FindByName(*gEffectSprites, P<const char>(o.kind == 1 ? 0x2E6560u : 0x2E6550u));
}
void Burst_Start(Smoke* self, const PuffOrder& o) {                         // 0x240F50
    Puff_StartOrder(self, o);
    self->base.life = self->total = o.life;
    RandomUnit(self->vel);
    Scale3(self->vel, 0.1f);
}
inline int Smoke_Step(Smoke* self, bool sway, float lift, float grow) {
    const float* wind = Mgr()->flash;                           // (+0x50: the wind)
    for (int i = 0; i < 3; i++)
        self->pos[i] += wind[i] * 0.1f + self->vel[i];
    if (sway)
        self->pos[0] += sinf((float)(self->base.life * 10) * 3.14f / 180.0f) * 0.01f;
    Scale3(self->vel, 0.99f);
    self->vel[1] += lift;
    self->size += grow;
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int Smoke_Update(Smoke* self) { return Smoke_Step(self, true, 0.001f, 0.002f); }            // 0x241A90
int Steam_Update(Smoke* self) { return Smoke_Step(self, true, 0.0001f, 0.002f); }           // 0x241670
int HeavySmoke_Update(Smoke* self) { return Smoke_Step(self, false, 0.001f, 0.005f); }      // 0x2414A0
int Puff_UpdateOrder(Smoke* self) { return Smoke_Step(self, false, self->lift, 0.002f); }   // 0x241290
int Burst_Update(Smoke* self) {                                                             // 0x241D80
    for (int i = 0; i < 3; i++)
        self->pos[i] += self->vel[i];
    Scale3(self->vel, 0.6f);
    self->size += 0.01f;
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
// A square of `size` facing the viewer at `pos` (corners 0x2DA1D0 / 0x2DA210 / 0x2DA250 / 0x2DA190).
inline void Smoke_Quad(const float* pos, float size, int sprite, u32 flags, u32 rgba) {
    static const float kCorners[4][4] = {{1, -1, 0, 1}, {1, 1, 0, 1}, {-1, -1, 0, 1}, {-1, 1, 0, 1}};
    QuadParams quad;
    memset(&quad, 0, sizeof quad);
    quad.flags = flags;
    quad.rgba = rgba;
    int outside = 0;
    for (int i = 0; i < 4; i++) {
        alignas(16) float p[4];
        ScaleXYZ(p, kCorners[i], size);
        Apply(p, InverseView(), p);
        Add(p, p, pos);
        if (GsDevice_Project(*gGsDevice, quad.at[i], p) == 0)
            outside++;
    }
    if (outside < 3)
        SpriteSet_DrawQuad(*gEffectSprites, sprite, &quad);
}
inline u32 Smoke_Fade(const Smoke* self) {      // in over ten frames, then out
    s32 alpha = self->base.life * 0x60 / self->total;
    if (self->total - 10 < self->base.life)
        alpha = (self->total - self->base.life) * 0x60 / 10;
    return self->colour | (u32)alpha << 24;
}
int Smoke_Draw(Smoke* self) {                                   // 0x2418A0
    Fx_SetBlend(self->blend);
    Smoke_Quad(self->pos, self->size, SpriteSet_FindByName(*gEffectSprites, P<const char>(0x2E6550u)), 1,
               Smoke_Fade(self));                               // "kemuri"
    return 1;
}
int Puff_DrawOrder(Smoke* self) {                               // 0x2410C0
    Fx_SetBlend(self->blend);
    Smoke_Quad(self->pos, self->size, self->sprite, 1, Smoke_Fade(self));
    return 1;
}
int Burst_Draw(Smoke* self) {                                   // 0x241E10
    Fx_SetBlend(self->blend);
    Smoke_Quad(self->pos, self->size, self->sprite, 1,
               self->colour | (u32)((self->base.life << 7) / self->total) << 24);
    return 1;
}
void SmokeSpark_Start(SmokeSpark* self, const float* pos, s32 blend) {      // 0x23F970
    self->blend = blend;
    Copy4(self->pos, pos);
    memset(self->vel, 0, 16);
    self->base.life = self->total = (Rand_Next() & 7) + 20;
    self->size = 0.1f;
    alignas(16) float r[4];
    RandomUnit(r);
    Scale4(r, r, 0.05f);
    Add(self->pos, self->pos, r);
    RandomUnit(r);
    Scale4(r, r, 0.005f);
    r[1] += 0.02f;
    Add(self->vel, self->vel, r);
}
int SmokeSpark_Update(SmokeSpark* self) {                       // 0x23F820
    const float* wind = Mgr()->flash;
    for (int i = 0; i < 3; i++)
        self->pos[i] += wind[i] * 0.1f + self->vel[i];
    self->pos[0] += sinf((float)(self->base.life * 10) * 3.14f / 180.0f) * 0.01f;
    self->vel[1] += 0.002f;
    self->size += self->size < 0.3f ? 0.1f : 0.001f;
    const s32 was = self->base.life;
    if (was != 0)
        self->base.life--;
    return was != 0;
}
int SmokeSpark_Draw(SmokeSpark* self) {                         // 0x23F6A0
    Fx_SetBlend(self->blend);
    Smoke_Quad(self->pos, self->size, SpriteSet_FindByName(*gEffectSprites, P<const char>(0x2E6540u)), 0,
               self->colour | (u32)((self->base.life << 7) / self->total) << 24);      // "particle"
    return 1;
}
GUEST_BIND(0x241CA0, Smoke_Dtor);
static const guest::Binder guest_bind_Smoke_Dtor_1{0x241FE0, reinterpret_cast<void*>(Smoke_Dtor), "Smoke_Dtor"};
static const guest::Binder guest_bind_Smoke_Dtor_2{0x241F70, reinterpret_cast<void*>(Smoke_Dtor), "Smoke_Dtor"};
static const guest::Binder guest_bind_Smoke_Dtor_3{0x240EE0, reinterpret_cast<void*>(Smoke_Dtor), "Smoke_Dtor"};
static const guest::Binder guest_bind_Smoke_Dtor_4{0x241D00, reinterpret_cast<void*>(Smoke_Dtor), "Smoke_Dtor"};
static const guest::Binder guest_bind_Smoke_Dtor_5{0x23FA50, reinterpret_cast<void*>(Smoke_Dtor), "Smoke_Dtor"};
static const guest::Binder guest_bind_Smoke_Dtor_6{0x241010, reinterpret_cast<void*>(Smoke_Dtor), "Smoke_Dtor"};
GUEST_BIND(0x241A90, Smoke_Update);
GUEST_BIND(0x241670, Steam_Update);
GUEST_BIND(0x2414A0, HeavySmoke_Update);
GUEST_BIND(0x241290, Puff_UpdateOrder);
GUEST_BIND(0x241D80, Burst_Update);
GUEST_BIND(0x2418A0, Smoke_Draw);
GUEST_BIND(0x2410C0, Puff_DrawOrder);
GUEST_BIND(0x241E10, Burst_Draw);
GUEST_BIND(0x23F820, SmokeSpark_Update);
GUEST_BIND(0x23F6A0, SmokeSpark_Draw);

static void* Smoke_New(u32 size, u32 vtable) {
    Effect* e = static_cast<Effect*>(MemAlloc(size));
    if (e == nullptr)
        return nullptr;
    memset(e, 0, size);
    Effect_Ctor(e);
    e->vtable = vtable;
    return e;
}
inline void Emit_Order(const PuffOrder& o, u32 vtable = 0x2EC7B0u) {
    if (Smoke* e = static_cast<Smoke*>(Smoke_New(0x60, vtable))) {
        if (vtable == 0x2EC790u)
            Burst_Start(e, o);
        else
            Puff_StartOrder(e, o);
        AddToWorld(&e->base);
    }
}

struct SmokeEmitter {
    Effect base;            // life: a counter that runs 60 … 0 round and round
    u32 _0c;
    float pos[4];           // +0x10
    s32 variety;            // +0x20
    s32 fresh;              // +0x24  not stepped yet
    u32 _28[2];
};
// A place near the emitter: `spread` each way, on its level unless `flat` is given.
inline void Emit_Near(float* out, const SmokeEmitter* self, float spread, float flat = 0.0f) {
    RandomUnit(out);
    Scale4(out, out, spread);
    out[1] *= flat;
    Add(out, self->pos, out);
}
int SmokeEmitter_Update(SmokeEmitter* self) {                   // 0x23FB10
    alignas(16) float at[4];
    const s32 t = self->base.life;
    switch (self->variety) {
    case 0:                                                     // 0x240B50: smoke and sparks
        if (t % 8 == 0) {
            Emit_Near(at, self, 0.15f);
            if (Smoke* e = static_cast<Smoke*>(Smoke_New(0x50, 0x2EC810u))) {
                Smoke_Start(e, at, 2);
                AddToWorld(&e->base);
            }
        }
        if (t % 3 == 0) {
            Emit_Near(at, self, 0.15f);
            if (SmokeSpark* e = static_cast<SmokeSpark*>(Smoke_New(0x40, 0x2EC750u))) {
                e->colour = 0x404080;                           // (0x23FAB0)
                SmokeSpark_Start(e, at, 0);
                AddToWorld(&e->base);
            }
        }
        break;
    case 1:                                                     // steam
        if ((t & 1) == 0) {
            Emit_Near(at, self, 0.15f);
            if (Smoke* e = static_cast<Smoke*>(Smoke_New(0x50, 0x2EC810u))) {
                Smoke_Start(e, at, 2);
                e->colour = 0xFFFFFF;
                AddToWorld(&e->base);
            }
        }
        break;
    case 2:                                                     // 0x23FFD0: a wisp of steam
        if ((t & 1) == 0) {
            Emit_Near(at, self, 0.0075f);
            if (Smoke* e = static_cast<Smoke*>(Smoke_New(0x50, 0x2EC7F0u))) {
                Steam_Start(e, at, 0.05f, 2);
                e->colour = 0xFFFFFF;
                AddToWorld(&e->base);
            }
        }
        break;
    case 3:                                                     // 0x23FD10: a column of black smoke
        if (self->fresh != 0) {                                 // the column is there when first seen
            for (int age = 0; age < 110; age += 2) {
                Emit_Near(at, self, 0.15f);
                if (Smoke* e = static_cast<Smoke*>(Smoke_New(0x50, 0x2EC7D0u))) {
                    HeavySmoke_Start(e, at, 0.2f);
                    AddToWorld(&e->base);
                    for (int i = age; i != 0; i--)
                        HeavySmoke_Update(e);
                }
            }
            self->fresh = 0;
            return 1;
        }
        if ((t & 1) == 0) {
            Emit_Near(at, self, 0.15f);
            if (Smoke* e = static_cast<Smoke*>(Smoke_New(0x50, 0x2EC7D0u))) {
                HeavySmoke_Start(e, at, 0.2f);
                AddToWorld(&e->base);
            }
        }
        break;
    case 4:                                                     // 0x240130: a fire, and its smoke above
        if ((t & 1) == 0) {
            Emit_Near(at, self, 0.2f, 0.1f);
            at[1] += 0.3f;
            Emit_Order({at, 0.4f, 0.02f, 0.0001f, 0.2f, 0, 0x202020, 2, 30});
            at[1] -= 0.3f;
            Emit_Order({at, 0.4f, 0.02f, 0.0001f, 0.2f, 0, 0x40FF, 0, 30});
        }
        break;
    case 5:                                                     // 0x2405A0: a torch
        Emit_Near(at, self, 0.05f, 1.0f);
        if ((t & 1) == 0) {
            at[1] += 0.3f;
            Emit_Order({at, 0.2f, 0.02f, 0.0001f, 0.16f, -1, 0x202020, 2, 15});
        } else {
            Emit_Order({at, 0.16f, 0.02f, 0.0001f, 0.16f, -1, 0x50FF, 0, 15});
        }
        if (t % 3 == 0) {
            Emit_Near(at, self, 0.05f, 1.0f);
            at[1] += 0.1f;
            Emit_Order({at, 0.14f, 0.02f, 0.0001f, 0.16f, 1, 0x4040FF, 0, 30});
        }
        break;
    case 6:                                                     // 0x2409F0: the bath house's steam
        if ((t & 1) == 0)
            Emit_Order({self->pos, 0.4f, 0.02f, 0.0001f, 0.16f, -1, 0xFFFFFF, 2, 25});
        break;
    case 7:                                                     // 0x240450: a field on fire
        if ((t & 1) == 0)
            Emit_Order({self->pos, 0.7f, 0.04f, 0.0001f, 0.25f, 0, 0x40FF, 0, 15});
        break;
    case 8:                                                     // 0x240D60: one burst, and it is gone
        for (int i = 0; i < 10; i++)
            Emit_Order({self->pos, 0.2f, 0.0f, 0.0f, 0.0f, -1, 0x85ECFF, 2, 0x20}, 0x2EC790u);
        return 0;
    default:
        return 0;
    }
    self->base.life = t == 0 ? 60 : t - 1;
    return 1;
}
int SmokeEmitter_Draw(SmokeEmitter*) { return 1; }              // 0x23FB00
GUEST_BIND(0x23FB10, SmokeEmitter_Update);
GUEST_BIND(0x23FB00, SmokeEmitter_Draw);

// ═══ A firefly (0x150 bytes, vtable 0x2ED8C0, 0x2BAAA0–0x2BAF10): object
// type 8 with a = 3 ═════════════════════════════════════════════════════════
// It wanders — a random push every frame, a pull back home that grows with
// the distance, a sudden slowing now and then — and is drawn as its last
// fifteen places: additive dots that glow and dim together.
struct Firefly {
    Effect base;            // life: the glow's phase, degrees
    u32 _0c;
    float home[4];          // +0x10
    float pos[4];           // +0x20
    float vel[4];           // +0x30
    u32 oldest, next;       // +0x40  into the ring
    u32 _48[2];
    float trail[16][4];     // +0x50
};
int Firefly_Start(Firefly* self, const float* pos, u32) {       // 0x2BAE50
    Copy4(self->pos, pos);
    Copy4(self->home, pos);
    memset(self->vel, 0, 16);
    self->base.life = 0;
    self->oldest = self->next = 0;
    return 1;
}
int Firefly_Update(Firefly* self) {                             // 0x2BAC90
    alignas(16) float v[4];
    Add(self->pos, self->pos, self->vel);
    RandomUnit(v);
    Scale4(v, v, 0.005f);
    Add(self->vel, self->vel, v);
    Sub(v, self->home, self->pos);
    const float away = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    Normalize(v, v);
    Scale4(v, v, away * 0.004f);
    Add(self->vel, self->vel, v);
    self->base.life = (self->base.life + (Rand_Next() & 7) + 7) % 360;
    Scale4(self->vel, self->vel, (Rand_Next() & 0xF) == 0 ? 0.7f : 0.98f);
    Copy4(self->trail[self->next], self->pos);
    self->next = (self->next + 1) & 0xF;
    if (self->oldest == self->next)
        self->oldest = (self->oldest + 1) & 0xF;
    return 1;
}
int Firefly_Draw(Firefly* self) {                               // 0x2BAAA0
    // The original hands each place, projected, to SpriteSet_DrawRotated
    // {x, y, z / 16, scale 12}: a sprite of a fixed size on the screen at the
    // place's depth.  The native stand-in for that function has no depth of
    // the 3D scene's kind, so the same square goes through the quad path.
    const u32 glow = (u32)(s32)(sinf((float)self->base.life * 3.1415927f / 180.0f) * 6.0f + 12.0f);
    u64 tex0 = 0;
    const u8* record = Fx_Sprite(P<const char>(0x2E8C50u), &tex0);                         // "dot"
    if (record == nullptr)
        return 1;
    const int dot = SpriteSet_FindByName(*gEffectSprites, P<const char>(0x2E8C50u));
    const s32 halfW = (*reinterpret_cast<const s16*>(record + 0x0C) >> 1) * 12;
    const s32 halfH = (*reinterpret_cast<const s16*>(record + 0x0E) >> 1) * 12;
    for (u32 i = self->oldest; i != self->next; i = (i + 1) & 0xF) {
        Fx_SetBlend(0);
        s32 at[4];
        if (GsDevice_Project(*gGsDevice, at, self->trail[i]) == 0)
            continue;
        QuadParams quad;
        memset(&quad, 0, sizeof quad);
        quad.rgba = glow << 24 | 0x0B2020;
        for (int c = 0; c < 4; c++) {
            quad.at[c][0] = at[0] + (c < 2 ? halfW : -halfW);
            quad.at[c][1] = at[1] + ((c & 1) == 0 ? halfH : -halfH);
            quad.at[c][2] = at[2];
            quad.at[c][3] = at[3];
        }
        SpriteSet_DrawQuad(*gEffectSprites, dot, &quad);
    }
    return 1;
}
GUEST_BIND(0x2BAE50, Firefly_Start);
GUEST_BIND(0x2BAC90, Firefly_Update);
GUEST_BIND(0x2BAAA0, Firefly_Draw);
static const guest::Binder guest_bind_Smoke_Dtor_7{0x2BAEB0, reinterpret_cast<void*>(Smoke_Dtor), "Smoke_Dtor"};
void Fx_PlaceFirefly(const float* pos) {
    if (Firefly* e = static_cast<Firefly*>(Smoke_New(0x150, 0x2ED8C0u))) {
        Firefly_Start(e, pos, 0);
        AddToWorld(&e->base);
    }
}

// (ItemDrops_Place, case 8, makes the effects of the place in place.)
void Fx_PlaceSmoke(const float* pos, int variety) {
    SmokeEmitter* e = static_cast<SmokeEmitter*>(Smoke_New(0x30, 0x2EC770u));
    if (e == nullptr)
        return;
    e->base.life = 60;                                          // 0x241070
    e->fresh = 1;
    Copy4(e->pos, pos);                                         // 0x240FB0
    if (variety != 0) {
        e->variety = variety;
        if (variety == 1)
            e->base.life = 5;
    }
    AddToWorld(&e->base);
}
void Fx_PlaceCandle(const float* pos) {
    CandleFlame* e = static_cast<CandleFlame*>(MemAlloc(0x20));
    if (e == nullptr)
        return;
    memset(e, 0, 0x20);
    Effect_Ctor(&e->base);                                      // 0x2B2E00
    e->base.vtable = 0x2ED770u;
    CandleFlame_Start(e, pos, 0);
    AddToWorld(&e->base);
}

// 0x2ACCB0 (class 0x2ED6D0): two trails of sparks behind the shoulders —
// the last 32 places of two points of the head, the newest brightest.
void Fx_AccTrail(const float (*left)[4], const float (*right)[4], u32 newest, u32 oldest) {
    static const float kCorners[4][4] = {{0.015f, -0.015f, 0, 0}, {0.015f, 0.015f, 0, 0},      // 0x2DB5D0
                                         {-0.015f, -0.015f, 0, 0}, {-0.015f, 0.015f, 0, 0}};
    Acc_Begin();
    if (SpriteSet_Bind(P<SpriteSet>(*gEffectSprites)) != 0) {
        u32 rgba = 0x10000000u;
        for (u32 i = oldest; i != newest; i = (i + 1) & 0x1F, rgba += 0x3000000u) {
            Acc_Quad(P<const char>(0x2E8770u), kCorners, InverseView(), right[i], rgba | 0x10140);     // "dageki"
            Acc_Quad(P<const char>(0x2E8770u), kCorners, InverseView(), left[i], rgba | 0x10140);
        }
    }
    Acc_End();
}

// ── 0x2435E0: what a character's motion events and blows show ─────────────
// `what` (0–29) comes from the motion's event list (footsteps) or from the
// blow code: dust underfoot on soft ground, blood from a cut, sparks from a
// blunt blow.  Four of them only switch body state bits.
void CharaSounds_Play(void*, Chara* chara, int what) {
    if ((u32)what >= 30)
        return;
    const u32 model = P<CharaParams>(chara->params)->link[1];
    const u32 points = model != 0 ? P<u32>(model)[1] : 0;
    const float* up = P<float>(0x2DA2C0u);
    const auto point = [&](u32 offset, float* out) {
        if (points != 0)
            Copy4(out, P<float>(points + offset));
        else
            memset(out, 0, 16);
    };
    const auto midpoint = [&](u32 a, u32 b, float* out) {
        alignas(16) float pa[4], pb[4];
        point(a, pa);
        point(b, pb);
        Add(out, pa, pb);
        ScaleXYZ(out, out, 0.5f);
        out[3] = 1.0f;
    };
    alignas(16) float at[4];
    const bool soft = chara->_158 == 1;         // the kind of ground underfoot
    CharaBody* body = P<CharaBody>(chara->body);
    switch (what) {
    case 0: case 3:   point(0x210, at); if (soft) Fx_Burst(0, at, up); break;
    case 1: case 4:   point(0x210, at); if (soft) Fx_Burst(1, at, up); break;
    case 2: case 5:   point(0x210, at); if (soft) Fx_Burst(2, at, up); break;
    case 15: case 16: point(0x530, at); if (soft) Fx_Burst(2, at, up); break;
    case 17:          midpoint(0x2B0, 0xD0, at); if (soft) Fx_Burst(2, at, up); break;
    case 25:          point(0x990, at); if (soft) Fx_Burst(0, at, up); break;
    case 24:          point(0xC10, at); if (soft) Fx_Burst(0, at, up); break;
    case 7: case 18: case 19: point(0x490, at); Chara_Bleed(chara, at, 0); break;
    case 8:           point(0x490, at); Chara_Bleed(chara, at, 1); break;
    case 9:           point(0x490, at); Chara_Bleed(chara, at, 2); break;
    case 20:          midpoint(0xA30, 0x7B0, at); Chara_Bleed(chara, at, 2); break;
    case 21:          point(0xAD0, at); Chara_Bleed(chara, at, 2); break;
    case 14:          point(0x490, at); Chara_Bleed(chara, at, 3); break;
    case 10: case 11: case 12: case 13: {
        if (chara->flagHandle == 0)
            break;
        SparkSource from;
        Copy4(from.pos, P<float>(chara->flagHandle + 0x1B0));
        Copy4(from.dir, P<float>(chara->flagHandle + 0x1A0));
        Normalize(from.dir, from.dir);
        ScaleXYZ(from.dir, from.dir, -0.09f);
        from.kind = what - 10;
        Impact* impact = static_cast<Impact*>(MemAlloc(0x40));
        memset(impact, 0, 0x40);
        Effect_Ctor(&impact->base);
        impact->base.vtable = 0x2EC6D0u;
        Impact_Start(impact, &from, 0);
        AddToWorld(&impact->base);
        break;
    }
    case 6: {
        if (chara->flagHandle == 0)
            break;
        Flash* flash = static_cast<Flash*>(MemAlloc(0x30));
        memset(flash, 0, 0x30);
        Effect_Ctor(&flash->base);
        flash->base.vtable = 0x2ECC20u;
        flash->base.type = 0x12;
        Flash_Start(flash, P<float>(chara->flagHandle + 0x1B0), 0);
        AddToWorld(&flash->base);
        break;
    }
    case 28: {                                  // a guard broken (0x2BAFB0)
        if (chara->flagHandle == 0)
            break;
        const float* at = P<float>(chara->flagHandle + 0x1B0);
        Flash* flash = static_cast<Flash*>(MemAlloc(0x30));
        memset(flash, 0, 0x30);
        Effect_Ctor(&flash->base);
        flash->base.vtable = 0x2ECC20u;
        flash->base.type = 0x12;
        Flash_Start(flash, at, 0x80709090u);
        AddToWorld(&flash->base);
        for (int i = 0; i < 10; i++) {
            Streak* streak = static_cast<Streak*>(MemAlloc(0x40));
            memset(streak, 0, 0x40);
            Effect_Ctor(&streak->base);
            streak->base.vtable = 0x2ECC00u;
            streak->base.type = 0x12;
            Streak_Start(streak, at, 0x80709090u);
            AddToWorld(&streak->base);
        }
        // … and effect 0x13: the picture flares where it happened.
        {
            Glare* glare = static_cast<Glare*>(MemAlloc(sizeof(Glare)));
            memset(glare, 0, sizeof *glare);
            Effect_Ctor(&glare->base);
            glare->base.vtable = 0x2EC660u;
            glare->base.type = 0x13;
            Glare_Start(glare, at);
            AddToWorld(&glare->base);
        }
        break;
    }
    case 29: {                                  // a blow landing: a ring spreads from it
        if (chara->flagHandle == 0)
            break;
        Ring* ring = static_cast<Ring*>(MemAlloc(0x30));
        memset(ring, 0, 0x30);
        Effect_Ctor(&ring->base);
        ring->base.vtable = 0x2ED900u;
        Ring_Start(ring, P<float>(chara->flagHandle + 0x1B0), *reinterpret_cast<const s32*>(
                                                                  reinterpret_cast<const u8*>(body) + 0x8C));
        AddToWorld(&ring->base);
        break;
    }
    case 27: body->state = (body->state | 0x8000) & ~0x4000u; break;
    case 26: body->state = (body->state | 0x4000) & ~0x8000u; break;
    case 23: body->state |= 0x10000; break;
    case 22: body->state &= ~0x10000u; break;
    default: {
        // 28 (0x22F8E0: a ripple that bends the picture behind it), 29 (0x2BB980): not decompiled.
        static u32 told = 0;
        if ((told & 1u << what) == 0) {
            told |= 1u << what;
            printf("[stub] character effect %d is not decompiled\n", what);
        }
        break;
    }
    }
}

// What the scripts' Effect.AddCharEffect makes (0x204210): blood pouring
// from a character (the spray at its fullest), or a pool under it.
void Fx_BloodSpray(u32 params, const float* dir) {
    SprayParams from;
    memset(&from, 0, sizeof from);
    Copy4(from.dir, dir);
    from.params = params;
    from.level = 3;
    Spray* spray = static_cast<Spray*>(MemAlloc(0x40));
    memset(spray, 0, 0x40);
    Effect_Ctor(&spray->base);
    spray->base.vtable = 0x2EC510u;
    spray->base.type = 0x17;
    spray->base.life = 10;
    Spray_Start(spray, &from, 0);
    AddToWorld(&spray->base);
}
void Fx_BloodPool(const float* pos, const float* dir) {
    Mote* pool = NewMote(0x2EB0F0u, 0x16);
    Pool_Start(pool, pos, dir);
    AddToWorld(&pool->base);
}

void Shadow_SetHidden(void* shadow, int off) { static_cast<ShadowEffect*>(shadow)->off = off; }     // 0x1673F0
void Shadow_Release(void* effect) { static_cast<Effect*>(effect)->type = 0; }                      // 0x172090

// ═══ Type 0x1C: a sound of the place (0x20 bytes, vtable 0x2ED730) ═════════
// Wind, a crowd: a sound effect started again every so many frames for as
// long as the effect lives (the scripts' Effect.AddEnvSound / DelEnvSound).
// The original keeps a voice of its own for it (the object at +0x0C,
// 0x2AD730–0x2AD9C0), so each start cuts the last short; here they overlap
// if the sample is longer than the interval.
struct EnvSound {
    Effect base;            // life = frames to the next start
    u32 voice;              // +0x0C  (not made here)
    s32 interval;           // +0x10
    s32 id;                 // +0x14
    s32 volume;             // +0x18  of 0x1FFF
    s32 handle;             // +0x1C  what the script knows it by
};
void* EnvSound_Dtor(EnvSound* self, int flag) {                 // 0x2B25B0
    if (self != nullptr && flag > 0)
        MemFree(self);
    return self;
}
int EnvSound_Start(EnvSound* self, const s32* params, u32) {    // 0x2B2540
    self->interval = params[0];
    self->id = params[1];
    self->volume = params[2];
    self->handle = params[3];
    self->base.life = 0;        // (the original also starts it here; the draw below does at once)
    return 1;
}
int EnvSound_Update(EnvSound* self) { return self->base.type != 0; }    // 0x2B2500
int EnvSound_Draw(EnvSound* self) {                             // 0x2B24C0
    if (self->base.life == 0) {
        self->base.life = self->interval;
        Sound_PlaySE(self->id, 0, self->volume);
    } else {
        self->base.life--;
    }
    return 1;
}
GUEST_BIND(0x2B25B0, EnvSound_Dtor);
GUEST_BIND(0x2B2540, EnvSound_Start);
GUEST_BIND(0x2B2500, EnvSound_Update);
GUEST_BIND(0x2B24C0, EnvSound_Draw);

// ═══ A sound placed on the map (0x30 bytes, vtable 0x2EC490, 0x220800–
// 0x220BE0): object type 9 of the placement files — crickets, a dog far
// off, gulls, the night watch's call ═══════════════════════════════════════
// Heard only within the effects' range of its place (14 units); out of
// range it neither sounds nor counts.  The sounds of the table 0x2D9880
// ({id, interval, six volumes to pick from} × 11) take the table's interval,
// which is 0 for every one of them: a remainder by zero follows, and on the
// PS2 that is the dividend, some frames up to 2³¹ — so a looped one (the
// crowd, the waves, the wind) is in effect never started, and the others
// sound once, when first come near.  Only the bell cricket (sound 53, not
// in the table) keeps on, every three seconds.  Kept as it is.
// Native: the voice object (+0x0C, 0x2AD730–0x2AD9C0) is not made; the
// volume is set when a sound starts and not followed while it plays.
struct PlacedSound {
    Effect base;            // life = frames to the next start
    u32 voice;              // +0x0C
    float pos[4];           // +0x10
    s32 interval;           // +0x20
    s32 id;                 // +0x24
    s32 looped;             // +0x28
    s32 volume;             // +0x2C
};
const GuestPtrVar<void> gSeDriver{0x2EE5E0u};
s32& gSeVolume = *guest::ptr<s32>(0x2EDAD0u);
inline s32 RemR5900(s32 a, s32 b) { return b == 0 ? a : a % b; }
const s32* PlacedSound_Row(s32 id) {
    const s32* table = P<s32>(0x2D9880u);
    for (int i = 0; i < 11; i++)
        if (table[i * 8] == id)
            return &table[i * 8];
    return nullptr;
}
void* PlacedSound_Dtor(PlacedSound* self, int flag) {           // 0x220B30
    if (self != nullptr && flag > 0)
        MemFree(self);
    return self;
}
int PlacedSound_Start(PlacedSound* self, const u32* params, u32) {      // 0x220A00
    memmove(self->pos, P<float>(params[0]), 16);
    self->interval = (s32)params[1];
    self->id = (s32)params[2];
    self->looped = (s32)params[3];
    self->base.life = 0;
    self->volume = 0x1FFF;
    if (const s32* row = PlacedSound_Row(self->id)) {
        self->interval = row[1];
        self->volume = row[2 + (u32)Rand_Next() % 6];
    }
    self->base.life = self->looped == 0 ? Rand_Next() & 0x1F : RemR5900(Rand_Next(), self->interval);
    return 1;
}
int PlacedSound_Update(PlacedSound* self) {                     // 0x220810
    int right = self->volume * gSeVolume / 10;                  // (0x2AD860 → 0x180E00)
    int left = right;
    Sound_ApplyPan(4.6f, 14.0f, &left, &right, (int)guest::addr(self->pos));
    if (left <= 0 && right <= 0)
        return 1;
    if (self->base.life != 0) {
        self->base.life--;
        return 1;
    }
    self->base.life = self->looped != 0 ? self->interval
                                        : self->interval + RemR5900(Rand_Next(), self->interval / 30);
    if (const s32* row = PlacedSound_Row(self->id))
        self->volume = row[2 + (u32)Rand_Next() % 6];
    static const bool trace = getenv("WOTS2_TRACE_SE") != nullptr;      // (native debug aid)
    if (trace)
        printf("[se] %d (of the place: %.1f %.1f %.1f)\n", self->id, self->pos[0], self->pos[1], self->pos[2]);
    SeDriver_Play(*gSeDriver, self->id, 0x1000, left, right);   // (0x2AD780; with the volumes found before the new pick)
    return 1;
}
int PlacedSound_Draw(PlacedSound*) { return 1; }                // 0x220800
GUEST_BIND(0x220B30, PlacedSound_Dtor);
GUEST_BIND(0x220A00, PlacedSound_Start);
GUEST_BIND(0x220810, PlacedSound_Update);
GUEST_BIND(0x220800, PlacedSound_Draw);

// (ItemDrops_Place, case 9, makes it in place.)
void Fx_PlaceSound(const float* pos, int interval, int id, int looped) {
    PlacedSound* e = static_cast<PlacedSound*>(MemAlloc(0x30));
    if (e == nullptr)
        return;
    memset(e, 0, 0x30);
    Effect_Ctor(&e->base);                                      // 0x220BE0
    e->base.vtable = 0x2EC490u;
    memcpy(e->pos, pos, 16);                                    // (the caller's matrix is the host's)
    const u32 params[4] = {guest::addr(e->pos), (u32)interval, (u32)id, (u32)looped};
    PlacedSound_Start(e, params, 0);
    AddToWorld(&e->base);
}

// ── 0x14EA50 / 0x14E920 ───────────────────────────────────────────────────
int RenderMgr_AddEnvSound(void* mem, int id, int volume, int interval) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    s32 handle = 0;
    for (GuestListNode* node = GuestList_Begin(&self->world); node != GuestList_End(&self->world);
         node = GuestList_Next(node))
        if (P<Effect>(node->value)->type == 0x1C)
            handle++;
    EnvSound* e = static_cast<EnvSound*>(MemAlloc(0x20));
    if (e == nullptr)
        return handle;
    memset(e, 0, 0x20);
    Effect_Ctor(&e->base);
    e->base.vtable = 0x2ED730u;
    e->base.type = 0x1C;
    e->handle = -1;
    const s32 params[4] = {interval, id, volume, handle};
    EnvSound_Start(e, params, 0);
    GuestList_Insert(&self->world, GuestList_End(&self->world), guest::addr(e));
    return handle;
}
int RenderMgr_RemoveEnvSound(void* mem, int handle) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    for (GuestListNode* node = GuestList_Begin(&self->world); node != GuestList_End(&self->world);
         node = GuestList_Next(node)) {
        EnvSound* e = P<EnvSound>(node->value);
        if (e->base.type == 0x1C && e->handle == handle) {
            e->base.type = 0;
            return 1;
        }
    }
    return 0;
}

// What 0x14FE60 (the constructor) sets up for this half.
void WorldEffects_Init(void* mem) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    GuestList_Init(&self->world);
    GuestList_Init(&self->screen);
    *gEffectMgr = guest::addr(self);
    SpriteSet* sprites = SpriteSet_Ctor(MemAlloc(0x14));
    SpriteSet_Load(sprites, 9, -1);
    self->sprites = guest::addr(sprites);
    *gEffectSprites = self->sprites;
    *gFxBlend = -1;
    // The particle orders (0x1793F0): {_, count, _, _, records of 0x40 bytes}.
    void* file = ArchiveFile_Create();
    ArchiveFile_Open(file, 10);
    const u32 size = ArchiveFile_GetSize(file);
    u8* data = static_cast<u8*>(MemAlloc(size));
    ArchiveFile_Read(file, data, size);
    u32* orders = static_cast<u32*>(MemAlloc(0x10));
    orders[0] = 0;
    orders[1] = 1;
    orders[2] = *reinterpret_cast<u32*>(data + 4);
    orders[3] = guest::addr(data + 0x10);
    self->orders = guest::addr(orders);
}

// ── 0x14ECE0: makes a world effect and starts it ──────────────────────────
void* RenderMgr_MakeWorldEffect(void* mem, int type, u32 a, u32 b) {
    Effect* made = nullptr;
    switch (type) {
    case 0x0F:
        made = &Shadow_Ctor(static_cast<ShadowEffect*>(MemAlloc(0x18)))->base;
        break;
    case 0x19: {
        // b = which order; its kind picks the class.
        const u32* orders = P<u32>(static_cast<RenderMgr*>(mem)->orders);
        if (orders[3] == 0 || b >= orders[2])
            return nullptr;
        const u32 order = orders[3] + b * 0x40;
        const u8 kind = P<FxOrder>(order)->kind;
        Particle* p = static_cast<Particle*>(MemAlloc(0x80));
        memset(p, 0, 0x80);
        Particle_Ctor(p, kind == 0 ? 0x2EAF80u : kind == 3 ? 0x2EAF60u : kind == 1 ? 0x2EAF40u : 0x2EAFA0u);
        FxStart(&p->base, a, order);
        return p;
    }
    case 0x1A: {
        Emitter* e = static_cast<Emitter*>(MemAlloc(0x40));
        memset(e, 0, 0x40);
        Effect_Ctor(&e->base);
        e->base.vtable = 0x2EAFC0u;
        e->base.type = 0x1A;
        made = &e->base;
        break;
    }
    default: {
        static u32 told = 0;
        if (type < 32 && (told & 1u << type) == 0) {
            told |= 1u << type;
            printf("[stub] world effect %#x is not decompiled\n", type);
        }
        return nullptr;
    }
    }
    FxStart(made, a, b);
    return made;
}

// The tail of RenderMgr_TriggerEffect (0x14F190) for types of 14 and over.
void* RenderMgr_AddWorldEffect(void* mem, int type, u32 a, u32 b) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    void* made = RenderMgr_MakeWorldEffect(mem, type, a, b);
    if (made != nullptr)
        GuestList_Insert(&self->world, GuestList_End(&self->world), guest::addr(made));
    return made;
}

// ── 0x14EB50: the n-th world effect of a type ─────────────────────────────
void* RenderMgr_FindWorldEffect(void* mem, int type, int n) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    for (GuestListNode* node = GuestList_Begin(&self->world); node != GuestList_End(&self->world);
         node = GuestList_Next(node)) {
        if (P<Effect>(node->value)->type != type)
            continue;
        if (n == 0)
            return P(node->value);
        n--;
    }
    return nullptr;
}

// ── 0x14F900: once a frame from the world — steps the world effects ───────
// (It also makes the two splinter models at +0x60 on first use; here they
// are read by the native draw: see Splinter_Draw.)
void RenderMgr_SetMap(void* mem, u32 map) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    *gEffectMap = map;
    Fx_PrepareView();
    // WOTS2_FX="updates:what[:character]" (debug): that many updates on, 1 = the
    // glint of a technique learnt at that character's eye, 2 = the shards of
    // a sword worn down from its right hand (bone 29), 3 = the glare of a guard
    // broken at its chest.
    static const char* fx = getenv("WOTS2_FX");
    static int fxUpdates = 0;
    if (fx != nullptr) {
        int at = 0, what = 0, who = 0;
        sscanf(fx, "%d:%d:%d", &at, &what, &who);
        if (++fxUpdates >= at) {
            fx = nullptr;
            CharaParams* params = CharaResources_GetParams(CharaResources_Get(), who);
            const u32 points = params != nullptr && params->link[1] != 0 ? P<u32>(params->link[1])[1] : 0;
            if (what == 1 && params != nullptr)
                Fx_TechniqueGlint(params);
            else if (what == 2 && points != 0)
                Fx_SwordShards(P<float>(points + 29 * 0xA0 + 0x30));
            else if (what == 3 && points != 0) {            // the glare of a guard broken, at the chest
                Glare* glare = static_cast<Glare*>(MemAlloc(sizeof(Glare)));
                memset(glare, 0, sizeof *glare);
                Effect_Ctor(&glare->base);
                glare->base.vtable = 0x2EC660u;
                glare->base.type = 0x13;
                Glare_Start(glare, P<float>(points + 9 * 0xA0 + 0x30));
                AddToWorld(&glare->base);
            }
        }
    }
    // WOTS2_DUMP_FX=n (debug): every n frames, how many world effects of each class there are.
    static const int dumpEvery = getenv("WOTS2_DUMP_FX") ? atoi(getenv("WOTS2_DUMP_FX")) : 0;
    static int dumpFrame = 0;
    if (dumpEvery > 0 && ++dumpFrame % dumpEvery == 0) {
        std::map<u32, int> count;
        for (GuestListNode* node = GuestList_Begin(&self->world); node != GuestList_End(&self->world);
             node = GuestList_Next(node))
            count[P<Effect>(node->value)->vtable]++;
        printf("[fx]");
        for (const auto& c : count)
            printf(" %#x × %d", c.first, c.second);
        printf("\n");
    }
    for (GuestListNode* node = GuestList_Begin(&self->world); node != GuestList_End(&self->world);) {
        Effect* e = P<Effect>(node->value);
        if (FxUpdate(e) == 0) {
            FxDestroy(e);
            MemFree(e);
            node = GuestList_Erase(&self->world, node);
        } else {
            node = GuestList_Next(node);
        }
    }
}

// ── The world half of RenderMgr_Draw (0x14F580) ───────────────────────────
void RenderMgr_DrawWorld(void* mem) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    Fx_PrepareView();
    Fx_ResetBlend();
    GsDevice_WriteRegister(*gGsDevice, 0x47, 0x53001);
    if (SpriteSet_Bind(P<SpriteSet>(self->sprites)) == 1)
        for (GuestListNode* node = GuestList_Begin(&self->world); node != GuestList_End(&self->world);
             node = GuestList_Next(node))
            FxDraw(P<Effect>(node->value));
    Fx_ResetBlend();
    GsDevice_WriteRegister(*gGsDevice, 0x47, 0);
}

// ── The world half of RenderMgr_ClearEffects (0x14FA80) ───────────────────
void RenderMgr_ClearWorld(void* mem) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    for (GuestListNode* node = GuestList_Begin(&self->world); node != GuestList_End(&self->world);) {
        Effect* e = P<Effect>(node->value);
        FxDestroy(e);
        MemFree(e);
        node = GuestList_Erase(&self->world, node);
    }
}

// ── 0x14FC00: the scripts' Effect.DeleteEffect — types 1, 0x16 and 0x1C go ─
int RenderMgr_DeleteMarks(void* mem) {
    RenderMgr* self = static_cast<RenderMgr*>(mem);
    for (GuestListNode* node = GuestList_Begin(&self->world); node != GuestList_End(&self->world);) {
        Effect* e = P<Effect>(node->value);
        if (e->type == 1 || e->type == 0x16 || e->type == 0x1C) {
            FxDestroy(e);
            MemFree(e);
            node = GuestList_Erase(&self->world, node);
        } else {
            node = GuestList_Next(node);
        }
    }
    return 1;
}

}  // extern "C"
