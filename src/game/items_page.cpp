// The status menu's second page, "Item Selection" (0x1E5C30–0x1E7440, 0x40
// bytes, vtable 0x2EBB70), its three-column list (0x22F930–0x230610 and the
// column class 0x2308F0–0x2315B0) and what using an item does (0x2333A0–
// 0x233DB0, methods of the item table).
//
// Three lists stand one behind another — what is carried (8 places), what is
// kept (4) and the accessories (28); Left / Right bring another to the front,
// Up / Down move in it.  Cross on a thing opens a small menu: Use / Move /
// Drop for a carried thing, Wear (or Take off) / Move for an accessory.
// "Move" marks the place, and Cross on another swaps the two.
//
// While loading the page also reads the chart of each carried sword's
// techniques (skill_sheets.cpp), which the scrolls that teach a technique
// (effect 8) word their notice from.

#include "engine/containers.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/pad_repeat.h"
#include "engine/text_label.h"
#include "engine/window.h"
#include "game/gameplay_scene.h"
#include "game/item_select.h"
#include "game/player_data.h"
#include "hal/ps2_types.h"
#include "native/guest.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

struct SpriteSet;
extern "C" {
    u32   SwordDb_fn15e4b0(void* db, u32 sheet);                            // 0x15E4B0  a style's skill sheet
    int   SwordDb_TeachByItem(void* db, void* sword, int level);            // 0x15C960
    int   SkillSheets_Request(void* self, int sheet, int slot);             // 0x23D870
    int   SkillSheets_Poll(void* self, int slot);                           // 0x23D7D0
    const u16* SkillSheets_MakeNotice(void* self, const void* sword, int slot, int technique, int next);    // 0x23BA90
    void  Accessories_Refresh();                                        // 0x2AB4C0
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    SpriteSet* SpriteSet_Ctor(void* mem);                               // 0x17C630
    char  SpriteSet_LoadByName(SpriteSet* set, const char* a, const char* b);   // 0x17C2F0
    int   SpriteSet_IsLoaded(SpriteSet* set);                           // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                               // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);                     // 0x17C570
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    int   SpriteSet_DrawEx(void* set, int index, void* params);         // 0x17ADF0
    void  Gs_FillRect(const s32* rect, u32 z, u32 rgba);                // 0x225CD0
    u32   Screen_X(int v, int align);                                   // 0x12E5E0
    u32   Screen_Y(int v, int align);                                   // 0x12E550
    void  Sound_PlaySE(int id, int randomPitch, int volume);            // 0x180E70
    void* PadMgr_Get();                                                 // 0x13A920
    void* MenuRes_Get();                                                // 0x22C8F0
    void  StatusFrame_SetHelp(void* frame, const u16* text);            // 0x22D460
    void  StatusFrame_SetHints(void* frame, int which);
    void  StatusFrame_ShowArrows(void* frame, int on);
    void  StatusFrame_TakeHealth(void* frame);
    void  StatusFrame_TakeVigour(void* frame);
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
const GuestPtrVar<Font>       gFont{0x2EE67Cu};
const GuestPtrVar<WindowMgr>  gWindowMgr{0x2EE680u};
const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
u32* const gFrameRate = guest::ptr<u32>(0x2EDA00u);

inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
inline u8* Pd() { return reinterpret_cast<u8*>(*gPlayerData); }

// ═══ What an item does ═════════════════════════════════════════════════════
// An item's record (ItemInfo) starts: u8 usable, u8 effects (two kinds, a
// nibble each), s16 amount for the first, s16 for the second, s16 the object
// it is when dropped; +0x10 its help line, +0x18 what is said on using it.
struct ItemUse { u8 usable, effects; s16 amount[2]; s16 object; };

// ── 0x2333A0: one effect; → 0 if it would do nothing ──────────────────────
// 1 health, 2 the most health (up to 2000), 3 vigour, 4 the most vigour (up
// to 15), 6 / 7 the sword's attack / defence (up to 500), 9–11 the three
// states of PlayerData+0x6E80.
bool Item_Apply(IconSet* set, int kind, s16 amount, u8* params, u8* sword) {
    u8* pd = Pd();
    const auto s16at = [](u8* p) -> s16& { return *reinterpret_cast<s16*>(p); };
    if ((kind == 7 || kind == 6)
        && (u32)*reinterpret_cast<u16*>(sword + 0x66) + 5 <= (u32)*reinterpret_cast<u16*>(sword + 0x64)) {
        set->_0c = guest::addr(Text(0xA5));
        return false;
    }
    switch (kind) {
    case 1: {
        const s16 max = s16at(params + 0x42), now = s16at(params + 0x40);
        if ((amount > 0 && max <= now) || (amount < 0 && now < 2) || now == 0)
            return false;
        if (amount > 0) {
            const s32 total = (s32)amount + *reinterpret_cast<u16*>(pd + 0x852);
            *reinterpret_cast<u16*>(pd + 0x852) = (u16)(total > 0xFFFF ? 0xFFFF : total);
        }
        s16 to = (s16)(now + amount);
        if (max < to)
            to = max;
        if (to < 1)
            to = 1;
        const s16 was = s16at(pd + 0x10E);
        s16at(pd + 0x10E) = s16at(pd + 0x110) <= to ? s16at(pd + 0x110) : to;
        if (s16at(pd + 0x10E) - was < 0)
            s16at(pd + 0x854) = (s16)(s16at(pd + 0x854) + (was - s16at(pd + 0x10E)));
        s16at(params + 0x40) = s16at(params + 0x42) <= to ? s16at(params + 0x42) : to;
        break;
    }
    case 2: {
        s16 max = s16at(params + 0x42);
        if ((amount > 0 && max > 1999) || (amount < 0 && max < 2))
            return false;
        max = (s16)(max + amount);
        if (max > 2000)
            max = 2000;
        if (max < 1)
            max = 1;
        s16 now = s16at(params + 0x40);
        if (max < now)
            now = max;
        s16at(pd + 0x110) = max > 1999 ? (s16)2000 : max;
        const s16 was = s16at(pd + 0x10E);
        s16at(pd + 0x10E) = s16at(pd + 0x110) <= now ? s16at(pd + 0x110) : now;
        if (s16at(pd + 0x10E) - was < 0)
            s16at(pd + 0x854) = (s16)(s16at(pd + 0x854) + (was - s16at(pd + 0x10E)));
        s16at(params + 0x42) = max;
        s16at(params + 0x40) = max <= now ? max : now;
        break;
    }
    case 3: {
        const s16 max = s16at(pd + 0x10A), now = s16at(pd + 0x108);
        if ((amount > 0 && max <= now) || (amount < 0 && now < 2))
            return false;
        s16 to = (s16)(now + amount);
        if (max < to)
            to = max;
        if (to < 1)
            to = 1;
        s16at(pd + 0x108) = max <= to ? max : to;
        break;
    }
    case 4: {
        const s16 max = s16at(pd + 0x10A);
        if ((amount > 0 && max > 14) || (amount < 0 && max < 2))
            return false;
        s16 to = (s16)(max + amount);
        if (to > 15)
            to = 15;
        if (to < 1)
            to = 1;
        s16 now = s16at(pd + 0x108);
        if (to < now)
            now = to;
        if (max <= now)
            now = max;
        s16at(pd + 0x108) = now;
        s16at(pd + 0x10A) = to;
        break;
    }
    case 6:
    case 7: {
        s32* value = reinterpret_cast<s32*>(sword + (kind == 6 ? 0x58 : 0x5C));
        if (*value > 499)
            return false;
        *value = *value + amount > 500 ? 500 : *value + amount;
        break;
    }
    case 9:
    case 10:
    case 11: {
        s32* state = reinterpret_cast<s32*>(pd + 0x6E80);
        if (*state != 0)
            return false;
        *state = kind - 8;
        break;
    }
    }
    return true;
}

// ── 0x233DB0: 1 it is used on the player, 2 on a sword ────────────────────
int Item_UseKind(IconSet* set, u32 item) {
    if (set->data != 0 && (u32)*P<s16>(set->data + 4) <= item)
        return 0;
    const ItemUse* use = P<ItemUse>(P<u32>(set->table)[item]);
    int kinds = 0;
    for (int i = 0; i < 2; i++) {
        const int kind = i == 0 ? use->effects & 0xF : (use->effects >> 4) & 0xF;
        switch (kind) {
        case 0: break;
        case 1: case 2: case 3: case 4: case 9: case 10: case 11: case 12: kinds |= 1; break;
        case 6: case 7: case 8: kinds |= 2; break;
        default: return 0;
        }
    }
    return kinds;
}

// ── 0x233A30: uses an item; what there is to say about it is left in the
// table (+0x0C) either way ─────────────────────────────────────────────────
int Item_Use(IconSet* set, u32 item, u8* params, int swordSlot) {
    u8* pd = Pd();
    u8* sword = pd + swordSlot * 0x100 + 0x180;
    if (set->data != 0 && (u32)*P<s16>(set->data + 4) <= item)
        return 0;
    const u32 record = P<u32>(set->table)[item];
    const ItemUse* use = P<ItemUse>(record);
    if (use->usable != 1)
        return 0;
    set->_0c = 0;
    if (use->effects == 8) {
        // A scroll: the sword learns a technique, and what is said is the
        // notice for it.  (The original leaves +0x0C empty and puts the
        // notice in the string at +0x10; callers read whichever is set.)
        void* db = SwordDb_Get();
        const int learnt = SwordDb_TeachByItem(db, sword, use->amount[0]);
        if (learnt == -1) {
            set->_0c = guest::addr(Text(0xA4));
            return 0;
        }
        const u32 sheetAt = SwordDb_fn15e4b0(db, *reinterpret_cast<const u32*>(sword + 0x70));
        if (sheetAt == 0)
            return 0;
        const s32 count = *P<s32>(sheetAt);
        if (learnt >= count)
            return 0;
        s32 next = P<s32>(P<u32>(sheetAt)[1])[learnt * 4 + 2];
        if (next < 0 || next > 0x3F || sword[0x90 + next] != 2)
            next = -1;
        const u16* notice = SkillSheets_MakeNotice(P(*P<u32>(0x2EE698u)), sword, swordSlot, learnt, next);
        if (notice == nullptr)
            return 0;
        set->_0c = guest::addr(notice);
        return 1;
    }
    const int first = use->effects & 0xF, second = (use->effects >> 4) & 0xF;
    bool did = false;
    if (first != 0)
        did = Item_Apply(set, first, use->amount[0], params, sword);
    if (second != 0)
        did = Item_Apply(set, second, use->amount[1], params, sword) || did;
    if (did) {
        u16* count = reinterpret_cast<u16*>(pd + 0x85C);
        if (item == 0x16 && *count < 999)
            (*count)++;
        set->_0c = *P<u32>(record + 0x18);
        return 1;
    }
    if (set->_0c == 0)
        set->_0c = guest::addr(Text(0xA3));
    return 0;
}

// ── 0x2342A0 / 0x234350: an item's picture, read when first wanted ────────
bool Icon_Load(ItemInfo* icon) {
    if (icon->sprites == 0) {
        icon->sprites = guest::addr(SpriteSet_Ctor(MemAlloc(0x14)));
        if (SpriteSet_LoadByName(P<SpriteSet>(icon->sprites), P<const char>(icon->iconFile), nullptr) == 0) {
            SpriteSet_Dtor(P<SpriteSet>(icon->sprites), 1);
            icon->sprites = 0;
            return false;
        }
    }
    return icon->sprites != 0 && SpriteSet_IsLoaded(P<SpriteSet>(icon->sprites)) != 0;
}
void Icon_Unload(ItemInfo* icon) {
    if (icon->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(icon->sprites), 1);
    icon->sprites = 0;
}

// ═══ The three-column list ═════════════════════════════════════════════════
struct Column {             // 0x6C bytes
    s32 x, y;               // +0x00  its tab
    s32 listX, listY;       // +0x08  its panel
    s16 count;              // +0x10  places
    s16 nameSprite;         // +0x12
    s16 tabSprite;          // +0x14
    s16 rowSprite;          // +0x16
    s16 cursorSprite;       // +0x18
    s16 _1a;
    u32 sprites;            // +0x1C  → the SpriteSet pointer
    u32 icons;              // +0x20
    s16 cursor;             // +0x24  which of the four rows shown
    s16 top;                // +0x26  the first one shown
    s16 level, levelTo;     // +0x28  0 in front … 2 at the back
    s16 marked;             // +0x2C  the place picked to move, −1 none
    s16 worn;               // +0x2E  (accessories) the one that is on
    s32 sliding;            // +0x30
    s16 timer;              // +0x34
    s16 _36;
    float path[8];          // +0x38  (a slide along a curve whose points are all zero)
    s32 upArrow, upTimer;   // +0x58  0 none, 1 shown, 2 lit, 4 lit then gone
    s32 downArrow, downTimer;   // +0x60
    u32 vtable;             // +0x68  0x2EC680
};
struct MenuList {           // 0x198 bytes
    Column col[3];          // carried, kept, accessories
    u32 sprites;            // +0x144
    u32 icons;              // +0x148
    u32 accessories;        // +0x14C  the accessory models' owner (World+0xFC), told when what is worn changes
    s32 active;             // +0x150
    PadRepeatPair upDown;   // +0x154
};
static_assert(sizeof(Column) == 0x6C && sizeof(MenuList) == 0x198, "MenuList");

inline const u8* ColumnItems(int which) {
    return Pd() + (which == 2 ? 0x6CEA : which == 1 ? 0x158 : 0x150);
}
inline s32 SlideFrames() { return (s32)(*gFrameRate * 7) / 0x1E; }

// ── 0x231470 ──────────────────────────────────────────────────────────────
void Column_Init(Column* c, s32 x, s32 y, s32 listX, s32 listY, s16 count, s16 name, s16 tab, s16 row, s16 cursor,
                 u32 sprites, u32 icons, s16 start) {
    memset(c, 0, sizeof *c);
    c->vtable = 0x2EC680u;
    c->marked = -1;
    c->worn = -1;
    c->x = x; c->y = y; c->listX = listX; c->listY = listY;
    c->count = count; c->nameSprite = name; c->tabSprite = tab; c->rowSprite = row; c->cursorSprite = cursor;
    c->sprites = sprites;
    c->icons = icons;
    c->top = start;
    if (count > 4)
        c->downArrow = 1;
    if (count - 4 < start) {
        c->cursor = (s16)(start - (count - 4));
        c->top = (s16)(count - 4);
        if (c->cursor > 3)
            c->cursor = 3;
        c->downArrow = 0;
    }
    if (start > 1)
        c->upArrow = 1;
}

// ── 0x2308F0: one column ──────────────────────────────────────────────────
void Column_Draw(Column* c, const u8* items) {
    const float t = (float)c->timer / (float)SlideFrames();
    float front = 1.0f - (float)c->level / 2.0f;
    if (c->sliding == 1)
        front += t * (float)(c->level - c->levelTo) / 2.0f;
    const u32 z = (u32)((s32)(front * 100.0f) + 0xFFFF82);
    const u32 dim = (u32)(s32)((1.0f - front) * 75.0f) & 0xFF;
    const u32 sx = Screen_X(0, -1), sy = Screen_Y(0, -1);
    const s32 lx = (u16)((s16)sx + (s16)c->listX), ly = (u16)((s16)sy + (s16)c->listY);
    const s32 bx = (s32)(((sx & 0xFFFF) + (u32)c->x) & 0xFFFF), by = (s32)(((sy & 0xFFFF) + (u32)c->y) & 0xFFFF);
    SpriteSet* sprites = P<SpriteSet>(*P<u32>(c->sprites));
    Font* font = *gFont;
    SpriteSet_Draw(sprites, 8, bx + 0x20, by + 0x20, z, 0x80808080u);
    SpriteSet_Draw(sprites, c->tabSprite, bx, by, z, 0x80808080u);
    SpriteSet_Draw(sprites, c->nameSprite, bx, by, z, (u32)(s32)(front * 128.0f) << 24 | 0x808080);
    s32 box[4] = {lx + 0x30, ly + 0x30, 0xC80, 0x760};
    Gs_FillRect(box, z, 0x52080404u);
    box[0] -= 0x30;
    box[1] -= 0x30;
    Gs_FillRect(box, z, 0x7FACB2BEu);
    box[0] += 0x20;
    box[1] += 0x20;
    box[2] -= 0x40;
    box[3] -= 0x40;
    Gs_FillRect(box, z, 0x7FACB2BEu);
    const s32 rx = (lx + 0x40) & 0xFFFF;
    s32 ry = ly + 0x40;
    for (s32 i = c->top; i < c->top + 4; i++, ry += 0x1C0) {
        ry &= 0xFFFF;
        Font_SetColor(font, 0x7F100E0Fu);
        SpriteSet_Draw(sprites, c->rowSprite, rx, ry, z, 0x80808080u);
        const bool under = c->cursor == i - c->top;
        if (under) {
            SpriteSet_Draw(sprites, c->cursorSprite, rx, ry, z, 0x80808080u);
            Font_SetColor(font, 0x7FBCBEC8u);
        }
        if (i == c->marked)
            SpriteSet_Draw(sprites, c->rowSprite, rx, ry, z, 0x404682C8u);
        if (i == c->worn && !under)
            Font_SetColor(font, 0x7F41244Eu);
        if (items[i] != 0) {
            if (const ItemInfo* icon = IconSet_Get(P<IconSet>(c->icons), items[i])) {
                GsDevice_End2D(*gGsDevice);
                Font_DrawString(font, P<const u16>(icon->name), rx + 0xA0, ry + 0x30, z, 0.9f, 0.0f);
                GsDevice_Begin2D(*gGsDevice);
            }
        }
    }
    const s32 shade[4] = {lx, ly, 0xC80, 0x760};
    Gs_FillRect(shade, z, dim << 24);
    if (c->levelTo != 0)
        return;
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } arrow;
    const auto draw = [&](s32 state, s32 y, u32 flags) {
        if (state == 0)
            return;
        for (int sprite : {7, 5, 6}) {
            if (sprite == 6 && state != 2 && state != 4)
                break;
            arrow.flags = flags;
            arrow.x = ((lx + 0xAF0) & 0xFFFF) + 0x20;
            arrow.y = y;
            arrow.z = z;
            arrow.w = arrow.h = -1;
            arrow.rgba = 0x80808080u;
            SpriteSet_DrawEx(sprites, sprite, &arrow);
        }
    };
    draw(c->upArrow, ((ly - 0xB0) & 0xFFFF) + 0x20, 1);
    draw(c->downArrow, ((ly + 0x700) & 0xFFFF) + 0x20, 0x40);
}

