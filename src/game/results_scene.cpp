// The Results scene (scene 25; 0x2A3090–0x2A3460, vtable 0x2ED5E0) and the
// evaluation screen it shows (0x270DE0–0x273FD0, with the title data at
// 0x2B3150–0x2B3A70): what a finished game — won, lost or abandoned — was
// worth.
//
// Sprites assets 0x5E (menu/EvaluationScreen/BG.bpm) and 0x5F (Main.bpm),
// numbers asset 0x60 (Param.tp: 0xC0 bytes of positions and delays), the
// titles asset 0x62 (menu/Title/TitleData).
//
// The screen has two pages.  The first is a tally that plays by itself
// (Cross hurries every step along):
//   the "samurai rate" counts up — (a bonus for the difficulty + a factor for
//   how far the story got × samurai points / evaluation points) × 100,
//   between 5 and 120;
//   the title earned appears — the first of the 30 titles whose conditions
//   hold (statistics of the game: money spent, people killed, the rate…);
//   its points (by difficulty, + what was added to health and vigour, + 50
//   for each of three rare items owned) run into the running total, which is
//   the score that unlocks difficulties, heads and clothes
//   (PlayerData+0x08, at most 99,999);
//   and for every threshold the total has passed a message says what has
//   been unlocked (text 0xFA + n; some come with an accessory).
// Cross then fades to the second page: ten statistics of the game.  Cross
// again leaves; the scene clears the game in progress, saves and goes back
// to the title.
//
// Kept host-side: the list of titles (the original parses it into a vector
// of 0x28-byte records with strings), and the ten statistics' strings are
// plain 16-bit buffers where the original has ref-counted strings.

#include "engine/bgm.h"
#include "engine/config.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/param_file.h"
#include "engine/scene.h"
#include "engine/text_label.h"
#include "game/player_data.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

struct SpriteSet;

extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    void* Scene_BaseCtor(void* mem);                                    // 0x1E25F0
    void* Scene_BaseDtor(void* mem, int flag);                          // 0x1E2580
    SpriteSet* SpriteSet_Ctor(void* mem);                               // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    char  SpriteSet_LoadByName(SpriteSet* set, const char* a, const char* b);   // 0x17C2F0
    int   SpriteSet_IsLoaded(SpriteSet* set);                           // 0x17C090
    int   SpriteSet_Bind(SpriteSet* set);                               // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);                     // 0x17C570
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    void  SpriteSet_DrawEx(SpriteSet* set, int index, const void* params);             // 0x17ADF0
    void* SpriteSet_GetSprite(SpriteSet* set, int index);               // 0x17BDF0
    void  GsDevice_SetBlend(void* dev, u64 alpha);                      // 0x130C50
    void* AssetRef_Ctor(void* mem);                                     // 0x174C80
    void  AssetRef_Dtor(void* ref, int flag);                           // 0x174BE0
    void* AssetRef_GetData(void* ref);                                  // 0x174A00
    char  AssetQueue_Request(void* ref, int assetId);                   // 0x174C90
    void  RenderMgr_TriggerEffect(void* mgr, u32 type, u32 arg1, u32 arg2);    // 0x14F190
    void  RenderMgr_UpdateEffects(void* mgr);                           // 0x14F690
    void  RenderMgr_Draw(void* mgr);                                    // 0x14F580
    void* SaveScreen_Ctor(void* mem, int mode);                         // 0x260050
    void  SaveScreen_Start(void* self);                                 // 0x25FF80
    int   SaveScreen_Update(void* self);                                // 0x25E550
    void  Mode0_Draw(void* self);                                       // 0x262470
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<GsDevice>   gGsDevice{0x2EE670u};
const GuestPtrVar<Font>       gFont{0x2EE67Cu};
const GuestPtrVar<void>       gRenderMgr{0x2EE684u};
const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);

const u32 kZ = 0xFFFF82u;

inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
inline u8* Pd() { return reinterpret_cast<u8*>(*gPlayerData); }
template <typename T> inline T& PdAt(u32 offset) { return *reinterpret_cast<T*>(Pd() + offset); }
inline s32 Frames(int n) { return *gFrameRate * n / 0x1E; }
inline bool CrossPressed() { return (Pad_GetPressed(PadMgr_Get(), 0) & kPadCross) != 0; }
inline s32 Ending() { return PdAt<s16>(0x868); }                // −1: the game was not finished
// The difficulty as the tables count it (3 = the flag at +0x104).
inline int Difficulty() { return PdAt<s8>(0x104) != 0 ? 3 : PdAt<s8>(0x105); }
// The screen's numbers are pixels from the top left corner.
inline u32 OriginX() { return Screen_X(0, -1) & 0xFFFF; }
inline u32 OriginY() { return Screen_Y(0, -1) & 0xFFFF; }
inline int At(u32 origin, s32 pixels) { return (int)((origin + (u32)pixels * 0x10) & 0xFFFF); }
inline u32 Alpha(float a) { return (u32)(s32)(a * 128.0f); }

// ═══ The titles (0x2B3150–0x2B3A70; object 0x1C bytes) ════════════════════
// menu/Title/TitleData: s32 count (30), s32 conditions each (4), then per
// title {u32 length, sprite sheet path (padded to 4), s32 points[4] by
// difficulty, conditions × {s16 what, s16 value}}.  A condition holds when
// statistic |what| is at least `value` (what > 0) or at most (what < 0).
struct TitleRec {
    std::string sheet;
    s32 points[4];
    std::vector<std::pair<s16, s16>> conditions;
};
struct Titles {
    u32 _00[3];             // (the original's vector of records)
    u32 file;               // +0x0C  AssetRef*
    s32 chosen;             // +0x10  (the original: the record's address)
    u32 sprites;            // +0x14  the chosen title's picture
    s32 rate;               // +0x18  the samurai rate × 10
};
static_assert(sizeof(Titles) == 0x1C, "Titles");
std::vector<TitleRec> sTitles;

