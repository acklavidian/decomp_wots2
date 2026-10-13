// Swords lying in the world (the sword database's list at +0x98; the class
// at 0x16A480–0x16ACB0 and the database's methods 0x15C180–0x15C780).
// Whoever dies with a sword in hand drops it (CharaMgr → SwordDb_fn15c330),
// scripts lay some out (SwordDb_DropAt), and the player picks them up with
// the same button as anything else (the carrier's functions in
// world_objects.cpp ask SwordDb_Nearest and call DroppedSword_Take…).
//
// A dropped sword (0x60 bytes) owns a copy of the sword.  It is thrown up
// (0.4 a frame, less 0.01 and a fifth of its speed each frame), tumbles end
// over end, and lies flat where it lands (SE 0x1D); picked up it hops again
// and is gone once it comes back down (SE 0x3E) — or just hops, if the
// player's hands are full.  Swords stay with their map: leaving an interior
// keeps the ones outside (+0x5C on this map, +0x5E which map).
//
// Not done: the frustum test before drawing (0x12F590 — left to the GPU).

#include "engine/bgm.h"
#include "engine/containers.h"
#include "game/map_collision.h"
#include "game/player_data.h"
#include "game/sword.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    void  Matrix_Rotate(void* dst, const void* src, float angle);       // 0x107618  about y
    void  Matrix_Multiply(void* dst, const void* a, const void* b);     // 0x107130  b first
    u32   SwordDb_fn15d890(void* db, u32 style, int forPlayer);         // 0x15D890  the style's model
    int   SwordModel_DrawHand(void* model, int part, const float* matrix, int own);    // 0x15A610
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }
const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};

struct DroppedSword {
    u32 model;              // +0x00  the style's SwordModel
    u32 sword;              // +0x04  Sword*, its own
    u32 _08[2];
    float matrix[16];       // +0x10
    float rise;             // +0x50  upward speed
    float fromY;            // +0x54  the height it was dropped at
    s16 broken;             // +0x58  1: the broken sword's parts
    s16 state;              // +0x5A  1 in the air, 0 lying, 2 picked up (in the air), 3 gone
    s16 here;               // +0x5C  it is on the map the player is on
    s16 map;                // +0x5E
};
static_assert(sizeof(DroppedSword) == 0x60 && offsetof(DroppedSword, rise) == 0x50, "DroppedSword");

const float kIdentity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};

inline GuestList* ListOf(void* db) { return &static_cast<SwordDb*>(db)->_98; }
inline bool ModelReady(const DroppedSword* d) {
    const u32* m = P<u32>(d->model);
    return d->model != 0 && (m[3] & m[4]) != 0;
}
// 0x15A7B0 / 0x15A7E0: how thick one of the model's four blade boxes is, and where it sits.
float BoxHeight(u32 model, int which) {
    const u32 box = P<u32>(model + 0x44)[which];
    return box != 0 ? *P<float>(box + 0x54) : 0.0f;
}
const float* BoxOffset(u32 model, int which) {
    static const float kNone[4] = {0, 0, 0, 0};
    const u32 box = P<u32>(model + 0x44)[which];
    return box != 0 ? P<float>(box + 0x40) : kNone;
}
// sceVu0RotMatrixX (0x107570): `src` turned about its own x axis first.
void RotateX(float* dst, const float* src, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    alignas(16) const float r[16] = {1, 0, 0, 0,  0, c, s, 0,  0, -s, c, 0,  0, 0, 0, 1};
    alignas(16) float out[16];
    Matrix_Multiply(out, r, src);
    memcpy(dst, out, sizeof out);
}
void Cross(float* o, const float* a, const float* b) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z; o[3] = 0.0f;
}
void Normalize(float* v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n != 0.0f)
        for (int i = 0; i < 3; i++)
            v[i] /= n;
}

// ── 0x16ABF0 / 0x16A820 ───────────────────────────────────────────────────
DroppedSword* Dropped_New(u32 model, const Sword* sword, const float* matrix) {
    DroppedSword* d = static_cast<DroppedSword*>(MemAlloc(sizeof(DroppedSword)));
    if (d == nullptr)
        return nullptr;
    memset(d, 0, sizeof *d);
    memcpy(d->matrix, matrix, sizeof d->matrix);
    d->model = model;
    Sword* own = static_cast<Sword*>(MemAlloc(sizeof(Sword)));
    Sword_Ctor(own);
    memcpy(own, sword, sizeof(Sword));
    d->sword = guest::addr(own);
    d->rise = 0.4f;
    d->state = 1;
    d->fromY = d->matrix[13];
    d->broken = own->durability == 0;
    d->matrix[13] += BoxHeight(model, d->broken * 2 + 1);
    d->here = 1;
    return d;
}
// ── 0x16AB90 ──────────────────────────────────────────────────────────────
void Dropped_Free(DroppedSword* d) {
    if (d->sword != 0) {
        Sword_Dtor(P<Sword>(d->sword), 0);
        MemFree(P(d->sword));
    }
    d->sword = 0;
    MemFree(d);
}

