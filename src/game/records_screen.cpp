// The Records screen (the title menu's "Records"; GameScene mode 4;
// 0x2A6B80–0x2AAD00 with the accessory text list at 0x2BC1D0–0x2BC490): what
// all the games played so far have left behind.
//
// One pack, asset 100 (menu/RecodeScreen/Data): three sprite sheets (offsets
// at +0x10, +0x18, +0x20: the list pages', the first page's, the frame's)
// and the first page's numbers as text (+0x28, size +0x2C).  Four pages,
// Left / Right turn them with a 12-frame cross-fade while the page names
// slide along the top; Triangle leaves.
//   0  the records: best samurai rate, score, games played, kills, instant
//      kills — and the endings seen, as the pieces of a picture
//   1  the thirty titles, each with how often it was earned
//   2  the swords found (of the database's 70), ten rows of name and kind,
//      Up / Down, with the picture of the one under the cursor
//   3  the accessories found (28), the same with their icons and where they
//      are worn
//
// Not reproduced: the accessory text list also loads the accessory database
// (0x2AD710) before its file; nothing here reads it.

#include "engine/bgm.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/pad_repeat.h"
#include "engine/param_file.h"
#include "game/item_select.h"
#include "game/player_data.h"
#include "game/sword.h"
#include "game/sword_cards.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>

struct SpriteSet;

extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    void  Gs_FillRect(const s32* rect, u32 z, u32 rgba);                // 0x225CD0  {x, y, w, h} in 1/16 px
    void  GsDevice_DrawTexturedPrim(void* dev, const void* packet, int count);  // 0x12FAF0
    SpriteSet* SpriteSet_Ctor(void* mem);                               // 0x17C630
    char  SpriteSet_ParseSized(SpriteSet* set, const void* data, u32 size);     // 0x17C230 (+ the size, natively)
    int   SpriteSet_Bind(SpriteSet* set);                               // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);                     // 0x17C570
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    void  SpriteSet_DrawEx(SpriteSet* set, int index, const void* params);             // 0x17ADF0
    void  SpriteSet_DrawRotated(SpriteSet* set, int index, const void* params);        // 0x179A40
    void* SpriteSet_GetSprite(SpriteSet* set, int index);               // 0x17BDF0
    void* AssetRef_Ctor(void* mem);                                     // 0x174C80
    void  AssetRef_Dtor(void* ref, int flag);                           // 0x174BE0
    void* AssetRef_GetData(void* ref);                                  // 0x174A00
    char  AssetQueue_Request(void* ref, int assetId);                   // 0x174C90
    void* SwordDb_fn15d0d0(void* db, u32 type);                         // 0x15D0D0  a sword's entry
    int   ItemIcon_Load(void* icon);                                    // 0x2342A0  → loaded
    void  ItemIcon_Unload(void* icon);                                  // 0x234350
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
const GuestPtrVar<Font>       gFont{0x2EE67Cu};
const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);

const u32 kZ = 0xFFFF82u;
const u32 kInk = 0x100E0Fu;

inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
inline u8* Pd() { return reinterpret_cast<u8*>(*gPlayerData); }
inline u32 OriginX() { return Screen_X(0, -1) & 0xFFFF; }
inline u32 OriginY() { return Screen_Y(0, -1) & 0xFFFF; }
inline u32 Alpha(float a) { return (u32)(s32)(a * 128.0f); }
inline u32 TextWidth(const u16* text) {                         // 0x225D70 (1/16 px)
    if (text == nullptr)
        return 0;
    Font_Measure(*gFont, text, 1.0f);
    return Font_GetExtent(*gFont)->w;
}
// "<number><unit>" in a scratch buffer.
const u16* Counted(u16* out, const char* number, const u16* unit, const char* tail = nullptr) {
    u16* at = out;
    for (const char* c = number; *c; c++)
        *at++ = (u16)*c;
    if (unit != nullptr)
        for (; *unit; unit++)
            *at++ = *unit;
    if (tail != nullptr)
        for (const char* c = tail; *c; c++)
            *at++ = (u16)*c;
    *at = 0;
    return out;
}
// The round stamp a count is written on, with the count: "seen/all".
void DrawCount(SpriteSet* set, u32 x, u32 y, u32 rgba, int seen, int all) {
    struct { u32 flags; s32 x, y; u32 z; u32 rgba; float scaleX, scaleY, angle; } stamp{};
    stamp.x = (s32)x;
    stamp.y = (s32)y;
    stamp.z = kZ;
    stamp.rgba = rgba;
    stamp.scaleX = stamp.scaleY = 16.0f;
    SpriteSet_DrawRotated(set, 2, &stamp);
    if (seen < 0)
        return;
    GsDevice_End2D(*gGsDevice);
    char a[16], b[16];
    snprintf(a, sizeof a, "%d", seen);
    snprintf(b, sizeof b, "%d", all);
    u16 text[40];
    Font_SetColor(*gFont, (rgba & 0xFF000000u) + kInk);
    Font_DrawCentered(*gFont, Counted(text, a, Text(2), b), (int)x, (int)y, kZ, 1.0f, 0.0f);
    GsDevice_Begin2D(*gGsDevice);
}

// ═══ The frame and the page names (0x2AA370–0x2AAD00; 0x24 bytes) ═════════
struct Tabs {
    s32 page;               // +0x00
    s32 next;               // +0x04
    s32 turning;            // +0x08  1 to the right, 2 to the left
    s32 timer;              // +0x0C
    u32 name[4];            // +0x10  text 0x110–0x113
    u32 sprites;            // +0x20
};
static_assert(sizeof(Tabs) == 0x24, "Tabs");

