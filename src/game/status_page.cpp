// The status menu's third page, "Status Check" (0x212380–0x213D20, 0xC0
// bytes, vtable 0x2EC290): a map of the area drawn from the collision file's
// spans, with a dot for everyone in it (coloured by faction) and an arrow for
// the player; beside it the date, where this is, the sword in hand and what
// the player is doing.  The d-pad scrolls the map, L2 / R2 zoom, Triangle
// leaves, Start goes back to the pause menu.
//
// The map: the collision file ends with `count` pairs {flags, cells}: runs
// of cells of one kind, row after row (Collision+0x10 / +0x1C).  Flags: 1
// ground, 2 wall, 4 / 8 two kinds of door or exit; −1 ground.

#include "engine/containers.h"
#include "engine/font.h"
#include "engine/pad.h"
#include "engine/screen_layout.h"
#include "engine/text_label.h"
#include "game/chara_resources.h"
#include "game/gameplay_scene.h"
#include "game/map_collision.h"
#include "hal/ps2_types.h"
#include "native/guest.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    void* PadMgr_Get();                                                 // 0x13A920
    u16   Pad_GetStickHeld(void* mgr, int port);                        // 0x139F90
    void  GsDevice_DrawPrim(void* dev, const void* packet, int count);  // 0x12FC30
    void  Gs_FillRect(const s32* rect, u32 z, u32 rgba);                // 0x225CD0
    u32   Screen_X(int v, int align);                                   // 0x12E5E0
    u32   Screen_Y(int v, int align);                                   // 0x12E550
    void* MenuRes_Get();                                                // 0x22C8F0
    void  StatusFrame_SetHelp(void* frame, const u16* text);            // 0x22D460
    void  StatusFrame_SetHints(void* frame, int which);
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<void> gGsDevice{0x2EE670u};
const GuestPtrVar<Font> gFont{0x2EE67Cu};
const GuestPtrVar<u8>   gPlayerData{0x2EE688u};
u32* const gFrameRate = guest::ptr<u32>(0x2EDA00u);

inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }

struct Marker { s32 x, y; u32 rgba; };

struct StatusPage {
    u32 menu;           // +0x00
    s16 loaded;         // +0x04
    s16 _06;
    u32 vtable;         // +0x08
    u32 layout;         // +0x0C  layout 4 (AreaMapMenu)
    u32 swords;         // +0x10  the carried swords (their names)
    u32 header;         // +0x14  ColHeader*
    u32 spans;          // +0x18  {u32 flags, s32 cells}[]
    s32 spanCount;      // +0x1C
    s32 width, height;  // +0x20  cells
    s32 viewX, viewY;   // +0x28  the window on the map, layout pixels (y up)
    s32 viewW, viewH;   // +0x30
    s32 scrollX, scrollY;   // +0x38
    u8  fitsX, fitsY;   // +0x40  the whole map fits the window that way
    s16 cell;           // +0x42  a cell's size in 1/16 pixel (8 … 0x80)
    s32 playerX, playerY;   // +0x44  the player's cell
    float arrow[6];     // +0x4C  the arrow's three corners, in cells
    GuestList pulses;   // +0x64  ages of the rings round the arrow
    s32 pulseTimer;     // +0x70
    u32 markerCapacity; // +0x74  std::vector<Marker>
    u32 markerCount;    // +0x78
    u32 markers;        // +0x7C
    s32 hasTarget;      // +0x80
    s32 targetX, targetY;   // +0x84
    WString line1;      // +0x8C  the date
    u32 line1Vt;
    WString line2;      // +0x9C  the area
    u32 line2Vt;
    WString line3;      // +0xAC  the place
    u32 line3Vt;
    u32 title;          // +0xBC  what the player is doing (the help line)
};
static_assert(sizeof(StatusPage) == 0xC0 && offsetof(StatusPage, pulses) == 0x64 && offsetof(StatusPage, line1) == 0x8C,
              "StatusPage");

inline u32 FrameOf(const StatusPage* self) { return P<u32>(self->menu)[1]; }
inline void SetAction(StatusPage* self, s32 what) { P<s32>(self->menu)[7] = what; }

