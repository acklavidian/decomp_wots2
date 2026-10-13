// Pause menu — decompiled from 0x1721E0–0x174560.  See include/game/pause_menu.h.

#include "game/pause_menu.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <utility>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void* PadMgr_Get();                                         // 0x13A920
    u16   Pad_GetPressed(void* mgr, int port);                  // 0x13A0C0
    int   Pad_IsConnected(void* mgr, int port);                 // 0x13A0A0
    void  Sound_PlaySE(u32 id, u32 positional, u32 volume);     // 0x180E70
    int   RenderMgr_FindEffect(void* mgr, u32 type);            // 0x14F0F0
    void  GsDevice_ClearScreenColor(GsDevice* dev, u32 rgba);   // 0x12F1F0
    // Textured geometry: TEX0 and PRIM, then UV / RGBAQ / XYZ2 per vertex.
    void  GsDevice_DrawTexturedPrim(GsDevice* dev, const void* packet, int vertexCount);    // 0x12FAF0
    SpriteSet* SpriteSet_Ctor(void* mem);                       // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    int   SpriteSet_IsLoaded(SpriteSet* set);                   // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                       // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);             // 0x17C570
    void* SpriteSet_GetSprite(SpriteSet* set, int index);       // 0x17BDF0
}

static_assert(sizeof(SlideEntry) == 0x24 && sizeof(SlideList) == 0x10C, "SlideList");
static_assert(offsetof(PauseMenu, list) == 0x1C && offsetof(PauseMenu, chosen) == 0x128, "PauseMenu");
static_assert(offsetof(PauseMenu, upDown) == 0x130 && sizeof(PauseMenu) == 0x18C, "PauseMenu");

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const GuestPtrVar<Font>     gFont{0x2EE67Cu};
static const GuestPtrVar<void>     gWindowMgr{0x2EE680u};
static const GuestPtrVar<void>     gRenderMgr{0x2EE684u};
static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);
static s32& gScreenW   = *guest::ptr<s32>(0x2EE4C0u);
static s32& gScreenH   = *guest::ptr<s32>(0x2EE4C4u);

enum { kAssetPauseSprites = 0x55, kRowHeight = 38, kButtonWidth = 240 };
enum : u32 { kZ = 0xFFFFE7 };

// ══ A sprite stretched over a rectangle ═══════════════════════════════════
struct SpriteRect {
    s32 x0, y0, x1, y1;     // layout coordinates
    u32 z;
    u32 blend;
    u32 rgba;
    s16 u0, v0, u1, v1;     // texels; filled in from the sprite
    s32 sprite;
};

// 0x17BA50 — the sprite's record and the TEX0 value that selects its texture
static bool SpriteSet_GetTex0(SpriteSet* set, int index, u64* tex0, const u8** record) {
    const u32* fields = reinterpret_cast<const u32*>(set);
    const u8* descriptor = guest::ptr<const u8>(fields[2]);
    if (!descriptor)
        return false;
    *record = static_cast<const u8*>(SpriteSet_GetSprite(set, index));
    if (!*record)
        return false;
    const u8 texture = (*record)[0x19], clut = (*record)[0x18];
    descriptor += texture * 0x30;
    auto rd16 = [&](int offset) { u16 v; memcpy(&v, descriptor + offset, 2); return (u64)v; };
    *tex0 = rd16(10) << 30 | rd16(8) << 26 | (u64)descriptor[2] << 20 | rd16(4) | rd16(6) << 14
          | 0x400000000ull | (u64)(u32)(rd16(0x1E) + clut) << 37 | 0x2000000000000000ull;
    return true;
}

// 0x174560
static void SpriteRect_Emit(const SpriteRect* r, u64 tex0) {
    GsDevice* dev = *gGsDevice;
    if (!dev)
        return;
    struct {
        u64 tex0, prim;
        struct { s16 u, v; u32 _4; u32 rgba; float q; u16 x, y; u32 z; } v[4];
    } packet;
    memset(&packet, 0, sizeof(packet));
    packet.tex0 = tex0;
    packet.prim = (u64)r->blend << 6 | 0x115;           // textured triangle fan
    const s32 pos[4][2] = {{r->x0, r->y0}, {r->x1, r->y0}, {r->x1, r->y1}, {r->x0, r->y1}};
    const s16 uv[4][2] = {{r->u0, r->v0}, {r->u1, r->v0}, {r->u1, r->v1}, {r->u0, r->v1}};
    for (int i = 0; i < 4; i++) {
        packet.v[i].u = (s16)(uv[i][0] << 4);
        packet.v[i].v = (s16)(uv[i][1] << 4);
        packet.v[i].rgba = r->rgba;
        packet.v[i].q = 1.0f;
        packet.v[i].x = (u16)Screen_XAligned(pos[i][0] << 4);
        packet.v[i].y = (u16)Screen_XAligned(pos[i][1] * -16);      // the x conversion, as in the original
        packet.v[i].z = r->z;
    }
    GsDevice_DrawTexturedPrim(dev, &packet, 4);
}

