// The shop menu: what a shop's script opens with UI.Shop(kind, buyer, seller).
//
// Decompiled from 0x229C80–0x22C450 (the HUD overlay, vtable 0x2EC610) and
// 0x23F2A0–0x23F610 (the shop data).  See "UI.Shop" in the project notes.
//
// Goods come from the shop's record by how strong the townspeople are; the
// player picks one, takes it, and is then asked to pay — and may refuse,
// which the shop's script hears about.
//
// The shop data (asset 0x8C) is read at once here rather than through the
// load queue, and its records live on the host heap.

#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/pad_repeat.h"
#include "game/chara_resources.h"
#include "game/hud.h"
#include "game/item_select.h"
#include "game/player_data.h"
#include "game/speech.h"
#include "native/archive.h"
#include "native/guest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void  Sound_PlaySE(int id, int positional, int volume);     // 0x180E70
    void  Voice_Play(int id, int positional, int volume, int a);    // 0x1800F0
    SpriteSet* SpriteSet_Ctor(void* mem);                       // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    char  SpriteSet_LoadByName(SpriteSet* set, const char* name, const char* name2);   // 0x17C2F0
    int   SpriteSet_Bind(SpriteSet* set);                       // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);             // 0x17C570
    void  SpriteSet_DrawEx(SpriteSet* set, int index, const void* params);     // 0x17ADF0
}

namespace {

template <typename T = void> inline T* P(u32 a) { return guest::ptr<T>(a); }
const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
const GuestPtrVar<Font>       gFont{0x2EE67Cu};
const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};

// ── The shop data (0x23F2A0): ui/GameUI/ShopList.dat ──────────────────────
struct ShopRecord {
    s16 goods[4][8][2];             // +0x00  by tier: {item, price}
    s16 voices[11];                 // +0x80
    u32 lines[11];                  // +0x98  const u16* (guest)
    u32 sheet;                      // +0xC4  const char* (guest): its signboard sprites
};
struct ShopData {
    bool loaded = false;
    u32 common[5] = {};             // the two yes / no menus, "Money", "You got the %s", "③ Quit Shopping"
    std::vector<ShopRecord> shops;
};
ShopData sData;

bool ShopData_Load() {
    if (sData.loaded)
        return true;
    std::vector<u8> file;
    if (!archive::read_asset(0x8C, file) || file.size() < 8)
        return false;
    // The strings are used where they lie, so the file has to be in guest memory.
    u8* d = static_cast<u8*>(guest::alloc((u32)file.size()));
    memcpy(d, file.data(), file.size());
    s32 version, count;
    memcpy(&version, d, 4);
    memcpy(&count, d + 4, 4);
    if (version != 3)
        return false;
    size_t p = 8;
    const auto text = [&]() {
        const u32 at = guest::addr(d + p);
        while (p + 1 < file.size() && (d[p] | d[p + 1]) != 0)
            p += 2;
        p += 2;
        return at;
    };
    for (u32& c : sData.common)
        c = text();
    for (s32 k = 0; k < count && p + 0x80 <= file.size(); k++) {
        ShopRecord r{};
        memcpy(r.goods, d + p, 0x80);
        p += 0x80;
        for (int i = 0; i < 11; i++) {
            memcpy(&r.voices[i], d + p, 2);
            p += 2;
            r.lines[i] = text();
        }
        r.sheet = guest::addr(d + p);
        size_t n = strlen(reinterpret_cast<const char*>(d + p)) + 1;
        n += n & 1;
        p += n;
        sData.shops.push_back(r);
    }
    sData.loaded = true;
    return true;
}
inline const ShopRecord* ShopOf(u32 kind) {                 // 0x23F4D0
    return kind < sData.shops.size() ? &sData.shops[kind] : nullptr;
}

