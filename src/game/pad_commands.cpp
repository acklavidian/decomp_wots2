// The pad's command buffer — decompiled from 0x190630–0x1910F0.
//
// The pad routine (chara_control.cpp) records the last 32 frames of buttons
// and stick direction here and asks it what they add up to: which blow a
// button press means (with the direction held, or flicked just before), a
// double tap for a dodge step, and so on.  The answers are codes the sword
// style's combo tree is keyed by: high nibble the button (1 Triangle,
// 2 Square, 3 / 4 the same in the air, 5 / 6 / 7 with R1), low nibble the
// direction (1 towards where the character faces, 2 away, 3 away then let
// go, 8 the button held down).

#include "engine/pad.h"
#include "game/chara.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstring>

extern "C" {
    void  MemFree(void* p);                             // 0x100650
    u16   Pad_GetHeld(void* mgr, int port);             // 0x13A0E0
    u16   Pad_GetPressed(void* mgr, int port);          // 0x13A0C0
    int   Pad_GetStick(void* mgr, int port, u8* x, u8* y);  // 0x139FB0
}

struct PadFrame {
    u32   held;             // +0x00
    u32   pressed;          // +0x04
    u8    stickX, stickY;   // +0x08
    u8    _0a[6];
    float direction[4];     // +0x10  where the stick points, in the world
};
struct PadCommands {
    u32 at;                 // +0x00  where the next frame goes
    s32 count;              // +0x04  frames recorded (up to 32)
    s32 port;               // +0x08
    u32 pad;                // +0x0C  PadMgr*
    PadFrame frame[32];     // +0x10
};
static_assert(sizeof(PadCommands) == 0x410, "PadCommands");

static inline PadCommands* C(void* self) { return static_cast<PadCommands*>(self); }
static inline bool Pointing(const float* d) { return d[0] != 0.0f || d[2] != 0.0f; }
// How far `direction` points along where the character faces.
static float Along(const float* direction, const float* facing) {
    const float length = sqrtf(facing[0] * facing[0] + facing[1] * facing[1] + facing[2] * facing[2]);
    const float scale = length != 0.0f ? 1.0f / length : 0.0f;
    return (direction[0] * facing[0] + direction[1] * facing[1] + direction[2] * facing[2]) * scale;
}

// ── 0x1910F0 / 0x1910A0 ───────────────────────────────────────────────────
void* PadCommands_Ctor(void* mem, void* padMgr, int port) {
    C(mem)->pad = guest::addr(padMgr);
    C(mem)->port = port;
    C(mem)->count = 0;
    C(mem)->at = 0;
    return mem;
}
void* PadCommands_Dtor(void* self, int flag) {
    if (self && (s16)flag > 0)
        MemFree(self);
    return self;
}

// ── 0x190FF0 ──────────────────────────────────────────────────────────────
void PadCommands_Push(void* self, const float* direction) {
    PadCommands* c = C(self);
    PadFrame* f = &c->frame[c->at];
    f->held = Pad_GetHeld(guest::ptr(c->pad), c->port);
    f->pressed = Pad_GetPressed(guest::ptr(c->pad), c->port);
    Pad_GetStick(guest::ptr(c->pad), c->port, &f->stickX, &f->stickY);
    memcpy(f->direction, direction, 16);
    if (c->count < 0x20)
        c->count++;
    c->at = (c->at + 1) & 0x1F;
}
// ── 0x190FE0 ──────────────────────────────────────────────────────────────
void PadCommands_Clear(void* self) {
    C(self)->count = 0;
    C(self)->at = 0;
}

// ── 0x1906F0 ──────────────────────────────────────────────────────────────
// Was one of these buttons pressed within the last few frames?
int PadCommands_Pressed(void* self, u32 buttons, int frames) {
    PadCommands* c = C(self);
    u32 at = c->at;
    for (int i = 0; i < frames; i++) {
        at = (at - 1) & 0x1F;
        if (c->frame[at].pressed & buttons)
            return 1;
    }
    return 0;
}

// ── 0x190630 ──────────────────────────────────────────────────────────────
// Which way a step goes: 0 forward, 1 back, 2 / 3 to a side.
int PadCommands_Dodge(void*, const float* facing, const float* direction) {
    const float along = Along(direction, facing);
    if (along > 0.75f)
        return 0;
    if (along < -0.75f)
        return 1;
    const float length = sqrtf(facing[0] * facing[0] + facing[1] * facing[1] + facing[2] * facing[2]);
    const float fx = length != 0.0f ? facing[0] / length : 0.0f, fz = length != 0.0f ? facing[2] / length : 0.0f;
    return direction[0] * fz - direction[2] * fx <= 0.0f ? 3 : 2;
}

