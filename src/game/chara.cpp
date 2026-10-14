// The character class — decompiled from 0x1CC8A0–0x1D0ED0 (the base class,
// vtable 0x2EB900) and 0x269030–0x269C40 (the two concrete classes: 0x2ECC90
// the player's, 0x2ECC40 everyone else's).
//
// Still native (src/native/hle_chara.cpp): reading the model files and
// drawing them (load phases 0–2 and the draw method), and the smaller parts a
// character owns — sword holder, voice, head turning, effects.

#include "engine/archive_file.h"
#include "game/chara.h"
#include "game/chara_motions.h"
#include "game/collision_world.h"
#include "game/map_collision.h"
#include "native/guest.h"
#include "native/hle.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* MemAllocAligned(u32 align, u32 size);         // 0x1228C8
    void  MemFreeAligned(void* p);                      // 0x122B50
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Matrix_Identity(void* m);                     // 0x107428

    // Parts of a character that are still stand-ins (src/native/hle_chara.cpp).
    void  CharaParts_Create(Chara* self);               // the constructors in 0x1D0970
    void  CharaParts_Destroy(Chara* self);              // …and destructors in 0x1D06B0
    int   CharaModels_Start(Chara* self, int phase);    // load phases 0–2 (0x1D03A0, 0x1D0190, 0x1CFF00)
    void  CharaModels_Release(Chara* self);
    void  CharaModels_Draw(Chara* self, int lod);       // 0x1CDD60 / 0x1CDF10
    void  CharaSwords_BeginFrame(void* swords);         // 0x2454D0
    void  CharaSwords_Draw(void* swords, CharaBody* body);      // 0x245500
    void  CharaSwords_Take(void* swords, const void* sword);    // 0x245750
    void  CharaSwords_Give(void* swords, const void* sword);    // 0x245760
    void  CharaVoice_Update(void* voice);               // 0x26A7F0
    void  CharaVoice_Refresh(void* voice);              // 0x26A900
    void  CharaSounds_Play(void* sounds, Chara* chara, int what);   // 0x2435E0
    void  CharaFace_Update(void* face, Chara* chara, CharaBody* body);  // 0x2A2F90
    void  CharaFace_Set(void* face, CharaParams* params, int expression);   // 0x2A2880
    void  Sound_PlaySE(int id, int positional, int volume);             // 0x180E70
    void  CharaEffects_Update(void* effects, CharaBody* body);  // 0x222CB0
    void  CharaSheath_Update(void* sheath, CharaBody* body, CharaParams* params);   // 0x243330
    void  CharaSheath_fn242e10(void* sheath, CharaBody* body);  // 0x242E10
    void  CharaSheath_fn242e80(void* sheath, CharaBody* body, int a);   // 0x242E80
    void  CharaSwords_DrawShadow(void* swords, CharaBody* body);        // 0x245880
    void  CharaSheath_fn242ec0(void* sheath, CharaBody* body);  // 0x242EC0
    void  CharaSheath_fn242f40(void* sheath, CharaBody* body);  // 0x242F40
    void  CharaSheath_fn242ff0(void* sheath, CharaBody* body);  // 0x242FF0
    void  CharaSheath_fn243070(void* sheath, int found);        // 0x243070
    void  CharaSheath_fn243150(void* sheath, CharaBody* body, int found);   // 0x243150
    void  CharaSheath_fn243240(void* sheath, CharaBody* body);  // 0x243240
    void  CharaSheath_fn2432c0(void* sheath, CharaBody* body, int found);   // 0x2432C0
    // Head turning (0x200ED0–0x201E90).
    s16   CharaLook_State(void* look);                  // 0x201790
    void  CharaLook_Begin(void* look, Skeleton* skeleton);          // 0x201E10
    void  CharaLook_SetTarget(void* look, const float* point);      // 0x201E30
    void  CharaLook_Apply(void* look, Skeleton* skeleton, const float* matrix);     // 0x2017A0
    void  CharaLook_End(void* look);                    // 0x201DF0
    void  CharaLook_Feet(void* look, Skeleton* skeleton, void* map, const float* matrix);   // 0x200ED0
}

static_assert(sizeof(Chara) == 0x210 && offsetof(Chara, vtable) == 0x1F4 && offsetof(Chara, look) == 0x18C, "Chara");
static_assert(offsetof(Chara, lookAt) == 0xE0 && offsetof(Chara, light) == 0x120 && offsetof(Chara, _150) == 0x150, "Chara");
static_assert(offsetof(Chara, swords) == 0x1D4 && offsetof(Chara, loadPhase) == 0x1E0, "Chara");

enum : u32 { kVtCharaBase = 0x2EB900u, kVtCharaPlayer = 0x2ECC90u, kVtCharaOther = 0x2ECC40u };

static s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline CharaParams*  Params(Chara* c)  { return P<CharaParams>(c->params); }
static inline CharaBody*    Body(Chara* c)    { return P<CharaBody>(c->body); }
static inline CharaControl* Control(Chara* c) { return P<CharaControl>(c->action); }
static inline Skeleton*     Bones(Chara* c)   { return P<Skeleton>(Params(c)->link[1]); }
static inline void Copy(float* d, const float* s) { memcpy(d, s, 16); }
static inline void UnitW(float* d) { d[0] = d[1] = d[2] = 0.0f; d[3] = 1.0f; }

// ── 0x1D05B0 ──────────────────────────────────────────────────────────────
static void ResetState(Chara* self) {
    UnitW(self->lookAt);
    self->_1dc = 0;
    self->lightSteps = 0;
    self->light[0] = self->light[1] = self->light[2] = 127.0f;
    self->light[3] = 1.0f;
    Copy(self->lightCell, self->light);
    Copy(self->lightTo, self->light);
    UnitW(self->lightStep);
    *reinterpret_cast<u32*>(reinterpret_cast<u8*>(self) + 0x168) = 0;
    self->cellFlags = 2;
    self->cellIndex = 0;
}
void Chara_Reset(Chara* self) {
    self->_68 = self->_6c = self->_70 = 0;
    self->partner = -1;
    self->_b5 = 0;
    ResetState(self);
    if (const CharaParams* t = CharaResources_FindParamTemplate(P<CharaResources>(self->resources), self->kind)) {
        CharaParams* p = Params(self);
        p->hp = t->maxHp < p->maxHp ? t->maxHp : p->maxHp;
        p->_48 = t->_48;
        p->stat[0] = p->stat[1] = p->stat[2] = p->stat[3] = 0;
    }
}