// ── 0x16A700: has it come down? ───────────────────────────────────────────
void Dropped_Land(DroppedSword* d, MapCollision* map) {
    if (d->rise > 0.0f)
        return;
    alignas(16) float m[16];
    memcpy(m, d->matrix, sizeof m);
    const ColCellType* cell = Map_CellAt(map, &m[12]);
    const float ground = (cell != nullptr ? cell->height : 0.0f) + BoxHeight(d->model, d->broken * 2 + 1);
    if (m[13] >= ground)
        return;
    // Flat on the ground, pointing the way it was.
    const float up[4] = {0.0f, 1.0f, 0.0f, 0.0f};          // 0x2D2020
    memcpy(&m[4], up, 16);
    Cross(&m[8], &m[4], &m[0]);
    Normalize(&m[8]);
    Cross(&m[0], &m[8], &m[4]);
    Normalize(&m[0]);
    m[13] = ground;
    memcpy(d->matrix, m, sizeof m);
    Sound_PlaySE(0x1D, (int)guest::addr(&d->matrix[12]), 0x3FFF);
    d->state = 0;
}

// ── 0x16A9B0 ──────────────────────────────────────────────────────────────
void Dropped_Update(DroppedSword* d, MapCollision* map) {
    if (!ModelReady(d) || d->here != 1 || (d->state != 1 && d->state != 2))
        return;
    alignas(16) float turn[16], m[16];
    RotateX(turn, kIdentity, 1.0f);
    Matrix_Multiply(m, d->matrix, turn);
    d->rise += -std::fabs(d->rise * 0.2f) - 0.01f;
    m[13] += d->rise;
    memcpy(d->matrix, m, sizeof m);
    if (d->state == 1) {
        Dropped_Land(d, map);
    } else if (d->rise < 0.0f && d->matrix[13] <= d->fromY + 0.8f) {
        Sound_PlaySE(0x3E, 0, 0x3FFF);
        d->state = 3;
    }
}

// ── 0x16A540: its two parts, lying on their side ──────────────────────────
void Dropped_Draw(DroppedSword* d) {
    for (int half = 0; half < 2; half++) {
        const int part = d->broken * 2 + half;
        alignas(16) float at[16], turned[16], world[16];
        memcpy(at, kIdentity, sizeof at);
        const float* offset = BoxOffset(d->model, part);
        for (int i = 0; i < 3; i++)
            at[12 + i] += offset[i];
        at[15] = 1.0f;
        RotateX(turned, at, -2.0f);
        if (half == 1)
            Matrix_Rotate(turned, turned, 3.1415927f);
        Matrix_Multiply(world, d->matrix, turned);
        SwordModel_DrawHand(P(d->model), part, world, 0);
    }
}

}  // namespace

