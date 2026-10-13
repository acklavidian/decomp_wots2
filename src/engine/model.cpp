// A2MF model objects — decompiled from 0x12CE90–0x12D1D0, 0x131E60–0x1329E0
// and 0x1332E0–0x1335F0.  See include/engine/model.h.

#include "engine/model.h"
#include "native/guest.h"

#include <cstring>

extern "C" {
    void  MemFree(void* p);                                             // 0x100650
    int   GsDevice_UploadTexture(GsDevice* dev, void* descriptors);     // 0x130390
}

static inline u32& U32(u32 addr) { return *guest::ptr<u32>(addr); }
static inline s16  S16(u32 addr) { return *guest::ptr<s16>(addr); }
static inline u16  U16(u32 addr) { return *guest::ptr<u16>(addr); }

// ══ Model (base) ══════════════════════════════════════════════════════════

// ── 0x132880 ──────────────────────────────────────────────────────────────
u8 Model_GetType(const void* file) {
    if (strncmp("A2MF", static_cast<const char*>(file), 4) == 0)
        return static_cast<const u8*>(file)[4];
    return 0xFF;
}

// ── 0x1329E0 ──────────────────────────────────────────────────────────────
Model* Model_Ctor(Model* self, void* file) {
    self->vtable = kVtModel;
    self->file = guest::addr(file);
    self->data = strncmp("A2MF", static_cast<const char*>(file), 4) == 0 ? guest::addr(file) + 0x10
                                                                          : guest::addr(file);
    self->relocated = 0;
    return self;
}