// ── 0x1D0970 ──────────────────────────────────────────────────────────────
Chara* Chara_BaseCtor(Chara* self, int id, int kind, int model, int arg) {
    self->vtable = kVtCharaBase;
    self->file = 0;
    self->fileBuffer = self->packBuffer = 0;
    self->loadPhase = 0;
    self->loaded = 0;
    self->active = 1;
    self->_60 = 1;
    self->frameRate = *gFrameRate;
    self->indoors = 0;
    self->interior = -1;
    self->_68 = self->_6c = self->_70 = 0;
    self->partner = -1;
    self->swordPackReady = 0;
    memset(self, 0, 0x30);
    memset(reinterpret_cast<u8*>(self) + 0x30, 0, 0x30);
    self->model = model;
    self->_8c = 0;
    self->model2 = model;
    self->fileSize = 0;
    self->arg = arg;
    self->_9c = -1;
    self->_a0 = -1;
    self->packSize = 0;
    self->kind = kind;
    self->id = id;
    self->_b0 = 1;
    self->visible = 1;
    self->cameraDistance = 0.0f;
    self->aboveGround = 0.0f;
    self->_b5 = 0;
    self->lookState = 0;
    ResetState(self);
    // Everyone but the player fades in from dark.
    self->tint[0] = self->tint[1] = self->tint[2] = id == 0 ? 0.0f : 8.0f;
    self->_14c = 1.0f;

    CharaResources* res = CharaResources_Get();
    self->resources = guest::addr(res);
    CharaParams* p = CharaResources_GetParams(res, id);
    self->params = guest::addr(p);
    if (const CharaParams* t = CharaResources_FindParamTemplate(res, kind)) {
        // Its own copy of the template: everything but the id, and the name
        // by value.
        const s32 ownId = p->id;
        const WString name = p->name;
        const u32 nameVtable = p->nameVtable;
        memcpy(p, t, sizeof(CharaParams));
        p->name = name;
        p->nameVtable = nameVtable;
        const u16* text = P<const u16>(P<WStringRep>(t->name.rep)->data);
        int length = 0;
        while (text[length] != 0)
            length++;
        WString_Assign(&p->name, text, text + length);      // 0x194CA0
        p->id = ownId;
        p->link[4] = guest::addr(&self->active);
        // (The original first asks the name table at CharaResources+4 for a
        // name of this kind — 0x1D2A30 — which is not decompiled.)
        p->_b0 = P<WStringRep>(p->name.rep)->data;
    }
    p->link[2] = guest::addr(&self->_150);

    void* body = MemAlloc(sizeof(CharaBody));
    self->body = body ? guest::addr(CharaBody_Ctor(static_cast<CharaBody*>(body), p)) : 0;
    void* control = MemAlloc(sizeof(CharaControl));
    self->action = control ? guest::addr(CharaControl_Ctor(static_cast<CharaControl*>(control))) : 0;
    CharaControl_Attach(Control(self), Body(self));
    memset(self->_1ac, 0, sizeof self->_1ac);
    self->flagHandle = 0;
    self->flagHandles[0] = self->flagHandles[1] = 0;
    self->flagArg = 0;
    self->flagArgs[0] = 0;
    CharaParts_Create(self);
    Matrix_Copy(p->matrix, P<float>(0x2D1A80u));            // identity
    if (arg == -1) {
        self->arg = CharaResources_PackOf(res, model);
        if (self->arg == -1)
            self->arg = 0;
    }
    self->listParam[0] = CharaResources_GetParam(res, model, 0);
    self->listParam[1] = CharaResources_GetParam(res, model, 1);
    return self;
}

// ── 0x1D06B0 ──────────────────────────────────────────────────────────────
Chara* Chara_BaseDtor(Chara* self, s16 flag) {
    if (!self)
        return self;
    self->vtable = kVtCharaBase;
    self->loadPhase = 0;
    self->loaded = 0;
    if (self->file != 0)
        ArchiveFile_Destroy(P<ArchiveFile>(self->file));
    self->file = 0;
    if (self->packBuffer != 0)
        MemFreeAligned(P(self->packBuffer));
    CharaParams* p = Params(self);
    if (p->link[1] != 0) {
        Skeleton* skeleton = P<Skeleton>(p->link[1]);       // 0x139330
        MemFree(P(skeleton->bones));
        MemFree(skeleton);
    }
    p->link[0] = p->link[1] = 0;
    p->link[4] = p->link[5] = p->link[6] = p->link[7] = p->link[8] = 0;
    p->link[2] = 0;
    if (self->action != 0)
        CharaControl_Dtor(Control(self), 1);
    self->action = 0;
    if (self->body != 0)
        CharaBody_Dtor(Body(self), 1);
    self->body = 0;
    CharaParts_Destroy(self);
    CharaModels_Release(self);
    CharaResources* res = P<CharaResources>(self->resources);
    CharaResources_ReleasePack(res, self->arg);
    self->arg = -1;
    if (self->_9c != -1) {
        if (self->swordPackReady != 0)
            CharaResources_ReleasePack(res, self->_9c);
        self->_9c = -1;
    }
    if (self->flagArg != 0)
        MemFree(P(self->flagArg));
    self->flagArg = 0;
    if (self->flagArgs[0] != 0)
        MemFree(P(self->flagArgs[0]));
    self->flagArgs[0] = 0;
    if (flag > 0)
        MemFree(self);
    return self;
}

// ═══ Loading ══════════════════════════════════════════════════════════════

// ── 0x1CF030 ──────────────────────────────────────────────────────────────
u32 Chara_InstallMotions(Chara* self, int fromFile, s32 pack, int) {
    CharaResources* res = P<CharaResources>(self->resources);
    u32 table;
    if (!fromFile) {
        table = CharaResources_InstallPack(res, pack, nullptr);
    } else {
        table = CharaResources_InstallPack(res, pack, P<const u32>(self->packBuffer));
        MemFreeAligned(P(self->packBuffer));
        self->packBuffer = 0;
        if (table == 0)
            return 0;
    }
    CharaVoice_Refresh(P(self->voice));
    return table;
}

// Starts reading a motion pack's file into `packBuffer`.
static int ReadPack(Chara* self, s32 pack) {
    const char* path = CharaResources_PackPath(P<CharaResources>(self->resources), (u32)pack);
    if (!path)
        return 0;
    ArchiveFile* file = ArchiveFile_Create();
    ArchiveFile_OpenByName(file, path);
    self->file = guest::addr(file);
    self->packSize = (s32)ArchiveFile_GetSize(file);
    self->packBuffer = guest::addr(MemAllocAligned(0x40, (u32)self->packSize));
    if (self->packBuffer != 0) {
        ArchiveFile_ReadAsync(file, P(self->packBuffer), (u32)self->packSize);
        return 1;
    }
    ArchiveFile_Destroy(file);
    self->file = 0;
    return 0;
}
static int FinishLoad(Chara* self) {
    self->loadPhase = 0;
    self->loaded = 1;
    Skeleton_Update(Bones(self), Params(self)->matrix);
    return 1;
}
// 0x1CFD70: its own motion pack.
static int StartPack(Chara* self) {
    CharaResources* res = P<CharaResources>(self->resources);
    if (!CharaResources_HasPack(res, self->arg))
        return ReadPack(self, self->arg);
    Body(self)->packA = Chara_InstallMotions(self, 0, self->arg, 0);
    CharaBody_PlayScripted(Body(self), 0, 0, 1, 0, 0, 0);
    self->loadPhase = 3;
    return 1;
}
// 0x1CFB90: its sword's.
static int StartSwordPack(Chara* self) {
    if (self->_9c == -1)
        return FinishLoad(self);
    CharaResources* res = P<CharaResources>(self->resources);
    if (!CharaResources_HasPack(res, self->_9c))
        return ReadPack(self, self->_9c);
    if (self->_9c != self->_a0) {
        VirtualCall<int, Chara*>(self->vtable, kCharaUnloadSword, self);
        Body(self)->packB = Chara_InstallMotions(self, 0, self->_9c, self->id == 0);
    }
    self->swordPackReady = 1;
    return FinishLoad(self);
}

