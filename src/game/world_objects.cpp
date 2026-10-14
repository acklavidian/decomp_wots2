// Things in the world: what lies about, what can be picked up, what people
// hold in story scenes, doors and crates.
//
// Decompiled from 0x21B470–0x21C570 (the manager at World+0xEC, older notes'
// "ItemDrops"), 0x153110–0x153430 (the object base class), 0x29B5A0–0x29BD90
// and 0x222610–0x222940 (the classes for pickups and held things).  See
// "World objects" in the project notes.
//
// NOT decompiled, and stood in for here:
//  - the object database (0x2B7B60–0x2BAA50): it is kept as host tables read
//    from the same two files, and an object's model (a 0xC0-byte class around
//    an A2MF model) is a small guest block {size, id, uses} drawn by native
//    code (hle_object_draw) — the model draw is the VU1 boundary anyway;
//  - the classes of types 8 (effect emitters) and 9: not made at all;
//  - the culling and lighting calls before a draw.

#include "engine/containers.h"
#include "game/chara_resources.h"
#include "game/map_collision.h"
#include "game/player_data.h"
#include "native/archive.h"
#include "native/guest.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
    // src/game/dropped_swords.cpp
    u32   SwordDb_Nearest(void* db, const float* matrix);               // 0x15C180
    void* DroppedSword_TakeOut(u32 dropped);                            // 0x16A480
    void  DroppedSword_Hop(u32 dropped, int state);                     // 0x16A4E0
    void  DroppedSword_SetGone(u32 dropped);
    const float* DroppedSword_Position(u32 dropped);
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void  Matrix_Identity(void* m);                             // 0x107428
    void  Sound_PlaySE(int id, int positional, int volume);     // 0x180E70
    int   Rand_Next();                                          // 0x1200F0
    // Native: draws the model file `path` at a world matrix.
    void  hle_object_draw(const char* path, const float* matrix);
    void  hle_object_draw_loose(const char* path, const float* matrix);
    void  hle_object_light(const float* level, int glow);
    u32   FlagsObj_AddItem(void* flags, u32 matrix, const float* size);     // 0x156F20
    void  FlagsObj_Remove(void* flags, u32 volume);                         // 0x156E80
    void  Fx_BreakBox(const float* pos, float size, int kind);              // 0x29AE00 (world_effects.cpp)
    void  Fx_PlaceCandle(const float* pos);                                    // (0x2B2E00, world_effects.cpp)
    void  Fx_PlaceSmoke(const float* pos, int variety);                        // (0x241070, world_effects.cpp)
    void  Fx_PlaceFirefly(const float* pos);                                   // (0x2BAF10, world_effects.cpp)
    void  Fx_PlaceSound(const float* pos, int interval, int id, int looped);   // (0x220BE0 + 0x220A00, world_effects.cpp)
    void* ItemDrops_Drop(void* self, int id, const void* matrix);           // 0x21C0E0
    int   Hud_FlashPrompt(void* hud);                           // 0x21DFA0
    int   CharaBody_fn1d1be0(void* body);                       // 0x1D1BE0  start lifting something
    int   CharaBody_fn1d1c30(void* body);                       // 0x1D1C30  start picking something up
}

namespace {

template <typename T = void> inline T* P(u32 a) { return guest::ptr<T>(a); }
const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};

// ── The object list: object/ObjectName.Lst (0x2B9280) and the per-map
// placement files named by object/ObjectIom.Lst (0x2B8AF0) ────────────────
struct ObjectInfo {
    std::string model;      // +0x10 in the game's record (a string)
    std::u16string name;
    float size[3];          // its box; y is how high its origin rests above the ground
    u8 param;               // +0x2C  e.g. how many mon a coin is worth
    u8 type;                // +0x2D  which class it is made as
    u8 a;                   // +0x2E  (held things: the bone it sits on)
    u8 b;                   // +0x2F  the item it is in the player's bag (0: money)
};
std::vector<ObjectInfo> sObjects;
std::vector<std::string> sMapFiles;     // by map id; "Nop" = none

void LoadLists() {
    if (!sObjects.empty())
        return;
    std::vector<u8> d;
    if (archive::read("object/ObjectName.Lst", d) && d.size() > 3) {
        size_t p = 3;                                       // "003"
        while (p < d.size()) {
            ObjectInfo o{};
            while (p < d.size() && d[p] != 0)
                o.model += (char)d[p++];
            p++;
            while (p + 1 < d.size() && (d[p] | d[p + 1]) != 0) {
                o.name += (char16_t)(d[p] | d[p + 1] << 8);
                p += 2;
            }
            p += 2;
            if (p + 16 > d.size())
                break;
            memcpy(o.size, &d[p], 12);
            o.param = d[p + 12];
            o.type = d[p + 13];
            o.a = d[p + 14];
            o.b = d[p + 15];
            p += 16;
            sObjects.push_back(std::move(o));
        }
    }
    if (archive::read("object/ObjectIom.Lst", d)) {
        size_t p = 0;
        while (p < d.size()) {
            std::string name;
            while (p < d.size() && d[p] != 0)
                name += (char)d[p++];
            p++;
            sMapFiles.push_back(std::move(name));
        }
    }
    printf("[objects] %zu kinds of object, placements for %zu maps\n", sObjects.size(), sMapFiles.size());
}
inline const ObjectInfo* InfoOf(int id) {                   // 0x2B7BB0
    LoadLists();
    return id >= 0 && (size_t)id < sObjects.size() ? &sObjects[(size_t)id] : nullptr;
}

// What stands in for the game's model object: only the fields object code reads.
struct ObjModel {
    float size[4];          // +0x00  (+0x04: the height its origin rests at)
    float corners[8][4];    // +0x10  of its box, about its origin (0x2B82B0)
    u32 _90;
    s32 id;                 // +0x94
    s32 uses;               // +0x98
};
std::vector<u32> sModels;                                   // by object id
ObjModel* ModelOf(int id) {                                 // 0x2B7DD0
    const ObjectInfo* info = InfoOf(id);
    if (info == nullptr)
        return nullptr;
    if (sModels.size() < sObjects.size())
        sModels.resize(sObjects.size(), 0);
    if (sModels[(size_t)id] == 0) {
        ObjModel* m = static_cast<ObjModel*>(guest::alloc(sizeof(ObjModel)));
        memcpy(m->size, info->size, 12);
        m->size[3] = 1.0f;
        const float* unit = P<float>(0x2DB910u);            // eight corners of a unit cube
        for (int i = 0; i < 8; i++) {
            for (int k = 0; k < 3; k++)
                m->corners[i][k] = m->size[k] * unit[i * 3 + k];
            m->corners[i][3] = 1.0f;
        }
        m->id = id;
        m->uses = 0;
        sModels[(size_t)id] = guest::addr(m);
    }
    return P<ObjModel>(sModels[(size_t)id]);
}

// ── The object (0x80 bytes; the classes below add to it) ──────────────────
struct WorldObject {
    float matrix[16];       // +0x00  (its place: +0x30)
    float velocity[4];      // +0x40
    float light[4];         // +0x50  from the map cell it is over
    u32 model;              // +0x60  ObjModel*
    u32 holder;             // +0x64  CharaParams* of who holds it
    s16 id;                 // +0x68  which object of the list
    u16 flags;              // +0x6A  0x20 held, 0x40 being taken, 1 hidden
    s32 kind;               // +0x6C  7 plain, 1 / 2 can be picked up (bag: carried / kept), 6 held in scenes
    s32 a;                  // +0x70
    s32 param;              // +0x74
    u32 vtable;             // +0x78
    u32 _7c;
};
struct PickupObject {       // 0xA0 bytes (0x29B9F0)
    WorldObject o;
    s32 settle;             // +0x80
    u32 _84[3];
    s32 takeDelay;          // +0x90  frames until it is in the bag
    u32 _94[3];
};
static_assert(sizeof(WorldObject) == 0x80 && sizeof(PickupObject) == 0xA0, "WorldObject");
enum : u16 { kObjHidden = 1, kObjHeld = 0x20, kObjTaken = 0x40 };
enum : u32 {
    kVtObject = 0x2EACD0u, kVtPickupBase = 0x2EBCF0u, kVtPickup = 0x2EC530u, kVtDrop = 0x2ED260u,
    kVtDrop7 = 0x2EC3A0u, kVtHeld = 0x2ED2B0u, kVtStatic = 0x2ED300u,
};

inline float* PosOf(WorldObject* o) { return &o->matrix[12]; }
inline const ObjectInfo* InfoOf(const WorldObject* o) { return InfoOf(o->id); }
// A bone of whoever holds it: its matrix in the world (the skeleton's 0xA0-byte entries).
inline const float* HolderBone(const WorldObject* o, int bone) {
    const CharaParams* holder = P<CharaParams>(o->holder);
    const u32 skeleton = *P<u32>(holder->link[1] + 4);
    return P<float>(skeleton + (u32)bone * 0xA0);
}
// (Before the draw the game sets the lights up for the place, 0x166680: the
// map's, the ambient level scaled by the light of the cell the object is
// over — natively hle_object_light, with 127 taken as full.)
inline void DrawModel(WorldObject* o, bool lit = false) {
    if (const ObjectInfo* info = InfoOf(o)) {               // (model vtable +0x14: the VU1 boundary)
        if (!lit) {
            const float cell[3] = {o->light[0] / 127.0f, o->light[1] / 127.0f, o->light[2] / 127.0f};
            hle_object_light(cell, 0);
        }
        if (o->holder != 0 && (o->flags & kObjHeld))
            hle_object_draw(info->model.c_str(), o->matrix);
        else
            hle_object_draw_loose(info->model.c_str(), o->matrix);
    }
}
// 0x153360: the light of the map cell it is over.
void TakeLight(WorldObject* o, const ColCellType* cell) {
    if (cell == nullptr)
        return;
    const s8* c = reinterpret_cast<const s8*>(cell) + 0x10;
    o->light[0] = (float)c[0];
    o->light[1] = (float)c[1];
    o->light[2] = (float)c[2];
    o->light[3] = 1.0f;
}

