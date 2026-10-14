// The collision world — decompiled from 0x155270–0x1571F0 (the list and the
// pass over it), 0x157250–0x157450 (a volume), 0x159BB0–0x15A580 (a body),
// 0x16B0C0–0x16B680 (a sword, fist or foot) and 0x210350–0x210540 (an item).

#include "game/collision_world.h"
#include "game/chara.h"
#include "game/chara_motions.h"
#include "game/player_data.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Matrix_Multiply(void* dst, const void* a, const void* b);     // 0x107130
    float Matrix_FacingCosine(const float* matrix, const float* point); // 0x1C8BC0
}

static_assert(offsetof(Volume, found) == 0x174 && offsetof(Volume, info) == 0x180 && offsetof(Volume, matrix) == 0x110, "Volume");
static_assert(sizeof(BodyVolume) == 0x1D0 && sizeof(SwordVolume) == 0x1A0 && sizeof(ItemVolume) == 0x1B0, "volumes");
static_assert(offsetof(BodyVolume, blowFrom) == 0x1A0 && offsetof(BodyVolume, clash) == 0x1C4, "BodyVolume");

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static const GuestPtrVar<u8> gPlayerData{0x2EE688u};
enum : u32 { kVtVolume = 0x2EAD60u, kVtBody = 0x2EAE30u, kVtSword = 0x2EAEF0u, kVtItem = 0x2EC230u };
enum : u32 { kVolDestroy = 0x08, kVolRefresh = 0x0C, kVolClear = 0x10, kVolActive = 0x14, kVolPresent = 0x18 };

