// Item icons and the item selection screen — decompiled from
// 0x233EC0–0x234390, 0x26F390–0x270CD0 and 0x2A1360–0x2A2860.
// See include/game/item_select.h.

#include "game/item_select.h"
#include "game/player_data.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/param_file.h"
#include "engine/window.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void* PadMgr_Get();                                         // 0x13A920
    u16   Pad_GetPressed(void* mgr, int port);                  // 0x13A0C0
    u16   Pad_GetStickHeld(void* mgr, int port);                // 0x139F90
    void  Sound_PlaySE(u32 id, u32 positional, u32 volume);     // 0x180E70
    void  Gs_FillRect(const s32* rect, u32 z, u32 rgba);        // 0x225CD0  {x, y, w, h} in 1/16 px
    SpriteSet* SpriteSet_Ctor(void* mem);                       // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    char  SpriteSet_LoadByName(SpriteSet* set, const char* name, const char* name2);   // 0x17C2F0
    int   SpriteSet_IsLoaded(SpriteSet* set);                   // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                       // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);             // 0x17C570
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    void  SpriteSet_DrawEx(SpriteSet* set, int index, const void* params);             // 0x17ADF0
    void  SpriteSet_DrawRotated(SpriteSet* set, int index, const void* params);        // 0x179A40
    void* SpriteSet_GetSprite(SpriteSet* set, int index);                              // 0x17BDF0
    void* AssetRef_Ctor(void* mem);                             // 0x174C70
    char  AssetQueue_Request(void* ref, int assetId);           // 0x174C90
    void* AssetRef_GetData(void* ref);                          // 0x174A00
    void  AssetRef_Dtor(void* ref, int flag);                   // 0x174B80
}

static_assert(sizeof(IconSet) == 0x20 && sizeof(ItemList) == 0x54, "item parts");
static_assert(offsetof(ItemSelect, tabs) == 0x6C && offsetof(ItemSelect, upDown) == 0x124, "ItemSelect");
static_assert(sizeof(ItemSelect) == 0x16C, "ItemSelect");

static const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
static const GuestPtrVar<Font>       gFont{0x2EE67Cu};
static const GuestPtrVar<WindowMgr>  gWindowMgr{0x2EE680u};
static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);

enum { kAssetItemSprites = 0x5C, kAssetItemLayout = 0x5D, kAssetItemData = 0x6A };
enum : u32 { kZ = 0xFFFF82, kPlain = 0x80808080u };
enum { kRowsShown = 8 };
enum : u32 { kVtList = 0x2ED5D0, kVtStockList = 0x2ED5B0, kVtCarriedList = 0x2ED5C0, kListDrawRow = 0x0C };

static inline SpriteSet* SheetOf(u32 context) { return guest::ptr<SpriteSet>(guest::ptr<ItemContext>(context)->sprites); }
static inline IconSet*   IconsOf(u32 context) { return guest::ptr<IconSet>(guest::ptr<ItemContext>(context)->icons); }
static u16 SpriteW(const void* sprite) { u16 v; memcpy(&v, static_cast<const u8*>(sprite) + 0xC, 2); return v; }
static u16 SpriteH(const void* sprite) { u16 v; memcpy(&v, static_cast<const u8*>(sprite) + 0xE, 2); return v; }
static u16 PadPressed() {
    return (u16)(Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetStickHeld(PadMgr_Get(), 0));
}

// ══ Item icons ════════════════════════════════════════════════════════════
// ── 0x233340 ──────────────────────────────────────────────────────────────
int Item_FromIndex(int index) {
    return index > 9 ? index + 1 : index;
}

// ── 0x234240 ──────────────────────────────────────────────────────────────
IconSet* IconSet_Ctor(IconSet* self) {
    self->vtable = 0x2EAF30;
    WString_Ctor(&self->_10);
    WString_Reserve(&self->_10, 0);
    self->ref = 0;
    self->data = 0;
    self->table = 0;
    return self;
}

// ── 0x234010 ──────────────────────────────────────────────────────────────
void IconSet_Load(IconSet* self) {
    void* mem = MemAlloc(0xC);
    self->ref = mem ? guest::addr(AssetRef_Ctor(mem)) : 0;
    if (self->ref && !AssetQueue_Request(guest::ptr(self->ref), kAssetItemData)) {
        AssetRef_Dtor(guest::ptr(self->ref), 1);
        self->ref = 0;
    }
}

