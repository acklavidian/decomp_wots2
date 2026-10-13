// Gameplay scene: the mode / phase machine — decompiled from 0x2ADA00–0x2B2230.
// See include/game/gameplay_scene.h and "Gameplay scene" in the project notes.

#include "game/gameplay_scene.h"
#include "game/chara_resources.h"
#include "game/item_select.h"
#include "game/map_db.h"
#include "game/player_data.h"
#include "game/settings_screen.h"
#include "engine/bgm.h"
#include "engine/config.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/text_label.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <string>

void App_PresentBlackFrames();                                  // 0x1E0660
extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void* Scene_BaseCtor(void* self);                           // 0x1E25F0
    void* Scene_BaseDtor(void* self, int flag);                 // 0x1E2580
    int   Scene_CheckSoftReset(void* self);                     // 0x1E24C0
    u16   Pad_GetHeld(void* mgr, int port);                     // 0x13A0E0
    u16   Pad_GetPressed(void* mgr, int port);                  // 0x13A0C0
    void  PadMgr_Poll(void* mgr);                               // 0x13A7B0
    void  PadMgr_Reset(void* mgr);                              // 0x13A820
    void  RenderMgr_TriggerEffect(void* mgr, u32 type, u32 arg1, u32 arg2);         // 0x14F190
    void  RenderMgr_UpdateEffects(void* mgr);                   // 0x14F5B0
    void  RenderMgr_ClearEffects(void* mgr);                    // 0x14FA80
    int   RenderMgr_FindEffect(void* mgr, u32 type);            // 0x14F0F0
    char  CharaMgr_IsReady(void* mgr);                          // 0x1441B0
    void  GsDevice_ClearScreenColor(GsDevice* dev, u32 rgba);   // 0x12F1F0
    void  Matrix_Copy(void* dst, const void* src);              // 0x1073C0
}

static_assert(sizeof(GameplayScene) == 0xF0 && offsetof(GameplayScene, world) == 0xBC, "GameplayScene");
static_assert(sizeof(StatusMenu) == 0x7C && sizeof(EventPlayer) == 0x18, "gameplay parts");

static const GuestPtrVar<void> gRenderMgr{0x2EE684u};
static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
static u32& gWorldLink = *guest::ptr<u32>(0x2EE690u);
static s32& gFrameRate      = *guest::ptr<s32>(0x2EDA00u);
static s32& gCurrentBgm     = *guest::ptr<s32>(0x2EDADCu);
static s32& gLoadingActive  = *guest::ptr<s32>(0x2EE6B4u);
static s32& gLoadingDotTimer = *guest::ptr<s32>(0x2EE6B8u);
static s32& gLoadingDots    = *guest::ptr<s32>(0x2EE6BCu);
static s32& gLoadingFrames  = *guest::ptr<s32>(0x2EE6C0u);
static u8* const gClearColor = guest::ptr<u8>(0x2EE6A8u);
static s32& gNoAreaBgm = *guest::ptr<s32>(0x2EE518u);

// Tunes: the area map by time of day, then each area by time of day.
static const s32* const kMapBgm  = guest::ptr<const s32>(0x2DB740u);
static const s32* const kAreaBgm = guest::ptr<const s32>(0x2DB748u);
// Button sequences entered in the pause menu (and one on the area map).
static const u32* const kCheatHealth = guest::ptr<const u32>(0x2DB7A0u);
static const u32* const kCheatLives  = guest::ptr<const u32>(0x2DB7C0u);
static const u32* const kCheatMoney  = guest::ptr<const u32>(0x2DB7E0u);
static const u32* const kCheatSword  = guest::ptr<const u32>(0x2DB800u);
static const u32* const kCheatMap    = guest::ptr<const u32>(0x2DB830u);
static const char* const kArrestScript = guest::ptr<const char>(0x2E87D0u);
static u16* const sBannerText  = guest::ptr<u16>(0x30D000u);
static u16* const sBannerText2 = guest::ptr<u16>(0x30D200u);
static u16* const sNoticeText  = guest::ptr<u16>(0x30D400u);     // 3200000
static u16* const sNoticeText2 = guest::ptr<u16>(0x30D600u);
static u16* const sDateText    = guest::ptr<u16>(0x30D800u);

template <typename T> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline PlayerData* Player(GameplayScene* s) { return P<PlayerData>(s->playerData); }
static inline u8* PlayerBytes(GameplayScene* s) { return P<u8>(s->playerData); }
static inline World* WorldOf(GameplayScene* s) { return P<World>(s->world); }
static inline PauseMenu* Pause(GameplayScene* s) { return P<PauseMenu>(s->pauseMenu); }
static inline EventPlayer* Events(GameplayScene* s) { return P<EventPlayer>(s->events); }
static inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }

static inline void RenderMgr_SetFrozen(u32 mgr, u32 frozen) { P<u32>(mgr)[0x6C / 4] = frozen; }      // 0x14F8C0
static inline void RenderMgr_SetPaused(u32 mgr, bool paused) { P<u32>(mgr)[0x68 / 4] = paused ? 2 : 0; }   // 0x14F8D0

static void ShowLoading() {
    if (gLoadingActive == 0) {
        gLoadingFrames = 0;
        gLoadingDots = 0;
        gLoadingDotTimer = 0;
    }
    gLoadingActive = 1;
}
static void HideLoading() {
    gLoadingFrames = 0;
    gLoadingDots = 0;
    gLoadingDotTimer = 0;
    gLoadingActive = 0;
}

// What the pause menu may do changes with the situation; the old value is kept.
static void SetPauseAllowed(GameplayScene* self, int allowed) {
    PauseMenu* menu = Pause(self);
    menu->allowedBefore = menu->allowed;
    menu->allowed = allowed;
}
static void SetPauseAllowedIfNot(GameplayScene* self, int allowed) {
    if (Pause(self)->allowed != allowed)
        SetPauseAllowed(self, allowed);
}

static void Leave(GameplayScene* self, int nextMode) {
    self->phase = kPlayLeaving;
    self->nextMode = nextMode;
}

// Inlined PlayerData setters that tally every change.
static void Player_SetHealth(PlayerData* pd, s16 value) {
    const s16 before = pd->health;
    pd->health = value;
    s16& lost = *reinterpret_cast<s16*>(reinterpret_cast<u8*>(pd) + 0x854);
    if (pd->health - before < 0)
        lost = (s16)(lost + abs(pd->health - before));
}
static void Player_SetMoney(PlayerData* pd, int value) {
    const s16 before = pd->_106;
    pd->_106 = (s16)value;
    int delta = pd->_106 - before;
    if (delta < 1) {
        int total = pd->_84e + abs(delta);
        pd->_84e = (u16)(total > 0xFFFF ? 0xFFFF : total);
    } else {
        int total = pd->_84c + delta;
        pd->_84c = (u16)(total > 0xFFFF ? 0xFFFF : total);
    }
}
static void Player_RefillLives(PlayerData* pd) {
    s16 lives = pd->_10a < 1 ? 0 : pd->_10a;
    if (pd->_10a <= lives)
        lives = pd->_10a;
    pd->_108 = lives;
}
// 0x1772D0
static int ClampIndex(int index, int count) { return (index < 0 || index >= count) ? 0 : index; }

// The player's parameter block (health at +0x40, maximum at +0x42).
static u8* PlayerParams() {
    return reinterpret_cast<u8*>(CharaResources_GetParams(CharaResources_Get(), 0));
}
static s16& ParamHealth(u8* params) { return *reinterpret_cast<s16*>(params + 0x40); }
static s16& ParamHealthMax(u8* params) { return *reinterpret_cast<s16*>(params + 0x42); }