void Tabs_Ctor(Tabs* t) {                                       // 0x2AAD00
    t->page = 0;
    t->next = -1;
    t->turning = 0;
    t->timer = 0;
    t->sprites = 0;
    for (int i = 0; i < 4; i++) {
        const u16* name = Text(0x110 + i);
        t->name[i] = name != nullptr ? guest::addr(name) : 0;
    }
}
// 0x2AAB50 → 1 while a page is turning.
int Tabs_Update(Tabs* t) {
    if (t->turning != 0) {
        if (++t->timer <= *gFrameRate * 0xC / 0x1E)
            return 1;
        t->turning = 0;
        t->timer = 0;
        t->page = t->next;
        t->next = 0;
    }
    const u32 pad = (u32)Pad_GetPressed(PadMgr_Get(), 0) | Pad_GetRepeat(PadMgr_Get(), 0);
    if (pad & kPadRight) {
        t->turning = 1;
        t->timer = 0;
        t->next = t->page + 1 > 3 ? 0 : t->page + 1;
    } else if (pad & kPadLeft) {
        t->turning = 2;
        t->timer = 0;
        t->next = t->page - 1 < 0 ? 3 : t->page - 1;
    }
    return 0;
}
// 0x2AA370: the page's name in the middle, its neighbours to either side;
// while a page turns they all slide one place along.
void Tabs_DrawNames(const Tabs* t) {
    const u32 ox = OriginX(), oy = OriginY();
    Font* font = *gFont;
    const int cur = t->page;
    const int prev = cur - 1 < 0 ? cur + 3 : cur - 1, prev2 = cur - 2 < 0 ? cur + 2 : cur - 2;
    const int next = cur + 1 > 3 ? cur - 3 : cur + 1, next2 = cur + 2 > 3 ? cur - 2 : cur + 2;
    const auto name = [t](int i) { return P<const u16>(t->name[i]); };
    const float turned = (float)t->timer / ((float)*gFrameRate * 12.0f / 30.0f);
    const s32 hc = (s32)(TextWidth(name(cur)) & 0xFFFF) >> 1;
    const s32 hp = (s32)(TextWidth(name(prev)) & 0xFFFF) >> 1, hp2 = (s32)(TextWidth(name(prev2)) & 0xFFFF) >> 1;
    const s32 hn = (s32)(TextWidth(name(next)) & 0xFFFF) >> 1, hn2 = (s32)(TextWidth(name(next2)) & 0xFFFF) >> 1;
    Font_SetColor(font, 0x3E161616u);
    u32 x;
    if (t->turning == 2) {
        const s32 from = 0x13E8 - hc - hp;
        x = (u32)(hp + hc) + ox + ((u32)(s32)((float)from + turned * (float)(0x1518 - from)) & 0xFFFF) + 0x130;
    } else if (t->turning == 1) {
        const s32 from = hc + 0x1648 + hn;
        x = ox + ((u32)(s32)((float)from + turned * (float)(0x1518 - from)) & 0xFFFF) - (u32)hc - (u32)hn - 0x130;
    } else {
        x = ox + 0x1518;
    }
    x &= 0xFFFF;
    if (t->turning == 0)
        Font_DrawCenteredPlain(font, name(cur), (int)(x + 0x20), (int)(oy + 0x420), kZ);
    Font_SetColor(font, 0x7FBCBEC8u);
    const int y = (int)(oy + 0x400);
    Font_DrawCenteredPlain(font, name(cur), (int)x, y, kZ);
    const u32 right = (x + ((u32)(hc + hn + 0x130) & 0xFFFF)) & 0xFFFF;
    Font_DrawCenteredPlain(font, name(next), (int)right, y, kZ);
    Font_DrawCenteredPlain(font, name(next2), (int)((right + ((u32)(hn + hn2 + 0x130) & 0xFFFF)) & 0xFFFF), y, kZ);
    const u32 left = (x - ((u32)(hc + hp + 0x130) & 0xFFFF)) & 0xFFFF;
    Font_DrawCenteredPlain(font, name(prev), (int)left, y, kZ);
    Font_DrawCenteredPlain(font, name(prev2), (int)((left - ((u32)(hp + hp2 + 0x130) & 0xFFFF)) & 0xFFFF), y, kZ);
}
// 0x2259A0: a sprite with a colour for each corner (top left, bottom left, top right, bottom right).
void DrawShaded(SpriteSet* set, int index, s32 x, s32 y, const u32 rgba[4]) {
    const u32* fields = reinterpret_cast<const u32*>(set);
    const u8* descriptor = P<const u8>(fields[2]);
    const u8* record = static_cast<const u8*>(SpriteSet_GetSprite(set, index));
    if (fields[2] == 0 || record == nullptr)
        return;
    descriptor += record[0x19] * 0x30;
    const auto rd16 = [](const u8* p) { u16 v; memcpy(&v, p, 2); return v; };
    const u64 tex0 = (u64)rd16(descriptor + 10) << 30 | (u64)rd16(descriptor + 8) << 26 | (u64)descriptor[2] << 20
                   | (u64)rd16(descriptor + 4) | (u64)rd16(descriptor + 6) << 14 | 0x400000000ull
                   | (u64)(u32)(rd16(descriptor + 0x1E) + record[0x18]) << 37 | 0x2000000000000000ull;
    const s16 u0 = (s16)(rd16(record + 8) << 4), v0 = (s16)(rd16(record + 10) << 4);
    const s16 u1 = (s16)((rd16(record + 8) + rd16(record + 12)) * 0x10), v1 = (s16)((rd16(record + 10) + rd16(record + 14)) * 0x10);
    const s16 w = (s16)(rd16(record + 12) << 4), h = (s16)(rd16(record + 14) << 4);
    struct {
        u64 tex0, prim;
        struct { s16 u, v; u32 _4; u32 rgba; float q; u16 x, y; u32 z; } v[4];
    } packet;
    memset(&packet, 0, sizeof packet);
    packet.tex0 = tex0;
    packet.prim = 0x15D;                                        // a shaded, blended, textured fan
    const s16 us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
    const s32 xs[4] = {x, x + w, x + w, x}, ys[4] = {y, y, y + h, y + h};
    const u32 colour[4] = {rgba[0], rgba[2], rgba[3], rgba[1]};
    for (int i = 0; i < 4; i++) {
        packet.v[i].u = us[i];
        packet.v[i].v = vs[i];
        packet.v[i].rgba = colour[i];
        packet.v[i].q = 1.0f;
        packet.v[i].x = (u16)xs[i];
        packet.v[i].y = (u16)ys[i];
        packet.v[i].z = kZ;
    }
    GsDevice_DrawTexturedPrim(*gGsDevice, &packet, 4);
}
// 0x2AA8E0: the background, the names, and over them the two ends of the
// bar they slide along (each fading out towards the middle) and the arrows.
void Tabs_Draw(const Tabs* t) {
    SpriteSet* set = P<SpriteSet>(t->sprites);
    SpriteSet_Bind(set);
    const u32 ox = OriginX(), oy = OriginY();
    SpriteSet_Draw(set, 0, (int)ox, (int)oy, kZ, 0x80808080u);
    Tabs_DrawNames(t);
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } q = {1, (s32)ox, (s32)oy + 0x2E0, kZ, -1, -1, 0x80808080u};
    SpriteSet_DrawEx(set, 1, &q);
    static const u32 kFadeRight[4] = {0x80808080u, 0x80808080u, 0x808080u, 0x808080u};
    static const u32 kFadeLeft[4] = {0x808080u, 0x808080u, 0x80808080u, 0x80808080u};
    DrawShaded(set, 2, (s32)ox + 0xD10, (s32)oy + 0x2E0, kFadeRight);
    q.x = (s32)ox + 0x1D20;
    q.w = q.h = -1;
    SpriteSet_DrawEx(set, 4, &q);
    DrawShaded(set, 3, (s32)ox + 0x1A00, (s32)oy + 0x2E0, kFadeLeft);
    q = {1, (s32)ox + 0xBA0, (s32)oy + 0x390, kZ, -1, -1, 0x80808080u};
    SpriteSet_DrawEx(set, 5, &q);
    q = {0x20, (s32)ox + 0x1D90, (s32)oy + 0x390, kZ, -1, -1, 0x80808080u};
    SpriteSet_DrawEx(set, 5, &q);
}
void Tabs_DrawHint() {                                          // 0x2AA7F0
    GsDevice_End2D(*gGsDevice);
    Font_SetColor(*gFont, 0x7FBCBEC8u);
    Font_DrawString(*gFont, Text(0x11A), (int)(OriginX() + 0x2170), (int)(OriginY() + 0x18B0), kZ, 1.0f, 0.0f);
    GsDevice_Begin2D(*gGsDevice);
}

