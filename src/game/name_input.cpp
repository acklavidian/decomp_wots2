// Name entry — decompiled from 0x17C8A0–0x17CE70, 0x275CC0–0x278360 and the
// text label at 0x224920–0x224F60.  See include/game/name_input.h.

#include "game/name_input.h"
#include "engine/font.h"
#include "engine/pad.h"
#include "engine/param_file.h"
#include "engine/screen_layout.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>

struct SpriteSet;
extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void  MemSet(void* dst, int value, u32 size);               // 0x123880
    void* PadMgr_Get();                                         // 0x13A920
    u16   Pad_GetHeld(void* mgr, int port);                     // 0x13A0E0
    u16   Pad_GetPressed(void* mgr, int port);                  // 0x13A0C0
    u16   Pad_GetStickHeld(void* mgr, int port);                // 0x139F90
    void  Sound_PlaySE(u32 id, u32 positional, u32 volume);     // 0x180E70
    SpriteSet* SpriteSet_Ctor(void* mem);                       // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    int   SpriteSet_Bind(SpriteSet* set);                       // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);             // 0x17C570
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    void  SpriteSet_DrawEx(SpriteSet* set, int index, void* params);                   // 0x17ADF0
    void* SpriteSet_GetSprite(SpriteSet* set, int index);                              // 0x17BDF0
}

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const GuestPtrVar<Font>     gFont{0x2EE67Cu};
static s32& gFrameRate    = *guest::ptr<s32>(0x2EDA00u);
// Frames before a held direction repeats: the first time, then every time.
static const s32* const kRepeatDelays = guest::ptr<const s32>(0x2EE3A0u);

enum { kAssetCharFile = 0x47, kAssetNameSprites = 0x48 };
enum { kTextDone = 0xBD, kTextEnter = 0xC2, kTextDelete = 0xC3 };
enum { kSpriteCaret = 0, kSpriteCorner = 1, kSpriteBlank = 2 };
enum : u32 { kDepth = 0xFFFFEF, kInk = 0x7F2D160Bu };
enum { kMaxName = 14, kRowsShown = 6 };

// The parts keep a pointer to NameInput::page0 as their context.
static inline NameInput* Owner(u32 context) { return guest::ptr<NameInput>(context - 8); }
static inline ScreenLayout* LayoutOf(NameInput* n) { return guest::ptr<ScreenLayout>(n->layout); }
static inline SpriteSet*    SpritesOf(NameInput* n) { return guest::ptr<SpriteSet>(n->spriteSet); }

// ══ Held-button repeat ════════════════════════════════════════════════════
// 0x277E60
static void KeyRepeat_Update(KeyRepeat* self) {
    u32 held = (u32)(Pad_GetHeld(PadMgr_Get(), 0) | Pad_GetStickHeld(PadMgr_Get(), 0));
    self->fired = 0;
    if (held == self->last) {
        if (++self->counter >= kRepeatDelays[self->stage]) {
            self->fired = held;
            self->counter = 0;
            if (++self->stage > 1)
                self->stage = 1;
        }
    } else {
        self->fired = held;
        self->last = held;
        self->counter = 0;
        self->stage = 0;
    }
}

// ══ The name being typed ══════════════════════════════════════════════════
// 0x277DF0
static void NameField_Ctor(NameField* self, u32 context) {
    self->y = 0;
    self->x = 0;
    self->context = context;
    self->caret = 0;
    self->length = 0;
    self->blinkTimer = (s16)((gFrameRate * 10 / 30) >> 1);
    self->caretVisible = 1;
    self->leftFlash = 0;
    self->rightFlash = 0;
}

// 0x277D30
static void NameField_Init(NameField* self, const u16* name) {
    MemSet(self, 0, 0x1C);
    if (name) {
        self->length = (s16)WStr_Length(name);
        WStr_Copy(self->text, name);
        self->caret = self->length;
    }
}

// 0x277CD0
static void NameField_Place(NameField* self) {
    s32 rect[4];
    ScreenLayout_GetGroupRect(rect, LayoutOf(Owner(self->context)), 2, -1);
    self->x = rect[0] + 20;
    self->y = (rect[1] + rect[3]) >> 1;
}

// 0x2778B0
static void NameField_Insert(NameField* self, u16 character) {
    if (self->length == kMaxName)
        return;
    Sound_PlaySE(5, 0, 0x3FFF);
    self->text[self->length + 1] = 0;
    for (int i = self->length; i > self->caret; i--)
        self->text[i] = self->text[i - 1];
    self->text[self->caret] = character;
    self->length++;
    self->caret++;
}

