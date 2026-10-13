// Decompiled from 0x274430 – 0x27541C
//
// GameScene — scene 22, the title screen / front end.  Reached from the Boot
// scene; leaves for Gameplay (start game), Attract (menu timed out) or Boot
// (soft reset).
//
// The scene owns one "mode" object at a time (title menu, new-game setup, …)
// and wraps every mode in the same phase machine:
//
//   Begin → Loading → Loaded → FadeIn → Running → Leave → FadeOut → next mode
//
// GameScene_PreUpdateCheck advances the phases that do not depend on the mode;
// GameScene_UpdateMode advances Loading → Loaded when the mode object reports
// it is ready, and Running → Leave when it returns a result.

#include "game/game_scene.h"
#include "game/player_data.h"
#include "game/settings_screen.h"
#include "game/chara_setup.h"

// ── External functions ─────────────────────────────────────────────────────
struct CharaMgr;
extern "C" {
    void* MemAlloc(u32 size);                               // 0x100820
    void  MemFree(void* p);                                 // 0x100650
    void  MemSet(void* dst, int value, u32 size);           // 0x123880

    void* Scene_BaseCtor(void* self);                       // 0x1E25F0
    void* Scene_BaseDtor(void* self, int flag);             // 0x1E2580
    int   Scene_CheckSoftReset(void* self);                 // 0x1E24C0  combo held for 2 s
    void  fn_1656f0();                                      // 0x1656F0  empty

    void  Bgm_StopAll(int fade);                            // 0x180350  0 = stop now
    void  Bgm_Play(int id, int a, int b, int volume);       // 0x180A80  ids 0–36
    void  Sound_PlaySE(u32 id, u32 randomPitch, u32 volume);// 0x180E70

    void  RenderMgr_TriggerEffect(void* mgr, u32 type, u32 arg1, u32 arg2);  // 0x14F190
    void  RenderMgr_UpdateEffects(void* mgr);               // 0x14F690

    void* fn_1d6db0();                                      // 0x1D6DB0
    void  fn_1d6a80(void* obj, int arg);                    // 0x1D6A80
    void  fn_1d6d80();                                      // 0x1D6D80
    void  Display_ApplyOffset(int arg);                               // 0x1E0450
    void  Config_SetDefaults(void* config);                          // 0x183700
    void  Sound_ApplyConfig();                                      // 0x17FFA0

    // Mode 0
    void* Mode0_Ctor(void* mem);                            // 0x260EB0
    void  Mode0_Start(void* obj);                           // 0x260E20
    int   Mode0_Update(void* obj);                          // 0x260100  → 0 when finished
    void  Mode0_Draw(void* obj);                            // 0x262470
    // Mode 1
    void* Mode1_Ctor(void* mem);                            // 0x25E270
    void  Mode1_Start(void* obj);                           // 0x25DD70
    int   Mode1_Update(void* obj);                          // 0x257550  → 0 when finished
    void  Mode1_Draw(void* obj);                            // 0x257000
    // Mode 2: title menu
    void* TitleMenu_Ctor(void* mem);                        // 0x26D280
    char  TitleMenu_Load(void* obj);                        // 0x26D0D0
    void  TitleMenu_Restart(void* obj);                     // 0x26CE60
    void  TitleBg_Reset(void* bg);                          // 0x26E0A0
    int   TitleMenu_Update(void* obj);                      // 0x26CCC0  → 0, or a result code
    void  TitleMenu_Draw(void* obj);                        // 0x26BF40
    void  TitleMenu_Dtor(void* obj, int flag);              // 0x26D180
    // Mode 3: new-game setup
    CharaMgr* CharaMgr_Ctor(void* mem);                     // 0x1471C0
    void  CharaMgr_Init(CharaMgr* mgr, int arg);            // 0x146EC0
    void  CharaMgr_Dtor(CharaMgr* mgr, int flag);           // 0x146F50
    // Mode 4
    void* Mode4_Ctor(void* mem);                            // 0x2A72F0
    char  Mode4_Load(void* obj);                            // 0x2A7150
    int   Mode4_Update(void* obj);                          // 0x2A6FC0
    void  Mode4_Draw(void* obj);                            // 0x2A6B80
    void  Mode4_Dtor(void* obj, int flag);                  // 0x2A7200
    // Mode 5
}