// ── The overlay (0x88 bytes) ──────────────────────────────────────────────
struct ShopMenu {
    s32 kind;               // +0x00  (HUD widget)
    u32 vtable;             // +0x04
    u32 data;               // +0x08  (the shop data object; host tables here)
    u32 sprites;            // +0x0C  SpriteSet*: asset 0x69, the panels
    u32 sign;               // +0x10  SpriteSet*: the shop's own sheet
    u32 icons;              // +0x14  IconSet*
    s32 cursor;             // +0x18
    s16 count;              // +0x1C
    s16 goods[8][2];        // +0x1E  {item, price}
    u16 _3e;
    u32 arrows;             // +0x40  1 more above, 2 more below, 4 / 8 the arrow just used
    s32 arrowTimer;         // +0x44
    u32 first;              // +0x48  first row shown
    s32 state;              // +0x4C
    s32 timer;              // +0x50
    u32 message;            // +0x54  ChoiceWindow*: the yes / no box
    s32 answer;             // +0x58  1 yes, 2 no
    u32 balloon;            // +0x5C  PlacedBalloon*: what the keeper says
    u32 shop;               // +0x60  which record
    u32 buyer;              // +0x64  CharaParams*
    u32 seller;             // +0x68
    s32 strength;           // +0x6C  the townspeople's
    s32 tier;               // +0x70
    s32 slideSign;          // +0x74  frames left of sliding in (out: counted up)
    s32 slideList;          // +0x78
    s32 slideText;          // +0x7C
    u32 hud;                // +0x80
    u32 repeat;             // +0x84  PadRepeatPair*
};
static_assert(sizeof(ShopMenu) == 0x88, "ShopMenu");
enum {
    kOpening = 1, kList = 2, kAskTake = 3, kTaken = 4, kBagFull = 5, kAskPay = 6, kPaid = 7,
    kLeft = 8, kCannotPay = 9, kWillNotPay = 10,
};

struct Rect { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; };
// 0x21E040 — a flat rectangle.
void FillRect(const Rect* r) {
    struct { u64 prim; u32 rgba0, q0; u16 x0, y0; u32 z0; u32 rgba1, q1; u16 x1, y1; u32 z1; } packet{};
    packet.prim = 0x14E;
    packet.x0 = (u16)r->x;
    packet.y0 = (u16)r->y;
    packet.z0 = packet.z1 = r->z;
    packet.rgba0 = packet.rgba1 = r->rgba;
    packet.x1 = (u16)(r->x + r->w);
    packet.y1 = (u16)(r->y + r->h);
    GsDevice_DrawPrim(*gGsDevice, &packet, 2);
}
// A piece of a panel, from the tables in the game's data: {sprite (< 0: a
// flat rectangle), x, y, w, h, colour}.
void DrawPart(ShopMenu* self, const s32* part, s32 x, s32 y) {
    Rect r{part[3] > 0 ? 4u : 1u, x + part[1] * 16, y + part[2] * 16, 0xFFF000u, part[3] << 4, part[4] << 4,
           (u32)part[5]};
    if (part[0] < 0)
        FillRect(&r);
    else
        SpriteSet_DrawEx(P<SpriteSet>(self->sprites), part[0], &r);
}
inline void DrawParts(ShopMenu* self, u32 table, int from, int to, s32 x, s32 y) {
    for (int i = from; i < to; i++)
        DrawPart(self, P<s32>(table) + (i - from) * 6, x, y);
}
inline const u16* Mon() { return TextTable_Find(TextTable_Get(), 5); }
inline ItemInfo* ItemOf(ShopMenu* self, int row) {
    return IconSet_Get(P<IconSet>(self->icons), (u32)(u16)self->goods[row][0]);
}

// ── 0x22C160: what is on sale ─────────────────────────────────────────────
void BuildGoods(ShopMenu* self) {
    memset(self->goods, 0, sizeof self->goods);
    const ShopRecord* shop = ShopOf(self->shop);
    s16 count = 0;
    for (int i = 0; i < 8 && shop != nullptr; i++) {
        const s16 item = shop->goods[self->tier][i][0];
        self->goods[count][0] = 0;
        self->goods[count][1] = 0;
        if (PlayerData_HasAccessory(*gPlayerData, (u32)(u16)item) != 0)
            continue;                                       // one of each accessory is enough
        self->goods[count][0] = item;
        self->goods[count][1] = shop->goods[self->tier][i][1];
        if (item != 0)
            count++;
    }
    self->count = count;
}

