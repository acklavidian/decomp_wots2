// The actor spawner — decompiled from 0x1837C0–0x186980.  See include/game/spawner.h.

#include "game/spawner.h"
#include "engine/archive_file.h"
#include "engine/mt_random.h"
#include "game/chara_resources.h"
#include "game/map_db.h"
#include "game/player_data.h"
#include "game/sword.h"
#include "game/world.h"
#include "game/world_parts.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* MemAllocAligned(u32 align, u32 size);         // 0x1228C8
    void  MemFreeAligned(void* p);                      // 0x122B50
    void  TaskMgr_Add(void* self, u32 actor, u32 a, u32 b);     // 0x1BF960
    void  CharaResources_Purge(void* res);              // 0x1D2AC0
    int   CharaResources_PickSword(void* res, int kind);    // 0x1D2AF0 (sword_table.cpp)
}

static_assert(sizeof(ActorTemplate) == 0x1C0 && offsetof(ActorTemplate, stat) == 0x118, "ActorTemplate");
static_assert(offsetof(ActorTemplate, compare) == 0x138 && offsetof(ActorTemplate, flags) == 0x178, "ActorTemplate");
static_assert(sizeof(Spawner) == 0x58 && offsetof(Spawner, tasks) == 0x30, "Spawner");
static_assert(sizeof(AreaCapacity) == 0x1C && sizeof(ActorDesc) == 0x40, "AreaCapacity");

static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
static u32& sLoaded         = *guest::ptr<u32>(0x2EE5E8u);
static s32& sPositionCount  = *guest::ptr<s32>(0x2EE5ECu);
static u32* const sCapacity = guest::ptr<u32>(0x2EE5F0u);       // AreaCapacity[areas], day / night
static const GuestPtrVar<u32> gTaskArchive{0x2EE5F8u};          // task/alltask, read at boot
static u32* const sPositions = guest::ptr<u32>(0x309F90u);      // three tables of {count, float[4][]}
static GuestList* const sTemplates = guest::ptr<GuestList>(0x309FB0u);  // ActorTemplate*
static const u32* const kTaskNames = guest::ptr<const u32>(0x2D7D10u);  // the files in the archive, in order

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }

static bool Contains(GuestList* list, u32 value) {
    for (GuestListNode* n = GuestList_Begin(list); n != GuestList_End(list); n = GuestList_Next(n))
        if (n->value == value)
            return true;
    return false;
}
// A list local to a function.  (On the PS2 these live on the stack; a list's
// end node is part of it, so here it has to be in guest memory.)
struct LocalLists {
    GuestList* lists;
    int count;
    explicit LocalLists(int n = 1) : lists(static_cast<GuestList*>(MemAlloc((u32)n * sizeof(GuestList)))), count(n) {
        for (int i = 0; i < n; i++)
            GuestList_Init(&lists[i]);
    }
    ~LocalLists() {
        for (int i = count - 1; i >= 0; i--)
            GuestList_Clear(&lists[i]);
        MemFree(lists);
    }
    GuestList* operator&() { return lists; }
    GuestList* operator->() { return lists; }
    GuestList& operator[](int i) { return lists[i]; }
};
static inline void Append(GuestList* list, u32 value) { GuestList_Insert(list, GuestList_End(list), value); }

static inline bool AppliesNow(const ActorTemplate* t, const PlayerData* pd) {
    return t->areas[pd->area] != 0 && t->periods[pd->day * 5 + pd->time] != 0;
}
static inline const AreaCapacity* Capacity(const PlayerData* pd) {
    return P<AreaCapacity>(sCapacity[pd->night]) + pd->area;
}

static u8* ReadFile(int assetId) {
    ArchiveFile* file = ArchiveFile_Create();
    ArchiveFile_Open(file, assetId);
    const u32 size = ArchiveFile_GetSize(file);
    u8* data = static_cast<u8*>(MemAllocAligned(0x40, size));
    ArchiveFile_Read(file, data, size);
    ArchiveFile_Destroy(file);
    return data;
}

// ══ The tables ════════════════════════════════════════════════════════════

