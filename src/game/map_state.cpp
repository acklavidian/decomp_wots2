// The map state (World+0xE4) — decompiled from 0x14B640–0x14C3C0.  See
// include/game/map_state.h and "Map state" in the project notes.
//
// It keeps two loaders: the area's map (+0x14) and the room behind a door
// (+0x18).  Which one is in use — drawn, walked on, stepped — is decided by the
// indoors flag (*0x2EE518) alone, which World_EnterMap sets once the screen is
// black; so a door's room is only read once it is the room in use, and the
// street goes on being drawn until then.  Coming back out (MapState_Return)
// needs no reading at all: the street's loader was kept.
//
// A load is a machine of five steps, one a frame (MapState_IsLoaded, from
// World_Step, which waits until it answers 1): the sky, then (the map's light,
// fog and clear colour) the water, then the model, then the collision file,
// then the route tables.

#include "game/map_state.h"
#include "game/ai_moves.h"
#include "game/map_collision.h"
#include "game/map_db.h"
#include "native/guest.h"

#include <cstring>

struct GsDevice;

extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    void  GsDevice_SetFogParams(float a, float b, float c, float d, GsDevice* dev, const void* vec);  // 0x12E9D0
    void  GsDevice_SendColorReg(GsDevice* dev, u32 rgba);               // 0x130E70
    void  GsDevice_StoreClearColor(GsDevice* dev, u32 rgba);            // 0x130F30
    void  GsDevice_SetProjection(float scale, float aspectX, float aspectY, float centerX, float centerY,
                                 float zMin, float zMax, float nearZ, GsDevice* dev, float farZ);  // 0x12F8A0
    void  GsDevice_UpdateMatrices(GsDevice* dev);                       // 0x12ECA0
    void  GsDevice_ApplyView(GsDevice* dev);                            // 0x12EBD0
    void  Light_SetMatrices(void* light, const void* directions, const void* colours);   // 0x166920
    void  Light_Upload(void* light, void* dev);                         // 0x166910
    void  Light_UploadPoints(void* light, void* dev);                   // 0x165CA0
}

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const GuestPtrVar<void>     gLight{0x2EE678u};       // the lighting object ("Camera" in older notes)
static const GuestPtrVar<u8>       gPlayerData{0x2EE688u};
static s32* const   gIndoors   = guest::ptr<s32>(0x2EE518u);
static u32* const   gClearColor = guest::ptr<u32>(0x2EE6A8u);
static float* const gFog       = guest::ptr<float>(0x2D9460u);  // what GsDevice_SetFogParams is given
static const void*  const kFogVec = guest::ptr<const void>(0x2D9470u);
static float* const gFarZ      = guest::ptr<float>(0x2EDD28u);
static float* const gNearZ     = guest::ptr<float>(0x2EDD2Cu);
static float* const gZoom      = guest::ptr<float>(0x2EDD30u);

static inline MapLoader* L(u32 a) { return guest::ptr<MapLoader>(a); }

static MapLoader* NewLoader() {
    void* mem = MemAlloc(sizeof(MapLoader));
    return mem ? MapLoader_Ctor(static_cast<MapLoader*>(mem)) : nullptr;
}

