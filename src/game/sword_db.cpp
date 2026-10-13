// Sword database — decompiled from 0x15CBF0–0x15F430 and 0x16A160–0x16A280.
// See include/game/sword.h.  The model / texture / motion caches the object
// also owns are filled by code that is not decompiled yet, so their lists
// are always empty here.

#include "game/sword.h"
#include "game/chara.h"
#include "engine/archive_file.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* MemAllocAligned(u32 align, u32 size);         // 0x1228C8
    void  MemFreeAligned(void* p);                      // 0x122B50
}

static_assert(sizeof(SwordDb) == 0xA4 && offsetof(SwordDb, swords) == 0x88, "SwordDb");

static u32& gSwordDb = *guest::ptr<u32>(0x2EE588u);
enum { kAssetSwordData = 0x3E };

// The tables come from the C library's malloc (0x122AF8) in the original.
static inline void* TableAlloc(u32 size) { return MemAllocAligned(0x10, size); }
static inline s32 Read32(const u8* p) { s32 v; memcpy(&v, p, 4); return v; }
static inline s16 Read16(const u8* p) { s16 v; memcpy(&v, p, 2); return v; }

// ══ Sword ═════════════════════════════════════════════════════════════════
// ── 0x16A160 ──────────────────────────────────────────────────────────────
void Sword_Init(Sword* self) {
    memset(&self->attack, 0, 0x28);
    memset(self->name, 0, sizeof(self->name));
    self->_80 = 0;
    self->_88 = 0;
    self->_84 = 0;
    self->_8c = -1;
    self->number = 0;
    memset(self->skill, 0, sizeof(self->skill));
}

// ── 0x16A280 ──────────────────────────────────────────────────────────────
Sword* Sword_Ctor(Sword* self) {
    Sword_Init(self);
    return self;
}

// ── 0x16A230 ──────────────────────────────────────────────────────────────
Sword* Sword_Dtor(Sword* self, s16 flag) {
    if (self && flag > 0)
        MemFree(self);
    return self;
}

// ══ Loading ═══════════════════════════════════════════════════════════════
// ── 0x15E0D0 ──────────────────────────────────────────────────────────────
// A technique blob: a count, that many 20-byte records whose last word is an
// offset, then nothing but offsets.  All become addresses.
static int Technique_Relocate(u32* blob, s32 size) {
    const u32 base = guest::addr(blob);
    const s32 count = (s32)blob[0];
    blob[0] = 0;
    for (s32 i = 0; i < count; i++)
        if (blob[i * 5 + 4] != 0)
            blob[i * 5 + 4] += base;
    u32* end = reinterpret_cast<u32*>(reinterpret_cast<u8*>(blob) + size);
    for (u32* p = blob + count * 5; p < end; p++)
        *p += base;
    return 1;
}

// ── 0x15E4F0 ──────────────────────────────────────────────────────────────
static void SwordDb_ParseSkillSheets(SwordDb* self, const u8* p) {
    const s32 count = Read32(p);
    p += 4;
    self->skillSheetCount = count;
    u32* table = static_cast<u32*>(TableAlloc((u32)count << 2));
    self->skillSheets = guest::addr(table);
    for (s32 i = 0; i < count; i++) {
        const s32 size = Read32(p);
        p += 4;
        if (size == 0) {
            table[i] = 0;
            continue;
        }
        SkillSheet* sheet = static_cast<SkillSheet*>(TableAlloc((u32)size));
        table[i] = guest::addr(sheet);
        memcpy(sheet, p, (size_t)size);
        sheet->entries += guest::addr(sheet);
        p += size;
    }
}

// ── 0x15E610 ──────────────────────────────────────────────────────────────
static void SwordDb_ParseTechniques(SwordDb* self, const u8* p) {
    const s32 count = Read32(p + 4);
    p += 8;
    self->techniqueCount = count;
    u32* table = static_cast<u32*>(TableAlloc((u32)count << 2));
    self->techniques = guest::addr(table);
    for (s32 i = 0; i < count; i++) {
        const s32 size = Read32(p);
        p += 4;
        if (size == 0) {
            table[i] = 0;
            continue;
        }
        u32* blob = static_cast<u32*>(TableAlloc((u32)size));
        table[i] = guest::addr(blob);
        memcpy(blob, p, (size_t)size);
        Technique_Relocate(blob, size);
        p += size;
    }
}