// ── 0x230610 ──────────────────────────────────────────────────────────────
MenuList* MenuList_Ctor(MenuList* self, u32 icons, u32 accessories) {
    memset(self, 0, sizeof *self);
    self->icons = icons;
    self->accessories = accessories;
    PadRepeatPair_Ctor(&self->upDown);
    self->upDown.vtable = 0x2EAA00u;
    PadRepeatPair_SetMasks(&self->upDown, kPadUp, kPadDown);
    self->sprites = static_cast<const u32*>(MenuRes_Get())[5];
    const u32 handle = guest::addr(&self->sprites);
    Column_Init(&self->col[0], 0x370, 0x480, 0x370, 0x5F0, 8, 9, 10, 0xF, 0x10, handle, icons, 0);
    Column_Init(&self->col[1], 0x6F0, 0x540, 0x6F0, 0x6A0, 4, 0xB, 0xC, 0x11, 0x12, handle, icons, 0);
    int worn = PlayerData_WornAccessorySlot(*gPlayerData);
    Column_Init(&self->col[2], 0xAB0, 0x610, 0xAB0, 0x770, 0x1C, 0xD, 0xE, 0x13, 0x14, handle, icons,
                (s16)(worn == -1 ? 0 : worn));
    for (int i = 0; i < 3; i++) {
        self->col[i].level = self->col[i].levelTo = (s16)i;
        self->col[i].marked = -1;
        self->col[i].sliding = 0;
        self->col[i].timer = 0;
    }
    self->col[2].worn = (s16)worn;
    self->active = 0;
    PadRepeatPair_SetTiming(&self->upDown, 10, 3, 7);
    return self;
}