// ── 0x2B2230 ──────────────────────────────────────────────────────────────
GameplayScene* GameplayScene_Ctor(GameplayScene* self) {
    Scene_BaseCtor(self);
    self->vtable = 0x2ED710;
    CharaResources_Load(CharaResources_Get(), 0);
    MapDb_Load(MapDb_Get(), 0);
    self->world = self->areaMap = self->settings = self->spawner = self->exits = 0;
    self->statusMenu = self->swordInfo = self->saveScreen = self->swordSelect = self->itemSelect = 0;
    void* mem = MemAlloc(sizeof(PauseMenu));
    self->pauseMenu = mem ? guest::addr(PauseMenu_Ctor(static_cast<PauseMenu*>(mem))) : 0;
    self->softReset = self->pauseWanted = self->paused = self->toStaffRoll = 0;
    self->playerDead = self->resumeBgm = self->statusFromField = self->hintShown = 0;
    self->eventLocked = 0;
    self->bannerState = self->bannerTimer = self->bannerKind = self->bannerAlpha = 0;

    PlayerData* pd = Player(self);
    int start = -1;
    if (pd->state == 2) {
        start = kPlayAreaMap;
    } else if (pd->state == 3 || pd->state == 1) {
        start = kPlayEvent;
        *reinterpret_cast<s16*>(PlayerBytes(self) + 0x86A) = pd->healthMax;
        *reinterpret_cast<s16*>(PlayerBytes(self) + 0x86C) = pd->_10a;
    } else if (pd->state == 0) {
        PlayerData_Reset(pd, 0);
        start = kPlayField;
    }
    if (start != -1)
        self->nextMode = self->mode = self->previousMode = self->returnMode = start;
    if (pd->difficulty == 3) {
        pd->_104 = 1;
        pd->difficulty = 1;
    }
    self->_54 = 0;
    self->fadeTimer = 0;
    self->dayFlags = 0;
    self->dayEndShown = 0;
    self->newDay = 0;
    self->dayEnding = 0;
    self->inEvent = 0;
    self->arrested = 0;
    self->area = (s8)pd->area;
    self->mapArea = (s8)pd->area;
    self->phase = kPlayBegin;
    self->pauseShown = 1;

    mem = MemAlloc(sizeof(EventPlayer));
    self->events = mem ? guest::addr(EventPlayer_Ctor(static_cast<EventPlayer*>(mem))) : 0;
    EventPlayer_Reset(Events(self), 0);
    mem = MemAlloc(0x80);
    self->itemDrops = mem ? guest::addr(ItemDrops_Ctor(mem, 0)) : 0;

    u8* params = PlayerParams();
    ParamHealthMax(params) = pd->healthMax;
    ParamHealth(params) = pd->health < ParamHealthMax(params) ? pd->health : ParamHealthMax(params);

    mem = MemAlloc(0x22C);
    Hud2_Set(mem ? Hud2_Ctor(mem) : nullptr);
    DayMgr_Get();
    return self;
}

static void ResetClearColor() {
    gClearColor[3] = 0x80;
    gClearColor[0] = gClearColor[1] = gClearColor[2] = 0;
}

static void DestroyWorld(GameplayScene* self) {
    ResetClearColor();
    RenderMgr_TriggerEffect(*gRenderMgr, 4, 0, 0);
    Bgm_StopAll(0);
    if (self->world)
        World_Dtor(WorldOf(self), 1);
    self->world = 0;
    RenderMgr_ClearEffects(*gRenderMgr);
    Voice_Free();
}

static void DestroyField(GameplayScene* self) {
    if (self->spawner)
        Spawner_Dtor(P<void>(self->spawner), 1);
    self->spawner = 0;
    if (self->exits)
        TaskMgr_Dtor(P<void>(self->exits), 1);
    self->exits = 0;
    DestroyWorld(self);
}

static void DestroyAreaMap(GameplayScene* self) {
    if (self->paused == 0)
        Bgm_StopFade(gCurrentBgm, 0);
    if (self->pauseMenu)
        Pause(self)->_18 = 0;
    if (self->areaMap)
        AreaMap_Dtor(P<AreaMap>(self->areaMap), 1);
    self->areaMap = 0;
}

static void DestroyStatus(GameplayScene* self) {
    memset(PlayerBytes(self) + 0x6D40, 0, 0x100);
    if (self->statusMenu)
        VirtualCall<void, StatusMenu*, int>(P<StatusMenu>(self->statusMenu)->vtable, 8,
                                            P<StatusMenu>(self->statusMenu), 1);
    self->statusMenu = 0;
}

