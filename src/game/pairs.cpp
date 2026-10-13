// Who fights whom — the "pairs" object (World+0xF8, 8 bytes) and the list of
// fighters it works from.  Decompiled from 0x227B30–0x229630 and
// 0x174F30 / 0x1750C0.
//
// The *fighters* (a std::map<id, CharaParams*> at 0x309F00, rebuilt each
// frame) are the characters set to fight (target flags 0x10000000 "anyone of
// another faction" or 0x20000000 "only who it is given") that have their
// sword out, plus the player whenever there is at least one of them.
//
// Each frame every fighter's *opponent* (CharaParams+0xC0) is reviewed:
//   - someone who was just hit turns on whoever hit it, and someone who hit a
//     bystander turns on them (flags 0x400 / 0x800 from the blow code);
//   - an opponent that is dead or no longer fighting is dropped, and a new one
//     chosen: whoever is attacking it, else the nearest enemy it can get to,
//     else the nearest enemy;
//   - a fighter and its opponent that can reach each other and have each
//     other as opponent *duel* (state 1: the AI gets its fight routine); a
//     fighter whose opponent is busy with someone else waits its turn (state
//     2, AI mode 9) or goes for whoever attacks it; one that cannot reach its
//     opponent walks to it (state 4, AI mode 3).
// The low four target flags hold that state; 0x1000000 marks a fighter that
// stands facing its opponent (the pad routine and the body read it).

#include "engine/containers.h"
#include "game/ai_moves.h"
#include "game/chara.h"
#include "game/chara_mgr.h"
#include "game/map_db.h"
#include "native/guest.h"

#include <cmath>
#include <cstring>

extern "C" {
    void  MemFree(void* p);                                         // 0x100650
    void  CharaFace_Set(void* face, CharaParams* params, int expression);   // 0x2A2880
    // The HUD's side (enemy gauges, not decompiled: stand-ins).
    void  Hud_HideEnemies(void* hud);                               // 0x21CBB0 + 0x21CF60
    void  Hud_ShowEnemy(void* hud, CharaParams* who, int kind);     // 0x21CBC0
    void  Hud_EnemiesShown(void* hud, int a);                       // 0x21CC60
}

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static const GuestPtrVar<u8>   gPlayerData{0x2EE688u};
static const GuestPtrVar<void> gHud{0x2EE69Cu};
static u32* const gCharaMgr = guest::ptr<u32>(0x2EE5C4u);           // set by Collision_Update
static GuestIntMap* const gFighters = guest::ptr<GuestIntMap>(0x309F00u);

struct Pairs { u32 charas; u32 mapState; };

enum : u32 {
    kTfState = 0xF, kTfDuel = 1, kTfWait = 2, kTfApproach = 4, kTfHeld = 8,
    kTfNewOpponent = 0x10, kTfFree = 0x20, kTfHurt = 0x400, kTfHit = 0x800, kTfSwitch = 0x1000,
    kTfGroup = 0x2000, kTfFacing = 0x1000000, kTfAnnounced = 0x2000000,
    kTfHostile = 0x10000000, kTfDuelist = 0x20000000,
};

static inline u32 Word(u32 pointer) { return pointer ? *P<u32>(pointer) : 0; }
static inline u32 TargetFlags(const CharaParams* p) { return Word(p->link[8]); }    // +0xD4 → body+0x10
static inline u32 BodyFlags(const CharaParams* p)   { return Word(p->link[5]); }    // +0xC8 → body+0x08
static inline CharaParams* Opponent(const CharaParams* p) { return p->link[3] ? P<CharaParams>(p->link[3]) : nullptr; }
static inline CharaParams* ParamsOf(Chara* c) { return P<CharaParams>(c->params); }
static inline u32& Flags(Chara* c) { return P<CharaBody>(c->body)->targetFlags; }
static inline Chara* Find(Pairs* self, const CharaParams* p) { return CharaMgr_Find(P<CharaMgr>(self->charas), p->id); }
static inline float Apart(const CharaParams* a, const CharaParams* b) {
    const float x = a->matrix[12] - b->matrix[12], y = a->matrix[13] - b->matrix[13], z = a->matrix[14] - b->matrix[14];
    return sqrtf(x * x + y * y + z * z);
}
static inline CharaParams* Fighter(GuestMapNode* n) { return P<CharaParams>(n->value); }

