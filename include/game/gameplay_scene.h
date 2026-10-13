#pragma once
#include "hal/ps2_types.h"
#include "engine/scene.h"
#include "game/day_mgr.h"
#include "game/pause_menu.h"
#include "game/area_map.h"
#include "game/world.h"
#include "game/world_parts.h"

// The Gameplay scene (scene 14, vtable 0x2ED710, 0xF0 bytes).  Its shell —
// the mode / phase machine — is decompiled in src/game/gameplay_scene.cpp
// from 0x2ADA00–0x2B2230.  The objects it drives are declared here; those
// not decompiled yet are stood in for by src/native/hle_gameplay.cpp.

struct GsDevice;
struct PlayerData;

enum GameplayMode : s32 {
    kPlayEvent = 0,         // a story scene: the world with a script running
    kPlayMode1 = 1,
    kPlayField = 2,         // free roaming in an area
    kPlayAreaMap = 3,       // choosing where to go
    kPlaySettings = 4,
    kPlayStatus = 5,
    kPlayEnterDoor = 6,     // becomes kPlayField
    kPlayNextDay = 8,       // becomes kPlayAreaMap
    kPlaySwordInfo = 10,
    kPlayArrest = 0xB,      // becomes kPlayField
    kPlaySave = 0xC,
    kPlaySwordSelect = 0xD,
    kPlayItemSelect = 0xE,
    kPlayLeave = -1,
};

enum GameplayPhase : s32 {
    kPlayBegin = 0, kPlayLoading = 1, kPlayLoaded = 2, kPlayFadeIn = 3, kPlayRunning = 4,
    kPlayLeaving = 5, kPlayFadeOut = 7,
};

// The parts of the objects the shell reads and writes directly.
struct EventPlayer {
    u32 world;              // +0x00
    u8  _04[8];
    s32 state;              // +0x0C  2 = a script is running
    s32 result;             // +0x10  0 day goes on, 1 area map, 3 ending, 4 game over
    u32 _14;
};

struct StatusMenu {
    u32 layout;             // +0x00
    u32 _04;                // +0x04  object: +4 s32, +0x12 s16
    u8  _08[0xC];
    s32 page;               // +0x14
    u8  _18[0x14];
    u32 vtable;             // +0x2C
    u8  _30[0x4C];
};

struct GameplayScene {
    u32 gsDevice;           // +0x00  (scene base, 0x30 bytes)
    u32 padMgr;             // +0x04
    u32 _08, _0c, _10;
    u32 playerData;         // +0x14
    u32 config;             // +0x18
    u32 renderMgr;          // +0x1C
    u32 hud;                // +0x20
    u32 _24;
    u32 hasQueuedScene;     // +0x28
    u32 vtable;             // +0x2C

    s32 mode;               // +0x30
    s32 previousMode;       // +0x34
    s32 returnMode;         // +0x38  where sword details go back to
    s32 nextMode;           // +0x3C
    s32 statusPage;         // +0x40
    s32 statusDone;         // +0x44
    s32 statusWait;         // +0x48  waiting for the character to reload after a sword change
    s32 fadeTimer;          // +0x4C
    s32 phase;              // +0x50
    s32 _54;
    s32 area;               // +0x58
    s32 mapArea;            // +0x5C  area the map cursor is on
    s32 newDay;             // +0x60
    s32 dayEnding;          // +0x64
    u32 dayFlags;           // +0x68  1 morning message pending, 0x10 show it, 0x20 …
    s32 inEvent;            // +0x6C  the field is running a script
    s32 arrested;           // +0x70
    s32 pauseShown;         // +0x74
    s32 eventLocked;        // +0x78
    s32 softReset;          // +0x7C
    s32 paused;             // +0x80
    s32 pauseWanted;        // +0x84
    s32 toStaffRoll;        // +0x88
    s32 dayEndShown;        // +0x8C
    s32 savedBgm;           // +0x90
    s32 bannerState;        // +0x94  1 fading in, 2 shown, 3 fading out
    s32 bannerTimer;        // +0x98
    s32 bannerKind;         // +0x9C
    s32 bannerAlpha;        // +0xA0
    s32 cheatStep;          // +0xA4
    s32 cheatTimer;         // +0xA8
    s32 playerDead;         // +0xAC
    s32 resumeBgm;          // +0xB0
    s32 statusFromField;    // +0xB4
    s32 hintShown;          // +0xB8
    u32 world;              // +0xBC
    u32 areaMap;            // +0xC0
    u32 settings;           // +0xC4
    u32 spawner;            // +0xC8
    u32 exits;              // +0xCC
    u32 statusMenu;         // +0xD0
    u32 events;             // +0xD4
    u32 swordInfo;          // +0xD8
    u32 itemDrops;          // +0xDC
    u32 saveScreen;         // +0xE0
    u32 pauseMenu;          // +0xE4
    u32 swordSelect;        // +0xE8
    u32 itemSelect;         // +0xEC
};

