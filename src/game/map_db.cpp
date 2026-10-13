// Map database — decompiled from 0x1475D0–0x1492E0, 0x191110–0x191430 and
// 0x193EC0–0x194100.  See include/game/map_db.h.

#include "game/map_db.h"
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

static_assert(sizeof(MapInfo) == 0xC0 && sizeof(AreaLink) == 0x18 && sizeof(MapDb) == 0x48, "map db");

static u32& gMapDb = *guest::ptr<u32>(0x2EE504u);
// Three fixed pools the maps are loaded into.
static u32* const gMapPool = guest::ptr<u32>(0x2EE508u);
struct Arena { u32 base, size, align, header; };
static Arena* const gMapArena = guest::ptr<Arena>(0x308C64u);       // 0x30 bytes apart
static const u32 kPoolSize[3] = {0x2C0000, 0x170000, 0xA0000};
enum { kAssetMapList = 0x22, kAssetLinkList = 0x23, kAssetPathPoint = 0x25, kAssetPathGroup = 0x26,
       kAssetTargetPoint = 0x27 };

// The C library's malloc (0x122AF8) in the original.
static inline void* Alloc(u32 size) { return MemAllocAligned(0x10, size); }
static inline s32 Read32(const u8* p) { s32 v; memcpy(&v, p, 4); return v; }

// 0x142F10 — one free block spanning the pool
static void Arena_Init(Arena* arena, u32 base, u32 size, u32 align) {
    arena->base = base;
    arena->size = size;
    arena->align = align;
    arena->header = align < 8 ? 8 : align;
    u32* first = guest::ptr<u32>(base);
    first[0] = 0;
    first[1] = (u32)((s32)(size - arena->header) / (s32)align);
}

// ══ Map list ══════════════════════════════════════════════════════════════
// ── 0x191430 ──────────────────────────────────────────────────────────────
static MapInfo* MapInfo_Ctor(MapInfo* self) {
    memset(self, 0, sizeof(*self));
    self->_18 = -1.0f;
    self->id = -1;
    self->light[3][0] = self->light[3][1] = self->light[3][2] = 0.5f;
    self->light[3][3] = 1.0f;
    return self;
}

// ── 0x191380 ──────────────────────────────────────────────────────────────
static void MapInfo_Dtor(MapInfo* self) {
    if (self->model)     MemFreeAligned(guest::ptr(self->model));
    if (self->collision) MemFreeAligned(guest::ptr(self->collision));
    if (self->sky)       MemFreeAligned(guest::ptr(self->sky));
    if (self->water)     MemFreeAligned(guest::ptr(self->water));
    MemFree(self);
}

static u32 ReadString(const u8* text, s32 length) {
    char* copy = static_cast<char*>(Alloc((u32)length + 1));
    strncpy(copy, reinterpret_cast<const char*>(text), (size_t)length);
    copy[length] = 0;
    return guest::addr(copy);
}

// ── 0x191110 ──────────────────────────────────────────────────────────────
// One record; later versions of the file add fields.
static const u8* MapInfo_Parse(MapInfo* self, const u8* p, u16 version) {
    self->id = (s8)p[0];
    s32 length = Read32(p + 4);
    p += 8;
    self->model = ReadString(p, length);
    p += length;
    length = Read32(p);
    self->collision = ReadString(p + 4, length);
    p += 4 + length;
    if (version > 3) {
        length = Read32(p);
        p += 4;
        if (length != 0) {
            self->sky = ReadString(p, length);
            p += length;
            if (version > 4) {
                memcpy(&self->skyParam, p, 4);
                p += 4;
            }
        }
        length = Read32(p);
        p += 4;
        if (length != 0) {
            self->water = ReadString(p, length);
            p += length;
            if (version > 4) {
                memcpy(&self->waterParam, p, 4);
                p += 4;
            }
        }
        if (version > 5) {
            const s32 present = Read32(p);
            p += 4;
            if (present != 0) {
                memcpy(&self->_18, p, 4);
                p += 4;
            }
        }
    }
    if (version != 0) {
        memcpy(self->color, p, 4);
        p += 4;
        if (version > 1) {
            memcpy(self->light, p, 0x40);
            memcpy(self->lightColor, p + 0x40, 0x40);
            p += 0x80;
        }
        if (version > 2) {
            memcpy(self->fog, p, 0x10);
            p += 0x10;
        }
    }
    return p;
}

