// Decompiled from 0x26BF40 – 0x26D2FC
//
// TitleMenu — GameScene mode 2.  Three states:
//
//   PressStart  intro animation (200 frames, Start skips it), then a blinking
//               "press start".  Start → result 2.  30 s idle → result 1.
//   MainMenu    three entries, Up/Down to move, Cross to confirm.
//   SubMenu     up to four entries, unlocked by PlayerData+8 reaching the
//               thresholds at 0x2EE390 (0, 0, 1000, 7500).  Triangle goes back;
//               Cross stores the choice in PlayerData+0x105 and returns 3.
//
// All drawing goes through SpriteSet_Draw.  The intro animation behind the
// menu is TitleBg, in title_bg.cpp.

#include "game/title_menu.h"
#include "game/player_data.h"
#include "native/guest.h"
#include "engine/pad.h"

// ── External functions ─────────────────────────────────────────────────────
extern "C" {
    void* MemAlloc(u32 size);                               // 0x100820
    void  MemFree(void* p);                                 // 0x100650
    void  Sound_PlaySE(u32 id, u32 randomPitch, u32 volume);// 0x180E70

    // Sprite sets: a 0x14-byte handle to a sprite sheet loaded from the archive.
    void* SpriteSet_Ctor(void* mem);                        // 0x17C630
    char  SpriteSet_Load(void* set, int fileId, int arg);   // 0x17C430  starts an async load
    int   SpriteSet_IsLoaded(void* set);                    // 0x17C090
    void  SpriteSet_Dtor(void* set, int flag);              // 0x17C570
    // x/y are screen coordinates in 1/16 pixel; rgba 0x80 = full intensity.
    void  SpriteSet_Draw(void* set, int index, int x, int y, u32 z, u32 rgba);  // 0x17B780

    // Screen coordinate of pixel `px` in 1/16-pixel units; align −1 = from the left/top edge.
    u32   Screen_X(int px, int align);                      // 0x12E5E0
    u32   Screen_Y(int px, int align);                      // 0x12E550

}

// ── Globals and tables ────────────────────────────────────────────────────
static const GuestPtrVar<u8> gPlayerData{0x2EE688u};
static const s32* const gFrameRate  = reinterpret_cast<const s32*>(0x2EDA00u);

struct Pos16 { s16 x, y; };   // 1/16-pixel offsets from the screen's top-left
// 0x2DAFF0: (153, 256) (153, 292) (153, 328)
static const Pos16* const kMainItemPos = reinterpret_cast<const Pos16*>(0x2DAFF0u);
// 0x2DB000: (153, 255) (153, 288) (153, 321) (153, 354)
static const Pos16* const kSubItemPos  = reinterpret_cast<const Pos16*>(0x2DB000u);
// 0x2DB010: 14, 17, 20, 23
static const s32* const kSubItemSprites = reinterpret_cast<const s32*>(0x2DB010u);
// 0x2EE390: 0, 0, 1000, 7500
static const u16* const kSubItemUnlock  = reinterpret_cast<const u16*>(0x2EE390u);

static const u32 kSpriteZ     = 0xFFFFE6u;
static const u32 kWhite       = 0x80808080u;
static const u32 kVolumeMax   = 0x3FFF;

enum : u32 { kSeConfirm = 5, kSeDecide = 6, kSeCancel = 7, kSeCursor = 26 };
enum : int { kFileTitleBg = 0x51, kFileTitleMenu = 0x52, kFileTitle53 = 0x53, kFileTitleLogo = 0x54 };
enum : int { kSpritePressStart = 12, kSpritePressStartGlow = 13 };

template <typename T = void>
static T* P(u32 addr) { return reinterpret_cast<T*>(static_cast<uintptr_t>(addr)); }
static u32 A(void* p) { return static_cast<u32>(reinterpret_cast<uintptr_t>(p)); }

// The asm spells this (frameRate * 30) / 30.
static s32 OneSecond() { return (*gFrameRate * 30) / 30; }

// 0–1 → alpha in the top byte over mid-grey RGB.
static u32 GlowColor(float t) { return ((u32)(s32)(t * 128.0f) << 24) | 0x808080u; }