// Nudges one of the two arrows on the name plate (layout groupB 3 = left, 2 = right).
static void NudgeArrow(NameField* self, int groupB, int dx, int dy) {
    const s32 offset[2] = {dx, dy};
    ScreenLayout_SetGroupBOffset(LayoutOf(Owner(self->context)), offset, groupB);
}

// 0x277990 — caret blink, Circle deletes, L1 / R1 move the caret
static void NameField_Update(NameField* self) {
    u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);
    if (--self->blinkTimer < 1) {
        self->blinkTimer = (s16)(gFrameRate * 10 / 30);
        self->caretVisible = self->caretVisible == 0;
    }
    if (pressed & kPadCircle) {
        if (self->caret != 0) {
            Sound_PlaySE(8, 0, 0x3FFF);
            int i = self->caret;
            for (; i < self->length; i++)
                self->text[i - 1] = self->text[i];
            self->text[i - 1] = 0;
            self->caret--;
            self->length--;
        }
        return;
    }
    if (pressed & kPadL1) {
        if (--self->caret < 0)
            self->caret = 0;
        self->caretVisible = 1;
        self->blinkTimer = (s16)(gFrameRate * 10 / 30);
        self->leftFlash = (s16)(gFrameRate * 5 / 30);
        NudgeArrow(self, 3, -2, -2);
    }
    if (pressed & kPadR1) {
        if (++self->caret > self->length)
            self->caret = self->length;
        self->caretVisible = 1;
        self->blinkTimer = (s16)(gFrameRate * 10 / 30);
        self->rightFlash = (s16)(gFrameRate * 5 / 30);
        NudgeArrow(self, 2, 2, -2);
    }
    if (self->leftFlash != 0 && --self->leftFlash < 1) {
        self->leftFlash = 0;
        NudgeArrow(self, 3, 0, 0);
    }
    if (self->rightFlash != 0 && --self->rightFlash < 1) {
        self->rightFlash = 0;
        NudgeArrow(self, 2, 0, 0);
    }
}

// 0x2776D0 — one character every 13 pixels, then the caret
static void NameField_Draw(NameField* self) {
    Font* font = *gFont;
    u32 x0 = Screen_X(self->x << 4, 0) & 0xFFFF;
    u32 y = Screen_Y(self->y * -16, 0) & 0xFFFF;
    Font_SetColor(font, kInk);
    u32 x = x0;
    for (int i = 0; i < self->length; i++, x = (x + 0xD0) & 0xFFFF) {
        const u16 one[2] = {self->text[i], 0};
        Font_DrawCenteredPlain(font, one, (int)x, (int)y, kDepth);
    }
    if (!self->caretVisible)
        return;
    SpriteSet* set = SpritesOf(Owner(self->context));
    u32 cx = ((x0 + (((u32)(self->caret * 13) - 6) & 0xFFF) * 16) & 0xFFFF) + 0xFFF0;
    cx &= 0xFFFF;
    u32 cy = y;
    if (const u8* sprite = static_cast<const u8*>(SpriteSet_GetSprite(set, kSpriteCaret))) {
        u16 w, h;
        memcpy(&w, sprite + 0xC, 2);
        memcpy(&h, sprite + 0xE, 2);
        cx = (cx - ((w >> 1) & 0xFFF) * 16) & 0xFFFF;
        cy = (cy - ((h >> 1) & 0xFFF) * 16) & 0xFFFF;
    }
    SpriteSet_Draw(set, kSpriteCaret, (int)cx, (int)cy, kDepth, 0x80808080u);
}

// ══ Cursor frame ══════════════════════════════════════════════════════════
static void SetCursor(NameInput* n, int x0, int y0, int x1, int y1) {
    n->cursor[0] = x0;
    n->cursor[1] = y0;
    n->cursor[2] = x1;
    n->cursor[3] = y1;
}

