// The status menu's first page, "Sword Selection" (0x192650–0x193CE0, 0x23C
// bytes, vtable 0x2EB2B0), and what it is made of:
//
//   the sword pictures   0x2A34A0–0x2A3A70  one sprite sheet per kind of sword,
//                        named by Common/weaponimage/list (asset 0x4E), read
//                        when first wanted
//   the weapon column    0x28A070–0x28BC70  a sword's card: picture, kind,
//                        name, durability, attack, upgrades, defence, a count
//                        (resources: pack 0x4D menu/WeaponColumn/Data)
//   the carousel         0x28FC60–0x291390  the carried swords' cards on an
//                        ellipse; Up / Down turn it
//
// Cross on a sword opens Ready / Drop / Details (or, when something is to be
// handed over, Give / Details / Cancel).  Readying only marks the card: the
// scene readies the sword when the menu closes (StatusMenu_SwordChanged).
// Coming from the items page with an item to use on a sword, Cross uses it.
//
// When a sword is readied its mark is stamped on: a short timeline of copies
// of the mark, each fading in and out at its own offset (0x28C050, from the
// pack's 0x18-byte records).

#include "engine/containers.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/param_file.h"
#include "engine/text_label.h"
#include "engine/window.h"
#include "game/gameplay_scene.h"
#include "game/item_select.h"
#include "game/player_data.h"
#include "hal/ps2_types.h"
#include "game/sword_cards.h"
#include "native/guest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <vector>

struct SpriteSet;
extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    SpriteSet* SpriteSet_Ctor(void* mem);                               // 0x17C630
    char  SpriteSet_LoadByName(SpriteSet* set, const char* a, const char* b);   // 0x17C2F0
    char  SpriteSet_ParseSized(SpriteSet* set, const void* data, u32 size);
    int   SpriteSet_IsLoaded(SpriteSet* set);                           // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                               // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);                     // 0x17C570
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    int   SpriteSet_DrawEx(void* set, int index, void* params);         // 0x17ADF0
    void* AssetRef_Ctor(void* mem);                                     // 0x174C50
    char  AssetQueue_Request(void* ref, int assetId);                   // 0x174C90
    void* AssetRef_GetData(void* ref);                                  // 0x174A00
    void* ArchiveFile_Create();
    void  ArchiveFile_Open(void* file, int assetId);                    // 0x142AA0
    u32   ArchiveFile_GetSize(void* file);
    void  ArchiveFile_Read(void* file, void* dst, u32 size);
    void  GsDevice_SetBlend(GsDevice* dev, u64 alpha);                  // 0x130C50
    u32   Screen_X(int v, int align);                                   // 0x12E5E0
    u32   Screen_Y(int v, int align);                                   // 0x12E550
    void  Sound_PlaySE(int id, int randomPitch, int volume);            // 0x180E70
    void* PadMgr_Get();                                                 // 0x13A920
    int   CharaMgr_PlayerIsFree(void* mgr);                             // 0x143190
    void* MenuRes_Get();                                                // 0x22C8F0
    void  StatusFrame_SetHelp(void* frame, const u16* text);            // 0x22D460
    void  StatusFrame_SetHints(void* frame, int which);
    void  StatusFrame_ShowArrows(void* frame, int on);
    void  StatusFrame_SetDurability(void* frame, int durability);       // 0x22D630
    // src/game/items_page.cpp
    int   StatusItem_Use(void* icons, u32 item, u8* params, int swordSlot); // 0x233A30
    void  ItemsPage_Consume(void* page);                                // 0x211F10
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
const GuestPtrVar<Font>       gFont{0x2EE67Cu};
const GuestPtrVar<WindowMgr>  gWindowMgr{0x2EE680u};
const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
u32* const gFrameRate   = guest::ptr<u32>(0x2EDA00u);
u32* const gColSprites  = guest::ptr<u32>(0x2EE79Cu);   // the weapon column's sprites
u32* const gColParams   = guest::ptr<u32>(0x2EE7A0u);   // …and numbers (a ParamFile)
u32* const gColPack     = guest::ptr<u32>(0x2EE7A4u);
s32* const gColState    = guest::ptr<s32>(0x2EE7A8u);
u32* const gImages      = guest::ptr<u32>(0x2EE7ACu);   // {SwordImage[], count}

inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }

// A reader's next `size` bytes (0x223530-style: nothing if fewer remain).
void Take(BufferReader* r, void* out, u32 size) {
    u32 got = 0;
    if (r->remaining >= size) {
        memcpy(out, P(r->cur), size);
        got = size;
    }
    r->cur += got;
    r->remaining -= got;
}
inline void Rewind(BufferReader* r) { r->cur = r->start; r->remaining = (u32)r->size; }

// ═══ The sword pictures ════════════════════════════════════════════════════
struct SwordImage { u32 sprites; u32 name; };

// 0x2A37B0 (→ 0x2A34A0): the table, made on first use from the list file.
u32* Images() {
    if (*gImages != 0)
        return P<u32>(*gImages);
    u32* table = static_cast<u32*>(MemAlloc(8));
    table[0] = table[1] = 0;
    void* file = ArchiveFile_Create();
    ArchiveFile_Open(file, 0x4E);
    const u32 size = ArchiveFile_GetSize(file);
    if (size >= 4) {
        u8* data = static_cast<u8*>(MemAlloc(size));
        ArchiveFile_Read(file, data, size);
        u32 count;
        memcpy(&count, data, 4);
        SwordImage* images = static_cast<SwordImage*>(MemAlloc(count * sizeof(SwordImage) + 0x10));
        const u8* at = data + 4;
        for (u32 i = 0; i < count; i++) {
            const s8 length = (s8)*at++;
            char* name = static_cast<char*>(MemAlloc((u32)length + 1));
            memcpy(name, at, (size_t)length);
            name[length] = 0;
            at += length;
            images[i].sprites = 0;
            images[i].name = guest::addr(name);
        }
        table[0] = guest::addr(images);
        table[1] = count;
        MemFree(data);
    }
    *gImages = guest::addr(table);
    return table;
}
// 0x2A38E0: → still being read
bool Image_Loading(SwordImage* image) {
    if (image->sprites == 0) {
        image->sprites = guest::addr(SpriteSet_Ctor(MemAlloc(0x14)));
        if (SpriteSet_LoadByName(P<SpriteSet>(image->sprites), P<const char>(image->name), nullptr) == 0) {
            SpriteSet_Dtor(P<SpriteSet>(image->sprites), 1);
            image->sprites = 0;
            return true;
        }
    }
    return SpriteSet_IsLoaded(P<SpriteSet>(image->sprites)) == 0;
}
// 0x2A3990
void Image_Draw(SwordImage* image, void* params) {
    if (Image_Loading(image))
        return;
    SpriteSet_Bind(P<SpriteSet>(image->sprites));
    SpriteSet_DrawEx(P(image->sprites), 0, params);
}

