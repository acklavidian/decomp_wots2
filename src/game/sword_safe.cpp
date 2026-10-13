// The sword safe ("Equip Swords / Sword Safe / Done"; 0x26EA40–0x26F320 with
// its parts at 0x291900–0x2997C0): which swords are carried and which are
// put away.  Reached from the character setup (where two may be carried)
// and from the player's home (three).
//
// Sprites assets 0x58 (menu/WeaponStore/main.bpm) and 0x59 (BG.bpm), numbers
// asset 0x5B (param.tp).  Three views, cross-faded over 15 frames:
//   1  the top menu: Equip Swords / Sword Safe / Done
//   2  the carried swords as a column of cards (the same cards as the status
//      menu's, src/game/swords_page.cpp): Up / Down, Cross → Put in safe /
//      Equip / Drop / Check moves
//   3  the safe: pages of ten rows (name and three numbers; Square changes
//      which three), a page tab (Left / Right) and a "Change Order" tab that
//      sorts the safe; Cross on a row → Ready (take it out) / Drop / Check
//      moves, with the sword's card shown beside the list
// SwordSelect_Update answers {1, 0} when the player is done and {2, sword}
// when a sword's moves are to be shown.
//
// Not done: the animation of the "readied" mark on a card (as on the status
// menu), and the cards' pictures are not released when a card goes.

#include "engine/bgm.h"
#include "engine/containers.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/pad_repeat.h"
#include "engine/param_file.h"
#include "engine/text_label.h"
#include "engine/window.h"
#include "game/player_data.h"
#include "game/sword.h"
#include "game/sword_cards.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

struct SpriteSet;

extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    void  Gs_FillRect(const s32* rect, u32 z, u32 rgba);                // 0x225CD0  {x, y, w, h} in 1/16 px
    SpriteSet* SpriteSet_Ctor(void* mem);                               // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    int   SpriteSet_IsLoaded(SpriteSet* set);                           // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                               // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);                     // 0x17C570
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    void  SpriteSet_DrawEx(SpriteSet* set, int index, const void* params);             // 0x17ADF0
    void* SpriteSet_GetSprite(SpriteSet* set, int index);               // 0x17BDF0
    u16   Pad_GetStickHeld(void* mgr, int port);                        // 0x139F90
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
const GuestPtrVar<Font>       gFont{0x2EE67Cu};
const GuestPtrVar<WindowMgr>  gWindowMgr{0x2EE680u};
const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
u32* const gFrameRate = guest::ptr<u32>(0x2EDA00u);

const u32 kZ = 0xFFFF82u;

inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
inline u8* Pd() { return reinterpret_cast<u8*>(*gPlayerData); }
inline s16& Carried() { return *reinterpret_cast<s16*>(Pd() + 0x178); }
inline s16& Stored() { return *reinterpret_cast<s16*>(Pd() + 0x6C80); }
inline s16 Capacity() { return *reinterpret_cast<s16*>(Pd() + 0x6C84); }
inline u8* StoredSword(int i) { return Pd() + 0x880 + i * 0x100; }
inline s32 Frames(int n) { return (s32)(*gFrameRate * (u32)n) / 0x1E; }
inline u16 PadNow() { return (u16)(Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0)); }
inline u16 PadStick() { return (u16)(Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetStickHeld(PadMgr_Get(), 0)); }
inline u32 Alpha(float a) { return (u32)(s32)(a * 128.0f) << 24; }
inline int X0() { return (int)(Screen_X(0, -1) & 0xFFFF); }
inline int Y0() { return (int)(Screen_Y(0, -1) & 0xFFFF); }

void Take(BufferReader* r, void* out, u32 size) {
    u32 got = 0;
    if (r->remaining >= size) {
        memcpy(out, P(r->cur), size);
        got = size;
    }
    r->cur += got;
    r->remaining -= got;
}

// A number as text the font can draw (a few live at a time).
const u16* Num(int value, const u16* middle = nullptr, int second = 0) {
    static u32 buffers = 0;
    static int next = 0;
    if (buffers == 0)
        buffers = guest::addr(guest::alloc(8 * 0x40));
    u16* text = P<u16>(buffers + (u32)(next++ & 7) * 0x40);
    char digits[16];
    int n = 0;
    snprintf(digits, sizeof digits, "%d", value);
    for (const char* c = digits; *c != 0; c++)
        text[n++] = (u16)*c;
    if (middle != nullptr) {
        for (; *middle != 0 && n < 20; middle++)
            text[n++] = *middle;
        snprintf(digits, sizeof digits, "%d", second);
        for (const char* c = digits; *c != 0; c++)
            text[n++] = (u16)*c;
    }
    text[n] = 0;
    return text;
}

// A menu of the window manager.  (The options have to be in guest memory.)
u32 OpenMenu(u32 mgr, int title, const int* ids, int count, s32 y, s32 halfW, s32 halfH, s32 titleHeight, s32 cursor) {
    WindowDesc desc;
    memset(&desc, 0, sizeof desc);
    WindowDesc_Init(&desc);
    u32* options = static_cast<u32*>(MemAlloc((u32)count * sizeof(u32)));
    for (int i = 0; i < count; i++)
        options[i] = guest::addr(Text(ids[i]));
    desc.y = y;
    desc.halfWidth = halfW;
    desc.halfHeight = halfH;
    desc.z = kZ;
    desc.title = title != 0 ? guest::addr(Text(title)) : 0;
    desc.options = {(u32)count, (u32)count, guest::addr(options)};
    desc.titleHeight = titleHeight;
    desc.cursor = cursor;
    desc.priority = 2;
    const u32 window = guest::addr(WindowMgr_Open(P<WindowMgr>(mgr), &desc));
    MemFree(options);
    return window;
}
u32 OpenMessage(u32 mgr, int text, u32 z) {
    return guest::addr(WindowMgr_OpenMessage(P<WindowMgr>(mgr), 0, 0, z, 2, 0, 0, Text(text), 0));
}
void CloseWindow(u32 mgr, u32& window) {
    if (window != 0)
        WindowMgr_Close(P<WindowMgr>(mgr), P<Window>(window));
    window = 0;
}

// ═══ The background and the cross-fade between views (0x44 bytes) ══════════
struct Bg {
    s32 view;               // +0x00  1 top, 2 carried, 3 safe
    s32 next;               // +0x04  the view being faded to, 0 none
    s32 timer;              // +0x08
    s32 pattern[3][2];      // +0x0C  how strong the two patterns are in each view
    s32 title[2];           // +0x24
    s32 ornamentL[2];       // +0x2C
    s32 ornamentR[2];       // +0x34
    u8  busy;               // +0x3C
    u8  hold;               // +0x3D  the fade waits (nothing to show in the next view)
    u8  _3e[2];
    u32 sprites;            // +0x40  asset 0x59
};
static_assert(sizeof(Bg) == 0x44, "Bg");

// ── 0x2958B0: → still fading ──────────────────────────────────────────────
int Bg_Step(Bg* self) {
    if (self->next == 0)
        return 0;
    if (self->hold != 0)
        return 1;
    if (Frames(0xF) < ++self->timer) {
        self->view = self->next;
        self->next = 0;
        return 0;
    }
    return 1;
}
void Bg_Hold(Bg* self) {                    // 0x295950
    if (self->next != 0)
        self->hold = 1;
}
void Bg_TurnBack(Bg* self) {                // 0x295970
    if (self->next != 0) {
        const s32 was = self->next;
        self->next = self->view;
        self->view = was;
        self->hold = 0;
    }
}
void Bg_GoTo(Bg* self, s32 view) {          // 0x2959A0
    self->next = view;
    self->timer = 0;
    self->hold = 0;
}

// ── 0x2948A0 / 0x294B60 ───────────────────────────────────────────────────
void Bg_DrawView(Bg* self, int view, float amount) {
    SpriteSet* sprites = P<SpriteSet>(self->sprites);
    const auto pattern = [&](int sprite, s32 strength, int x) {
        const u32 a = (u32)(s32)((float)strength * amount);
        if ((a & 0xFF) != 0)
            SpriteSet_Draw(sprites, sprite, (int)Screen_X(x, -1), (int)Screen_Y(0, -1), kZ, a << 24 | 0x808080);
    };
    pattern(1, self->pattern[view - 1][0], 0);
    pattern(2, self->pattern[view - 1][1], 0x1A90);
    const u32 tint = Alpha(amount) | 0x808080;
    if (view == 1) {
        SpriteSet_Draw(sprites, 3, X0() + self->title[0] * 0x10, Y0() + self->title[1] * 0x10, kZ, tint);
        SpriteSet_Draw(sprites, 4, X0() + self->ornamentL[0] * 0x10, Y0() + self->ornamentL[1] * 0x10, kZ, tint);
        struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } flipped;
        flipped.flags = 0x20;
        flipped.x = X0() + self->ornamentR[0] * 0x10;
        flipped.y = Y0() + self->ornamentR[1] * 0x10;
        flipped.z = kZ;
        flipped.w = flipped.h = -1;
        flipped.rgba = tint;
        SpriteSet_DrawEx(sprites, 4, &flipped);
    } else {
        SpriteSet_Draw(sprites, view == 2 ? 5 : 6, X0() + self->title[0] * 0x10, Y0() + self->title[1] * 0x10, kZ,
                       tint);
    }
}
void Bg_Draw(Bg* self) {
    if (SpriteSet_Bind(P<SpriteSet>(self->sprites)) == 0)
        return;
    SpriteSet_Draw(P<SpriteSet>(self->sprites), 0, (int)Screen_X(0, -1), (int)Screen_Y(0, -1), kZ, 0x80808080u);
    if (self->next == 0) {
        Bg_DrawView(self, self->view, 1.0f);
        return;
    }
    const s32 total = Frames(0xF);
    const float t = (float)self->timer / (float)(total != 0 ? total : 1);
    Bg_DrawView(self, self->view, 1.0f - t);
    Bg_DrawView(self, self->next, t);
}