// ── 0x185E30 ──────────────────────────────────────────────────────────────
// Common/GroupAI/mapdata: a count, then for day and for night that many
// {total, {min, max} × 3}; −1 stands for the total.
static void LoadCapacities() {
    u8* file = ReadFile(0x2D);
    const s32* in = reinterpret_cast<const s32*>(file);
    const s32 count = *in++;
    for (int night = 0; night < 2; night++) {
        AreaCapacity* table = static_cast<AreaCapacity*>(MemAllocAligned(0x10, (u32)count * sizeof(AreaCapacity)));
        sCapacity[night] = guest::addr(table);
        for (int i = 0; i < count; i++) {
            table[i].total = *in++;
            for (int f = 0; f < 3; f++) {
                s32 v = *in++;
                table[i].faction[f].min = v == -1 ? table[i].total : v;
                v = *in++;
                table[i].faction[f].max = v == -1 ? table[i].total : v;
            }
        }
    }
    MemFreeAligned(file);
}

// ── 0x185D10 ──────────────────────────────────────────────────────────────
static void StorePositions(const s32* in, int count, int which) {
    struct Set { s32 count; u32 points; };
    Set* sets = static_cast<Set*>(MemAllocAligned(0x10, (u32)count * sizeof(Set)));
    sPositions[which] = guest::addr(sets);
    for (int i = 0; i < count; i++) {
        sets[i].count = *in++;
        if (sets[i].count == 0) {
            sets[i].points = 0;
            continue;
        }
        float* points = static_cast<float*>(MemAllocAligned(0x10, (u32)sets[i].count * 16));
        sets[i].points = guest::addr(points);
        memcpy(points, in, (size_t)sets[i].count * 16);
        in += sets[i].count * 4;
    }
}

// ── 0x185A90 ──────────────────────────────────────────────────────────────
// Common/GroupAI/mapposdata, mapposdata2 and MapPosAngleData: places the
// tasks send actors to.  (The original reads each while parsing the last.)
static void LoadPositions() {
    for (int which = 0; which < 3; which++) {
        u8* file = ReadFile(0x2E + which);
        sPositionCount = *reinterpret_cast<const s32*>(file);
        StorePositions(reinterpret_cast<const s32*>(file) + 1, sPositionCount, which);
        MemFreeAligned(file);
    }
}

// ── 0x1852F0 ──────────────────────────────────────────────────────────────
// task/DataList: u16 count, u16 version (7 on this disc), then the templates.
static void LoadTemplates() {
    u8* file = ReadFile(0x40);
    const u32 header = *reinterpret_cast<const u32*>(file);
    const u32 count = header & 0xFFFF, version = header >> 16;
    const s32* in = reinterpret_cast<const s32*>(file) + 1;
    for (u32 i = 0; i < count; i++) {
        ActorTemplate* t = static_cast<ActorTemplate*>(MemAlloc(sizeof(ActorTemplate)));
        if (t != nullptr)
            for (int k = 0; k < 2; k++) {
                GuestList_Init(&t->flags[k]);
                GuestList_Init(&t->flagsB[k]);
                GuestList_Init(&t->dayFlags[k]);
            }
        const u32 length = (u32)*in++;
        char* name = static_cast<char*>(MemAllocAligned(0x10, length + 1));
        t->task = guest::addr(name);
        memcpy(name, in, length);
        name[length] = 0;
        in = reinterpret_cast<const s32*>(reinterpret_cast<const u8*>(in) + (length & ~3u));
        t->minStrength = *in++;
        t->repeatable = *in++;
        t->faction = *in++;
        if (version != 0) {
            t->role = *in++;
            t->fixed = *in++;
            t->grouped = *in++;
            t->group = *in++;
            t->story = *in++;
            for (int k = 0; k < 10; k++) t->areas[k] = *in++;
            for (int k = 0; k < 50; k++) t->periods[k] = *in++;
            if (version < 7) {
                for (int k = 0; k < 2; k++)
                    t->stat[k].index = t->stat[k].op = t->stat[k].value = -1;
                t->stat[0].index = *in++;
            } else {
                for (int k = 0; k < 2; k++) {
                    t->stat[k].index = *in++;
                    t->stat[k].op = *in++;
                    t->stat[k].value = *in++;
                }
            }
            for (int k = 0; k < 4; k++)
                for (int j = 0; j < 2; j++) {
                    t->compare[k][j].op = *in++;
                    t->compare[k][j].value = *in++;
                }
            if (version < 6) {
                t->taskA = t->taskB = -1;
            } else {
                t->taskA = *in++;
                t->taskB = *in++;
            }
            for (GuestList* lists : {t->flags, t->flagsB, t->dayFlags})
                for (int k = 0; k < 2; k++) {
                    const s32 n = *in++;
                    GuestList_Clear(&lists[k]);
                    for (s32 j = 0; j < n; j++)
                        Append(&lists[k], (u32)*in++);
                }
        }
        Append(sTemplates, guest::addr(t));
    }
    MemFreeAligned(file);
}