// ═══ A sword's card ════════════════════════════════════════════════════════
// Where things go on a card: 0x124 bytes of numbers out of the pack (pixels
// from the card's corner): frame, shadow, the kind's name, eleven fields
// {sprite, x, y, text x, text y}, the picture, a rule, the "readied" mark.
struct ColumnLayout {
    s32 frame[4];           // +0x00  x0, y0, x1, y1
    s32 shadow[4];          // +0x10
    s32 kindX, kindY;       // +0x20
    struct Field { s32 sprite, x, y, textX, textY; } field[11];     // +0x28
    s32 pictureX, pictureY; // +0x104
    s32 rule[4];            // +0x10C
    s32 markX, markY;       // +0x11C
};
static_assert(sizeof(ColumnLayout) == 0x124, "ColumnLayout");

struct WStringV { WString s; u32 vtable; };

struct Card {               // 0xE0 bytes
    u16 x, y;               // +0x00
    u32 z;                  // +0x04
    float alpha;            // +0x08
    u8  alphaByte;          // +0x0C
    u8  shade;              // +0x0D  how dark, by where it is on the carousel
    u8  marked;             // +0x0E  this is the readied sword
    u8  _0f;
    s32 markFrame;          // +0x10  the mark's animation: frames since it was readied,
    s32 markNext;           // +0x14  the next record to start,
    u8  markPlaying;        // +0x18  running
    u8  _19[3];
    u32 markList[3];        // +0x1C  (the copies showing: kept on the host side here)
    u32 markRecords;        // +0x28
    u32 markSprites;        // +0x2C
    s32 flash;              // +0x30  1 attack / 2 defence has just gone up
    s32 flashTimer;         // +0x34
    u32 sword;              // +0x38
    u32 layout;             // +0x3C  ColumnLayout* (the carousel begins with it)
    u32 sprites;            // +0x40
    u32 image;              // +0x44  SwordImage*
    u32 kindName;           // +0x48
    WStringV name;          // +0x4C
    u32 label[1];           // +0x5C  "Dur"
    WStringV durability;    // +0x60
    u32 attackLabel;        // +0x70
    WStringV attack;        // +0x74
    u32 upgradesLabel;      // +0x84
    WStringV upgrades;      // +0x88
    u32 defenceLabel;       // +0x98
    WStringV defence;       // +0x9C
    u32 countLabel;         // +0xAC
    WStringV count;         // +0xB0
    u32 vtable;             // +0xC0  0x2ED148
    s16 place, placeFrom;   // +0xC4  which of the carousel's places
    float angle;            // +0xC8  …and its angle
    float angleFrom;        // +0xCC
    float turn;             // +0xD0
    s32 shadeTo, shadeFrom; // +0xD4
    u32 table;              // +0xDC  the carousel's places
};
static_assert(sizeof(Card) == 0xE0 && offsetof(Card, vtable) == 0xC0 && offsetof(Card, name) == 0x4C, "Card");

// The carousel's numbers (0x60 bytes at +0x15C; read by 0x291390): the angle
// and the shade of each place when there are one, two or three swords.
struct Places {
    s32 angle[3][3];        // +0x00  [count − 1][place], degrees
    s32 shade[3][3];        // +0x24
    s32 centreX, centreY;   // +0x48
    float radiusX, radiusY; // +0x50
    u32 _58;
    u32 count;              // +0x5C  → the carousel's count
};
static_assert(sizeof(Places) == 0x60, "Places");

struct Carousel {           // 0x1EC bytes (the page's +0x28)
    ColumnLayout layout;    // +0x000
    s32 recordCount;        // +0x124
    GuestVector records;    // +0x128
    u32 baseVtable;         // +0x134
    s32 loadState;          // +0x138  1 pictures loading, 3 ready
    s16 current;            // +0x13C  index of the card in front
    s16 _13e;
    GuestVector cards;      // +0x140  Card*[]
    GuestVector slots;      // +0x14C  s16[]: which carried sword each card is
    s16 readied;            // +0x158  index of the readied sword's card
    s16 _15a;
    Places places;          // +0x15C
    float slide;            // +0x1BC  0 … 1 through a turn
    u8  sliding;            // +0x1C0
    u8  _1c1[3];
    s32 direction;          // +0x1C4  1 / 2: which arrow is lit
    s32 queued;             // +0x1C8  another turn asked for meanwhile
    u8  confirm;            // +0x1CC  Cross was pressed with a card in front
    u8  _1cd[3];
    s32 arrowUp[2];         // +0x1D0
    s32 arrowDown[2];       // +0x1D8
    s8  count;              // +0x1E0
    s8  readiedSlot;        // +0x1E1
    s8  readiedAtStart;     // +0x1E2
    u8  _1e3;
    u32 swords;             // +0x1E4
    u32 vtable;             // +0x1E8
};
static_assert(sizeof(Carousel) == 0x1EC && offsetof(Carousel, places) == 0x15C && offsetof(Carousel, count) == 0x1E0,
              "Carousel");

inline Card* CardAt(Carousel* c, int i) { return P<Card>(P<u32>(c->cards.data)[i]); }
inline s16 SlotAt(Carousel* c, int i) { return P<s16>(c->slots.data)[i]; }
inline int Wrap(int v, int n) { return v < n ? (v < 0 ? v + n : v) : v - n; }

void SetNumber(WStringV* out, int a, const u16* middle = nullptr, int b = 0) {
    static u32 buffer = 0;
    if (buffer == 0)
        buffer = guest::addr(guest::alloc(0x80));
    u16* text = P<u16>(buffer);
    char digits[16];
    int n = 0;
    snprintf(digits, sizeof digits, "%d", a);
    for (const char* c = digits; *c != 0; c++)
        text[n++] = (u16)*c;
    if (middle != nullptr) {
        for (; *middle != 0 && n < 40; middle++)
            text[n++] = *middle;
        snprintf(digits, sizeof digits, "%d", b);
        for (const char* c = digits; *c != 0; c++)
            text[n++] = (u16)*c;
    }
    text[n] = 0;
    WString_Assign(&out->s, text, text + n);
}
inline const u16* Chars(const WStringV* s) { return P<const u16>(P<WStringRep>(s->s.rep)->data); }

// ── 0x28B500: the card's words, from its sword ────────────────────────────
void Card_SetTexts(Card* card) {
    if (card->sword == 0)
        return;
    const u8* sword = P<u8>(card->sword);
    const auto s32at = [sword](u32 o) { s32 v; memcpy(&v, sword + o, 4); return v; };
    const auto u16at = [sword](u32 o) { u16 v; memcpy(&v, sword + o, 2); return v; };
    const u16* name = P<const u16>(card->sword);
    int n = 0;
    while (name[n] != 0)
        n++;
    WString_Assign(&card->name.s, name, name + n);
    card->kindName = guest::addr(Text(s32at(0x74) + 0x33));
    card->label[0] = guest::addr(Text(0x2E));
    SetNumber(&card->durability, s32at(0x60));
    card->attackLabel = guest::addr(Text(0x2F));
    SetNumber(&card->attack, s32at(0x58));
    card->upgradesLabel = guest::addr(Text(0x31));
    SetNumber(&card->upgrades, u16at(0x64), Text(2), u16at(0x66));
    card->defenceLabel = guest::addr(Text(0x30));
    SetNumber(&card->defence, s32at(0x5C));
    card->countLabel = guest::addr(Text(0x32));
    SetNumber(&card->count, s32at(0x88));
}

