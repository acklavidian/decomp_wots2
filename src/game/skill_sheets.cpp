// Skill sheets as the player sees them (object *0x2EE698, 0x768 bytes;
// 0x23A3C0–0x23DF60): for each sword style a chart of its techniques — where
// each stands on the chart, the buttons that do it, its name, and the lines
// that join them.  The "Check moves" screen draws the chart (sword_info.cpp)
// and the notices "You learned …!" are worded from it.
//
// Common/weapon/skillsheetlist (asset 0x3D, read at boot): u32 count (88),
// then {u32 length, name}: the file of each sheet, by `Sword+0x70`.
// A sheet (weapon/skillsheet/*.ssd) is 16-bit words:
//     version                       (below 3: 21 or 22 one-character labels and
//                                    five counted strings follow — the Japanese
//                                    release's; this one's files are version 3)
//     count, then per technique: key (its number on the sword's sheet), column,
//         row, kind (0 plain, else which word is printed on it: 1 Float, 2
//         Break, 3 Crush, 4 Throw, 5), buttons (see Buttons below), tally
//         needed, special (1: the name is printed in gold), frame (which
//         border: 0 / 1 / 2), follows (a key, or 0), name length, name
//     columns, rows
//     count, then groups {count, keys}: the cells that are one technique
//     count, then links {row, column, row, column, key, key}: lines on the chart
//     (version 1 only) count, then a second set of groups
//
// The object holds four charts: one for each carried sword (0–2) and one
// for the screen (4).  Only one file is read at a time.
//
// Kept on the host heap, not in guest memory: nothing but this file and the
// screen looks inside (the original's maps and lists of ref-counted strings
// are not reproduced; names are copied into guest memory for the font).

#include "game/skill_sheets.h"

#include "engine/bgm.h"
#include "engine/font.h"
#include "game/hud.h"
#include "game/sword.h"
#include "native/guest.h"

#include <cstring>
#include <string>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void* AssetRef_Ctor(void* mem);                             // 0x174C80
    void  AssetRef_Dtor(void* ref, int flag);                   // 0x174BE0
    void* AssetRef_GetData(void* ref);                          // 0x174A00
    s32   AssetRef_GetSize(void* ref);
    char  AssetQueue_Request(void* ref, int assetId);           // 0x174C90
    char  AssetQueue_RequestByName(void* ref, const char* name);
}

