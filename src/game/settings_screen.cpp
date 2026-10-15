// Settings screen — decompiled from 0x183230–0x183650 (screen position),
// 0x186AD0–0x186C70 (colour bars) and 0x191C40–0x192590 (the screen itself).

#include "game/settings_screen.h"
#include "engine/config.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/screen_layout.h"
#include "native/guest.h"

#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void* PadMgr_Get();                                         // 0x13A920
    u16   Pad_GetPressed(void* mgr, int port);                  // 0x13A0C0
    void  GsDevice_ClearScreen(GsDevice* dev);                  // 0x12F550
    void  GsDevice_ClearScreenColor(GsDevice* dev, u32 rgba);   // 0x12F1F0
    SpriteSet* SpriteSet_Ctor(void* mem);                       // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);   // 0x17C430
    int   SpriteSet_IsLoaded(SpriteSet* set);                   // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                       // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);             // 0x17C570
}

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const GuestPtrVar<Font>     gFont{0x2EE67Cu};

// Native, not the game's: R1 opens the port's "Enhancements" page
// (src/native/enhancements.cpp) as a third sub-screen.
extern "C" void Native_EnhOpen();
extern "C" int  Native_EnhUpdate();
extern "C" void Native_EnhDraw();
extern "C" void Native_EnhHint();
static const GuestPtrVar<Config>   gConfig{0x2EE68Cu};

enum { kAssetSettingsSprites = 0x56, kAssetSettingsParams = 0x57 };
enum {
    kTextSelect = 0x11, kTextButtonCross = 0x14, kTextBack = 0x17, kTextPlay = 0x84,
    kTextScreenPosAccept = 0x163, kTextScreenPosReset = 0x164,
};
enum : u32 { kMenuDepth = 0xFFFFBE };

// ══ Screen position ═══════════════════════════════════════════════════════

// ── 0x183650 ──────────────────────────────────────────────────────────────
static ScreenPosScreen* ScreenPosScreen_Ctor(ScreenPosScreen* self) {
    const Config* config = *gConfig;
    self->offsetX = config->screenX;
    self->offsetY = config->screenY;
    self->baseX = (s32)Display_GetX() - self->offsetX;
    self->baseY = (s32)Display_GetY() - self->offsetY;
    return self;
}