// ── 0x233F00 ──────────────────────────────────────────────────────────────
// Once the file has arrived its offsets are turned into addresses.
char IconSet_IsLoading(IconSet* self) {
    if (self->data != 0 || self->ref == 0)
        return 0;
    void* data = AssetRef_GetData(guest::ptr(self->ref));
    if (!data)
        return 1;
    self->data = guest::addr(data);
    self->table = self->data + 8;
    u32* table = guest::ptr<u32>(self->table);
    const s16 count = *guest::ptr<s16>(self->data + 4);
    for (int i = 0; i < count; i++) {
        table[i] += self->data;
        ItemInfo* item = guest::ptr<ItemInfo>(table[i]);
        item->name += table[i];
        item->_10 += table[i];
        item->description += table[i];
        item->_18 += table[i];
        item->iconFile += table[i];
        item->sprites = 0;
    }
    return 0;
}

// ── 0x233EC0 ──────────────────────────────────────────────────────────────
ItemInfo* IconSet_Get(IconSet* self, u32 item) {
    if (self->data == 0 || (u32)(s32)*guest::ptr<s16>(self->data + 4) <= item)
        return nullptr;
    return guest::ptr<ItemInfo>(guest::ptr<u32>(self->table)[item]);
}

// ── 0x234390 ──────────────────────────────────────────────────────────────
int Icon_Draw(ItemInfo* item, const void* params) {
    if (item->sprites == 0) {
        void* mem = MemAlloc(0x14);
        item->sprites = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
        if (!SpriteSet_LoadByName(guest::ptr<SpriteSet>(item->sprites), guest::ptr<const char>(item->iconFile), nullptr)) {
            if (item->sprites)
                SpriteSet_Dtor(guest::ptr<SpriteSet>(item->sprites), 1);
            item->sprites = 0;
        }
    }
    if (item->sprites == 0)
        return 0;
    if (SpriteSet_Bind(guest::ptr<SpriteSet>(item->sprites)))
        SpriteSet_DrawEx(guest::ptr<SpriteSet>(item->sprites), 0, params);
    return 1;
}

// 0x234090
static void IconSet_Unload(IconSet* self) {
    if (self->data != 0) {
        const u32* table = guest::ptr<u32>(self->table);
        for (int i = 0; i < *guest::ptr<s16>(self->data + 4); i++) {
            ItemInfo* item = guest::ptr<ItemInfo>(table[i]);
            if (!item)
                continue;
            if (item->sprites)
                SpriteSet_Dtor(guest::ptr<SpriteSet>(item->sprites), 1);
            item->sprites = 0;
        }
    }
    self->data = 0;
    self->table = 0;
    if (self->ref)
        AssetRef_Dtor(guest::ptr(self->ref), 1);
    self->ref = 0;
}

