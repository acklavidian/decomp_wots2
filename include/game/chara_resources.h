#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"
#include "engine/text_label.h"

// The character resource manager (singleton at *0x2EE63C, 0xC4 bytes): the
// list of characters and their files, parameter and motion lists, and the
// caches of loaded models, textures and motions.  Only the character list is
// decompiled so far (0x1D4E00–0x1D4F00, 0x1D6500, 0x1D6630, 0x1D68A0–0x1D6A70).

// One character of Common/chara/list.
struct CharaInfo {
    s32   id;               // +0x00
    s32   pack;             // +0x04  its motion pack (an index of Common/motion/list)
    float param[2];         // +0x08
    u32   name;             // +0x10  char*
    u32   texturePath;      // +0x14  .pim, or 0 when the models carry their own
    u32   bodyPath[3];      // +0x18  by level of detail; only [1] is used on this disc
    u32   headPath[3];      // +0x24
};

// A character's parameters (0xE0 bytes; 0x1C8670–0x1C8AA0).  The templates
// come from chara/paramlist (asset 0x29), one per kind of character; a
// character in the world gets its own copy, linked to it by the pointers at
// the end.
struct CharaParams {
    s32   id;               // +0x00  index in the list
    s32   chara;            // +0x04  model: an id of Common/chara/list
    s32   _08;              // +0x08  −1
    s32   unique;           // +0x0C  a named character (1) or story character (2); 0 = a face in the crowd
    s32   child;            // +0x10
    s32   _14;              // +0x14  1 when the file's eighth field is not −1
    s32   _18;
    s32   role;             // +0x1C  what actor templates ask for
    s32   _20, _24, _28;
    s32   stat[4];          // +0x2C  what task commands call character values 3–6
    u32   _3c;
    s16   hp, maxHp;        // +0x40
    s16   sword;            // +0x44  −1 = unarmed
    s16   faction;          // +0x46  0 magistrate's men, 1 townspeople, 2 the gang
    s16   _48;              // +0x48  −1
    s16   _4a;
    u32   _4c;
    float matrix[16];       // +0x50
    float scale[4];         // +0x90
    WString name;           // +0xA0
    u32   nameVtable;       // +0xAC  0x2EAF30
    u32   _b0;
    u32   link[9];          // +0xB4  into the live character (+0xC4 alive flag, +0xCC state, +0xD0 time)
    u32   _d8[2];
};

struct CharaParamsRef { u32 params; };  // what the lists hold: a pointer to this

struct CharaResources {
    u32 swordTable;         // +0x00  which sword each character carries, by difficulty (0x2B4xxx)
    u32 _04;
    u32 loaded;             // +0x08
    u32 _0c, _10;
    GuestList charas;       // +0x14  CharaInfo*
    GuestList motions;      // +0x20  MotionEntry*: motions in memory
    u8  _2c[0x18];          // +0x2C / +0x38: body and head files in memory (not decompiled)
    GuestList params;       // +0x44  CharaParamsRef*: the parameters of characters in the world
    GuestList templates;    // +0x50  CharaParamsRef*: chara/paramlist
    GuestList packs;        // +0x5C  PackEntry*: motion packs in memory
    GuestList motionNames;  // +0x68  char*: Common/motion/list, by pack id
    GuestIntMap packTables; // +0x74  pack id → u32* → its table
    GuestIntMap motionMap;  // +0x84  motion id → u32* → the motion
    GuestIntMap actionMap;  // +0x94  motion id → u32* → its ActionDef
    u8  _a4[0x20];          // which files belong to the player (not decompiled)
};

extern "C" {
    CharaResources* CharaResources_Get();                                   // 0x1D6DB0
    void CharaResources_Load(CharaResources* self, int force);              // 0x1D6A80
    // `file` is Common/chara/list as read from the archive.
    void CharaResources_ParseList(CharaResources* self, const u32* file);   // 0x1D6630
    void CharaResources_FreeList(CharaResources* self);                     // 0x1D6500

    const CharaInfo* CharaResources_Find(CharaResources* self, int id);     // (inlined search)
    const char* CharaResources_GetTexturePath(CharaResources* self, int id);            // 0x1D4F00
    const char* CharaResources_GetBodyPath(CharaResources* self, int id, int lod);      // 0x1D4EB0
    const char* CharaResources_GetHeadPath(CharaResources* self, int id, int lod);      // 0x1D4E60
    float CharaResources_GetParam(CharaResources* self, int id, int which);             // 0x1D4E00

    CharaParamsRef* CharaParams_New(CharaParamsRef* ref, int id);           // 0x1C8AA0
    // Reads one entry of chara/paramlist; returns where the next one starts.
    const u8* CharaParams_Parse(CharaParamsRef* ref, const u8* data, u16 version);  // 0x1C8670
    CharaParamsRef* CharaParams_Delete(CharaParamsRef* ref, s16 flag);      // 0x1C8990
    // `file` is chara/paramlist as read from the archive.
    void CharaResources_ParseParams(CharaResources* self, const u32* file); // 0x1D6400
    void CharaResources_FreeParams(CharaResources* self);                   // 0x1D5280
    CharaParams* CharaResources_FindParamTemplate(CharaResources* self, int id);    // 0x1D5230
    // Fill `out` (a list of ints) with the ids of the templates of a role
    // (and faction); −1 matches any.
    void CharaResources_ListByRole(CharaResources* self, int role, GuestList* out);             // 0x1D4F50
    void CharaResources_ListByFaction(CharaResources* self, s16 faction, int role, GuestList* out);  // 0x1D5030
    int  CharaResources_MemoryBudget(CharaResources* self);                 // 0x1D6D10
    CharaParams* CharaResources_GetParams(CharaResources* self, int id);    // 0x1D5330
}
