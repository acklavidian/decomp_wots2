// The fight — an AI move (SetAI mode 1) and the tables it works from.
// Decompiled from 0x18D230–0x18E630 (the move) and 0x29A360–0x29AA10 with
// 0x2A0530–0x2A0C40 and 0x2A4210–0x2A4410 (the tables: Common/BattleAI/
// totaldata, asset 0x85).  The group side of the tables (0x29FFE0–0x2A00D0)
// is not read.
//
// How a fighter decides.  Its *profile* (by its role, else by what kind of
// sword it carries) holds a preferred distance, a list of *situations* it
// answers — what the opponent is doing: attacking high or low, guarding,
// running, down… — each with a patience (how many frames of it before it
// answers), a list of *responses* {what, for how long}, and a table of
// chances: for each situation, the odds of each response.  A response is an
// attack sequence (a list of the same button codes the player's pad makes,
// fed to the same combo tree), a guard, a sidestep, closing or opening the
// distance, or waiting.  Easy and Normal blunt it at random.

#include "game/chara_mgr.h"
#include "game/ai_moves.h"
#include "game/chara.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    int   Rand_Next();                                  // 0x1200F0
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Matrix_FaceTowards(float* matrix, const float* point);    // 0x1C8DB0
}
extern "C" int Collision_AttackerOf(int id);            // 0x1754C0 (task_mgr.cpp)
int Matrix_TurnTowards(float degrees, float* matrix, const float* point);   // 0x1C8C40 (task_actions.cpp)

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);
static const GuestPtrVar<u8> gPlayerData{0x2EE688u};
static inline s8 Difficulty() { return (s8)(*gPlayerData)[0x105]; }

// ═══ The tables ═══════════════════════════════════════════════════════════

struct PairNode { u32 prev, next; s32 a, b; };          // a list node holding two ints
struct AiProfile {
    float distance;         // +0x00  where it likes to stand from its opponent
    s32   attackA, attackB; // +0x04  which rows of the two attack tables are its own
    s32   _0c;
    s32   memory;           // +0x10  how many of the opponent's blows it remembers
    s32   _14;
    GuestList situations;   // +0x18  {situation, patience}
    GuestList responses;    // +0x24  {response, frames}
    u32   chances;          // +0x30  s32[situations × responses], per cent
};
struct AttackTable {        // 0x2A0620
    s32 count;              // attack sequences to choose from…
    u32 sequences;          // …their ids
    s32 rows;
    u32 rowIds;             // which profile row is which
    u32 chances;            // s32[rows × count]
};
struct AttackSequence { s32 count; u32 codes; };
struct BattleSingle {       // 0x64 bytes (0x2A0C40)
    u8  _00[0x38];
    GuestIntMap profiles;   // +0x38  index → AiProfile*
    u32 bySword;            // +0x48  s32[8]: the profile for each kind of sword
    GuestIntMap byRole;     // +0x4C  role → profile index
    u32 attacks[2];         // +0x5C  AttackTable*
};
struct BattleAi {           // 0x18 bytes at *0x2EE6A4 (0x29AA10)
    u32 file;
    s32 step;
    u32 buffer;
    u32 single;             // BattleSingle*
    u32 group;              // (not read)
    u32 sequences;          // GuestIntMap*: id → AttackSequence*
};
static_assert(offsetof(BattleSingle, byRole) == 0x4C && sizeof(BattleSingle) == 0x64, "BattleSingle");

static void PushPair(GuestList* list, s32 a, s32 b) {
    PairNode* node = static_cast<PairNode*>(MemAlloc(sizeof(PairNode)));
    node->a = a;
    node->b = b;
    PairNode* end = reinterpret_cast<PairNode*>(&list->endPrev);
    node->next = guest::addr(end);
    node->prev = end->prev;
    P<PairNode>(end->prev)->next = guest::addr(node);
    end->prev = guest::addr(node);
    list->size++;
}
static const PairNode* PairAt(const GuestList* list, int index) {
    const PairNode* end = reinterpret_cast<const PairNode*>(&list->endPrev);
    const PairNode* n = P<PairNode>(list->endNext);
    for (int i = 0; i < index && n != end; i++)
        n = P<PairNode>(n->next);
    return n != end ? n : nullptr;
}

