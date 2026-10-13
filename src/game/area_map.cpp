// The area map — decompiled from 0x16F060–0x170D60 and 0x224130–0x224580.
// See include/game/area_map.h.

#include "game/area_map.h"
#include "game/player_data.h"
#include "engine/bgm.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/param_file.h"
#include "engine/window.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void* PadMgr_Get();                                         // 0x13A920
    u16   Pad_GetHeld(void* mgr, int port);                     // 0x13A0E0
    u16   Pad_GetPressed(void* mgr, int port);                  // 0x13A0C0
    u16   Pad_GetStickHeld(void* mgr, int port);                // 0x139F90
    int   Pad_GetStick(void* mgr, int port, u8* x, u8* y);      // 0x139FB0
    void  Voice_Play(int id, int positional, int volume, int a);    // 0x1800F0
    void  Se_Resume();                                          // 0x180C50
    void  Se_Pause();                                           // 0x180C60
    void  GsDevice_ClearScreen(GsDevice* dev);                  // 0x12F550
    void  GsDevice_ClearScreenColor(GsDevice* dev, u32 rgba);   // 0x12F1F0
    void  GsDevice_SetBlend(GsDevice* dev, u64 alpha);          // 0x130C50
    void  Gs_FillRect(const s32* rect, u32 z, u32 rgba);        // 0x225CD0
    SpriteSet* SpriteSet_Ctor(void* mem);                       // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    int   SpriteSet_IsLoaded(SpriteSet* set);                   // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                       // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);             // 0x17C570
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    void  SpriteSet_DrawEx(SpriteSet* set, int index, const void* params);             // 0x17ADF0
    void  SpriteSet_DrawRotated(SpriteSet* set, int index, const void* params);        // 0x179A40
    void* SpriteSet_GetSprite(SpriteSet* set, int index);       // 0x17BDF0
}

static_assert(sizeof(Repeat4) == 0x4C && sizeof(MapCursor) == 0xF4, "MapCursor");
static_assert(offsetof(AreaMap, cursor) == 0x34 && offsetof(AreaMap, dialog) == 0x15C, "AreaMap");
static_assert(offsetof(AreaMap, tint) == 0x168 && sizeof(AreaMap) == 0x198, "AreaMap");

static const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
static const GuestPtrVar<Font>       gFont{0x2EE67Cu};
static const GuestPtrVar<WindowMgr>  gWindowMgr{0x2EE680u};
static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);
static s32& gScreenW   = *guest::ptr<s32>(0x2EE4C0u);
static s32& gScreenH   = *guest::ptr<s32>(0x2EE4C4u);

enum { kAssetMapSprites = 0x49, kAssetMapParams = 0x4A };
enum : u32 { kZ = 0xFFFFE6, kPlain = 0x80808080u };
enum { kDirUp, kDirDown, kDirLeft, kDirRight };
enum { kKeyLeft, kKeyRight, kKeyUp, kKeyDown };

static inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
static inline SpriteSet* Sheet(AreaMap* self) { return guest::ptr<SpriteSet>(self->sprites); }

// ══ Four-way key repeat ═══════════════════════════════════════════════════
// ── 0x224580 ──────────────────────────────────────────────────────────────
Repeat4* Repeat4_Ctor(Repeat4* self) {
    memset(self, 0, sizeof(*self));
    self->start = 8;
    self->min = 8;
    return self;
}

// ── 0x224560 ──────────────────────────────────────────────────────────────
void Repeat4_SetTiming(Repeat4* self, int start, int min, int step) {
    self->start = start;
    self->min = min;
    self->step = step;
    for (Repeat4::Key& key : self->key)
        key.interval = start;
}

// 0x224130 — the same scheme as PadRepeat
static void Repeat4_UpdateKey(Repeat4* self, Repeat4::Key* key, bool held) {
    s32 fired = 0;
    if (!held) {
        if (key->held != 0)
            key->counter = key->interval;
        if (--key->counter < 1) {
            key->interval += self->step;
            if (key->interval > self->start)
                key->interval = self->start;
            key->counter = key->interval;
        }
    } else {
        if (key->held == 0)
            key->counter = 0;
        fired = key->held == 0;
        if (++key->counter >= key->interval) {
            key->counter = 0;
            key->interval -= self->step;
            if (key->interval <= self->min)
                key->interval = self->min;
            fired = 1;
        }
    }
    key->held = held;
    key->fired = fired;
}