// ── Base class (0x153110–0x153430) ────────────────────────────────────────
WorldObject* Object_Ctor(void* mem) {                       // 0x153430
    WorldObject* o = static_cast<WorldObject*>(mem);
    o->vtable = kVtObject;
    o->flags = 0;
    o->model = 0;
    o->id = 0;
    o->a = 0;
    o->holder = 0;
    o->light[0] = o->light[1] = o->light[2] = 128.0f;
    o->light[3] = 1.0f;
    o->kind = 7;
    o->velocity[0] = o->velocity[1] = o->velocity[2] = 0.0f;
    o->velocity[3] = 1.0f;
    Matrix_Identity(o->matrix);
    return o;
}
WorldObject* Object_Dtor(WorldObject* o, s16 flag) {        // 0x1533E0
    if (o != nullptr) {
        o->vtable = kVtObject;
        if (flag > 0)
            MemFree(o);
    }
    return o;
}
int Object_Update(WorldObject* o, MapCollision* map) {      // 0x1532D0
    TakeLight(o, Map_CellAt(map, PosOf(o)));
    return 1;
}
int Object_Draw(WorldObject* o) {                           // 0x153200
    if (o->model == 0)
        return 1;
    if (o->holder != 0 && (o->flags & kObjHeld))
        memcpy(o->matrix, HolderBone(o, 29), sizeof o->matrix);     // the right hand
    DrawModel(o);
    return 1;
}
int Object_One(WorldObject*) { return 1; }                  // 0x1531F0, 0x153170
int Object_Place(WorldObject* o, const float* matrix, int) {    // 0x153180
    memcpy(o->matrix, matrix, sizeof o->matrix);
    return 1;
}
void Object_SetModel(WorldObject* o, ObjModel* model) {     // 0x1533B0
    o->model = guest::addr(model);
    o->id = (s16)model->id;
    model->uses++;
}
int Object_SetHolder(WorldObject* o, u32 holder) {          // 0x1532B0
    o->holder = holder;
    o->flags |= kObjHeld;
    return 1;
}
int Object_Release(WorldObject* o, const float* velocity, int) {    // 0x1531A0
    o->holder = 0;
    o->flags &= (u16)~kObjHeld;
    memcpy(o->velocity, velocity, sizeof o->velocity);
    for (int i = 0; i < 3; i++)
        PosOf(o)[i] += o->velocity[i];
    return 1;
}
int Object_Zero(WorldObject*) { return 0; }                 // 0x153110, 0x153140 … 0x153160
void Object_Nothing(WorldObject*) {}                        // 0x153120, 0x153130

inline int ObjUpdate(WorldObject* o, MapCollision* map) {
    return VirtualCall<int, WorldObject*, MapCollision*>(o->vtable, 0x0C, o, map);
}
inline void ObjDestroy(WorldObject* o) { VirtualCall<void*, WorldObject*, int>(o->vtable, 0x08, o, 1); }
inline void ObjJoin(WorldObject* o, u32 collision) { VirtualCall<void, WorldObject*, void*>(o->vtable, 0x2C, o, P(collision)); }
inline void ObjLeave(WorldObject* o, u32 collision) { VirtualCall<void, WorldObject*, void*>(o->vtable, 0x30, o, P(collision)); }
int Objects_TakeIntoBag(void* self, WorldObject* o, int keep);

// ── What can be picked up (0x29B9F0 and its two classes) ──────────────────
PickupObject* PickupBase_Ctor(void* mem) {                  // 0x29B9F0
    PickupObject* p = static_cast<PickupObject*>(mem);
    memset(p, 0, sizeof *p);
    Object_Ctor(&p->o);
    p->o.vtable = kVtPickupBase;
    p->settle = 10;
    return p;
}
WorldObject* Pickup_Dtor(WorldObject* o, s16 flag) {        // 0x2228D0, 0x29B840, 0x1EDB50, 0x21C620
    if (o != nullptr) {
        o->vtable = kVtPickupBase;
        Object_Dtor(o, 0);
        if (flag > 0)
            MemFree(o);
    }
    return o;
}
// 0x29B910: in the hand, easing halfway from where it was.
void Pickup_FollowHand(PickupObject* p) {
    float before[3];
    memcpy(before, PosOf(&p->o), sizeof before);
    memcpy(p->o.matrix, HolderBone(&p->o, 29), sizeof p->o.matrix);
    if (p->settle != 0)
        for (int i = 0; i < 3; i++)
            PosOf(&p->o)[i] += (before[i] - PosOf(&p->o)[i]) * 0.5f;
}
int Pickup_Draw(PickupObject* p) {                          // 0x222610, 0x29B600
    if (p->o.model == 0)
        return 1;
    if (p->o.holder == 0 || !(p->o.flags & kObjHeld))
        p->settle = 10;                                     // 0x29B900
    else
        Pickup_FollowHand(p);
    DrawModel(&p->o);
    return 1;
}

// The end of both updates: a moment after it is taken it goes into the bag —
// or, if there is no room, stays where it is.
int Pickup_Finish(PickupObject* p) {
    if (p->o.flags & kObjTaken) {
        if (p->takeDelay == 0) {
            if (Objects_TakeIntoBag(P(*P<u32>(0x2EE738u)), &p->o, 1) != 0)
                return 0;
            p->takeDelay = 0x10;
            p->o.flags &= (u16)~kObjTaken;
            return 1;
        }
        p->takeDelay--;
    }
    return 1;
}
// 0x2226C0: a coin, food, medicine — falls until it rests on the ground.
int Pickup_Update(PickupObject* p, MapCollision* map) {
    WorldObject* o = &p->o;
    const ColCellType* cell = Map_CellAt(map, PosOf(o));
    TakeLight(o, cell);
    const float rest = P<ObjModel>(o->model)->size[1];
    if (cell != nullptr && (!(o->flags & kObjHeld) || (o->flags & kObjTaken))) {
        const u32 cellFlags = *(reinterpret_cast<const u32*>(cell) + 1);
        const float ground = *(reinterpret_cast<const float*>(cell) + 6) + rest;
        if (!(cellFlags & 2) || (o->flags & kObjTaken)) {
            if (ground < PosOf(o)[1]) {
                for (int i = 0; i < 3; i++)
                    PosOf(o)[i] += o->velocity[i];
                o->velocity[1] -= 0.016333334f;
            } else {
                PosOf(o)[1] = ground;
            }
        } else {                                            // into a wall: back out and drop
            for (int i = 0; i < 3; i++)
                PosOf(o)[i] -= o->velocity[i];
            o->velocity[0] = 0.0f;
            o->velocity[2] = 0.0f;
            if (PosOf(o)[1] < ground) {
                o->velocity[1] = 0.0f;
                PosOf(o)[1] = ground;
            }
        }
    }
    return Pickup_Finish(p);
}
// 0x29B6A0: the same for things dropped by people; once down it lies flat.
int Drop_Update(PickupObject* p, MapCollision* map) {
    WorldObject* o = &p->o;
    const ColCellType* cell = Map_CellAt(map, PosOf(o));
    TakeLight(o, cell);
    if (cell != nullptr && (!(o->flags & kObjHeld) || (o->flags & kObjTaken))) {
        const u32 cellFlags = *(reinterpret_cast<const u32*>(cell) + 1);
        if (!(cellFlags & 2)) {
            const float ground = *(reinterpret_cast<const float*>(cell) + 6) + P<ObjModel>(o->model)->size[1];
            if (ground < PosOf(o)[1]) {
                for (int i = 0; i < 3; i++)
                    PosOf(o)[i] += o->velocity[i];
                o->velocity[1] -= 0.016333334f;
            } else {
                PosOf(o)[1] = ground;
                float at[4];
                memcpy(at, PosOf(o), sizeof at);
                Matrix_Identity(o->matrix);
                memcpy(PosOf(o), at, sizeof at);
            }
        } else {
            for (int i = 0; i < 3; i++)
                PosOf(o)[i] -= o->velocity[i];
            o->velocity[0] = 0.0f;
            o->velocity[2] = 0.0f;
        }
    }
    return Pickup_Finish(p);
}
// 0x222850 / 0x29B5A0 (vtable +0x28): taken — it hops up and, a moment
// later, is in the bag.
int Pickup_Take(WorldObject* o) {
    alignas(16) static const float kHop[4] = {0.0f, 0.2f, 0.0f, 0.0f};     // 0x2D9A40 / 0x2DB3A0
    VirtualCall<int, WorldObject*, const float*, int>(o->vtable, 0x24, o, kHop, 0);
    o->flags |= kObjTaken;
    return 1;
}
PickupObject* Pickup_Ctor(void* mem) {                      // 0x222940
    PickupObject* p = PickupBase_Ctor(mem);
    p->o.vtable = kVtPickup;
    p->o.kind = 1;
    p->takeDelay = 0x10;
    return p;
}
PickupObject* Drop_Ctor(void* mem) {                        // 0x29B8B0
    PickupObject* p = PickupBase_Ctor(mem);
    p->o.vtable = kVtDrop;
    p->o.kind = 2;
    p->takeDelay = 0x10;
    return p;
}