// ── 0x190DF0 ──────────────────────────────────────────────────────────────
// A tap of the stick (centred, pushed, centred again — within five frames):
// the way it was pushed, as above; −1 when there was none.
int PadCommands_Special(void* self, const float* facing) {
    static const u8 kWant[4] = {0, 1, 0, 0};        // 0x2EDB14, newest first: centred, pushed, centred
    PadCommands* c = C(self);
    u32 at = c->at;
    u32 step = 0;
    float tapped[4] = {0, 0, 0, 0};
    int frames = 0;
    for (s32 n = c->count; n >= 0; n--) {
        at = (at - 1) & 0x1F;
        const float* d = c->frame[at].direction;
        if (!Pointing(d)) {
            if (kWant[step] == 0) {
                step = (step + 1) & 0xFF;
                if (step == 3)
                    return PadCommands_Dodge(self, facing, tapped);
            }
        } else if (kWant[step] == 1) {
            memcpy(tapped, d, 16);
            step = (step + 1) & 0xFF;
        }
        if (frames > 4)
            return -1;
        frames++;
    }
    return -1;
}

// What the button pressed on a frame is, as a code's high nibble.
static u32 ButtonCode(const PadFrame* f) {
    if ((f->held & kPadR1) && (f->pressed & kPadTriangle)) return 0x50;
    if ((f->held & kPadR1) && (f->pressed & kPadSquare))   return 0x60;
    if (f->pressed & kPadTriangle) return 0x10;
    if (f->pressed & kPadSquare)   return 0x20;
    return 0;
}

// ── 0x190AC0 ──────────────────────────────────────────────────────────────
// The blow the button pressed this frame asks for.
u8 PadCommands_Attack(void* self, const float* facing, int airborne) {
    PadCommands* c = C(self);
    if (c->count == 0)
        return 0xFF;
    u32 at = (c->at - 1) & 0x1F;
    const PadFrame* now = &c->frame[at];
    if (airborne) {
        if (now->held & kPadR1)
            return (now->pressed & kPadSquare) ? 0x70 : 0;
        if (now->held & kPadTriangle) return 0x30;
        if (now->held & kPadSquare)   return 0x40;
        return 0;
    }
    u32 code = 0;
    int away = -1;
    if (Pointing(now->direction)) {
        away = !(Along(now->direction, facing) > 0.0f);
        code = away ? 2 : 1;
    }
    const u32 button = ButtonCode(now);
    if (button == 0)
        return 0xFF;
    code |= button;
    if (button >= 0x50 || away != 0)
        return (u8)code;
    // Towards: was the stick pulled away and let go just before?  (A "back,
    // forward" flick.)
    bool pulled = false;
    int frames = 0;
    for (s32 n = c->count - 1; n >= 0; n--) {
        at = (at - 1) & 0x1F;
        const PadFrame* f = &c->frame[at];
        if (f->pressed & (kPadTriangle | kPadSquare))
            return (u8)code;
        if (!Pointing(f->direction)) {
            if (pulled)
                return (u8)(code | 3);
        } else if (!pulled && Along(f->direction, facing) <= 0.0f) {
            pulled = true;
        }
        if (frames > 10)
            return (u8)code;
        frames++;
    }
    return (u8)code;
}

// ── 0x190750 ──────────────────────────────────────────────────────────────
// The same while a combo is under way: the most recent press in the buffer
// counts (so a blow can be asked for early), and a button held for five
// frames is a code of its own.
u8 PadCommands_ComboAttack(void* self, const float* facing, int) {
    PadCommands* c = C(self);
    if (c->count == 0)
        return 0xFF;
    u32 at = c->at;
    u32 code = 0;
    int away = -1;
    int phase = 0;              // 1 once the press is found, 2 once the stick has been seen pulled away
    int frames = 0;
    s8 heldTriangle = 0, heldSquare = 0;
    for (s32 n = c->count; n >= 0; n--) {
        at = (at - 1) & 0x1F;
        const PadFrame* f = &c->frame[at];
        if (f->held & kPadTriangle) heldTriangle++;
        if (f->held & kPadSquare)   heldSquare++;
        if ((f->pressed & (kPadTriangle | kPadSquare)) == 0) {
            if (phase != 0) {
                if (away != 0)
                    break;
                if (!Pointing(f->direction)) {
                    if (phase == 2) {
                        code |= 3;
                        break;
                    }
                } else if (phase == 1 && Along(f->direction, facing) <= 0.0f) {
                    phase = 2;
                }
            }
            if (frames > 10)
                break;
        } else {
            if (phase != 0)
                return (u8)code;
            if (Pointing(f->direction)) {
                away = !(Along(f->direction, facing) > 0.0f);
                code |= away ? 2 : 1;
            }
            const u32 button = ButtonCode(f);
            if (button >= 0x50)
                return (u8)(code | button);
            code |= button;
            phase = 1;
        }
        frames++;
    }
    if (code == 0) {
        if (heldTriangle >= 5)     code = 0x18;
        else if (heldSquare > 4)   code = 0x28;
    }
    return (u8)code;
}