// 0x174400
static bool SpriteRect_Draw(SpriteRect* r, SpriteSet* set) {
    if (!set)
        return false;
    u64 tex0;
    const u8* record;
    if (!SpriteSet_GetTex0(set, r->sprite, &tex0, &record))
        return false;
    s16 u, v, w, h;
    memcpy(&u, record + 8, 2);
    memcpy(&v, record + 10, 2);
    memcpy(&w, record + 12, 2);
    memcpy(&h, record + 14, 2);
    r->u0 = u;
    r->v0 = v;
    r->u1 = (s16)(u + w);
    r->v1 = (s16)(v + h);
    SpriteRect_Emit(r, tex0);
    return true;
}

// ══ The sliding list ══════════════════════════════════════════════════════
static float RowY(int row) { return (float)((gScreenH >> 1) - 22 + row * -kRowHeight); }

// 0x1744B0 — returns 1 while still moving
static int SlideEntry_Step(SlideEntry* e) {
    if (++e->step > e->steps)
        return 0;
    const float share = 1.0f / e->ease, rest = e->ease - 1.0f;
    e->x = (e->x * rest + e->targetX) * share;
    e->y = (e->y * rest + e->targetY) * share;
    if (e->step < e->steps)
        return 1;
    e->x = e->targetX;
    e->y = e->targetY;
    return 0;
}

// 0x172D40 (the entry under the cursor, 40 pixels further left) / 0x172F00
static void SlideList_Place(SlideList* list, int row, bool selected) {
    SlideEntry* e = &list->entry[row];
    e->targetX = (float)((gScreenW >> 1) - (selected ? 240 : 200));
    e->targetY = RowY(row);
    e->step = 0;
    if (list->phase == kSlideReady) {
        e->steps = gFrameRate * 8 / 30;
        e->ease = 3.0f;
    } else if (gFrameRate == 25) {
        e->steps = 6;
        e->ease = 2.8f;
    } else {
        e->steps = gFrameRate * 8 / 30;
        e->ease = 2.5f;
    }
}

// 0x172C40 / 0x172CC0
static bool SlideList_Move(SlideList* list, int delta) {
    if ((list->phase & (kSlideIn | kSlideOut | kSlideGone)) != 0)
        return false;
    SlideList_Place(list, list->cursor, false);
    list->cursor = (s16)(list->cursor + delta);
    if (list->cursor < 0)
        list->cursor = (s16)(list->count - 1);
    else if (list->cursor >= (s16)list->count)
        list->cursor = 0;
    SlideList_Place(list, list->cursor, true);
    return true;
}

static void SetEntries(SlideList* list, std::initializer_list<std::pair<u16, int>> entries) {
    TextTable* table = TextTable_Get();
    list->count = (u16)entries.size();
    int i = 0;
    for (const auto& [id, text] : entries) {
        list->entry[i].id = id;
        list->entry[i].text = guest::addr(TextTable_Find(table, text));
        i++;
    }
}

