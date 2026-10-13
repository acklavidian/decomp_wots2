// The scenes that only play a movie — decompiled from 0x2BB9C0–0x2BC0A0.
//
// Staff roll (27): the ending's credits, one of four movies — STAFF0 for a
// total score under 3000, else STAFF1–3 by the ending reached — and then the
// Results scene.  Attract (29): what the title screen falls back to when
// left alone, OPENING and PLAYDEMO in turn, then the title again.
// Movie_Play itself is hardware (native/movie.cpp).

#include "engine/bgm.h"
#include "engine/gs_device.h"
#include "engine/scene.h"
#include "game/player_data.h"
#include "native/guest.h"

#include <cstring>

extern "C" {
    void* Scene_BaseCtor(void* mem);                    // 0x1E25F0
    void  Scene_BaseDtor(void* self, int flag);         // 0x1E2590
    void  MemFree(void* p);                             // 0x100650
    int   Movie_Play(const char* path);                 // 0x1613D0
    void  GsDevice_PresentMode(GsDevice* dev, u32 a, u32 b, u32 on, u32 c);  // 0x131A80
}
void App_PresentBlackFrames();                          // 0x1E0660

namespace {

const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);
u32* const gDisplayArg = guest::ptr<u32>(0x2EE4C8u);
u32* const gAttractTurn = guest::ptr<u32>(0x2EE7BCu);    // which of the two movies comes next
const u32* const kMoviesNtsc = guest::ptr<u32>(0x2D1B20u);  // ACQUIRE, SPIKE, OPENING, STAFF0–3, PLAYDEMO
const u32* const kMoviesPal = guest::ptr<u32>(0x2D1B40u);
enum : u32 { kVtStaffRoll = 0x2ED920u, kVtAttract = 0x2ED960u };

// 0x1E1AF0: two black frames, the display switched to (or back from) the
// movie's mode, two more.
void MovieMode(bool on) {
    App_PresentBlackFrames();
    GsDevice_PresentMode(*gGsDevice, 1, *gDisplayArg, on ? 1 : 0, 0);
    App_PresentBlackFrames();
}
const char* Movie(int index) {
    return guest::ptr<const char>((*gFrameRate == 25 ? kMoviesPal : kMoviesNtsc)[index]);
}

int Movie_Start(Scene*) {                               // 0x2BBAB0, 0x2BBFE0
    App_PresentBlackFrames();
    return 0;
}
int Movie_Draw(Scene*) {                                // 0x2BB9E0, 0x2BBF60
    App_PresentBlackFrames();
    return 0;
}
Scene* Movie_GetNextScene(Scene*) { return nullptr; }   // 0x2BB9D0, 0x2BBF50
void* Movie_Destroy(Scene* self, int flag) {            // 0x2BBAD0, 0x2BC000
    if (self != nullptr) {
        Scene_BaseDtor(self, 0);
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}

int StaffRoll_Update(Scene* self) {                     // 0x2BBA00
    MovieMode(true);
    const u8* data = guest::ptr<u8>(*guest::ptr<u32>(guest::addr(self) + 0x14));    // the base's player data
    s32 score;
    s16 ending;
    memcpy(&score, data + 8, 4);
    memcpy(&ending, data + 0x868, 2);
    const int which = score >= 3000 ? ending % 3 + 1 : 0;
    Movie_Play(Movie(3 + which));
    MovieMode(false);
    return 0;
}
s32 StaffRoll_GetNextSceneId(Scene*) { return kSceneResults; }  // 0x2BB9C0

int Attract_Update(Scene*) {                            // 0x2BBF80
    MovieMode(true);
    const bool demo = *gAttractTurn != 0;
    Movie_Play(Movie(demo ? 7 : 2));
    *gAttractTurn = !demo;
    MovieMode(false);
    return 0;
}
s32 Attract_GetNextSceneId(Scene*) { return kSceneTitle; }      // 0x2BBF40

}  // namespace

extern "C" {

Scene* StaffRollScene_Ctor(void* mem) {                 // 0x2BBB30
    Scene* self = static_cast<Scene*>(Scene_BaseCtor(mem));
    self->vtable = kVtStaffRoll;
    Bgm_StopAll(0);
    return self;
}
Scene* AttractScene_Ctor(void* mem) {                   // 0x2BC060
    Scene* self = static_cast<Scene*>(Scene_BaseCtor(mem));
    self->vtable = kVtAttract;
    Bgm_StopAll(0);
    return self;
}

// Bound from hle::bind_all (nothing else refers to this file).
void MovieScenes_Bind() {
    const auto bind = [](u32 address, auto fn, const char* name) {
        guest::bind(address, reinterpret_cast<void*>(fn), name);
    };
    bind(0x2BBB30, StaffRollScene_Ctor, "StaffRollScene_Ctor");
    bind(0x2BBAD0, Movie_Destroy, "StaffRollScene_Destroy");
    bind(0x2BBAB0, Movie_Start, "StaffRollScene_Start");
    bind(0x2BBA00, StaffRoll_Update, "StaffRollScene_Update");
    bind(0x2BB9E0, Movie_Draw, "StaffRollScene_Draw");
    bind(0x2BB9D0, Movie_GetNextScene, "StaffRollScene_GetNextScene");
    bind(0x2BB9C0, StaffRoll_GetNextSceneId, "StaffRollScene_GetNextSceneId");
    bind(0x2BC060, AttractScene_Ctor, "AttractScene_Ctor");
    bind(0x2BC000, Movie_Destroy, "AttractScene_Destroy");
    bind(0x2BBFE0, Movie_Start, "AttractScene_Start");
    bind(0x2BBF80, Attract_Update, "AttractScene_Update");
    bind(0x2BBF60, Movie_Draw, "AttractScene_Draw");
    bind(0x2BBF50, Movie_GetNextScene, "AttractScene_GetNextScene");
    bind(0x2BBF40, Attract_GetNextSceneId, "AttractScene_GetNextSceneId");
}

}  // extern "C"