extern "C" {

// ── 0x15C180: the sword within reach in front of someone (as for objects:
// from 0.3 behind them, the nearest within √2.2 that is ahead) ─────────────
u32 SwordDb_Nearest(void* db, const float* matrix) {
    float from[3], forward[3];
    for (int i = 0; i < 3; i++) {
        forward[i] = matrix[8 + i];                         // (0, 0, 1, 0) through the matrix
        from[i] = matrix[12 + i] - forward[i] * 0.3f;       // (0, 0, −0.3, 1)
    }
    float nearest = 2.2f;
    u32 found = 0;
    GuestList* list = ListOf(db);
    for (GuestListNode* n = GuestList_Begin(list); n != GuestList_End(list); n = GuestList_Next(n)) {
        const DroppedSword* d = P<DroppedSword>(n->value);
        if (d->here == 0)
            continue;
        float along = 0.0f, square = 0.0f;
        for (int i = 0; i < 3; i++) {
            const float diff = from[i] - d->matrix[12 + i];
            along += diff * forward[i];
            square += diff * diff;
        }
        if (along < 0.0f && square < nearest) {
            nearest = square;
            found = n->value;
        }
    }
    return found;
}

// ── 0x15C330: dropped by whoever held it (its model is known) ─────────────
void SwordDb_fn15c330(void* db, u32 model, u32 sword, const void* matrix) {
    if (model == 0 || sword == 0)
        return;
    if (DroppedSword* d = Dropped_New(model, P<Sword>(sword), static_cast<const float*>(matrix))) {
        GuestList_Insert(ListOf(db), GuestList_End(ListOf(db)), guest::addr(d));
        if (getenv("WOTS2_TRACE_DROPS")) {
            const float* m = static_cast<const float*>(matrix);
            printf("[swords] one dropped at %.2f %.2f %.2f\n", m[12], m[13], m[14]);
        }
    }
}
// ── 0x15C290: a sword out of the player's hands ───────────────────────────
void SwordDb_DropSword(void* db, int swordAt, const void* matrix) {
    const Sword* sword = P<Sword>((u32)swordAt);
    const u32 style = *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(sword) + 0x68);
    if (DroppedSword* d = Dropped_New(SwordDb_fn15d890(db, style, 0), sword, static_cast<const float*>(matrix)))
        GuestList_Insert(ListOf(db), GuestList_End(ListOf(db)), guest::addr(d));
}
// ── 0x15C3C0: a new sword of a type, at a place ───────────────────────────
void SwordDb_DropAt(void* db, int type, const float* pos) {
    alignas(16) Sword sword;
    Sword_Ctor(&sword);
    if (SwordDb_MakeSword(static_cast<SwordDb*>(db), type, &sword) == 0)
        return;
    const u32 style = *reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(&sword) + 0x68);
    DroppedSword* d = Dropped_New(SwordDb_fn15d890(db, style, 0), &sword, kIdentity);
    if (d == nullptr)
        return;
    memcpy(&d->matrix[12], pos, 16);
    GuestList_Insert(ListOf(db), GuestList_End(ListOf(db)), guest::addr(d));
}

// ── 0x15C560 (→ 0x16A500): the player goes from one map to another ────────
void SwordDb_ChangeMap(void* db, int map, int other) {
    GuestList* list = ListOf(db);
    for (GuestListNode* n = GuestList_Begin(list); n != GuestList_End(list); n = GuestList_Next(n)) {
        DroppedSword* d = P<DroppedSword>(n->value);
        if (!ModelReady(d))
            continue;
        if (d->here == 1)
            d->map = (s16)other;
        d->here = (s16)map == d->map;
    }
}

// ── 0x15C5F0 ──────────────────────────────────────────────────────────────
void SwordDb_Draw(void* db) {
    GuestList* list = ListOf(db);
    for (GuestListNode* n = GuestList_Begin(list); n != GuestList_End(list); n = GuestList_Next(n)) {
        DroppedSword* d = P<DroppedSword>(n->value);
        if (ModelReady(d) && d->here != 0)
            Dropped_Draw(d);
    }
}

// ── 0x15C690: once a frame; `map` is the collision map ────────────────────
void SwordDb_Update(void* db, u32 map) {
    GuestList* list = ListOf(db);
    for (GuestListNode* n = GuestList_Begin(list); n != GuestList_End(list);) {
        DroppedSword* d = P<DroppedSword>(n->value);
        Dropped_Update(d, P<MapCollision>(map));
        if (d->state == 3) {
            Dropped_Free(d);
            n = GuestList_Erase(list, n);
        } else {
            n = GuestList_Next(n);
        }
    }
}

// ── 0x15C780: all of them go (the world is over) ──────────────────────────
void SwordDb_FreeModels(void* db) {
    GuestList* list = ListOf(db);
    for (GuestListNode* n = GuestList_Begin(list); n != GuestList_End(list);) {
        Dropped_Free(P<DroppedSword>(n->value));
        n = GuestList_Erase(list, n);
    }
}

// ── 0x16A480: the sword itself, for whoever takes it — counted the first
// time a sword is found, and its tallies start afresh ─────────────────────
void* DroppedSword_TakeOut(u32 dropped) {
    Sword* sword = P<Sword>(P<DroppedSword>(dropped)->sword);
    if (sword->number == 0) {
        u16* found = reinterpret_cast<u16*>(reinterpret_cast<u8*>(*gPlayerData) + 0x846);
        (*found)++;
        sword->number = 1;
    }
    sword->_84 = 0;
    sword->_88 = 0;
    sword->_80 = 0;
    return sword;
}
// ── 0x16A4E0: up it goes again — 2 taken, 1 only knocked ──────────────────
void DroppedSword_Hop(u32 dropped, int state) {
    P<DroppedSword>(dropped)->rise = 0.4f;
    P<DroppedSword>(dropped)->state = (s16)state;
}
void DroppedSword_SetGone(u32 dropped) { P<DroppedSword>(dropped)->state = 3; }
const float* DroppedSword_Position(u32 dropped) { return &P<DroppedSword>(dropped)->matrix[12]; }

}  // extern "C"