// ── Globals ───────────────────────────────────────────────────────────────
static const GuestPtrVar<u32> gPlayerData{0x2EE688u};
static const GuestPtrVar<void> gRenderMgrPtr{0x2EE684u};
static u32* const   gLoadingActive   = reinterpret_cast<u32*>(0x2EE6B4u);
static u32* const   gLoadingDotTimer = reinterpret_cast<u32*>(0x2EE6B8u);
static u32* const   gLoadingDots     = reinterpret_cast<u32*>(0x2EE6BCu);
static u32* const   gLoadingFrames   = reinterpret_cast<u32*>(0x2EE6C0u);
static s32* const   gFadeOutFrames   = reinterpret_cast<s32*>(0x2EE76Cu);

static const u32 kGameSceneVtable = 0x2ECDB0u;

// Render effect 11 is the full-screen fade.
enum : u32 { kEffectFade = 11, kFadeIn = 0, kFadeOut = 1, kFadeBlack = 2 };

template <typename T = void>
static T* P(u32 addr) { return reinterpret_cast<T*>(static_cast<uintptr_t>(addr)); }
static u32 A(void* p) { return static_cast<u32>(reinterpret_cast<uintptr_t>(p)); }

// Objects with a vtable at +0: slot +8 is the deleting destructor.
static void DestroyVirtual(u32& obj) {
    if (obj) {
        u32 vt = *P<u32>(obj);
        GuestCall<void, void*, int>(*P<u32>(vt + 8), P(obj), 1);
    }
    obj = 0;
}

static void LeaveMode(GameScene* self, u32 nextMode) {
    self->phase    = kPhaseLeave;
    self->nextMode = nextMode;
}

static void LeaveForSoftReset(GameScene* self) {
    self->nextMode  = kModeExit;
    self->phase     = kPhaseLeave;
    self->startGame = 0;
}

// ── 0x275380 GameScene_Ctor ───────────────────────────────────────────────
GameScene* GameScene_Ctor(GameScene* self) {
    Scene_BaseCtor(self);
    self->base.vtable = kGameSceneVtable;
    fn_1656f0();
    self->mode0Obj = 0;
    self->mode1Obj = 0;
    self->charaMgr = 0;
    self->titleMenu = 0;
    self->newGame = 0;
    self->mode4Obj = 0;
    self->settings = 0;
    self->mode = kModeTitleMenu;
    self->phase = kPhaseBegin;
    self->mode1Result = 0;
    self->startGame = 0;
    self->goAttract = 0;
    fn_1d6a80(fn_1d6db0(), 0);
    PlayerData_Reset(guest::ptr<PlayerData>(self->base.playerData), 1);
    Display_ApplyOffset(1);
    Config_SetDefaults(P(self->base.config));
    return self;
}

// ── 0x274D40 GameScene_StartTitleMenu ─────────────────────────────────────
// The title menu object is kept across mode switches, so this also handles
// coming back to it.
static int GameScene_StartTitleMenu(GameScene* self) {
    u32* menu = P<u32>(self->titleMenu);
    if (!menu) {
        void* mem = MemAlloc(0x1CC);
        self->titleMenu = mem ? A(TitleMenu_Ctor(mem)) : 0;
        if (!self->titleMenu)
            return 0;
        if (!TitleMenu_Load(P(self->titleMenu)))
            return 0;
        Bgm_Play(0, 1, 0, 0x3FFF);
    } else if (self->mode1Result == 1) {
        Bgm_StopAll(0);
        Bgm_Play(0, 1, 0, 0x3FFF);
        TitleMenu_Restart(menu);
    } else if (self->mode1Result == 0) {
        menu[0] = 0;
        *reinterpret_cast<u16*>(menu + 6) = 0;
        TitleBg_Reset(menu + 14);
    }
    return 1;
}

// ── 0x274C10 GameScene_StartNewGame ───────────────────────────────────────
static int GameScene_StartNewGame(GameScene* self) {
    u8* player = reinterpret_cast<u8*>(*gPlayerData);
    if (*reinterpret_cast<s16*>(player + 0x178) == 0 &&
        *reinterpret_cast<s16*>(player + 0x6C80) == 0) {
        // Nothing owned: the player gets a new sword of the starting kind.
        s16 id = *reinterpret_cast<s16*>(player + 0x118);
        Sword sword;
        Sword_Ctor(&sword);
        SwordDb_MakeSword(SwordDb_Get(), id, &sword);
        sword.number = 1;
        PlayerData_CarrySword(reinterpret_cast<PlayerData*>(player), &sword);
        Sword_Dtor(&sword, -1);
    }

    void* mem = MemAlloc(sizeof(CharaSetup));
    self->newGame = mem ? A(CharaSetup_Ctor(static_cast<CharaSetup*>(mem))) : 0;
    if (!self->newGame)
        return 0;

    if (!self->charaMgr) {
        mem = MemAlloc(0x120);
        self->charaMgr = mem ? A(CharaMgr_Ctor(mem)) : 0;
        if (!self->charaMgr)
            return 1;
        CharaMgr_Init(P<CharaMgr>(self->charaMgr), 0);
    }

    u32 arg;
    MemSet(&arg, 0, 4);
    arg = self->charaMgr;
    CharaSetup_Attach(P<CharaSetup>(self->newGame), &arg);
    return 1;
}