static inline void Copy(float* d, const float* s) { memcpy(d, s, 16); }
static inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float Distance(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(Dot(d, d));
}
// (a − b), normalised (0x136D50).
static void Towards(float* out, const float* a, const float* b) {
    float d[4] = {a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]};
    const float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3]);
    const float scale = length != 0.0f ? 1.0f / length : 0.0f;
    for (int i = 0; i < 4; i++)
        out[i] = d[i] * scale;
}
static void Apply(float* out, const float* m, const float* v) {     // v × m
    float r[4];
    for (int j = 0; j < 4; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    Copy(out, r);
}
static inline bool InHalfCube(const float* v) {                     // 0x136C80 with 0.5
    return fabsf(v[0]) <= 0.5f && fabsf(v[1]) <= 0.5f && fabsf(v[2]) <= 0.5f;
}
static inline float* Centre(Volume* v) { return &v->matrix[12]; }
static inline u32* Info(Volume* v) { return P<u32>(v->info); }
static inline CharaParams* Owner(Volume* v) { return P<CharaParams>(Info(v)[0]); }
static inline u32 Linked(const CharaParams* p, int link) { return p->link[link] ? *P<u32>(p->link[link]) : 0; }
static inline u32 BodyFlags(const CharaParams* p) { return Linked(p, 5); }      // CharaBody::flags
static inline u32 ActFlags(const CharaParams* p)  { return Linked(p, 6); }      // CharaBody::actionFlags

// ═══ A volume ═════════════════════════════════════════════════════════════

// ── 0x157250 ──────────────────────────────────────────────────────────────
void Volume_ToWorld(Volume* self) {
    for (int i = 0; i < 8; i++)
        Apply(self->world[i], self->matrix, self->corners[i]);
}
// ── 0x1572E0 ──────────────────────────────────────────────────────────────
// `size` is the box's half-extent along each axis.
void Volume_SetSize(Volume* self, const float* size) {
    static const float (*const kUnit)[4] = guest::ptr<const float[4]>(0x2D1E80u);  // the corners of a unit cube
    const float full[3] = {size[0] / 0.5f, size[1] / 0.5f, size[2] / 0.5f};
    for (int i = 0; i < 8; i++) {
        self->corners[i][0] = kUnit[i][0] * full[0];
        self->corners[i][1] = kUnit[i][1] * full[1];
        self->corners[i][2] = kUnit[i][2] * full[2];
        self->corners[i][3] = kUnit[i][3];
    }
    self->radius = sqrtf(Dot(full, full));
    self->invSize[0] = 1.0f / full[0];
    self->invSize[1] = 1.0f / full[1];
    self->invSize[2] = 1.0f / full[2];
}
// ── 0x157450 ──────────────────────────────────────────────────────────────
static Volume* Volume_Ctor(Volume* self, u32 kind) {
    self->vtable = kVtVolume;
    self->kind = kind;
    Matrix_Copy(self->matrix, P<float>(0x2D1A80u));
    self->offset[0] = self->offset[1] = self->offset[2] = 0.0f; self->offset[3] = 1.0f;
    self->invSize[0] = self->invSize[1] = self->invSize[2] = 0.0f; self->invSize[3] = 1.0f;
    self->radius = 0.0f;
    self->found = 0;
    self->on = 0;
    memset(self->corners, 0, sizeof self->corners);
    memset(self->world, 0, sizeof self->world);
    self->shape = 7;
    return self;
}
static Volume* Volume_Dtor(Volume* self, s16 flag) {                // 0x157400 and the derived ones
    if (self) {
        self->vtable = kVtVolume;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
static bool OwnerPresent(Volume* v) {                               // 0x159CE0 / 0x16B170
    return v->info != 0 && *P<s32>(Info(v)[1]) != 0 && *P<s32>(Info(v)[2]) != 0;
}

// ═══ A body ═══════════════════════════════════════════════════════════════

static void Body_Clear(BodyVolume* self) {                          // 0x15A260
    self->base.found = 0;
    self->damage = 0;
    self->guardDamage = 0;
    self->hurt = 0;
    self->power = 0;
    self->bump = 0;
}
static int Body_Active(BodyVolume* self) {                          // 0x159FE0
    return self->base.on != 0 && *P<s32>(Info(&self->base)[2]) != 0;
}
static int Body_Present(BodyVolume* self) {                         // 0x15A390
    return OwnerPresent(&self->base) && self->base.on != 0;
}
// 0x15A410: half the height and the middle, from the joints.
static void Body_Extent(BodyVolume* self, float* half, float* middle) {
    const CharaParams* p = Owner(&self->base);
    if (p->link[1] == 0)
        return;
    const u8* bones = P<u8>(P<Skeleton>(p->link[1])->bones);
    auto y = [&](u32 at) { float v; memcpy(&v, bones + at, 4); return v; };
    const float ground = p->matrix[13];
    const float top = (y(0xDF4) > y(0xD54) ? y(0xDF4) : y(0xD54)) - ground + 0.1f;
    const float bottom = (y(0x214) <= y(0x3F4) ? y(0x214) : y(0x3F4)) - ground - 0.05f;
    *half = (top - bottom) * 0.5f;
    if (*half < 0.1875f)
        *half = 0.1875f;
    *middle = (top + bottom) * 0.5f;
}
// 0x15A280: a box round the character; with the sword out it reaches forward.
static void Body_Refresh(BodyVolume* self) {
    float half = 0.0f, middle = 0.0f;
    Body_Extent(self, &half, &middle);
    const CharaParams* p = Owner(&self->base);
    const bool armed = (BodyFlags(p) & kBodyArmed) != 0;
    const float size[4] = {0.2f, half, armed ? 0.4f : 0.2f, 1.0f};
    const float offset[4] = {0.0f, middle, armed ? 0.2f : 0.0f, 1.0f};
    Volume_SetSize(&self->base, size);
    alignas(16) float m[16];
    Matrix_Copy(m, p->matrix);
    Apply(&m[12], m, offset);
    Matrix_Copy(self->base.matrix, m);
    Volume_ToWorld(&self->base);
}
static BodyVolume* Body_Dtor(BodyVolume* self, s16 flag) {          // 0x15A520
    return reinterpret_cast<BodyVolume*>(Volume_Dtor(&self->base, flag));
}
static BodyVolume* Body_Ctor(BodyVolume* self, u32 info) {          // 0x15A580
    Volume_Ctor(&self->base, kVolBody);
    self->base.vtable = kVtBody;
    self->base.info = info;
    memset(self->blowFrom, 0, 16);
    memset(self->blowAt, 0, 16);
    self->noPush = 0;
    Body_Clear(self);
    self->base.on = 1;
    self->base.shape = 3;
    return self;
}
static s32 Body_AttackerId(BodyVolume* self) {                      // 0x159D30
    if (self->base.info == 0)
        return -1;
    const u32 attacker = *P<u32>(Info(&self->base)[7]);
    return attacker ? P<CharaParams>(attacker)->id : -1;
}
// 0x15A180: is it walking into something that lies in `dir`?  Then it is stopped.
static bool Body_MovingInto(BodyVolume* self, const float* dir) {
    const float* last = P<float>(Info(&self->base)[4]);
    const float* next = P<float>(Info(&self->base)[5]);
    const float move[3] = {next[0] - last[0], 0.0f, next[2] - last[2]};
    if (Dot(dir, move) <= 0.0f)
        return false;
    self->base.found |= kHitBlocked;
    return true;
}
// 0x15A0C0: run into.
static void Body_Bump(BodyVolume* self, const float* dir) {
    float facing[4];
    Towards(facing, &self->base.matrix[8], P<float>(0x306900u));
    u32 how = 0;
    if (ActFlags(Owner(&self->base)) & kActfRun)
        how = Dot(dir, facing) >= 0.0f ? 1 : 2;
    self->bump = how;
    self->base.found |= kHitBumped;
}
// 0x15A010: stepped on while lying there by someone locked on to it.
static void Body_StepOver(BodyVolume* self, const float* point) {
    const CharaParams* p = Owner(&self->base);
    if ((Linked(p, 8) & 0x1000000) && (ActFlags(p) & 0x700000) == 0x200000 &&
        Matrix_FacingCosine(p->matrix, point) < 0.6108653f)
        self->base.found |= kHitSteppedOn;
}

// ═══ A sword, fist or foot ════════════════════════════════════════════════

static void Sword_Clear(SwordVolume* self) {                        // 0x16B600
    self->base.found = 0;
    self->wear = 0;
    self->kills = self->finishes = self->score = 0;
}
static int Sword_Active(SwordVolume* self) {                        // 0x16B320
    return self->base.on == 1 && *P<s32>(Info(&self->base)[2]) != 0;
}
static int Sword_Present(SwordVolume* self) {                       // 0x16B410
    return OwnerPresent(&self->base) && self->base.on != 0;
}
static void Sword_Refresh(SwordVolume* self) {                      // 0x16B3A0
    const CharaParams* p = Owner(&self->base);
    alignas(16) float m[16];
    Matrix_Copy(m, P<Bone>(P<Skeleton>(p->link[1])->bones)[self->bone].world);
    Apply(&m[12], m, self->base.offset);
    Matrix_Copy(self->base.matrix, m);
    Volume_ToWorld(&self->base);
}
static SwordVolume* SwordVol_Dtor(SwordVolume* self, s16 flag) {       // 0x16B620
    return reinterpret_cast<SwordVolume*>(Volume_Dtor(&self->base, flag));
}
static SwordVolume* SwordVol_Ctor(SwordVolume* self, u32 info) {       // 0x16B680
    Volume_Ctor(&self->base, kVolSword);
    self->base.vtable = kVtSword;
    self->base.info = info;
    Sword_Clear(self);
    self->form = 0;
    self->bone = 0;
    self->_18c = 0;
    self->base.shape = 7;
    return self;
}
// The action record of what its owner is doing.
static const ActionDef* Sword_Action(SwordVolume* self) {           // 0x16B120
    const s32 motion = *P<s32>(Info(&self->base)[3]);
    if (motion == -1)
        return nullptr;
    const u32 holder = CharaResources_ActionOf(CharaResources_Get(), motion);
    return holder && *P<u32>(holder) ? P<const ActionDef>(*P<u32>(holder)) : nullptr;
}
static inline const ActionAttack* AttackOf(const ActionDef* def) { return P<const ActionAttack>(def->attack); }
static const u8* Sword_InHand(Volume* v, int slot) {                // the Sword behind info[slot], or null
    const u32 sword = *P<u32>(Info(v)[slot]);
    return sword ? P<const u8>(sword) : nullptr;
}
static s32 SwordField(const u8* sword, int offset) { s32 v = 0; if (sword) memcpy(&v, sword + offset, 4); return v; }

// ── 0x16B490 ──────────────────────────────────────────────────────────────
void SwordVolume_Arm(SwordVolume* self, s32 form, s32 bone) {
    self->form = form;
    self->bone = bone;
    self->_18c = 0;
    float offset[4] = {0.0f, 0.0f, 0.0f, 1.0f}, size[4];
    if (form == 0x10) {
        // The blade: its box comes with the sword's model (four of them: one
        // or two hands, whole or broken).
        const u8* sword = Sword_InHand(&self->base, 6);
        if (!sword)
            return;                     // (the original complains)
        int which = bone != 0x1C;
        if (SwordField(sword, 0x60) == 0)
            which += 2;
        const u32 model = *P<u32>(Info(&self->base)[5]);
        const u32 box = model ? P<u32>(model)[0x44 / 4 + which] : 0;       // 0x15A880 / 0x15A8B0
        Copy(offset, P<float>(box ? box : 0x306900u));
        Copy(size, P<float>(box ? box + 0x10 : 0x306900u));
    } else {
        Copy(size, P<float>(0x2D2030u + (u32)form * 0x10));
    }
    Copy(self->base.offset, offset);
    self->base.on = 1;
    Volume_SetSize(&self->base, size);
}

// ═══ An item ══════════════════════════════════════════════════════════════

static void Item_Clear(ItemVolume* self) { self->base.found = 0; }  // 0x210410
static void Item_Refresh(ItemVolume* self) {                        // 0x210420
    alignas(16) float m[16];
    Matrix_Copy(m, P<float>(self->base.info));
    Apply(&m[12], m, self->base.offset);
    Matrix_Copy(self->base.matrix, m);
    Volume_ToWorld(&self->base);
}
static int Item_Active(ItemVolume* self) {                          // 0x2105A0
    const u16 state = *P<u16>(self->base.info + 0x6A);
    return (state & 1) == 0 && ((state & 0x20) == 0 || (self->flags & 2) != 0);
}
static int Item_Present(ItemVolume*) { return 1; }                  // 0x210470
static ItemVolume* Item_Dtor(ItemVolume* self, s16 flag) {          // 0x2104E0
    return reinterpret_cast<ItemVolume*>(Volume_Dtor(&self->base, flag));
}
static ItemVolume* Item_Ctor(ItemVolume* self, u32 matrix) {        // 0x210540
    Volume_Ctor(&self->base, kVolItem);
    self->base.vtable = kVtItem;
    self->thrower = 0;
    self->base.info = matrix;
    self->flags = 1;
    self->base.shape = 1;
    return self;
}
static int Volume_Zero(Volume*) { return 0; }                       // 0x155260, 0x156E70
static void Volume_Nop(Volume*) {}                                  // 0x156E60, 0x157240 and their copies
static void Volume_ClearFound(Volume* self) { self->found = 0; }    // 0x1552D0

// ═══ Do two volumes touch? ════════════════════════════════════════════════

// `b` in the space of `a`'s box, scaled so the box is the cube ±0.5.
static void Relative(Volume* a, Volume* b, float* m) {
    alignas(16) float ma[16], inverse[16];
    Matrix_Copy(ma, a->matrix);
    Matrix_Copy(m, b->matrix);
    for (int i = 0; i < 4; i++)
        m[12 + i] -= ma[12 + i];
    for (int r = 0; r < 4; r++)                 // the rotation's transpose undoes it (0x107248)
        for (int c = 0; c < 4; c++)
            inverse[r * 4 + c] = r < 3 && c < 3 ? ma[c * 4 + r] : (r == c ? 1.0f : 0.0f);
    Matrix_Multiply(m, inverse, m);
}
// ── 0x1555C0 ──────────────────────────────────────────────────────────────
// Is `b`'s centre or one of its corners inside `a`?
static bool CornerInside(Volume* a, Volume* b) {
    alignas(16) float m[16];
    Relative(a, b, m);
    const float* s = a->invSize;
    const float c[3] = {m[12] * s[0], m[13] * s[1], m[14] * s[2]};
    if (InHalfCube(c))
        return true;
    for (int i = 7; i >= 0; i--) {
        float p[4];
        Apply(p, m, b->corners[i]);
        p[0] *= s[0]; p[1] *= s[1]; p[2] *= s[2];
        if (InHalfCube(p))
            return true;
    }
    return false;
}
// ── 0x1552E0 ──────────────────────────────────────────────────────────────
// Does an edge of `b` pass through `a`?  (For each edge: where it crosses a
// second face plane of the cube, is that point inside?)
static bool EdgeThrough(Volume* a, Volume* b) {
    static const s32* const kEdge = guest::ptr<const s32>(0x2D1E20u);              // 12 pairs of corners
    static const float (*const kNormal)[4] = guest::ptr<const float[4]>(0x2D1DB0u);
    alignas(16) float m[16];
    Relative(a, b, m);
    float corner[8][4];
    for (int i = 0; i < 8; i++) {
        Apply(corner[i], m, b->corners[i]);
        for (int k = 0; k < 3; k++)
            corner[i][k] *= a->invSize[k];
    }
    for (int e = 11; e >= 0; e--) {
        const float* p0 = corner[kEdge[e * 2]];
        const float* p1 = corner[kEdge[e * 2 + 1]];
        bool crossed = false;
        for (int face = 5; face >= 0; face--) {
            const float plane = (face & 1) ? -0.5f : 0.5f;
            const int axis = face >> 1;
            const float d0 = p0[axis] - plane;
            if (d0 * (p1[axis] - plane) >= 0.0f)
                continue;
            if (!crossed) {
                crossed = true;
                continue;
            }
            const float along[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
            const float slope = Dot(along, kNormal[face]);
            const float reach = (slope < 0.0f ? fabsf(plane - p1[axis]) : fabsf(d0)) / slope;
            const float* from = slope < 0.0f ? p1 : p0;
            const float at[3] = {along[0] * reach + from[0], along[1] * reach + from[1], along[2] * reach + from[2]};
            if (InHalfCube(at))
                return true;
            break;
        }
    }
    return false;
}

// ── 0x156BE0 ──────────────────────────────────────────────────────────────
// A finishing blow in progress always connects with its victim.
static bool Paired(Volume* a, Volume* b) {
    if ((a->kind | b->kind) != (kVolBody | kVolSword))
        return false;
    Volume* body = a->kind == kVolBody ? a : b;
    Volume* sword = a->kind == kVolBody ? b : a;
    if (!OwnerPresent(body) || !OwnerPresent(sword))
        return false;
    const CharaParams* attacker = Owner(sword);
    if ((ActFlags(attacker) & 0x200) == 0)
        return false;
    if (Body_AttackerId(reinterpret_cast<BodyVolume*>(body)) != attacker->id)
        return false;
    const u32 victim = attacker->_d8[0];
    return (victim ? P<CharaParams>(victim)->id : -1) == Owner(body)->id;
}

// ── 0x1567A0 ──────────────────────────────────────────────────────────────
static bool Touching(Volume* a, Volume* b) {
    if (Paired(a, b))
        return true;
    if (Distance(Centre(a), Centre(b)) > a->radius + b->radius)
        return false;
    const bool ballA = (a->shape & ~1u) == 0, ballB = (b->shape & ~1u) == 0;
    if (ballA && ballB)
        return true;
    if (ballA || ballB) {
        Volume* ball = ballA ? a : b;
        Volume* box = ballA ? b : a;
        for (int i = 0; i < 8; i++)
            if (Distance(Centre(ball), box->world[i]) < ball->radius * 0.5f)
                return true;
        return false;
    }
    if (CornerInside(a, b) || CornerInside(b, a))
        return true;
    return ((a->shape | b->shape) & 4) && EdgeThrough(a, b);
}

// ═══ What touching means ══════════════════════════════════════════════════

// ── 0x156370: two bodies ──────────────────────────────────────────────────
static void BodyBody(BodyVolume* a, BodyVolume* b) {
    if (!OwnerPresent(&a->base) || !OwnerPresent(&b->base) || a->noPush != 0 || b->noPush != 0)
        return;
    const CharaParams* pa = Owner(&a->base);
    const CharaParams* pb = Owner(&b->base);
    const u32 actA = ActFlags(pa), actB = ActFlags(pb), act = actA | actB;
    const u32 flagsA = BodyFlags(pa), flags = flagsA | BodyFlags(pb);
    if (act & 0x100)
        return;
    if (flags & kBodyCorpse) {
        if ((act & 0x1000) == 0) {
            if ((flagsA & kBodyCorpse) == 0)
                Body_StepOver(a, P<float>(Info(&b->base)[5]));
            else
                Body_StepOver(b, P<float>(Info(&a->base)[5]));
        }
        return;
    }
    // Whoever is walking at the other is stopped.
    int walkingIn = 0;
    float dir[4];
    Towards(dir, P<float>(Info(&b->base)[4]), P<float>(Info(&a->base)[4]));
    dir[1] = 0.0f;
    if (dir[0] != 0.0f || dir[2] != 0.0f) {
        walkingIn = Body_MovingInto(a, dir);
        for (float& d : dir)
            d = -d;
        if (Body_MovingInto(b, dir))
            walkingIn++;
    }
    if (flags & (kBodyFaceUp | kBodyFaceDown))
        return;
    if ((act & 0x400) == 0) {
        // Running into someone staggers both.
        if ((act & kActfRun) && walkingIn != 0 && (flags & (kBodyArmed | kBodyDead)) == 0 && (act & 0x5000) == 0) {
            Towards(dir, P<float>(Info(&b->base)[4]), P<float>(Info(&a->base)[4]));
            Body_Bump(a, dir);
            *P<u32>(Info(&a->base)[7]) = Info(&b->base)[0];
            for (float& d : dir)
                d = -d;
            Body_Bump(b, dir);
            *P<u32>(Info(&b->base)[7]) = Info(&a->base)[0];
        }
    } else if ((actA & 0x400) == 0) {
        if (Body_AttackerId(b) != pa->id && (actA & 0x1000) == 0)
            a->base.found |= kHitShoved;
    } else if (Body_AttackerId(a) != pb->id && (actB & 0x1000) == 0) {
        b->base.found |= kHitShoved;
    }
}

// ── 0x155710 ──────────────────────────────────────────────────────────────
// How much a blow takes off: its power, raised by the sword's attack and
// lowered by the victim's sword's defence (per cent), and by difficulty —
// the player hits half again as hard on Easy and half as hard on Hard, and
// is hurt the other way round.
static s32 Damage(BodyVolume* body, SwordVolume* sword) {
    const u8* pd = *gPlayerData;
    const u32 perks = *reinterpret_cast<const u32*>(pd + 0x6E80);
    const s8 difficulty = (s8)pd[0x105];
    const ActionDef* def = Sword_Action(sword);
    s32 power = def ? AttackOf(def)->value[0] : -1;
    if (power == -1)
        power = 0;
    if (Owner(&sword->base)->id == 0) {
        if (perks & 1) power = (s32)((float)power * 1.5f);
        if (difficulty == 2)      power = (s32)((float)power * 0.5f);
        else if (difficulty == 0) power = (s32)((float)power * 1.5f);
    }
    s32 attack = SwordField(Sword_InHand(&sword->base, 6), 0x58);
    s32 defence = SwordField(Sword_InHand(&body->base, 6), 0x5C);
    if (attack < defence) {
        if (attack < 0) { defence -= attack; attack = 0; }
    } else if (defence < 0) {
        attack -= defence;
        defence = 0;
    }
    s32 damage = (s32)((float)power * ((float)attack * 0.01f + 1.0f) / ((float)defence * 0.01f + 1.0f));
    if (Owner(&body->base)->id == 0) {
        if (perks & 2) damage = (s32)((float)damage * 0.5f);
        if (difficulty == 2)      damage = (s32)((float)damage * 1.5f);
        else if (difficulty == 0) damage = (s32)((float)damage * 0.5f);
    }
    if (damage == 0 && power != 0)
        damage = 1;
    return damage;
}

// ── 0x1559A0 ──────────────────────────────────────────────────────────────
// A blow that answers a parry or a guard break: → 1 it kills outright, else
// a multiplier (0 = none).
static s32 Reprisal(BodyVolume* body, SwordVolume* sword) {
    static const s32 kByTiming[4] = {0, 2, 3, 1};          // 0x2D1E10
    const CharaParams* victim = Owner(&body->base);
    const CharaParams* attacker = Owner(&sword->base);
    const u32 act = ActFlags(victim);
    if (act & 0x40) {
        const u32 theirs = victim->_d8[0];
        if (theirs == 0 || P<CharaParams>(theirs)->id != attacker->id)
            return 0;
        return victim->unique != 0 ? kByTiming[*P<s32>(Info(&sword->base)[4])] : 1;
    }
    if ((act & 0x80) && Body_AttackerId(body) == attacker->id)
        return victim->unique == 0 ? 1 : 2;
    return 0;
}

// 0x159E40: is the victim in the middle of being finished by this attacker?
static s32 Body_Marked(BodyVolume* body, const CharaParams* attacker) {
    const CharaParams* p = Owner(&body->base);
    const u32 act = ActFlags(p);
    bool theirs = false;
    if (act & 0x40) {
        const u32 victim = p->_d8[0];
        theirs = victim != 0 && P<CharaParams>(victim)->id == attacker->id;
    } else if (act & 0x80) {
        theirs = Body_AttackerId(body) == attacker->id;
    }
    if (!theirs)
        return 0;
    body->base.found |= kHitMarked;
    return p->unique != 1 ? 2 : 1;
}
// 0x159F80: guarding, with a sword that is not broken.
static bool Body_Guarding(BodyVolume* body) {
    return (ActFlags(Owner(&body->base)) & 0x2000) && SwordField(Sword_InHand(&body->base, 6), 0x60) != 0;
}
// 0x159C10: cannot be hurt by this faction (children; the dead; someone at
// the floor of their health; a named character by its own side).
static bool Body_Immune(BodyVolume* body, s16 faction) {
    const CharaParams* p = Owner(&body->base);
    if (p->child != 0 || (BodyFlags(p) & kBodyDead))
        return true;
    return !(p->_48 < p->hp && (faction != p->faction || p->unique == 0));
}
// 0x16B1F0: the reaction the blow calls for; bit 28 when it comes from behind.
static u32 Sword_Reaction(SwordVolume* sword, const float* victim) {
    const ActionDef* def = Sword_Action(sword);
    u32 kind = def ? (u32)AttackOf(def)->value[1] : 0;
    if ((sword->base.found & kSwordKilled) == 0) {
        float to[4], facing[4];
        Towards(to, &victim[12], &Owner(&sword->base)->matrix[12]);
        Towards(facing, &victim[8], P<float>(0x306900u));
        if (Dot(facing, to) > 0.0f)
            kind |= 0x10000000u;
    }
    return kind;
}

// 0x155C40: a blow on a body already down.
static void CorpseStruck(BodyVolume* body, SwordVolume* sword) {
    if (ActFlags(Owner(&sword->base)) & 0x800)
        return;
    const ActionDef* def = Sword_Action(sword);
    if (!def)
        return;
    const ActionAttack* attack = AttackOf(def);
    if (attack->strength == 0x10) {
        if (Sword_InHand(&sword->base, 6)) {
            const u32 model = *P<u32>(Info(&sword->base)[5]);
            body->base.found |= (model && *P<s16>(model + 6) != 0) ? kHitCutBlunt : kHitCut;
            sword->base.found |= kSwordCut;
            sword->wear += attack->value[3] == -1 ? 0 : attack->value[3];
        }
    } else {
        body->base.found |= kHitStruck;
        sword->base.found |= kSwordStruck;
    }
    body->hurt = 0;
    sword->base.found |= kSwordHitCorpse;
}

// 0x156CD0: may this sword hit this body at all?
static bool CanHit(BodyVolume* body, SwordVolume* sword) {
    if (!OwnerPresent(&body->base) || !OwnerPresent(&sword->base))
        return false;
    const CharaParams* victim = Owner(&body->base);
    const CharaParams* attacker = Owner(&sword->base);
    if (victim->id == attacker->id)
        return false;
    if ((ActFlags(attacker) & 0x200) == 0)
        return *P<s32>(Info(&body->base)[3]) == 0;      // not while it still reels from the last one
    return !(victim->child != 0 || (BodyFlags(victim) & kBodyCorpse));
}

// ── 0x155D70: a blow ──────────────────────────────────────────────────────
static void BodySword(BodyVolume* body, SwordVolume* sword) {
    if (!CanHit(body, sword))
        return;
    CharaParams* victim = Owner(&body->base);
    CharaParams* attacker = Owner(&sword->base);
    alignas(16) float where[16];
    Matrix_Copy(where, victim->matrix);
    Towards(body->blowFrom, &attacker->matrix[12], &victim->matrix[12]);
    Copy(body->blowAt, Centre(&sword->base));
    const u32 flags = BodyFlags(victim);
    if (flags & kBodyCorpse) {
        CorpseStruck(body, sword);
        return;
    }
    *P<u32>(Info(&body->base)[7]) = guest::addr(attacker);
    attacker->_d8[0] = guest::addr(victim);
    const ActionDef* def = Sword_Action(sword);
    if (!def)
        return;
    const ActionAttack* attack = AttackOf(def);
    auto wear = [&](s32 amount) { sword->wear += amount == -1 ? 0 : amount; };
    const bool blade = attack->strength == 0x10;

    if (ActFlags(attacker) & 0x800) {
        if (!blade) {
            sword->base.found |= kSwordStruck;
            return;
        }
        if (!Body_Guarding(body)) {
            wear(attack->value[3]);
            sword->base.found |= kSwordStruck;
            return;
        }
    }
    const bool finishing = (ActFlags(attacker) & 0x200) != 0;
    if (finishing)
        body->base.found |= kHitFinished;
    bool landed = finishing;
    s32 score = 0;
    if (!blade) {
        landed = true;
        body->base.found |= kHitStruck;
        sword->base.found |= kSwordStruck;
    } else if (!Body_Guarding(body) || finishing) {
        if (Sword_InHand(&sword->base, 6)) {
            const u32 model = *P<u32>(Info(&sword->base)[5]);
            body->base.found |= (model && *P<s16>(model + 6) != 0) ? kHitCutBlunt : kHitCut;
            if (attacker->hp < 400)
                sword->base.found |= 0x800;
            if (flags & kBodyInAir)
                sword->base.found |= 0x400;
            if (ActFlags(victim) & 0x40)
                sword->base.found |= 0x200;
            if (const s32 marked = Body_Marked(body, attacker)) {
                sword->base.found |= 0x10;
                if (marked == 2)
                    sword->base.found |= 0x20;
            }
            landed = true;
            sword->base.found |= kSwordCut;
            wear(attack->value[3]);
            score = 3;
        }
    } else {
        // Guarded: it costs the guard's sword, and the attacker's.
        s32 cost = attack->value[5];
        if (SwordField(Sword_InHand(&sword->base, 6), 0x60) == 0)
            cost >>= 4;
        body->guardDamage += cost == -1 ? 0 : cost;
        wear(attack->value[4]);
        if ((ActFlags(attacker) & 0x4000000) == 0) {
            body->base.found |= kHitGuarded;
            sword->base.found |= kSwordGuarded;
            body->clash = attack->value[2] == -1 ? 0 : attack->value[2];
            score = 1;
        } else {
            score = 5;
            body->base.found |= kHitParried;
            sword->base.found |= kSwordGuarded | kSwordParried;
        }
    }
    body->power = attack->value[0];

    s32 reprisal = 0;
    if (!Body_Immune(body, attacker->faction)) {
        sword->score += score;
        sword->base.found |= kSwordLanded;
        s32 damage;
        if (!blade) {
            damage = Damage(body, sword);
        } else {
            reprisal = Reprisal(body, sword);
            if (reprisal == 1 || (*gPlayerData)[0x104] == 1) {
                damage = victim->hp;
            } else {
                damage = Damage(body, sword);
                if (SwordField(Sword_InHand(&sword->base, 6), 0x60) == 0)
                    damage >>= 4;               // a broken sword
                if (reprisal != 0)
                    damage *= reprisal;
            }
        }
        body->damage += damage;
    }
    if (!landed)
        return;
    const s16 health = victim->hp;
    body->hurt = Sword_Reaction(sword, where);
    if (health > 0 && health - body->damage < 1 && victim->_48 == -1) {
        if (blade) {
            sword->base.found |= kSwordKilled;
            sword->score += 5;
        }
        sword->kills++;
        if (reprisal == 1)
            sword->finishes++;
    }
}

// ── 0x155AB0: a body and an item ──────────────────────────────────────────
static void BodyItem(BodyVolume* body, ItemVolume* item) {
    CharaParams* p = Owner(&body->base);
    item->base.found |= 1;
    Copy(item->touchedAt, Centre(&body->base));
    float dir[4];
    Towards(dir, Centre(&item->base), Centre(&body->base));
    dir[1] = 0.0f;
    Body_MovingInto(body, dir);
    if (ActFlags(p) & 0x400) {
        item->base.found |= 3;
        Copy(item->touchedAt, Centre(&body->base));
    }
    if ((item->flags & 2) == 0 || item->thrower == guest::addr(p))
        return;
    // Thrown, and it has hit someone.
    const float* at = P<float>(item->base.info) + 12;
    Copy(body->blowAt, at);
    Towards(body->blowFrom, at, &p->matrix[12]);
    item->base.found |= 4;
    *P<u32>(Info(&body->base)[7]) = item->thrower;
    body->base.found |= kHitStruck;
    item->flags &= ~2u;
    item->thrower = 0;
    if (p->hp > 0)
        body->damage += 1;
}

// ── 0x156680 ──────────────────────────────────────────────────────────────
static void Meet(Volume* a, Volume* b) {
    if (a->kind > b->kind) {
        Volume* t = a; a = b; b = t;
    }
    if (a->kind == kVolBody) {
        BodyVolume* body = reinterpret_cast<BodyVolume*>(a);
        if (b->kind == kVolBody)       BodyBody(body, reinterpret_cast<BodyVolume*>(b));
        else if (b->kind == kVolSword) BodySword(body, reinterpret_cast<SwordVolume*>(b));
        else if (b->kind == kVolItem)  BodyItem(body, reinterpret_cast<ItemVolume*>(b));
    } else if (a->kind == kVolSword && b->kind == kVolItem) {
        // A sword knocks an item away.
        ItemVolume* item = reinterpret_cast<ItemVolume*>(b);
        item->base.found |= 3;
        Copy(item->touchedAt, Centre(a));
    }
}

// ═══ The world ════════════════════════════════════════════════════════════

static inline CollisionWorld* W(void* self) { return static_cast<CollisionWorld*>(self); }
static inline Volume* At(GuestListNode* n) { return P<Volume>(n->value); }
#define EACH_VOLUME(n, w) \
    for (GuestListNode* n = GuestList_Begin(&(w)->volumes); n != GuestList_End(&(w)->volumes); n = GuestList_Next(n))

// ── 0x1571F0 / 0x1570A0 ───────────────────────────────────────────────────
void* FlagsObj_Ctor(void* mem) {
    GuestList_Init(&W(mem)->volumes);
    W(mem)->arg = 0;
    return mem;
}
void FlagsObj_Dtor(void* self, int flag) {
    if (!self)
        return;
    EACH_VOLUME(n, W(self))
        if (n->value != 0)
            VirtualCall<void*, Volume*, int>(At(n)->vtable, kVolDestroy, At(n), 1);
    GuestList_Clear(&W(self)->volumes);
    if ((s16)flag > 0)
        MemFree(self);
}
// ── 0x155270 ──────────────────────────────────────────────────────────────
void FlagsObj_Reset(void* self) {
    EACH_VOLUME(n, W(self))
        VirtualCall<void, Volume*>(At(n)->vtable, kVolClear, At(n));
}
// ── 0x156DE0 ──────────────────────────────────────────────────────────────
void FlagsObj_StepA(void* self) {
    EACH_VOLUME(n, W(self))
        if (VirtualCall<int, Volume*, u32>(At(n)->vtable, kVolPresent, At(n), W(self)->arg))
            VirtualCall<void, Volume*>(At(n)->vtable, kVolRefresh, At(n));
}
// ── 0x156A00 ──────────────────────────────────────────────────────────────
void FlagsObj_StepB(void* self) {
    FlagsObj_Reset(self);
    EACH_VOLUME(a, W(self)) {
        if (!VirtualCall<int, Volume*>(At(a)->vtable, kVolActive, At(a)))
            continue;
        for (GuestListNode* b = GuestList_Next(a); b != GuestList_End(&W(self)->volumes); b = GuestList_Next(b)) {
            if (!VirtualCall<int, Volume*>(At(b)->vtable, kVolActive, At(b)))
                continue;
            const u32 kinds = At(a)->kind | At(b)->kind;
            if ((kinds == 0xC || kinds == 9 || kinds == 5 || kinds == 1) && Touching(At(a), At(b)))
                Meet(At(a), At(b));
        }
    }
}
// ── 0x156E80 ──────────────────────────────────────────────────────────────
void FlagsObj_Remove(void* flags, u32 volume) {
    EACH_VOLUME(n, W(flags)) {
        if (n->value != volume)
            continue;
        GuestList_Erase(&W(flags)->volumes, n);
        if (volume != 0)
            VirtualCall<void*, Volume*, int>(P<Volume>(volume)->vtable, kVolDestroy, P<Volume>(volume), 1);
        return;
    }
}
// ── 0x157020 / 0x156FB0 / 0x156F20 ────────────────────────────────────────
u32 FlagsObj_AddChara(void* flags, u32 info, int) {
    void* mem = MemAlloc(sizeof(BodyVolume));
    const u32 volume = mem ? guest::addr(Body_Ctor(static_cast<BodyVolume*>(mem), info)) : 0;
    GuestList_Insert(&W(flags)->volumes, GuestList_End(&W(flags)->volumes), volume);
    return volume;
}
u32 FlagsObj_Add(void* flags, u32 info) {
    void* mem = MemAlloc(sizeof(SwordVolume));
    const u32 volume = mem ? guest::addr(SwordVol_Ctor(static_cast<SwordVolume*>(mem), info)) : 0;
    GuestList_Insert(&W(flags)->volumes, GuestList_End(&W(flags)->volumes), volume);
    return volume;
}
u32 FlagsObj_AddItem(void* flags, u32 matrix, const float* size) {
    void* mem = MemAlloc(sizeof(ItemVolume));
    ItemVolume* item = mem ? Item_Ctor(static_cast<ItemVolume*>(mem), matrix) : nullptr;
    GuestList_Insert(&W(flags)->volumes, GuestList_End(&W(flags)->volumes), guest::addr(item));
    item->base.offset[0] = item->base.offset[1] = item->base.offset[2] = 0.0f;     // 0x210480
    item->base.offset[3] = 1.0f;
    Volume_SetSize(&item->base, size);
    return guest::addr(item);
}

// The methods, by the addresses the vtables hold.
static const struct VolumeBinder {
    VolumeBinder() {
        const struct { u32 address; guest::Fn fn; const char* name; } methods[] = {
            {0x157400, Volume_Dtor, "Volume_Dtor"},   {0x156E60, Volume_Nop, "Volume_Nop"},
            {0x1552D0, Volume_ClearFound, "Volume_ClearFound"}, {0x155260, Volume_Zero, "Volume_Zero"},
            {0x156E70, Volume_Zero, "Volume_Zero"},   {0x157240, Volume_Nop, "Volume_Nop"},
            {0x15A520, Body_Dtor, "Body_Dtor"},       {0x15A280, Body_Refresh, "Body_Refresh"},
            {0x15A260, Body_Clear, "Body_Clear"},     {0x159FE0, Body_Active, "Body_Active"},
            {0x15A390, Body_Present, "Body_Present"}, {0x159BA0, Volume_Nop, "Volume_Nop"},
            {0x16B620, SwordVol_Dtor, "SwordVol_Dtor"},     {0x16B3A0, Sword_Refresh, "Sword_Refresh"},
            {0x16B600, Sword_Clear, "Sword_Clear"},   {0x16B320, Sword_Active, "Sword_Active"},
            {0x16B410, Sword_Present, "Sword_Present"}, {0x16B0B0, Volume_Nop, "Volume_Nop"},
            {0x2104E0, Item_Dtor, "Item_Dtor"},       {0x210420, Item_Refresh, "Item_Refresh"},
            {0x210410, Item_Clear, "Item_Clear"},     {0x2105A0, Item_Active, "Item_Active"},
            {0x210470, Item_Present, "Item_Present"}, {0x2103F0, Volume_Nop, "Volume_Nop"},
        };
        for (const auto& m : methods)
            guest::bind(m.address, m.fn, m.name);
    }
} sVolumeBinder;
