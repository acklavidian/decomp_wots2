// The status menu (GameplayScene mode 5; the pause menu's "Sword Selection",
// "Item Selection" and "Status Check" are its three pages).
//
//   MenuRes      0x22C670–0x22C8F0  what its pages share, loaded once at boot
//   StatusFrame  0x22C990–0x22E2F0  the frame round every page: health, vigour
//                (+ gauges 0x22E750–0x22EB70)  and sword gauges, money, hints, help line
//   StatusMenu   0x210E20–0x211DE0  the shell: makes the pages, turns them
//
// A page (made by 0x2117B0) starts {menu, s16 loaded, vtable}: +0x08 destroy,
// +0x0C begin loading (→ 0: failed), +0x10 enter, +0x14 draw, +0x18 update,
// +0x1C the frame's numbers changed.  It answers through the menu's action
// slot (+0x1C: {what, argument}): 1 close and show the pause menu, 2 close,
// 3 drop a sword, 4 show a sword's details, 5 drop an item, 6 hand something
// over, 7 / 8 go to the swords / items page.

#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/param_file.h"
#include "engine/screen_layout.h"
#include "engine/text_label.h"
#include "game/gameplay_scene.h"
#include "game/item_select.h"
#include "hal/ps2_types.h"
#include "native/guest.h"

#include <cstdio>
#include <cstring>

struct SpriteSet;
extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    SpriteSet* SpriteSet_Ctor(void* mem);                               // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    int   SpriteSet_IsLoaded(SpriteSet* set);                           // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                               // 0x17BFD0
    void* SpriteSet_GetSprite(SpriteSet* set, int index);               // 0x17BDF0
    int   SpriteSet_DrawEx(void* set, int index, void* params);         // 0x17ADF0
    // (the real 0x17C230 takes no size; the native loader wants one)
    char  SpriteSet_ParseSized(SpriteSet* set, const void* data, u32 size);
    void* AssetRef_Ctor(void* mem);                                     // 0x174C50
    char  AssetQueue_Request(void* ref, int assetId);                   // 0x174C90
    void* AssetRef_GetData(void* ref);                                  // 0x174A00
    void  Gs_FillRect(const s32* rect, u32 z, u32 rgba);                // 0x225CD0
    u32   Screen_X(int v, int align);                                   // 0x12E5E0
    u32   Screen_Y(int v, int align);                                   // 0x12E550
    void  Sound_PlaySE(int id, int randomPitch, int volume);            // 0x180E70
    int   PlayerData_GetReadiedSword(void* self);                       // 0x177570

    // The pages (their own files, or stand-ins).
    void* SwordsPage_Ctor(void* mem, StatusMenu* menu, void* info, u8 noWorld, s32 mode);   // 0x193CE0
    void* ItemsPage_Ctor(void* mem, StatusMenu* menu, void* info, u8 noWorld);              // 0x1E7440
    void* StatusPage_Ctor(void* mem, StatusMenu* menu, void* info);                         // 0x213D20
    void  SwordsPage_Select(void* page, s32 sword);                                         // 0x193A80
    void  ItemsPage_Reset(void* page);                                                      // 0x1E7230
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
const GuestPtrVar<Font> gFont{0x2EE67Cu};
const GuestPtrVar<u8>   gPlayerData{0x2EE688u};
u32* const gFrameRate = guest::ptr<u32>(0x2EDA00u);
u32* const gMenuRes   = guest::ptr<u32>(0x2EE748u);

const u32 kZ = 0xFFFF82u;

inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }

// ═══ What the pages share (0x20 bytes, *0x2EE748) ══════════════════════════
struct MenuRes {
    s32 state;          // 1 … 6 = everything is there
    u32 sprites;        // +0x04  menu/StatusMenuCommon/Img.bpm
    u32 layout;         // +0x08  layout 1, StatusMenuCommon
    u32 weaponSprites;  // +0x0C  from the pack menu/WeaponMenu/Data
    u32 weaponParams;   // +0x10  its numbers (a ParamFile)
    u32 itemSprites;    // +0x14  menu/ItemMenu/Img.bpm
    u32 itemLayout;     // +0x18  layout 4 (AreaMapMenu: the status page's map)
    u32 pack;           // +0x1C  AssetRef
};

// A gauge that runs to its value instead of jumping (0x20 bytes).
struct RunGauge {
    s16 x, y;           // +0x00
    s16 value, max;     // +0x04
    s16 scale;          // +0x08  the value that fills 100 pixels
    s16 step;           // +0x0A
    u32 colour[2];      // +0x0C
    s32 ticks;          // +0x14  1: a mark per point
    u8  ofPlayer;       // +0x18
    u8  _19;
    s16 shown, shownMax;    // +0x1A
    s16 _1e;
};
struct NotchGauge { s16 x, y, count, _06; };

struct StatusFrame {
    u32 params;         // +0x00  the player's CharaParams (health at +0x40 / +0x42)
    s32 resume;         // +0x04  what the scene's "pause shown" becomes on leaving
    u8  noWorld;        // +0x08
    u8  _09[3];
    s32 phase;          // +0x0C  3 = ready
    s16 durability;     // +0x10
    s16 arrows;         // +0x12  the page arrows are shown (L1 / R1 turn pages)
    s16 rightTimer;     // +0x14
    s16 leftTimer;      // +0x16
    u8  changed;        // +0x18  a number has just settled: pages refresh
    u8  _19[3];
    u32 layout;         // +0x1C
    s32 cover[4];       // +0x20  the page area: left, top, right, bottom (y up)
    s32 area[4];        // +0x30
    RunGauge health;    // +0x40
    RunGauge vigour;    // +0x60
    NotchGauge sword;   // +0x80
    TextLabel names[4]; // +0x88
    TextLabel values[4];// +0x138  health, vigour, durability, money
    s32 helpX, helpY;   // +0x1E8
    WString help;       // +0x1F0
    u32 helpVtable;     // +0x1FC
    u32 hint[2];        // +0x200
    u32 hintDefault[2]; // +0x208
    s32 hintAt[2][2];   // +0x210
    u32 vtable;         // +0x220
};
static_assert(sizeof(TextLabel) == 0x2C && sizeof(StatusFrame) == 0x224 && offsetof(StatusFrame, help) == 0x1F0,
              "StatusFrame");

