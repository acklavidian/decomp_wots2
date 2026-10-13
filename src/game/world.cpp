// The world — decompiled from 0x1DC1D0–0x1DECF0.  See include/game/world.h.

#include "game/world.h"
#include "game/world_parts.h"
#include "game/chara_resources.h"
#include "game/day_mgr.h"
#include "game/map_collision.h"
#include "game/map_db.h"
#include "game/player_data.h"
#include "engine/bgm.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstring>

void App_PresentBlackFrames();                                  // 0x1E0660
extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    int   Rand_Next();                                          // 0x1200F0
    int   RenderMgr_TriggerEffect(void* mgr, u32 type, u32 arg1, u32 arg2); // 0x14F190
    int   RenderMgr_FindEffect(void* mgr, u32 type);            // 0x14F0F0
    void  GsDevice_ClearScreenColor(GsDevice* dev, u32 rgba);   // 0x12F1F0
    void  GsDevice_SwapBuffers(GsDevice* dev, u32 a);           // 0x131240
    void  Matrix_Copy(void* dst, const void* src);              // 0x1073C0
    void* Hud2_Ctor(void* mem);                                 // 0x2A6670
    void  Hud2_Set(void* hud);                                  // 0x1E06C0
    void  Native_HudAnchor(int on);
    void  Hud_Draw(void* hud);                                  // 0x21D9E0
    void  Hud_HideHint(void* hud);                              // 0x21D6E0
    void  Hud_ShowHint(void* hud, const u16* text, int a);      // 0x21D720
    void  Hud_Reset(void* hud);                                 // 0x21D8F0
    void  Hud_Load(void* hud);                                 // 0x21DD10
    void  Hud_Update(void* hud);                                // 0x21DAB0
    char  SwordDb_PollLoads(void* db);                          // 0x15D800
}

static_assert(sizeof(LightFlicker) == 0x40 && offsetof(World, flicker) == 0x80, "World");
static_assert(offsetof(World, stage) == 0xE0 && sizeof(World) == 0x110, "World");
static_assert(sizeof(ActorDesc) == 0x40, "ActorDesc");

static const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
static const GuestPtrVar<void>       gCamera{0x2EE678u};
static const GuestPtrVar<void>       gRenderMgr{0x2EE684u};
static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
static const GuestPtrVar<void>       gHud{0x2EE69Cu};
static u32& gWorldLink = *guest::ptr<u32>(0x2EE690u);
static u32& gHud2      = *guest::ptr<u32>(0x2EE694u);
static s32& gIndoors   = *guest::ptr<s32>(0x2EE518u);
static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);
static s32& gCurrentBgm = *guest::ptr<s32>(0x2EDADCu);
static s32& gSeVolume  = *guest::ptr<s32>(0x2EDAD0u);
static s32& gLoadingActive   = *guest::ptr<s32>(0x2EE6B4u);
static s32& gLoadingDotTimer = *guest::ptr<s32>(0x2EE6B8u);
static s32& gLoadingDots     = *guest::ptr<s32>(0x2EE6BCu);
static s32& gLoadingFrames   = *guest::ptr<s32>(0x2EE6C0u);

enum { kBgmBattle = 0x11, kBgmGameOver = 0x19 };
// The player's actor description; chara, sword and lights are filled in.
static const ActorDesc* const kPlayerActor = guest::ptr<const ActorDesc>(0x2D90C0u);
// The maps that belong to each area (and to "elsewhere"): 11 lists of up to
// 19 map indices, -1 ends.
static const s16* const kAreaMaps = guest::ptr<const s16>(0x2D8F00u);
// Per area: six {spot, text id} pairs shown when the player stands on a spot.
static const s32* const kAreaHints = guest::ptr<const s32>(0x2D9100u);
static const void* const kCamOutdoors = guest::ptr(0x2E0020u);
static const void* const kCamIndoors  = guest::ptr(0x2E0070u);
static u16* const sHintText = guest::ptr<u16>(0x30A720u);

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline void* Charas(World* w) { return P(w->charaMgr); }
static inline u8* StageBytes(World* w) { return P<u8>(w->stage); }
static inline void* CamList(World* w) { return StageBytes(w) + 0x60; }
static inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
static inline s8 Variant() { return (*gPlayerData)->night; }

// ── 0x1DC1D0 ──────────────────────────────────────────────────────────────
// Returns 1 while a burst is on, with its brightness in *out.
static int LightFlicker_Step(LightFlicker* f, float* out) {
    if (f->wait != 0) {
        f->wait--;
        return 0;
    }
    *out = f->level * f->scale;
    f->level += (float)(Rand_Next() % 20 + 1) * 0.0001f * f->direction;
    if (f->high < f->level) {
        f->direction = -1.0f;
        f->low = (float)(Rand_Next() % 10 + 1) * 0.0001f;
    } else if (f->level < f->low) {
        f->direction = 1.0f;
        f->high = (float)(Rand_Next() % 10 + 1) * 0.01f;
        f->_04 = 1;
        f->wait = Rand_Next() % 60 + 30;
    }
    return 1;
}

// ── 0x1DC340 ──────────────────────────────────────────────────────────────
int World_PollFault() {
    return 0;
}

// ── 0x1DC570 / 0x1DD140 / 0x1DC540 ────────────────────────────────────────
int  World_GetIndoors() { return gIndoors; }
void World_SetIndoors(int indoors) { gIndoors = indoors; }
int  World_IsIndoors(World*) { return World_GetIndoors() == 1; }