namespace {

const GuestPtrVar<void> gHud{0x2EE69Cu};
const GuestPtrVar<void> gSheets{0x2EE698u};
u32* const gSingleton = guest::ptr<u32>(0x2EE750u);
u32* const gFrameRatePtr = guest::ptr<u32>(0x2EDA00u);

struct Sheets {
    void* pending = nullptr;                // +0x000  the file being read
    std::vector<std::string> files;         // +0x004  by sheet number
    SkillChart charts[4];                   // +0x014 (the screen's), +0x1E4 × 3 (the carried swords')
    std::u16string notice;                  // +0x754
    s32 noticeKind = 0;                     // +0x764  1 learnt, 2 improved
    u32 noticeText = 0;                     // the same in guest memory
};
Sheets sSheets;

SkillChart* ChartOf(int slot) {
    if (slot == 4)
        return &sSheets.charts[0];
    if (slot >= 0 && slot <= 2)
        return &sSheets.charts[1 + slot];
    return nullptr;
}

inline const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
std::u16string Str(const u16* text) {
    std::u16string s;
    for (; text != nullptr && *text != 0; text++)
        s.push_back((char16_t)*text);
    return s;
}

// ── 0x23DF60 ──────────────────────────────────────────────────────────────
void Chart_Reset(SkillChart* c) {
    for (auto& t : c->techniques)
        if (t.second.name != 0)
            MemFree(guest::ptr(t.second.name));
    c->loaded = 0;
    c->columns = c->rows = 0;
    c->sheet = -1;
    c->techniques.clear();
    c->groups.clear();
    c->links.clear();
    c->groups2.clear();
}

// ── 0x23C880: the buttons as the file has them → as the chart keeps them ──
// High nibble: 0x1000 Triangle, 0x2000 Square, 0x4000 R1 + Triangle, 0x8000
// in the air; low bits 1 towards, 2 away, 4 held / R1, 8 jump first.
u16 Buttons(s16 code) {
    static const struct { s16 from; u16 to; } kTable[] = {
        {0x70, 0x800C}, {0x60, 0x8004}, {0x40, 0x8002}, {0x30, 0x8001}, {0x52, 0x4002}, {0x51, 0x4001},
        {0x50, 0x4000}, {0x2C, 0x2006}, {0x2B, 0x2005}, {0x28, 0x2004}, {0x23, 0x2003}, {0x22, 0x2002},
        {0x21, 0x2001}, {0x20, 0x2000}, {0x1C, 0x1006}, {0x1B, 0x1005}, {0x18, 0x1004}, {0x13, 0x1003},
        {0x12, 0x1002}, {0x11, 0x1001}, {0x10, 0x1000},
    };
    for (const auto& e : kTable)
        if (e.from == code)
            return e.to;
    return 0;
}

// ── 0x23A3C0: the buttons in words (pieces of the menu text) ──────────────
// 0x165 "J(ump)", 0x167 "+", 0x168 "←", 0x169 "→", 0x16A Triangle, 0x16B
// Square, 0x176 R1, 0x177 "Charge".
std::u16string ButtonText(u16 buttons) {
    static const struct { u16 buttons; s16 text[5]; } kTable[] = {
        {0x800C, {0x165, 0x167, 0x176, 0x167, 0x16B}}, {0x8004, {0x176, 0x167, 0x16B}},
        {0x8002, {0x165, 0x16B}},                      {0x8001, {0x165, 0x16A}},
        {0x4002, {0x168, 0x167, 0x176, 0x167, 0x16A}}, {0x4001, {0x169, 0x167, 0x176, 0x167, 0x16A}},
        {0x4000, {0x176, 0x167, 0x16A}},               {0x2006, {0x168, 0x167, 0x16B, 0x177}},
        {0x2005, {0x169, 0x167, 0x16B, 0x177}},        {0x2004, {0x16B, 0x177}},
        {0x2003, {0x168, 0x169, 0x167, 0x16B}},        {0x2002, {0x168, 0x167, 0x16B}},
        {0x2001, {0x169, 0x167, 0x16B}},               {0x2000, {0x16B}},
        {0x1006, {0x168, 0x167, 0x16A, 0x177}},        {0x1005, {0x169, 0x167, 0x16A, 0x177}},
        {0x1004, {0x16A, 0x177}},                      {0x1003, {0x168, 0x169, 0x167, 0x16A}},
        {0x1002, {0x168, 0x167, 0x16A}},               {0x1001, {0x169, 0x167, 0x16A}},
        {0x1000, {0x16A}},
    };
    std::u16string out;
    if (TextTable_Get() == nullptr)
        return out;
    for (const auto& e : kTable) {
        if (e.buttons != buttons)
            continue;
        for (const s16 id : e.text)
            if (id != 0)
                out += Str(Text(id));
        break;
    }
    return out;
}

std::u16string Described(const SkillCell& cell) {        // buttons + name (0x23A3C0, 0x199850)
    return ButtonText(cell.buttons) + Str(guest::ptr<u16>(cell.name));
}

// printf with only %s (0x16DF10)
std::u16string Format(const u16* format, const std::u16string& a, const std::u16string& b = {}) {
    std::u16string out;
    int n = 0;
    for (; format != nullptr && *format != 0; format++) {
        if (format[0] == '%' && format[1] == 's') {
            out += n++ == 0 ? a : b;
            format++;
        } else {
            out.push_back((char16_t)*format);
        }
    }
    return out;
}

// ── 0x23B6E0: is the technique alone in its cell group (1), or one of
// several of which another is already known (2)?  0: it is on no group ────
int GroupState(const SkillChart* c, const Sword* sword, s16 technique) {
    for (const auto& group : c->groups) {
        bool has = false;
        for (const s16 key : group)
            has = has || key == technique;
        if (!has)
            continue;
        if (group.size() == 1)
            return 1;
        for (const s16 key : group)
            if (key != technique && key >= 0 && key < 0x40 && sword->skill[key] == 1)
                return 2;
        return 1;
    }
    return 0;
}

// ── 0x23B540: the known technique a new one links on from; −2 if what it
// links from is not known at all, −1 none ────────────────────────────────
int LinkedFrom(const SkillChart* c, const Sword* sword, s16 technique) {
    for (const SkillLink& link : c->links) {
        if (link.to != technique)
            continue;
        s16 key = link.from;
        if (key < 0 || key > 0x3F)
            return -1;
        const u8 state = sword->skill[key];
        if (state == 1)
            return key;
        if (state == 0)
            return -2;
        if (state != 2)
            return -1;
        auto at = c->techniques.find(key);
        if (at == c->techniques.end())
            return -1;
        key = at->second.follows;
        if (key == 0 || key < 0 || key > 0x3F)
            return -1;
        for (;;) {
            if (sword->skill[key] == 1)
                return key;
            at = c->techniques.find(at->second.follows);
            if (at == c->techniques.end())
                return -1;
            key = at->second.follows;
            if (key < 0 || key > 0x3F)
                return -1;
        }
    }
    return -1;
}

void ShowNotice() {
    if (*gHud == nullptr)
        return;
    const u16* text = guest::ptr<u16>(sSheets.noticeText);
    if (sSheets.noticeKind == 1)
        Hud_ShowDate(*gHud, text, (int)((float)(s32)*gFrameRatePtr * 5.0f), 1);
    else if (sSheets.noticeKind == 2)
        Hud_ShowDate(*gHud, text, (int)((float)(s32)*gFrameRatePtr * 1.5f), 6);
}

// ── 0x23CE20 ──────────────────────────────────────────────────────────────
int Chart_Parse(SkillChart* c, const s16* p, const s16* end) {
    if (c->loaded == 1) {
        const s32 sheet = c->sheet;
        Chart_Reset(c);
        c->sheet = sheet;
    }
    const auto take = [&]() -> s16 { return p < end ? *p++ : (s16)0; };
    const s16 version = take();
    if (version < 3) {
        // 0x23CA20: the labels of the Japanese chart — one character each
        // (version 2 has one more), then five strings.  Not used here.
        p += 21 + (version == 2 ? 1 : 0);
        for (int i = 0; i < 5; i++) {
            const s16 n = take();
            p += n > 0 ? n : 0;
        }
    }
    const s16 count = take();
    for (s16 i = 0; i < count; i++) {
        const s16 key = take();
        SkillCell cell;
        cell.column = take();
        cell.row = take();
        cell.kind = take();
        cell.buttons = Buttons(take());
        cell.tally = take();
        cell.special = take();
        cell.frame = take();
        cell.follows = take();
        const s16 length = take();
        u16* name = static_cast<u16*>(MemAlloc(((u32)(length > 0 ? length : 0) + 1) * 2));
        s16 n = 0;
        for (; n < length; n++)
            name[n] = (u16)take();
        name[n] = 0;
        cell.name = guest::addr(name);
        if (!c->techniques.emplace(key, cell).second)
            MemFree(name);
    }
    c->columns = take();
    c->rows = take();
    const auto groups = [&](std::vector<std::vector<s16>>& out) {
        const s16 n = take();
        for (s16 i = 0; i < n; i++) {
            std::vector<s16> group;
            const s16 members = take();
            for (s16 k = 0; k < members; k++)
                group.push_back(take());
            out.push_back(std::move(group));
        }
    };
    groups(c->groups);
    const s16 links = take();
    for (s16 i = 0; i < links; i++) {
        SkillLink link;
        link.row0 = take();
        link.column0 = take();
        link.row1 = take();
        link.column1 = take();
        link.from = take();
        link.to = take();
        c->links.push_back(link);
    }
    if (version == 1)
        groups(c->groups2);
    c->loaded = 1;
    return 1;
}

}  // namespace