// ═══ Page 0: the records (0x2A9830–0x2AA320; 0xA4 bytes) ══════════════════
struct RecordsPage {
    s32 plateX, plateY;     // +0x00
    s32 endingsX, endingsY; // +0x08  the word "Endings"
    s32 labelX, valueX;     // +0x10  the five lines: left edge, right edge
    s32 rowY[5];            // +0x18
    s32 piece[14][2];       // +0x2C  the endings' pieces
    u32 sprites;            // +0x9C
    u32 paramFile;          // +0xA0
};
static_assert(sizeof(RecordsPage) == 0xA4 && offsetof(RecordsPage, sprites) == 0x9C, "RecordsPage");

void RecordsPage_DrawLines(const RecordsPage* p, u32 alpha) {   // 0x2A9830
    Font* font = *gFont;
    const u32 ox = OriginX(), oy = OriginY();
    Font_SetColor(font, alpha * 0x1000000u + kInk);
    const int labelX = (int)((ox + (u32)p->labelX * 0x10) & 0xFFFF);
    const auto rowY = [&](int i) { return (int)((oy + (u32)p->rowY[i] * 0x10) & 0xFFFF); };
    for (int i = 0; i < 4; i++)
        Font_DrawStringPlain(font, Text(0x114 + i), labelX, rowY(i), kZ);
    Font_DrawString(font, Text(0x118), labelX, rowY(4), kZ, 0.96f, 0.0f);
    u16 text[48];
    char number[16];
    const auto value = [&](int row, const u16* string) {
        Font_DrawStringPlain(font, string, (int)((ox + (u32)p->valueX * 0x10 - (TextWidth(string) & 0xFFFF)) & 0xFFFF),
                             rowY(row), kZ);
    };
    float best;
    memcpy(&best, Pd() + 0x18, 4);
    const s32 rate = ((s32)(best * 100.0f) + 5) / 10;           // 0x273FD0
    snprintf(number, sizeof number, "%3d.%1d", rate / 10, rate % 10);
    value(0, Counted(text, number, Text(0xD)));
    const struct { u32 at; int unit; } kRows[4] = {{0x08, 0xE}, {0x04, 7}, {0x0C, 6}, {0x10, 6}};
    for (int i = 0; i < 4; i++) {
        s32 n;
        memcpy(&n, Pd() + kRows[i].at, 4);
        snprintf(number, sizeof number, "%d", n);
        value(i + 1, Counted(text, number, Text(kRows[i].unit)));
    }
}
void RecordsPage_Draw(const RecordsPage* p, float alpha) {      // 0x2A9F10
    SpriteSet* set = P<SpriteSet>(p->sprites);
    SpriteSet_Bind(set);
    const u32 ox = OriginX(), oy = OriginY();
    const u32 a = Alpha(alpha);
    const u32 rgba = a << 24 | 0x808080;
    SpriteSet_Draw(set, 0, (int)((ox + (u32)p->plateX * 0x10) & 0xFFFF), (int)((oy + (u32)p->plateY * 0x10) & 0xFFFF), kZ, rgba);
    RecordsPage_DrawLines(p, a & 0xFF);
    // "Endings", a letter at a time, 14 px apart.
    if (const u16* word = Text(0x119)) {
        u32 x = ox + (u32)p->endingsX * 0x10;
        for (; *word; word++) {
            const u16 letter[2] = {*word, 0};
            Font_DrawStringPlain(*gFont, letter, (int)(x & 0xFFFF), (int)((oy + (u32)p->endingsY * 0x10) & 0xFFFF), kZ);
            x = (x & 0xFFFF) + 0xE0;
        }
    }
    for (int i = 0; i < 14; i++)
        if (Pd()[0x1C + i] != 0)
            SpriteSet_Draw(set, i + 1, (int)((ox + (u32)p->piece[i][0] * 0x10) & 0xFFFF),
                           (int)((oy + (u32)p->piece[i][1] * 0x10) & 0xFFFF), kZ, rgba);
}