// ── 0x234150 ──────────────────────────────────────────────────────────────
IconSet* IconSet_Dtor(IconSet* self, s16 flag) {
    if (!self)
        return self;
    IconSet_Unload(self);
    WString_Release(&self->_10);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ══ Item lists ════════════════════════════════════════════════════════════
static void ItemList_Ctor(ItemList* self, u32 context, int count, u32 vtable) {    // 0x2A17B0 / 0x2A1A30
    memset(self, 0, sizeof(*self));
    self->_00 = 5;
    self->_04 = 6;
    self->count = count;
    self->context = context;
    self->vtable = vtable;
}

// 0x2A21A0
static void ItemList_SetLayout(ItemList* self, const s32* row, const s32* panel) {
    memcpy(&self->rowHeight, row, 5 * sizeof(s32));
    memcpy(self->panel, panel, 6 * sizeof(s32));
}

// 0x2A1FA0 — the focus fade and the arrows' bob
static void ItemList_Tick(ItemList* self) {
    if (self->state == kListUnfocusing) {
        if (--self->focus < 1)
            self->state = kListIdle;
    } else if (self->state == kListFocusing && ++self->focus >= gFrameRate * 7 / 30) {
        self->state = kListFocused;
    }
    if (++self->blink > (gFrameRate * 15 / 30) * 2)
        self->blink = 0;
}

// 0x2A2080
static void ItemList_Up(ItemList* self) {
    bool moved = true;
    if (--self->cursor < 0) {
        self->cursor++;
        if (--self->scroll < 0) {
            moved = false;
            self->scroll++;
        }
    }
    if (moved)
        Sound_PlaySE(3, 0, 0x3FFF);
}

// 0x2A2100
static void ItemList_Down(ItemList* self) {
    bool moved = true;
    if (++self->cursor > kRowsShown - 1) {
        self->cursor--;
        self->scroll++;
        if (self->count <= self->scroll + (kRowsShown - 1)) {
            moved = false;
            self->scroll--;
        }
    }
    if (moved)
        Sound_PlaySE(3, 0, 0x3FFF);
}

// 0x2A16F0 — the item kind under the cursor, 0 if there are none in stock
static int StockList_Current(ItemList* self) {
    int item = Item_FromIndex(self->scroll + self->cursor + 1);
    return PlayerData_GetItemCount(*gPlayerData, item) == 0 ? 0 : item;
}

// 0x2A19B0
static int CarriedList_Current(ItemList* self) {
    return (*gPlayerData)->carried[self->scroll + self->cursor];
}

// ── 0x2A1360 ──────────────────────────────────────────────────────────────
static void StockList_DrawRow(ItemList* self, u32 x, u32 y, int index) {
    PlayerData* pd = *gPlayerData;
    Font* font = *gFont;
    GsDevice* dev = *gGsDevice;
    SpriteSet* sheet = SheetOf(self->context);
    const bool other = index != self->scroll + self->cursor;
    const int item = Item_FromIndex(index + 1);
    x &= 0xFFFF;
    y &= 0xFFFF;

    SpriteSet_Draw(sheet, other ? 7 : 8, (int)x, (int)y, kZ, kPlain);
    SpriteSet_Draw(sheet, other ? 9 : 10, (int)((x + self->countX * 16) & 0xFFFF),
                   (int)((y + self->countY * 16) & 0xFFFF), kZ, kPlain);
    GsDevice_End2D(dev);
    if (!PlayerData_IsItemSeen(pd, item))
        return;
    Font_SetColor(font, other ? 0x7F100E0Fu : 0x7FBCBEC8u);
    if (ItemInfo* info = IconSet_Get(IconsOf(self->context), (u32)item))
        Font_DrawString(font, guest::ptr<const u16>(info->name), (int)((x + self->nameX * 16) & 0xFFFF),
                        (int)((y + self->nameY * 16) & 0xFFFF), kZ, 0.9f, 0.0f);
    if (const void* box = SpriteSet_GetSprite(sheet, 9)) {
        const u8 count = PlayerData_GetItemCount(pd, item);
        const u32 cx = (SpriteW(box) * 8 + x + self->countX * 16) & 0xFFFF;
        const u32 cy = (SpriteH(box) * 8 + y + self->countY * 16) & 0xFFFF;
        if (const u16* times = TextTable_Find(TextTable_Get(), 0xC))
            Font_DrawString(font, times, (int)cx + 0x40, (int)cy - 0x90, kZ, 0.92f, 0.0f);
        char digits[32];
        u16 text[32];
        snprintf(digits, sizeof(digits), "%d", count);      // a WString in the original
        int n = 0;
        for (; digits[n]; n++)
            text[n] = (u8)digits[n];
        text[n] = 0;
        Font_DrawCentered(font, text, (int)cx, (int)cy, kZ, 0.9f, 0.0f);
    }
    GsDevice_Begin2D(dev);
}
GUEST_BIND(0x2A1360, StockList_DrawRow);

// ── 0x2A1830 ──────────────────────────────────────────────────────────────
static void CarriedList_DrawRow(ItemList* self, u32 x, u32 y, int index) {
    PlayerData* pd = *gPlayerData;
    Font* font = *gFont;
    GsDevice* dev = *gGsDevice;
    const bool other = index != self->scroll + self->cursor;
    x &= 0xFFFF;
    y &= 0xFFFF;
    SpriteSet_Draw(SheetOf(self->context), other ? 5 : 6, (int)x, (int)y, kZ, kPlain);
    if (pd->carried[index] == 0)
        return;
    ItemInfo* info = IconSet_Get(IconsOf(self->context), pd->carried[index]);
    if (!info)
        return;
    Font_SetColor(font, other ? 0x7F100E0Fu : 0x7FBCBEC8u);
    GsDevice_End2D(dev);
    Font_DrawString(font, guest::ptr<const u16>(info->name), (int)((x + self->nameX * 16) & 0xFFFF),
                    (int)((y + self->nameY * 16) & 0xFFFF), kZ, 0.9f, 0.0f);
    GsDevice_Begin2D(dev);
}
GUEST_BIND(0x2A1830, CarriedList_DrawRow);

// ── 0x2A1AB0 ──────────────────────────────────────────────────────────────
static void ItemList_Draw(ItemList* self, bool focused) {
    GsDevice* dev = *gGsDevice;
    SpriteSet* sheet = SheetOf(self->context);
    const u32 sx = Screen_X(0, -1), sy = Screen_Y(0, -1) & 0xFFFF;
    const float fade = (float)self->focus / (float)(gFrameRate * 7 / 30);
    const s32 panel[4] = {(s32)sx + self->panel[0] * 16, (s32)sy + self->panel[1] * 16,
                          (self->panel[2] - self->panel[0]) * 16, (self->panel[3] - self->panel[1]) * 16};
    if (focused) {
        const s32 glow[4] = {panel[0] - 0x30, panel[1] - 0x30, panel[2] + 0x60, panel[3] + 0x60};
        Gs_FillRect(glow, kZ, (u32)(int)(fade * 128.0f) << 24 | 0x111976);
    }
    Gs_FillRect(panel, kZ, 0x80B8C9D7u);
    u32 y = sy + self->rowsY * 16;
    for (int i = self->scroll; i < self->scroll + kRowsShown; i++) {
        VirtualCall<void, ItemList*, u32, u32, int>(self->vtable, kListDrawRow, self,
                                                    (sx + (u32)(s16)(self->rowsX << 4)), y & 0xFFFF, i);
        y = (y & 0xFFFF) + ((u32)self->rowHeight & 0xFFF) * 16;
    }
    Gs_FillRect(panel, kZ, (u32)(int)((1.0f - fade) * 32.0f) << 24 | 0x0A0A0A);     // dimmed while idle
    GsDevice_End2D(dev);
    if (focused) {
        // arrows where there is more to scroll to
        const int bob = self->blink < gFrameRate * 15 / 30 ? 0 : 0x30;
        struct { u32 flags; s32 x, y; u32 z; u32 rgba; float scaleX, scaleY, angle; } p =
            {1, 0, 0, kZ, kPlain, 16.0f, 16.0f, 0.0f};
        p.x = (s32)sx + self->panel[0] * 8 + self->panel[2] * 8;
        if (self->scroll > 0) {
            p.angle = 270.0f;
            p.y = ((s32)sy + self->panel[1] * 16 - 0x50) - bob;
            SpriteSet_DrawRotated(sheet, 1, &p);
        }
        if (self->scroll + kRowsShown < self->count) {
            p.angle = 90.0f;
            p.y = bob + (s32)sy + self->panel[3] * 16 + 0x50;
            SpriteSet_DrawRotated(sheet, 1, &p);
        }
    }
    GsDevice_Begin2D(dev);
}

// ══ Tabs ══════════════════════════════════════════════════════════════════
enum { kTabCarried = 0, kTabStock = 1, kTabDone = 2 };
enum { kTabsNothing = 0, kTabsEntered = 1, kTabsLeft = 2, kTabsDone = 3 };

// ── 0x2A2690 ──────────────────────────────────────────────────────────────
static int ItemTabs_Update(ItemTabs* self) {
    const u16 pad = (u16)(Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0));
    if (pad & kPadCross) {
        if (self->tab == kTabDone) {
            Sound_PlaySE(9, 0, 0x3FFF);
            return kTabsDone;
        }
        if (self->inList == 0) {
            Sound_PlaySE(5, 0, 0x3FFF);
            self->inList = 1;
            return kTabsEntered;
        }
    }
    if ((pad & kPadTriangle) && self->inList == 1) {
        Sound_PlaySE(7, 0, 0x3FFF);
        self->inList = 0;
        return kTabsLeft;
    }
    if (self->inList == 0) {
        if (pad & kPadUp) {
            Sound_PlaySE(3, 0, 0x3FFF);
            if (--self->tab < 0)
                self->tab = 2;
        } else if (pad & kPadDown) {
            Sound_PlaySE(3, 0, 0x3FFF);
            if (++self->tab > 2)
                self->tab = 0;
        }
    }
    return kTabsNothing;
}