// ═══ The top menu (0x8C bytes) ═════════════════════════════════════════════
struct Top {
    s32 state;              // +0x00  1: a message is up
    s32 cursor;             // +0x04
    u32 window;             // +0x08
    s32 at[3][2];           // +0x0C
    s32 width, height;      // +0x24
    s32 dim[3];             // +0x2C  0 = the entry the cursor is on
    u32 text[3];            // +0x38
    u32 sprites;            // +0x44  → the screen's sprite set pointer
    PadRepeatPair keys;     // +0x48
};
static_assert(sizeof(Top) == 0x8C, "Top");

Top* Top_Ctor(Top* self, u32 sprites) {     // 0x294720
    memset(self, 0, sizeof *self);
    self->sprites = sprites;
    PadRepeatPair_Ctor(&self->keys);
    self->keys.vtable = 0x2EAA00u;
    PadRepeatPair_SetMasks(&self->keys, kPadUp, kPadDown);
    for (int i = 0; i < 3; i++) {
        self->dim[i] = i != 0;
        self->text[i] = guest::addr(Text(0xC4 + i));
    }
    PadRepeatPair_SetTiming(&self->keys, 10, 5, 0);
    return self;
}

// ── 0x2941E0: → 1 done, 3 to the carried swords, 4 to the safe ────────────
int Top_Update(Top* self, bool turning) {
    const u16 pad = PadNow();
    if (!turning) {
        if (self->state == 1) {
            if ((Pad_GetPressed(PadMgr_Get(), 0) & (kPadCross | kPadTriangle)) != 0) {
                CloseWindow(guest::addr(*gWindowMgr), self->window);
                self->state = 0;
            }
            return 0;
        }
        if ((pad & kPadCross) != 0) {
            if (self->cursor == 2) {
                if (Carried() < 1) {            // "Equip a sword"
                    self->window = OpenMessage(guest::addr(*gWindowMgr), 0xD2, 0xFFFFE6u);
                    self->state = 1;
                    return 0;
                }
                Sound_PlaySE(9, 0, 0x3FFF);
                return 1;
            }
            if (self->cursor == 1) {
                Sound_PlaySE(6, 0, 0x3FFF);
                return 4;
            }
            if (self->cursor == 0) {
                Sound_PlaySE(6, 0, 0x3FFF);
                return 3;
            }
        }
    }
    for (s32& d : self->dim)
        if (d != 0 && d != 1)
            d++;
    PadRepeat_Update(&self->keys.a);
    PadRepeat_Update(&self->keys.b);
    const auto moveTo = [self](int to) {
        Sound_PlaySE(3, 0, 0x3FFF);
        self->dim[self->cursor] = 1;
        self->cursor = to;
        self->dim[to] = 0;
    };
    if ((pad & kPadTriangle) != 0) {
        Sound_PlaySE(7, 0, 0x3FFF);
        self->dim[self->cursor] = 1;
        self->cursor = 2;
        self->dim[2] = 0;
    } else if (!PadRepeat_IsHeld(&self->keys.b) && self->keys.a.fired != 0) {
        moveTo(self->cursor - 1 < 0 ? self->cursor + 2 : self->cursor - 1);
    } else if (!PadRepeat_IsHeld(&self->keys.a) && self->keys.b.fired != 0) {
        moveTo(self->cursor + 1 < 3 ? self->cursor + 1 : self->cursor - 2);
    }
    return 0;
}

// ── 0x293D80 ──────────────────────────────────────────────────────────────
void Top_Draw(Top* self, float amount) {
    Font* font = *gFont;
    SpriteSet* sprites = P<SpriteSet>(*P<u32>(self->sprites));
    const u32 a = Alpha(amount);
    if (SpriteSet_GetSprite(sprites, 0) != nullptr) {
        for (int i = 0; i < 3; i++) {
            const int x = (X0() + self->at[i][0] * 0x10) & 0xFFFF, y = (Y0() + self->at[i][1] * 0x10) & 0xFFFF;
            const s32 shadow[4] = {x + 0x20, y + 0x20, self->width << 4, self->height << 4};
            Gs_FillRect(shadow, kZ, (u32)(s32)(amount * 40.0f) << 24 | 0x80404);
            SpriteSet_Draw(sprites, 0, x, y, kZ, a | 0x808080);
            const s32 off = self->dim[i] < 0 ? -self->dim[i] : self->dim[i];
            SpriteSet_Draw(sprites, 1, x, y, kZ, (u32)(s32)(amount * (1.0f - (float)off) * 128.0f) << 24 | 0x808080);
            Font_SetColor(font, a | (i == self->cursor ? 0xACB2BEu : 0x212121u));
            Font_DrawCenteredPlain(font, P<const u16>(self->text[i]), (x + self->width * 8) & 0xFFFF,
                                   (y + self->height * 8) & 0xFFFF, kZ);
        }
    }
    Font_SetColor(font, a | 0xACB2BE);
    GsDevice_End2D(*gGsDevice);
    Font_DrawString(font, Text(0x16), X0() + 0x1F30, Y0() + 0x18B0, kZ, 1.0f, 0.0f);
    GsDevice_Begin2D(*gGsDevice);
}

// ═══ The carried swords: a column of cards (0x1B0 bytes) ═══════════════════
// A card here has, after the class's 0xC4 bytes:
struct ColumnCard {
    CardHead head;
    u8  _0f[0xC4 - sizeof(CardHead)];
    u8  current;            // +0xC4  the cursor is on it
    u8  _c5[3];
    s32 fade;               // +0xC8  1 lighting up, 2 going dark, 0 dark
    s32 fadeTimer;          // +0xCC
    s32 place;              // +0xD0  which row it is in…
    s32 placeTo;            // +0xD4  …and which it is sliding to
    s32 slideTimer;         // +0xD8
    u32 metrics;            // +0xDC
};
static_assert(sizeof(ColumnCard) == 0xE0 && offsetof(ColumnCard, current) == 0xC4, "ColumnCard");

struct ColumnMetrics {      // 0x2C bytes of the numbers file
    s32 at[3][2];           // +0x00  where each row's card goes
    s32 cursorX, cursorY;   // +0x18  the cursor sprite, from the card's corner
    s32 fadeFrames;         // +0x20
    s32 shade;              // +0x24  how dark the others are
    s32 slideFrames;        // +0x28
};

struct Column {
    u8  holder[0x138];      // +0x000  the card layout (see sword_cards.h)
    u8  ready;              // +0x138
    u8  _139[3];
    s32 current;            // +0x13C
    GuestVector cards;      // +0x140  ColumnCard*[]
    GuestVector slots;      // +0x14C  s16[]: which carried sword each is
    s32 readied;            // +0x158  index of the readied sword's card
    ColumnMetrics metrics;  // +0x15C
    u32 leaving;            // +0x188  a card fading away after its sword went
    s32 leavingRow;         // +0x18C
    s32 leavingTimer;       // +0x190
    u32 preview;            // +0x194  a card for a sword of the safe
    s8  count;              // +0x198
    s8  readiedSlot;        // +0x199
    s8  readiedAtStart;     // +0x19A
    u8  _19b;
    u32 swords;             // +0x19C
    s32 state;              // +0x1A0  1 browse, 2 menu, 3 message, 4 "drop?", 5 nothing carried
    u32 window;             // +0x1A4
    u32 _1a8;
    u32 windowMgr;          // +0x1AC
};
static_assert(sizeof(Column) == 0x1B0 && offsetof(Column, metrics) == 0x15C && offsetof(Column, state) == 0x1A0,
              "Column");

inline ColumnCard* CardAt(Column* c, int i) { return P<ColumnCard>(P<u32>(c->cards.data)[i]); }
inline s16 SlotAt(Column* c, int i) { return P<s16>(c->slots.data)[i]; }
void Light(ColumnCard* card) { card->current = 1; card->fade = 1; card->fadeTimer = 0; }
void Darken(ColumnCard* card) { card->current = 0; card->fade = 2; card->fadeTimer = 0; }

Column* Column_Ctor(Column* self) {          // 0x293D40 / 0x292840
    memset(self, 0, sizeof *self);
    swordcards::HolderInit(self->holder);
    *reinterpret_cast<u32*>(self->holder + 0x134) = 0x2ED168u;
    self->windowMgr = guest::addr(*gWindowMgr);
    self->state = 1;
    return self;
}

void Column_FreeCards(Column* self) {
    if (self->leaving != 0)
        swordcards::FreeCard(P(self->leaving));
    self->leaving = 0;
    for (u32 i = 0; i < self->cards.count; i++)
        swordcards::FreeCard(CardAt(self, (int)i));
    self->cards.count = 0;
    self->slots.count = 0;
}