// ── 0x224210 ──────────────────────────────────────────────────────────────
// A diagonal counts as one direction: sideways on the d-pad, the stronger
// axis on the stick.
void Repeat4_Update(Repeat4* self) {
    u8 sx = 0, sy = 0;
    Pad_GetStick(PadMgr_Get(), 0, &sx, &sy);
    const u16 pad = Pad_GetHeld(PadMgr_Get(), 0);
    const u16 stick = Pad_GetStickHeld(PadMgr_Get(), 0);
    bool left = pad & kPadLeft, right = pad & kPadRight, up = pad & kPadUp, down = pad & kPadDown;
    if (up && left)         up = false;
    else if (up && right)   up = false;
    else if (down && left)  down = false;
    else if (down && right) down = false;

    bool sLeft = stick & kPadLeft, sRight = stick & kPadRight, sUp = stick & kPadUp, sDown = stick & kPadDown;
    if (sUp && sLeft) {
        if (sy < sx) sLeft = false; else sUp = false;
    } else if (sUp && sRight) {
        if (sy < 0xFF - sx) sRight = false; else sUp = false;
    } else if (sDown && sLeft) {
        if (0xFF - sy < sx) sLeft = false; else sDown = false;
    } else if (sDown && sRight) {
        if (0xFF - sy < 0xFF - sx) sRight = false; else sDown = false;
    }
    Repeat4_UpdateKey(self, &self->key[kKeyLeft], left || sLeft);
    Repeat4_UpdateKey(self, &self->key[kKeyRight], right || sRight);
    Repeat4_UpdateKey(self, &self->key[kKeyUp], up || sUp);
    Repeat4_UpdateKey(self, &self->key[kKeyDown], down || sDown);
}

// ══ Colour fade ═══════════════════════════════════════════════════════════
// 0x170D60
static void ColorFade_Set(ColorFade* self, const float* target, int duration) {
    self->duration = duration;
    self->frame = 0;
    memcpy(self->target, target, sizeof(self->target));
}

// 0x170C10
static int ColorFade_Step(ColorFade* self) {
    if (self->duration < ++self->frame)
        return 0;
    if (self->frame < self->duration) {
        const float share = 1.0f / (float)(self->duration - self->frame);
        for (int i = 0; i < 4; i++)
            self->value[i] += (self->target[i] - self->value[i]) * share;
        return 1;
    }
    memcpy(self->value, self->target, sizeof(self->value));
    return 0;
}

// ══ Cursor ════════════════════════════════════════════════════════════════
// 0x170990 — which spot each direction leads to
static void MapCursor_InitLinks(MapCursor* self) {
    static const s8 kLinks[kMapSpots][4] = {    // up, down, left, right
        { 1, 10,  6,  8}, { 4,  0,  9,  8}, { 7,  8,  8, -1}, { 5,  6, -1,  9},
        {-1,  1,  5,  7}, {-1,  3,  3,  4}, { 9, 10,  3,  0}, { 4,  8,  4,  2},
        { 7,  0,  1,  2}, { 5,  6,  3,  1}, { 0, -1,  6,  0}};
    memcpy(self->link, kLinks, sizeof(kLinks));
}

// 0x170AE0
static MapCursor* MapCursor_Ctor(MapCursor* self, u32 context) {
    memset(self->pos, 0, sizeof(self->pos));
    Repeat4_Ctor(&self->keys);
    self->context = context;
    self->area = 0;
    self->areaShown = 0;
    self->moveTimer = 0;
    self->spin = 0;
    self->pulse = 0.5f;
    self->pulseDown = 0;
    self->pulseCount = 0;
    self->glow = 0.0f;
    self->glowDown = 0;
    self->glowCount = 0;
    Repeat4_SetTiming(&self->keys, 20, 10, 10);
    MapCursor_InitLinks(self);
    return self;
}

