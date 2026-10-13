// Decompiled from 0x1DB7A0 – 0x1DB8F0 (BootScene) and 0x1E24C0 – 0x1E2660 (scene base)
//
// The base every scene starts with, and scene 0 — the empty "Boot" scene that
// App_Init creates.  Boot finishes on its first frame and names the title
// scene (or the attract movies) as its successor.

#include "game/game_scene.h"
#include "engine/pad.h"

// ── External functions ─────────────────────────────────────────────────────
extern "C" {
    void  MemFree(void* p);                                 // 0x100650
    void  PadMgr_Poll(void* mgr);                           // 0x13A7B0
    void  PadMgr_Reset(void* mgr);                          // 0x13A820
    void  Bgm_StopAll(int fade);                            // 0x180350
    void  Sound_PlaySE(u32 id, u32 randomPitch, u32 volume);// 0x180E70
    int   RenderMgr_FindEffect(void* mgr, u32 type);        // 0x14F140  non-zero while it exists
    void  fn_14e7e0(void* mgr);                             // 0x14E7E0
    void  fn_21d8f0(void* obj);                             // 0x21D8F0
    void  Display_ApplyOffset(int arg);                               // 0x1E0450
    void  Config_SetDefaults(void* config);                          // 0x183700
    void  Sound_ApplyConfig();                                      // 0x17FFA0
}

// ── Globals ───────────────────────────────────────────────────────────────
static u32* const   gGsDevice     = reinterpret_cast<u32*>(0x2EE670u);
static u32* const   gFont         = reinterpret_cast<u32*>(0x2EE67Cu);
static u32* const   gWindowMgr    = reinterpret_cast<u32*>(0x2EE680u);
static u32* const   gRenderMgrPtr = reinterpret_cast<u32*>(0x2EE684u);
static u32* const   gPlayerData   = reinterpret_cast<u32*>(0x2EE688u);
static u32* const   gConfig       = reinterpret_cast<u32*>(0x2EE68Cu);
static u32* const   gField20Src   = reinterpret_cast<u32*>(0x2EE69Cu);
static u32* const   gProfiler     = reinterpret_cast<u32*>(0x2EE6B0u);
static const s32* const gFrameRate = reinterpret_cast<const s32*>(0x2EDA00u);
static u32* const   gSeVolume     = reinterpret_cast<u32*>(0x2EDAD0u);
static u32* const   gGoAttract    = reinterpret_cast<u32*>(0x2EDD10u);
static float* const gAspect       = reinterpret_cast<float*>(0x2EDD30u);

static const u32 kSceneBaseVtable = 0x2EBB00u;
static const u32 kBootSceneVtable = 0x2EBA10u;

template <typename T = void>
static T* P(u32 addr) { return reinterpret_cast<T*>(static_cast<uintptr_t>(addr)); }
static u32 A(void* p) { return static_cast<u32>(reinterpret_cast<uintptr_t>(p)); }

extern "C" {

// ── 0x1E25F0 Scene_BaseCtor ───────────────────────────────────────────────
void* Scene_BaseCtor(void* mem) {
    SceneBase* self = static_cast<SceneBase*>(mem);
    self->vtable         = kSceneBaseVtable;
    self->gsDevice       = *gGsDevice;
    self->padMgr         = A(PadMgr_Get());
    self->profiler       = *gProfiler;
    self->font           = *gFont;
    self->windowMgr      = *gWindowMgr;
    self->playerData     = *gPlayerData;
    self->config         = *gConfig;
    self->renderMgr      = *gRenderMgrPtr;
    self->field_20       = *gField20Src;
    self->softResetHold  = 0;
    self->hasQueuedScene = 0;
    return self;
}

// ── 0x1E2580 Scene_BaseDtor ───────────────────────────────────────────────
void* Scene_BaseDtor(void* mem, int flag) {
    SceneBase* self = static_cast<SceneBase*>(mem);
    if (!self)
        return self;
    self->vtable = kSceneBaseVtable;
    PadMgr_Poll(P(self->padMgr));
    PadMgr_Reset(P(self->padMgr));
    fn_21d8f0(P(self->field_20));
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

// ── 0x1E24C0 Scene_CheckSoftReset ─────────────────────────────────────────
// Start + Select + L1 + R1 held for two seconds, while no fade is running.
int Scene_CheckSoftReset(void* mem) {
    SceneBase* self = static_cast<SceneBase*>(mem);

    if (RenderMgr_FindEffect(P(*gRenderMgrPtr), 11) == 0 &&
        (Pad_GetHeld(P(self->padMgr), 0) & kPadSoftReset) == kPadSoftReset)
        self->softResetHold++;
    else
        self->softResetHold = 0;

    bool reset = (s32)self->softResetHold > *gFrameRate * 2;
    if (reset) {
        Bgm_StopAll(1);
        Sound_PlaySE(5, 0, 0x3FFF);
        fn_14e7e0(P(*gRenderMgrPtr));
        *gSeVolume = *P<u32>(self->config);
        self->softResetHold = 0;
    }
    return reset;
}

}  // extern "C"

// ─────────────────────────────────────────────────────────────────────────
// BootScene (scene 0, 52 bytes)
// ─────────────────────────────────────────────────────────────────────────
struct BootScene {
    SceneBase base;
    s32 field_30;
};

// ── 0x1DB8B0 BootScene_Ctor ───────────────────────────────────────────────
BootScene* BootScene_Ctor(BootScene* self) {
    Scene_BaseCtor(self);
    self->base.vtable = kBootSceneVtable;
    self->field_30 = -1;
    return self;
}

// ── 0x1DB850 BootScene_Destroy ────────────────────────────────────────────
BootScene* BootScene_Destroy(BootScene* self, s16 flag) {
    if (!self)
        return self;
    self->base.vtable = kBootSceneVtable;
    Scene_BaseDtor(self, 0);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x1DB800 BootScene_Start ──────────────────────────────────────────────
int BootScene_Start(BootScene* self) {
    *gAspect = 1.0f;
    Display_ApplyOffset(1);
    Config_SetDefaults(P(self->base.config));
    Sound_ApplyConfig();
    return 0;
}

// ── 0x1DB7F0 / 0x1DB7E0 / 0x1DB7D0 ────────────────────────────────────────
int    BootScene_Update(BootScene*)       { return 0; }
int    BootScene_Draw(BootScene*)         { return 0; }
Scene* BootScene_GetNextScene(BootScene*) { return nullptr; }

// ── 0x1DB7A0 BootScene_GetNextSceneId ─────────────────────────────────────
s32 BootScene_GetNextSceneId(BootScene*) {
    if (*gGoAttract == 0)
        return kSceneTitle;
    *gGoAttract = 0;
    return kSceneAttract;
}