// ── 0x2A2250 ──────────────────────────────────────────────────────────────
static void ItemTabs_Draw(ItemTabs* self) {
    Font* font = *gFont;
    GsDevice* dev = *gGsDevice;
    SpriteSet* sheet = SheetOf(self->context);
    TextTable* table = TextTable_Get();
    const u32 sx = Screen_X(0, -1) & 0xFFFF, sy = Screen_Y(0, -1) & 0xFFFF;
    const u16* label[3] = {TextTable_Find(table, 0xE7), TextTable_Find(table, 0xE8),
                           TextTable_Find(table, 0x1A)};
    // The tab positions follow the context in ItemSelect.
    const s32* pos = reinterpret_cast<const s32*>(guest::ptr<ItemContext>(self->context) + 1);

    Font_SetColor(font, 0x80282B36u);
    for (int i = 0; i < 3; i++, pos += 2) {
        const void* tab = SpriteSet_GetSprite(sheet, 2);
        if (!tab)
            continue;
        const u32 x = (sx + pos[0] * 16) & 0xFFFF, y = (sy + pos[1] * 16) & 0xFFFF;
        const u32 w = ((u32)SpriteW(tab) << 4) & 0xFFFF, h = ((u32)SpriteH(tab) << 4) & 0xFFFF;
        const s32 shadow[4] = {(s32)x + 0x20, (s32)y + 0x20, (s32)w, (s32)h};
        Gs_FillRect(shadow, kZ, 0x40140E0Fu);
        SpriteSet_Draw(sheet, 2, (int)x, (int)y, kZ, kPlain);
        if (self->tab == i)
            SpriteSet_Draw(sheet, 3, (int)x, (int)y, kZ, kPlain);
        GsDevice_End2D(dev);
        const int halfH = (int)h >> 1;
        Font_DrawCentered(font, label[i], (int)x + ((int)w >> 1), (int)y + halfH, kZ, 0.9f, 0.0f);
        GsDevice_Begin2D(dev);
        if (self->tab != i)
            continue;
        // a pointer beside the chosen tab: right of "stock", left of "carried"
        const void* arrow = SpriteSet_GetSprite(sheet, 1);
        if (!arrow)
            continue;
        struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } p = {0, 0, 0, kZ, -1, -1, kPlain};
        p.y = (s32)y + halfH - SpriteH(arrow) * 8;
        if (self->tab == kTabStock) {
            p.flags = 0x20;
            p.x = (s32)x + (s32)w + 0x40 - SpriteW(arrow) * 8;
            SpriteSet_DrawEx(sheet, 1, &p);
        } else if (self->tab == kTabCarried) {
            p.flags = 1;
            p.x = ((s32)x - 0x40) - SpriteW(arrow) * 8;
            SpriteSet_DrawEx(sheet, 1, &p);
        }
    }
}