// 0x277F50 — four corner brackets
static void Cursor_Draw(const s32* rect, SpriteSet* set) {
    int x0 = (int)(Screen_X(rect[0] << 4, 0) & 0xFFFF) - 0x20;
    int y0 = (int)(Screen_Y(rect[1] * -16, 0) & 0xFFFF) - 0x20;
    int x1 = (int)(Screen_X(rect[2] << 4, 0) & 0xFFFF) - 0x60;
    int y1 = (int)(Screen_Y(rect[3] * -16, 0) & 0xFFFF) - 0x60;
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } p = {1, x0, y0, kDepth, -1, -1, 0x80808080u};
    SpriteSet_DrawEx(set, kSpriteCorner, &p);
    p.flags = 0x40; p.x = x0; p.y = y1;
    SpriteSet_DrawEx(set, kSpriteCorner, &p);
    p.flags = 0x60; p.x = x1; p.y = y1;
    SpriteSet_DrawEx(set, kSpriteCorner, &p);
    p.flags = 0x20; p.x = x1; p.y = y0;
    SpriteSet_DrawEx(set, kSpriteCorner, &p);
}

// ══ Menu beside the keyboard ══════════════════════════════════════════════
// The cursor frame sits on entry `cursor`; the chosen entry's art is drawn
// pressed in, so the frame is shifted for the others.
static void NameMenu_FrameCursor(NameMenu* self) {
    NameInput* n = Owner(self->context);
    s32 rect[4];
    ScreenLayout_GetGroupRect(rect, LayoutOf(n), self->cursor + 10, -1);
    if (self->chosen == self->cursor)
        SetCursor(n, rect[0], rect[1], rect[2], rect[3]);
    else
        SetCursor(n, rect[0] + 1, rect[1] - 3, rect[2], rect[3]);
}

// 0x2775D0
static void NameMenu_Ctor(NameMenu* self, u32 context) {
    TextLabel_Ctor(&self->label);
    self->context = context;
    Owner(context)->focus = 0;
    self->chosen = 0;
    self->cursor = 0;
    TextLabel_SetText(&self->label, TextTable_Find(TextTable_Get(), kTextDone));
    TextLabel_SetScale(&self->label, 1.0f);
}

// 0x277500
static void NameMenu_Place(NameMenu* self) {
    s32 pos[2];
    ScreenLayout_GetGroupCenter(pos, LayoutOf(Owner(self->context)), 10, -1);
    self->label.x = pos[0];
    self->label.y = pos[1];
}

// 0x276DC0 / 0x276EC0 — take the focus, at the entry beside `row`
static void NameMenu_Focus(NameMenu* self, int row) {
    Owner(self->context)->focus = 1;
    self->cursor = row > 0 ? 0 : row;
    NameMenu_FrameCursor(self);
}

// 0x276FD0
static void NameMenu_Choose(NameMenu* self) {
    ScreenLayout* layout = LayoutOf(Owner(self->context));
    ScreenLayout_HideGroup(layout, self->chosen + 10);
    ScreenLayout_ShowGroup(layout, self->cursor + 10);
    self->chosen = self->cursor;
    NameMenu_FrameCursor(self);
}

// 0x277100 — true when an entry is confirmed
static bool NameMenu_Update(NameMenu* self) {
    NameInput* n = Owner(self->context);
    u16 pressed = (u16)(Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetStickHeld(PadMgr_Get(), 0));
    if (n->focus != 1)
        return false;
    if (pressed & kPadCross) {
        NameMenu_Choose(self);
        Sound_PlaySE(self->cursor == 0 ? 9 : 6, 0, 0x3FFF);
        return true;
    }
    self->toGrid = 0;
    if (pressed & kPadTriangle) {
        if (self->cursor != 0)
            Sound_PlaySE(7, 0, 0x3FFF);
        self->cursor = 0;
        NameMenu_FrameCursor(self);
        return false;
    }
    const u32 fired = n->repeat.fired;
    if (fired & (kPadLeft | kPadRight)) {
        self->toGrid = 1;
    } else if (fired & kPadUp) {
        if (--self->cursor < 0)
            self->cursor = 0;
        NameMenu_FrameCursor(self);
        Sound_PlaySE(3, 0, 0x3FFF);
    } else if (fired & kPadDown) {
        if (++self->cursor > 0)
            self->cursor = 0;
        NameMenu_FrameCursor(self);
        Sound_PlaySE(3, 0, 0x3FFF);
    }
    return false;
}

// 0x276DA0
static void NameMenu_Draw(NameMenu* self) {
    TextLabel_DrawCentered(&self->label, kInk, kDepth);
}

// ══ Keyboard ══════════════════════════════════════════════════════════════
static inline CharPage* PageOf(CharGrid* g) { return guest::ptr<CharPage>(g->page); }
static inline const u16* CharsOf(CharGrid* g) { return guest::ptr<const u16>(PageOf(g)->chars); }

