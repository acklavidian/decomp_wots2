// "Check moves": the chart of a sword's techniques (0x234460–0x2379C0;
// GameplayScene mode 10 and the character setup's sub-screen 6).  The chart
// itself comes from the skill sheets (skill_sheets.cpp, slot 4); this draws
// it on layout 5 (WeaponSheet), whose pieces it places itself:
//   group 0 / 11 the header bar's end and middle, 1 its pattern, 2 the stamp
//   shown when every technique is known, 3 a cell's ground, 4 the tag for a
//   technique's kind, 5 / 6 the top of a cell's border (lit / unlit), 7 the
//   unlit bottom, 8 / 9 / 10 the lit bottom by the technique's frame.
// Layout items of groupB 0 are the paper behind, 2 the bars above and below,
// 1 those pieces (never drawn where the layout has them).
//
// A cell is 193 × 76 pixels; the d-pad (or the stick) scrolls the chart when
// it is wider than three columns or taller than four rows, faster with a
// face button or R1 held.  Cells light up as the sword learns them.

#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/screen_layout.h"
#include "game/skill_sheets.h"
#include "game/sword.h"
#include "native/guest.h"

#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                   // 0x100820
    void  MemFree(void* p);                     // 0x100650
    u16   Pad_GetStickHeld(void* mgr, int port);
}