// ══ The screen ════════════════════════════════════════════════════════════
static inline Window* PopUp(ItemSelect* self) { return guest::ptr<Window>(self->window); }

static void ClosePopUp(ItemSelect* self) {
    WindowMgr_Close(*gWindowMgr, PopUp(self));
    self->window = 0;
}

// ── 0x270CD0 ──────────────────────────────────────────────────────────────
ItemSelect* ItemSelect_Ctor(ItemSelect* self) {
    const u32 context = guest::addr(&self->context);
    memset(self->tabPos, 0, 0x50);
    self->tabs.context = context;                               // 0x2A2860
    self->tabs.inList = 0;
    self->tabs.tab = 0;
    ItemList_Ctor(&self->carried, context, kCarriedItems, kVtCarriedList);
    ItemList_Ctor(&self->stock, context, kItemKinds, kVtStockList);
    PadRepeatPair_Ctor(&self->upDown);
    PadRepeatPair_SetMasks(&self->upDown, kPadUp, kPadDown);
    self->mode = kItemBrowse;
    self->phase = 0;
    self->window = 0;
    self->params = 0;
    self->context.sprites = 0;
    self->context.icons = 0;
    self->description = 0;
    PadRepeatPair_SetTiming(&self->upDown, 10, 3, 7);
    return self;
}

// ── 0x270AF0 ──────────────────────────────────────────────────────────────
char ItemSelect_Start(ItemSelect* self) {
    if (self->context.sprites)
        SpriteSet_Dtor(guest::ptr<SpriteSet>(self->context.sprites), 1);
    self->context.sprites = 0;
    void* mem = MemAlloc(0x14);
    self->context.sprites = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    if (!self->context.sprites)
        return 0;
    if (!SpriteSet_Load(guest::ptr<SpriteSet>(self->context.sprites), kAssetItemSprites, -1))
        return 0;
    self->phase = 1;
    if (self->context.icons == 0) {
        mem = MemAlloc(sizeof(IconSet));
        self->context.icons = mem ? guest::addr(IconSet_Ctor(static_cast<IconSet*>(mem))) : 0;
        if (!self->context.icons)
            return 0;
        IconSet_Load(guest::ptr<IconSet>(self->context.icons));
    }
    return 1;
}

// ── 0x270160 ──────────────────────────────────────────────────────────────
// Returns 0 if loading failed.
static char ItemSelect_Load(ItemSelect* self) {
    if (self->phase == 3) {
        ParamFile* params = guest::ptr<ParamFile>(self->params);
        if (!ParamFile_Poll(params)) {
            self->phase = 4;
            s32 row[5] = {}, carried[6] = {}, stock[6] = {};
            BufferReader_Read(&params->out, self->tabPos, 0x50);
            BufferReader_Read(&params->out, row, sizeof(row));
            BufferReader_Read(&params->out, carried, sizeof(carried));
            BufferReader_Read(&params->out, stock, sizeof(stock));
            ItemList_SetLayout(&self->carried, row, carried);
            ItemList_SetLayout(&self->stock, row, stock);
            ParamFile_Dtor(params, 1);
            self->params = 0;
        }
    } else if (self->phase == 2) {
        if (!IconSet_IsLoading(guest::ptr<IconSet>(self->context.icons))) {
            self->phase = 3;
            if (self->params)
                ParamFile_Dtor(guest::ptr<ParamFile>(self->params), 1);
            self->params = 0;
            void* mem = MemAlloc(sizeof(ParamFile));
            self->params = mem ? guest::addr(ParamFile_Ctor(static_cast<ParamFile*>(mem))) : 0;
            if (!self->params)
                return 0;
            if (!ParamFile_Request(guest::ptr<ParamFile>(self->params), kAssetItemLayout))
                return 0;
        }
    } else if (self->phase == 1 && SpriteSet_IsLoaded(guest::ptr<SpriteSet>(self->context.sprites))) {
        self->phase = 2;
    }
    return 1;
}