struct Page {
    u32 menu;
    s16 loaded;
    s16 _06;
    u32 vtable;
};
inline void PageDestroy(Page* p) { VirtualCall<void*, Page*, int>(p->vtable, 0x08, p, 1); }
inline char PageLoad(Page* p)    { return VirtualCall<char, Page*>(p->vtable, 0x0C, p); }
inline void PageEnter(Page* p)   { VirtualCall<void, Page*>(p->vtable, 0x10, p); }
inline void PageDraw(Page* p)    { VirtualCall<void, Page*>(p->vtable, 0x14, p); }
inline void PageUpdate(Page* p)  { VirtualCall<void, Page*>(p->vtable, 0x18, p); }
inline void PageRefresh(Page* p) { VirtualCall<void, Page*>(p->vtable, 0x1C, p); }

// The shell, as it really is (the scene's header only names what it reads).
struct Menu {
    u32 current;        // +0x00  Page*
    u32 frame;          // +0x04
    u32 pages[3];       // +0x08  swords, items, status
    s32 page;           // +0x14
    u32 icons;          // +0x18
    s32 action[2];      // +0x1C
    s32 turning;        // +0x24  1 a page turn, 2 / 3 a jump to the swords / items page
    s32 mode;           // +0x28
    u32 vtable;         // +0x2C
    u32 info[12];       // +0x30
    u32 loading;        // +0x60  the page being loaded in the background
    s16 turnTimer;      // +0x64
    s16 _66;
    s32 jumpSword;      // +0x68
    s32 state;          // +0x6C  1 frame loading, 2 first page, 3 the others, 4 all there
    s32 from, to;       // +0x70
    u8  noWorld;        // +0x78
    u8  _79[3];
};
static_assert(sizeof(Menu) == 0x7C && sizeof(Menu) == sizeof(StatusMenu), "StatusMenu");

inline s32 TurnFrames() { return (s32)(*gFrameRate * 0xE) / 0x1E; }

// ── 0x22C8F0 ──────────────────────────────────────────────────────────────
MenuRes* MenuRes_Make() {
    if (*gMenuRes == 0) {
        MenuRes* res = static_cast<MenuRes*>(MemAlloc(sizeof(MenuRes)));
        memset(res, 0, sizeof *res);
        *gMenuRes = guest::addr(res);
        res->sprites = guest::addr(SpriteSet_Ctor(MemAlloc(0x14)));
        SpriteSet_Load(P<SpriteSet>(res->sprites), 0x4B, 0);
        res->state = 1;
    }
    return P<MenuRes>(*gMenuRes);
}