// ── 0x2A07D0 ──────────────────────────────────────────────────────────────
static void ParseProfiles(BattleSingle* single, const u8* at) {
    s32 count;
    memcpy(&count, at, 4);
    at += 4;
    for (s32 i = 0; i < count; i++) {
        s32 size;
        memcpy(&size, at, 4);
        at += 4;
        if (size == 0)
            continue;
        AiProfile* p = static_cast<AiProfile*>(MemAlloc(sizeof(AiProfile)));
        GuestList_Init(&p->situations);
        GuestList_Init(&p->responses);
        memcpy(p, at, 0x18);
        at += 0x18;
        s32 situations, responses;
        memcpy(&situations, at, 4);
        at += 4;
        for (s32 k = 0; k < situations; k++, at += 8) {
            s32 pair[2];
            memcpy(pair, at, 8);
            PushPair(&p->situations, pair[0], pair[1]);
        }
        memcpy(&responses, at, 4);
        at += 4;
        for (s32 k = 0; k < responses; k++, at += 8) {
            s32 pair[2];
            memcpy(pair, at, 8);
            PushPair(&p->responses, pair[0], pair[1]);
        }
        const u32 bytes = (u32)(situations * responses) * 4;
        void* chances = MemAlloc(bytes);
        memcpy(chances, at, bytes);
        p->chances = guest::addr(chances);
        at += bytes;
        GuestIntMap_Insert(&single->profiles, i, guest::addr(p));
    }
}
// ── 0x2A0620 / 0x2A0710 ───────────────────────────────────────────────────
static u32 ParseAttackTable(const u8* at) {
    AttackTable* t = static_cast<AttackTable*>(MemAlloc(sizeof(AttackTable)));
    auto block = [&](s32 words) {
        void* copy = MemAlloc((u32)words * 4);
        memcpy(copy, at, (size_t)words * 4);
        at += words * 4;
        return guest::addr(copy);
    };
    memcpy(&t->count, at, 4);
    at += 4;
    t->sequences = block(t->count);
    memcpy(&t->rows, at, 4);
    at += 4;
    t->rowIds = block(t->rows);
    t->chances = block(t->rows * t->count);
    return guest::addr(t);
}
static void ParseAttackTables(BattleSingle* single, const u8* at) {
    s32 size = 0;
    memcpy(&size, at + 4, 4);
    at += 8;
    if (size != 0) {
        single->attacks[0] = ParseAttackTable(at);
        at += size;
    }
    memcpy(&size, at, 4);
    if (size != 0)
        single->attacks[1] = ParseAttackTable(at + 4);
}
// ── 0x2A4260 ──────────────────────────────────────────────────────────────
static void ParseSequences(GuestIntMap* map, const s32* at) {
    const s32 count = *at++;
    for (s32 i = 0; i < count; i++) {
        const s32 length = *at++;
        if (length == 0)
            continue;
        AttackSequence* seq = static_cast<AttackSequence*>(MemAlloc(sizeof(AttackSequence) + (u32)length * 4));
        seq->count = length;
        seq->codes = guest::addr(seq + 1);
        memcpy(seq + 1, at, (size_t)length * 4);
        at += length;
        GuestIntMap_Insert(map, i, guest::addr(seq));
    }
}

// ── 0x29AA10 + 0x29A7D0 ───────────────────────────────────────────────────
// `file` is Common/BattleAI/totaldata as read: six blocks, each a size and
// its data — profiles, the two attack tables, the profiles by sword, the
// profiles by role, the attack sequences, the group tables.
extern "C" void BattleAi_Create(const u8* file) {
    BattleAi* ai = static_cast<BattleAi*>(MemAlloc(sizeof(BattleAi)));
    memset(ai, 0, sizeof(BattleAi));
    BattleSingle* single = static_cast<BattleSingle*>(MemAlloc(sizeof(BattleSingle)));
    memset(single, 0, sizeof(BattleSingle));
    GuestIntMap_Init(&single->profiles);
    GuestIntMap_Init(&single->byRole);
    GuestIntMap* sequences = static_cast<GuestIntMap*>(MemAlloc(sizeof(GuestIntMap)));
    GuestIntMap_Init(sequences);
    ai->single = guest::addr(single);
    ai->sequences = guest::addr(sequences);

    const u8* at = file;
    auto next = [&](const u8*& data) {
        s32 size;
        memcpy(&size, at, 4);
        data = at + 4;
        at += 4 + size;
    };
    const u8* block;
    next(block); ParseProfiles(single, block);
    next(block); ParseAttackTables(single, block);
    next(block);                                        // 0x2A0AF0: eight profile indices
    if (*reinterpret_cast<const s32*>(block) == 8) {
        void* bySword = MemAlloc(0x20);
        memcpy(bySword, block + 4, 0x20);
        single->bySword = guest::addr(bySword);
    }
    next(block);                                        // 0x2A0A50: {role, profile} pairs
    {
        const s32* pair = reinterpret_cast<const s32*>(block);
        for (s32 i = 0; i < pair[0]; i++)
            GuestIntMap_Insert(&single->byRole, pair[1 + i * 2], (u32)pair[2 + i * 2]);
    }
    next(block); ParseSequences(sequences, reinterpret_cast<const s32*>(block));
    ai->step = 2;
    *guest::ptr<u32>(0x2EE6A4u) = guest::addr(ai);
}
static inline BattleAi* Ai() { return P<BattleAi>(*guest::ptr<u32>(0x2EE6A4u)); }