// 0x2761E0
static int CharGrid_Index(CharGrid* self) {
    return self->column + (self->row + self->scroll) * PageOf(self)->columns;
}

// Cells are 22 pixels apart with 18 more between groups, rows 31 apart.
static void CharGrid_FrameCursor(CharGrid* self) {
    const int x = self->column * 22 + (self->column / PageOf(self)->group) * 18;
    SetCursor(Owner(self->context), x - 134, self->row * -31 + 50, x - 114, self->row * -31 + 30);
}

// 0x276BE0
static void CharGrid_Ctor(CharGrid* self, u32 context) {
    self->row = 0;
    self->column = 0;
    self->savedRow = 0;
    self->savedColumn = 0;
    self->context = context;
    self->page = context;               // page 0
    self->scroll = 0;
    self->level = 0;
    self->offEdge = 0;
    self->cancelled = 0;
    NameInput* n = Owner(context);
    n->page0.columns = 15;
    n->page0.group = 5;
    for (CharPage& page : n->pages) {
        page.columns = 17;
        page.group = 17;
    }
    CharGrid_FrameCursor(self);
}

// 0x2780D0 — one page of the character file: lines of characters up to a blank line
static bool CharPage_Parse(CharPage* self, BufferReader* r) {
    const int total = WText_FindBlankLine(r);
    self->count = total - WText_CountLineBreaks(r, total) * 2;
    if (self->chars)
        MemFree(guest::ptr(self->chars));
    self->chars = 0;
    u16* chars = static_cast<u16*>(MemAlloc((u32)self->count << 1));
    self->chars = guest::addr(chars);
    for (int i = 0; i < self->count;) {
        if (BufferReader_Peek16(r) == '\r'
            && (r->remaining < 2 ? 0 : guest::ptr<const u16>(r->cur)[1]) == '\n') {
            r->cur += 4;
            r->remaining -= 4;
        } else {
            chars[i++] = BufferReader_Read16(r);
        }
    }
    r->cur += 8;                        // the blank line
    r->remaining -= 8;
    self->rows = self->count / self->columns + (self->count % self->columns != 0 ? 1 : 0);
    self->lastRow = self->count % self->columns;
    if (self->lastRow == 0)
        self->lastRow = self->columns;
    return true;
}

// 0x276AD0 — the file is 16-bit text with a byte-order mark
static bool CharGrid_Load(CharGrid* self) {
    NameInput* n = Owner(self->context);
    BufferReader* r = &guest::ptr<AssetBuffer>(n->charFile)->reader;
    if (BufferReader_Read16(r) != 0xFEFF)
        return false;
    n->page0.columns = 15;
    n->page0.group = 5;
    return CharPage_Parse(&n->page0, r);
}

static void ClampToLastRow(CharGrid* self) {
    const CharPage* page = PageOf(self);
    if (self->row + self->scroll == page->rows - 1 && page->lastRow <= self->column)
        self->column = page->lastRow - 1;
}

// 0x276210
static void CharGrid_MoveUp(CharGrid* self) {
    const CharPage* page = PageOf(self);
    if (--self->row >= 0)
        return;
    self->scroll--;
    self->row = 0;
    if (self->wrapped == 1 && CharsOf(self)[CharGrid_Index(self)] != 0xFFFF
        && self->row + self->scroll < kRowsShown) {
        self->row += self->scroll;
        self->scroll = 0;
    }
    if (self->scroll < 0) {             // wrap to the bottom
        self->row = page->rows - 1;
        if (self->row < kRowsShown) {
            self->scroll = 0;
        } else {
            self->scroll = self->row - (kRowsShown - 1);
            self->row -= self->scroll;
            self->wrapped = 1;
        }
    }
    if (page->lastRow <= self->column)
        self->column = page->lastRow - 1;
}