// ── 0x186060 ──────────────────────────────────────────────────────────────
void Spawner_ReleaseAll() {
    struct Set { s32 count; u32 points; };
    sLoaded = 0;
    for (int which = 0; which < 3; which++) {
        if (sPositions[which] == 0)
            continue;
        Set* sets = P<Set>(sPositions[which]);
        for (int i = 0; i < sPositionCount; i++)
            if (sets[i].points != 0)
                MemFreeAligned(P(sets[i].points));
        MemFreeAligned(sets);
        sPositions[which] = 0;
    }
    for (int night = 0; night < 2; night++)
        if (sCapacity[night] != 0) {
            MemFreeAligned(P(sCapacity[night]));
            sCapacity[night] = 0;
        }
    // (A static constructor makes this list in the original; here it is made
    // when the tables are first read, which a scene that ends at once never does.)
    if (sTemplates->endNext == 0)
        GuestList_Init(sTemplates);
    GuestListNode* node = GuestList_Begin(sTemplates);
    while (node != GuestList_End(sTemplates)) {
        ActorTemplate* t = P<ActorTemplate>(node->value);
        MemFreeAligned(P(t->task));
        if (t != nullptr) {
            for (int k = 1; k >= 0; k--) GuestList_Clear(&t->dayFlags[k]);
            for (int k = 1; k >= 0; k--) GuestList_Clear(&t->flagsB[k]);
            for (int k = 1; k >= 0; k--) GuestList_Clear(&t->flags[k]);
            MemFree(t);
        }
        node = GuestList_Erase(sTemplates, node);
    }
}

// ══ Choosing ══════════════════════════════════════════════════════════════

// ── 0x1844B0 ──────────────────────────────────────────────────────────────
// Picks one of `candidates` (CharaParams ids).  While there is memory to
// spare a character not yet in the area is preferred; after that only ones
// already there are used.  0 = nobody.
static int ChooseChara(Spawner* self, GuestList* candidates) {
    CharaResources* res = CharaResources_Get();
    if (candidates->size == 0)
        return 0;
    Mt_Seed(Timer1_Count());
    int pick = 0;
    {
        const int r = (int)Mt_Below(candidates->size);
        int i = 0;
        for (GuestListNode* n = GuestList_Begin(candidates); n != GuestList_End(candidates); n = GuestList_Next(n))
            if (i++ == r) {
                pick = (int)n->value;
                break;
            }
    }
    bool ok = false;
    if (self->memoryUsed < CharaResources_MemoryBudget(res)) {
        ok = true;
        for (GuestListNode* n = GuestList_Begin(candidates); n != GuestList_End(candidates); n = GuestList_Next(n))
            if (!Contains(&self->usedCharas, n->value)) {
                pick = (int)n->value;
                break;
            }
    } else {
        ok = Contains(&self->usedCharas, (u32)pick);
        if (!ok)
            for (GuestListNode* u = GuestList_Begin(&self->usedCharas); u != GuestList_End(&self->usedCharas);
                 u = GuestList_Next(u))
                for (GuestListNode* n = GuestList_Begin(candidates); n != GuestList_End(candidates);
                     n = GuestList_Next(n))
                    if (n->value == u->value) {
                        ok = true;
                        pick = (int)n->value;
                        Mt_Seed(Timer1_Count());
                        if (Mt_Below(100) > 50)
                            break;
                    }
    }
    if (!ok)
        return 0;
    if (Contains(&self->usedCharas, (u32)pick))
        return pick;
    const CharaParams* params = CharaResources_FindParamTemplate(res, pick);
    if (!Contains(&self->usedModels, (u32)params->chara)) {
        self->memoryUsed += params->unique != 0 ? 0x60000 : 0x50000;
        Append(&self->usedModels, (u32)params->chara);
    }
    Append(&self->usedCharas, (u32)pick);
    return pick;
}