// ── 0x15E740 ──────────────────────────────────────────────────────────────
static void SwordDb_ParseTextureNames(SwordDb* self, const u8* p) {
    const s32 count = Read32(p);
    p += 4;
    self->textureNameCount = count;
    u32* table = static_cast<u32*>(TableAlloc((u32)count << 2));
    self->textureNames = guest::addr(table);
    for (s32 i = 0; i < count; i++) {
        const s32 length = Read32(p);
        char* name = static_cast<char*>(TableAlloc((u32)length + 1));
        table[i] = guest::addr(name);
        memcpy(name, p + 4, (size_t)length);
        name[length] = 0;
        p += 4 + length;
    }
}

// ── 0x15E840 ──────────────────────────────────────────────────────────────
// The file is seven sections, each (after the first) led by its size.
static void SwordDb_Parse(SwordDb* self, const u8* data) {
    // 1: motion pack names
    s32 size = Read32(data + 4);
    s32 count = Read32(data + 8);
    const u8* p = data + 0xC;
    self->motionNameCount = count;
    u32* names = static_cast<u32*>(TableAlloc((u32)count << 2));
    self->motionNames = guest::addr(names);
    for (s32 i = 0; i < count; i++) {
        const s8 length = (s8)*p;
        char* name = static_cast<char*>(TableAlloc((u32)length));
        names[i] = guest::addr(name);
        memcpy(name, p + 4, (size_t)length);
        p += 4 + length;
    }
    // 2: texture names
    p = data + 8 + size;
    size = Read32(p);
    p += 4;
    SwordDb_ParseTextureNames(self, p);
    p += size;
    // 3: styles (version 4 or later)
    size = Read32(p);
    count = Read16(p + 6);
    self->styles = guest::addr(TableAlloc((u32)count * 0x3C));
    memcpy(guest::ptr(self->styles), p + 8, (size_t)count * 0x3C);
    p += 4 + size;
    // 4: kinds
    size = Read32(p);
    count = Read32(p + 8);
    self->kinds = guest::addr(TableAlloc((u32)count * 0x60));
    memcpy(guest::ptr(self->kinds), p + 0xC, (size_t)count * 0x60);
    p += 4 + size;
    // 5: swords
    size = Read32(p);
    count = Read32(p + 8);
    self->swordCount = count;
    self->swords = guest::addr(TableAlloc((u32)count << 7));
    memcpy(guest::ptr(self->swords), p + 0xC, (size_t)count << 7);
    p += 4 + size;
    // 6: techniques
    size = Read32(p);
    p += 4;
    SwordDb_ParseTechniques(self, p);
    p += size;
    // 7: skill sheets
    SwordDb_ParseSkillSheets(self, p + 4);
}

// ── 0x15EB10 ──────────────────────────────────────────────────────────────
static void SwordDb_Load(SwordDb* self) {
    ArchiveFile* file = ArchiveFile_Create();
    ArchiveFile_Open(file, kAssetSwordData);
    const u32 size = ArchiveFile_GetSize(file);
    u8* data = static_cast<u8*>(MemAllocAligned(0x40, size));
    ArchiveFile_Read(file, data, size);
    ArchiveFile_Close(file);
    SwordDb_Parse(self, data);
    MemFreeAligned(data);
    ArchiveFile_Destroy(file);
}

// ── 0x15F210 ──────────────────────────────────────────────────────────────
static SwordDb* SwordDb_Ctor(SwordDb* self) {
    memset(self, 0, sizeof(*self));
    GuestList_Init(&self->models);
    GuestList_Init(&self->_1c);
    GuestList_Init(&self->textures);
    GuestList_Init(&self->_50);
    GuestList_Init(&self->_64);
    GuestList_Init(&self->_98);
    self->_94 = -1;
    SwordDb_Load(self);
    return self;
}

static void FreeTable(u32& table, s32 count) {
    if (!table)
        return;
    u32* entries = guest::ptr<u32>(table);
    for (s32 i = 0; i < count; i++)
        if (entries[i])
            MemFreeAligned(guest::ptr(entries[i]));
    MemFreeAligned(entries);
    table = 0;
}

