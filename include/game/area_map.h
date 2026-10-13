#pragma once
#include "hal/ps2_types.h"

// The area map: the province with its ten areas and the way out; the player
// picks where to spend the next period.  Decompiled from 0x16F060–0x170D60
// and the four-way key repeat at 0x224130–0x224580.  0x198 bytes.

struct SpriteSet;

// Auto-repeat for the four directions; each key speeds up while held.
struct Repeat4 {
    s32 start, min, step;       // intervals in frames
    struct Key { s32 counter, interval, fired, held; } key[4];     // left, right, up, down
};

enum { kMapSpots = 11, kMapExit = 10 };     // spot 10 leaves the province

struct MapCursor {
    s32 pos[kMapSpots][2];      // +0x00  pixels from the top left (from the parameter file)
    s8  link[kMapSpots][4];     // +0x58  neighbour up / down / left / right, -1 = none
    s32 area;                   // +0x84
    s32 areaShown;              // +0x88
    s32 moveTimer;              // +0x8C
    s32 spin;                   // +0x90  degrees
    float pulse;                // +0x94
    u8  pulseDown; s8 pulseCount;
    float glow;                 // +0x9C
    u8  glowDown; s8 glowCount;
    Repeat4 keys;               // +0xA4
    u32 context;                // +0xF0  → AreaMap::sprites (and, 8 bytes on, `closed`)
};

// Four floats that move towards a target over a number of frames.
struct ColorFade {
    float value[4];
    float target[4];
    s32 duration, frame;
};

struct AreaMap {
    s32 hintPos[2];             // +0x000
    s32 dayPos[2];              // +0x008
    s32 periodPos[2];           // +0x010
    s32 ready;                  // +0x018  opened (the scene starts it once)
    s32 state;                  // +0x01C  1 sprites, 2 parameters, 3 running
    u32 sprites;                // +0x020  ui/WorldMap/Img.bpm (asset 0x49)
    u32 params;                 // +0x024  ui/WorldMap/Param.uip (asset 0x4A)
    u8  closed[kMapSpots];      // +0x028  1 = cannot be chosen
    MapCursor cursor;           // +0x034
    u32 areaText;               // +0x128  what the box says about the area under the cursor
    u32 infoContext;            // +0x12C
    u32 names[kMapSpots];       // +0x130
    u32 dialog;                 // +0x15C  Window*: "leave the province?"
    u8  arriving;               // +0x160  the new day's plate is being shown
    s32 timer;                  // +0x164
    ColorFade tint;             // +0x168
    float plateAlpha;           // +0x190
    float periodAlpha;          // +0x194
};

extern "C" {
    AreaMap* AreaMap_Ctor(AreaMap* self);                       // 0x16FF60
    int  AreaMap_Load(AreaMap* self);                           // 0x16FE40
    AreaMap* AreaMap_Dtor(AreaMap* self, s16 flag);             // 0x16FEE0
    // Opens the map with the cursor on `area`.
    int  AreaMap_Open(AreaMap* self, int area);                 // 0x16FD90
    // The same, first showing the day and period that have begun.
    int  AreaMap_Arrive(AreaMap* self, int area);               // 0x16FCD0
    // Returns 1 when an area is chosen (cursor.area).
    int  AreaMap_Update(AreaMap* self);                         // 0x16F6E0
    void AreaMap_Draw(AreaMap* self);                           // 0x16F2B0

    Repeat4* Repeat4_Ctor(Repeat4* self);                       // 0x224580
    void Repeat4_SetTiming(Repeat4* self, int start, int min, int step);    // 0x224560
    void Repeat4_Update(Repeat4* self);                         // 0x224210
}