// ── What people hold in story scenes (0x29BB90) ───────────────────────────
WorldObject* Held_Dtor(WorldObject* o, s16 flag) {          // 0x29BB30, 0x29BCF0
    if (o != nullptr) {
        Object_Dtor(o, 0);
        if (flag > 0)
            MemFree(o);
    }
    return o;
}
int Held_Draw(WorldObject* o) {                             // 0x29BA30
    if ((o->flags & kObjHidden) || o->model == 0)
        return 1;
    if (o->holder != 0 && (o->flags & kObjHeld))
        memcpy(o->matrix, HolderBone(o, o->a), sizeof o->matrix);
    DrawModel(o);       // (held: the model's second draw method, +0x18)
    return 1;
}
WorldObject* Held_Ctor(void* mem) {                         // 0x29BB90
    WorldObject* o = Object_Ctor(mem);
    o->vtable = kVtHeld;
    o->kind = 6;
    return o;
}

// ── What just stands there (0x29BD50) ─────────────────────────────────────
int Static_Draw(WorldObject* o) {                           // 0x29BBD0
    if (o->model == 0)
        return 1;
    if (o->holder != 0 && (o->flags & kObjHeld) && o->a != 0)
        memcpy(o->matrix, HolderBone(o, o->a), sizeof o->matrix);
    DrawModel(o);
    return 1;
}
WorldObject* Static_Ctor(void* mem) {                       // 0x29BD50
    WorldObject* o = Object_Ctor(mem);
    o->vtable = kVtStatic;
    return o;
}

// ── Crates and pots (0x1EC9D0–0x1EDAE0, vtable 0x2EBCA0, 0x1C0 bytes) ─────
// A box with a volume in the collision world.  Walked into, it is nudged
// away; picked up (Square with the sword away) it is carried and can be
// thrown; struck — or thrown at someone — it takes a blow off its count
// (`param`) and at none breaks: splinters, dust and, by the table of
// item/ItemDrop.lst, something inside.  Loose, it is a small rigid body: its
// eight corners are tested against the ground under each (0x1ECE80) and
// every corner under it pushes back and sets it spinning (0x1ECCD0) until
// it comes to rest (flag 0x10).
struct CrateObject {
    WorldObject o;
    s32 settle;             // +0x80  (as a pickup: easing into the hand)
    u32 _84[3];
    float safe[16];         // +0x90  where it last was clear of walls
    float turn[16];         // +0xD0  how it has turned
    float inertiaBody[16];  // +0x110 its moments of inertia (see Crate_SetModel)
    float inertia[16];      // +0x150 … in the world
    float _190[4];
    float spin[4];          // +0x1A0 angles a frame
    float mass;             // +0x1B0 (its volume)
    s32 quiet;              // +0x1B4
    s32 clear;              // +0x1B8 no corner is in a wall: it may be put down or thrown
    u32 volume;             // +0x1BC ItemVolume*
};
static_assert(sizeof(CrateObject) == 0x1C0, "CrateObject");
enum : u16 { kObjAtRest = 0x10, kObjBroken = 0x80, kObjLanded = 0x100 };

inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Cross3(float* o, const float* a, const float* b) {
    const float r[4] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0.0f};
    memcpy(o, r, sizeof r);
}
inline void ApplyRow(float* o, const float* m, const float* v) {    // sceVu0ApplyMatrix
    float r[4];
    for (int j = 0; j < 4; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(o, r, sizeof r);
}
inline void Normalize3(float* v) {
    const float length = sqrtf(Dot3(v, v));
    if (length > 0.0f)
        for (int i = 0; i < 3; i++)
            v[i] /= length;
}
// sceVu0RotMatrix (0x1076C0): turned about z, then y, then x.
void RotateBy(float* m, const float* angles) {
    const auto turn = [&](int a, int b, float angle) {
        const float c = cosf(angle), sn = sinf(angle);
        for (int row = 0; row < 4; row++) {
            const float x = m[row * 4 + a], y = m[row * 4 + b];
            m[row * 4 + a] = x * c - y * sn;
            m[row * 4 + b] = x * sn + y * c;
        }
    };
    turn(0, 1, angles[2]);
    turn(2, 0, angles[1]);
    turn(1, 2, angles[0]);
}
// Its corners in the world, half a unit up (so a low kerb is not a wall).
void Crate_Corners(const CrateObject* c, float out[8][4]) {
    const ObjModel* model = P<ObjModel>(c->o.model);
    for (int i = 0; i < 8; i++) {
        ApplyRow(out[i], c->o.matrix, model->corners[i]);
        out[i][1] += 0.5f;
    }
}
// (inlined three times in 0x1ED3F0)
void Crate_CheckClear(CrateObject* c, MapCollision* map) {
    if ((c->o.flags & kObjHeld) || !(c->o.flags & kObjAtRest)) {
        c->clear = 1;
        float corners[8][4];
        Crate_Corners(c, corners);
        for (int i = 0; i < 8; i++)
            if (Map_CellAt(map, corners[i])->flags & 2) {
                c->clear = 0;
                return;
            }
    }
    memcpy(c->safe, c->o.matrix, sizeof c->safe);
}
// 0x1ECCD0: a corner that is under the ground pushes back.
bool Crate_Bounce(CrateObject* c, const float* corner, const float* normal, float* push, float* twist) {
    float arm[4], t[4], v[4];
    for (int i = 0; i < 4; i++)
        arm[i] = corner[i] - PosOf(&c->o)[i];
    Cross3(t, c->spin, arm);
    for (int i = 0; i < 4; i++)
        v[i] = c->o.velocity[i] + t[i];
    const float speed = Dot3(v, normal);
    if (speed >= 0.0f)
        return false;
    const float a = Dot3(arm, arm) / c->mass;
    Cross3(t, arm, normal);
    ApplyRow(t, c->inertia, t);
    Cross3(t, arm, t);
    const float b = Dot3(t, normal);
    float impulse[4];
    for (int i = 0; i < 3; i++)
        impulse[i] = normal[i] * ((a + b) * speed * -1.01f);
    impulse[3] = normal[3];
    for (int i = 0; i < 4; i++)
        push[i] -= v[i];
    for (int i = 0; i < 3; i++)
        push[i] -= arm[i] * 0.01f;
    push[3] -= arm[3];
    Cross3(t, arm, impulse);
    for (int i = 0; i < 4; i++)
        twist[i] += t[i];
    return true;
}
inline void Crate_SetThrown(CrateObject* c, int on) {
    VirtualCall<int, WorldObject*, int>(c->o.vtable, 0x34, &c->o, on);
}
// 0x1ECE80: a frame of it loose.
int Crate_Fly(CrateObject* c, MapCollision* map) {
    WorldObject* o = &c->o;
    static const float kUp[4] = {0.0f, 1.0f, 0.0f, 0.0f};                   // 0x2D9500
    o->velocity[3] = 0.0f;
    for (int i = 0; i < 3; i++)
        PosOf(o)[i] += o->velocity[i];
    RotateBy(c->turn, c->spin);
    float corners[8][4];
    Crate_Corners(c, corners);
    float push[4] = {0, 0, 0, 0}, twist[4] = {0, 0, 0, 0};
    float deepest = 0.0f, hits = 0.0f, under = 0.0f;
    o->velocity[1] -= 0.03266667f;
    for (int i = 0; i < 8; i++) {
        const ColCellType* cell = Map_CellAt(map, corners[i]);
        corners[i][1] -= 0.5f;
        if (!(cell->flags & 2)) {
            const float depth = corners[i][1] - cell->height;
            if (depth < 0.0f) {
                under += 1.0f;
                if (depth < deepest)
                    deepest = depth;
                if (Crate_Bounce(c, corners[i], kUp, push, twist))
                    hits += 1.0f;
                for (int k = 0; k < 4; k++) {
                    c->spin[k] += twist[k];
                    o->velocity[k] += push[k];
                    push[k] = twist[k] = 0.0f;
                }
            }
        } else {                                            // a wall: thrown back from it
            float away[4];
            for (int k = 0; k < 4; k++)
                away[k] = PosOf(o)[k] - corners[i][k];
            away[1] = 0.0f;
            Normalize3(away);
            const float into = Dot3(away, o->velocity);
            if (into < 0.0f) {
                const float k2 = sqrtf(-into) * 0.51f;
                for (int k = 0; k < 4; k++) {
                    o->velocity[k] += away[k] * k2;
                    c->spin[k] = -c->spin[k];
                }
            }
            Crate_SetThrown(c, 0);
        }
    }
    if (hits > 0.0f) {
        for (int k = 0; k < 4; k++) {
            o->velocity[k] *= 1.0f / hits;
            c->spin[k] *= 0.75f / hits;
        }
        if (under > 2.0f && Dot3(c->spin, c->spin) < 0.001f)
            o->flags |= kObjAtRest;
        if (deepest < -0.0f)
            PosOf(o)[1] -= deepest;
        if (!(o->flags & kObjLanded)) {
            static const s32 kThud[3] = {36, 38, 49};                       // 0x2D9510
            Sound_PlaySE(kThud[o->a], (int)guest::addr(PosOf(o)), 0x3FFF);
            o->flags |= kObjLanded;
            c->quiet = *P<s32>(0x2EDA00u) * 3;
        }
        Crate_SetThrown(c, 0);
    }
    memcpy(o->matrix, c->turn, 12 * sizeof(float));
    for (int k = 0; k < 4; k++) {
        o->velocity[k] *= 0.98f;
        c->spin[k] *= 0.97f;
    }
    return 1;
}
// 0x21BFF0 (with 0x2BA880): what was inside — by the table of
// item/ItemDrop.lst: u8 count, then 0x1F bytes each {u8 chance %, 3 tiers ×
// 5 × {u8 object, u8 weight %}} — tossed out.
void* Objects_DropContent(void* objects, int table, int tier, const float* matrix) {
    static std::vector<u8> data;
    if (data.empty() && !archive::read("item/ItemDrop.lst", data))
        data.assign(1, 0);
    if (table < 0 || table >= data[0] || (size_t)(table + 1) * 0x1F + 1 > data.size())
        return nullptr;
    const int t = tier > 3 ? (tier > 6 ? 2 : 1) : 0;
    const u8* record = &data[(size_t)table * 0x1F + 1];
    if ((u32)(Rand_Next() % 100 & 0xFF) >= record[0])
        return nullptr;
    const u32 roll = (u32)(Rand_Next() % 100 & 0xFF);
    u32 sum = 0;
    for (int i = 0; i < 5; i++) {
        sum = (sum + record[t * 10 + 2 + i * 2]) & 0xFF;
        if (roll <= sum)
            return ItemDrops_Drop(objects, record[t * 10 + 1 + i * 2], matrix);
    }
    return nullptr;
}
// 0x1ED880 (vtable +0x38): it is struck.  → 1 = broken
int Crate_Strike(CrateObject* c) {
    WorldObject* o = &c->o;
    if (o->param < 1) {                                     // cannot be broken: it only jumps
        if (c->clear != 0) {
            alignas(16) static const float kHop[4] = {0.0f, 0.01f, 0.0f, 0.0f};    // 0x2D94F0
            VirtualCall<int, WorldObject*, const float*, int>(o->vtable, 0x24, o, kHop, 0);
        }
        return 0;
    }
    if (--o->param != 0)
        return 0;
    const ObjModel* model = P<ObjModel>(o->model);
    const float size = sqrtf(Dot3(model->size, model->size));              // 0x136C20
    static const s32 kCrash[3] = {35, 37, 49};                              // 0x2D94E0
    Sound_PlaySE(kCrash[o->a], (int)guest::addr(PosOf(o)), 0x3FFF);
    alignas(16) float at[16];
    Matrix_Identity(at);
    memcpy(&at[12], PosOf(o), 4 * sizeof(float));
    const ObjectInfo* info = InfoOf(o);
    Objects_DropContent(P(*P<u32>(0x2EE738u)), info != nullptr ? info->b : 0xFF, 1, at);
    Fx_BreakBox(PosOf(o), size, o->a);
    o->flags |= kObjBroken;
    return 1;
}
inline int Crate_StrikeV(CrateObject* c) { return VirtualCall<int, WorldObject*>(c->o.vtable, 0x38, &c->o); }
// 0x1ED3F0
int Crate_Update(CrateObject* c, MapCollision* map) {
    WorldObject* o = &c->o;
    if (o->flags & kObjBroken)
        return 0;
    TakeLight(o, Map_CellAt(map, PosOf(o)));
    if (o->flags & kObjHeld) {
        memcpy(c->turn, o->matrix, sizeof c->turn);
        o->flags &= (u16)~kObjLanded;
        Crate_CheckClear(c, map);
        return 1;
    }
    if (c->volume != 0) {
        const u32 found = *P<u32>(c->volume + 0x174);
        if (found & 2) {                                    // struck
            if (Crate_StrikeV(c) != 0)
                return 0;
        } else if (found & 4) {                             // thrown, it hit someone
            Crate_CheckClear(c, map);
            if (o->holder == 0 && Crate_StrikeV(c) != 0)
                return 0;
        } else if (found & 1) {                             // someone walks into it
            Crate_CheckClear(c, map);
            if (c->clear != 0) {
                float away[4];
                const float* from = P<float>(c->volume + 0x190);
                for (int k = 0; k < 4; k++)
                    away[k] = PosOf(o)[k] - from[k];
                away[1] = 0.0f;
                Normalize3(away);
                for (int k = 0; k < 4; k++)
                    o->velocity[k] += away[k] * 0.01f;
                o->flags &= (u16)~kObjAtRest;
            }
        }
    }
    if (!(o->flags & kObjAtRest))
        return Crate_Fly(c, map);
    memcpy(c->turn, o->matrix, sizeof c->turn);
    c->turn[12] = c->turn[13] = c->turn[14] = c->turn[15] = 0.0f;
    memcpy(c->safe, o->matrix, sizeof c->safe);
    o->flags &= (u16)~kObjLanded;
    return 1;
}
// 0x1ECB20 (vtable +0x1C): with the model come its mass and inertia.
void Crate_SetModel(CrateObject* c, ObjModel* model) {
    Object_SetModel(&c->o, model);
    const float x = model->size[0], y = model->size[1], z = model->size[2];
    const float mass = z * x * y;
    // The moments of inertia of a box.  The original "inverts" this matrix
    // with sceVu0InversMatrix (0x1ECB20), which only undoes a rotation and a
    // shift: a diagonal matrix comes back as it went in, and the numbers
    // the bounce works with are the moments themselves.
    Matrix_Identity(c->inertiaBody);
    c->inertiaBody[0] = mass * (y * y + z * z) / 12.0f;
    c->inertiaBody[5] = mass * (x * x + z * z) / 12.0f;
    c->inertiaBody[10] = mass * (x * x + y * y) / 12.0f;
    // (turnᵀ × that × turn; it is only ever worked out here, with no turn yet)
    memcpy(c->inertia, c->inertiaBody, sizeof c->inertia);
    c->mass = mass;
    c->o.flags |= kObjAtRest;
}
int Crate_SetHolder(CrateObject* c, u32 holder) {           // 0x1EDA30
    c->o.holder = holder;
    c->o.flags |= kObjHeld;
    if (c->volume != 0)
        *P<u32>(c->volume + 0x1A4) = c->o.holder;
    return 1;
}
// 0x1ECA80 (vtable +0x24): let go of — back to where it was clear if it is in a wall now.
int Crate_Release(CrateObject* c, const float* velocity, int thrown) {
    WorldObject* o = &c->o;
    if (c->clear == 0)
        memcpy(o->matrix, c->safe, sizeof o->matrix);
    Crate_SetThrown(c, thrown);
    o->holder = 0;
    o->flags &= (u16)~kObjHeld;
    o->flags &= (u16)~kObjAtRest;
    memcpy(o->velocity, velocity, sizeof o->velocity);
    for (int k = 0; k < 4; k++)
        PosOf(o)[k] += o->velocity[k];
    return 1;
}
// 0x1ED360 (vtable +0x34): thrown — its volume hurts whoever it meets but the thrower.
int Crate_Thrown(CrateObject* c, int on) {
    if (c->volume != 0) {
        u32* flags = P<u32>(c->volume + 0x1A0);
        if ((s8)on == 1) {
            *flags |= 2;
            *P<u32>(c->volume + 0x1A4) = c->o.holder;
        } else {
            *flags &= ~2u;
            *P<u32>(c->volume + 0x1A4) = 0;
        }
    }
    return 1;
}
void Crate_Join(CrateObject* c, void* collision) {          // 0x1ECC70
    if (c->volume != 0)
        return;
    alignas(16) float half[4];
    const ObjModel* model = P<ObjModel>(c->o.model);
    for (int k = 0; k < 4; k++)
        half[k] = model->size[k] * 0.5f;
    c->volume = FlagsObj_AddItem(collision, guest::addr(c), half);
}
void Crate_Leave(CrateObject* c, void* collision) {         // 0x1ECC30
    if (c->volume != 0) {
        FlagsObj_Remove(collision, c->volume);
        c->volume = 0;
    }
}
u32 Crate_Volume(CrateObject* c) { return c->volume; }      // 0x1ED870
int Crate_Clear(CrateObject* c) { return c->clear; }        // 0x1ED860
CrateObject* Crate_Ctor(void* mem) {                        // 0x1EDAE0
    CrateObject* c = static_cast<CrateObject*>(mem);
    memset(c, 0, sizeof *c);
    PickupBase_Ctor(mem);
    c->o.vtable = 0x2EBCA0u;
    c->o.kind = 3;
    Matrix_Identity(c->turn);
    memset(c->spin, 0, sizeof c->spin);
    memset(c->o.velocity, 0, sizeof c->o.velocity);
    c->volume = 0;
    c->quiet = 0;
    c->clear = 0;
    return c;
}

// ── Sliding doors and screens (0x2168E0–0x216BD0, vtable 0x2EC2B0, 0xB0
// bytes).  A script slides one with Object.Action (vtable +0x28): over ten
// frames it moves along its own x axis by up to its width, eased by the
// table at 0x2D96D0 (0, ½, ¾ … 1).  Modes 1 / 0 slide it away one way or the
// other; 3 / 2 bring it back (as the original: to half way). ───────────────
struct DoorObject {
    WorldObject o;
    s32 timer;              // +0x80  frames of the slide left
    s32 mode;               // +0x84
    u32 _88[2];
    float home[4];          // +0x90  where it stands shut
    float axis[4];          // +0xA0  the way it slides
};
static_assert(sizeof(DoorObject) == 0xB0, "DoorObject");

int Door_Update(DoorObject* d, MapCollision* map) {         // 0x2169E0
    TakeLight(&d->o, Map_CellAt(map, PosOf(&d->o)));
    if (d->timer != 0) {
        const float* ease = P<float>(0x2D96D0u);
        float by = 0.0f;
        switch (d->mode) {
        case 3: by = ease[d->timer]; break;
        case 2: by = -ease[d->timer]; break;
        case 1: by = ease[10 - d->timer]; break;
        case 0: by = -ease[10 - d->timer]; break;
        }
        for (int i = 0; i < 3; i++)
            PosOf(&d->o)[i] = d->home[i] + d->axis[i] * by;
        d->timer--;
    }
    return 1;
}
int Door_Draw(DoorObject* d) { return Object_Draw(&d->o); } // 0x2168E0 (the base draw, never culled)
int Door_Place(DoorObject* d, const float* matrix, int) {   // 0x216970
    Object_Place(&d->o, matrix, 0);
    memcpy(d->home, PosOf(&d->o), sizeof d->home);
    for (int k = 0; k < 4; k++)
        d->axis[k] = d->o.matrix[k];                        // (1, 0, 0, 0) × its matrix
    return 1;
}
int Door_Slide(DoorObject* d, s32 mode) {                   // 0x216B50
    d->mode = mode;
    d->timer = 10;
    return 1;
}
DoorObject* Door_Ctor(void* mem) {                          // 0x216BD0
    DoorObject* d = static_cast<DoorObject*>(mem);
    memset(d, 0, sizeof *d);
    Object_Ctor(&d->o);
    d->o.vtable = 0x2EC2B0u;
    d->o.kind = 0;
    return d;
}

// ── Scenery (0x21A660–0x21ABA0, vtable 0x2EC330, 0xD0 bytes): tubs, casks,
// trees, lanterns, wanted posters, the carp.  It takes the light of the cell
// it stands on once; the carp (`param` 5) drifts about where it was put; a
// lantern (`a` 1) flickers at night and one of `a` 2 is always fully lit.
// ──────────────────────────────────────────────────────────────────────────
struct SceneryObject {
    WorldObject o;
    s32 phase;              // +0x80  degrees
    s32 lit;                // +0x84  has its cell's light
    u32 _88[2];
    float home[16];         // +0x90  where it was put
};
static_assert(sizeof(SceneryObject) == 0xD0, "SceneryObject");
int Scenery_Update(SceneryObject* self, MapCollision* map) {    // 0x21A950
    WorldObject* o = &self->o;
    if (self->lit == 0) {
        TakeLight(o, Map_CellAt(map, PosOf(o)));
        self->lit = 1;
    }
    if (o->param == 5) {
        const float angle = (float)self->phase * 3.1415927f / 180.0f;
        alignas(16) float drift[4] = {0.0f, sinf(angle) * 0.01f, sinf(angle) * 0.05f, 0.0f};
        ApplyRow(drift, self->home, drift);
        for (int k = 0; k < 4; k++)
            PosOf(o)[k] = drift[k] + self->home[12 + k];
        self->phase += (Rand_Next() & 7) + 3;
        if (self->phase > 360)
            self->phase -= 360;
    }
    if (o->a != 0) {
        self->phase += (Rand_Next() & 3) + 3;
        if (self->phase > 360)
            self->phase -= 360;
    }
    return 1;
}
// How it is lit (the lighting object's set-ups 0x1667E0 / 0x166730 /
// 0x166870, native: hle_object_light): ordinary scenery by the map's lights
// with the ambient level scaled by its cell's light (taken here as 127 =
// full, as for characters; the original divides by 256); `a` 2 by a level of
// its own; a lantern (`a` 1) like scenery by day and glowing at night —
// within 10 of the viewer the glow flickers, a little red.
int Scenery_Draw(SceneryObject* self) {                         // 0x21A660
    WorldObject* o = &self->o;
    if (o->model == 0)
        return 1;
    const float one[3] = {1.0f, 1.0f, 1.0f};
    const float cell[3] = {o->light[0] / 127.0f, o->light[1] / 127.0f, o->light[2] / 127.0f};
    if (o->a == 2) {
        hle_object_light(one, 1);
    } else if (o->a == 1) {
        if (*P<s8>(*P<u32>(0x2EE688u) + 0x102) == 0) {
            hle_object_light(cell, 0);
        } else {
            const float* eye = P<float>(*P<u32>(0x2EE528u) + 0x40);     // (the original: the place in view space)
            float d2 = 0.0f;
            for (int k = 0; k < 3; k++)
                d2 += (PosOf(o)[k] - eye[k]) * (PosOf(o)[k] - eye[k]);
            if (d2 < 100.0f) {
                const float t = (float)self->phase * 0.0055555557f * 3.1415927f;
                const float wave = sinf(t) + sinf(t * 2.0f) * 0.5f + sinf(t * 4.0f) * 0.25f +
                                   sinf(t * 6.0f) * 0.16f + sinf(t * 8.0f) * 0.125f;
                const float level = wave * 0.08f + (float)(Rand_Next() & 0xFF) * 0.00031250002f + 1.0f;
                const float glow[3] = {level + 0.1f, level, level};
                hle_object_light(glow, 1);
            } else {
                hle_object_light(one, 1);
            }
        }
    } else if (o->a == 0) {
        hle_object_light(cell, 0);
    }
    DrawModel(o, true);
    return 1;
}
int Scenery_Place(SceneryObject* self, const float* matrix, int) {  // 0x21AB00
    Object_Place(&self->o, matrix, 0);
    memcpy(self->home, self->o.matrix, sizeof self->home);
    self->lit = 0;
    return 1;
}
SceneryObject* Scenery_Ctor(void* mem) {                        // 0x21ABA0
    SceneryObject* self = static_cast<SceneryObject*>(mem);
    memset(self, 0, sizeof *self);
    Object_Ctor(&self->o);
    self->o.vtable = 0x2EC330u;
    self->phase = 0;
    self->o.kind = 4;
    return self;
}

const struct ObjectBinder {
    ObjectBinder() {
        const auto bind = [](u32 address, auto fn, const char* name) {
            guest::bind(address, fn, name);
        };
        bind(0x1533E0, Object_Dtor, "Object_Dtor");
        bind(0x1532D0, Object_Update, "Object_Update");
        bind(0x153200, Object_Draw, "Object_Draw");
        bind(0x1531F0, Object_One, "Object_One");
        bind(0x153180, Object_Place, "Object_Place");
        bind(0x1533B0, Object_SetModel, "Object_SetModel");
        bind(0x1532B0, Object_SetHolder, "Object_SetHolder");
        bind(0x1531A0, Object_Release, "Object_Release");
        bind(0x153110, Object_Zero, "Object_Zero");
        bind(0x153120, Object_Nothing, "Object_Nothing");
        bind(0x153130, Object_Nothing, "Object_Nothing");
        bind(0x153140, Object_Zero, "Object_Zero");
        bind(0x153150, Object_Zero, "Object_Zero");
        bind(0x153160, Object_Zero, "Object_Zero");
        bind(0x153170, Object_One, "Object_One");
        bind(0x216B70, Held_Dtor, "Door_Dtor");
        bind(0x2169E0, Door_Update, "Door_Update");
        bind(0x2168E0, Door_Draw, "Door_Draw");
        bind(0x2169D0, Object_One, "Object_One");
        bind(0x216970, Door_Place, "Door_Place");
        bind(0x216B50, Door_Slide, "Door_Slide");
        bind(0x1EDB50, Pickup_Dtor, "Pickup_Dtor");
        bind(0x21AB40, Held_Dtor, "Scenery_Dtor");
        bind(0x21A950, Scenery_Update, "Scenery_Update");
        bind(0x21A660, Scenery_Draw, "Scenery_Draw");
        bind(0x21AB00, Scenery_Place, "Scenery_Place");
        bind(0x1EDA70, Pickup_Dtor, "Crate_Dtor");
        bind(0x1ED3F0, Crate_Update, "Crate_Update");
        bind(0x1EC9D0, Pickup_Draw, "Crate_Draw");
        bind(0x1ED3E0, Object_One, "Object_One");
        bind(0x1ECB20, Crate_SetModel, "Crate_SetModel");
        bind(0x1EDA30, Crate_SetHolder, "Crate_SetHolder");
        bind(0x1ECA80, Crate_Release, "Crate_Release");
        bind(0x1ECC70, Crate_Join, "Crate_Join");
        bind(0x1ECC30, Crate_Leave, "Crate_Leave");
        bind(0x1ED360, Crate_Thrown, "Crate_Thrown");
        bind(0x1ED880, Crate_Strike, "Crate_Strike");
        bind(0x1ED870, Crate_Volume, "Crate_Volume");
        bind(0x1ED860, Crate_Clear, "Crate_Clear");
        bind(0x2228D0, Pickup_Dtor, "Pickup_Dtor");
        bind(0x29B840, Pickup_Dtor, "Pickup_Dtor");
        bind(0x21C620, Pickup_Dtor, "Pickup_Dtor");
        bind(0x2226C0, Pickup_Update, "Pickup_Update");
        bind(0x222610, Pickup_Draw, "Pickup_Draw");
        bind(0x2228B0, Object_Place, "Object_Place");
        bind(0x222850, Pickup_Take, "Pickup_Take");
        bind(0x29B6A0, Drop_Update, "Drop_Update");
        bind(0x29B600, Pickup_Draw, "Pickup_Draw");
        bind(0x29B820, Object_Place, "Object_Place");
        bind(0x29B5A0, Pickup_Take, "Pickup_Take");
        bind(0x29BB30, Held_Dtor, "Held_Dtor");
        bind(0x29BB10, Object_One, "Object_One");
        bind(0x29BA30, Held_Draw, "Held_Draw");
        bind(0x29BB20, Object_Place, "Object_Place");
        bind(0x29BCF0, Held_Dtor, "Held_Dtor");
        bind(0x29BC90, Object_Update, "Object_Update");
        bind(0x29BBD0, Static_Draw, "Static_Draw");
        bind(0x29BCD0, Object_Place, "Object_Place");
    }
} sObjectBinder;

// ── The manager (0x80 bytes, World+0xEC, also *0x2EE738) ──────────────────
struct Objects {
    GuestList list;         // +0x00  WorldObject*
    u32 pending;            // +0x0C  the map's placement file being read (here: read at once)
    u32 _10;                // +0x10  (item data, asset 0x6C — not read here)
    u32 database;           // +0x14  (the object database — host tables here)
    u32 collision;          // +0x18  the collision world objects join
    u32 _1c, _20;
    u8  visible;            // +0x24
    u8  _25[11];
    float carried[16];      // +0x30  where the thing carried over from the last map was
    s32 carriedId;          // +0x70  … and which (−1: nothing)
    u32 _74[3];
};
static_assert(sizeof(Objects) == 0x80, "Objects");

void RemoveAll(Objects* self, bool keepCarried) {
    for (GuestListNode* n = GuestList_Begin(&self->list); n != GuestList_End(&self->list);) {
        WorldObject* o = P<WorldObject>(n->value);
        if (keepCarried && o->kind == 2) {                  // (0x21B650)
            memcpy(self->carried, o->matrix, sizeof self->carried);
            self->carriedId = o->id;
        }
        if (self->collision != 0)
            ObjLeave(o, self->collision);
        ObjDestroy(o);
        n = GuestList_Erase(&self->list, n);
    }
}

}  // namespace

