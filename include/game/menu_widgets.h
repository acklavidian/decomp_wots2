#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"
#include "engine/pad_repeat.h"
#include "engine/param_file.h"

// The Settings screen's menu: rows with a label and a value that can be
// changed with left/right, plain buttons, and a pinwheel cursor.
//
// Class layout (vtables in the game's data segment):
//
//   MenuItem            0x2ED040   label at a position           (never created on its own)
//   ├─ MenuRow          0x2ED000   slides right when selected, shows a MenuValue
//   └─ MenuButton       0x2ECFC0   fades between two backgrounds
//
//   MenuValue           0x2EB290
//   ├─ ChoiceValue      0x2ED0D0   one of a list of strings
//   ├─ GaugeValue       0x2ED0F0   ten-step volume bar
//   └─ NumberValue      0x2ED0B0   a number picked from a scrolling strip of five

struct SpriteSet;

// ── Values ────────────────────────────────────────────────────────────────
struct MenuValue {
    s32 x, y;           // +0x00  from the parameter file
    s32 offsetX;        // +0x08  set by the row that shows it (its slide)
    s32 offsetY;        // +0x0C
    s16 blink;          // +0x10  arrow animation counter
    u32 vtable;         // +0x14
};

enum MenuValueSlot : u32 {
    kValueDtor = 0x08,      // (self, s16 flag)
    kValueNext = 0x0C,      // (self) → changed
    kValuePrev = 0x10,      // (self) → changed
    kValueDraw = 0x14,      // (self, bool selected, u8 alpha, SpriteSet*)
};

struct ChoiceEntry {
    u32 text;           // u16 string
    s16 width;          // pixels, plus 24
    s16 height;
};

struct ChoiceValue {
    MenuValue   base;
    GuestVector entries;    // +0x18  ChoiceEntry
    s32         index;      // +0x24
};

struct GaugeValue {
    MenuValue base;
    s32 value;          // +0x18  0–10
    s32 shown;          // +0x1C  value before the last change, while it fades
    s32 timer;          // +0x20
};

struct NumberValue {
    MenuValue base;
    s32 value;          // +0x18  0-based; shown as value + 1
    s32 max;            // +0x1C
    s32 top;            // +0x20  first of the five numbers on screen
};

// The Settings screen's nine values, in one block.
struct SettingsValues {
    ChoiceValue camera;      // +0x000  Normal / Reverse
    ChoiceValue vibration;   // +0x028  Off / On
    ChoiceValue subtitles;   // +0x050  Off / On
    ChoiceValue blood;       // +0x078  Off / On
    ChoiceValue audio;       // +0x0A0  STEREO / MONO
    GaugeValue  bgmVolume;   // +0x0C8
    GaugeValue  seVolume;    // +0x0EC
    NumberValue bgmTest;     // +0x110
    NumberValue seTest;      // +0x134
};

// ── Items ─────────────────────────────────────────────────────────────────
// What a panel's items share: the first three words of the screen that owns it.
struct MenuContext {
    u32 spriteSet;      // SpriteSet*
    u32 params;         // ParamFile*
    u32 values;         // SettingsValues*
};

struct MenuItem {
    s32 x, y;           // +0x00  from the parameter file
    s32 width, height;  // +0x08
    u32 label;          // +0x10  u16 string
    u32 value;          // +0x14  MenuValue*, or 0
    u32 context;        // +0x18  MenuContext*
    u32 vtable;         // +0x1C
};

enum MenuItemSlot : u32 {
    kItemGetPos       = 0x08,   // (s32 out[2], self)
    kItemDtor         = 0x0C,   // (self, s16 flag)
    kItemSetLabel     = 0x10,   // (self, const u16*)
    kItemLoad         = 0x14,   // (self)  reads x, y from the parameter file
    kItemUpdate       = 0x18,   // (self)
    kItemDraw         = 0x1C,   // (self)
    kItemFocus        = 0x20,   // (self)  cursor arrived
    kItemBlur         = 0x24,   // (self)  cursor left
    kItemHide         = 0x28,   // (self, bool atOnce)
    kItemShow         = 0x2C,   // (self, bool selected)
    kItemConfirm      = 0x30,   // (self) → int
    kItemIsSelected   = 0x34,   // (self) → bool
    kItemGetCursorPos = 0x38,   // (s32 out[2], self)
};

// MenuRow states
enum : s32 {
    kRowIdle        = 0,
    kRowFocusing    = 1,      // sliding out to the selected position
    kRowBlurring    = 2,
    kRowSelected    = 4,
    kRowHiding      = 8,
    kRowShowing     = 0x10,   // appearing unselected
    kRowShowingSel  = 0x20,   // appearing selected
    kRowHidden      = 0x40,
};