// ═══ Page 1: the titles (0x2A90C0–0x2A9810; 0x14 bytes) ═══════════════════
struct TitlesPage {
    u32 sprites;            // +0x00  → the list pages' sprite set pointer
    u32 file;               // +0x04  AssetRef*: menu/Title/TitleNameList
    u32 capacity;           // +0x08  the names: 25 characters each
    u32 count;              // +0x0C
    u32 names;              // +0x10
};
static_assert(sizeof(TitlesPage) == 0x14, "TitlesPage");

bool TitlesPage_Request(TitlesPage* p) {                        // 0x2A9730
    void* mem = MemAlloc(0xC);
    p->file = mem ? guest::addr(AssetRef_Ctor(mem)) : 0;
    return p->file != 0 && AssetQueue_Request(P(p->file), 99) != 0;
}
// 0x2A95F0 → 0 once the names are in (the call after they were read).
int TitlesPage_Poll(TitlesPage* p) {
    if (p->file == 0)
        return 0;
    if (const u8* data = static_cast<const u8*>(AssetRef_GetData(P(p->file)))) {
        s32 count;
        memcpy(&count, data, 4);
        if (count > 0) {
            p->names = guest::addr(MemAlloc((u32)count * 0x32));
            memcpy(P(p->names), data + 4, (size_t)count * 0x32);
            p->count = p->capacity = (u32)count;
        }
        AssetRef_Dtor(P(p->file), 1);
        p->file = 0;
    }
    return 1;
}
void TitlesPage_Dtor(TitlesPage* p) {                           // 0x2A9790
    if (p->file != 0)
        AssetRef_Dtor(P(p->file), 1);
    p->file = 0;
    if (p->names != 0)
        MemFree(P(p->names));
    p->names = 0;
}
void TitlesPage_Draw(const TitlesPage* p, float alpha) {        // 0x2A90C0
    SpriteSet* set = P<SpriteSet>(*P<u32>(p->sprites));
    Font* font = *gFont;
    SpriteSet_Bind(set);
    const u32 a = Alpha(alpha);
    const u32 rgba = a << 24 | 0x808080;
    const u32 ox = OriginX(), oy = OriginY();
    SpriteSet_Draw(set, 0, (int)ox, (int)(oy + 0x640), kZ, rgba);
    GsDevice_End2D(*gGsDevice);
    Font_SetColor(font, a * 0x1000000u + kInk);
    u32 title = 0;
    for (int column = 0; column < 3; column++) {
        const int x = (int)(ox + 2000 + (u32)column * 0xAF0);
        for (int row = 0; row < 10; row++, title++) {
            const s8 times = (s8)Pd()[0x2C + title];
            if (times == 0)
                continue;
            const int y = (int)((oy + 0x760 + (u32)row * 0x1C0) & 0xFFFF);
            if (title < p->count)
                Font_DrawCentered(font, P<const u16>(p->names + title * 0x32), x, y, kZ, 1.0f, 0.0f);
            char number[16];
            u16 text[16];
            snprintf(number, sizeof number, "%d", times);
            Font_DrawCentered(font, Counted(text, number, nullptr), x + 0x500, y, kZ, 1.0f, 0.0f);
        }
    }
    GsDevice_Begin2D(*gGsDevice);
    DrawCount(set, ox + 0x21C0, oy + 0x550, rgba, PlayerData_CountSeenA(*gPlayerData), 0x1E);
}

// ═══ The two lists (0x2A87E0–0x2A9040; base 0x68 bytes, vtable 0x2ED660) ══
struct ListBase {
    s32 count;              // +0x00
    s32 cursor;             // +0x04  row, 0–9
    s32 scroll;             // +0x08
    PadRepeatPair keys;     // +0x0C  up, down
    s16 glow[10];           // +0x50  each row's highlight, 0–0x80
    u32 vtable;             // +0x64
};
static_assert(sizeof(ListBase) == 0x68 && offsetof(ListBase, glow) == 0x50, "ListBase");

