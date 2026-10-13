// Button auto-repeat — decompiled from 0x2245E0–0x2247F0.

#include "engine/pad_repeat.h"
#include "native/guest.h"

#include <initializer_list>

extern "C" {
    void* PadMgr_Get();                            // 0x13A920
    u16   Pad_GetHeld(void* mgr, int port);        // 0x13A0E0
    u16   Pad_GetStickHeld(void* mgr, int port);   // 0x139F90  pad state +0x10
}

static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);

// ── 0x224730 ──────────────────────────────────────────────────────────────
PadRepeatPair* PadRepeatPair_Ctor(PadRepeatPair* self) {
    self->vtable = 0x2EA9F0u;
    self->a = {0, 8, 8, 0, 0, 0, 0, 0};
    self->b = {0, 8, 8, 0, 0, 0, 0, 0};
    return self;
}

// ── 0x224720 ──────────────────────────────────────────────────────────────
void PadRepeatPair_SetMasks(PadRepeatPair* self, u32 maskA, u32 maskB) {
    self->a.mask = maskA;
    self->b.mask = maskB;
}

// ── 0x2245E0 ──────────────────────────────────────────────────────────────
void PadRepeatPair_SetTiming(PadRepeatPair* self, int start, int min, int step) {
    for (PadRepeat* r : {&self->a, &self->b}) {
        int s = start * gFrameRate / 30;
        int m = min * gFrameRate / 30;
        int d = step * gFrameRate / 30;
        if (s < 1) s = 1;
        if (m < 1) m = 1;
        if (d < 1) d = 1;
        r->startInterval = s;
        r->interval = s;
        r->minInterval = m;
        r->step = d;
    }
}

// ── 0x224790 ──────────────────────────────────────────────────────────────
bool PadRepeat_IsHeld(PadRepeat* self) {
    u16 held = Pad_GetHeld(PadMgr_Get(), 0);
    u16 stick = Pad_GetStickHeld(PadMgr_Get(), 0);
    return ((held | stick) & self->mask) != 0;
}

// ── 0x2247F0 ──────────────────────────────────────────────────────────────
void PadRepeat_Update(PadRepeat* self) {
    u16 held = Pad_GetHeld(PadMgr_Get(), 0);
    u16 stick = Pad_GetStickHeld(PadMgr_Get(), 0);
    u32 down = (held | stick) & self->mask;
    u32 fired = 0;

    if (down == 0) {
        // Released: let the interval grow back towards its starting value.
        if (self->held != 0)
            self->counter = self->interval;
        if (--self->counter < 1) {
            self->interval += self->step;
            if (self->interval > self->startInterval)
                self->interval = self->startInterval;
            self->counter = self->interval;
        }
    } else {
        if (self->held == 0)
            self->counter = 0;
        fired = self->held == 0;
        if (++self->counter >= self->interval) {
            self->counter = 0;
            self->interval -= self->step;
            if (self->interval <= self->minInterval)
                self->interval = self->minInterval;
            fired = 1;
        }
    }
    self->held = down;
    self->fired = fired;
}