void Assign(WString* s, const u16* text) {
    int n = 0;
    while (text[n] != 0)
        n++;
    WString_Assign(s, text, text + n);
}
// 0x212600 → 0x199870: text onto the end of a string.
void Append(WString* s, const u16* text) {
    const WStringRep* rep = P<WStringRep>(s->rep);
    static u32 buffer = 0;
    if (buffer == 0)
        buffer = guest::addr(guest::alloc(0x400));
    u16* out = P<u16>(buffer);
    int n = 0;
    for (const u16* c = P<const u16>(rep->data); *c != 0 && n < 0x1F0; c++)
        out[n++] = *c;
    for (; text != nullptr && *text != 0 && n < 0x1FE; text++)
        out[n++] = *text;
    out[n] = 0;
    WString_Assign(s, out, out + n);
}
inline const u16* Chars(const WString* s) { return P<const u16>(P<WStringRep>(s->rep)->data); }

// ── 0x2131F0: keeps the window on the map (or the map centred in it) ──────
void Clamp(StatusPage* self) {
    self->fitsX = 0;
    self->fitsY = 0;
    const s32 mapW = self->width * self->cell >> 4, mapH = self->height * self->cell >> 4;
    if (mapW <= self->viewW) {
        self->fitsX = 1;
        self->scrollX = -(self->viewW - mapW) >> 1;
    }
    if (mapH <= self->viewH) {
        self->fitsY = 1;
        self->scrollY = -(self->viewH - mapH) >> 1;
    }
    if (self->fitsX == 0) {
        if (self->scrollX < 0)
            self->scrollX = 0;
        else if (mapW <= self->scrollX + self->viewW)
            self->scrollX = mapW - self->viewW - 1;
    }
    if (self->fitsY == 0) {
        if (self->scrollY < 0)
            self->scrollY = 0;
        else if (mapH <= self->scrollY + self->viewH)
            self->scrollY = mapH - self->viewH - 1;
    }
}

// ── 0x213510: the player's cell and the arrow that shows the way it faces ─
void SetPlayer(StatusPage* self, const float* pos, const float* dir) {
    if (self->header == 0)
        return;
    float degrees = std::acos(-dir[0]) * 57.295776f;
    if (dir[2] < 0.0f)
        degrees = (90.0f - degrees) + 90.0f;
    const float a = degrees * 3.1415927f / 180.0f;
    const float c = std::cos(a), s = std::sin(a);
    const ColHeader* h = P<ColHeader>(self->header);
    self->playerX = (s32)(pos[0] * 100.0f / h->cellSize) + h->offsetX;
    self->playerY = (s32)(pos[2] * 100.0f / h->cellSize) + h->offsetZ;
    self->arrow[0] = s * -5.5f + c * 0.0f;
    self->arrow[1] = -(c * -5.5f + -s * 0.0f);
    self->arrow[2] = s * 2.5f + c * 2.5f;
    self->arrow[3] = -(c * 2.5f + -s * 2.5f);
    self->arrow[4] = s * 2.5f + c * -2.5f;
    self->arrow[5] = -(c * 2.5f + -s * -2.5f);
}

// ── 0x213380 (→ 0x1D5130): a dot for everyone of a faction who is about ───
void AddMarkers(StatusPage* self, int faction, u32 rgba) {
    const ColHeader* h = P<ColHeader>(self->header);
    CharaResources* res = CharaResources_Get();
    for (GuestListNode* n = GuestList_Begin(&res->params); n != GuestList_End(&res->params); n = GuestList_Next(n)) {
        const u32 paramsAddr = P<CharaParamsRef>(n->value)->params;
        const CharaParams* params = P<CharaParams>(paramsAddr);
        if (faction != -1 && params->faction != faction)
            continue;
        const u32 active = *P<u32>(paramsAddr + 0xC4);
        if (active == 0 || *P<s32>(active) == 0)
            continue;
        const float* pos = P<float>(paramsAddr + 0x80);
        if (!(pos[1] > -50.0f))
            continue;
        if (self->markerCount == self->markerCapacity) {        // (0x2142B0: the vector grows)
            const u32 capacity = self->markerCapacity != 0 ? self->markerCapacity * 2 : 1;
            Marker* grown = static_cast<Marker*>(MemAlloc(capacity * sizeof(Marker)));
            if (self->markers != 0) {
                memcpy(grown, P(self->markers), self->markerCount * sizeof(Marker));
                MemFree(P(self->markers));
            }
            self->markers = guest::addr(grown);
            self->markerCapacity = capacity;
        }
        Marker* m = P<Marker>(self->markers) + self->markerCount++;
        m->x = (s32)(pos[0] * 100.0f / h->cellSize) + h->offsetX;
        m->y = (s32)(pos[2] * 100.0f / h->cellSize) + h->offsetZ;
        m->rgba = rgba;
    }
}

