// Decompiled from 0x1E2670 – 0x1E2A74
//
// Scene factory.  The original is a 30-way switch (jump table at 0x2E4530)
// whose cases are all identical in shape:
//
//     mem = MemAlloc(size);  return mem ? Ctor(mem) : null;
//
// so it is written here as a table.  Each constructor stores the scene's
// vtable pointer at +0x2C.

#include "engine/scene.h"

extern "C" void* MemAlloc(u32 size);  // 0x100820

struct SceneDesc {
    u32 size;     // bytes passed to MemAlloc
    u32 ctor;     // constructor address: Scene* (*)(void* mem)
    u32 vtable;   // vtable the constructor installs (for reference)
};

static const SceneDesc kScenes[kSceneCount] = {
    /*  0 Boot      */ {  52, 0x1DB8B0, 0x2EBA10 },
    /*  1           */ { 112, 0x27DB30, 0x2ECED0 },
    /*  2           */ {  72, 0x2802B0, 0x2ECF40 },
    /*  3           */ {  80, 0x1DAEB0, 0x2EB9D0 },
    /*  4           */ {  84, 0x1DFC00, 0x2EBAC0 },
    /*  5           */ {  48, 0x1DA560, 0x2EB9B0 },
    /*  6           */ {  60, 0x1DF780, 0x2EBA80 },
    /*  7           */ {  56, 0x1DB760, 0x2EB9F0 },
    /*  8           */ { 348, 0x27D230, 0x2ECEB0 },
    /*  9           */ {  52, 0x27DE80, 0x2ECEF0 },
    /* 10           */ { 768, 0x1DBFA0, 0x2EBA30 },
    /* 11           */ {  84, 0x1D94C0, 0x2EB970 },
    /* 12           */ {  64, 0x1DA400, 0x2EB990 },
    /* 13           */ {  52, 0x1DF980, 0x2EBAA0 },
    /* 14 Gameplay  */ { 240, 0x2B2230, 0x2ED710 },
    /* 15           */ {  72, 0x27F200, 0x2ECF10 },
    /* 16 ModelView */ {  72, 0x1E03F0, 0x2EBAE0 },
    /* 17           */ {  60, 0x1D8A70, 0x2EB950 },
    /* 18           */ {  64, 0x227AF0, 0x2EC5F0 },
    /* 19           */ {  88, 0x2815F0, 0x2ECF60 },
    /* 20           */ { 332, 0x239430, 0x2EC6B0 },
    /* 21           */ {  80, 0x27BC20, 0x2ECE90 },
    /* 22 Title     */ { 104, 0x275380, 0x2ECDB0 },
    /* 23           */ {  76, 0x27B0A0, 0x2ECE70 },
    /* 24           */ {  64, 0x29A320, 0x2ED1E0 },
    /* 25 Results   */ {  64, 0x2A3460, 0x2ED5E0 },
    /* 26           */ { 104, 0x2A61F0, 0x2ED620 },
    /* 27 StaffRoll */ {  48, 0x2BBB30, 0x2ED920 },
    /* 28 MovieView */ {  52, 0x2BBE80, 0x2ED940 },
    /* 29 Attract   */ {  48, 0x2BC060, 0x2ED960 },
};

// ── 0x1E2670 Scene_Create ─────────────────────────────────────────────────
Scene* Scene_Create(s32 id) {
    if ((u32)id >= kSceneCount)
        return nullptr;

    const SceneDesc& desc = kScenes[id];
    void* mem = MemAlloc(desc.size);
    if (!mem)
        return nullptr;
    return GuestCall<Scene*, void*>(desc.ctor, mem);
}