// ── 0x15EC60 ──────────────────────────────────────────────────────────────
// (The original first empties the six cache lists; see the note at the top.)
static SwordDb* SwordDb_Dtor(SwordDb* self, s16 flag) {
    if (!self)
        return self;
    if (self->_04)
        MemFreeAligned(guest::ptr(self->_04));
    self->_04 = 0;
    FreeTable(self->_48, self->_4c);
    FreeTable(self->motionNames, self->motionNameCount);
    FreeTable(self->textureNames, self->textureNameCount);
    if (self->styles) MemFreeAligned(guest::ptr(self->styles));
    if (self->kinds)  MemFreeAligned(guest::ptr(self->kinds));
    if (self->swords) MemFreeAligned(guest::ptr(self->swords));
    FreeTable(self->skillSheets, self->skillSheetCount);
    FreeTable(self->techniques, self->techniqueCount);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x15F430 ──────────────────────────────────────────────────────────────
SwordDb* SwordDb_Get() {
    if (gSwordDb == 0) {
        void* mem = MemAlloc(sizeof(SwordDb));
        gSwordDb = mem ? guest::addr(SwordDb_Ctor(static_cast<SwordDb*>(mem))) : 0;
    }
    return guest::ptr<SwordDb>(gSwordDb);
}

// ── 0x15F400 ──────────────────────────────────────────────────────────────
void SwordDb_Release() {
    if (gSwordDb != 0)
        SwordDb_Dtor(guest::ptr<SwordDb>(gSwordDb), 1);
    gSwordDb = 0;
}

// ══ Queries ═══════════════════════════════════════════════════════════════
// ── 0x15CBF0 ──────────────────────────────────────────────────────────────
// Marks the techniques a new sword starts with, and those that follow them.
static void SwordDb_InitSkills(SwordDb* self, Sword* sword) {
    const s32 index = sword->skillSheet;
    const SkillSheet* sheet = nullptr;
    if (index < self->skillSheetCount && index >= 0)
        sheet = guest::ptr<SkillSheet>(guest::ptr<u32>(self->skillSheets)[index]);
    if (!sheet)
        return;
    const SkillEntry* entries = guest::ptr<SkillEntry>(sheet->entries);
    for (s32 i = 0; i < sheet->count; i++) {
        if (entries[i].requirement == 0 && sword->skill[i] == 0) {
            sword->skill[i] = 1;
            if (entries[i].next != -1)
                sword->skill[entries[i].next] = 2;
        }
    }
}

// ── 0x15CFF0 ──────────────────────────────────────────────────────────────
int SwordDb_MakeSword(SwordDb* self, int type, Sword* out) {
    if (type >= self->swordCount || type < 0)
        return 0;
    memcpy(out, guest::ptr<u8>(self->swords) + type * 0x80, 0x80);
    SwordDb_InitSkills(self, out);
    return 1;
}

// ═══ What characters ask of it (0x15CCD0–0x15E4B0) ════════════════════════
extern "C" int Rand_Next();     // 0x1200F0

// ── 0x15E4B0 / 0x15E470 / 0x15D0D0 ────────────────────────────────────────
u32 SwordDb_fn15e4b0(void* db, u32 sheet) {             // the skill sheet of a style
    SwordDb* self = static_cast<SwordDb*>(db);
    return (s32)sheet < self->skillSheetCount && (s32)sheet >= 0 ? guest::ptr<u32>(self->skillSheets)[sheet] : 0;
}
u32 SwordDb_fn15e470(void* db, u32 sheet) {             // …and its tree of blows
    SwordDb* self = static_cast<SwordDb*>(db);
    return (s32)sheet < self->techniqueCount && (s32)sheet >= 0 ? guest::ptr<u32>(self->techniques)[sheet] : 0;
}
void* SwordDb_fn15d0d0(void* db, u32 type) {            // a sword's entry (0x80 bytes)
    SwordDb* self = static_cast<SwordDb*>(db);
    return (s32)type < self->swordCount && (s32)type >= 0 ? guest::ptr<u8>(self->swords) + type * 0x80 : nullptr;
}

// ── 0x15C960 ──────────────────────────────────────────────────────────────
// A scroll teaches a sword one of the techniques of its level that it does
// not know — failing that of the level below, and so on down — at random.
// → which, or −1 if there is nothing left to learn.
extern "C" int SwordDb_TeachByItem(void* db, void* swordPtr, int level) {
    Sword* sword = static_cast<Sword*>(swordPtr);
    const u32 at = SwordDb_fn15e4b0(db, (u32)sword->skillSheet);
    if (at == 0)
        return -1;                      // (the original complains)
    const SkillSheet* sheet = guest::ptr<SkillSheet>(at);
    const SkillEntry* entries = guest::ptr<SkillEntry>(sheet->entries);
    s32 found[0x40], count = 0;
    for (; level >= 0 && count == 0; level--)
        for (s32 i = 0; i < sheet->count && count < 0x40; i++)
            if (sword->skill[i] == 0 && entries[i]._0c == level)
                found[count++] = i;
    if (count == 0)
        return -1;
    const s32 learnt = found[Rand_Next() % count];
    sword->skill[learnt] = 1;
    if (entries[learnt].next != -1)
        sword->skill[entries[learnt].next] = 2;
    return learnt;
}

// ── 0x15D800 ──────────────────────────────────────────────────────────────
// Once a frame while the world loads: takes in the chart of the sword in
// hand (see skill_sheets.cpp) and steps the queue of sword models and
// textures.  True when nothing is left to read.  (The queue's reader,
// 0x15D670, is not decompiled: the native build reads those on first draw,
// so the queue is always empty.)
extern "C" {
    int SkillSheets_Loaded(void* self, int slot);       // 0x23D750
    int SkillSheets_Poll(void* self, int slot);         // 0x23D7D0
    int PlayerData_GetReadiedSword(void* pd);           // 0x178220
    char SwordDb_PollLoads(void* db) {
        void* sheets = guest::ptr(*guest::ptr<u32>(0x2EE698u));
        void* pd = guest::ptr(*guest::ptr<u32>(0x2EE688u));
        if (SkillSheets_Loaded(sheets, PlayerData_GetReadiedSword(pd)) == 0)
            SkillSheets_Poll(sheets, PlayerData_GetReadiedSword(pd));
        return static_cast<SwordDb*>(db)->_1c.size == 0;
    }
}

// ── 0x15C820 ──────────────────────────────────────────────────────────────
// The blacksmith brings out a sword's hidden technique: the first entry of
// its sheet of kind 3 that it does not have yet.
extern "C" void SwordRecords_Learnt(void* records, void* sword, int slot, int technique, int next);  // 0x23B7F0
extern "C" int SwordDb_TeachSecret(void* db, void* swordPtr, int slot) {
    Sword* sword = static_cast<Sword*>(swordPtr);
    const u32 at = SwordDb_fn15e4b0(db, (u32)sword->skillSheet);
    if (at == 0)
        return 0;                       // (the original complains)
    const SkillSheet* sheet = guest::ptr<SkillSheet>(at);
    const SkillEntry* entries = guest::ptr<SkillEntry>(sheet->entries);
    for (s32 i = 0; i < sheet->count; i++) {
        if (sword->skill[i] != 0 || entries[i]._0c != 3)
            continue;
        sword->skill[i] = 1;
        if (entries[i].next != -1)
            sword->skill[entries[i].next] = 2;
        if (const u32 records = *guest::ptr<u32>(0x2EE698u))
            SwordRecords_Learnt(guest::ptr(records), sword, slot, i, entries[i].next);
        return 1;
    }
    return 0;
}

// ── 0x15CE60 ──────────────────────────────────────────────────────────────
// A sword for somebody other than the player: every technique of its sheet
// is known.
int SwordDb_fn15ce60(void* db, u32 type, void* out) {
    SwordDb* self = static_cast<SwordDb*>(db);
    if ((s32)type >= self->swordCount || (s32)type < 0)
        return 0;
    Sword* sword = static_cast<Sword*>(out);
    memcpy(sword, guest::ptr<u8>(self->swords) + type * 0x80, 0x80);
    const u32 at = SwordDb_fn15e4b0(db, (u32)sword->skillSheet);
    if (at == 0)
        return 1;                       // (the original complains)
    const SkillSheet* sheet = guest::ptr<SkillSheet>(at);
    const SkillEntry* entries = guest::ptr<SkillEntry>(sheet->entries);
    for (s32 i = 0; i < sheet->count; i++) {
        if (sword->skill[i] != 0)
            continue;
        sword->skill[i] = 1;
        if (entries[i].next != -1)
            sword->skill[entries[i].next] = 2;
    }
    return 1;
}

// ── 0x15CCD0 ──────────────────────────────────────────────────────────────
// No two swords of the crowd are quite alike: attack and defence vary by up
// to ±10 (more on higher difficulties), and below Hard they are a little
// less durable and worse balanced.
void SwordDb_fn15ccd0(void*, void* out) {
    static const GuestPtrVar<u8> gPlayer{0x2EE688u};
    static const s32 kSpread[3][2] = {{3, 2}, {3, 2}, {3, 2}};     // 0x2EDA68 / 0x2EDA70 / 0x2EDA78
    Sword* sword = static_cast<Sword*>(out);
    const s8 difficulty = (s8)(*gPlayer)[0x105];
    const int range = (difficulty + 1) * 0x14;
    auto clamp = [](s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; };
    sword->attack = clamp(sword->attack + Rand_Next() % range - 10, -500, 500);
    sword->defense = clamp(sword->defense + Rand_Next() % range - 10, -500, 500);
    if (difficulty == 2)
        return;
    u32 durability = (u32)(sword->durability - Rand_Next() % kSpread[0][difficulty]);
    if (durability > 5) durability = 5;
    sword->durability = durability == 0 ? 1 : (s32)durability;
    u16 a = (u16)((u16)sword->_66 - Rand_Next() % kSpread[1][difficulty]);
    sword->_66 = (s16)(a >= 0x10 ? 0xF : a == 0 ? 1 : a);
    u32 b = (u16)((u16)sword->_64 + Rand_Next() % kSpread[2][difficulty]);
    b = b >= 0x15 ? 0x14 : b == 0 ? 1 : b;
    if ((u32)(u16)sword->_66 + 5 < b)
        b = (u32)(u16)sword->_66 + 5;
    sword->_64 = (s16)b;
}

// ── 0x15D890 ──────────────────────────────────────────────────────────────
// The object a sword is shown and hits with (0x58 bytes), made from its
// style record and shared by everyone carrying one like it.  NOT done here:
// queueing its model and textures to be read (0x15FA90, 0x15D110, the lists
// at +0x1C / +0x38 / +0x68) — nothing draws swords yet, so both are simply
// marked as there.
struct SwordModel {
    s16 style;              // +0x00
    s16 texture, model;     // +0x02
    s16 _06, _08;
    u8  _0a[2];
    s32 texturesReady;      // +0x0C
    s32 modelReady;         // +0x10
    u32 mounts;             // +0x14  s16[6]: the bones it hangs from, by state
    u32 hands;              // +0x18  s16[4]: the bones it is held by
    u8  _1c[0x28];
    u32 box[4];             // +0x44  the blade's box: one- or two-handed, whole or broken
    u8  _54[4];
};
static_assert(sizeof(SwordModel) == 0x58 && offsetof(SwordModel, box) == 0x44, "SwordModel");

u32 SwordDb_fn15d890(void* db, u32 style, int forPlayer) {
    SwordDb* self = static_cast<SwordDb*>(db);
    if (forPlayer)
        self->_94 = (s32)style;
    GuestList* models = &self->models;
    for (GuestListNode* n = GuestList_Begin(models); n != GuestList_End(models); n = GuestList_Next(n))
        if (*guest::ptr<s16>(n->value) == (s16)style)
            return n->value;
    const u8* record = guest::ptr<u8>(self->styles) + style * 0x3C;
    auto word = [&](int at) { s32 v; memcpy(&v, record + at, 4); return v; };
    auto half = [&](int at) { s16 v; memcpy(&v, record + at, 2); return v; };
    SwordModel* m = static_cast<SwordModel*>(MemAlloc(sizeof(SwordModel)));
    memset(m, 0, sizeof(SwordModel));
    m->style = (s16)style;
    m->texture = (s16)word(0);
    m->model = (s16)word(4);
    m->_06 = half(8);
    m->_08 = half(10);
    m->hands = guest::addr(record + 0x34);
    m->mounts = guest::addr(record + 0x18);
    for (int i = 0; i < 4; i++) {
        const s16 kind = half(0x2C + i * 2);
        m->box[i] = kind == -1 ? 0 : self->kinds + (u32)kind * 0x60;
    }
    m->texturesReady = m->modelReady = 1;
    GuestList_Insert(models, GuestList_End(models), guest::addr(m));
    return guest::addr(m);
}
