#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"

// The world: one area (or the scene of a story event) with everything in it.
// Decompiled from 0x1DC1D0–0x1DECF0 (src/game/world.cpp).  It owns and steps
// the subsystems below; those are not decompiled yet and are stood in for by
// src/native/hle_gameplay.cpp.

struct Sword;
struct GsDevice;

// The wind (misnamed: it was first taken for a flickering light).  Idle for a
// while, then a gust that swells and dies away; its strength × `scale` is
// the x of the vector World keeps at `flash`, copies to CharaMgr+0x20 (which
// the characters' draw hands to the hair-and-cloth swing) and to
// RenderMgr+0x50.  `scale` is World+0xB0: the scripts' Map.SetWindGain.
struct LightFlicker {
    s32   wait;             // +0x00  frames until the next burst
    s32   _04;
    float direction;        // +0x08  +1 rising, -1 falling
    float level;            // +0x0C
    float high, low;        // +0x10
    u8    _18[0x18];
    float scale;            // +0x30  1 outdoors, 0.1 indoors
    u8    _34[0xC];
};

enum WorldKind : s32 { kWorldEvent = 0, kWorldField = 1, kWorldKind2 = 2, kWorldAskStatus = 3, kWorldAskDate = 4 };

struct World {
    s32 hideMap;            // +0x000
    s32 hideHud;            // +0x004
    s32 kind;               // +0x008
    s32 area;               // +0x00C  -1: no area (a story scene picks its own map)
    s32 exitUsed;           // +0x010  exit the player left the map through
    s32 targetMap;          // +0x014
    s32 doorIndex;          // +0x018
    s32 savedBgm;           // +0x01C
    s32 battleBgm;          // +0x020
    s32 placePlayer;        // +0x024  the player has not been put on the map yet
    s32 _28, _2c;
    s32 dying;              // +0x030
    s32 deathTimer;         // +0x034
    s32 deathState;         // +0x038  1 started, 2 running, 3 game-over shown
    s32 savedSeVolume;      // +0x03C
    s32 dayEndFade;         // +0x040
    s32 dayEnding;          // +0x044
    s32 dayEndShown;        // +0x048
    s32 introAction;        // +0x04C  the walk-in the player does on arrival
    s32 introTimer;         // +0x050
    s32 ownsItemDrops;      // +0x054
    s32 handOver;           // +0x058
    s32 exitHint;           // +0x05C
    s32 doorHint;           // +0x060
    s32 dialogChoice;       // +0x064
    s32 paused;             // +0x068
    s32 talkHint;           // +0x06C
    s32 introPending;       // +0x070
    s32 ownsHud2;           // +0x074
    s32 _78, _7c;
    LightFlicker flicker;   // +0x080
    float flash[4];         // +0x0C0
    float startPos[4];      // +0x0D0
    u32 stage;              // +0x0E0  lights and camera controllers (0xD0 bytes)
    u32 mapState;           // +0x0E4  the loaded map (0x80 bytes)
    u32 charaMgr;           // +0x0E8
    u32 itemDrops;          // +0x0EC
    u32 flagsObj;           // +0x0F0  (0x10 bytes; flags at +0xC)
    u32 effects;            // +0x0F4  (0x3C bytes)
    u32 pairs;              // +0x0F8  (8 bytes)
    u32 targeting;          // +0x0FC  (0x18 bytes)
    GuestList shadows;      // +0x100  {chara id, shadow object}
    u32 _10c;
};

// What a spawner hands to World_AddActor.
struct ActorDesc {
    s32 id;                 // +0x00  slot; 0 is the player
    s32 kind;               // +0x04
    s32 chara;              // +0x08  index into Common/chara/list
    s32 sword;              // +0x0C  -1 none
    s32 group;              // +0x10  bit 7: belongs to a group (low 7 bits; 0xFF = none)
    u32 _14[3];
    float position[4];      // +0x20
    s32 state[3];           // +0x30  passed to CharaMgr_SetState
    u32 box;                // +0x3C  trigger box, or 0
};

extern "C" {
    World* World_Ctor(World* self);                             // 0x1DECB0
    World* World_CtorArea(World* self, int area);               // 0x1DEC60
    int    World_Init(World* self);                             // 0x1DE5F0
    World* World_Dtor(World* self, s16 flag);                   // 0x1DEB70
    int    World_Draw(World* self);                             // 0x1DD530
    int    World_PollFault();                                   // 0x1DC340
    // Battle music follows whether the player is fighting.
    int    World_PreStep(World* self);                          // 0x1DC350
    // One frame of everything.  0 the map is loading, 1 running, 2 the player
    // is dying, 4 the death sequence is over.
    int    World_Step(World* self);                             // 0x1DD710
    void   World_GoToMap(World* self, s16 map);                 // 0x1DC770
    void   World_AddActor(World* self, const ActorDesc* actor, int a, int b);   // 0x1DE320
    void   World_RemoveActor(World* self, int id);              // 0x1DE230
    void   World_Begin(World* self);                            // 0x1DE050
    // The player went through a door (or was arrested): load the other map.
    void   World_EnterMap(World* self, int arrest);             // 0x1DCE90
    const char* World_TakeDoorScript(World* self);              // 0x1DCBB0
    // 0 nothing, 1 the player left an interior, 2 the player left the area.
    int    World_CheckExit(World* self);                        // 0x1DCC20
    int    World_DeathPending(World* self);                     // 0x1DC580
    int    World_IsIndoors(World* self);                        // 0x1DC540
    int    World_DayEndPending(World* self);                    // 0x1DC480
    // Puts the player where a save left it, or at the area's entrance.
    int    World_PlaceAtStart(World* self);                     // 0x1DDBE0
    int    World_StartIntro(World* self);                       // 0x1DDFC0
    int    World_GetHint(World* self);                          // 0x1DDF00
    // True once the walk-in is over.
    int    World_IntroDone(World* self);                        // 0x1DDD80
    int    World_UpdateTalkHint(World* self, int area);         // 0x1DCD70
    // exit -1: is the player at a door?  Otherwise use that exit.  Non-zero: go.
    int    World_UseExit(World* self, int exit);                // 0x1DC880
    int    World_SetSword(World* self, Sword* sword);           // 0x1DC6A0
    u32    World_GetFlags(World* self);                         // 0x1DC700
    void   World_ClearFlags(World* self, u32 flags);            // 0x1DC720
    void   World_SetFlags(World* self, u32 flags);              // 0x1DC750
    int    World_GetIndoors();                                  // 0x1DC570
    void   World_SetIndoors(int indoors);                       // 0x1DD140
    // Puts the player at the spawn point that goes with an exit (0 = the
    // area's entrance), facing the point paired with it.
    void   World_PlacePlayer(World* self, int exit);            // 0x1DD150
    // Turns a model matrix about the vertical axis to face a point.
    void   Matrix_FaceTowards(float* matrix, const float* point);   // 0x1C8DB0
}