// 0x170540 — the next open spot in a direction, or -1
static int MapCursor_Neighbour(MapCursor* self, int area, int dir) {
    const u8* closed = guest::ptr<const u8>(self->context) + 8;
    int next = self->link[area][dir];
    while (next != -1 && closed[next] == 1)
        next = self->link[next][dir];
    return next;
}

// 0x1706A0 — the sparkle on the cursor: it spins, swells and fades
static void MapCursor_Animate(MapCursor* self) {
    self->spin += 120 / gFrameRate;
    if (self->spin > 359)
        self->spin = 0;
    if (self->pulseDown == 1) {
        self->pulse -= 0.038461536f;
        if (++self->pulseCount > 25) {
            self->pulseCount = 0;
            self->pulseDown = 0;
            self->pulse = 0.5f;
        }
    } else {
        self->pulse += 0.25f;
        if (++self->pulseCount > 3) {
            self->pulseCount = 0;
            self->pulseDown = 1;
            self->pulse = 1.0f;
        }
    }
    if (self->glowDown == 0) {
        self->glow += 0.25f;
        if (++self->glowCount > 3) {
            self->glowCount = 0;
            self->glowDown = 1;
            self->glow = 1.0f;
        }
    } else {
        self->glow -= 0.038461536f;
        if (++self->glowCount > 25) {
            self->glowCount = 0;
            self->glowDown = 0;
            self->glow = 1.0f;
        }
    }
}

// 0x170840 — true when the cursor moved
static bool MapCursor_Update(MapCursor* self) {
    if (self->moveTimer != 0)
        self->moveTimer--;
    MapCursor_Animate(self);
    Repeat4_Update(&self->keys);
    const int before = self->area;
    auto go = [&](int dir) {
        const int next = MapCursor_Neighbour(self, self->area, dir);
        if (next != -1)
            self->area = next;
    };
    if (self->keys.key[kKeyUp].fired != 0)
        go(kDirUp);
    else if (self->keys.key[kKeyDown].fired != 0)
        go(kDirDown);
    if (self->keys.key[kKeyRight].fired != 0)
        go(kDirRight);
    else if (self->keys.key[kKeyLeft].fired != 0)
        go(kDirLeft);
    if (before == self->area)
        return false;
    self->areaShown = self->area;
    self->moveTimer = 1;
    self->spin = 0;
    self->pulse = 0.5f;
    self->pulseDown = 0;
    self->pulseCount = 0;
    self->glow = 0.0f;
    self->glowDown = 0;
    self->glowCount = 0;
    return true;
}

// 0x170320
static void MapCursor_Draw(MapCursor* self) {
    GsDevice* dev = *gGsDevice;
    SpriteSet* sheet = guest::ptr<SpriteSet>(*guest::ptr<u32>(self->context));
    const u32 x = Screen_X(self->pos[self->area][0] << 4, -1) & 0xFFFF;
    const u32 y = Screen_Y(self->pos[self->area][1] << 4, -1) & 0xFFFF;
    if (self->moveTimer == 0)
        SpriteSet_Draw(sheet, 1, (int)x, (int)y, kZ, kPlain);
    GsDevice_SetBlend(dev, 0x8000000048ull);            // additive
    if (self->moveTimer == 0) {
        if (const u8* sparkle = static_cast<const u8*>(SpriteSet_GetSprite(sheet, 3))) {
            u16 w, h;
            memcpy(&w, sparkle + 0xC, 2);
            memcpy(&h, sparkle + 0xE, 2);
            struct { u32 flags; s32 x, y; u32 z; u32 rgba; float scaleX, scaleY, angle; } p = {
                1, (s32)x + ((w >> 1) - 15) * 16, (s32)y + ((h >> 1) + 17) * 16, kZ,
                (u32)(int)(self->glow * 127.0f) << 24 | 0x808080, self->pulse * 20.0f, self->pulse * 20.0f,
                (float)self->spin};
            SpriteSet_DrawRotated(sheet, 3, &p);
        }
    } else {
        SpriteSet_Draw(sheet, 2, (int)x + 0x130, (int)y - 0x1B0, kZ, kPlain);
    }
    GsDevice_SetBlend(dev, 0x8000000044ull);
}