inline s32 Place(const Column* c) { return c->top + c->cursor; }
// 0x22F970: what the cursor is on
u8 MenuList_Current(const MenuList* self) { return ColumnItems(self->active)[Place(&self->col[self->active])]; }
// 0x22F930
bool MenuList_OnWorn(const MenuList* self) { return PlayerData_WornAccessorySlot(*gPlayerData) == Place(&self->col[2]); }
// 0x230190: the thing under the cursor leaves the list
bool MenuList_Remove(MenuList* self) {
    if (self->active == 2)
        return false;
    return PlayerData_RemoveItem(*gPlayerData, self->active, (u32)Place(&self->col[self->active]) & 0xFF) != 0;
}
void MenuList_Mark(MenuList* self) { self->col[self->active].marked = (s16)Place(&self->col[self->active]); }  // 0x2303B0
void MenuList_Unmark(MenuList* self) { self->col[self->active].marked = -1; }                                   // 0x230250
// 0x230280: the marked place and the one under the cursor change over
bool MenuList_Swap(MenuList* self) {
    Column* c = &self->col[self->active];
    const s32 a = Place(c), b = c->marked;
    bool done = false;
    if (a != b) {
        if (self->active == 2) {
            PlayerData_SwapAccessories(*gPlayerData, a, b);
            done = true;
            self->col[2].worn = (s16)PlayerData_WornAccessorySlot(*gPlayerData);
        } else {
            done = PlayerData_SwapItems(*gPlayerData, self->active, (u32)a & 0xFF, (u32)b & 0xFF) != 0;
        }
    }
    if (done)
        c->marked = -1;
    return done;
}
// 0x2300C0 / 0x230110: nothing worn / the accessory under the cursor worn;
// the world's accessory object is told (0x2AB4C0; src/game/accessories.cpp).
void MenuList_Wear(MenuList* self, bool on) {
    u16* worn = reinterpret_cast<u16*>(Pd() + 0x11A);
    const s32 place = Place(&self->col[2]);
    *worn = on && place != -1 ? Pd()[0x6CEA + place] : (u16)0;
    Accessories_Refresh();
    self->col[2].worn = (s16)PlayerData_WornAccessorySlot(*gPlayerData);
}
// 0x230510
void MenuList_Reset(MenuList* self) {
    for (Column& c : self->col) {
        c.level = c.levelTo;
        c.marked = -1;
        c.sliding = 0;
        c.timer = 0;
    }
    self->col[2].worn = (s16)PlayerData_WornAccessorySlot(*gPlayerData);
}