// ── 0x1DC700 / 0x1DC720 / 0x1DC750 ────────────────────────────────────────
u32 World_GetFlags(World* self) {
    return self->flagsObj ? P<u32>(self->flagsObj)[3] : 0;
}
void World_ClearFlags(World* self, u32 flags) {
    if (self->flagsObj)
        P<u32>(self->flagsObj)[3] &= ~flags;
}
void World_SetFlags(World* self, u32 flags) {
    if (self->flagsObj)
        P<u32>(self->flagsObj)[3] |= flags;
}

// ── 0x1DC350 ──────────────────────────────────────────────────────────────
int World_PreStep(World* self) {
    if (self->deathState == 0 && self->kind == kWorldField) {
        const bool fighting = (CharaMgr_GetFlags(Charas(self), 0) & 0x100) != 0
                           && CharaMgr_HasOpponent(Charas(self), 0) != 0;
        if (!fighting) {
            if (self->battleBgm != 0) {
                Bgm_StopFade(kBgmBattle, 1);
                Bgm_Play(self->savedBgm, 1, 0, 0x3FFF);
                self->battleBgm = 0;
            }
            return 0;
        }
        if (self->battleBgm == 0) {
            self->savedBgm = gCurrentBgm;
            Bgm_StopFade(self->savedBgm, 1);
            Bgm_Play(kBgmBattle, 1, 0, 0x3FFF);
            self->battleBgm = 1;
        }
        return 1;
    }
    if (self->deathState == 1)
        Bgm_StopAll(0);
    else if (self->deathState == 3 && gCurrentBgm != kBgmGameOver)
        Bgm_Play(kBgmGameOver, 0, 0, 0x3FFF);
    return 0;
}

// ── 0x1DD3A0 ──────────────────────────────────────────────────────────────
// Feeds the HUD: health, vigour, the readied sword, the time of day.
static void World_UpdateGauges(World* self) {
    PlayerData* pd = *gPlayerData;
    u8* params = CharaMgr_GetParams(Charas(self), 0);
    int time = 0;
    if (self->kind == kWorldField || self->kind == kWorldAskStatus) {
        const s32* clock = P<s32>(*reinterpret_cast<u32*>(params + 0xD0));
        time = (clock ? *clock : 0) + pd->periodTime;
        if (time > 899)
            time = 900;
    }
    const Sword* sword = &pd->sword[PlayerData_GetReadiedSword(pd)];
    struct {
        s32 x, y; u32 z; u32 _c, _10; u32 money; s32 healthMax, health, vigourMax, vigour;
        s32 swordSegments; s32 swordHeat; s32 time; s32 timeMax; s32 period;
    } g;
    memset(&g, 0, sizeof(g));
    Native_HudAnchor(1);                // (native: the edge of a wider picture, if asked for)
    g.x = (s32)(Screen_X(0, -1) & 0xFFFF) + 0x180;
    Native_HudAnchor(0);
    g.y = (s32)(Screen_Y(0, -1) & 0xFFFF) + 0x1720;
    g.z = 0xFFF000;
    g.money = pd->_6e80[0];
    g.healthMax = *reinterpret_cast<s16*>(params + 0x42);
    g.health = *reinterpret_cast<s16*>(params + 0x40);
    g.vigourMax = pd->_10a;
    g.vigour = pd->_108;
    g.swordSegments = sword->durability;
    g.swordHeat = sword->_80;
    g.time = time;
    g.timeMax = 900;
    g.period = pd->time;
    Hud_SetGauges(*gHud, &g);
}

// ── 0x1DC480 ──────────────────────────────────────────────────────────────
// The day ends: the screen darkens, "the day is over" appears.  0 once done.
int World_DayEndPending(World* self) {
    if (self->dayEnding == 0) {
        self->dayEndFade = 0;
        self->dayEnding = 1;
        self->dayEndShown = 0;
        Bgm_StopAll(0);
    }
    if (self->dayEndFade < 0x41) {
        self->dayEndFade += 8;
    } else if (self->dayEndShown == 0) {
        self->dayEndShown = 1;
        Hud_ShowGameOver(*gHud);
        Sound_PlaySE(0x2A, 0, 0x3FFF);
    }
    World_UpdateGauges(self);
    Hud_Update(*gHud);
    const int busy = Hud_GameOverBusy(*gHud);
    return self->dayEndShown == 0 ? 1 : busy;
}

// ── 0x1DC580 ──────────────────────────────────────────────────────────────
// Starts the death sequence the first time; afterwards reports its state.
int World_DeathPending(World* self) {
    if (self->dying == 0) {
        self->dying = 1;
        self->deathTimer = 0;
        self->deathState = 1;
        self->savedSeVolume = gSeVolume;
        gSeVolume = 0;
        CharaMgr_OnPlayerDeath(Charas(self));
        return 0;
    }
    return self->deathState;
}

// ── 0x1DC5E0 ──────────────────────────────────────────────────────────────
void World_DropSwordAtPlayer(World* self, int sword) {
    if (SwordDb_Get() && self->charaMgr) {
        alignas(16) float pos[4];
        memcpy(pos, CharaMgr_GetPosition(Charas(self), 0), sizeof(pos));
        SwordDb_DropAt(SwordDb_Get(), sword, pos);
    }
}

