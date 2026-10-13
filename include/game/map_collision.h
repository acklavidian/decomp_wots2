#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"

// A map's collision file (Common/collision/*.col): named points (spawn and
// facing points) and a grid of typed floor cells.  Decompiled from
// 0x157DF0–0x1581A0 and 0x16B6F0–0x16C350.
//
// File layout (little-endian):
//   s32 pointCount;  {s32 name; float x, y, z}[pointCount]
//   ColHeader (0x24 bytes);  ColCellType[typeCount] (0x3C bytes each)
//   {s16 ?, s16 type}[cellCount]  — rows of `width` cells, -1 = nothing there
//   s32 extraCount;  u32 extra[extraCount]

struct ColHeader {
    s32   _00;
    float cellSize;         // +0x04  in centimetres (25 = cells of 0.25)
    s32   offsetX, offsetZ; // +0x08  cell index of the origin
    s32   width;            // +0x10  cells per row
    s32   cellCount;        // +0x14
    s32   _18, _1c;
    s32   typeCount;        // +0x20
};

enum : u32 {
    kCellFlag1 = 1,
    kCellBlocked = 2,       // cannot be walked on / seen through
    kCellEventOnly = 4,     // blocked while a story scene runs
    kCellNoNpc = 0x10,
    kCellFlag20 = 0x20,
    kCellFlag40 = 0x40,
    kCellFlag100 = 0x100,
};

struct ColCellType {
    s16 id;                 // +0x00
    s16 _02;
    u32 flags;              // +0x04
    u32 _08;
    s32 _0c;                // +0x0C  -1 is read as 0
    s8  light[3];           // +0x10  how brightly a character standing there is lit
    u8  _13[3];
    u8  _16;                // +0x16
    u8  _17;
    float height;           // +0x18  of the ground
    u8  _1c[0x20];
};

struct ColSpot {
    s32   name;             // +0x00
    u32   _04[3];
    float pos[4];           // +0x10  w = 1
};

struct Collision {
    u32 points;             // +0x00  in the file
    u32 spots;              // +0x04  ColSpot[]
    u32 header;             // +0x08  ColHeader*
    u32 cells;              // +0x0C  {s16, s16 type}[]
    u32 extra;              // +0x10
    s32 pointCount;         // +0x14
    s32 typeCount;          // +0x18
    s32 extraCount;         // +0x1C
    s32 eventMode;          // +0x20
    ColCellType current;    // +0x24  the type of the cell last looked at
    GuestIntMap types;      // +0x60  id → ColCellType*
    GuestList found;        // +0x70  result of the last point search: float[4]*
};

// What the rest of the game holds: null until the file has arrived.
struct MapCollision {
    u32 collision;
};

extern "C" {
    Collision* Collision_Ctor(Collision* self, const s32* file);                // 0x16BE70
    Collision* Collision_Dtor(Collision* self, s16 flag);                       // 0x16BD50
    // The points with a name, as a list of positions (valid until the next search).
    GuestList* Collision_FindPoints(Collision* self, int name);                 // 0x16B6F0
    ColCellType* Collision_CellAt(Collision* self, const float* pos);           // 0x16BA90
    ColCellType* Collision_CellAtIndex(Collision* self, int x, int z);          // 0x16BC00
    // Null if nothing of the given kind (0 walls, 1, 2) lies between the two
    // points; otherwise where the line is stopped.
    const float* Collision_LineBlocked(Collision* self, const float* from, const float* to, int kind);  // 0x16B7C0

    MapCollision* MapCollision_Ctor(MapCollision* self);                        // 0x1580D0
    MapCollision* MapCollision_Dtor(MapCollision* self, s16 flag);              // 0x158070
    void MapCollision_SetFile(MapCollision* self, const s32* file);             // 0x158010
    void Map_SetEventMode(MapCollision* self, int eventMode);                   // 0x157DF0
    GuestList* Map_FindPoints(MapCollision* self, int name);                    // 0x157E70
    const float* Map_LineBlocked(MapCollision* self, const float* from, const float* to, int kind);     // 0x157F70
    // The cell under a point (a blank walkable one while no file is loaded).
    ColCellType* Map_CellAt(MapCollision* self, const float* pos);              // 0x157FA0
    const ColHeader* Map_Header(MapCollision* self);                            // 0x157E50
}
