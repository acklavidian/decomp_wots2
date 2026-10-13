#pragma once
#include "engine/containers.h"
#include "engine/text_label.h"
#include "hal/ps2_types.h"

// The in-world HUD (0x21C900–0x220630), the object at *0x2EE69C: a list of
// widgets drawn over the 3D view with the sprites of asset 0x68.
//
// Every widget starts {kind, vtable}; the vtable holds
//   +0x08 Destroy(self, flag)   +0x0C Show(self, params, arg)
//   +0x10 Update(self) → 0 when finished   +0x14 Draw(self, sprites)

// The frame behind a text widget.
struct HudBox {
    s32   w, h;         // +0x00  of the text, 1/16 pixel
    s32   _8;
    s32   frame;        // +0x0C  6–18: the glow flashes
    float alpha;        // +0x10
};

// How a text widget looks (HudTextParams::type).
enum HudTextType : s32 {
    kHudPlain = 0,      // light frame, fades in and out
    kHudDark = 1,       // dark frame; hidden while effect 8 runs
    kHudSlideA = 2,     // slide in from the right
    kHudSlideB = 3,
    kHudSlideDimA = 4,  // …at half strength
    kHudSlideDimB = 5,
    kHudDefault = 7,    // the same as kHudPlain
};

struct HudTextParams {
    s32 type;
    s32 time;           // frames; −1 = until closed
    u32 text;           // const u16*
};

// A line of text in a frame at the bottom right (0x21ED50–0x21F900).
struct HudText {
    s32     kind;       // +0x00  1
    u32     vtable;     // +0x04  0x2EC470
    u32     box;        // +0x08  HudBox*
    WString string;     // +0x0C
    u32     labelVtable;// +0x18  0x2EAF30
    u32     _1c;
    s32     x, y;       // +0x20  centre, 1/16 pixel
    u32     z;          // +0x28
    u32     _2c;
    s32     halfW, halfH;   // +0x30
    s32     time;       // +0x38  frames left; −1 = stays
    s32     type;       // +0x3C
    s32     fade;       // +0x40  5 → 0
    s32     state;      // +0x44  0 appearing, 1 leaving, 2 flashing before it leaves
    u32     _48[2];
};

// A stacking notice (0x21EAC0–0x21ECC0): each one sits above the last.
struct HudNotice {
    HudText text;
    s32     stackY;     // +0x50
    u32     _54[3];
};

// The name plate over whoever the player can talk to (0x21E160–0x21EA80).
// It has to be shown again every frame to stay.
struct HudTalkPrompt {
    s32 kind;           // +0x00
    u32 vtable;         // +0x04  0x2EC430
    u32 params;         // +0x08  CharaParams* of the person
    u32 name;           // +0x0C  u16*
    s32 x, y;           // +0x10  their head on screen
    s32 frames;         // +0x18  left to show; −1 = delete
    s32 special;        // +0x1C  1: the purple plate
    s32 onScreen;       // +0x20
};

struct HudTalkInfo { u32 params; u32 name; s32 frames; s32 special; };

struct Hud {
    GuestList widgets;  // +0x00
    u32 _0c[3];
    u32 date;           // +0x18  HudText*
    u32 prompt;         // +0x1C  HudText*
    u32 hint;           // +0x20  HudText* (also the area name)
    u32 _24;
    u32 gauges;         // +0x28
    u32 overlay;        // +0x2C  drawn last; not in the list
    u32 talkPrompt;     // +0x30  HudTalkPrompt*
    u32 death;          // +0x34  drawn alone while it exists
    u32 _38[3];
    u32 message;        // +0x44
    u32 sprites;        // +0x48  SpriteSet*, asset 0x68
    u32 _4c;
    s32 hintTimer;      // +0x50  frames until the area name leaves; −1 for a hint
};

extern "C" {
    void* Hud_Ctor(void* self);                                     // 0x21DD70
    void  Hud_ShowKanban(void* hud, int which);                     // 0x21CA60  a shop's signboard
    void  Hud_HideKanban(void* hud);                                // 0x21CA20
    void  Hud_ShowMoney(void* hud, int x, int y);                   // 0x21DC60  the player's money
    void  Hud_HideMoney(void* hud);                                 // 0x21DC20
    void  Hud_Load(void* hud);                                      // 0x21DD10  loads the sprites
    void  Hud_Reset(void* hud);                                     // 0x21D8F0  frees everything
    void  Hud_Update(void* hud);                                    // 0x21DAB0
    void  Hud_Draw(void* hud);                                      // 0x21D9E0
    void  Hud_ShowHint(void* hud, const u16* text, int type);       // 0x21D720
    void  Hud_HideHint(void* hud);                                  // 0x21D6E0
    void  Hud_ShowAreaName(void* hud, const u16* text, int frames, int type);   // 0x21D7E0
    void  Hud_ShowDate(void* hud, const u16* text, int frames, int type);       // 0x21D510
    void  Hud_ClearChoice(void* hud);                               // 0x21D4D0  closes the date line
    void  Hud_ShowNotice(void* hud, const u16* text, int frames, int type);     // 0x21D630
    int   Hud_FlashPrompt(void* hud);                               // 0x21DFA0
    void  Hud_ShowTalkPrompt(void* hud, const void* info);          // 0x21CD60  HudTalkInfo
    void Hud_SetFriend(void* hud, struct CharaParams* who);            // 0x21CB20 / 0x21CAF0 (null)
}
