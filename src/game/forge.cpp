// The blacksmith's forge (object *0x2EE6A0, 0x28CC70–0x28EED0): what the
// event scripts' EventFlag.Forge… / CheckForge… built-ins do to a carried
// sword.  `n` is which carried sword (0–2).
//
// A sword has an attack and a defence (−500 … 500), a quality (1–15: how many
// times it can safely be tempered), a count of temperings so far and a
// durability (0 = broken, at most 5).  Tempering within the quality always
// works; beyond it the chance falls (90, 80, 50, 25, 10 per cent, then none)
// and a failure costs quality, durability — or the sword.
//
// Appraisal gives a sword a *title*, which goes in front of its name and
// adds to its numbers.  The titles are in weapon/smithdata (asset 0x72:
// u32 size, {u32 count, u32, count × {u32 length, 16-bit name, s32 judged,
// needed, attack, defence, quality, flag}}, then u32 size, {u32 count,
// count × {u32 key, u32 length, 16-bit name}} — the famous swords' names).
// A title is earned by attack (0), defence (1), quality (2), kills (3),
// experience (4) or by the sword's own name (5: `needed` is the key of the
// name); of those a sword qualifies for, the one that needs most is given.

#include "hal/ps2_types.h"
#include "native/guest.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
    int   Rand_Next();                                      // 0x1200F0
    struct SwordDb* SwordDb_Get();                           // 0x15F430
    int   SwordDb_TeachSecret(void* db, void* sword, int slot);     // 0x15C820
    void* ArchiveFile_Create();
    void  ArchiveFile_Open(void* file, int assetId);        // 0x142AA0
    u32   ArchiveFile_GetSize(void* file);
    void  ArchiveFile_Read(void* file, void* dst, u32 size);
}

namespace {

const GuestPtrVar<u8> gPlayerData{0x2EE688u};

// What the last operation changed (object +0x48 … +0x5C): attack, defence,
// tempered, quality, durability, title.  The scripts read it with GetUpParam.
s32 sUp[6];

u8* SwordAt(int n) {
    return n >= 0 && n < 3 ? *gPlayerData + n * 0x100 + 0x180 : nullptr;
}
inline s32& Attack(u8* s)     { return *reinterpret_cast<s32*>(s + 0x58); }
inline s32& Defence(u8* s)    { return *reinterpret_cast<s32*>(s + 0x5C); }
inline s32& Durability(u8* s) { return *reinterpret_cast<s32*>(s + 0x60); }
inline u16& Tempered(u8* s)   { return *reinterpret_cast<u16*>(s + 0x64); }
inline u16& Quality(u8* s)    { return *reinterpret_cast<u16*>(s + 0x66); }
inline s32 Clamp500(s32 v) { return v > 500 ? 500 : v < -500 ? -500 : v; }     // 0x16A090 / 0x16A060
inline u16 ClampQuality(s32 v) { return (u16)(v >= 0x10 ? 0xF : v <= 0 ? 1 : v); }  // 0x169FC0

// The four ways a tempering comes out end alike (0x28E560 … 0x28EBF0).
void Temper(u8* sword, s32 attack, s32 defence) {
    const s32 a = Attack(sword), d = Defence(sword);
    Attack(sword) = Clamp500(a + attack);
    Defence(sword) = Clamp500(d + defence);
    Tempered(sword)++;
    memset(sUp, 0, sizeof sUp);
    sUp[0] = Attack(sword) - a;
    sUp[1] = Defence(sword) - d;
    sUp[2] = 1;
}

// ── The titles (object +0x18 and +0x28; loaded by 0x28F120) ───────────────
struct Title {
    std::u16string name;
    s32 judged, needed, attack, defence, quality, flag;
};
std::vector<Title> sTitles;
std::map<s32, std::u16string> sFamous;
s32 sChosen[6] = {-1, -1, -1, -1, -1, -1};      // object +0x60 … +0x74

void LoadTitles() {
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    void* file = ArchiveFile_Create();
    ArchiveFile_Open(file, 0x72);
    const u32 size = ArchiveFile_GetSize(file);
    std::vector<u8> data(size);
    if (size < 16)
        return;
    ArchiveFile_Read(file, data.data(), size);
    const auto u32at = [&](u32 at) { u32 v = 0; if (at + 4 <= size) memcpy(&v, &data[at], 4); return v; };
    const auto text = [&](u32 at, u32 length) {
        std::u16string out;
        for (u32 i = 0; i < length && at + i * 2 + 2 <= size; i++) {
            u16 c;
            memcpy(&c, &data[at + i * 2], 2);
            if (c == 0)
                break;
            out.push_back((char16_t)c);
        }
        return out;
    };
    const u32 firstSize = u32at(0);
    u32 at = 4;
    const u32 count = u32at(at);
    at += 8;
    for (u32 i = 0; i < count && at + 4 <= size; i++) {
        const u32 length = u32at(at);
        Title t;
        t.name = text(at + 4, length);
        at += 4 + length * 2;
        if (at + 0x18 > size)
            break;
        s32 v[6];
        memcpy(v, &data[at], 0x18);
        at += 0x18;
        t.judged = v[0]; t.needed = v[1]; t.attack = v[2]; t.defence = v[3]; t.quality = v[4]; t.flag = v[5];
        sTitles.push_back(std::move(t));
    }
    at = 4 + firstSize + 4;
    const u32 names = u32at(at);
    at += 4;
    for (u32 i = 0; i < names && at + 8 <= size; i++) {
        const u32 key = u32at(at), length = u32at(at + 4);
        sFamous[(s32)key] = text(at + 8, length);
        at += 8 + length * 2;
    }
}

// What 0x28CEE0 … 0x28D770 begin with.
void ResetAppraisal() {
    memset(sUp, 0, sizeof sUp);
    for (s32& c : sChosen)
        c = -1;
}

}  // namespace