// ── 0x1CFA50 ──────────────────────────────────────────────────────────────
int Chara_StartLoadPhase(Chara* self, int phase) {
    if (self->file != 0)
        ArchiveFile_Destroy(P<ArchiveFile>(self->file));
    self->file = 0;
    switch (phase) {
    case 0: case 1: case 2:
    case 5: case 6: case 8: case 9:
        return CharaModels_Start(self, phase);
    case 3: return StartPack(self);
    case 4: return StartSwordPack(self);
    }
    return 0;
}

// ── 0x1CF580 ──────────────────────────────────────────────────────────────
int Chara_PollLoadPhase(Chara* self, int phase) {
    if (self->file == 0)
        return 1;
    ArchiveFile* file = P<ArchiveFile>(self->file);
    if (ArchiveFile_IsBusy(file))
        return 0;
    ArchiveFile_Close(file);
    ArchiveFile_Destroy(file);
    self->file = 0;
    switch (phase) {
    case 3: {
        const u32 table = Chara_InstallMotions(self, 1, self->arg, 0);
        Body(self)->packA = table;
        if (table == 0)
            return -1;
        CharaBody_PlayScripted(Body(self), 0, 0, 1, 0, 0, 0);
        self->loadPhase = 3;
        return 1;
    }
    case 4: {
        VirtualCall<int, Chara*>(self->vtable, kCharaUnloadSword, self);
        const u32 table = Chara_InstallMotions(self, 1, self->_9c, self->id == 0);
        Body(self)->packB = table;
        if (table == 0)
            return -1;
        self->swordPackReady = 1;
        return FinishLoad(self);
    }
    }
    return 1;       // (the model phases never leave a file open here)
}

// ═══ Small methods the character manager calls ════════════════════════════

void Chara_fn1cca90(Chara* self) {                                          // 0x1CCA90
    if (Body(self)->flags & kBodyCarrying)
        CharaSheath_fn242ec0(P(self->sheath), Body(self));
}
void Chara_fn1ccad0(Chara* self, u32 arg) {                                 // 0x1CCAD0
    if (arg == 0)
        Body(self)->flags &= ~(u32)kBodyCarrying;
    else
        Body(self)->flags |= kBodyCarrying;
    *P<u32>(self->sheath) = arg;
}
void Chara_fn1ccb20(Chara* self, u32 arg) { *P<u32>(self->sheath) = arg; }  // 0x1CCB20
void Chara_fn1ccb30(Chara* self) {                                          // 0x1CCB30
    CharaSheath_fn242e80(P(self->sheath), Body(self), (Body(self)->state & 0xF00000) == 0x800000);
}
void Chara_fn1ccb90(Chara* self) {                                          // 0x1CCB90
    switch (Body(self)->state & 0xF00000) {
    case 0x400000: CharaSheath_fn242ff0(P(self->sheath), Body(self)); break;
    case 0x500000: CharaSheath_fn242f40(P(self->sheath), Body(self)); break;
    case 0x600000: CharaSheath_fn242ec0(P(self->sheath), Body(self)); break;
    case 0x700000: CharaSheath_fn243240(P(self->sheath), Body(self)); break;
    }
}
void Chara_fn1ccc30(Chara* self, int drop) {                                // 0x1CCC30
    switch (Body(self)->state & 0xF00000) {
    case 0x100000: CharaSheath_fn2432c0(P(self->sheath), Body(self), drop); break;
    case 0x200000: CharaSheath_fn243150(P(self->sheath), Body(self), drop); break;
    case 0x300000: CharaSheath_fn243070(P(self->sheath), drop); break;
    }
}
void Chara_fn1cccb0(Chara* self) { CharaSheath_fn242e10(P(self->sheath), Body(self)); }     // 0x1CCCB0
// A task's motion: `arg` is the request {action, loops, blend, start, end, still}.
void Chara_fn1cccc0(Chara* self, u32 arg) {                                 // 0x1CCCC0
    if (self->loaded != 0)
        CharaBody_PlayScript(Body(self), P<const s32>(arg));
}
void Chara_fn1cccf0(Chara* self) {                                          // 0x1CCCF0
    if (Body(self)->flags & kBodyParried)
        CharaBody_fn1d1200(Body(self));
}
int Chara_fn1cce20(Chara* self, u32 a, u32 b) {                             // 0x1CCE20
    if (self->loaded == 0)
        return 0;
    const u8* bones = P<u8>(Bones(self)->bones);
    memcpy(P(a), bones + 0x210, 16);        // bone 3's place
    memcpy(P(b), bones + 0x3F0, 16);        // bone 6's
    return 1;
}
// Held by something the collision world found: keep the height, take x and z back.
void Chara_fn1ccfa0(Chara* self) {                                          // 0x1CCFA0
    if (self->flagHandle == 0 || (P<u32>(self->flagHandle)[0x174 / 4] & 1) == 0)
        return;
    float* pos = &Params(self)->matrix[12];
    pos[0] = Body(self)->lastPosition[0];
    pos[2] = Body(self)->lastPosition[2];
}
void Chara_fn1cd690(Chara* self, u32 v) {                                   // 0x1CD690
    P<CharaSwords>(self->swords)->_8 = v;
    Body(self)->techniqueCount = v;
}
void Chara_fn1cd6b0(Chara* self, u32 v) { Body(self)->comboRoot = v; }      // 0x1CD6B0
void Chara_fn1cd850(Chara* self, u32 v) { P<CharaSwords>(self->swords)->_0 = v; }   // 0x1CD850

// What a sword in hand means for the body.
static void UseSword(Chara* self, const void* sword) {
    const u8* held = P<u8>(P<CharaSwords>(self->swords)->sword);
    const u8* given = static_cast<const u8*>(sword);
    Body(self)->techniques = guest::addr(held + 0x90);
    memcpy(&Body(self)->swordValue, given + 0x74, 4);
    s32 type;
    memcpy(&type, given + 0x7C, 4);
    Params(self)->sword = (s16)type;
    self->_a0 = self->_9c;
}
// ── 0x1CD6C0 ──────────────────────────────────────────────────────────────
void Chara_fn1cd6c0(Chara* self, void* sword, u32 a) {
    CharaSwords_Give(P(self->swords), sword);
    UseSword(self, sword);
    if (a == 0)
        memcpy(&self->_9c, P<u8>(P<CharaSwords>(self->swords)->sword) + 0x6C, 4);
    else
        self->_9c = -1;
}
// ── 0x1CD770 ──────────────────────────────────────────────────────────────
int Chara_TakeSword(Chara* self, void* sword) {
    CharaSwords_Take(P(self->swords), sword);
    UseSword(self, sword);
    memcpy(&self->_9c, P<u8>(P<CharaSwords>(self->swords)->sword) + 0x6C, 4);
    if (self->_a0 == -1)
        return 0;
    // It had another: back to standing unarmed while the new style loads.
    Body(self)->flags &= ~(u32)kBodyArmed;
    CharaBody_PlayScripted(Body(self), 0, 0, 1, 0, 0, 0);
    CharaSheath_fn243240(P(self->sheath), Body(self));
    return 1;
}