// ── 0x2303F0: another column comes to the front ───────────────────────────
void MenuList_Turn(MenuList* self) {
    for (int i = 0; i < 3; i++) {
        Column* c = &self->col[i];
        if (self->active == i) {
            c->levelTo = 0;                 // 0x231130
        } else {                            // 0x2312D0
            c->levelTo = (s16)(c->level + 1);
            if (c->levelTo > 2)
                c->levelTo--;
        }
        c->sliding = 1;
        c->timer = 0;
    }
    Sound_PlaySE(0, 0, 0x3FFF);
}

// ── 0x22FA50: Up / Down; → the cursor moved ───────────────────────────────
int MenuList_MoveCursor(MenuList* self) {
    PadRepeat_Update(&self->upDown.a);
    PadRepeat_Update(&self->upDown.b);
    Column* c = &self->col[self->active];
    // A picture still being read: wait for it before moving on.
    const u8 item = MenuList_Current(self);
    if (item != 0)
        if (ItemInfo* icon = IconSet_Get(P<IconSet>(self->icons), item))
            if (!Icon_Load(icon))
                return 0;
    if (c->sliding == 1)
        return 0;
    if (!PadRepeat_IsHeld(&self->upDown.b) && self->upDown.a.fired != 0) {
        if (Place(c) > 0) {
            if (--c->cursor < 0) {
                c->cursor++;
                c->top--;
                c->upArrow = Place(c) < 1 ? 4 : 2;
                c->upTimer = 0;
                c->downArrow = 1;
            }
            Sound_PlaySE(2, 0, 0x3FFF);
        }
        return 1;
    }
    if (!PadRepeat_IsHeld(&self->upDown.a) && self->upDown.b.fired != 0) {
        if (Place(c) < c->count - 1) {
            if (++c->cursor > 3) {
                c->cursor--;
                c->top++;
                c->downArrow = Place(c) < c->count - 1 ? 2 : 4;
                c->downTimer = 0;
                c->upArrow = 1;
            }
            Sound_PlaySE(2, 0, 0x3FFF);
        }
        return 1;
    }
    return 0;
}