// ── 0x2A0550 ──────────────────────────────────────────────────────────────
static AiProfile* ProfileFor(s32 role, s32 swordKind) {
    BattleSingle* single = P<BattleSingle>(Ai()->single);
    s32 index = -1;
    GuestMapNode* n = GuestIntMap_Find(&single->byRole, role);
    if (n != GuestIntMap_End(&single->byRole))
        index = (s32)n->value;
    else if (swordKind >= 0 && swordKind < 8 && single->bySword != 0)
        index = P<s32>(single->bySword)[swordKind];
    if (index == -1)
        return nullptr;
    n = GuestIntMap_Find(&single->profiles, index);
    return n != GuestIntMap_End(&single->profiles) ? P<AiProfile>(n->value) : nullptr;
}
// ── 0x29A430 / 0x29A500 ───────────────────────────────────────────────────
// A column of a row of chances that add up to 100.
static int PickColumn(const s32* chances, int row, int columns) {
    const int roll = Rand_Next() % 100;
    int sum = 0;
    for (int i = 0; i < columns; i++) {
        sum += chances[row * columns + i];
        if (roll < sum)
            return i;
    }
    return -1;
}
// ── 0x29A660 ──────────────────────────────────────────────────────────────
// What the opponent is doing, as the situation a profile answers.
static s32 Situation(const CharaParams* opponent) {
    if (!opponent)
        return -1;
    const u32 act = opponent->link[6] ? *P<u32>(opponent->link[6]) : 0;
    if (act & kActfCombo) {                             // attacking:
        if (act & 0x1000000) return (act & 0x4000000) ? 2 : 0;
        if (act & 0x2000000) return (act & 0x4000000) ? 3 : 1;
        return (act & 0x800) ? 0xD : 5;
    }
    if (act & 0x10)    return 10;
    if (act & 0x2000)  return 7;                        // guarding
    if (act & 0xF0000) return 8;                        // on the move
    if (act & 0x40000) return 9;
    if (act & 0x80)    return 0xC;
    const u32 flags = opponent->link[5] ? *P<u32>(opponent->link[5]) : 0;
    if (flags & kBodyInAir)
        return 4;
    if (flags & kBodyIdle) {
        if (flags & (kBodyFaceUp | kBodyFaceDown)) return 0xB;
        if (flags & kBodyStanding) return 6;
    }
    return -1;
}

// ═══ The move (0xB0 bytes) ════════════════════════════════════════════════

struct AiFight {
    AiMove base;            // +0x00  (kind doubles as the patience counter)
    s32   wait;             // +0x08  frames the current response still runs
    s32   ordered;          // +0x0C  a task told it how to open
    s32   response;         // +0x10
    s32   closing;          // +0x14  1 backing off, 2 closing in
    s32   turning;          // +0x18
    s32   step;             // +0x1C  how far into an attack sequence
    u32   profile;          // +0x20  AiProfile*
    u32   sequence;         // +0x24  AttackSequence*
    u8    _28[8];
    float lastPlace[4];     // +0x30
    s32   stalled;          // +0x40
    s32   openFrames;       // +0x44
    float openDistance;     // +0x48
    s32   openWith;         // +0x4C
    s32   seenAction[10];   // +0x50  the opponent's last blows
    u32   seenLean[10];     // +0x78
    s32   seenClock;        // +0xA0
    s32   seenCount;        // +0xA4
    s32   seenAt;           // +0xA8
    u32   _ac;
};
static_assert(sizeof(AiFight) == 0xB0 && offsetof(AiFight, seenClock) == 0xA0, "AiFight");
enum : u32 { kVtFight = 0x2EB250u, kVtMoveBase = 0x2EAD40u };

static inline CharaParams* Params(CharaBody* b) { return P<CharaParams>(b->params); }
static inline CharaParams* Opponent(CharaBody* b) { return Params(b)->link[3] ? P<CharaParams>(Params(b)->link[3]) : nullptr; }
// A direction relative to facing the opponent: `z` 1 towards, −1 away.
static void Facing(CharaBody* body, float z, float* out) {
    alignas(16) float m[16];
    Matrix_Copy(m, Params(body)->matrix);
    Matrix_FaceTowards(m, &Opponent(body)->matrix[12]);
    for (int j = 0; j < 4; j++)
        out[j] = z * m[8 + j];
}