// ── 0x225BB0: a rectangle with a colour at each corner ────────────────────
void FillGradient(const s32* rect, const u32* colours, u32 z) {
    struct V { u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 prim; V v[4]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.prim = 0x4C;
    const u16 x0 = (u16)rect[0], y0 = (u16)rect[1], x1 = (u16)(rect[0] + rect[2]), y1 = (u16)(rect[1] + rect[3]);
    packet.v[0] = {colours[0], 1.0f, x0, y0, z};
    packet.v[1] = {colours[1], 1.0f, x0, y1, z};
    packet.v[2] = {colours[2], 1.0f, x1, y0, z};
    packet.v[3] = {colours[3], 1.0f, x1, y1, z};
    GsDevice_DrawPrim(*gGsDevice, &packet, 4);
}

// ── 0x22EB70: one frame of a running gauge; → still running ───────────────
bool RunGauge_Step(RunGauge* g) {
    const auto run = [g](s16 target, s16* shown) {
        s32 gap = (s32)target - (s32)*shown;
        if (gap == 0)
            return;
        const s32 sign = gap < 1 ? -(s32)(gap < 0) : 1;
        const s32 dist = gap < 0 ? -gap : gap;
        s32 speed = g->step;
        if (g->step * 10 < dist) {
            s32 extra = (dist - g->step * 10) / (g->step != 0 ? g->step : 1);
            if (extra < 0)
                extra++;
            speed = g->step + (extra >> 1);
        }
        const s32 move = sign * speed;
        *shown = (s16)(*shown + move);
        if ((move > 0 && target < *shown) || (move < 0 && *shown < target))
            *shown = target;
    };
    run(g->value, &g->shown);
    run(g->max, &g->shownMax);
    if (g->shownMax < g->shown)
        g->shown = g->shownMax;
    return g->shown != g->value || g->shownMax != g->max;
}

// ── 0x22E900 ──────────────────────────────────────────────────────────────
void RunGauge_Draw(const RunGauge* g) {
    const s32 x = (u16)g->x + 0x20, y = (u16)g->y + 0x20;
    const s32 full = (s32)(((float)g->shownMax / (float)g->scale) * 1600.0f);
    s32 box[4] = {x, y, full + 0x40, 0x100};
    Gs_FillRect(box, kZ, 0x52080404u);
    box[0] -= 0x20;
    box[1] -= 0x20;
    Gs_FillRect(box, kZ, 0x7FACB2BEu);
    s32 bed[4] = {x, y, full, 0xC0};
    Gs_FillRect(bed, kZ, 0x7F181A1Au);
    u32 a = g->colour[0], b = g->colour[1];
    if (*reinterpret_cast<s32*>(*gPlayerData + 0x6E80) != 0 && g->ofPlayer != 0)
        a = b = 0x80337AD2u;
    const s32 bar[4] = {x, y, (s32)(((float)g->shown / (float)g->scale) * 1600.0f), 0xC0};
    const u32 colours[4] = {a, b, a, b};
    FillGradient(bar, colours, kZ);
    if (g->ticks == 1) {
        s32 tick[4] = {x, y, 1, 0xC0};
        for (s32 i = 0; i < g->shownMax; i++) {
            tick[0] += (0x640 / g->scale) & 0xFFFF;
            Gs_FillRect(tick, kZ, 0x7FACB2BEu);
        }
    }
}

// ── 0x22E750: the sword's durability, a block per point ───────────────────
void NotchGauge_Draw(const NotchGauge* g) {
    const s32 x = (u16)g->x + 0x20, y = (u16)g->y + 0x20;
    s32 box[4] = {x, y, g->count * 0x210 + 0x40, 0xA0};
    Gs_FillRect(box, kZ, 0x52080404u);
    box[0] -= 0x20;
    box[1] -= 0x20;
    Gs_FillRect(box, kZ, 0x7F1A1C20u);
    const s32 bar[4] = {x, y, g->count * 0x210 - 0x10, 0x60};
    const u32 colours[4] = {0x7F70666Fu, 0x7FA9A1A8u, 0x7F70666Fu, 0x7FA9A1A8u};
    FillGradient(bar, colours, kZ);
    s32 gap[4] = {x, y, 0x10, 0x60};
    for (s32 i = 0; i < g->count - 1; i++) {
        gap[0] += 0x200;
        Gs_FillRect(gap, kZ, 0x7F1A1C20u);
    }
}

// Text built the way the original does with sprintf("%d") and a wide string.
struct Wide {
    u16 text[64];
    int n = 0;
    Wide& num(int v) {
        char digits[16];
        snprintf(digits, sizeof digits, "%d", v);
        for (const char* c = digits; *c != 0 && n < 62; c++)
            text[n++] = (u16)*c;
        text[n] = 0;
        return *this;
    }
    Wide& add(const u16* s) {
        for (; s != nullptr && *s != 0 && n < 62; s++)
            text[n++] = *s;
        text[n] = 0;
        return *this;
    }
};
void SetValue(TextLabel* label, const Wide& w, float scale) {
    // (TextLabel_SetString copies, but reads its argument as guest memory.)
    static u32 buffer = 0;
    if (buffer == 0)
        buffer = guest::addr(guest::alloc(sizeof w.text));
    memcpy(P(buffer), w.text, sizeof w.text);
    TextLabel_SetString(label, P<const u16>(buffer));
    TextLabel_SetScale(label, scale);
}

// ── 0x22D910 / 0x22D760 / 0x22D630 ────────────────────────────────────────
void Frame_SetHealthText(StatusFrame* self) {
    if (self->params == 0)
        return;
    s16 health = *P<s16>(self->params + 0x40);
    if (health == -1)
        health = 0;
    Wide w;
    w.num(health).add(Text(2)).num(*P<s16>(self->params + 0x42));
    SetValue(&self->values[0], w, 0.82f);
}
void Frame_SetVigourText(StatusFrame* self) {
    const u8* pd = *gPlayerData;
    Wide w;
    w.num(*reinterpret_cast<const s16*>(pd + 0x108)).add(Text(2)).num(*reinterpret_cast<const s16*>(pd + 0x10A));
    SetValue(&self->values[1], w, 0.82f);
}
void Frame_SetDurability(StatusFrame* self, int durability) {
    self->durability = (s16)durability;
    self->sword.count = (s16)durability;
    Wide w;
    w.num(durability);
    SetValue(&self->values[2], w, 0.82f);
}

// ── 0x22DAE0: the labels' text ────────────────────────────────────────────
void Frame_SetTexts(StatusFrame* self) {
    Frame_SetHealthText(self);
    Frame_SetVigourText(self);
    TextLabel_SetString(&self->names[0], Text(0x8B));
    TextLabel_SetString(&self->names[1], Text(0x8C));
    TextLabel_SetString(&self->names[2], Text(0x8D));
    TextLabel_SetString(&self->names[3], Text(0x1B));
    Wide w;
    w.num(*reinterpret_cast<const s16*>(*gPlayerData + 0x106)).add(Text(5));
    SetValue(&self->values[3], w, 0.9f);
}

void* Frame_Dtor(StatusFrame* self, int flag);

// ── 0x22E2F0 ──────────────────────────────────────────────────────────────
StatusFrame* Frame_Ctor(StatusFrame* self, u32 params, s32 resume, u8 noWorld) {
    memset(self, 0, sizeof *self);
    self->vtable = 0x2EC628u;
    self->params = params;
    self->resume = resume;
    self->noWorld = noWorld;
    for (int i = 0; i < 4; i++) {
        TextLabel_Ctor(&self->names[i]);
        TextLabel_Ctor(&self->values[i]);
    }
    self->helpVtable = 0x2EAF30u;
    WString_Ctor(&self->help);
    WString_Reserve(&self->help, 0);
    self->arrows = noWorld == 0 ? 1 : 0;
    Frame_SetTexts(self);
    for (int i = 0; i < 4; i++)
        TextLabel_SetScale(&self->names[i], 0.9f);
    for (int i = 0; i < 4; i++)
        TextLabel_SetScale(&self->values[i], 0.9f);
    const u8* pd = *gPlayerData;
    const s16 sx = (s16)Screen_X(0, 0), sy = (s16)Screen_Y(0, 0);
    if (params != 0) {
        s16 health = *P<s16>(params + 0x40);
        if (health == -1)
            health = 0;
        const s16 max = *P<s16>(params + 0x42);
        const s16 x = (s16)(sx - 0xEE0);
        const s32 rate = (s32)*gFrameRate;
        self->health.x = x;
        self->health.y = (s16)(sy + 0x1A0);
        self->health.value = self->health.shown = health;
        self->health.max = self->health.shownMax = max;
        self->health.scale = 1000;
        self->health.step = (s16)(((s16)(300 / rate) * 0x1E) / rate);
        self->health.colour[1] = 0x7F759A2Bu;
        self->health.colour[0] = 0x7F3B6001u;
        self->health.ofPlayer = 1;
        self->health.ticks = 0;
        self->vigour.x = x;
        self->vigour.y = (s16)(sy + 0x300);
        self->vigour.value = self->vigour.shown = *reinterpret_cast<const s16*>(pd + 0x108);
        self->vigour.max = self->vigour.shownMax = *reinterpret_cast<const s16*>(pd + 0x10A);
        self->vigour.scale = 10;
        self->vigour.step = (s16)(0x1E / rate);
        self->vigour.colour[1] = 0x7F3B5B75u;
        self->vigour.colour[0] = 0x7F192E92u;
        self->vigour.ofPlayer = 0;
        self->vigour.ticks = 1;
        self->sword.x = x;
        self->sword.y = (s16)(sy + 0x460);
        self->sword.count = 0;
    }
    // The two hints at the bottom: what Cross and Triangle do.
    if (resume == 1) {
        self->hintDefault[0] = guest::addr(Text(0x85));
        self->hintDefault[1] = guest::addr(Text(0x87));
    } else if (resume == 0) {
        self->hintDefault[0] = guest::addr(Text(0x85));
        self->hintDefault[1] = guest::addr(Text(0x86));
    }
    self->hint[0] = self->hintDefault[0];
    self->hint[1] = self->hintDefault[1];
    return self;
}

// ── 0x22E190 ──────────────────────────────────────────────────────────────
void* Frame_Dtor(StatusFrame* self, int flag) {
    if (self != nullptr) {
        self->vtable = 0x2EC628u;
        WString_Release(&self->help);
        for (int i = 3; i >= 0; i--)
            TextLabel_Dtor(&self->values[i], -1);
        for (int i = 3; i >= 0; i--)
            TextLabel_Dtor(&self->names[i], -1);
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}
GUEST_BIND(0x22E190, Frame_Dtor);

// ── 0x22DD20: where things go, from the layout ────────────────────────────
void Frame_Place(StatusFrame* self) {
    ScreenLayout* layout = P<ScreenLayout>(self->layout);
    if (self->layout == 0 || layout->ready == 0)
        return;
    ScreenLayout_SetDepth(layout, kZ);
    ScreenLayout_SetGroupDepth(layout, 0xFFFF80u, 4);
    ScreenLayout_HideGroup(layout, 0);
    s32 r[4];
    ScreenLayout_GetGroupRect(r, layout, 0x19, -1);
    self->cover[0] = r[2] + 1;
    ScreenLayout_GetGroupRect(r, layout, 0x1A, -1);
    self->cover[1] = r[3] - 1;
    ScreenLayout_GetGroupRect(r, layout, 0x1B, -1);
    self->cover[2] = r[2] - 1;
    ScreenLayout_GetGroupRect(r, layout, 0x1C, -1);
    self->cover[3] = r[1] + 1;
    ScreenLayout_GetGroupRect(r, layout, 0x14, -1);
    self->area[0] = r[2] + 1;
    self->area[1] = 0;
    ScreenLayout_GetGroupRect(r, layout, 0x15, -1);
    self->area[2] = r[0] - 1;
    ScreenLayout_GetGroupRect(r, layout, 0x1A, -1);
    self->area[3] = r[1] + 3;
    const u8* pd = *gPlayerData;
    self->health.max = *P<s16>(self->params + 0x42);
    s16 health = *P<s16>(self->params + 0x40);
    if (health == -1)
        health = 0;
    self->health.value = health;
    self->vigour.max = *reinterpret_cast<const s16*>(pd + 0x10A);
    self->vigour.value = *reinterpret_cast<const s16*>(pd + 0x108);
    Frame_SetDurability(self, self->durability);
    s32 c[2];
    static const int kNameGroups[4] = {0x0C, 0x0E, 0x10, 0x12}, kValueGroups[4] = {0x0D, 0x0F, 0x11, 0x13};
    for (int i = 0; i < 4; i++) {
        ScreenLayout_GetGroupCenter(c, layout, kNameGroups[i], -1);
        self->names[i].x = c[0];
        self->names[i].y = c[1];
        ScreenLayout_GetGroupCenter(c, layout, kValueGroups[i], -1);
        self->values[i].x = c[0];
        self->values[i].y = c[1];
    }
    self->values[2].x -= 3;
    ScreenLayout_GetGroupCenter(c, layout, 0x16, -1);
    self->helpX = c[0];
    self->helpY = c[1];
    ScreenLayout_GetGroupCenter(self->hintAt[0], layout, 0x17, -1);
    ScreenLayout_GetGroupCenter(self->hintAt[1], layout, 0x18, -1);
    if (self->noWorld == 0) {
        self->arrows = 1;
        ScreenLayout_ShowGroup(layout, 3);
        ScreenLayout_ShowGroup(layout, 2);
    } else {
        self->arrows = 0;
        ScreenLayout_HideGroup(layout, 3);
        ScreenLayout_HideGroup(layout, 2);
    }
}

// ── 0x22E0D0 ──────────────────────────────────────────────────────────────
int Frame_Begin(StatusFrame* self) {
    self->layout = MenuRes_Make()->layout;
    Frame_Place(self);
    self->phase = 3;
    return 1;
}

// ── 0x22C990: everything written on the frame ─────────────────────────────
void Frame_DrawTexts(StatusFrame* self) {
    Font* font = *gFont;
    GsDevice* dev = *gGsDevice;
    const s32 sx = (s32)(Screen_X(0, 0) & 0xFFFF), sy = (s32)(Screen_Y(0, 0) & 0xFFFF);
    GsDevice_End2D(dev);
    Font_SetColor(font, 0x7FBCBEC8u);
    Font_DrawCentered(font, Text(0x88), sx - 0xFF0, sy + 0x230, kZ, 0.9f, 0.0f);
    Font_DrawCentered(font, Text(0x89), sx - 0xFF0, sy + 0x390, kZ, 0.9f, 0.0f);
    Font_DrawCentered(font, Text(0x8A), sx - 0xFF0, sy + 0x4C0, kZ, 0.9f, 0.0f);
    for (int i = 0; i < 4; i++) {
        TextLabel_DrawCentered(&self->names[i], 0x7FBCBEC8u, kZ);
        TextLabel_DrawCentered(&self->values[i], 0x7F100E0Fu, kZ);
    }
    GsDevice_Begin2D(dev);
    Font_SetColor(font, 0x7FBCBEC8u);
    for (int i = 0; i < 2; i++)
        if (self->hint[i] != 0)
            Font_DrawStringPlain(font, P<const u16>(self->hint[i]), sx + self->hintAt[i][0] * 0x10 - 0xA0,
                                 sy + self->hintAt[i][1] * -0x10, kZ);
    Font_SetColor(font, 0x7F100E0Fu);
    TextBox box;
    memset(&box, 0, sizeof box);
    box.text = P<WStringRep>(self->help.rep)->data;
    box.scale = 0.88f;
    box.x = (u16)Screen_X(self->helpX << 4, 0);
    box.y = (u16)Screen_Y(self->helpY * -0x10, 0);
    box.z = kZ;
    box.w = 0x1EA0;
    box.h = 800;
    box.rgba = 0x7F100E0Fu;
    box.flags = 3;
    Font_DrawBox(font, &box);
    GsDevice_Begin2D(dev);
}

// ── 0x22D100 ──────────────────────────────────────────────────────────────
void Frame_Draw(StatusFrame* self) {
    if (self->phase != 3)
        return;
    GsDevice* dev = *gGsDevice;
    if (self->layout != 0)
        ScreenLayout_Draw(P<ScreenLayout>(self->layout));
    GsDevice_Begin2D(dev);
    RunGauge_Draw(&self->health);
    RunGauge_Draw(&self->vigour);
    NotchGauge_Draw(&self->sword);
    Frame_DrawTexts(self);
    GsDevice_End2D(dev);
}

// ── 0x22CD50: the page's name plate at the top, with its two end pieces ───
void Frame_DrawTitle(StatusFrame*, int page, int alpha) {
    SpriteSet* sprites = P<SpriteSet>(MenuRes_Make()->sprites);
    SpriteSet_Bind(sprites);
    const int index = page == 2 ? 2 : page == 1 ? 1 : 0;
    const s32 sx = (s32)(Screen_X(0, 0) & 0xFFFF), sy = (s32)(Screen_Y(0, 0) & 0xFFFF);
    const u8* sprite = static_cast<const u8*>(SpriteSet_GetSprite(sprites, index));
    if (sprite == nullptr)
        return;
    u16 w16, h16;
    memcpy(&w16, sprite + 0xC, 2);
    memcpy(&h16, sprite + 0xE, 2);
    const s32 w = w16 & 0x1FFF, h = h16 & 0x1FFF;
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } draw;
    draw.flags = 1;
    draw.rgba = (u32)alpha << 24 | 0x808080;
    draw.x = sx + w * -8;
    draw.y = (sy - 0xBB0) + h * -8;
    draw.z = kZ;
    draw.w = draw.h = -1;
    SpriteSet_DrawEx(sprites, index, &draw);
    draw.x = sx + w * -8 - 0x380;
    draw.w = draw.h = -1;
    SpriteSet_DrawEx(sprites, 3, &draw);
    draw.x = sx + w * 8 - 400;
    draw.flags = 0x20;
    draw.w = draw.h = -1;
    SpriteSet_DrawEx(sprites, 3, &draw);
}

// ── 0x22CF90: the sheet drawn over the page area while pages turn ─────────
void Frame_DrawCover(StatusFrame* self, u32 alpha) {
    if (self->phase != 3)
        return;
    SpriteSet* sprites = P<SpriteSet>(MenuRes_Make()->sprites);
    SpriteSet_Bind(sprites);
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } draw;
    draw.flags = 4;
    draw.x = (s32)(Screen_X(self->cover[0] << 4, 0) & 0xFFFF);
    draw.y = (s32)(Screen_Y(self->cover[1] * -0x10, 0) & 0xFFFF);
    draw.z = 0xFFFFE6u;
    draw.rgba = alpha * 0x1000000u + 0x7F7F7Fu;
    draw.w = (self->cover[2] - self->cover[0]) * 0x10;
    draw.h = (self->cover[1] - self->cover[3]) * 0x10;
    SpriteSet_DrawEx(sprites, 4, &draw);
}

