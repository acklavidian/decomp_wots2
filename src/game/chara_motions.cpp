// Motions and action records — decompiled from 0x1518B0–0x151E60 (the
// records), 0x2BB140–0x2BB1A0 (the common motions) and the motion side of
// the resource manager, 0x1D2D50–0x1D4C00.
//
// NOT reproduced: the cache's memory budget.  The original keeps motions in
// two fixed pools (0x4A0000 bytes for everybody, 0xA0000 for the player's
// sword style) and moves a pack from one to the other when the player takes
// a different sword in hand (0x1D3030–0x1D3AA0).  Here every entry is its own
// heap block and nothing is ever moved; the reference counting is the same.

#include "game/chara_motions.h"
#include "engine/motion.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);       // 0x100820
    void  MemFree(void* p);         // 0x100650
}

static_assert(sizeof(ActionDef) == 0x1C && sizeof(ActionAttack) == 0x40, "ActionDef");
static_assert(offsetof(CharaResources, packTables) == 0x74 && offsetof(CharaResources, actionMap) == 0x94, "CharaResources");
static_assert(offsetof(CharaResources, motions) == 0x20 && offsetof(CharaResources, packs) == 0x5C, "CharaResources");

// ── 0x151940 ──────────────────────────────────────────────────────────────
void ActionDef_Relocate(ActionDef* def) {
    const u32 base = guest::addr(def);
    def->frames += base;
    u32* field = &def->attack;
    for (int i = 0; i < 5; i++)
        if (field[i] != 0)
            field[i] += base;
}

// ── 0x151B10, 0x1519D0–0x151AD0 ───────────────────────────────────────────
int ActionDef_LinkCount(const ActionDef* def) { return *guest::ptr<s32>(def->links); }
const ActionLink* ActionDef_Link(const ActionDef* def, int index) {
    return guest::ptr<const ActionLink>(def->links + 4) + index;
}

// ── 0x151C00 / 0x151B60 ───────────────────────────────────────────────────
// The lists are sorted by frame; a cursor (0x2EE52C / 0x2EE530) remembers how
// far the caller has got, so calling until −1 gives every entry of a frame.
static int EventAt(u32 list, s32* cursor, int frame) {
    if (list != 0) {
        const s32* events = guest::ptr<s32>(list);
        while (*cursor < events[0]) {
            const s32* entry = events + 1 + *cursor * 2;        // {what, frame}
            if (entry[1] == frame) {
                ++*cursor;
                return entry[0];
            }
            if (frame < entry[1])
                break;
            ++*cursor;
        }
    }
    *cursor = 0;
    return -1;
}
int ActionDef_SoundAt(const ActionDef* def, int frame) { return EventAt(def->sounds, guest::ptr<s32>(0x2EE52Cu), frame); }
int ActionDef_VoiceAt(const ActionDef* def, int frame) { return EventAt(def->voices, guest::ptr<s32>(0x2EE530u), frame); }

// ── 0x2BB1A0 (through 0x1D6D20) ───────────────────────────────────────────
static CommonMotions* const gCommon = guest::ptr<CommonMotions>(0x30A6E0u);

void CommonMotions_Load(const u32* file) {
    if (file[0] != 1)
        return;
    const u32 count = file[1];
    gCommon->count = count;
    s32* ids = static_cast<s32*>(MemAlloc(count * 4));
    u32* motions = static_cast<u32*>(MemAlloc(count * 4));
    u32* actions = static_cast<u32*>(MemAlloc(count * 4));
    gCommon->ids = guest::addr(ids);
    gCommon->motions = guest::addr(motions);
    gCommon->actions = guest::addr(actions);
    const u8* at = reinterpret_cast<const u8*>(file + 2);
    for (u32 i = 0; i < count; i++) {
        u32 size;
        memcpy(&ids[i], at, 4);
        memcpy(&size, at + 4, 4);
        u8* motion = static_cast<u8*>(MemAlloc(size));
        memcpy(motion, at + 8, size);
        Motion_Relocate(motion);
        motions[i] = guest::addr(motion);
        at += 8 + size;
        memcpy(&size, at, 4);
        ActionDef* def = static_cast<ActionDef*>(MemAlloc(size));
        memcpy(def, at + 4, size);
        ActionDef_Relocate(def);
        actions[i] = guest::addr(def);
        at += 4 + size;
    }
}

// ── The maps: id → a 4-byte block holding the pointer ─────────────────────
static u32 MapGet(GuestIntMap* map, s32 key) {
    GuestMapNode* node = GuestIntMap_Find(map, key);
    return node != GuestIntMap_End(map) ? node->value : 0;
}
static void MapSet(GuestIntMap* map, s32 key, u32 value) {
    GuestMapNode* node = GuestIntMap_Find(map, key);
    if (node != GuestIntMap_End(map)) {
        *guest::ptr<u32>(node->value) = value;
        return;
    }
    u32* holder = static_cast<u32*>(MemAlloc(4));
    *holder = value;
    GuestIntMap_Insert(map, key, guest::addr(holder));
}
static void MapDrop(GuestIntMap* map, s32 key) {
    GuestMapNode* node = GuestIntMap_Find(map, key);
    if (node == GuestIntMap_End(map))
        return;
    if (node->value != 0)
        MemFree(guest::ptr(node->value));
    node->value = 0;
    GuestIntMap_Erase(map, node);
}

