// The character manager — decompiled from 0x143030–0x1471C0.
// See include/game/chara_mgr.h.  Nearly every function finds a character by
// its id and does one small thing to it; the `Chara` class underneath is not
// decompiled (include/game/chara.h lists what is called on it).

#include "game/chara_mgr.h"
#include "engine/gs_device.h"
#include "game/player_data.h"
#include "game/sword.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>
#include <initializer_list>

extern "C" {
    void* MemAlloc(u32 size);                               // 0x100820
    void  MemFree(void* p);                                 // 0x100650
    void  Matrix_Copy(void* dst, const void* src);          // 0x1073C0
    void  Matrix_Multiply(void* dst, const void* a, const void* b);     // 0x107130
}

static_assert(offsetof(Chara, id) == 0x84 && offsetof(Chara, kind) == 0xAC && offsetof(Chara, visible) == 0xB4, "Chara");
static_assert(offsetof(Chara, tint) == 0x140 && offsetof(Chara, cellFlags) == 0x154, "Chara");
static_assert(offsetof(Chara, voice) == 0x190 && offsetof(Chara, swords) == 0x1D4, "Chara");
static_assert(offsetof(Chara, loaded) == 0x1E4 && offsetof(Chara, vtable) == 0x1F4 && sizeof(Chara) == 0x210, "Chara");
static_assert(offsetof(CharaBody, position) == 0xD0 && offsetof(CharaBody, player) == 0x1B4, "CharaBody");
static_assert(offsetof(CharaMgr, charas) == 0x30 && offsetof(CharaMgr, projection) == 0x60, "CharaMgr");
static_assert(sizeof(CharaMgr) == 0x120, "CharaMgr");

static const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
static const GuestPtrVar<void>       gObj2EE698{0x2EE698u};
static s32& sScreenW = *guest::ptr<s32>(0x2EE4C0u);
static s32& sScreenH = *guest::ptr<s32>(0x2EE4C4u);
static float* const kZeroVector = guest::ptr<float>(0x306900u);
static void* const  kIdentity   = guest::ptr<void>(0x2D1A80u);
static const u32* const kTargetBits = guest::ptr<const u32>(0x2EDA28u);     // two flag words, by kind of target

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline Chara*       CharaOf(GuestListNode* n) { return P<Chara>(n->value); }
static inline CharaParams* Params(Chara* c) { return P<CharaParams>(c->params); }
static inline CharaBody*   Body(Chara* c)   { return P<CharaBody>(c->body); }
static inline CharaAction* Action(Chara* c) { return P<CharaAction>(c->action); }
static inline CharaSwords* Swords(Chara* c) { return P<CharaSwords>(c->swords); }

static Chara* Find(CharaMgr* mgr, int id) {
    for (GuestListNode* n = GuestList_Begin(&mgr->charas); n != GuestList_End(&mgr->charas); n = GuestList_Next(n))
        if (CharaOf(n)->id == id)
            return CharaOf(n);
    return nullptr;
}
#define EACH_CHARA(mgr, c) \
    for (GuestListNode* n_ = GuestList_Begin(&(mgr)->charas); n_ != GuestList_End(&(mgr)->charas); \
         n_ = GuestList_Next(n_)) \
        if (Chara* c = CharaOf(n_))