// ── 0x1CD860 ──────────────────────────────────────────────────────────────
// What the collision world is given to know this character by.
void Chara_fn1cd860(Chara* self) {
    u32* body = static_cast<u32*>(MemAlloc(0x20));
    self->flagArg = guest::addr(body);
    body[0] = self->params;
    body[1] = guest::addr(&self->loaded);
    body[2] = guest::addr(&self->active);
    body[3] = guest::addr(&Body(self)->stagger);
    body[4] = guest::addr(Body(self)->lastPosition);
    body[5] = guest::addr(Body(self)->position);
    body[6] = self->swords + 4;
    body[7] = guest::addr(&self->_1dc);
    u32* sword = static_cast<u32*>(MemAlloc(0x1C));
    self->flagArgs[0] = guest::addr(sword);
    sword[0] = self->params;
    sword[1] = guest::addr(&self->loaded);
    sword[2] = guest::addr(&self->active);
    sword[4] = guest::addr(&Body(self)->inputRating);
    sword[3] = self->body + 0x1A8;
    sword[6] = self->swords + 4;
    sword[5] = self->swords;
}

// ── 0x1CDB00 ──────────────────────────────────────────────────────────────
// Down onto the ground of the cell it stands on.
void Chara_fn1cdb00(Chara* self) {
    if (self->cellFlags & 2)
        return;
    memcpy(&Params(self)->matrix[13], reinterpret_cast<u8*>(self) + 0x168, 4);
}
// ── 0x1CDB70 ──────────────────────────────────────────────────────────────
void Chara_ClearTransient(Chara* self) {
    *reinterpret_cast<u32*>(reinterpret_cast<u8*>(self) + 0x168) = 0;
    self->cellFlags = 2;
    self->cellIndex = 0;
}
// ── 0x1CDB90 ──────────────────────────────────────────────────────────────
int Chara_SetAction(Chara* self, u32 routine) {
    return routine != 0 ? (int)CharaControl_Set(Control(self), routine) : 0;
}
// ── 0x1CEC20 ──────────────────────────────────────────────────────────────
// Whether the map step may place it this frame: not while it stands idle.
void Chara_fn1cec20(Chara* self) {
    CharaBody* body = Body(self);
    body->mapFlags = 0;
    if ((body->flags & kBodyIdle) == 0 || (body->flags & kBodyInAir))
        body->mapFlags |= 1;
}
// ── 0x1CEDB0 ──────────────────────────────────────────────────────────────
void Chara_LookAt(Chara* self, const float* point) {
    Copy(self->lookAt, point);
    self->_b5 = 1;
}

// ═══ The frame ════════════════════════════════════════════════════════════

// ── 0x1CED80 (vtable +0x0C) ───────────────────────────────────────────────
static void Chara_BeginFrame(Chara* self) {
    CharaSwords_BeginFrame(P(self->swords));
    CharaBody_BeginFrame(Body(self));
}

// ── 0x1CEDE0 ──────────────────────────────────────────────────────────────
// Turning the head: to the opponent, or to a point it was told to look at.
static void UpdateLook(Chara* self) {
    CharaParams* p = Params(self);
    if (p->hp <= 0)
        return;
    alignas(16) float matrix[16];
    Matrix_Copy(matrix, p->matrix);
    void* look = P(self->look);
    Skeleton* skeleton = Bones(self);
    const u32 opponent = p->link[3];
    if ((Body(self)->state & kStNoLook) == 0 && (opponent != 0 || self->_b5 != 0)) {
        if (CharaLook_State(look) != 1)
            CharaLook_Begin(look, skeleton);
        if (self->_b5 == 0) {
            const CharaParams* o = P<CharaParams>(opponent);
            if (o->link[4] != 0 && *P<s32>(o->link[4]) != 0) {
                float at[4];
                Copy(at, &o->matrix[12]);
                memcpy(&at[1], &o->_3c, 4);         // (its eye height…)
                at[1] += o->matrix[13];             // …above where it stands
                CharaLook_SetTarget(look, at);
            }
        } else {
            CharaLook_SetTarget(look, self->lookAt);
        }
        CharaLook_Apply(look, skeleton, matrix);
        self->lookState = 0;
        return;
    }
    const s16 state = CharaLook_State(look);
    if (state == 1) {
        CharaLook_End(look);
        CharaLook_Apply(look, skeleton, matrix);
    } else if (state == 2) {
        CharaLook_Apply(look, skeleton, matrix);
    } else if (state == 0) {
        // After putting the sword away the head comes back to the front.
        const bool armed = (Body(self)->flags & kBodyArmed) != 0;
        if (self->lookState == 0) {
            if (armed) self->lookState = 1;
        } else if (self->lookState == 1 && !armed) {
            self->lookState = 2;
        }
        if (self->lookState == 2) {
            const float* bones = P<float>(skeleton->bones);
            const float* a = bones + 0x5C0 / 4;
            const float* b = bones + 0xCA0 / 4;
            if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] < 0.98f)
                CharaLook_End(look);
            else
                self->lookState = 0;
        }
    }
}

// ── 0x1CEC30 (vtable +0x10) ───────────────────────────────────────────────
static void Chara_Update(Chara* self) {
    if (self->loaded == 0)
        return;
    if (self->tint[0] <= self->light[0]) {
        const float step = (float)*gFrameRate / 30.0f * 16.0f;
        self->tint[0] += step;
        self->tint[1] += step;
        self->tint[2] += step;
    }
    CharaControl_Step(Control(self));
    CharaBody_Update(Body(self), Bones(self));
    UpdateLook(self);
    CharaVoice_Update(P(self->voice));
    CharaBody* body = Body(self);
    if ((body->actionFlags & kActfFrozen) == 0)
        for (int what = CharaBody_SoundNow(body); what != -1; what = CharaBody_SoundNow(body))
            CharaSounds_Play(P(self->sounds), self, what);
    CharaFace_Update(P(self->_1d8), self, body);
    CharaEffects_Update(P(self->effects), body);
    CharaSheath_Update(P(self->sheath), body, Params(self));
}

// ── 0x1CEAF0 ──────────────────────────────────────────────────────────────
// The light of the cell underfoot, eased in over four frames.
static void UpdateLight(Chara* self, const ColCellType* cell) {
    self->lightCell[0] = (float)cell->light[0];
    self->lightCell[1] = (float)cell->light[1];
    self->lightCell[2] = (float)cell->light[2];
    self->lightCell[3] = 1.0f;
    if (self->lightTo[0] == self->lightCell[0] && self->lightTo[1] == self->lightCell[1] &&
        self->lightTo[2] == self->lightCell[2]) {
        if (self->lightSteps++ < 4) {
            for (int i = 0; i < 4; i++)
                self->light[i] += self->lightStep[i];
        } else {
            Copy(self->light, self->lightCell);
        }
    } else {
        for (int i = 0; i < 4; i++)
            self->lightStep[i] = (self->lightCell[i] - self->lightTo[i]) / 4.0f;
        Copy(self->light, self->lightTo);
        Copy(self->lightTo, self->lightCell);
        self->lightSteps = 0;
    }
    self->light[3] = 1.0f;
}

