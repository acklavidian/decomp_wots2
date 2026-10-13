// Map collision — decompiled from 0x157DF0–0x1581A0 and 0x16B6F0–0x16C350.
// See include/game/map_collision.h.

#include "game/map_collision.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);       // 0x100820
    void  MemFree(void* p);         // 0x100650
}

static_assert(sizeof(ColHeader) == 0x24 && sizeof(ColCellType) == 0x3C && sizeof(ColSpot) == 0x20, "collision file");
static_assert(offsetof(Collision, current) == 0x24 && offsetof(Collision, types) == 0x60, "Collision");
static_assert(sizeof(Collision) == 0x7C, "Collision");

static float* const sHitPoint = guest::ptr<float>(0x309E40u);
static GuestList* const sNoPoints = guest::ptr<GuestList>(0x308E50u);

static inline const ColHeader* Header(const Collision* c) { return guest::ptr<ColHeader>(c->header); }

// ══ Collision ═════════════════════════════════════════════════════════════
// ── 0x16BE70 ──────────────────────────────────────────────────────────────
// The object points into the file, which must stay loaded.
Collision* Collision_Ctor(Collision* self, const s32* file) {
    GuestIntMap_Init(&self->types);
    GuestList_Init(&self->found);
    self->eventMode = 0;
    self->current.flags = kCellBlocked;
    self->current._08 = 0;
    self->current._0c = 0;
    self->current.height = 0.0f;
    self->pointCount = file[0];
    const s32* p = file + 1;
    if (self->pointCount == 0) {
        self->points = 0;
    } else {
        self->points = guest::addr(p);
        p += self->pointCount * 4;
    }
    self->header = guest::addr(p);
    const ColHeader* header = reinterpret_cast<const ColHeader*>(p);
    self->typeCount = header->typeCount;
    const ColCellType* type = reinterpret_cast<const ColCellType*>(header + 1);
    for (s32 i = 0; i < self->typeCount; i++, type++)
        GuestIntMap_Insert(&self->types, type->id, guest::addr(type));
    self->cells = guest::addr(type);
    const s32* afterCells = reinterpret_cast<const s32*>(type) + header->cellCount;
    self->extraCount = afterCells[0];
    self->extra = guest::addr(afterCells + 1);

    ColSpot* spots = static_cast<ColSpot*>(MemAlloc((u32)self->pointCount << 5));
    self->spots = guest::addr(spots);
    const s32* point = guest::ptr<const s32>(self->points);
    for (s32 i = 0; i < self->pointCount; i++, point += 4) {
        spots[i].name = point[0];
        memcpy(spots[i].pos, point + 1, 12);
        spots[i].pos[3] = 1.0f;
    }
    return self;
}