// ── 0x22D180 / 0x22D210: a page arrow jumps when its button is pressed ────
void Frame_NudgeArrow(StatusFrame* self, bool left) {
    if (self->layout == 0)
        return;
    (left ? self->leftTimer : self->rightTimer) = (s16)((s32)(*gFrameRate * 7) / 0x1E);
    const s32 offset[2] = {2, -3};
    ScreenLayout_SetGroupBOffset(P<ScreenLayout>(self->layout), offset, left ? 3 : 2);
    Sound_PlaySE(9, 0, 0x3FFF);
}

// ── 0x22D4C0: one frame; → the gauges are still running ───────────────────
int Frame_Update(StatusFrame* self) {
    self->changed = 0;
    const s32 none[2] = {0, 0};
    if (self->rightTimer > 0 && --self->rightTimer < 1)
        ScreenLayout_SetGroupBOffset(P<ScreenLayout>(self->layout), none, 2);
    if (self->leftTimer > 0 && --self->leftTimer < 1)
        ScreenLayout_SetGroupBOffset(P<ScreenLayout>(self->layout), none, 3);
    int running = 0;
    if (self->health.shown != self->health.value || self->health.shownMax != self->health.max) {
        if (!RunGauge_Step(&self->health)) {
            Frame_SetHealthText(self);
            self->changed = 1;
        } else {
            running = 1;
        }
    }
    if (self->vigour.shown != self->vigour.value || self->vigour.shownMax != self->vigour.max) {
        if (!RunGauge_Step(&self->vigour)) {
            Frame_SetVigourText(self);
            self->changed = 1;
        } else {
            running = 1;
        }
    }
    return running;
}