// ══ The box at the bottom ═════════════════════════════════════════════════
// 0x170000 — "<day> <period> <what is in the area>"
static void AreaMap_DrawInfo(AreaMap* self) {
    SpriteSet* sheet = guest::ptr<SpriteSet>(*guest::ptr<u32>(self->infoContext));
    const s32 sx = (s32)(Screen_X(0, -1) & 0xFFFF), sy = (s32)(Screen_Y(0, -1) & 0xFFFF);
    s32 box[4] = {sx + 0x230, sy + 0x1410, 0x23C0, 0x3A0};
    Gs_FillRect(box, kZ, 0x52080404u);
    box[0] = sx + 0x210;
    box[1] = sy + 0x13F0;
    Gs_FillRect(box, kZ, 0x7FB6C1D1u);
    const struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } strip =
        {2, sx + 0x730, sy + 0x1410, kZ, 0x1E60, -1, kPlain};
    SpriteSet_DrawEx(sheet, 4, &strip);
    SpriteSet_Draw(sheet, 5, (sx + 0x230) & 0xFFFF, (sy + 0x13B0) & 0xFFFF, kZ, kPlain);
    if (self->areaText == 0)
        return;
    Font* font = *gFont;
    Font_SetColor(font, 0x7F100E0Fu);
    PlayerData* pd = *gPlayerData;
    u16 line[0x100];
    WStr_Copy(line, Text(pd->day + 0x23));
    WStr_Append(line, Text(1), 0x80);
    WStr_Append(line, Text(pd->time + 0x1E), 0x80);
    WStr_Append(line, Text(1), 0x80);
    WStr_Append(line, guest::ptr<const u16>(self->areaText), 0x80);
    if (pd->day == 5 && pd->time == 4) {                // the festival
        WStr_Append(line, Text(4), 0x80);
        WStr_Append(line, Text(0x143), 0x80);
    }
    Font_DrawString(font, line, sx + 0x770, sy + 0x1470, kZ, 1.0f, 0.0f);
}

// ══ The screen ════════════════════════════════════════════════════════════
// ── 0x16FF60 ──────────────────────────────────────────────────────────────
AreaMap* AreaMap_Ctor(AreaMap* self) {
    const u32 context = guest::addr(&self->sprites);
    MapCursor_Ctor(&self->cursor, context);
    self->infoContext = context;
    self->areaText = 0;
    memset(&self->tint, 0, sizeof(self->tint));
    self->ready = 0;
    self->sprites = 0;
    self->params = 0;
    self->timer = 0;
    self->arriving = 0;
    self->state = 0;
    self->plateAlpha = 0.0f;
    self->periodAlpha = 0.0f;
    self->dialog = 0;
    memset(self->closed, 0, sizeof(self->closed));
    return self;
}

// ── 0x16FE40 ──────────────────────────────────────────────────────────────
int AreaMap_Load(AreaMap* self) {
    for (int i = 0; i < kMapSpots; i++)
        self->names[i] = guest::addr(Text(0x52 + i));
    return 1;
}

// ── 0x16FEE0 ──────────────────────────────────────────────────────────────
AreaMap* AreaMap_Dtor(AreaMap* self, s16 flag) {
    if (!self)
        return self;
    if (self->sprites)
        SpriteSet_Dtor(Sheet(self), 1);
    self->sprites = 0;
    if (self->params)
        ParamFile_Dtor(guest::ptr<ParamFile>(self->params), 1);
    self->params = 0;
    if (flag > 0)
        MemFree(self);
    return self;
}

// 0x16FBD0
static bool AreaMap_StartLoading(AreaMap* self) {
    void* mem = MemAlloc(0x14);
    self->sprites = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    if (!self->sprites)
        return false;
    if (!SpriteSet_Load(Sheet(self), kAssetMapSprites, -1)) {
        SpriteSet_Dtor(Sheet(self), 1);
        self->sprites = 0;
        return false;
    }
    mem = MemAlloc(sizeof(ParamFile));
    self->params = mem ? guest::addr(ParamFile_Ctor(static_cast<ParamFile*>(mem))) : 0;
    if (!self->params)
        return false;
    if (!ParamFile_Request(guest::ptr<ParamFile>(self->params), kAssetMapParams)) {
        ParamFile_Dtor(guest::ptr<ParamFile>(self->params), 1);
        self->params = 0;
        return false;
    }
    self->state = 1;
    return true;
}