// ── 0x1DC660 ──────────────────────────────────────────────────────────────
int World_SwordKinds() {
    return SwordDb_Get() ? SwordDb_Get()->swordCount : 0;
}

// ── 0x1DC6A0 ──────────────────────────────────────────────────────────────
int World_SetSword(World* self, Sword* sword) {
    if (self->charaMgr == 0)
        return -1;
    return CharaMgr_EquipSword(Charas(self), SwordDb_Get(), sword) & 0xFF;
}

static void LoadMap(World* self, int map) {
    ItemDrops_Clear(P(self->itemDrops));
    ItemDrops_LoadMap(P(self->itemDrops), map);
    MapState_Load(P(self->mapState), map);
    MapState_SetCurrent(P(self->mapState), map);
    Camera_Reset(*gCamera);
    Camera_SetMap(*gCamera, map);
}

// ── 0x1DC770 ──────────────────────────────────────────────────────────────
// Used by story scenes: drops what is loaded and brings up a map by index.
void World_GoToMap(World* self, s16 map) {
    for (int i = 0; i < 2; i++) {                       // 0x1E0710: two blank frames
        GsDevice_BeginFrame(*gGsDevice);
        GsDevice_EndFrame(*gGsDevice);
        sceGsSyncPath(0, 0);
        GsDevice_SwapBuffers(*gGsDevice, 0);
    }
    if (self->stage)
        *reinterpret_cast<u32*>(StageBytes(self) + 0xC4) = 0;
    if (SwordDb_Get())
        SwordDb_FreeModels(SwordDb_Get());
    CharaResources_Purge(CharaResources_Get());
    self->area = -1;
    ItemDrops_Clear(P(self->itemDrops));
    ItemDrops_LoadMap(P(self->itemDrops), map);
    Camera_Reset(*gCamera);
    Camera_SetMap(*gCamera, map);
    MapState_Load(P(self->mapState), map);
    MapState_SetCurrent(P(self->mapState), map);
    // Which area the map belongs to.
    for (int area = 0; area < 11; area++) {
        for (const s16* m = kAreaMaps + area * 0x14; *m != -1; m++) {
            if (*m == map) {
                (*gPlayerData)->area = (s8)area;
                return;
            }
        }
    }
}

static void HideHint(s32& shown) {
    if (shown != 0) {
        shown = 0;
        Hud_HideHint(*gHud);
    }
}

// ── 0x1DC880 ──────────────────────────────────────────────────────────────
int World_UseExit(World* self, int exit) {
    void* charas = Charas(self);
    bool go = exit != -1;
    if (exit == -1) {
        // Is the player standing at a door, and not fighting?
        u8* params = CharaMgr_GetParams(charas, 0);
        if (!params)
            return 0;
        bool atDoor = false;
        if ((CharaMgr_GetFlags(charas, 0) & 0x200) == 0) {
            const u32* state = P<u32>(*reinterpret_cast<u32*>(params + 0xCC));
            if (((state ? *state : 0) & 0xF0000) != 0 && Effects_IsBusy(P(self->effects), 0) == 0) {
                atDoor = true;
                exit = CharaMgr_DoorAt(charas, 0);
                PlayerData* pd = *gPlayerData;
                if (exit == -1) {
                    HideHint(self->doorHint);
                } else if (MapDb_GetExitMap(MapDb_Get(), pd->area, exit, pd->night) != -1) {
                    if ((CharaMgr_GetFlags(charas, 0) & 0x100) == 0) {
                        go = true;
                    } else if (self->doorHint == 0) {       // "you cannot leave while fighting"
                        if (RenderMgr_FindEffect(*gRenderMgr, 8) == 0)
                            Hud_ShowHint(*gHud, Text(0x13A), 7);
                        self->doorHint = 1;
                    }
                }
            }
        }
        if (!atDoor && self->doorHint != 0 && (CharaMgr_GetFlags(charas, 0) & 0x100) == 0)
            HideHint(self->doorHint);
    }
    if (!go)
        return 0;
    HideHint(self->doorHint);
    const s8 variant = Variant();
    self->area = (*gPlayerData)->area;
    MapDb* db = MapDb_Get();
    const s32 target = MapDb_GetExitMap(db, self->area, exit, variant);
    if (target == -1)
        return 0;
    self->doorIndex = exit;
    self->targetMap = target;
    if (MapDb_GetExitScript(db, self->area, self->doorIndex, variant) == nullptr)
        CharaMgr_Release(charas, 0);
    else
        CharaMgr_SetIdle(charas, 0);                    // a shop: its script takes over
    Camera_Reset(*gCamera);
    Camera_SetMap(*gCamera, self->targetMap);
    MapState_LoadNext(P(self->mapState), self->targetMap);
    MapState_SetNext(P(self->mapState), self->targetMap);
    SwordDb_ChangeMap(SwordDb_Get(), self->targetMap, MapDb_GetAreaMap(db, self->area, variant));
    self->placePlayer = 1;
    return 8;
}

// ── 0x1DCBB0 ──────────────────────────────────────────────────────────────
const char* World_TakeDoorScript(World* self) {
    if (World_GetIndoors() != 1)
        return nullptr;
    return MapDb_GetExitScript(MapDb_Get(), self->area, self->doorIndex, Variant());
}