// ═══ The shell ═════════════════════════════════════════════════════════════

// One frame of a page (inlined everywhere a page is stepped).
void StepPage(Menu* self, Page* page) {
    self->action[0] = 0;
    self->action[1] = 0;
    if (self->frame != 0 && P<StatusFrame>(self->frame)->changed != 0)
        PageRefresh(page);
    PageUpdate(page);
}

// ── 0x2117B0: makes a page and starts it loading ──────────────────────────
void MakePage(Menu* self, int which) {
    u32* slot = nullptr;
    if (which == 2) {
        slot = &self->pages[2];
        if (*slot == 0)
            *slot = guest::addr(StatusPage_Ctor(MemAlloc(0xC0), reinterpret_cast<StatusMenu*>(self), self->info));
    } else if (which == 1) {
        slot = &self->pages[1];
        if (*slot == 0)
            *slot = guest::addr(ItemsPage_Ctor(MemAlloc(0x40), reinterpret_cast<StatusMenu*>(self), self->info,
                                               self->noWorld));
    } else if (which == 0) {
        slot = &self->pages[0];
        if (*slot == 0)
            *slot = guest::addr(SwordsPage_Ctor(MemAlloc(0x23C), reinterpret_cast<StatusMenu*>(self), self->info,
                                                self->noWorld, self->mode));
    } else {
        return;
    }
    Page* page = P<Page>(*slot);
    if (*slot != 0 && PageLoad(page) == 0) {
        PageDestroy(page);
        *slot = 0;
    }
    self->loading = *slot;
}
void MakeNextPage(Menu* self) {
    if (self->pages[0] == 0)
        MakePage(self, 0);
    else if (self->pages[1] == 0)
        MakePage(self, 1);
    else if (self->pages[2] == 0)
        MakePage(self, 2);
    else
        self->loading = 0;
}
inline u32 PageAt(const Menu* self, int index) { return index >= 0 && index <= 2 ? self->pages[index] : 0; }