// ── The mark's animation (0x28B4A0, 0x28C050–0x28C390, 0x28BE50) ──────────
// The holder's numbers end with it (0x28A4F0): s32 n, s32 frames in all, n ×
// {s32 start frame, frames fading in, frames fading out, strength (128 =
// full), x, y (px from the mark's place)}.  Readying a sword starts the
// timeline on its card: a record's copy of the mark appears at its frame,
// fades in, fades out and goes; when the frames are up the mark just shows.
struct MarkRecord { s32 start, in, out, strength, x, y; };
struct MarkCopy { s32 fadingIn, timer; const MarkRecord* record; };
static std::map<u32, std::vector<MarkCopy>> sMarkCopies;       // by card (the original: a list at card +0x1C)
static void Holder_ReadMarks(void* holder, BufferReader* r) {          // 0x28A4F0
    s32 n = 0;
    s32* total = reinterpret_cast<s32*>(static_cast<u8*>(holder) + 0x124);
    GuestVector* records = reinterpret_cast<GuestVector*>(static_cast<u8*>(holder) + 0x128);
    Take(r, &n, 4);
    Take(r, total, 4);
    if (n <= 0 || n > 256)
        return;
    MarkRecord* data = static_cast<MarkRecord*>(MemAlloc((u32)n * sizeof(MarkRecord)));
    memset(data, 0, (u32)n * sizeof(MarkRecord));
    for (s32 i = 0; i < n; i++)
        Take(r, &data[i], sizeof(MarkRecord));
    *records = {(u32)n, (u32)n, guest::addr(data)};
}
static void Holder_FreeMarks(void* holder) {
    GuestVector* records = reinterpret_cast<GuestVector*>(static_cast<u8*>(holder) + 0x128);
    if (records->data != 0)
        MemFree(P(records->data));
    *records = {0, 0, 0};
}
void Card_Mark(Card* card) {                                            // 0x28B4A0
    card->marked = 1;
    card->markPlaying = 1;
    card->markFrame = 0;
    card->markNext = 0;
    sMarkCopies[guest::addr(card)].clear();
}
void Card_Unmark(Card* card) { card->marked = 0; }                      // 0x28B480
static void Card_StepMark(Card* card) {                                 // 0x28C390 (0x28C050, 0x28C120)
    if (card->markPlaying != 1 || card->markRecords == 0)
        return;
    const s32 total = *P<s32>(card->markRecords);
    const GuestVector* records = P<GuestVector>(card->markRecords + 4);
    const MarkRecord* data = P<MarkRecord>(records->data);
    std::vector<MarkCopy>& copies = sMarkCopies[guest::addr(card)];
    while ((u32)card->markNext < records->count && card->markFrame == data[card->markNext].start)
        copies.push_back({1, 0, &data[card->markNext++]});
    for (size_t i = 0; i < copies.size();) {
        MarkCopy& c = copies[i];
        if (c.fadingIn == 1 && c.record->in <= c.timer) {
            c.fadingIn = 0;
            c.timer = 0;
        }
        if (c.fadingIn == 0 && c.record->out <= c.timer) {
            copies.erase(copies.begin() + (long)i);
            continue;
        }
        c.timer++;
        i++;
    }
    if (total <= ++card->markFrame)
        card->markPlaying = 0;
}
static void Card_DrawMark(Card* card, s32 x, s32 y) {                   // 0x28BE50
    if (card->markPlaying == 1) {
        const auto found = sMarkCopies.find(guest::addr(card));
        if (found == sMarkCopies.end())
            return;
        for (const MarkCopy& c : found->second) {
            const MarkRecord* rec = c.record;
            const float part = c.fadingIn == 1 ? (float)c.timer / (float)rec->in : 1.0f - (float)c.timer / (float)rec->out;
            SpriteSet_Draw(P<SpriteSet>(card->markSprites), 5, (x & 0xFFFF) + rec->x * 0x10, (y & 0xFFFF) + rec->y * 0x10,
                           card->z, (u32)(s32)(part * card->alpha * (float)rec->strength) << 24 | 0x808080);
        }
    } else if (card->marked != 0) {
        SpriteSet_Draw(P<SpriteSet>(card->markSprites), 5, x & 0xFFFF, y & 0xFFFF, card->z,
                       (u32)(s32)(card->alpha * 128.0f) << 24 | 0x808080);
    }
}

// ── 0x28BC70 (with 0x28A070, which gathers what it is given) ──────────────
Card* Card_Ctor(Card* card, Carousel* owner, u32 sword) {
    memset(card, 0, sizeof *card);
    card->vtable = 0x2ED148u;
    for (WStringV* s : {&card->name, &card->durability, &card->attack, &card->upgrades, &card->defence, &card->count}) {
        s->vtable = 0x2EAF30u;
        WString_Ctor(&s->s);
        WString_Reserve(&s->s, 0);
    }
    card->x = (u16)Screen_X(0, -1);
    card->y = (u16)Screen_Y(0, -1);
    card->alpha = 1.0f;
    card->layout = guest::addr(owner);
    card->markRecords = guest::addr(&owner->recordCount);
    card->markSprites = *gColSprites;
    card->sprites = *gColSprites;
    card->sword = sword;
    const u32* images = Images();
    const u32 kind = *P<u32>(sword + 0x78);
    card->image = kind < images[1] ? images[0] + kind * 8 : 0;
    return card;
}
// 0x291330 / 0x28B830 (vtable +0x08)
void* Card_Dtor(Card* card, int flag) {
    if (card == nullptr)
        return card;
    for (WStringV* s : {&card->count, &card->defence, &card->upgrades, &card->attack, &card->durability, &card->name})
        WString_Release(&s->s);
    sMarkCopies.erase(guest::addr(card));
    if ((s16)flag > 0)
        MemFree(card);
    return card;
}
GUEST_BIND(0x291330, Card_Dtor);

inline const ColumnLayout* LayoutOf(const Card* card) { return P<ColumnLayout>(card->layout); }