// 0x276330 — move by the repeat-filtered d-pad; blank cells are skipped
static void CharGrid_Move(CharGrid* self) {
    const CharPage* page = PageOf(self);
    const u32 fired = Owner(self->context)->repeat.fired;
    bool moved = false;
    self->offEdge = 0;

    if (fired & kPadUp) {
        CharGrid_MoveUp(self);
        moved = true;
    } else if (fired & kPadDown) {
        if (++self->row >= kRowsShown) {
            self->scroll++;
            self->row--;
        }
        if (self->row + self->scroll < page->rows) {
            ClampToLastRow(self);
        } else {
            self->row = 0;
            self->scroll = 0;
        }
        moved = true;
    }
    if (fired & kPadRight) {
        self->column++;
        if (self->column < page->columns) {
            if (self->row + self->scroll == page->rows - 1 && page->lastRow <= self->column)
                self->offEdge = 1;
        } else {
            self->offEdge = 1;
        }
        moved = true;
    } else if (fired & kPadLeft) {
        self->column--;
        if (self->column == -1) {
            self->offEdge = 1;
        } else if (self->column < 0) {
            self->column = page->columns - 1;
            ClampToLastRow(self);
        }
        moved = true;
    }

    if (self->offEdge) {
        Sound_PlaySE(2, 0, 0x3FFF);
    } else if (CharsOf(self)[CharGrid_Index(self)] == 0xFFFF) {
        CharGrid_Move(self);
    } else if (moved) {
        Sound_PlaySE(2, 0, 0x3FFF);
    }
}

// 0x2765A0 — take the focus back from the menu, entering from the left
static void CharGrid_Focus(CharGrid* self, int row) {
    const CharPage* page = PageOf(self);
    Owner(self->context)->focus = 0;
    self->column = -1;
    self->row = row;
    CharGrid_Move(self);
    if (self->row >= page->rows)
        self->row = page->rows - 1;
    ClampToLastRow(self);
    CharGrid_FrameCursor(self);
}

// 0x2766D0 — switch between the keyboard and a character's sub-page
static void CharGrid_SetLevel(CharGrid* self, int level, int index) {
    NameInput* n = Owner(self->context);
    if (level < 1 || level > 0) {
        if (n->pages[CharGrid_Index(self)].chars == 0) {
            Sound_PlaySE(7, 0, 0x3FFF);
            return;
        }
        self->page = guest::addr(&n->pages[index]);
        self->level = level;
        self->savedColumn = self->column;
        self->savedRow = self->row;
        self->column = 0;
        self->row = 0;
        CharGrid_FrameCursor(self);
    } else {
        self->page = guest::addr(&n->page0) + (u32)level * sizeof(CharPage);
        self->level = level;
    }
    self->scroll = 0;
}

// 0x276850 — true when Cross picks a character
static bool CharGrid_Update(CharGrid* self) {
    u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);
    self->offEdge = 0;
    self->cancelled = 0;
    if (pressed & kPadCross) {
        if (self->level == 2) {
            CharGrid_SetLevel(self, 2, CharGrid_Index(self));
            return false;
        }
        return true;
    }
    if (pressed & kPadTriangle) {
        Sound_PlaySE(7, 0, 0x3FFF);
        if (self->level == 2) {
            CharGrid_SetLevel(self, 2, 0);
            self->column = self->savedColumn;
            self->row = self->savedRow;
            CharGrid_FrameCursor(self);
        } else {
            self->cancelled = 1;
        }
        return false;
    }
    if (Owner(self->context)->repeat.fired & (kPadUp | kPadDown | kPadLeft | kPadRight)) {
        self->wrapped = 0;
        CharGrid_Move(self);
        CharGrid_FrameCursor(self);
    }
    return false;
}

// 0x275EF0
static void CharGrid_Draw(CharGrid* self) {
    Font* font = *gFont;
    GsDevice* dev = *gGsDevice;
    NameInput* n = Owner(self->context);
    const CharPage* page = PageOf(self);
    const u16* chars = CharsOf(self);
    u32 x = Screen_X(-124 * 16, 0) & 0xFFFF;
    u32 y = Screen_Y(-40 * 16, 0) & 0xFFFF;

    GsDevice_End2D(dev);
    Font_SetColor(font, kInk);
    int column = page->columns, inGroup = page->group, rows = kRowsShown;
    for (int i = self->scroll * page->columns; i < page->count; i++) {
        const u16 one[2] = {chars[i], 0};
        if (one[0] == 0xFFFF) {
            SpriteSet_Draw(SpritesOf(n), kSpriteBlank, (int)x - 0x90, (int)y - 0x90, kDepth, 0x80808080u);
        } else if (self->level == 2 && n->pages[i].chars == 0) {
            Font_SetColor(font, 0x402D160Bu);           // nothing behind this one
            Font_DrawCentered(font, one, (int)x, (int)y, kDepth, 1.2f, 0.0f);
            Font_SetColor(font, kInk);
        } else {
            Font_DrawCentered(font, one, (int)x, (int)y, kDepth, 1.2f, 0.0f);
        }
        if (--column == 0) {
            if (--rows == 0)
                break;
            x = Screen_X(-124 * 16, 0) & 0xFFFF;
            y = (y + 0x1F0) & 0xFFFF;
            column = page->columns;
            inGroup = page->group;
        } else if (--inGroup == 0) {
            x = (x + 0x280) & 0xFFFF;
            inGroup = page->group;
        } else {
            x = (x + 0x160) & 0xFFFF;
        }
    }
    GsDevice_End2D(dev);
}