void ListBase_Ctor(ListBase* l, s32 count) {                    // (in 0x2A8710 / 0x2A7BC0)
    memset(l, 0, sizeof *l);
    l->vtable = 0x2ED660u;
    l->count = count;
    PadRepeatPair_Ctor(&l->keys);
    PadRepeatPair_SetMasks(&l->keys, kPadUp, kPadDown);
    PadRepeatPair_SetTiming(&l->keys, 10, 3, 7);
    l->glow[0] = 0x80;
}
// 0x2A8FA0: the highlights move with the rows when the list scrolls.
void ListBase_ShiftGlow(ListBase* l, int down) {
    if (down == 0) {
        for (int i = 0; i < 9; i++)
            l->glow[i] = l->glow[i + 1];
        l->glow[9] = 0;
    } else {
        for (int i = 9; i > 0; i--)
            l->glow[i] = l->glow[i - 1];
        l->glow[0] = 0;
    }
}
void ListBase_Update(ListBase* l) {                             // 0x2A8D50
    PadRepeat_Update(&l->keys.a);
    PadRepeat_Update(&l->keys.b);
    bool moved = true;
    if (!PadRepeat_IsHeld(&l->keys.a) && l->keys.b.fired != 0) {
        if (++l->cursor > 9) {
            l->cursor--;
            l->scroll++;
            if (l->scroll + l->cursor < l->count) {
                ListBase_ShiftGlow(l, 0);
            } else {
                moved = false;
                l->scroll--;
            }
        }
    } else if (!PadRepeat_IsHeld(&l->keys.b) && l->keys.a.fired != 0) {
        if (--l->cursor < 0) {
            l->cursor++;
            l->scroll--;
            if (l->scroll + l->cursor < 0) {
                moved = false;
                l->scroll++;
            } else {
                ListBase_ShiftGlow(l, 1);
            }
        }
    } else {
        moved = false;
    }
    if (moved)
        Sound_PlaySE(2, 0, 0x3FFF);
    const s16 step = (s16)(0x2D0 / *gFrameRate);
    for (int i = 0; i < 10; i++) {
        if (i == l->cursor) {
            l->glow[i] = (s16)(l->glow[i] + step);
            if (l->glow[i] > 0x80)
                l->glow[i] = 0x80;
        } else {
            l->glow[i] = (s16)(l->glow[i] - step);
            if (l->glow[i] < 0)
                l->glow[i] = 0;
        }
    }
}
// 0x2A87E0: the ruled table of ten rows and two columns.
void List_DrawRules(float alpha) {
    const u32 ox = OriginX();
    s32 y = (s32)OriginY() + 0x720;
    const s32 x = (s32)ox + 0x140;
    const u32 rgba = (u32)(s32)(alpha * 64.0f) << 24 | 0x232A37;
    for (int i = 0; i < 10; i++, y += 0x1C0) {
        if (i == 0) {
            const s32 top[4] = {x, y, 4672, 24};                // 0x2DB500 …
            Gs_FillRect(top, kZ, rgba);
        }
        const s32 left[4] = {x, y + 0x18, 48, 400};
        Gs_FillRect(left, kZ, rgba);
        const s32 right[4] = {(s32)ox + 0x1350, y + 0x18, 48, 400};
        Gs_FillRect(right, kZ, rgba);
        const s32 middle[4] = {(s32)ox + 0xCC8, y + 0x18, 48, 400};
        Gs_FillRect(middle, kZ, rgba);
        const s32 bottom[4] = {x, y + 0x1A8, 4672, 48};
        Gs_FillRect(bottom, kZ, rgba);
    }
}
// A row's two texts in ink; 0x2A8A00: and again, outlined and lit, by how
// much the row is highlighted.
void List_DrawRow(const ListBase* l, float alpha, const u16* a, const u16* b, int row, u32 x, u32 y) {
    Font* font = *gFont;
    const int ax = (int)((x + 0x5F0) & 0xFFFF), bx = (int)((x + 0xEF0) & 0xFFFF), ty = (int)((y + 0xF0) & 0xFFFF);
    GsDevice_End2D(*gGsDevice);
    Font_SetColor(font, Alpha(alpha) * 0x1000000u + kInk);
    if (a != nullptr)
        Font_DrawCentered(font, a, ax, ty, kZ, 1.0f, 0.0f);
    if (b != nullptr)
        Font_DrawCentered(font, b, bx, ty, kZ, 1.0f, 0.0f);
    if (l->glow[row] != 0) {
        const u32 lit = (u32)(s32)((float)l->glow[row] * alpha);
        const struct { const u16* text; int x; } both[2] = {{a, ax}, {b, bx}};
        for (const auto& t : both) {
            if (t.text == nullptr)
                continue;
            Font_SetColor(font, lit * 0x1000000u + 0xBCBEC8u);
            Font_DrawCentered(font, t.text, t.x + 0x20, ty, kZ, 1.0f, 0.0f);
            Font_DrawCentered(font, t.text, t.x - 0x20, ty, kZ, 1.0f, 0.0f);
            Font_DrawCentered(font, t.text, t.x, ty + 0x20, kZ, 1.0f, 0.0f);
            Font_DrawCentered(font, t.text, t.x, ty - 0x20, kZ, 1.0f, 0.0f);
            Font_SetColor(font, lit << 24 | 0x3A3767u);
            Font_DrawCentered(font, t.text, t.x, ty, kZ, 1.0f, 0.0f);
        }
    }
    GsDevice_Begin2D(*gGsDevice);
}
void List_DrawCursor(const ListBase* l, float alpha) {
    const s32 bar[4] = {(s32)OriginX() + 0x140, (s32)OriginY() + 0x720 + l->cursor * 0x1C0, 0x1240, 0x1C0};
    Gs_FillRect(bar, kZ, (u32)(s32)(alpha * 32.0f) << 24);
}