// ── 0x2110D0: a turn in progress ──────────────────────────────────────────
void StepTurn(Menu* self) {
    self->turnTimer++;
    if (TurnFrames() < self->turnTimer) {
        self->turning = 0;
        self->from = -1;
        self->to = -1;
    }
    if (self->turnTimer != (TurnFrames() >> 1))
        return;
    // Half way, under the cover: whatever is still loading is finished now.
    while (self->loading != 0) {
        Page* page = P<Page>(self->loading);
        if (page->loaded == 0)
            StepPage(self, page);
        else
            MakeNextPage(self);
    }
    self->page = self->to;
    self->current = PageAt(self, self->to);
    if (self->current != 0) {
        PageEnter(P<Page>(self->current));
        if (self->turning == 2)
            SwordsPage_Select(P(self->pages[0]), self->jumpSword);
        else if (self->turning == 3)
            ItemsPage_Reset(P(self->pages[1]));
    }
    self->jumpSword = 0;
}

void* Menu_Dtor(Menu* self, int flag);

}  // namespace

extern "C" {

// The numbers pages read off the frame and the calls they make on it.
void StatusFrame_SetHelp(void* frame, const u16* text) {                    // 0x22D460
    StatusFrame* self = static_cast<StatusFrame*>(frame);
    int n = 0;
    while (text[n] != 0)
        n++;
    WString_Assign(&self->help, text, text + n);
}
void StatusFrame_SetHints(void* frame, int which) {                         // 0x22D350 / 0x22D3C0 / 0x22D430 / 0x22D450
    StatusFrame* self = static_cast<StatusFrame*>(frame);
    switch (which) {
    case 0: self->hint[0] = self->hintDefault[0]; self->hint[1] = self->hintDefault[1]; break;
    case 1: self->hint[0] = 0; self->hint[1] = guest::addr(Text(0x162)); break;
    case 2: self->hint[0] = 0; self->hint[1] = guest::addr(Text(0x17)); break;
    default: self->hint[0] = 0; self->hint[1] = 0; break;
    }
}
void StatusFrame_ShowArrows(void* frame, int on) {                          // 0x22D2F0 / 0x22D2A0
    StatusFrame* self = static_cast<StatusFrame*>(frame);
    if (self->layout == 0)
        return;
    if (on != 0) {
        if (self->noWorld != 0)
            return;
        self->arrows = 1;
        ScreenLayout_ShowGroup(P<ScreenLayout>(self->layout), 3);
        ScreenLayout_ShowGroup(P<ScreenLayout>(self->layout), 2);
    } else {
        self->arrows = 0;
        ScreenLayout_HideGroup(P<ScreenLayout>(self->layout), 3);
        ScreenLayout_HideGroup(P<ScreenLayout>(self->layout), 2);
    }
}
void StatusFrame_SetDurability(void* frame, int durability) {               // 0x22D630
    Frame_SetDurability(static_cast<StatusFrame*>(frame), durability);
}
// 0x22DAB0 / 0x22DAD0 / 0x22D8F0 / 0x22D900: the gauges take the new values and run to them.
void StatusFrame_TakeHealth(void* frame) {
    StatusFrame* self = static_cast<StatusFrame*>(frame);
    s16 health = *P<s16>(self->params + 0x40);
    self->health.value = health == -1 ? (s16)0 : health;
    self->health.max = *P<s16>(self->params + 0x42);
}
void StatusFrame_TakeVigour(void* frame) {
    StatusFrame* self = static_cast<StatusFrame*>(frame);
    self->vigour.value = *reinterpret_cast<const s16*>(*gPlayerData + 0x108);
    self->vigour.max = *reinterpret_cast<const s16*>(*gPlayerData + 0x10A);
}
void StatusFrame_RefreshTexts(void* frame) { Frame_SetTexts(static_cast<StatusFrame*>(frame)); }    // 0x22DAE0

// ── 0x22C670: the shared resources, a step at a time (at boot) ────────────
int MenuRes_Poll() {
    MenuRes* res = MenuRes_Make();
    switch (res->state) {
    case 1:
        if (SpriteSet_IsLoaded(P<SpriteSet>(res->sprites)) != 0) {
            res->state = 2;
            ScreenLayout* layout = ScreenLayout_Ctor(static_cast<ScreenLayout*>(MemAlloc(0x30)));
            res->layout = guest::addr(layout);
            layout->id = 1;
        }
        break;
    case 2:
        if (ScreenLayout_Poll(P<ScreenLayout>(res->layout))) {
            res->state = 3;
            res->pack = guest::addr(AssetRef_Ctor(MemAlloc(0xC)));
            AssetQueue_Request(P(res->pack), 0x4C);
        }
        break;
    case 3:
        if (const u8* pack = static_cast<const u8*>(AssetRef_GetData(P(res->pack)))) {
            const s32* at = reinterpret_cast<const s32*>(pack);
            res->weaponSprites = guest::addr(SpriteSet_Ctor(MemAlloc(0x14)));
            SpriteSet_ParseSized(P<SpriteSet>(res->weaponSprites), pack + at[4], (u32)at[5]);
            res->weaponParams = guest::addr(ParamFile_Ctor(static_cast<ParamFile*>(MemAlloc(0x4C))));
            ParamFile_SetData(P<ParamFile>(res->weaponParams), reinterpret_cast<const char*>(pack + at[6]), at[7]);
            res->state = 4;
            res->itemSprites = guest::addr(SpriteSet_Ctor(MemAlloc(0x14)));
            SpriteSet_Load(P<SpriteSet>(res->itemSprites), 0x4F, 0);
        }
        break;
    case 4:
        if (SpriteSet_IsLoaded(P<SpriteSet>(res->itemSprites)) != 0) {
            res->state = 5;
            ScreenLayout* layout = ScreenLayout_Ctor(static_cast<ScreenLayout*>(MemAlloc(0x30)));
            res->itemLayout = guest::addr(layout);
            layout->id = 4;
        }
        break;
    case 5:
        if (ScreenLayout_Poll(P<ScreenLayout>(res->itemLayout)))
            res->state = 6;
        break;
    }
    return res->state == 6;
}
void* MenuRes_Get() { return MenuRes_Make(); }

// ── 0x211DE0 ──────────────────────────────────────────────────────────────
StatusMenu* StatusMenu_Ctor(StatusMenu* mem) {
    Menu* self = reinterpret_cast<Menu*>(mem);
    memset(self, 0, sizeof *self);
    self->vtable = 0x2EC270u;
    self->from = -1;
    self->to = -1;
    return mem;
}

// ── 0x211A70 ──────────────────────────────────────────────────────────────
void StatusMenu_Start(StatusMenu* mem, const void* info, int page, int resume, int noWorld, int handOver, int mode) {
    Menu* self = reinterpret_cast<Menu*>(mem);
    memcpy(self->info, info, sizeof self->info);
    self->page = page;
    self->noWorld = (u8)noWorld;
    if (self->frame == 0)
        self->frame = guest::addr(Frame_Ctor(static_cast<StatusFrame*>(MemAlloc(sizeof(StatusFrame))),
                                             self->info[1], resume, (u8)handOver));
    if (self->frame != 0) {
        StatusFrame* frame = P<StatusFrame>(self->frame);
        Frame_Begin(frame);
        const int readied = PlayerData_GetReadiedSword(*gPlayerData);
        Frame_SetDurability(frame, *P<s32>(self->info[0] + (u32)readied * 0x100 + 0x60));
    }
    self->state = 1;
    self->mode = mode;
    if (mode == 1)
        StatusFrame_SetHints(P(self->frame), 1);
    self->icons = guest::addr(IconSet_Ctor(static_cast<IconSet*>(MemAlloc(0x20))));
}

// ── 0x212310 (vtable +0x08) → 0x211C10 ──────────────────────────────────────
void* Menu_DtorImpl(Menu* self, int flag) {
    if (self == nullptr)
        return self;
    while (self->loading != 0) {
        Page* page = P<Page>(self->loading);
        if (page->loaded == 0)
            StepPage(self, page);
        else
            MakeNextPage(self);
    }
    if (self->frame != 0)
        VirtualCall<void*, void*, int>(P<StatusFrame>(self->frame)->vtable, 8, P(self->frame), 1);
    self->frame = 0;
    for (int i = 0; i < 3; i++) {
        if (self->pages[i] != 0)
            PageDestroy(P<Page>(self->pages[i]));
        self->pages[i] = 0;
    }
    if (self->icons != 0)
        IconSet_Dtor(P<IconSet>(self->icons), 1);
    self->icons = 0;
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

// ── 0x211930: L1 / R1 ─────────────────────────────────────────────────────
void StatusMenu_SetPage(StatusMenu* mem, int page) {
    Menu* self = reinterpret_cast<Menu*>(mem);
    if (self->state <= 2)
        return;
    if (self->turning == 1) {
        if (self->turnTimer <= (TurnFrames() >> 1))
            return;
    } else if (self->turning != 0) {
        return;
    }
    StatusFrame* frame = P<StatusFrame>(self->frame);
    if (self->page == (page + 1) % 3)
        Frame_NudgeArrow(frame, true);
    else if (self->page == (page + 2) % 3)
        Frame_NudgeArrow(frame, false);
    self->turnTimer = 0;
    self->from = self->page;
    self->to = page;
    self->turning = 1;
}

// ── 0x211350: → what the scene should do, or null ─────────────────────────
s32* StatusMenu_Update(StatusMenu* mem) {
    Menu* self = reinterpret_cast<Menu*>(mem);
    if (self->state != 4) {
        if (self->state == 3) {
            if (self->loading == 0) {
                self->state = 4;
            } else {
                StepPage(self, P<Page>(self->loading));
                if (P<Page>(self->loading)->loaded != 0)
                    MakeNextPage(self);
            }
        } else if (self->state == 2) {
            if (self->current != 0) {
                Page* page = P<Page>(self->current);
                StepPage(self, page);
                if (page->loaded != 0) {
                    self->state = 3;
                    PageEnter(page);
                    MakeNextPage(self);
                }
            }
            return nullptr;
        } else if (self->state == 1) {
            if (self->frame != 0 && P<StatusFrame>(self->frame)->phase == 3) {
                self->state = 2;
                MakePage(self, self->page);
                self->current = PageAt(self, self->page);
            }
            return nullptr;
        }
    }
    if (self->frame != 0 && Frame_Update(P<StatusFrame>(self->frame)) != 0)
        return nullptr;
    if (self->current == 0) {
        self->action[0] = 1;
        return self->action;
    }
    StepPage(self, P<Page>(self->current));
    s32* result = self->action[0] != 0 ? self->action : nullptr;
    if (self->turning != 0)
        StepTurn(self);
    if (result != nullptr) {
        if (result[0] == 8) {
            self->to = 1;
            self->from = self->page;
            self->turnTimer = 0;
            self->turning = 3;
            result = nullptr;
        } else if (result[0] == 7) {
            self->jumpSword = result[1];
            self->to = 0;
            self->from = self->page;
            self->turnTimer = 0;
            self->turning = 2;
            result = nullptr;
        }
    }
    return result;
}

// ── 0x210E20 ──────────────────────────────────────────────────────────────
void StatusMenu_Draw(StatusMenu* mem) {
    Menu* self = reinterpret_cast<Menu*>(mem);
    GsDevice* dev = *gGsDevice;
    GsDevice_Begin2D(dev);
    StatusFrame* frame = P<StatusFrame>(self->frame);
    if (self->frame != 0)
        Frame_Draw(frame);
    if (self->current != 0)
        PageDraw(P<Page>(self->current));
    if (self->turning == 0) {
        Frame_DrawTitle(frame, self->page, 0x80);
    } else if (self->frame != 0) {
        const float t = (float)self->turnTimer / (float)TurnFrames();
        float away = t - 0.5f;
        if (away < 0.0f)
            away = -away;
        Frame_DrawCover(frame, (u32)((1.0f - away * 2.0f) * 128.0f));
        const u32 a = (u32)(t * 128.0f) & 0xFF;
        Frame_DrawTitle(frame, self->from, (int)((0x80 - a) & 0xFF));
        Frame_DrawTitle(frame, self->to, (int)a);
    }
    GsDevice_End2D(dev);
}

// ── 0x211080 / 0x2110B0: did the swords page ready another sword ──────────
int StatusMenu_SwordChanged(StatusMenu* mem) {
    const Menu* self = reinterpret_cast<const Menu*>(mem);
    if (self->pages[0] == 0)
        return 0;
    const u8* page = P<u8>(self->pages[0]);
    return page[0x209] != page[0x20A];
}
int StatusMenu_GetSword(StatusMenu* mem) {
    const Menu* self = reinterpret_cast<const Menu*>(mem);
    return self->pages[0] == 0 ? 0xFF : P<u8>(self->pages[0])[0x209];
}

}  // extern "C"

namespace {
void* Menu_Dtor(Menu* self, int flag) { return Menu_DtorImpl(self, flag); }
GUEST_BIND(0x212310, Menu_Dtor);
}  // namespace
