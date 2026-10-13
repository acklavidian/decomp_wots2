#pragma once
#include "hal/ps2_types.h"
#include "engine/scene.h"

// ── Scene base class (constructor 0x1E25F0, vtable 0x2EBB00) ──────────────
// Every scene starts with cached pointers to the engine singletons.
struct SceneBase {
    u32 gsDevice;        // +0x00  *0x2EE670
    u32 padMgr;          // +0x04  PadMgr_Get()
    u32 profiler;        // +0x08  *0x2EE6B0
    u32 font;            // +0x0C  *0x2EE67C
    u32 windowMgr;       // +0x10  *0x2EE680
    u32 playerData;      // +0x14  *0x2EE688
    u32 config;          // +0x18  *0x2EE68C  (first word: SE volume setting)
    u32 renderMgr;       // +0x1C  *0x2EE684
    u32 field_20;        // +0x20  *0x2EE69C
    u32 softResetHold;   // +0x24  frames the soft-reset combo has been held
    u32 hasQueuedScene;  // +0x28
    u32 vtable;          // +0x2C
};
static_assert(sizeof(SceneBase) == 0x30, "SceneBase size");

// ── GameScene: scene 22, the title / front end (104 bytes) ────────────────
// One "mode" object is alive at a time; switching modes fades out, frees the
// old object, creates the new one and fades back in.
enum GameSceneMode : u32 {
    kModeUnknown0   = 0,  // 0x150E8-byte object (same class Results uses)
    kModeUnknown1   = 1,  // 0x15158-byte object; sets `mode1Result` on exit
    kModeTitleMenu  = 2,  // 0x1CC-byte object — the scene starts here
    kModeNewGame    = 3,  // ScriptGroupB (0x5AC bytes) + its render node
    kModeRecords    = 4,  // "Records" menu entry: 0x1D4-byte object
    kModeSettings   = 5,  // "Settings" menu entry: 0x56C-byte object
    kModeNone       = 6,
    kModeExit       = 7,  // as nextMode only: leave the scene
};

enum GameScenePhase : u32 {
    kPhaseBegin     = 0,  // show loading text, screen black
    kPhaseLoading   = 1,  // mode object still loading
    kPhaseLoaded    = 2,
    kPhaseFadeIn    = 3,  // hide loading text, start fade-in
    kPhaseRunning   = 4,
    kPhaseLeave     = 5,  // start fade-out
    kPhaseFadeOut   = 7,  // wait, then switch to nextMode or end the scene
};

struct GameScene {
    SceneBase base;
    u32 mode;            // +0x30  GameSceneMode
    u32 nextMode;        // +0x34  GameSceneMode to switch to after the fade-out
    u32 phase;           // +0x38  GameScenePhase
    u32 fadeFrames;      // +0x3C  frames spent in kPhaseFadeOut
    u32 mode1Result;     // +0x40  0 / 1 from mode 1; changes how the title menu restarts
    u32 startGame;       // +0x44  1: next scene is Gameplay
    u32 goAttract;       // +0x48  1: next scene is Attract (title menu timed out)
    u32 charaMgr;      // +0x4C  0x120-byte render node used by the new-game screen
    u32 titleMenu;       // +0x50  mode 2 object
    u32 newGame;         // +0x54  mode 3 object (ScriptGroupB*)
    u32 mode4Obj;        // +0x58
    u32 settings;        // +0x5C  SettingsScreen*
    u32 mode0Obj;        // +0x60  has a vtable; destroyed through slot +8
    u32 mode1Obj;        // +0x64  has a vtable; destroyed through slot +8
};
static_assert(sizeof(GameScene) == 104, "GameScene size");

GameScene* GameScene_Ctor(GameScene* self);                 // 0x275380
GameScene* GameScene_Destroy(GameScene* self, s16 flag);    // 0x275230
int        GameScene_Start(GameScene* self);                // 0x2751D0
int        GameScene_Update(GameScene* self);               // 0x274BD0
int        GameScene_Draw(GameScene* self);                 // 0x2744A0
Scene*     GameScene_GetNextScene(GameScene* self);         // 0x274490
s32        GameScene_GetNextSceneId(GameScene* self);       // 0x274430