// ═══ Page 2: the swords found (0x2A7CA0–0x2A8710; 0x6C bytes) ═════════════
struct SwordsPage {
    ListBase list;
    u32 sprites;            // +0x68  → the list pages' sprite set pointer
};
// The order they are listed in is not the database's (inlined everywhere).
int SwordOfRow(int row) {
    int type = row + 1;
    if (type == 0x14) return 0x46;
    if (type == 0x13) return 0x45;
    if (type == 0x2B) return 0x42;
    if (type > 0x2A) type = row;
    if (type > 0x13) type--;
    if (type > 0x12) type--;
    if (type > 0x41) type++;
    return type;
}
inline s32 SwordCount() { return *reinterpret_cast<s32*>(reinterpret_cast<u8*>(SwordDb_Get()) + 0x8C) - 1; }
inline bool SwordSeen(int type) { return Pd()[0x4C + type] != 0; }
void* SwordPicture(int type) {
    const u8* record = static_cast<const u8*>(SwordDb_fn15d0d0(SwordDb_Get(), (u32)type));
    if (record == nullptr)
        return nullptr;
    u32 picture;
    memcpy(&picture, record + 0x78, 4);
    const u32* table = swordcards::ImageTable();
    return picture < table[1] ? P<u8>(table[0]) + picture * 8 : nullptr;
}
void SwordsPage_Draw(const SwordsPage* p, float alpha) {        // 0x2A7CA0
    const ListBase* l = &p->list;
    SpriteSet* set = P<SpriteSet>(*P<u32>(p->sprites));
    SpriteSet_Bind(set);
    const u32 ox = OriginX(), oy = OriginY();
    List_DrawRules(alpha);
    const u32 rgba = Alpha(alpha) << 24 | 0x808080;
    SpriteSet_Draw(set, 3, (int)(ox + 0x1460), (int)(oy + 0x970), kZ, rgba);
    const int chosen = SwordOfRow(l->scroll + l->cursor);
    void* picture = SwordPicture(chosen);
    if (SwordSeen(chosen) && picture != nullptr) {
        struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } q = {1, (s32)ox + 0x14E0, (s32)oy + 0x9D0, kZ, 0xD40, 0x4A0, rgba};
        swordcards::ImageDraw(picture, &q);
    }
    for (int i = 0; i < SwordCount(); i++) {                    // the other pictures are let go
        void* other = SwordPicture(SwordOfRow(i));
        if (other != nullptr && other != picture && swordcards::ImageLoaded(other))
            swordcards::ImageFree(other);
    }
    u32 y = oy + 0x720;
    for (int row = 0; row < 10; row++, y += 0x1C0) {
        const int type = SwordOfRow(l->scroll + row);
        if (!SwordSeen(type))
            continue;
        const u8* record = static_cast<const u8*>(SwordDb_fn15d0d0(SwordDb_Get(), (u32)type));
        if (record == nullptr)
            continue;
        s32 kind;
        memcpy(&kind, record + 0x74, 4);
        List_DrawRow(l, alpha, reinterpret_cast<const u16*>(record), Text(0x33 + kind), row, ox + 0x140, y & 0xFFFF);
    }
    List_DrawCursor(l, alpha);
    const s32 all = SwordCount();
    s32 seen = all;
    for (s32 i = 1; i <= all; i++)
        if (Pd()[0x4C + i] == 0)
            seen--;
    DrawCount(set, ox + 0x20A0, oy + 0x7C0, rgba, seen > 0 && seen <= all ? seen : -1, all);
}

// ═══ Page 3: the accessories found (0x2A7360–0x2A7BC0; 0x78 bytes) ════════
// With the list of where each is worn (0x2BC1D0–0x2BC490; asset 0x88,
// Accessory/AccessoryPos.Lst: 16-bit strings one after another).
struct WornList {           // 0x14 bytes
    u32 file;               // +0x00  AssetRef*
    u32 capacity;           // +0x04  the strings (here: pointers into a copy of the file)
    u32 count;              // +0x08
    u32 strings;            // +0x0C
    u32 text;               // +0x10  (the original: the accessory database; here the copy)
};
bool WornList_Poll(WornList* w) {                               // 0x2BC230
    if (w->count != 0)
        return true;
    if (w->file == 0) {
        void* mem = MemAlloc(0xC);
        w->file = mem ? guest::addr(AssetRef_Ctor(mem)) : 0;
        if (w->file != 0)
            AssetQueue_Request(P(w->file), 0x88);
    }
    const u16* data = w->file != 0 ? static_cast<const u16*>(AssetRef_GetData(P(w->file))) : nullptr;
    if (data == nullptr)
        return false;
    u32 strings = 0, length = 0;
    for (const u16* s = data + 1; *s != 0; strings++) {
        while (*s != 0)
            s++;
        s++;
        length = (u32)(s - data);
    }
    u16* copy = static_cast<u16*>(MemAlloc((length + 1) * 2));
    memcpy(copy, data, (length + 1) * 2);
    u32* list = static_cast<u32*>(MemAlloc((strings ? strings : 1) * 4));
    u32 n = 0;
    for (u16* s = copy + 1; n < strings; n++) {
        list[n] = guest::addr(s);
        while (*s != 0)
            s++;
        s++;
    }
    w->text = guest::addr(copy);
    w->strings = guest::addr(list);
    w->count = w->capacity = strings;
    return true;
}
const u16* WornList_At(const WornList* w, u32 index) {          // 0x2BC1D0
    if (w == nullptr || w->count == 0)
        return nullptr;
    if (index >= w->count)
        index = 0;
    return P<const u16>(P<u32>(w->strings)[index]);
}
void WornList_Dtor(WornList* w) {                               // 0x2BC3D0
    if (w->file != 0)
        AssetRef_Dtor(P(w->file), 1);
    if (w->strings != 0)
        MemFree(P(w->strings));
    if (w->text != 0)
        MemFree(P(w->text));
    MemFree(w);
}

