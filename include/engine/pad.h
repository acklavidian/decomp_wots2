#pragma once
#include "hal/ps2_types.h"

// ── Pad button bits ───────────────────────────────────────────────────────
// The pad manager stores the DualShock button word with its two bytes
// swapped relative to the usual PS2 layout.  Derived from how the menus use
// the bits (0x1000/0x4000 move a vertical cursor, 0x800 is "press start",
// 0x90C is the soft-reset combo); not yet confirmed on hardware.
enum PadButton : u16 {
    kPadL2       = 0x0001,
    kPadR2       = 0x0002,
    kPadL1       = 0x0004,
    kPadR1       = 0x0008,
    kPadTriangle = 0x0010,   // menus: cancel
    kPadCircle   = 0x0020,
    kPadCross    = 0x0040,   // menus: confirm
    kPadSquare   = 0x0080,
    kPadSelect   = 0x0100,
    kPadL3       = 0x0200,
    kPadR3       = 0x0400,
    kPadStart    = 0x0800,
    kPadUp       = 0x1000,
    kPadRight    = 0x2000,
    kPadDown     = 0x4000,
    kPadLeft     = 0x8000,

    kPadSoftReset = kPadStart | kPadSelect | kPadL1 | kPadR1,  // 0x90C
};

extern "C" {
    void* PadMgr_Get();                      // 0x13A920
    u16   Pad_GetHeld(void* mgr, int port);    // 0x13A0E0  field +0x08
    u16   Pad_GetPressed(void* mgr, int port); // 0x13A0C0  field +0x0C
    u16   Pad_GetRepeat(void* mgr, int port);  // 0x139F50  field +0x14
}