// std::vector<MenuItem>::push_back — the original inlines insert() (0x26E370).
static void ItemList_Push(MenuItemList* list, const MenuItem& item) {
    if (list->count >= list->capacity) {
        u32 cap = list->capacity ? list->capacity * 2 : 1;
        MenuItem* data = static_cast<MenuItem*>(MemAlloc(cap * sizeof(MenuItem)));
        MenuItem* old = P<MenuItem>(list->data);
        for (u32 i = 0; i < list->count; i++)
            data[i] = old[i];
        if (old)
            MemFree(old);
        list->capacity = cap;
        list->data = A(data);
    }
    P<MenuItem>(list->data)[list->count++] = item;
}

static u32 NewSpriteSet() {
    void* mem = MemAlloc(0x14);
    return mem ? A(SpriteSet_Ctor(mem)) : 0;
}

// ── 0x26D280 TitleMenu_Ctor ───────────────────────────────────────────────
TitleMenu* TitleMenu_Ctor(TitleMenu* self) {
    self->mainItems = {0, 0, 0};
    self->subItems  = {0, 0, 0};
    TitleBg_Ctor(&self->bg, &self->sprites51, &self->sprites54, &self->sprites52, &self->sprites53);
    self->loadStep  = 0;
    self->sprites51 = 0;
    self->sprites52 = 0;
    self->sprites53 = 0;
    self->sprites54 = 0;
    self->subSel    = 0;
    self->mainItems.count = 0;
    return self;
}

// ── 0x26CFD0 TitleMenu_BuildMainMenu ──────────────────────────────────────
static void TitleMenu_BuildMainMenu(TitleMenu* self) {
    u32 set = A(&self->sprites52);
    ItemList_Push(&self->mainItems, {3, kTitleResultNewGame, 0, 0, 0, set});
    ItemList_Push(&self->mainItems, {6, kTitleResultRecords,  0, 0, 0, set});
    ItemList_Push(&self->mainItems, {9, kTitleResultSettings,  0, 0, 0, set});
}

// ── 0x26D0D0 TitleMenu_Load ───────────────────────────────────────────────
// Starts loading the first sprite set.  Returns 0 on failure.
char TitleMenu_Load(TitleMenu* self) {
    self->idleTimer = (s16)(*gFrameRate * 30);
    self->state = kTitlePressStart;
    self->mainSel = 0;

    self->sprites51 = NewSpriteSet();
    if (!self->sprites51)
        return 0;
    if (!SpriteSet_Load(P(self->sprites51), kFileTitleBg, -1))
        return 0;

    self->loadStep = 1;
    TitleMenu_BuildMainMenu(self);
    return 1;
}

// ── 0x26CB10 TitleMenu_LoadStep ───────────────────────────────────────────
// Loads the four sprite sets one after another.  Returns 0 on failure.
static char TitleMenu_LoadStep(TitleMenu* self) {
    struct Step { u32 TitleMenu::* current; u32 TitleMenu::* next; int nextFile; };
    static const Step kSteps[] = {
        { &TitleMenu::sprites51, &TitleMenu::sprites52, kFileTitleMenu },   // loadStep 1
        { &TitleMenu::sprites52, &TitleMenu::sprites53, kFileTitle53   },   // loadStep 2
        { &TitleMenu::sprites53, &TitleMenu::sprites54, kFileTitleLogo },   // loadStep 3
    };

    if (self->loadStep >= 1 && self->loadStep <= 3) {
        const Step& step = kSteps[self->loadStep - 1];
        if (SpriteSet_IsLoaded(P(self->*step.current))) {
            self->loadStep++;
            self->*step.next = NewSpriteSet();
            if (!(self->*step.next))
                return 0;
            if (!SpriteSet_Load(P(self->*step.next), step.nextFile, -1))
                return 0;
        }
    } else if (self->loadStep == 4) {
        if (SpriteSet_IsLoaded(P(self->sprites54)))
            self->loadStep = 5;
    }
    return 1;
}