// ── 0x28A9D0: a flat rectangle on the card ────────────────────────────────
void Card_FillRect(const Card* card, const s32* rect, u32 rgba) {
    struct V { u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 prim; V v[2]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.prim = 0x46;
    const u32 colour = (rgba & 0xFFFFFF) | (u32)(s32)(card->alpha * (float)(rgba >> 24)) << 24;
    packet.v[0] = {colour, 1.0f, (u16)(card->x + (s16)(rect[0] << 4)), (u16)(card->y + (s16)(rect[1] << 4)), card->z};
    packet.v[1] = {colour, 1.0f, (u16)(card->x + (s16)(rect[2] << 4)), (u16)(card->y + (s16)(rect[3] << 4)), card->z};
    GsDevice_DrawPrim(*gGsDevice, &packet, 2);
}

// ── 0x28AC00: frame and field plates ──────────────────────────────────────
void Card_DrawBody(const Card* card) {
    const ColumnLayout* l = LayoutOf(card);
    SpriteSet* sprites = P<SpriteSet>(card->sprites);
    const u32 tint = (u32)card->alphaByte << 24 | 0x808080;
    Card_FillRect(card, l->shadow, 0x53080404u);
    Card_FillRect(card, l->frame, 0x7FACB2BEu);
    for (int i = 0; i < 11; i++) {
        const ColumnLayout::Field& f = l->field[i];
        const int x = (card->x + f.x * 0x10) & 0xFFFF, y = (card->y + f.y * 0x10) & 0xFFFF;
        SpriteSet_Draw(sprites, f.sprite, x, y, card->z, tint);
        // The attack (field 4) or defence (field 8) plate glows when an item has just raised it.
        if ((i == 4 && (card->flash & 1)) || (i == 8 && (card->flash & 2))) {
            GsDevice_SetBlend(*gGsDevice, 0x8000000048ull);
            const s32 half = ((s32)(*gFrameRate * 0x12) / 0x1E) >> 1;
            float away = (float)card->flashTimer / (float)(half != 0 ? half : 1) - 1.0f;
            if (away < 0.0f)
                away = -away;
            const float glow = (1.0f - away) * 0.8f * (float)card->alphaByte;
            SpriteSet_Draw(sprites, 6, x, y, card->z, (u32)(s32)glow << 24 | 0x808080);
            GsDevice_SetBlend(*gGsDevice, 0x8000000044ull);
        }
    }
    Card_FillRect(card, l->rule, 0x7F1A1818u);
}

// ── 0x28A900 / 0x28AAD0: the name and the five label–number pairs ─────────
void Card_DrawTexts(const Card* card) {
    const ColumnLayout* l = LayoutOf(card);
    Font* font = *gFont;
    Font_SetColor(font, (u32)card->alphaByte << 24 | 0xBCBEC8);
    const auto at = [&](int field, int* x, int* y) {
        *x = card->x + l->field[field].textX * 0x10;
        *y = card->y + l->field[field].textY * 0x10;
    };
    int x, y;
    at(0, &x, &y);
    Font_DrawString(font, Chars(&card->name), x, y, card->z, 1.0f, 0.0f);
    GsDevice_End2D(*gGsDevice);
    const u16* const texts[10] = {
        P<const u16>(card->label[0]), Chars(&card->durability), P<const u16>(card->attackLabel), Chars(&card->attack),
        P<const u16>(card->upgradesLabel), Chars(&card->upgrades), P<const u16>(card->defenceLabel),
        Chars(&card->defence), P<const u16>(card->countLabel), Chars(&card->count)};
    for (int i = 0; i < 10; i++) {
        at(i + 1, &x, &y);
        Font_DrawCentered(font, texts[i], x, y, card->z, 0.9f, 0.0f);
    }
    GsDevice_Begin2D(*gGsDevice);
}

// ── 0x28B2D0: a card, all but its picture ─────────────────────────────────
void Card_Draw(Card* card) {
    card->alphaByte = (u8)(s32)(card->alpha * 128.0f);
    const ColumnLayout* l = LayoutOf(card);
    Card_DrawBody(card);
    Card_DrawTexts(card);
    Card_DrawMark(card, card->x + l->markX * 0x10, card->y + l->markY * 0x10);
    Card_FillRect(card, l->frame, (u32)card->shade << 24 | 0x80404);
}

// ── 0x28A6B0: its picture, and the kind of sword written over it ──────────
void Card_DrawPicture(Card* card) {
    if (card->image == 0)
        return;
    const ColumnLayout* l = LayoutOf(card);
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } draw;
    draw.flags = 1;
    draw.x = card->x + l->pictureX * 0x10;
    draw.y = card->y + l->pictureY * 0x10;
    draw.z = card->z;
    draw.w = draw.h = -1;
    draw.rgba = (u32)(s32)(card->alpha * 128.0f) << 24 | 0x808080;
    Image_Draw(P<SwordImage>(card->image), &draw);
    Font* font = *gFont;
    const int x = (l->kindX * 0x10 + card->x) & 0xFFFF, y = (l->kindY * 0x10 + card->y) & 0xFFFF;
    const u16* kind = P<const u16>(card->kindName);
    Font_SetColor(font, (u32)card->alphaByte << 24 | 0x101010);
    Font_DrawCentered(font, kind, x + 0x20, y, card->z, 0.9f, 0.0f);
    Font_DrawCentered(font, kind, x - 0x20, y, card->z, 0.9f, 0.0f);
    Font_DrawCentered(font, kind, x, y + 0x20, card->z, 0.9f, 0.0f);
    Font_DrawCentered(font, kind, x, y - 0x20, card->z, 0.9f, 0.0f);
    Font_SetColor(font, (u32)card->alphaByte << 24 | 0xBCBEC8);
    Font_DrawCentered(font, kind, x, y, card->z, 0.9f, 0.0f);
}

// ── 0x28B3C0: one frame (the glow runs out) ───────────────────────────────
void Card_Update(Card* card) {
    Card_StepMark(card);
    if (card->flash != 0 && (s32)(*gFrameRate * 0x12) / 0x1E < ++card->flashTimer)
        card->flash = 0;
}

// ── 0x290EF0: where the card is, `t` of the way through a turn ────────────
void Card_Place(Card* card, float t) {
    const Places* places = P<Places>(card->table);
    float a;
    if (card->place == 1 && card->placeFrom == 2)
        a = card->angleFrom + card->turn * (t != 0.0f ? (t + 1.0f) / 2.0f : 0.0f);
    else
        a = card->angleFrom + card->turn * t;
    if (a >= 360.0f)
        a -= 360.0f;
    else if (a < 0.0f)
        a += 360.0f;
    const float rad = a * 3.1415927f / 180.0f;
    card->x = (u16)Screen_X(((s32)(places->radiusX * std::cos(rad)) + places->centreX) * 0x10, -1);
    card->y = (u16)Screen_Y(((s32)(places->radiusY * std::sin(rad)) + places->centreY) * 0x10, -1);
    card->z = 0xFFFFB4u + (u32)(s32)(std::cos((a - 40.0f) * 3.1415927f / 180.0f) * 30.0f);
    card->shade = (u8)(s32)((float)card->shadeFrom + t * (float)(card->shadeTo - card->shadeFrom));
}

// ── 0x291190: sends the card to another place; `way` 1 / 2 the long way round ─
void Card_GoTo(Card* card, int place, int way) {
    const Places* places = P<Places>(card->table);
    const int n = *P<s8>(places->count);
    card->placeFrom = card->place;
    card->place = (s16)Wrap(place, n);
    card->shadeFrom = card->shadeTo;
    card->shadeTo = places->shade[n - 1][card->place];
    card->angleFrom = card->angle;
    card->angle = (float)places->angle[n - 1][card->place];
    card->turn = card->angle - card->angleFrom;
    if (way == 2) {
        if (card->turn > 0.0f)
            card->turn -= 360.0f;
    } else if (way == 1) {
        if (card->turn < 0.0f)
            card->turn += 360.0f;
    } else {
        if (card->turn > 180.0f)
            card->turn -= 180.0f;
        if (card->turn < -180.0f)
            card->turn += 180.0f;
    }
}

