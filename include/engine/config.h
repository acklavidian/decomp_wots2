#pragma once
#include "hal/ps2_types.h"

// Game settings (the object at *0x2EE68C; 0x40 bytes).
struct Config {
    s32 seVolume;       // +0x00  0–10
    s32 bgmVolume;      // +0x04  0–10
    s32 mono;           // +0x08  0 stereo, 1 mono
    s32 blood;          // +0x0C
    s32 cameraMode;     // +0x10  1 normal, 2 reverse
    s32 screenX;        // +0x14  display offset, set on the Screen Pos. screen
    s32 screenY;        // +0x18
    s32 vibration;      // +0x1C
    s32 subtitles;      // +0x20
    u8  _24[0x1C];
};

extern "C" {
    void Config_SetDefaults(Config* config);         // 0x183700
    void Config_SetDefaultVolumes(Config* config);   // 0x1836E0
    // Moves the display by the configured offset (remove = 0) or takes the
    // offset back out (remove = 1).
    void Display_ApplyOffset(int remove);            // 0x1E0450
}

// The display position lives in the device's two DISPLAY register copies
// (GsDevice +0x38 and +0x60): DX in bits 0–11, DY in bits 12–22.
u32  Display_GetX();
u32  Display_GetY();
void Display_Set(u32 x, u32 y);