// ── 0x229E00: the keeper's line for the state just entered ────────────────
void Say(ShopMenu* self) {
    if (self->balloon == 0) {
        void* mem = MemAlloc(sizeof(PlacedBalloon));
        self->balloon = mem != nullptr ? guest::addr(SpeechPlaced_Ctor(mem)) : 0;
    }
    const ShopRecord* shop = ShopOf(self->shop);
    if (shop == nullptr || self->balloon == 0)
        return;
    // state → which of the record's eleven lines
    static const s8 kLine[11] = {-1, 0, 1, 2, 3, 9, 4, 10, 8, 5, 6};
    const int line = self->state >= 1 && self->state <= 10 ? kLine[self->state] : -1;
    if (line < 0)
        return;
    PlacedBalloon* balloon = P<PlacedBalloon>(self->balloon);
    SpeechParams params{};
    params.speaker = self->seller;
    params.listener = self->buyer;
    params.frames = -1;
    params.direction = -1;
    params.text = shop->lines[line];
    VirtualCall<int, void*, const SpeechParams*>(balloon->b.w.vtable, 0x0C, balloon, &params);
    if (self->state == kAskTake || self->state == kAskPay) {
        SpeechParams menu{};
        menu.frames = -1;
        menu.direction = -1;
        menu.text = sData.common[self->state == kAskTake ? 0 : 1];
        ChoiceWindow* box = P<ChoiceWindow>(self->message);
        VirtualCall<int, void*, const SpeechParams*>(box->w.vtable, 0x0C, box, &menu);
    }
    if (shop->voices[line] != 0)
        Voice_Play(shop->voices[line], 0, 0x3FFF, 0);
}
inline void Enter(ShopMenu* self, int state) {
    self->state = state;
    Say(self);
}

// ── 0x229C80: the sounds of the buttons ───────────────────────────────────
void ButtonSounds(ShopMenu* self, u32 pad) {
    bool confirm = false, cancel = false, yesNo = false;
    switch (self->state) {
    case kList:
        confirm = cancel = true;
        if (!((pad & kPadUp) && self->cursor == 0) && !((pad & kPadDown) && self->cursor == 7) &&
            (pad & (kPadUp | kPadDown)))
            Sound_PlaySE(3, 0, 0x3FFF);
        break;
    case kAskTake: yesNo = confirm = cancel = true; break;
    case kAskPay:  yesNo = confirm = true; break;
    }
    if ((pad & kPadCross) && confirm)
        Sound_PlaySE(5, 0, 0x3FFF);
    if ((pad & kPadTriangle) && cancel)
        Sound_PlaySE(7, 0, 0x3FFF);
    if (yesNo && !((pad & kPadUp) && self->answer == 1) && !((pad & kPadDown) && self->answer == 2) &&
        (pad & (kPadUp | kPadDown)))
        Sound_PlaySE(3, 0, 0x3FFF);
}

// Money off the purse, with the running totals (as 0x1FA210 does).
void Pay(s32 price) {
    u8* pd = reinterpret_cast<u8*>(*gPlayerData);
    s16* money = reinterpret_cast<s16*>(pd + 0x106);
    s32 now = (u16)*money - price;
    if (now > 9999)
        now = 9999;
    const s32 change = now - *money;
    *money = (s16)now;
    u16* total = reinterpret_cast<u16*>(pd + (change < 1 ? 0x84E : 0x84C));
    const s32 sum = *total + (change < 0 ? -change : change);
    *total = (u16)(sum > 0xFFFF ? 0xFFFF : sum);
}