extern "C" {

// 0x28D770 / 0x28D5C0 / 0x28D410 / 0x28D260 / 0x28D0B0 / 0x28CEE0: the title
// the sword has earned by `judged` (0 attack … 5 its name), −1 none
int Forge_Judge(int n, int judged) {
    u8* sword = SwordAt(n);
    if (sword == nullptr || judged < 0 || judged > 5)
        return -1;
    LoadTitles();
    ResetAppraisal();
    s32 value = 0;
    switch (judged) {
    case 0: value = Attack(sword); break;
    case 1: value = Defence(sword); break;
    case 2: value = Quality(sword); break;
    case 3: value = *reinterpret_cast<s32*>(sword + 0x88); break;      // kills
    case 4: value = *reinterpret_cast<s32*>(sword + 0x84); break;      // experience
    }
    s32& chosen = sChosen[judged];
    for (s32 id = 0; id < (s32)sTitles.size(); id++) {
        const Title& t = sTitles[(size_t)id];
        if (t.judged != judged)
            continue;
        if (judged == 5) {
            // By name: the sword is one of the famous ones.
            const auto famous = sFamous.find(t.needed);
            if (chosen != -1 || famous == sFamous.end())
                continue;
            const u16* name = reinterpret_cast<const u16*>(sword);
            size_t length = 0;
            while (name[length] != 0)
                length++;
            if (length == famous->second.size() && memcmp(name, famous->second.data(), length * 2) == 0)
                chosen = id;
        } else if (t.needed <= value && (chosen == -1 || sTitles[(size_t)chosen].needed < t.needed)) {
            chosen = id;
        }
    }
    return chosen;
}

// 0x28DA90
const char16_t* Forge_TitleName(int title) {
    LoadTitles();
    return title >= 0 && title < (s32)sTitles.size() ? sTitles[(size_t)title].name.c_str() : u"";
}

// 0x28DAE0: the title goes on the sword
void Forge_SetTitle(int n, int title) {
    u8* sword = SwordAt(n);
    LoadTitles();
    if (sword == nullptr || title < 0 || title >= (s32)sTitles.size())
        return;
    const Title& t = sTitles[(size_t)title];
    const s32 a = Attack(sword), d = Defence(sword);
    const u16 q = Quality(sword);
    Attack(sword) = Clamp500(a + t.attack);
    Defence(sword) = Clamp500(d + t.defence);
    Quality(sword) = ClampQuality(((u32)q + (u32)t.quality) & 0xFFFF);
    // "<title> <name>", as much as the name's 44 characters hold.
    u16* name = reinterpret_cast<u16*>(sword);
    std::u16string full = t.name + u" ";
    for (const u16* c = name; *c != 0; c++)
        full.push_back((char16_t)*c);
    if (full.size() > 43)
        full.resize(43);
    memcpy(name, full.data(), full.size() * 2);
    name[full.size()] = 0;
    *reinterpret_cast<s32*>(sword + 0x8C) = title;
    memset(sUp, 0, sizeof sUp);
    sUp[0] = Attack(sword) - a;
    sUp[1] = Defence(sword) - d;
    sUp[3] = (s32)Quality(sword) - (s32)q;
    if (t.flag != 0)
        sUp[5] = 1;
}

// 0x28CE90
const u16* Forge_SwordName(int n) {
    static const u16 none[1] = {0};
    u8* sword = SwordAt(n);
    return sword != nullptr ? reinterpret_cast<const u16*>(sword) : none;
}

// 0x28E4F0: 1 it cannot be made tougher (over 5), 2 it is broken, 0 it can
int Forge_CheckHardness(int n) {
    u8* sword = SwordAt(n);
    if (sword == nullptr)
        return -1;
    return Durability(sword) < 6 ? (Durability(sword) == 0 ? 2 : 0) : 1;
}

// 0x28EED0
int Forge_GetKoudo(int n) {
    u8* sword = SwordAt(n);
    return sword != nullptr ? Durability(sword) : 0;
}

// 0x28E1D0: one more point of durability (from 1–4)
void Forge_Hardness(int n) {
    u8* sword = SwordAt(n);
    if (sword == nullptr)
        return;
    const s32 now = Durability(sword);
    if (now < 5 && now > 0) {
        Durability(sword) = now + 1;
        memset(sUp, 0, sizeof sUp);
        sUp[4] = 1;
    }
}

// 0x28E470: 1 within its quality, 2 up to five temperings over, 0 too far gone
int Forge_CheckQuality(int n) {
    u8* sword = SwordAt(n);
    if (sword == nullptr)
        return -1;
    const s32 over = (s32)Tempered(sword) - (s32)Quality(sword);
    return over < 5 ? (over < 0 ? 1 : 2) : 0;
}

// 0x28E360: how the tempering goes — 1 as it should, 2 the gamble came off, 0 it failed
int Forge_CheckSuccess(int n) {
    u8* sword = SwordAt(n);
    if (sword == nullptr)
        return -1;
    const s32 over = (s32)Tempered(sword) - (s32)Quality(sword);
    if (over < 0)
        return 1;
    (*reinterpret_cast<s16*>(*gPlayerData + 0x860))++;         // gambles taken
    const s32* chances = guest::ptr<s32>(0x2DB340u);            // 90, 80, 50, 25, 10
    const s32 chance = over < 5 ? chances[over] : 0;
    return Rand_Next() % 100 < chance ? 2 : 0;
}

// 0x28E120: `kind` 0 attack, 1 defence, 2 as the smith likes; `gamble` 0 an
// ordinary tempering, else one that beat the odds
void Forge_Quality(int n, int kind, int gamble) {
    u8* sword = SwordAt(n);
    if (sword == nullptr)
        return;
    const s32 tempered = Tempered(sword), quality = Quality(sword);
    if (kind == 2) {
        if (gamble == 0) {                                      // 0x28E7A0
            const s32 r1 = Rand_Next(), r2 = Rand_Next();
            const bool swap = Rand_Next() % 2 == 0;
            const s32 big = r1 % 21 - 10, small = r2 % 11 - 5;
            Temper(sword, swap ? small : big, swap ? big : small);
        } else {                                                // 0x28E560
            const s32 much = tempered + quality;
            const s32 attack = Rand_Next() % 2 != 0 ? much : -much;
            const s32 defence = Rand_Next() % 2 == 0 ? -much : much;
            Temper(sword, attack, defence);
        }
    } else if (gamble == 0) {                                   // 0x28EBF0
        const s32 gain = tempered + Rand_Next() % 3;
        const s32 other = Rand_Next() % 3 - 2;
        Temper(sword, kind == 0 ? gain : other, kind == 0 ? other : gain);
    } else {                                                    // 0x28E9F0
        const s32 gain = tempered + quality;
        Temper(sword, kind == 0 ? gain : 0, kind == 0 ? 0 : gain);
    }
}

// 0x28D920: the hidden technique of carried sword n, brought out
int Forge_Secret(int n) {
    u8* sword = SwordAt(n);
    return sword != nullptr ? SwordDb_TeachSecret(SwordDb_Get(), sword, n) : 0;
}

// 0x28DEE0: what a failed tempering costs — 0 the sword breaks, 1 a point of
// quality, 2 a point of durability; −1 nothing (it was broken already)
int Forge_Penalty(int n) {
    u8* sword = SwordAt(n);
    if (sword == nullptr)
        return -1;
    s16* broken = reinterpret_cast<s16*>(*gPlayerData + 0x862);
    switch (Rand_Next() % 3) {
    case 2:
        if (Durability(sword) == 0)
            return -1;
        if (--Durability(sword) == 0) {
            (*broken)++;
            return 0;
        }
        return 2;
    case 1:
        if (Quality(sword) != 0)
            Quality(sword) = ClampQuality((s32)(s16)Quality(sword) - 1);
        return 1;
    default:
        Durability(sword) = 0;
        (*broken)++;
        return 0;
    }
}

// 0x28DE60: can it be appraised — 2 it is broken, 1 it has a title already, 0 yes
int Forge_CheckJudge(int n) {
    u8* sword = SwordAt(n);
    if (sword == nullptr)
        return -1;
    if (Durability(sword) == 0)
        return 2;
    return *reinterpret_cast<s32*>(sword + 0x8C) == -1 ? 0 : 1;
}

// 0x28CE30
int Forge_GetUpParam(int which) { return which >= 0 && which < 6 ? sUp[which] : 0; }

}  // extern "C"