extern "C" {

// ── 0x174F30: the fighters, made on first asking each frame ───────────────
GuestIntMap* Fighters_Get() {
    if (gFighters->leftmost == 0)
        GuestIntMap_Init(gFighters);
    if (gFighters->count != 0)
        return gFighters;
    CharaMgr* mgr = P<CharaMgr>(*gCharaMgr);
    for (GuestListNode* n = GuestList_Begin(&mgr->charas); n != GuestList_End(&mgr->charas); n = GuestList_Next(n)) {
        Chara* c = P<Chara>(n->value);
        if (c->active == 0)
            continue;
        CharaParams* p = ParamsOf(c);
        if (p->id != 0 && (TargetFlags(p) & (kTfHostile | kTfDuelist)) != 0 && (BodyFlags(p) & 0x100) != 0)
            GuestIntMap_Insert(gFighters, p->id, guest::addr(p));
    }
    Chara* player = CharaMgr_Find(mgr, 0);
    if (gFighters->count == 0) {
        if (ParamsOf(player)->link[3] != 0) {
            ParamsOf(player)->link[3] = 0;
            Flags(player) &= 0xFFFFEFF0u;
        }
    } else {
        if ((Flags(player) & kTfDuelist) == 0)
            Flags(player) |= kTfHostile;
        GuestIntMap_Insert(gFighters, player->id, player->params);
    }
    return gFighters;
}
// ── 0x1750C0 ──────────────────────────────────────────────────────────────
int Fighters_Any() { return gFighters->count != 0; }
// (Part of Collision_Release, 0x175A00: the list lasts one frame.)
void Fighters_Clear() {
    if (gFighters->leftmost == 0)
        GuestIntMap_Init(gFighters);
    GuestIntMap_Clear(gFighters);
}

}  // extern "C"

// Can `a` get at `b`: within 6, or either is waiting its turn, and nothing in
// the way.
static bool CanReach(const CharaParams* a, const CharaParams* b, float* apart) {
    *apart = Apart(a, b);
    if (6.0f < *apart && ((TargetFlags(a) | TargetFlags(b)) & kTfWait) == 0)
        return false;
    return Query_LineBlocked(&a->matrix[12], &b->matrix[12], 2) == nullptr;
}

static void DropOpponent(Chara* c) {
    if (1 < (u32)c->id && CharaControl_Kind(P<CharaControl>(c->action)) != 0)
        Chara_SetAction(c, CharaAction_Make(2, 0, nullptr));
    ParamsOf(c)->link[3] = 0;
    Flags(c) &= 0xF0000000u;
}
static void SetOpponent(Chara* c, CharaParams* opponent) {
    ParamsOf(c)->link[3] = guest::addr(opponent);
    Flags(c) &= 0xFDFFFFF0u;
}
static void SetOrDrop(Chara* c, CharaParams* opponent) {
    if (opponent == nullptr)
        DropOpponent(c);
    else
        SetOpponent(c, opponent);
}

// ── 0x227E10: cannot get at its opponent from here: walk to it ────────────
static void Pairs_Approach(Pairs* self, CharaParams* p) {
    Chara* c = Find(self, p);
    Flags(c) &= ~(u32)kTfState;
    Flags(c) |= kTfApproach;
    if (1 < (u32)p->id && CharaControl_Kind(P<CharaControl>(c->action)) != 3) {
        const u32 links = self->mapState + (u32)*P<s32>(0x2EE518u) * 0x10;
        struct { u32 route, linksA, linksB, target; float within; u32 _14; u32 flag; } data{};
        data.route = guest::addr(MapDb_GetRoutePoints(MapDb_Get(), (s8)(*gPlayerData)[0x103]));
        data.linksA = links + 0x20;
        data.linksB = links + 0x60;
        data.target = p->link[3];
        data.within = 1.0f;
        data.flag = 1;
        Chara_SetAction(c, CharaAction_Make(2, 3, &data));
    }
}