// ── 0x22B850 (vtable +0x10) ───────────────────────────────────────────────
int Shop_Update(ShopMenu* self) {
    if (!ShopData_Load() || IconSet_IsLoading(P<IconSet>(self->icons)) != 0)
        return 1;
    if (self->sign == 0) {
        if (ShopOf(self->shop) == nullptr)
            self->shop = 0;
        void* mem = MemAlloc(0x14);
        self->sign = mem != nullptr ? guest::addr(SpriteSet_Ctor(mem)) : 0;
        SpriteSet_LoadByName(P<SpriteSet>(self->sign), P<const char>(ShopOf(self->shop)->sheet), nullptr);
    }
    if (self->balloon != 0) {
        PlacedBalloon* balloon = P<PlacedBalloon>(self->balloon);
        VirtualCall<int, void*>(balloon->b.w.vtable, 0x10, balloon);
    }
    if (self->state == 0) {
        BuildGoods(self);
        Enter(self, kOpening);
        return 1;
    }
    if (self->state >= kLeft) {                             // over: the panels slide away
        self->slideSign++;
        self->slideList++;
        self->slideText++;
        if (self->slideText == 0x1E) {
            P<Hud>(self->hud)->_4c = (u32)self->state;      // what the script asks about
            return 0;
        }
        return 1;
    }

    u32 pad = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    PadRepeatPair* repeat = P<PadRepeatPair>(self->repeat);
    PadRepeat_Update(&repeat->a);
    PadRepeat_Update(&repeat->b);
    if (!PadRepeat_IsHeld(&repeat->b) && repeat->a.fired != 0)
        pad |= kPadUp;
    if (!PadRepeat_IsHeld(&repeat->a) && repeat->b.fired != 0)
        pad |= kPadDown;
    if (self->slideSign != 0)
        self->slideSign--;
    if (self->slideList != 0)
        self->slideList--;
    if (self->slideText == 0) {
        if (self->state == kOpening)
            Enter(self, kList);
    } else {
        self->slideText--;
    }
    if (self->state == kOpening)
        return 1;

    ButtonSounds(self, pad);
    const bool pressedOn = (pad & (kPadCross | kPadTriangle)) != 0;
    switch (self->state) {
    case kList:
        if (self->arrowTimer == 0)
            self->arrows &= ~0xCu;
        else
            self->arrowTimer--;
        if (pad & kPadTriangle) {
            Enter(self, kLeft);
            return 1;
        }
        if (pad & kPadCross) {
            if (self->goods[self->cursor][0] != 0 && self->cursor >= 0) {
                self->answer = 1;
                Enter(self, kAskTake);
            }
            break;
        }
        if (pad & kPadUp) {
            self->arrows |= 4;
            self->arrowTimer = 3;
            self->cursor--;
        }
        if (pad & kPadDown) {
            self->arrows |= 8;
            self->arrowTimer = 3;
            self->cursor++;
        }
        if (self->cursor < 1)
            self->cursor = 0;
        if ((u32)self->cursor < self->first)
            self->first = (u32)self->cursor;
        if (self->first == 0) self->arrows &= ~1u; else self->arrows |= 1;
        if (self->cursor > 6)
            self->cursor = 7;
        if (self->first + 3 < (u32)self->cursor)
            self->first++;
        if (self->first < 4) self->arrows |= 2; else self->arrows &= ~2u;
        break;

    case kAskTake:
        if (pad & kPadUp) self->answer = 1;
        if (pad & kPadDown) self->answer = 2;
        if (pad & kPadCross) {
            if (self->answer == 1) {
                const u16 item = (u16)self->goods[self->cursor][0];
                const ItemInfo* info = IconSet_Get(P<IconSet>(self->icons), item);
                bool got = false;
                if (info != nullptr && *reinterpret_cast<const char*>(info) == 4) {
                    PlayerData_AddAccessory(*gPlayerData, item);
                    got = true;
                } else if (PlayerData_HasFreeItemSlot(*gPlayerData, 0) != 0) {
                    PlayerData_AddItemSorted(*gPlayerData, 0, (u8)item);
                    got = true;
                }
                if (got) {
                    self->timer = 0x50;
                    Enter(self, kTaken);
                    if (self->hud != 0 && info != nullptr) {
                        // "You got the %s" (0x16DF10)
                        static u32 buffer = 0;
                        if (buffer == 0)
                            buffer = guest::addr(guest::alloc(0x200));
                        u16* out = P<u16>(buffer);
                        size_t n = 0;
                        for (const u16* f = P<const u16>(sData.common[3]); *f != 0 && n < 0xFE; f++) {
                            if (f[0] == '%' && f[1] == 's') {
                                for (const u16* s = P<const u16>(info->name); *s != 0 && n < 0xFE; s++)
                                    out[n++] = *s;
                                f++;
                            } else {
                                out[n++] = *f;
                            }
                        }
                        out[n] = 0;
                        Hud_ShowDate(P(self->hud), out, 0x50, 0);
                    }
                } else {
                    self->timer = 0x50;
                    Enter(self, kBagFull);
                }
            } else {
                Enter(self, kList);
            }
        }
        if (pad & kPadTriangle)
            Enter(self, kList);
        ShopMessage_SetSelection(P<ChoiceWindow>(self->message), self->answer);
        break;

    case kBagFull:
        if (pressedOn) {
            self->timer = 0;
            Enter(self, kList);
        }
        break;
    case kTaken:
        if (pressedOn) {
            self->timer = 0;
            Enter(self, kAskPay);
        }
        break;

    case kAskPay:
        if (pad & kPadUp) self->answer = 1;
        if (pad & kPadDown) self->answer = 2;
        if (pad & kPadCross) {
            if (self->answer != 1) {
                Enter(self, kWillNotPay);
            } else {
                const s16 price = self->goods[self->cursor][1];
                const u16 money = *reinterpret_cast<const u16*>(reinterpret_cast<const u8*>(*gPlayerData) + 0x106);
                if ((s32)money < (s32)price) {
                    Enter(self, kCannotPay);
                    return 1;
                }
                Pay(price);
                self->timer = 0x50;
                Enter(self, kPaid);
                return 1;
            }
        }
        ShopMessage_SetSelection(P<ChoiceWindow>(self->message), self->answer);
        break;

    case kPaid:
        if (pressedOn) {
            self->timer = 0;
            Enter(self, kList);
            BuildGoods(self);
        }
        break;
    }
    return 1;
}