// ── 0x186570 ──────────────────────────────────────────────────────────────
// The crowd is never made of named characters.
static void DropUnique(GuestList* candidates) {
    CharaResources* res = CharaResources_Get();
    GuestListNode* n = GuestList_Begin(candidates);
    while (n != GuestList_End(candidates)) {
        if (CharaResources_FindParamTemplate(res, (int)n->value)->unique == 0)
            n = GuestList_Next(n);
        else
            n = GuestList_Erase(candidates, n);
    }
}

// ── 0x184370 ──────────────────────────────────────────────────────────────
// The actors that are always there.
static void PickFixed(Spawner* self, GuestList* out) {
    LocalLists candidates;
    const PlayerData* pd = *gPlayerData;
    CharaResources* res = CharaResources_Get();
    for (GuestListNode* n = GuestList_Begin(sTemplates); n != GuestList_End(sTemplates); n = GuestList_Next(n)) {
        ActorTemplate* t = P<ActorTemplate>(n->value);
        if (!AppliesNow(t, pd) || t->fixed <= 0)
            continue;
        CharaResources_ListByRole(res, t->role, &candidates);
        const int chara = ChooseChara(self, &candidates);
        if (chara != 0) {
            t->chosen = chara;
            Append(out, n->value);
        }
    }
}

static inline bool Compare(s32 op, s32 have, s32 want, bool unchanged) {
    return op == 2 ? want < have : op == 1 ? have < want : op == 0 ? have == want : unchanged;
}

// ── 0x183E30 ──────────────────────────────────────────────────────────────
// The story actors whose conditions hold now.  Returns how many; they are
// added to `out` unless another actor there runs the same task.
static int PickStory(Spawner* self, GuestList* out) {
    LocalLists candidates, picked;
    const PlayerData* pd = *gPlayerData;
    const s8* bytes = reinterpret_cast<const s8*>(pd);
    int count = 0;
    CharaResources* res = CharaResources_Get();
    for (GuestListNode* n = GuestList_Begin(sTemplates); n != GuestList_End(sTemplates); n = GuestList_Next(n)) {
        ActorTemplate* t = P<ActorTemplate>(n->value);
        if (!AppliesNow(t, pd) || t->story == 0)
            continue;
        int failed = 0;
        bool ok = false;        // (so a story actor without any condition never appears)
        const struct { GuestList* lists; int offset; } flagSets[3] = {
            {t->flags, 0x740}, {t->flagsB, 0x7E5}, {t->dayFlags, 0x6C0}};
        for (const auto& set : flagSets)
            for (int want = 0; want < 2; want++)
                for (GuestListNode* f = GuestList_Begin(&set.lists[want]); f != GuestList_End(&set.lists[want]);
                     f = GuestList_Next(f)) {
                    ok = want == bytes[set.offset + (s32)f->value];
                    if (!ok)
                        failed++;
                }
        for (int k = 0; k < 2; k++)
            if (t->stat[k].index != -1) {
                ok = Compare(t->stat[k].op, bytes[0x806 + t->stat[k].index], t->stat[k].value, ok);
                if (!ok)
                    failed++;
            }
        for (int k = 0; k < 4; k++)
            for (int j = 0; j < 2; j++)
                if (t->compare[k][j].value != -1) {
                    ok = Compare(t->compare[k][j].op, bytes[0x7FC + k], t->compare[k][j].value, ok);
                    if (!ok)
                        failed++;
                }
        if (t->taskA != -1) {
            ok = t->taskA == *reinterpret_cast<const s16*>(bytes + 0x11E);
            if (!ok)
                failed++;
        }
        if (t->taskB != -1) {
            ok = t->taskB == *reinterpret_cast<const s16*>(bytes + 0x11C);
            if (!ok)
                failed++;
        }
        if (!ok || failed != 0)
            continue;
        CharaResources_ListByRole(res, t->role, &candidates);
        const int chara = ChooseChara(self, &candidates);
        if (chara != 0) {
            t->chosen = chara;
            Append(&picked, n->value);
            count++;
        }
    }
    for (GuestListNode* p = GuestList_Begin(&picked); p != GuestList_End(&picked); p = GuestList_Next(p)) {
        const ActorTemplate* t = P<ActorTemplate>(p->value);
        bool unique = true;
        for (GuestListNode* o = GuestList_Begin(out); o != GuestList_End(out); o = GuestList_Next(o))
            if (p->value != o->value && t->taskB == -1
                && strcmp(P<char>(t->task), P<char>(P<ActorTemplate>(o->value)->task)) == 0)
                unique = false;
        if (unique)
            Append(out, p->value);
    }
    return count;
}