// ── 0x1488F0 ──────────────────────────────────────────────────────────────
static void MapDb_FreeMaps(MapDb* self) {
    GuestListNode* n = GuestList_Begin(&self->maps);
    while (n != GuestList_End(&self->maps)) {
        if (n->value)
            MapInfo_Dtor(guest::ptr<MapInfo>(n->value));
        n = GuestList_Erase(&self->maps, n);
    }
}

// ── 0x1489B0 ──────────────────────────────────────────────────────────────
static void MapDb_ParseMaps(MapDb* self, const u8* p) {
    MapDb_FreeMaps(self);
    const u32 header = (u32)Read32(p);
    p += 4;
    for (u32 i = 0; i < (header & 0xFFFF); i++) {
        void* mem = MemAlloc(sizeof(MapInfo));
        MapInfo* info = mem ? MapInfo_Ctor(static_cast<MapInfo*>(mem)) : nullptr;
        p = MapInfo_Parse(info, p, (u16)(header >> 16));
        GuestList_Insert(&self->maps, GuestList_End(&self->maps), guest::addr(info));
    }
}

// ══ Area links ════════════════════════════════════════════════════════════
// ── 0x194100 ──────────────────────────────────────────────────────────────
static AreaLink* AreaLink_Ctor(AreaLink* self) {
    GuestIntMap_Init(&self->exits);
    self->map = -1;
    self->_04 = -1;
    return self;
}

// ── 0x194000 ──────────────────────────────────────────────────────────────
static void AreaLink_Dtor(AreaLink* self) {
    for (GuestMapNode* n = GuestIntMap_Begin(&self->exits); n != GuestIntMap_End(&self->exits);
         n = GuestIntMap_Next(n)) {
        AreaExit* exit = guest::ptr<AreaExit>(n->value);
        if (!exit)
            continue;
        if (exit->name)
            MemFreeAligned(guest::ptr(exit->name));
        MemFree(exit);
    }
    GuestIntMap_Clear(&self->exits);
    MemFree(self);
}

// ── 0x193EC0 ──────────────────────────────────────────────────────────────
static const u8* AreaLink_Parse(AreaLink* self, const u8* p) {
    memcpy(&self->map, p, 4);
    memcpy(&self->_04, p + 4, 4);
    const s32 count = Read32(p + 8);
    p += 0xC;
    for (s32 i = 0; i < count; i++) {
        AreaExit* exit = static_cast<AreaExit*>(MemAlloc(sizeof(AreaExit)));
        exit->target = -1;
        exit->name = 0;
        memcpy(&exit->target, p, 4);
        const s32 length = Read32(p + 4);
        p += 8;
        if (length != 0) {
            char* name = static_cast<char*>(Alloc((u32)length + 1));
            memcpy(name, p, (size_t)length);
            p += length;
            name[length] = 0;
            exit->name = guest::addr(name);
        }
        GuestIntMap_Insert(&self->exits, i, guest::addr(exit));
    }
    return p;
}

// ── 0x148690 ──────────────────────────────────────────────────────────────
static void MapDb_FreeLinks(MapDb* self) {
    for (GuestList& list : self->links) {
        GuestListNode* n = GuestList_Begin(&list);
        while (n != GuestList_End(&list)) {
            AreaLink_Dtor(guest::ptr<AreaLink>(n->value));
            n = GuestList_Erase(&list, n);
        }
    }
}

// ── 0x148760 ──────────────────────────────────────────────────────────────
// The second set of links (version 2 files) is for the other time of day.
static void MapDb_ParseLinks(MapDb* self, const u8* p) {
    MapDb_FreeLinks(self);
    const u32 header = (u32)Read32(p);
    const u32 version = header >> 16, count = header & 0xFFFF;
    p += 4;
    for (int set = 0; set < 2; set++) {
        if (set == 1 && version <= 1)
            break;
        for (u32 i = 0; i < count; i++) {
            void* mem = MemAlloc(sizeof(AreaLink));
            AreaLink* link = mem ? AreaLink_Ctor(static_cast<AreaLink*>(mem)) : nullptr;
            p = AreaLink_Parse(link, p);
            GuestList_Insert(&self->links[set], GuestList_End(&self->links[set]), guest::addr(link));
        }
    }
}

// ══ Route tables ══════════════════════════════════════════════════════════
static void FreePointSets(u32& table, s32 count) {
    if (!table)
        return;
    PointSet* sets = guest::ptr<PointSet>(table);
    for (s32 i = 0; i < count; i++) {
        if (sets[i].points)
            MemFreeAligned(guest::ptr(sets[i].points));
        sets[i].points = 0;
    }
    MemFreeAligned(sets);
    table = 0;
}