// ── Drawing (0x22A2D0 and its parts) ──────────────────────────────────────
void DrawPrice(Font* font, s16 price, u16* out) {
    WStr_Format(out, 0x100, "%d", (int)price);
    WStr_Append(out, Mon(), 0x100);
}
// 0x22AFE0 — the list: four rows, the chosen one lit, arrows when there is more.
void DrawList(ShopMenu* self, s32 x, s32 y) {
    Font* font = *gFont;
    DrawParts(self, 0x2D9B60u, 0, 2, x, y);
    int rows = self->count - (s32)self->first;
    const int chosen = self->cursor - (s32)self->first;
    if (rows > 4)
        rows = 4;
    for (int i = 0; i < rows; i++) {
        s32 row[12];
        memcpy(row, P<s32>(0x2D9EA0u) + i * 12, sizeof row);
        row[0] = i == chosen ? 0x0F : 0x0D;
        row[6] = i == chosen ? 0x10 : 0x0E;
        DrawPart(self, row, x, y);
        DrawPart(self, row + 6, x, y);
        ItemInfo* info = ItemOf(self, (int)self->first + i);
        if (info == nullptr)
            continue;
        const s32* text = P<s32>(0x2D9F60u) + i * 12;
        Font_SetColor(font, i == chosen ? 0x80BEC8D2u : 0x80161B22u);
        Font_DrawString(font, P<const u16>(info->name), x + text[1] * 16, y + text[2] * 16, 0xFFF000u, 0.82f, 0.0f);
        u16 price[0x100];
        DrawPrice(font, self->goods[self->first + (u32)i][1], price);
        Font_SetColor(font, i == chosen ? 0x803131D4u : 0x80161B22u);
        static u32 buffer = 0;
        if (buffer == 0)
            buffer = guest::addr(guest::alloc(0x200));
        memcpy(P(buffer), price, sizeof price);
        TextBox box{};
        box.text = buffer;
        box.scale = 0.82f;
        box.x = (u16)(x + text[7] * 16);
        box.y = (u16)(y + text[8] * 16);
        box.z = 0xFFF000u;
        box.w = (u16)(text[9] << 4);
        box.h = (u16)(text[10] << 4);
        box.flags = 0x10;                                   // right-aligned
        Font_DrawBox(font, &box);
        const s32* icon = P<s32>(0x2D9C30u) + i * 6;
        Rect r{1, x + icon[1] * 16, y + icon[2] * 16, 0xFFF000u, icon[3] << 4, icon[4] << 4, 0x80808080u};
        Icon_Draw(info, &r);
        SpriteSet_Bind(P<SpriteSet>(self->sprites));
    }
    if (self->arrows & 1)
        DrawParts(self, (self->arrows & 4) ? 0x2D9C90u : 0x2D9CC0u, 0, 2, x, y);
    if (self->arrows & 2)
        DrawParts(self, (self->arrows & 8) ? 0x2D9CF0u : 0x2D9D20u, 0, 2, x, y);
}
// 0x22AD90 — what the chosen thing is.
void DrawDescription(ShopMenu* self, s32 x, s32 y) {
    Font* font = *gFont;
    DrawParts(self, 0x2D9BD0u, 0, 3, x, y);
    if (self->goods[self->cursor][0] == 0)
        return;
    const ItemInfo* info = ItemOf(self, self->cursor);
    if (info == nullptr)
        return;
    const s32* at = P<s32>(0x2D9C18u);
    Font_SetColor(font, (u32)at[5]);
    u16& lineHeight = *P<u16>(0x2EE53Au);                   // (0x154F60 / 0x154F70: a little more room between lines)
    const u16 usual = lineHeight;
    lineHeight = (u16)(usual + 0x20);
    TextBox box{};
    box.text = info->description;
    box.scale = 0.82f;
    box.x = (u16)(x + at[1] * 16);
    box.y = (u16)(y + at[2] * 16);
    box.z = 0xFFF000u;
    box.w = (u16)(at[3] << 4);
    box.h = (u16)(at[4] << 4);
    box.flags = 3;
    Font_DrawBox(font, &box);
    lineHeight = usual;
}
// 0x22AB40 — the purse.
void DrawMoney(ShopMenu* self, s32 x, s32 y) {
    Font* font = *gFont;
    DrawParts(self, 0x2D9E40u, 0, 2, x, y);
    const s32* label = P<s32>(0x2D9E70u);
    Font_SetColor(font, (u32)label[5]);
    Font_DrawCentered(font, P<const u16>(sData.common[2]), x + label[1] * 16, y + label[2] * 16, 0xFFF000u, 0.82f, 0.0f);
    u16 amount[0x100];
    DrawPrice(font, *reinterpret_cast<const s16*>(reinterpret_cast<const u8*>(*gPlayerData) + 0x106), amount);
    const s32* at = P<s32>(0x2D9E88u);
    Font_DrawCentered(font, amount, x + at[1] * 16, y + at[2] * 16, 0xFFF000u, 0.82f, 0.0f);
}
// 0x22A5C0 — the thing being bought, on its own: 0 with the "take it?" frame,
// 1 with the "pay?" frame, 2 bare.
void DrawChosen(ShopMenu* self, s32 x, s32 y, int mode) {
    Font* font = *gFont;
    DrawParts(self, 0x2D9D50u, 0, 3, x, y);
    if (ItemInfo* info = ItemOf(self, self->cursor)) {
        const s32* name = P<s32>(0x2D9DE0u);
        Font_SetColor(font, (u32)name[5]);
        Font_DrawString(font, P<const u16>(info->name), x + name[1] * 16, y + name[2] * 16, 0xFFF000u, 0.82f, 0.0f);
        const s32* cost = P<s32>(0x2D9DF8u);
        Font_SetColor(font, (u32)cost[5]);
        u16 price[0x100];
        DrawPrice(font, self->goods[self->cursor][1], price);
        Font_DrawString(font, price, x + cost[1] * 16, y + cost[2] * 16, 0xFFF000u, 0.82f, 0.0f);
        const s32* icon = P<s32>(0x2D9DC8u);
        Rect r{1, x + icon[1] * 16, y + icon[2] * 16, 0xFFF000u, icon[3] << 4, icon[4] << 4, 0x80808080u};
        Icon_Draw(info, &r);
        SpriteSet_Bind(P<SpriteSet>(self->sprites));
    }
    if (mode == 0) {
        DrawParts(self, 0x2D9D98u, 3, 5, x, y);
        DrawParts(self, 0x2D9E10u, 8, 10, x, y);
    } else if (mode == 1) {
        DrawParts(self, 0x2D9D98u, 3, 5, x, y);
    }
}