// ── 0x22FDD0: one frame; `listen`: takes the pad, `turn`: Left / Right too ─
int MenuList_Update(MenuList* self, bool listen, bool turn) {
    for (Column& c : self->col) {
        if (c.sliding == 1 && SlideFrames() < ++c.timer) {
            c.timer = 0;
            c.sliding = 0;
            c.level = c.levelTo;
        }
        const s32 lit = (s32)(*gFrameRate * 4) / 0x1E;
        if ((c.upArrow & 6) != 0 && lit < ++c.upTimer) {
            c.upTimer = 0;
            c.upArrow = c.upArrow == 4 ? 0 : c.upArrow == 2 ? 1 : c.upArrow;
        }
        if ((c.downArrow & 6) != 0 && lit < ++c.downTimer) {
            c.downTimer = 0;
            c.downArrow = c.downArrow == 4 ? 0 : c.downArrow == 2 ? 1 : c.downArrow;
        }
    }
    if (!listen)
        return 1;
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    if (turn && self->col[self->active].sliding != 1) {
        if (pressed & kPadRight) {
            if (++self->active > 2)
                self->active = 0;
            MenuList_Turn(self);
        } else if (pressed & kPadLeft) {
            if (--self->active < 0)
                self->active = 2;
            MenuList_Turn(self);
        }
    }
    if (pressed & (kPadRight | kPadLeft))
        return 1;
    return MenuList_MoveCursor(self);
}

// ── 0x22FA00 ──────────────────────────────────────────────────────────────
void MenuList_Draw(MenuList* self) {
    for (int i = 0; i < 3; i++)
        Column_Draw(&self->col[i], ColumnItems(i));
}