extern "C" {

// ── 0x21BAB0: makes object `id` at a place; → the object (what scripts hold) ─
void* ItemDrops_Place(void* mem, int id, const void* matrix) {
    Objects* self = static_cast<Objects*>(mem);
    const ObjectInfo* info = InfoOf(id);
    if (info == nullptr)
        return nullptr;
    WorldObject* o = nullptr;
    switch (info->type) {
    case 3: o = &Pickup_Ctor(MemAlloc(0xA0))->o; break;
    case 5: o = &Drop_Ctor(MemAlloc(0xA0))->o; break;
    case 7: o = &Drop_Ctor(MemAlloc(0xA0))->o; o->vtable = kVtDrop7; break;
    case 6: o = Held_Ctor(MemAlloc(0x80)); break;
    case 4: o = Static_Ctor(MemAlloc(0x80)); break;
    case 1: o = &Door_Ctor(MemAlloc(0xB0))->o; break;
    case 0: o = &Crate_Ctor(MemAlloc(0x1C0))->o; break;
    case 8:                                                 // effects of the place
        if (info->a == 2) {                                     // a wanted poster: one of three
            return ItemDrops_Place(mem, guest::ptr<s32>(0x2D9760u)[Rand_Next() % 3], matrix);
        }
        if (info->a == 1)                                       // a candle's flame
            Fx_PlaceCandle(static_cast<const float*>(matrix) + 12);
        else if (info->a == 0)                                  // steam, smoke, fire
            Fx_PlaceSmoke(static_cast<const float*>(matrix) + 12, info->b);
        else if (info->a == 3)                                  // a firefly
            Fx_PlaceFirefly(static_cast<const float*>(matrix) + 12);
        return nullptr;
    case 9:                                                 // a sound of the place
        Fx_PlaceSound(static_cast<const float*>(matrix) + 12, info->b, info->a, info->param);
        return nullptr;
    case 2: o = &Scenery_Ctor(MemAlloc(0xD0))->o; break;
    default: o = Static_Ctor(MemAlloc(0x80)); break;
    }
    if (o == nullptr)
        return nullptr;
    VirtualCall<void, WorldObject*, ObjModel*>(o->vtable, 0x1C, o, ModelOf(id));
    o->id = (s16)id;
    o->a = (s8)info->a;
    o->param = (s8)info->param;
    VirtualCall<int, WorldObject*, const void*, int>(o->vtable, 0x18, o, matrix, 0);
    if (self->collision != 0)
        ObjJoin(o, self->collision);
    GuestList_Insert(&self->list, GuestList_End(&self->list), guest::addr(o));
    return o;
}

// ── 0x21C0E0: the same, tossed a little way in a random direction ─────────
void* ItemDrops_Drop(void* self, int id, const void* matrix) {
    WorldObject* o = static_cast<WorldObject*>(ItemDrops_Place(self, id, matrix));
    if (o == nullptr)
        return nullptr;
    alignas(16) float toss[4] = {(float)(Rand_Next() % 2001 - 1000), 0.0f, (float)(Rand_Next() % 2001 - 1000), 0.0f};
    const float length = sqrtf(toss[0] * toss[0] + toss[2] * toss[2]);
    if (length > 0.0f) {
        toss[0] *= 0.02f / length;
        toss[2] *= 0.02f / length;
    }
    toss[1] = 0.2f;
    VirtualCall<int, WorldObject*, const float*, int>(o->vtable, 0x24, o, toss, 0);
    return o;
}

// 0x21B860: is it (still) one of the world's objects.
int ItemDrops_Has(void* mem, const void* object) {
    Objects* self = static_cast<Objects*>(mem);
    for (GuestListNode* n = GuestList_Begin(&self->list); n != GuestList_End(&self->list); n = GuestList_Next(n))
        if (n->value == guest::addr(object))
            return 1;
    return 0;
}
// 0x21B7A0
int ItemDrops_Remove(void* mem, void* object) {
    Objects* self = static_cast<Objects*>(mem);
    for (GuestListNode* n = GuestList_Begin(&self->list); n != GuestList_End(&self->list); n = GuestList_Next(n)) {
        if (n->value != guest::addr(object))
            continue;
        WorldObject* o = static_cast<WorldObject*>(object);
        if (self->collision != 0)
            ObjLeave(o, self->collision);
        ObjDestroy(o);
        GuestList_Erase(&self->list, n);
        return 1;
    }
    return 0;
}
// 0x21B310: the `nth` object of a kind (from 1) is acted on — a door slides
// (Object.Action).
void ItemDrops_Action(void* mem, int id, int nth, int how) {
    Objects* self = static_cast<Objects*>(mem);
    if (nth == 0)
        nth = 1;
    for (GuestListNode* n = GuestList_Begin(&self->list); n != GuestList_End(&self->list); n = GuestList_Next(n)) {
        WorldObject* o = P<WorldObject>(n->value);
        if (o->id == (s16)id && --nth == 0)
            VirtualCall<int, WorldObject*, int>(o->vtable, 0x28, o, how);
    }
}
// 0x21B470: whatever character `id` was holding goes with it.
void ItemDrops_ForgetChara(void* mem, int id) {
    Objects* self = static_cast<Objects*>(mem);
    for (GuestListNode* n = GuestList_Begin(&self->list); n != GuestList_End(&self->list); n = GuestList_Next(n)) {
        WorldObject* o = P<WorldObject>(n->value);
        if (o->holder == 0 || P<CharaParams>(o->holder)->id != id)
            continue;
        if (self->collision != 0)
            ObjLeave(o, self->collision);
        ObjDestroy(o);
        GuestList_Erase(&self->list, n);
        return;
    }
}

// 0x21C1A0: the item an object is in the bag.
int ItemDrops_ItemId(void*, int id) {
    const ObjectInfo* info = InfoOf(id);
    return info != nullptr ? info->b : 0;
}


// 0x21C240: an object's name, for the HUD (kept in guest memory once asked for).
const u16* ItemDrops_ObjectName(int id) {
    static std::vector<u32> names;
    const ObjectInfo* info = InfoOf(id);
    if (info == nullptr || id < 0)
        return nullptr;
    if (names.size() <= (size_t)id)
        names.resize((size_t)id + 1, 0);
    if (names[(size_t)id] == 0) {
        u16* text = static_cast<u16*>(MemAlloc(((u32)info->name.size() + 1) * 2));
        memcpy(text, info->name.data(), info->name.size() * 2);
        text[info->name.size()] = 0;
        names[(size_t)id] = guest::addr(text);
    }
    return guest::ptr<u16>(names[(size_t)id]);
}
// 0x21B0E0: the item it is in the bag (0 = money, 0xFF = no such object).
int ItemDrops_BagItem(int id) {
    const ObjectInfo* info = InfoOf(id);
    return info != nullptr ? info->b : 0xFF;
}

// 0x21C230 (→ 0x2B7BB0): the class an object of the list is made as.
int ItemDrops_ObjectType(int id) {
    const ObjectInfo* info = InfoOf(id);
    return info != nullptr ? info->type : -1;
}

}  // extern "C"