// ── 0x132990 ──────────────────────────────────────────────────────────────
static Model* Model_Dtor(Model* self, s16 flag) {
    if (self) {
        self->vtable = kVtModel;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// ── 0x1328E0 ──────────────────────────────────────────────────────────────
// A file is relocated in place once; byte 7 of its header remembers that.
static bool Model_IsRelocated(Model* self) {
    if (self->file != self->data && *guest::ptr<u8>(self->file + 7) != 0)
        self->relocated = 1;
    return self->relocated != 0;
}

// ── 0x132910 ──────────────────────────────────────────────────────────────
static void Model_MarkRelocated(Model* self) {
    if (self->file != self->data)
        *guest::ptr<u8>(self->file + 7) = 1;
    self->relocated = 1;
}

static void* Model_ReturnNull(Model*)       { return nullptr; }     // 0x132870, 0x131E50, 0x132960, 0x132950, 0x132940
static void  Model_Nothing(Model*)          {}                      // 0x132980, 0x132970

GUEST_BIND(0x132990, Model_Dtor);
GUEST_BIND(0x132870, Model_ReturnNull);
GUEST_BIND(0x132980, Model_Nothing);
GUEST_BIND(0x132910, Model_MarkRelocated);

// TEX0 for a texture descriptor plus a palette offset: TBP, TBW, PSM, TW, TH,
// then the palette's base pointer and "load the palette".
static u64 Tex0For(u32 descriptor, u32 clutOffset, bool withAlpha) {
    u64 tex0 = (u64)U16(descriptor + 4)
             | (u64)U16(descriptor + 6) << 14
             | (u64)*guest::ptr<u8>(descriptor + 2) << 20
             | (u64)U16(descriptor + 8) << 26
             | (u64)U16(descriptor + 10) << 30;
    tex0 |= (u64)(u32)(U16(descriptor + 0x1E) + clutOffset) << 37 | 0x2000000000000000ull;
    tex0 |= withAlpha ? 0x400000000ull : 0x400000000ull;    // both forms set TCC
    return tex0;
}

// Patches the TEX0 qword of one mesh material: {u64 tex0; u16 texture; u16 clut; …}.
static void BindMaterial(u32 material, u32 textures) {
    if (!material)
        return;
    u64 tex0 = Tex0For(textures + U16(material + 8) * 0x30u, U16(material + 10), false);
    memcpy(guest::ptr(material), &tex0, 8);
}

// The block of CPU-skinned parts shared by body and skinned models.
static void RelocateSkinBlock(u32 block) {
    for (u32 off = 0x0C; off <= 0x30; off += 4)
        U32(block + off) += block;
    u32 count = U16(block + 8) + U16(block + 10);
    for (u32 i = 0; i < count; i++) {
        u32& mesh = U32(block + 0x34 + i * 4);
        mesh += block;
        u32 strips = U16(mesh + 0x0E);
        for (u32 k = 0; k < strips; k++)
            U32(mesh + 0x10 + k * 4) += mesh;
    }
}

static void BindSkinBlock(u32 block, u32 textures) {
    u32 count = U16(block + 8) + U16(block + 10);
    for (u32 i = 0; i < count; i++)
        BindMaterial(U32(block + 0x34 + i * 4), textures);
}

// Bones: 0xA0 each, the parent index at +0x80 becomes a pointer (root: null).
static void RelocateBones(u32 bones, int count) {
    for (int i = 0; i < count; i++) {
        s32& parent = *guest::ptr<s32>(bones + i * 0xA0 + 0x80);
        parent = parent < 0 ? 0 : (s32)(bones + (u32)parent * 0xA0);
    }
}

// ══ BodyModel (type 0) ════════════════════════════════════════════════════
//   +0x02 s16 bones   +0x04 s16 meshCount[4]   +0x0C bones   +0x10 ?   +0x14 textures
//   +0x18 skin block  +0x1C mesh[]
//   mesh: u8[4], u32 materialCount, u32 size, u32 data, u32 material[]

Model* BodyModel_Ctor(Model* self, void* file) {
    Model_Ctor(self, file);
    self->vtable = kVtBodyModel;
    return self;
}

static u32 BodyMeshCount(u32 d) { return (u32)(S16(d + 4) + S16(d + 6) + S16(d + 8) + S16(d + 10)); }

// 0x12CE90
static Model* BodyModel_Dtor(Model* self, s16 flag) {
    if (self) {
        self->vtable = kVtBodyModel;
        Model_Dtor(self, 0);
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
static void* BodyModel_GetTextures(Model* self) { return guest::ptr(U32(self->data + 0x14)); }   // 0x12CF10
static void* BodyModel_Get10(Model* self)       { return guest::ptr(U32(self->data + 0x10)); }   // 0x12CF30
static s16   BodyModel_GetBoneCount(Model* self) { return S16(self->data + 2); }                 // 0x12CEF0
static void* BodyModel_GetBones(Model* self)    { return guest::ptr(U32(self->data + 0x0C)); }   // 0x12CF00
static void  BodyModel_SetTextures(Model* self, void* descriptors) {                             // 0x12CF20
    U32(self->data + 0x14) = guest::addr(descriptors);
}
// 0x12D3C0 (shared with SkinModel: both keep the pointer the device needs at +0x14)
static bool BodyModel_Upload(Model* self, GsDevice* dev) {
    return GsDevice_UploadTexture(dev, guest::ptr(U32(self->data + 0x14))) == 0;
}

// 0x12CF40
static bool BodyModel_Relocate(Model* self) {
    if (Model_IsRelocated(self))
        return false;
    const u32 d = self->data;
    if (U32(d + 0x0C)) U32(d + 0x0C) += d;
    if (U32(d + 0x18)) U32(d + 0x18) += d;
    if (U32(d + 0x0C)) RelocateBones(U32(d + 0x0C), S16(d + 2));
    if (U32(d + 0x10)) U32(d + 0x10) += d;
    if (U32(d + 0x14)) U32(d + 0x14) += d;
    for (u32 i = 0, n = BodyMeshCount(d); i < n; i++) {
        u32& mesh = U32(d + 0x1C + i * 4);
        mesh += d;
        U32(mesh + 0x0C) += mesh;
        const u32 data = U32(mesh + 0x0C);
        for (u32 k = 0, m = U32(mesh + 4); k < m; k++)
            U32(mesh + 0x10 + k * 4) += data;
    }
    if (U32(d + 0x18)) RelocateSkinBlock(U32(d + 0x18));
    Model_MarkRelocated(self);
    return true;
}

// 0x12D1D0
static bool BodyModel_BindTextures(Model* self) {
    const u32 d = self->data, textures = U32(d + 0x14);
    if (!textures)
        return false;
    for (u32 i = 0, n = BodyMeshCount(d); i < n; i++) {
        const u32 mesh = U32(d + 0x1C + i * 4);
        for (u32 k = 0, m = U32(mesh + 4); k < m; k++)
            BindMaterial(U32(mesh + 0x10 + k * 4), textures);
    }
    if (U32(d + 0x18)) BindSkinBlock(U32(d + 0x18), textures);
    return true;
}

GUEST_BIND(0x12CE90, BodyModel_Dtor);
GUEST_BIND(0x12CF10, BodyModel_GetTextures);
GUEST_BIND(0x12CF30, BodyModel_Get10);
GUEST_BIND(0x12CEF0, BodyModel_GetBoneCount);
GUEST_BIND(0x12CF00, BodyModel_GetBones);
GUEST_BIND(0x12CF20, BodyModel_SetTextures);
GUEST_BIND(0x12D3C0, BodyModel_Upload);
GUEST_BIND(0x12CF40, BodyModel_Relocate);
GUEST_BIND(0x12D1D0, BodyModel_BindTextures);

// ══ SkinModel (type 1) ════════════════════════════════════════════════════
//   +0x02 s16 bones   +0x04 s16 meshCount[2]   +0x08 bones   +0x0C ?   +0x10 textures
//   +0x14 skin block  +0x18 mesh[]
//   mesh: u8[4], u32 size, u32 data, u32 material (relative to data, 0 = none)

Model* SkinModel_Ctor(Model* self, void* file) {
    BodyModel_Ctor(self, file);
    self->vtable = kVtSkinModel;
    return self;
}

static u32 SkinMeshCount(u32 d) { return (u32)(S16(d + 4) + S16(d + 6)); }

// 0x1332E0
static Model* SkinModel_Dtor(Model* self, s16 flag) {
    if (self) {
        self->vtable = kVtBodyModel;
        Model_Dtor(self, 0);
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
static void* SkinModel_GetTextures(Model* self) { return guest::ptr(U32(self->data + 0x10)); }   // 0x133370
static void* SkinModel_Get0C(Model* self)       { return guest::ptr(U32(self->data + 0x0C)); }   // 0x133380
static s16   SkinModel_GetBoneCount(Model* self) { return S16(self->data + 2); }                 // 0x133350
static void* SkinModel_GetBones(Model* self)    { return guest::ptr(U32(self->data + 8)); }      // 0x133360

// 0x133390
static bool SkinModel_Relocate(Model* self) {
    if (Model_IsRelocated(self))
        return false;
    const u32 d = self->data;
    if (U32(d + 0x08)) U32(d + 0x08) += d;
    if (U32(d + 0x14)) U32(d + 0x14) += d;
    if (U32(d + 0x08)) RelocateBones(U32(d + 0x08), S16(d + 2));
    if (U32(d + 0x0C)) U32(d + 0x0C) += d;
    if (U32(d + 0x10)) U32(d + 0x10) += d;
    for (u32 i = 0, n = SkinMeshCount(d); i < n; i++) {
        u32& mesh = U32(d + 0x18 + i * 4);
        mesh += d;
        U32(mesh + 8) += mesh;
        if (U32(mesh + 0x0C))
            U32(mesh + 0x0C) += U32(mesh + 8);
    }
    if (U32(d + 0x14)) RelocateSkinBlock(U32(d + 0x14));
    Model_MarkRelocated(self);
    return true;
}

// 0x1335F0
static bool SkinModel_BindTextures(Model* self) {
    const u32 d = self->data, textures = U32(d + 0x10);
    if (!textures)
        return false;
    for (u32 i = 0, n = SkinMeshCount(d); i < n; i++)
        BindMaterial(U32(U32(d + 0x18 + i * 4) + 0x0C), textures);
    if (U32(d + 0x14)) BindSkinBlock(U32(d + 0x14), textures);
    return true;
}

GUEST_BIND(0x1332E0, SkinModel_Dtor);
GUEST_BIND(0x133370, SkinModel_GetTextures);
GUEST_BIND(0x133380, SkinModel_Get0C);
GUEST_BIND(0x133350, SkinModel_GetBoneCount);
GUEST_BIND(0x133360, SkinModel_GetBones);
GUEST_BIND(0x133390, SkinModel_Relocate);
GUEST_BIND(0x1335F0, SkinModel_BindTextures);

// ══ MapModel (type 3) ═════════════════════════════════════════════════════
//   +0x20 s32 nx, ny, nz   +0x2C textures   +0x30 cell[]: {u32 offset, u32 size}
//   cell: s16 meshCount[2], u32 mesh[]; meshes as in SkinModel

static void MapModel_Init(MapModel* self) {             // 0x1327D0
    self->_10 = 0;
    self->scratch = 0;
}
static void MapModel_Term(MapModel* self) {             // 0x132790
    if (self->scratch) {
        MemFree(guest::ptr(self->scratch));
        self->scratch = 0;
    }
}

// 0x1327E0
MapModel* MapModel_Ctor(MapModel* self, void* file) {
    Model_Ctor(&self->base, file);
    self->base.vtable = kVtMapModel;
    VirtualCall<void, MapModel*>(self->base.vtable, kModelInit, self);
    return self;
}

// 0x131E60
static MapModel* MapModel_Dtor(MapModel* self, s16 flag) {
    if (self) {
        self->base.vtable = kVtMapModel;
        VirtualCall<void, MapModel*>(self->base.vtable, kModelTerm, self);
        Model_Dtor(&self->base, 0);
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
static void* MapModel_GetTextures(Model* self) { return guest::ptr(U32(self->data + 0x2C)); }   // 0x131EE0
static bool  MapModel_Upload(Model* self, GsDevice* dev) {                                      // 0x132760
    return GsDevice_UploadTexture(dev, guest::ptr(U32(self->data + 0x2C))) == 0;
}

static u32 MapCellCount(u32 d) { return U32(d + 0x20) * U32(d + 0x24) * U32(d + 0x28); }

// 0x1324E0 — a cell's size word is cleared once the cell has been relocated
static bool MapModel_Relocate(Model* self) {
    if (Model_IsRelocated(self))
        return false;
    const u32 d = self->data;
    if (U32(d + 0x2C)) U32(d + 0x2C) += d;
    for (u32 i = 0, n = MapCellCount(d); i < n; i++) {
        const u32 entry = d + 0x30 + i * 8;
        if (U32(entry + 4) == 0)
            continue;
        U32(entry) += d;
        const u32 cell = U32(entry);
        const u32 meshes = (u32)(S16(cell) + S16(cell + 2));
        for (u32 k = 0; k < meshes; k++) {
            u32& mesh = U32(cell + 4 + k * 4);
            mesh += cell;
            U32(mesh + 8) += mesh;
            if (U32(mesh + 0x0C))
                U32(mesh + 0x0C) += U32(mesh + 8);
        }
        U32(entry + 4) = 0;
    }
    Model_MarkRelocated(self);
    return true;
}

// 0x132600
static bool MapModel_BindTextures(Model* self) {
    const u32 d = self->data, textures = U32(d + 0x2C);
    if (!textures)
        return false;
    for (u32 i = 0, n = MapCellCount(d); i < n; i++) {
        const u32 cell = U32(d + 0x30 + i * 8);
        if (!cell)
            continue;
        const u32 meshes = (u32)(S16(cell) + S16(cell + 2));
        for (u32 k = 0; k < meshes; k++)
            BindMaterial(U32(U32(cell + 4 + k * 4) + 0x0C), textures);
    }
    return true;
}

GUEST_BIND(0x1327D0, MapModel_Init);
GUEST_BIND(0x132790, MapModel_Term);
GUEST_BIND(0x131E60, MapModel_Dtor);
GUEST_BIND(0x131EE0, MapModel_GetTextures);
GUEST_BIND(0x132760, MapModel_Upload);
GUEST_BIND(0x1324E0, MapModel_Relocate);
GUEST_BIND(0x132600, MapModel_BindTextures);