// A two-entry menu under an optional title.
static void OpenMenu(ItemSelect* self, int title, int first, int second, bool confirm) {
    TextTable* table = TextTable_Get();
    WindowDesc desc;
    memset(&desc, 0, sizeof(desc));
    WindowDesc_Init(&desc);
    desc.z = kZ;
    if (confirm) {
        desc.title = guest::addr(TextTable_Find(table, title));
        desc.halfWidth = 180;
        desc.halfHeight = 110;
        desc.titleHeight = 100;
        desc.cursor = 1;
        desc.priority = 2;
    }
    u32 options[2] = {guest::addr(TextTable_Find(table, first)), guest::addr(TextTable_Find(table, second))};
    u32* list = static_cast<u32*>(MemAlloc(sizeof(options)));     // a vector<u32> in the original
    memcpy(list, options, sizeof(options));
    desc.options.capacity = 2;
    desc.options.count = 2;
    desc.options.data = guest::addr(list);
    self->window = guest::addr(WindowMgr_Open(*gWindowMgr, &desc));
    Sound_PlaySE(1, 0, 0x3FFF);
    MemFree(list);
}

// ── 0x270590 ──────────────────────────────────────────────────────────────
static void ItemSelect_AskDiscard(ItemSelect* self, int from) {
    OpenMenu(self, 0xEB, 0xF, 0x10, true);          // "Throw it away?"  Yes / No
    self->mode = kItemDiscard;
    self->discardFrom = from;
}

// ── 0x2707B0 ──────────────────────────────────────────────────────────────
static void ItemSelect_OpenStockMenu(ItemSelect* self) {
    OpenMenu(self, 0, 0xEA, 0x19, false);           // Take / Discard
    self->mode = kItemStockMenu;
}

// ── 0x270950 ──────────────────────────────────────────────────────────────
static void ItemSelect_OpenCarriedMenu(ItemSelect* self) {
    OpenMenu(self, 0, 0xE9, 0x19, false);           // Put back / Discard
    self->mode = kItemCarriedMenu;
}

static void ShowMessage(ItemSelect* self, int text) {
    self->window = guest::addr(WindowMgr_OpenMessage(*gWindowMgr, 0, 0, 0xFFFFE6, 2, 0, 0,
                                                     TextTable_Find(TextTable_Get(), text), 0x10));
    self->mode = kItemMessage;
}

// ── 0x26F890 ──────────────────────────────────────────────────────────────
static void ItemSelect_UpdateDiscard(ItemSelect* self) {
    const u16 pad = PadPressed();
    if (pad & kPadTriangle) {
        self->mode = kItemBrowse;
        Sound_PlaySE(7, 0, 0x3FFF);
        ClosePopUp(self);
    }
    if (pad & kPadCross) {
        const int choice = WindowMgr_GetCursor(*gWindowMgr, PopUp(self));
        ClosePopUp(self);
        if (choice == 1) {
            Sound_PlaySE(7, 0, 0x3FFF);
        } else if (choice == 0) {
            Sound_PlaySE(5, 0, 0x3FFF);
            PlayerData* pd = *gPlayerData;
            if (self->discardFrom == 1) {
                const int item = StockList_Current(&self->stock);
                PlayerData_SetItemCount(pd, item, (u8)(PlayerData_GetItemCount(pd, item) - 1));
            } else if (self->discardFrom == 0) {
                PlayerData_RemoveItem(pd, 0, (u32)(self->carried.scroll + self->carried.cursor) & 0xFF);
            }
        }
        self->mode = kItemBrowse;
    }
}

// ── 0x26FA10 ──────────────────────────────────────────────────────────────
static void ItemSelect_UpdateStockMenu(ItemSelect* self) {
    const u16 pad = PadPressed();
    if (pad & kPadTriangle) {
        self->mode = kItemBrowse;
        Sound_PlaySE(7, 0, 0x3FFF);
        ClosePopUp(self);
    }
    if (pad & kPadCross) {
        const int choice = WindowMgr_GetCursor(*gWindowMgr, PopUp(self));
        ClosePopUp(self);
        Sound_PlaySE(5, 0, 0x3FFF);
        PlayerData* pd = *gPlayerData;
        if (choice == 1) {
            ItemSelect_AskDiscard(self, 1);
        } else if (choice == 0) {
            const int item = StockList_Current(&self->stock);
            if (PlayerData_CountItems(pd, 0) < kCarriedItems) {
                PlayerData_SetItemCount(pd, item, (u8)(PlayerData_GetItemCount(pd, item) - 1));
                PlayerData_AddItemSorted(pd, 0, (u8)item);
                self->mode = kItemBrowse;
            } else {
                ShowMessage(self, 0xEC);            // cannot carry any more
            }
        }
    }
}