// ── 0x1DCC20 ──────────────────────────────────────────────────────────────
int World_CheckExit(World* self) {
    if ((CharaMgr_GetFlags(Charas(self), 0) & 0x200) != 0)
        return 0;
    const int exit = CharaMgr_ExitAt(Charas(self));
    if (exit == -1) {
        HideHint(self->exitHint);
        return 0;
    }
    if (exit == 0xFFFFFF) {                             // at an exit that cannot be used now
        if (self->exitHint == 0 && RenderMgr_FindEffect(*gRenderMgr, 8) == 0) {
            Hud_ShowHint(*gHud, Text(0x139), 7);
            self->exitHint = 1;
        }
        return 0;
    }
    self->exitUsed = exit;
    return World_GetIndoors() == 1 ? 1 : 2;
}

// ── 0x1DCD70 ──────────────────────────────────────────────────────────────
// "<area>: talk" while someone can be talked to.
int World_UpdateTalkHint(World* self, int area) {
    if (self->introPending != 0)
        return 0;
    const int target = CharaMgr_TalkTarget(Charas(self));
    if (self->talkHint == 0) {
        if (target != 0) {
            WStr_Copy(sHintText, Text(area + 0x52));
            WStr_Append(sHintText, Text(0x13E), 0x80);
            Hud_ShowHint(*gHud, sHintText, 7);
            self->talkHint = 1;
            return 1;
        }
    } else if (target == 0) {
        self->talkHint = 0;
        Hud_HideHint(*gHud);
    }
    return 0;
}

static void AttachCamera(World* self, const void* config) {
    void* mem = MemAlloc(0xC0);
    void* cam = mem ? FollowCam_Ctor(mem, CharaMgr_GetParams(Charas(self), 0), config) : nullptr;
    CamList_Push(CamList(self), cam);
}

// ── 0x1DCE90 ──────────────────────────────────────────────────────────────
// The screen is black: swap to the map the door leads to — or, when coming
// back out, to the area's own map.
void World_EnterMap(World* self, int arrest) {
    App_PresentBlackFrames();
    void* charas = Charas(self);
    CharaMgr_ClearTransient(charas);
    if (World_GetIndoors() == 1) {
        const s8 variant = Variant();
        MapDb* db = MapDb_Get();
        const s32 map = MapDb_GetAreaMap(db, self->area, variant);
        MapState_Return(P(self->mapState), map);
        World_SetIndoors(0);
        CharaMgr_Respawn(charas, 0, -1);
        World_PlacePlayer(self, self->exitUsed + 1);
        CharaMgr_ResetAll(charas);
        ItemDrops_Reset(P(self->itemDrops));
        ItemDrops_LoadMap(P(self->itemDrops), map);
        Camera_Reset(*gCamera);
        Camera_SetMap(*gCamera, map);
        SwordDb_ChangeMap(SwordDb_Get(), map, self->targetMap);
        CharaMgr_Release(charas, 0);
        self->flicker.level = 0.0f;
        self->flicker.wait = 0;
        self->flicker._04 = 0;
        self->flicker.direction = 1.0f;
        self->flicker.high = 0.05f;
        self->flicker.low = 0.001f;
        self->flicker.scale = 1.0f;
    } else {
        World_SetIndoors(1);
        if (arrest == 0) {
            CharaMgr_Respawn(charas, 1, self->doorIndex);
        } else {
            CharaMgr_Respawn(charas, 1, -1);
            self->doorIndex = -1;
        }
        CharaMgr_ResetAll(charas);
        ItemDrops_Reset(P(self->itemDrops));
        ItemDrops_LoadMap(P(self->itemDrops), self->targetMap);
        self->flicker.scale = 0.1f;
    }
    u8* params = reinterpret_cast<u8*>(CharaResources_GetParams(CharaResources_Get(), 0));
    *reinterpret_cast<s16*>(params + 0x48) = -1;
    CharaMgr_fn143c60(charas, 0);
    CharaMgr_fn1466e0(charas, 0, 0);
    if (CharaMgr_GetFlags(charas, 0) & 0x100)
        CharaMgr_Sheathe(charas, 0, 0);
    if (CharaMgr_GetFlags(charas, 0) & 6)
        CharaMgr_fn146230(charas, 0);
    if (CharaMgr_GetFlags(charas, 0) & 8)
        CharaMgr_fn146230(charas, 0);
    if (self->flagsObj)
        FlagsObj_Reset(P(self->flagsObj));
}

// ── 0x1C8DB0 ──────────────────────────────────────────────────────────────
// (A character function; here because the world is its first user.)
void Matrix_FaceTowards(float* matrix, const float* point) {
    float x = point[0] - matrix[12], z = point[2] - matrix[14];
    const float length = sqrtf(x * x + z * z);
    if (length != 0.0f) {
        x /= length;
        z /= length;
    }
    matrix[0] = z;
    matrix[2] = -x;
    matrix[8] = x;
    matrix[10] = z;
}