void Titles_Parse(const u8* d) {                                // 0x2B3700
    sTitles.clear();
    s32 count, conditions;
    memcpy(&count, d, 4);
    memcpy(&conditions, d + 4, 4);
    const u8* p = d + 8;
    for (s32 i = 0; i < count; i++) {
        TitleRec rec;
        u32 length;
        memcpy(&length, p, 4);
        p += 4;
        rec.sheet.assign(reinterpret_cast<const char*>(p), length);
        while (!rec.sheet.empty() && rec.sheet.back() == 0)
            rec.sheet.pop_back();
        p += (length + 3) & ~3u;
        memcpy(rec.points, p, 16);
        p += 16;
        for (s32 k = 0; k < conditions; k++, p += 4) {
            s16 what, value;
            memcpy(&what, p, 2);
            memcpy(&value, p + 2, 2);
            rec.conditions.emplace_back(what, value);
        }
        sTitles.push_back(std::move(rec));
    }
}
// 0x2B3410
bool Titles_Holds(const Titles* self, s16 what, s16 value) {
    if (what == 0)
        return true;
    s32 stat = 0;
    switch (what < 0 ? -what : what) {
    case 1:  stat = PdAt<u16>(0x84E); break;                    // money spent
    case 2:  stat = PdAt<s16>(0x106); break;                    // money in hand
    case 3:  stat = PdAt<u16>(0x854); break;
    case 4:  stat = PdAt<u16>(0x850); break;
    case 5:  stat = PdAt<u16>(0x856); break;
    case 6:  stat = PdAt<u16>(0x858); break;
    case 7:  stat = PdAt<u16>(0x85A); break;
    case 8:  stat = PdAt<u16>(0x85C); break;
    case 9:  stat = PdAt<u16>(0x85E); break;
    case 10: stat = PdAt<u16>(0x844); break;
    case 11: stat = PdAt<u16>(0x860); break;                    // tempering gambles lost
    case 12: stat = PdAt<u16>(0x862); break;                    // swords broken at the forge
    case 13: stat = PdAt<u16>(0x866); break;
    case 14: stat = PdAt<u16>(0x842); break;
    case 15: stat = PdAt<u16>(0x840); break;
    case 16: stat = PdAt<u16>(0x848); break;
    case 17: stat = PdAt<u16>(0x84A); break;
    case 18: stat = self->rate / 10; break;
    case 19: stat = (Ending() >= 0 && Ending() <= 13) ? 1 : 0; break;   // 0x177400: the story reached an ending
    case 20: stat = PdAt<u16>(0x84C); break;                    // money earned
    case 21: stat = Difficulty(); break;
    }
    return what < 0 ? stat <= value : value <= stat;
}
// 0x2B35B0: the first title all of whose conditions hold (else the first of
// the list); how often each was earned is counted at PlayerData+0x2C.
void Titles_Choose(Titles* self) {
    size_t found = sTitles.size();
    for (size_t i = 0; i < sTitles.size() && found == sTitles.size(); i++) {
        bool all = true;
        for (const auto& c : sTitles[i].conditions)
            all = all && Titles_Holds(self, c.first, c.second);
        if (all)
            found = i;
    }
    if (found == sTitles.size())
        found = 0;
    self->chosen = (s32)found;
    s8& times = PdAt<s8>(0x2C + (found < 0x20 ? (u32)found : 0));
    if (PdAt<s8>(0x2C + (u32)found) < 99)
        times = (s8)(PdAt<s8>(0x2C + (u32)found) + 1);
}
// 0x2B3150: what the title is worth.
s32 Titles_Points(const Titles* self) {
    if (sTitles.empty())
        return 0;
    const TitleRec& rec = sTitles[(size_t)self->chosen];
    s32 points = 0;
    const s8 difficulty = PdAt<s8>(0x105);
    if (difficulty >= 0 && difficulty <= 2)
        points = rec.points[difficulty];
    if (PdAt<s8>(0x104) != 0)
        points = rec.points[3];
    // What the game added to the greatest health and vigour.
    if ((s32)PdAt<u16>(0x86A) < (s32)PdAt<s16>(0x110))
        points += ((s32)PdAt<s16>(0x110) - (s32)PdAt<u16>(0x86A)) / 5;
    if ((s32)PdAt<u16>(0x86C) < (s32)PdAt<s16>(0x10A))
        points += ((s32)PdAt<s16>(0x10A) - (s32)PdAt<u16>(0x86C)) * 0x14;
    static const u32 kRare[3] = {29, 31, 32};                   // 0x2DB8F0
    for (u32 item : kRare)
        if (PlayerData_HasItem(*gPlayerData, 1, item) != 0)
            points += 0x32;
    return points;
}
bool Titles_Request(Titles* self, s32 rate) {                   // 0x2B3970
    self->rate = rate;
    void* mem = MemAlloc(0xC);
    self->file = mem ? guest::addr(AssetRef_Ctor(mem)) : 0;
    return self->file != 0 && AssetQueue_Request(P(self->file), 0x62) != 0;
}
// 0x2B3330: once the file is in — the title, and its picture asked for.
bool Titles_Poll(Titles* self) {
    if (self->sprites != 0)
        return true;
    if (self->file == 0 || AssetRef_GetData(P(self->file)) == nullptr)
        return false;
    Titles_Parse(static_cast<const u8*>(AssetRef_GetData(P(self->file))));
    AssetRef_Dtor(P(self->file), 1);
    self->file = 0;
    Titles_Choose(self);
    void* mem = MemAlloc(0x14);
    self->sprites = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    if (!sTitles.empty()) {
        const std::string& sheet = sTitles[(size_t)self->chosen].sheet;
        char* name = static_cast<char*>(MemAlloc((u32)sheet.size() + 1));
        memcpy(name, sheet.c_str(), sheet.size() + 1);
        SpriteSet_LoadByName(P<SpriteSet>(self->sprites), name, nullptr);
        MemFree(name);
    }
    return true;
}
Titles* Titles_Dtor(Titles* self, int flag) {                   // 0x2B39E0
    if (self == nullptr)
        return nullptr;
    if (self->file != 0)
        AssetRef_Dtor(P(self->file), 1);
    self->file = 0;
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

// ═══ The first page: the tally (0x272DB0–0x273FD0; 0xE4 bytes) ════════════
struct Plate {              // a heading that lights up when its number is in
    u32 sprites;            // → the screen's sprite set pointer
    u32 at;                 // → {x, y}
    u32 flashAt;            // → {x, y}
    s32 plate, lit, flash;  // sprites
    s32 light;              // frames since it lit, to 7
    s32 flashed;            // … to 20: the flash peaks at 10
    u8  on;
    u8  _21[3];
};
struct Digits {
    s32 width[13];          // of the sprites 0–9, the point (10), the "%" (11) and 12
    s32 height;
    u8  loaded;
    u8  _39[3];
    u32 sprites;            // → the screen's sprite set pointer
};
struct Tally {
    s32 state;              // +0x00  0 wait, 1 rate counts, 2 wait, 3 title plate, 4 title appears,
                            //        5 points glow, 6 wait, 7 points run into the total, 8 done
    s32 titleLoad;          // +0x04  1 the title file, 2 its picture, 3 there
    s32 timer;              // +0x08
    float rate;             // +0x0C  shown
    float rateEarned;       // +0x10
    s32 titleAlpha;         // +0x14  0–0x80
    s32 points;             // +0x18  the title's
    s32 pointsLeft;         // +0x1C
    s32 total;              // +0x20  the score after
    s32 before;             // +0x24  … and before
    s32 shown;              // +0x28
    s32 glow;               // +0x2C  0–0x80
    Plate plate[3];         // +0x30  rate, title, total
    Digits digits;          // +0x9C
    u32 params;             // +0xDC
    u32 sprites;            // +0xE0  → the screen's sprite set pointer
};
static_assert(sizeof(Plate) == 0x24 && sizeof(Digits) == 0x40 && sizeof(Tally) == 0xE4, "Tally");

inline s32 Param(const Tally* t, u32 offset) { return *P<s32>(t->params + offset); }

// 0x273710: the samurai rate.
void Tally_WorkOutRate(Tally* t) {
    float samurai = (float)PdAt<s32>(0x124), evaluation = (float)PdAt<s32>(0x128);
    if (samurai == 0.0f)
        samurai = 1.0f;
    if (evaluation == 0.0f)
        evaluation = 1.0f;
    float story = P<float>(0x2DB020u)[PdAt<s8>(0x100)];          // by the day the game ended on
    switch (Ending()) {
    case 0: case 3: case 5: case 6: case 7:
        story = *P<float>(0x2EE398u);
        break;
    case 1: case 2: case 4: case 8: case 9: case 10: case 11: case 12: case 13:
        story = *P<float>(0x2EE39Cu);
        break;
    }
    const float rate = (P<float>(0x2DB050u)[Difficulty()] + story * (samurai / evaluation)) * 100.0f;
    t->rateEarned = rate > 120.0f ? 120.0f : rate < 5.0f ? 5.0f : rate;
    if (PdAt<float>(0x18) < t->rateEarned)
        PdAt<float>(0x18) = t->rateEarned;                       // the best ever
}
// 0x273840.  `sprites` → the screen's sprite set pointer, `params` → its numbers.
void Tally_Ctor(Tally* t, u32 sprites, u32 params) {
    static const s32 kPlates[3][5] = {      // {at, flash at (offsets into the numbers), plate, lit, flash}
        {0x08, 0x10, 0x0F, 0x10, 0x0E}, {0x20, 0x28, 0x12, 0x13, 0x11}, {0x50, 0x58, 0x15, 0x16, 0x14}};
    memset(t, 0, sizeof *t);
    for (int i = 0; i < 3; i++) {
        t->plate[i].sprites = sprites;
        t->plate[i].at = params + (u32)kPlates[i][0];
        t->plate[i].flashAt = params + (u32)kPlates[i][1];
        t->plate[i].plate = kPlates[i][2];
        t->plate[i].lit = kPlates[i][3];
        t->plate[i].flash = kPlates[i][4];
    }
    t->digits.sprites = sprites;
    t->before = t->shown = PdAt<s32>(0x08);
    t->params = params;
    t->sprites = sprites;
    Tally_WorkOutRate(t);
    // The game's two counts go into the totals of all games, and the ending is one seen.
    PdAt<s32>(0x0C) += PdAt<u16>(0x840);
    PdAt<s32>(0x10) += PdAt<u16>(0x842);
    if (Ending() != -1)
        PdAt<u8>(0x1C + (Ending() >= 0 && Ending() < 0x10 ? (u32)Ending() : 0)) = 1;
}
// 0x2731A0 → 1 once it is all there and Cross is pressed.
int Tally_Update(Tally* t) {
    t->timer++;
    for (Plate& p : t->plate) {
        if (!p.on)
            continue;
        if (p.light < Frames(7))
            p.light++;
        if (p.flashed < Frames(0x14))
            p.flashed++;
    }
    const bool cross = CrossPressed();
    switch (t->state) {
    case 0:
        if (Param(t, 0x78) < t->timer) {
            t->timer = 0;
            t->state = 1;
        }
        break;
    case 1:
        Sound_PlaySE(0x3D, 0, 0x3FFF);
        t->rate += 0.5f;
        if (t->rateEarned < t->rate || cross) {
            t->rate = t->rateEarned;
            t->plate[0].on = 1;
            t->state = 2;
            t->timer = 0;
            Sound_PlaySE(0x1B, 0, 0x3FFF);
        }
        break;
    case 2:
        if (Param(t, 0x7C) < t->timer || cross) {
            t->plate[1].on = 1;
            t->timer = 0;
            t->state = 3;
        }
        break;
    case 3:
        if (t->plate[1].flashed == Frames(0x14) || cross) {
            t->timer = 0;
            t->state = 4;
            Sound_PlaySE(0x1B, 0, 0x3FFF);
        }
        break;
    case 4:
        t->titleAlpha += 0xB4 / *gFrameRate;
        if (t->titleAlpha > 0x80 || cross) {
            t->titleAlpha = 0x80;
            t->timer = 0;
            t->state = 5;
            t->pointsLeft = t->points;
            Sound_PlaySE(0x1B, 0, 0x3FFF);
        }
        break;
    case 5:
        t->glow += 0x1E0 / *gFrameRate;
        if (t->glow > 0x80 || cross) {
            t->glow = 0x80;
            t->timer = 0;
            t->state = 6;
        }
        break;
    case 6:
        if (Param(t, 0x80) < t->timer || cross) {
            t->timer = 0;
            t->state = 7;
        }
        break;
    case 7:
        Sound_PlaySE(0x3D, 0, 0x3FFF);
        t->pointsLeft--;
        t->shown++;
        if (t->pointsLeft < 1 || cross) {
            t->pointsLeft = 0;
            t->shown = t->total;
            t->plate[2].on = 1;
            t->state = 8;
            t->timer = 0;
            Sound_PlaySE(0x1B, 0, 0x3FFF);
        }
        break;
    case 8:
        if (t->glow != 0) {
            t->glow -= 0x1E0 / *gFrameRate;
            if (t->glow <= 0)
                t->glow = 0;
        }
        if (cross)
            return 1;
        break;
    }
    return 0;
}
// 0x2739C0
void Plate_Draw(const Plate* p, float alpha, u32 ox, u32 oy, u32 shade) {
    SpriteSet* set = P<SpriteSet>(*P<u32>(p->sprites));
    const s32* flashAt = P<s32>(p->flashAt);
    const s32* at = P<s32>(p->at);
    GsDevice_SetBlend(*gGsDevice, 0x8000000048ull);
    const s32 peak = Frames(10);
    s32 off = peak - p->flashed;
    if (off < 0)
        off = -off;
    SpriteSet_Draw(set, p->flash, At(ox, flashAt[0]), At(oy, flashAt[1]), kZ,
                   (shade & 0xFF) * 0x10101 | Alpha(alpha * (1.0f - (float)off / (float)peak)) << 24);
    GsDevice_SetBlend(*gGsDevice, 0x8000000044ull);
    const int x = At(ox, at[0]), y = At(oy, at[1]);
    SpriteSet_Draw(set, p->plate, x, y, kZ, Alpha(alpha) << 24 | 0x808080);
    SpriteSet_Draw(set, p->lit, x, y, kZ, Alpha(alpha * (float)(p->light / Frames(7))) << 24 | 0x808080);
}
// 0x273CE0: a number in the screen's own digits, right-aligned at x.
// `spaced`: every digit takes its own width (else 30 px, so a number that
// is counting does not jitter); `tail`: a sprite after it (the "%"), or −1;
// `decimals`: digits after the point.
void Digits_Draw(Digits* d, float scale, s32 value, u32 x, u32 y, u32 rgba, bool spaced, int tail, int decimals) {
    SpriteSet* set = P<SpriteSet>(*P<u32>(d->sprites));
    if (!d->loaded) {
        for (int i = 0; i < 13; i++) {
            const u8* sprite = static_cast<const u8*>(SpriteSet_GetSprite(set, i));
            if (sprite == nullptr)
                return;
            u16 w, h;
            memcpy(&w, sprite + 0xC, 2);
            memcpy(&h, sprite + 0xE, 2);
            d->width[i] = w;
            if (i == 0)
                d->height = h;
        }
        d->loaded = 1;
    }
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } q;
    q.flags = 1;
    q.z = kZ;
    q.y = (s32)(y & 0xFFFF) + (s32)((float)d->height * scale) * -8;
    q.h = (s32)((float)d->height * scale) << 4;
    q.rgba = rgba;
    if (tail != -1) {
        q.w = (s32)(scale * (float)d->width[tail]) << 4;
        x = (x - ((u32)q.w & 0xFFFF)) & 0xFFFF;
        q.x = (s32)x;
        SpriteSet_DrawEx(set, tail, &q);
    }
    for (;;) {
        q.w = (s32)(scale * (float)d->width[value % 10]) << 4;
        const u32 step = spaced ? (u32)q.w : (u32)((s32)(scale * 30.0f) << 4);
        x = (x - (step & 0xFFFF)) & 0xFFFF;
        q.x = (s32)x;
        SpriteSet_DrawEx(set, value % 10, &q);
        if (value < 10)
            break;
        value /= 10;
        if (decimals == 1) {
            q.w = (s32)((float)d->width[10] * scale) << 4;
            x = (x - ((u32)q.w & 0xFFFF)) & 0xFFFF;
            q.x = (s32)x;
            SpriteSet_DrawEx(set, 10, &q);
        }
        decimals--;
    }
}
// 0x272DB0
void Tally_Draw(Tally* t, float alpha) {
    const u32 ox = OriginX(), oy = OriginY();
    const u32 a = Alpha(alpha);
    const u32 shade = *P<u8>(t->params + 0x88);
    for (const Plate& p : t->plate)
        Plate_Draw(&p, alpha, ox, oy, shade);
    Digits_Draw(&t->digits, 1.0f, ((s32)(t->rate * 100.0f) + 5) / 10, (u32)At(ox, Param(t, 0x18)),
                (u32)At(oy, Param(t, 0x1C)), a << 24 | 0x252539, t->state != 1, 0xB, 1);
    Digits_Draw(&t->digits, 0.5f, t->pointsLeft, (u32)At(ox, Param(t, 0x38)), (u32)At(oy, Param(t, 0x3C)),
                a << 24 | 0x1E202A, t->state != 7, -1, 0);
    Digits_Draw(&t->digits, 0.7f, t->shown, (u32)At(ox, Param(t, 0x60)), (u32)At(oy, Param(t, 0x64)),
                a << 24 | 0x252539, t->state != 7, -1, 0);
    SpriteSet* set = P<SpriteSet>(*P<u32>(t->sprites));
    SpriteSet_Draw(set, 0xC, At(ox, Param(t, 0x48)), At(oy, Param(t, 0x4C)), kZ, a << 24 | 0x808080);
    GsDevice_SetBlend(*gGsDevice, 0x8000000048ull);
    const u32 glow = (u32)(s32)((float)(a & 0xFF) / 128.0f * 0.32f * (float)t->glow);
    SpriteSet_Draw(set, 0x17, At(ox, Param(t, 0x40)), At(oy, Param(t, 0x44)), kZ, glow << 24 | 0x808080);
    GsDevice_SetBlend(*gGsDevice, 0x8000000044ull);
}

