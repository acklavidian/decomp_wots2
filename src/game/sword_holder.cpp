// The sword a character has in hand (Chara+0x1D4; 0x1C bytes) — decompiled
// from 0x244C70–0x245A00, with the wear rules at 0x16A0C0 / 0x16A0F0.
//
// What it sets off elsewhere: the blade's trail, the glint when a technique
// is learnt and the shards when the sword is worn down a point
// (world_effects.cpp), the notice of the technique (skill_sheets.cpp).

#include "game/chara.h"
#include "game/sword.h"
#include "native/guest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
    // src/game/world_effects.cpp: the blade's trail (effect 0x10)
    void* Fx_NewTrail();
    int   Fx_TrailAlive(u32 trail);
    void  Fx_TrailStyle(void* trail, int style);
    void  Fx_TrailAdd(void* trail, const float* matrix, const float* tip, const float* base);
    void  Fx_TrailRelease(u32 trail);
    void  Fx_TechniqueGlint(CharaParams* owner);    // 0x187550: the glint at the eye
    void  Fx_SwordShards(const float* pos);         // 0x24C340: steel flying from a hand
    // Native: where a hand part of a sword model is, given the hand's bone
    // (the part's own root matrix × the bone) → 0 if the part is not there.
    int   hle_sword_part_matrix(void* swordModel, int slot, const float* hand, float* out);
}
extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void  Sound_PlaySE(int id, int positional, int volume);     // 0x180E70
    int   PlayerData_GetReadiedSword(void* pd);
    int   SwordModel_DrawMount(void* model, int slot, const float* matrix);            // 0x15A6F0 (native)
    int   SwordModel_DrawHand(void* model, int slot, const float* matrix, int own);    // 0x15A610 (native)
    void  SwordRecords_Learnt(void* records, void* sword, int slot, int technique, int next);  // 0x23B7F0
}

struct SwordHolder {
    u32 model;              // +0x00  SwordModel* (sword_db.cpp)
    u32 sword;              // +0x04  Sword*
    u32 sheet;              // +0x08  SkillSheet*: what the style can learn
    u32 shadow[2];          // +0x0C
    s32 ownsSword;          // +0x14  the sword is the holder's own copy
    s32 learnt;             // +0x18  a technique was learnt this frame
};
static_assert(sizeof(SwordHolder) == 0x1C && sizeof(CharaSwords) == 0x1C, "SwordHolder");

static inline SwordHolder* H(void* self) { return static_cast<SwordHolder*>(self); }
static inline Sword* InHand(SwordHolder* h) { return guest::ptr<Sword>(h->sword); }
static const GuestPtrVar<u8> gPlayerData{0x2EE688u};