static const u8* ReadPoints(PointSet* set, const u8* p) {
    if (set->count == 0) {
        set->points = 0;
        return p;
    }
    void* points = Alloc((u32)set->count << 4);
    set->points = guest::addr(points);
    memcpy(points, p, (size_t)set->count << 4);
    return p + ((size_t)set->count << 4);
}

// ── 0x148250 ──────────────────────────────────────────────────────────────
static void MapDb_ParsePathPoints(MapDb* self, const u8* p) {
    FreePointSets(self->pathPoints, self->count);                   // 0x148140
    FreePointSets(self->pathPoints2, self->count2);
    const u32 header = (u32)Read32(p);
    p += 4;
    self->count = (s32)(header & 0xFFFF);
    if (header >> 16 == 0) {
        self->count2 = 0;
    } else {
        self->count2 = Read32(p);
        p += 4;
    }
    PointSet* sets = static_cast<PointSet*>(Alloc((u32)self->count * sizeof(PointSet)));
    self->pathPoints = guest::addr(sets);
    for (s32 i = 0; i < self->count; i++) {
        sets[i].id = i;
        sets[i].count = Read32(p);
        p = ReadPoints(&sets[i], p + 4);
    }
    if (self->count2 != 0) {
        sets = static_cast<PointSet*>(Alloc((u32)self->count2 * sizeof(PointSet)));
        self->pathPoints2 = guest::addr(sets);
        for (s32 i = 0; i < self->count2; i++) {
            sets[i].id = Read32(p);
            sets[i].count = Read32(p + 4);
            p = ReadPoints(&sets[i], p + 8);
        }
    }
}

// ── 0x1484E0 ──────────────────────────────────────────────────────────────
static void MapDb_ParseTargetPoints(MapDb* self, const u8* p) {
    FreePointSets(self->targetPoints, self->count);
    self->count = Read32(p);
    p += 4;
    PointSet* sets = static_cast<PointSet*>(Alloc((u32)self->count * sizeof(PointSet)));
    self->targetPoints = guest::addr(sets);
    for (s32 i = 0; i < self->count; i++) {
        sets[i].count = Read32(p);
        p = ReadPoints(&sets[i], p + 4);
    }
}

// The group array is a C++ array-new block: its length sits before it.
static inline s32& GroupCount(u32 groups) { return *guest::ptr<s32>(groups - 4); }

// ── 0x147DA0 ──────────────────────────────────────────────────────────────
static void MapDb_FreePathGroups(MapDb* self) {
    if (!self->pathGroups)
        return;
    GuestVector* groups = guest::ptr<GuestVector>(self->pathGroups);
    const s32 count = GroupCount(self->pathGroups);
    for (s32 i = 0; i < count; i++) {
        const u32* entries = guest::ptr<u32>(groups[i].data);
        for (u32 j = 0; j < groups[i].count; j++) {
            PointSet* set = guest::ptr<PointSet>(entries[j]);
            if (!set)
                continue;
            if (set->points)
                MemFreeAligned(guest::ptr(set->points));
            MemFree(set);
        }
        if (groups[i].data)
            MemFree(guest::ptr(groups[i].data));
    }
    MemFree(guest::ptr(self->pathGroups - 0x10));
    self->pathGroups = 0;
}

// ── 0x147F60 ──────────────────────────────────────────────────────────────
static void MapDb_ParsePathGroups(MapDb* self, const u8* p) {
    MapDb_FreePathGroups(self);
    const s32 count = Read32(p);
    p += 4;
    u8* block = static_cast<u8*>(MemAlloc((u32)count * sizeof(GuestVector) + 0x10));
    memset(block, 0, (size_t)count * sizeof(GuestVector) + 0x10);
    GuestVector* groups = reinterpret_cast<GuestVector*>(block + 0x10);
    self->pathGroups = guest::addr(groups);
    GroupCount(self->pathGroups) = count;
    for (s32 i = 0; i < count; i++) {
        const s32 sets = Read32(p);
        p += 4;
        for (s32 j = 0; j < sets; j++) {
            PointSet* set = static_cast<PointSet*>(MemAlloc(sizeof(PointSet)));
            set->id = Read32(p);
            set->count = Read32(p + 4);
            set->points = 0;
            p = ReadPoints(set, p + 8);
            VectorU32_PushBack(&groups[i], guest::addr(set));
        }
    }
}