// ══ Button prompts ════════════════════════════════════════════════════════
// 0x275EB0
static void NamePrompts_Ctor(NamePrompts* self, u32 context) {
    self->context = context;
    TextLabel_Ctor(&self->enter);
    TextLabel_Ctor(&self->erase);
}

// 0x275D30
static void NamePrompts_Place(NamePrompts* self) {
    TextTable* text = TextTable_Get();
    ScreenLayout* layout = LayoutOf(Owner(self->context));
    s32 pos[2];
    ScreenLayout_GetGroupOrigin(pos, layout, 0x14, -1);
    self->enter.x = pos[0];
    self->enter.y = pos[1];
    TextLabel_SetScale(&self->enter, 1.0f);
    TextLabel_SetText(&self->enter, TextTable_Find(text, kTextEnter));
    ScreenLayout_GetGroupOrigin(pos, layout, 0x15, -1);
    self->erase.x = pos[0];
    self->erase.y = pos[1];
    TextLabel_SetScale(&self->erase, 1.0f);
    TextLabel_SetText(&self->erase, TextTable_Find(text, kTextDelete));
}

// 0x275CC0
static void NamePrompts_Draw(NamePrompts* self) {
    GsDevice* dev = *gGsDevice;
    GsDevice_Begin2D(dev);
    TextLabel_Draw(&self->enter, 0x7FACB2BEu, kDepth);
    TextLabel_Draw(&self->erase, 0x7FACB2BEu, kDepth);
    GsDevice_End2D(dev);
}

// ══ The screen ════════════════════════════════════════════════════════════

// ── 0x17CE70 ──────────────────────────────────────────────────────────────
NameInput* NameInput_Ctor(NameInput* self) {
    const u32 context = guest::addr(&self->page0);
    memset(&self->page0, 0, sizeof(CharPage));          // 0x278300 for each page
    memset(self->pages, 0, sizeof(self->pages));
    memset(self->cursor, 0, sizeof(self->cursor));
    self->repeat.last = 0;                              // 0x277F30
    self->repeat.stage = 0;
    self->repeat.counter = 0;
    NameField_Ctor(&self->field, context);
    NameMenu_Ctor(&self->menu, context);
    CharGrid_Ctor(&self->grid, context);
    NamePrompts_Ctor(&self->prompts, context);
    self->phase = 0;
    self->layout = 0;
    self->spriteSet = 0;
    self->charFile = 0;
    return self;
}

// ── 0x17CBF0 ──────────────────────────────────────────────────────────────
int NameInput_Setup(NameInput* self, const u16* name, int mode) {
    void* mem = MemAlloc(sizeof(ScreenLayout));
    self->layout = mem ? guest::addr(ScreenLayout_Ctor(static_cast<ScreenLayout*>(mem))) : 0;
    if (!self->layout)
        return 0;
    ScreenLayout_Init(LayoutOf(self));
    LayoutOf(self)->id = kLayoutNameInput;

    mem = MemAlloc(0x14);
    self->spriteSet = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    if (!self->spriteSet)
        return 0;
    if (!SpriteSet_Load(SpritesOf(self), kAssetNameSprites, -1)) {
        SpriteSet_Dtor(SpritesOf(self), 1);
        self->spriteSet = 0;
        return 0;
    }

    mem = MemAlloc(sizeof(AssetBuffer));
    self->charFile = mem ? guest::addr(AssetBuffer_Ctor(static_cast<AssetBuffer*>(mem))) : 0;
    if (!self->charFile)
        return 0;
    if (!AssetBuffer_Request(guest::ptr<AssetBuffer>(self->charFile), kAssetCharFile)) {
        if (self->spriteSet)
            SpriteSet_Dtor(SpritesOf(self), 1);
        self->spriteSet = 0;
        return 0;
    }
    self->phase = 1;
    self->mode = (s16)mode;
    NameField_Init(&self->field, name);
    return 1;
}