// ── 0x173CD0 ──────────────────────────────────────────────────────────────
// Fills the list for one of the situations and parks every button off the
// right edge.
static void SlideList_Build(SlideList* list, int kind) {
    list->phase = kSlideIn;
    list->stepTimer = gFrameRate == 25 ? 1 : 2;
    list->next = 0;
    list->cursor = 0;
    list->confirm = 0;
    switch (kind) {
    case 0x20: SetEntries(list, {{9, 0x46}, {10, 0x47}, {7, 0x12}}); break;                             // 0x1734C0
    case 0x10: SetEntries(list, {{0xE, 0x49}, {0xF, 0x4A}, {0x10, 0x4B}, {0x11, 0x4C}, {0x12, 0x4D},
                                 {0x14, 0x4F}}); break;                                                 // 0x1735C0
    case 8:    SetEntries(list, {{8, 0x45}, {0xB, 0x46}, {0xC, 0x47}, {0xD, 0x48}}); break;             // 0x173770
    case 4:    SetEntries(list, {{0, 0x3E}, {1, 0x3F}, {2, 0x40}, {5, 0x43}, {3, 0x41}, {6, 0x44}}); break;   // 0x1738B0
    case 2:    SetEntries(list, {{2, 0x40}, {3, 0x41}, {4, 0x42}}); break;                              // 0x173A60
    case 1:    SetEntries(list, {{0, 0x3E}, {1, 0x3F}, {2, 0x40}, {3, 0x41}, {4, 0x42}}); break;        // 0x173B60
    }
    for (int i = 0; i < list->count; i++) {
        list->entry[i].x = (float)(gScreenW >> 1);
        list->entry[i].y = RowY(i);
        list->entry[i].step = 0;
        list->entry[i].steps = 0;
    }
}

static bool AnyMoving(const SlideList* list) {
    for (int i = 0; i < list->count; i++)
        if (list->entry[i].step < list->entry[i].steps)
            return true;
    return false;
}

// ── 0x1730C0 ──────────────────────────────────────────────────────────────
// Moves the buttons.  Returns 1 while the list is not taking input.
static int SlideList_Update(SlideList* list) {
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);
    if (list->confirm != 0)
        list->confirm--;
    if (pressed & kPadCross)
        list->confirm = (s16)(gFrameRate * 5 / 30);
    for (int i = 0; i < list->count; i++)
        if (list->entry[i].step < list->entry[i].steps)
            SlideEntry_Step(&list->entry[i]);

    switch (list->phase) {
    case kSlideGone:
        return 1;
    case kSlideOut:
        // One button after another, last first, back off the right edge.
        if (--list->stepTimer < 0) {
            list->stepTimer = gFrameRate == 25 ? 1 : 2;
            if (--list->next < 0) {
                list->phase = kSlideGone;
            } else {
                SlideEntry* e = &list->entry[list->next];
                const bool arriving = list->phaseBefore == kSlideIn && e->step < e->steps;
                e->targetX = (float)(gScreenW >> 1);
                e->targetY = RowY(list->next);
                e->steps = arriving ? e->step : gFrameRate * 10 / 30;
                e->step = 0;
                e->ease = 2.5f;
            }
        }
        return 1;
    case kSlideReady:
        return AnyMoving(list) ? 1 : 0;
    case kSlideSettle:
        if (!AnyMoving(list))
            list->phase = kSlideReady;
        return 0;
    case kSlideIn:
        if (++list->stepTimer > (gFrameRate == 25 ? 1 : 2)) {
            list->stepTimer = 0;
            SlideList_Place(list, list->next, list->next == list->cursor);
            if (++list->next == (s16)list->count)
                list->phase = kSlideSettle;
        }
        return 1;
    default:
        return 0;
    }
}