// ══ The database ══════════════════════════════════════════════════════════
static u8* ReadFile(int assetId) {
    ArchiveFile* file = ArchiveFile_Create();
    ArchiveFile_Open(file, assetId);
    const u32 size = ArchiveFile_GetSize(file);
    u8* data = static_cast<u8*>(MemAllocAligned(0x40, size));
    ArchiveFile_Read(file, data, size);
    ArchiveFile_Destroy(file);
    return data;
}

// ── 0x148B30 ──────────────────────────────────────────────────────────────
// (The original reads each file while it parses the one before.)
void MapDb_Load(MapDb* self, int reload) {
    if (reload == 0 && self->loaded != 0)
        return;
    static const struct { int asset; void (*parse)(MapDb*, const u8*); } kFiles[] = {
        {kAssetMapList, MapDb_ParseMaps},
        {kAssetLinkList, MapDb_ParseLinks},
        {kAssetPathPoint, MapDb_ParsePathPoints},
        {kAssetPathGroup, MapDb_ParsePathGroups},
        {kAssetTargetPoint, MapDb_ParseTargetPoints},
    };
    for (const auto& f : kFiles) {
        u8* data = ReadFile(f.asset);
        f.parse(self, data);
        MemFreeAligned(data);
    }
    self->loaded = 1;
}

// ── 0x1491C0 ──────────────────────────────────────────────────────────────
static MapDb* MapDb_Ctor(MapDb* self) {
    GuestList_Init(&self->maps);
    GuestList_Init(&self->links[0]);
    GuestList_Init(&self->links[1]);
    for (int i = 0; i < 3; i++)
        gMapPool[i] = guest::addr(MemAllocAligned(0x40, kPoolSize[i]));
    self->loaded = 0;
    self->_20 = -1;
    self->_0c = 0;
    self->pathPoints = 0;
    self->pathPoints2 = 0;
    self->targetPoints = 0;
    self->pathGroups = 0;
    self->count = 0;
    self->count2 = 0;
    for (int i = 0; i < 3; i++)
        Arena_Init(reinterpret_cast<Arena*>(reinterpret_cast<u8*>(gMapArena) + i * 0x30),
                   gMapPool[i], kPoolSize[i], 0x40);
    return self;
}

// ── 0x148FA0 ──────────────────────────────────────────────────────────────
static void MapDb_Dtor(MapDb* self) {
    self->_0c = 0;                  // (a map still loaded would be released here)
    for (int i = 0; i < 3; i++) {
        if (gMapPool[i])
            MemFreeAligned(guest::ptr(gMapPool[i]));
        gMapPool[i] = 0;
    }
    MapDb_FreeMaps(self);
    MapDb_FreeLinks(self);
    FreePointSets(self->pathPoints, self->count);
    FreePointSets(self->pathPoints2, self->count2);
    MapDb_FreePathGroups(self);
    FreePointSets(self->targetPoints, self->count);
    MemFree(self);
}

// ── 0x148AE0 ──────────────────────────────────────────────────────────────
MapDb* MapDb_Get() {
    if (gMapDb == 0) {
        void* mem = MemAlloc(sizeof(MapDb));
        gMapDb = mem ? guest::addr(MapDb_Ctor(static_cast<MapDb*>(mem))) : 0;
    }
    return guest::ptr<MapDb>(gMapDb);
}

// ── 0x148AB0 ──────────────────────────────────────────────────────────────
void MapDb_Release() {
    if (gMapDb != 0)
        MapDb_Dtor(guest::ptr<MapDb>(gMapDb));
    gMapDb = 0;
}

// ── 0x1475D0 ──────────────────────────────────────────────────────────────
s32 MapDb_GetAreaMap(MapDb* self, int area, int variant) {
    GuestList* list = &self->links[variant];
    int index = 0;
    for (GuestListNode* n = GuestList_Begin(list); n != GuestList_End(list); n = GuestList_Next(n), index++)
        if (index == area)
            return guest::ptr<AreaLink>(n->value)->map;
    return -1;
}

static AreaLink* FindLink(MapDb* self, int area, int variant) {
    GuestList* list = &self->links[variant];
    int index = 0;
    for (GuestListNode* n = GuestList_Begin(list); n != GuestList_End(list); n = GuestList_Next(n), index++)
        if (index == area)
            return guest::ptr<AreaLink>(n->value);
    return nullptr;
}

