#pragma once
#include "ps2_types.h"

// PAD (DualShock 2 controller) → SDL2 gamepad/keyboard abstraction.

namespace hal::pad {

void init();
void shutdown();
void poll();  // call once per frame
// While a tool takes the keyboard and mouse (the level editor): nothing pressed.
void set_blocked(bool blocked);

// Button bits (matches PS2 pad button byte layout)
enum Button : u16 {
    SELECT   = 0x0001,
    L3       = 0x0002,      // the sticks pressed in
    R3       = 0x0004,
    START    = 0x0008,
    UP       = 0x0010,
    RIGHT    = 0x0020,
    DOWN     = 0x0040,
    LEFT     = 0x0080,
    L2       = 0x0100,
    R2       = 0x0200,
    L1       = 0x0400,
    R1       = 0x0800,
    TRIANGLE = 0x1000,
    CIRCLE   = 0x2000,
    CROSS    = 0x4000,
    SQUARE   = 0x8000,
};

// Returns bitmask of currently-held buttons (0 = pressed, like PS2 hardware)
u16 get_buttons(u32 port);

// Analog stick values (0-255, 128 = center)
u8 get_left_x(u32 port);
u8 get_left_y(u32 port);
u8 get_right_x(u32 port);
u8 get_right_y(u32 port);

// Is there anything to read on this port?  (Port 0 always: the keyboard.)
bool connected(u32 port);
// The two motors of a DualShock: the small one is on or off, the big one has
// a strength.  Lasts until told otherwise (the game sends it every frame).
void rumble(u32 port, bool small, u8 big);

// Debug aid (scripted runs): hold port 0's left stick at (x, y) for the next
// two polls.
void debug_stick(u8 x, u8 y);

}  // namespace hal::pad