// ═══ The second page: ten statistics (0x272050, 0x272A00; 0xA8 bytes) ═════
struct Stats {
    u32 params;
    u32 sprites;            // → the screen's sprite set pointer
    u32 text[10][4];        // (the original: ref-counted strings; here word 0 → the characters)
};
static_assert(sizeof(Stats) == 0xA8, "Stats");

void Stats_Ctor(Stats* s, u32 sprites, u32 params) {            // 0x272A00
    memset(s, 0, sizeof *s);
    s->params = params;
    s->sprites = sprites;
    // {statistic, the text of its unit}: left column, right column, row by row
    const struct { s32 value; int unit; } kRows[10] = {
        {PdAt<u16>(0x840), 6}, {PdAt<u16>(0x84C), 5}, {PdAt<u16>(0x842), 6}, {PdAt<u16>(0x84E), 5},
        {PdAt<u16>(0x846), 8}, {PdAt<u16>(0x850), 9}, {PdAt<u16>(0x848), 7}, {PdAt<u16>(0x852), 10},
        {PdAt<u16>(0x844), 7}, {Ending() + 1, 0xB}};
    for (int i = 0; i < 10; i++) {
        if (i == 9 && Ending() == -1)
            break;
        char number[16];
        snprintf(number, sizeof number, "%d", kRows[i].value);
        const u16* unit = Text(kRows[i].unit);
        const size_t digits = strlen(number);
        const int unitLength = unit != nullptr ? WStr_Length(unit) : 0;
        u16* text = static_cast<u16*>(MemAlloc((u32)(digits + (size_t)(unitLength > 0 ? unitLength : 0) + 1) * 2));
        for (size_t k = 0; k < digits; k++)
            text[k] = (u16)number[k];
        text[digits] = 0;
        if (unitLength > 0)
            WStr_Copy(text + digits, unit);
        s->text[i][0] = guest::addr(text);
    }
}
void Stats_Dtor(Stats* s) {
    for (auto& text : s->text) {
        if (text[0] != 0)
            MemFree(P(text[0]));
        text[0] = 0;
    }
}
// 0x272050
void Stats_Draw(const Stats* s, float alpha) {
    Font* font = *gFont;
    const u32 ox = OriginX(), oy = OriginY();
    const u32 a = Alpha(alpha);
    const s32* p = P<s32>(s->params);
    const auto param = [p](u32 offset) { return p[offset / 4]; };
    SpriteSet_Draw(P<SpriteSet>(*P<u32>(s->sprites)), 0xD, At(ox, param(0x8C)), At(oy, param(0x90)), kZ,
                   a << 24 | 0x808080);
    Font_SetColor(font, a << 24 | 0x3C4157);
    static const u32 kRowY[5] = {0x94, 0x98, 0x9C, 0xA0, 0xA4};
    for (int i = 0; i < 10; i++)                                // the headings: text 0xF0–0xF9
        Font_DrawStringPlain(font, Text(0xF0 + i), At(ox, param(i % 2 ? 0xB0 : 0xA8)), At(oy, param(kRowY[i / 2])), kZ);
    for (int i = 0; i < 10; i++) {                              // the numbers, right-aligned
        if (i == 9 && Ending() == -1)
            break;
        const u16* text = P<const u16>(s->text[i][0]);
        if (s->text[i][0] == 0)
            continue;
        s32 size[2] = {0, 0};
        Text_MeasurePixels(text, size);
        Font_DrawStringPlain(font, text,
                             (int)(((u32)At(ox, param(i % 2 ? 0xB4 : 0xAC)) + (u32)(size[0] & 0xFFF) * (u32)-0x10) & 0xFFFF),
                             At(oy, param(kRowY[i / 2])), kZ);
    }
}