// ── 0x147550 (0x193E70) ───────────────────────────────────────────────────
s32 MapDb_GetExitMap(MapDb* self, int area, int exit, int variant) {
    AreaLink* link = FindLink(self, area, variant);
    if (!link)
        return -1;
    GuestMapNode* node = GuestIntMap_Find(&link->exits, exit);
    return node == GuestIntMap_End(&link->exits) ? -1 : guest::ptr<AreaExit>(node->value)->target;
}

// ── 0x1474D0 (0x193E20) ───────────────────────────────────────────────────
const char* MapDb_GetExitScript(MapDb* self, int area, int exit, int variant) {
    AreaLink* link = FindLink(self, area, variant);
    if (!link)
        return nullptr;
    GuestMapNode* node = GuestIntMap_Find(&link->exits, exit);
    if (node == GuestIntMap_End(&link->exits))
        return nullptr;
    const u32 name = guest::ptr<AreaExit>(node->value)->name;
    return name ? guest::ptr<const char>(name) : nullptr;
}

// ══ Map list getters ══════════════════════════════════════════════════════
const MapInfo* MapDb_FindMap(MapDb* self, int map) {
    for (GuestListNode* n = GuestList_Begin(&self->maps); n != GuestList_End(&self->maps); n = GuestList_Next(n))
        if (guest::ptr<MapInfo>(n->value)->id == map)
            return guest::ptr<MapInfo>(n->value);
    return nullptr;
}
static const char* PathOrNull(u32 path) { return path ? guest::ptr<const char>(path) : nullptr; }

// ── 0x147B30 / 0x147AE0 / 0x147A90 / 0x147A40 ─────────────────────────────
const char* MapDb_GetModelPath(MapDb* self, int map) {
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? PathOrNull(info->model) : nullptr;
}
const char* MapDb_GetCollisionPath(MapDb* self, int map) {
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? PathOrNull(info->collision) : nullptr;
}
const char* MapDb_GetSkyPath(MapDb* self, int map) {
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? PathOrNull(info->sky) : nullptr;
}
const char* MapDb_GetWaterPath(MapDb* self, int map) {
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? PathOrNull(info->water) : nullptr;
}

// ── 0x1479A0 / 0x1479F0 / 0x147950 / 0x1478F0 ─────────────────────────────
s32 MapDb_GetWaterParam(MapDb* self, int map) {
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? info->waterParam : 0;
}
s32 MapDb_GetSkyParam(MapDb* self, int map) {
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? info->skyParam : 0;
}
float MapDb_GetFarPlane(MapDb* self, int map) {
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? info->_18 : 0.0f;
}
u32 MapDb_GetClearColor(MapDb* self, int map) {
    *guest::ptr<u32>(0x2EE514u) = 0;
    const MapInfo* info = MapDb_FindMap(self, map);
    u32 color = 0;
    if (info)
        memcpy(&color, info->color, 4);
    return color;
}

// ── 0x147870 / 0x1477F0 / 0x1477A0 ────────────────────────────────────────
static float* IdentityAt(u32 address) {
    float* m = guest::ptr<float>(address);
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}
const float* MapDb_GetLights(MapDb* self, int map) {
    const float* fallback = IdentityAt(0x308CE0u);
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? &info->light[0][0] : fallback;
}
const float* MapDb_GetLightColors(MapDb* self, int map) {
    const float* fallback = IdentityAt(0x308D20u);
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? &info->lightColor[0][0] : fallback;
}
const float* MapDb_GetFog(MapDb* self, int map) {
    const MapInfo* info = MapDb_FindMap(self, map);
    return info ? info->fog : guest::ptr<const float>(0x2D1D30u);
}

// ── 0x1473B0 ──────────────────────────────────────────────────────────────
const PointSet* MapDb_GetRoutePoints(MapDb* self, int area) {
    static const GuestPtrVar<u8> gPlayerData{0x2EE688u};
    static s32& gIndoors = *guest::ptr<s32>(0x2EE518u);
    s32 loaded;
    memcpy(&loaded, *gPlayerData + (gIndoors == 0 ? 0x6E84 : 0x6E88), 4);
    const PointSet* extra = guest::ptr<PointSet>(self->pathPoints2);
    for (s32 i = 0; i < self->count2; i++)
        if (extra[i].id == loaded)
            return &extra[i];
    return area < self->count ? guest::ptr<PointSet>(self->pathPoints) + area : nullptr;
}