namespace {
// ── 0x21AE10: a pickup goes into the player's bag (coins into the purse) ──
int Objects_TakeIntoBag(void* self, WorldObject* o, int keep) {
    if (o->kind != 1 && o->kind != 2)
        return 0;
    const ObjectInfo* info = InfoOf(o);
    if (info == nullptr)
        return 0;
    u8* pd = reinterpret_cast<u8*>(*gPlayerData);
    if (info->b == 0) {
        s16* money = reinterpret_cast<s16*>(pd + 0x106);
        s32 now = *money + (s8)info->param;
        if (now > 9999)
            now = 9999;
        const s32 change = now - *money;
        *money = (s16)now;
        u16* total = reinterpret_cast<u16*>(pd + (change < 1 ? 0x84E : 0x84C));    // spent / earned
        const s32 sum = *total + (change < 0 ? -change : change);
        *total = (u16)(sum > 0xFFFF ? 0xFFFF : sum);
    } else if (PlayerData_AddItemSorted(*gPlayerData, o->kind == 2 ? 1 : 0, info->b) == 0) {
        return 0;
    }
    if (keep == 0)
        ItemDrops_Remove(self, o);
    Sound_PlaySE(0x3E, 0, 0x3FFF);
    return 1;
}
}  // namespace

extern "C" {

// ── 0x21C570 ──────────────────────────────────────────────────────────────
void* ItemDrops_Ctor(void* mem, u32 collision) {
    Objects* self = static_cast<Objects*>(mem);
    memset(self, 0, sizeof *self);
    GuestList_Init(&self->list);
    self->carriedId = -1;
    Matrix_Identity(self->carried);
    *P<u32>(0x2EE738u) = guest::addr(self);
    self->collision = collision;
    self->visible = 1;
    LoadLists();
    return self;
}
// 0x21B540
void ItemDrops_Clear(void* mem) {
    Objects* self = static_cast<Objects*>(mem);
    self->pending = 0;
    self->carriedId = -1;
    Matrix_Identity(self->carried);
    RemoveAll(self, false);
}
// 0x21B650: the same, but what the player was carrying comes along to the next map.
void ItemDrops_Reset(void* mem) {
    Objects* self = static_cast<Objects*>(mem);
    self->pending = 0;
    self->carriedId = -1;
    Matrix_Identity(self->carried);
    RemoveAll(self, true);
}
// 0x21C460
void ItemDrops_Dtor(void* mem, int flag) {
    if (mem == nullptr)
        return;
    ItemDrops_Clear(mem);
    if (*P<u32>(0x2EE738u) == guest::addr(mem))
        *P<u32>(0x2EE738u) = 0;
    if (flag > 0)
        MemFree(mem);
}
// 0x21C530
void ItemDrops_SetFlagsObj(void* mem, u32 collision) {
    Objects* self = static_cast<Objects*>(mem);
    self->collision = 0;
    ItemDrops_Clear(mem);
    self->collision = collision;
}
// 0x21BA60: are the files the objects need in (always, here).
int ItemDrops_OnSwordsLoaded(void*) { return 1; }

// ── 0x21C280 (with 0x21C390): the things that lie about in a map ──────────
// An .Iom file: s32 count, 12 bytes, then count × {float matrix[16], s32 object, 12 bytes}.
void ItemDrops_LoadMap(void* mem, int map) {
    Objects* self = static_cast<Objects*>(mem);
    LoadLists();
    if (self->carriedId > 0)
        ItemDrops_Place(self, self->carriedId, self->carried);
    if (map < 0 || (size_t)map >= sMapFiles.size())
        return;
    const std::string& name = sMapFiles[(size_t)map];
    if (name.size() < 3 || name.compare(name.size() - 3, 3, "Iom") != 0)
        return;
    std::vector<u8> file;
    if (!archive::read(name.c_str(), file) || file.size() < 16)
        return;
    s32 count;
    memcpy(&count, file.data(), 4);
    for (s32 i = 0; i < count && 16 + (size_t)(i + 1) * 0x50 <= file.size(); i++) {
        alignas(16) float matrix[16];
        s32 id;
        memcpy(matrix, &file[16 + (size_t)i * 0x50], sizeof matrix);
        memcpy(&id, &file[16 + (size_t)i * 0x50 + 0x40], 4);
        // WOTS2_DUMP_OBJECTS=1 (debug): what the map's placement file puts where.
        static const bool dump = getenv("WOTS2_DUMP_OBJECTS") != nullptr;
        if (dump)
            printf("[objects] map %d: object %d at %.2f %.2f %.2f (up %.2f %.2f %.2f)\n", map, id, matrix[12], matrix[13], matrix[14],
                   matrix[4], matrix[5], matrix[6]);
        ItemDrops_Place(self, id, matrix);
    }
}

// ── 0x21B930 ──────────────────────────────────────────────────────────────
void ItemDrops_Update(void* mem, u32 map) {
    Objects* self = static_cast<Objects*>(mem);
    // Debug hook: WOTS2_DROP="frame:object" puts an object of the list (0 a
    // 1-mon coin, 3 a rice ball, 4 dried fish…) on the ground a step in front
    // of the player, that many updates after the first.
    static const char* drop = getenv("WOTS2_DROP");
    static int updates = 0;
    if (drop != nullptr) {
        int frame = 0, id = 0;
        sscanf(drop, "%d:%d", &frame, &id);
        if (++updates >= frame) {
            drop = nullptr;
            if (const CharaParams* player = CharaResources_GetParams(CharaResources_Get(), 0)) {
                alignas(16) float m[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
                for (int i = 0; i < 3; i++)
                    m[12 + i] = player->matrix[12 + i] + player->matrix[8 + i] * 0.7f;
                m[13] += 0.5f;
                ItemDrops_Place(self, id, m);
                printf("[objects] dropped object %d at %.2f %.2f %.2f\n", id, m[12], m[13], m[14]);
            }
        }
    }
    for (GuestListNode* n = GuestList_Begin(&self->list); n != GuestList_End(&self->list);) {
        WorldObject* o = P<WorldObject>(n->value);
        if (ObjUpdate(o, P<MapCollision>(map)) != 0) {
            n = GuestList_Next(n);
            continue;
        }
        if (self->collision != 0)
            ObjLeave(o, self->collision);
        ObjDestroy(o);
        n = GuestList_Erase(&self->list, n);
    }
}
// ── 0x21B8B0 ──────────────────────────────────────────────────────────────
void ItemDrops_Draw(void* mem) {
    Objects* self = static_cast<Objects*>(mem);
    if (!self->visible)
        return;
    for (GuestListNode* n = GuestList_Begin(&self->list); n != GuestList_End(&self->list); n = GuestList_Next(n)) {
        WorldObject* o = P<WorldObject>(n->value);
        VirtualCall<int, WorldObject*>(o->vtable, 0x10, o);
    }
}

// ── 0x243330: what a character carries (the part at Chara+… that older
// notes call the "sheath": its first word is the object in hand; the rest of
// that class, 0x242E10–0x2432C0 — picking up, putting down, throwing — is
// still a stand-in).  Every frame the object is told who holds it.
void CharaSheath_Update(void* carrier, void* body, void* params) {
    u32* held = static_cast<u32*>(carrier);
    if (*held == 0)
        return;
    const u32 objects = *P<u32>(0x2EE738u);
    if (objects == 0 || ItemDrops_Has(P(objects), P(*held)) == 0) {
        *held = 0;
        return;
    }
    WorldObject* o = P<WorldObject>(*held);
    const u32 volume = VirtualCall<u32, WorldObject*>(o->vtable, 0x3C, o);
    if (volume == 0 || (*P<u32>(volume + 0x174) & 4) == 0 || VirtualCall<int, WorldObject*>(o->vtable, 0x38, o) == 0) {
        VirtualCall<int, WorldObject*, u32>(o->vtable, 0x20, o, guest::addr(params));
    } else {                                                // knocked out of the hand
        *held = 0;
        *(static_cast<u32*>(body) + 2) &= ~0x400u;
    }
}

// ── Picking up, carrying, putting down (0x242E80–0x2432C0, 0x21B000–0x21B120,
// 0x242550).  Swords lying about are the second word of what is found (the
// sword database's 0x15C180; src/game/dropped_swords.cpp).
// The body's state machine says when: it sets a step in its state word and
// the character calls the function for it. ──────────────────────────────

// 0x21B120: what lies within reach in front of someone — the nearest thing
// that can be pocketed, and the nearest that can only be carried.
static u32* NearestObjects(Objects* self, const float* matrix) {
    const float forward[3] = {matrix[8], matrix[9], matrix[10]};
    float from[3];
    for (int i = 0; i < 3; i++)
        from[i] = matrix[12 + i] - forward[i] * 0.3f;
    float nearPocket = 2.2f, nearCarry = 2.2f;
    u32 pocket = 0, carry = 0;
    for (GuestListNode* n = GuestList_Begin(&self->list); n != GuestList_End(&self->list); n = GuestList_Next(n)) {
        WorldObject* o = P<WorldObject>(n->value);
        if ((o->flags & (kObjHeld | kObjTaken)) || o->kind == 0 || (o->kind >= 4 && o->kind <= 6))
            continue;
        float d[3], along = 0, square = 0;
        for (int i = 0; i < 3; i++) {
            d[i] = from[i] - PosOf(o)[i];
            along += d[i] * forward[i];
            square += d[i] * d[i];
        }
        if (along >= 0.0f)
            continue;                                       // behind
        if (o->kind == 1 || o->kind == 2) {
            if (square < nearPocket) { nearPocket = square; pocket = n->value; }
        } else if (square < nearCarry) {
            nearCarry = square;
            carry = n->value;
        }
    }
    self->_1c = pocket;
    self->_20 = carry;
    return &self->_1c;
}
// 0x242550 → {thing to pocket, sword, thing to carry} at 0x30AA30.
int ItemDrops_fn242550(void* drops, const void* matrix, void* swordDb) {
    u32* found = P<u32>(0x30AA30u);
    found[0] = found[1] = found[2] = 0;
    if (swordDb != nullptr)
        found[1] = SwordDb_Nearest(swordDb, static_cast<const float*>(matrix));
    if (drops != nullptr) {
        const u32* near = NearestObjects(static_cast<Objects*>(drops), static_cast<const float*>(matrix));
        found[0] = near[0];
        found[2] = near[1];
    }
    return 0x30AA30;
}
// 0x21B000: is there room for it (coins always fit).
static int CanTake(WorldObject* o) {
    if (o->kind != 1 && o->kind != 2)
        return 0;
    const ObjectInfo* info = InfoOf(o);
    if (info == nullptr)
        return 0;
    if (info->b == 0)
        return 1;
    return PlayerData_HasFreeItemSlot(*gPlayerData, o->kind == 2 ? 1 : 0);
}
static inline u32& BodyFlags(void* body) { return *(static_cast<u32*>(body) + 2); }
static void LetGo(u32* held, void* body, const float* velocity, int thrown) {
    WorldObject* o = P<WorldObject>(*held);
    VirtualCall<int, WorldObject*, const float*, int>(o->vtable, 0x24, o, velocity, thrown);
    BodyFlags(body) &= ~0x400u;
    *held = 0;
}

// 0x2432C0: the button — bend down for it if there is something (a thing to
// carry only with the sword away).
void CharaSheath_fn2432c0(void*, void* body, int foundAt) {
    const u32* found = P<u32>((u32)foundAt);
    if (found[0] == 0 && found[1] == 0) {
        if (found[2] != 0 && (BodyFlags(body) & 0x100) == 0)
            CharaBody_fn1d1be0(body);
    } else {
        CharaBody_fn1d1c30(body);
    }
}
// 0x243150: the hand closes on it.
void CharaSheath_fn243150(void* carrier, void* body, int foundAt) {
    u32* held = static_cast<u32*>(carrier);
    const u32* found = P<u32>((u32)foundAt);
    if (found[0] == 0) {
        if (found[1] == 0) {
            if (found[2] != 0) {
                *held = found[2];
                BodyFlags(body) |= 0x400;
            }
        } else if (PlayerData_CanCarryAnother(*gPlayerData)) {   // a sword: straight into the hands
            PlayerData_CarrySword(*gPlayerData, static_cast<const Sword*>(DroppedSword_TakeOut(found[1])));
            DroppedSword_SetGone(found[1]);
            Hud_FlashPrompt(P(*P<u32>(0x2EE69Cu)));
        }
    } else if (CanTake(P<WorldObject>(found[0])) != 0) {
        Hud_FlashPrompt(P(*P<u32>(0x2EE69Cu)));
        *held = found[0];
        BodyFlags(body) |= 0x400;
    }
}
// 0x243070: taken where it lies, without carrying it first.
void CharaSheath_fn243070(void*, int foundAt) {
    const u32* found = P<u32>((u32)foundAt);
    if (found[0] == 0) {
        if (found[1] != 0) {                    // a sword: it hops, and is gone if there was a hand free
            int state = 1;
            if (PlayerData_CanCarryAnother(*gPlayerData)) {
                PlayerData_CarrySword(*gPlayerData, static_cast<const Sword*>(DroppedSword_TakeOut(found[1])));
                state = 2;
                Hud_FlashPrompt(P(*P<u32>(0x2EE69Cu)));
            }
            DroppedSword_Hop(found[1], state);
            Sound_PlaySE(0x12, (int)guest::addr(DroppedSword_Position(found[1])), 0x3FFF);
        }
        return;
    }
    WorldObject* o = P<WorldObject>(found[0]);
    if (CanTake(o) != 0)
        Hud_FlashPrompt(P(*P<u32>(0x2EE69Cu)));
    VirtualCall<int, WorldObject*, int>(o->vtable, 0x28, o, 0);
}
// 0x243240: what is in the hand goes into the bag.
void CharaSheath_fn243240(void* carrier, void* body) {
    u32* held = static_cast<u32*>(carrier);
    if (*held == 0)
        return;
    WorldObject* o = P<WorldObject>(*held);
    if ((o->kind == 1 || o->kind == 2) && Objects_TakeIntoBag(P(*P<u32>(0x2EE738u)), o, 0) != 0) {
        *held = 0;
        BodyFlags(body) &= ~0x400u;
    }
}
// 0x242EC0 / 0x242F40 / 0x242FF0: dropped, thrown ahead, put down.
void CharaSheath_fn242ec0(void* carrier, void* body) {
    alignas(16) static const float kDrop[4] = {0.0f, 0.1f, 0.0f, 0.0f};     // 0x2DA2A0
    if (*static_cast<u32*>(carrier) != 0)
        LetGo(static_cast<u32*>(carrier), body, kDrop, 0);
}
void CharaSheath_fn242f40(void* carrier, void* body) {
    u32* held = static_cast<u32*>(carrier);
    if (*held == 0 || VirtualCall<int, WorldObject*>(P<WorldObject>(*held)->vtable, 0x40, P<WorldObject>(*held)) == 0)
        return;
    const float* m = P<CharaParams>(*(static_cast<u32*>(body) + 0x1B0 / 4))->matrix;
    alignas(16) float toss[4];                              // (0, 0.1, 0.1) the way it faces — 0x2DA290
    for (int k = 0; k < 4; k++)
        toss[k] = 0.1f * m[4 + k] + 0.1f * m[8 + k];
    LetGo(held, body, toss, 1);
}
void CharaSheath_fn242ff0(void* carrier, void* body) {
    alignas(16) static const float kStill[4] = {0, 0, 0, 0};
    u32* held = static_cast<u32*>(carrier);
    if (*held != 0 && VirtualCall<int, WorldObject*>(P<WorldObject>(*held)->vtable, 0x40, P<WorldObject>(*held)) != 0)
        LetGo(held, body, kStill, 0);
}
// 0x242E10: an action marked for it (action flag 0x100000 → state 0x2000000)
// takes what lies within reach at the frame its blow begins (0x1CAD40),
// without the bending down: the step "taken where it lies".
void CharaSheath_fn242e10(void*, void* body) {
    u32* state = static_cast<u32*>(body) + 0x18 / 4;
    if ((*state & 0x2000000) == 0)
        return;
    const u32 defAt = *(static_cast<u32*>(body) + 0x1AC / 4);
    const u32 def = defAt != 0 ? *P<u32>(defAt) : 0;
    const u32 attack = def != 0 ? P<u32>(def)[1] : 0;
    if (attack == 0)
        return;
    const s32 frame = (s32)*P<float>(*(static_cast<u32*>(body) + 0x1B4 / 4) + 8);
    if (P<s32>(attack)[2] != frame)
        return;
    *state |= 0x1300000;
    *state &= ~0x2000000u;
}
// 0x242E80
void CharaSheath_fn242e80(void* carrier, void*, int a) {
    const u32 held = *static_cast<u32*>(carrier);
    if (held != 0)
        VirtualCall<int, WorldObject*, int>(P<WorldObject>(held)->vtable, 0x34, P<WorldObject>(held), a);
}

// What the scripts' Object built-ins need of an object (event_player.cpp).
float* WorldObject_Matrix(void* object) { return static_cast<WorldObject*>(object)->matrix; }
u16*   WorldObject_Flags(void* object) { return &static_cast<WorldObject*>(object)->flags; }
u32    WorldObject_Vtable(void* object) { return static_cast<WorldObject*>(object)->vtable; }

}  // extern "C"

// 0x21BFF0 for callers outside this file (a character that dies: chara.cpp).
void* Objects_DropFromTable(void* objects, int table, int tier, const float* matrix) {
    return Objects_DropContent(objects, table, tier, matrix);
}

// ── For the level editor (native/editor.cpp) — not in the game ───────────
#include "native/editor_hooks.h"

int Objects_KindCount() {
    LoadLists();
    return (int)sObjects.size();
}
bool Objects_Kind(int id, EditorObjectKind* out) {
    const ObjectInfo* info = InfoOf(id);
    if (info == nullptr)
        return false;
    out->model = info->model;
    out->name = info->name;
    memcpy(out->size, info->size, sizeof out->size);
    out->type = info->type;
    out->a = info->a;
    out->b = info->b;
    out->param = info->param;
    return true;
}
std::string Objects_PlacementFile(int map) {
    LoadLists();
    if (map < 0 || (size_t)map >= sMapFiles.size())
        return {};
    const std::string& name = sMapFiles[(size_t)map];
    return name.size() >= 3 && name.compare(name.size() - 3, 3, "Iom") == 0 ? name : std::string();
}
// The map's things made again from an edited placement file.  Effects and
// sounds of the place (types 8 and 9) are world effects, made once when the
// map is entered: they are left as they are and come right on the next entry.
void Objects_ReloadPlacements(const std::vector<u8>& file) {
    const u32 objects = *P<u32>(0x2EE738u);
    if (objects == 0)
        return;
    Objects* self = P<Objects>(objects);
    ItemDrops_Clear(self);
    s32 count = 0;
    if (file.size() >= 16)
        memcpy(&count, file.data(), 4);
    for (s32 i = 0; i < count && 16 + (size_t)(i + 1) * 0x50 <= file.size(); i++) {
        alignas(16) float matrix[16];
        s32 id;
        memcpy(matrix, &file[16 + (size_t)i * 0x50], sizeof matrix);
        memcpy(&id, &file[16 + (size_t)i * 0x50 + 0x40], 4);
        const ObjectInfo* info = InfoOf(id);
        if (info == nullptr || info->type == 8 || info->type == 9)
            continue;
        ItemDrops_Place(self, id, matrix);
    }
}