// ═══ The screen (0x270DE0–0x271F30; 0x288 bytes) ══════════════════════════
struct Eval {
    s32 page;               // +0x000 0 the tally, 1 turning over, 2 the statistics
    s32 phase;              // +0x004 loading: 1 sprites, 2 background, 3 numbers, 4 the title, 5 ready
    s32 timer;              // +0x008
    s32 params[0x30];       // +0x00C the numbers of Param.tp
    Tally tally;            // +0x0CC
    u32 titles;             // +0x1B0 Titles*
    Stats stats;            // +0x1B4
    u32 background;         // +0x25C SpriteSet*
    u32 sprites;            // +0x260 SpriteSet*
    u32 paramFile;          // +0x264
    s32 msgState;           // +0x268 0 / 1 a pair of lines coming in, 2 all said
    u8  msgBuilt;           // +0x26C
    u8  _26d[3];
    s32 msgFade;            // +0x270
    s32 _274;
    u32 msgCapacity;        // +0x278 the lines (a vector of text pointers)
    u32 msgCount;           // +0x27C
    u32 msgs;               // +0x280
    s32 msgFirst;           // +0x284 the first line showing
};
static_assert(offsetof(Eval, tally) == 0xCC && offsetof(Eval, titles) == 0x1B0 && offsetof(Eval, stats) == 0x1B4 &&
              offsetof(Eval, background) == 0x25C && sizeof(Eval) == 0x288, "Eval");