// ═══ The carousel ══════════════════════════════════════════════════════════

// ── 0x28A1A0: the weapon column's pack, a step at a time (at boot) ────────
int Column_Poll() {
    if (*gColState == 2)
        return 1;
    if (*gColState == 0) {
        *gColPack = guest::addr(AssetRef_Ctor(MemAlloc(0xC)));
        AssetQueue_Request(P(*gColPack), 0x4D);
        *gColState = 1;
        return 0;
    }
    const u8* pack = static_cast<const u8*>(AssetRef_GetData(P(*gColPack)));
    if (pack == nullptr)
        return 0;
    const s32* at = reinterpret_cast<const s32*>(pack);
    *gColSprites = guest::addr(SpriteSet_Ctor(MemAlloc(0x14)));
    SpriteSet_ParseSized(P<SpriteSet>(*gColSprites), pack + at[4], (u32)at[5]);
    *gColParams = guest::addr(ParamFile_Ctor(static_cast<ParamFile*>(MemAlloc(0x4C))));
    ParamFile_SetData(P<ParamFile>(*gColParams), reinterpret_cast<const char*>(pack + at[6]), at[7]);
    *gColState = 2;
    return 1;
}

// ── 0x290DE0 (base 0x28A370): the layout comes out of the column's numbers ─
void Carousel_Ctor(Carousel* self) {
    memset(self, 0, sizeof *self);
    self->vtable = 0x2ED128u;
    self->baseVtable = 0x2ED138u;
    self->places.count = guest::addr(&self->count);
    if (*gColParams != 0) {
        BufferReader* r = &P<ParamFile>(*gColParams)->out;
        Rewind(r);
        Take(r, &self->layout, sizeof self->layout);
        Holder_ReadMarks(self, r);
    }
}

// ── 0x290B00: a card for every carried sword ──────────────────────────────
void Carousel_Begin(Carousel* self, int count, int readied, u32 swords, const s16* order) {
    self->loadState = 1;
    self->swords = swords;
    self->count = (s8)count;
    self->readiedSlot = self->readiedAtStart = (s8)readied;
    u32* cards = static_cast<u32*>(MemAlloc(3 * sizeof(u32)));
    s16* slots = static_cast<s16*>(MemAlloc(3 * sizeof(s16)));
    self->cards = {3, 0, guest::addr(cards)};
    self->slots = {3, 0, guest::addr(slots)};
    for (int slot = 0; slot < 3; slot++) {
        if (order[slot] == -1)
            continue;
        Card* card = Card_Ctor(static_cast<Card*>(MemAlloc(sizeof(Card))), self, swords + (u32)slot * 0x100);
        card->table = guest::addr(&self->places);
        Card_SetTexts(card);
        cards[self->cards.count++] = guest::addr(card);
        slots[self->slots.count++] = (s16)slot;
        if (slot == readied)
            self->readied = (s16)(self->cards.count - 1);
    }
    self->count = (s8)self->cards.count;
}

// ── 0x290030: pictures in, then the carousel's own numbers ────────────────
int Carousel_LoadStep(Carousel* self) {
    if (self->loadState != 1)
        return 1;
    for (int i = 0; i < self->count; i++) {
        const Card* card = CardAt(self, i);
        if (card->image != 0 && Image_Loading(P<SwordImage>(card->image)))
            return 1;
    }
    self->loadState = 2;
    BufferReader* r = &P<ParamFile>(static_cast<const u32*>(MenuRes_Get())[4])->out;
    Rewind(r);
    // 0x291390: centre, radii, then angles and shades for one, two and three swords.
    Take(r, &self->places.centreX, 8);
    Take(r, &self->places.radiusX, 8);
    s32 v[12] = {};
    for (s32& value : v)
        Take(r, &value, 4);
    memset(self->places.angle, 0, sizeof self->places.angle);
    memset(self->places.shade, 0, sizeof self->places.shade);
    self->places.angle[0][0] = v[0];
    self->places.angle[1][0] = v[1];
    self->places.angle[1][1] = v[2];
    self->places.angle[2][0] = v[3];
    self->places.angle[2][1] = v[4];
    self->places.angle[2][2] = v[5];
    self->places.shade[0][0] = v[6];
    self->places.shade[1][0] = v[7];
    self->places.shade[1][1] = v[8];
    self->places.shade[2][0] = v[9];
    self->places.shade[2][1] = v[10];
    self->places.shade[2][2] = v[11];
    Take(r, self->arrowUp, 8);
    Take(r, self->arrowDown, 8);
    self->loadState = 3;
    return 1;
}

// ── 0x290950: the readied sword in front, nothing moving ──────────────────
void Carousel_Reset(Carousel* self) {
    const int n = self->count;
    for (int i = 0; i < n; i++) {
        Card* card = CardAt(self, Wrap(i + self->readied, n));
        card->place = card->placeFrom = (s16)i;
        card->angle = card->angleFrom = (float)self->places.angle[n - 1][i];
        card->shadeTo = card->shadeFrom = self->places.shade[n - 1][i];
        card->turn = 0.0f;
        Card_Place(card, 0.0f);
        card->marked = i == 0 ? 1 : 0;
    }
    self->current = self->readied;
    self->sliding = 0;
    self->slide = 0.0f;
    self->queued = 0;
}

// The next (way 1) or previous (way 2) card comes to the front.
void Carousel_Turn(Carousel* self, int way) {
    const int n = self->count;
    self->current = (s16)Wrap(self->current + (way == 1 ? 1 : -1), n);
    self->sliding = 1;
    self->direction = way;
    for (int i = 0; i < n; i++) {
        Card* card = CardAt(self, i);
        Card_GoTo(card, card->place + (way == 1 ? -1 : 1), way == 1 ? 2 : 1);
    }
    Sound_PlaySE(0x1A, 0, 0x3FFF);
}
// 0x290730 / 0x290830: Up / Down
void Carousel_Ask(Carousel* self, int way) {
    if (self->count == 1)
        return;
    if (self->sliding == 0) {
        Carousel_Turn(self, way);
        self->slide = 0.0f;
    } else if (self->queued == 0) {
        self->queued = way;
    }
}
// 0x2906E0: Cross
void Carousel_Confirm(Carousel* self) {
    if (self->sliding == 0 || (self->queued == 0 && self->slide >= 0.75f))
        self->confirm = 1;
}

// ── 0x28FF10 (with 0x290540): a turn in progress ──────────────────────────
void Carousel_Slide(Carousel* self) {
    if (self->sliding == 0)
        return;
    const float rate = (float)*gFrameRate;
    float step = (9.0f / rate) * ((self->queued == 0 ? 1.0f : 2.0f) - self->slide);
    if (step > 3.6f / rate)
        step = 3.6f / rate;
    self->slide += step;
    if (self->slide >= 1.0f) {
        if (self->queued == 1 || self->queued == 2)
            Carousel_Turn(self, self->queued);
        else
            self->sliding = 0;
        self->queued = 0;
        self->slide -= 1.0f;
    }
    if (step <= 0.005f) {
        self->slide = 1.0f;
        self->sliding = 0;
        self->direction = 0;
    }
}