struct MenuRow {
    MenuItem base;
    s32 state;          // +0x20
    s32 slide;          // +0x24  x offset
    s32 slideTarget;    // +0x28
    s32 timer;          // +0x2C
    s32 duration;       // +0x30
};

// MenuButton states
enum : s32 {
    kButtonIdle     = 0,
    kButtonPressed  = 1,
    kButtonHiding   = 2,
    kButtonShowing  = 4,
    kButtonHidden   = 8,
    kButtonFocusing = 0x10,
    kButtonBlurring = 0x20,
    kButtonSelected = 0x40,
};

struct MenuButton {
    MenuItem base;
    s32 state;          // +0x20
    s32 timer;          // +0x24
    s32 duration;       // +0x28
};

// ── Cursor ────────────────────────────────────────────────────────────────
// A pinwheel whose speed drifts at random and jumps when a choice is made.
struct MenuCursor {
    s32   timer;        // +0x00  frames until the drift changes
    float accel;        // +0x04
    float speed;        // +0x08  degrees per frame
    float angle;        // +0x0C
    u8    kicked;       // +0x10
};

// ── Panel ─────────────────────────────────────────────────────────────────
enum SettingsItem {
    kItemCamera, kItemVibration, kItemSubtitles, kItemBlood, kItemSound,
    kItemScreenPos, kItemColorBars, kItemReset, kItemEnd,
    // the Sound sub-menu
    kItemAudio, kItemBgmTest, kItemBgmVolume, kItemSeTest, kItemSeVolume, kItemSoundEnd,
    kSettingsItemCount,
};

// What SettingsPanel_Update reports.
enum SettingsResult {
    kSettingsNone         = 0,
    kSettingsEnd          = 1,
    kSettingsScreenPos    = 2,
    kSettingsColorBars    = 3,
    kSettingsReset        = 4,
    kSettingsResetVolumes = 5,   // never returned by the code as shipped
};

struct SettingsPanel {
    MenuRow    camera;       // +0x000
    MenuRow    vibration;    // +0x034
    MenuRow    subtitles;    // +0x068
    MenuRow    blood;        // +0x09C
    MenuRow    sound;        // +0x0D0
    MenuRow    screenPos;    // +0x104
    MenuRow    colorBars;    // +0x138
    MenuButton reset;        // +0x16C
    MenuButton end;          // +0x198
    MenuRow    audio;        // +0x1C4
    MenuRow    bgmTest;      // +0x1F8
    MenuRow    bgmVolume;    // +0x22C
    MenuRow    seTest;       // +0x260
    MenuRow    seVolume;     // +0x294
    MenuButton soundEnd;     // +0x2C8
    u32 items[kSettingsItemCount];   // +0x2F4  MenuItem*: 9 main entries, then the 6 sound entries
    u32 context;             // +0x330  MenuContext*
    u32 vtable;              // +0x334
    s32 cursor;              // +0x338  index into `list`
    s32 _33c;
    u32 list;                // +0x340  &items[0] or &items[9]
    s32 listCount;           // +0x344
    PadRepeatPair upDown;    // +0x348
    PadRepeatPair rightLeft; // +0x38C
    MenuCursor pinwheel;     // +0x3D0
    s32 testBgm;             // +0x3E4  tune started from BGM Test, or −1
    s32 savedBgm;            // +0x3E8  tune to restart on leaving the sound menu, or −1
};

extern "C" {
    SettingsValues* SettingsValues_Ctor(SettingsValues* self);                   // 0x283A90
    void SettingsValues_Dtor(SettingsValues* self);                              // inlined in 0x192410
    void SettingsValues_LoadPositions(SettingsValues* self, ParamFile* params);  // 0x283600
    void SettingsValues_FromConfig(SettingsValues* self);                        // 0x2839B0
    void SettingsValues_ToConfig(SettingsValues* self);                          // 0x283950
    void SettingsValues_ResetAll(SettingsValues* self);                          // 0x283450
    void SettingsValues_ResetVolumes(SettingsValues* self);                      // 0x2833A0

    SettingsPanel* SettingsPanel_Ctor(SettingsPanel* self, MenuContext* context);// 0x2849B0
    SettingsPanel* SettingsPanel_Dtor(SettingsPanel* self, s16 flag);            // 0x2848F0
    void SettingsPanel_LinkValues(SettingsPanel* self);                          // 0x284FB0
    void SettingsPanel_LoadPositions(SettingsPanel* self);                       // 0x284B00
    int  SettingsPanel_Update(SettingsPanel* self);                              // 0x2846E0
    void SettingsPanel_Draw(SettingsPanel* self);                                // 0x283D70
    // Which prompt to show: 2 "Play", 1 "Select", 0 none.
    int  SettingsPanel_GetHint(SettingsPanel* self);                             // 0x283EC0
}