// ── 0x227F20: the nearest fighter of another faction ──────────────────────
static CharaParams* Pairs_NearestFoe(Pairs*, CharaParams* p) {
    GuestIntMap* all = Fighters_Get();
    float best = 1000.0f;
    CharaParams* found = nullptr;
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        CharaParams* f = Fighter(n);
        if (p->faction == f->faction)
            continue;
        const float apart = Apart(p, f);
        if (apart < best) {
            best = apart;
            found = f;
        }
    }
    return found;
}
// ── 0x228020: the nearest fighter that has it as opponent ─────────────────
static CharaParams* Pairs_NearestAttacker(Pairs*, CharaParams* p) {
    GuestIntMap* all = Fighters_Get();
    float best = 1000.0f;
    CharaParams* found = nullptr;
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        CharaParams* f = Fighter(n);
        if (Opponent(f) == nullptr || Opponent(f)->id != p->id)
            continue;
        const float apart = Apart(p, f);
        if (apart < best) {
            best = apart;
            found = f;
        }
    }
    return found;
}
// ── 0x228120: the nearest fighter of another faction it can get at ────────
static CharaParams* Pairs_NearestReachableFoe(Pairs*, CharaParams* p) {
    GuestIntMap* all = Fighters_Get();
    float best = 1000.0f;
    CharaParams* found = nullptr;
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        CharaParams* f = Fighter(n);
        float apart;
        if (p->faction == f->faction || !CanReach(p, f, &apart))
            continue;
        if (apart < best) {
            best = apart;
            found = f;
        }
    }
    return found;
}
// ── 0x2282A0: the nearest fighter with it as opponent that it can get at ──
static CharaParams* Pairs_NearestReachableAttacker(Pairs*, CharaParams* p) {
    GuestIntMap* all = Fighters_Get();
    float best = 1000.0f;
    CharaParams* found = nullptr;
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        CharaParams* f = Fighter(n);
        float apart;
        if (Opponent(f) == nullptr || Opponent(f)->id != p->id || !CanReach(p, f, &apart))
            continue;
        if (apart < best) {
            best = apart;
            found = f;
        }
    }
    return found;
}

// Who a fighter without an opponent should take.
static CharaParams* Pairs_Choose(Pairs* self, CharaParams* p) {
    CharaParams* found = Pairs_NearestAttacker(self, p);
    if ((TargetFlags(p) & kTfHostile) == 0) {
        if (found == nullptr)
            found = Pairs_NearestReachableFoe(self, p);
        if (found == nullptr)
            found = Pairs_NearestFoe(self, p);
    } else if (found == nullptr) {
        found = Opponent(p);
    }
    return found;
}

// ── 0x2288D0: face to face: fight ─────────────────────────────────────────
static void Pairs_Duel(Pairs*, Chara* c) {
    Flags(c) &= ~3u;
    Flags(c) |= kTfDuel;
    if ((u32)c->id <= 1)
        return;
    const s32 kind = CharaControl_Kind(P<CharaControl>(c->action));
    if ((Flags(c) & kTfHeld) == 0) {
        if (kind != 1) {
            // (With kTfGroup the original passes the group side of the battle
            // tables for the sword it carries — 0x29A390 — which is not read.)
            Flags(c) &= ~(u32)kTfGroup;
            Chara_SetAction(c, CharaAction_Make(2, 1, nullptr));
        }
        if ((Flags(c) & kTfAnnounced) == 0) {
            if (c->_1d8 != 0)                           // 0x1CCA50: the fighting face
                CharaFace_Set(P(c->_1d8), ParamsOf(c), 8);
            Flags(c) |= kTfAnnounced;
        }
    } else if (kind != 10) {
        Chara_SetAction(c, CharaAction_Make(2, 10, nullptr));
    }
}

// ── 0x2285B0: it can get at its opponent.  → 0 duelling, 1 changed, −1 ────
static int Pairs_Engage(Pairs* self, CharaParams* p) {
    CharaParams* opponent = Opponent(p);
    if (opponent == nullptr)
        return -1;
    CharaParams* theirs = Opponent(opponent);
    if (theirs == nullptr)
        return -1;
    Chara* c = Find(self, p);
    if ((TargetFlags(p) & kTfDuel) != 0) {
        if (p->id == theirs->id)
            return 0;
        Flags(c) |= kTfNewOpponent;         // its opponent has turned to someone else
        Flags(c) &= ~(u32)kTfDuel;
        return -1;
    }
    if ((TargetFlags(theirs) & (kTfDuel | kTfHeld)) == 0) {
        Chara* other = CharaMgr_Find(P<CharaMgr>(self->charas), opponent->id);
        const CharaParams* mine = Opponent(ParamsOf(c));
        const CharaParams* back = Opponent(ParamsOf(other));
        if (mine != nullptr && back != nullptr && mine->id == other->id && back->id == c->id) {
            Pairs_Duel(self, c);
            Pairs_Duel(self, other);
        }
        return 0;
    }
    // Its opponent is busy with someone: take whoever is after it, or wait.
    CharaParams* attacker = Pairs_NearestAttacker(self, p);
    if (attacker == nullptr) {
        Flags(c) &= 0xFDFFFFFCu;
        Flags(c) |= kTfWait;
        if (1 < (u32)c->id && (Flags(c) & kTfHeld) == 0 && CharaControl_Kind(P<CharaControl>(c->action)) != 9)
            Chara_SetAction(c, CharaAction_Make(2, 9, nullptr));
    } else {
        Flags(c) &= 0xFDFFFFF0u;
        ParamsOf(c)->link[3] = guest::addr(attacker);
    }
    return 1;
}