// ── 0x185080 ──────────────────────────────────────────────────────────────
// Shares the crowd's places among the factions by strength, within the
// area's limits; what is left over goes to whoever is under its minimum,
// then at random.
static void SplitCrowd(Spawner* self) {
    const AreaCapacity* cap = Capacity(*gPlayerData);
    int placed = 0;
    for (int f = 0; f < 3; f++) {
        const int byShare = (int)(self->share[f] * (float)self->crowd);
        self->count[f] = cap->faction[f].max < byShare ? cap->faction[f].max : byShare;
        placed += self->count[f];
    }
    Mt_Seed(Timer1_Count());
    while (placed < self->crowd) {
        bool grown = false;
        for (int f = 0; f < 3; f++)
            if (self->count[f] < cap->faction[f].min && self->count[f] < cap->faction[f].max) {
                placed++;
                self->count[f]++;
                grown = true;
                break;
            }
        if (!grown) {
            const int f = (int)Mt_Below(3);
            if (self->count[f] < cap->faction[f].max) {
                placed++;
                self->count[f]++;
            }
        }
    }
}

// ── 0x183980 ──────────────────────────────────────────────────────────────
// The crowd: for each faction, templates that apply now, starting from a
// random one, until the faction has its share.  `out` is three lists.
static int PickCrowd(Spawner* self, GuestList* out) {
    LocalLists candidates, pool;
    const PlayerData* pd = *gPlayerData;
    CharaResources* res = CharaResources_Get();
    int total = 0;
    for (GuestListNode* n = GuestList_Begin(sTemplates); n != GuestList_End(sTemplates); n = GuestList_Next(n)) {
        const ActorTemplate* t = P<ActorTemplate>(n->value);
        if (t->fixed == 0 && t->story == 0)
            Append(&pool, n->value);
    }
    Mt_Seed(Timer1_Count());
    for (int f = 0; f < 3; f++) {
        const int want = self->count[f];
        int got = 0;
        LocalLists rotated, mine;
        for (GuestListNode* n = GuestList_Begin(&pool); n != GuestList_End(&pool); n = GuestList_Next(n)) {
            const s32 faction = P<ActorTemplate>(n->value)->faction;
            if (faction == -1 || faction == f)
                Append(&mine, n->value);
        }
        const int start = (int)Mt_Below(mine->size);
        int i = 0;
        for (GuestListNode* n = GuestList_Begin(&mine); n != GuestList_End(&mine); n = GuestList_Next(n))
            if (start <= i++)
                Append(&rotated, n->value);
        i = 0;
        for (GuestListNode* n = GuestList_Begin(&mine); n != GuestList_End(&mine); n = GuestList_Next(n))
            if (i++ < start)
                Append(&rotated, n->value);
        for (GuestListNode* n = GuestList_Begin(&rotated); n != GuestList_End(&rotated); n = GuestList_Next(n)) {
            ActorTemplate* t = P<ActorTemplate>(n->value);
            if (t->minStrength > pd->_144[f] || !AppliesNow(t, pd))
                continue;
            if (got >= want)
                break;
            CharaResources_ListByFaction(res, (s16)t->faction, t->role, &candidates);
            DropUnique(&candidates);
            const int chara = ChooseChara(self, &candidates);
            if (chara == 0)
                continue;       // (the original prints a debug message here)
            t->chosen = chara;
            Append(&out[f], n->value);
            got++;
            total++;
        }
    }
    return total;
}

// ══ Actors ════════════════════════════════════════════════════════════════