// ── 0x18D480 ──────────────────────────────────────────────────────────────
// Step towards the distance it likes; → 0 once there (and it stands).
static int KeepDistance(float wanted, AiFight* self, CharaBody* body) {
    const float distance = Query_Distance(Params(body)->id, Opponent(body)->id);
    const float off = fabsf(wanted - distance);
    float dir[4];
    if (off > 0.25f) {
        self->closing = distance < wanted ? 1 : 2;
    } else {
        if (self->closing == 0 || off < 0.05f) {
            self->closing = 0;
            CharaBody_Idle(body);
            return 0;
        }
    }
    Facing(body, self->closing == 1 ? -1.0f : 1.0f, dir);
    CharaBody_Dodge(body, dir);
    return 1;
}
// ── 0x18D230 ──────────────────────────────────────────────────────────────
// Press forward; → 1 when it has not moved for three frames (it is there).
static int PressIn(AiFight* self, CharaBody* body) {
    float dir[4];
    Facing(body, 1.0f, dir);
    CharaBody_Dodge(body, dir);
    const float* pos = &Params(body)->matrix[12];
    if (pos[0] == self->lastPlace[0] && pos[2] == self->lastPlace[2] && ++self->stalled > 2) {
        memset(self->lastPlace, 0, 16);
        self->stalled = 0;
        return 1;
    }
    memcpy(self->lastPlace, pos, 16);
    return 0;
}
// ── 0x18D350 ──────────────────────────────────────────────────────────────
// Not facing the opponent: step back until it is.
static int TurnToFace(AiFight* self, CharaBody* body) {
    CharaParams* opponent = Opponent(body);
    if (!opponent)
        return 0;
    const float facing = Matrix_FacingCosine(Params(body)->matrix, &opponent->matrix[12]);
    float dir[4];
    Facing(body, -1.0f, dir);
    if (facing < 0.5f) {
        CharaBody_GuardEnd(body);
        CharaBody_Dodge(body, dir);
        self->turning = 1;
        return 1;
    }
    if (self->turning == 1) {
        if (facing <= 0.875f) {
            CharaBody_Dodge(body, dir);
            return 1;
        }
        self->turning = 0;
    }
    return 0;
}

// ── 0x18D640 ──────────────────────────────────────────────────────────────
// The defensive responses, kept up each frame they last.
static void Respond(AiFight* self, CharaBody* body) {
    static const s32 kParryFrames[3] = {8, 4, 2};           // 0x2D8220, by difficulty
    const CharaParams* p = Params(body);
    const u32 act = p->link[6] ? *P<u32>(p->link[6]) : 0;
    auto guard = [&](bool answer) {
        // In the hit stop of a blow on its guard it leans against a high or
        // low blow: a parry, timed tighter the harder the game.
        if (act & kActfFrozen) {
            body->inputCount = 0;
            const CharaParams* o = Opponent(body);
            if ((o->link[6] ? *P<u32>(o->link[6]) : 0) & 0x3000000)
                CharaBody_PushInput(body, 0x3000000, kParryFrames[Difficulty()]);
        }
        if (answer && (act & 0x20))
            CharaBody_fn1d2160(body, 0);
        CharaBody_Guard(body, 0);
    };
    switch (self->response) {
    case 4: CharaBody_Idle(body); break;
    case 5: body->inputCount = 0; CharaBody_Guard(body, 0); break;
    case 6: guard(false); break;
    case 7: guard(true); break;
    case 9:
    case 10: {
        // A step to one side or the other.
        float f[4];
        Facing(body, 1.0f, f);
        const float down[3] = {0.0f, -1.0f, 0.0f};
        float side[4] = {0, 0, 0, 0};
        const float* a = self->response == 9 ? f : down;
        const float* b = self->response == 9 ? down : f;
        side[0] = a[1] * b[2] - a[2] * b[1];
        side[1] = a[2] * b[0] - a[0] * b[2];
        side[2] = a[0] * b[1] - a[1] * b[0];
        CharaBody_Dodge(body, side);
        break;
    }
    case 0x10: KeepDistance(P<AiProfile>(self->profile)->distance, self, body); break;
    case 0xB:
        if (CharaBody_fn1d2110(body) == -1 && PressIn(self, body)) {
            CharaBody_Attack(body, 0x60);
            self->wait = 0;
        }
        break;
    }
}

// ── 0x18D9B0 ──────────────────────────────────────────────────────────────
// The attacking responses, begun once.
static void Act(AiFight* self, CharaBody* body) {
    static const u8 kGrab[3] = {0x50, 0x51, 0x52};          // 0x2EDB10
    const s32 r = self->response;
    if ((u32)(r - 5) > 2)
        CharaBody_GuardEnd(body);
    switch (r) {
    case 0:
    case 1: {
        // An attack sequence of its own, by the table's odds.
        const AiProfile* profile = P<AiProfile>(self->profile);
        const u32 at = P<BattleSingle>(Ai()->single)->attacks[r];
        if (at == 0)
            break;
        const AttackTable* table = P<AttackTable>(at);
        const s32 mine = r == 0 ? profile->attackA : profile->attackB;
        int row = -1;
        for (s32 i = 0; i < table->rows; i++)
            if (P<s32>(table->rowIds)[i] == mine) {
                row = i;
                break;
            }
        if (row == -1)
            break;
        const int column = PickColumn(P<s32>(table->chances), row, table->count);
        GuestIntMap* sequences = P<GuestIntMap>(Ai()->sequences);
        GuestMapNode* n = column >= 0 ? GuestIntMap_Find(sequences, P<s32>(table->sequences)[column])
                                      : GuestIntMap_End(sequences);
        self->sequence = n != GuestIntMap_End(sequences) ? n->value : 0;
        if (self->sequence != 0)
            CharaBody_Attack(body, (u8)P<s32>(P<AttackSequence>(self->sequence)->codes)[self->step++]);
        break;
    }
    case 2: CharaBody_Attack(body, 0x13); break;
    case 3: CharaBody_Attack(body, 0x23); break;
    case 8: CharaBody_Special(body, Rand_Next() & 3); break;
    case 0xC: CharaBody_Attack(body, 0x60); break;
    case 0xD: CharaBody_Attack(body, kGrab[Rand_Next() % 3]); break;
    }
}