// ── 0x2B1FF0 ──────────────────────────────────────────────────────────────
GameplayScene* GameplayScene_Destroy(GameplayScene* self, s16 flag) {
    if (!self)
        return self;
    self->vtable = 0x2ED710;
    SkillSheets_Reset(SkillSheets_Get());       // 0x23DC20, 0x23DB90
    DestroyField(self);
    DestroyAreaMap(self);
    if (self->settings)
        SettingsScreen_Dtor(P<SettingsScreen>(self->settings), 1);
    self->settings = 0;
    DestroyStatus(self);
    if (self->events)
        EventPlayer_Dtor(Events(self), 1);
    self->events = 0;
    if (self->itemDrops)
        ItemDrops_Dtor(P<void>(self->itemDrops), 1);
    self->itemDrops = 0;
    if (self->pauseMenu)
        PauseMenu_Dtor(Pause(self), 1);
    self->pauseMenu = 0;
    CharaResources_Release();
    MapDb_Release();
    SwordDb_Release();
    DayMgr_Release();
    if (gWorldLink)
        VirtualCall<void, void*, int>(*P<u32>(gWorldLink), 8, P<void>(gWorldLink), 1);
    gWorldLink = 0;
    Hud2_Set(nullptr);
    Spawner_ReleaseAll();
    TaskMgr_ReleaseAll();
    Scene_BaseDtor(self, 0);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x2B19E0 ──────────────────────────────────────────────────────────────
// An event world has no area of its own; chapter 5 keeps one for the finale.
static void GameplayScene_CreateWorld(GameplayScene* self) {
    if (self->world)
        return;
    u8* pd = PlayerBytes(self);
    self->playerDead = 0;
    self->area = (s8)pd[0x103];
    bool finale = false;
    void* mem = MemAlloc(sizeof(World));
    World* world = nullptr;
    if (self->mode == kPlayEvent) {
        world = mem ? World_Ctor(static_cast<World*>(mem)) : nullptr;
    } else if (self->area == 5 && (pd[0x75C] != 0 || pd[0x779] != 0)) {
        world = mem ? World_Ctor(static_cast<World*>(mem)) : nullptr;
        finale = true;
    } else {
        world = mem ? World_CtorArea(static_cast<World*>(mem), self->area) : nullptr;
    }
    self->world = guest::addr(world);
    world->itemDrops = self->itemDrops;
    World_Init(world);
    Events(self)->world = self->world;
    s16& chapterEnd = *reinterpret_cast<s16*>(pd + 0x11E);
    if (chapterEnd == 4) {
        memset(pd + 0x4A0, 0, 0x30);
        memset(pd + 0x4D0, 0, 0xF0);
        chapterEnd = 0;
    }
    if (finale)
        World_GoToMap(world, pd[0x102] == 0 ? 0x52 : 0x53);
}

// ── 0x2B14A0 ──────────────────────────────────────────────────────────────
static void GameplayScene_StartField(GameplayScene* self) {
    if (self->spawner == 0) {
        ShowLoading();
        PlayerData* pd = Player(self);
        for (int i = 0; i < 3; i++) {
            const s32 value = pd->_12c[i] < 1 ? 0 : pd->_12c[i];
            pd->_138[ClampIndex(i, 3)] = value;
        }
        if (pd->night == 0)
            RenderMgr_TriggerEffect(*gRenderMgr, 4, 1, 0);
        PadMgr_Poll(P<void>(self->padMgr));
        PadMgr_Reset(P<void>(self->padMgr));
        void* mem = MemAlloc(0x58);
        self->spawner = mem ? guest::addr(Spawner_Ctor(mem)) : 0;
        mem = MemAlloc(0x58);
        self->exits = mem ? guest::addr(TaskMgr_Ctor(mem)) : 0;
        Spawner_Load(P<void>(self->spawner));
        // Every actor the area starts with goes into the world; those with a
        // trigger box are registered as exits.
        GuestList* actors = reinterpret_cast<GuestList*>(P<u8>(self->spawner) + 0x24);
        for (GuestListNode* n = GuestList_Begin(actors); n != GuestList_End(actors); n = GuestList_Next(n)) {
            World_AddActor(WorldOf(self), P<ActorDesc>(n->value), -1, 0);
            const u32* actor = P<u32>(n->value);
            if (const u32* box = P<u32>(actor[0xF]))
                TaskMgr_Add(P<void>(self->exits), actor[0], box[0], box[1]);
        }
        World_Begin(WorldOf(self));
    }
    self->dayEnding = 0;
    WorldOf(self)->kind = 1;
    SetPauseAllowed(self, 0);
    RenderMgr_SetFrozen(self->renderMgr, 0);
}

// ── 0x2B1170 ──────────────────────────────────────────────────────────────
static void GameplayScene_StartAreaMap(GameplayScene* self) {
    PadMgr_Poll(P<void>(self->padMgr));
    PadMgr_Reset(P<void>(self->padMgr));
    if (self->areaMap)
        AreaMap_Dtor(P<AreaMap>(self->areaMap), 1);
    self->areaMap = 0;
    void* mem = MemAlloc(sizeof(AreaMap));
    self->areaMap = mem ? guest::addr(AreaMap_Ctor(static_cast<AreaMap*>(mem))) : 0;
    AreaMap* map = P<AreaMap>(self->areaMap);
    AreaMap_Load(map);
    SetPauseAllowed(self, 0);
    RenderMgr_SetFrozen(self->renderMgr, 1);

    // Some story moments leave only one place to go.
    u8* pd = PlayerBytes(self);
    u8 closed[11];
    memset(closed, 1, 10);
    closed[10] = 0;
    auto force = [&](int area) {
        closed[area] = 0;
        pd[0x103] = (u8)area;
        memcpy(map->closed, closed, sizeof(closed));
        if (self->mapArea != area && self->mapArea != 10)
            self->mapArea = (s8)pd[0x103];
    };
    const s8 chapter = (s8)pd[0x100];
    if (chapter == 5) {
        if (pd[0x7FE] == 10 && pd[0x78C] == 1)
            force(3);
        else
            force(4);
    } else if (chapter == 6 && (pd[0x81A] == 2 || pd[0x7FC] == 8)) {
        force(6);
    } else if (chapter == 9 && pd[0x7FC] == 'c' && pd[0x7FD] == 'c' && pd[0x7FE] == 'c' && pd[0x7FF] == 'c') {
        force(6);
    } else {
        memset(map->closed, 0, sizeof(map->closed));
    }
}

// ── 0x2B1670 ──────────────────────────────────────────────────────────────
static void GameplayScene_StartStatus(GameplayScene* self) {
    if (self->statusMenu == 0) {
        void* mem = MemAlloc(sizeof(StatusMenu));
        self->statusMenu = mem ? guest::addr(StatusMenu_Ctor(static_cast<StatusMenu*>(mem))) : 0;
        u8* pd = PlayerBytes(self);
        World* world = self->world ? WorldOf(self) : nullptr;
        u8* params = PlayerParams();
        u8* shown = nullptr;
        if (world) {
            const u32* state = P<u32>(*reinterpret_cast<u32*>(params + 0xC4));
            if ((state ? state[0] : 0) == 0 && world->kind == 0)
                shown = reinterpret_cast<u8*>(CharaResources_GetParams(CharaResources_Get(), 100));
        }
        struct {
            u32 swords, params, stats, statsB, _10, swordDb, map, _1c, charaMgr, title, areaName, place;
        } info;
        memset(&info, 0, sizeof(info));
        info.swords = self->playerData + 0x180;
        info.params = guest::addr(params);
        info.swordDb = guest::addr(SwordDb_Get());
        u8* source = shown ? shown : params;
        info.statsB = guest::addr(source + 0x70);
        info.stats = guest::addr(source + 0x80);
        if (world) {
            info._1c = world->targeting;
            info.charaMgr = world->charaMgr;
        }
        info.areaName = guest::addr(Text((s8)pd[0x103] + 0x52));
        // The line under the title: what the player is doing, if anything says.
        u32 title = 0;
        if (WStr_Length(reinterpret_cast<u16*>(pd + 0x5C0)) != 0)
            title = self->playerData + 0x5C0;
        const s16 chapterEnd = *reinterpret_cast<s16*>(pd + 0x11E);
        if ((u32)(chapterEnd - 3) < 2 || chapterEnd == 0) {
            memset(pd + 0x4D0, 0, 0xF0);
            memset(pd + 0x4A0, 0, 0x30);
        } else {
            title = self->playerData + 0x4D0;
            info.place = self->playerData + 0x4A0;
        }
        if (WStr_Length(reinterpret_cast<u16*>(pd + 0x6D40)) != 0)
            title = self->playerData + 0x6D40;
        info.title = title;
        if (world && world->mapState)
            info.map = MapState_GetMap(P<void>(world->mapState));

        const bool noWorld = world == nullptr;
        if (world && world->kind == 3)
            StatusMenu_Start(P<StatusMenu>(self->statusMenu), &info, self->statusPage, self->pauseShown, noWorld, 1, 1);
        else
            StatusMenu_Start(P<StatusMenu>(self->statusMenu), &info, self->statusPage, self->pauseShown, noWorld,
                             Pause(self)->allowed == 2, 0);
    }
    self->statusWait = 0;
    self->statusDone = 0;
    if (Pause(self)->open != 0) {
        PauseMenu_Close(Pause(self), 0);
        self->pauseWanted = 0;
    }
    SetPauseAllowed(self, 0);
    RenderMgr_SetFrozen(self->renderMgr, 1);
}

static void ClosePauseForScreen(GameplayScene* self) {
    if (Pause(self)->open != 0) {
        PauseMenu_Close(Pause(self), 0);
        self->pauseWanted = 0;
    }
    SetPauseAllowed(self, 0);
    RenderMgr_SetFrozen(self->renderMgr, 1);
}

// ── 0x2B1B80 ──────────────────────────────────────────────────────────────
int GameplayScene_Start(GameplayScene* self) {
    self->cheatTimer = 0;
    self->cheatStep = 0;
    const bool fromMenu = self->previousMode == kPlayStatus || self->previousMode == kPlaySettings;
    if (self->mode == kPlayEvent) {
        App_PresentBlackFrames();
        if (!fromMenu) {
            EventPlayer_StartDay(Events(self), 0, 0);
            if (Events(self)->state == 2)
                memset(PlayerBytes(self) + 0x5C0, 0, 0x100);
            else
                self->mode = kPlayField;                 // nothing to play today
        }
    }
    if (self->mode == kPlayField || self->mode == kPlayMode1 || self->mode == kPlayEvent)
        GameplayScene_CreateWorld(self);

    void* mem;
    switch (self->mode) {
    case kPlayEvent:
        if (!fromMenu) {
            WorldOf(self)->kind = 0;
            self->eventLocked = 0;
            CharaMgr_SetIdle(P<void>(WorldOf(self)->charaMgr), 0);
        }
        SetPauseAllowedIfNot(self, WorldOf(self)->kind == 0 ? 2 : 1);
        RenderMgr_SetFrozen(self->renderMgr, 0);
        self->bannerState = self->bannerTimer = self->bannerKind = self->bannerAlpha = 0;
        break;
    case kPlayMode1:
        WorldOf(self)->kind = 2;
        SetPauseAllowed(self, 1);
        RenderMgr_SetFrozen(self->renderMgr, 0);
        break;
    case kPlayField:
        GameplayScene_StartField(self);
        break;
    case kPlayAreaMap:
        GameplayScene_StartAreaMap(self);
        break;
    case kPlaySettings:
        if (self->settings == 0) {
            mem = MemAlloc(sizeof(SettingsScreen));
            self->settings = mem ? guest::addr(SettingsScreen_Ctor(static_cast<SettingsScreen*>(mem))) : 0;
            SettingsScreen_Load(P<SettingsScreen>(self->settings));
        }
        ClosePauseForScreen(self);
        break;
    case kPlayStatus:
        GameplayScene_StartStatus(self);
        break;
    case kPlaySwordInfo:
        if (self->swordInfo == 0) {
            mem = MemAlloc(0x14);
            self->swordInfo = mem ? guest::addr(SwordInfo_Ctor(mem)) : 0;
        }
        RenderMgr_SetFrozen(self->renderMgr, 1);
        break;
    case kPlaySave:
        Sound_PlaySE(6, 0, 0x3FFF);
        if (self->saveScreen == 0) {
            mem = MemAlloc(0x150F8);
            self->saveScreen = mem ? guest::addr(SaveScreen_Ctor(mem, 0)) : 0;
        }
        SaveScreen_Start(P<void>(self->saveScreen));
        if (Pause(self)->open != 0) {
            PauseMenu_Close(Pause(self), 0);
            self->pauseWanted = 0;
        }
        Player(self)->state = 2;
        SetPauseAllowed(self, 0);
        RenderMgr_SetFrozen(self->renderMgr, 1);
        break;
    case kPlaySwordSelect:
        if (self->swordSelect == 0) {
            mem = MemAlloc(0x464);
            self->swordSelect = mem ? guest::addr(SwordSelect_Ctor(mem)) : 0;
            SwordSelect_Start(P<void>(self->swordSelect), 3);
        }
        ClosePauseForScreen(self);
        break;
    case kPlayItemSelect:
        if (self->itemSelect == 0) {
            mem = MemAlloc(sizeof(ItemSelect));
            self->itemSelect = mem ? guest::addr(ItemSelect_Ctor(static_cast<ItemSelect*>(mem))) : 0;
        }
        ItemSelect_Start(P<ItemSelect>(self->itemSelect));
        ClosePauseForScreen(self);
        break;
    default:
        break;
    }
    return 0;
}

// ── 0x2AE2C0 ──────────────────────────────────────────────────────────────
// The screen is black: drop what the old mode owned and start the next.
// Returns 0 when the scene is over.
static int GameplayScene_SwitchMode(GameplayScene* self) {
    self->previousMode = self->mode;
    const int next = self->nextMode;
    if ((self->previousMode == kPlayField || self->previousMode == kPlayEvent)
        && (next == kPlayAreaMap || next == kPlayNextDay))
        DayMgr_Save(DayMgr_Get());

    const bool toMenu = next == kPlayStatus || next == kPlaySettings;
    switch (self->mode) {
    case kPlayEvent:
        if (!toMenu)
            DestroyWorld(self);
        break;
    case kPlayField:
        if (!toMenu && next != kPlayEnterDoor && next != kPlayArrest)
            DestroyField(self);
        break;
    case kPlayAreaMap:
    case kPlayNextDay:
        DestroyAreaMap(self);
        break;
    case kPlaySettings:
        if (self->settings)
            SettingsScreen_Dtor(P<SettingsScreen>(self->settings), 1);
        self->settings = 0;
        break;
    case kPlayStatus:
        if (next != kPlaySwordInfo)
            DestroyStatus(self);
        break;
    case kPlaySwordInfo:
        if (self->swordInfo)
            SwordInfo_Dtor(P<void>(self->swordInfo), 1);
        self->swordInfo = 0;
        break;
    case kPlaySave:
        if (self->saveScreen)
            VirtualCall<void, void*, int>(*P<u32>(self->saveScreen), 8, P<void>(self->saveScreen), 1);
        self->saveScreen = 0;
        break;
    case kPlaySwordSelect:
        if (next != kPlaySwordInfo) {
            if (self->swordSelect)
                SwordSelect_Dtor(P<void>(self->swordSelect), 1);
            self->swordSelect = 0;
        }
        break;
    case kPlayItemSelect:
        if (self->itemSelect)
            ItemSelect_Dtor(P<ItemSelect>(self->itemSelect), 1);
        self->itemSelect = 0;
        break;
    default:
        break;
    }

    if (self->nextMode == kPlayNextDay) {
        PlayerData* pd = Player(self);
        if (pd->_108 == 0)
            Player_RefillLives(pd);
        self->inEvent = 0;
        self->nextMode = kPlayAreaMap;
        self->newDay = 1;
    } else if (self->nextMode == kPlayEnterDoor) {
        World_EnterMap(WorldOf(self), 0);
        const char* script = World_TakeDoorScript(WorldOf(self));
        void* charas = P<void>(WorldOf(self)->charaMgr);
        if (!script) {
            if (self->exits)
                TaskMgr_Reset(P<void>(self->exits), WorldOf(self));
            self->inEvent = 0;
            DayMgr_SetEvent(DayMgr_Get(), 0);
            CharaMgr_SetState(charas, 0, 0, 0, 0);
        } else {
            DayMgr_SetEvent(DayMgr_Get(), 1);
            CharaMgr_SetState(charas, 0, 2, 0, 0);
            EventPlayer_StartScript(Events(self), script, 0);
            self->inEvent = 1;
        }
        self->nextMode = kPlayField;
    } else if (self->nextMode == kPlayArrest) {
        World_EnterMap(WorldOf(self), 1);
        CharaMgr_SetState(P<void>(WorldOf(self)->charaMgr), 0, 2, 0, 0);
        EventPlayer_StartScript(Events(self), kArrestScript, 0);
        self->nextMode = kPlayField;
        self->arrested = 1;
        Bgm_StopAll(0);
        Bgm_Play(5, 1, 0, 0x3FFF);
    }

    if (self->nextMode == kPlayLeave)
        return 0;
    if (self->arrested == 0) {
        self->phase = kPlayBegin;
    } else {
        self->phase = kPlayRunning;
        WorldOf(self)->paused = 0;
    }
    self->mode = self->nextMode;
    SceneCall<int>(reinterpret_cast<Scene*>(self), kSceneStart);
    return 1;
}

// True when the mode changes without the screen fading (the world stays up).
static bool SeamlessSwitch(GameplayScene* self) {
    if (self->inEvent == 0 || self->pauseWanted != 0)
        return false;
    for (int mode : {self->mode, self->nextMode})
        if (mode == kPlayStatus || mode == kPlaySettings || mode == kPlaySave || mode == kPlaySwordSelect
            || mode == kPlayItemSelect || mode == kPlaySwordInfo)
            return false;
    return true;
}

static void ApplyStatusReturn(GameplayScene* self) {
    if (self->statusFromField == 0)
        return;
    if (Pause(self)->open == 0) {
        if (self->world)
            WorldOf(self)->paused = 0;
        RenderMgr_SetPaused(self->renderMgr, false);
    }
    self->statusFromField = 0;
}

// ── 0x2B0C00 ──────────────────────────────────────────────────────────────
// The phase machine.  Returns 0 when the scene is over.
static int GameplayScene_UpdatePhase(GameplayScene* self) {
    switch (self->phase) {
    case kPlayBegin:
        ShowLoading();
        RenderMgr_TriggerEffect(*gRenderMgr, 11, 2, 0);
        self->phase = kPlayLoading;
        break;
    case kPlayLoading:
        break;
    case kPlayLoaded:
        self->phase = kPlayFadeIn;
        break;
    case kPlayFadeIn:
        if (self->mode == kPlayAreaMap) {
            if ((self->dayFlags & 0x10) == 0) {
                SetPauseAllowed(self, 4);
                if (self->pauseShown == 0) {
                    if (self->previousMode == kPlaySwordSelect || self->previousMode == kPlayItemSelect)
                        SetPauseAllowed(self, 0x20);
                    Sound_PlaySE(9, 0, 0x3FFF);
                    PauseMenu_Open(Pause(self), 1);
                    self->pauseWanted = 1;
                    self->pauseShown = 1;
                } else if (self->paused != 0) {
                    self->pauseWanted = 1;
                }
                ApplyStatusReturn(self);
            }
        } else if (self->mode == kPlayField || self->mode == kPlayEvent) {
            if (self->inEvent != 0)
                SetPauseAllowed(self, 2);
            else if (self->mode == kPlayEvent)
                SetPauseAllowedIfNot(self, WorldOf(self)->kind == 0 ? 2 : 1);
            else
                SetPauseAllowed(self, 1);
            if (self->pauseShown == 0) {
                PauseMenu_Open(Pause(self), 1);
                self->pauseWanted = 1;
                self->pauseShown = 1;
            } else if (self->paused != 0) {
                self->pauseWanted = 1;
            }
            ApplyStatusReturn(self);
        }
        HideLoading();
        if (!SeamlessSwitch(self))
            RenderMgr_TriggerEffect(*gRenderMgr, 11, 0, 0);
        self->phase = kPlayRunning;
        break;
    case kPlayLeaving:
        self->fadeTimer = 0;
        self->phase = kPlayFadeOut;
        if (!SeamlessSwitch(self))
            RenderMgr_TriggerEffect(*gRenderMgr, 11, 1, 0);
        Bgm_StopVoiceLayer(0);
        if (self->nextMode == kPlayEnterDoor && self->world)
            WorldOf(self)->paused = 1;
        break;
    case kPlayFadeOut:
        if (self->fadeTimer++ > 0x26) {
            if (self->nextMode == kPlayEnterDoor && self->world)
                WorldOf(self)->paused = 0;
            App_PresentBlackFrames();
            return GameplayScene_SwitchMode(self);
        }
        break;
    default:
        break;
    }
    RenderMgr_UpdateEffects(*gRenderMgr);
    return 1;
}

// A sequence of buttons entered one at a time; true when the last one lands.
static bool CheatStep(GameplayScene* self, const u32* sequence) {
    const u32 pressed = Pad_GetPressed(P<void>(self->padMgr), 0);
    if ((sequence[self->cheatStep] & pressed) == 0) {
        if (pressed != 0) {
            self->cheatTimer = 0;
            self->cheatStep = 0;
        }
        return false;
    }
    self->cheatTimer = 0;
    self->cheatStep++;
    if (sequence[self->cheatStep] != 0xFFFFFFFFu)
        return false;
    self->cheatTimer = 0;
    self->cheatStep = 0;
    return true;
}

// ── 0x2ADD50 ──────────────────────────────────────────────────────────────
// Pause-menu cheats, each with two shoulder buttons held.
static void GameplayScene_CheckPauseCheats(GameplayScene* self) {
    const u16 held = Pad_GetHeld(P<void>(self->padMgr), 0);
    if (self->cheatTimer++ >= gFrameRate >> 1)
        self->cheatStep = 0;
    PlayerData* pd = Player(self);

    if ((held & kPadR1) && (held & kPadR2) && CheatStep(self, kCheatHealth)) {
        u8* params = PlayerParams();
        Player_SetHealth(pd, ParamHealth(params) < pd->healthMax ? ParamHealth(params) : pd->healthMax);
        Player_SetHealth(pd, pd->healthMax);
        ParamHealth(params) = pd->health < ParamHealthMax(params) ? pd->health : ParamHealthMax(params);
        Sound_PlaySE(10, 0, 0x3FFF);
    }
    if ((held & kPadL1) && (held & kPadL2) && CheatStep(self, kCheatLives)) {
        Player_RefillLives(pd);
        Sound_PlaySE(14, 0, 0x3FFF);
    }
    if ((held & kPadR2) && (held & kPadL2) && CheatStep(self, kCheatMoney)) {
        int money = (u16)(pd->_106 + 500);
        if (money > 9999)
            money = 9999;
        Player_SetMoney(pd, money);
        Voice_Play(0x8DB, 0, 0x3FFF, 0);
    }
    if ((held & kPadR1) && (held & kPadL2) && CheatStep(self, kCheatSword)) {
        u8* params = PlayerParams();
        Sword* sword = &pd->sword[PlayerData_GetReadiedSword(pd)];
        const u32* flags = P<u32>(*reinterpret_cast<u32*>(params + 0xC8));
        if (((flags ? flags[0] : 0) & 0x100) == 0) {
            pd->sword[PlayerData_GetReadiedSword(pd)].durability = 0;
            Sound_PlaySE(0x1B, 0, 0x3FFF);
        } else {
            int value = sword->durability + 1;
            value = value < 6 ? (value < 0 ? 0 : value) : 5;
            pd->sword[PlayerData_GetReadiedSword(pd)].durability = value;
            Sound_PlaySE(14, 0, 0x3FFF);
        }
    }
}

// ── 0x2ADC30 ──────────────────────────────────────────────────────────────
static void GameplayScene_CheckMapCheat(GameplayScene* self) {
    if (self->cheatTimer++ >= gFrameRate >> 1)
        self->cheatStep = 0;
    Pad_GetHeld(P<void>(self->padMgr), 0);
    if (CheatStep(self, kCheatMap)) {
        Player(self)->_144[ClampIndex(1, 3)] = 100;
        Voice_Play(0x8D2, 0, 0x3FFF, 0);
    }
}

static int PauseChoice(PauseMenu* menu) {
    if (menu->chosen == 0)
        return -1;
    return menu->chosenId != -1 ? menu->chosenId : menu->list.entry[menu->list.cursor].id;
}

// ── 0x2B07A0 ──────────────────────────────────────────────────────────────
// Opening and closing the pause menu, and what its entries do.
static int GameplayScene_UpdatePause(GameplayScene* self) {
    PauseMenu* menu = Pause(self);
    if (menu->open == 0) {
        if (self->pauseWanted != 0 && self->paused != 0) {
            Bgm_TogglePause();
            Se_Resume();
            if (self->resumeBgm != 0 && self->savedBgm != -1 && !Bgm_IsPlaying(self->savedBgm))
                Bgm_Play(self->savedBgm, 1, 0, 0x3FFF);
            self->resumeBgm = 0;
            self->pauseWanted = 0;
            self->paused = 0;
            if (menu->allowed == 0x20)
                SetPauseAllowed(self, 4);
            if (self->world && menu->allowed != 0x10)
                WorldOf(self)->paused = 0;
            RenderMgr_SetPaused(self->renderMgr, false);
            if (Player(self)->night == 0)
                RenderMgr_TriggerEffect(*gRenderMgr, 4, 1, 0);
        }
        return 0;
    }
    if (self->paused == 0 && self->pauseWanted == 0) {
        self->savedBgm = gCurrentBgm;
        Se_Pause();
        Sound_PlaySE(9, 0, 0x3FFF);
        Bgm_TogglePause();
        self->pauseWanted = 1;
        self->paused = 1;
        if (self->world && menu->allowed != 0x10)
            WorldOf(self)->paused = 1;
        RenderMgr_TriggerEffect(*gRenderMgr, 4, 0, 0);
        self->cheatTimer = 0;
        self->cheatStep = 0;
        self->resumeBgm = 0;
    }
    if (self->phase == kPlayLeaving)
        return 0;
    RenderMgr_SetPaused(self->renderMgr, true);
    GameplayScene_CheckPauseCheats(self);

    const int choice = PauseChoice(menu);
    switch (choice) {
    case 0: case 1: case 2:                     // the status pages
        self->statusPage = choice;
        Leave(self, kPlayStatus);
        self->pauseShown = 0;
        Player(self)->_014 = self->statusPage;
        RenderMgr_SetFrozen(self->renderMgr, 1);
        break;
    case 3:
        Leave(self, kPlaySettings);
        RenderMgr_SetFrozen(self->renderMgr, 1);
        break;
    case 4:
        PauseMenu_Back(menu);
        break;
    case 5:
        PauseMenu_Back(menu);
        menu->_18 = 0x20;
        break;
    case 6:
        Leave(self, kPlaySave);
        self->pauseShown = 0;
        break;
    case 7:
        PauseMenu_Back(menu);
        menu->_18 = 4;
        break;
    case 9:
        Leave(self, kPlaySwordSelect);
        self->pauseShown = 0;
        break;
    case 10:
        Leave(self, kPlayItemSelect);
        self->pauseShown = 0;
        break;
    case 0xE: case 0xF: case 0x10: case 0x11: case 0x12:    // an answer to a question in the world
        self->dayFlags = DayMgr_GetChoiceFlags(DayMgr_Get(), choice - 0xD);
        WorldOf(self)->dialogChoice = choice - 0xD;
        PauseMenu_Close(menu, 1);
        SetPauseAllowed(self, 0);
        Hud_ClearChoice(P<void>(self->hud));
        break;
    case 0x14:
        WorldOf(self)->dialogChoice = -1;
        PauseMenu_Close(menu, 1);
        SetPauseAllowed(self, 0);
        Hud_ClearChoice(P<void>(self->hud));
        break;
    default:
        break;
    }
    return 1;
}

// The line that opens a chapter's last day, fading in and out over the world.
static void ShowBanner(GameplayScene* self, u16* text) {
    struct Banner { u16 x, y; u32 z; u32 rgba; u32 _c; WString string; u32 vtable; };
    Banner* banner = static_cast<Banner*>(MemAlloc(sizeof(Banner)));    // on the stack in the original
    banner->vtable = 0x2EAF30;                                          // 0x194900
    WString_Ctor(&banner->string);
    WString_Reserve(&banner->string, 0);
    banner->x = (u16)Screen_X(0x100, -1);
    banner->y = (u16)Screen_Y(-0x400, 1);
    banner->z = 0xFFFFF0;
    int length = 0;
    while (text[length] != 0)
        length++;
    WString_Assign(&banner->string, text, text + length);
    banner->_c = 0;
    banner->rgba = (u32)(u8)self->bannerAlpha * 0x1000000 + 0xF0F0F0;
    RenderMgr_TriggerEffect(*gRenderMgr, 13, guest::addr(banner), 0);
    WString_Release(&banner->string);
    MemFree(banner);
}

// ── 0x2AF8C0 ──────────────────────────────────────────────────────────────
static int GameplayScene_UpdateEvent(GameplayScene* self) {
    World* world = WorldOf(self);
    if (self->pauseWanted == 0 && self->paused == 0)
        World_PreStep(world);
    PlayerData* pd = Player(self);
    u8* bytes = PlayerBytes(self);

    if (self->bannerState != 0) {
        u16* name = reinterpret_cast<u16*>(bytes + 0x4A0);
        if (self->bannerKind == 1) {
            WStr_Copy(sBannerText, name);
            WStr_Append(sBannerText, Text(0x13C), 0x80);
            ShowBanner(self, sBannerText);
        } else if (self->bannerKind == 2) {
            WStr_Copy(sBannerText2, Text(0x13D));
            WStr_Append(sBannerText2, name, 0x80);
            ShowBanner(self, sBannerText2);
        }
        if (self->bannerState == 1 && (self->bannerAlpha += 8) > 0x80) {
            self->bannerAlpha = 0x80;
            self->bannerState = 2;
        }
        if (self->bannerState == 2
            && ((Pad_GetPressed(P<void>(self->padMgr), 0) & 0xF0) != 0
                || (float)self->bannerTimer++ >= (float)gFrameRate * 2.0f))
            self->bannerState = 3;
        if (self->bannerState != 3)
            return 1;
        self->bannerAlpha -= 8;
        if (self->bannerAlpha > 0)
            return 1;
        self->bannerAlpha = 0;
        self->nextMode = kPlayNextDay;
        self->bannerState = 0;
        self->phase = kPlayFadeOut;
        return 1;
    }
    if (self->eventLocked != 0)
        return 1;

    const int state = World_Step(world);
    if (state != 1 && state != 2 && state != 4)
        return 1;
    if ((self->previousMode == kPlayStatus || self->previousMode == kPlaySettings) && self->phase == kPlayLoading)
        self->phase = kPlayLoaded;
    if (state == 2)
        SetPauseAllowed(self, 0);
    else
        SetPauseAllowedIfNot(self, WorldOf(self)->kind == 0 ? 2 : 1);

    EventPlayer* events = Events(self);
    if (events->state == 2) {                   // the script is still running
        if (state == 1)
            World_CheckExit(world);
        return 1;
    }
    if (events->result == 2) {                  // the player died in it
        if (state == 2) {
            if (World_DeathPending(WorldOf(self)) == 0)
                SetPauseAllowed(self, 0);
        } else if (state == 4) {
            Leave(self, kPlayLeave);
        }
        return 1;
    }

    EventPlayer_Finish();
    self->fadeTimer = 0;
    self->phase = kPlayFadeOut;
    self->newDay = 0;
    const int result = events->result;
    if (result == 1) {
        self->nextMode = kPlayAreaMap;
        return 1;
    }
    if (result == 3 || result == 4) {
        if (result == 3) {                      // an ending: on to the staff roll
            if (pd->_004 < 9999)
                pd->_004++;
            self->toStaffRoll = 1;
        }
        Leave(self, kPlayLeave);
        return 1;
    }
    // The day goes on, or a new one starts.
    bool banner = false;
    if (*reinterpret_cast<s16*>(bytes + 0x11C) != 0) {
        const s16 kind = *reinterpret_cast<s16*>(bytes + 0x11E);
        banner = kind == 1 || kind == 2;
        self->bannerKind = kind;
    }
    const u32 flags = DayMgr_Advance(DayMgr_Get(), 1);
    if (flags == 0) {
        self->nextMode = kPlayField;
    } else {
        self->dayFlags = flags;
        if (banner) {
            self->bannerState = 1;
            self->bannerTimer = 0;
            self->bannerAlpha = 0;
            self->phase = kPlayRunning;
            SetPauseAllowed(self, 0);
            Sound_PlaySE(0, 0, 0x3FFF);
        } else {
            self->nextMode = kPlayNextDay;
        }
    }
    u8* params = PlayerParams();
    pd->healthMax = ParamHealthMax(params) > 1999 ? 2000 : ParamHealthMax(params);
    Player_SetHealth(pd, pd->healthMax <= ParamHealth(params) ? pd->healthMax : ParamHealth(params));
    return 1;
}

// ── 0x2AF340 ──────────────────────────────────────────────────────────────
// Things the world asks the scene for.
static void GameplayScene_CheckWorldRequest(GameplayScene* self) {
    World* world = WorldOf(self);
    if (world->kind == 3) {
        self->statusPage = 0;
        Leave(self, kPlayStatus);
        self->pauseShown = 1;
    } else if (world->kind == 4) {              // show the date
        world->kind = 1;
        SetPauseAllowed(self, 0x10);
        PauseMenu_Open(Pause(self), 0);
        PlayerData* pd = *gPlayerData;
        WStr_Copy(sDateText, Text((s8)pd->day + 0x23));
        WStr_Append(sDateText, Text(1), 0x80);
        WStr_Append(sDateText, Text((s8)pd->time + 0x1E), 0x80);
        Hud_ShowDate(P<void>(self->hud), sDateText, -1, 0);
    }
}

// ── 0x2AF4C0 ──────────────────────────────────────────────────────────────
// Returns 1 when the field is being left for the area map or the next day.
static int GameplayScene_CheckLeaveField(GameplayScene* self) {
    if (self->inEvent == 0) {
        if (self->arrested == 0)
            return 0;
        (*reinterpret_cast<s16*>(PlayerBytes(self) + 0x858))++;
        self->phase = kPlayLeaving;
        if (Events(self)->result == 0) {
            self->nextMode = kPlayAreaMap;
            Voice_Play(0x978, 0, 0x3FFF, 0);
        } else {
            DayMgr_Arrest(DayMgr_Get());
            self->nextMode = kPlayNextDay;
            self->dayFlags = 1 | 0x10;
        }
        self->arrested = 0;
        return 1;
    }
    if (WorldOf(self)->dialogChoice < 1) {
        const u32 flags = DayMgr_Advance(DayMgr_Get(), 0);
        if (flags == 0) {
            WorldOf(self)->exitUsed = WorldOf(self)->doorIndex;
            Leave(self, kPlayEnterDoor);
        } else {
            self->dayFlags = flags;
            self->nextMode = kPlayNextDay;
            self->phase = kPlayLeaving;
        }
    } else {
        Leave(self, kPlayNextDay);
    }
    return 1;
}

// ── 0x2AF030 ──────────────────────────────────────────────────────────────
// A frame of free roaming.  `exit` is what the exit manager reported.
static int GameplayScene_RunField(GameplayScene* self, int exit) {
    World* world = WorldOf(self);
    if (self->phase == kPlayLoading) {
        self->phase = kPlayLoaded;
        if (self->previousMode != kPlayStatus && self->previousMode != kPlaySettings
            && World_PlaceAtStart(world) == 0)
            World_StartIntro(world);
        return 0;
    }
    const int hint = World_GetHint(world);
    if (hint == -1) {
        if (self->hintShown != 0) {
            Hud_HideHint(P<void>(self->hud));
            self->hintShown = 0;
        }
    } else {
        Hud_ShowHint(P<void>(self->hud), Text(hint), 0);
        self->hintShown = 1;
    }
    if (World_IntroDone(world) == 0)
        World_UpdateTalkHint(world, self->area);
    else
        Hud_ShowAreaName(P<void>(self->hud), Text(self->area + 0x52), (int)((float)gFrameRate * 3.5f), 0);

    if (exit != -1) {
        if (exit == 0x80) {                     // caught: off to the magistrate's office
            SetPauseAllowed(self, 0);
            Leave(self, kPlayArrest);
            PlayerBytes(self)[0x103] = 2;
            self->mapArea = 2;
            World_UseExit(world, exit);
            return 1;
        }
        if (World_UseExit(world, exit) != 0) {
            Leave(self, kPlayEnterDoor);
            return 1;
        }
        return 0;
    }
    if (self->inEvent != 0)
        return 0;
    const int way = World_CheckExit(world);
    if (way == 0) {
        if (World_UseExit(world, -1) != 0) {
            Leave(self, kPlayEnterDoor);
            return 1;
        }
    } else if (way == 1) {
        Leave(self, kPlayEnterDoor);
        return 1;
    } else if (way == 2) {                      // walked out of the area
        if (self->dayFlags == 0) {
            Leave(self, kPlayAreaMap);
            return 1;
        }
        self->dayEnding = 1;
        self->dayFlags = DayMgr_EndDay(DayMgr_Get());
        self->dayEndShown = 0;
        SetPauseAllowed(self, 0);
    }
    if (self->dayFlags == 0 && DayMgr_IsNight(DayMgr_Get()) != 0)
        self->dayFlags = 1;
    return 0;
}

// ── 0x2AF610 ──────────────────────────────────────────────────────────────
// The day is over: say so, let the world wind down, then go to the next day.
static void GameplayScene_EndOfDay(GameplayScene* self) {
    const int frames = (int)((float)gFrameRate * 3.5f);
    void* hud = P<void>(self->hud);
    if ((self->dayFlags & 0x20) != 0) {
        self->dayFlags &= ~0x20u;
        Hud_ShowNotice(hud, Text(0x140), frames, 0);
        Sound_PlaySE(0x28, 0, 0x3FFF);
        (*reinterpret_cast<s16*>(PlayerBytes(self) + 0x856))++;
        self->dayEndShown = 1;
        return;
    }
    u8* bytes = PlayerBytes(self);
    if (*reinterpret_cast<s16*>(bytes + 0x11C) != 0) {
        const s16 kind = *reinterpret_cast<s16*>(bytes + 0x11E);
        u16* name = reinterpret_cast<u16*>(bytes + 0x4A0);
        if (kind == 1) {
            WStr_Copy(sNoticeText, name);
            WStr_Append(sNoticeText, Text(0x13C), 0x80);
            Hud_ShowNotice(hud, sNoticeText, frames, 0);
        } else if (kind == 2) {
            WStr_Copy(sNoticeText2, Text(0x13D));
            WStr_Append(sNoticeText2, name, 0x80);
            Hud_ShowNotice(hud, sNoticeText2, frames, 0);
        }
    }
    if (self->dayEndShown == 0) {
        Hud_ShowNotice(hud, Text(0x13F), frames, 0);
        Sound_PlaySE(0x28, 0, 0x3FFF);
        self->dayEndShown = 1;
    }
    DayMgr_Tick(DayMgr_Get());
    if (World_DayEndPending(WorldOf(self)) == 0) {
        Leave(self, kPlayNextDay);
        self->dayEnding = 0;
    }
}

// ── 0x2AEE00 ──────────────────────────────────────────────────────────────
static int GameplayScene_UpdateField(GameplayScene* self, int scriptRunning) {
    World* world = WorldOf(self);
    if (self->pauseWanted == 0 && self->paused == 0)
        World_PreStep(world);
    if (self->dayEnding != 0) {
        GameplayScene_EndOfDay(self);
        return 1;
    }
    GameplayScene_CheckWorldRequest(self);
    if (scriptRunning == 0 && World_IsIndoors(world) != 0 && GameplayScene_CheckLeaveField(self) != 0)
        return 1;

    int exit = -1;
    if (self->phase != kPlayLoading && self->playerDead == 0) {
        Spawner_Update(P<void>(self->spawner), world, P<void>(self->exits));
        exit = TaskMgr_Update(P<void>(self->exits), world, P<void>(self->spawner));
    }
    const int state = World_Step(world);
    if (state == 0) {
        // Still loading: start the area's tune unless the map has its own.
        const int tune = kAreaBgm[self->area * 2 + (s8)Player(self)->night];
        if (!Bgm_IsPlaying(tune) && gNoAreaBgm == 0 && P<s32>(world->mapState)[3] == -1)
            Bgm_Play(tune, 1, 0, 0x3FFF);
    } else if (state == 4) {
        Leave(self, kPlayLeave);
    } else if (state == 2) {
        if (World_DeathPending(world) == 0) {
            self->playerDead = 1;
            SetPauseAllowed(self, 0);
        }
    } else if (state == 1) {
        GameplayScene_RunField(self, exit);
    }
    return 1;
}

// ── 0x2B0030 ──────────────────────────────────────────────────────────────
static int GameplayScene_UpdateAreaMap(GameplayScene* self) {
    AreaMap* map = P<AreaMap>(self->areaMap);
    void* hud = P<void>(self->hud);
    if (self->newDay != 0 && (self->dayFlags & 0x10) != 0) {
        // The morning message comes before the map.
        if ((self->dayFlags & 1) == 0) {
            if (Hud_MessageBusy(hud) == 0) {
                Hud_CloseMessage(hud);
                Hud_Reset(hud);
                self->dayFlags = 0;
                self->phase = kPlayBegin;
            }
        } else {
            Hud_Load(hud);
            Hud_OpenMessage(hud);
            self->dayFlags &= ~1u;
            self->phase = kPlayLoaded;
        }
        Hud_Update(hud);
        return 1;
    }
    if (map->ready == 0) {
        if (self->paused == 0)
            Bgm_Play(kMapBgm[(s8)Player(self)->night], 1, 0, 0x3FFF);
        self->area = (s8)Player(self)->area;
        const int shown = self->area != self->mapArea ? self->mapArea : self->area;
        if (self->newDay == 0) {
            AreaMap_Open(map, shown);
        } else {
            AreaMap_Arrive(map, shown);
            self->newDay = 0;
            self->dayFlags = 0;
            SetPauseAllowed(self, 0);
        }
        return 1;
    }
    GameplayScene_CheckMapCheat(self);
    // WOTS2_TOUR=seed (debug): the cursor is kept on the next stop of a round
    // of all the areas — home (Otsuka) every other time, to rest — so that
    // a scripted run that only presses Cross goes everywhere.
    static const char* tour = getenv("WOTS2_TOUR");
    static int tourStop = 0;
    if (tour != nullptr) {
        static const int kOthers[9] = {0, 1, 2, 3, 4, 5, 7, 8, 9};
        const int seed = atoi(tour);
        static const int kStride[6] = {1, 2, 4, 5, 7, 8};
        int target = (tourStop & 1) ? 6 : kOthers[(tourStop / 2 * kStride[seed / 9 % 6] + seed) % 9];
        // WOTS2_TOUR_SEEK=1: go instead where StartUp2 would start a story
        // script now (each script once per period), and home to rest when
        // there is none: the tour then follows the story as far as its
        // flags lead.
        static const bool seek = getenv("WOTS2_TOUR_SEEK") != nullptr;
        if (seek) {
            static int seekStop = -1, seekTarget = 6;
            static std::set<std::string> tried;
            if (seekStop != tourStop) {
                seekStop = tourStop;
                seekTarget = 6;
                const u8* pd = PlayerBytes(self);
                for (int i = 0; i < 10; i++) {
                    const int area = (i + seed) % 10;
                    if (map->closed[area] != 0)
                        continue;
                    const char* script = EventPlayer_ProbeStory(MapDb_GetAreaMap(MapDb_Get(), area, pd[0x102]));
                    if (script == nullptr)
                        continue;
                    if (!tried.insert(std::string(script) + "@" + std::to_string(pd[0x100] * 5 + pd[0x101])).second)
                        continue;
                    printf("[tour] %s waits in area %d\n", script, area);
                    seekTarget = area;
                    break;
                }
            }
            target = seekTarget;
        }
        if (map->closed[target] != 0)
            for (target = 0; target < 10 && map->closed[target] != 0; target++) {}
        if (target < 10)
            map->cursor.area = target;
    }
    const int chosen = AreaMap_Update(map);
    self->mapArea = map->cursor.area;
    if (map->dialog == 0 && map->arriving == 0) {
        if (Pause(self)->allowed == 0)
            SetPauseAllowed(self, 4);
    } else {
        SetPauseAllowed(self, 0);
    }
    if (map->state == 3) {
        if (self->phase == kPlayLoading) {
            self->phase = kPlayLoaded;
        } else if (self->phase != kPlayFadeOut && self->pauseWanted == 0 && self->paused == 0 && chosen != 0) {
            self->fadeTimer = 0;
            Leave(self, Player(self)->state == 0 ? kPlayField : kPlayEvent);
            self->area = map->cursor.area;
            self->mapArea = self->area;
            Player(self)->area = (u8)self->area;
            if (tour != nullptr) {
                tourStop++;
                printf("[tour] stop %d: area %d, day %d period %d\n", tourStop, self->area, (s8)PlayerBytes(self)[0x100], (s8)PlayerBytes(self)[0x101]);
            }
            if (self->area == 10)
                Leave(self, kPlayLeave);
        }
    }
    return 1;
}

static void OpenSwordInfo(GameplayScene* self, int sword) {
    if (self->swordInfo == 0) {
        void* mem = MemAlloc(0x14);
        self->swordInfo = mem ? guest::addr(SwordInfo_Ctor(mem)) : 0;
    }
    RenderMgr_SetFrozen(self->renderMgr, 1);
    SwordInfo_Show(P<void>(self->swordInfo), sword);
}

// ── 0x2AE810 ──────────────────────────────────────────────────────────────
static int GameplayScene_UpdateStatus(GameplayScene* self) {
    StatusMenu* menu = P<StatusMenu>(self->statusMenu);
    if (self->statusWait != 0) {
        // A different sword was readied: wait for the character to change.
        if (self->world == 0) {
            self->statusDone = 1;
            self->statusWait = 0;
        } else if (CharaMgr_IsBusy(P<void>(WorldOf(self)->charaMgr)) != 0) {
            SwordDb_PollLoads(SwordDb_Get());
            if (CharaMgr_IsReady(P<void>(WorldOf(self)->charaMgr))) {
                self->statusDone = 1;
                self->statusWait = 0;
            }
        }
        return 1;
    }
    if (self->statusDone != 0) {
        Leave(self, self->previousMode == kPlaySwordInfo ? self->returnMode : self->previousMode);
        self->statusDone = 0;
    }
    void* pad = P<void>(self->padMgr);
    if (self->inEvent == 0 && (Pad_GetPressed(pad, 0) & (kPadL1 | kPadR1)) != 0
        && *reinterpret_cast<s16*>(P<u8>(menu->_04) + 0x12) != 0) {
        int page = menu->page;
        if (Pad_GetPressed(pad, 0) & kPadR1)
            page = page + 1 < 3 ? page + 1 : 0;
        else if (Pad_GetPressed(pad, 0) & kPadL1)
            page = page - 1 < 0 ? 2 : page - 1;
        StatusMenu_SetPage(menu, page);
        Player(self)->_014 = page;
    }
    const s32* action = StatusMenu_Update(menu);
    if (self->phase == kPlayLoading) {
        if (menu->layout != 0 && *reinterpret_cast<s16*>(P<u8>(menu->layout) + 4) != 0)
            self->phase = kPlayLoaded;
        return 1;
    }
    if (!action)
        return 1;

    bool close = false;
    const int resume = *reinterpret_cast<s32*>(P<u8>(menu->_04) + 4);
    if (self->inEvent == 0) {
        switch (action[0]) {
        case 4:
            Leave(self, kPlaySwordInfo);
            if (self->previousMode != kPlaySwordInfo)
                self->returnMode = self->previousMode;
            OpenSwordInfo(self, action[1]);
            break;
        case 5:                                 // drop an item where the player stands
            if (self->world) {
                alignas(16) u8 matrix[64];
                Matrix_Copy(matrix, PlayerParams() + 0x50);
                ItemDrops_Drop(P<void>(WorldOf(self)->itemDrops), action[1], matrix);
            }
            break;
        case 3: {                               // drop a sword
            alignas(16) u8 matrix[64];
            Matrix_Copy(matrix, PlayerParams() + 0x50);
            SwordDb_DropSword(SwordDb_Get(), action[1], matrix);
            break;
        }
        case 2:
            self->pauseShown = resume;
            close = true;
            break;
        case 1:
            self->pauseShown = 1;
            close = true;
            break;
        }
    } else if (WorldOf(self)->kind == 3) {      // the menu was opened to hand something over
        switch (action[0]) {
        case 2:
            close = true;
            self->pauseShown = resume;
            WorldOf(self)->handOver = -1;
            break;
        case 4:
            Leave(self, kPlaySwordInfo);
            self->returnMode = kPlayField;
            OpenSwordInfo(self, action[1]);
            WorldOf(self)->handOver = -1;
            break;
        case 6:
            close = true;
            self->pauseShown = resume;
            WorldOf(self)->handOver = action[1] + 1;
            break;
        }
    } else if (action[0] == 2) {
        close = true;
        self->pauseShown = resume;
    } else if (action[0] == 1) {
        self->pauseShown = 1;
        close = true;
    }
    if (close) {
        self->statusWait = 1;
        self->statusDone = 0;
        if (StatusMenu_SwordChanged(menu) != 0) {
            Sword* sword = PlayerData_ReadySword(Player(self), StatusMenu_GetSword(menu));
            if (self->world)
                World_SetSword(WorldOf(self), sword);
        }
    }
    return 1;
}

// ── 0x2AECF0 ──────────────────────────────────────────────────────────────
static int GameplayScene_UpdateSwordSelect(GameplayScene* self) {
    void* screen = P<void>(self->swordSelect);
    if (self->phase == kPlayLoading) {
        SwordSelect_Load(screen);
        if (*static_cast<s32*>(screen) == 5)
            self->phase = kPlayFadeIn;
        return 1;
    }
    s32 result[2];
    SwordSelect_Update(result, screen);
    if (result[0] == 2) {                       // look at a sword
        if (self->swordInfo == 0) {
            void* mem = MemAlloc(0x14);
            self->swordInfo = mem ? guest::addr(SwordInfo_Ctor(mem)) : 0;
        }
        RenderMgr_SetFrozen(self->renderMgr, 1);
        SwordInfo_Start(P<void>(self->swordInfo), result[1]);
        Leave(self, kPlaySwordInfo);
        self->pauseShown = 0;
    } else if (result[0] == 1) {
        Leave(self, kPlayAreaMap);
        self->pauseShown = 0;
    }
    return 1;
}

// ── 0x2B02D0 ──────────────────────────────────────────────────────────────
static int GameplayScene_UpdateMode(GameplayScene* self) {
    if (self->phase == kPlayFadeOut)
        return 1;
    PauseMenu* menu = Pause(self);
    void* pad = P<void>(self->padMgr);
    if (self->mode != kPlaySave && Scene_CheckSoftReset(self) != 0) {
        Leave(self, kPlayLeave);
        self->softReset = 1;
        if (menu->open != 0)
            PauseMenu_Close(menu, 0);
        return 1;
    }
    // Select opens the status menu.
    if ((menu->allowed == 4 || menu->allowed == 1) && (Pad_GetPressed(pad, 0) & kPadSelect) != 0) {
        Sound_PlaySE(9, 0, 0x3FFF);
        Leave(self, kPlayStatus);
        self->statusPage = Player(self)->_014;
        self->pauseShown = 1;
        self->statusFromField = 1;
        if (self->world)
            WorldOf(self)->paused = 1;
        RenderMgr_SetPaused(self->renderMgr, true);
        return 1;
    }
    // Start skips a story scene that allows it.
    bool skipped = false;
    if (self->mode == kPlayEvent && RenderMgr_FindEffect(*gRenderMgr, 11) == 0
        && EventPlayer_CanSkip(Events(self)) != 0) {
        SetPauseAllowed(self, 0);
        if ((Pad_GetPressed(pad, 0) & kPadStart) != 0) {
            EventPlayer_Skip(Events(self));
            skipped = true;
        }
    }
    if (!((!skipped && PauseMenu_Update(menu) == 0) || menu->allowed == 0x10))
        return 1;

    int scriptRunning = 0;
    if (self->mode == kPlayMode1 || self->mode == kPlayField || self->mode == kPlayEvent) {
        if (self->world == 0 || WorldOf(self)->paused == 0)
            scriptRunning = EventPlayer_Update(Events(self), 0);
        else
            scriptRunning = Events(self)->state == 2;
    }
    switch (self->mode) {
    case kPlayEvent:
        return GameplayScene_UpdateEvent(self);
    case kPlayMode1:
        return 1;
    case kPlayField:
        return GameplayScene_UpdateField(self, scriptRunning);
    case kPlayAreaMap:
        return GameplayScene_UpdateAreaMap(self);
    case kPlaySettings: {
        SettingsScreen* settings = P<SettingsScreen>(self->settings);
        if (settings->phase == 4) {
            if (SettingsScreen_Update(settings) == 0) {
                self->resumeBgm = 1;
                Leave(self, self->previousMode);
                self->pauseShown = 0;
            }
        } else {
            SettingsScreen_Update(settings);
            if (settings->phase == 4) {
                SettingsScreen_Start(settings);
                self->phase = kPlayFadeIn;
            }
        }
        return 1;
    }
    case kPlayStatus:
        return GameplayScene_UpdateStatus(self);
    case kPlaySwordInfo: {
        u8* info = P<u8>(self->swordInfo);
        if (info[0xD] == 0) {
            SwordInfo_Update(info);
            if (info[0xD] != 0)
                self->phase = kPlayFadeIn;
        } else {
            SwordInfo_Update(info);
            if ((Pad_GetPressed(pad, 0) & kPadTriangle) != 0) {
                Leave(self, self->previousMode);
                self->previousMode = self->returnMode;
            }
        }
        return 1;
    }
    case kPlaySave:
        if (self->phase == kPlayLoading)
            self->phase = kPlayLoaded;
        if (SaveScreen_Update(P<void>(self->saveScreen)) == 0) {
            const s32 result = *reinterpret_cast<s32*>(P<u8>(self->saveScreen) + 0x150E8);
            if (result == 0) {
                Leave(self, self->previousMode);
            } else if (result == 1) {
                Leave(self, kPlayLeave);
                self->softReset = 1;
            }
        }
        return 1;
    case kPlaySwordSelect:
        return GameplayScene_UpdateSwordSelect(self);
    case kPlayItemSelect: {
        const bool done = ItemSelect_Update(P<ItemSelect>(self->itemSelect));
        if (self->phase == kPlayLoading && P<ItemSelect>(self->itemSelect)->phase == 4) {
            self->phase = kPlayFadeIn;
        } else if (done) {
            Leave(self, self->previousMode);
            self->pauseShown = 0;
        }
        return 1;
    }
    default:
        return 1;
    }
}

// ── 0x2B10F0 ──────────────────────────────────────────────────────────────
int GameplayScene_Update(GameplayScene* self) {
    if (PauseMenu_Poll(Pause(self)) == 0)
        return 1;
    if (GameplayScene_UpdatePhase(self) == 0)
        return 0;
    if (self->phase != kPlayFadeOut)
        GameplayScene_UpdatePause(self);
    return GameplayScene_UpdateMode(self);
}

// ── 0x2ADAE0 ──────────────────────────────────────────────────────────────
int GameplayScene_Draw(GameplayScene* self) {
    switch (self->mode) {
    case kPlayEvent:
    case kPlayMode1:
    case kPlayField:
        World_Draw(WorldOf(self));
        break;
    case kPlayAreaMap:
        AreaMap_Draw(P<AreaMap>(self->areaMap));
        if ((self->dayFlags & 0x10) != 0)
            Hud_Draw(P<void>(self->hud));
        if (P<AreaMap>(self->areaMap)->dialog != 0)
            GsDevice_ClearScreenColor(P<GsDevice>(self->gsDevice), 0x40000000);
        break;
    case kPlaySettings:
        SettingsScreen_Draw(P<SettingsScreen>(self->settings));
        break;
    case kPlayStatus:
        StatusMenu_Draw(P<StatusMenu>(self->statusMenu));
        break;
    case kPlaySwordInfo:
        SwordInfo_Draw(P<void>(self->swordInfo));
        break;
    case kPlaySave:
        Mode0_Draw(P<void>(self->saveScreen));
        break;
    case kPlaySwordSelect:
        SwordSelect_Draw(P<void>(self->swordSelect));
        break;
    case kPlayItemSelect:
        ItemSelect_Draw(P<ItemSelect>(self->itemSelect));
        break;
    default:
        break;
    }
    PauseMenu_Draw(Pause(self));
    if (self->eventLocked != 0)
        GsDevice_ClearScreenColor(P<GsDevice>(self->gsDevice), 0x40000000);
    if (self->world != 0 && World_PollFault() == -1) {
        self->phase = kPlayLeaving;
        self->nextMode = kPlayLeave;
    }
    return 1;
}

// ── 0x2ADA70 ──────────────────────────────────────────────────────────────
Scene* GameplayScene_GetNextScene(GameplayScene* self) {
    if (self->pauseWanted != 0 && self->paused != 0) {
        Bgm_TogglePause();
        Se_Resume();
    }
    DayMgr_Save(DayMgr_Get());
    Player(self)->state = 0;
    Bgm_StopAll(0);
    return nullptr;
}

// ── 0x2ADA00 ──────────────────────────────────────────────────────────────
s32 GameplayScene_GetNextSceneId(GameplayScene* self) {
    if (self->toStaffRoll != 0)
        return kSceneStaffRoll;
    if (self->softReset == 0)
        return kSceneResults;
    Display_ApplyOffset(1);
    Config_SetDefaults(P<Config>(self->config));
    Sound_ApplyConfig();
    return kSceneBoot;
}