extern "C" {

// ── 0x245500: the sword, where the character carries it ───────────────────
// Sheathed: the two parts that hang at the hip (the broken ones if it has
// broken).  Drawn: the empty sheath at the hip, and the sword in the hand.
// (A sword of kind 7 shows in the hand only while it strikes.)
void CharaSwords_Draw(void* swords, CharaBody* body) {
    SwordHolder* h = H(swords);
    if (h->model == 0 || h->sword == 0)
        return;
    const u32* model = guest::ptr<u32>(h->model);
    if ((model[3] & model[4]) == 0)                     // textures and model both read
        return;
    const u8* sword = guest::ptr<u8>(h->sword);
    s32 kind, durability;
    memcpy(&kind, sword + 0x74, 4);
    memcpy(&durability, sword + 0x60, 4);
    bool drawn = false;
    if (body->flags & 0x100) {
        drawn = (body->actionFlags & 0x4000) != 0 || kind != 7;
        if (body->actionFlags & 0x8000)
            drawn = false;
    }
    const Skeleton* skeleton = guest::ptr<Skeleton>(guest::ptr<CharaParams>(body->params)->link[1]);
    const Bone* bones = guest::ptr<Bone>(skeleton->bones);
    const s16* mounts = guest::ptr<s16>(model[5]);
    const s16* hands = guest::ptr<s16>(model[6]);
    void* m = guest::ptr(h->model);
    if (drawn) {
        SwordModel_DrawMount(m, 2, bones[mounts[2]].world);
        SwordModel_DrawMount(m, 3, bones[mounts[3]].world);
        const int a = durability == 0 ? 2 : 0, b = durability == 0 ? 3 : 1;
        SwordModel_DrawHand(m, a, bones[hands[a]].world, 1);
        SwordModel_DrawHand(m, b, bones[hands[b]].world, 1);
    } else {
        const int a = durability == 0 ? 4 : 0, b = durability == 0 ? 5 : 1;
        SwordModel_DrawMount(m, a, bones[mounts[a]].world);
        SwordModel_DrawMount(m, b, bones[mounts[b]].world);
    }
}

// ── 0x245A00 / 0x245940 ───────────────────────────────────────────────────
void* SwordHolder_Ctor(void* mem) {
    memset(mem, 0, sizeof(SwordHolder));
    return mem;
}
void* SwordHolder_Dtor(void* self, int flag) {
    if (!self)
        return self;
    SwordHolder* h = H(self);
    if (h->ownsSword != 0) {
        if (h->sword != 0)
            Sword_Dtor(InHand(h), 1);
        h->sword = 0;
    }
    h->model = 0;
    h->shadow[0] = h->shadow[1] = 0;            // (the original releases them)
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

// ── 0x245750 ──────────────────────────────────────────────────────────────
// The player's: the sword itself, where it lives in the player data.
void CharaSwords_Take(void* swords, const void* sword) { H(swords)->sword = guest::addr(sword); }

// ── 0x245760 ──────────────────────────────────────────────────────────────
// Anyone else's: a copy.
void CharaSwords_Give(void* swords, const void* sword) {
    SwordHolder* h = H(swords);
    if (h->sword == 0) {
        void* mem = MemAlloc(sizeof(Sword));
        h->sword = mem ? guest::addr(Sword_Ctor(static_cast<Sword*>(mem))) : 0;
        h->ownsSword = 1;
    }
    Sword_Init(InHand(h));
    const u8* from = static_cast<const u8*>(sword);
    u8* to = guest::ptr<u8>(h->sword);
    memcpy(to, from, 0x80);
    memcpy(to + 0x80, from + 0x80, 0x10);
    memcpy(to + 0x90, from + 0x90, 0x40);
    memcpy(to + 0xD0, from + 0xD0, 4);
    memcpy(to + 0xD4, from + 0xD4, 0x2C);
}

// ── 0x245710 ──────────────────────────────────────────────────────────────
int CharaSwords_IsReady(CharaSwords* swords) {
    SwordHolder* h = H(swords);
    if (h->model == 0 || h->sword == 0)
        return 0;
    const s32* model = guest::ptr<s32>(h->model);
    return model[3] & model[4];
}
// ── 0x2454B0 ──────────────────────────────────────────────────────────────
void CharaSwords_fn2454b0(CharaSwords* swords) {
    if (H(swords)->sword != 0)
        InHand(H(swords))->_80 = 0;
}
// ── 0x2454D0 → 0x16A0C0 ───────────────────────────────────────────────────
// The strain on a sword eases by five a frame.
void CharaSwords_BeginFrame(void* swords) {
    SwordHolder* h = H(swords);
    if (h->sword == 0)
        return;
    Sword* sword = InHand(h);
    if (sword->durability != 0 && (sword->_80 -= 5) < 0)
        sword->_80 = 0;
}

// ── 0x2452F0 → 0x16A0F0 ───────────────────────────────────────────────────
// Blows given and guarded strain the sword; at 600 × its durability it
// loses a point of it.  → 1 chipped, 2 broken (durability gone), 0.
int CharaSwords_Wear(void* swords, int amount, CharaParams* owner) {
    SwordHolder* h = H(swords);
    if (amount == 0 || h->sword == 0)
        return 0;
    Sword* sword = InHand(h);
    if (sword->durability == 0)
        return 0;
    sword->_80 += amount;
    if ((u32)sword->_80 < (u32)(sword->durability * 600))
        return 0;
    sword->durability--;
    sword->_80 = 0;
    const int state = sword->durability != 0 ? 1 : 2;
    Sound_PlaySE(27, owner != nullptr, 0x3FFF);         // 0x2DA380
    if (h->model != 0 && owner != nullptr) {            // shards from each hand that holds a blade
        const u32 points = owner->link[1] != 0 ? guest::ptr<u32>(owner->link[1])[1] : 0;
        const u32* parts = guest::ptr<u32>(h->model + 0x34);
        const s16* bones = guest::ptr<s16>(*guest::ptr<u32>(h->model + 0x18));
        for (int i = 0; i < 2 && points != 0; i++)
            if (parts[i] != 0)
                Fx_SwordShards(guest::ptr<float>(points + (u32)(bones[i] * 0xA0 + 0x30)));
    }
    return state;
}

// ── 0x245150 ──────────────────────────────────────────────────────────────
// A sword just made: nothing learnt but what its sheet starts with.
void CharaSwords_fn245150(CharaSwords* swords) {
    SwordHolder* h = H(swords);
    if (h->sheet == 0 || h->sword == 0)
        return;                                 // (the original complains)
    Sword* sword = InHand(h);
    sword->_84 = 0;
    sword->_88 = 0;
    const SkillSheet* sheet = guest::ptr<SkillSheet>(h->sheet);
    const SkillEntry* entries = guest::ptr<SkillEntry>(sheet->entries);
    for (s32 i = 0; i < sheet->count; i++)
        sword->skill[i] = 0;
    for (s32 i = 0; i < sheet->count; i++) {
        if (entries[i].requirement != 0 || sword->skill[i] != 0)
            continue;
        sword->skill[i] = 1;
        if (entries[i].next != -1)
            sword->skill[entries[i].next] = 2;
    }
}

// ── 0x244C70 / 0x244CB0 ───────────────────────────────────────────────────
// Two tallies kept on the sword (up to 9,999): the second is what
// techniques cost.
void CharaSwords_AddTally(void* swords, int amount) {
    if (H(swords)->sword == 0) return;
    s32& v = InHand(H(swords))->_84;
    v = v + amount > 9999 ? 9999 : v + amount;
}
void CharaSwords_AddKills(void* swords, int amount) {
    if (H(swords)->sword == 0) return;
    s32& v = InHand(H(swords))->_88;
    v = v + amount > 9999 ? 9999 : v + amount;
}

// ── 0x244DD0 ──────────────────────────────────────────────────────────────
// Learn technique `index` if the sword has the tally for it (one a frame).
int CharaSwords_Learn(void* swords, int index, CharaParams* owner) {
    SwordHolder* h = H(swords);
    if (h->learnt == 1 || h->sheet == 0)
        return 0;
    const SkillSheet* sheet = guest::ptr<SkillSheet>(h->sheet);
    if (index < 0 || index >= sheet->count)
        return 0;
    Sword* sword = InHand(h);
    const SkillEntry* entry = guest::ptr<SkillEntry>(sheet->entries) + index;
    if (sword->skill[index] != 0 || entry->_04 > sword->_84) {
        if (sword->skill[index] == 0 && getenv("WOTS2_TRACE_HITS"))
            printf("[hit] technique %d wants a tally of %d (have %d)\n", index, entry->_04, sword->_84);
        return 0;
    }
    sword->skill[index] = 1;
    if (entry->next != -1)
        sword->skill[entry->next] = 2;
    if (getenv("WOTS2_TRACE_HITS"))                 // (native debug aid)
        printf("[hit] learnt technique %d (cost %d, tally %d)\n", index, entry->_04, sword->_84);
    SwordRecords_Learnt(guest::ptr(*guest::ptr<u32>(0x2EE698u)), sword, PlayerData_GetReadiedSword(*gPlayerData),
                        index, entry->next);
    Fx_TechniqueGlint(owner);
    h->learnt = 1;
    return 1;
}
// ── 0x244F90 ──────────────────────────────────────────────────────────────
// …the first unlearnt technique of a kind (what the deed that earns it was).
int CharaSwords_LearnKind(void* swords, int kind, CharaParams* owner) {
    SwordHolder* h = H(swords);
    if (h->learnt == 1 || h->sheet == 0)
        return 0;
    const SkillSheet* sheet = guest::ptr<SkillSheet>(h->sheet);
    const SkillEntry* entries = guest::ptr<SkillEntry>(sheet->entries);
    for (s32 i = 0; i < sheet->count; i++)
        if (entries[i].requirement == kind && InHand(h)->skill[i] == 0 && entries[i]._04 <= InHand(h)->_84)
            return CharaSwords_Learn(swords, i, owner);
    return 0;
}


// ── 0x244CF0 ──────────────────────────────────────────────────────────────
// …the techniques that follow on from the blow in progress (`node`, a node
// of the combo tree: +0x0C count, +0x10 → the nodes after it, each naming
// the technique it needs at +0x08) and are earned by a deed of this kind.
int CharaSwords_LearnNext(void* swords, int kind, u32 node, CharaParams* owner) {
    if (node == 0)
        return 0;
    const SwordHolder* h = H(swords);
    if (h->sheet == 0)
        return 0;
    const SkillSheet* sheet = guest::ptr<SkillSheet>(h->sheet);
    const SkillEntry* entries = guest::ptr<SkillEntry>(sheet->entries);
    const s32 count = *guest::ptr<s32>(node + 0x0C);
    const u32* next = guest::ptr<u32>(*guest::ptr<u32>(node + 0x10));
    for (s32 i = 0; i < count; i++) {
        const u8 technique = *guest::ptr<u8>(next[i] + 8);
        if (kind == entries[technique].requirement && CharaSwords_Learn(swords, technique, owner) != 0)
            return 1;
    }
    return 0;
}

// ── 0x2458C0: the sword has left the hand for good ────────────────────────
void CharaSwords_Clear(void* self) {
    SwordHolder* h = H(self);
    if (h->ownsSword != 0) {
        if (h->sword != 0)
            Sword_Dtor(guest::ptr<Sword>(h->sword), 1);
        h->sword = 0;
    }
    h->model = 0;
    for (u32& trail : h->shadow) {          // its trails fade out by themselves (0x172090)
        if (trail != 0)
            Fx_TrailRelease(trail);
        trail = 0;
    }
}
// ── 0x245880 (with 0x244B60 / 0x244940): the blade's trail.  In the frames
// of an attack in which the blow can land, each blade in use — the right
// hand's, the left's, or both, by the attack's record — hands its two ends
// to a trail effect (made when first wanted; older notes took this for the
// sword's shadow, and the holder's field is still called that). ───────────
void CharaSwords_DrawShadow(void* swords, CharaBody* body) {
    SwordHolder* h = H(swords);
    if (h->model == 0 || body == nullptr)
        return;
    const u32* model = guest::ptr<u32>(h->model);
    if ((model[3] & model[4]) == 0)
        return;
    h->learnt = 0;
    // 0x1CAD90 / 0x1CAB40 / 0x1CABE0: the action's attack record, its frames, the bone that strikes.
    const u32 def = body->actionDef != 0 ? *guest::ptr<u32>(body->actionDef) : 0;
    const u32 attackAt = def != 0 ? guest::ptr<u32>(def)[1] : 0;
    if (attackAt == 0)
        return;
    const s32* attack = guest::ptr<s32>(attackAt);
    const s32 frame = (s32)*reinterpret_cast<const float*>(guest::ptr<u8>(body->player) + 8);
    if (frame < attack[4] || attack[5] < frame)
        return;
    int count = 2, part = 0;
    if (attack[0] != 0x100) {
        count = 1;
        if (attack[0] == 0x1D)
            part = 1;
    }
    s32 durability;
    memcpy(&durability, guest::ptr<u8>(h->sword) + 0x60, 4);
    if (durability == 0)
        part += 2;                          // the broken blade's parts
    const Skeleton* skeleton = guest::ptr<Skeleton>(guest::ptr<CharaParams>(body->params)->link[1]);
    const Bone* bones = guest::ptr<Bone>(skeleton->bones);
    const s16* hands = guest::ptr<s16>(model[6]);
    for (int i = 0; i < count; i++, part++) {
        if (part > 3)
            break;
        u32& trail = h->shadow[i];
        if (trail != 0 && !Fx_TrailAlive(trail))
            trail = 0;
        if (trail == 0) {
            void* made = Fx_NewTrail();
            trail = made != nullptr ? guest::addr(made) : 0;
        }
        if (trail == 0)
            continue;
        Fx_TrailStyle(guest::ptr(trail), (body->actionFlags & 0x2000000) ? 1 : (body->actionFlags & 0x1000000) ? 2 : 0);
        const u32 box = model[0x11 + part];                     // SwordModel+0x44: the blade's box (0x15A810)
        if (box == 0)
            continue;
        alignas(16) float matrix[16];
        if (!hle_sword_part_matrix(guest::ptr(h->model), part, bones[hands[part]].world, matrix))
            continue;
        Fx_TrailAdd(guest::ptr(trail), matrix, guest::ptr<float>(box + 0x30), guest::ptr<float>(box + 0x20));
    }
}
// ── 0x2691F0 / 0x2691E0 (vtable +0x24 / +0x28 of everyone but the player):
// someone who has died still holding a sword lets it fall — the character
// manager drops it into the world (CharaMgr_StepSwords) and then has the
// holder forget it. ────────────────────────────────────────────────────────
int CharaOther_SwordLeft(Chara* self) {
    if (guest::ptr<SwordHolder>(self->swords)->sword == 0)
        return 0;
    const u32 flagsAt = guest::ptr<CharaParams>(self->params)->link[5];      // +0xC8 → the body's flags
    return flagsAt != 0 && (*guest::ptr<u32>(flagsAt) & 0x200) != 0;
}
void CharaOther_SwordGone(Chara* self) { CharaSwords_Clear(guest::ptr<void>(self->swords)); }
GUEST_BIND(0x2458C0, CharaSwords_Clear);
GUEST_BIND(0x2691F0, CharaOther_SwordLeft);
GUEST_BIND(0x2691E0, CharaOther_SwordGone);

}  // extern "C"