// ── 0x18DE50 ──────────────────────────────────────────────────────────────
// On Easy it usually just stands for a second instead; on Normal sometimes,
// and half the time it makes the plainer version of an attack or guard.
static void Blunt(AiFight* self, int difficulty) {
    const s32 chosen = self->response;
    bool plainer = false;
    if (difficulty == 0) {
        if (Rand_Next() % 10 < 2) {
            self->response = 4;
            self->wait = *gFrameRate;
        } else {
            plainer = true;
        }
    } else if (difficulty == 1) {
        const s32 r = self->response;
        if (Rand_Next() % 10 < 2 && (r == 0xD || r == 0xC || r == 3 || r == 2 || r == 1 || r == 0)) {
            self->response = 4;
            self->wait = *gFrameRate;
        }
        plainer = (Rand_Next() & 1) != 0;
    } else {
        return;
    }
    if (!plainer)
        return;
    if (chosen == 2)      self->response = 0;
    else if (chosen == 3) self->response = 1;
    else if (chosen == 7) self->response = 6;
}

// ── 0x18DC70 ──────────────────────────────────────────────────────────────
static void Choose(AiFight* self, CharaBody* body, int index) {
    const PairNode* r = PairAt(&P<AiProfile>(self->profile)->responses, index);
    if (!r)
        return;
    self->response = r->a;
    self->wait = r->b;
    Blunt(self, Difficulty());
    Act(self, body);
    Respond(self, body);
}

// ── 0x18DD30 ──────────────────────────────────────────────────────────────
// On the ground: get up — striking from there if the opponent is close.
static int GetUp(AiFight* self, CharaBody* body) {
    const CharaParams* p = Params(body);
    const u32 flags = p->link[5] ? *P<u32>(p->link[5]) : 0;
    if ((flags & (kBodyFaceUp | kBodyFaceDown)) == 0 || (flags & kBodyIdle) == 0)
        return 0;
    const CharaParams* o = Opponent(body);
    if ((body->flags & kBodyArmed) && o) {
        const u32 theirs = o->link[6] ? *P<u32>(o->link[6]) : 0;
        if ((theirs & 0x2000) == 0 && Vec_Distance(&p->matrix[12], &o->matrix[12]) < 1.5f && (Rand_Next() & 1)) {
            CharaBody_fn1d1960(body);
            return 1;
        }
    }
    CharaBody_RollUp(body);
    if (Rand_Next() & 1) {
        self->response = 5;
        self->wait = 0x1E;
    }
    return 1;
}

// ── 0x18DFD0 ──────────────────────────────────────────────────────────────
static void Think(AiFight* self, CharaBody* body) {
    if (self->step != 0) {
        // In the middle of an attack sequence: its next blow, when the combo
        // tree has one and the moment comes.
        const s32 next = CharaBody_ComboNext(body, (u8)P<s32>(P<AttackSequence>(self->sequence)->codes)[self->step]);
        if (next == -1) {
            if (body->comboNode == 0 || P<s32>(body->comboNode)[3] == 0) {
                self->sequence = 0;
                self->step = 0;
            }
        } else if (CharaBody_TryLink(body, next)) {
            self->step++;
        }
        return;
    }
    if (self->wait != 0) {
        self->wait--;
        Respond(self, body);
        return;
    }
    CharaParams* opponent = Opponent(body);
    if (Query_Distance(Params(body)->id, opponent->id) > 4.0f) {
        CharaBody_GuardEnd(body);
        float dir[4];
        Facing(body, 1.0f, dir);
        CharaBody_Dodge(body, dir);
        return;
    }
    if (TurnToFace(self, body) || self->profile == 0)
        return;
    const AiProfile* profile = P<AiProfile>(self->profile);
    const s32 situation = Situation(opponent);
    int index = 0;
    const PairNode* found = nullptr;
    const PairNode* end = reinterpret_cast<const PairNode*>(&profile->situations.endPrev);
    for (const PairNode* n = P<PairNode>(profile->situations.endNext); n != end; n = P<PairNode>(n->next), index++)
        if (n->a == situation) {
            found = n;
            break;
        }
    if (found) {
        if (self->base.kind >= found->b) {              // it has watched long enough
            Choose(self, body, PickColumn(P<s32>(profile->chances), index, (int)profile->responses.size));
            return;
        }
        self->base.kind++;
    }
    KeepDistance(profile->distance, self, body);
}

