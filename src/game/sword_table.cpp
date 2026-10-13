// Who carries which sword — decompiled from 0x2B4AD0–0x2B5050 (the table)
// and 0x1D2A90–0x1D2B20 (CharaResources' three calls into it).
//
// `weapon/…` asset 0x42: u32 count, then per kind of character (an id of
// chara/paramlist) and for each of the four difficulties ten pairs {sword
// type, chance %} (−1 = no entry).  A character made for the world or for a
// story scene asks here first and carries its parameters' own sword only if
// the table has nothing for it — so the same ronin turns up with different
// swords, and with better ones on the harder settings.
//
// Each different sword costs memory (a model, a motion pack), so the table
// remembers which types are in use: once there are seven, a newcomer gets
// one of those.  The list is forgotten when the world's resources are purged
// (a change of area, a new script).
//
// Nothing else reads the table's layout, so it is kept on the host side.

#include "native/archive.h"
#include "native/guest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
    int   Rand_Next();                                  // 0x1200F0
}

namespace {

struct Entry { s32 sword, chance; };
struct Record { s32 kind; Entry byDifficulty[4][10]; };
static_assert(sizeof(Record) == 0x144, "Record");

std::vector<Record> sRecords;
std::vector<s32> sInUse;
bool sLoaded = false;

void Load() {                                           // 0x2B4D70
    sLoaded = true;
    std::vector<u8> file;
    if (!archive::read_asset(0x42, file) || file.size() < 4)
        return;
    u32 count;
    memcpy(&count, file.data(), 4);
    count &= 0xFFFF;
    for (u32 i = 0; i < count && 4 + (size_t)(i + 1) * sizeof(Record) <= file.size(); i++) {
        Record r;
        memcpy(&r, &file[4 + (size_t)i * sizeof(Record)], sizeof r);
        sRecords.push_back(r);
    }
}
void Use(s32 sword) {                                   // 0x2B4AD0
    for (s32 used : sInUse)
        if (used == sword)
            return;
    sInUse.push_back(sword);
}

}  // namespace

extern "C" {

// ── 0x1D2AF0 / 0x2B4B50 ───────────────────────────────────────────────────
// The sword a character of this kind carries; −1 = the table does not say.
int CharaResources_PickSword(void*, int kind) {
    if (!sLoaded)
        Load();
    const u8* player = guest::ptr<u8>(*guest::ptr<u32>(0x2EE688u));
    int difficulty = (s8)player[0x105];
    if (player[0x104] != 0)
        difficulty = 3;
    if (difficulty < 0 || difficulty > 3)
        difficulty = 0;
    for (const Record& r : sRecords) {
        if (r.kind != kind)
            continue;
        const Entry* entries = r.byDifficulty[difficulty];
        if (sInUse.size() > 6) {
            // Enough different swords are loaded: its first choice if that
            // is one of them, else the one taken into use last.
            s32 last = -1;
            for (s32 used : sInUse) {
                last = used;
                if (used == entries[0].sword)
                    return used;
            }
            return last;
        }
        const int roll = Rand_Next() % 100;
        int sum = 0;
        for (int i = 0; i < 10; i++) {
            if (entries[i].sword == -1)
                continue;
            sum += entries[i].chance;
            if (roll <= sum) {
                if (getenv("WOTS2_TRACE_SWORDS"))           // (native debug aid)
                    printf("[swords] kind %d, difficulty %d: sword %d (%d types in use)\n", kind, difficulty,
                           entries[i].sword, (int)sInUse.size());
                Use(entries[i].sword);
                return entries[i].sword;
            }
        }
    }
    return -1;
}

// ── 0x1D2A90 ──────────────────────────────────────────────────────────────
void CharaResources_UseSword(void*, int sword) { Use(sword); }

// ── 0x1D2AC0 / 0x2B4D30 ───────────────────────────────────────────────────
void CharaResources_Purge(void*) { sInUse.clear(); }

}  // extern "C"