// ── 0x2901A0 ──────────────────────────────────────────────────────────────
int Carousel_Update(Carousel* self) {
    if (self->loadState != 3)
        return Carousel_LoadStep(self);
    for (int i = 0; i < self->count; i++)
        Card_Update(CardAt(self, i));
    if (self->sliding != 0) {
        Carousel_Slide(self);
        for (int i = 0; i < self->count; i++)
            Card_Place(CardAt(self, i), self->slide);
    }
    return 1;
}

// ── 0x290490: the card in front becomes the readied one; → it was not ─────
bool Carousel_Ready(Carousel* self) {
    const s16 slot = SlotAt(self, self->current);
    if (self->readiedSlot == slot)
        return false;
    Card_Unmark(CardAt(self, self->readied));
    Card_Mark(CardAt(self, self->current));
    self->readiedSlot = (s8)slot;
    self->readied = self->current;
    return true;
}

// ── 0x2902A0: the card in front goes; → its slot, −1 it is the readied
// sword, −2 it is the only one ────────────────────────────────────────────
int Carousel_Remove(Carousel* self) {
    if (self->count < 2)
        return -2;
    const int index = self->current;
    const int slot = SlotAt(self, index);
    if (self->readiedSlot == slot)
        return -1;
    u32* cards = P<u32>(self->cards.data);
    s16* slots = P<s16>(self->slots.data);
    Card_Dtor(P<Card>(cards[index]), 1);
    for (int i = index; i + 1 < self->count; i++) {
        cards[i] = cards[i + 1];
        slots[i] = slots[i + 1];
    }
    self->cards.count--;
    self->slots.count--;
    if (index < self->readied)
        self->readied--;
    self->count--;
    const int n = self->count;
    self->current = (s16)Wrap(index, n);
    for (int i = 0; i < n; i++)
        Card_GoTo(CardAt(self, Wrap(i + index, n)), i, 0);
    self->sliding = 1;
    self->slide = 0.0f;
    self->queued = 0;
    return slot;
}

// ── 0x28FC60: the two arrows beside the carousel ──────────────────────────
void Carousel_DrawArrows(Carousel* self) {
    const s32 sx = (s32)(Screen_X(0, -1) & 0xFFFF), sy = (s32)(Screen_Y(0, -1) & 0xFFFF);
    void* sprites = P(static_cast<const u32*>(MenuRes_Get())[3]);
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } draw;
    const auto one = [&](const s32* at, u32 flags, bool lit) {
        draw.flags = flags;
        draw.z = 0xFFFFE6u;
        draw.rgba = 0x80808080u;
        draw.x = sx + (at[0] + 2) * 0x10;
        draw.y = sy + (at[1] + 2) * 0x10;
        draw.w = draw.h = -1;
        SpriteSet_DrawEx(sprites, 0, &draw);
        draw.x = sx + at[0] * 0x10;
        draw.y = sy + at[1] * 0x10;
        draw.w = draw.h = -1;
        SpriteSet_DrawEx(sprites, lit ? 2 : 1, &draw);
    };
    one(self->arrowUp, 1, self->direction == 1);
    one(self->arrowDown, 0x40, self->direction == 2);
}

// ── 0x28FE30 ──────────────────────────────────────────────────────────────
void Carousel_Draw(Carousel* self) {
    if (self->loadState != 3)
        return;
    if (*gColSprites != 0)
        SpriteSet_Bind(P<SpriteSet>(*gColSprites));
    for (int i = 0; i < self->count; i++)
        Card_Draw(CardAt(self, i));
    for (int i = 0; i < self->count; i++)
        Card_DrawPicture(CardAt(self, i));
    if (self->count != 1) {
        SpriteSet_Bind(P<SpriteSet>(static_cast<const u32*>(MenuRes_Get())[3]));
        Carousel_DrawArrows(self);
    }
}

// 0x290C90
void Carousel_Dtor(Carousel* self) {
    for (u32 i = 0; i < self->cards.count; i++)
        Card_Dtor(CardAt(self, (int)i), 1);
    if (self->cards.data != 0)
        MemFree(P(self->cards.data));
    if (self->slots.data != 0)
        MemFree(P(self->slots.data));
    self->cards = {0, 0, 0};
    self->slots = {0, 0, 0};
    Holder_FreeMarks(self);
}

// ═══ The page ══════════════════════════════════════════════════════════════
enum : u32 {
    kBrowse = 0x193840u, kMenu = 0x193620u, kGive = 0x193390u, kMessage = 0x1932C0u, kUsed = 0x1931E0u,
};

struct SwordsPage {
    u32 menu;           // +0x00
    s16 loaded;         // +0x04
    s16 _06;
    u32 vtable;         // +0x08
    s32 choosing;       // +0x0C  1: choosing the sword an item is used on
    u32 window;         // +0x10
    s32 item;           // +0x14
    s32 usedTimer;      // +0x18
    s32 removed;        // +0x1C  the slot of a sword just dropped (−1 none)
    u8  noWorld;        // +0x20
    u8  _21[3];
    s32 mode;           // +0x24  1: something is to be handed over
    Carousel list;      // +0x28
    u32 swords;         // +0x214
    u32 swordDb;        // +0x218
    u32 charaMgr;       // +0x21C
    u32 state[3];       // +0x220
    u32 gsDevice;       // +0x22C
    u32 padMgr;         // +0x230
    u32 windowMgr;      // +0x234
    u32 playerData;     // +0x238
};
static_assert(sizeof(SwordsPage) == 0x23C && offsetof(SwordsPage, list) == 0x28 && offsetof(SwordsPage, swords) == 0x214,
              "SwordsPage");