namespace {

const GuestPtrVar<Font> gFont{0x2EE67Cu};
const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
const GuestPtrVar<u8> gPlayerData{0x2EE688u};

struct SwordInfo {              // 0x14 bytes
    u32 sheets;                 // +0x00
    u32 layout;                 // +0x04
    u32 sword;                  // +0x08
    u8  active;                 // +0x0C
    u8  ready;                  // +0x0D
    s16 scrollX;                // +0x0E
    s16 scrollY;                // +0x10
    s16 speed;                  // +0x12
};

inline int Screen_GetWidth() { return *guest::ptr<s32>(0x2EE4C0u); }     // 0x135EB0
inline ScreenLayout* Layout(SwordInfo* s) { return guest::ptr<ScreenLayout>(s->layout); }
inline const Sword* SwordOf(SwordInfo* s) { return guest::ptr<Sword>(s->sword); }
inline const SkillChart* Chart(SwordInfo* s) { return SkillSheets_Chart(guest::ptr(s->sheets), 4); }
inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
inline bool Known(const Sword* sword, s16 key) { return key >= 0 && key < 0x40 && sword->skill[key] == 1; }
inline s32 CellX(SwordInfo* s, s16 column) { return s->scrollX + column * 0xC1; }
inline s32 CellY(SwordInfo* s, s16 row) { return row * -0x4C - s->scrollY; }
const float kSmall = 0.82f;

void Put(const u16* text, s32 x, s32 y) {
    Font_DrawString(*gFont, text, (int)Screen_X(x * 16, 0), (int)Screen_Y(y * -16, 0), 0, kSmall, 0.0f);
}

// ── 0x236200: a cell's ground and its shadow ──────────────────────────────
void CellBack(SwordInfo* self, const SkillCell* cell, int lit) {
    const LayoutItem* item = ScreenLayout_FindItem(Layout(self), 3);
    if (item == nullptr)
        return;
    const s32 shadow[2] = {CellX(self, cell->column) - 0x124, CellY(self, cell->row) + 0x90};
    ScreenLayout_FillItem(Layout(self), item, shadow, 0x400C0808u);
    const s32 at[2] = {CellX(self, cell->column) - 0x127, CellY(self, cell->row) + 0x93};
    ScreenLayout_FillItem(Layout(self), item, at,
                          lit == 0 ? 0x80686D69u : cell->follows == 0 ? 0x80ACB2BEu : 0x804F788Fu);
}

// ── 0x235B60: its border, three pieces along the top and the bottom ───────
void CellFrame(SwordInfo* self, const SkillCell* cell, int lit) {
    const auto row = [&](int group, s32 y) {
        const LayoutItem* item = ScreenLayout_FindItem(Layout(self), group);
        if (item == nullptr)
            return;
        const s32 w = item->v[1].x - item->v[0].x;
        s32 at[2] = {CellX(self, cell->column) - 0x125, CellY(self, cell->row) + y};
        for (int i = 0; i < 3; i++, at[0] += w)
            ScreenLayout_DrawSpriteAt(Layout(self), item, at);
    };
    if (lit == 0) {
        row(6, 0x91);
        row(7, 0x75);
    } else if (cell->frame >= 0 && cell->frame <= 2) {
        row(5, 0x91);
        row(cell->frame == 2 ? 10 : cell->frame == 1 ? 9 : 8, 0x75);
    }
}

// ── 0x235270: the technique's name, and the word for its kind on a tag ────
void CellName(SwordInfo* self, const SkillCell* cell) {
    Font* font = *gFont;
    const LayoutItem* tag = ScreenLayout_FindItem(Layout(self), 4);
    if (tag == nullptr || TextTable_Get() == nullptr)
        return;
    if (cell->kind != 0) {
        const s32 at[2] = {CellX(self, cell->column) - 0x122, CellY(self, cell->row) + 0x9F};
        ScreenLayout_DrawSpriteAt(Layout(self), tag, at);
        if (cell->kind >= 1 && cell->kind <= 5) {
            const u16* word = Text(0x170 + cell->kind);
            const int x = (int)Screen_X((CellX(self, cell->column) - 0x10E) * 16, 0);
            const int y = (int)Screen_Y((CellY(self, cell->row) + 0x94) * -16, 0);
            Font_SetColor(font, 0x80418DC7u);
            Font_DrawCentered(font, word, x, y, 0, 0.92f, 0.0f);
            Font_SetColor(font, 0x80000000u);
            Font_DrawCentered(font, word, x, y, 0, kSmall, 0.0f);
        }
    }
    Font_SetColor(font, cell->special == 1 ? 0x80BAA96Cu : 0x80ACB2BEu);
    Put(guest::ptr<u16>(cell->name), CellX(self, cell->column) - 0x11F, CellY(self, cell->row) + 0x8C);
}

// ── 0x234590: the buttons that do it, and the tally it takes to learn ─────
void CellButtons(SwordInfo* self, const SkillCell* cell) {
    if (TextTable_Get() == nullptr)
        return;
    Font_SetColor(*gFont, 0x80FFFFFFu);
    const u16 b = cell->buttons;
    s32 x = CellX(self, cell->column) - 0x11F;
    const s32 y = CellY(self, cell->row) + 0x71;
    const auto put = [&](int id, s32 at) { Put(Text(id), at, y); };
    const auto next = [&](int id) { put(id, x); x += 0x12; };
    s32 end = x;
    if ((b & 0x1000) != 0 || (b & 0x2000) != 0) {
        if ((b & 2) != 0) next(0x168);
        if ((b & 1) != 0) next(0x169);
        if ((b & 3) != 0) next(0x167);
        if ((b & 0x1000) != 0) next(0x16A);
        if ((b & 0x2000) != 0) next(0x16B);
        end = x;
        if ((b & 4) != 0) {
            put(0x177, x);
            end = x + 0x24;
        }
    } else if ((b & 0x4000) != 0) {
        if ((b & 1) != 0) next(0x169);
        else if ((b & 2) != 0) next(0x168);
        if ((b & 3) != 0) next(0x167);
        put(0x176, x);
        put(0x167, x + 0x12);
        put(0x16A, x + 0x24);
        end = x + 0x36;
    } else if ((b & 0x8000) != 0) {
        if ((b & 4) != 0) {
            if ((b & 8) != 0) next(0x165);
            put(0x176, x);
            put(0x167, x + 0x12);
            put(0x16B, x + 0x24);
            end = x + 0x36;
        } else {
            put(0x165, x);
            end = x + 0x12;
            if ((b & 1) != 0) {
                put(0x16A, x + 0x12);
                put(0x16C, x + 0x24);
                put(0x16B, x + 0x48);
                end = x + 0x5A;
            } else if ((b & 2) != 0) {
                put(0x16B, x + 0x12);
                end = x + 0x24;
            }
        }
    }
    put(0x166, end);
    static u32 number = 0;
    if (number == 0)
        number = guest::addr(guest::alloc(0x100 * 2));
    WStr_Format(guest::ptr<u16>(number), 0x100, "%d", (int)cell->tally);
    Put(guest::ptr<u16>(number), end + 0x12, y);
}

void CellLit(SwordInfo* self, const SkillCell* cell) {
    CellBack(self, cell, 1);
    CellFrame(self, cell, 1);
    CellName(self, cell);
    CellButtons(self, cell);
}
void CellUnlit(SwordInfo* self, const SkillCell* cell) {
    CellBack(self, cell, 0);
    CellFrame(self, cell, 0);
}

// ── 0x2363A0: a key off the sword's sheet stands for the first real one of
// its group in the second set ─────────────────────────────────────────────
s16 Resolve(SwordInfo* self, s16 key) {
    for (const auto& group : Chart(self)->groups2) {
        bool has = false;
        for (const s16 member : group)
            has = has || member == key;
        if (!has)
            continue;
        for (const s16 member : group)
            if (member >= 0 && member < 0x40)
                return member;
    }
    return -1;
}

// ── 0x2364E0: the line from one cell to the next — black on the first
// pass, blue over it once both techniques are known ───────────────────────
void DrawLink(SwordInfo* self, SkillLink link, int pass) {
    if (link.from < 0 || link.from > 0x3F) {
        link.from = Resolve(self, link.from);
        if (link.from == -1)
            return;
    }
    if (link.to < 0 || link.to > 0x3F) {
        link.to = Resolve(self, link.to);
        if (link.to == -1)
            return;
    }
    const Sword* sword = SwordOf(self);
    u32 rgba = 0x802323A2u;
    if (sword->skill[link.from] == 0 || sword->skill[link.to] == 0) {
        if (pass != 0)
            return;
        rgba = 0x80000000u;
    }
    s32 rows = link.row0 - link.row1;
    const bool up = rows < 0;
    if (up)
        rows = -rows;
    const s16 slant = rows == 2 ? 0x12 : rows == 1 ? 10 : 4;
    s16 x[4], y[4];
    x[0] = x[2] = (s16)(self->scrollX + link.column0 * 0xC1 - 0x8A);
    x[1] = x[3] = (s16)(self->scrollX + link.column1 * 0xC1 - 0x127);
    if (up) {
        y[0] = (s16)(link.row0 * -0x4C + 0x78 - self->scrollY);
        y[2] = (s16)(y[0] - slant);
        y[3] = (s16)(link.row1 * -0x4C + 0x74 - self->scrollY);
        y[1] = (s16)(y[3] + slant);
    } else {
        y[2] = (s16)(link.row0 * -0x4C + 0x74 - self->scrollY);
        y[0] = (s16)(y[2] + slant);
        y[1] = (s16)(link.row1 * -0x4C + 0x78 - self->scrollY);
        y[3] = (s16)(y[1] - slant);
    }
    struct Packet {
        u64 prim;
        struct { u32 rgba; float q; u16 x, y; u32 z; } v[4];
    } packet;
    memset(&packet, 0, sizeof packet);
    packet.prim = 0x4C;
    for (int i = 0; i < 4; i++) {
        packet.v[i].rgba = rgba;
        packet.v[i].q = 1.0f;
        packet.v[i].x = (u16)Screen_X((int)(u16)x[i] << 4, 0);
        packet.v[i].y = (u16)Screen_Y((int)(u16)y[i] * -16, 0);
    }
    GsDevice_DrawPrim(*gGsDevice, &packet, 4);
}

// ── 0x234460: a technique's first form shows unless a later form of it in
// the same group is known ─────────────────────────────────────────────────
bool ShowsFirstForm(SwordInfo* self, s16 key, const std::vector<s16>& group) {
    if (key == 0)
        return true;
    const SkillChart* chart = Chart(self);
    for (const s16 member : group) {
        const auto cell = chart->techniques.find(member);
        if (cell != chart->techniques.end() && cell->second.follows != 0 && cell->second.follows == key &&
            Known(SwordOf(self), member))
            return false;
    }
    return true;
}

// ── 0x236AC0: the lines, then every group of cells ────────────────────────
void DrawChart(SwordInfo* self) {
    const SkillChart* chart = Chart(self);
    const Sword* sword = SwordOf(self);
    for (const SkillLink& link : chart->links)
        DrawLink(self, link, 0);
    for (const SkillLink& link : chart->links)
        DrawLink(self, link, 1);
    for (const auto& group : chart->groups) {
        bool any = false, lit = false;
        for (const s16 member : group)
            any = any || Known(sword, member);
        if (any) {
            for (const s16 member : group) {
                const auto cell = chart->techniques.find(member);
                if (cell == chart->techniques.end())
                    continue;
                if (cell->second.follows == 0) {
                    if (ShowsFirstForm(self, member, group)) {
                        CellLit(self, &cell->second);
                        lit = true;
                    }
                } else if (Known(sword, member)) {
                    CellLit(self, &cell->second);
                    lit = true;
                    break;
                }
            }
        }
        if (!lit) {
            for (const s16 member : group) {
                const auto cell = chart->techniques.find(member);
                if (cell != chart->techniques.end())
                    CellUnlit(self, &cell->second);
            }
        }
    }
}

// ── 0x236890: the second set of groups (older sheet files only) ───────────
void DrawChart2(SwordInfo* self) {
    const SkillChart* chart = Chart(self);
    for (const auto& group : chart->groups2) {
        bool lit = false;
        for (const s16 member : group)
            lit = lit || Known(SwordOf(self), member);
        for (const s16 member : group) {
            const auto cell = chart->techniques.find(member);
            if (cell == chart->techniques.end())
                continue;
            if (lit)
                CellLit(self, &cell->second);
            else
                CellUnlit(self, &cell->second);
        }
    }
}

// ── 0x236EA0: a bar across the top, seventeen pieces wide ─────────────────
s32 BarLeft(const LayoutItem* end) {
    const s32 w = end->v[1].x - end->v[0].x;
    return (Screen_GetWidth() - w * 0x11) / 2 - Screen_GetWidth() / 2;
}
void DrawBar(SwordInfo* self, s32 y, s32 dx, u32 rgba) {
    const LayoutItem* end = ScreenLayout_FindItem(Layout(self), 0);
    const LayoutItem* middle = ScreenLayout_FindItem(Layout(self), 0xB);
    if (end == nullptr || middle == nullptr)
        return;
    s32 at[2] = {dx + BarLeft(end), y};
    ScreenLayout_FillItem(Layout(self), end, at, rgba);
    at[0] += end->v[1].x - end->v[0].x;
    for (int i = 0; i < 0xF; i++, at[0] += middle->v[1].x - middle->v[0].x)
        ScreenLayout_FillItem(Layout(self), middle, at, rgba);
    ScreenLayout_FillItem(Layout(self), end, at, rgba);
}

// ── 0x237010: the header — the bar, its pattern and the sword's name ──────
void DrawHeader(SwordInfo* self) {
    Font* font = *gFont;
    DrawBar(self, 0xCD, 3, 0x400C0808u);
    DrawBar(self, 0xD0, 0, 0x80152D3Cu);
    const LayoutItem* end = ScreenLayout_FindItem(Layout(self), 0);
    if (end == nullptr)
        return;
    s32 left = BarLeft(end);
    const LayoutItem* pattern = ScreenLayout_FindItem(Layout(self), 1);
    if (pattern == nullptr)
        return;
    const s32 w = pattern->v[1].x - pattern->v[0].x;
    for (int i = 0; i < 0x11; i++) {
        const s32 at[2] = {left + i * w + 2, 0xCE};
        ScreenLayout_DrawSpriteAt(Layout(self), pattern, at);
    }
    left += w * 0x11 / 2;
    const u16* name = SwordOf(self)->name;
    Font_SetColor(font, 0x801F2534u);
    Font_DrawCentered(font, name, (int)Screen_X((left + 2) * 16, 0), (int)Screen_Y(-0xBE0, 0), 0, 1.0f, 0.0f);
    Font_SetColor(font, 0x80ACB2BEu);
    Font_DrawCentered(font, name, (int)Screen_X(left * 16, 0), (int)Screen_Y(-0xC00, 0), 0, 1.0f, 0.0f);
}

// ── 0x237230: the stamp, when the sword knows everything on its chart ─────
void DrawStamp(SwordInfo* self) {
    for (const auto& technique : Chart(self)->techniques)
        if (SwordOf(self)->skill[technique.first] == 0)
            return;
    if (const LayoutItem* stamp = ScreenLayout_FindItem(Layout(self), 2)) {
        const s32 none[2] = {0, 0};
        ScreenLayout_DrawSprite(Layout(self), stamp, none);
    }
}

// ── 0x2374A0 ──────────────────────────────────────────────────────────────
void Scroll(SwordInfo* self) {
    const u16 held = (u16)(Pad_GetHeld(PadMgr_Get(), 0) | Pad_GetStickHeld(PadMgr_Get(), 0));
    self->speed = (held & (kPadSquare | kPadTriangle | kPadCircle | kPadR1)) != 0 ? 0x14 : 10;
    const SkillChart* chart = Chart(self);
    if ((held & kPadRight) != 0 && chart->columns > 3) {
        self->scrollX = (s16)(self->scrollX - self->speed);
        if (self->scrollX < (chart->columns - 3) * -0xC1)
            self->scrollX = (s16)((chart->columns - 3) * -0xC1);
    }
    if ((held & kPadLeft) != 0) {
        self->scrollX = (s16)(self->scrollX + self->speed);
        if (self->scrollX > 0)
            self->scrollX = 0;
    }
    if ((held & kPadDown) != 0 && chart->rows > 4) {
        self->scrollY = (s16)(self->scrollY - self->speed);
        if (self->scrollY < (chart->rows - 4) * -0x4C)
            self->scrollY = (s16)((chart->rows - 4) * -0x4C);
    }
    if ((held & kPadUp) != 0) {
        self->scrollY = (s16)(self->scrollY + self->speed);
        if (self->scrollY > 0)
            self->scrollY = 0;
    }
}

}  // namespace