// ── 0x16FD90 ──────────────────────────────────────────────────────────────
int AreaMap_Open(AreaMap* self, int area) {
    self->ready = 1;
    self->cursor.area = area;
    self->cursor.areaShown = area;
    self->timer = 0;
    self->arriving = 0;
    if (!AreaMap_StartLoading(self))
        return 0;
    self->areaText = guest::addr(Text(area + 0x5E));
    return 1;
}

// ── 0x16FCD0 ──────────────────────────────────────────────────────────────
int AreaMap_Arrive(AreaMap* self, int area) {
    self->ready = 1;
    self->cursor.area = area;
    self->cursor.areaShown = area;
    self->timer = 0;
    self->arriving = 1;
    self->plateAlpha = 0.0f;
    self->periodAlpha = 0.0f;
    if (!AreaMap_StartLoading(self))
        return 0;
    self->areaText = guest::addr(Text(area + 0x5E));
    Se_Pause();
    Bgm_TogglePause();
    return 1;
}

// ── 0x16F440 ──────────────────────────────────────────────────────────────
// The new day: the screen darkens, the day plate fades in, then the period,
// and everything fades away again.  Timings are in 1/30 s × fps.
static void AreaMap_UpdateArrival(AreaMap* self) {
    ColorFade_Step(&self->tint);
    const float dark[4] = {0, 0, 0, 64.0f}, clear[4] = {0, 0, 0, 0};
    if (self->timer == 0) {
        memcpy(self->tint.value, clear, sizeof(clear));
        ColorFade_Set(&self->tint, dark, 450 / gFrameRate);
        Voice_Play(0x8C7, 0, 0x3FFF, 0);
    }
    if (self->timer == 2700 / gFrameRate) {
        memcpy(self->tint.value, dark, sizeof(dark));
        ColorFade_Set(&self->tint, clear, 450 / gFrameRate);
    }
    const float step = 128.0f / (float)(900 / gFrameRate);
    if (self->timer < 1800 / gFrameRate) {
        if (self->timer >= 450 / gFrameRate && (self->plateAlpha += step) > 128.0f)
            self->plateAlpha = 128.0f;
        if (self->timer >= 900 / gFrameRate && (self->periodAlpha += step) > 128.0f)
            self->periodAlpha = 128.0f;
    } else if (self->timer >= 2700 / gFrameRate) {
        self->plateAlpha -= step;
        self->periodAlpha -= step;
    }
    if (++self->timer > 3150 / gFrameRate) {
        self->arriving = 0;
        Bgm_TogglePause();
        Se_Resume();
    }
}

// ── 0x16FA00 ──────────────────────────────────────────────────────────────
static void AreaMap_AskLeave(AreaMap* self) {
    WindowDesc desc;
    memset(&desc, 0, sizeof(desc));
    WindowDesc_Init(&desc);
    desc.title = guest::addr(Text(0x51));
    desc.z = 0xFFFF82;
    u32* options = static_cast<u32*>(MemAlloc(8));      // a vector<u32> in the original
    options[0] = guest::addr(Text(0xF));
    options[1] = guest::addr(Text(0x10));
    desc.options.capacity = 2;
    desc.options.count = 2;
    desc.options.data = guest::addr(options);
    desc.cursor = 1;
    self->dialog = guest::addr(WindowMgr_Open(*gWindowMgr, &desc));
    Sound_PlaySE(1, 0, 0x3FFF);
    MemFree(options);
}