// ── 0x1DD150 ──────────────────────────────────────────────────────────────
// The map's collision file names its points; each way in has a pair of them:
// where to stand and what to face (the walk-in heads for the second).
void World_PlacePlayer(World* self, int exit) {
    static const s32* const kSpawnNames = guest::ptr<const s32>(0x2D92E0u);
    MapCollision* map = P<MapCollision>(MapState_GetMap(P(self->mapState)));
    GuestList* found = Map_FindPoints(map, kSpawnNames[exit * 2]);
    if (found->size == 0)
        return;
    alignas(16) float stand[4], face[4];
    memcpy(stand, P<float>(GuestList_Begin(found)->value), sizeof(stand));
    // (The search reuses one list, so this replaces what was just found.)
    found = Map_FindPoints(map, kSpawnNames[exit * 2 + 1]);
    if (found->size == 0)
        return;
    memcpy(face, P<float>(GuestList_Begin(found)->value), sizeof(face));
    memcpy(self->startPos, face, sizeof(face));
    void* charas = Charas(self);
    CharaMgr_SetPosition(charas, 0, stand);
    alignas(16) float matrix[16];
    Matrix_Copy(matrix, CharaMgr_GetTransform(charas, 0));
    Matrix_FaceTowards(matrix, face);
    AttachCamera(self, World_GetIndoors() == 1 ? kCamIndoors : kCamOutdoors);
    CharaMgr_SetTransform(charas, 0, matrix);
}

// ── 0x1DD530 ──────────────────────────────────────────────────────────────
int World_Draw(World* self) {
    // Nothing is drawn while a map is still coming in.
    const s32* loading = P<s32>(self->mapState);
    if ((World_GetIndoors() == 0 ? loading[3] : loading[4]) != -1)
        return 0;
    void* charas = Charas(self);
    if (self->dying != 0) {
        GsDevice_ClearScreenColor(*gGsDevice, 0x80050C62u);
        CharaMgr_Draw(charas, 0);
        RenderMgr_Draw(*gRenderMgr);
        Hud_Draw(*gHud);
        return 1;
    }
    void* stage = P(self->stage);
    const float* viewer = CharaMgr_Exists(charas, 0) ? CharaMgr_GetPosition(charas, 0) : nullptr;
    if (self->hideMap != 0) {
        GsDevice_ClearScreenColor(*gGsDevice, 0x80908982u);
    } else {
        MapState_BeginDraw(P(self->mapState), stage);
        MapState_Draw(P(self->mapState), viewer, stage, StageBytes(self) + 0x10);
        MapState_EndDraw(P(self->mapState));
    }
    CharaMgr_Draw(charas, (CamList_GetFlags(CamList(self)) & 2) != 0);
    if (self->hideMap == 0) {
        ItemDrops_Draw(P(self->itemDrops));
        SwordDb_Draw(SwordDb_Get());
    }
    Targeting_Draw(P(self->targeting), MapState_GetMap(P(self->mapState)));
    RenderMgr_Draw(*gRenderMgr);
    if (self->dayEnding != 0)
        GsDevice_ClearScreenColor(*gGsDevice, (u32)(u8)self->dayEndFade << 24);
    if (self->hideHud == 0) {
        Hud_Draw(*gHud);
        Effects_Draw(P(self->effects));
    }
    return 1;
}

// ── 0x1DD710 ──────────────────────────────────────────────────────────────
int World_Step(World* self) {
    // The death sequence: three seconds after "game over" has appeared, done.
    if (self->dying != 0) {
        if (self->deathState == 1)
            self->deathState = 2;
        Camera_Update(*gCamera, *gGsDevice);
        const int shown = Hud_DeathState(*gHud);
        if (shown == 1) {
            self->deathState = 3;
        } else if (shown == 2 && self->deathTimer++ >= gFrameRate * 3) {
            Hud_EndDeath(*gHud);
            gSeVolume = self->savedSeVolume;
            return 4;
        }
    }
    void* mapState = P(self->mapState);
    if (MapState_IsLoaded(mapState) == 0) {
        if (gLoadingActive == 0) {
            gLoadingFrames = 0;
            gLoadingDots = 0;
            gLoadingDotTimer = 0;
        }
        gLoadingActive = 1;
        return 0;
    }
    void* charas = Charas(self);
    P<u32>(self->charaMgr)[4] = self->kind == kWorldEvent;
    Map_SetEventMode(P<MapCollision>(MapState_GetMap(mapState)), self->kind != kWorldField);
    u8* stage = StageBytes(self);
    void* flags = P(self->flagsObj);
    SwordDb* swords = SwordDb_Get();
    int result = self->paused != 0;

    if (self->paused == 0 && CharaMgr_IsBusy(charas) != 0) {
        const char swordsReady = SwordDb_PollLoads(swords);
        if (swordsReady)
            ItemDrops_OnSwordsLoaded(P(self->itemDrops));
        result = (CharaMgr_IsReady(charas) & Targeting_IsReady(P(self->targeting))) & 0xFF;
        if (self->deathTimer == 0) {
            // Shadows follow whether their character is shown.
            for (GuestListNode* n = GuestList_Begin(&self->shadows); n != GuestList_End(&self->shadows);
                 n = GuestList_Next(n)) {
                const u32* entry = P<u32>(n->value);
                if (entry[1] != 0)
                    Shadow_SetHidden(P(entry[1]), CharaMgr_IsHidden(charas, (int)entry[0]) == 0);
            }
            CharaMgr_StepA(charas);
            if (LightFlicker_Step(&self->flicker, self->flash) != 0) {
                memcpy(P<u8>(self->charaMgr) + 0x20, self->flash, sizeof(self->flash));
                RenderMgr_SetFlash(*gRenderMgr, self->flash);
            }
            Collision_Update(charas, mapState, P(self->itemDrops));
            Pairs_Update(P(self->pairs), charas, mapState);
            Effects_Update(P(self->effects), stage + 0x60, charas, *gHud);
            CharaMgr_StepB(charas);
            CharaMgr_StepFlags(charas, flags);
            CharaMgr_Update(charas, swordsReady);
            CharaMgr_StepDrops(charas, P(self->itemDrops), swords);
            CharaMgr_StepC(charas);
            CharaMgr_StepMap(charas, MapState_GetMap(mapState), stage);
            CharaMgr_StepD(charas);
            ItemDrops_Update(P(self->itemDrops), MapState_GetMap(mapState));
            FlagsObj_StepA(flags);
            FlagsObj_StepB(flags);
            CharaMgr_StepFlags2(charas, flags);
        }
        Hud_Update(*gHud);
        CharaMgr_StepSwords(charas, swords);
        SwordDb_Update(swords, MapState_GetMap(mapState));
        if (RenderMgr_FindEffect(*gRenderMgr, 8) == 0 && RenderMgr_FindEffect(*gRenderMgr, 11) == 0) {
            Pairs_UpdateHud(P(self->pairs), charas, mapState);
            World_UpdateGauges(self);
        }
        if (RenderMgr_FindEffect(*gRenderMgr, 8) == 0 && self->charaMgr != 0 && self->itemDrops != 0
            && SwordDb_Get() != nullptr) {
            if (u8* params = CharaMgr_GetParams(charas, 0))
                Hud_UpdateDrops(*gHud, P(self->itemDrops), params + 0x50, SwordDb_Get());
        }
        *reinterpret_cast<u32*>(stage + 0xC4) = MapState_GetMap(mapState);
    }
    if (self->paused == 0) {
        Stage_Update(stage);
        RenderMgr_SetMap(*gRenderMgr, MapState_GetMap(mapState));
        Camera_Finish(*gCamera);
        Stage_Apply(stage, reinterpret_cast<u8*>(*gGsDevice) + 0x360);
        GsDevice_ApplyView(*gGsDevice);
        if (CharaMgr_GetFlags(charas, 0) & 0x200)
            result = 2;
    }
    return result;
}

