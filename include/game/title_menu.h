#pragma once
#include "hal/ps2_types.h"

// ── Keyframe track (0x20 bytes) ───────────────────────────────────────────
// A float value animated over a fixed number of frames by linear interpolation
// between keys.  Key times are given in 1/30 s and converted to frames.
struct TrackKey {
    s32   frame;
    float value;
};

struct Track {
    u32 capacity;   // +0x00  CodeWarrior std::vector<TrackKey>
    u32 count;      // +0x04
    u32 keys;       // +0x08  TrackKey*
    s32 length;     // +0x0C  frames
    s32 frame;      // +0x10  current frame
    u32 keyA;       // +0x14  key at or before the current frame
    u32 keyB;       // +0x18  next key
    s32 loopKey;    // +0x1C  key to jump back to at the end, or −1
};
static_assert(sizeof(Track) == 0x20, "Track size");

// ── Title background: the 200-frame intro animation (0x194 bytes) ─────────
struct TitleBg {
    u32   bgSprites;       // +0x00  → TitleMenu::sprites51  (BG.bpm)
    u32   mistSprites;     // +0x04  → TitleMenu::sprites54  (Kasumi.bpm)
    u32   menuSprites;     // +0x08  → TitleMenu::sprites52  (ImgA.bpm: logo, text)
    u32   patternSprites;  // +0x0C  → TitleMenu::sprites53  (ImgB.bpm: patterns, waves)
    s32   frame;           // +0x10  0 … 200; the menu is usable once it reaches 200
    Track tracks[12];      // +0x14
};
static_assert(sizeof(TitleBg) == 0x194, "TitleBg size");

// ── Menu entry (0x18 bytes) ───────────────────────────────────────────────
struct MenuItem {
    s32 sprite;       // +0x00  base sprite; +1 = highlighted, +2 = glow overlay
    s32 result;       // +0x04  code returned to GameScene when confirmed
    s32 field_08;
    s32 glow;         // +0x0C  0 … one second, drives the overlay's alpha
    s32 glowRising;   // +0x10
    u32 spriteSet;    // +0x14  SpriteSet** (address of the owning set pointer)
};
static_assert(sizeof(MenuItem) == 0x18, "MenuItem size");

// CodeWarrior std::vector<MenuItem>
struct MenuItemList {
    u32 capacity;
    u32 count;
    u32 data;         // MenuItem*
};

// ── TitleMenu: GameScene mode 2 (0x1CC bytes) ─────────────────────────────
enum TitleMenuState : u32 {
    kTitlePressStart = 0,
    kTitleMainMenu   = 1,
    kTitleSubMenu    = 2,   // difficulty: Easy / Normal / Hard / Death → PlayerData+0x105
};

// Values TitleMenu_Update returns to GameScene (0 = keep running).
enum TitleMenuResult : s32 {
    kTitleResultNone      = 0,
    kTitleResultTimeout   = 1,  // idle on "press start" → Attract scene
    kTitleResultStart     = 2,  // Start pressed → GameScene mode 1
    kTitleResultNewGame   = 3,  // "Start" (after difficulty select) → mode 3 / Gameplay
    kTitleResultRecords   = 7,  // "Records" → mode 4
    kTitleResultSettings  = 8,  // "Settings" → mode 5
};

struct TitleMenu {
    u32          state;       // +0x00  TitleMenuState
    u32          loadStep;    // +0x04  1–4 while the sprite sets load, 5 = ready
    u32          sprites51;   // +0x08  archive file 0x51: background
    u32          sprites52;   // +0x0C  archive file 0x52: menu text
    u32          sprites53;   // +0x10  archive file 0x53
    u32          sprites54;   // +0x14  archive file 0x54: logo
    s16          idleTimer;   // +0x18  frames left before returning to the attract movies
    s16          mainSel;     // +0x1A
    s16          subSel;      // +0x1C
    s16          _pad1e;
    MenuItemList mainItems;   // +0x20
    MenuItemList subItems;    // +0x2C
    TitleBg      bg;          // +0x38
};
static_assert(sizeof(TitleMenu) == 0x1CC, "TitleMenu size");

extern "C" {
    TitleMenu* TitleMenu_Ctor(TitleMenu* self);              // 0x26D280
    char       TitleMenu_Load(TitleMenu* self);              // 0x26D0D0
    void       TitleMenu_Restart(TitleMenu* self);           // 0x26CE60
    int        TitleMenu_Update(TitleMenu* self);            // 0x26CCC0
    void       TitleMenu_Draw(TitleMenu* self);              // 0x26BF40
    TitleMenu* TitleMenu_Dtor(TitleMenu* self, s16 flag);    // 0x26D180

    TitleBg* TitleBg_Ctor(TitleBg* self, u32* bg, u32* mist, u32* menu, u32* pattern);  // 0x26E1B0
    void     TitleBg_Reset(TitleBg* self);                   // 0x26E0A0
    int      TitleBg_Update(TitleBg* self);                  // 0x26DCE0
    void     TitleBg_Draw(TitleBg* self);                    // 0x26DA10
    TitleBg* TitleBg_Dtor(TitleBg* self, s16 flag);          // 0x26E0D0

    void  Track_Init(float value, Track* track, int length);     // 0x225210
    void  Track_AddKey(float value, Track* track, int time);     // 0x225160
    void  Track_Step(Track* track);                              // 0x225070
    float Track_Value(const Track* track);                       // 0x224FE0
    void  Track_SkipToEnd(Track* track);                         // 0x225130
    void  Track_Dtor(Track* track, s16 flag);                    // 0x2252E0
}