// ── 0x17C940 ──────────────────────────────────────────────────────────────
// Returns 0 once everything is loaded (or loading failed).
static int NameInput_Load(NameInput* self) {
    switch (self->phase) {
    case 3:
        if (!AssetBuffer_Poll(guest::ptr<AssetBuffer>(self->charFile))) {
            if (!CharGrid_Load(&self->grid)) {
                self->phase = 0;
                return 1;
            }
            self->phase = 4;
            return 0;
        }
        break;
    case 2:
        if (SpriteSet_Bind(SpritesOf(self)))
            self->phase = 3;
        break;
    case 1:
        if (ScreenLayout_Poll(LayoutOf(self))) {
            ScreenLayout_HideGroupB(LayoutOf(self), 0);
            ScreenLayout_SetDepth(LayoutOf(self), (int)kDepth);
            NameField_Place(&self->field);
            NameMenu_Place(&self->menu);
            NamePrompts_Place(&self->prompts);
            if (self->mode == 2)
                ScreenLayout_HideGroup(LayoutOf(self), 1);
            self->phase = 2;
        }
        break;
    }
    return 1;
}

// ── 0x17CA70 ──────────────────────────────────────────────────────────────
int NameInput_Update(NameInput* self) {
    if (!self->layout || !self->spriteSet || self->phase == 0)
        return 1;
    if (self->phase != 4 && NameInput_Load(self) != 0)
        return 0;

    NameField_Update(&self->field);
    KeyRepeat_Update(&self->repeat);
    if (self->focus == 1) {
        if (NameMenu_Update(&self->menu)) {
            if (self->menu.cursor == 0)
                return 1;                               // "Done"
            CharGrid_SetLevel(&self->grid, self->menu.cursor, 0);
        }
        if (self->menu.toGrid) {
            CharGrid_Focus(&self->grid, self->menu.cursor);
            if (self->grid.offEdge)
                NameMenu_Focus(&self->menu, self->grid.row);
        }
    } else {
        if (CharGrid_Update(&self->grid)) {
            NameField_Insert(&self->field, CharsOf(&self->grid)[CharGrid_Index(&self->grid)]);
        } else if (self->grid.offEdge) {
            NameMenu_Focus(&self->menu, self->grid.row);
        }
        if (self->grid.cancelled)
            NameMenu_Focus(&self->menu, self->menu.chosen);
    }
    return 0;
}

// ── 0x17C8A0 ──────────────────────────────────────────────────────────────
void NameInput_Draw(NameInput* self) {
    if (!self->layout || !self->spriteSet || self->phase != 4)
        return;
    ScreenLayout_Draw(LayoutOf(self));
    if (!SpriteSet_Bind(SpritesOf(self)))
        return;
    NameField_Draw(&self->field);
    NameMenu_Draw(&self->menu);
    CharGrid_Draw(&self->grid);
    NamePrompts_Draw(&self->prompts);
    Cursor_Draw(self->cursor, SpritesOf(self));
}

// ── 0x17CD60 ──────────────────────────────────────────────────────────────
NameInput* NameInput_Dtor(NameInput* self, s16 flag) {
    if (!self)
        return self;
    if (self->layout)
        ScreenLayout_Dtor(LayoutOf(self), 1);
    self->layout = 0;
    if (self->spriteSet)
        SpriteSet_Dtor(SpritesOf(self), 1);
    self->spriteSet = 0;
    if (self->charFile)
        AssetBuffer_Dtor(guest::ptr<AssetBuffer>(self->charFile), 1);
    self->charFile = 0;
    if (self->page0.chars)                              // 0x2782A0 for each page
        MemFree(guest::ptr(self->page0.chars));
    for (CharPage& page : self->pages)
        if (page.chars)
            MemFree(guest::ptr(page.chars));
    if (flag > 0)
        MemFree(self);
    return self;
}

static_assert(sizeof(CharPage) == 0x18, "name input parts");
static_assert(offsetof(NameInput, cursor) == 0x890 && offsetof(NameInput, field) == 0x8C0, "NameInput layout");
static_assert(offsetof(NameInput, menu) == 0x8F8 && offsetof(NameInput, grid) == 0x934, "NameInput layout");
static_assert(offsetof(NameInput, prompts) == 0x95C && sizeof(NameInput) == 0x9B8, "NameInput layout");