extern "C" {

// ── 0x14B640 ──────────────────────────────────────────────────────────────
u32 MapState_GetMap(void* state) {
    MapState* self = static_cast<MapState*>(state);
    if (self->useFallback == 0) {
        if (*gIndoors == 0 && self->loader != 0)
            return L(self->loader)->collision;
        if (self->nextLoader != 0)
            return L(self->nextLoader)->collision;
    }
    return self->fallback;
}

// ── 0x14B8C0 / 0x14B6A0: one step of a load.  → 1 when it is over ─────────
// (The two are the same but for which map and loader they work on; the
// room's model is opened as kind 6, which the map database does not keep.)
static int StepLoad(MapState* self, s32* step, s32 map, u32 loaderAddr, int modelKind) {
    MapDb* db = MapDb_Get();
    MapLoader* loader = L(loaderAddr);
    switch (*step) {
    case 0:
        if (MapLoader_Finish(loader, 1, -1)) {
            if (const char* sky = MapDb_GetSkyPath(db, map))
                MapLoader_Open(loader, sky, *step);
            (*step)++;
        }
        break;
    case 1:
        if (MapLoader_Finish(loader, 1, 0)) {
            MapState_Return(self, map);
            if (const char* water = MapDb_GetWaterPath(db, map))
                MapLoader_Open(loader, water, *step);
            (*step)++;
        }
        break;
    case 2:
        if (MapLoader_Finish(loader, 1, 1)) {
            if (!MapLoader_Open(loader, MapDb_GetModelPath(db, map), modelKind == 6 ? 6 : *step))
                *step = 5;
            else
                (*step)++;
        }
        break;
    case 3:
        if (MapLoader_Finish(loader, 1, 2)) {
            MapLoader_Open(loader, MapDb_GetCollisionPath(db, map), *step);
            MapLoader_BuildModel(loader, loader->modelFile);
            (*step)++;
        }
        break;
    case 4:
        if (MapLoader_Finish(loader, 1, 3)) {
            MapLoader_SetCollision(loader, loader->collisionFile);
            MapState_LinkRoutePoints(self);
            MapState_BuildRouteTable(self);
            (*step)++;
        }
        break;
    default:
        *step = -1;
        return 1;
    }
    return 0;
}

// ── 0x14BAE0 / 0x14BAF0 ───────────────────────────────────────────────────
void MapState_SetNext(void* state, int map) {
    MapState* self = static_cast<MapState*>(state);
    self->nextMap = map;
    self->nextStep = 0;
}
void MapState_SetCurrent(void* state, int map) {
    MapState* self = static_cast<MapState*>(state);
    self->map = map;
    self->step = 0;
}

// ── 0x14BB60 / 0x14BB00: the sky before the map, the water after it ──────
void MapState_BeginDraw(void* state, void*) {
    MapState* self = static_cast<MapState*>(state);
    const u32 loader = *gIndoors == 0 ? self->loader : self->nextLoader;
    if (loader != 0)
        MapLoader_DrawSky(L(loader));       // (the original passes it 0.002, how far the sky turns)
}
void MapState_EndDraw(void* state) {
    MapState* self = static_cast<MapState*>(state);
    const u32 loader = *gIndoors == 0 ? self->loader : self->nextLoader;
    if (loader != 0)
        MapLoader_DrawWater(L(loader));
}

// ── 0x14BBD0 ──────────────────────────────────────────────────────────────
void MapState_Draw(void* state, const float* viewer, void* stage, void* lights) {
    MapState* self = static_cast<MapState*>(state);
    GsDevice* dev = *gGsDevice;
    const float zoom = *gZoom;
    GsDevice_SetFogParams(gFog[0], gFog[1], gFog[2], gFog[3], dev, kFogVec);
    GsDevice_SetProjection(-1.0f, zoom, zoom, 0.0f, 0.0f, 256.0f, 16773120.0f, *gNearZ, dev, *gFarZ);
    GsDevice_ApplyView(dev);
    Light_Upload(*gLight, dev);
    Light_UploadPoints(*gLight, dev);
    const u32 loader = *gIndoors == 0 ? self->loader : self->nextLoader;
    if (loader != 0)
        MapLoader_Draw(L(loader), viewer, stage, lights);
}

// ── 0x14BD20 ──────────────────────────────────────────────────────────────
int MapState_IsLoaded(void* state) {
    MapState* self = static_cast<MapState*>(state);
    if (*gIndoors == 0)
        return self->loader != 0 ? StepLoad(self, &self->step, self->map, self->loader, 2) : 0;
    return self->nextLoader != 0 ? StepLoad(self, &self->nextStep, self->nextMap, self->nextLoader, 6) : 0;
}

// ── 0x14BD80: a fresh loader for the room behind a door ──────────────────
void MapState_LoadNext(void* state, int map) {
    MapState* self = static_cast<MapState*>(state);
    if (self->nextLoader != 0)
        MapLoader_Dtor(L(self->nextLoader), 1);
    self->nextLoader = 0;
    *reinterpret_cast<s32*>(*gPlayerData + 0x6E88) = map;
    MapLoader* loader = NewLoader();
    self->nextLoader = guest::addr(loader);
    self->nextMap = map;
    MapDb* db = MapDb_Get();
    MapLoader_SetSkyParam(MapDb_GetSkyParam(db, map), loader);
    MapLoader_SetWaterHeight(MapDb_GetWaterParam(db, map), loader);
}

// ── 0x14BE30: a fresh loader for the area's map ──────────────────────────
void MapState_Load(void* state, int map) {
    MapState* self = static_cast<MapState*>(state);
    if (self->loader != 0)
        MapLoader_Dtor(L(self->loader), 1);
    self->loader = 0;
    *reinterpret_cast<s32*>(*gPlayerData + 0x6E84) = map;
    self->map = map;
    MapLoader* loader = NewLoader();
    self->loader = guest::addr(loader);
    MapDb* db = MapDb_Get();
    MapLoader_SetSkyParam(MapDb_GetSkyParam(db, map), loader);
    MapLoader_SetWaterHeight(MapDb_GetWaterParam(db, map), loader);
}

// ── 0x14BEE0: a map's clear colour, light and fog become the screen's ────
// (Also how the street's look comes back when the player leaves a room.)
void MapState_Return(void* state, int map) {
    MapState* self = static_cast<MapState*>(state);
    MapDb* db = MapDb_Get();
    const u32 clear = MapDb_GetClearColor(db, map);
    if (clear != 0)
        *gClearColor = clear;
    const float* colours = MapDb_GetLights(db, map);
    const float* directions = MapDb_GetLightColors(db, map);
    const float* fog = MapDb_GetFog(db, map);
    Light_SetMatrices(*gLight, directions, colours);
    Light_Upload(*gLight, *gGsDevice);
    memcpy(gFog, fog, 16);
    float farZ = MapDb_GetFarPlane(db, map);
    if (farZ == -1.0f)
        farZ = *gFarZ;
    *gFarZ = farZ;
    // 0x1E1F20: the screen's fog, clear colour and projection.
    GsDevice* dev = *gGsDevice;
    GsDevice_SetFogParams(gFog[0], gFog[1], gFog[2], gFog[3], dev, kFogVec);
    GsDevice_SendColorReg(dev, *gClearColor);
    GsDevice_StoreClearColor(dev, *gClearColor);
    GsDevice_SetProjection(-1.0f, *gZoom, *gZoom, 0.0f, 0.0f, 256.0f, 16773120.0f, *gNearZ, dev, *gFarZ);
    GsDevice_UpdateMatrices(dev);
    if (self->loader != 0)
        MapLoader_SetFrustum(L(self->loader));
    else if (self->nextLoader != 0)
        MapLoader_SetFrustum(L(self->nextLoader));
}

// ── 0x14C0B0 ──────────────────────────────────────────────────────────────
void MapState_Dtor(void* state, int flag) {
    MapState* self = static_cast<MapState*>(state);
    if (self == nullptr)
        return;
    if (self->loader != 0)
        MapLoader_Dtor(L(self->loader), 1);
    self->loader = 0;
    if (self->nextLoader != 0)
        MapLoader_Dtor(L(self->nextLoader), 1);
    self->nextLoader = 0;
    if (self->fallback != 0)
        MapCollision_Dtor(guest::ptr<MapCollision>(self->fallback), 1);
    self->fallback = 0;
    // (The route tables' nodes are not given back: as before this was decompiled.)
    if ((s16)flag > 0)
        MemFree(self);
}

// ── 0x14C330 ──────────────────────────────────────────────────────────────
void* MapState_Ctor(void* state) {
    MapState* self = static_cast<MapState*>(state);
    RouteTables_Init(reinterpret_cast<RouteTables*>(self->routes));
    self->map = 0;
    self->nextMap = 0;
    self->step = -1;
    self->nextStep = -1;
    // A new map state starts out of doors.  (A world made while the flag was
    // still set — after resting in a house, which leaves by way of the area
    // map — would otherwise never end its walk-in: World_IntroDone answers 0
    // indoors.)
    *gIndoors = 0;
    self->useFallback = 0;
    self->loader = 0;
    self->nextLoader = 0;
    void* mem = MemAlloc(sizeof(MapCollision));
    self->fallback = mem ? guest::addr(MapCollision_Ctor(static_cast<MapCollision*>(mem))) : 0;
    return self;
}

}  // extern "C"