// 0x22A2D0 (vtable +0x14)
int Shop_Draw(ShopMenu* self, SpriteSet*) {
    if (self->state == 0)
        return 1;
    if (self->balloon != 0) {
        PlacedBalloon* balloon = P<PlacedBalloon>(self->balloon);
        VirtualCall<int, void*>(balloon->b.w.vtable, 0x14, balloon);
    }
    Font* font = *gFont;
    const s32 x = (s32)(Screen_X(0, -1) & 0xFFFF), y = (s32)(Screen_Y(0, -1) & 0xFFFF);
    if (SpriteSet_Bind(P<SpriteSet>(self->sign)) == 0)
        return 1;
    const s32* sign = P<s32>(0x2D9BB0u);
    Rect r{1, x + self->slideSign * -0x1E0 + sign[1] * 16, y + sign[2] * 16, 0xFFF000u, sign[3] << 4, sign[4] << 4,
           (u32)sign[5]};
    SpriteSet_DrawEx(P<SpriteSet>(self->sign), 0, &r);
    if (SpriteSet_Bind(P<SpriteSet>(self->sprites)) == 0)
        return 1;
    Font_SetColor(font, 0x80FFFFFFu);
    if (self->state == kOpening || self->state == kList)
        DrawList(self, x + self->slideList * -0x1E0, y);
    DrawDescription(self, x + self->slideText * -0x1E0, y);
    DrawMoney(self, x + self->slideList * -0x1E0, y);
    if (self->state == kTaken || self->state == kPaid)
        DrawChosen(self, x, y, 2);
    if (self->state == kBagFull || self->state == kAskPay)
        DrawChosen(self, x, y, 1);
    if (self->state == kAskTake)
        DrawChosen(self, x, y, 0);
    if (self->state == kAskTake || self->state == kAskPay) {
        ChoiceWindow* box = P<ChoiceWindow>(self->message);
        VirtualCall<int, void*>(box->w.vtable, 0x14, box);
    }
    if (self->state == kList) {
        Font_SetColor(font, 0x80FFFFFFu);
        Font_DrawString(font, P<const u16>(sData.common[4]), x + 0x1DA0, y + 0x1870, 0xFFF000u, 0.82f, 0.0f);
    }
    return 1;
}

