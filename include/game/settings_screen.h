#pragma once
#include "hal/ps2_types.h"
#include "game/menu_widgets.h"

// The Settings screen (GameScene mode 5, title menu "Settings") and its two
// full-screen helpers.

// "Screen Pos.": moves the picture with the d-pad over a grid; Square
// resets, Cross accepts.  The offset is kept in the config.
struct ScreenPosScreen {
    s32 offsetX, offsetY;   // +0x00  −80…80 in steps of 4, −20…20
    s32 baseX, baseY;       // +0x08  display position without the offset
};

// "Color Bars": eleven flat quads, each a GS packet ready to send.
struct ColorBarQuad {
    u64 prim;
    struct { u32 rgba; u32 q; u16 x, y; u32 z; } v[4];
};
struct ColorBarsScreen {
    ColorBarQuad quads[11];   // 8 bars, a grey strip each side, a gradient along the bottom
};

enum SettingsPhase : s32 {
    kSettingsLoadLayout  = 1,
    kSettingsLoadSprites = 2,
    kSettingsLoadParams  = 3,
    kSettingsReady       = 4,
};

enum SettingsSub : s32 { kSubNone = 0, kSubScreenPos = 1, kSubColorBars = 2,
                         kSubEnhancements = 3 };     // (native: the port's own page)

struct SettingsScreen {
    MenuContext    context;     // +0x000  sprite set (asset 0x56), parameter file (0x57), &values
    SettingsPanel  panel;       // +0x00C
    SettingsValues values;      // +0x3F8
    u32 layout;                 // +0x550  ScreenLayout*  (Layout/OptionScreen)
    u32 screenPos;              // +0x554  ScreenPosScreen*
    u32 colorBars;              // +0x558  ColorBarsScreen*
    s32 sub;                    // +0x55C  SettingsSub
    s32 phase;                  // +0x560  SettingsPhase
    s32 hintX, hintY;           // +0x564  where the button prompt goes
};

extern "C" {
    SettingsScreen* SettingsScreen_Ctor(SettingsScreen* self);              // 0x192590
    int  SettingsScreen_Load(SettingsScreen* self);                         // 0x1923F0
    void SettingsScreen_Start(SettingsScreen* self);                        // 0x192280
    // 1 while the screen is open (or still loading), 0 once "End" is chosen.
    int  SettingsScreen_Update(SettingsScreen* self);                       // 0x192080
    int  SettingsScreen_Draw(SettingsScreen* self);                         // 0x191E50
    SettingsScreen* SettingsScreen_Dtor(SettingsScreen* self, s16 flag);    // 0x192410
}
