// Settings defaults and the display offset — decompiled from 0x1836E0,
// 0x183700 and 0x1E0450.

#include "engine/config.h"
#include "native/guest.h"

#include <cstring>
#include <initializer_list>

static const GuestPtrVar<u8>     gGsDevice{0x2EE670u};
static const GuestPtrVar<Config> gConfig{0x2EE68Cu};
static s32& sOffsetApplied = *guest::ptr<s32>(0x2EE6D8u);

u32 Display_GetX() {
    return *reinterpret_cast<u32*>(*gGsDevice + 0x38) & 0xFFF;
}

u32 Display_GetY() {
    return (*reinterpret_cast<u32*>(*gGsDevice + 0x38) >> 12) & 0x7FF;
}

void Display_Set(u32 x, u32 y) {
    for (u32 offset : {0x38u, 0x60u}) {
        u32* reg = reinterpret_cast<u32*>(*gGsDevice + offset);
        *reg = (*reg & 0xFF800000u) | ((y & 0x7FF) << 12) | (x & 0xFFF);
    }
}

// ── 0x1836E0 ──────────────────────────────────────────────────────────────
void Config_SetDefaultVolumes(Config* config) {
    config->seVolume = 10;
    config->bgmVolume = 10;
    config->mono = 0;
}

// ── 0x183700 ──────────────────────────────────────────────────────────────
void Config_SetDefaults(Config* config) {
    memset(config, 0, 0x40);
    config->blood = 1;
    config->cameraMode = 1;
    config->screenX = 0;
    config->screenY = 0;
    config->vibration = 1;
    config->subtitles = 1;
    config->seVolume = 10;
    config->bgmVolume = 10;
    config->mono = 0;
}

// ── 0x1E0450 ──────────────────────────────────────────────────────────────
void Display_ApplyOffset(int remove) {
    Config* config = *gConfig;
    const s32 dx = config->screenX, dy = config->screenY;

    if (remove == 0) {
        if (sOffsetApplied == 0)
            Display_Set(Display_GetX() + (u32)dx, Display_GetY() + (u32)dy);
        else
            Display_Set(Display_GetX(), Display_GetY());   // only re-syncs the second copy
        if (dx != 0 || dy != 0)
            sOffsetApplied = 1;
    } else {
        if (dx != 0 || dy != 0)
            sOffsetApplied = 1;
        if (sOffsetApplied != 0) {
            Display_Set(Display_GetX() - (u32)dx, Display_GetY() - (u32)dy);
            sOffsetApplied = 0;
        }
    }
}