// ── 0x26FBF0 ──────────────────────────────────────────────────────────────
static void ItemSelect_UpdateCarriedMenu(ItemSelect* self) {
    const u16 pad = PadPressed();
    if (pad & kPadTriangle) {
        self->mode = kItemBrowse;
        Sound_PlaySE(7, 0, 0x3FFF);
        ClosePopUp(self);
    }
    if (pad & kPadCross) {
        const int choice = WindowMgr_GetCursor(*gWindowMgr, PopUp(self));
        ClosePopUp(self);
        Sound_PlaySE(5, 0, 0x3FFF);
        PlayerData* pd = *gPlayerData;
        if (choice == 1) {
            ItemSelect_AskDiscard(self, 0);
        } else if (choice == 0) {
            const u32 slot = (u32)(self->carried.scroll + self->carried.cursor);
            const u8 item = pd->carried[slot];
            const u8 count = PlayerData_GetItemCount(pd, item);
            if (count < 99) {
                PlayerData_SetItemCount(pd, item, (u8)(count + 1));
                PlayerData_RemoveItem(pd, 0, slot & 0xFF);
                self->mode = kItemBrowse;
            } else {
                ShowMessage(self, 0xED);            // the stock is full
            }
        }
    }
}

// ── 0x26FDE0 ──────────────────────────────────────────────────────────────
// Returns 1 when "Done" is chosen.
static int ItemSelect_UpdateBrowse(ItemSelect* self) {
    const u16 pad = PadPressed();
    ItemList* list = self->tabs.tab == kTabStock ? &self->stock
                   : self->tabs.tab == kTabCarried ? &self->carried : nullptr;
    const int result = ItemTabs_Update(&self->tabs);
    if (result == kTabsDone)
        return 1;
    if (result == kTabsLeft) {
        if (list)
            list->state = kListUnfocusing;
    } else if (result == kTabsEntered) {
        if (list)
            list->state = kListFocusing;
        return 0;
    }
    if (self->tabs.inList == 0)
        return 0;

    PadRepeat_Update(&self->upDown.a);
    PadRepeat_Update(&self->upDown.b);
    if (self->tabs.tab == kTabDone) {
        self->description = guest::addr(TextTable_Find(TextTable_Get(), 0xEE));
        return 0;
    }
    int item = 0;
    if (list) {
        const bool stock = self->tabs.tab == kTabStock;
        auto current = [&]() { return stock ? StockList_Current(list) : CarriedList_Current(list); };
        if (pad & kPadCross) {
            if (current() != 0) {
                if (stock)
                    ItemSelect_OpenStockMenu(self);
                else
                    ItemSelect_OpenCarriedMenu(self);
                return 0;
            }
            Sound_PlaySE(7, 0, 0x3FFF);
        } else if (!PadRepeat_IsHeld(&self->upDown.b) && self->upDown.a.fired != 0) {
            ItemList_Up(list);
        } else if (!PadRepeat_IsHeld(&self->upDown.a) && self->upDown.b.fired != 0) {
            ItemList_Down(list);
        }
        item = current();
    }
    if (item == 0) {
        self->description = 0;
    } else if (ItemInfo* info = IconSet_Get(guest::ptr<IconSet>(self->context.icons), (u32)item)) {
        self->description = info->description;
    }
    return 0;
}

// ── 0x270440 ──────────────────────────────────────────────────────────────
bool ItemSelect_Update(ItemSelect* self) {
    if (self->phase != 4)
        return ItemSelect_Load(self) == 0;
    ItemList_Tick(&self->carried);
    ItemList_Tick(&self->stock);
    switch (self->mode) {
    case kItemDiscard:
        ItemSelect_UpdateDiscard(self);
        break;
    case kItemMessage:
        if (PadPressed() & (kPadCross | kPadTriangle)) {
            self->mode = kItemBrowse;
            ClosePopUp(self);
        }
        break;
    case kItemStockMenu:
        ItemSelect_UpdateStockMenu(self);
        break;
    case kItemCarriedMenu:
        ItemSelect_UpdateCarriedMenu(self);
        break;
    case kItemBrowse:
        if (ItemSelect_UpdateBrowse(self))
            return true;
        break;
    }
    return false;
}