// ── 0x2922D0: a card for every carried sword ──────────────────────────────
void Column_Begin(Column* self, int count, int readied, u32 swords, const s16* order) {
    Column_FreeCards(self);
    if (self->cards.data == 0) {
        self->cards = {3, 0, guest::addr(MemAlloc(3 * sizeof(u32)))};
        self->slots = {3, 0, guest::addr(MemAlloc(3 * sizeof(s16)))};
    }
    self->count = (s8)count;
    self->readiedSlot = self->readiedAtStart = (s8)readied;
    self->swords = swords;
    for (int slot = 0; slot < 3; slot++) {
        if (order[slot] == -1)
            continue;
        ColumnCard* card =
            static_cast<ColumnCard*>(swordcards::NewCard(self->holder, swords + (u32)slot * 0x100, 0x2ED158u));
        P<u32>(self->cards.data)[self->cards.count++] = guest::addr(card);
        card->metrics = guest::addr(&self->metrics);
        swordcards::SetTexts(card);
        P<s16>(self->slots.data)[self->slots.count++] = (s16)slot;
        if (slot == self->readiedSlot)
            self->readied = (s32)self->cards.count - 1;
    }
    self->current = self->readied;
    for (int i = 0; i < self->count; i++) {
        ColumnCard* card = CardAt(self, i);
        if (i == self->current) {
            card->head.marked = 1;
            Light(card);
        } else {
            card->head.marked = 0;
            card->current = 0;
            card->fade = 0;
        }
        card->place = card->placeTo = i;
        card->slideTimer = 0;
    }
}

// ── 0x291ED0: the card under the cursor goes (it fades where it was); the
// others close up.  → which carried sword it was ──────────────────────────
s16 Column_Remove(Column* self) {
    if (self->leaving != 0)
        swordcards::FreeCard(P(self->leaving));
    const s32 gone = self->current;
    self->leaving = P<u32>(self->cards.data)[gone];
    self->leavingRow = gone;
    self->leavingTimer = 0;
    const s16 slot = SlotAt(self, gone);
    u32* cards = P<u32>(self->cards.data);
    s16* slots = P<s16>(self->slots.data);
    for (u32 i = (u32)gone; i + 1 < self->cards.count; i++) {
        cards[i] = cards[i + 1];
        slots[i] = slots[i + 1];
    }
    self->cards.count--;
    self->slots.count--;
    self->count--;
    if (self->current >= self->count)
        self->current -= self->count;
    if (self->count > 0) {
        Light(CardAt(self, self->current));
        if (gone == self->readied && self->readiedSlot != SlotAt(self, self->current)) {
            // (the original also takes the mark off "the card at the old
            // index", which by now is another card)
            CardAt(self, self->current)->head.marked = 1;
            self->readiedSlot = (s8)SlotAt(self, self->current);
            self->readied = self->current;
        }
    }
    if (gone < self->readied)
        self->readied--;
    for (int i = 0; i < self->count; i++) {
        CardAt(self, i)->placeTo = i;
        CardAt(self, i)->slideTimer = 0;
    }
    return slot;
}

// ── 0x292120: the sword under the cursor becomes the readied one; → it was not
bool Column_Ready(Column* self) {
    if (self->readiedSlot == SlotAt(self, self->current))
        return false;
    swordcards::Unmark(CardAt(self, self->readied));
    swordcards::Mark(CardAt(self, self->current));
    self->readiedSlot = (s8)SlotAt(self, self->current);
    self->readied = self->current;
    return true;
}
void Column_Move(Column* self, int by) {     // 0x2921D0 / 0x292250
    if (self->count == 0)
        return;
    Darken(CardAt(self, self->current));
    self->current += by;
    if (self->current < 0)
        self->current = self->count - 1;
    else if (self->current >= self->count)
        self->current = 0;
    Light(CardAt(self, self->current));
}

// ── 0x291C80: one frame of the cards ──────────────────────────────────────
void Column_Tick(Column* self) {
    if (self->leaving != 0 && Frames(0xF) <= ++self->leavingTimer) {
        swordcards::FreeCard(P(self->leaving));
        self->leaving = 0;
    }
    for (int i = 0; i < self->count; i++) {
        ColumnCard* card = CardAt(self, i);
        swordcards::Update(card);
        if (card->place != card->placeTo && self->metrics.slideFrames < ++card->slideTimer)
            card->place = card->placeTo;
        if (card->fadeTimer < self->metrics.fadeFrames && self->metrics.fadeFrames <= ++card->fadeTimer &&
            card->fade == 2)
            card->fade = 0;
    }
}

// ── 0x292940: where a card is and how dark ────────────────────────────────
void Column_Place(float amount, ColumnCard* card, int row) {
    const ColumnMetrics* m = P<ColumnMetrics>(card->metrics);
    const float through = (float)card->fadeTimer / (float)(m->fadeFrames != 0 ? m->fadeFrames : 1);
    if (card->fade == 1)
        card->head.shade = (u8)(m->shade - (s32)((float)m->shade * through));
    else if (card->fade == 2)
        card->head.shade = (u8)(s32)((float)m->shade * through);
    else if (card->fade == 0)
        card->head.shade = (u8)m->shade;
    if (card->place == card->placeTo) {
        card->head.x = (u16)Screen_X(m->at[row][0] << 4, -1);
        card->head.y = (u16)Screen_Y(m->at[row][1] << 4, -1);
    } else {
        const s32* from = m->at[card->place];
        const s32* to = m->at[card->placeTo];
        const float t = (float)card->slideTimer / (float)(m->slideFrames != 0 ? m->slideFrames : 1);
        card->head.x = (u16)Screen_X((from[0] + (s32)((float)(to[0] - from[0]) * t)) * 0x10, -1);
        card->head.y = (u16)Screen_Y((from[1] + (s32)((float)(to[1] - from[1]) * t)) * 0x10, -1);
    }
    card->head.z = kZ;
    card->head.alpha = amount;
}

// ── 0x291A00 ──────────────────────────────────────────────────────────────
void Column_DrawCards(Column* self, float amount, SpriteSet* sprites) {
    if (self->ready == 0)
        return;
    SpriteSet_Bind(sprites);
    for (int i = 0; i < self->count; i++) {
        ColumnCard* card = CardAt(self, i);
        Column_Place(amount, card, i);
        if (card->current != 0)
            SpriteSet_Draw(sprites, 0xB, card->head.x + self->metrics.cursorX * 0x10,
                           card->head.y + self->metrics.cursorY * 0x10, kZ, Alpha(card->head.alpha) | 0x808080);
    }
    if (!swordcards::Ready())
        return;
    swordcards::BindSprites();
    for (int i = 0; i < self->count; i++)
        swordcards::Draw(CardAt(self, i));
    if (self->leaving != 0) {
        const s32 total = Frames(0xF);
        Column_Place(amount * (1.0f - (float)self->leavingTimer / (float)(total != 0 ? total : 1)),
                     P<ColumnCard>(self->leaving), self->leavingRow);
        swordcards::Draw(P(self->leaving));
    }
    for (int i = 0; i < self->count; i++)
        swordcards::DrawPicture(CardAt(self, i));
    if (self->leaving != 0)
        swordcards::DrawPicture(P(self->leaving));
    SpriteSet_Bind(sprites);
}

// ── 0x2919A0: the safe's preview card ─────────────────────────────────────
void Column_DrawPreview(Column* self) {
    if (self->ready == 0 || !swordcards::Ready() || self->preview == 0)
        return;
    swordcards::BindSprites();
    swordcards::Draw(P(self->preview));
    swordcards::DrawPicture(P(self->preview));
}
void* Column_MakePreview(Column* self, u32 sword) {      // 0x291E60 (→ 0x289F70)
    if (self->preview != 0)
        swordcards::FreeCard(P(self->preview));
    self->preview = guest::addr(swordcards::NewCard(self->holder, sword, 0x2ED148u));
    return P(self->preview);
}
void Column_DropPreview(Column* self) {                  // 0x291DF0
    if (self->preview != 0)
        swordcards::FreeCard(P(self->preview));
    self->preview = 0;
}

// ── 0x292BF0 ──────────────────────────────────────────────────────────────
void Column_Draw(Column* self, float amount, SpriteSet* sprites) {
    Column_DrawCards(self, amount, sprites);
    Font* font = *gFont;
    GsDevice_End2D(*gGsDevice);
    Font_SetColor(font, Alpha(amount) | 0xACB2BE);
    if (self->state == 5) {
        GsDevice_Begin2D(*gGsDevice);
        Font_SetColor(font, 0x80ACB2BEu);
        Font_DrawString(font, Text(0x17), X0() + 0x1F30, Y0() + 0x18B0, kZ, 1.0f, 0.0f);
        Font_DrawCenteredPlain(font, Text(0xCC), (int)Screen_X(0, 0), (int)Screen_Y(0, 0), kZ);     // "Not equipped"
        return;
    }
    if (self->count > 0)
        Font_DrawString(font, Text(0x16), X0() + 0x17C0, Y0() + 0x18B0, kZ, 1.0f, 0.0f);
    Font_DrawString(font, Text(0x17), X0() + 0x1F30, Y0() + 0x18B0, kZ, 1.0f, 0.0f);
    GsDevice_Begin2D(*gGsDevice);
}