// 0x165600
static s32 HashName(const char* s) {
    s32 h = 0;
    for (; *s != 0; s++)
        h = (s32)*s + h * 0x13;
    return h;
}

// 0x13BA60 — LZSS, 4 KB window starting at 0xFEE; `src` = {packed size,
// unpacked size, data}.
static void Unpack(u8* dst, const u32* src) {
    u8 window[0x1000];
    memset(window, 0, 0xFEE);
    const s32 packed = (s32)src[0], size = (s32)src[1];
    const u8* in = reinterpret_cast<const u8*>(src + 2);
    u32 w = 0xFEE, flags = 0;
    s32 read = 8, written = 0;
    for (;;) {
        flags >>= 1;
        if ((flags & 0x100) == 0) {
            if (packed < read++) return;
            flags = *in++ | 0xFF00u;
        }
        if (flags & 1) {
            if (packed < read++) return;
            const u8 b = *in++;
            *dst++ = b;
            if (size < written++) return;
            window[w] = b;
            w = (w + 1) & 0xFFF;
        } else {
            if (packed < read++) return;
            const u8 lo = *in++;
            if (packed < read++) return;
            const u8 hi = *in++;
            const u32 from = lo | (u32)(hi & 0xF0) << 4;
            for (u32 k = 0; k <= (u32)(hi & 0xF) + 2; k++) {
                const u8 b = window[(from + k) & 0xFFF];
                *dst++ = b;
                if (size < written++) return;
                window[w] = b;
                w = (w + 1) & 0xFFF;
            }
        }
    }
}

// The task file `name` out of task/alltask: {count, alignment, data start,
// 0, packed sizes…}, files in the order of the name table at 0x2D7D10.
static TaskData* LoadTask(const char* name) {
    const u32* archive = *gTaskArchive;
    u32 offset = 0;
    for (int i = 0; kTaskNames[i] != 0; i++) {
        if (strcmp(name, P<char>(kTaskNames[i])) == 0) {
            TaskData* task = static_cast<TaskData*>(MemAlloc(sizeof(TaskData)));
            if (task != nullptr) {
                task->data = 0;
                task->size = 0;
            }
            const u32* entry = reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(archive) + archive[2] + offset);
            const u32 size = entry[1];
            // (A little more than the original asks for: its unpacker can
            // write two bytes past the end.)
            task->data = guest::addr(MemAllocAligned(0x40, size + 4));
            task->size = size;
            Unpack(P<u8>(task->data), entry);
            return task;
        }
        const u32 mask = archive[1] - 1;
        offset += ~mask & (mask + archive[4 + i]);
    }
    return nullptr;
}

// ── 0x184DE0 ──────────────────────────────────────────────────────────────
static void AddActor(Spawner* self, ActorTemplate* t, int slot, int chara, int group) {
    CharaResources* res = CharaResources_Get();
    ActorDesc* actor = static_cast<ActorDesc*>(MemAlloc(sizeof(ActorDesc)));
    actor->id = slot + 2;
    actor->kind = chara;
    const CharaParams* params = CharaResources_FindParamTemplate(res, chara);
    actor->chara = params->chara;
    // The sword table's choice for this kind of character and difficulty,
    // else its own (sword_table.cpp).
    const int picked = CharaResources_PickSword(res, chara);
    actor->sword = picked != -1 ? picked : params->sword;
    actor->box = 0;
    actor->group = group;
    actor->_14[0] = 0;
    if (t != nullptr) {
        const s32 key = HashName(P<char>(t->task));
        GuestMapNode* found = GuestIntMap_Find(&self->tasks, key);
        if (found == GuestIntMap_End(&self->tasks)) {
            if (TaskData* task = LoadTask(P<char>(t->task))) {
                GuestIntMap_Insert(&self->tasks, key, guest::addr(task));
                actor->box = guest::addr(task);
            }
        } else {
            actor->box = found->value;
        }
    }
    actor->state[0] = 2;
    actor->state[1] = 0;
    actor->state[2] = 0;
    actor->position[0] = 0.0f;
    actor->position[1] = -50.0f;     // out of sight until its task places it
    actor->position[2] = 0.0f;
    actor->position[3] = 1.0f;
    // (The original would fault on a task that is not in the archive; here
    // such an actor is left out.)
    if (actor->box != 0 && P<TaskData>(actor->box)->data != 0)
        Append(&self->actors, guest::addr(actor));
}