extern "C" {
    GameplayScene* GameplayScene_Ctor(GameplayScene* self);                 // 0x2B2230
    GameplayScene* GameplayScene_Destroy(GameplayScene* self, s16 flag);    // 0x2B1FF0
    int  GameplayScene_Start(GameplayScene* self);                          // 0x2B1B80
    int  GameplayScene_Update(GameplayScene* self);                         // 0x2B10F0
    int  GameplayScene_Draw(GameplayScene* self);                           // 0x2ADAE0
    Scene* GameplayScene_GetNextScene(GameplayScene* self);                 // 0x2ADA70
    s32  GameplayScene_GetNextSceneId(GameplayScene* self);                 // 0x2ADA00

    // ── Not decompiled yet (src/native/hle_gameplay.cpp) ──
    // Event (script) player (0x1E2E20–0x1E4DF0)
    EventPlayer* EventPlayer_Ctor(EventPlayer* mem);                        // 0x1E4DF0
    void   EventPlayer_Reset(EventPlayer* self, int a);                     // 0x1E3810
    EventPlayer* EventPlayer_Dtor(EventPlayer* self, s16 flag);             // 0x1E4D50
    void   EventPlayer_StartDay(EventPlayer* self, int a, int b);           // 0x1E3450
    void   EventPlayer_StartScript(EventPlayer* self, const char* script, int a);   // 0x1E3460
    int    EventPlayer_CanSkip(EventPlayer* self);                          // 0x1E3160
    void   EventPlayer_Skip(EventPlayer* self);                             // 0x1E2E20
    int    EventPlayer_Update(EventPlayer* self, int a);                    // 0x1E3790
    const char* EventPlayer_ProbeStory(int map);                            // (debug aid, not game code)
    void   EventPlayer_Finish();                                            // 0x1E3350

    // Status menu (0x210E20–0x211DE0)
    StatusMenu* StatusMenu_Ctor(StatusMenu* mem);                           // 0x211DE0
    void   StatusMenu_Start(StatusMenu* self, const void* info, int page, int pauseShown,
                            int noWorld, int a, int b);                     // 0x211A70
    void   StatusMenu_Draw(StatusMenu* self);                               // 0x210E20
    s32*   StatusMenu_Update(StatusMenu* self);                             // 0x211350  {action, argument} or null
    void   StatusMenu_SetPage(StatusMenu* self, int page);                  // 0x211930
    int    StatusMenu_SwordChanged(StatusMenu* self);                       // 0x211080
    int    StatusMenu_GetSword(StatusMenu* self);                           // 0x2110B0

    // The rest
    void*  SwordInfo_Ctor(void* mem);                                       // 0x2379C0
    void   SwordInfo_Dtor(void* self, int flag);                            // 0x237950
    void   SwordInfo_Show(void* self, int sword);                           // 0x237930
    int    SwordInfo_Start(void* self, int sword);                          // 0x237840  (a guest address)
    void   SwordInfo_Update(void* self);                                    // 0x237700
    void   SwordInfo_Draw(void* self);                                      // 0x237330
    void*  SaveScreen_Ctor(void* mem, int mode);                            // 0x260050
    void   SaveScreen_Start(void* self);                                    // 0x25FF80
    int    SaveScreen_Update(void* self);                                   // 0x25E550
    void   Mode0_Draw(void* self);                                          // 0x262470
    void*  SwordSelect_Ctor(void* mem);                                     // 0x26F320
    char   SwordSelect_Start(void* self, int mode);                         // 0x26F150
    void   SwordSelect_Load(void* self);                                    // 0x26EE90
    void   SwordSelect_Update(s32 out[2], void* self);                      // 0x26F040
    void   SwordSelect_Draw(void* self);                                    // 0x26EA40
    void   SwordSelect_Dtor(void* self, int flag);                          // 0x26F270
    void*  Spawner_Ctor(void* mem);                                         // 0x1863F0  0x58 bytes, actor list at +0x24
    void   Spawner_Dtor(void* self, int flag);                              // 0x186250
    void   Spawner_Load(void* self);                                        // 0x184B10
    void   Spawner_Update(void* self, World* world, void* exits);           // 0x1848B0
    void   Spawner_ReleaseAll();                                            // 0x186060
    void*  TaskMgr_Ctor(void* mem);                                         // 0x1C0030  0x58 bytes
    void   TaskMgr_Dtor(void* self, int flag);                              // 0x1BFC20
    void   TaskMgr_Add(void* self, u32 actor, u32 a, u32 b);                // 0x1BF960
    int    TaskMgr_Update(void* self, World* world, void* spawner);         // 0x1BE5E0  exit used, -1 none, 0x80 arrested
    void   TaskMgr_Reset(void* self, World* world);                         // 0x1BFA30
    void   TaskMgr_ReleaseAll();                                            // 0x1BFFE0
    void   ItemDrops_Drop(void* self, int item, const void* matrix);        // 0x21C0E0
    void   SwordDb_DropSword(void* db, int sword, const void* matrix);      // 0x15C290
    char   SwordDb_PollLoads(void* db);                                     // 0x15D800
    void*  Hud2_Ctor(void* mem);                                            // 0x2A6670  0x22C bytes, global *0x2EE690
    void   Hud2_Set(void* hud);                                             // 0x1E06C0
    void   CharaResources_Release();                                        // 0x1D6D80
    void*  SkillSheets_Get();                                               // 0x23DC20
    void   SkillSheets_Reset(void* self);                                   // 0x23DB90
    void   Voice_Free();                                                    // 0x17EC30
    void   Voice_Play(int id, int positional, int volume, int a);           // 0x1800F0
    void   Se_Resume();                                                     // 0x180C50
    void   Se_Pause();                                                      // 0x180C60
    void   Hud_Draw(void* hud);                                             // 0x21D9E0
    void   Hud_ClearChoice(void* hud);                                      // 0x21D4D0
    void   Hud_ShowDate(void* hud, const u16* text, int a, int b);          // 0x21D510
    void   Hud_HideHint(void* hud);                                         // 0x21D6E0
    void   Hud_ShowHint(void* hud, const u16* text, int a);                 // 0x21D720
    void   Hud_ShowAreaName(void* hud, const u16* text, int frames, int a); // 0x21D7E0
    void   Hud_ShowNotice(void* hud, const u16* text, int frames, int a);   // 0x21D630
    int    Hud_MessageBusy(void* hud);                                      // 0x21C900
    void   Hud_CloseMessage(void* hud);                                     // 0x21C950
    void   Hud_Reset(void* hud);                                            // 0x21D8F0
    void   Hud_Load(void* hud);                                             // 0x21DD10
    void   Hud_OpenMessage(void* hud);                                      // 0x21C990
    void   Hud_Update(void* hud);                                           // 0x21DAB0
}