// ═══ The page ══════════════════════════════════════════════════════════════
enum : u32 { kBrowse = 0x1E6E00u, kPopup = 0x1E6750u, kMove = 0x1E6420u, kMessage = 0x1E6620u };

struct ItemsPage {
    u32 menu;           // +0x00
    s16 loaded;         // +0x04
    s16 _06;
    u32 vtable;         // +0x08
    s32 phase;          // +0x0C  2 the item table is loading, 5 ready
    u32 window;         // +0x10  the pop-up that is open
    u32 list;           // +0x14
    s32 popup;          // +0x18  0 carried thing, 1 accessory, 2 the accessory that is on
    u8  noWorld;        // +0x1C
    u8  _1d[3];
    u32 loadingIcon;    // +0x20
    u32 shownIcon;      // +0x24
    u32 swords;         // +0x28
    u32 params;         // +0x2C  the player's CharaParams
    u32 state[3];       // +0x30  a pointer to member: {0, −1, function}
    u32 windowMgr;      // +0x3C
};
static_assert(sizeof(ItemsPage) == 0x40, "ItemsPage");

inline u32 FrameOf(const ItemsPage* self) { return P<u32>(self->menu)[1]; }
inline IconSet* Icons(const ItemsPage* self) { return P<IconSet>(P<u32>(self->menu)[6]); }
inline MenuList* List(const ItemsPage* self) { return P<MenuList>(self->list); }
inline void SetAction(ItemsPage* self, s32 what, s32 arg = 0) {
    P<s32>(self->menu)[7] = what;
    if (what >= 3)
        P<s32>(self->menu)[8] = arg;
}
inline void SetState(ItemsPage* self, u32 fn) { self->state[0] = 0; self->state[1] = 0xFFFFFFFFu; self->state[2] = fn; }
inline void SetHelp(ItemsPage* self, const u16* text) {
    if (FrameOf(self) != 0)
        StatusFrame_SetHelp(P(FrameOf(self)), text);
}
void CloseWindow(ItemsPage* self) {
    WindowMgr_Close(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
    self->window = 0;
}
// The help line: what the thing under the cursor is.
void ShowHelp(ItemsPage* self) {
    const u8 item = MenuList_Current(List(self));
    if (item == 0)
        SetHelp(self, Text(1));
    else
        SetHelp(self, P<const u16>(IconSet_Get(Icons(self), item)->_10));
}
// Back to moving about the lists.
void Browse(ItemsPage* self) {
    CloseWindow(self);
    ShowHelp(self);
    if (FrameOf(self) != 0) {
        StatusFrame_ShowArrows(P(FrameOf(self)), 1);
        StatusFrame_SetHints(P(FrameOf(self)), 0);
    }
    SetState(self, kBrowse);
}
void ShowMessage(ItemsPage* self, const u16* text) {
    CloseWindow(self);
    SetHelp(self, Text(1));
    self->window = guest::addr(WindowMgr_OpenMessage(P<WindowMgr>(self->windowMgr), 0, 0, 0xFFFF82u, 2, 0, 0, text, 0));
    SetState(self, kMessage);
}

// ── 0x1E6060: the small menu for the thing under the cursor ───────────────
void OpenPopup(ItemsPage* self, int kind) {
    self->popup = kind;
    WindowDesc desc;
    memset(&desc, 0, sizeof desc);
    WindowDesc_Init(&desc);
    static const int kOptions[3][3] = {{0x99, 0x9A, 0x9B}, {0x9C, 0x9A, 0}, {0x9D, 0x9A, 0}};
    u32* options = static_cast<u32*>(MemAlloc(3 * sizeof(u32)));     // (a vector<u32> in the original)
    u32 count = 0;
    for (int id : kOptions[kind])
        if (id != 0)
            options[count++] = guest::addr(Text(id));
    desc.options.capacity = 3;
    desc.options.count = count;
    desc.options.data = guest::addr(options);
    desc.z = 0xFFFFE6u;
    desc.priority = 2;
    self->window = guest::addr(WindowMgr_Open(P<WindowMgr>(self->windowMgr), &desc));
    SetHelp(self, Text(0x8E));
    if (FrameOf(self) != 0) {
        StatusFrame_SetHints(P(FrameOf(self)), 3);
        StatusFrame_ShowArrows(P(FrameOf(self)), 0);
    }
    SetState(self, kPopup);
    if (desc.options.data != 0)
        MemFree(P(desc.options.data));
}

// ── 0x1E5EC0: "Use"; → the pop-up is done with ────────────────────────────
bool UseItem(ItemsPage* self) {
    const u8 item = MenuList_Current(List(self));
    const int kind = Item_UseKind(Icons(self), item);
    if (kind == 1) {
        if (Item_Use(Icons(self), item, P<u8>(self->params), 0) == 0) {
            const IconSet* icons = Icons(self);         // 0x233370: what it says
            ShowMessage(self, P<const u16>(icons->_0c));
            return false;
        }
        MenuList_Remove(List(self));
        if (FrameOf(self) != 0) {
            StatusFrame_TakeHealth(P(FrameOf(self)));
            StatusFrame_TakeVigour(P(FrameOf(self)));
        }
        if (const ItemInfo* icon = IconSet_Get(Icons(self), item)) {
            const ItemUse* use = reinterpret_cast<const ItemUse*>(icon);
            Sound_PlaySE(use->amount[0] < 1 && use->amount[1] < 1 ? 0x28 : 10, 0, 0x3FFF);
        }
    } else if (kind == 2) {
        SetAction(self, 7, item);           // on to the swords page, to choose which
    }
    return true;
}

// ── 0x1E6E00 ──────────────────────────────────────────────────────────────
void State_Browse(ItemsPage* self) {
    MenuList* list = List(self);
    if (MenuList_Update(list, self->loadingIcon == 0, true) != 0)
        ShowHelp(self);
    if (list->col[list->active].sliding == 1)
        return;
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    if (pressed & kPadTriangle) {
        SetAction(self, 2);
    } else if (pressed & kPadStart) {
        SetAction(self, 1);
    } else if ((pressed & kPadCross) && self->loadingIcon == 0) {
        if (MenuList_Current(list) == 0) {
            Sound_PlaySE(7, 0, 0x3FFF);
        } else if (list->active == 2) {
            Sound_PlaySE(1, 0, 0x3FFF);
            OpenPopup(self, MenuList_OnWorn(list) ? 2 : 1);
        } else if (list->active == 1) {
            Sound_PlaySE(7, 0, 0x3FFF);
        } else {
            Sound_PlaySE(1, 0, 0x3FFF);
            OpenPopup(self, 0);
        }
    }
}

// "Move": the place is marked and the next Cross swaps (0x1E6750's shared part).
void StartMove(ItemsPage* self) {
    MenuList_Mark(List(self));
    CloseWindow(self);
    SetHelp(self, Text(0x97));
    if (FrameOf(self) != 0) {
        StatusFrame_SetHints(P(FrameOf(self)), 3);
        StatusFrame_ShowArrows(P(FrameOf(self)), 0);
    }
    SetState(self, kMove);
}

// ── 0x1E6750 ──────────────────────────────────────────────────────────────
void State_Popup(ItemsPage* self) {
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    if (pressed & kPadTriangle) {
        Browse(self);
        Sound_PlaySE(7, 0, 0x3FFF);
        return;
    }
    if ((pressed & kPadCross) == 0)
        return;
    Sound_PlaySE(5, 0, 0x3FFF);
    const int choice = WindowMgr_GetCursor(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
    MenuList* list = List(self);
    if (self->popup == 2 || self->popup == 1) {
        if (choice == 1) {
            StartMove(self);
        } else if (choice == 0) {
            MenuList_Wear(list, self->popup == 1);
            Browse(self);
        }
        return;
    }
    if (choice == 2) {                      // Drop
        if (self->noWorld == 0) {
            const u8 item = MenuList_Current(list);
            if (MenuList_Remove(list)) {
                Browse(self);
                if (const ItemInfo* icon = IconSet_Get(Icons(self), item))
                    SetAction(self, 5, reinterpret_cast<const ItemUse*>(icon)->object);
            }
        } else {
            ShowMessage(self, Text(0x98));
        }
    } else if (choice == 1) {
        StartMove(self);
    } else if (choice == 0 && UseItem(self)) {
        Browse(self);
        if (FrameOf(self) != 0)
            StatusFrame_ShowArrows(P(FrameOf(self)), 0);
    }
}

// ── 0x1E6420 ──────────────────────────────────────────────────────────────
void State_Move(ItemsPage* self) {
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    if (pressed & kPadTriangle) {
        MenuList_Unmark(List(self));
        Browse(self);
        Sound_PlaySE(7, 0, 0x3FFF);
    }
    MenuList_Update(List(self), self->loadingIcon == 0, false);
    if (pressed & kPadCross) {
        if (!MenuList_Swap(List(self))) {
            Sound_PlaySE(7, 0, 0x3FFF);
        } else {
            Sound_PlaySE(5, 0, 0x3FFF);
            Browse(self);
        }
    }
}

// ── 0x1E6620 ──────────────────────────────────────────────────────────────
void State_Message(ItemsPage* self) {
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    if (pressed & (kPadTriangle | kPadCross)) {
        Browse(self);
        Sound_PlaySE(7, 0, 0x3FFF);
    } else if (pressed & kPadStart) {
        SetAction(self, 1);
    }
}

// ── 0x1E7190 (vtable +0x18) ───────────────────────────────────────────────
void ItemsPage_Update(ItemsPage* self) {
    if (self->phase != 5) {
        // 0x1E7020: the item table, then the chart of each carried sword's
        // techniques (phases 3 and 4, one sword at a time: here at once).
        if (self->phase == 2 && IconSet_IsLoading(Icons(self)) == 0) {
            void* sheets = P(*P<u32>(0x2EE698u));
            for (int slot = 0; slot < 3; slot++) {
                if (*reinterpret_cast<const s16*>(Pd() + 0x17A + slot * 2) == -1)
                    continue;
                const int asked = SkillSheets_Request(sheets, *reinterpret_cast<const s32*>(Pd() + 0x180 + slot * 0x100 + 0x70), slot);
                if (asked == 3 || asked == -1)
                    continue;
                SkillSheets_Poll(sheets, slot);
            }
            self->phase = 5;
            self->loaded = 1;
        }
        return;
    }
    if (P<s32>(self->menu)[9] != 0) {           // pages are turning: only animate
        MenuList_Update(List(self), false, true);
        return;
    }
    switch (self->state[2]) {
    case kBrowse:  State_Browse(self);  break;
    case kPopup:   State_Popup(self);   break;
    case kMove:    State_Move(self);    break;
    case kMessage: State_Message(self); break;
    }
}

// ── 0x1E5C40 (vtable +0x14) ───────────────────────────────────────────────
void ItemsPage_Draw(ItemsPage* self) {
    if (self->phase != 5)
        return;
    GsDevice* dev = *gGsDevice;
    GsDevice_Begin2D(dev);
    SpriteSet* sprites = P<SpriteSet>(static_cast<const u32*>(MenuRes_Get())[5]);
    SpriteSet_Bind(sprites);
    MenuList* list = List(self);
    MenuList_Draw(list);
    // The picture's panel, tinted like the list in front.
    const s32 sx = (s32)(Screen_X(0, -1) & 0xFFFF), sy = (s32)(Screen_Y(0, -1) & 0xFFFF);
    s32 box[4] = {sx + 0x1830, sy + 0x4E0, 0xCC0, 0x9A0};
    Gs_FillRect(box, 0xFFFF82u, 0x52080404u);
    box[0] -= 0x30;
    box[1] -= 0x30;
    Gs_FillRect(box, 0xFFFF82u, 0x7FACB2BEu);
    struct Draw { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; };
    Draw panel{1, box[0] + 0x20, box[1] + 0x20, 0xFFFF82u, box[2] - 0x40, box[3] - 0x40,
               P<u32>(0x2D9480u)[list->active]};
    SpriteSet_DrawEx(sprites, 0x15, &panel);
    Draw picture{1, sx + 0x1A60, sy + 0x580, 0xFFFF82u, 0x830, 0x830, 0x80808080u};
    GsDevice_End2D(dev);
    const u8 item = MenuList_Current(list);
    if (item != 0) {
        if (ItemInfo* icon = IconSet_Get(Icons(self), item)) {
            if (!Icon_Load(icon)) {
                if (self->shownIcon != 0) {
                    Icon_Unload(P<ItemInfo>(self->shownIcon));
                    self->shownIcon = 0;
                }
                self->loadingIcon = guest::addr(icon);
            } else {
                self->loadingIcon = 0;
                self->shownIcon = guest::addr(icon);
                Icon_Draw(icon, &picture);
            }
        }
    }
    GsDevice_End2D(dev);
}

// ── 0x1E72B0 (vtable +0x10) ───────────────────────────────────────────────
void ItemsPage_Enter(ItemsPage* self) {
    if (self->list != 0)
        MenuList_Reset(List(self));
    Browse(self);
}
// 0x1E5C30 (vtable +0x1C): the gauges have settled
void ItemsPage_Refresh(ItemsPage* self) {
    if (FrameOf(self) != 0)
        StatusFrame_ShowArrows(P(FrameOf(self)), 1);
}
char ItemsPage_Load(ItemsPage*) { return 1; }           // 0x193E10 (bound by the status page)

// ── 0x1E7380 (vtable +0x08) ───────────────────────────────────────────────
void* ItemsPage_Dtor(ItemsPage* self, int flag) {
    if (self == nullptr)
        return self;
    self->vtable = 0x2EBB70u;
    if (self->list != 0)
        MemFree(P(self->list));
    self->list = 0;
    CloseWindow(self);
    self->vtable = 0x2EB2D0u;
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

GUEST_BIND(0x1E7380, ItemsPage_Dtor);
GUEST_BIND(0x1E72B0, ItemsPage_Enter);
GUEST_BIND(0x1E5C40, ItemsPage_Draw);
GUEST_BIND(0x1E7190, ItemsPage_Update);
GUEST_BIND(0x1E5C30, ItemsPage_Refresh);

}  // namespace

extern "C" {

// For the Records screen (0x2342A0 / 0x234350).
int  ItemIcon_Load(void* icon) { return Icon_Load(static_cast<ItemInfo*>(icon)); }
void ItemIcon_Unload(void* icon) { Icon_Unload(static_cast<ItemInfo*>(icon)); }

// ── 0x1E7440 ──────────────────────────────────────────────────────────────
void* ItemsPage_Ctor(void* mem, StatusMenu* menu, void* infoBlock, u8 noWorld) {
    ItemsPage* self = static_cast<ItemsPage*>(mem);
    const u32* info = static_cast<const u32*>(infoBlock);
    memset(self, 0, sizeof *self);
    self->menu = guest::addr(menu);
    self->vtable = 0x2EBB70u;
    self->windowMgr = guest::addr(*gWindowMgr);
    self->noWorld = noWorld;
    self->phase = 2;
    IconSet_Load(Icons(self));
    self->swords = info[0];
    self->params = info[1];
    self->list = guest::addr(MenuList_Ctor(static_cast<MenuList*>(MemAlloc(sizeof(MenuList))),
                                           P<u32>(self->menu)[6], info[7]));
    return self;
}

// For the swords page: an item used on a sword (0x233A30), and the item
// under this page's cursor used up (0x211F10).
int StatusItem_Use(void* icons, u32 item, u8* params, int swordSlot) {
    return Item_Use(static_cast<IconSet*>(icons), item, params, swordSlot);
}
void ItemsPage_Consume(void* page) {
    ItemsPage* self = static_cast<ItemsPage*>(page);
    if (self->list != 0)
        MenuList_Remove(List(self));
}

// 0x1E7230: the menu came here from another page
void ItemsPage_Reset(void* page) {
    ItemsPage* self = static_cast<ItemsPage*>(page);
    ShowHelp(self);
    if (FrameOf(self) != 0)
        StatusFrame_SetHints(P(FrameOf(self)), 0);
}

}  // extern "C"