inline u32 FrameOf(const SwordsPage* self) { return P<u32>(self->menu)[1]; }
inline void SetAction(SwordsPage* self, s32 what, s32 arg = 0) {
    P<s32>(self->menu)[7] = what;
    if (what >= 3 && what != 8)
        P<s32>(self->menu)[8] = arg;
}
inline void SetState(SwordsPage* self, u32 fn) { self->state[0] = 0; self->state[1] = 0xFFFFFFFFu; self->state[2] = fn; }
inline void SetHelp(SwordsPage* self, const u16* text) {
    if (FrameOf(self) != 0)
        StatusFrame_SetHelp(P(FrameOf(self)), text);
}
void CloseWindow(SwordsPage* self) {
    WindowMgr_Close(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
    self->window = 0;
}
// Back to turning the carousel.
void Browse(SwordsPage* self) {
    CloseWindow(self);
    SetState(self, kBrowse);
    SetHelp(self, Text(0x92));
    if (FrameOf(self) == 0)
        return;
    if (self->mode == 1) {
        StatusFrame_SetHints(P(FrameOf(self)), 1);
    } else if (self->mode == 0) {
        StatusFrame_ShowArrows(P(FrameOf(self)), 1);
        StatusFrame_SetHints(P(FrameOf(self)), 0);
    }
}
void ShowMessage(SwordsPage* self, int text) {
    CloseWindow(self);
    SetHelp(self, Text(1));
    self->window = guest::addr(WindowMgr_OpenMessage(P<WindowMgr>(self->windowMgr), 0, 0, 0xFFFFE6u, 2, 0, 0, Text(text), 0));
    SetState(self, kMessage);
}
// 0x192FD0 / 0x192DC0: the menu for the sword in front
void OpenMenu(SwordsPage* self, bool give) {
    WindowDesc desc;
    memset(&desc, 0, sizeof desc);
    WindowDesc_Init(&desc);
    u32* options = static_cast<u32*>(MemAlloc(3 * sizeof(u32)));
    const int ids[3] = {give ? 0x11 : 0x8F, give ? 0x91 : 0x90, give ? 0x13 : 0x91};
    for (int i = 0; i < 3; i++)
        options[i] = guest::addr(Text(ids[i]));
    desc.options = {3, 3, guest::addr(options)};
    desc.z = 0xFFFFE6u;
    desc.priority = 2;
    self->window = guest::addr(WindowMgr_Open(P<WindowMgr>(self->windowMgr), &desc));
    SetHelp(self, Text(0x8E));
    if (FrameOf(self) != 0) {
        StatusFrame_SetHints(P(FrameOf(self)), 3);
        StatusFrame_ShowArrows(P(FrameOf(self)), 0);
    }
    SetState(self, give ? kGive : kMenu);
    MemFree(options);
}

// ── 0x192B50: "Ready"; → done ─────────────────────────────────────────────
bool ReadySword(SwordsPage* self) {
    const s16 slot = SlotAt(&self->list, self->list.current);
    if (self->charaMgr != 0 && CharaMgr_PlayerIsFree(P(self->charaMgr)) == 0) {
        ShowMessage(self, 0x142);           // not while the player is in the middle of something
        return false;
    }
    if (!Carousel_Ready(&self->list)) {
        ShowMessage(self, 0x93);            // it is already
        return false;
    }
    Browse(self);
    if (FrameOf(self) != 0)
        StatusFrame_SetDurability(P(FrameOf(self)), *P<s32>(self->swords + (u32)slot * 0x100 + 0x60));
    return true;
}

// ── 0x192850: "Drop" ──────────────────────────────────────────────────────
void DropSword(SwordsPage* self) {
    if (self->noWorld == 1) {
        ShowMessage(self, 0x96);
        return;
    }
    const int slot = Carousel_Remove(&self->list);
    if (slot == -2) {
        ShowMessage(self, 0x94);
    } else if (slot == -1) {
        ShowMessage(self, 0x95);
    } else {
        self->removed = slot;
        SetAction(self, 3, (s32)(self->swords + (u32)slot * 0x100));
        Browse(self);
    }
}

// ── 0x192690: the item brought from the items page, on the sword in front ─
void UseItemOnSword(SwordsPage* self) {
    IconSet* icons = P<IconSet>(P<u32>(self->menu)[6]);
    const ItemInfo* icon = IconSet_Get(icons, (u32)self->item);
    const s16 slot = SlotAt(&self->list, self->list.current);
    if (StatusItem_Use(icons, (u32)self->item, nullptr, slot) == 0) {
        SetHelp(self, P<const u16>(icons->_0c));
        Sound_PlaySE(7, 0, 0x3FFF);
        return;
    }
    Card* card = CardAt(&self->list, self->list.current);
    Card_SetTexts(card);
    const u8 effects = reinterpret_cast<const u8*>(icon)[1];
    const int first = effects & 0xF, second = (effects >> 4) & 0xF;
    if (first == 6 || second == 6) {
        card->flash = 1;
        card->flashTimer = 0;
    }
    if (first == 7 || second == 7) {
        card->flash = 2;
        card->flashTimer = 0;
    }
    Sound_PlaySE(first == 8 ? 0x27 : 0x1B, 0, 0x3FFF);
    if (P<u32>(self->menu)[3] != 0)
        ItemsPage_Consume(P(P<u32>(self->menu)[3]));
    SetHelp(self, P<const u16>(icons->_0c));
    SetState(self, kUsed);
    self->usedTimer = 0;
}

// ── 0x193840 ──────────────────────────────────────────────────────────────
void State_Browse(SwordsPage* self) {
    const u16 pressed = Pad_GetPressed(P(self->padMgr), 0) | Pad_GetRepeat(P(self->padMgr), 0);
    if (pressed & kPadTriangle) {
        if (self->choosing == 0) {
            SetAction(self, 2);
            Sound_PlaySE(7, 0, 0x3FFF);
        } else {
            SetAction(self, 8);             // back to the items page
        }
        return;
    }
    if ((pressed & kPadStart) && self->choosing == 0) {
        SetAction(self, 1);
        return;
    }
    if (pressed & kPadDown)
        Carousel_Ask(&self->list, 2);
    else if (pressed & kPadUp)
        Carousel_Ask(&self->list, 1);
    if (pressed & kPadCross)
        Carousel_Confirm(&self->list);
    if (self->list.sliding == 0 && self->list.confirm != 0) {
        self->list.confirm = 0;
        if (self->choosing == 0) {
            Sound_PlaySE(1, 0, 0x3FFF);
            OpenMenu(self, P<s32>(self->menu)[10] != 0);
        } else {
            UseItemOnSword(self);
        }
    }
}
// ── 0x193620: Ready / Drop / Details ──────────────────────────────────────
void State_Menu(SwordsPage* self) {
    const u16 pressed = Pad_GetPressed(P(self->padMgr), 0);
    if (pressed & kPadTriangle) {
        Browse(self);
        Sound_PlaySE(7, 0, 0x3FFF);
        return;
    }
    if ((pressed & kPadCross) == 0)
        return;
    const int choice = WindowMgr_GetCursor(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
    if (choice == 2) {
        Sound_PlaySE(5, 0, 0x3FFF);
        SetAction(self, 4, SlotAt(&self->list, self->list.current));
        Browse(self);
    } else if (choice == 1) {
        Sound_PlaySE(5, 0, 0x3FFF);
        DropSword(self);
    } else if (choice == 0 && ReadySword(self)) {
        Sound_PlaySE(5, 0, 0x3FFF);
        Sound_PlaySE(0x1B, 0, 0x3FFF);
    }
}
// ── 0x193390: Give / Details / Cancel ─────────────────────────────────────
void State_Give(SwordsPage* self) {
    const u16 pressed = Pad_GetPressed(P(self->padMgr), 0);
    if (pressed & kPadTriangle) {
        Browse(self);
        Sound_PlaySE(7, 0, 0x3FFF);
        return;
    }
    if ((pressed & kPadCross) == 0)
        return;
    const int choice = WindowMgr_GetCursor(P<WindowMgr>(self->windowMgr), P<Window>(self->window));
    const s16 slot = SlotAt(&self->list, self->list.current);
    if (choice == 2) {
        Sound_PlaySE(7, 0, 0x3FFF);
        Browse(self);
    } else if (choice == 1) {
        Sound_PlaySE(5, 0, 0x3FFF);
        SetAction(self, 4, slot);
        Browse(self);
    } else if (choice == 0) {
        Sound_PlaySE(5, 0, 0x3FFF);
        SetAction(self, 6, slot);
        CloseWindow(self);
    }
}
// ── 0x1932C0 ──────────────────────────────────────────────────────────────
void State_Message(SwordsPage* self) {
    if (Pad_GetPressed(P(self->padMgr), 0) & (kPadTriangle | kPadCross)) {
        Browse(self);
        Sound_PlaySE(7, 0, 0x3FFF);
    }
}
// ── 0x1931E0: the item's effect shows for a moment, then back to the items ─
void State_Used(SwordsPage* self) {
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    if (CardAt(&self->list, self->list.current)->flash == 0 && (pressed & kPadTriangle))
        SetAction(self, 8);
    if ((s32)(*gFrameRate * 0x37) / 0x1E < ++self->usedTimer)
        SetAction(self, 8);
}

// ── 0x1939C0 (vtable +0x18) ───────────────────────────────────────────────
void SwordsPage_Update(SwordsPage* self) {
    if (self->loaded == 0) {
        if (self->list.loadState != 3)
            Carousel_Update(&self->list);
        if (self->list.loadState == 3)
            self->loaded = 1;
        return;
    }
    if (self->removed != -1) {
        PlayerData_RemoveSword(P<PlayerData>(self->playerData), self->removed);
        self->removed = -1;
    }
    Carousel_Update(&self->list);
    if (P<s32>(self->menu)[9] != 0)
        return;
    switch (self->state[2]) {
    case kBrowse:  State_Browse(self);  break;
    case kMenu:    State_Menu(self);    break;
    case kGive:    State_Give(self);    break;
    case kMessage: State_Message(self); break;
    case kUsed:    State_Used(self);    break;
    }
}

// 0x192660 (vtable +0x14)
void SwordsPage_Draw(SwordsPage* self) {
    if (self->loaded != 0)
        Carousel_Draw(&self->list);
}
// 0x193B40 (vtable +0x10)
void SwordsPage_Enter(SwordsPage* self) {
    self->choosing = 0;
    Carousel_Reset(&self->list);
    Browse(self);
}
// 0x193BF0 (vtable +0x0C)
char SwordsPage_Load(SwordsPage* self) {
    const u8* pd = P<u8>(self->playerData);
    Carousel_Begin(&self->list, *reinterpret_cast<const s16*>(pd + 0x178),
                   PlayerData_GetReadiedSword(P<PlayerData>(self->playerData)), self->swords,
                   reinterpret_cast<const s16*>(pd + 0x17A));
    return 1;
}
// 0x193C50 (vtable +0x08)
void* SwordsPage_Dtor(SwordsPage* self, int flag) {
    if (self == nullptr)
        return self;
    CloseWindow(self);
    Carousel_Dtor(&self->list);
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

GUEST_BIND(0x193C50, SwordsPage_Dtor);
GUEST_BIND(0x193BF0, SwordsPage_Load);
GUEST_BIND(0x193B40, SwordsPage_Enter);
GUEST_BIND(0x192660, SwordsPage_Draw);
GUEST_BIND(0x1939C0, SwordsPage_Update);

}  // namespace

// ═══ For the other screen that shows cards (sword_safe.cpp) ═══════════════
namespace swordcards {
// The sword pictures, for the Records screen (0x2A37B0, 0x2A3990, 0x2A3890,
// 0x2A3A70, 0x2A3720).  An image is the 8-byte entry of the table.
u32* ImageTable() { return Images(); }
void ImageDraw(void* image, void* params) { Image_Draw(static_cast<SwordImage*>(image), params); }
bool ImageLoaded(void* image) {
    const SwordImage* i = static_cast<const SwordImage*>(image);
    return i->sprites != 0 && SpriteSet_IsLoaded(P<SpriteSet>(i->sprites)) != 0;
}
void ImageFree(void* image) {
    SwordImage* i = static_cast<SwordImage*>(image);
    if (i->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(i->sprites), 1);
    i->sprites = 0;
}
void ImageFreeAll() {
    u32* table = Images();
    for (u32 k = 0; k < table[1]; k++)
        ImageFree(P<SwordImage>(table[0]) + k);
}
void HolderInit(void* holder) {
    memset(holder, 0, 0x138);
    *reinterpret_cast<u32*>(static_cast<u8*>(holder) + 0x134) = 0x2ED138u;
    if (*gColParams != 0) {
        BufferReader* r = &P<ParamFile>(*gColParams)->out;
        Rewind(r);
        Take(r, holder, sizeof(ColumnLayout));
        Holder_ReadMarks(holder, r);
    }
}
void HolderFree(void* holder) { Holder_FreeMarks(holder); }
void Mark(void* card) { Card_Mark(static_cast<Card*>(card)); }
void Unmark(void* card) { Card_Unmark(static_cast<Card*>(card)); }
bool Ready() { return *gColState == 2 && *gColSprites != 0; }
void BindSprites() {
    if (*gColSprites != 0)
        SpriteSet_Bind(P<SpriteSet>(*gColSprites));
}
void* NewCard(void* holder, u32 sword, u32 vtable) {
    void* mem = MemAlloc(sizeof(Card));
    if (mem == nullptr)
        return nullptr;
    Card* card = Card_Ctor(static_cast<Card*>(mem), static_cast<Carousel*>(holder), sword);
    card->vtable = vtable;
    return card;
}
void FreeCard(void* card) { Card_Dtor(static_cast<Card*>(card), 1); }
void SetTexts(void* card) { Card_SetTexts(static_cast<Card*>(card)); }
void Update(void* card) { Card_Update(static_cast<Card*>(card)); }
void Draw(void* card) { Card_Draw(static_cast<Card*>(card)); }
void DrawPicture(void* card) { Card_DrawPicture(static_cast<Card*>(card)); }
}  // namespace swordcards

extern "C" {

int WeaponColumn_Poll() { return Column_Poll(); }           // 0x28A1A0

// ── 0x193CE0 ──────────────────────────────────────────────────────────────
void* SwordsPage_Ctor(void* mem, StatusMenu* menu, void* infoBlock, u8 noWorld, s32 mode) {
    SwordsPage* self = static_cast<SwordsPage*>(mem);
    const u32* info = static_cast<const u32*>(infoBlock);
    memset(self, 0, sizeof *self);
    self->menu = guest::addr(menu);
    self->vtable = 0x2EB2B0u;
    Carousel_Ctor(&self->list);
    self->gsDevice = guest::addr(*gGsDevice);
    self->padMgr = guest::addr(PadMgr_Get());
    self->windowMgr = guest::addr(*gWindowMgr);
    self->playerData = guest::addr(*gPlayerData);
    self->removed = -1;
    self->noWorld = noWorld;
    self->mode = mode;
    self->swords = info[0];
    self->swordDb = info[5];
    self->charaMgr = info[8];
    return self;
}

// ── 0x193A80: come from the items page to use `item` on one of the swords ─
void SwordsPage_Select(void* page, s32 item) {
    SwordsPage* self = static_cast<SwordsPage*>(page);
    self->item = item;
    self->choosing = 1;
    Browse(self);
    if (FrameOf(self) != 0) {
        StatusFrame_ShowArrows(P(FrameOf(self)), 0);
        StatusFrame_SetHints(P(FrameOf(self)), 2);
    }
}

}  // extern "C"
