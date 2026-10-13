// Stand-ins for what the task system (src/game/task.cpp, task_mgr.cpp) calls
// that is not decompiled:
//
// the talk camera, the line-of-sight test and a few one-liners.  (What task
// commands leave running is decompiled: src/game/task_actions.cpp.)

#include "game/hud.h"
#include "game/map_collision.h"
#include "game/map_db.h"
#include "game/player_data.h"
#include "game/task_mgr.h"
#include "native/guest.h"
#include "native/hle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
    int   Rand_Next();                                              // 0x1200F0
    u32   MapState_GetMap(void* self);                              // 0x14B640
    void  Matrix_FaceTowards(void* matrix, const void* target);     // 0x1C8DB0
    int   CharaMgr_IsActive(CharaMgr* mgr, int id);                 // 0x146CB0
}

namespace {

template <typename T = void> inline T* P(u32 a) { return guest::ptr<T>(a); }
const PlayerData* player() { return P<PlayerData>(*P<u32>(0x2EE688u)); }

int resolve(s32 who, int self, const TaskCommand* cmd, CharaMgr* charas) {
    switch (who) {
    case kWhoOther:   return cmd->other;
    case kWhoPartner: {
        Chara* c = charas ? CharaMgr_Find(charas, self) : nullptr;
        return c && c->partner != -1 ? c->partner : self;
    }
    case kWhoTarget:  return cmd->target;
    case kWhoPlayer:  return 0;
    case kWhoSelf:    return self;
    default:          return who;
    }
}

}  // namespace

extern "C" {

// ── The camera on two people talking (0x18BFA0): the stand-in camera
// keeps following the player. ─────────────────────────────────────────────

// ── 0x175600: can two characters see each other?  The real one caches the
// answer per pair for the frame; the test itself is the map's (decompiled). ──
int Collision_CanSee(int idA, int idB, int) {
    CharaMgr* charas = P<CharaMgr>(*P<u32>(0x2EE5C4u));
    void* mapState = P(*P<u32>(0x2EE5C8u));
    if (!charas || !mapState)
        return 0;
    if (!CharaMgr_IsActive(charas, idA) || !CharaMgr_IsActive(charas, idB))
        return 0;
    MapCollision* map = P<MapCollision>(MapState_GetMap(mapState));
    if (!map)
        return 1;
    return Map_LineBlocked(map, CharaMgr_GetPosition(charas, idA), CharaMgr_GetPosition(charas, idB), 0) == nullptr;
}

// ── One-liners ────────────────────────────────────────────────────────────

}  // extern "C"