// ── 0x1DDBE0 ──────────────────────────────────────────────────────────────
// Returns 1 if the player was put back where a save had left it.
int World_PlaceAtStart(World* self) {
    int restored = 0;
    if (self->placePlayer != 0) {
        PlayerData* pd = *gPlayerData;
        const float* saved = pd->_6e40;
        if (saved[12] == 0.0f && saved[13] == 0.0f && saved[14] == 0.0f) {
            World_PlacePlayer(self, 0);
        } else {
            AttachCamera(self, World_GetIndoors() == 1 ? kCamIndoors : kCamOutdoors);
            CharaMgr_SetTransform(Charas(self), 0, saved);
            Matrix_Copy(pd->_6e40, guest::ptr(0x2D1A80u));
            CharaMgr_SetState(Charas(self), 0, 2, 0, 0);
            restored = 1;
        }
        self->placePlayer = 0;
    }
    CamList_Update(CamList(self));
    CamList_Apply(CamList(self));
    return restored;
}

static bool EndIntro(World* self) {
    RenderMgr_TriggerEffect(*gRenderMgr, 8, 0, 0);
    if (RenderMgr_FindEffect(*gRenderMgr, 8) != 0)
        return false;
    CharaMgr_SetState(Charas(self), 0, 0, self->stage + 0x10, 0);
    DayMgr_SetEvent(DayMgr_Get(), 0);
    return true;
}

// ── 0x1DDD80 ──────────────────────────────────────────────────────────────
// On arrival the player walks a few steps in by itself, behind letterbox
// bars (render effect 8).  Returns 1 on the frame control is handed over.
int World_IntroDone(World* self) {
    if (self->introPending == 0 || World_GetIndoors() == 1)
        return 0;
    if (self->introAction == CharaMgr_GetAction(Charas(self), 0) || self->introTimer++ > gFrameRate << 1) {
        if (EndIntro(self)) {
            self->introPending = 0;
            return 1;
        }
    } else if (self->introAction == -1) {
        if (EndIntro(self)) {
            self->introAction = 0xFFFFFFF;
            self->introPending = 0;
            return 1;
        }
    }
    return 0;
}

// ── 0x1DDF00 ──────────────────────────────────────────────────────────────
int World_GetHint(World* self) {
    if (World_GetIndoors() == 1)
        return -1;
    const int spot = CharaMgr_HintSpot(Charas(self));
    if (spot == -1)
        return -1;
    const s32* hints = kAreaHints + (*gPlayerData)->area * 12;
    for (int i = 0; i < 6; i++)
        if (hints[i * 2] == spot)
            return hints[i * 2 + 1];
    return -1;
}

// ── 0x1DDFC0 ──────────────────────────────────────────────────────────────
int World_StartIntro(World* self) {
    if (World_GetIndoors() == 1 || self->introAction != -1)
        return 0;
    float* target = static_cast<float*>(MemAlloc(0x10));        // on the stack in the original
    memcpy(target, self->startPos, 0x10);
    target[3] = 0.0f;
    self->introAction = CharaMgr_SetState(Charas(self), 0, 2, 8, guest::addr(target));
    MemFree(target);
    DayMgr_SetEvent(DayMgr_Get(), 1);
    return 1;
}

// ── 0x1DE050 ──────────────────────────────────────────────────────────────
void World_Begin(World* self) {
    self->introAction = -1;
    self->introTimer = 0;
    RenderMgr_TriggerEffect(*gRenderMgr, 8, 1, 0);
}