extern "C" {

// ══ Creation and loading ══════════════════════════════════════════════════

// ── 0x1471C0 ──────────────────────────────────────────────────────────────
CharaMgr* CharaMgr_Ctor(void* mem) {
    CharaMgr* mgr = static_cast<CharaMgr*>(mem);
    GuestList_Init(&mgr->charas);
    GuestList_Init(&mgr->loadQueue);
    GuestList_Init(&mgr->reloadQueue);
    GuestList_Init(&mgr->drawn);
    mgr->showNear = 0;
    mgr->loadStep = 0;
    mgr->pendingStep = -1;
    mgr->savedStep = -1;
    memcpy(mgr->light, kZeroVector, sizeof mgr->light);
    mgr->reloading = 0;
    return mgr;
}

// ── 0x146F50 ──────────────────────────────────────────────────────────────
CharaMgr* CharaMgr_Dtor(CharaMgr* mgr, s16 flag) {
    if (mgr == nullptr)
        return mgr;
    GuestListNode* n = GuestList_Begin(&mgr->charas);
    while (n != GuestList_End(&mgr->charas)) {
        if (Chara* c = CharaOf(n))
            VirtualCall<void*, Chara*, int>(c->vtable, kCharaDestroy, c, 1);
        n = GuestList_Erase(&mgr->charas, n);
    }
    mgr->loadStep = 0;
    GuestList_Clear(&mgr->loadQueue);
    GuestList_Clear(&mgr->reloadQueue);
    GuestList_Clear(&mgr->drawn);
    if (flag > 0)
        MemFree(mgr);
    return mgr;
}

// ── 0x146EC0 ──────────────────────────────────────────────────────────────
void CharaMgr_Init(CharaMgr* mgr, int forceLoad) {
    CharaResources_Load(CharaResources_Get(), forceLoad);
    Frustum_LoadPlanes(P(0x2DFC00u), P(0x2DFBF0u), mgr->planes, 8);
    Frustum_MakeMatrix((float)sScreenW, (float)sScreenH, 512.0f, 0.2f, 40.0f, 0, mgr->projection);
}

// ── 0x146E90 ──────────────────────────────────────────────────────────────
void CharaMgr_fn146e90() { CharaResources_fn1d6300(CharaResources_Get()); }

// ── 0x146DA0 ──────────────────────────────────────────────────────────────
// Ids 0 and 1 are of the player's class.
void CharaMgr_Add(CharaMgr* mgr, int id, int kind, int model, int arg) {
    Chara* c = nullptr;
    if (id == 0 || id == 1) {
        void* mem = MemAlloc(0x200);
        if (mem != nullptr)
            c = Chara_CtorA(mem, id, kind, model, arg);
    } else {
        void* mem = MemAlloc(0x210);
        if (mem != nullptr)
            c = Chara_CtorB(mem, id, kind, model, arg);
    }
    GuestList_Insert(&mgr->charas, GuestList_End(&mgr->charas), guest::addr(c));
}

// ── 0x145790 ──────────────────────────────────────────────────────────────
void CharaMgr_QueueLoad(CharaMgr* mgr, int id) {
    if (Chara* c = Find(mgr, id)) {
        GuestList_Insert(&mgr->loadQueue, GuestList_End(&mgr->loadQueue), guest::addr(c));
        GuestList_Insert(&mgr->reloadQueue, GuestList_End(&mgr->reloadQueue), guest::addr(c));
    }
}

// ── 0x145DF0 ──────────────────────────────────────────────────────────────
void CharaMgr_Remove(CharaMgr* mgr, int id) {
    for (GuestList* queue : {&mgr->loadQueue, &mgr->reloadQueue})
        for (GuestListNode* n = GuestList_Begin(queue); n != GuestList_End(queue); n = GuestList_Next(n))
            if (CharaOf(n)->id == id) {
                GuestList_Erase(queue, n);
                break;
            }
    for (GuestListNode* n = GuestList_Begin(&mgr->charas); n != GuestList_End(&mgr->charas); n = GuestList_Next(n)) {
        Chara* c = CharaOf(n);
        if (c->id != id)
            continue;
        if (c != nullptr)
            VirtualCall<void*, Chara*, int>(c->vtable, kCharaDestroy, c, 1);
        GuestList_Erase(&mgr->charas, n);
        return;
    }
}

// Steps 0–7 of loading the character at the head of the queue: phases 0 to
// 4 of its files, each started when the one before is done.  (0x1450D0 and
// the tail of 0x145400 are the same code.)
static bool AdvanceLoad(CharaMgr* mgr, Chara* c) {
    if (mgr->loadStep == 0)
        mgr->loadStep = Chara_StartLoadPhase(c, 0) == 0 ? 7 : mgr->loadStep + 1;
    static const u32 kFailText[4] = {0x2DFC50u, 0x2DFC90u, 0x2DFCF0u, 0x2DFCC0u};
    for (int phase = 0; phase < 3; phase++) {
        if (mgr->loadStep != phase + 1)
            continue;
        const int done = Chara_PollLoadPhase(c, phase);
        if (done == -1) {
            mgr->loadStep = 7;
            Debug_Print(P<char>(kFailText[phase]));
        } else if (done == 1) {
            if (Chara_StartLoadPhase(c, phase + 1) == 0) {
                mgr->loadStep = 7;
                Debug_Print(P<char>(phase == 2 ? kFailText[3] : kFailText[phase]));
            } else {
                mgr->loadStep++;
            }
        }
    }
    if (mgr->loadStep == 4)
        mgr->loadStep = 5;
    if (mgr->loadStep == 5 && Chara_PollLoadPhase(c, 3) != 0) {
        if (Chara_StartLoadPhase(c, 4) == 0) {
            mgr->loadStep = 7;
            Debug_Print(P<char>(0x2DFD20u));
        } else {
            mgr->loadStep++;
        }
    }
    if (mgr->loadStep == 6)
        mgr->loadStep = 7;
    return mgr->loadStep == 7 && Chara_PollLoadPhase(c, 4) != 0;
}

// ── 0x1450D0 ──────────────────────────────────────────────────────────────
int CharaMgr_LoadStep(CharaMgr* mgr) {
    GuestListNode* head = GuestList_Begin(&mgr->loadQueue);
    Chara* c = CharaOf(head);
    if (c->id == 0 && mgr->pendingStep != -1) {
        mgr->loadStep = mgr->pendingStep;
        mgr->pendingStep = -1;
    }
    if (!AdvanceLoad(mgr, c))
        return 0;
    GuestList_Erase(&mgr->loadQueue, head);
    mgr->loadStep = 0;
    return 1;
}

// ── 0x144F50 ──────────────────────────────────────────────────────────────
// Phases 5 and 6, for the head of the second queue.
int CharaMgr_ReloadStep(CharaMgr* mgr) {
    GuestListNode* head = GuestList_Begin(&mgr->reloadQueue);
    Chara* c = CharaOf(head);
    if (mgr->loadStep == 0) {
        mgr->reloading = 1;
        mgr->loadStep = Chara_StartLoadPhase(c, 5) == 0 ? 3 : mgr->loadStep + 1;
    }
    if (mgr->loadStep == 1) {
        const int done = Chara_PollLoadPhase(c, 5);
        if (done == -1) {
            mgr->loadStep = 3;
            Debug_Print(P<char>(0x2DFC10u));
        } else if (done == 1) {
            Chara_StartLoadPhase(c, 6);
            mgr->loadStep++;
        }
    }
    if (mgr->loadStep == 2) {
        const int done = Chara_PollLoadPhase(c, 6);
        if (done == -1) {
            mgr->loadStep = 3;
            Debug_Print(P<char>(0x2DFC10u));
        } else if (done == 1) {
            mgr->loadStep++;
        }
    }
    if (mgr->loadStep != 3)
        return 0;
    GuestList_Erase(&mgr->reloadQueue, head);
    mgr->loadStep = 0;
    mgr->reloading = 0;
    return 1;
}

// ── 0x145400 ──────────────────────────────────────────────────────────────
// While the player is waiting in the load queue its files are loaded here,
// ahead of everyone else's, and this returns 0 until they are in.
int CharaMgr_IsBusy(CharaMgr* mgr) {
    GuestListNode* n = GuestList_Begin(&mgr->loadQueue);
    for (; n != GuestList_End(&mgr->loadQueue); n = GuestList_Next(n))
        if (CharaOf(n)->id == 0)
            break;
    if (n == GuestList_End(&mgr->loadQueue))
        return 1;
    if (mgr->pendingStep != -1) {
        mgr->savedStep = mgr->loadStep;
        mgr->loadStep = mgr->pendingStep;
        mgr->pendingStep = -1;
    }
    if (!AdvanceLoad(mgr, CharaOf(n)))
        return 0;
    GuestList_Erase(&mgr->loadQueue, n);
    if (mgr->savedStep == -1) {
        mgr->loadStep = 0;
    } else {
        mgr->loadStep = mgr->savedStep;
        mgr->savedStep = -1;
    }
    return 1;
}

// ── 0x1441F0 ──────────────────────────────────────────────────────────────
int CharaMgr_IsLoaded(CharaMgr* mgr, int id) {
    Chara* c = Find(mgr, id);
    return c != nullptr ? c->loaded : 0;
}

// ── 0x1441B0 ──────────────────────────────────────────────────────────────
char CharaMgr_IsReady(CharaMgr* mgr) {
    char ready = 1;
    EACH_CHARA(mgr, c)
        if (c->loaded == 0)
            ready = 0;
    return ready;
}

// ── 0x143E50 ──────────────────────────────────────────────────────────────
int CharaMgr_IsPlayerReady(CharaMgr* mgr) {
    Chara* c = Find(mgr, 0);
    return c != nullptr && CharaSwords_IsReady(Swords(c)) != 0 && c->loaded != 0;
}

// ── 0x144250 ──────────────────────────────────────────────────────────────
// Was it drawn this frame?
int CharaMgr_Exists(CharaMgr* mgr, int id) {
    for (GuestListNode* n = GuestList_Begin(&mgr->drawn); n != GuestList_End(&mgr->drawn); n = GuestList_Next(n))
        if (CharaOf(n)->id == id)
            return 1;
    return 0;
}

// ══ The frame ═════════════════════════════════════════════════════════════

// ── 0x145BE0 ──────────────────────────────────────────────────────────────
void CharaMgr_Update(CharaMgr* mgr, int load) {
    if (load != 0) {
        if (mgr->reloading == 0 && mgr->loadQueue.size != 0)
            CharaMgr_LoadStep(mgr);
        else if (mgr->reloadQueue.size != 0)
            CharaMgr_ReloadStep(mgr);
    }
    EACH_CHARA(mgr, c)
        if (c->active != 0)
            VirtualCall<void, Chara*>(c->vtable, kCharaUpdate, c);
}

// ── 0x145C90 ──────────────────────────────────────────────────────────────
void CharaMgr_StepA(CharaMgr* mgr) {
    EACH_CHARA(mgr, c)
        if (c->active != 0)
            VirtualCall<void, Chara*>(c->vtable, kCharaStepA, c);
}

// ── 0x1435D0 ──────────────────────────────────────────────────────────────
void CharaMgr_StepB(CharaMgr* mgr) {
    EACH_CHARA(mgr, c)
        if (c->active != 0)
            VirtualCall<void, Chara*>(c->vtable, kCharaStepB, c);
    EACH_CHARA(mgr, c)
        if (c->active != 0)
            Chara_fn1cccf0(c);
}

// ── 0x145B80 ──────────────────────────────────────────────────────────────
void CharaMgr_StepC(CharaMgr* mgr) {
    EACH_CHARA(mgr, c)
        if (c->active != 0)
            Chara_fn1cec20(c);
}

// ── 0x144AB0 ──────────────────────────────────────────────────────────────
void CharaMgr_StepD(CharaMgr* mgr) {
    EACH_CHARA(mgr, c)
        if (c->active != 0)
            VirtualCall<void, Chara*>(c->vtable, kCharaStepD, c);
}

// ── 0x144300 ──────────────────────────────────────────────────────────────
void CharaMgr_StepFlags(CharaMgr* mgr, void*) {
    EACH_CHARA(mgr, c)
        if (c->active != 0)
            VirtualCall<void, Chara*>(c->vtable, kCharaStepFlags, c);
}

// ── 0x1442A0 ──────────────────────────────────────────────────────────────
void CharaMgr_StepFlags2(CharaMgr* mgr, void*) {
    EACH_CHARA(mgr, c)
        if (c->active != 0)
            Chara_fn1ccfa0(c);
}

// ── 0x145A60 ──────────────────────────────────────────────────────────────
// Every character against the map; `viewer` is where the camera is.
void CharaMgr_StepMap(CharaMgr* mgr, u32 map, const float* viewer) {
    EACH_CHARA(mgr, c) {
        if (c->active == 0)
            continue;
        c->visible = 1;
        if (map == 0 || viewer == nullptr)
            continue;
        VirtualCall<void, Chara*, u32>(c->vtable, kCharaStepMap, c, map);
        alignas(16) float a[4], b[4];
        memcpy(a, &Params(c)->matrix[12], sizeof a);
        memcpy(b, viewer, sizeof b);
        b[1] = 0.0f;
        a[1] = 0.0f;
        const float distance = Vec_Distance(a, b);
        c->cameraDistance = distance;
        if (mgr->showNear == 0 && distance < 0.5f)
            c->visible = 0;
    }
}

// ── 0x1436C0 ──────────────────────────────────────────────────────────────
// The player picking things up.
void CharaMgr_StepDrops(CharaMgr* mgr, void* itemDrops, void* swordDb) {
    Chara* c = Find(mgr, 0);
    if (c == nullptr || c->active == 0)
        return;
    CharaBody_fn1cb600(Body(c));
    if (Body(c)->state & 0x2F00000u) {
        Chara_fn1cccb0(c);
        if (Body(c)->state & 0x1000000u) {
            alignas(16) float matrix[16];
            Matrix_Copy(matrix, Params(c)->matrix);
            Chara_fn1ccc30(c, ItemDrops_fn242550(itemDrops, matrix, swordDb));
        }
        Chara_fn1ccb90(c);
    }
    Chara_fn1ccb30(c);
}

// ── 0x144540 ──────────────────────────────────────────────────────────────
// Characters whose sword changed get its model from the sword database.
void CharaMgr_StepSwords(CharaMgr* mgr, void* swordDb) {
    EACH_CHARA(mgr, c) {
        if (VirtualCall<int, Chara*>(c->vtable, kCharaSwordChanged, c) == 0)
            continue;
        alignas(16) float matrix[16];
        Matrix_Copy(matrix, Params(c)->matrix);
        if (c->swords != 0)
            CharaSwords_fn245150(Swords(c));
        if (c->params != 0 && Params(c)->unique != 0)
            SwordDb_fn15ccd0(swordDb, P(Swords(c)->sword));
        SwordDb_fn15c330(swordDb, Swords(c)->_0, Swords(c)->sword, matrix);
        VirtualCall<void, Chara*>(c->vtable, kCharaSwordDone, c);
    }
}

// ── 0x145830 ──────────────────────────────────────────────────────────────
void CharaMgr_Draw(CharaMgr* mgr, int pass) {
    GuestList_Clear(&mgr->drawn);
    float nearest = 0.0f;
    int opponent = -1;
    if (Chara* player = Find(mgr, 0)) {
        nearest = player->cameraDistance / 1.5f;
        if (const s32* other = P<s32>(Params(player)->link[3]))
            opponent = *other;      // (a CharaParams: its first word is the id)
    }
    alignas(16) float clip[16];
    Matrix_Multiply(clip, mgr->projection, reinterpret_cast<u8*>(*gGsDevice) + 0x360);
    const s32 showNear = mgr->showNear;
    EACH_CHARA(mgr, c) {
        u32 shown = 1;
        if (c->active != 0) {
            if (c->visible != 0 && Frustum_Outside(clip, &Params(c)->matrix[12], mgr->planes, 8) != 0)
                c->visible = 0;
            const float limit = opponent == c->id ? 0.0f : nearest;
            if (VirtualCall<int, Chara*, int, float>(c->vtable, kCharaCull, c, pass, limit) == 0) {
                VirtualCall<void, Chara*, int, float*>(c->vtable, kCharaDraw, c, 0, mgr->light);
            } else {
                GuestList_Insert(&mgr->drawn, GuestList_End(&mgr->drawn), guest::addr(c));
                VirtualCall<void, Chara*, int, float*>(c->vtable, kCharaDraw, c, 1, mgr->light);
                if (showNear == 0)
                    shown = 0;
            }
        }
        if (c->flagHandle != 0)
            P<u32>(c->flagHandle)[0x194 / 4] = shown;
    }
}

// ══ By id: the plain getters and setters ══════════════════════════════════

Chara* CharaMgr_Find(CharaMgr* mgr, int id) { return Find(mgr, id); }                          // 0x145F90

// ── 0x145FE0 ──────────────────────────────────────────────────────────────
int CharaMgr_IdAt(CharaMgr* mgr, int index) {
    int i = 0;
    EACH_CHARA(mgr, c)
        if (i++ == index)
            return c->id;
    return -1;
}

// ── 0x146030 ──────────────────────────────────────────────────────────────
int CharaMgr_Renumber(CharaMgr* mgr, int id, int newId) {
    Chara* c = Find(mgr, id);
    if (c != nullptr) {
        c->id = newId;
        Params(c)->id = newId;
    }
    return c != nullptr;
}

CharaParams* CharaMgr_GetParams(CharaMgr* mgr, int id) {                                       // 0x145F30
    Chara* c = Find(mgr, id);
    return c != nullptr ? Params(c) : nullptr;
}
void* CharaMgr_GetTransform(CharaMgr* mgr, int id) {                                           // 0x146D10
    Chara* c = Find(mgr, id);
    if (c == nullptr) {
        Debug_Print(P<char>(0x2DFD70u));
        return kIdentity;
    }
    return Params(c)->matrix;
}
void CharaMgr_SetTransform(CharaMgr* mgr, int id, const void* matrix) {                        // 0x146A20
    if (Chara* c = Find(mgr, id))
        Matrix_Copy(Params(c)->matrix, matrix);
}
float* CharaMgr_GetPosition(CharaMgr* mgr, int id) {                                           // 0x146990
    Chara* c = Find(mgr, id);
    if (c == nullptr) {
        Debug_Print(P<char>(0x2DFD50u));
        return kZeroVector;
    }
    return &Params(c)->matrix[12];
}
void CharaMgr_SetPosition(CharaMgr* mgr, int id, const void* position) {                       // 0x1468F0
    if (Chara* c = Find(mgr, id)) {
        memcpy(&Params(c)->matrix[12], position, 16);
        memcpy(Body(c)->position, position, 16);
    }
}

#define CHARA_SETTER(name, field) \
    void name(CharaMgr* mgr, int id, int value) { if (Chara* c = Find(mgr, id)) c->field = value; }
#define CHARA_GETTER(name, field, missing) \
    int name(CharaMgr* mgr, int id) { Chara* c = Find(mgr, id); return c != nullptr ? (int)c->field : (missing); }

CHARA_SETTER(CharaMgr_SetActive, active)        // 0x1465E0
CHARA_GETTER(CharaMgr_IsActive, active, 0)      // 0x146CB0
CHARA_SETTER(CharaMgr_SetGrouped, indoors)      // 0x146580  (really: belongs to an interior)
CHARA_SETTER(CharaMgr_SetGroup, interior)       // 0x146520
CHARA_SETTER(CharaMgr_SetPartner, partner)      // 0x146400
CHARA_GETTER(CharaMgr_GetPartner, partner, -1)  // 0x146B00
CHARA_SETTER(CharaMgr_Set70, _70)               // 0x146460
CHARA_GETTER(CharaMgr_Get70, _70, 0)            // 0x146B60
CHARA_SETTER(CharaMgr_Set6C, _6c)               // 0x1460A0
CHARA_GETTER(CharaMgr_Get6C, _6c, 0)            // 0x146BC0
CHARA_SETTER(CharaMgr_Set68, _68)               // 0x1464C0
CHARA_SETTER(CharaMgr_SetB0, _b0)               // 0x146890
CHARA_GETTER(CharaMgr_Get1DC, _1dc, 0)          // 0x146AA0
CHARA_GETTER(CharaMgr_GetB5, _b5, 0)            // 0x146680

void CharaMgr_fn1466e0(CharaMgr* mgr, int id, int on) {                                        // 0x1466E0
    if (Chara* c = Find(mgr, id))
        c->_b5 = on != 0;
}
void CharaMgr_ClearB5(CharaMgr* mgr) {                                                         // 0x146640
    EACH_CHARA(mgr, c)
        c->_b5 = 0;
}

// ── 0x143030 ──────────────────────────────────────────────────────────────
void CharaMgr_ResetTint(CharaMgr* mgr, int id) {
    if (Chara* c = Find(mgr, id))
        c->tint[0] = c->tint[1] = c->tint[2] = 128.0f;
}

// ── 0x145D00 ──────────────────────────────────────────────────────────────
// A character other than the player takes part only in the place it belongs to.
void CharaMgr_Spawn(CharaMgr* mgr, int id, int indoors, int interior) {
    Chara* c = Find(mgr, id);
    if (c != nullptr && c->id != 0)
        c->active = indoors == c->indoors && interior == c->interior;
}

// ── 0x145D90 ──────────────────────────────────────────────────────────────
void CharaMgr_Respawn(CharaMgr* mgr, int indoors, int interior) {
    EACH_CHARA(mgr, c)
        if (c->id != 0)
            c->active = indoors == c->indoors && interior == c->interior;
}

// ── 0x146C20 ──────────────────────────────────────────────────────────────
// (Despite the name callers gave it: 1 when the character can be seen.)
int CharaMgr_IsHidden(CharaMgr* mgr, int id) {
    Chara* c = Find(mgr, id);
    return c != nullptr && c->loaded != 0 && c->active != 0 && (Body(c)->flags & 0x10) == 0;
}

// ══ By id: state ══════════════════════════════════════════════════════════

u32 CharaMgr_GetFlags(CharaMgr* mgr, int id) {                                                 // 0x1467B0
    Chara* c = Find(mgr, id);
    return c != nullptr ? Body(c)->flags : 0;
}
void CharaMgr_SetFlags(CharaMgr* mgr, int id, u32 bits) {                                      // 0x1461D0
    if (Chara* c = Find(mgr, id))
        Body(c)->flags |= bits;
}
void CharaMgr_ClearFlags(CharaMgr* mgr, int id, u32 bits) {                                    // 0x146160
    if (Chara* c = Find(mgr, id))
        Body(c)->flags &= ~bits;
}
void CharaMgr_Kill(CharaMgr* mgr, int id) {                                                    // 0x146100
    if (Chara* c = Find(mgr, id))
        Body(c)->flags |= 0x210;
}

// ── 0x146300 / 0x146380 ───────────────────────────────────────────────────
void CharaMgr_Sheathe(CharaMgr* mgr, int id, int quiet) {
    if (Chara* c = Find(mgr, id)) {
        Body(c)->flags &= ~0x100u;
        if (quiet == 0)
            CharaBody_Refresh(Body(c));
    }
}
void CharaMgr_Draw_Sword(CharaMgr* mgr, int id, int quiet) {
    if (Chara* c = Find(mgr, id)) {
        Body(c)->flags |= 0x100;
        if (quiet == 0)
            CharaBody_Refresh(Body(c));
    }
}

// ── 0x146230 ──────────────────────────────────────────────────────────────
// Back to the standing stance, unless dead.
void CharaMgr_fn146230(CharaMgr* mgr, int id) {
    Chara* c = Find(mgr, id);
    if (c == nullptr)
        return;
    const u32* flags = P<u32>(Params(c)->link[5]);
    if (((flags != nullptr ? *flags : 0) & 0x200) != 0)
        return;
    Body(c)->flags &= ~0xFu;
    Body(c)->flags |= 1;
    Chara_fn1cdb00(c);
    CharaBody_Refresh(Body(c));
}

void CharaMgr_fn143c60(CharaMgr* mgr, int id) {                                                // 0x143C60
    if (Chara* c = Find(mgr, id))
        CharaBody_Refresh(Body(c));
}
void CharaMgr_fn143cd0(CharaMgr* mgr, int id) {                                                // 0x143CD0
    if (Chara* c = Find(mgr, id))
        CharaBody_fn1d1c90(Body(c));
}
void CharaMgr_fn143d40(CharaMgr* mgr, int id) {                                                // 0x143D40
    if (Chara* c = Find(mgr, id))
        CharaBody_fn1d1ce0(Body(c));
}
u8 CharaMgr_fn143a10(CharaMgr* mgr, int id) {                                                  // 0x143A10
    Chara* c = Find(mgr, id);
    return c != nullptr ? CharaBody_fn1cae80(Body(c)) : 0;
}
int CharaMgr_GetHealthShown(CharaMgr* mgr, int id) {                                           // 0x143A90
    Chara* c = Find(mgr, id);
    return c != nullptr ? (int)P<MotionPlayer>(Body(c)->player)->frame : 0;
}
void CharaMgr_fn143b10(CharaMgr* mgr, int id) {                                                // 0x143B10
    if (Chara* c = Find(mgr, id))
        MotionPlayer_SetFlags(P<MotionPlayer>(Body(c)->player), 5);
}
void CharaMgr_fn143b80(CharaMgr* mgr, int id) {                                                // 0x143B80
    if (Chara* c = Find(mgr, id))
        MotionPlayer_SetFlags(P<MotionPlayer>(Body(c)->player), 0);
}
void CharaMgr_fn143bf0(CharaMgr* mgr, int id, u32 arg) {                                       // 0x143BF0
    if (Chara* c = Find(mgr, id))
        Chara_fn1cccc0(c, arg);
}
void CharaMgr_Revive(CharaMgr* mgr, int id) {                                                  // 0x143090
    if (Chara* c = Find(mgr, id))
        Chara_Reset(c);
}
int CharaMgr_fn1437d0(CharaMgr* mgr, int id) {                                                 // 0x1437D0
    Chara* c = Find(mgr, id);
    if (c != nullptr)
        Chara_fn1cca90(c);
    return c != nullptr;
}
int CharaMgr_fn143850(CharaMgr* mgr, u32 arg, int id) {                                        // 0x143850
    Chara* c = Find(mgr, id);
    if (c != nullptr)
        Chara_fn1ccad0(c, arg);
    return c != nullptr;
}
int CharaMgr_fn1438d0(CharaMgr* mgr, u32 arg, int id) {                                        // 0x1438D0
    Chara* c = Find(mgr, id);
    if (c != nullptr)
        Chara_fn1ccb20(c, arg);
    return c != nullptr;
}
void CharaMgr_ResetAll(CharaMgr* mgr) {                                                        // 0x143950
    EACH_CHARA(mgr, c)
        Chara_fn1ccad0(c, 0);
}
void CharaMgr_ClearTransient(CharaMgr* mgr) {                                                  // 0x1440D0
    EACH_CHARA(mgr, c)
        Chara_ClearTransient(c);
}
int CharaMgr_fn144130(CharaMgr* mgr, int id, u32 a, u32 b) {                                   // 0x144130
    Chara* c = Find(mgr, id);
    return c != nullptr ? Chara_fn1cce20(c, a, b) : 0;
}
void CharaMgr_fn146740(CharaMgr* mgr, int id, const float* point) {                            // 0x146740  look at
    if (Chara* c = Find(mgr, id))
        Chara_LookAt(c, point);
}
void CharaMgr_fn1439b0(CharaMgr*, u32 arg) { CharaResources_fn1d2b20(CharaResources_Get(), arg); }   // 0x1439B0
void CharaMgr_fn1439e0() { CharaResources_fn1d2bb0(CharaResources_Get()); }                   // 0x1439E0

// ── 0x143DB0 ──────────────────────────────────────────────────────────────
// Can the player be interrupted (not busy, not standing in a doorway)?
int CharaMgr_PlayerIsFree(CharaMgr* mgr) {
    Chara* c = Find(mgr, 0);
    if (c == nullptr)
        return 0;
    const u32* state = P<u32>(Params(c)->link[6]);
    return ((state != nullptr ? *state : 0) & 0x7000) == 0 && (c->cellFlags & 8) == 0;
}

// ── Where the player stands (collision cell flags; see "Map collision files") ──
int CharaMgr_DoorAt(CharaMgr* mgr, int id) {                                                   // 0x143EF0
    Chara* c = Find(mgr, id);
    return c != nullptr && (c->cellFlags & 8) ? c->cellIndex : -1;
}
int CharaMgr_HintSpot(CharaMgr* mgr) {                                                         // 0x143F60
    Chara* c = Find(mgr, 0);
    return c != nullptr && (c->cellFlags & 0x80) ? c->cellIndex : -1;
}
int CharaMgr_TalkTarget(CharaMgr* mgr) {                                                       // 0x143FD0
    Chara* c = Find(mgr, 0);
    return c != nullptr && (c->cellFlags & 0x40) != 0;
}
int CharaMgr_ExitAt(CharaMgr* mgr) {                                                           // 0x144040
    Chara* c = Find(mgr, 0);
    if (c == nullptr || (c->cellFlags & 4) == 0)
        return -1;
    return (c->cellFlags & 2) && (c->cellFlags & 0x100) ? 0xFFFFFF : c->cellIndex;
}

// ══ Opponents ═════════════════════════════════════════════════════════════

int CharaMgr_HasOpponent(CharaMgr* mgr, int id) {                                              // 0x146820
    Chara* c = Find(mgr, id);
    return c != nullptr ? (int)Params(c)->link[3] : 0;
}
void CharaMgr_OnPlayerDeath(CharaMgr* mgr) {                                                   // 0x143100  nobody has an opponent
    EACH_CHARA(mgr, c) {
        Params(c)->link[3] = 0;
        Body(c)->targetFlags = 0;
    }
}
void CharaMgr_fn143140(CharaMgr* mgr, int id) {                                                // 0x143140
    if (Chara* c = Find(mgr, id)) {
        Params(c)->link[3] = 0;
        Body(c)->targetFlags = 0;
    }
}
// ── 0x1431A0 ──────────────────────────────────────────────────────────────
void CharaMgr_SetOpponent(CharaMgr* mgr, int id, int other, int kind) {
    Chara* c = Find(mgr, id);
    if (c == nullptr)
        return;
    // (The original does not look whether `other` is there: it reads the word
    // at 0x198.  It happens — the festival's C01B3x names as a fighter an id
    // that is still one of the extras the scene before left hidden, which
    // has no armed half.  Here that is "no opponent": the pairs give it one.)
    Chara* o = Find(mgr, other);
    Params(c)->link[3] = o != nullptr ? o->params : 0;
    Body(c)->targetFlags &= 0xCFFFFFFFu;
    Body(c)->targetFlags |= kTargetBits[kind];
}

// ══ Actions ═══════════════════════════════════════════════════════════════

// ── 0x144EC0 ──────────────────────────────────────────────────────────────
int CharaMgr_SetState(CharaMgr* mgr, int id, int a, u32 b, u32 c3) {
    Chara* c = Find(mgr, id);
    return c != nullptr ? Chara_SetAction(c, CharaAction_Make(a, b, P(c3))) : 0;
}
int CharaMgr_GetAction(CharaMgr* mgr, int id) {                                                // 0x144D10
    Chara* c = Find(mgr, id);
    return c != nullptr && Action(c)->current != 0 ? Action(c)->handle : 0;
}
void CharaMgr_Release(CharaMgr* mgr, int id) {                                                 // 0x144E00
    if (Chara* c = Find(mgr, id))
        Action(c)->released = 1;
}
void CharaMgr_SetIdle(CharaMgr* mgr, int id) {                                                 // 0x144E60
    if (Chara* c = Find(mgr, id))
        Action(c)->released = 0;
}
int CharaMgr_IsHeld(CharaMgr* mgr, int id) {                                                   // 0x144CA0
    Chara* c = Find(mgr, id);
    return c != nullptr && Action(c)->released == 0;
}
void CharaMgr_fn144d90(CharaMgr* mgr, int id) {                                                // 0x144D90
    if (Chara* c = Find(mgr, id))
        CharaAction_fn1c7d20(Action(c));
}

// ══ Voice (Chara+0x190: three players at +0x08, +0x24 and +0x40) ══════════

void CharaMgr_fn143280(CharaMgr* mgr, int id, u32 arg) {                                       // 0x143280
    Chara* c = Find(mgr, id);
    if (c != nullptr && c->voice != 0)
        VirtualCall<void, void*, u32, int, int>(P<u32>(c->voice)[0x50 / 4], 0x18, P<u8>(c->voice) + 0x40, arg, 0, 0);
}
void CharaMgr_fn143310(CharaMgr* mgr, int id) {                                                // 0x143310
    Chara* c = Find(mgr, id);
    if (c != nullptr && c->voice != 0) {
        P<u32>(c->voice)[0x40 / 4] = 2;
        P<u32>(c->voice)[0x48 / 4] = 3;
    }
}
void CharaMgr_fn143380(CharaMgr* mgr, int id, u32 value) {                                     // 0x143380
    Chara* c = Find(mgr, id);
    if (c != nullptr && c->voice != 0) {
        P<u32>(c->voice)[0x5C / 4] = value;
        P<u32>(c->voice)[0x40 / 4] = value;
    }
}
void CharaMgr_fn1433f0(CharaMgr* mgr, int id, u32 value) {                                     // 0x1433F0
    Chara* c = Find(mgr, id);
    if (c != nullptr && c->voice != 0) {
        VirtualCall<void, void*, u32, int, int>(P<u32>(c->voice)[0x34 / 4], 0x18, P<u8>(c->voice) + 0x24, 0, 0, 0);
        P<u32>(c->voice)[0x24 / 4] = value;
    }
}
void CharaMgr_fn1434a0(CharaMgr* mgr, int id, u32 arg) {                                       // 0x1434A0
    Chara* c = Find(mgr, id);
    if (c != nullptr && c->voice != 0) {
        VirtualCall<void, void*, u32, int, int>(P<u32>(c->voice)[0x18 / 4], 0x18, P<u8>(c->voice) + 8, arg, 0, 0);
        if (arg == 0)
            P<u32>(c->voice)[8 / 4] = 0;
    }
}
void CharaMgr_fn143550(CharaMgr* mgr, int id, u32 arg) {                                       // 0x143550
    Chara* c = Find(mgr, id);
    if (c != nullptr && c->voice != 0)
        CharaVoice_fn26a7c0(P(c->voice), arg, 0, 0);
}

// ══ Swords ════════════════════════════════════════════════════════════════

void CharaMgr_fn144370(CharaMgr* mgr, int id) {                                                // 0x144370
    Chara* c = Find(mgr, id);
    if (c != nullptr && c->swords != 0)
        CharaSwords_fn2454b0(Swords(c));
}
void CharaMgr_fn1443f0(CharaMgr* mgr, int id) {                                                // 0x1443F0
    Chara* c = Find(mgr, id);
    if (c != nullptr && Swords(c)->sword != 0)
        P<u32>(Swords(c)->sword)[0x60 / 4] = 0;
}
int CharaMgr_fn144460(CharaMgr* mgr, int id) {                                                 // 0x144460
    Chara* c = Find(mgr, id);
    if (c == nullptr || Swords(c)->sword == 0)
        return -1;
    return P<u32>(Swords(c)->sword)[0x60 / 4] == 0;
}
void CharaMgr_fn1444d0(CharaMgr* mgr, int id) {                                                // 0x1444D0
    if (Chara* c = Find(mgr, id))
        VirtualCall<void, Chara*>(c->vtable, kCharaSwordDone, c);
}

// ── 0x144640 ──────────────────────────────────────────────────────────────
// Gives the character's sword back its original values (they are kept in
// the database under the id at Sword+0x7C).
void CharaMgr_RestoreSword(CharaMgr* mgr, void* swordDb, int id) {
    Chara* c = Find(mgr, id);
    if (c == nullptr || Swords(c)->sword == 0)
        return;
    u8* sword = P<u8>(Swords(c)->sword);
    const void* original = SwordDb_fn15d0d0(swordDb, *reinterpret_cast<u32*>(sword + 0x7C));
    u8 copy[0x80];
    memcpy(copy, original, sizeof copy);
    memcpy(sword, copy, sizeof copy);
    SwordDb_fn15ccd0(swordDb, sword);
}

// ── 0x1447F0 ──────────────────────────────────────────────────────────────
// The player takes `sword` in hand.  0 when the sword's model cannot be shown.
char CharaMgr_EquipSword(CharaMgr* mgr, void* swordDb, Sword* sword) {
    Chara* c = Find(mgr, 0);
    if (c == nullptr)
        return 1;
    const u8* s = reinterpret_cast<const u8*>(sword);
    if (Chara_TakeSword(c, sword) != 0) {
        // Its model has to be loaded: run the load from step 4 again.
        if (mgr->loadQueue.size == 0)
            mgr->loadStep = 4;
        else
            mgr->pendingStep = 4;
        c->loaded = 0;
        GuestList_Insert(&mgr->loadQueue, GuestList_End(&mgr->loadQueue), guest::addr(c));
    }
    Chara_fn1cd850(c, SwordDb_fn15d890(swordDb, *reinterpret_cast<const u32*>(s + 0x68), 1));
    const u32 type = *reinterpret_cast<const u32*>(s + 0x70);
    Chara_fn1cd690(c, SwordDb_fn15e4b0(swordDb, type));
    Chara_fn1cd6b0(c, SwordDb_fn15e470(swordDb, type));
    const int r = SkillSheets_Request(*gObj2EE698, (int)type, PlayerData_GetReadiedSword(*gPlayerData));
    return !(r == -1 || r == 3);
}

// ── 0x144960 ──────────────────────────────────────────────────────────────
// Anyone else gets a new sword of a kind.
int CharaMgr_EquipKind(CharaMgr* mgr, int id, int kind, int arg) {
    void* db = SwordDb_Get();
    Chara* c = Find(mgr, id);
    if (c == nullptr)
        return 1;
    alignas(16) Sword sword;
    Sword_Ctor(&sword);
    if (SwordDb_fn15ce60(db, (u32)kind, &sword) != 0) {
        const u8* s = reinterpret_cast<const u8*>(&sword);
        if (Params(c)->unique == 0)
            SwordDb_fn15ccd0(db, &sword);
        Chara_fn1cd6c0(c, &sword, (u32)arg);
        Chara_fn1cd850(c, SwordDb_fn15d890(db, *reinterpret_cast<const u32*>(s + 0x68), 0));
        const u32 type = *reinterpret_cast<const u32*>(s + 0x70);
        Chara_fn1cd690(c, SwordDb_fn15e4b0(db, type));
        Chara_fn1cd6b0(c, SwordDb_fn15e470(db, type));
    }
    Sword_Dtor(&sword, -1);
    return 1;
}

// ══ The world's flags object ══════════════════════════════════════════════

// ── 0x144B20 ──────────────────────────────────────────────────────────────
void CharaMgr_Unflag(CharaMgr* mgr, void* flags, int id) {
    Chara* c = Find(mgr, id);
    if (c == nullptr)
        return;
    FlagsObj_Remove(flags, c->flagHandle);
    c->flagHandle = 0;
    for (int i = 0; i < 2; i++) {
        FlagsObj_Remove(flags, c->flagHandles[i]);
        c->flagHandles[i] = 0;
    }
}

// ── 0x144BD0 ──────────────────────────────────────────────────────────────
void CharaMgr_Flag(CharaMgr* mgr, void* flags, int id) {
    Chara* c = Find(mgr, id);
    if (c == nullptr)
        return;
    Chara_fn1cd860(c);
    c->flagHandle = FlagsObj_AddChara(flags, c->flagArg, id);
    for (int i = 0; i < 2; i++)
        c->flagHandles[i] = FlagsObj_Add(flags, c->flagArgs[0]);
}

}  // extern "C"
