#pragma once
#include "hal/ps2_types.h"

// Auto-repeat for a held button, used by the menus: fires when the button
// goes down, then at an interval that shrinks while it stays down and grows
// back while it is up.  They come in pairs (up/down, left/right).

struct PadRepeat {
    u32 mask;            // +0x00  buttons watched
    s32 startInterval;   // +0x04  frames before the first repeat
    s32 minInterval;     // +0x08
    s32 step;            // +0x0C  how much the interval changes each time
    u32 held;            // +0x10  watched buttons down last frame
    s32 counter;         // +0x14
    s32 interval;        // +0x18  current interval
    u32 fired;           // +0x1C  set for one frame when it triggers
};

struct PadRepeatPair {
    PadRepeat a, b;
    u32 vtable;          // +0x40
};

extern "C" {
    PadRepeatPair* PadRepeatPair_Ctor(PadRepeatPair* self);                         // 0x224730
    void PadRepeatPair_SetMasks(PadRepeatPair* self, u32 maskA, u32 maskB);         // 0x224720
    // Intervals in frames at 30 fps.
    void PadRepeatPair_SetTiming(PadRepeatPair* self, int start, int min, int step);// 0x2245E0
    bool PadRepeat_IsHeld(PadRepeat* self);                                         // 0x224790
    void PadRepeat_Update(PadRepeat* self);                                         // 0x2247F0
}
