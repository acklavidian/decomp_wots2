#pragma once
#include "hal/ps2_types.h"
#include "native/guest.h"

// ── Scene interface ───────────────────────────────────────────────────────
// Every scene has a CodeWarrior vtable pointer at +0x2C.  App_Run (0x1E09F0)
// drives the current scene through it.
struct Scene {
    u8  _pad00[0x28];
    u32 hasQueuedScene;   // +0x28  non-zero: scene wants its successor started early
    u32 vtable;           // +0x2C
};
static_assert(offsetof(Scene, vtable) == 0x2C, "Scene::vtable");

enum SceneSlot : u32 {
    kSceneDestroy        = 0x08,  // (self, 1)
    kSceneStart          = 0x0C,  // () → 0 on success
    kSceneUpdate         = 0x10,  // () → 0 when the scene has finished
    kSceneDraw           = 0x14,
    kSceneGetNextScene   = 0x18,  // () → Scene* or null
    kSceneGetNextSceneId = 0x1C,  // () → SceneId, or negative for none
};

// Virtual call.  The vtable holds PS2 code addresses, so the call goes through
// the native function registry (see native/guest.h).
template <typename R, typename... Args>
static inline R SceneCall(Scene* scene, SceneSlot slot, Args... args) {
    u32 fn = *guest::ptr<u32>(scene->vtable + slot);
    return GuestCall<R, Scene*, Args...>(fn, scene, args...);
}

// ── Scene ids accepted by Scene_Create (0x1E2670) ─────────────────────────
// Only the ids marked "top level" are ever returned by a GetNextSceneId
// method; the rest return -1 there and are reached some other way (not yet
// traced).  Names without a comment are placeholders.
enum SceneId : s32 {
    kSceneBoot       = 0,   // top level: does nothing, hands over to Title (or Attract)
    kScene01         = 1,
    kScene02         = 2,
    kScene03         = 3,
    kScene04         = 4,
    kScene05         = 5,
    kScene06         = 6,
    kScene07         = 7,
    kScene08         = 8,
    kScene09         = 9,
    kScene10         = 10,
    kScene11         = 11,
    kScene12         = 12,
    kScene13         = 13,
    kSceneGameplay   = 14,  // top level: the main game (→ Results, Boot or StaffRoll)
    kScene15         = 15,  // top level: → Boot, or itself
    kSceneModelView  = 16,  // top level: debug model viewer (→ Boot)
    kScene17         = 17,
    kScene18         = 18,
    kScene19         = 19,
    kScene20         = 20,
    kScene21         = 21,
    kSceneTitle      = 22,  // top level: front end, "GameScene" (→ Gameplay, Boot or Attract)
    kScene23         = 23,
    kScene24         = 24,
    kSceneResults    = 25,  // top level: runs after the staff roll (→ Boot)
    kScene26         = 26,
    kSceneStaffRoll  = 27,  // top level: plays MOVIE/*/STAFFn.PSS (→ Results)
    kSceneMovieView  = 28,  // plays MOVIE/OPENING.PSS style paths picked from a list
    kSceneAttract    = 29,  // top level: alternates OPENING.PSS / PLAYDEMO.PSS (→ Title)
    kSceneCount      = 30,
};

extern "C" Scene* Scene_Create(s32 id);  // 0x1E2670