u32 CharaResources_ActionOf(CharaResources* self, s32 motion) { return MapGet(&self->actionMap, motion); }   // 0x1D2D50
u32 CharaResources_MotionOf(CharaResources* self, s32 motion) { return MapGet(&self->motionMap, motion); }   // 0x1D2DA0
u32 CharaResources_PackTable(CharaResources* self, s32 pack)  { return MapGet(&self->packTables, pack); }    // 0x1D2DF0

// ── 0x1D2E90 ──────────────────────────────────────────────────────────────
void CharaResources_RegisterCommon(CharaResources* self) {
    const s32* ids = guest::ptr<s32>(gCommon->ids);
    for (u32 i = 0; i < gCommon->count; i++) {
        if (ids[i] == -1)
            continue;
        MapSet(&self->motionMap, ids[i], guest::ptr<u32>(gCommon->motions)[i]);
        MapSet(&self->actionMap, ids[i], guest::ptr<u32>(gCommon->actions)[i]);
    }
}

void CharaResources_InitMotions(CharaResources* self) {
    GuestList_Init(&self->motions);
    GuestList_Init(&self->packs);
    GuestIntMap_Init(&self->packTables);
    GuestIntMap_Init(&self->motionMap);
    GuestIntMap_Init(&self->actionMap);
    CharaResources_RegisterCommon(self);
}

// ── 0x1D4CA0 ──────────────────────────────────────────────────────────────
void CharaResources_ParseMotionList(CharaResources* self, const u32* file) {
    for (GuestListNode* n = GuestList_Begin(&self->motionNames); n != GuestList_End(&self->motionNames); n = GuestList_Next(n))
        MemFree(guest::ptr(n->value));
    GuestList_Clear(&self->motionNames);
    const s32 count = (s32)file[0];
    const u8* at = reinterpret_cast<const u8*>(file + 1);
    for (s32 i = 0; i < count; i++) {
        const s8 length = (s8)at[0];
        at += 4;
        char* name = static_cast<char*>(MemAlloc((u32)length + 1));
        if (length != 0) {
            memcpy(name, at, (size_t)length);
            at += length;
        }
        name[length] = 0;
        GuestList_Insert(&self->motionNames, GuestList_End(&self->motionNames), guest::addr(name));
    }
}

// ── 0x1D4C00 ──────────────────────────────────────────────────────────────
const char* CharaResources_PackPath(CharaResources* self, u32 pack) {
    if (pack >= self->motionNames.size)
        return nullptr;
    GuestListNode* n = GuestList_Begin(&self->motionNames);
    for (u32 i = 0; i < pack; i++)
        n = GuestList_Next(n);
    return guest::ptr<const char>(n->value);
}

// ── 0x1D4BB0 ──────────────────────────────────────────────────────────────
s32 CharaResources_PackOf(CharaResources* self, s32 chara) {
    const CharaInfo* info = CharaResources_Find(self, chara);
    return info ? info->pack : -1;
}

// ── Cache entries (0x1655D0 base; 0x1C85D0 a motion, 0x1C90C0 a pack) ─────
struct MotionEntry {
    s32 refs;       // +0x00
    s32 id;         // +0x04
    u32 size;       // +0x08
    u32 data;       // +0x0C  {u32 motion, u32 ActionDef, …}
    u32 pool;       // +0x10
    u32 vtable;     // +0x14  0x2EB8C0
    u32 handle;     // +0x18  = data once relocated
};
struct PackEntry {
    s32 refs;
    s32 id;
    u32 size;
    u32 data;       // +0x0C  the table {count, pairs*, pairs…}
    u32 pool;
    u32 vtable;     // +0x14  0x2EB8E0
    u32 handle;     // +0x18
    GuestList motions;  // +0x1C  ids of the motions it brought
};

static MotionEntry* FindMotion(CharaResources* self, s32 id, GuestListNode** at = nullptr) {
    for (GuestListNode* n = GuestList_Begin(&self->motions); n != GuestList_End(&self->motions); n = GuestList_Next(n)) {
        MotionEntry* e = guest::ptr<MotionEntry>(n->value);
        if (e->id == id) {
            if (at) *at = n;
            return e;
        }
    }
    return nullptr;
}
static PackEntry* FindPack(CharaResources* self, s32 id, GuestListNode** at = nullptr) {
    for (GuestListNode* n = GuestList_Begin(&self->packs); n != GuestList_End(&self->packs); n = GuestList_Next(n)) {
        PackEntry* e = guest::ptr<PackEntry>(n->value);
        if (e->id == id) {
            if (at) *at = n;
            return e;
        }
    }
    return nullptr;
}

