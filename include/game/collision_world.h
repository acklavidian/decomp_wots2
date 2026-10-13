#pragma once
#include "engine/containers.h"
#include "hal/ps2_types.h"

// The collision world (World+0xF0; 0x155270–0x1571F0): the volumes of
// everything that can touch — characters' bodies, their swords (or fists and
// feet), dropped items — tested against each other once a frame.  What a
// volume met is left in `found` for its owner to act on.

struct Volume {
    float corners[8][4];    // +0x000  the box in its own space
    float world[8][4];      // +0x080  …and in the world
    u32   kind;             // +0x100  VolumeKind
    u32   shape;            // +0x104  0 / 1: a sphere will do; 4: test edges too
    u8    _108[8];
    float matrix[16];       // +0x110  (the centre is its last row, +0x140)
    float offset[4];        // +0x150
    float invSize[4];       // +0x160
    float radius;           // +0x170
    u32   found;            // +0x174
    s32   on;               // +0x178
    u32   vtable;           // +0x17C  +0x08 destroy, +0x0C refresh, +0x10 clear, +0x14 active, +0x18 present
    u32   info;             // +0x180  pointers into the owner (Chara_fn1cd860)
};
enum VolumeKind : u32 { kVolBody = 1, kVolSword = 4, kVolItem = 8 };

// A character's body (0x1D0 bytes; 0x159BB0–0x15A580).
// info: {params, &loaded, &active, &stagger, last position, position, &sword in hand, &who hit it}
struct BodyVolume {
    Volume base;
    s32   damage;           // +0x184
    s32   power;            // +0x188  of the blow that landed
    u32   hurt;             // +0x18C  the reaction it calls for (bit 28: from behind)
    u32   bump;             // +0x190  walked into: 1 from in front, 2 from behind
    s32   noPush;           // +0x194
    u8    _198[8];
    float blowFrom[4];      // +0x1A0  direction to the attacker
    float blowAt[4];        // +0x1B0
    s32   guardDamage;      // +0x1C0
    s32   clash;            // +0x1C4
    u8    _1c8[8];
};
enum BodyFound : u32 {
    kHitBlocked = 1,       // someone is in the way: take x and z back
    kHitBumped = 2, kHitShoved = 4, kHitSteppedOn = 8,
    kHitStruck = 0x10,     // by a fist, a foot or a thrown thing
    kHitCut = 0x20, kHitCutBlunt = 0x40,
    kHitFinished = 0x100, kHitMarked = 0x200,
    kHitGuarded = 0x1000, kHitParried = 0x2000,
};

// A sword, fist or foot (0x1A0 bytes; 0x16B0C0–0x16B680).
// info: {params, &loaded, &active, &motion id, &input rating, sword holder, &sword}
struct SwordVolume {
    Volume base;
    s32   bone;             // +0x184  the bone it rides on
    s32   form;             // +0x188  0x10 the sword; else a box of table 0x2D2030
    u32   _18c;
    s32   wear;             // +0x190
    s32   kills;            // +0x194
    s32   finishes;         // +0x198
    s32   score;            // +0x19C
};
enum SwordFound : u32 {
    kSwordGuarded = 1, kSwordParried = 2, kSwordCut = 4, kSwordStruck = 8,
    kSwordKilled = 0x100, kSwordLanded = 0x10000000, kSwordHitCorpse = 0x20000000,
};

// A dropped item (0x1B0 bytes; 0x210350–0x210540).  info: its matrix.
struct ItemVolume {
    Volume base;
    u8    _184[0xC];
    float touchedAt[4];     // +0x190
    u32   flags;            // +0x1A0  2: thrown
    u32   thrower;          // +0x1A4  CharaParams*
    u8    _1a8[8];
};

struct CollisionWorld {
    GuestList volumes;      // +0x00
    u32 arg;                // +0x0C
};

extern "C" {
    void* FlagsObj_Ctor(void* mem);                                 // 0x1571F0
    void  FlagsObj_Dtor(void* self, int flag);                      // 0x1570A0
    void  FlagsObj_StepA(void* self);                               // 0x156DE0  volumes follow their owners
    void  FlagsObj_StepB(void* self);                               // 0x156A00  test them against each other
    void  FlagsObj_Reset(void* self);                               // 0x155270  clear what was found
    void  FlagsObj_Remove(void* flags, u32 volume);                 // 0x156E80
    u32   FlagsObj_AddChara(void* flags, u32 info, int id);         // 0x157020
    u32   FlagsObj_Add(void* flags, u32 info);                      // 0x156FB0  a sword
    u32   FlagsObj_AddItem(void* flags, u32 matrix, const float* size);     // 0x156F20
    // An attack's hit frames begin: the volume takes the shape of what hits.
    void  SwordVolume_Arm(SwordVolume* self, s32 form, s32 bone);   // 0x16B490
    void  Volume_SetSize(Volume* self, const float* size);          // 0x1572E0
    void  Volume_ToWorld(Volume* self);                             // 0x157250
}