void Column_Message(Column* self, int text) {
    if (self->window != 0)
        CloseWindow(self->windowMgr, self->window);
    self->window = OpenMessage(self->windowMgr, text, 0xFFFFE6u);
    self->state = 3;
}

// What is left to do once a sword has gone out of the hands: → 2 when none is left.
int Column_AfterRemoval(Column* self) {
    const s16 slot = Column_Remove(self);
    PlayerData_RemoveSword(*gPlayerData, slot);
    if (Carried() < 1) {
        self->state = 0;
        return 2;
    }
    self->state = 1;
    return 0;
}
// ── 0x293580: "Drop" confirmed ────────────────────────────────────────────
int Column_Drop(Column* self) {
    if (Carried() + Stored() < 2) {
        Column_Message(self, 0xD1);             // "Cannot drop all of your swords"
        return 0;
    }
    return Column_AfterRemoval(self);
}
// ── 0x2936A0: "Put in safe" ───────────────────────────────────────────────
int Column_Store(Column* self) {
    const Sword* sword = P<Sword>(self->swords + (u32)SlotAt(self, self->current) * 0x100);
    if (PlayerData_StoreSword(*gPlayerData, sword) == 0) {
        Column_Message(self, 0xCE);             // "Sword safe is full"
        return 0;
    }
    return Column_AfterRemoval(self);
}

