#pragma once
// The map state (World+0xE4, 0x80 bytes; 0x14B640–0x14C3C0): two map
// loaders — the area's map and the room behind a door — each read in five
// steps, and the route tables of both.  See "Map state" in the project notes.
#include "hal/ps2_types.h"

struct MapState {
    s32 useFallback;    // +0x00  non-zero: GetMap answers the empty holder at +0x1C
    s32 map;            // +0x04  the area's map
    s32 nextMap;        // +0x08  the room's
    s32 step;           // +0x0C  where the area's map is in its load (−1 = loaded)
    s32 nextStep;       // +0x10  the same for the room
    u32 loader;         // +0x14  MapLoader* (0x70 bytes): the area's map
    u32 nextLoader;     // +0x18  MapLoader*: the room
    u32 fallback;       // +0x1C  MapCollision* with nothing in it
    u8  routes[0x60];   // +0x20  RouteTables (ai_moves.h): [0] out of doors, [1] indoors
};
static_assert(sizeof(MapState) == 0x80, "MapState is 0x80 bytes");

// The loader (0x70 bytes; 0x149520–0x14AF50).  Only +0x18 is read outside
// it: the collision holder that MapState_GetMap hands everyone.  Its reading
// and drawing are native (src/native/hle_gameplay.cpp): the model's draw is
// the VU1 boundary.
struct MapLoader {
    u32 file;           // +0x00  the ArchiveFile being read
    u32 modelFile;      // +0x04
    u32 skyFile;        // +0x08
    u32 collisionFile;  // +0x0C
    u32 waterFile;      // +0x10
    u32 model;          // +0x14  MapModel*
    u32 collision;      // +0x18  MapCollision*
    u32 sky;            // +0x1C
    u32 water;          // +0x20
    s32 modelId;        // +0x24
    s32 reading;        // +0x28
    u8  ready;          // +0x2C  the model is there to draw
    u8  _2D[3];
    u8  frustum[0x40];  // +0x30
};
static_assert(sizeof(MapLoader) == 0x70, "MapLoader is 0x70 bytes");

extern "C" {
    // Native (hle_gameplay.cpp).
    MapLoader* MapLoader_Ctor(MapLoader* self);                                  // 0x14AA00
    MapLoader* MapLoader_Dtor(MapLoader* self, s16 flag);                        // 0x14A870
    // Starts reading one of the map's files: kind 0 sky, 1 water, 2 / 6 model
    // (6: not shared through the map database's pool), 3 collision.  → 0 if
    // there is no such file.
    int   MapLoader_Open(MapLoader* self, const char* path, int kind);           // 0x14A290
    // → 1 once the read is done, and hands the file to what it is for (kind −1: nothing).
    int   MapLoader_Finish(MapLoader* self, int wait, int kind);                 // 0x14A000
    void  MapLoader_BuildModel(MapLoader* self, u32 file);                       // 0x14A770
    void  MapLoader_SetCollision(MapLoader* self, u32 file);                     // 0x14A760
    void  MapLoader_SetSkyParam(s32 param, MapLoader* self);                     // 0x14A740
    void  MapLoader_SetWaterHeight(s32 height, MapLoader* self);                 // 0x14A720
    void  MapLoader_SetFrustum(MapLoader* self);                                 // 0x149520
    void  MapLoader_Draw(MapLoader* self, const float* viewer, void* stage, void* lights);   // 0x1499C0
    void  MapLoader_DrawSky(MapLoader* self);                                    // 0x149FA0
    void  MapLoader_DrawWater(MapLoader* self);                                  // 0x149FD0

    // Decompiled (src/game/map_state.cpp); declared with void* as world_parts.h has them.
    void* MapState_Ctor(void* self);                                             // 0x14C330
    void  MapState_Dtor(void* self, int flag);                                   // 0x14C0B0
    u32   MapState_GetMap(void* self);                                           // 0x14B640
    int   MapState_IsLoaded(void* self);                                         // 0x14BD20
    void  MapState_Load(void* self, int map);                                    // 0x14BE30
    void  MapState_SetCurrent(void* self, int map);                              // 0x14BAF0
    void  MapState_LoadNext(void* self, int map);                                // 0x14BD80
    void  MapState_SetNext(void* self, int map);                                 // 0x14BAE0
    void  MapState_Return(void* self, int map);                                  // 0x14BEE0
    void  MapState_BeginDraw(void* self, void* stage);                           // 0x14BB60
    void  MapState_Draw(void* self, const float* viewer, void* stage, void* lights);    // 0x14BBD0
    void  MapState_EndDraw(void* self);                                          // 0x14BB00
}