// ── 0x18E260 (vtable +0x14) ───────────────────────────────────────────────
static int Fight_Result(AiFight* self, CharaBody* body) {
    if (Opponent(body) == nullptr || self->profile == 0)
        return 0;
    if ((body->flags & kBodyArmed) == 0) {
        const CharaParams* p = Params(body);
        const u32 flags = p->link[5] ? *P<u32>(p->link[5]) : 0;
        if ((flags & (kBodyFaceUp | kBodyFaceDown)) && (flags & kBodyIdle))
            CharaBody_RollUp(body);
        else
            CharaBody_fn1d1ce0(body);                   // draw
        return 0;
    }
    if (self->ordered == 0) {
        if (!GetUp(self, body))
            Think(self, body);
    } else if (self->openFrames != 0) {
        KeepDistance(self->openDistance, self, body);
        self->openFrames--;
    } else if (self->openWith == -1) {
        self->openWith = Rand_Next() & 1;
        CharaBody_fn1d1b20(body, self->openWith);
    } else if ((body->state & 0x20000) == 0) {
        CharaBody_Attack(body, self->openWith == 0 ? 0x10 : 0x20);
        self->ordered = 0;
    }
    return 0;
}

// ── 0x18E420 (vtable +0x10) ───────────────────────────────────────────────
static int Fight_Step(AiFight* self, CharaBody* body) {
    if (!body)
        return 0;
    CharaParams* p = Params(body);
    if (self->profile == 0) {
        self->profile = guest::addr(ProfileFor(p->role, body->swordValue));
        if (self->profile == 0)
            return 0;                                   // (the original complains)
    }
    // Whoever hit it last is its opponent, if it has none.
    if (p->link[3] == 0) {
        const int attacker = Collision_AttackerOf(p->id);
        if (attacker != -1)
            p->link[3] = guest::addr(CharaResources_GetParams(CharaResources_Get(), attacker));
        return 0;
    }
    // Remember the opponent's blows, each once.
    const CharaParams* o = P<CharaParams>(p->link[3]);
    const u32 act = o->link[6] ? *P<u32>(o->link[6]) : 0;
    if ((act & kActfCombo) == 0)
        return 0;
    const s32 clock = o->link[7] ? *P<s32>(o->link[7]) : 0;
    if (self->seenClock == clock)
        return 0;
    const int length = P<AiProfile>(self->profile)->memory + 1;
    if (self->seenCount < length)
        self->seenCount++;
    self->seenClock = clock;
    self->seenAction[self->seenAt] = o->link[0] ? *P<s32>(o->link[0]) : -1;
    self->seenLean[self->seenAt] = act & 0x3000000;
    self->seenAt = (self->seenAt + 1) % length;
    return 0;
}

static int Fight_Start(AiFight*) { return 0; }                              // 0x18E5C0
static int Fight_End(AiFight*, CharaBody* body) {                           // 0x18E230
    if (body)
        CharaBody_GuardEnd(body);
    return 0;
}
static AiMove* Fight_Dtor(AiFight* self, s16 flag) {                        // 0x18E5D0
    if (self) {
        self->base.vtable = kVtMoveBase;
        if (flag > 0)
            MemFree(self);
    }
    return &self->base;
}
// ── 0x18E630 ──────────────────────────────────────────────────────────────
// `order` (from a task): {frames to hold a distance first, the distance}.
extern "C" u32 AiFight_Create(const void* order) {
    AiFight* self = static_cast<AiFight*>(MemAlloc(sizeof(AiFight)));
    memset(self, 0, sizeof(AiFight));
    self->base.vtable = kVtFight;
    self->response = -1;
    self->openWith = -1;
    for (int i = 0; i < 10; i++)
        self->seenAction[i] = -1;
    self->seenClock = -1;
    if (order) {
        memcpy(&self->openFrames, order, 4);
        memcpy(&self->openDistance, static_cast<const u8*>(order) + 4, 4);
        self->ordered = 1;
    }
    return guest::addr(self);
}