// 0x22C280 (vtable +0x0C)
int Shop_Show(ShopMenu* self, const u32* params, u32 hud) {
    self->shop = params[0];
    self->buyer = params[1];
    self->seller = params[2];
    self->hud = hud;
    self->strength = *reinterpret_cast<const s32*>(reinterpret_cast<const u8*>(*gPlayerData) + 0x148);
    if (self->strength <= 30) self->tier = 0;
    else if (self->strength <= 60) self->tier = 1;
    else if (self->strength <= 80) self->tier = 2;
    else if (self->strength <= 100) self->tier = 3;
    return 1;
}

// 0x22C310 (vtable +0x08)
ShopMenu* Shop_Dtor(ShopMenu* self, s16 flag) {
    if (self == nullptr)
        return self;
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    if (self->sign != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sign), 1);
    if (self->icons != 0)
        IconSet_Dtor(P<IconSet>(self->icons), 1);
    for (u32 widget : {self->message, self->balloon})
        if (widget != 0)
            VirtualCall<void*, void*, int>(P<SpeechWidget>(widget)->vtable, 0x08, P(widget), 1);
    if (self->repeat != 0)
        MemFree(P(self->repeat));
    self->sprites = self->sign = self->icons = self->message = self->balloon = self->repeat = 0;
    if (flag > 0)
        MemFree(self);
    return self;
}