// ── 0x1835F0 ──────────────────────────────────────────────────────────────
// Leaving the screen is what stores the offset.
static ScreenPosScreen* ScreenPosScreen_Dtor(ScreenPosScreen* self, s16 flag) {
    Config* config = *gConfig;
    if (self) {
        config->screenX = self->offsetX;
        config->screenY = self->offsetY;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// ── 0x183410 ──────────────────────────────────────────────────────────────
// 0 once Cross is pressed.
static int ScreenPosScreen_Update(ScreenPosScreen* self) {
    void* pad = PadMgr_Get();
    if (Pad_GetPressed(pad, 0) & kPadCross)
        return 0;

    if ((Pad_GetPressed(pad, 0) & kPadUp) && self->offsetY > -20)    self->offsetY -= 1;
    if ((Pad_GetPressed(pad, 0) & kPadDown) && self->offsetY < 20)   self->offsetY += 1;
    if ((Pad_GetPressed(pad, 0) & kPadRight) && self->offsetX < 80)  self->offsetX += 4;
    if ((Pad_GetPressed(pad, 0) & kPadLeft) && self->offsetX > -80)  self->offsetX -= 4;
    if (Pad_GetPressed(pad, 0) & kPadSquare) {
        self->offsetX = 0;
        self->offsetY = 0;
    }
    Display_Set((u32)(self->baseX + self->offsetX), (u32)(self->baseY + self->offsetY));
    return 1;
}

// ── 0x183230 ──────────────────────────────────────────────────────────────
// A grey screen with a grid of 36 lines and the two prompts.
static void ScreenPosScreen_Draw(ScreenPosScreen*, GsDevice* dev, Font* font) {
    GsDevice_ClearScreenColor(dev, 0x80808080u);

    struct GridPoint { s32 x, y, z, _pad; };
    const GridPoint* grid = guest::ptr<GridPoint>(0x2D7890u);   // 72 line ends, centre origin, y up
    struct {
        u64 prim;
        struct { u32 rgba; float q; u16 x, y; u32 z; } v[72];
    } packet;
    packet.prim = 0xC9;                                 // anti-aliased, blended lines
    for (int i = 0; i < 72; i++) {
        packet.v[i].x = (u16)Screen_XAligned(grid[i].x << 4);
        packet.v[i].y = (u16)Screen_XAligned(grid[i].y * -16);   // the x conversion, as in the original
        packet.v[i].z = (u32)grid[i].z;
        packet.v[i].q = 1.0f;
        packet.v[i].rgba = 0x80101010u;
    }
    GsDevice_DrawPrim(dev, &packet, 72);

    TextTable* text = TextTable_Get();
    u32 x = Screen_X(0, -1);
    u32 y = Screen_Y(0, -1);
    Font_SetColor(font, 0x7FBCBEC8u);
    const int textY = (int)(y & 0xFFFF) + 400 * 16;
    Font_DrawStringPlain(font, TextTable_Find(text, kTextScreenPosReset), (int)(x & 0xFFFF) + 20 * 16, textY, 0);
    Font_DrawStringPlain(font, TextTable_Find(text, kTextScreenPosAccept), (int)(x & 0xFFFF) + 400 * 16, textY, 0);
}

// ══ Colour bars ═══════════════════════════════════════════════════════════

// ── 0x186C70 ──────────────────────────────────────────────────────────────
static ColorBarsScreen* ColorBarsScreen_Ctor(ColorBarsScreen* self) {
    u32 colors[8];
    memcpy(colors, guest::ptr<const u32>(0x2D7FF0u), sizeof(colors));   // white, yellow, cyan, green, magenta, red, blue, black

    for (ColorBarQuad& q : self->quads) {
        q.prim = 0x10C;                       // Gouraud triangle strip
        for (auto& v : q.v)
            v.z = 0xFFFF;
    }

    // Eight vertical bars, 75 pixels apart.  The right edge adds a whole
    // screen coordinate rather than a width, so each bar actually runs off
    // the right of the screen and is covered by the next one.
    for (int i = 0; i < 8; i++) {
        ColorBarQuad& q = self->quads[i];
        for (auto& v : q.v)
            v.rgba = colors[i];
        q.v[0].x = q.v[2].x = (u16)Screen_X((20 + 75 * i) << 4, -1);
        s16 width = (s16)Screen_X(75 * 16, -1);
        q.v[1].x = q.v[3].x = (u16)((s16)q.v[0].x + width);
        q.v[0].y = q.v[1].y = (u16)Screen_Y(0, -1);
        q.v[2].y = q.v[3].y = (u16)Screen_Y(360 * 16, -1);
    }

    // Grey strips down the left and right edges.
    static const int kStripX[2][2] = {{0, 20}, {620, 640}};
    for (int i = 0; i < 2; i++) {
        ColorBarQuad& q = self->quads[8 + i];
        for (auto& v : q.v)
            v.rgba = 0x80808080u;
        q.v[0].x = q.v[2].x = (u16)Screen_X(kStripX[i][0] * 16, -1);
        q.v[1].x = q.v[3].x = (u16)Screen_X(kStripX[i][1] * 16, -1);
        q.v[0].y = q.v[1].y = (u16)Screen_Y(0, -1);
        q.v[2].y = q.v[3].y = (u16)Screen_Y(448 * 16, -1);
    }

    // A white-to-black ramp along the bottom.
    ColorBarQuad& ramp = self->quads[10];
    ramp.v[0].rgba = ramp.v[2].rgba = colors[0];
    ramp.v[1].rgba = ramp.v[3].rgba = colors[7];
    ramp.v[0].x = ramp.v[2].x = (u16)Screen_X(20 * 16, -1);
    ramp.v[1].x = ramp.v[3].x = (u16)Screen_X(620 * 16, -1);
    ramp.v[0].y = ramp.v[1].y = (u16)Screen_Y(360 * 16, -1);
    ramp.v[2].y = ramp.v[3].y = (u16)Screen_Y(448 * 16, -1);
    return self;
}

// ── 0x186C20 ──────────────────────────────────────────────────────────────
static ColorBarsScreen* ColorBarsScreen_Dtor(ColorBarsScreen* self, s16 flag) {
    if (self && flag > 0)
        MemFree(self);
    return self;
}

// ── 0x186AD0 ──────────────────────────────────────────────────────────────
// 0 once Start or Triangle is pressed.
static int ColorBarsScreen_Update(ColorBarsScreen*) {
    return (Pad_GetPressed(PadMgr_Get(), 0) & (kPadStart | kPadTriangle)) == 0;
}

// ── 0x186B10 ──────────────────────────────────────────────────────────────
static void ColorBarsScreen_Draw(ColorBarsScreen* self, GsDevice* dev) {
    GsDevice_ClearScreen(dev);
    for (ColorBarQuad& q : self->quads)
        GsDevice_DrawPrim(dev, &q, 4);

    GsDevice_Begin2D(dev);
    Font* font = *gFont;
    TextTable* text = TextTable_Get();
    s16 x = (s16)Screen_X(0, -1);
    s16 y = (s16)Screen_Y(0, -1);
    Font_SetColor(font, 0x7FBCBEC8u);
    Font_DrawStringPlain(font, TextTable_Find(text, kTextBack), x + 500 * 16, y + 400 * 16, 0xFFFF);
    GsDevice_End2D(dev);
}

// ══ The Settings screen ═══════════════════════════════════════════════════

// ── 0x192590 ──────────────────────────────────────────────────────────────
SettingsScreen* SettingsScreen_Ctor(SettingsScreen* self) {
    self->context.values = guest::addr(&self->values);
    SettingsPanel_Ctor(&self->panel, &self->context);
    SettingsValues_Ctor(&self->values);
    self->hintY = 0;
    self->hintX = 0;
    self->sub = kSubNone;
    self->screenPos = 0;
    self->colorBars = 0;
    self->layout = 0;
    self->phase = 0;
    self->context.spriteSet = 0;
    self->context.params = 0;
    SettingsPanel_LinkValues(&self->panel);
    return self;
}

// ── 0x192410 ──────────────────────────────────────────────────────────────
SettingsScreen* SettingsScreen_Dtor(SettingsScreen* self, s16 flag) {
    if (self) {
        if (self->screenPos)
            ScreenPosScreen_Dtor(guest::ptr<ScreenPosScreen>(self->screenPos), 1);
        self->screenPos = 0;
        if (self->colorBars)
            ColorBarsScreen_Dtor(guest::ptr<ColorBarsScreen>(self->colorBars), 1);
        self->colorBars = 0;
        if (self->layout)
            ScreenLayout_Dtor(guest::ptr<ScreenLayout>(self->layout), 1);
        self->layout = 0;
        if (self->context.spriteSet)
            SpriteSet_Dtor(guest::ptr<SpriteSet>(self->context.spriteSet), 1);
        self->context.spriteSet = 0;
        if (self->context.params)
            ParamFile_Dtor(guest::ptr<ParamFile>(self->context.params), 1);
        self->context.params = 0;
        SettingsValues_Dtor(&self->values);
        SettingsPanel_Dtor(&self->panel, -1);
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// ── 0x192290 ──────────────────────────────────────────────────────────────
// Requests the layout, the sprite sheet and the parameter file.
static void SettingsScreen_Request(SettingsScreen* self) {
    self->phase = kSettingsLoadLayout;

    if (self->layout)
        ScreenLayout_Dtor(guest::ptr<ScreenLayout>(self->layout), 1);
    self->layout = 0;
    void* mem = MemAlloc(sizeof(ScreenLayout));
    self->layout = mem ? guest::addr(ScreenLayout_Ctor(static_cast<ScreenLayout*>(mem))) : 0;
    if (self->layout) {
        ScreenLayout_Init(guest::ptr<ScreenLayout>(self->layout));
        guest::ptr<ScreenLayout>(self->layout)->id = kLayoutOptionScreen;
    }

    if (self->context.spriteSet)
        SpriteSet_Dtor(guest::ptr<SpriteSet>(self->context.spriteSet), 1);
    self->context.spriteSet = 0;
    mem = MemAlloc(0x14);
    self->context.spriteSet = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    if (self->context.spriteSet)
        SpriteSet_Load(guest::ptr<SpriteSet>(self->context.spriteSet), kAssetSettingsSprites, -1);

    if (self->context.params)
        ParamFile_Dtor(guest::ptr<ParamFile>(self->context.params), 1);
    self->context.params = 0;
    mem = MemAlloc(sizeof(ParamFile));
    self->context.params = mem ? guest::addr(ParamFile_Ctor(static_cast<ParamFile*>(mem))) : 0;
    if (self->context.params
        && !ParamFile_Request(guest::ptr<ParamFile>(self->context.params), kAssetSettingsParams)) {
        ParamFile_Dtor(guest::ptr<ParamFile>(self->context.params), 1);
        self->context.params = 0;
    }
}

// ── 0x1923F0 ──────────────────────────────────────────────────────────────
int SettingsScreen_Load(SettingsScreen* self) {
    SettingsScreen_Request(self);
    return 1;
}

// ── 0x192280 ──────────────────────────────────────────────────────────────
void SettingsScreen_Start(SettingsScreen* self) {
    SettingsValues_FromConfig(&self->values);
}

// ── 0x191F40 ──────────────────────────────────────────────────────────────
static int SettingsScreen_UpdateMenu(SettingsScreen* self) {
    void* pad = PadMgr_Get();
    // (the original fetches the repeat and pressed words here and ignores them)
    if (Pad_GetPressed(pad, 0) & 0x08) {        // (native: R1, the Enhancements page)
        Native_EnhOpen();
        self->sub = kSubEnhancements;
        return 1;
    }

    switch (SettingsPanel_Update(&self->panel)) {
    case kSettingsResetVolumes:
        SettingsValues_ResetVolumes(&self->values);
        break;
    case kSettingsReset:
        SettingsValues_ResetAll(&self->values);
        Display_ApplyOffset(0);
        break;
    case kSettingsColorBars:
        self->sub = kSubColorBars;
        if (!self->colorBars) {
            void* mem = MemAlloc(sizeof(ColorBarsScreen));
            self->colorBars = mem ? guest::addr(ColorBarsScreen_Ctor(static_cast<ColorBarsScreen*>(mem))) : 0;
        }
        break;
    case kSettingsScreenPos:
        self->sub = kSubScreenPos;
        if (!self->screenPos) {
            void* mem = MemAlloc(sizeof(ScreenPosScreen));
            self->screenPos = mem ? guest::addr(ScreenPosScreen_Ctor(static_cast<ScreenPosScreen*>(mem))) : 0;
        }
        break;
    case kSettingsEnd:
        SettingsValues_ToConfig(&self->values);
        return 0;
    }
    return 1;
}

// ── 0x192080 ──────────────────────────────────────────────────────────────
int SettingsScreen_Update(SettingsScreen* self) {
    if (!self->layout || !self->context.spriteSet)
        return 0;

    switch (self->phase) {
    case kSettingsReady:
        switch (self->sub) {
        case kSubColorBars:
            if (self->colorBars && !ColorBarsScreen_Update(guest::ptr<ColorBarsScreen>(self->colorBars)))
                self->sub = kSubNone;
            break;
        case kSubScreenPos:
            if (self->screenPos && !ScreenPosScreen_Update(guest::ptr<ScreenPosScreen>(self->screenPos))) {
                ScreenPosScreen_Dtor(guest::ptr<ScreenPosScreen>(self->screenPos), 1);
                self->screenPos = 0;
                self->sub = kSubNone;
            }
            break;
        case kSubEnhancements:
            if (!Native_EnhUpdate())
                self->sub = kSubNone;
            break;
        case kSubNone:
            return SettingsScreen_UpdateMenu(self);
        }
        return 1;

    case kSettingsLoadParams: {
        ParamFile* params = guest::ptr<ParamFile>(self->context.params);
        if (ParamFile_Poll(params) == 0) {
            BufferReader_Read(&params->out, &self->hintX, 8);
            SettingsPanel_LoadPositions(&self->panel);
            SettingsValues_LoadPositions(&self->values, params);
            self->phase = kSettingsReady;
        }
        break;
    }
    case kSettingsLoadSprites:
        if (SpriteSet_IsLoaded(guest::ptr<SpriteSet>(self->context.spriteSet)))
            self->phase = kSettingsLoadParams;
        break;
    case kSettingsLoadLayout:
        if (ScreenLayout_Poll(guest::ptr<ScreenLayout>(self->layout))) {
            ScreenLayout_SetDepth(guest::ptr<ScreenLayout>(self->layout), kMenuDepth);
            self->phase = kSettingsLoadSprites;
        }
        break;
    }
    return 1;
}

// ── 0x191C40 ──────────────────────────────────────────────────────────────
// The button prompt: the Cross glyph followed by what it does here.
static void SettingsScreen_DrawHint(SettingsScreen* self) {
    Font* font = *gFont;
    TextTable* table = TextTable_Get();
    u32 x = Screen_X(0, -1);
    u32 y = Screen_Y(0, -1);
    const u16* button = TextTable_Find(table, kTextButtonCross);

    const u16* action = nullptr;
    switch (SettingsPanel_GetHint(&self->panel)) {
    case 2: action = TextTable_Find(table, kTextPlay);   break;
    case 1: action = TextTable_Find(table, kTextSelect); break;
    }
    if (!action)
        return;

    Font_SetColor(font, 0x80FFFFFFu);
    x = ((x & 0xFFFF) + (u32)self->hintX * 16) & 0xFFFF;
    y = ((y & 0xFFFF) + (u32)self->hintY * 16) & 0xFFFF;
    Font_DrawString(font, button, (int)x, (int)y, kMenuDepth, 1.0f, 0.0f);
    Font_SetColor(font, 0x7F141414u);
    Font_Measure(font, button, 1.0f);
    Font_DrawString(font, action, (int)((x + Font_GetExtent(font)->w) & 0xFFFF), (int)y, kMenuDepth,
                    1.0f, 0.0f);
}

// ── 0x191E50 ──────────────────────────────────────────────────────────────
int SettingsScreen_Draw(SettingsScreen* self) {
    if (!self->layout || !self->context.spriteSet)
        return 0;
    if (self->phase != kSettingsReady)
        return 1;

    switch (self->sub) {
    case kSubColorBars:
        if (self->colorBars)
            ColorBarsScreen_Draw(guest::ptr<ColorBarsScreen>(self->colorBars), *gGsDevice);
        break;
    case kSubScreenPos:
        if (self->screenPos)
            ScreenPosScreen_Draw(guest::ptr<ScreenPosScreen>(self->screenPos), *gGsDevice, *gFont);
        break;
    case kSubNone:
    case kSubEnhancements:
        ScreenLayout_Draw(guest::ptr<ScreenLayout>(self->layout));
        SpriteSet_Bind(guest::ptr<SpriteSet>(self->context.spriteSet));
        SettingsPanel_Draw(&self->panel);
        SettingsScreen_DrawHint(self);
        if (self->sub == kSubEnhancements)
            Native_EnhDraw();
        else
            Native_EnhHint();
        break;
    }
    return 1;
}