struct AccessoriesPage {
    ListBase list;
    s32 load;               // +0x68  1 the icons, 2 the list of places, 3 ready
    u32 sprites;            // +0x6C  → the list pages' sprite set pointer
    u32 icons;              // +0x70  IconSet*
    u32 worn;               // +0x74  WornList*
};
static_assert(sizeof(AccessoriesPage) == 0x78, "AccessoriesPage");

bool AccessoriesPage_Start(AccessoriesPage* p) {                // 0x2A7A60
    if (p->icons != 0)
        IconSet_Dtor(P<IconSet>(p->icons), 1);
    p->icons = 0;
    if (p->worn != 0)
        WornList_Dtor(P<WornList>(p->worn));
    p->worn = 0;
    void* mem = MemAlloc(0x20);
    p->icons = mem ? guest::addr(IconSet_Ctor(static_cast<IconSet*>(mem))) : 0;
    if (p->icons == 0)
        return false;
    IconSet_Load(P<IconSet>(p->icons));
    p->load = 1;
    return true;
}
void AccessoriesPage_Poll(AccessoriesPage* p) {                 // 0x2A79C0
    if (p->load == 2) {
        if (WornList_Poll(P<WornList>(p->worn)))
            p->load = 3;
    } else if (p->load == 1 && IconSet_IsLoading(P<IconSet>(p->icons)) == 0) {
        WornList* w = static_cast<WornList*>(MemAlloc(sizeof(WornList)));
        memset(w, 0, sizeof *w);
        p->worn = guest::addr(w);
        p->load = 2;
    }
}
void AccessoriesPage_Dtor(AccessoriesPage* p) {                 // 0x2A7B00
    if (p->icons != 0)
        IconSet_Dtor(P<IconSet>(p->icons), 1);
    p->icons = 0;
    if (p->worn != 0)
        WornList_Dtor(P<WornList>(p->worn));
    p->worn = 0;
}
void AccessoriesPage_Draw(const AccessoriesPage* p, float alpha) {  // 0x2A7360
    const ListBase* l = &p->list;
    SpriteSet* set = P<SpriteSet>(*P<u32>(p->sprites));
    SpriteSet_Bind(set);
    const u32 ox = OriginX(), oy = OriginY();
    List_DrawRules(alpha);
    const u32 rgba = Alpha(alpha) << 24 | 0x808080;
    SpriteSet_Draw(set, 4, (int)(ox + 0x1540), (int)(oy + 0x890), kZ, rgba);
    IconSet* icons = P<IconSet>(p->icons);
    const u8* seen = Pd() + 0xCC;
    const int chosen = l->scroll + l->cursor;
    ItemInfo* icon = IconSet_Get(icons, (u32)chosen + 0x21);
    if (seen[chosen] != 0 && icon != nullptr) {
        struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } q = {1, (s32)ox + 0x1860, (s32)oy + 0x920, kZ, 0x630, 0x580, rgba};
        Icon_Draw(icon, &q);
    }
    for (int i = 0; i < 0x1C; i++) {                            // the other icons are let go
        if (i == chosen)
            continue;
        ItemInfo* other = IconSet_Get(icons, (u32)i + 0x21);
        if (other != nullptr && other->sprites != 0 && ItemIcon_Load(other) != 0)
            ItemIcon_Unload(other);
    }
    u32 y = oy + 0x720;
    for (int row = 0; row < 10; row++, y += 0x1C0) {
        const int which = l->scroll + row;
        if (seen[which] == 0)
            continue;
        const ItemInfo* info = IconSet_Get(icons, (u32)which + 0x21);
        if (info == nullptr)
            continue;
        List_DrawRow(l, alpha, P<const u16>(info->name), WornList_At(P<WornList>(p->worn), (u32)which), row,
                     ox + 0x140, y & 0xFFFF);
    }
    List_DrawCursor(l, alpha);
    DrawCount(set, ox + 0x20A0, oy + 0x7C0, rgba, PlayerData_CountAccessoriesSeen(*gPlayerData), 0x1C);
}

// ═══ The screen (0x2A6B80–0x2A72F0; 0x1D4 bytes) ══════════════════════════
struct Records {
    s32 phase;              // +0x000 1 the pack, 3 the titles' names, 4 the accessories, 5 ready
    Tabs tabs;              // +0x004
    RecordsPage records;    // +0x028
    TitlesPage titles;      // +0x0CC
    SwordsPage swords;      // +0x0E0
    AccessoriesPage accessories;    // +0x14C
    u32 listSprites;        // +0x1C4
    u32 recordSprites;      // +0x1C8
    u32 frameSprites;       // +0x1CC
    u32 pack;               // +0x1D0 AssetRef*
};
static_assert(offsetof(Records, records) == 0x28 && offsetof(Records, titles) == 0xCC &&
              offsetof(Records, swords) == 0xE0 && offsetof(Records, accessories) == 0x14C &&
              sizeof(Records) == 0x1D4, "Records");