// ── 0x1CE080 ──────────────────────────────────────────────────────────────
// The way is blocked: slide along the wall.  The move is split along the
// axis it mostly follows and the other; each half is tried by itself.
static void Slide(Chara* self, MapCollision* map) {
    static const float (*const kAxis)[4] = guest::ptr<const float[4]>(0x2D8580u);  // twelve headings
    CharaBody* body = Body(self);
    float* pos = &Params(self)->matrix[12];
    float move[4];
    Copy(move, CharaBody_Moved(body));
    int nearest = 0;
    float least = 1.0f;
    for (int i = 0; i < 12; i++) {
        const float d = kAxis[i][0] * move[0] + kAxis[i][1] * move[1] + kAxis[i][2] * move[2];
        if (d < least) {
            least = d;
            nearest = i;
        }
    }
    int axis;
    if (nearest < 6) {
        move[2] = 0.0f;
        axis = 0;
    } else {
        move[0] = 0.0f;
        axis = 2;
    }
    const ColHeader* header = Map_Header(map);
    if (least > -0.2f && (body->actionFlags & 0xF0000)) {
        const float nudge = header->cellSize * ((body->actionFlags & kActfWalk) ? 0.00025f : 0.0005f);
        auto place = [&](const float* at) {
            ColCellType* cell = Map_CellAt(map, at);
            if ((cell->flags & kCellFlag1) == 0)
                return false;
            float to[4];
            Copy(to, at);
            to[1] = cell->height;
            Copy(pos, to);
            return true;
        };
        float to[4];
        for (int i = 0; i < 4; i++)
            to[i] = body->lastPosition[i] + move[i];
        if (Map_LineBlocked(map, body->lastPosition, to, 0) == nullptr && place(to))
            return;
        // A cell's worth along one axis, then the other.
        const float* whole = CharaBody_Moved(body);
        for (int k : {axis, axis == 0 ? 2 : 0}) {
            Copy(to, body->lastPosition);
            to[k] += whole[k] >= 0.0f ? nudge : -nudge;
            if (place(to))
                return;
        }
    }
    Copy(pos, body->lastPosition);
}

// ── 0x1CE580 (vtable +0x14) ───────────────────────────────────────────────
// Takes the position the body wants and settles it against the map: the
// ground's height, walls, drops (falling) and landings.
void Chara_StepMap(Chara* self, MapCollision* map) {
    if (self->loaded == 0)
        return;
    CharaParams* p = Params(self);
    CharaBody* body = Body(self);
    float* pos = &p->matrix[12];
    UpdateLight(self, Map_CellAt(map, pos));
    if (body->mapFlags & 1) {
        // (Every lookup returns the same record inside the map object, so
        // after this one `here` describes the cell it wants to go to — the
        // original relies on that.)
        float want[4];
        Copy(want, body->position);
        ColCellType* there = Map_CellAt(map, want);
        memcpy(&self->_150, there, sizeof(ColCellType));
        if ((there->flags & kCellBlocked) && (body->flags & kBodyInAir))
            body->mapFlags |= 2;
        if (body->mapFlags & 2) {
            // In the air over something solid: only the height changes.
            pos[1] = want[1];
        } else if ((there->flags & kCellFlag1) == 0) {
            Slide(self, map);
        } else if (body->flags & kBodyInAir) {
            Copy(pos, want);
        } else if (there->height - body->lastPosition[1] < 0.5f) {
            float to[4];
            Copy(to, want);
            to[1] = there->height;
            Copy(pos, to);
        } else {
            Copy(pos, body->lastPosition);      // a step up of half a unit or more
        }

        // The ground under where that left it.
        auto groundAt = [&](const float* at) {
            ColCellType* cell = Map_CellAt(map, at);
            if (cell->flags & kCellBlocked) {
                float above[4];
                Copy(above, at);
                above[1] += 1.0f;
                Map_CellAt(map, above);
            }
            cell = Map_CellAt(map, pos);
            if (cell->flags & kCellFlag1)
                self->aboveGround = fabsf(pos[1] - cell->height);
            else if (cell->flags & kCellBlocked)
                cell->height = body->takeoff[1];
            return cell->height;
        };
        if ((body->flags & kBodyInAir) == 0) {
            const float y = pos[1];
            if (groundAt(want) < y - 0.5f) {
                CharaBody_Fall(body, 0);        // it has dropped away
            } else {
                float to[4];
                Copy(to, want);
                ColCellType* cell = Map_CellAt(map, to);
                if ((cell->flags & kCellFlag1) == 0) {
                    to[1] += 1.0f;
                    cell = Map_CellAt(map, to);
                }
                if (cell->flags & kCellFlag1) {
                    memcpy(&self->_150, cell, sizeof(ColCellType));
                    to[1] = cell->height;
                    Copy(pos, to);
                }
            }
        } else {
            float at[4];
            Copy(at, pos);
            const float ground = groundAt(at);
            if (at[1] <= ground + 0.125f && body->velocity[1] <= 0.0f) {
                CharaBody_Land(body);
                at[1] = ground;
                Copy(pos, at);
            }
        }
    }
    alignas(16) float matrix[16];
    Matrix_Copy(matrix, p->matrix);
    Skeleton_UpdateWorld(Bones(self), matrix);
    CharaLook_Feet(P(self->look), Bones(self), map, p->matrix);
    // Eye height: the head joint above the feet.
    const float head = *P<float>(Bones(self)->bones + 0xCB4);
    const float eye = head - p->matrix[13];
    memcpy(&p->_3c, &eye, 4);
}