// 0x22C450
ShopMenu* Shop_Ctor(void* mem) {
    ShopMenu* self = static_cast<ShopMenu*>(mem);
    memset(self, 0, sizeof *self);
    self->vtable = 0x2EC610u;
    void* repeat = MemAlloc(sizeof(PadRepeatPair));
    if (repeat != nullptr) {
        PadRepeatPair_Ctor(static_cast<PadRepeatPair*>(repeat));
        static_cast<PadRepeatPair*>(repeat)->vtable = 0x2EAA00u;
        PadRepeatPair_SetMasks(static_cast<PadRepeatPair*>(repeat), kPadUp, kPadDown);
    }
    self->repeat = guest::addr(repeat);
    PadRepeatPair_SetTiming(P<PadRepeatPair>(self->repeat), 10, 3, 7);
    self->slideSign = 10;
    self->slideList = 20;
    self->slideText = 30;
    void* box = MemAlloc(sizeof(ChoiceWindow));
    self->message = box != nullptr ? guest::addr(ShopMessage_Ctor(box)) : 0;
    void* sprites = MemAlloc(0x14);
    self->sprites = sprites != nullptr ? guest::addr(SpriteSet_Ctor(sprites)) : 0;
    SpriteSet_Load(P<SpriteSet>(self->sprites), 0x69, -1);
    void* icons = MemAlloc(sizeof(IconSet));
    self->icons = icons != nullptr ? guest::addr(IconSet_Ctor(static_cast<IconSet*>(icons))) : 0;
    IconSet_Load(P<IconSet>(self->icons));
    return self;
}

const struct ShopBinder {
    ShopBinder() {
        guest::bind(0x22C310, Shop_Dtor, "Shop_Dtor");
        guest::bind(0x22C280, Shop_Show, "Shop_Show");
        guest::bind(0x22B850, Shop_Update, "Shop_Update");
        guest::bind(0x22A2D0, Shop_Draw, "Shop_Draw");
    }
} sShopBinder;

}  // namespace

extern "C" {

// 0x21CE00: opens the shop menu over the HUD.  params = {shop, buyer's
// CharaParams, seller's}.
int Hud_OpenShop(void* hudMem, const u32* params) {
    Hud* hud = static_cast<Hud*>(hudMem);
    if (hud->overlay == 0) {
        void* mem = MemAlloc(sizeof(ShopMenu));
        hud->overlay = mem != nullptr ? guest::addr(Shop_Ctor(mem)) : 0;
    }
    ShopMenu* shop = P<ShopMenu>(hud->overlay);
    VirtualCall<int, void*, const u32*, u32>(shop->vtable, 0x0C, shop, params, guest::addr(hud));
    return 1;
}
// 0x21CDF0
int Hud_ShopOpen(void* hudMem) { return static_cast<Hud*>(hudMem)->overlay != 0; }
// How it ended (Hud+0x4C): 8 the player left, 9 could not pay, 10 would not.
int Hud_ShopResult(void* hudMem) { return (int)static_cast<Hud*>(hudMem)->_4c; }

}  // extern "C"