// ── 0x26CE60 TitleMenu_Restart ────────────────────────────────────────────
// Called when GameScene comes back from mode 1: rebuild the sub-menu from
// what the player has unlocked and go straight to the main menu.
void TitleMenu_Restart(TitleMenu* self) {
    u8* player = *gPlayerData;
    SwordDb_Get();

    self->subItems.count = 0;
    s32 points = *reinterpret_cast<s32*>(player + 8);
    for (int i = 0; i < 4; i++) {
        if (points < (s32)kSubItemUnlock[i])
            break;
        ItemList_Push(&self->subItems, {kSubItemSprites[i], 0, 1, 0, 0, A(&self->sprites52)});
    }
    self->state = kTitleMainMenu;

    if (*reinterpret_cast<s16*>(player + 0x178) == 0 &&
        *reinterpret_cast<s16*>(player + 0x6C80) == 0) {
        Sword sword;
        Sword_Ctor(&sword);
        SwordDb_MakeSword(SwordDb_Get(), 0, &sword);
        PlayerData_CarrySword(reinterpret_cast<PlayerData*>(player), &sword);
        Sword_Dtor(&sword, -1);
    }
}

// Shared by both menus: move the cursor, then run every entry's glow.
static void Menu_Navigate(MenuItemList* list, s16* sel, u32 pad) {
    MenuItem* items = P<MenuItem>(list->data);

    bool moved = false;
    if (pad & kPadUp) {
        if (--*sel < 0)
            *sel = (s16)list->count - 1;
        moved = true;
    } else if (pad & kPadDown) {
        if ((u32)(s32)++*sel >= list->count)
            *sel = 0;
        moved = true;
    }
    if (moved) {
        items[*sel].glow = OneSecond();
        items[*sel].glowRising = 0;
        Sound_PlaySE(kSeCursor, 0, kVolumeMax);
    }

    // Each entry's glow ping-pongs between 0 and one second.
    for (u32 i = 0; i < list->count; i++) {
        MenuItem& it = items[i];
        if (it.glowRising == 1) {
            if (++it.glow >= OneSecond())
                it.glowRising = 0;
        } else if (it.glowRising == 0) {
            if (--it.glow < 1)
                it.glowRising = 1;
        }
    }
}

// ── 0x26C7E0 TitleMenu_UpdateMainMenu ─────────────────────────────────────
static int TitleMenu_UpdateMainMenu(TitleMenu* self) {
    u32* player = reinterpret_cast<u32*>(*gPlayerData);
    u32 pad = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);

    if (pad & kPadCross) {
        Sound_PlaySE(kSeDecide, 0, kVolumeMax);
        int result = P<MenuItem>(self->mainItems.data)[self->mainSel].result;
        if (result == kTitleResultNewGame && player[0] != 2) {
            // No game in progress: ask the sub-menu question first.
            self->subSel = 0;
            MenuItem* sub = P<MenuItem>(self->subItems.data);
            sub[0].glow = OneSecond();
            sub[0].glowRising = 0;
            self->state = kTitleSubMenu;
            return kTitleResultNone;
        }
        return result;
    }

    Menu_Navigate(&self->mainItems, &self->mainSel, pad);
    return kTitleResultNone;
}

// ── 0x26C520 TitleMenu_UpdateSubMenu ──────────────────────────────────────
static int TitleMenu_UpdateSubMenu(TitleMenu* self) {
    u32 pad = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    u8* player = *gPlayerData;

    if (pad & kPadCross) {
        Sound_PlaySE(kSeDecide, 0, kVolumeMax);
        player[0x105] = (u8)self->subSel;
        return kTitleResultNewGame;
    }
    if (pad & kPadTriangle) {
        Sound_PlaySE(kSeCancel, 0, kVolumeMax);
        self->state = kTitleMainMenu;
    }
    Menu_Navigate(&self->subItems, &self->subSel, pad);
    return kTitleResultNone;
}