// ═══ Waiting for a turn (AI move 9; 0x269C50–0x26A150, vtable 0x2ECCE0) ═══
// Someone whose opponent is busy with another keeps to the ring round the
// two: just outside the distance they fight at, backing off when too near,
// closing when more than 3 further, giving way to whoever stands beside it
// and otherwise idling, shuffling sideways or making a feint.
struct AiWait {
    u32 vtable;
    s32 timer;              // +0x04  frames the present step lasts
    s32 step;               // +0x08  0 idle, 1 / 2 to a side, 3 forward, 4 back
    s32 _0c;
};
// 0x1750D0: is anyone (active, not `id`) within `reach` in the direction
// `dir` (cosine over `spread`)?  → the nearest's distance, −1 nobody.
static float CrowdAhead(float reach, float spread, int id, const float* dir) {
    CharaMgr* mgr = P<CharaMgr>(*P<u32>(0x2EE5C4u));
    Chara* self = nullptr;
    for (GuestListNode* n = GuestList_Begin(&mgr->charas); n != GuestList_End(&mgr->charas); n = GuestList_Next(n))
        if (P<Chara>(n->value)->id == id)
            self = P<Chara>(n->value);
    if (self == nullptr)
        return -1.0f;
    const float* at = &P<CharaParams>(self->params)->matrix[12];
    float nearest = reach * reach;
    for (GuestListNode* n = GuestList_Begin(&mgr->charas); n != GuestList_End(&mgr->charas); n = GuestList_Next(n)) {
        const Chara* other = P<Chara>(n->value);
        if (other == self || other->active == 0)
            continue;
        const float* there = &P<CharaParams>(other->params)->matrix[12];
        float to[3] = {there[0] - at[0], there[1] - at[1], there[2] - at[2]};
        const float far = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (far > 0.0f)
            for (float& c : to)
                c /= far;
        // (as the original: the distance itself is compared with the square of the reach)
        if (far < nearest && dir[0] * to[0] + dir[1] * to[1] + dir[2] * to[2] > spread)
            nearest = far;
    }
    return nearest != reach * reach ? nearest : -1.0f;
}
static inline void Cross(float* o, const float* a, const float* b) {
    const float r[4] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0.0f};
    memcpy(o, r, sizeof r);
}
static int Wait_Start(AiWait* self) {                           // 0x26A0B0
    self->timer = 0x1E;
    self->step = 0;
    self->_0c = (Rand_Next() & 3) + 5;
    return 0;
}
// 0x26A030: with no opponent, whoever struck it last becomes one.
static int Wait_Turn(AiWait*, CharaBody* body) {
    if (body != nullptr) {
        CharaParams* p = Params(body);
        if (p->link[3] == 0) {
            const int attacker = Collision_AttackerOf(p->id);
            if (attacker != -1)
                p->link[3] = guest::addr(CharaResources_GetParams(CharaResources_Get(), attacker));
        }
    }
    return 0;
}
static int Wait_Step(AiWait* self, CharaBody* body) {           // 0x269C50
    CharaParams* p = Params(body);
    const CharaParams* foe = Opponent(body);
    if (foe == nullptr)
        return 0;
    if ((body->flags & kBodyArmed) == 0) {
        CharaBody_fn1d1ce0(body);                               // draw
        return 0;
    }
    if (foe->link[3] == 0)
        return 0;
    const CharaParams* theirs = P<CharaParams>(foe->link[3]);
    const float mine = Query_Distance(p->id, foe->id), ring = Query_Distance(foe->id, theirs->id) + 0.5625f;
    alignas(16) float m[16];
    Matrix_Copy(m, p->matrix);
    Matrix_FaceTowards(m, &foe->matrix[12]);
    const float forward[4] = {m[8], m[9], m[10], m[11]};
    const float back[4] = {-m[8], -m[9], -m[10], -m[11]};
    static const float kUp[4] = {0, 1, 0, 0}, kDown[4] = {0, -1, 0, 0};
    float left[4], right[4];
    Cross(left, kUp, back);
    Cross(right, kDown, back);
    if (self->timer == 0) {
        if (mine <= ring) {
            self->step = 4;
            self->timer = 7;
        } else if (3.0f < mine - ring) {
            self->step = 3;
            self->timer = 7;
        } else if (CrowdAhead(1.0f, 0.75f, p->id, left) != -1.0f) {
            self->step = 4;
            self->timer = 0xF;
        } else if (CrowdAhead(1.0f, 0.75f, p->id, right) != -1.0f) {
            self->step = 3;
            self->timer = 0xF;
        } else if ((Rand_Next() & 4) == 0) {
            self->step = 0;
            self->timer = 0xF;
        } else if ((Rand_Next() & 3) == 0) {
            static const s32 kFeint[2] = {2, 3};                // 0x2EE380
            CharaBody_Special(body, kFeint[Rand_Next() & 1]);
        } else {
            static const s32 kSide[2] = {1, 2};                 // 0x2EE388
            self->step = kSide[Rand_Next() & 1];
            self->timer = 0x1E;
        }
        return 0;
    }
    switch (self->step) {
    case 0: CharaBody_Idle(body); break;
    case 1: CharaBody_Dodge(body, left); break;
    case 2: CharaBody_Dodge(body, right); break;
    case 3: CharaBody_Dodge(body, forward); break;
    case 4: CharaBody_Dodge(body, back); break;
    }
    self->timer--;
    return 0;
}
static int Wait_Zero(void*, void*) { return 0; }                // 0x269C40, 0x274380, 0x2742E0
static void* Wait_Dtor(AiWait* self, s16 flag) {                // 0x26A0F0, 0x2743B0
    if (self != nullptr) {
        self->vtable = kVtMoveBase;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
extern "C" u32 AiWait_Create() {                                // 0x26A150
    AiWait* self = static_cast<AiWait*>(MemAlloc(sizeof(AiWait)));
    memset(self, 0, sizeof *self);
    self->vtable = 0x2ECCE0u;
    Wait_Start(self);
    return guest::addr(self);
}

// ═══ Holding off (AI move 10; 0x274020–0x274410, vtable 0x2ECD90) ═════════
// Keeps 2.5 from its opponent, facing it, and steps aside for anyone who
// stands between them.
struct AiHold {
    u32 vtable;
    s32 _04, _08;
    s32 closing;            // +0x0C  1 backing off, 2 closing in
};
static int Hold_Start(AiHold* self) {                           // 0x274390
    self->_04 = self->_08 = self->closing = 0;
    return 0;
}
static void Hold_Keep(AiHold* self, CharaBody* body) {          // 0x274020
    CharaParams* p = Params(body);
    const CharaParams* foe = Opponent(body);
    const float distance = Query_Distance(p->id, foe->id);
    alignas(16) float m[16];
    Matrix_Copy(m, p->matrix);
    Matrix_FaceTowards(m, &foe->matrix[12]);
    const float forward[4] = {m[8], m[9], m[10], m[11]};
    const float back[4] = {-m[8], -m[9], -m[10], -m[11]};
    const float off = fabsf(2.5f - distance);
    if (0.25f < off) {
        self->closing = distance < 2.5f ? 1 : 2;
        CharaBody_Dodge(body, self->closing == 1 ? back : forward);
    } else if (self->closing == 0) {
        if (CrowdAhead(1.0f, -0.5f, p->id, forward) == -1.0f) {
            self->_04 = 0;
            CharaBody_GuardEnd(body);
            alignas(16) float turned[16];
            Matrix_Copy(turned, p->matrix);
            Matrix_TurnTowards(10.0f, turned, &foe->matrix[12]);
            Matrix_Copy(p->matrix, turned);
            CharaBody_Idle(body);
        } else {
            static const float kDown[4] = {0, -1, 0, 0};        // 0x2DB110
            float aside[4];
            Cross(aside, forward, kDown);
            CharaBody_Dodge(body, aside);
        }
    } else if (off < 0.05f) {
        self->closing = 0;
    } else {
        CharaBody_Dodge(body, self->closing == 1 ? back : forward);
    }
}
static int Hold_Step(AiHold* self, CharaBody* body) {           // 0x2742F0
    s32* pending = reinterpret_cast<s32*>(reinterpret_cast<u8*>(body) + 0x34);     // an action a task asked for
    if (pending[0] == 0) {
        if (Opponent(body) != nullptr) {
            if ((body->flags & kBodyArmed) == 0)
                CharaBody_fn1d1ce0(body);
            else
                Hold_Keep(self, body);
        }
    } else {
        pending[0] = 0;
        if (CharaBody_Try(body, pending[1]) == 0)
            pending[2] = 1;
    }
    return 0;
}
extern "C" u32 AiHold_Create() {                                // 0x274410
    AiHold* self = static_cast<AiHold*>(MemAlloc(sizeof(AiHold)));
    memset(self, 0, sizeof *self);
    self->vtable = 0x2ECD90u;
    return guest::addr(self);
}

static const struct FightBinder {
    FightBinder() {
        guest::bind(0x18E5D0, Fight_Dtor, "Fight_Dtor");
        guest::bind(0x18E5C0, Fight_Start, "Fight_Start");
        guest::bind(0x18E420, Fight_Step, "Fight_Step");
        guest::bind(0x18E260, Fight_Result, "Fight_Result");
        guest::bind(0x18E230, Fight_End, "Fight_End");
        guest::bind(0x26A0F0, Wait_Dtor, "AiWait_Dtor");
        guest::bind(0x26A0B0, Wait_Start, "AiWait_Start");
        guest::bind(0x26A030, Wait_Turn, "AiWait_Turn");
        guest::bind(0x269C50, Wait_Step, "AiWait_Step");
        guest::bind(0x269C40, Wait_Zero, "AiWait_Zero");
        guest::bind(0x2743B0, Wait_Dtor, "AiHold_Dtor");
        guest::bind(0x274390, Hold_Start, "AiHold_Start");
        guest::bind(0x274380, Wait_Zero, "AiHold_Zero");
        guest::bind(0x2742F0, Hold_Step, "AiHold_Step");
        guest::bind(0x2742E0, Wait_Zero, "AiHold_Zero");
    }
} sFightBinder;