// ── 0x173E40 ──────────────────────────────────────────────────────────────
// A button is sprite 2, or sprite 1 when it is under the cursor; while it
// slides between the two places the sprites and the text colours cross-fade.
static void SlideEntry_Draw(const SlideEntry* e, SpriteSet* set, bool selected, bool fading) {
    SpriteRect r = {(s32)e->x, (s32)e->y, (s32)(e->x + (float)kButtonWidth), (s32)(e->y - (float)kRowHeight),
                    kZ, 1, 0x80808080u, 0, 0, 0, 0, 0};
    const float t = e->steps != 0 ? (float)e->step / (float)e->steps : 0.0f;
    const u32 alpha = (u32)(int)(t * 127.0f) & 0xFF;
    if (!fading) {
        r.sprite = 2;
        SpriteRect_Draw(&r, set);
    } else {
        r.sprite = selected ? 1 : 2;
        r.rgba = alpha << 24 | 0x808080;
        SpriteRect_Draw(&r, set);
        r.sprite = selected ? 2 : 1;
        r.rgba = (u32)(int)(127.0f - (float)alpha) << 24 | 0x808080;
        SpriteRect_Draw(&r, set);
    }
    if (e->text == 0)
        return;
    auto mix = [t](int r0, float dr, int g0, float dg, int b0, float db) {
        return (u32)(((int)(t * dr) + r0) & 0xFF) | (u32)(((int)(t * dg) + g0) & 0xFF) << 8
             | (u32)(((int)(t * db) + b0) & 0xFF) << 16 | 0x7F000000u;
    };
    u32 color = 0, shadow = 0;
    if (!selected && !fading) {
        color = 0x7F858D92u;
        shadow = 0x7F1A1C1Eu;
    } else if (selected && !fading) {
        color = shadow = 0x7FACB2BEu;
    } else if (selected) {
        color = mix(0x92, 44, 0x8D, 37, 0x85, 39);
        shadow = mix(0x1E, 10, 0x1C, 17, 0x1A, 0);
    } else {
        color = mix(0xBE, -44, 0xB2, -37, 0xAC, -39);
        shadow = mix(0x28, -10, 0x2D, -17, 0x1A, 0);
    }
    Font* font = *gFont;
    const int x = (int)(Screen_X(((s32)e->x + 50) * 16, 0) & 0xFFFF);
    const int y = (int)(Screen_Y(((s32)e->y - 8) * -16, 0) & 0xFFFF);
    const u16* text = guest::ptr<const u16>(e->text);
    Font_SetColor(font, shadow);
    Font_DrawString(font, text, x + 0x10, y + 0x10, kZ, 1.0f, 0.0f);
    Font_SetColor(font, color);
    Font_DrawString(font, text, x, y, kZ, 1.0f, 0.0f);
}

// ══ The menu ══════════════════════════════════════════════════════════════
static bool Fading() { return RenderMgr_FindEffect(*gRenderMgr, 11) != 0; }

static void Show(PauseMenu* self) {
    self->chosenId = -1;
    self->_0c = -1;
    self->open = 1;
    self->chosen = 0;
    self->state = 1;
    SlideList_Build(&self->list, self->allowed);
}

// Starts the buttons sliding away, if they are not already.
static bool SlideAway(PauseMenu* self) {
    SlideList* list = &self->list;
    if ((list->phase & (kSlideIn | kSlideSettle | kSlideReady)) == 0)
        return false;
    if (list->phase == kSlideReady) {
        list->next = (s16)list->count;
        list->stepTimer = 0;
    }
    list->phaseBefore = list->phase;
    list->phase = kSlideOut;
    self->state = 3;
    return true;
}

// ── 0x172B10 ──────────────────────────────────────────────────────────────
PauseMenu* PauseMenu_Ctor(PauseMenu* self) {
    memset(self->list.entry, 0, sizeof(self->list.entry));
    PadRepeatPair_Ctor(&self->upDown);
    PadRepeatPair_SetMasks(&self->upDown, kPadUp, kPadDown);
    self->gsDevice = guest::addr(*gGsDevice);
    self->font = guest::addr(*gFont);
    self->windowMgr = guest::addr(*gWindowMgr);
    self->open = 0;
    self->state = 0;
    self->dim = 0;
    self->_0c = -1;
    self->allowed = 1;
    self->allowedBefore = 1;
    self->_18 = 0;
    self->chosenId = -1;
    PadRepeatPair_SetTiming(&self->upDown, 18, 8, 10);
    void* mem = MemAlloc(0x14);
    self->sprites = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    if (!SpriteSet_Load(guest::ptr<SpriteSet>(self->sprites), kAssetPauseSprites, -1)) {
        if (self->sprites)
            SpriteSet_Dtor(guest::ptr<SpriteSet>(self->sprites), 1);
        self->sprites = 0;
    }
    return self;
}