// ── 0x293390 (with 0x293260, 0x292FF0, 0x292EA0): → {what, which} ─────────
// 2 back to the top menu, 6 show the moves of carried sword `which`, 7 hold
// the fade (there is nothing to show), 8 turn back.
void Column_Update(s32 out[2], Column* self, bool turning) {
    out[0] = out[1] = 0;
    Column_Tick(self);
    if (self->state == 0)
        return;
    const u16 pad = PadNow();
    if (turning) {
        if (self->state == 5) {
            if ((pad & (kPadCross | kPadTriangle)) != 0) {
                self->state = 0;
                out[0] = 8;
            }
        } else if (self->count < 1) {
            self->state = 5;
            out[0] = 7;
        } else if (self->state == 1 && self->count > 1) {
            if ((pad & kPadUp) != 0) {
                Column_Move(self, -1);
                Sound_PlaySE(0x1A, 0, 0x3FFF);
            } else if ((pad & kPadDown) != 0) {
                Sound_PlaySE(0x1A, 0, 0x3FFF);
                Column_Move(self, 1);
            }
        }
        return;
    }
    switch (self->state) {
    case 1:                                     // 0x293260
        if ((pad & kPadCross) != 0) {
            Sound_PlaySE(1, 0, 0x3FFF);
            static const int kOptions[4] = {0xC7, 0xC8, 0xC9, 0xCB};    // Put in safe / Equip / Drop / Check moves
            self->window = OpenMenu(self->windowMgr, 0, kOptions, 4, 0, 0, 0, 0, 0);
            self->state = 2;
        } else if ((pad & kPadTriangle) != 0) {
            Sound_PlaySE(7, 0, 0x3FFF);
            out[0] = 2;
        } else if (self->count > 1) {
            if ((pad & kPadUp) != 0) {
                Column_Move(self, -1);
                Sound_PlaySE(0x1A, 0, 0x3FFF);
            } else if ((pad & kPadDown) != 0) {
                Sound_PlaySE(0x1A, 0, 0x3FFF);
                Column_Move(self, 1);
            }
        }
        break;
    case 2:                                     // 0x292FF0
        if ((pad & kPadCross) != 0) {
            const int chosen = WindowMgr_GetCursor(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
            CloseWindow(self->windowMgr, self->window);
            Sound_PlaySE(chosen >= 0 && chosen <= 3 ? 5 : -1, 0, 0x3FFF);
            if (chosen == 3) {
                out[0] = 6;
                out[1] = SlotAt(self, self->current);
                self->state = 1;
            } else if (chosen == 2) {
                static const int kYesNo[2] = {0xF, 0x10};
                self->window = OpenMenu(self->windowMgr, 0xCD, kYesNo, 2, 0, 0xB4, 0x6E, 100, 1);
                self->state = 4;
            } else if (chosen == 1) {
                if (Column_Ready(self)) {
                    Sound_PlaySE(0x1B, 0, 0x3FFF);
                    self->state = 1;
                } else {
                    Column_Message(self, 0x93);     // "Already equipped"
                }
            } else if (chosen == 0) {
                out[0] = Column_Store(self);
            }
        } else if ((pad & kPadTriangle) != 0) {
            Sound_PlaySE(7, 0, 0x3FFF);
            CloseWindow(self->windowMgr, self->window);
            self->state = 1;
        }
        break;
    case 3:
        if ((pad & (kPadCross | kPadTriangle)) != 0) {
            CloseWindow(self->windowMgr, self->window);
            self->state = 1;
        }
        break;
    case 4:                                     // 0x292EA0
        if ((pad & kPadCross) != 0) {
            const int chosen = WindowMgr_GetCursor(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
            CloseWindow(self->windowMgr, self->window);
            if (chosen == 1) {
                Sound_PlaySE(7, 0, 0x3FFF);
                self->state = 1;
            } else if (chosen == 0) {
                Sound_PlaySE(5, 0, 0x3FFF);
                out[0] = Column_Drop(self);
            }
        } else if ((pad & kPadTriangle) != 0) {
            Sound_PlaySE(7, 0, 0x3FFF);
            CloseWindow(self->windowMgr, self->window);
            self->state = 1;
        }
        break;
    default:
        self->state = 0;
        out[0] = 2;
        break;
    }
}

void Column_Enter(Column* self) {            // 0x293C60
    self->state = 1;
    Column_Begin(self, Carried(), PlayerData_GetReadiedSword(*gPlayerData), guest::addr(Pd() + 0x180),
                 reinterpret_cast<const s16*>(Pd() + 0x17A));
}
void Column_Leave(Column* self) {            // 0x293C10
    if (Carried() != 0)
        PlayerData_ReadySword(*gPlayerData, self->readiedSlot);
    if (self->window != 0)
        CloseWindow(self->windowMgr, self->window);
}
void Column_Dtor(Column* self) {             // 0x293CD0 / 0x2926A0
    if (self->window != 0)
        CloseWindow(self->windowMgr, self->window);
    Column_DropPreview(self);
    Column_FreeCards(self);
    swordcards::HolderFree(self->holder);
    if (self->slots.data != 0)
        MemFree(P(self->slots.data));
    if (self->cards.data != 0)
        MemFree(P(self->cards.data));
    self->cards = {0, 0, 0};
    self->slots = {0, 0, 0};
}

// ═══ The safe (0x1CC bytes) ════════════════════════════════════════════════
struct RowMetrics {         // 0x24 bytes of the numbers file
    s32 cell[4][2];         // +0x00  the name and the three numbers, from the row's corner
    s32 fadeFrames;         // +0x20
};
struct Row {                // 0x14 bytes
    u32 sprites;            // +0x00  → the screen's sprite set pointer
    u32 display;            // +0x04  → Safe::display
    u32 metrics;            // +0x08
    s32 state;              // +0x0C  1 lighting up, 4 lit, 2 going out, 0 plain
    s32 timer;              // +0x10
};
struct Tab {                // 0x18 bytes: a tab that sticks out at the side
    u32 sprites;            // +0x00
    u32 owner;              // +0x04
    u32 text;               // +0x08
    s32 state;              // +0x0C  2 coming out, 4 out, 1 going back, 0 in
    s32 timer;              // +0x10
    u32 vtable;             // +0x14  +0x08 which sprite, +0x0C where
};
struct Safe {
    s32 frame[4];           // +0x000  the list's rectangle
    s32 rowStep;            // +0x010
    s32 countAt[2];         // +0x014  the "12/20" plate
    s32 sortAt[2];          // +0x01C  the "Change Order" tab
    s32 pageAt[2];          // +0x024  the page tab
    s32 cardAt[2];          // +0x02C  the preview card
    RowMetrics rowMetrics;  // +0x034
    u32 sprites;            // +0x058  → the screen's {sprites, column, most carried}
    s32 display;            // +0x05C  0 stance / durability / quality, 1 attack / defence / kills
    PadRepeatPair keys;     // +0x060
    s32 state;              // +0x0A4  1 the tabs, 2 the rows, 4 a row's menu, 0x10 message, 0x20 "drop?", 0x40 order menu
    u32 window;             // +0x0A8
    Row rows[10];           // +0x0AC
    s32 cursor;             // +0x174
    Tab sort;               // +0x178
    Tab page;               // +0x190
    s32 pageNo;             // +0x1A8
    u8  arrowRight;         // +0x1AC  (the page tab's: there are pages that way)
    u8  arrowLeft;          // +0x1AD
    u8  _1ae[2];
    u32 tabs[2];            // +0x1B0  page, sort
    s32 tab;                // +0x1B8
    u32 preview;            // +0x1BC  the card of the row whose menu is open
    float previewAlpha;     // +0x1C0
    u32 windowMgr;          // +0x1C4
    u32 playerData;         // +0x1C8
};
static_assert(sizeof(Safe) == 0x1CC && offsetof(Safe, rows) == 0xAC && offsetof(Safe, cursor) == 0x174 &&
              offsetof(Safe, page) == 0x190 && offsetof(Safe, tabs) == 0x1B0 && offsetof(Safe, state) == 0xA4,
              "Safe");

inline SpriteSet* SafeSprites(Safe* s) { return P<SpriteSet>(*P<u32>(s->sprites)); }
inline Column* SafeColumn(Safe* s) { return P<Column>(P<u32>(s->sprites)[1]); }
inline s32 MostCarried(Safe* s) { return P<s32>(s->sprites)[2]; }
inline int Selected(Safe* s) { return s->cursor + s->pageNo * 10; }

Safe* Safe_Ctor(Safe* self, u32 sprites) {   // 0x298970
    memset(self, 0, sizeof *self);
    self->sprites = sprites;
    PadRepeatPair_Ctor(&self->keys);
    self->keys.vtable = 0x2EAA00u;
    PadRepeatPair_SetMasks(&self->keys, kPadUp, kPadDown);
    for (Row& row : self->rows) {
        row.sprites = sprites;
        row.display = guest::addr(&self->display);
        row.metrics = guest::addr(&self->rowMetrics);
    }
    self->sort = {sprites, guest::addr(self), guest::addr(Text(0xE6)), 0, 0, 0x2ED1A0u};
    self->page = {sprites, guest::addr(self), 0, 0, 0, 0x2ED180u};
    self->windowMgr = guest::addr(*gWindowMgr);
    self->playerData = guest::addr(*gPlayerData);
    self->state = 1;
    self->tabs[0] = guest::addr(&self->page);
    self->tabs[1] = guest::addr(&self->sort);
    self->sort.state = 2;
    PadRepeatPair_SetTiming(&self->keys, 10, 3, 7);
    return self;
}

// ── 0x2986C0: on coming to the safe ───────────────────────────────────────
void Safe_Enter(Safe* self) {
    self->state = 1;
    self->cursor = 0;
    self->tab = 0;
    self->pageNo = 0;
    self->arrowLeft = 0;
    self->arrowRight = 1;
    self->page.text = guest::addr(Text(0xD4));
    self->page.timer = Frames(7);
    self->page.state = 4;
    self->sort.state = 0;
    self->sort.timer = 0;
    for (Row& row : self->rows)
        row.state = row.timer = 0;
    self->display = *reinterpret_cast<s16*>(Pd() + 0x6C82);
}
void Safe_Leave(Safe* self) { *reinterpret_cast<s16*>(Pd() + 0x6C82) = (s16)self->display; }    // 0x2986B0

// The page before / after; → there was one.
bool Safe_PageBack(Safe* self) {
    if (--self->pageNo < 0) {
        self->pageNo = 0;
        return false;
    }
    self->arrowRight = 1;
    if (self->pageNo < 1)
        self->arrowLeft = 0;
    self->page.text = guest::addr(Text(0xD4 + self->pageNo));
    Sound_PlaySE(2, 0, 0x3FFF);
    return true;
}
bool Safe_PageOn(Safe* self) {
    const s32 pages = Capacity() / 10;
    if (++self->pageNo >= pages) {
        self->pageNo = pages - 1;
        return false;
    }
    self->arrowLeft = 1;
    if (pages - 1 <= self->pageNo)
        self->arrowRight = 0;
    self->page.text = guest::addr(Text(0xD4 + self->pageNo));
    Sound_PlaySE(2, 0, 0x3FFF);
    return true;
}
void Safe_TurnPages(Safe* self, u16 pad) {
    if ((pad & kPadLeft) != 0)
        Safe_PageBack(self);
    else if ((pad & kPadRight) != 0)
        Safe_PageOn(self);
}

// ── 0x297830 / 0x2979D0: the cursor a row down / up, over the page's edge
// onto the next page; → it moved ──────────────────────────────────────────
bool Safe_CursorDown(Safe* self) {
    const s32 old = self->cursor;
    bool turned = false;
    if (++self->cursor > 9) {
        if (!Safe_PageOn(self)) {
            self->cursor = 9;
            return false;
        }
        self->cursor = 0;
        turned = true;
    }
    self->rows[old].state = turned ? 2 : 0;
    if (turned)
        self->rows[old].timer = 0;
    self->rows[self->cursor].state = 4;
    return true;
}
bool Safe_CursorUp(Safe* self, bool fade) {
    const s32 old = self->cursor;
    if (--self->cursor < 0) {
        if (!Safe_PageBack(self)) {
            self->cursor = 0;
            return false;
        }
        fade = true;
        self->cursor = 9;
    }
    self->rows[old].state = fade ? 2 : 0;
    if (fade)
        self->rows[old].timer = 0;
    self->rows[self->cursor].state = 4;
    return true;
}

void Safe_Message(Safe* self, int text) {
    if (self->window != 0)
        CloseWindow(self->windowMgr, self->window);
    self->window = OpenMessage(self->windowMgr, text, kZ);
    self->state = 0x10;
}

// After a sword has left the safe: back to the rows, or to the tabs if it
// was the last.
int Safe_AfterRemoval(Safe* self) {
    if (Stored() == 0) {
        self->state = 1;
        self->rows[self->cursor].state = 2;
        self->rows[self->cursor].timer = 0;
        P<Tab>(self->tabs[self->tab])->state = 2;
        return 2;
    }
    self->state = 2;
    return 1;
}
// ── 0x297B30: "Drop" confirmed ────────────────────────────────────────────
int Safe_Drop(Safe* self) {
    if (Carried() + Stored() == 1) {
        Safe_Message(self, 0xD1);
        return 0;
    }
    const int index = Selected(self);
    if (Stored() - 1 <= index)
        Safe_CursorUp(self, true);
    PlayerData_RemoveStoredSword(*gPlayerData, index);
    return Safe_AfterRemoval(self);
}
// ── 0x297CB0: "Ready" — out of the safe and into the hands ────────────────
int Safe_Take(Safe* self) {
    const s32 most = MostCarried(self);
    if (Carried() >= most) {
        if (most == 2)
            Safe_Message(self, 0xCF);           // "Cannot equip any more swords"
        else if (most == 3)
            Safe_Message(self, 0xD0);           // "You already have 3 swords equipped"
        return 0;
    }
    const int index = Selected(self);
    if (Stored() - 1 <= index)
        Safe_CursorUp(self, true);
    PlayerData_CarrySword(*gPlayerData, reinterpret_cast<const Sword*>(StoredSword(index)));
    PlayerData_RemoveStoredSword(*gPlayerData, index);
    return Safe_AfterRemoval(self);
}

// ── 0x299B80: the order of the kinds of sword in the "Normal" sorting ─────
int KindOrder(int type) {
    if (type == 0x46) return 0x14;
    if (type == 0x45) return 0x13;
    if (type == 0x42) return 0x2D;
    if (type > 0x12) type++;
    if (type > 0x13) type++;
    if (type > 0x2C) type++;
    if (type > 0x45) type--;
    if (type > 0x44) type--;
    if (type > 0x41) type--;
    return type;
}
// ── 0x296740: sorts the safe ──────────────────────────────────────────────
void Safe_Sort(int by) {
    const auto goesAfter = [by](const Sword* a, const Sword* b) {
        const u8* x = reinterpret_cast<const u8*>(a);
        const u8* y = reinterpret_cast<const u8*>(b);
        const auto i32 = [](const u8* p, u32 o) { s32 v; memcpy(&v, p + o, 4); return v; };
        const auto u16at = [](const u8* p, u32 o) { u16 v; memcpy(&v, p + o, 2); return v; };
        switch (by) {
        case 0: {
            const int ka = KindOrder(i32(x, 0x7C)), kb = KindOrder(i32(y, 0x7C));
            return ka == kb ? i32(y, 0x8C) < i32(x, 0x8C) : kb < ka;
        }
        case 1: return i32(x, 0x74) < i32(y, 0x74);
        case 2: return i32(x, 0x60) < i32(y, 0x60);
        case 3: return u16at(x, 0x66) == u16at(y, 0x66) ? u16at(x, 0x64) < u16at(y, 0x64) : u16at(x, 0x66) < u16at(y, 0x66);
        case 4: return i32(x, 0x58) < i32(y, 0x58);
        case 5: return i32(x, 0x5C) < i32(y, 0x5C);
        case 6: return i32(x, 0x88) < i32(y, 0x88);
        default: return false;
        }
    };
    Sword* swords = reinterpret_cast<Sword*>(Pd() + 0x880);
    const int count = Stored();
    for (int i = 1; i < count; i++) {
        for (int j = i - 1; j >= 0 && goesAfter(&swords[j], &swords[j + 1]); j--) {
            alignas(16) Sword held;
            memcpy(&held, &swords[j], sizeof held);
            memcpy(&swords[j], &swords[j + 1], sizeof held);
            memcpy(&swords[j + 1], &held, sizeof held);
        }
    }
}

// ── 0x298430: a row's menu, with the sword's card beside the list ─────────
void Safe_OpenRowMenu(Safe* self) {
    static const int kOptions[3] = {0xCA, 0xC9, 0xCB};      // Ready / Drop / Check moves
    self->window = OpenMenu(self->windowMgr, 0, kOptions, 3, 0, 0, 0, 0, 0);
    CardHead* card = static_cast<CardHead*>(
        Column_MakePreview(SafeColumn(self), guest::addr(StoredSword(Selected(self)))));
    self->preview = guest::addr(card);
    if (card != nullptr) {
        swordcards::SetTexts(card);
        card->z = kZ;
        card->x = (u16)Screen_X(self->cardAt[0] << 4, -1);
        card->y = (u16)Screen_Y(self->cardAt[1] << 4, -1);
        self->previewAlpha = 0.0f;
        card->alpha = 0.0f;
    }
    self->state = 4;
}

// ── 0x297410 (with 0x2970F0, 0x296DC0, 0x296C40, 0x296B20, 0x2969D0) ──────
// → {what, which}: 2 back to the top menu, 6 show the moves of stored sword `which`
void Safe_Update(s32 out[2], Safe* self, bool turning) {
    out[0] = out[1] = 0;
    for (const u32 at : self->tabs) {
        Tab* tab = P<Tab>(at);
        if (tab->state == 2) {
            if (Frames(7) <= ++tab->timer) {
                tab->state = 4;
                tab->timer = Frames(7);
            }
        } else if (tab->state == 1 && --tab->timer < 1) {
            tab->state = 0;
            tab->timer = 0;
        }
    }
    for (Row& row : self->rows) {
        if (row.state != 0 && row.state != 4 && self->rowMetrics.fadeFrames < ++row.timer) {
            if (row.state == 2)
                row.state = 0;
            else if (row.state == 1)
                row.state = 4;
        }
    }
    if (self->preview != 0) {
        const s32 frames = Frames(8);
        const float step = 1.0f / (float)(frames != 0 ? frames : 1);
        if (self->state == 4 || self->state == 0x20) {
            self->previewAlpha += step;
            if (self->previewAlpha > 1.0f)
                self->previewAlpha = 1.0f;
            P<CardHead>(self->preview)->alpha = self->previewAlpha;
        } else {
            self->previewAlpha -= step;
            if (self->previewAlpha <= 0.0f) {
                Column_DropPreview(SafeColumn(self));
                self->preview = 0;
            } else {
                P<CardHead>(self->preview)->alpha = self->previewAlpha;
            }
        }
    }
    if ((self->state & 3) != 0 && (Pad_GetPressed(PadMgr_Get(), 0) & kPadSquare) != 0)
        self->display = self->display == 0;

    switch (self->state) {
    case 1: {                                   // 0x2970F0: on the tabs
        const u16 pad = PadNow();
        if (!turning) {
            if ((pad & kPadTriangle) != 0) {
                Sound_PlaySE(7, 0, 0x3FFF);
                out[0] = 2;
                return;
            }
            if ((pad & kPadCross) != 0) {
                Sound_PlaySE(6, 0, 0x3FFF);
                if (self->tab == 1) {           // "Change Order"
                    static const int kOrders[7] = {0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4};
                    self->window = OpenMenu(self->windowMgr, 0, kOrders, 7, 0, 0x3C, 0, 0, 0);
                    self->state = 0x40;
                } else if (self->tab == 0) {    // into the rows
                    self->state = 2;
                    self->rows[self->cursor].state = 1;
                    self->rows[self->cursor].timer = 0;
                }
                return;
            }
        }
        const s32 old = self->tab;
        if ((pad & kPadUp) != 0) {
            if (--self->tab < 0)
                self->tab = 1;
        } else if ((pad & kPadDown) != 0) {
            if (++self->tab > 1)
                self->tab = 0;
        }
        if (self->tab != old) {
            P<Tab>(self->tabs[old])->state = 1;
            P<Tab>(self->tabs[self->tab])->state = 2;
            Sound_PlaySE(3, 0, 0x3FFF);
        }
        if (self->tab == 0)
            Safe_TurnPages(self, pad);
        break;
    }
    case 2: {                                   // 0x296DC0: on the rows
        const u16 pad = PadStick();
        if ((pad & kPadCross) != 0) {
            if (Selected(self) < Stored()) {
                Sound_PlaySE(1, 0, 0x3FFF);
                Safe_OpenRowMenu(self);
            } else {
                Sound_PlaySE(7, 0, 0x3FFF);
            }
        } else if ((pad & kPadTriangle) != 0) {
            self->state = 1;
            self->rows[self->cursor].state = 2;
            self->rows[self->cursor].timer = 0;
            self->page.state = 2;
        } else {
            Safe_TurnPages(self, pad);
            PadRepeat_Update(&self->keys.a);
            PadRepeat_Update(&self->keys.b);
            if (!PadRepeat_IsHeld(&self->keys.b) && self->keys.a.fired != 0) {
                if (Safe_CursorUp(self, false))
                    Sound_PlaySE(0x1A, 0, 0x3FFF);
            } else if (!PadRepeat_IsHeld(&self->keys.a) && self->keys.b.fired != 0) {
                if (Safe_CursorDown(self))
                    Sound_PlaySE(0x1A, 0, 0x3FFF);
            }
        }
        break;
    }
    case 4: {                                   // 0x296C40: a row's menu
        const u16 pad = PadStick();
        if ((pad & kPadCross) != 0) {
            const int chosen = WindowMgr_GetCursor(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
            CloseWindow(self->windowMgr, self->window);
            if (chosen < 0 || chosen > 2)
                break;
            Sound_PlaySE(5, 0, 0x3FFF);
            if (chosen == 2) {
                self->state = 2;
                self->previewAlpha = 0.0f;
                out[0] = 6;
                out[1] = Selected(self);
            } else if (chosen == 1) {
                static const int kYesNo[2] = {0xF, 0x10};
                self->window = OpenMenu(self->windowMgr, 0xCD, kYesNo, 2, -15, 0xAA, 100, 100, 1);
                self->state = 0x20;
            } else {
                Safe_Take(self);
            }
        } else if ((pad & kPadTriangle) != 0) {
            Sound_PlaySE(7, 0, 0x3FFF);
            CloseWindow(self->windowMgr, self->window);
            self->state = 2;
        }
        break;
    }
    case 0x10:
        if ((PadStick() & (kPadCross | kPadTriangle)) != 0) {
            CloseWindow(self->windowMgr, self->window);
            self->state = 2;
        }
        break;
    case 0x20: {                                // 0x296B20: "drop?"
        const u16 pad = PadStick();
        if ((pad & kPadCross) != 0) {
            const int chosen = WindowMgr_GetCursor(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
            CloseWindow(self->windowMgr, self->window);
            if (chosen == 1) {
                Sound_PlaySE(7, 0, 0x3FFF);
                self->state = 2;
            } else if (chosen == 0) {
                Sound_PlaySE(5, 0, 0x3FFF);
                Safe_Drop(self);
            }
        } else if ((pad & kPadTriangle) != 0) {
            Sound_PlaySE(7, 0, 0x3FFF);
            CloseWindow(self->windowMgr, self->window);
            self->state = 2;
        }
        break;
    }
    case 0x40: {                                // 0x2969D0: the order menu
        const u16 pad = PadStick();
        if ((pad & kPadCross) != 0) {
            Sound_PlaySE(5, 0, 0x3FFF);
            const int chosen = WindowMgr_GetCursor(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
            CloseWindow(self->windowMgr, self->window);
            self->state = 1;
            Safe_Sort(chosen);
            if (chosen >= 1 && chosen <= 3)
                self->display = 0;
            else if (chosen >= 4 && chosen <= 6)
                self->display = 1;
        }
        if ((pad & kPadTriangle) != 0) {
            Sound_PlaySE(7, 0, 0x3FFF);
            CloseWindow(self->windowMgr, self->window);
            self->state = 1;
        }
        break;
    }
    default:
        break;
    }
}

// ── 0x298EB0: a tab; → where its text is (x | y << 16) ────────────────────
u32 Tab_Draw(Safe* self, Tab* tab, int sprite, const s32* at, u8 alpha) {
    Font* font = *gFont;
    SpriteSet* sprites = SafeSprites(self);
    const s32 full = Frames(7);
    const float out = (float)tab->timer / (float)(full != 0 ? full : 1);
    const u32 x = (Screen_X(at[0] << 4, -1) + ((u32)(s32)(out * 28.0f) & 0xFFF) * 0x10) & 0xFFFF;
    const u32 y = Screen_Y(at[1] << 4, -1) & 0xFFFF;
    SpriteSet_Draw(sprites, sprite, (int)x, (int)y, kZ,
                   (((u32)(s32)(out * 40.0f) & 0xFF) * 0x10101 + 0x585858) | (u32)alpha << 24);
    Font_SetColor(font, ((tab->state & 6) != 0 ? 0xACB2BEu : 0x212121u) | (u32)alpha << 24);
    GsDevice_End2D(*gGsDevice);
    u32 tx = x, ty = y;
    const u8* record = static_cast<const u8*>(SpriteSet_GetSprite(sprites, sprite));
    if (record != nullptr && tab->text != 0) {
        s16 w, h;
        memcpy(&w, record + 0xC, 2);
        memcpy(&h, record + 0xE, 2);
        ty = y + (u32)(h * 8);
        tx = (x + (((u32)(u16)w * 8 + 0xE0) & 0xFFFF)) & 0xFFFF;
        Font_DrawCentered(font, P<const u16>(tab->text), (int)tx, (int)ty, kZ, 0.9f, 0.0f);
    }
    GsDevice_Begin2D(*gGsDevice);
    return tx | ty << 16;
}

// A flat triangle (0x2258E0).
void FillTriangle(const s32 x[3], const s32 y[3], u32 rgba) {
    struct Packet {
        u64 prim;
        struct { u32 rgba; float q; u16 x, y; u32 z; } v[3];
    } packet;
    memset(&packet, 0, sizeof packet);
    packet.prim = 0x43;
    for (int i = 0; i < 3; i++)
        packet.v[i] = {rgba, 1.0f, (u16)x[i], (u16)y[i], kZ};
    GsDevice_DrawPrim(*gGsDevice, &packet, 3);
}

// ── 0x298BF0: the page tab, with an arrow each side of its text ───────────
void PageTab_Draw(Safe* self, u8 alpha) {
    const u32 at = Tab_Draw(self, &self->page, 4, self->pageAt, alpha);
    const float t = (float)self->page.timer / 7.0f;
    const u32 r = 62 + (u32)(s32)((float)(0x59 - 62) * t), g = 128 + (u32)(s32)((float)(0xB7 - 128) * t);
    const u32 rgba = (r & 0xFF) | (g & 0xFF) << 8 | (g & 0xFF) << 16 | (u32)alpha << 24;
    s32 size[2] = {0, 0};
    Text_MeasurePixels(P<const u16>(self->page.text), size);
    const s32 cx = (s32)(at & 0xFFFF), cy = (s32)(at >> 16);
    const s32 ys[3] = {cy - 0xA0, cy + 0xA0, cy};
    if (self->arrowRight != 0) {
        const s32 x0 = cx + (size[0] + 0xC) * 8;
        const s32 xs[3] = {x0, x0, x0 + 0xA0};
        FillTriangle(xs, ys, rgba);
    }
    if (self->arrowLeft != 0) {
        const s32 x0 = cx - (size[0] + 0xC) * 8;
        const s32 xs[3] = {x0, x0, x0 - 0xA0};
        FillTriangle(xs, ys, rgba);
    }
}

// ── 0x2997C0 / 0x299170: a row's plates, and its words ────────────────────
void Row_DrawPlates(Safe* self, const Row* row, float amount, int x, int y) {
    SpriteSet* sprites = SafeSprites(self);
    const RowMetrics* m = &self->rowMetrics;
    const auto plates = [&](int first, int rest, u32 tint) {
        SpriteSet_Draw(sprites, first, x + m->cell[0][0] * 0x10, y + m->cell[0][1] * 0x10, kZ, tint);
        for (int i = 1; i < 4; i++)
            SpriteSet_Draw(sprites, rest, x + m->cell[i][0] * 0x10, y + m->cell[i][1] * 0x10, kZ, tint);
    };
    plates(7, 9, Alpha(amount) | 0x808080);
    const float through = (float)row->timer / (float)(m->fadeFrames != 0 ? m->fadeFrames : 1);
    float lit;
    switch (row->state) {
    case 2: lit = 1.0f - through; break;
    case 1: lit = through; break;
    case 4: lit = 1.0f; break;
    default: return;
    }
    plates(8, 10, Alpha(lit * amount) | 0x808080);
}
void Row_DrawWords(Safe* self, const Row* row, float amount, int x, int y, const u8* sword) {
    Font* font = *gFont;
    SpriteSet* sprites = SafeSprites(self);
    const RowMetrics* m = &self->rowMetrics;
    GsDevice_End2D(*gGsDevice);
    Font_SetColor(font, Alpha(amount) | ((row->state & 5) == 0 ? 0xACB2BEu : 0x212121u));
    const u8* wide = static_cast<const u8*>(SpriteSet_GetSprite(sprites, 7));
    const u8* narrow = static_cast<const u8*>(SpriteSet_GetSprite(sprites, 9));
    if (wide != nullptr && narrow != nullptr) {
        const auto half = [](const u8* record, u32 o) { u16 v; memcpy(&v, record + o, 2); return (int)v * 8; };
        const auto i32 = [sword](u32 o) { s32 v; memcpy(&v, sword + o, 4); return v; };
        const auto u16at = [sword](u32 o) { u16 v; memcpy(&v, sword + o, 2); return (int)v; };
        Font_DrawCentered(font, reinterpret_cast<const u16*>(sword), half(wide, 0xC) + x + m->cell[0][0] * 0x10,
                          half(wide, 0xE) + y + m->cell[0][1] * 0x10, kZ, 0.9f, 0.0f);
        const u16* words[3];
        if (self->display == 0) {
            words[0] = Text(i32(0x74) + 0x33);
            words[1] = Num(i32(0x60));
            words[2] = Num(u16at(0x64), Text(2), u16at(0x66));
        } else {
            words[0] = Num(i32(0x58));
            words[1] = Num(i32(0x5C));
            words[2] = Num(i32(0x88));
        }
        for (int i = 0; i < 3; i++)
            Font_DrawCentered(font, words[i], half(narrow, 0xC) + x + m->cell[i + 1][0] * 0x10,
                              half(narrow, 0xE) + y + m->cell[i + 1][1] * 0x10, kZ, 0.9f, 0.0f);
    }
    GsDevice_Begin2D(*gGsDevice);
}

// ── 0x2964A0 (with 0x296010, 0x295D60, 0x295BB0) ──────────────────────────
void Safe_Draw(Safe* self, float amount) {
    Font* font = *gFont;
    SpriteSet* sprites = SafeSprites(self);
    const u32 a = (u32)(s32)(amount * 128.0f);
    const int x = X0() + self->frame[0] * 0x10, y = Y0() + self->frame[1] * 0x10;
    const s32 rect[4] = {x, y, (self->frame[2] - self->frame[0]) * 0x10, (self->frame[3] - self->frame[1]) * 0x10};
    Gs_FillRect(rect, kZ, a << 24 | 0xACB2BE);

    // The headings (0x296010).
    const RowMetrics* m = &self->rowMetrics;
    const int hx = x & 0xFFFF, hy = y & 0xFFFF;
    SpriteSet_Draw(sprites, 5, hx + m->cell[0][0] * 0x10, hy + m->cell[0][1] * 0x10, kZ, a << 24 | 0x808080);
    for (int i = 1; i < 4; i++)
        SpriteSet_Draw(sprites, 6, hx + m->cell[i][0] * 0x10, hy + m->cell[i][1] * 0x10, kZ, a << 24 | 0x808080);
    const u8* wide = static_cast<const u8*>(SpriteSet_GetSprite(sprites, 5));
    const u8* narrow = static_cast<const u8*>(SpriteSet_GetSprite(sprites, 6));
    if (wide != nullptr && narrow != nullptr) {
        const auto half = [](const u8* record, u32 o) { u16 v; memcpy(&v, record + o, 2); return (int)v * 8; };
        GsDevice_End2D(*gGsDevice);
        Font_SetColor(font, a << 24 | 0xACB2BE);
        Font_DrawCentered(font, Text(0xE5), half(wide, 0xC) + hx + m->cell[0][0] * 0x10,
                          half(wide, 0xE) + hy + m->cell[0][1] * 0x10, kZ, 0.9f, 0.0f);
        static const int kHeads[2][3] = {{0x2D, 0x2E, 0x31}, {0x2F, 0x30, 0x32}};
        for (int i = 0; i < 3; i++)
            Font_DrawCentered(font, Text(kHeads[self->display != 0][i]), half(narrow, 0xC) + hx + m->cell[i + 1][0] * 0x10,
                              half(narrow, 0xE) + hy + m->cell[i + 1][1] * 0x10, kZ, 0.9f, 0.0f);
        GsDevice_Begin2D(*gGsDevice);
    }

    // The rows.
    int shown = Stored() - self->pageNo * 10;
    if (shown > 9)
        shown = 10;
    u32 rowY = (u32)hy + (((u32)self->rowStep * 0x10 + 0x20) & 0xFFFF);
    for (int i = 0; i < 10; i++) {
        rowY &= 0xFFFF;
        Row_DrawPlates(self, &self->rows[i], amount, hx, (int)rowY);
        if (i < shown)
            Row_DrawWords(self, &self->rows[i], amount, hx, (int)rowY, StoredSword(self->pageNo * 10 + i));
        rowY += ((u32)self->rowStep & 0xFFF) * 0x10;
    }

    // How full the safe is (0x295D60).
    {
        const int cx = (X0() + self->countAt[0] * 0x10) & 0xFFFF, cy = (Y0() + self->countAt[1] * 0x10) & 0xFFFF;
        GsDevice_End2D(*gGsDevice);
        Font_SetColor(font, (a & 0xFF) << 24 | 0xACB2BE);
        SpriteSet_Draw(sprites, 2, cx, cy, kZ, (a & 0xFF) << 24 | 0x808080);
        if (const u8* plate = static_cast<const u8*>(SpriteSet_GetSprite(sprites, 2))) {
            u16 w, h;
            memcpy(&w, plate + 0xC, 2);
            memcpy(&h, plate + 0xE, 2);
            Font_DrawCentered(font, Num(Stored(), Text(2), Capacity()), (cx + (w & 0x1FFF) * 8) & 0xFFFF,
                              (cy + (h & 0x1FFF) * 8) & 0xFFFF, kZ, 0.9f, 0.0f);
        }
        GsDevice_Begin2D(*gGsDevice);
    }
    Tab_Draw(self, &self->sort, 3, self->sortAt, (u8)a);
    PageTab_Draw(self, (u8)a);

    // What the buttons do (0x295BB0).
    GsDevice_End2D(*gGsDevice);
    Font_SetColor(font, (a & 0xFF) << 24 | 0xACB2BE);
    if ((self->state & 3) != 0) {
        Font_DrawString(font, Text(0xD3), X0() + 0xF50, Y0() + 0x18B0, kZ, 1.0f, 0.0f);
        Font_DrawString(font, Text(0x16), X0() + 0x18E0, Y0() + 0x18B0, kZ, 1.0f, 0.0f);
        Font_DrawString(font, Text(0x17), X0() + 0x1F30, Y0() + 0x18B0, kZ, 1.0f, 0.0f);
    }
    GsDevice_Begin2D(*gGsDevice);
}

// ═══ The screen (0x464 bytes) ══════════════════════════════════════════════
struct SwordSelect {
    s32 phase;              // +0x000  1 sprites, 2 background, 3 numbers, 4, 5 ready
    u32 sprites;            // +0x004  asset 0x58
    u32 column;             // +0x008  → the carried swords' part
    s32 mostCarried;        // +0x00C  2 at the character setup, 3 at home
    Bg bg;                  // +0x010
    Top top;                // +0x054
    Column carried;         // +0x0E0
    Safe safe;              // +0x290
    u32 params;             // +0x45C
    u32 swords;             // +0x460
};
static_assert(sizeof(SwordSelect) == 0x464 && offsetof(SwordSelect, carried) == 0xE0 &&
              offsetof(SwordSelect, safe) == 0x290 && offsetof(SwordSelect, params) == 0x45C, "SwordSelect");

// ── 0x26EC80: the view in front (or the one being faded to) ───────────────
void Dispatch(s32 out[2], SwordSelect* self, s32 view, bool turning) {
    out[0] = out[1] = 0;
    s32 result[2] = {0, 0};
    if (view == 3) {
        Safe_Update(result, &self->safe, turning);
        if (result[0] == 6) {
            out[0] = 2;
            out[1] = (s32)guest::addr(StoredSword(result[1]));
        } else if (result[0] == 2) {
            Bg_GoTo(&self->bg, 1);
            Safe_Leave(&self->safe);
        }
    } else if (view == 2) {
        Column_Update(result, &self->carried, turning);
        if (result[0] == 8) {
            Bg_TurnBack(&self->bg);
        } else if (result[0] == 7) {
            Bg_Hold(&self->bg);
        } else if (result[0] == 6) {
            out[0] = 2;
            out[1] = (s32)guest::addr(Pd() + 0x180 + result[1] * 0x100);
        } else if (result[0] == 2) {
            Bg_GoTo(&self->bg, 1);
            Column_Leave(&self->carried);
        }
    } else if (view == 1) {
        const int chosen = Top_Update(&self->top, turning);
        if (chosen == 1) {
            out[0] = 1;
        } else if (chosen == 4) {
            Bg_GoTo(&self->bg, 3);
            Safe_Enter(&self->safe);
        } else if (chosen == 3) {
            Bg_GoTo(&self->bg, 2);
            Column_Enter(&self->carried);
        }
    }
}

void DrawView(SwordSelect* self, s32 view, float amount) {
    SpriteSet* sprites = P<SpriteSet>(self->sprites);
    if (view == 3) {
        SpriteSet_Bind(sprites);
        Safe_Draw(&self->safe, amount);
        Column_DrawPreview(&self->carried);
    } else if (view == 2) {
        Column_Draw(&self->carried, amount, sprites);
    } else if (view == 1) {
        SpriteSet_Bind(sprites);
        Top_Draw(&self->top, amount);
    }
}

}  // namespace

extern "C" {

// ── 0x26F320 ──────────────────────────────────────────────────────────────
void* SwordSelect_Ctor(void* mem) {
    SwordSelect* self = static_cast<SwordSelect*>(mem);
    memset(self, 0, sizeof *self);
    self->bg.view = 1;
    Top_Ctor(&self->top, guest::addr(&self->sprites));
    Column_Ctor(&self->carried);
    Safe_Ctor(&self->safe, guest::addr(&self->sprites));
    self->column = guest::addr(&self->carried);
    return self;
}

// ── 0x26F270 ──────────────────────────────────────────────────────────────
void SwordSelect_Dtor(void* mem, int flag) {
    SwordSelect* self = static_cast<SwordSelect*>(mem);
    if (self == nullptr)
        return;
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    if (self->params != 0)
        ParamFile_Dtor(P<ParamFile>(self->params), 1);
    self->params = 0;
    Column_Dtor(&self->carried);
    if (self->bg.sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->bg.sprites), 1);
    self->bg.sprites = 0;
    if ((s16)flag > 0)
        MemFree(self);
}

// ── 0x26F150: `mostCarried` swords may be carried ─────────────────────────
char SwordSelect_Start(void* mem, int mostCarried) {
    SwordSelect* self = static_cast<SwordSelect*>(mem);
    self->swords = guest::addr(Pd() + 0x180);
    self->carried.ready = 1;
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    void* set = MemAlloc(0x14);
    self->sprites = set != nullptr ? guest::addr(SpriteSet_Ctor(set)) : 0;
    if (self->sprites == 0)
        return 0;
    if (SpriteSet_Load(P<SpriteSet>(self->sprites), 0x58, -1) == 0) {
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
        self->sprites = 0;
        return 0;
    }
    self->phase = 1;
    self->mostCarried = mostCarried;
    return 1;
}

// ── 0x26EE90: a step of the loading ───────────────────────────────────────
void SwordSelect_Load(void* mem) {
    SwordSelect* self = static_cast<SwordSelect*>(mem);
    switch (self->phase) {
    case 1:
        if (SpriteSet_IsLoaded(P<SpriteSet>(self->sprites)) != 0) {
            void* set = MemAlloc(0x14);                         // 0x295A20
            self->bg.sprites = set != nullptr ? guest::addr(SpriteSet_Ctor(set)) : 0;
            if (self->bg.sprites == 0)
                return;
            if (SpriteSet_Load(P<SpriteSet>(self->bg.sprites), 0x59, -1) == 0) {
                SpriteSet_Dtor(P<SpriteSet>(self->bg.sprites), 1);
                self->bg.sprites = 0;
                return;
            }
            self->bg.busy = 0;
            self->phase = 2;
        }
        break;
    case 2:
        if (SpriteSet_IsLoaded(P<SpriteSet>(self->bg.sprites)) != 0) {
            self->phase = 3;
            void* file = MemAlloc(sizeof(ParamFile));
            self->params = file != nullptr ? guest::addr(ParamFile_Ctor(static_cast<ParamFile*>(file))) : 0;
            if (self->params != 0)
                ParamFile_Request(P<ParamFile>(self->params), 0x5B);
        }
        break;
    case 3:
        if (self->params != 0 && ParamFile_Poll(P<ParamFile>(self->params)) == 0) {
            BufferReader* r = &P<ParamFile>(self->params)->out;
            Take(r, self->bg.pattern, 0x30);                    // 0x2959B0
            Take(r, self->top.at[0], 8);                        // 0x294560
            Take(r, self->top.at[1], 8);
            Take(r, self->top.at[2], 8);
            Take(r, &self->top.width, 8);
            Take(r, &self->carried.metrics, 0x2C);              // 0x292610
            Take(r, &self->safe, 0x34);                         // 0x2987D0
            Take(r, &self->safe.rowMetrics, 0x24);
            ParamFile_Dtor(P<ParamFile>(self->params), 1);
            self->params = 0;
            self->phase = 4;
            self->carried.ready = 1;
        }
        break;
    case 4:
        self->phase = 5;
        break;
    default:
        break;
    }
}

// ── 0x26F040: → {0} going on, {1} done, {2, sword} show that sword's moves ─
void SwordSelect_Update(s32 out[2], void* mem) {
    SwordSelect* self = static_cast<SwordSelect*>(mem);
    out[0] = out[1] = 0;
    if (self->sprites == 0) {
        out[0] = 1;
        return;
    }
    if (self->phase != 5) {
        SwordSelect_Load(self);
        return;
    }
    if (Bg_Step(&self->bg) == 0) {
        Dispatch(out, self, self->bg.view, false);
    } else if ((float)self->bg.timer / 15.0f >= 0.5f) {
        s32 ignored[2];
        Dispatch(ignored, self, self->bg.next, true);
    }
}

// ── 0x26EA40 ──────────────────────────────────────────────────────────────
void SwordSelect_Draw(void* mem) {
    SwordSelect* self = static_cast<SwordSelect*>(mem);
    if (self->phase != 5)
        return;
    GsDevice_Begin2D(*gGsDevice);
    Bg_Draw(&self->bg);
    if (self->bg.next == 0) {
        DrawView(self, self->bg.view, 1.0f);
    } else {
        const float t = (float)self->bg.timer / 15.0f;
        DrawView(self, self->bg.view, 1.0f - t);
        DrawView(self, self->bg.next, t);
    }
    GsDevice_End2D(*gGsDevice);
}

}  // extern "C"
