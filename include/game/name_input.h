#pragma once
#include "hal/ps2_types.h"
#include "engine/gs_device.h"
#include "engine/text_label.h"

// Name entry: an on-screen keyboard (layout 0, NameInput).  Decompiled from
// 0x17C8A0–0x17CE70, 0x275CC0–0x278360 and the text label at 0x224920–0x224F60.
//
// Controls: d-pad moves, Cross types, Circle deletes, L1 / R1 move the caret,
// Triangle jumps to "Done".

// One set of characters: `columns` to a row, in groups of `group`.
struct CharPage {
    u32 chars;          // +0x00  u16[]; 0xFFFF = blank cell
    s32 count;          // +0x04
    s32 columns;        // +0x08
    s32 rows;           // +0x0C
    s32 group;          // +0x10
    s32 lastRow;        // +0x14  cells in the last row
};

struct KeyRepeat {
    u32 fired;          // buttons that count this frame
    u32 last;
    s32 counter;
    s32 stage;          // 0 before the first repeat, 1 after
};

struct NameField {
    u16 text[16];       // +0x00
    s16 caret;          // +0x20
    s16 length;         // +0x22
    s32 x, y;           // +0x24
    s16 blinkTimer;     // +0x2C
    u8  caretVisible;   // +0x2E
    u32 context;        // +0x30
    s16 leftFlash;      // +0x34  frames the L1 / R1 arrows stay nudged
    s16 rightFlash;     // +0x36
};

// The column of buttons beside the keyboard; this release has only "Done".
struct NameMenu {
    s32 chosen;         // +0x00
    s32 cursor;         // +0x04
    s32 toGrid;         // +0x08  Left / Right was pressed
    TextLabel label;    // +0x0C
    u32 context;        // +0x38
};

struct CharGrid {
    s32 column, row;            // +0x00
    s32 savedColumn, savedRow;  // +0x08
    s32 scroll;                 // +0x10  first row shown
    s32 level;                  // +0x14
    u8  offEdge;                // +0x18  the cursor left the grid sideways
    u8  cancelled;              // +0x19
    u32 page;                   // +0x1C  CharPage*
    u32 context;                // +0x20
    u8  wrapped;                // +0x24
};

struct NamePrompts {
    u32 context;        // +0x00
    TextLabel enter;    // +0x04
    TextLabel erase;    // +0x30
};

struct NameInput {
    s32 phase;          // +0x000  1 layout, 2 sprites, 3 character file, 4 ready, 0 failed
    s16 mode;           // +0x004  2 = opened from character setup
    CharPage page0;     // +0x008  the keyboard
    CharPage pages[90]; // +0x020  per-character sub-pages (unused in this release)
    s32 cursor[4];      // +0x890  x0, y0, x1, y1 in layout coordinates
    KeyRepeat repeat;   // +0x8A0
    u32 spriteSet;      // +0x8B0  ui/NameInput/Img.bpm
    u32 layout;         // +0x8B4
    u32 charFile;       // +0x8B8  AssetBuffer*: ui/NameInput/CharGlance.txt
    s32 focus;          // +0x8BC  0 keyboard, 1 menu
    NameField field;    // +0x8C0
    NameMenu  menu;     // +0x8F8
    CharGrid  grid;     // +0x934
    NamePrompts prompts;// +0x95C
};

extern "C" {
    NameInput* NameInput_Ctor(NameInput* self);                         // 0x17CE70
    int  NameInput_Setup(NameInput* self, const u16* name, int mode);   // 0x17CBF0
    // 1 once the name has been confirmed (or the screen could not load).
    int  NameInput_Update(NameInput* self);                             // 0x17CA70
    void NameInput_Draw(NameInput* self);                               // 0x17C8A0
    NameInput* NameInput_Dtor(NameInput* self, s16 flag);               // 0x17CD60
}