static inline int GroupOf(const ActorTemplate* t) {
    if (t->grouped == 0)
        return 0;
    return t->group == -1 ? 0xFF : t->group + 0x80;
}

// ── 0x1837C0 ──────────────────────────────────────────────────────────────
// `lists` = the crowd's three, then the fixed and story actors.
static void CreateActors(Spawner* self, GuestList* lists) {
    int slot = 0;
    for (GuestListNode* n = GuestList_Begin(&lists[3]); n != GuestList_End(&lists[3]); n = GuestList_Next(n)) {
        ActorTemplate* t = P<ActorTemplate>(n->value);
        AddActor(self, t, slot++, t->chosen, GroupOf(t));
    }
    for (int f = 0; f < 3; f++) {
        GuestListNode* n = GuestList_Begin(&lists[f]);
        ActorTemplate* current = nullptr;
        ActorTemplate* repeat = nullptr;
        for (int i = 0; i < self->count[f]; i++) {
            if (n == GuestList_End(&lists[f])) {
                if (repeat != nullptr)
                    current = repeat;
            } else {
                current = P<ActorTemplate>(n->value);
                if (current->repeatable != 0)
                    repeat = current;
                n = GuestList_Next(n);
            }
            if (current != nullptr)
                AddActor(self, current, slot++, current->chosen, GroupOf(current));
        }
    }
}

// ── 0x185240 ──────────────────────────────────────────────────────────────
static void FreeActors(Spawner* self) {
    GuestListNode* n = GuestList_Begin(&self->actors);
    while (n != GuestList_End(&self->actors)) {
        MemFree(P(n->value));
        n = GuestList_Erase(&self->actors, n);
    }
    GuestList_Clear(&self->actors);
}

