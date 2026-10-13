#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"

// The map database (singleton at *0x2EE504, 0x48 bytes): which maps exist,
// which map each area uses, and the route tables NPCs walk.  Decompiled from
// 0x1475D0–0x1492E0, 0x191110–0x191430 and 0x193EC0–0x194100.

// One map of Common/map/list (asset 0x22).
struct MapInfo {
    u32   model;            // +0x00  char*: Common/map/*.mdl
    u32   collision;        // +0x04  char*: Common/collision/*.col
    u32   sky;              // +0x08  char*: sky model, or 0
    u32   water;            // +0x0C  char*: animated water texture, or 0
    s32   waterParam;       // +0x10
    s32   skyParam;         // +0x14
    float _18;              // +0x18  far plane distance; -1 = unchanged
    s32   id;               // +0x1C
    u8    color[4];         // +0x20  clear colour
    u8    _24[0xC];
    float light[4][4];      // +0x30  (misnamed) the three lights' colours, then the ambient level
    float lightColor[4][4]; // +0x70  (misnamed) the directions towards the three lights
    float fog[4];           // +0xB0
};

// Where one way out of an area leads.
struct AreaExit {
    s32 target;             // area, -1 = none
    u32 name;               // char*
};

// One area of Common/map/linklist (asset 0x23).
struct AreaLink {
    s32 map;                // +0x00  index into the map list
    s32 _04;
    GuestIntMap exits;      // +0x08  exit number → AreaExit*
};

// A list of 16-byte points belonging to one map.
struct PointSet {
    s32 id;
    s32 count;
    u32 points;             // float[4][]
};

struct MapDb {
    u32 pathPoints;         // +0x00  PointSet[count]            (Common/map/PathPoint)
    u32 pathPoints2;        // +0x04  PointSet[count2]
    u32 targetPoints;       // +0x08  PointSet[count]            (Common/map/TargetPoint)
    u32 _0c;
    u32 pathGroups;         // +0x10  GuestVector<PointSet*>[]   (Common/map/PathGroup)
    s32 loaded;             // +0x14
    s32 count;              // +0x18
    s32 count2;             // +0x1C
    s32 _20;                // +0x20  -1
    GuestList maps;         // +0x24  MapInfo*
    GuestList links[2];     // +0x30  AreaLink*, one list per time of day
};

extern "C" {
    // Looked up by MapInfo::id; null / defaults when there is no such map.
    const MapInfo* MapDb_FindMap(MapDb* self, int map);         // inlined in each getter below
    const char* MapDb_GetModelPath(MapDb* self, int map);       // 0x147B30
    const char* MapDb_GetCollisionPath(MapDb* self, int map);   // 0x147AE0
    const char* MapDb_GetSkyPath(MapDb* self, int map);         // 0x147A90
    const char* MapDb_GetWaterPath(MapDb* self, int map);       // 0x147A40
    s32   MapDb_GetWaterParam(MapDb* self, int map);            // 0x1479A0
    s32   MapDb_GetSkyParam(MapDb* self, int map);              // 0x1479F0
    float MapDb_GetFarPlane(MapDb* self, int map);              // 0x147950  (-1: keep the current one)
    u32   MapDb_GetClearColor(MapDb* self, int map);            // 0x1478F0
    const float* MapDb_GetLights(MapDb* self, int map);         // 0x147870  identity if unknown
    const float* MapDb_GetLightColors(MapDb* self, int map);    // 0x1477F0
    const float* MapDb_GetFog(MapDb* self, int map);            // 0x1477A0
    // The route points of the map that is loaded (found by map id among the
    // second table), else those of the area.
    const PointSet* MapDb_GetRoutePoints(MapDb* self, int area);// 0x1473B0

    MapDb* MapDb_Get();                                         // 0x148AE0
    void   MapDb_Load(MapDb* self, int reload);                 // 0x148B30
    void   MapDb_Release();                                     // 0x148AB0
    // The map an area uses (variant = PlayerData+0x102), or -1.
    s32    MapDb_GetAreaMap(MapDb* self, int area, int variant);// 0x1475D0
    // Where exit `exit` of an area leads (-1: no such exit) and the script its door runs (or null).
    s32    MapDb_GetExitMap(MapDb* self, int area, int exit, int variant);          // 0x147550
    const char* MapDb_GetExitScript(MapDb* self, int area, int exit, int variant);  // 0x1474D0
}