// ── 0x26F390 ──────────────────────────────────────────────────────────────
void ItemSelect_Draw(ItemSelect* self) {
    if (self->phase != 4)
        return;
    SpriteSet* sheet = guest::ptr<SpriteSet>(self->context.sprites);
    SpriteSet_Bind(sheet);
    Font* font = *gFont;
    GsDevice* dev = *gGsDevice;
    TextTable* table = TextTable_Get();
    GsDevice_Begin2D(dev);
    const u32 sx = Screen_X(0, -1), sy = Screen_Y(0, -1);
    SpriteSet_Draw(sheet, 0, (int)sx, (int)sy, kZ, kPlain);     // background
    ItemTabs_Draw(&self->tabs);
    const int tab = self->tabs.tab;
    ItemList_Draw(&self->carried, tab == kTabCarried);
    ItemList_Draw(&self->stock, tab == kTabStock);
    SpriteSet_Draw(sheet, 4, (int)sx + self->arrowPos[0] * 16, (int)sy + self->arrowPos[1] * 16, kZ, kPlain);

    if (self->description != 0 && SpriteSet_GetSprite(sheet, 4)) {
        GsDevice_End2D(dev);
        Font_SetColor(font, 0x7F171B22u);
        TextBox box = {};
        box.text = self->description;
        box.scale = 0.82f;
        box.x = (u16)((s16)sx + (s16)(self->textBox[0] << 4));
        box.y = (u16)((s16)sy + (s16)self->textBox[1] * 16);
        box.z = kZ;
        box.w = (u16)(((s16)self->textBox[2] - (s16)self->textBox[0]) * 16);
        box.h = (u16)(((s16)self->textBox[3] - (s16)self->textBox[1]) * 16);
        box.rgba = 0x7F171B22u;
        box.flags = kTextBoxWrap | kTextBoxClip;
        Font_DrawBox(font, &box);
        GsDevice_Begin2D(dev);
    }

    // The icon of the item under the cursor, in a framed box with a shadow.
    s32 rect[4] = {(s32)sx + self->iconBox[0] * 16 + 0x20, (s32)sy + self->iconBox[1] * 16 + 0x20,
                   (self->iconBox[2] - self->iconBox[0]) * 16, (self->iconBox[3] - self->iconBox[1]) * 16};
    Gs_FillRect(rect, kZ, 0x3E161616u);
    rect[0] -= 0x20;
    rect[1] -= 0x20;
    Gs_FillRect(rect, kZ, 0x7F9AABB9u);
    rect[0] += 0x20;
    rect[1] += 0x20;
    rect[2] -= 0x40;
    rect[3] -= 0x40;
    Gs_FillRect(rect, kZ, 0x7F070504u);
    const int item = tab == kTabStock ? StockList_Current(&self->stock)
                   : tab == kTabCarried ? CarriedList_Current(&self->carried) : 0;
    if (item != 0) {
        GsDevice_End2D(dev);
        if (ItemInfo* info = IconSet_Get(guest::ptr<IconSet>(self->context.icons), (u32)item)) {
            const struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } p =
                {1, rect[0], rect[1], kZ, rect[2], rect[3], kPlain};
            Icon_Draw(info, &p);
        }
        GsDevice_Begin2D(dev);
    }

    // Button hints: "back" is offered only while a pop-up is open.
    const u16* first = nullptr;
    const u16* second;
    if (self->mode == kItemBrowse) {
        second = TextTable_Find(table, 0x16);
    } else {
        first = TextTable_Find(table, 0x16);
        second = TextTable_Find(table, 0x17);
    }
    GsDevice_End2D(dev);
    Font_SetColor(font, 0x7FBCBEC8u);
    if (first)
        Font_DrawString(font, first, (int)sx + self->hintPos[0][0] * 16, (int)sy + self->hintPos[0][1] * 16, kZ, 1.0f, 0.0f);
    if (second)
        Font_DrawString(font, second, (int)sx + self->hintPos[1][0] * 16, (int)sy + self->hintPos[1][1] * 16, kZ, 1.0f, 0.0f);
}

// ── 0x270BE0 ──────────────────────────────────────────────────────────────
ItemSelect* ItemSelect_Dtor(ItemSelect* self, s16 flag) {
    if (!self)
        return self;
    if (self->context.icons)
        IconSet_Dtor(guest::ptr<IconSet>(self->context.icons), 1);
    self->context.icons = 0;
    if (self->context.sprites)
        SpriteSet_Dtor(guest::ptr<SpriteSet>(self->context.sprites), 1);
    self->context.sprites = 0;
    if (self->params)
        ParamFile_Dtor(guest::ptr<ParamFile>(self->params), 1);
    self->params = 0;
    WindowMgr_Clear(*gWindowMgr);
    self->stock.vtable = kVtList;
    self->carried.vtable = kVtList;
    if (flag > 0)
        MemFree(self);
    return self;
}