extern "C" {

// ── 0x2379C0 ──────────────────────────────────────────────────────────────
void* SwordInfo_Ctor(void* mem) {
    SwordInfo* self = static_cast<SwordInfo*>(mem);
    memset(self, 0, sizeof *self);
    self->sheets = *guest::ptr<u32>(0x2EE698u);
    return self;
}

// ── 0x237950 ──────────────────────────────────────────────────────────────
void SwordInfo_Dtor(void* mem, int flag) {
    SwordInfo* self = static_cast<SwordInfo*>(mem);
    if (self == nullptr)
        return;
    if (self->layout != 0)
        ScreenLayout_Dtor(Layout(self), 1);
    self->layout = 0;
    SkillSheets_Free(guest::ptr(self->sheets), 4);
    if ((s16)flag > 0)
        MemFree(self);
}

// ── 0x237840: `sword` is the sword's address ──────────────────────────────
int SwordInfo_Start(void* mem, int sword) {
    SwordInfo* self = static_cast<SwordInfo*>(mem);
    if (self->active == 1)
        return 1;
    if (sword == 0)
        return 0;
    self->sword = (u32)sword;
    const int asked = SkillSheets_Request(guest::ptr(self->sheets), SwordOf(self)->skillSheet, 4);
    if (asked == -1 || asked == 3)
        return 0;
    void* layout = MemAlloc(sizeof(ScreenLayout));
    self->layout = layout != nullptr ? guest::addr(ScreenLayout_Ctor(static_cast<ScreenLayout*>(layout))) : 0;
    if (self->layout == 0)
        return 0;
    ScreenLayout_Init(Layout(self));
    Layout(self)->id = kLayoutWeaponSheet;
    self->speed = 10;
    self->active = 1;
    return 1;
}

// ── 0x237930: by which carried sword ──────────────────────────────────────
void SwordInfo_Show(void* self, int slot) {
    SwordInfo_Start(self, (int)guest::addr(*gPlayerData + slot * 0x100 + 0x180));
}

// ── 0x237700 ──────────────────────────────────────────────────────────────
void SwordInfo_Update(void* mem) {
    SwordInfo* self = static_cast<SwordInfo*>(mem);
    if (self->active == 0)
        return;
    if (self->ready == 1) {
        Scroll(self);
        return;
    }
    void* sheets = guest::ptr(self->sheets);
    int chart = SkillSheets_Loaded(sheets, 4);
    if (chart == 0) {
        chart = SkillSheets_Poll(sheets, 4);
        if (chart == -1 || chart == 4)
            return;
    }
    bool layout = Layout(self)->ready != 0;
    if (!layout)
        layout = ScreenLayout_Poll(Layout(self));
    if (chart == 1 && layout) {
        self->ready = 1;
        ScreenLayout_HideGroupB(Layout(self), 1);
    }
}

// ── 0x237330 ──────────────────────────────────────────────────────────────
void SwordInfo_Draw(void* mem) {
    SwordInfo* self = static_cast<SwordInfo*>(mem);
    if (self->active == 0 || self->ready != 1)
        return;
    ScreenLayout_ShowGroupB(Layout(self), 0);
    ScreenLayout_HideGroupB(Layout(self), 2);
    ScreenLayout_Draw(Layout(self));
    DrawChart(self);
    DrawChart2(self);
    ScreenLayout_ShowGroupB(Layout(self), 2);
    ScreenLayout_HideGroupB(Layout(self), 0);
    ScreenLayout_Draw(Layout(self));
    DrawHeader(self);
    Font_SetColor(*gFont, 0x80838A82u);
    if (TextTable_Get() != nullptr)
        if (const u16* back = Text(0x17))
            Font_DrawString(*gFont, back, (int)Screen_X(0xA80, 0), (int)Screen_Y(0xB80, 0), 0, kSmall, 0.0f);
    DrawStamp(self);
}

}  // extern "C"