// ── 0x213890 ──────────────────────────────────────────────────────────────
void SetTexts(StatusPage* self, const u32* info) {
    const u8* pd = *gPlayerData;
    Assign(&self->line1, Text(0x9F));
    Append(&self->line1, Text((s8)pd[0x100] + 0x23));
    Append(&self->line1, Text(1));
    Append(&self->line1, Text((s8)pd[0x101] + 0x1E));
    Assign(&self->line2, Text(0xA0));
    if (info[10] != 0)
        Append(&self->line2, P<const u16>(info[10]));
    Assign(&self->line3, Text(0xA2));
    if (info[11] != 0)
        Append(&self->line3, P<const u16>(info[11]));
    self->title = info[9];
}

// ── 0x225820: a triangle with a colour at each corner ─────────────────────
void FillTriangle(const s32* xy, const u32* colours, u32 z) {
    struct V { u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 prim; V v[3]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.prim = 0x4B;
    for (int i = 0; i < 3; i++)
        packet.v[i] = {colours[i], 1.0f, (u16)xy[i], (u16)xy[3 + i], z};
    GsDevice_DrawPrim(*gGsDevice, &packet, 3);
}

// ── 0x214120 ──────────────────────────────────────────────────────────────
void DrawArrow(const StatusPage* self, float strength, s32 x, s32 y, s32 size) {
    s32 xy[6];
    for (int i = 0; i < 3; i++) {
        xy[i] = x + (s32)self->arrow[i * 2] * size;
        xy[3 + i] = y + (s32)self->arrow[i * 2 + 1] * size;
    }
    const u32 alpha = (u32)(s32)(strength * 128.0f) << 24;
    const u32 colours[3] = {alpha | 0xFF4040u, alpha | 0x80DD60u, alpha | 0x80DD60u};
    for (s32& v : xy)
        v = v < 0 ? 0 : v > 0xFFF0 ? 0xFFF0 : v;
    FillTriangle(xy, colours, 0xFFFF81u);
}

inline s32 OriginX(const StatusPage* self) {
    return self->viewX * 0x10 + (s32)(Screen_X(0, 0) & 0xFFFF) + self->scrollX * -0x10;
}
inline s32 OriginY(const StatusPage* self) {
    return (s32)(Screen_Y(0, 0) & 0xFFFF) + self->viewY * -0x10 + self->scrollY * -0x10;
}

// ── 0x212820: the map's spans; pass 0 the ground, pass 1 everything else ──
void DrawSpans(const StatusPage* self, int pass) {
    if (self->header == 0 || self->spans == 0)
        return;
    const u32* span = P<u32>(self->spans);
    const s32 x0 = OriginX(self);
    const s32 sy = (s32)(Screen_Y(0, 0) & 0xFFFF);
    const s32 top = sy + self->viewY * -0x10 - self->cell;
    const s32 bottom = self->viewH * 0x10 + sy + self->viewY * -0x10;
    const s32 left = self->viewX * 0x10 + (s32)(Screen_X(0, 0) & 0xFFFF);
    const s32 right = left + self->viewW * 0x10;
    const s32 rowEnd = x0 + self->width * self->cell;
    s32 x = x0, y = OriginY(self);
    for (s32 i = 0; i < self->spanCount; i++, span += 2) {
        const s32 w = self->cell * (s32)span[1];
        if (bottom < y)
            return;
        if (top <= y && left <= x + w && x <= right) {
            s32 rect[4] = {x < 0 ? 0 : x, y, w, self->cell};
            if (x + w > 0xFFFE)
                rect[2] = 0xFFFE - x;
            const u32 flags = span[0];
            u32 colour = (flags & 1) ? 0x802A2832u : (flags & 2) ? 0x804D4A55u : 0x80000000u;
            if (flags != 0xFFFFFFFFu) {
                if (flags & 4)
                    colour = 0x80202034u;
                else if (flags & 8)
                    colour = 0x80302022u;
            }
            if ((pass == 0) == (colour == 0x802A2832u))
                Gs_FillRect(rect, 0xFFFF81u, colour);
        }
        x += w;
        if (rowEnd <= x) {
            y += self->cell;
            x = x0;
        }
    }
}

// ── 0x212640: the dots ────────────────────────────────────────────────────
void DrawMarkers(const StatusPage* self) {
    const s32 ox = OriginX(self), oy = OriginY(self);
    const Marker* m = P<Marker>(self->markers);
    for (u32 i = 0; i < self->markerCount; i++, m++) {
        const s32 size = self->cell * 4 + self->cell;
        s32 rect[4] = {ox + self->cell * m->x - (size >> 1), oy + self->cell * m->y - (size >> 1), size, size};
        if (rect[0] < 0)
            rect[0] = 0;
        if (rect[1] < 0)
            rect[1] = 0;
        if (rect[0] + size > 0xFFF7F)
            rect[2] = 0xFFF80 - rect[0];
        if (rect[1] + size > 0xFFF7F)
            rect[3] = 0xFFF80 - rect[1];
        Gs_FillRect(rect, 0xFFFF81u, m->rgba);
    }
}

// ── 0x212380: the four lines beside the map ───────────────────────────────
void DrawTexts(StatusPage* self) {
    Font* font = *gFont;
    const u32 swordsPage = P<u32>(self->menu)[2];
    const s32 x = (s32)(Screen_X(0, 0) & 0xFFFF) - 0xFC0, sy = (s32)(Screen_Y(0, 0) & 0xFFFF);
    Font_SetColor(font, 0x7F100E0Fu);
    Font_DrawString(font, Chars(&self->line1), x, sy - 0x8B0, 0xFFFF82u, 0.9f, 0.0f);
    Font_DrawString(font, Chars(&self->line2), x, sy - 0x650, 0xFFFF82u, 0.9f, 0.0f);
    static u32 buffer = 0;
    if (buffer == 0)
        buffer = guest::addr(guest::alloc(0x200));
    u16* line = P<u16>(buffer);
    int n = 0;
    for (const u16* c = Text(0xA1); *c != 0 && n < 0x7F; c++)
        line[n++] = *c;
    if (swordsPage != 0)        // the name of the sword now in hand, as the swords page has it
        for (const u16* c = P<const u16>(self->swords + (u32)(s8)P<u8>(swordsPage)[0x209] * 0x100);
             *c != 0 && n < 0xFE; c++)
            line[n++] = *c;
    line[n] = 0;
    Font_DrawString(font, line, x, sy - 0x3F0, 0xFFFF82u, 0.9f, 0.0f);
    Font_DrawString(font, Chars(&self->line3), x, sy - 400, 0xFFFF82u, 0.9f, 0.0f);
}

// ── 0x212B20 (vtable +0x14) ───────────────────────────────────────────────
void StatusPage_Draw(StatusPage* self) {
    if (self->layout == 0 || P<ScreenLayout>(self->layout)->ready == 0)
        return;
    ScreenLayout_Draw(P<ScreenLayout>(self->layout));
    DrawSpans(self, 0);
    DrawMarkers(self);
    if (self->header != 0) {
        const s32 size = (u16)self->cell;
        const s32 x = OriginX(self) + self->playerX * size, y = OriginY(self) + self->playerY * size;
        DrawArrow(self, 1.0f, x, y, size);
        for (GuestListNode* n = GuestList_Begin(&self->pulses); n != GuestList_End(&self->pulses);
             n = GuestList_Next(n)) {
            const float grown = (float)*gFrameRate * (float)((s32)n->value / 0x1E) / 30.0f;
            DrawArrow(self, 1.0f - grown, x, y, (s32)(grown * (float)size * 3.0f));
        }
    }
    DrawSpans(self, 1);
    DrawTexts(self);
}

// ── 0x212D40: the d-pad (or stick) moves the window ───────────────────────
void Scroll(StatusPage* self) {
    const u16 held = Pad_GetHeld(PadMgr_Get(), 0) | Pad_GetStickHeld(PadMgr_Get(), 0);
    const s32 step = 150 / (s32)*gFrameRate;            // (0x2EE734)
    if (self->fitsY == 0) {
        if (held & kPadUp) {
            self->scrollY -= step;
            if (self->scrollY < 0)
                self->scrollY = 0;
        } else if (held & kPadDown) {
            self->scrollY += step;
            const s32 mapH = self->height * self->cell >> 4;
            if (mapH <= self->scrollY + self->viewH)
                self->scrollY = mapH - self->viewH - 1;
        }
    }
    if (self->fitsX == 0) {
        if (held & kPadRight) {
            self->scrollX += step;
            const s32 mapW = self->width * self->cell >> 4;
            if (mapW <= self->scrollX + self->viewW)
                self->scrollX = mapW - self->viewW - 1;
        } else if (held & kPadLeft) {
            self->scrollX -= step;
            if (self->scrollX < 0)
                self->scrollX = 0;
        }
    }
}

// ── 0x212EB0 (vtable +0x18) ───────────────────────────────────────────────
void StatusPage_Update(StatusPage* self) {
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    if (pressed & kPadTriangle) {
        SetAction(self, 2);
        return;
    }
    if (pressed & kPadStart) {
        SetAction(self, 1);
        return;
    }
    const s32 rate = (s32)*gFrameRate;
    for (GuestListNode* n = GuestList_Begin(&self->pulses); n != GuestList_End(&self->pulses);) {
        n->value++;
        if (rate * 0x1E / 0x1E < (s32)n->value)
            n = GuestList_Erase(&self->pulses, n);
        else
            n = GuestList_Next(n);
    }
    if (rate * 0x28 / 0x1E < ++self->pulseTimer)
        self->pulseTimer = 0;
    if (self->pulseTimer == 0 || self->pulseTimer == rate * 10 / 0x1E)
        GuestList_Insert(&self->pulses, GuestList_End(&self->pulses), 0);
    if (pressed & kPadR2) {
        const s16 was = self->cell;
        self->cell = (s16)(was * 2);
        if (self->cell < 0x81) {
            const float k = (float)self->cell / (float)was;
            self->scrollX = (self->viewW >> 1) + (s32)(k * (float)self->scrollX);
            self->scrollY = (self->viewH >> 1) + (s32)(k * (float)self->scrollY);
            Clamp(self);
        } else {
            self->cell = 0x80;
        }
    } else if (pressed & kPadL2) {
        const s16 was = self->cell;
        self->cell = (s16)(was >> 1);
        if (self->cell < 8) {
            self->cell = 8;
        } else {
            const float k = (float)self->cell / (float)was;
            self->scrollX = (s32)(k * (float)self->scrollX) - (self->viewW >> 2);
            self->scrollY = (s32)(k * (float)self->scrollY) - (self->viewH >> 2);
            Clamp(self);
        }
    }
    Scroll(self);
}

// ── 0x213330 (vtable +0x10): the help line and hints are this page's ──────
void StatusPage_Enter(StatusPage* self) {
    const u32 frame = FrameOf(self);
    if (frame != 0) {
        StatusFrame_SetHelp(P(frame), self->title == 0 ? Text(1) : P<const u16>(self->title));
        StatusFrame_SetHints(P(frame), 0);
    }
}

char StatusPage_Load(StatusPage*) { return 1; }         // 0x193E10 (the base's)
void StatusPage_Refresh(StatusPage*) {}                 // 0x192650

// ── 0x213A90 (vtable +0x08) ───────────────────────────────────────────────
void* StatusPage_Dtor(StatusPage* self, int flag) {
    if (self == nullptr)
        return self;
    self->vtable = 0x2EC290u;
    WString_Release(&self->line3);
    WString_Release(&self->line2);
    WString_Release(&self->line1);
    if (self->markers != 0)
        MemFree(P(self->markers));
    GuestList_Clear(&self->pulses);
    self->vtable = 0x2EB2D0u;
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

GUEST_BIND(0x213A90, StatusPage_Dtor);
GUEST_BIND(0x193E10, StatusPage_Load);
GUEST_BIND(0x213330, StatusPage_Enter);
GUEST_BIND(0x212B20, StatusPage_Draw);
GUEST_BIND(0x212EB0, StatusPage_Update);
GUEST_BIND(0x192650, StatusPage_Refresh);

}  // namespace

extern "C" {

// ── 0x213D20 ──────────────────────────────────────────────────────────────
void* StatusPage_Ctor(void* mem, StatusMenu* menu, void* infoBlock) {
    StatusPage* self = static_cast<StatusPage*>(mem);
    const u32* info = static_cast<const u32*>(infoBlock);
    memset(self, 0, sizeof *self);
    self->menu = guest::addr(menu);
    self->vtable = 0x2EC290u;
    GuestList_Init(&self->pulses);
    self->line1Vt = self->line2Vt = self->line3Vt = 0x2EAF30u;
    for (WString* s : {&self->line1, &self->line2, &self->line3}) {
        WString_Ctor(s);
        WString_Reserve(s, 0);
    }
    self->cell = 0x10;
    self->swords = info[0];
    const u32 map = info[6];
    if (map == 0) {
        self->width = 1;
        self->height = 1;
    } else {
        self->header = guest::addr(Map_Header(P<MapCollision>(map)));
        const u32 collision = P<MapCollision>(map)->collision;
        self->spans = collision != 0 ? P<Collision>(collision)->extra : 0;
        self->spanCount = collision != 0 ? P<Collision>(collision)->extraCount : 0;
        if (self->header != 0) {
            const ColHeader* h = P<ColHeader>(self->header);
            self->width = h->width;
            self->height = h->cellCount / h->width;
        }
    }
    SetTexts(self, info);
    self->layout = static_cast<const u32*>(MenuRes_Get())[6];
    ScreenLayout* layout = P<ScreenLayout>(self->layout);
    ScreenLayout_SetDepth(layout, 0xFFFF82);
    ScreenLayout_SetGroupDepth(layout, 0xFFFF80u, 0);       // (0x168080: the item of group 0)
    ScreenLayout_HideGroup(layout, 0);
    s32 r[4];
    ScreenLayout_GetGroupRect(r, layout, 0, -1);
    self->viewX = r[0];
    self->viewY = r[1];
    self->viewW = r[2] - r[0];
    self->viewH = r[1] - r[3];
    self->scrollX = self->playerX - (self->viewW >> 1);
    self->scrollY = self->playerY - (self->viewH >> 1);
    Clamp(self);
    self->loaded = 1;
    if (info[2] != 0 && info[3] != 0)
        SetPlayer(self, P<float>(info[2]), P<float>(info[3]));
    if (info[4] == 0) {
        self->hasTarget = 0;
    } else if (self->header != 0) {
        self->hasTarget = 1;
        const float* at = P<float>(info[4]);
        const ColHeader* h = P<ColHeader>(self->header);
        self->targetX = (s32)(at[0] * 100.0f / h->cellSize) + h->offsetX - 0x3C;
        self->targetY = -((s32)(at[2] * 100.0f / h->cellSize) + h->offsetZ);
    }
    if (map != 0 && self->header != 0) {
        AddMarkers(self, 0, 0x7F004000u);
        AddMarkers(self, 1, 0x7F00407Fu);
        AddMarkers(self, 2, 0x7F004040u);
    }
    return self;
}

}  // extern "C"