// ── 0x1DE070 ──────────────────────────────────────────────────────────────
static void World_CreatePlayer(World* self) {
    PlayerData* pd = *gPlayerData;
    void* mem = MemAlloc(0x120);
    self->charaMgr = mem ? guest::addr(CharaMgr_Ctor(mem)) : 0;
    CharaMgr_Init(Charas(self), 0);
    if (pd->swordCount == 0) {
        Sword sword;
        Sword_Ctor(&sword);
        SwordDb_MakeSword(SwordDb_Get(), pd->_118, &sword);
        PlayerData_CarrySword(pd, &sword);
        Sword_Dtor(&sword, -1);
    }
    ActorDesc actor = *kPlayerActor;
    actor.sword = PlayerData_GetReadiedSword(pd);
    actor.chara = pd->chara;
    actor.state[1] = (s32)(self->stage + 0x10);
    World_AddActor(self, &actor, -1, 0);

    pd = *gPlayerData;
    u8* params = reinterpret_cast<u8*>(CharaResources_GetParams(CharaResources_Get(), 0));
    if (pd->health != 0) {
        s16& max = *reinterpret_cast<s16*>(params + 0x42);
        max = pd->healthMax;
        *reinterpret_cast<s16*>(params + 0x40) = max <= pd->health ? max : pd->health;
    }
    AttachCamera(self, kCamOutdoors);
    CharaResources_Prepare(CharaResources_Get());
    PlayerData_ClearSwordMarks(*gPlayerData, -1);
}

static void ForgetShadow(World* self, int id) {
    for (GuestListNode* n = GuestList_Begin(&self->shadows); n != GuestList_End(&self->shadows);
         n = GuestList_Next(n)) {
        u32* entry = P<u32>(n->value);
        if ((int)entry[0] != id)
            continue;
        if (entry[1] != 0)
            Shadow_Release(P(entry[1]));
        MemFree(entry);
        GuestList_Erase(&self->shadows, n);
        return;
    }
}

// ── 0x1DE230 ──────────────────────────────────────────────────────────────
void World_RemoveActor(World* self, int id) {
    void* charas = Charas(self);
    CharaMgr_fn143140(charas, id);
    ItemDrops_ForgetChara(P(self->itemDrops), id);
    CharaMgr_fn144d90(charas, id);
    CharaMgr_Unflag(charas, P(self->flagsObj), id);
    CharaMgr_Remove(charas, id);
    if (id != 0)
        ForgetShadow(self, id);
}

// ── 0x1DE320 ──────────────────────────────────────────────────────────────
void World_AddActor(World* self, const ActorDesc* actor, int a, int b) {
    PlayerData* pd = *gPlayerData;
    void* charas = Charas(self);
    const int id = actor->id;
    CharaMgr_Add(charas, id, actor->kind, actor->chara, a);
    CharaMgr_SetState(charas, id, actor->state[0], (u32)actor->state[1], (u32)actor->state[2]);
    CharaMgr_QueueLoad(charas, id);
    CharaMgr_Flag(charas, P(self->flagsObj), id);
    if (actor->sword != -1) {
        if (id == 0)
            CharaMgr_EquipSword(charas, SwordDb_Get(), PlayerData_ReadySword(pd, actor->sword));
        else
            CharaMgr_EquipKind(charas, id, actor->sword, b);
    }
    CharaMgr_SetPosition(charas, id, actor->position);
    if (id != 0) {
        // Everyone but the player gets a shadow, remembered by character id.
        void* shadow = P(RenderMgr_TriggerEffect(*gRenderMgr, 0x0F, guest::addr(CharaMgr_GetParams(charas, id)),
                                                 MapState_GetMap(P(self->mapState))));
        ForgetShadow(self, id);
        u32* entry = static_cast<u32*>(MemAlloc(8));
        entry[0] = (u32)id;
        entry[1] = guest::addr(shadow);
        GuestList_Insert(&self->shadows, GuestList_End(&self->shadows), guest::addr(entry));
    }
    if ((actor->group & 0x80) == 0) {
        CharaMgr_SetGrouped(charas, id, 0);
    } else {
        CharaMgr_SetGrouped(charas, id, 1);
        CharaMgr_SetGroup(charas, id, actor->group == 0xFF ? -1 : (actor->group & 0x7F));
    }
    CharaMgr_Spawn(charas, id, World_GetIndoors(), -1);
}

// ── 0x1DE5F0 ──────────────────────────────────────────────────────────────
int World_Init(World* self) {
    void* mem = MemAlloc(0xD0);
    self->stage = mem ? guest::addr(Stage_Ctor(mem)) : 0;
    mem = MemAlloc(0x10);
    self->flagsObj = mem ? guest::addr(FlagsObj_Ctor(mem)) : 0;
    mem = MemAlloc(0x80);
    self->mapState = mem ? guest::addr(MapState_Ctor(mem)) : 0;
    mem = MemAlloc(0x3C);
    self->effects = mem ? guest::addr(Effects_Ctor(mem)) : 0;
    if (self->itemDrops == 0) {
        mem = MemAlloc(0x80);
        self->itemDrops = mem ? guest::addr(ItemDrops_Ctor(mem, self->flagsObj)) : 0;
        self->ownsItemDrops = 1;
    } else {
        ItemDrops_SetFlagsObj(P(self->itemDrops), self->flagsObj);
        self->ownsItemDrops = 0;
    }
    mem = MemAlloc(8);
    self->pairs = mem ? guest::addr(Pairs_Ctor(mem)) : 0;
    Hud_Load(*gHud);
    World_CreatePlayer(self);
    if (self->area != -1) {
        LoadMap(self, MapDb_GetAreaMap(MapDb_Get(), self->area, Variant()));
        self->placePlayer = 1;
    }
    mem = MemAlloc(0x18);
    self->targeting = mem ? guest::addr(Targeting_Ctor(mem)) : 0;
    Targeting_SetPlayer(P(self->targeting), CharaMgr_GetParams(Charas(self), 0));
    return 0;
}

