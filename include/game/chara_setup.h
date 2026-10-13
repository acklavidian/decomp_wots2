#pragma once
#include "hal/ps2_types.h"
#include "engine/gs_device.h"
#include "engine/pad_repeat.h"

// Character setup — the new-game screen (GameScene mode 3): a six-entry menu
// (Name, Head, Clothes, Sword, Items, Done) beside a 3D preview of the
// character.  Decompiled from 0x24CCE0–0x24ECF0 and 0x254300–0x256760.
//
// (Earlier notes called this "ScriptGroupB".)

struct ScreenLayout;
struct CharaMgr;

// What the parts share: CharaSetup +0x3C.
struct SetupContext {
    u32 layout;         // +0x00  ScreenLayout*  (layout 8, CharacterSetup)
    u32 hint;           // +0x04  SetupHint*
    u32 iconSet;        // +0x08  item icons
    u8  gender;         // +0x0C  0 male, 1 female: which clothes fit the chosen head
};

// One line of help text at layout group 0.
struct SetupHint {
    u32 text;           // u16 string
    s32 x, y;           // layout coordinates
};

// The six menu entries.  The selected one slides sideways.
struct SetupMenuEntry {
    s16 slideStep;      // +0x00  pixels per frame, 0 = at rest
    s16 slide;          // +0x02
    s16 lit;            // +0x04
    s16 _06;
    u32 label;          // +0x08
    s32 x, y;           // +0x0C
};

struct SetupMenu {
    s16 cursor;                 // +0x00
    s16 slideMax;               // +0x02
    SetupMenuEntry entry[6];    // +0x04
    PadRepeatPair upDown;       // +0x7C
    u32 context;                // +0xC0  SetupContext*
};

enum SetupMenuItem : s16 {
    kSetupName, kSetupHead, kSetupClothes, kSetupSword, kSetupItems, kSetupDone,
};

// A grid of choices with a frame cursor.  Base of the two pickers.
struct GridPicker {
    s32 count;              // +0x00  number of choices
    s32 columns, rows;      // +0x04
    s32 panelGroup;         // +0x0C  layout groupB of the whole panel
    s32 choiceGroup;        // +0x10  layout group of choice 0's "chosen" marker (−2, −1: frame parts)
    s32 cellGroup;          // +0x14  layout group of choice 0's cell
    s32 lockGroup;          // +0x18  layout group of choice 0's "not available" cover
    s32 chosen;             // +0x1C  choice in effect
    s32 chosenColumn, chosenRow;    // +0x20
    s32 column, row;        // +0x28  cursor
    FrameRect cursor;       // +0x30
    PadRepeatPair upDown;   // +0x4C
    PadRepeatPair rightLeft;// +0x90
    u8  disabled;           // +0xD4  a secret character is shown; the grid does not apply
    u32 context;            // +0xD8  SetupContext*
    u32 vtable;             // +0xDC
};

enum GridPickerSlot : u32 {
    kPickerSetHint     = 0x08,  // (self)
    kPickerIsAvailable = 0x0C,  // (self, choice) → bool
    kPickerDtor        = 0x10,  // (self, s16 flag)
    kPickerCurrent     = 0x14,  // (self) → choice under the cursor; 0x100 / 0x200 = empty cell
};

struct HeadPicker {
    GridPicker base;
    s32 choice[2][5];       // +0xE0
    u8  available[8];       // +0x108
};

struct ClothesPicker {
    GridPicker base;
    s32 choice[2][7];       // +0xE0
    u8  available[13];      // +0x118
    FrameRect cover[13];    // +0x128  drawn over clothes the chosen head cannot wear
};

// The two swords carried: name and remaining number.
struct SetupSwords {
    s32 rect[2][4];         // +0x00
    s32 namePos[2][2];      // +0x20
    s32 labelPos[2][2];     // +0x30
    s32 countPos[2][2];     // +0x40
    u32 context;            // +0x50
};

// Up to eight item icons.
struct SetupItems {
    s32 rect[8][4];         // +0x00
    u32 context;            // +0x80
};

enum SetupSub : s32 {
    kSubMenu = 0, kSubNameInput = 1, kSubHead = 2, kSubClothes = 3,
    kSubSwords = 4, kSubItems = 5, kSubSwordDetail = 6,
};

enum SetupTransition : s32 {
    kTransNone = 0,
    kTransFadeOut = 1,      // counting down, then the sub-screen changes
    kTransLoading = 2,      // waiting for the new sub-screen
    kTransFadeIn = 3,
    kTransApply = 4,        // a character change is waiting for the previous load
    kTransLeave = 5,        // waiting for the character to finish loading, then return `result`
};

struct CharaSetup {
    s32 phase;              // +0x000  1 layout loading, 2 icons loading, 3 ready
    s32 transition;         // +0x004  SetupTransition
    s32 result;             // +0x008
    s32 sub;                // +0x00C  SetupSub
    s32 nextSub;            // +0x010
    s32 countdown;          // +0x014
    s32 charaId;            // +0x018  character shown (index into Common/chara/list)
    s32 head;               // +0x01C
    s32 needSword;          // +0x020  "Done" was refused for lack of a sword
    s32 swordDetail;        // +0x024
    u8  secret;             // +0x028  a secret character is in effect
    s32 codeStep;           // +0x02C  progress through the secret button sequence
    s32 codeTimer;          // +0x030
    s32 secretOpen;         // +0x034  the secret character list is being browsed
    s32 secretIndex;        // +0x038
    SetupContext context;   // +0x03C
    u32 charaMgr;           // +0x04C
    SetupHint     hint;     // +0x050
    SetupMenu     menu;     // +0x05C
    HeadPicker    heads;    // +0x120
    ClothesPicker clothes;  // +0x230
    SetupSwords   swords;   // +0x4C4
    SetupItems    items;    // +0x518
    u32 nameInput;          // +0x59C
    u32 swordSelect;        // +0x5A0
    u32 itemSelect;         // +0x5A4
    u32 swordInfo;          // +0x5A8
};

// Results of CharaSetup_Update.
enum { kSetupRunning = 0, kSetupBack = 1, kSetupStartGame = 2 };

extern "C" {
    CharaSetup* CharaSetup_Ctor(CharaSetup* self);                      // 0x24ECF0
    void CharaSetup_Attach(CharaSetup* self, u32* charaMgr);            // 0x24E7E0
    int  CharaSetup_Update(CharaSetup* self);                           // 0x24E0E0
    void CharaSetup_Draw(CharaSetup* self);                             // 0x24D0B0
    CharaSetup* CharaSetup_Dtor(CharaSetup* self, s16 flag);            // 0x24EBB0
}