// ── 0x1D4130 ──────────────────────────────────────────────────────────────
int CharaResources_HasPack(CharaResources* self, s32 pack) { return FindPack(self, pack) != nullptr; }

// ── 0x1D4590 + 0x1D4400 (0x1C84C0) ────────────────────────────────────────
static void AddMotion(CharaResources* self, s32 id, const u8* data, u32 size) {
    MotionEntry* e = static_cast<MotionEntry*>(MemAlloc(sizeof(MotionEntry)));
    e->refs = 1;
    e->id = id;
    e->size = size;
    e->pool = 0;
    e->vtable = 0x2EB8C0u;
    u32* block = static_cast<u32*>(MemAlloc(size));
    memcpy(block, data, size);
    e->data = e->handle = guest::addr(block);
    GuestList_Insert(&self->motions, GuestList_End(&self->motions), guest::addr(e));
    block[0] += e->data;
    Motion_Relocate(guest::ptr<u8>(block[0]));
    block[1] += e->data;
    ActionDef_Relocate(guest::ptr<ActionDef>(block[1]));
    MapSet(&self->motionMap, id, block[0]);
    MapSet(&self->actionMap, id, block[1]);
}

// ── 0x1D3F00 ──────────────────────────────────────────────────────────────
static void ReleaseMotion(CharaResources* self, s32 id) {
    GuestListNode* at = nullptr;
    MotionEntry* e = FindMotion(self, id, &at);
    if (!e || --e->refs != 0)
        return;
    MapDrop(&self->motionMap, id);
    MapDrop(&self->actionMap, id);
    MemFree(guest::ptr(e->data));
    MemFree(e);
    GuestList_Erase(&self->motions, at);
}

// ── 0x1D3C90 ──────────────────────────────────────────────────────────────
void CharaResources_ReleasePack(CharaResources* self, s32 pack) {
    GuestListNode* at = nullptr;
    PackEntry* e = FindPack(self, pack, &at);
    if (!e)
        return;
    for (GuestListNode* n = GuestList_Begin(&e->motions); n != GuestList_End(&e->motions); n = GuestList_Next(n))
        ReleaseMotion(self, (s32)n->value);
    if (--e->refs != 0)
        return;
    MapDrop(&self->packTables, pack);
    GuestList_Clear(&e->motions);
    MemFree(guest::ptr(e->data));
    MemFree(e);
    GuestList_Erase(&self->packs, at);
}

// ── The cache's part of 0x1CF030 (with 0x1D48E0, 0x1D4AA0, 0x1D40A0,
//    0x1D41B0, 0x1D42F0 / 0x1C8F10) ────────────────────────────────────────
u32 CharaResources_InstallPack(CharaResources* self, s32 pack, const u32* file) {
    if (file == nullptr) {
        // Another user of a pack that is in memory.
        PackEntry* e = FindPack(self, pack);
        if (!e)
            return 0;
        e->refs++;
        for (GuestListNode* n = GuestList_Begin(&e->motions); n != GuestList_End(&e->motions); n = GuestList_Next(n))
            if (MotionEntry* m = FindMotion(self, (s32)n->value))
                m->refs++;
        return CharaResources_PackTable(self, pack);
    }
    if (file[0] < 5)
        return 0;                           // (the original prints "old version")
    PackEntry* e = static_cast<PackEntry*>(MemAlloc(sizeof(PackEntry)));
    e->refs = 1;
    e->id = pack;
    e->pool = 0;
    e->vtable = 0x2EB8E0u;
    GuestList_Init(&e->motions);

    const u32 count = file[1];
    const u8* at = reinterpret_cast<const u8*>(file + 2);
    for (u32 i = 0; i < count; i++) {
        s32 id;
        u32 size;
        memcpy(&id, at, 4);
        memcpy(&size, at + 4, 4);
        if (id != -1) {
            if (MotionEntry* m = FindMotion(self, id))
                m->refs++;
            else if (CharaResources_MotionOf(self, id) == 0)
                AddMotion(self, id, at + 8, size);
            GuestList_Insert(&e->motions, GuestList_End(&e->motions), (u32)id);
        }
        at += 8 + size;
    }
    u32 size;
    memcpy(&size, at, 4);
    u32* table = static_cast<u32*>(MemAlloc(size));
    memcpy(table, at + 4, size);
    e->size = size;
    e->data = e->handle = guest::addr(table);
    table[1] += e->data;
    GuestList_Insert(&self->packs, GuestList_End(&self->packs), guest::addr(e));
    MapSet(&self->packTables, pack, e->handle);
    return CharaResources_PackTable(self, pack);
}