// ── 0x228430: it cannot get at its opponent.  → 1 if it took another ──────
static int Pairs_Retarget(Pairs* self, CharaParams* p) {
    CharaParams* found = Pairs_Choose(self, p);
    if (found == nullptr || Opponent(p) == nullptr || Opponent(p)->id == found->id) {
        Pairs_Approach(self, p);
        return 0;
    }
    SetOrDrop(Find(self, p), found);
    return 1;
}

// ── 0x2287E0 ──────────────────────────────────────────────────────────────
static void Pairs_Keep(Pairs* self, CharaParams* p) {
    float apart;
    if (CanReach(p, Opponent(p), &apart))
        Pairs_Engage(self, p);
    else
        Pairs_Retarget(self, p);
}

static bool IsFighter(s32 id) {
    GuestIntMap* all = Fighters_Get();
    GuestMapNode* n = GuestIntMap_Find(all, id);
    return n != nullptr && n != GuestIntMap_End(all);
}

// ── 0x228A10: it was hit: turn on whoever did it ──────────────────────────
static int Pairs_OnHurt(Pairs* self, CharaParams* p) {
    Chara* c = Find(self, p);
    if (c == nullptr)
        return 0;
    Flags(c) &= ~(u32)kTfHurt;
    if (p->_d8[0] == 0)
        return 0;
    CharaParams* attacker = P<CharaParams>(p->_d8[0]);
    if (TargetFlags(p) & kTfHeld)
        return 0;
    if (Opponent(p) != nullptr && (TargetFlags(p) & kTfFree) == 0)
        return 0;
    if (p->faction == attacker->faction || !IsFighter(attacker->id))
        return 0;
    Flags(c) &= 0xFDFFFFF0u;
    ParamsOf(c)->link[3] = guest::addr(attacker);
    return 1;
}
// ── 0x228B90: it hit someone other than its opponent: turn on them ────────
static int Pairs_OnHit(Pairs* self, CharaParams* p) {
    Chara* c = Find(self, p);
    if (c == nullptr)
        return 0;
    Flags(c) &= ~(u32)kTfHit;
    if (Opponent(p) == nullptr || c->_1dc == 0)
        return 0;
    CharaParams* victim = P<CharaParams>(c->_1dc);
    if (victim->id == Opponent(p)->id || p->faction == victim->faction || !IsFighter(victim->id))
        return 0;
    Flags(c) &= 0xFDFFFFF0u;
    ParamsOf(c)->link[3] = guest::addr(victim);
    return 1;
}

// ── 0x228CD0: does it need a new opponent? ────────────────────────────────
static int Pairs_NeedsOpponent(Pairs* self, CharaParams* p) {
    Chara* c = Find(self, p);
    if (TargetFlags(p) & kTfNewOpponent) {
        Flags(c) &= ~(u32)kTfNewOpponent;
        return 1;
    }
    CharaParams* opponent = Opponent(p);
    if (opponent == nullptr)
        return 1;
    if ((TargetFlags(opponent) & (kTfHostile | kTfDuelist)) != 0 && (BodyFlags(opponent) & 0x200) == 0)
        return 0;
    ParamsOf(c)->link[3] = 0;               // it has stopped fighting, or died
    Flags(c) &= 0xF0000000u;
    return 1;
}

// ── 0x228F50: asked to change (flag 0x1000): take another of its opponent's
// side that is after it ────────────────────────────────────────────────────
static void Pairs_Switch(Pairs* self, CharaParams* p) {
    const u32 flags = TargetFlags(p);
    if ((flags & kTfSwitch) == 0)
        return;
    Chara* c = Find(self, p);
    Flags(c) &= ~(u32)kTfSwitch;
    CharaParams* opponent = Opponent(p);
    if ((flags & kTfFree) != 0 || opponent == nullptr)
        return;
    GuestIntMap* all = Fighters_Get();
    float best = 1000.0f;
    CharaParams* found = nullptr;
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        CharaParams* f = Fighter(n);
        float apart;
        if (opponent->id == f->id || opponent->faction != f->faction || Opponent(f) == nullptr
            || Opponent(f)->id != p->id || !CanReach(p, f, &apart))
            continue;
        if (apart < best) {
            best = apart;
            found = f;
        }
    }
    SetOrDrop(c, found);
    if (found != nullptr)
        Flags(Find(self, found)) |= kTfGroup;
}