// ── 0x172A80 ──────────────────────────────────────────────────────────────
PauseMenu* PauseMenu_Dtor(PauseMenu* self, s16 flag) {
    if (!self)
        return self;
    if (self->sprites)
        SpriteSet_Dtor(guest::ptr<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x172A50 ──────────────────────────────────────────────────────────────
int PauseMenu_Poll(PauseMenu* self) {
    return self->sprites != 0 ? (SpriteSet_IsLoaded(guest::ptr<SpriteSet>(self->sprites)) & 0xFF) : 0;
}

// ── 0x172810 ──────────────────────────────────────────────────────────────
void PauseMenu_Open(PauseMenu* self, int force) {
    if (self->allowed != 0 && (force != 0 || !Fading()))
        Show(self);
}

// ── 0x1727D0 ──────────────────────────────────────────────────────────────
void PauseMenu_Close(PauseMenu* self, int sound) {
    if (sound != 0)
        Sound_PlaySE(9, 0, 0x3FFF);
    self->open = 0;
    self->state = 0;
}

// ── 0x1726F0 ──────────────────────────────────────────────────────────────
int PauseMenu_Back(PauseMenu* self) {
    return self->state != 3 && SlideAway(self) ? 1 : 0;
}

// ── 0x1723C0 ──────────────────────────────────────────────────────────────
static void PauseMenu_Step(PauseMenu* self) {
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);
    SlideList* list = &self->list;
    if (self->state != 3) {
        PadRepeat_Update(&self->upDown.a);
        PadRepeat_Update(&self->upDown.b);
        if (!PadRepeat_IsHeld(&self->upDown.b) && self->upDown.a.fired != 0) {
            if (SlideList_Move(list, -1))
                Sound_PlaySE(2, 0, 0x3FFF);
        } else if (!PadRepeat_IsHeld(&self->upDown.a) && self->upDown.b.fired != 0) {
            if (SlideList_Move(list, 1))
                Sound_PlaySE(2, 0, 0x3FFF);
        }
    }
    // Every button has left: the menu is closed, or the next list opens.
    if (list->phase == kSlideGone && !AnyMoving(list)) {
        Sound_PlaySE(9, 0, 0x3FFF);
        self->open = 0;
        self->state = 0;
        if (self->_18 != 0) {
            self->allowedBefore = self->allowed;
            self->allowed = self->_18;
            self->_18 = 0;
            if (self->allowed != 0 && !Fading())
                Show(self);
        }
    }
    if (SlideList_Update(list) != 0)
        return;
    if (list->confirm != 0) {                   // Cross
        self->chosen = 1;
        list->confirm = 0;
        Sound_PlaySE(5, 0, 0x3FFF);
        return;
    }
    self->chosen = 0;
    if (pressed & kPadTriangle) {
        if (self->allowed == 0x10) {            // a question must be answered: "none of these"
            self->chosen = 1;
            self->chosenId = 0x14;
            list->confirm = 0;
            Sound_PlaySE(7, 0, 0x3FFF);
        } else if (self->state != 3) {
            SlideAway(self);
        }
    }
}

// ── 0x172890 ──────────────────────────────────────────────────────────────
// Start opens and closes the menu; so does unplugging the controller.
int PauseMenu_Update(PauseMenu* self) {
    void* pad = PadMgr_Get();
    const bool toggle = (Pad_GetPressed(pad, 0) & kPadStart) != 0
                     || (self->open == 0 && Pad_IsConnected(PadMgr_Get(), 0) == 0);
    if (toggle && !Fading()) {
        if (self->open == 0) {
            if (self->allowed != 0 && !Fading())
                Show(self);
        } else if (self->allowed != 0x10 && self->state != 3) {
            SlideAway(self);
        }
    }
    if (self->state != 0)
        PauseMenu_Step(self);
    return self->open;
}

// ── 0x1721E0 ──────────────────────────────────────────────────────────────
void PauseMenu_Draw(PauseMenu* self) {
    if (self->state == 0) {
        if (self->dim != 0)
            self->dim -= 0x10;
    } else if (self->dim < 0x41) {
        self->dim += 0x10;
    }
    if ((self->allowed & 0x10) == 0)
        GsDevice_ClearScreenColor(guest::ptr<GsDevice>(self->gsDevice), (u32)self->dim << 24);
    if (self->state == 0)
        return;
    SpriteSet* set = guest::ptr<SpriteSet>(self->sprites);
    if (self->sprites == 0 || !(SpriteSet_Bind(set) & 0xFF))
        return;
    const SlideList* list = &self->list;
    for (int i = 0; i < list->count; i++) {
        const bool selected = i == list->cursor;
        SlideEntry_Draw(&list->entry[i], set, selected, selected || list->phase == kSlideReady);
    }
    if ((self->allowed & 0x34) == 0) {          // the "PAUSE" plate
        SpriteRect plate = {-60, 16, 60, -16, kZ, 1, 0x80808080u, 0, 0, 0, 0, 0};
        SpriteRect_Draw(&plate, set);
    }
}