u32 NewSpriteSet(const u8* pack, u32 at) {
    u32 offset, size;
    memcpy(&offset, pack + at, 4);
    memcpy(&size, pack + at + 4, 4);
    void* mem = MemAlloc(0x14);
    SpriteSet* set = mem ? SpriteSet_Ctor(mem) : nullptr;
    if (set != nullptr)
        SpriteSet_ParseSized(set, pack + offset, size);
    return set != nullptr ? guest::addr(set) : 0;
}
// 0x2A6E00 → false if something could not be read.
bool Records_Load(Records* self) {
    switch (self->phase) {
    case 1: {
        const u8* pack = static_cast<const u8*>(AssetRef_GetData(P(self->pack)));
        if (pack == nullptr)
            break;
        self->listSprites = NewSpriteSet(pack, 0x10);
        self->recordSprites = NewSpriteSet(pack, 0x18);
        self->records.sprites = self->recordSprites;
        self->frameSprites = NewSpriteSet(pack, 0x20);
        self->tabs.sprites = self->frameSprites;
        // 0x2AA200: the first page's numbers, as text inside the pack.
        u32 offset, size;
        memcpy(&offset, pack + 0x28, 4);
        memcpy(&size, pack + 0x2C, 4);
        ParamFile* numbers = ParamFile_Ctor(static_cast<ParamFile*>(MemAlloc(0x4C)));
        ParamFile_SetData(numbers, reinterpret_cast<const char*>(pack + offset), (int)size);
        if (numbers->out.remaining >= 0x9C)
            memcpy(&self->records, P(numbers->out.cur), 0x9C);
        ParamFile_Dtor(numbers, 1);
        self->phase = 3;
        if (!TitlesPage_Request(&self->titles))
            return false;
        break;
    }
    case 3:
        if (TitlesPage_Poll(&self->titles) == 0) {
            self->phase = 4;
            if (!AccessoriesPage_Start(&self->accessories))
                return false;
        }
        break;
    case 4:
        AccessoriesPage_Poll(&self->accessories);
        if (self->accessories.load == 3)
            self->phase = 5;
        break;
    }
    return true;
}
void Records_DrawPage(Records* self, int page, float alpha) {
    switch (page) {
    case 3: AccessoriesPage_Draw(&self->accessories, alpha); break;
    case 2: SwordsPage_Draw(&self->swords, alpha); break;
    case 1: TitlesPage_Draw(&self->titles, alpha); break;
    case 0: RecordsPage_Draw(&self->records, alpha); break;
    }
}
void Records_UpdateList(Records* self, int page) {
    if (page == 3)
        ListBase_Update(&self->accessories.list);
    else if (page == 2)
        ListBase_Update(&self->swords.list);
}

}  // namespace

extern "C" {

void* Mode4_Ctor(void* mem) {                                   // 0x2A72F0
    Records* self = static_cast<Records*>(mem);
    memset(self, 0, sizeof *self);
    Tabs_Ctor(&self->tabs);
    const u32 sprites = guest::addr(&self->listSprites);
    self->titles.sprites = sprites;
    ListBase_Ctor(&self->swords.list, 0);                       // 0x2A8710
    self->swords.list.vtable = 0x2ED650u;
    self->swords.sprites = sprites;
    ListBase_Ctor(&self->accessories.list, 0x1C);               // 0x2A7BC0
    self->accessories.list.vtable = 0x2ED640u;
    self->accessories.sprites = sprites;
    return self;
}
char Mode4_Load(void* obj) {                                    // 0x2A7150
    Records* self = static_cast<Records*>(obj);
    void* mem = MemAlloc(0xC);
    self->pack = mem ? guest::addr(AssetRef_Ctor(mem)) : 0;
    if (self->pack == 0 || AssetQueue_Request(P(self->pack), 100) == 0)
        return 0;
    self->phase = 1;
    self->swords.list.count = SwordCount();                     // 0x2A8630
    return 1;
}
// 0x2A6FC0 → non-zero to leave.
int Mode4_Update(void* obj) {
    Records* self = static_cast<Records*>(obj);
    if (self->phase != 5)
        return !Records_Load(self);
    if (Pad_GetPressed(PadMgr_Get(), 0) & kPadTriangle)
        return 1;
    if (Tabs_Update(&self->tabs) == 0) {
        Records_UpdateList(self, self->tabs.page);
    } else {
        const s32 t = self->tabs.timer < 0 ? -self->tabs.timer : self->tabs.timer;
        Records_UpdateList(self, (float)t / 12.0f < 0.5f ? self->tabs.page : self->tabs.next);
    }
    return 0;
}
void Mode4_Draw(void* obj) {                                    // 0x2A6B80
    Records* self = static_cast<Records*>(obj);
    if (self->phase != 5)
        return;
    GsDevice_Begin2D(*gGsDevice);
    Tabs_Draw(&self->tabs);
    if (self->tabs.turning == 0) {
        Records_DrawPage(self, self->tabs.page, 1.0f);
    } else {
        const s32 t = self->tabs.timer < 0 ? -self->tabs.timer : self->tabs.timer;
        const float turned = (float)t / 12.0f;
        Records_DrawPage(self, self->tabs.page, 1.0f - turned);
        Records_DrawPage(self, self->tabs.next, turned);
    }
    Tabs_DrawHint();
    GsDevice_End2D(*gGsDevice);
}
void Mode4_Dtor(void* obj, int flag) {                          // 0x2A7200
    Records* self = static_cast<Records*>(obj);
    if (self == nullptr)
        return;
    for (u32* set : {&self->listSprites, &self->recordSprites, &self->frameSprites}) {
        if (*set != 0)
            SpriteSet_Dtor(P<SpriteSet>(*set), 1);
        *set = 0;
    }
    if (self->pack != 0)
        AssetRef_Dtor(P(self->pack), 1);
    self->pack = 0;
    AccessoriesPage_Dtor(&self->accessories);
    swordcards::ImageFreeAll();                                 // (0x2A8670)
    TitlesPage_Dtor(&self->titles);
    if ((s16)flag > 0)
        MemFree(self);
}

}  // extern "C"