// ── 0x229220 ──────────────────────────────────────────────────────────────
static void Pairs_Review(Pairs* self) {
    GuestIntMap* all = Fighters_Get();
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        const u32 flags = TargetFlags(Fighter(n));
        if (flags & (kTfHurt | kTfHit)) {
            if ((flags & kTfHit) == 0)
                Pairs_OnHurt(self, Fighter(n));
            else
                Pairs_OnHit(self, Fighter(n));
        }
    }
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        CharaParams* p = Fighter(n);
        Pairs_Switch(self, p);
        // Free to choose, or sword away: whoever comes at it becomes its opponent.
        if ((TargetFlags(p) & kTfFree) != 0 || (BodyFlags(p) & 0x100) == 0) {
            CharaParams* attacker = Pairs_NearestReachableAttacker(self, p);
            if (attacker != nullptr && (Opponent(p) == nullptr || attacker->id != Opponent(p)->id)) {
                Chara* c = Find(self, p);
                Chara* other = Find(self, attacker);
                ParamsOf(c)->link[3] = guest::addr(attacker);
                const CharaParams* back = Opponent(ParamsOf(other));
                if (back != nullptr && back->id == c->id) {
                    Pairs_Duel(self, c);
                    Pairs_Duel(self, other);
                }
            }
        }
        if (Pairs_NeedsOpponent(self, p) == 0)
            Pairs_Keep(self, p);
        else
            SetOrDrop(Find(self, p), Pairs_Choose(self, p));
    }
}

// ── 0x228DF0: who stands facing an opponent ───────────────────────────────
static void Pairs_MarkFacing(Pairs* self) {
    GuestIntMap* all = Fighters_Get();
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        CharaParams* p = Fighter(n);
        const u32 flags = TargetFlags(p);
        if (Opponent(p) != nullptr && (flags & 3) != 0 && (BodyFlags(p) & 0x100) != 0 && (flags & kTfFree) == 0)
            Flags(Find(self, p)) |= kTfFacing;
        else if (flags & kTfFacing)
            Flags(Find(self, p)) &= ~(u32)kTfFacing;
    }
}

extern "C" {

// ── 0x229630 / 0x2295E0 ───────────────────────────────────────────────────
void* Pairs_Ctor(void* mem) {
    Pairs* self = static_cast<Pairs*>(mem);
    self->charas = 0;
    self->mapState = 0;
    return mem;
}
void Pairs_Dtor(void* self, int flag) {
    if (self != nullptr && flag > 0)
        MemFree(self);
}

// ── 0x229580 ──────────────────────────────────────────────────────────────
void Pairs_Update(void* mem, void* charaMgr, void* mapState) {
    Pairs* self = static_cast<Pairs*>(mem);
    self->charas = guest::addr(charaMgr);
    self->mapState = guest::addr(mapState);
    if (self->mapState != 0 && self->charas != 0) {
        Pairs_Review(self);
        Pairs_MarkFacing(self);
    }
}

// ── 0x227B30: the enemy gauges: the player's opponent, then up to five
// others that are after the player, nearest first ──────────────────────────
void Pairs_UpdateHud(void*, void* charaMgr, void*) {
    if (charaMgr == nullptr)
        return;
    if (!Fighters_Any()) {
        Hud_HideEnemies(*gHud);
        return;
    }
    Chara* player = CharaMgr_Find(static_cast<CharaMgr*>(charaMgr), 0);
    if (player == nullptr || player->params == 0)
        return;
    CharaParams* me = ParamsOf(player);
    GuestIntMap* all = Fighters_Get();
    CharaParams* opponent = Opponent(me);
    if (opponent == nullptr) {
        Hud_HideEnemies(*gHud);
        return;
    }
    auto kindOf = [&](const CharaParams* who) {
        return me->faction == who->faction ? 0 : who->unique == 0 ? 1 : 3;
    };
    Hud_ShowEnemy(*gHud, opponent, kindOf(opponent));
    Hud_EnemiesShown(*gHud, 1);
    struct Entry { float apart; CharaParams* who; };
    Entry others[5];
    int count = 0;
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        CharaParams* f = Fighter(n);
        if (Opponent(f) == nullptr || Opponent(f)->id != 0 || f->id == opponent->id || f->id == 0)
            continue;
        const float apart = Apart(me, f);
        int at = count < 5 ? count : 5;
        while (at > 0 && apart < others[at - 1].apart) {    // (ties keep their order)
            if (at < 5)
                others[at] = others[at - 1];
            at--;
        }
        if (at < 5) {
            others[at] = {apart, f};
            if (count < 5)
                count++;
        }
    }
    for (int i = 0; i < count; i++)
        Hud_ShowEnemy(*gHud, others[i].who, kindOf(others[i].who));
}

}  // extern "C"