Eval* Eval_Ctor(void* mem) {                                    // 0x271F30
    Eval* self = static_cast<Eval*>(mem);
    memset(self, 0, sizeof *self);
    const u32 sprites = guest::addr(&self->sprites), params = guest::addr(self->params);
    Tally_Ctor(&self->tally, sprites, params);
    Stats_Ctor(&self->stats, sprites, params);
    return self;
}
Eval* Eval_Dtor(Eval* self, int flag) {                         // 0x271E20
    if (self == nullptr)
        return nullptr;
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    if (self->background != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->background), 1);
    self->background = 0;
    if (self->paramFile != 0)
        ParamFile_Dtor(P<ParamFile>(self->paramFile), 1);
    self->paramFile = 0;
    if (self->msgs != 0)
        MemFree(P(self->msgs));
    Stats_Dtor(&self->stats);
    if (self->titles != 0)
        Titles_Dtor(P<Titles>(self->titles), 1);
    self->titles = 0;
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}
bool Eval_Start(Eval* self) {                                   // 0x271D80
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    void* mem = MemAlloc(0x14);
    self->sprites = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    if (self->sprites == 0 || SpriteSet_Load(P<SpriteSet>(self->sprites), 0x5F, -1) == 0)
        return false;
    self->phase = 1;
    return true;
}
// 0x271650 → false if something could not be read.
bool Eval_Load(Eval* self) {
    Tally* t = &self->tally;
    switch (self->phase) {
    case 1:
        if (SpriteSet_IsLoaded(P<SpriteSet>(self->sprites)) != 0) {
            self->phase = 2;
            if (self->background != 0)
                SpriteSet_Dtor(P<SpriteSet>(self->background), 1);
            void* mem = MemAlloc(0x14);
            self->background = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
            if (self->background == 0 || SpriteSet_Load(P<SpriteSet>(self->background), 0x5E, -1) == 0)
                return false;
        }
        break;
    case 2:
        if (SpriteSet_IsLoaded(P<SpriteSet>(self->background)) != 0) {
            self->phase = 3;
            if (self->paramFile != 0)
                ParamFile_Dtor(P<ParamFile>(self->paramFile), 1);
            void* mem = MemAlloc(0x4C);
            self->paramFile = mem ? guest::addr(ParamFile_Ctor(static_cast<ParamFile*>(mem))) : 0;
            if (!ParamFile_Request(P<ParamFile>(self->paramFile), 0x60))
                return false;
        }
        break;
    case 3:
        if (ParamFile_Poll(P<ParamFile>(self->paramFile)) == 0) {
            self->phase = 4;
            void* mem = MemAlloc(sizeof(Titles));
            if (mem != nullptr)
                memset(mem, 0, sizeof(Titles));                 // 0x2B3A70
            self->titles = mem ? guest::addr(mem) : 0;
            if (self->titles == 0 || !Titles_Request(P<Titles>(self->titles), ((s32)(t->rateEarned * 100.0f) + 5) / 10))
                return false;
            t->titleLoad = 1;
            BufferReader* r = &P<ParamFile>(self->paramFile)->out;
            if (r->remaining >= sizeof self->params) {
                memcpy(self->params, P(r->cur), sizeof self->params);
                r->cur += sizeof self->params;
                r->remaining -= sizeof self->params;
            }
            ParamFile_Dtor(P<ParamFile>(self->paramFile), 1);
            self->paramFile = 0;
        }
        break;
    case 4:
        if (t->titleLoad == 1) {
            if (Titles_Poll(P<Titles>(self->titles))) {
                t->titleLoad = 2;
                t->points = Titles_Points(P<Titles>(self->titles));
                t->total = t->points + PdAt<s32>(0x08);
                if (t->total > 99999)
                    t->total = 99999;
                PdAt<s32>(0x08) = t->total;
            }
        } else if (t->titleLoad == 2) {
            if (SpriteSet_IsLoaded(P<SpriteSet>(P<Titles>(self->titles)->sprites)) != 0)
                t->titleLoad = 3;
        } else if (t->titleLoad == 3) {
            self->phase = 5;
        }
        break;
    }
    return true;
}
// 0x271B40: what comes with passing a threshold.
void Eval_Reward(int which) {
    static const struct { int which; u32 accessory; } kGifts[9] = {
        {7, 0x34}, {8, 0x35}, {0xB, 0x36}, {0xC, 0x37}, {0xF, 0x38}, {0x10, 0x39}, {0x11, 0x3A}, {0x13, 0x3B}, {0x14, 0x3C}};
    for (const auto& gift : kGifts)
        if (gift.which == which)
            PlayerData_AddAccessory(*gPlayerData, gift.accessory);
}
void Eval_AddLine(Eval* self, u32 text) {
    if (self->msgCount == self->msgCapacity) {
        const u32 capacity = self->msgCapacity ? self->msgCapacity * 2 : 1;
        u32* lines = static_cast<u32*>(MemAlloc(capacity * 4));
        if (self->msgs != 0) {
            memcpy(lines, P(self->msgs), self->msgCount * 4);
            MemFree(P(self->msgs));
        }
        self->msgs = guest::addr(lines);
        self->msgCapacity = capacity;
    }
    P<u32>(self->msgs)[self->msgCount++] = text;
}
// 0x271C50: a line for every threshold the score has passed with this game.
void Eval_BuildLines(Eval* self) {
    const s32* threshold = P<s32>(0x2DB060u);                   // 100, 300 … 10000
    for (int i = 0; i < 0x16; i++) {
        if (!(self->tally.before < threshold[i] && threshold[i] <= self->tally.total))
            continue;
        const u16* text = Text(0xFA + i);
        Eval_AddLine(self, text != nullptr ? guest::addr(text) : 0);
        if (i == 0x14)
            Eval_AddLine(self, 0);
        Eval_Reward(i);
    }
    if (self->msgCount == 0)
        self->msgState = 2;
}
// 0x2714B0: two lines show at a time; Cross brings the next two.
void Eval_UpdateLines(Eval* self) {
    const u32 pressed = Pad_GetPressed(PadMgr_Get(), 0), repeat = Pad_GetRepeat(PadMgr_Get(), 0);
    if (self->msgState == 2)
        return;
    if (++self->msgFade > Frames(0xF)) {
        self->msgFade = Frames(0xF);
        if (self->msgCount <= (u32)self->msgFirst + 2)
            self->msgState = 2;
    }
    if (((pressed | repeat) & kPadCross) == 0)
        return;
    self->_274 = 0;
    const u32 next = (u32)self->msgFirst + 2;
    if (next >= self->msgCount)
        return;
    self->msgFirst = (s32)next;
    if (P<u32>(self->msgs)[next] == 0) {
        if (next + 1 == self->msgCount) {
            self->msgState = 2;
            self->msgFirst -= 2;
        } else {
            self->msgFirst++;
            self->msgFade = 0;
            self->msgState = 1;
        }
    } else {
        self->msgFade = 0;
        self->msgState = 1;
    }
    Sound_PlaySE(2, 0, 0x3FFF);
}
// 0x2719E0 → true when the screen is done with (or could not load).
bool Eval_Update(Eval* self) {
    if (self->phase != 5)
        return !Eval_Load(self);
    if (self->page == 2) {
        if (CrossPressed())
            return true;
    } else if (self->page == 1) {
        if (Frames(0x23) < ++self->timer)
            self->page = 2;
    } else if (self->page == 0 && Tally_Update(&self->tally) != 0 && self->msgState == 2) {
        self->page = 1;
    }
    if (self->tally.state == 8) {
        if (!self->msgBuilt) {
            self->msgBuilt = 1;
            Eval_BuildLines(self);
        } else {
            Eval_UpdateLines(self);
        }
    }
    return false;
}
void Eval_DrawTitle(Eval* self, u32 ox, u32 oy, float alpha) {
    SpriteSet* set = P<SpriteSet>(P<Titles>(self->titles)->sprites);
    SpriteSet_Bind(set);
    SpriteSet_Draw(set, 0, At(ox, self->params[0x30 / 4]), At(oy, self->params[0x34 / 4]), kZ,
                   (u32)(s32)(alpha * (float)self->tally.titleAlpha) << 24 | 0x808080);
}
// 0x270DE0
void Eval_Draw(Eval* self) {
    if (self->phase != 5)
        return;
    const u32 ox = OriginX(), oy = OriginY();
    SpriteSet_Bind(P<SpriteSet>(self->background));
    SpriteSet_Draw(P<SpriteSet>(self->background), 0, (int)ox, (int)oy, kZ, 0x80808080u);
    SpriteSet_Bind(P<SpriteSet>(self->sprites));
    if (self->page == 2) {
        Stats_Draw(&self->stats, 1.0f);
    } else if (self->page == 1) {
        const float turned = (float)self->timer / (float)Frames(0x23);
        Tally_Draw(&self->tally, 1.0f - turned);
        Stats_Draw(&self->stats, turned);
        Eval_DrawTitle(self, ox, oy, 1.0f - turned);
    } else if (self->page == 0) {
        Tally_Draw(&self->tally, 1.0f);
        Eval_DrawTitle(self, ox, oy, 1.0f);
    }
    Font* font = *gFont;
    const u32 fade = (u32)(s32)((float)self->msgFade / (float)Frames(0xF) * 128.0f);
    for (int i = 0; i < 2; i++) {
        const u32 line = (u32)self->msgFirst + (u32)i;
        if (self->msgCount <= line)
            break;
        const int x = At(ox, self->params[(0x68 + i * 8) / 4]), y = At(oy, self->params[(0x6C + i * 8) / 4]);
        Font_SetColor(font, fade << 24 | 0xACB2BE);
        if (const u32 text = P<u32>(self->msgs)[line])
            Font_DrawString(font, P<const u16>(text), x, y, kZ, 0.88f, 0.0f);
        if (self->msgState == 1 && line >= 1) {                 // the line it replaces fades out under it
            Font_SetColor(font, (0x80 - (fade & 0xFF)) << 24 | 0xACB2BE);
            if (const u32 text = P<u32>(self->msgs)[line - 1])
                Font_DrawString(font, P<const u16>(text), x, y, kZ, 0.88f, 0.0f);
        }
    }
    const int x = At(ox, self->params[0xB8 / 4]), y = At(oy, self->params[0xBC / 4]);
    Font_SetColor(font, 0x7FBCBEC8u);
    Font_DrawString(font, Text(0x14), x, y, kZ, 1.0f, 0.0f);    // the button
    Font_SetColor(font, 0x7F3C4157u);
    Font_DrawString(font, Text(0xEF), (x + 0x120) & 0xFFFF, y, kZ, 1.0f, 0.0f);
}