// ── 0x16BD50 ──────────────────────────────────────────────────────────────
Collision* Collision_Dtor(Collision* self, s16 flag) {
    if (!self)
        return self;
    GuestIntMap_Clear(&self->types);
    if (self->spots)
        MemFree(guest::ptr(self->spots));
    self->spots = 0;
    GuestList_Clear(&self->found);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x16B6F0 ──────────────────────────────────────────────────────────────
GuestList* Collision_FindPoints(Collision* self, int name) {
    GuestList_Clear(&self->found);
    ColSpot* spots = guest::ptr<ColSpot>(self->spots);
    for (s32 i = 0; i < self->pointCount; i++)
        if (spots[i].name == name)
            GuestList_Insert(&self->found, GuestList_End(&self->found), guest::addr(spots[i].pos));
    return &self->found;
}

// Makes `current` a copy of a cell's type; a cell with nothing (or an unknown
// type) leaves it as it was.
static void LoadCell(Collision* self, u32 x, u32 z, bool clear40) {
    const ColHeader* header = Header(self);
    const u32 width = (u32)header->width;
    if (x < width && z < (u32)(header->cellCount / (s32)width)) {
        const s16* cell = guest::ptr<const s16>(self->cells) + (x + z * width) * 2;
        if (cell[1] == -1)
            return;
        GuestMapNode* node = GuestIntMap_Find(&self->types, cell[1]);
        if (node == GuestIntMap_End(&self->types))
            return;
        memcpy(&self->current, guest::ptr(node->value), sizeof(ColCellType));
        if (self->current._0c == -1)
            self->current._0c = 0;
        if (self->eventMode != 0) {
            if (self->current.flags & kCellEventOnly) {
                self->current.flags &= ~kCellFlag1;
                self->current.flags |= kCellBlocked | kCellFlag100;
            }
            if (clear40 && (self->current.flags & kCellFlag40))
                self->current.flags &= ~kCellFlag40;
        }
    } else {                                        // off the grid
        self->current._16 = 0;
        self->current.flags |= kCellBlocked;
    }
}

// ── 0x16BA90 ──────────────────────────────────────────────────────────────
ColCellType* Collision_CellAt(Collision* self, const float* pos) {
    const ColHeader* header = Header(self);
    LoadCell(self, (u32)((int)(pos[0] * 100.0f / header->cellSize) + header->offsetX),
             (u32)((int)(pos[2] * 100.0f / header->cellSize) + header->offsetZ), false);
    return &self->current;
}

// ── 0x16BC00 ──────────────────────────────────────────────────────────────
ColCellType* Collision_CellAtIndex(Collision* self, int x, int z) {
    const ColHeader* header = Header(self);
    LoadCell(self, (u32)(x + header->offsetX), (u32)(z + header->offsetZ), true);
    return &self->current;
}

// ── 0x16B7C0 ──────────────────────────────────────────────────────────────
const float* Collision_LineBlocked(Collision* self, const float* from, const float* to, int kind) {
    const u32 mask = kind == 2 ? kCellFlag20 : kind == 1 ? kCellNoNpc : kind == 0 ? kCellBlocked : 0xFFFFFFFFu;
    float dir[4] = {to[0] - from[0], 0.0f, to[2] - from[2], to[3] - from[3]};
    float length = sqrtf(dir[0] * dir[0] + dir[2] * dir[2] + dir[3] * dir[3]);      // 0x136BA0 normalises
    if (length != 0.0f)
        for (float& d : dir)
            d /= length;
    if (length > 400.0f) {
        memcpy(sHitPoint, from, 16);
        return sHitPoint;
    }
    // Ten samples along the line first …
    for (int i = 0; i < 10; i++) {
        const float t = length * 0.1f * (float)(i + 1);
        for (int k = 0; k < 4; k++)
            sHitPoint[k] = dir[k] * t + from[k];
        if (Collision_CellAt(self, sHitPoint)->flags & mask)
            return sHitPoint;
    }
    // … then every cell it crosses.
    const float cellSize = Header(self)->cellSize;
    const float step = cellSize / 100.0f, toCell = 100.0f / cellSize;
    for (float& d : dir)
        d *= step;
    memcpy(sHitPoint, from, 16);
    int x = (int)(sHitPoint[0] * toCell), z = (int)(sHitPoint[2] * toCell);
    for (; step <= length; length -= step) {
        for (int k = 0; k < 4; k++)
            sHitPoint[k] += dir[k];
        const int nx = (int)(sHitPoint[0] * toCell), nz = (int)(sHitPoint[2] * toCell);
        if ((nx != x || nz != z) && (Collision_CellAtIndex(self, nx, nz)->flags & mask))
            return sHitPoint;
        x = nx;
        z = nz;
    }
    return nullptr;
}

// ══ The holder ════════════════════════════════════════════════════════════
// ── 0x1580D0 ──────────────────────────────────────────────────────────────
MapCollision* MapCollision_Ctor(MapCollision* self) {
    self->collision = 0;
    return self;
}

// ── 0x158070 ──────────────────────────────────────────────────────────────
MapCollision* MapCollision_Dtor(MapCollision* self, s16 flag) {
    if (!self)
        return self;
    if (self->collision)
        Collision_Dtor(guest::ptr<Collision>(self->collision), 1);
    self->collision = 0;
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x158010 ──────────────────────────────────────────────────────────────
void MapCollision_SetFile(MapCollision* self, const s32* file) {
    if (!file)
        return;
    void* mem = MemAlloc(sizeof(Collision));
    self->collision = mem ? guest::addr(Collision_Ctor(static_cast<Collision*>(mem), file)) : 0;
}

// ── 0x157DF0 ──────────────────────────────────────────────────────────────
void Map_SetEventMode(MapCollision* self, int eventMode) {
    if (self->collision)
        guest::ptr<Collision>(self->collision)->eventMode = eventMode;
}

// ── 0x157E70 ──────────────────────────────────────────────────────────────
GuestList* Map_FindPoints(MapCollision* self, int name) {
    if (self->collision == 0) {
        static bool made = false;                   // a function-local static in the original
        if (!made) {
            GuestList_Init(sNoPoints);
            made = true;
        }
        return sNoPoints;
    }
    return Collision_FindPoints(guest::ptr<Collision>(self->collision), name);
}

// ── 0x157F70 ──────────────────────────────────────────────────────────────
const float* Map_LineBlocked(MapCollision* self, const float* from, const float* to, int kind) {
    return self->collision ? Collision_LineBlocked(guest::ptr<Collision>(self->collision), from, to, kind) : nullptr;
}

// ── 0x157FA0 ──────────────────────────────────────────────────────────────
ColCellType* Map_CellAt(MapCollision* self, const float* pos) {
    if (self->collision == 0) {
        ColCellType* blank = guest::ptr<ColCellType>(0x308E00u);
        blank->height = 0.0f;
        blank->flags = kCellFlag1;
        blank->light[0] = blank->light[1] = blank->light[2] = 0x7F;
        return blank;
    }
    return Collision_CellAt(guest::ptr<Collision>(self->collision), pos);
}

// ── 0x157E50 ──────────────────────────────────────────────────────────────
const ColHeader* Map_Header(MapCollision* self) {
    return self->collision ? guest::ptr<const ColHeader>(guest::ptr<Collision>(self->collision)->header) : nullptr;
}