// ── 0x16F6E0 ──────────────────────────────────────────────────────────────
int AreaMap_Update(AreaMap* self) {
    const u16 pad = (u16)(Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0));
    if (self->sprites == 0 || self->state == 0)
        return 1;
    if (self->state == 1) {
        if (SpriteSet_IsLoaded(Sheet(self)))
            self->state = 2;
        return 0;
    }
    if (self->state == 2) {
        ParamFile* params = guest::ptr<ParamFile>(self->params);
        if (!ParamFile_Poll(params)) {
            self->state = 3;
            // (The original asks for the spot positions eleven times; only the
            // first read finds enough left.)
            for (int i = 0; i < kMapSpots; i++)
                BufferReader_Read(&params->out, self->cursor.pos, sizeof(self->cursor.pos));
            BufferReader_Read(&params->out, self->hintPos, 0x18);
        }
        return 0;
    }
    if (self->state != 3)
        return 0;
    if (self->arriving) {
        AreaMap_UpdateArrival(self);
        return 0;
    }
    if (self->dialog != 0) {
        if (pad & kPadCross) {
            Window* dialog = guest::ptr<Window>(self->dialog);
            const int choice = WindowMgr_GetCursor(*gWindowMgr, dialog);
            WindowMgr_Close(*gWindowMgr, dialog);
            self->dialog = 0;
            if (choice == 1) {
                Sound_PlaySE(7, 0, 0x3FFF);
                return 0;
            }
            if (choice == 0) {
                Sound_PlaySE(9, 0, 0x3FFF);
                return 1;
            }
        }
        return 0;
    }
    if ((pad & kPadCross) == 0) {
        if (MapCursor_Update(&self->cursor)) {
            Sound_PlaySE(4, 0, 0x3FFF);
            self->areaText = guest::addr(Text(self->cursor.area + 0x5E));
        }
        return 0;
    }
    if (self->cursor.area == kMapExit) {
        AreaMap_AskLeave(self);
        return 0;
    }
    Sound_PlaySE(9, 0, 0x3FFF);
    return 1;
}

// 0x16F060
static void AreaMap_DrawArrival(AreaMap* self) {
    PlayerData* pd = *gPlayerData;
    const s32 sx = (s32)(Screen_X(0, -1) & 0xFFFF), sy = (s32)(Screen_Y(0, -1) & 0xFFFF);
    const float* tint = self->tint.value;
    GsDevice_ClearScreenColor(*gGsDevice, ((u32)(int)tint[0] & 0xFF) | ((u32)(int)tint[1] & 0xFF) << 8
                                          | ((u32)(int)tint[2] & 0xFF) << 16 | (u32)(int)tint[3] << 24);
    const u32 plate = (u32)(int)self->plateAlpha << 24 | 0x808080;
    SpriteSet_Draw(Sheet(self), 6, sx + 0xAF0, sy + 0xAE0, kZ, plate);
    SpriteSet_Draw(Sheet(self), pd->day + 7, sx + self->dayPos[0] * 16, sy + self->dayPos[1] * 16, kZ, plate);
    SpriteSet_Draw(Sheet(self), pd->time + 0x12, sx + self->periodPos[0] * 16, sy + self->periodPos[1] * 16, kZ,
                   (u32)(int)self->periodAlpha << 24 | 0x808080);
}

// ── 0x16F2B0 ──────────────────────────────────────────────────────────────
void AreaMap_Draw(AreaMap* self) {
    if (self->state != 3)
        return;
    GsDevice_ClearScreen(*gGsDevice);
    if (!(SpriteSet_Bind(Sheet(self)) & 0xFF))
        return;
    const s32 sx = (s32)(Screen_X(0, -1) & 0xFFFF), sy = (s32)(Screen_Y(0, -1) & 0xFFFF);
    const struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } background =
        {1, sx, sy, kZ, gScreenW << 4, gScreenH << 4, kPlain};
    SpriteSet_DrawEx(Sheet(self), 0, &background);
    if (self->arriving) {
        AreaMap_DrawArrival(self);
        return;
    }
    MapCursor_Draw(&self->cursor);
    AreaMap_DrawInfo(self);
    Font* font = *gFont;
    Font_SetColor(font, 0x7FBCBEC8u);
    Font_DrawStringPlain(font, Text(0x50), sx + self->hintPos[0] * 16, sy + self->hintPos[1] * 16, kZ);
}