// ═══ The scene (0x40 bytes) ═══════════════════════════════════════════════
struct ResultsScene {
    Scene base;             // (its +0x18: the settings)
    s32 state;              // +0x30  0 loading, 1 the screen, 2 fading out, 3 → 4 saving, 5 over
    s32 timer;              // +0x34
    u32 eval;               // +0x38
    u32 saveScreen;         // +0x3C
};
static_assert(sizeof(ResultsScene) == 0x40, "ResultsScene");

void DestroySaveScreen(ResultsScene* self) {
    if (self->saveScreen != 0)
        VirtualCall<void*, void*, int>(*P<u32>(self->saveScreen), 0x08, P(self->saveScreen), 1);
    self->saveScreen = 0;
}
int ResultsScene_Start(ResultsScene* self) {                    // 0x2A3350
    void* mem = MemAlloc(sizeof(Eval));
    self->eval = mem ? guest::addr(Eval_Ctor(mem)) : 0;
    Eval_Start(P<Eval>(self->eval));
    RenderMgr_TriggerEffect(*gRenderMgr, 0xB, 2, 0);
    self->state = 0;
    return 0;
}
int ResultsScene_Update(ResultsScene* self) {                   // 0x2A3170
    switch (self->state) {
    case 0:
        Eval_Update(P<Eval>(self->eval));
        if (P<Eval>(self->eval)->phase == 5) {
            RenderMgr_TriggerEffect(*gRenderMgr, 0xB, 0, 0);
            self->state++;
        }
        break;
    case 1:
        if (Eval_Update(P<Eval>(self->eval))) {
            RenderMgr_TriggerEffect(*gRenderMgr, 0xB, 1, 0);
            self->state++;
            self->timer = 0;
        }
        break;
    case 2:
        if (self->timer++ > 0x26)
            self->state++;
        break;
    case 3: {
        // The game in progress is cleared — what carries over between games
        // stays — and that is what gets saved.
        if (self->eval != 0)
            Eval_Dtor(P<Eval>(self->eval), 1);
        self->eval = 0;
        PlayerData_ResetGame(*gPlayerData, 0);
        *reinterpret_cast<s32*>(Pd()) = 1;
        void* mem = MemAlloc(0x150F8);
        self->saveScreen = mem ? guest::addr(SaveScreen_Ctor(mem, 1)) : 0;
        SaveScreen_Start(P(self->saveScreen));
        RenderMgr_TriggerEffect(*gRenderMgr, 0xB, 0, 0);
        self->state++;
        break;
    }
    case 4:
        if (SaveScreen_Update(P(self->saveScreen)) == 0) {
            self->state++;
            DestroySaveScreen(self);
            return 0;
        }
        break;
    }
    RenderMgr_UpdateEffects(*gRenderMgr);
    return 1;
}
int ResultsScene_Draw(ResultsScene* self) {                     // 0x2A30E0
    if (self->state == 5 || self->state == 4) {
        if (self->saveScreen != 0)
            Mode0_Draw(P(self->saveScreen));
    } else if ((self->state == 2 || self->state == 1) && self->eval != 0) {
        Eval_Draw(P<Eval>(self->eval));
    }
    RenderMgr_Draw(*gRenderMgr);
    return 0;
}
// 0x2A30A0: the settings go back to their defaults on the way out.
Scene* ResultsScene_GetNextScene(ResultsScene* self) {
    Display_ApplyOffset(1);
    Config_SetDefaults(P<Config>(*reinterpret_cast<u32*>(reinterpret_cast<u8*>(self) + 0x18)));
    Sound_ApplyConfig();
    return nullptr;
}
s32 ResultsScene_GetNextSceneId(ResultsScene*) { return kSceneBoot; }   // 0x2A3090
ResultsScene* ResultsScene_Destroy(ResultsScene* self, int flag) {      // 0x2A33B0
    if (self == nullptr)
        return nullptr;
    self->base.vtable = 0x2ED5E0u;
    if (self->eval != 0)
        Eval_Dtor(P<Eval>(self->eval), 1);
    self->eval = 0;
    DestroySaveScreen(self);
    Scene_BaseDtor(self, 0);
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

}  // namespace

extern "C" {

Scene* ResultsScene_Ctor(void* mem) {                           // 0x2A3460
    ResultsScene* self = static_cast<ResultsScene*>(Scene_BaseCtor(mem));
    self->base.vtable = 0x2ED5E0u;
    self->eval = 0;
    self->saveScreen = 0;
    return &self->base;
}

// Bound from hle::bind_all (nothing else refers to this file).
void ResultsScene_Bind() {
    const auto bind = [](u32 address, auto fn, const char* name) {
        guest::bind(address, reinterpret_cast<void*>(fn), name);
    };
    bind(0x2A3460, ResultsScene_Ctor, "ResultsScene_Ctor");
    bind(0x2A33B0, ResultsScene_Destroy, "ResultsScene_Destroy");
    bind(0x2A3350, ResultsScene_Start, "ResultsScene_Start");
    bind(0x2A3170, ResultsScene_Update, "ResultsScene_Update");
    bind(0x2A30E0, ResultsScene_Draw, "ResultsScene_Draw");
    bind(0x2A30A0, ResultsScene_GetNextScene, "ResultsScene_GetNextScene");
    bind(0x2A3090, ResultsScene_GetNextSceneId, "ResultsScene_GetNextSceneId");
}

}  // extern "C"
