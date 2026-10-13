#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"
#include "engine/pad_repeat.h"
#include "engine/text_label.h"

// Pop-up windows: a message, or a short list of choices under an optional
// title.  Decompiled from 0x13D0C0–0x13F330 and 0x13F8A0–0x13FA80.
// The manager (0x58 bytes, *0x2EE680) is updated and drawn by the main loop;
// screens open a window, poll its cursor when Cross is pressed and close it.

// A rectangle that moves towards a target over a few frames.
struct AnimRect {
    s32 x, y;               // +0x00  centre, layout coordinates
    s32 rect[4];            // +0x08  corners relative to the centre: x0, y0, x1, y1
    u32 z;                  // +0x18
    u32 blend;              // +0x1C
    u32 rgba;               // +0x20
    s32 target[6];          // +0x24  x, y, rect
    s32 duration;           // +0x3C  frames
    s32 frame;              // +0x40
};

enum WindowKind : s32 { kWindowMessage = 0, kWindowTitledMenu = 1, kWindowMenu = 2 };
enum WindowState : s32 { kWindowOpening = 0, kWindowOpen = 1, kWindowClosing = 2, kWindowClosed = 3 };

struct Window {
    s32 kind;               // +0x00
    TextLabel title;        // +0x04
    GuestList options;      // +0x30  nodes hold a TextLabel (0x34 bytes each)
    float _3c;              // +0x3C  0.4
    s32 state;              // +0x40
    s32 cursor;             // +0x44
    s32 x, y;               // +0x48
    s32 halfWidth, halfHeight;  // +0x50
    s32 titleHeight;        // +0x58
    u32 z;                  // +0x5C
    u32 priority;           // +0x60  low 4 bits: the highest open window takes the input
    u32 flags;              // +0x64  1 = never takes the input
    u32 titleAlign;         // +0x68  low nibble horizontal, next vertical: 0 start, 1 centre, 2 end
    u32 optionAlign;        // +0x6C
    AnimRect frame;         // +0x70
};

// What WindowMgr_Open takes.
struct WindowDesc {
    s32 x, y;               // +0x00
    s32 halfWidth, halfHeight;  // +0x08  0 = fit the text
    u32 z;                  // +0x10
    u32 title;              // +0x14  const u16*, 0 = none
    GuestVector options;    // +0x18  const u16*[]
    s32 titleHeight;        // +0x24  0 = fit
    u32 titleAlign;         // +0x28
    u32 optionAlign;        // +0x2C
    s32 cursor;             // +0x30
    u32 priority;           // +0x34  0 = that of the window with the input
};

struct WindowMgr {
    GuestList windows;      // +0x00
    u32 active;             // +0x0C  Window* that takes the input
    u32 locked;             // +0x10  non-zero: the cursor does not move
    PadRepeatPair upDown;   // +0x14
};

extern "C" {
    WindowMgr* WindowMgr_Ctor(WindowMgr* self);                         // 0x13DC20
    void WindowMgr_Update(WindowMgr* self);                             // 0x13D150
    void WindowMgr_Draw(WindowMgr* self, struct GsDevice* dev, struct Font* font);    // 0x13D0C0
    void WindowMgr_Clear(WindowMgr* self);                              // 0x13D4E0
    void WindowDesc_Init(WindowDesc* desc);                             // 0x13EA60
    Window* WindowMgr_Open(WindowMgr* self, WindowDesc* desc);          // 0x13D8E0
    Window* WindowMgr_OpenPassive(WindowMgr* self, WindowDesc* desc);   // 0x13D810  never takes the input
    // A message with no choices.
    Window* WindowMgr_OpenMessage(WindowMgr* self, int x, int y, u32 z, u32 priority,
                                  int halfWidth, int halfHeight, const u16* text,
                                  u32 titleAlign);                      // 0x13D9D0
    // Starts closing the window; both return its cursor (−1: no such window).
    int  WindowMgr_Close(WindowMgr* self, Window* window);              // 0x13D600
    int  WindowMgr_GetCursor(WindowMgr* self, Window* window);          // 0x13D6D0

    // A rectangle with a colour at each corner: {x0, y0, x1, y1, z, blend, rgba[4]}.
    void Gs_FillGradient(const s32* rect);                              // 0x13FA80
    void AnimRect_Fill(const AnimRect* rect);                           // 0x13F8A0
    void AnimRect_SetTarget(AnimRect* self, const s32* target, int duration);   // 0x13F330
    int  AnimRect_Step(AnimRect* self);                                 // 0x13EF20  0 = arrived
}
