#pragma once
#include "engine/containers.h"
#include "engine/text_label.h"
#include "game/chara_resources.h"
#include "hal/ps2_types.h"

// Speech on screen (0x188250–0x18BB10, 0x1910A0–0x191B10): hand-drawn-looking
// balloons that point at whoever is talking, and the window choices are
// picked from.  The conversation manager (talk_mgr) creates them.

// A balloon's outline: a fan of points around its centre, in 1/16 pixel.
struct BalloonShape {
    GuestVector points;     // +0x00  {s32 x, y}[]; the first is the centre
    u8    _0c[0x14];
    float scale;            // +0x20
    u32   vtable;           // +0x24  +0x08 Destroy, +0x0C Build(w, h), +0x10 Draw(pos, rgba)
};

// What every speech widget is shown with.
struct SpeechParams {
    u32 speaker;            // CharaParams*
    u32 listener;           // CharaParams*
    s32 frames;             // −1 = until closed
    s32 style;              // 0 speech, 1 shout
    s32 direction;          // where the balloon sat last time, −1
    u32 text;               // u16*
    u32 flags;              // 1: at a fixed place
    s32 x, y;
};

struct SpeechWidget {
    u32   speaker;          // +0x00
    u32   listener;         // +0x04
    u32   shape;            // +0x08  BalloonShape*
    s32   x, y;             // +0x0C  where it sits (centre), 1/16 pixel
    s32   halfW, halfH;     // +0x14  of the text
    u32   _1c[2];
    s32   anchorX, anchorY; // +0x24  the end of the tail
    s32   kind;             // +0x2C  1 balloon, 2 choices, 3 remark
    s32   _30;
    s32   appear;           // +0x34  0–3 while it grows
    u32   z;                // +0x38
    s32   closing;          // +0x3C
    WString text;           // +0x40
    u32   textVtable;       // +0x4C
    float scale;            // +0x50
    u32   vtable;           // +0x54  +0x08 Destroy, +0x0C Show(params), +0x10 Update → 0 when gone, +0x14 Draw
};

struct SpeechBalloon {
    SpeechWidget w;
    s32 frames;             // +0x58  left; −1 = until closed
    s32 direction;          // +0x5C  which side of the speaker it found room on
    s32 style;              // +0x60
    s32 onScreen;           // +0x64  the speaker is in view (the tail is drawn)
};

// A balloon at a place on the screen the script chose (0x70 bytes).
struct PlacedBalloon {
    SpeechBalloon b;
    s32 x, y;               // +0x68
};

struct ChoiceWindow {
    SpeechWidget w;
    s32 selection;          // +0x58  highlighted line, from 1; 0 none
};

// The widgets on screen (0x24 bytes).
struct SpeechList {
    GuestList widgets;      // +0x00
    u32 _0c[6];
};

extern "C" {
    SpeechList* SpeechList_Ctor(void* mem);                         // 0x188870
    SpeechList* SpeechList_Dtor(SpeechList* self, s16 flag);        // 0x188760
    int   SpeechList_Draw(SpeechList* self);                        // 0x1885E0
    int   SpeechList_Update(SpeechList* self);                      // 0x188640
    void  SpeechWidget_Close(SpeechWidget* self);                   // 0x188310
    SpeechBalloon* SpeechBalloon_Ctor(void* mem);                   // 0x189E90  0x68 bytes
    SpeechBalloon* SpeechRemark_Ctor(void* mem);                    // 0x256FB0  0x68 bytes
    PlacedBalloon* SpeechPlaced_Ctor(void* mem);                    // 0x23A340  0x70 bytes
    ChoiceWindow*  ShopMessage_Ctor(void* mem);                     // 0x23A190  0x5C bytes: the shop's yes / no box
    void           ShopMessage_SetSelection(ChoiceWindow* self, int line);  // 0x239D50
    ChoiceWindow*  ChoiceWindow_Ctor(void* mem);                    // 0x191A90  0x5C bytes
    int   ChoiceWindow_SetSelection(ChoiceWindow* self, int line);  // 0x1917E0

    // World → screen (1/16 pixel).  Not decompiled (native camera); see hle.
    int   GsDevice_Project(void* dev, s32* out, const float* world);    // 0x12F7E0  0 = behind the camera
}
