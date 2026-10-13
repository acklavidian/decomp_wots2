#pragma once
#include "hal/ps2_types.h"

// A2MF model objects — decompiled from 0x12CE90–0x12D1D0, 0x131E60–0x1329E0
// and 0x1332E0–0x1335F0.  The file layouts are described in the project notes.
//
//   Model       0x2EA940   base: holds the file, knows nothing of its contents
//   ├─ BodyModel   0x2EA8D0   type 0: ordinary character bodies
//   ├─ SkinModel   0x2EA970   type 1: heads, heroes, sky
//   └─ MapModel    0x2EA910   type 3: maps
//
// The draw methods (slot +0x1C, and the map's own) build VIF packets for VU1
// and are not decompiled; they are where the native renderer takes over.

struct GsDevice;

struct Model {
    u32 data;           // +0x00  the file past its 16-byte header
    u32 file;           // +0x04
    u8  relocated;      // +0x08
    u32 vtable;         // +0x0C
};

struct MapModel {
    Model base;
    u32   _10;
    u8    _14[0x7000];  // per-cell draw state (not decompiled)
    u32   scratch;      // +0x7014
};

enum ModelSlot : u32 {
    kModelDtor         = 0x08,  // (self, s16 flag)
    kModelGetTextures  = 0x0C,  // (self) → .Pim descriptors
    kModelInit         = 0x14,
    kModelTerm         = 0x18,
    kModelDraw         = 0x1C,  // (self, GsDevice*)
    kModelUpload       = 0x20,  // (self, GsDevice*) → bool
    kModelBindTextures = 0x24,  // (self) → bool
    kModelRelocate     = 0x28,  // (self) → bool
    kModelGetBoneCount = 0x2C,
    kModelGetBones     = 0x30,
    kModelSetTextures  = 0x34,  // (self, descriptors)
};

enum : u32 {
    kVtModel = 0x2EA940u, kVtBodyModel = 0x2EA8D0u, kVtSkinModel = 0x2EA970u, kVtMapModel = 0x2EA910u,
};

extern "C" {
    // The type byte of an A2MF file (0 body, 1 skinned, 3 map), or 0xFF.
    u8     Model_GetType(const void* file);                         // 0x132880
    Model* Model_Ctor(Model* self, void* file);                     // 0x1329E0
    MapModel* MapModel_Ctor(MapModel* self, void* file);            // 0x1327E0
    // Constructors the game inlines wherever it creates one:
    Model* BodyModel_Ctor(Model* self, void* file);
    Model* SkinModel_Ctor(Model* self, void* file);
}