// ── 0x274F90 GameScene_CreateMode ─────────────────────────────────────────
// Creates the object for `self->mode`.  Returns 0 on failure.
int GameScene_CreateMode(GameScene* self) {
    self->phase = kPhaseBegin;

    switch (self->mode) {
    case kModeUnknown0: {
        Bgm_StopAll(0);
        void* mem = MemAlloc(0x150E8);
        self->mode0Obj = mem ? A(Mode0_Ctor(mem)) : 0;
        if (!self->mode0Obj)
            return 0;
        Mode0_Start(P(self->mode0Obj));
        break;
    }
    case kModeUnknown1: {
        Bgm_StopAll(0);
        void* mem = MemAlloc(0x15158);
        self->mode1Obj = mem ? A(Mode1_Ctor(mem)) : 0;
        if (!self->mode1Obj)
            return 0;
        Mode1_Start(P(self->mode1Obj));
        break;
    }
    case kModeTitleMenu:
        Bgm_StopAll(1);
        return GameScene_StartTitleMenu(self);

    case kModeNewGame:
        Bgm_StopAll(1);
        Bgm_Play(24, 1, 0, 0x3FFF);
        return GameScene_StartNewGame(self);

    case kModeRecords: {
        Bgm_StopAll(1);
        Bgm_Play(24, 1, 0, 0x3FFF);
        void* mem = MemAlloc(0x1D4);
        self->mode4Obj = mem ? A(Mode4_Ctor(mem)) : 0;
        if (!self->mode4Obj)
            return 0;
        if (!Mode4_Load(P(self->mode4Obj)))
            return 0;
        break;
    }
    case kModeSettings: {
        Bgm_StopAll(1);
        Bgm_Play(24, 1, 0, 0x3FFF);
        void* mem = MemAlloc(sizeof(SettingsScreen));
        self->settings = mem ? A(SettingsScreen_Ctor(static_cast<SettingsScreen*>(mem))) : 0;
        if (!self->settings)
            return 0;
        if (!SettingsScreen_Load(P<SettingsScreen>(self->settings))) {
            if (self->settings)
                SettingsScreen_Dtor(P<SettingsScreen>(self->settings), 1);
            self->settings = 0;
            return 0;
        }
        SettingsScreen_Start(P<SettingsScreen>(self->settings));
        return 1;
    }
    }
    return 1;
}

// ── 0x274E50 GameScene_DestroyMode ────────────────────────────────────────
// Frees the current mode's object.  The title menu (mode 2) is kept.
static int GameScene_DestroyMode(GameScene* self) {
    switch (self->mode) {
    case kModeUnknown0:
        DestroyVirtual(self->mode0Obj);
        break;
    case kModeUnknown1:
        DestroyVirtual(self->mode1Obj);
        Display_ApplyOffset(0);
        Sound_ApplyConfig();
        break;
    case kModeNewGame:
        if (self->newGame)
            CharaSetup_Dtor(P<CharaSetup>(self->newGame), 1);
        self->newGame = 0;
        if (self->charaMgr)
            CharaMgr_Dtor(P<CharaMgr>(self->charaMgr), 1);
        self->charaMgr = 0;
        break;
    case kModeRecords:
        if (self->mode4Obj)
            Mode4_Dtor(P(self->mode4Obj), 1);
        self->mode4Obj = 0;
        break;
    case kModeSettings:
        if (self->settings)
            SettingsScreen_Dtor(P<SettingsScreen>(self->settings), 1);
        self->settings = 0;
        break;
    }
    return 1;
}

// ── 0x2751D0 GameScene_Start ──────────────────────────────────────────────
// Scene vtable +0x0C.  Returns 0 on success.
int GameScene_Start(GameScene* self) {
    SwordDb_Get();
    u32* player = *gPlayerData;
    PlayerData_Reset(reinterpret_cast<PlayerData*>(player), 0);
    player[0] = 1;
    return (GameScene_CreateMode(self) & 0xFF) == 0;
}

