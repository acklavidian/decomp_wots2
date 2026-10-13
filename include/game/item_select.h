#pragma once
#include "hal/ps2_types.h"
#include "engine/pad_repeat.h"
#include "engine/text_label.h"

// Items: the icon set shared by every item display, and the item selection
// screen of character setup (carried ⇄ stock).  Decompiled from
// 0x233EC0–0x234390, 0x26F390–0x270CD0 and 0x2A1360–0x2A2860.

struct SpriteSet;
struct ParamFile;
struct Window;

// One item of menu/ItemData (asset 0x6A).
struct ItemInfo {
    u32 _00, _04;
    u32 sprites;        // +0x08  SpriteSet*, loaded when first drawn
    u32 name;           // +0x0C  const u16*
    u32 _10;
    u32 description;    // +0x14  const u16*
    u32 _18;
    u32 iconFile;       // +0x1C  const char*: sprite sheet name
};

struct IconSet {
    u32 ref;            // +0x00  AssetRef*
    u32 data;           // +0x04  the file: s16 count at +4, offsets from +8
    u32 table;          // +0x08  ItemInfo*[]
    u32 _0c;
    WString _10;        // +0x10
    u32 vtable;         // +0x1C
};

// What the parts of the screen share.
struct ItemContext {
    u32 icons;          // IconSet*
    u32 sprites;        // SpriteSet*: menu/ItemSelect sheet (asset 0x5C)
};

// The three tabs on the left: carried, stock, done.
struct ItemTabs {
    s32 inList;         // +0x00  1 = the cursor is in the chosen list
    s32 tab;            // +0x04  0 carried, 1 stock, 2 done
    u32 _08;
    u32 context;        // +0x0C  ItemContext*
};

enum ItemListState : s32 { kListIdle = 0, kListFocusing = 1, kListUnfocusing = 2, kListFocused = 3 };

// A scrolling list of eight rows.
struct ItemList {
    s32 _00, _04;       // 5, 6
    s32 count;          // +0x08  8 carried / 21 kinds in stock
    s32 state;          // +0x0C  ItemListState
    s32 focus;          // +0x10  frames into the focus fade
    s32 cursor;         // +0x14  row under the cursor
    s32 scroll;         // +0x18  first row shown
    s32 blink;          // +0x1C
    s32 rowHeight;      // +0x20
    s32 nameX, nameY;   // +0x24
    s32 countX, countY; // +0x2C
    s32 panel[4];       // +0x34  x0, y0, x1, y1
    s32 rowsX, rowsY;   // +0x44
    u32 context;        // +0x4C
    u32 vtable;         // +0x50  +0x08 dtor, +0x0C draw row (self, x, y, index)
};

enum ItemMode : s32 {
    kItemBrowse = 1, kItemCarriedMenu = 2, kItemStockMenu = 4, kItemMessage = 8, kItemDiscard = 0x10,
};

struct ItemSelect {
    s32 phase;          // +0x000  1 sprites, 2 icons, 3 layout, 4 ready
    s32 mode;           // +0x004  ItemMode
    u32 window;         // +0x008  Window* of the pop-up
    s32 discardFrom;    // +0x00C  0 carried, 1 stock
    ItemContext context;// +0x010
    // from menu/ItemSelect layout parameters (asset 0x5D), pixels from the top left
    s32 tabPos[3][2];   // +0x018
    s32 iconBox[4];     // +0x030
    s32 arrowPos[2];    // +0x040
    s32 textBox[4];     // +0x048
    s32 hintPos[2][2];  // +0x058
    u32 description;    // +0x068  const u16*
    ItemTabs tabs;      // +0x06C
    ItemList carried;   // +0x07C
    ItemList stock;     // +0x0D0
    PadRepeatPair upDown;   // +0x124
    u32 params;         // +0x168  ParamFile*
};

extern "C" {
    IconSet*  IconSet_Ctor(IconSet* self);                      // 0x234240
    void      IconSet_Load(IconSet* self);                      // 0x234010
    char      IconSet_IsLoading(IconSet* self);                 // 0x233F00
    ItemInfo* IconSet_Get(IconSet* self, u32 item);             // 0x233EC0
    int       Icon_Draw(ItemInfo* item, const void* params);    // 0x234390  SpriteSet_DrawEx params
    IconSet*  IconSet_Dtor(IconSet* self, s16 flag);            // 0x234150
    // List position (0-based, +1) → item id: ids skip 10.
    int       Item_FromIndex(int index);                        // 0x233340

    ItemSelect* ItemSelect_Ctor(ItemSelect* self);              // 0x270CD0
    char ItemSelect_Start(ItemSelect* self);                    // 0x270AF0
    // true when the screen is finished (or could not load)
    bool ItemSelect_Update(ItemSelect* self);                   // 0x270440
    void ItemSelect_Draw(ItemSelect* self);                     // 0x26F390
    ItemSelect* ItemSelect_Dtor(ItemSelect* self, s16 flag);    // 0x270BE0
}