extern "C" {

// ── 0x23DC20 / 0x23E2D0 ───────────────────────────────────────────────────
void* SkillSheets_Get() {
    if (*gSingleton == 0) {
        // The handle the rest of the game passes about; what it stands for
        // is sSheets.
        void* handle = MemAlloc(0x10);
        memset(handle, 0, 0x10);
        *gSingleton = guest::addr(handle);
        for (SkillChart& c : sSheets.charts)
            Chart_Reset(&c);
    }
    return guest::ptr(*gSingleton);
}

// ── 0x23DA20: the list of files (asset 0x3D), at boot ─────────────────────
void SkillSheets_SetFiles(void*, const u8* list) {
    s32 count;
    memcpy(&count, list, 4);
    const u8* p = list + 4;
    sSheets.files.clear();
    for (s32 i = 0; i < count; i++) {
        s32 length;
        memcpy(&length, p, 4);
        sSheets.files.emplace_back(reinterpret_cast<const char*>(p + 4), (size_t)length);
        p += 4 + length;
    }
}

// What App_CreateDevices does with it (0x1E17E8…): makes the object, reads
// the list.
void SkillSheets_Boot() {
    void* self = SkillSheets_Get();
    *guest::ptr<u32>(0x2EE698u) = guest::addr(self);
    alignas(4) u8 ref[0xC];
    AssetRef_Ctor(ref);
    if (AssetQueue_Request(ref, 0x3D) != 0)
        SkillSheets_SetFiles(self, static_cast<const u8*>(AssetRef_GetData(ref)));
    AssetRef_Dtor(ref, 0);
}

// ── 0x23D870: asks for a sheet in a slot.  1 it is there, 2 being read,
// 3 busy with another, −1 no ───────────────────────────────────────────────
int SkillSheets_Request(void*, int sheet, int slot) {
    SkillChart* c = ChartOf(slot);
    if (c == nullptr)
        return -1;
    if (c->sheet == sheet && c->loaded == 1)
        return 1;
    if (sheet < 0 || (size_t)sheet >= sSheets.files.size())
        return -1;
    if (sSheets.pending != nullptr)
        return 3;
    void* ref = MemAlloc(0xC);
    sSheets.pending = ref != nullptr ? AssetRef_Ctor(ref) : nullptr;
    if (sSheets.pending == nullptr)
        return -1;
    if (AssetQueue_RequestByName(sSheets.pending, sSheets.files[(size_t)sheet].c_str()) == 0) {
        AssetRef_Dtor(sSheets.pending, 1);
        sSheets.pending = nullptr;
        return -1;
    }
    Chart_Reset(c);
    c->sheet = sheet;
    return 2;
}

// ── 0x23D750: 1 once the slot's sheet is read (−1: no such slot) ──────────
int SkillSheets_Loaded(void*, int slot) {
    const SkillChart* c = ChartOf(slot);
    return c != nullptr ? c->loaded : -1;
}

// ── 0x23D7D0: takes the file that was asked for into a slot.  4 nothing
// was asked for, 0 not read yet, 1 done ────────────────────────────────────
int SkillSheets_Poll(void*, int slot) {
    if (sSheets.pending == nullptr)
        return 4;
    const s16* data = static_cast<const s16*>(AssetRef_GetData(sSheets.pending));
    if (data == nullptr)
        return 0;
    SkillChart* c = ChartOf(slot);
    const int result = c != nullptr ? Chart_Parse(c, data, data + AssetRef_GetSize(sSheets.pending) / 2) : -1;
    AssetRef_Dtor(sSheets.pending, 1);
    sSheets.pending = nullptr;
    return result;
}

// ── 0x23DEC0 ──────────────────────────────────────────────────────────────
int SkillSheets_Free(void*, int slot) {
    SkillChart* c = ChartOf(slot);
    if (c != nullptr)
        Chart_Reset(c);
    return c != nullptr;
}

// ── 0x23DB90: when a scene ends ───────────────────────────────────────────
void SkillSheets_Reset(void*) {
    if (sSheets.pending != nullptr)
        AssetRef_Dtor(sSheets.pending, 1);
    sSheets.pending = nullptr;
    for (SkillChart& c : sSheets.charts)
        Chart_Reset(&c);
    sSheets.noticeKind = 0;
    sSheets.notice.clear();
}

// ── 0x23C580 … 0x23C800 ───────────────────────────────────────────────────
const SkillChart* SkillSheets_Chart(void*, int slot) {
    const SkillChart* c = ChartOf(slot);
    return c != nullptr ? c : &sSheets.charts[0];
}

// ── 0x23BA90: words the notice for a technique just learnt; `next` is the
// technique it opened the way to, or −1.  Returns the text, 0 if none ─────
const u16* SkillSheets_MakeNotice(void*, const void* swordPtr, int slot, int technique, int next) {
    const Sword* sword = static_cast<const Sword*>(swordPtr);
    const SkillChart* c = ChartOf(slot);
    if (sword == nullptr || c == nullptr || TextTable_Get() == nullptr)
        return nullptr;
    sSheets.noticeKind = 0;
    sSheets.notice.clear();
    const auto cell = c->techniques.find((s16)technique);
    if (cell == c->techniques.end())
        return nullptr;
    s32 kind = 1;
    if (next == -1) {
        const int group = GroupState(c, sword, (s16)technique);
        if (group == 1) {
            sSheets.notice = Format(Text(0x178), Described(cell->second));
        } else if (group == 2) {
            const int from = LinkedFrom(c, sword, (s16)technique);
            if (from == -1)
                return nullptr;
            if (from == -2) {
                sSheets.notice = Format(Text(0x178), Described(cell->second));
            } else {
                const auto base = c->techniques.find((s16)from);
                if (base == c->techniques.end())
                    return nullptr;
                sSheets.notice = Format(Text(0x179), Described(base->second), Described(cell->second));
            }
        } else {
            return nullptr;
        }
    } else {
        const auto opened = c->techniques.find((s16)next);
        if (opened == c->techniques.end())
            return nullptr;
        sSheets.notice = Format(Text(0x17A), Described(opened->second), Described(cell->second));
        kind = 2;
    }
    sSheets.noticeKind = kind;
    if (sSheets.noticeText == 0)
        sSheets.noticeText = guest::addr(guest::alloc(0x200 * 2));
    u16* out = guest::ptr<u16>(sSheets.noticeText);
    const size_t n = sSheets.notice.size() < 0x1FF ? sSheets.notice.size() : 0x1FF;
    memcpy(out, sSheets.notice.data(), n * 2);
    out[n] = 0;
    return out;
}

// ── 0x23B7F0: a technique was learnt — the sound and the notice ───────────
void SwordRecords_Learnt(void* self, void* swordPtr, int slot, int technique, int next) {
    const Sword* sword = static_cast<const Sword*>(swordPtr);
    Sound_PlaySE(0x27, 0, 0x3FFF);
    const bool opened = next >= 0 && next <= 0x3F && sword->skill[next] == 2;
    if (SkillSheets_MakeNotice(self, swordPtr, slot, technique, opened ? next : -1) != nullptr)
        ShowNotice();
}

// ── 0x23C4C0: the same without the sound (the sword screens) ──────────────
void SkillSheets_Announce(void* self, const void* sword, int slot, int technique, int next) {
    if (SkillSheets_MakeNotice(self, sword, slot, technique, next) != nullptr)
        ShowNotice();
}

}  // extern "C"