// ── 0x26CCC0 TitleMenu_Update ─────────────────────────────────────────────
int TitleMenu_Update(TitleMenu* self) {
    if (self->loadStep != 5)
        return TitleMenu_LoadStep(self) == 0;

    TitleBg_Update(&self->bg);

    switch (self->state) {
    case kTitleSubMenu:
        return TitleMenu_UpdateSubMenu(self);

    case kTitleMainMenu:
        return TitleMenu_UpdateMainMenu(self);

    case kTitlePressStart: {
        u32 pad = Pad_GetPressed(PadMgr_Get(), 0);
        if (--self->idleTimer < 0)
            return kTitleResultTimeout;

        if (self->bg.frame < 200) {
            if (pad & kPadStart) {
                // Skip the intro animation.
                Sound_PlaySE(kSeConfirm, 0, kVolumeMax);
                for (Track& track : self->bg.tracks)
                    Track_SkipToEnd(&track);
                self->bg.frame = 200;
            }
            return kTitleResultNone;
        }
        if (!(pad & kPadStart))
            return kTitleResultNone;
        Sound_PlaySE(kSeDecide, 0, kVolumeMax);
        return kTitleResultStart;
    }
    default:
        return kTitleResultNone;
    }
}

// Draw one menu: the selected entry uses sprite+1 with sprite+2 glowing over it.
static void Menu_Draw(const MenuItemList* list, s16 sel, const Pos16* pos, s16 left, s16 top) {
    const MenuItem* items = P<MenuItem>(list->data);
    for (u32 i = 0; i < list->count; i++) {
        const MenuItem& it = items[i];
        s16 x = left + pos[i].x;
        s16 y = top + pos[i].y;
        void* set = P(*P<u32>(it.spriteSet));
        if ((s32)i == sel) {
            SpriteSet_Draw(set, it.sprite + 1, x, y, kSpriteZ, kWhite);
            SpriteSet_Draw(set, it.sprite + 2, x, y, kSpriteZ,
                           GlowColor((float)it.glow / (float)OneSecond()));
        } else {
            SpriteSet_Draw(set, it.sprite, x, y, kSpriteZ, kWhite);
        }
    }
}

// ── 0x26BF40 TitleMenu_Draw ───────────────────────────────────────────────
void TitleMenu_Draw(TitleMenu* self) {
    if (self->loadStep != 5)
        return;

    TitleBg_Draw(&self->bg);

    s16 left = (s16)Screen_X(0, -1);
    s16 top  = (s16)Screen_Y(0, -1);

    switch (self->state) {
    case kTitleSubMenu:
        Menu_Draw(&self->subItems, self->subSel, kSubItemPos, left, top);
        break;

    case kTitleMainMenu:
        Menu_Draw(&self->mainItems, self->mainSel, kMainItemPos, left, top);
        break;

    case kTitlePressStart:
        if (self->bg.frame >= 200) {
            // "Press start" at (26, 288) with a glow that pulses once a second.
            s32 period = OneSecond();
            s32 half = period >> 1;
            s32 phase = self->idleTimer % period - half;
            if (phase < 0)
                phase = -phase;
            SpriteSet_Draw(P(self->sprites52), kSpritePressStart,
                           left + 0x1A0, top + 0x1200, kSpriteZ, kWhite);
            SpriteSet_Draw(P(self->sprites52), kSpritePressStartGlow,
                           left + 0x1A0, top + 0x1200, kSpriteZ,
                           GlowColor((float)phase / (float)half));
        }
        break;
    }
}

// ── 0x26D180 TitleMenu_Dtor ───────────────────────────────────────────────
static void ItemList_Free(MenuItemList* list) {
    list->count = 0;
    if (list->data)
        MemFree(P(list->data));
}

TitleMenu* TitleMenu_Dtor(TitleMenu* self, s16 flag) {
    if (!self)
        return self;

    u32* const sets[] = { &self->sprites51, &self->sprites52, &self->sprites53, &self->sprites54 };
    for (u32* set : sets) {
        if (*set)
            SpriteSet_Dtor(P(*set), 1);
        *set = 0;
    }
    self->mainItems.count = 0;
    TitleBg_Dtor(&self->bg, -1);
    ItemList_Free(&self->subItems);
    ItemList_Free(&self->mainItems);
    if (flag > 0)
        MemFree(self);
    return self;
}