// ── 0x1DE7D0 ──────────────────────────────────────────────────────────────
static void World_DestroyParts(World* self) {
    Camera_Reset(*gCamera);
    Camera_Release(*gCamera);
    GuestListNode* n = GuestList_Begin(&self->shadows);
    while (n != GuestList_End(&self->shadows)) {
        u32* entry = P<u32>(n->value);
        if (entry[1] != 0)
            Shadow_Release(P(entry[1]));
        MemFree(entry);
        n = GuestList_Erase(&self->shadows, n);
    }
    if (self->mapState)  MapState_Dtor(P(self->mapState), 1);
    self->mapState = 0;
    if (self->charaMgr)  CharaMgr_Dtor(Charas(self), 1);
    self->charaMgr = 0;
    if (self->stage)     Stage_Dtor(P(self->stage), 1);
    self->stage = 0;
    if (self->effects)   Effects_Dtor(P(self->effects), 1);
    self->effects = 0;
    if (self->pairs)     Pairs_Dtor(P(self->pairs), 1);
    self->pairs = 0;
    if (self->targeting) Targeting_Dtor(P(self->targeting), 1);
    self->targeting = 0;
    if (self->ownsItemDrops == 0) {
        ItemDrops_SetFlagsObj(P(self->itemDrops), 0);
    } else {
        if (self->itemDrops)
            ItemDrops_Dtor(P(self->itemDrops), 1);
        self->itemDrops = 0;
    }
    if (self->flagsObj)  FlagsObj_Dtor(P(self->flagsObj), 1);
    self->flagsObj = 0;
}

static void ReleaseWorldLink() {
    if (gWorldLink != 0)
        VirtualCall<void, void*, int>(*P<u32>(gWorldLink), 8, P(gWorldLink), 1);
    gWorldLink = 0;
}

// ── 0x1DE980 ──────────────────────────────────────────────────────────────
static void World_Construct(World* self) {
    self->stage = self->mapState = self->charaMgr = self->itemDrops = 0;
    self->flagsObj = self->effects = self->pairs = self->targeting = 0;
    self->kind = kWorldField;
    self->savedBgm = gCurrentBgm;
    self->battleBgm = 0;
    self->dying = 0;
    self->dayEnding = 0;
    self->ownsItemDrops = 0;
    self->exitHint = 0;
    self->doorHint = 0;
    self->paused = 0;
    self->talkHint = 0;
    self->ownsHud2 = 0;
    self->introPending = 1;
    self->hideMap = 0;
    self->hideHud = 0;
    self->deathTimer = 0;
    self->deathState = 0;
    self->area = -1;
    self->exitUsed = -1;
    self->targetMap = -1;
    self->doorIndex = -1;
    self->_28 = -1;
    self->_2c = -1;
    self->placePlayer = 1;
    self->handOver = 0;
    self->dialogChoice = 0;
    GuestList_Clear(&self->shadows);
    self->flicker.level = 0.0f;
    self->flicker.wait = 0;
    self->flicker._04 = 0;
    self->flicker.direction = 1.0f;
    self->flicker.high = 0.05f;
    self->flicker.low = 0.001f;
    self->flicker.scale = 1.0f;
    memcpy(self->flash, guest::ptr(0x306900u), sizeof(self->flash));
    CharaResources_Load(CharaResources_Get(), 0);
    MapDb_Load(MapDb_Get(), 0);
    SwordDb_Get();
    void* mem = MemAlloc(8);
    void* link = mem ? WorldLink_Ctor(mem, self) : nullptr;
    ReleaseWorldLink();
    if (link)
        gWorldLink = guest::addr(link);
    if (gHud2 == 0) {
        mem = MemAlloc(0x22C);
        Hud2_Set(mem ? Hud2_Ctor(mem) : nullptr);
        self->ownsHud2 = 1;
    }
    CharaResources_Purge(CharaResources_Get());
}

// ── 0x1DEB70 ──────────────────────────────────────────────────────────────
World* World_Dtor(World* self, s16 flag) {
    if (!self)
        return self;
    Hud_Reset(*gHud);
    Collision_Release();
    World_DestroyParts(self);
    ReleaseWorldLink();
    if (self->ownsHud2 != 0)
        Hud2_Set(nullptr);
    SwordDb_Unload(SwordDb_Get());
    GuestList_Clear(&self->shadows);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x1DEC60 ──────────────────────────────────────────────────────────────
World* World_CtorArea(World* self, int area) {
    GuestList_Init(&self->shadows);
    World_Construct(self);
    self->area = area;
    return self;
}

// ── 0x1DECB0 ──────────────────────────────────────────────────────────────
World* World_Ctor(World* self) {
    GuestList_Init(&self->shadows);
    World_Construct(self);
    return self;
}