extern "C" {

// ── 0x184B10 ──────────────────────────────────────────────────────────────
void Spawner_Load(void* mem) {
    Spawner* self = static_cast<Spawner*>(mem);
    self->memoryUsed = 0;
    GuestList_Clear(&self->usedCharas);
    GuestList_Clear(&self->usedModels);
    FreeActors(self);
    CharaResources_Purge(CharaResources_Get());
    const PlayerData* pd = *gPlayerData;
    LocalLists lists(4);
    PickFixed(self, &lists[3]);
    const int story = PickStory(self, &lists[3]);
    self->crowd = Capacity(pd)->total - story;
    if (self->crowd < 0)
        self->crowd = 0;
    self->strength = pd->_144[0] + pd->_144[1] + pd->_144[2];
    if (self->strength == 0)
        self->strength = 1;
    for (int f = 0; f < 3; f++)
        self->share[f] = (float)pd->_144[f] / (float)self->strength;
    SplitCrowd(self);
    PickCrowd(self, &lists);
    CreateActors(self, &lists);
    if (getenv("WOTS2_DUMP_ACTORS")) {      // debug hook, not in the original
        printf("[spawner] area %d day %d period %d: crowd %d = %d + %d + %d, %u actors\n", pd->area, pd->day,
               pd->time, self->crowd, self->count[0], self->count[1], self->count[2], self->actors.size);
        for (GuestListNode* n = GuestList_Begin(&self->actors); n != GuestList_End(&self->actors);
             n = GuestList_Next(n)) {
            const ActorDesc* a = P<ActorDesc>(n->value);
            printf("[spawner]   actor %d: params %d, model %d, sword %d, group %#x, task %u bytes\n", a->id,
                   a->kind, a->chara, a->sword, a->group, P<TaskData>(a->box)->size);
        }
        const PointSet* targets = &P<PointSet>(MapDb_Get()->targetPoints)[pd->area];
        for (int i = 0; i < 10; i++) {
            const float* v = P<float>(targets->points) + i * 4;
            printf("[spawner]   target point %d: %.1f %.1f %.1f\n", i, v[0], v[1], v[2]);
        }
    }
}

// ── 0x1848B0 ──────────────────────────────────────────────────────────────
void Spawner_Update(void* mem, World* world, void* taskMgr) {
    Spawner* self = static_cast<Spawner*>(mem);
    for (GuestListNode* n = GuestList_Begin(&self->actors); n != GuestList_End(&self->actors); n = GuestList_Next(n)) {
        ActorDesc* actor = P<ActorDesc>(n->value);
        if (actor->_14[0] == 0)
            continue;
        // Asked to come back: one of the area's ten target points, as new.
        const MapDb* db = MapDb_Get();
        void* charas = P(world->charaMgr);
        const s8 area = (*gPlayerData)->area;
        CharaMgr_Revive(charas, actor->id);
        Mt_Seed(Timer1_Count());
        const u32 r = Mt_Below(10);
        alignas(16) float position[4];
        memcpy(position, P<float>(P<PointSet>(db->targetPoints)[area].points) + r * 4, sizeof position);
        CharaMgr_SetPosition(charas, actor->id, position);
        CharaMgr_SetActive(charas, actor->id, 1);
        CharaMgr_Set68(charas, actor->id, 0);
        CharaMgr_Set6C(charas, actor->id, 0);
        CharaMgr_Set70(charas, actor->id, 0);
        CharaMgr_RestoreSword(charas, SwordDb_Get(), actor->id);
        CharaMgr_fn144370(charas, actor->id);
        if (CharaMgr_GetFlags(charas, actor->id) & 0x100)
            CharaMgr_Sheathe(charas, actor->id, 0);
        const int partner = CharaMgr_GetPartner(charas, actor->id);
        CharaMgr_SetPartner(charas, actor->id, -1);
        if (partner != -1) {
            CharaMgr_Set70(charas, partner, 0);
            CharaMgr_SetPartner(charas, partner, -1);
        }
        if (actor->box != 0) {
            const TaskData* task = P<TaskData>(actor->box);
            TaskMgr_Add(taskMgr, (u32)actor->id, task->data, task->size);
        }
        actor->_14[0] = 0;
    }
}

// ── 0x184840 ──────────────────────────────────────────────────────────────
// An actor left the area (MapOut): it is switched off and will come back.
void Spawner_ComeBack(void* mem, void* charas, int id) {
    Spawner* self = static_cast<Spawner*>(mem);
    for (GuestListNode* n = GuestList_Begin(&self->actors); n != GuestList_End(&self->actors); n = GuestList_Next(n)) {
        ActorDesc* actor = P<ActorDesc>(n->value);
        if (actor->id != id)
            continue;
        actor->_14[0] = 1;
        CharaMgr_SetActive(charas, id, 0);
        return;
    }
}

// ── 0x186250 ──────────────────────────────────────────────────────────────
void Spawner_Dtor(void* mem, int flag) {
    Spawner* self = static_cast<Spawner*>(mem);
    if (self == nullptr)
        return;
    FreeActors(self);
    for (GuestMapNode* n = GuestIntMap_Begin(&self->tasks); n != GuestIntMap_End(&self->tasks);
         n = GuestIntMap_Next(n)) {
        TaskData* task = P<TaskData>(n->value);
        if (task != nullptr) {
            if (task->data != 0) {
                MemFreeAligned(P(task->data));
                task->data = 0;
            }
            MemFree(task);
        }
    }
    GuestIntMap_Clear(&self->tasks);
    GuestList_Clear(&self->usedModels);
    GuestList_Clear(&self->usedCharas);
    GuestList_Clear(&self->actors);
    if ((s16)flag > 0)
        MemFree(self);
}

// ── 0x1863F0 ──────────────────────────────────────────────────────────────
void* Spawner_Ctor(void* mem) {
    Spawner* self = static_cast<Spawner*>(mem);
    GuestList_Init(&self->actors);
    GuestIntMap_Init(&self->tasks);
    GuestList_Init(&self->usedCharas);
    GuestList_Init(&self->usedModels);
    self->memoryUsed = 0;
    // (The template list is a global object; the original constructs it with
    // the other static constructors before main.)
    if (sTemplates->endNext == 0)
        GuestList_Init(sTemplates);
    if (sLoaded == 0) {
        LoadCapacities();
        LoadPositions();
        LoadTemplates();
        sLoaded = 1;
    }
    return self;
}

}  // extern "C"