// ── 0x1CDBC0 (vtable +0x18) ───────────────────────────────────────────────
static int Chara_Draw(Chara* self, int shown, const float* wind) {
    if (self->loaded == 0)
        return 0;
    alignas(16) float matrix[16];
    Matrix_Copy(matrix, Params(self)->matrix);
    Skeleton_UpdateWorld(Bones(self), matrix);
    // Hair and cloth swing — on the dead, without the wind.
    alignas(16) static const float kStill[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // 0x306900
    Skeleton_Swing(Bones(self), (Body(self)->flags & kBodyDead) ? kStill : wind);
    if (shown) {
        if (self->swords != 0)
            CharaSwords_DrawShadow(P(self->swords), Body(self));     // 0x245880: the blade's trail
        CharaModels_Draw(self, self->cameraDistance < 12.0f ? (self->bodyType == 1 ? 0 : self->_b0) : 2);
        if (self->swords != 0)
            CharaSwords_Draw(P(self->swords), Body(self));
    }
    return self->loaded;
}

// ── Culling: 0x269920 (the player) / 0x269240 ─────────────────────────────
static int CharaPlayer_Cull(Chara* self) { return self->visible ? self->loaded : 0; }
static int CharaOther_Cull(float nearest, Chara* self, int pass) {
    if (!self->visible)
        return 0;
    if (pass != 0 && nearest > self->cameraDistance)
        return 0;
    return self->loaded;
}

// ── 0x1CD550 (vtable +0x2C) ───────────────────────────────────────────────
// What the collision world found this frame.
static int Chara_StepFlags(Chara* self) {
    if (self->flagHandle == 0)
        return 0;                   // (no collision world yet: nothing can have happened)
    VirtualCall<int, Chara*>(self->vtable, kCharaOnDamage, self);
    const u32 found = P<u32>(self->flagHandle)[0x174 / 4];
    if (found & 0x3000)
        VirtualCall<int, Chara*>(self->vtable, kCharaOnGuard, self);
    VirtualCall<int, Chara*>(self->vtable, kCharaOnHit, self);
    if (found & 4)
        CharaBody_Bumped(Body(self));
    if (found & 8)
        CharaBody_Bumped(Body(self));
    if ((found & 2) == 0)
        return 0;
    const s32 hurt = (s32)P<u32>(self->flagHandle)[0x190 / 4];
    CharaBody_Hurt(Body(self), (u32)hurt);
    if (hurt != -1)
        CharaBody_Knock(Body(self), hurt, 0);
    Body(self)->flags |= kBodyBumped;
    return 1;
}

// ═══ Blows (what the collision world found) ═══════════════════════════════
// The base class's methods; what the two classes add to them (the player's
// tallies, statistics and pad rumble; what the others drop when they die)
// follows after them.

static inline BodyVolume* Hit(Chara* c) { return P<BodyVolume>(c->flagHandle); }
static inline SwordVolume* Blade(Chara* c, int i) { return P<SwordVolume>(c->flagHandles[i]); }
static void Sfx(Chara* self, int sound, int effect) {
    Sound_PlaySE(sound, 1, 0x3FFF);             // (at its position)
    CharaSounds_Play(P(self->sounds), self, effect);
}

// ── 0x1CD970 (vtable +0x44) ───────────────────────────────────────────────
// While an attack's hit frames last, its sword volumes are live.
static int Chara_ArmAttack(Chara* self) {
    if (self->loaded == 0 || self->flagHandles[0] == 0 || self->flagHandles[1] == 0)
        return 0;
    CharaBody* body = Body(self);
    if (!CharaBody_IsAttack(body) || !CharaBody_CanHit(body)) {
        Blade(self, 0)->base.on = 0;
        Blade(self, 1)->base.on = 0;
        return 0;
    }
    const s32 bone = CharaBody_AttackKind(body);
    if (bone == 0x100) {
        // Both hands.
        static const s32 kHand[2] = {28, 29};                   // 0x2EDCD8
        int armed = 0;
        for (int i = 0; i < 2; i++) {
            if (Blade(self, i)->base.on != 0)
                continue;
            SwordVolume_Arm(Blade(self, i), CharaBody_AttackStrength(body), kHand[i]);
            Blade(self, i)->base.on = 1;
            armed = 1;
        }
        return armed;
    }
    const int armed = Blade(self, 0)->base.on == 0;
    if (armed) {
        SwordVolume_Arm(Blade(self, 0), CharaBody_AttackStrength(body), bone);
        Blade(self, 0)->base.on = 1;
    }
    Blade(self, 1)->base.on = 0;
    return armed;
}

// ── 0x1CD010 (vtable +0x38) ───────────────────────────────────────────────
// Its own blows: what they met.
static int Chara_OnHit(Chara* self) {
    if (self->flagHandles[0] == 0 || self->flagHandles[1] == 0)
        return 0;
    CharaBody* body = Body(self);
    int landed = 0;
    for (int i = 0; i < 2; i++) {
        SwordVolume* blade = Blade(self, i);
        const u32 found = blade->base.found;
        if ((found & 0xF) == 0)
            continue;
        if (found & (kSwordCut | kSwordStruck)) {
            body->state |= kStHitA;
            body->targetFlags |= 0x400;
        }
        if (found & (kSwordGuarded | kSwordParried)) {
            VirtualCall<int, Chara*, s32>(self->vtable, kCharaWearSword, self, blade->wear);
            blade->base.on = 2;
            body->state |= kStHitB;
        }
        if (found & kSwordCut)
            VirtualCall<int, Chara*, s32>(self->vtable, kCharaWearSword, self, blade->wear);
        if (found & kSwordGuarded) {
            body->state &= ~(u32)kStHitA;
            CharaBody_Freeze(body);
        }
        if (found & kSwordParried)
            body->targetFlags |= 0x400;
        if (found & 0x10)
            body->flags |= 0x100000;
        if (found & (kSwordLanded | kSwordHitCorpse))
            landed = 1;
    }
    return landed;
}

// ── 0x1CCEA0 (vtable +0x34) ───────────────────────────────────────────────
// A blow taken on the guard: the swords clash, or the guard is broken.
static int Chara_OnGuard(Chara* self) {
    BodyVolume* hit = Hit(self);
    CharaBody* body = Body(self);
    if ((hit->base.found & kHitParried) == 0) {
        CharaBody_Clash(body, hit->damage > 200);
        Sfx(self, 0x19, 6);
        body->_2c = hit->clash;
    } else {
        Sfx(self, 0x1C, 0x1C);
        CharaBody_GuardBreak(body);
    }
    VirtualCall<int, Chara*, s32>(self->vtable, kCharaWearSword, self, hit->guardDamage);
    body->targetFlags |= 0x800;
    CharaBody_Freeze(body);
    return 1;
}

// ── 0x1CD1A0 (vtable +0x30) ───────────────────────────────────────────────
// A blow that landed: health, the sound of it, dying, and the reaction.
static int Chara_OnDamage(Chara* self) {
    static const s32 kCutSound[4] = {30, 30, 31, 50}, kCutEffect[4] = {7, 8, 9, 9};        // 0x2D8640 / 0x2D8650
    static const s32 kBluntSound[4] = {34, 34, 34, 50}, kBluntEffect[4] = {11, 12, 13, 13}; // 0x2D8660 / 0x2D8670
    BodyVolume* hit = Hit(self);
    const u32 found = hit->base.found;
    if ((found & 0xF0) == 0)
        return 0;
    CharaParams* p = Params(self);
    CharaBody* body = Body(self);
    const s32 damage = hit->damage;
    s32 health = p->hp - damage;
    if (health < p->_48) health = p->_48;       // (some cannot go below a floor)
    if (health > p->maxHp) health = p->maxHp;
    p->hp = (s16)health;

    if (found & kHitStruck) {
        Sfx(self, 0x22, 10);
    } else if (found & (kHitCut | kHitCutBlunt)) {
        const int level = (found & kHitMarked) ? 3 : damage > 199 ? 2 : damage > 99 ? 1 : 0;
        if (found & kHitCut)
            Sfx(self, kCutSound[level], kCutEffect[level]);
        else
            Sfx(self, kBluntSound[level], kBluntEffect[level]);
    }
    // A finishing move: the victim is stood in front of its killer.
    if ((found & kHitFinished) && (body->flags & kBodyCorpse) == 0 && self->_1dc != 0) {
        MotionPlayer_ClearFlags(P<MotionPlayer>(body->player), kMotionRoot);
        alignas(16) float m[16];
        Matrix_Copy(m, P<CharaParams>(self->_1dc)->matrix);
        for (int i = 0; i < 4; i++) {
            m[i] = -m[i];
            m[8 + i] = -m[8 + i];
        }
        Matrix_Copy(p->matrix, m);
        body->flags = (body->flags & ~0xFu) | kBodyStanding;
    }
    if ((body->flags & kBodyDead) == 0) {
        if (p->hp < 1 && p->_48 == -1) {
            if (found & kHitCut)
                CharaSounds_Play(P(self->sounds), self, 0xE);
            CharaFace_Set(P(self->_1d8), p, 0xB);
            body->flags |= kBodyDead;
            body->flags &= ~(u32)kBodyArmed;
            p->link[3] = 0;
        } else {
            CharaFace_Set(P(self->_1d8), p, 2);
        }
        body->flags |= kBodyHit;
        body->targetFlags |= 0x800;
        if (body->_40 != 0) {
            body->_40 = 0;
            if (body->targetFlags & 8)
                body->_3c = 1;
        }
    } else if ((body->flags & kBodyCorpse) == 0) {
        CharaFace_Set(P(self->_1d8), p, 0xB);
    }
    const s32 reaction = CharaBody_Hurt(body, hit->hurt);
    if (reaction != -1)
        CharaBody_Knock(body, reaction, hit->power);
    return 1;
}

// ── 0x1CCD50 (vtable +0x40) ───────────────────────────────────────────────
// Leaning into a blow at the right moment turns it aside.
static int Chara_TryParry(Chara* self) {
    if (self->_1dc == 0)
        return 0;
    const CharaParams* attacker = P<CharaParams>(self->_1dc);
    const u32 theirs = attacker->link[6] ? *P<u32>(attacker->link[6]) : 0;
    if (!CharaBody_Parries(Body(self), theirs) || !CharaBody_Parry(Body(self)))
        return 0;
    Sfx(self, 0xE, 0x1D);
    if (attacker->link[5] != 0)
        *P<u32>(attacker->link[5]) |= kBodyParried;
    return 1;
}

// ── Destructors and constructors (0x269BA0 / 0x2692D0, 0x269C00 / 0x269330) ─
static Chara* CharaPlayer_Dtor(Chara* self, s16 flag) {
    if (self) {
        self->vtable = kVtCharaPlayer;
        Chara_BaseDtor(self, 0);
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
static Chara* CharaOther_Dtor(Chara* self, s16 flag) {
    if (self) {
        self->vtable = kVtCharaOther;
        Chara_BaseDtor(self, 0);
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
Chara* Chara_CtorA(void* mem, int id, int kind, int model, int arg) {
    Chara* self = Chara_BaseCtor(static_cast<Chara*>(mem), id, kind, model, arg);
    self->vtable = kVtCharaPlayer;
    return self;
}
Chara* Chara_CtorB(void* mem, int id, int kind, int model, int arg) {
    Chara* self = Chara_BaseCtor(static_cast<Chara*>(mem), id, kind, model, arg);
    self->vtable = kVtCharaOther;
    self->deathReported = 0;
    return self;
}

// 0x269460 / 0x269030: drop the sword style it no longer uses.
static int Chara_UnloadSword(Chara* self) {
    const s32 old = self->_a0;                              // 0x1CCE10
    self->_a0 = -1;
    if (old == -1)
        return 0;
    CharaResources_ReleasePack(P<CharaResources>(self->resources), old);
    return 1;
}

// ── 0x1CD660 (vtable +0x48), with the player's 0x2694C0 ───────────────────
static int Chara_WearSword(Chara* self, s32 amount) {
    CharaSwords_Wear(P(self->swords), amount, Params(self));
    return 0;
}
// Some of the player's swords (effects 0x18–0x1C) take less strain; a
// sword that breaks is counted.
static int CharaPlayer_WearSword(Chara* self, s32 amount) {
    const u32 sword = P<CharaSwords>(self->swords)->sword;
    if (sword != 0) {
        const s32 effect = *P<s32>(sword + 0x8C);
        if (effect >= 0x18 && effect <= 0x1C)
            amount -= (s32)((float)amount * (float)(effect - 0x17) * 0.1f);
    }
    if (CharaSwords_Wear(P(self->swords), amount, Params(self)) == 2) {
        static const GuestPtrVar<u8> gPlayer{0x2EE688u};
        ++*reinterpret_cast<s16*>(*gPlayer + 0x862);
    }
    return 0;
}

// ═══ What the two classes add to the blow methods ═════════════════════════
extern "C" {
    void  CharaSwords_AddTally(void* swords, int amount);                       // 0x244C70
    void  CharaSwords_AddKills(void* swords, int amount);                       // 0x244CB0
    int   CharaSwords_LearnKind(void* swords, int kind, CharaParams* owner);    // 0x244F90
    int   CharaSwords_LearnNext(void* swords, int kind, u32 node, CharaParams* owner);  // 0x244CF0
    void* PadMgr_Get();                                                         // 0x13A920
    int   Pad_SetVibration(void* mgr, int port, const void* pattern);           // 0x139FF0
}
void* Objects_DropFromTable(void* objects, int table, int tier, const float* matrix);       // 0x21BFF0 (world_objects.cpp)

// 0x1904E0: shakes the pad (if the Vibrate setting is on).  Four patterns of
// one step: the big motor at 0x80 (a blow taken), 0xC0, 0x20, 0x60 (a blow
// guarded), five frames each.
static void Rumble(int port, int kind) {
    static const GuestPtrVar<u8> gConfigData{0x2EE68Cu};
    static const u32 kPatterns[4] = {0x2D8230u, 0x2D8260u, 0x2D8290u, 0x2D82C0u};
    s32 on;
    memcpy(&on, *gConfigData + 0x1C, 4);
    if (on == 1)
        Pad_SetVibration(PadMgr_Get(), port, P(kPatterns[kind]));
}

// ── The player (0x269370–0x2698B0) ────────────────────────────────────────
static int CharaPlayer_StepFlags(Chara* self) {                 // 0x2698B0
    if (Chara_StepFlags(self) != 0)
        Rumble(self->id, 0);
    return 0;
}
static int CharaPlayer_OnDamage(Chara* self) {                  // 0x269870
    if (Chara_OnDamage(self) != 0)
        Rumble(self->id, 0);
    return 0;
}
static int CharaPlayer_OnGuard(Chara* self) {                   // 0x269830
    if (Chara_OnGuard(self) != 0)
        Rumble(self->id, 3);
    return 0;
}
// 0x2695E0: what its blows earn — the sword's tallies, the techniques they
// buy (by the deed: 3 a blow landed, 7 / 8 from the combo's next steps; 2 a
// kill, 5, 6 a parried blow, 1 a point scored), and the game's statistics.
static int CharaPlayer_OnHit(Chara* self) {
    if (Chara_OnHit(self) == 0)
        return 0;
    static const GuestPtrVar<u8> gPlayer{0x2EE688u};
    void* swords = P(self->swords);
    CharaBody* body = Body(self);
    CharaParams* p = Params(self);
    const auto add = [](int at, s32 amount) {
        u16 v;
        memcpy(&v, *gPlayer + at, 2);
        const s32 sum = (s32)v + amount;
        v = (u16)(sum < 1 ? 0 : sum);
        memcpy(*gPlayer + at, &v, 2);
    };
    for (int i = 0; i < 2; i++) {
        const SwordVolume* blade = Blade(self, i);
        const u32 found = blade->base.found;
        if (found & kSwordLanded) {
            CharaSwords_LearnNext(swords, 3, body->comboNode, p);
            if (found & 0x100) CharaSwords_LearnKind(swords, 2, p);
            if (found & 0x200) CharaSwords_LearnKind(swords, 5, p);
            if (found & 0x400) CharaSwords_LearnNext(swords, 7, body->comboNode, p);
            if (found & 0x800) CharaSwords_LearnNext(swords, 8, body->comboNode, p);
            if (found & 0x20) body->targetFlags |= 0x1000;
            if (found & 2) CharaSwords_LearnKind(swords, 6, p);
            CharaSwords_AddKills(swords, blade->kills);
            add(0x840, blade->kills);
            add(0x842, blade->finishes);
            if (blade->score != 0) {
                CharaSwords_AddTally(swords, blade->score);
                CharaSwords_LearnKind(swords, 1, p);
            }
        }
        static const bool trace = getenv("WOTS2_TRACE_HITS") != nullptr;     // (native debug aid)
        if (trace && (found & kSwordLanded)) {
            const u32 sword = P<CharaSwords>(self->swords)->sword;
            printf("[hit] found %08x kills %d finishes %d score %d; sword tally %d kills %d\n", found, blade->kills,
                   blade->finishes, blade->score, sword ? *P<s32>(sword + 0x84) : -1, sword ? *P<s32>(sword + 0x88) : -1);
        }
        if ((found & kSwordHitCorpse) && (found & 0xC) && p->link[9] != 0) {
            const CharaParams* victim = P<CharaParams>(p->link[9]);
            if (victim->link[5] != 0 && (*P<u32>(victim->link[5]) & 0x10))
                add(0x85A, 1);
        }
    }
    return 0;
}
static int CharaPlayer_TryParry(Chara* self) {                  // 0x269580
    if (Chara_TryParry(self) != 0) {
        void* swords = P(self->swords);
        CharaSwords_AddTally(swords, 3);
        CharaSwords_LearnKind(swords, 4, Params(self));
        CharaSwords_LearnKind(swords, 1, Params(self));
    }
    return 0;
}
// 0x269370: a sword with one of the titles 0x13–0x17 takes its wielder's
// health with every swing of the blade: 2 … 10 a blow, never the last point.
static int CharaPlayer_ArmAttack(Chara* self) {
    if (Chara_ArmAttack(self) != 1)
        return 0;
    const u32 sword = P<CharaSwords>(self->swords)->sword;
    if (sword == 0)
        return 0;
    const s32 title = *P<s32>(sword + 0x8C);
    if (title < 0x13 || title > 0x17 || CharaBody_AttackStrength(Body(self)) != 0x10)
        return 0;
    CharaParams* p = Params(self);
    s32 hp = (s32)p->hp + (title - 0x12) * -2;
    if (hp < 1)
        hp = 1;
    p->hp = (s16)hp < p->maxHp ? (s16)hp : p->maxHp;
    return 0;
}

// 0x269B30 / 0x269940: the player alone casts a shadow of its own shape (the
// others have a dark blot, render effect 0xF): its body is drawn once more as
// a silhouette seen from the light — hardware, hle_chara_shadow — and laid on
// the ground, fainter the higher it is off it: 0.8 / (height + 1).
extern "C" void hle_chara_shadow(Chara* c, float strength);
static int CharaPlayer_Draw(Chara* self, int shown, const float* wind) {
    if (Chara_Draw(self, shown, wind) != 0)
        hle_chara_shadow(self, 0.8f / (self->aboveGround + 1.0f));
    return 0;
}

// ── Everyone else (0x2690F0) ──────────────────────────────────────────────
// Dying, it leaves something behind: an object out of the drop table its
// parameters name (CharaParams+0x28; −1 none), tossed from its chest.
static int CharaOther_OnDamage(Chara* self) {
    const int result = Chara_OnDamage(self);
    if ((Body(self)->flags & kBodyDead) != 0 && self->deathReported == 0) {
        self->deathReported = 1;
        alignas(16) float matrix[16];
        Matrix_Identity(matrix);
        memcpy(&matrix[12], P<float>(Bones(self)->bones + 0x490), 16);
        const CharaParams* p = Params(self);
        const u32 objects = *P<u32>(0x2EE738u);
        if (objects != 0 && p->_28 >= 0)
            Objects_DropFromTable(P(objects), p->_28, p->_20, matrix);
    }
    return result;
}

static void Chara_Nop(Chara*) {}
static int  Chara_Zero(Chara*) { return 0; }

void Chara_BindMethods() {
    using guest::bind;
    struct Method { u32 address; guest::Fn fn; const char* name; };
    const Method methods[] = {
        // the player's class                                    everyone else's
        {0x269BA0, CharaPlayer_Dtor, "CharaPlayer_Dtor"}, {0x2692D0, CharaOther_Dtor, "CharaOther_Dtor"},
        {0x269B90, Chara_BeginFrame, "Chara_BeginFrame"}, {0x2692C0, Chara_BeginFrame, "Chara_BeginFrame"},
        {0x269B80, Chara_Update, "Chara_Update"},         {0x2692B0, Chara_Update, "Chara_Update"},
        {0x269B70, Chara_StepMap, "Chara_StepMap"},       {0x2692A0, Chara_StepMap, "Chara_StepMap"},
        {0x269B30, CharaPlayer_Draw, "CharaPlayer_Draw"}, {0x269290, Chara_Draw, "Chara_Draw"},
        {0x269920, CharaPlayer_Cull, "CharaPlayer_Cull"}, {0x269240, CharaOther_Cull, "CharaOther_Cull"},
        {0x2698F0, Chara_Nop, "Chara_Nop"},               {0x2691B0, Chara_Nop, "Chara_Nop"},
        {0x269910, Chara_Zero, "Chara_Zero"},
        {0x269900, Chara_Nop, "Chara_Nop"},
        {0x2698B0, CharaPlayer_StepFlags, "CharaPlayer_StepFlags"}, {0x2691A0, Chara_StepFlags, "Chara_StepFlags"},
        {0x269460, Chara_UnloadSword, "Chara_UnloadSword"}, {0x269030, Chara_UnloadSword, "Chara_UnloadSword"},
        {0x269870, CharaPlayer_OnDamage, "CharaPlayer_OnDamage"}, {0x2690F0, CharaOther_OnDamage, "CharaOther_OnDamage"},
        {0x269830, CharaPlayer_OnGuard, "CharaPlayer_OnGuard"}, {0x2690E0, Chara_OnGuard, "Chara_OnGuard"},
        {0x2695E0, CharaPlayer_OnHit, "CharaPlayer_OnHit"},   {0x2690C0, Chara_OnHit, "Chara_OnHit"},
        {0x269580, CharaPlayer_TryParry, "CharaPlayer_TryParry"}, {0x2690A0, Chara_TryParry, "Chara_TryParry"},
        {0x269370, CharaPlayer_ArmAttack, "CharaPlayer_ArmAttack"}, {0x1CD970, Chara_ArmAttack, "Chara_ArmAttack"},
        {0x2694C0, CharaPlayer_WearSword, "CharaPlayer_WearSword"}, {0x269080, Chara_WearSword, "Chara_WearSword"},
    };
    for (const Method& m : methods)
        bind(m.address, m.fn, m.name);
}
