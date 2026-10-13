#pragma once
#include "engine/containers.h"
#include "game/chara_resources.h"
#include "hal/ps2_types.h"

// Motions and what they mean (the motion side of the resource manager,
// 0x1D2D50–0x1D4C00, with the action records of 0x1518B0–0x151E60).
//
// A character *action* is a number (0 stand, 1 walk, 3 run, 6 fall, 0x2F draw
// the sword, …).  Each motion pack carries a table that says which *motion*
// plays for which action; a motion comes with an *action record* that says
// what the body may do while it plays.
//
//   Common/motion/common.nmp   (asset 0x8B) 135 motions every pack shares:
//       u32 1, u32 count, count × {u32 id, u32 size, motion, u32 size, record}
//   Common/motion/*.mtp        a pack (ids from Common/motion/list, asset 0x2A):
//       u32 version (≥ 5), u32 count,
//       count × {u32 id (−1: it is one of the common ones), u32 size,
//                {u32 8, u32 offset of the record, motion, record}},
//       u32 size, table {u32 count, u32 8, {u32 action, u32 motion}[count]}

// What the body may do during a motion (all offsets become pointers when the
// record is relocated; null = none).
struct ActionDef {
    u32 frames;     // +0x00  ActionFrames*
    u32 attack;     // +0x04  ActionAttack*
    u32 sounds;     // +0x08  {s32 count; {s32 what, frame}[]}: effects to start at a frame
    u32 voices;     // +0x0C  the same, for the voice
    u32 cancel;     // +0x10  {u32 kinds, s32 frame}: kinds of action that may cut in from that frame
    u32 links;      // +0x14  {s32 count; ActionLink[]}: actions it follows on from specially
    u32 flags;      // +0x18  ActionFlag
};
struct ActionFrames { s32 start, end, blend; };
struct ActionAttack {
    s32 kind, strength;     // +0x00
    s32 hitFrom, hitTo;     // +0x08  frames the blow can land in
    s32 guardFrom, guardTo; // +0x10
    s32 stepFrom, stepTo;   // +0x18  frames it closes in on the opponent
    s32 turnFrom, turnTo;   // +0x20  frames it turns to the opponent
    s32 value[6];           // +0x28
};
struct ActionLink { s32 from, first, start, blend, length; };

// ActionDef::flags (what 0x1CB2B0 turns into body flags).
enum ActionFlag : u32 {
    kActJump = 4, kActCombo = 2, kActAttack = 8, kActLying = 0x10, kActWalk = 0x20, kActRun = 0x40,
    kActGuard = 0x80, kActFlagA = 0x100, kActFlagB = 0x1000, kActHop = 0x2000, kActAnyTime = 0x4000,
    kActGetUpA = 0x8000, kActGetUpB = 0x10000, kActDash = 0x40000, kActStand = 0x400000,
};

// The common motions (0x30A6E0).
struct CommonMotions {
    u32 count;
    u32 ids;        // s32[]
    u32 motions;    // u32[]: motion data
    u32 actions;    // u32[]: ActionDef
};

extern "C" {
    void BattleAi_Create(const u8* file);                                   // ai_fight.cpp: 0x29AA10 + 0x29A7D0
    void ActionDef_Relocate(ActionDef* def);                                // 0x151940
    int  ActionDef_LinkCount(const ActionDef* def);                         // 0x151B10
    const ActionLink* ActionDef_Link(const ActionDef* def, int index);      // 0x1519D0–0x151AD0
    // The effect (voice) to start at `frame`; call until it returns −1.
    int  ActionDef_SoundAt(const ActionDef* def, int frame);                // 0x151C00
    int  ActionDef_VoiceAt(const ActionDef* def, int frame);                // 0x151B60

    // Reads Common/motion/common.nmp (as loaded) into the table at 0x30A6E0.
    void CommonMotions_Load(const u32* file);                               // 0x1D6D20 / 0x2BB1A0
    void CharaResources_InitMotions(CharaResources* self);                  // part of the constructor (0x1D72F0)
    void CharaResources_RegisterCommon(CharaResources* self);               // 0x1D2E90

    // By motion id: a u32* whose value is the motion (the action record).
    u32  CharaResources_MotionOf(CharaResources* self, s32 motion);         // 0x1D2DA0
    u32  CharaResources_ActionOf(CharaResources* self, s32 motion);         // 0x1D2D50
    // By pack id: a u32* whose value is the pack's table {count, pairs*}.
    u32  CharaResources_PackTable(CharaResources* self, s32 pack);          // 0x1D2DF0
    int  CharaResources_HasPack(CharaResources* self, s32 pack);            // 0x1D4130
    const char* CharaResources_PackPath(CharaResources* self, u32 pack);    // 0x1D4C00
    s32  CharaResources_PackOf(CharaResources* self, s32 chara);            // 0x1D4BB0  the pack a character uses
    // Takes a pack into the cache from its file (or another reference to it
    // when `file` is null); → what CharaResources_PackTable gives.
    u32  CharaResources_InstallPack(CharaResources* self, s32 pack, const u32* file);   // the body of 0x1CF030
    void CharaResources_ReleasePack(CharaResources* self, s32 pack);        // 0x1D3C90
    void CharaResources_ParseMotionList(CharaResources* self, const u32* file);     // 0x1D4CA0
}
