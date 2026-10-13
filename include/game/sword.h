#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"

// A sword as the player owns it (0x100 bytes).  The first 0x80 bytes are a
// copy of its entry in the sword database; the rest is what has happened to
// this particular sword.
struct Sword {
    u16 name[44];       // +0x00
    s32 attack;         // +0x58  (signed: relative to the base)
    s32 defense;        // +0x5C
    s32 durability;     // +0x60
    s16 _64, _66;
    s32 _68;
    s32 textId;         // +0x6C  description
    s32 skillSheet;     // +0x70  index into SwordDb::skillSheets
    s32 _74;
    s32 _78;
    s32 type;           // +0x7C  database index (0 is recorded as 12 in the "seen" list)
    s32 _80, _84, _88;
    s32 _8c;            // +0x8C  -1
    u8  skill[0x40];    // +0x90  0 unknown, 1 known from the start, 2 follows from a known one
    s32 number;         // +0xD0
    u8  _d4[0x2C];
};

// One technique on a skill sheet.
struct SkillEntry {
    s32 requirement;    // 0 = known from the start
    s32 _04;
    s32 next;           // entry that becomes learnable, or -1
    s32 _0c;
};
struct SkillSheet {
    s32 count;
    u32 entries;        // SkillEntry[]
};

// The sword database: weapon/totaldata (asset 0x3E).  0xA4 bytes; singleton
// at *0x2EE588.  Besides the tables it owns caches of loaded weapon models,
// textures and motion packs, whose loaders are not decompiled yet.
struct SwordDb {
    u32 _00;                // object with a virtual destructor
    u32 _04;                // aligned buffer
    u32 _08;
    GuestList models;       // +0x0C
    u32 _18;
    GuestList _1c;          // +0x1C
    u32 textureNames;       // +0x28  char*[]
    s32 textureNameCount;   // +0x2C
    u32 _30;
    GuestList textures;     // +0x34
    u32 _40, _44;
    u32 _48;                // +0x48  pointer table
    s32 _4c;
    GuestList _50;          // +0x50
    u32 motionNames;        // +0x5C  char*[]: Common/weapon/wmp/*.wmp
    s32 motionNameCount;    // +0x60
    GuestList _64;          // +0x64
    s32 skillSheetCount;    // +0x70
    u32 skillSheets;        // +0x74  SkillSheet*[]
    s32 techniqueCount;     // +0x78
    u32 techniques;         // +0x7C  relocated blobs
    u32 styles;             // +0x80  0x3C-byte records
    u32 kinds;              // +0x84  0x60-byte records
    u32 swords;             // +0x88  0x80-byte records: the start of a Sword
    s32 swordCount;         // +0x8C
    u32 _90;
    s32 _94;                // -1
    GuestList _98;          // +0x98
};

extern "C" {
    Sword* Sword_Ctor(Sword* self);                             // 0x16A280
    Sword* Sword_Dtor(Sword* self, s16 flag);                   // 0x16A230
    void   Sword_Init(Sword* self);                             // 0x16A160

    SwordDb* SwordDb_Get();                                     // 0x15F430
    void     SwordDb_Release();                                 // 0x15F400
    // Fills `out` with a new sword of the given kind.
    int      SwordDb_MakeSword(SwordDb* self, int type, Sword* out);    // 0x15CFF0
}