// ── 0x274A40 GameScene_PreUpdateCheck ─────────────────────────────────────
// Mode-independent half of the phase machine.  Returns 0 when the scene ends.
int GameScene_PreUpdateCheck(GameScene* self) {
    switch (self->phase) {
    case kPhaseBegin:
        if (!*gLoadingActive) {
            *gLoadingFrames = 0;
            *gLoadingDots = 0;
            *gLoadingDotTimer = 0;
        }
        *gLoadingActive = 1;
        RenderMgr_TriggerEffect(*gRenderMgrPtr, kEffectFade, kFadeBlack, 0);
        self->phase = kPhaseLoading;
        break;

    case kPhaseLoaded:
        self->phase = kPhaseFadeIn;
        break;

    case kPhaseFadeIn:
        *gLoadingFrames = 0;
        *gLoadingDots = 0;
        *gLoadingDotTimer = 0;
        *gLoadingActive = 0;
        RenderMgr_TriggerEffect(*gRenderMgrPtr, kEffectFade, kFadeIn, 0);
        self->phase = kPhaseRunning;
        break;

    case kPhaseLeave:
        self->fadeFrames = 0;
        self->phase = kPhaseFadeOut;
        RenderMgr_TriggerEffect(*gRenderMgrPtr, kEffectFade, kFadeOut, 0);
        break;

    case kPhaseFadeOut:
        if ((s32)self->fadeFrames++ > *gFadeOutFrames) {
            if (self->startGame || self->nextMode == kModeExit)
                return 0;
            GameScene_DestroyMode(self);
            self->mode = self->nextMode;
            if (!(GameScene_CreateMode(self) & 0xFF))
                return 0;
        }
        break;

    default:  // kPhaseLoading, kPhaseRunning: driven by GameScene_UpdateMode
        break;
    }

    RenderMgr_UpdateEffects(*gRenderMgrPtr);
    return 1;
}

// ── 0x274650 GameScene_UpdateTitleMenu ────────────────────────────────────
static int GameScene_UpdateTitleMenu(GameScene* self) {
    if (self->phase == kPhaseLoading) {
        TitleMenu_Update(P(self->titleMenu));
        if (P<u32>(self->titleMenu)[1] == 5)
            self->phase = kPhaseLoaded;
    } else if (self->phase == kPhaseRunning) {
        if (Scene_CheckSoftReset(self)) {
            LeaveForSoftReset(self);
            return 1;
        }
        int result = TitleMenu_Update(P(self->titleMenu));
        if (result != 0) {
            LeaveMode(self, kModeTitleMenu);
            switch (result) {
            case 1:  // timed out
                self->nextMode  = kModeExit;
                self->startGame = 0;
                self->goAttract = 1;
                break;
            case 2:
                self->nextMode = kModeUnknown1;
                break;
            case 3:
                if ((*gPlayerData)[0] == 2)
                    self->startGame = 1;
                else
                    self->nextMode = kModeNewGame;
                break;
            case 7:
                self->nextMode = kModeRecords;
                break;
            case 8:
                self->nextMode = kModeSettings;
                break;
            }
        }
    }
    return 1;
}

// ── 0x274540 GameScene_UpdateNewGame (was ScriptGroup_ObjectUpdate) ───────
static int GameScene_UpdateNewGame(GameScene* self) {
    CharaSetup* setup = P<CharaSetup>(self->newGame);
    if (self->phase == kPhaseLoading) {
        CharaSetup_Update(setup);
        if (setup->phase == 3)
            self->phase = kPhaseLoaded;
    } else if (self->phase == kPhaseRunning) {
        if (Scene_CheckSoftReset(self)) {
            LeaveForSoftReset(self);
            return 1;
        }
        int result = CharaSetup_Update(setup);
        if (result == 1) {            // cancelled: back to the title menu
            LeaveMode(self, kModeTitleMenu);
        } else if (result == 2) {     // confirmed: start the game
            self->phase = kPhaseLeave;
            self->startGame = 1;
        }
    }
    return 1;
}

