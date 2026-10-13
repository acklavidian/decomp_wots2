#pragma once
#include "hal/ps2_types.h"
#include "engine/pad_repeat.h"

// The pause menu (Start) and the other lists of buttons that slide in from
// the right: answers to a question, the rest menu.  Decompiled from
// 0x1721E0–0x174560.  0x18C bytes; owned by the Gameplay scene.

struct SpriteSet;

// One button.  It eases towards its target a fixed share of the way each frame.
struct SlideEntry {
    float x, y;             // +0x00  top-left, layout coordinates
    float targetX, targetY; // +0x08
    float ease;             // +0x10  the share is 1 / ease
    s32   steps;            // +0x14
    s32   step;             // +0x18
    u32   text;             // +0x1C  const u16*
    u16   id;               // +0x20  what choosing it means (see PauseEntry)
    u16   _22;
};

enum SlidePhase : s16 { kSlideIn = 1, kSlideSettle = 2, kSlideReady = 4, kSlideOut = 8, kSlideGone = 0x10 };

struct SlideList {
    s16 phase;              // +0x00
    s16 stepTimer;          // +0x02  frames until the next button starts moving
    s16 next;               // +0x04  next button to move
    u16 count;              // +0x06
    s16 cursor;             // +0x08
    s16 phaseBefore;        // +0x0A
    SlideEntry entry[7];    // +0x0C
    s16 confirm;            // +0x108  frames since Cross was pressed
    s16 _10a;
};

// Entry ids.
enum PauseEntry : u16 {
    kPauseSwords = 0, kPauseItems = 1, kPauseStatus = 2,   // the three pages of the status menu
    kPauseSettings = 3, kPauseResume = 4,
    kPauseRest = 5, kPauseSave = 6, kPauseBack = 7, kPauseSwordBox = 9, kPauseItemBox = 10,
    kPauseAnswer1 = 0xE, kPauseAnswerNone = 0x14,
};

struct PauseMenu {
    u8  dim;                // +0x000  how dark the screen behind is
    s32 open;               // +0x004
    s32 state;              // +0x008  0 closed, 1 shown, 3 sliding away
    s32 _0c;                // +0x00C  -1
    s32 allowed;            // +0x010  which list opens: 0 none, 1 world, 2 story scene, 4 area map,
                            //         8 rest, 0x10 a question, 0x20 the storehouse
    s32 allowedBefore;      // +0x014
    s32 _18;                // +0x018  list to open once this one has closed
    SlideList list;         // +0x01C
    u8  chosen;             // +0x128  an entry was confirmed this frame
    s32 chosenId;           // +0x12C  -1 = the entry under the cursor
    PadRepeatPair upDown;   // +0x130
    u32 sprites;            // +0x174  SpriteSet* (asset 0x55)
    u32 gsDevice;           // +0x178
    u32 _17c, _180;
    u32 font;               // +0x184
    u32 windowMgr;          // +0x188
};

extern "C" {
    PauseMenu* PauseMenu_Ctor(PauseMenu* self);                 // 0x172B10
    PauseMenu* PauseMenu_Dtor(PauseMenu* self, s16 flag);       // 0x172A80
    int  PauseMenu_Poll(PauseMenu* self);                       // 0x172A50  0 until its sprites are loaded
    int  PauseMenu_Update(PauseMenu* self);                     // 0x172890  → open
    void PauseMenu_Open(PauseMenu* self, int force);            // 0x172810
    void PauseMenu_Close(PauseMenu* self, int sound);           // 0x1727D0
    int  PauseMenu_Back(PauseMenu* self);                       // 0x1726F0  starts sliding away
    void PauseMenu_Draw(PauseMenu* self);                       // 0x1721E0
}