// ── 0x2747D0 GameScene_UpdateMode ─────────────────────────────────────────
int GameScene_UpdateMode(GameScene* self) {
    switch (self->mode) {
    case kModeUnknown0:
        if (self->phase == kPhaseLoading)
            self->phase = kPhaseLoaded;
        if (self->phase == kPhaseRunning && Mode0_Update(P(self->mode0Obj)) == 0)
            LeaveMode(self, kModeTitleMenu);
        return 1;

    case kModeUnknown1:
        if (self->phase == kPhaseLoading)
            self->phase = kPhaseLoaded;
        if (self->phase == kPhaseRunning && Mode1_Update(P(self->mode1Obj)) == 0) {
            u8 r = *P<u8>(self->mode1Obj + 0x15151);
            if (r == 1) {
                LeaveMode(self, kModeTitleMenu);
                self->mode1Result = 1;
            } else if (r == 0) {
                LeaveMode(self, kModeTitleMenu);
                self->mode1Result = 0;
            }
        }
        return 1;

    case kModeTitleMenu:
        return GameScene_UpdateTitleMenu(self);

    case kModeNewGame:
        return GameScene_UpdateNewGame(self);

    case kModeRecords:
        if (self->phase == kPhaseLoading) {
            Mode4_Update(P(self->mode4Obj));
            if (*P<u32>(self->mode4Obj) == 5)
                self->phase = kPhaseLoaded;
        } else if (self->phase == kPhaseRunning && Mode4_Update(P(self->mode4Obj)) != 0) {
            LeaveMode(self, kModeTitleMenu);
        }
        return 1;

    case kModeSettings:
        if (self->phase == kPhaseLoading) {
            SettingsScreen_Update(P<SettingsScreen>(self->settings));
            if (P<SettingsScreen>(self->settings)->phase == kSettingsReady)
                self->phase = kPhaseLoaded;
        } else if (self->phase == kPhaseRunning && SettingsScreen_Update(P<SettingsScreen>(self->settings)) == 0) {
            LeaveMode(self, kModeTitleMenu);
        }
        return 1;

    default:
        return 1;
    }
}

// ── 0x274BD0 GameScene_Update ─────────────────────────────────────────────
// Scene vtable +0x10.  Returns 0 when the scene has finished.
int GameScene_Update(GameScene* self) {
    if (!GameScene_PreUpdateCheck(self))
        return 0;
    return GameScene_UpdateMode(self);
}

// ── 0x2744A0 GameScene_Draw ───────────────────────────────────────────────
// Scene vtable +0x14.
int GameScene_Draw(GameScene* self) {
    switch (self->mode) {
    case kModeUnknown0:  Mode0_Draw(P(self->mode0Obj));          break;
    case kModeUnknown1:  Mode1_Draw(P(self->mode1Obj));          break;
    case kModeTitleMenu: TitleMenu_Draw(P(self->titleMenu));     break;
    case kModeNewGame:   CharaSetup_Draw(P<CharaSetup>(self->newGame));    break;
    case kModeRecords:  Mode4_Draw(P(self->mode4Obj));          break;
    case kModeSettings:  SettingsScreen_Draw(P<SettingsScreen>(self->settings));          break;
    }
    return 0;
}

// ── 0x274490 GameScene_GetNextScene ───────────────────────────────────────
Scene* GameScene_GetNextScene(GameScene*) {
    return nullptr;
}

// ── 0x274430 GameScene_GetNextSceneId ─────────────────────────────────────
s32 GameScene_GetNextSceneId(GameScene* self) {
    if (self->startGame == 1) {
        Bgm_StopAll(1);
        return kSceneGameplay;
    }
    if (self->goAttract == 0)
        return kSceneBoot;
    Bgm_StopAll(0);
    return kSceneAttract;
}

// ── 0x275230 GameScene_Destroy ────────────────────────────────────────────
// Scene vtable +0x08.
GameScene* GameScene_Destroy(GameScene* self, s16 flag) {
    if (!self)
        return self;

    self->base.vtable = kGameSceneVtable;
    DestroyVirtual(self->mode0Obj);
    DestroyVirtual(self->mode1Obj);
    if (self->charaMgr) CharaMgr_Dtor(P<CharaMgr>(self->charaMgr), 1);
    self->charaMgr = 0;
    if (self->titleMenu) TitleMenu_Dtor(P(self->titleMenu), 1);
    self->titleMenu = 0;
    if (self->newGame) CharaSetup_Dtor(P<CharaSetup>(self->newGame), 1);
    self->newGame = 0;
    if (self->mode4Obj) Mode4_Dtor(P(self->mode4Obj), 1);
    self->mode4Obj = 0;
    if (self->settings) SettingsScreen_Dtor(P<SettingsScreen>(self->settings), 1);
    self->settings = 0;

    if (self->startGame == 0) {
        fn_1d6d80();
        SwordDb_Release();
    }
    Scene_BaseDtor(self, 0);
    if (flag > 0)
        MemFree(self);
    return self;
}
