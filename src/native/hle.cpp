// Native stand-ins for engine functions that are not decompiled yet.
//
// The decompiled code calls these by name.  Each one either does the job
// natively (memory, pad, sprites, fades), or is an honest stub that does
// nothing and says so once.  As functions get decompiled their stand-ins
// here should be deleted.
//
// Also here: the table that binds PS2 code addresses (as they appear in
// guest vtables) to the native functions that implement them.

#include "engine/window.h"
#include "native/hle.h"
#include "engine/font.h"
#include "native/archive.h"
#include "native/guest.h"
#include "native/editor.h"
#include "native/sprites.h"
#include "engine/model.h"
#include "engine/pad.h"
#include "engine/scene.h"
#include "game/chara.h"
#include "game/chara_motions.h"
#include "game/chara_resources.h"
#include "game/game_scene.h"
#include "game/gameplay_scene.h"
#include "game/hud.h"
#include "game/player_data.h"
#include "native/model.h"
#include "native/mods.h"
#include "hal/gs.h"
#include "hal/pad.h"
#include "hal/spu2.h"

#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// The text roll (render effect 0x1E; src/game/text_roll.cpp).
void TextRoll_Update();
void TextRoll_Draw();
void TextRoll_Stop();
extern "C" void Hud_Reset(void* hud);
bool TextRoll_Active();

extern "C" void* Scene_BaseCtor(void* mem);   // scene_base.cpp
extern "C" void  Config_SetDefaults(void* config);   // engine/config.cpp
extern "C" void  ResultsScene_Bind();                // game/results_scene.cpp
extern "C" void  MovieScenes_Bind();                 // game/movie_scenes.cpp
extern "C" void  McScreens_Bind();                   // game/mcard.cpp

namespace {

// ── Guest globals this layer owns ─────────────────────────────────────────
u32* const gGsDevice      = guest::ptr<u32>(0x2EE670);
u32* const gWindowMgr     = guest::ptr<u32>(0x2EE680);
u32* const gRenderMgrPtr  = guest::ptr<u32>(0x2EE684);
u32* const gPlayerData    = guest::ptr<u32>(0x2EE688);
u32* const gConfig        = guest::ptr<u32>(0x2EE68C);
u32* const gClearColor    = guest::ptr<u32>(0x2EE6A8);
s32* const gFadeOutFrames = guest::ptr<s32>(0x2EE76C);
s32* const gScreenW       = guest::ptr<s32>(0x2EE4C0);
s32* const gScreenH       = guest::ptr<s32>(0x2EE4C4);
u32* const gPadMgr        = guest::ptr<u32>(0x2EE4DC);
u32* const gFrameRate     = guest::ptr<u32>(0x2EDA00);

// Per-port pad state, 0x80 bytes each.
constexpr u32 kPadState = 0x307140;
struct PadFields { u16* held; u16* pressed; u16* repeat; u16* newPress; };
PadFields pad_fields(int port) {
    u8* base = guest::ptr<u8>(kPadState + port * 0x80);
    return { reinterpret_cast<u16*>(base + 0x08), reinterpret_cast<u16*>(base + 0x0C),
             reinterpret_cast<u16*>(base + 0x14), reinterpret_cast<u16*>(base + 0x16) };
}

// GS primitive coordinates of the screen's top-left corner.  The game adds
// these through Screen_X / Screen_Y and the sprite functions take them back off.
constexpr int kOriginX = 2048 - 320;
constexpr int kOriginY = 2048 - 224;

void note_once(const char* what) {
    static std::set<std::string> seen;
    if (seen.insert(what).second)
        printf("[stub] %s\n", what);
}

// ── Full-screen fade (render effect 11) ───────────────────────────────────
// (Class 0x17FF50, vtable 0x2EB070.)  Not a plain fade: black wipes across
// from one side, over 15 frames unless told otherwise.  Modes: 0 clearing
// (the effect is gone when done), 1 covering (→ 2 when done), 2 covered,
// 3 clear but still there.
struct Fade {
    bool exists = false;
    u8   mode = 0;
    s32  timer = 0, total = 15;
    s32 level() const {
        const s32 part = total != 0 ? (timer << 7) / total : 0;
        return mode == 1 ? 0x80 - part : mode == 0 ? part : mode == 3 ? 0 : 0x80;
    }
} g_fade;

// ── Render effect 12 (class 0x17F2E0): mode 0 nothing, 1 covered, 2 clearing, 3 covering
struct Wash {
    u8 mode = 0;
    s32 timer = 0, total = 0;
} g_wash;

// ── Sprite sets ───────────────────────────────────────────────────────────
// The guest-side handle is the 0x14 bytes the game allocates.  Its fields are
// kept the way the game leaves them once a set has loaded, because decompiled
// code reads them: [0] non-zero (here: index into g_sheets), [2] texture
// descriptors, [3] sprite records, [4] sprite count.
struct SheetEntry {
    sprites::Sheet* sheet = nullptr;
    u32 blob = 0;              // the file, copied into guest memory
    int loadFrame = 0;
    std::vector<u32> tbps;     // GS texture slots it holds
};
std::vector<SheetEntry> g_sheets{{}};

// Textures in "GS memory", by texture base pointer.  The real device hands
// out VRAM and writes the base pointers into the descriptors; here they are
// just unique numbers.
struct GsTexture { sprites::Sheet* sheet; int index; u32 cbp; };
std::map<u32, GsTexture> g_gsTextures;
std::vector<u32> g_freeTbps;
u32 g_nextTbp = 1;
constexpr u16 kCbpBase = 0x1000;

// Gives every descriptor of a .Pim (in guest memory) a texture slot.
void register_textures(sprites::Sheet* sheet, u8* descriptors, std::vector<u32>* slots) {
    for (size_t i = 0; i < sheet->textures.size(); i++) {
        u32 tbp;
        if (!g_freeTbps.empty()) { tbp = g_freeTbps.back(); g_freeTbps.pop_back(); }
        else tbp = g_nextTbp++;
        u16 tbp16 = (u16)tbp, cbp = kCbpBase;
        memcpy(descriptors + i * 0x30 + 4, &tbp16, 2);
        memcpy(descriptors + i * 0x30 + 0x1E, &cbp, 2);
        g_gsTextures[tbp] = {sheet, (int)i, cbp};
        if (slots) slots->push_back(tbp);
    }
}

// Open archive files; index 0 is unused.
std::vector<std::vector<u8>> g_files{{}};

sprites::Sheet* sheet_of(void* set) {
    if (!set)
        return nullptr;
    u32 index = *static_cast<u32*>(set);
    return index < g_sheets.size() ? g_sheets[index].sheet : nullptr;
}

void draw_sprite(sprites::Sheet* sheet, int index, float x, float y, float w, float h,
                 bool flipU, bool flipV, u32 rgba) {
    if (!sheet || index < 0 || index >= (int)sheet->rects.size())
        return;
    const sprites::Rect& r = sheet->rects[index];
    u32 tex = sheet->texture_for(r);
    if (!tex)
        return;
    const sprites::Texture& t = sheet->textures[r.texture];
    float u0 = r.u, v0 = r.v, u1 = r.u + r.w, v1 = r.v + r.h;
    if (flipU) std::swap(u0, u1);
    if (flipV) std::swap(v0, v1);
    hal::gs::draw_sprite(tex, t.width, t.height, x, y, w, h, u0, v0, u1, v1, rgba);
}

// ── Scenes that are not decompiled ────────────────────────────────────────
// Creating one gives this placeholder: it reports itself, ends at once and
// sends the game back to the Boot scene — except the staff roll (27), which
// is followed by the Results scene as in the game (0x2BBxxx: next = 25).
u32 g_stubSceneVtable = 0;
u32 g_stubStaffRollVtable = 0;

int  StubScene_Start(Scene*)          { return 0; }
int  StubScene_Update(Scene*)         { return 0; }
int  StubScene_Draw(Scene*)           { return 0; }
Scene* StubScene_GetNextScene(Scene*) { return nullptr; }
s32  StubScene_GetNextSceneId(Scene*) { return kSceneBoot; }
s32  StubStaffRoll_GetNextSceneId(Scene*) { return 25; }
void StubScene_Destroy(Scene* self, int flag) {
    if (self && (s16)flag > 0)
        guest::free(self);
}

template <int Id>
Scene* StubScene_Ctor(void* mem) {
    if (Id == 27)
        printf("[scene] the staff roll is not decompiled yet — on to the results\n");
    else
        printf("[scene] scene %d is not decompiled yet — returning to the title\n", Id);
    Scene* scene = static_cast<Scene*>(Scene_BaseCtor(mem));
    scene->vtable = Id == 27 ? g_stubStaffRollVtable : g_stubSceneVtable;
    return scene;
}

// ── Mode objects with a virtual destructor (GameScene modes 0 and 1) ──────
u32 g_modeObjVtable = 0;
void ModeObj_Destroy(void* self, int flag) {
    if (self && (s16)flag > 0)
        guest::free(self);
}

}  // namespace

// BootScene (scene_base.cpp)
struct BootScene;
BootScene* BootScene_Ctor(BootScene* self);
BootScene* BootScene_Destroy(BootScene* self, s16 flag);
int    BootScene_Start(BootScene* self);
int    BootScene_Update(BootScene*);
int    BootScene_Draw(BootScene*);
Scene* BootScene_GetNextScene(BootScene*);
s32    BootScene_GetNextSceneId(BootScene*);

// ─────────────────────────────────────────────────────────────────────────
// Registry
// ─────────────────────────────────────────────────────────────────────────
namespace hle {

std::string check_model(const std::vector<unsigned char>& file) {
    if (file.size() < 0x40)
        return "file too small";
    u8* copy = static_cast<u8*>(guest::alloc((u32)file.size() + 16));
    memcpy(copy, file.data(), file.size());
    const u32 lo = guest::addr(copy), hi = lo + (u32)file.size();
    auto inside = [&](u32 p) { return p >= lo && p < hi; };
    std::string problem;

    const u8 type = Model_GetType(copy);
    Model* m = static_cast<Model*>(guest::alloc(type == 3 ? sizeof(MapModel) : sizeof(Model)));
    if (type == 0)      BodyModel_Ctor(m, copy);
    else if (type == 1) SkinModel_Ctor(m, copy);
    else if (type == 3) MapModel_Ctor(reinterpret_cast<MapModel*>(m), copy);
    else                problem = "unknown type";

    if (problem.empty()) {
        if (!VirtualCall<bool, Model*>(m->vtable, kModelRelocate, m))
            problem = "relocate refused";
        else if (VirtualCall<bool, Model*>(m->vtable, kModelRelocate, m))
            problem = "relocated twice";
        void* textures = VirtualCall<void*, Model*>(m->vtable, kModelGetTextures, m);
        if (problem.empty() && textures && !inside(guest::addr(textures)))
            problem = "textures outside the file";
        if (problem.empty() && textures)
            VirtualCall<bool, Model*>(m->vtable, kModelBindTextures, m);
        const u32 d = m->data;
        if (problem.empty() && type != 3) {
            const int bones = VirtualCall<s16, Model*>(m->vtable, kModelGetBoneCount, m);
            const u32 boneData = guest::addr(VirtualCall<void*, Model*>(m->vtable, kModelGetBones, m));
            if (bones <= 0 || !inside(boneData) || !inside(boneData + (u32)bones * 0xA0 - 1))
                problem = "bones outside the file";
            const u32 table = type == 1 ? 0x18 : 0x1C;
            const u32 count = type == 1
                ? (u32)(*guest::ptr<s16>(d + 4) + *guest::ptr<s16>(d + 6))
                : (u32)(*guest::ptr<s16>(d + 4) + *guest::ptr<s16>(d + 6) + *guest::ptr<s16>(d + 8) + *guest::ptr<s16>(d + 10));
            for (u32 i = 0; i < count && problem.empty(); i++) {
                const u32 mesh = *guest::ptr<u32>(d + table + i * 4);
                if (!inside(mesh) || !inside(*guest::ptr<u32>(mesh + (type == 1 ? 8 : 0x0C))))
                    problem = "mesh outside the file";
            }
        }
        VirtualCall<void*, Model*, s16>(m->vtable, kModelDtor, m, (s16)0);
    }
    guest::free(m);
    guest::free(copy);
    return problem;
}

static std::string g_discRoot = "extracted";
void set_disc_root(const char* root) { g_discRoot = root; }
const std::string& disc_root() { return g_discRoot; }

#define BIND(addr, fn) guest::bind(addr, fn, #fn)

static u32 make_vtable(guest::Fn destroy, guest::Fn start, guest::Fn update, guest::Fn draw,
                       guest::Fn nextScene, guest::Fn nextId) {
    u32* vt = static_cast<u32*>(guest::alloc(0x20));
    const guest::Fn slots[6] = {destroy, start, update, draw, nextScene, nextId};
    for (int i = 0; i < 6; i++) {
        if (!slots[i].fn)
            continue;
        vt[2 + i] = guest::fake_address();
        guest::bind(vt[2 + i], slots[i], "native");
    }
    return guest::addr(vt);
}

template <int... Ids>
static void bind_stub_ctors(const u32* ctors, std::integer_sequence<int, Ids...>) {
    ((guest::lookup(ctors[Ids]) ? void() :
      guest::bind(ctors[Ids], &StubScene_Ctor<Ids>, "StubScene_Ctor")), ...);
}

void bind_all() {
    // The character classes (vtables 0x2ECC90 / 0x2ECC40): decompiled methods,
    // then the stand-ins for the rest.
    Chara_BindMethods();
    hle_chara_bind();

    ResultsScene_Bind();    // scene 25 (src/game/results_scene.cpp)
    MovieScenes_Bind();     // scenes 27 and 29 (src/game/movie_scenes.cpp)
    McScreens_Bind();       // the memory-card screens' destructors (src/game/mcard.cpp)
    // Boot scene (vtable 0x2EBA10)
    BIND(0x1DB8B0, BootScene_Ctor);
    BIND(0x1DB850, BootScene_Destroy);
    BIND(0x1DB800, BootScene_Start);
    BIND(0x1DB7F0, BootScene_Update);
    BIND(0x1DB7E0, BootScene_Draw);
    BIND(0x1DB7D0, BootScene_GetNextScene);
    BIND(0x1DB7A0, BootScene_GetNextSceneId);

    // Title scene (vtable 0x2ECDB0)
    BIND(0x275380, GameScene_Ctor);
    BIND(0x275230, GameScene_Destroy);
    BIND(0x2751D0, GameScene_Start);
    BIND(0x274BD0, GameScene_Update);
    BIND(0x2744A0, GameScene_Draw);
    BIND(0x274490, GameScene_GetNextScene);
    BIND(0x274430, GameScene_GetNextSceneId);

    // Gameplay scene (vtable 0x2ED710)
    BIND(0x2B2230, GameplayScene_Ctor);
    BIND(0x2B1FF0, GameplayScene_Destroy);
    BIND(0x2B1B80, GameplayScene_Start);
    BIND(0x2B10F0, GameplayScene_Update);
    BIND(0x2ADAE0, GameplayScene_Draw);
    BIND(0x2ADA70, GameplayScene_GetNextScene);
    BIND(0x2ADA00, GameplayScene_GetNextSceneId);

    // Everything else in the scene factory gets the placeholder.
    g_stubSceneVtable = make_vtable(StubScene_Destroy,
                                    StubScene_Start,
                                    StubScene_Update,
                                    StubScene_Draw,
                                    StubScene_GetNextScene,
                                    StubScene_GetNextSceneId);
    g_stubStaffRollVtable = make_vtable(StubScene_Destroy,
                                        StubScene_Start,
                                        StubScene_Update,
                                        StubScene_Draw,
                                        StubScene_GetNextScene,
                                        StubStaffRoll_GetNextSceneId);
    static const u32 kSceneCtors[kSceneCount] = {
        0x1DB8B0, 0x27DB30, 0x2802B0, 0x1DAEB0, 0x1DFC00, 0x1DA560, 0x1DF780, 0x1DB760,
        0x27D230, 0x27DE80, 0x1DBFA0, 0x1D94C0, 0x1DA400, 0x1DF980, 0x2B2230, 0x27F200,
        0x1E03F0, 0x1D8A70, 0x227AF0, 0x2815F0, 0x239430, 0x27BC20, 0x275380, 0x27B0A0,
        0x29A320, 0x2A3460, 0x2A61F0, 0x2BBB30, 0x2BBE80, 0x2BC060 };
    bind_stub_ctors(kSceneCtors, std::make_integer_sequence<int, kSceneCount>{});

    g_modeObjVtable = make_vtable(ModeObj_Destroy,
                                  nullptr, nullptr, nullptr, nullptr, nullptr);
}

}  // namespace hle

// ─────────────────────────────────────────────────────────────────────────
// Stand-ins, C linkage to match the decompiled code's declarations
// ─────────────────────────────────────────────────────────────────────────
extern "C" {
    // The render manager's world effects (src/game/world_effects.cpp).
    void  WorldEffects_Init(void* mgr);
    void* RenderMgr_AddWorldEffect(void* mgr, int type, u32 a, u32 b);
    void  RenderMgr_DrawWorld(void* mgr);
    void  RenderMgr_ClearWorld(void* mgr);
    // src/game/status_menu.cpp
    void* MenuRes_Get();
    int   MenuRes_Poll();
    int   WeaponColumn_Poll();          // src/game/swords_page.cpp
    void  SkillSheets_Boot();           // src/game/skill_sheets.cpp
}
extern "C" {

// ── Memory ────────────────────────────────────────────────────────────────
void* MemAlloc(u32 size)                    { return guest::alloc(size); }
void  MemFree(void* p)                      { guest::free(p); }
void  MemSet(void* dst, int value, u32 size) { memset(dst, value, size); }
void  Matrix_Copy(void* dst, const void* src) { memcpy(dst, src, 64); }

// ── Start-up ──────────────────────────────────────────────────────────────
void LibcInit() {}
void RunStaticCtors() {}
void App_LoadIopModules() { note_once("IOP modules (pad, memory card, sound drivers) are not loaded"); }
void fn_11e8b0() {}
void fn_2658a0() {}
int  GetThreadId() { return 1; }
void ChangeThreadPriority(int, int) {}
void SetDataPath(const char* path) { archive::set_prefix(path); }
void fn_1e1370(void*) {}
// The C library's rand (newlib's, 0x124AF8 / 0x124B10): a 64-bit LCG whose top
// 31 bits are the answer.  The host's rand is not used: msvcrt's stops at 32767.
static u64 sRandState = 1;
void Rand_Seed(u32 seed) { sRandState = seed; }
// Movie_Play (0x1613D0) is in native/movie.cpp.

// (The start-up notice screen — the publisher's logo — is engine/notice.cpp.)

// Creates the objects the rest of the game reaches through globals.  Only
// their existence matters to the code decompiled so far.
void App_CreateDevices(void*) {
    *gScreenW = hal::gs::screen_width();
    *gScreenH = hal::gs::screen_height();
    *gGsDevice     = guest::addr(guest::alloc(0x580));
    *gWindowMgr    = guest::addr(WindowMgr_Ctor(static_cast<WindowMgr*>(guest::alloc(0x58))));
    *gRenderMgrPtr = guest::addr(guest::alloc(0x80));
    memset(guest::ptr(*gRenderMgrPtr), 0, 0x80);
    WorldEffects_Init(guest::ptr(*gRenderMgrPtr));
    *gPlayerData   = guest::addr(guest::alloc(0x7000));
    *gConfig       = guest::addr(guest::alloc(0x100));
    *guest::ptr<u32>(0x2EE69Cu) = guest::addr(Hud_Ctor(guest::alloc(0x54)));
    // App_LoadResources (0x1E1370) reads the task archive at boot.
    {
        std::vector<u8> file;
        if (archive::read_asset(0x8A, file)) {              // task/alltask
            void* copy = guest::alloc((u32)file.size());
            memcpy(copy, file.data(), file.size());
            *guest::ptr<u32>(0x2EE5F8u) = guest::addr(copy);
        }
    }
    // The charts of the swords' techniques (0x1E180C).
    SkillSheets_Boot();
    // …and what the status menu's pages share (0x1E1990).
    MenuRes_Get();
    for (int i = 0; i < 10000 && MenuRes_Poll() == 0; i++) {}
    for (int i = 0; i < 10000 && WeaponColumn_Poll() == 0; i++) {}      // (0x1E19B8)
    Config_SetDefaults(guest::ptr(*gConfig));
    *gClearColor = 0x80000000u;
    *gFadeOutFrames = (s32)(*gFrameRate * 32 / 30);
    hal::gs::set_frame_rate((int)*gFrameRate);
}

// ── Pad ───────────────────────────────────────────────────────────────────
void* PadMgr_Get() {
    if (!*gPadMgr) {
        u32* mgr = static_cast<u32*>(guest::alloc(4));
        *mgr = 1;
        *gPadMgr = guest::addr(mgr);
    }
    return guest::ptr(*gPadMgr);
}

void PadMgr_Init(void*) {}

// The pad state the game keeps per port (0x80 bytes at 0x307140; 0x13A100):
//   +0x04 the port's state (6 = ready; 0 / 7 = nothing there)
//   +0x08 buttons held, +0x0A a frame ago, +0x0C newly pressed
//   +0x10 the LEFT stick as d-pad bits (pushed past 0x50 / 0xB0), +0x12 a
//         frame ago, +0x14 newly pushed   ("Pad_GetStickHeld", "Pad_GetRepeat")
//   +0x16 the RIGHT stick likewise, +0x18, +0x1A        ("Pad_GetNewPress")
//   +0x20 the pad's own data: +0x22 / +0x23 buttons, +0x24 / +0x25 right
//         stick x / y, +0x26 / +0x27 left stick
//   +0x40 a vibration pattern: sixteen steps of the small motor (on / off),
//         sixteen of the big one (strength); +0x60 frames a step lasts,
//         +0x62 frames left, +0x64 step, +0x66 frames into it
// and 0x307580 + port × 4 counts frames with nothing touched.
void PadMgr_Poll(void*) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (editor::event(ev))                  // the level editor (F2) took it
            continue;
        if (ev.type == SDL_QUIT ||
            (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) {
            printf("clean exit\n");
            exit(0);
        }
        // F11: the whole screen, and back.
        if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0 && ev.key.keysym.sym == SDLK_F11)
            hal::gs::toggle_fullscreen();
        // F3: the modern lighting off and on again (to compare).
        if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0 && ev.key.keysym.sym == SDLK_F3)
            printf("[GS] modern lighting %s\n", hal::gs::toggle_lighting() ? "on" : "off");
        // F4: print where the player and the camera are.
        if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0 && ev.key.keysym.sym == SDLK_F4) {
            void hle_ask_where();
            hle_ask_where();
        }
    }
    hal::pad::poll();

    for (int port = 0; port < 2; port++) {
        u8* s = guest::ptr<u8>(kPadState + port * 0x80);
        const auto w = [s](int at) { return reinterpret_cast<u16*>(s + at); };
        if (!hal::pad::connected((u32)port)) {
            *reinterpret_cast<s32*>(s + 4) = 0;
            *w(0x08) = *w(0x0A) = *w(0x0C) = 0;
            s[0x24] = s[0x25] = s[0x26] = s[0x27] = 0x80;
            *w(0x62) = 0;
            *w(0x10) = *w(0x12) = *w(0x14) = *w(0x16) = *w(0x18) = *w(0x1A) = 0;
            continue;
        }
        *reinterpret_cast<s32*>(s + 4) = 6;
        // The vibration pattern, a step at a time.
        if (*w(0x62) != 0) {
            const s16 index = (s16)*w(0x64);
            if (--*w(0x62) == 0)
                hal::pad::rumble((u32)port, false, 0);
            else
                hal::pad::rumble((u32)port, s[0x40 + index] != 0, s[0x50 + index]);
            const s16 into = (s16)*w(0x66);
            *w(0x66) = (u16)(into + 1);
            if ((s16)*w(0x60) < into) {
                *w(0x66) = 0;
                *w(0x64) = (u16)((index + 1) & 0xF);
            }
        }
        // hal::pad gives the hardware word (0 = pressed); the game keeps it
        // active-high with the two bytes swapped.
        const u16 hw = (u16)~hal::pad::get_buttons((u32)port);
        const u16 held = (u16)((hw << 8) | (hw >> 8));
        s[0x24] = hal::pad::get_right_x((u32)port);
        s[0x25] = hal::pad::get_right_y((u32)port);
        s[0x26] = hal::pad::get_left_x((u32)port);
        s[0x27] = hal::pad::get_left_y((u32)port);

        *w(0x0A) = *w(0x08);
        *w(0x08) = held;
        *w(0x0C) = (u16)(~*w(0x0A) & held);
        *w(0x12) = *w(0x10);
        *w(0x18) = *w(0x16);
        const auto asButtons = [](u8 x, u8 y) {
            u16 bits = 0;
            if (y < 0x50) bits = kPadUp; else if (y > 0xB0) bits = kPadDown;
            if (x < 0x50) bits |= kPadLeft; else if (x > 0xB0) bits |= kPadRight;
            return bits;
        };
        *w(0x10) = asButtons(s[0x26], s[0x27]);
        *w(0x16) = asButtons(s[0x24], s[0x25]);
        *w(0x14) = (u16)(~*w(0x12) & *w(0x10));
        *w(0x1A) = (u16)(~*w(0x18) & *w(0x16));
        s32* idle = guest::ptr<s32>(0x307580u + port * 4);
        *idle = (*w(0x16) == 0 && *w(0x08) == 0 && *w(0x10) == 0) ? *idle + 1 : 0;
    }
}

void PadMgr_Reset(void*) {
    for (int port = 0; port < 2; port++) {
        u8* s = guest::ptr<u8>(kPadState + port * 0x80);
        const auto w = [s](int at) { return reinterpret_cast<u16*>(s + at); };
        // (The original clears `held` too; here it is kept, so that a button
        // still down when a scene changes is not seen as pressed again.)
        *w(0x62) = *w(0x66) = 0;
        *w(0x0A) = *w(0x0C) = 0;
        s[0x24] = s[0x25] = s[0x26] = s[0x27] = 0x80;
        *w(0x10) = *w(0x12) = *w(0x14) = *w(0x16) = *w(0x18) = *w(0x1A) = 0;
        *guest::ptr<s32>(0x307580u + port * 4) = 0;
        hal::pad::rumble((u32)port, false, 0);
    }
}

u16 Pad_GetHeld(void*, int port)      { return *pad_fields(port).held; }          // 0x13A0E0
u16 Pad_GetPressed(void*, int port)   { return *pad_fields(port).pressed; }       // 0x13A0C0
u16 Pad_GetRepeat(void*, int port)    { return *pad_fields(port).repeat; }        // 0x139F50  the left stick, newly pushed
u16 Pad_GetNewPress(void*, int port)  { return *pad_fields(port).newPress; }      // 0x139F70  the right stick as buttons
u16 Pad_GetStickHeld(void*, int port) { return *guest::ptr<u16>(kPadState + port * 0x80 + 0x10); }   // 0x139F90
int Pad_GetRightStick(void*, int port, u8* x, u8* y) {                            // 0x139F10
    const u8* s = guest::ptr<u8>(kPadState + port * 0x80);
    *x = s[0x24];
    *y = s[0x25];
    return 0;
}
int Pad_GetStick(void*, int port, u8* x, u8* y) {                                 // 0x139FB0
    const u8* s = guest::ptr<u8>(kPadState + port * 0x80);
    *x = s[0x26];
    *y = s[0x27];
    return 0;
}
int Pad_IsConnected(void*, int port) {                                            // 0x13A0A0
    return *guest::ptr<s32>(kPadState + port * 0x80 + 4);
}
// 0x139FF0: a pattern of 0x24 bytes (see above).  The manager's first word
// is "vibration allowed".
int Pad_SetVibration(void* mgr, int port, const void* pattern) {
    if (*static_cast<const u32*>(mgr) == 0)
        return 1;
    u8* s = guest::ptr<u8>(kPadState + port * 0x80);
    memcpy(s + 0x40, pattern, 0x20);
    memcpy(s + 0x60, static_cast<const u8*>(pattern) + 0x20, 2);
    memcpy(s + 0x62, static_cast<const u8*>(pattern) + 0x22, 2);
    memset(s + 0x64, 0, 4);
    return 0;
}

// ── Display device ────────────────────────────────────────────────────────
void GsDevice_BeginFrame(void*)                  { hal::gs::begin_frame(); }
void GsDevice_EndFrame(void*)                    {}
void GsDevice_ClearScreen(void*)                 { hal::gs::clear(0x80000000u); }
// A full-screen sprite in the given colour: alpha 0x80 covers the screen,
// less darkens (or tints) what is already there.
void GsDevice_ClearScreenColor(void*, u32 rgba) {
    if ((rgba >> 24) >= 0x80)
        hal::gs::clear(rgba);
    else if ((rgba >> 24) != 0)
        hal::gs::fill_rect(0, 0, (float)*gScreenW, (float)*gScreenH, rgba);
}
u32  GsDevice_SwapBuffers(void*, u32)            { mods::frame(); hal::gs::end_frame(); return 0; }
void GsDevice_SetDisplay(void*, u32, s16, s32)   {}
void GsDevice_SetVu1Program(void*, u32)          {}
void GsDevice_SetFogParams(float, float, float, float, void*, const void*) {}
void GsDevice_SendColorReg(void*, u32)           {}
void GsDevice_StoreClearColor(void*, u32)        {}
void GsDevice_SetProjection(float, float, float, float, float, float, float, float, void*, float) {}
void GsDevice_UpdateMatrices(void*)              {}
void GsDevice_PresentMode(void*, u32, u32, u32, u32) {}
int  sceGsSyncPath(int, u32)                     { return 0; }

void GsDevice_SetAlphaRef(void*, int ref) { hal::gs::set_alpha_ref(ref); }

// `alphaReg` is the GS ALPHA register: A, B, C, D in the low byte.
void GsDevice_SetBlend(void*, u64 alphaReg) {
    switch (alphaReg & 0xFF) {
    case 0x44: hal::gs::set_blend(hal::gs::Blend::Normal);      break;
    case 0x48: hal::gs::set_blend(hal::gs::Blend::Additive);    break;
    case 0x42: hal::gs::set_blend(hal::gs::Blend::Subtractive); break;
    default:
        printf("[stub] unhandled GS blend %#llx\n", (unsigned long long)alphaReg);
        hal::gs::set_blend(hal::gs::Blend::Normal);
        break;
    }
}

// v is in 1/16 pixel.  (The original also multiplies v by a scale that is
// always 1.0 and takes the centre, 2048, from globals at 0x2F5F30–3C.)
// (Native, for a picture wider than the game's screen: while the in-world HUD
// draws its widgets, "from the left edge" and "from the right edge" can mean
// the picture's edges — hal::gs::Video::hudEdges.)
static bool g_hudAnchor = false;
extern "C" void Native_HudAnchor(int on) { g_hudAnchor = on != 0 && hal::gs::video().hudEdges; }
u32 Screen_X(int v, int align) {
    if (g_hudAnchor && align != 0)
        v += align * (int)(hal::gs::wide_margin() * 16.0f);
    return (u32)(v + (2048 + align * (*gScreenW >> 1)) * 16);
}
u32 Screen_Y(int v, int align) { return (u32)(v + (2048 + align * (*gScreenH >> 1)) * 16); }
// The same with the alignment taken from globals (0x2EE4D0 / 0x2EE4D4, both
// normally 0 = measured from the centre); used by the screen layouts.
u32 Screen_XAligned(int v) { return Screen_X(v, *guest::ptr<s32>(0x2EE4D0)); }
u32 Screen_YAligned(int v) { return Screen_Y(v, *guest::ptr<s32>(0x2EE4D4)); }
// Single-texture upload used by the model classes; the native renderer reads
// model textures straight from the file instead.
int  GsDevice_UploadTexture(void*, void*) { return 0; }
void GsDevice_Begin2D(void*) {}
void GsDevice_End2D(void*) {}

void Gs_FillRect(const s32* rect, u32 z, u32 rgba) {
    hal::gs::set_z(z);
    hal::gs::fill_rect((u16)rect[0] / 16.0f - kOriginX, (u16)rect[1] / 16.0f - kOriginY,
                       rect[2] / 16.0f, rect[3] / 16.0f, rgba);
}

// ── Textures sent to the GS directly (the font) ───────────────────────────
// `pim` has been through Pim_Relocate, so its descriptors hold addresses.
// The textures are decoded here and remembered by their GS base pointer.
int GsDevice_UploadTextures(void*, void* pim) {
    if (!pim)
        return -1;
    const u8* base = static_cast<const u8*>(pim);
    const u32 baseAddr = guest::addr(base);
    u32 count = 0, end = 0;
    for (const u8* d = base;; d += 0x30) {
        count++;
        u32 pix, pixSize, clut, clutSize;
        memcpy(&pixSize, d + 0x14, 4); memcpy(&pix, d + 0x18, 4);
        memcpy(&clutSize, d + 0x20, 4); memcpy(&clut, d + 0x24, 4);
        if (pix && pix + pixSize - baseAddr > end)    end = pix + pixSize - baseAddr;
        if (clut && clut + clutSize - baseAddr > end) end = clut + clutSize - baseAddr;
        u16 remaining; memcpy(&remaining, d + 0x28, 2);
        if (remaining < 2)
            break;
    }
    if (end < count * 0x30)
        end = count * 0x30;

    // Decode from a copy with the offsets made relative again.
    std::vector<u8> blob(base, base + end);
    for (u32 i = 0; i < count; i++) {
        for (u32 field : {0x18u, 0x24u}) {
            u32 value; memcpy(&value, &blob[i * 0x30 + field], 4);
            if (value) { value -= baseAddr; memcpy(&blob[i * 0x30 + field], &value, 4); }
        }
    }
    auto* sheet = new sprites::Sheet;
    if (!sheet->load(blob)) {
        fprintf(stderr, "[gs] texture upload could not be decoded\n");
        delete sheet;
        return -1;
    }
    register_textures(sheet, static_cast<u8*>(pim), nullptr);
    return 0;
}

// Flat or Gouraud-shaded geometry: the GS PRIM value, then RGBAQ and XYZ2
// for each vertex.
static bool g_depthTest2d = false;
void GsDevice_DrawPrim(void*, const void* packet, int count) {
    struct V { u32 rgba; u32 q; u16 x, y; u32 z; };
    const u64 prim = *static_cast<const u64*>(packet);
    const V* v = reinterpret_cast<const V*>(static_cast<const u8*>(packet) + 8);
    std::vector<hal::gs::Vtx> out((size_t)count);
    for (int i = 0; i < count; i++)
        out[i] = {v[i].x / 16.0f - kOriginX, v[i].y / 16.0f - kOriginY, 0, 0, v[i].rgba};
    if (g_depthTest2d && (prim & 7) == 1 && count <= 8) {
        // A line in the world (a streak of the effects), in the blend mode set.
        float depth[8];
        for (int i = 0; i < count; i++)
            depth[i] = (1.0f - (float)v[i].z / 16777215.0f) * 2.0f - 1.0f;
        hal::gs::draw_prim_depth(0, 1, 1, hal::gs::Prim::Lines, out.data(), depth, count);
        return;
    }
    if (g_depthTest2d && ((prim & 7) == 4 || (prim & 7) == 5) && count <= 128) {
        // A flat strip or fan in the world (a blade's plain trail).
        float depth[128];
        for (int i = 0; i < count; i++)
            depth[i] = (1.0f - (float)v[i].z / 16777215.0f) * 2.0f - 1.0f;
        hal::gs::draw_prim_depth(0, 1, 1, (prim & 7) == 5 ? hal::gs::Prim::TriangleFan : hal::gs::Prim::TriangleStrip,
                                 out.data(), depth, count);
        return;
    }
    if ((prim & 7) == 6 && count == 2) {
        // A sprite: two opposite corners, in the second one's colour.
        const float x0 = out[0].x, y0 = out[0].y, x1 = out[1].x, y1 = out[1].y;
        const u32 c = v[1].rgba;
        hal::gs::Vtx quad[4] = {{x0, y0, 0, 0, c}, {x1, y0, 0, 0, c}, {x0, y1, 0, 0, c}, {x1, y1, 0, 0, c}};
        hal::gs::set_blend(hal::gs::Blend::Normal);
        hal::gs::set_z(v[1].z);
        hal::gs::draw_prim(0, 1, 1, hal::gs::Prim::TriangleStrip, quad, 4);
        return;
    }
    hal::gs::Prim mode = (prim & 7) == 1 ? hal::gs::Prim::Lines
                       : (prim & 7) == 3 ? hal::gs::Prim::Triangles
                       : (prim & 7) == 5 ? hal::gs::Prim::TriangleFan
                                         : hal::gs::Prim::TriangleStrip;
    hal::gs::set_blend(hal::gs::Blend::Normal);
    if (count > 0)
        hal::gs::set_z(v[count - 1].z);
    hal::gs::draw_prim(0, 1, 1, mode, out.data(), count);
}

// Textured geometry (TEX0, PRIM, then UV / RGBAQ / XYZ2 per vertex).  Only
// what the menus send is handled: four vertices forming an axis-aligned
// rectangle, first and third being opposite corners.
void GsDevice_DrawSprite(void*, const void* packet);
// 0x130940: writes one GS register.  Only TEST (0x47) matters here: its
// depth test (bits 17–18 = 2, "greater or equal") is on while the render
// manager draws what is in the world.
void GsDevice_WriteRegister(void*, u32 reg, u64 value) {
    if (reg == 0x47)
        g_depthTest2d = ((value >> 16) & 1) != 0 && ((value >> 17) & 3) == 2;
}
// The PS2 can texture from what it has drawn: TEX0 then names no texture of
// ours, is 32- or 24-bit and as wide as the screen.  The frame so far is
// copied for it, once a frame.
static bool is_frame_tex0(u64 tex0) {
    return ((tex0 >> 20) & 0x3F) <= 1 && (int)((tex0 >> 14) & 0x3F) == hal::gs::screen_width() / 64;
}
static u32 frame_texture() {
    static int grabbedAt = -1;
    static u32 frame = 0;
    if (grabbedAt != hal::gs::frame_count()) {
        grabbedAt = hal::gs::frame_count();
        frame = hal::gs::grab_frame();
    }
    return frame;
}
void GsDevice_DrawTexturedPrim(void* dev, const void* packet, int count) {
    struct Vertex { s16 u, v; u32 _uvHi; u32 rgba; u32 q; u16 x, y; u32 z; };
    struct Packet { u64 tex0, prim; Vertex v[4]; };      // (or more)
    const Packet* p = static_cast<const Packet*>(packet);
    if ((p->prim & 7) == 6) {                   // a list of sprites: two corners each
        for (int i = 0; i + 1 < count; i += 2) {
            struct { u64 tex0, prim; Vertex c[2]; } sprite = {p->tex0, p->prim, {p->v[i], p->v[i + 1]}};
            GsDevice_DrawSprite(dev, &sprite);
        }
        return;
    }
    if (((p->prim & 7) == 4 || (p->prim & 7) == 5) && g_depthTest2d) {
        // A strip the game projected into the world itself (shadows, blood,
        // dust): tested against the 3D picture's depth.
        const u32 tbp = (u32)(p->tex0 & 0x3FFF), cbp = (u32)(p->tex0 >> 37) & 0x3FFF;
        auto it = g_gsTextures.find(tbp);
        if (count > 128)
            return;
        u32 tex = 0;
        int texW = 1, texH = 1;
        if (it != g_gsTextures.end()) {
            const GsTexture& gt = it->second;
            sprites::Rect rect{};
            rect.texture = (u8)gt.index;
            rect.clutOffset = (u8)(cbp - gt.cbp);
            tex = gt.sheet->texture_for(rect);
            texW = gt.sheet->textures[gt.index].width;
            texH = gt.sheet->textures[gt.index].height;
        } else if (is_frame_tex0(p->tex0)) {    // the picture so far (the glare of a broken guard)
            tex = frame_texture();
            texW = hal::gs::screen_width();
            texH = hal::gs::screen_height();
        }
        if (!tex)
            return;
        hal::gs::Vtx v[128];
        float depth[128];
        for (int i = 0; i < count; i++) {
            v[i] = {p->v[i].x / 16.0f - kOriginX, p->v[i].y / 16.0f - kOriginY,
                    p->v[i].u / 16.0f, p->v[i].v / 16.0f, p->v[i].rgba};
            depth[i] = (1.0f - (float)p->v[i].z / 16777215.0f) * 2.0f - 1.0f;
        }
        hal::gs::draw_prim_depth(tex, texW, texH,
                                 (p->prim & 7) == 5 ? hal::gs::Prim::TriangleFan : hal::gs::Prim::TriangleStrip,
                                 v, depth, count);
        return;
    }
    if ((p->prim & 7) == 4 || (p->prim & 7) == 5) {
        // A strip or a fan in screen space.  Its texture is one of the
        // registered ones — or the frame buffer itself (the PS2 can texture
        // from what it has drawn: TEX0 then names no texture of ours, is
        // 32- or 24-bit and as wide as the screen), for which the frame so far is
        // copied, once a frame.
        const u32 tbp = (u32)(p->tex0 & 0x3FFF), cbp = (u32)(p->tex0 >> 37) & 0x3FFF;
        const auto it = g_gsTextures.find(tbp);
        u32 tex = 0;
        int texW = 1, texH = 1;
        bool ownColours = false;
        for (int i = 1; i < count; i++)
            ownColours = ownColours || p->v[i].rgba != p->v[0].rgba;
        if (it != g_gsTextures.end()) {
            if (count == 4 && !ownColours) {    // the menus' plain rectangles: as a sprite (first and third corners)
                struct { u64 tex0, prim; Vertex c[2]; } sprite = {p->tex0, p->prim, {p->v[0], p->v[2]}};
                GsDevice_DrawSprite(dev, &sprite);
                return;
            }
            const GsTexture& gt = it->second;
            sprites::Rect rect{};
            rect.texture = (u8)gt.index;
            rect.clutOffset = (u8)(cbp - gt.cbp);
            tex = gt.sheet->texture_for(rect);
            texW = gt.sheet->textures[gt.index].width;
            texH = gt.sheet->textures[gt.index].height;
        } else if (is_frame_tex0(p->tex0)) {
            tex = frame_texture();
            texW = hal::gs::screen_width();
            texH = hal::gs::screen_height();
        }
        if (!tex || count > 32)
            return;
        hal::gs::Vtx v[32];
        for (int i = 0; i < count; i++)
            v[i] = {p->v[i].x / 16.0f - kOriginX, p->v[i].y / 16.0f - kOriginY,
                    p->v[i].u / 16.0f, p->v[i].v / 16.0f, p->v[i].rgba};
        hal::gs::set_z(p->v[count - 1].z);
        hal::gs::draw_prim(tex, texW, texH,
                           (p->prim & 7) == 5 ? hal::gs::Prim::TriangleFan : hal::gs::Prim::TriangleStrip, v, count);
        return;
    }
    if (count != 4) {
        note_once("GsDevice_DrawTexturedPrim: only strips, fans and sprites are implemented");
        return;
    }
    struct { u64 tex0, prim; Vertex c[2]; } sprite = {p->tex0, p->prim, {p->v[0], p->v[2]}};
    GsDevice_DrawSprite(dev, &sprite);
}

// One textured sprite given as GS register values (TEX0, PRIM, then UV /
// RGBAQ / XYZ2 for two corners).
void GsDevice_DrawSprite(void*, const void* packet) {
    struct Corner { s16 u, v; u32 _uvHi; u32 rgba; u32 q; u16 x, y; u32 z; };
    struct Packet { u64 tex0, prim; Corner c[2]; };
    const Packet* p = static_cast<const Packet*>(packet);

    u32 tbp = (u32)(p->tex0 & 0x3FFF);
    u32 cbp = (u32)(p->tex0 >> 37) & 0x3FFF;
    auto it = g_gsTextures.find(tbp);
    if (it == g_gsTextures.end())
        return;
    const GsTexture& gt = it->second;
    sprites::Rect rect{};
    rect.texture = (u8)gt.index;
    rect.clutOffset = (u8)(cbp - gt.cbp);
    u32 tex = gt.sheet->texture_for(rect);
    if (!tex)
        return;
    const sprites::Texture& t = gt.sheet->textures[gt.index];
    float x0 = p->c[0].x / 16.0f - kOriginX, y0 = p->c[0].y / 16.0f - kOriginY;
    float x1 = p->c[1].x / 16.0f - kOriginX, y1 = p->c[1].y / 16.0f - kOriginY;
    hal::gs::set_z(p->c[1].z);
    hal::gs::draw_sprite(tex, t.width, t.height, x0, y0, x1 - x0, y1 - y0,
                         p->c[0].u / 16.0f, p->c[0].v / 16.0f,
                         p->c[1].u / 16.0f, p->c[1].v / 16.0f, p->c[0].rgba);
}

// ── Archive files ─────────────────────────────────────────────────────────
// The guest-side object is the 0xC bytes the game allocates; its second word
// (the handle) indexes g_files.
void* ArchiveFile_Create() {
    return guest::alloc(0xC);
}

void ArchiveFile_Open(void* file, int assetId) {
    std::vector<u8> data;
    if (!archive::read_asset((u32)assetId, data))
        fprintf(stderr, "[archive] asset %#x could not be read\n", assetId);
    g_files.push_back(std::move(data));
    static_cast<u32*>(file)[1] = (u32)g_files.size() - 1;
}

void ArchiveFile_OpenByName(void* file, const char* name) {
    std::vector<u8> data;
    if (!archive::read(name, data))
        fprintf(stderr, "[archive] %s could not be read\n", name);
    g_files.push_back(std::move(data));
    static_cast<u32*>(file)[1] = (u32)g_files.size() - 1;
}

u32 ArchiveFile_GetSize(void* file) {
    return (u32)g_files[static_cast<u32*>(file)[1]].size();
}

void ArchiveFile_Read(void* file, void* dst, u32 size) {
    const std::vector<u8>& data = g_files[static_cast<u32*>(file)[1]];
    memcpy(dst, data.data(), size < data.size() ? size : data.size());
}

// The asynchronous read: done at once, so it is never busy.
void ArchiveFile_ReadAsync(void* file, void* dst, u32 size) { ArchiveFile_Read(file, dst, size); }
int  ArchiveFile_IsBusy(void*) { return 0; }

void* ArchiveFile_ReadAll(void* file, u32 size) {
    if (size == 0)
        size = ArchiveFile_GetSize(file);
    void* buffer = guest::alloc(size);
    if (buffer)
        ArchiveFile_Read(file, buffer, size);
    return buffer;
}

void ArchiveFile_Close(void* file) {
    std::vector<u8>().swap(g_files[static_cast<u32*>(file)[1]]);
}

void ArchiveFile_Destroy(void* file) {
    ArchiveFile_Close(file);
    guest::free(file);
}

void* MemAllocAligned(u32, u32 size) { return guest::alloc(size); }
void  MemFreeAligned(void* p)        { guest::free(p); }

// ── Sprite sets ───────────────────────────────────────────────────────────
void* SpriteSet_Ctor(void* mem) {
    memset(mem, 0, 0x14);
    return mem;
}

// The original queues an asynchronous load; here the file is read at once.
static char load_sprite_set(void* set, std::vector<u8>& blob, const char* name) {
    auto* sheet = new sprites::Sheet;
    if (blob.empty() || !sheet->load(blob)) {
        fprintf(stderr, "[sprites] failed to load %s\n", name);
        delete sheet;
        return 0;
    }
    printf("[sprites] %s: %zu sprites\n", name, sheet->rects.size());

    SheetEntry entry;
    entry.sheet = sheet;
    entry.loadFrame = hal::gs::frame_count();
    u8* copy = static_cast<u8*>(guest::alloc((u32)blob.size()));
    memcpy(copy, blob.data(), blob.size());
    entry.blob = guest::addr(copy);
    const bool bpm = memcmp(copy + 8, "BPMFile", 7) == 0;
    u32 count = 0;
    if (bpm) memcpy(&count, copy + 4, 4);
    u8* descriptors = copy + (bpm ? 0x10 + count * 0x20 : 0);
    register_textures(sheet, descriptors, &entry.tbps);

    u32* fields = static_cast<u32*>(set);
    g_sheets.push_back(entry);
    fields[0] = (u32)g_sheets.size() - 1;
    fields[1] = 0;
    fields[2] = guest::addr(descriptors);
    fields[3] = bpm ? guest::addr(copy + 0x10) : 0;
    fields[4] = count;
    return 1;
}

// A sheet that is part of a pack already in memory.  (The real
// SpriteSet_Parse, 0x17C230, takes no size; the packs give one.)
char SpriteSet_ParseSized(void* set, const void* data, u32 size) {
    std::vector<u8> blob(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
    return load_sprite_set(set, blob, "(part of a pack)");
}

char SpriteSet_Load(void* set, int assetId, int) {
    std::vector<u8> blob;
    archive::read_asset((u32)assetId, blob);
    return load_sprite_set(set, blob, archive::asset_path((u32)assetId));
}

char SpriteSet_LoadByName(void* set, const char* name, const char*) {
    std::vector<u8> blob;
    archive::read(name, blob);
    return load_sprite_set(set, blob, name);
}

// WOTS2_LOAD_DELAY=N makes every sprite set take N frames to "load", which
// stands in for disc latency when checking the loading screens.
int SpriteSet_IsLoaded(void* set) {
    static const int delay = getenv("WOTS2_LOAD_DELAY") ? atoi(getenv("WOTS2_LOAD_DELAY")) : 0;
    if (!sheet_of(set))
        return 0;
    return hal::gs::frame_count() - g_sheets[*static_cast<u32*>(set)].loadFrame >= delay;
}
int SpriteSet_Bind(void* set) { return SpriteSet_IsLoaded(set); }

// Sprite record `index` (0x20 bytes, see the .bpm notes), or null.
void* SpriteSet_GetSprite(void* set, int index) {
    u32* fields = static_cast<u32*>(set);
    if (!SpriteSet_IsLoaded(set) || index < 0 || (u32)index > fields[4] || !fields[3])
        return nullptr;
    return guest::ptr<u8>(fields[3]) + index * 0x20;
}

void SpriteSet_Dtor(void* set, int flag) {
    if (!set)
        return;
    u32 index = *static_cast<u32*>(set);
    if (index && index < g_sheets.size() && g_sheets[index].sheet) {
        SheetEntry& e = g_sheets[index];
        for (u32 tbp : e.tbps) {
            g_gsTextures.erase(tbp);
            g_freeTbps.push_back(tbp);
        }
        e.sheet->release();
        delete e.sheet;
        guest::free(guest::ptr(e.blob));
        e = SheetEntry{};
    }
    if ((s16)flag > 0)
        guest::free(set);
}

// x / y are GS primitive coordinates in 1/16 pixel.
void SpriteSet_Draw(void* set, int index, int x, int y, u32 z, u32 rgba) {
    sprites::Sheet* sheet = sheet_of(set);
    if (!sheet || index < 0 || index >= (int)sheet->rects.size())
        return;
    const sprites::Rect& r = sheet->rects[index];
    hal::gs::set_z(z);
    draw_sprite(sheet, index, (u16)x / 16.0f - kOriginX, (u16)y / 16.0f - kOriginY,
                r.w, r.h, false, false, rgba);
}

// A sprite turned about its centre, which is placed at (x, y).  `scale` is
// 16 for one texel per pixel; the angle is in degrees.  Only the plain form
// (flags & 0xF0 == 0) is handled; the others draw mirrored fans.
void SpriteSet_DrawRotated(void* set, int index, const void* params) {
    struct Params { u32 flags; s32 x, y; u32 z; u32 rgba; float scaleX, scaleY, angle; };
    const Params* p = static_cast<const Params*>(params);
    sprites::Sheet* sheet = sheet_of(set);
    if (!sheet || !SpriteSet_IsLoaded(set) || index < 0 || index >= (int)sheet->rects.size())
        return;
    const sprites::Rect& r = sheet->rects[index];
    u32 tex = sheet->texture_for(r);
    if (!tex)
        return;
    const sprites::Texture& t = sheet->textures[r.texture];
    const float inset = (p->flags & 1) ? 0.5f : 0.0f;
    const float u0 = r.u + inset, u1 = r.u + r.w - 2 * inset;
    const float v0 = r.v + inset, v1 = r.v + r.h - 2 * inset;
    // Flags 0x10 / 0x20: the sprite is a quarter of the picture (an
    // umbrella, a crest), mirrored into the other three — a fan of ten
    // points round the centre, where the sprite's right edge meets its top
    // (0x10) or its bottom (0x20).
    if ((p->flags & 0xF0) == 0x10 || (p->flags & 0xF0) == 0x20) {
        const int w = r.w * (int)p->scaleX, h = r.h * (int)p->scaleY;
        const float rad = p->angle * 3.1415927f / 180.0f;
        const float c = cosf(rad), sn = sinf(rad);
        const float vIn = (p->flags & 0xF0) == 0x10 ? v0 : v1, vOut = (p->flags & 0xF0) == 0x10 ? v1 : v0;
        const struct { int x, y; float u, v; } point[10] = {
            {0, 0, u1, vIn}, {w, 0, u0, vIn}, {w, -h, u0, vOut}, {0, -h, u1, vOut}, {-w, -h, u0, vOut},
            {-w, 0, u0, vIn}, {-w, h, u0, vOut}, {0, h, u1, vOut}, {w, h, u0, vOut}, {w, 0, u0, vIn}};
        hal::gs::Vtx out[10];
        for (int i = 0; i < 10; i++) {
            const int x = (int)(point[i].x * c - point[i].y * sn) + p->x;
            const int y = (int)(point[i].x * sn + point[i].y * c) + p->y;
            out[i] = {(u16)(x & 0xFFF0) / 16.0f - kOriginX, (u16)(y & 0xFFF0) / 16.0f - kOriginY,
                      point[i].u, point[i].v, p->rgba};
        }
        hal::gs::set_z(p->z);
        hal::gs::draw_prim(tex, t.width, t.height, hal::gs::Prim::TriangleFan, out, 10);
        return;
    }
    if (p->flags & 0xF0)
        return;                                         // (the original draws nothing either)
    const int hw = (r.w >> 1) * (int)p->scaleX, hh = (r.h >> 1) * (int)p->scaleY;
    const float rad = p->angle * 3.1415927f / 180.0f;
    const float c = cosf(rad), sn = sinf(rad);
    // Corner order and texture coordinates as the game emits them.
    const struct { int x, y; float u, v; } corner[4] = {
        { hw, -hh, u0, v0}, { hw, hh, u0, v1}, {-hw, -hh, u1, v0}, {-hw, hh, u1, v1}};
    hal::gs::Vtx out[4];
    for (int i = 0; i < 4; i++) {
        int x = (int)(corner[i].x * c - corner[i].y * sn) + p->x;
        int y = (int)(corner[i].x * sn + corner[i].y * c) + p->y;
        out[i] = {(u16)(x & 0xFFF0) / 16.0f - kOriginX, (u16)(y & 0xFFF0) / 16.0f - kOriginY,
                  corner[i].u, corner[i].v, p->rgba};
    }
    hal::gs::set_z(p->z);
    hal::gs::draw_prim(tex, t.width, t.height, hal::gs::Prim::TriangleStrip, out, 4);
}

// ── Asset references ──────────────────────────────────────────────────────
// 0xC bytes on the guest side: here {data, size, unused}.  The original loads
// in the background; these load at once.
void* AssetRef_Ctor(void* mem) {
    memset(mem, 0, 0xC);
    return mem;
}

static char fill_asset_ref(void* ref, std::vector<u8>& data, const char* name) {
    if (data.empty()) {
        fprintf(stderr, "[archive] %s could not be read\n", name);
        return 0;
    }
    u8* copy = static_cast<u8*>(guest::alloc((u32)data.size() + 1));
    memcpy(copy, data.data(), data.size());
    static_cast<u32*>(ref)[0] = guest::addr(copy);
    static_cast<u32*>(ref)[1] = (u32)data.size();
    return 1;
}

char AssetQueue_Request(void* ref, int assetId) {
    std::vector<u8> data;
    archive::read_asset((u32)assetId, data);
    return fill_asset_ref(ref, data, archive::asset_path((u32)assetId));
}

char AssetQueue_RequestByName(void* ref, const char* name) {
    std::vector<u8> data;
    archive::read(name, data);
    return fill_asset_ref(ref, data, name);
}

void* AssetRef_GetData(void* ref) { return guest::ptr(static_cast<u32*>(ref)[0]); }
s32   AssetRef_GetSize(void* ref) { return (s32)static_cast<u32*>(ref)[1]; }

void AssetRef_Dtor(void* ref, int flag) {
    if (!ref)
        return;
    guest::free(guest::ptr(static_cast<u32*>(ref)[0]));
    static_cast<u32*>(ref)[0] = 0;
    if ((s16)flag > 0)
        guest::free(ref);
}

// ── Render manager: the fade (effect 11), the letterbox (8) and text over
// the picture (13) exist; the rest are ignored ─────────────────────────────
// Effect 8 (class 0x18A160): black bars 62 px at the top and 85 px at the
// bottom, in or out over 30 frames.  Effect 13 (class 0x1954C0): a line of
// text {u16 x, y; u32 z, rgba; u16 centred; WString} with a shadow, drawn at
// scale 1.2 while the counter its second argument points at is not zero (no
// counter: for one frame).
struct Bars { int level = 0; bool on = false; } g_bars;
// Effect 4 (class 0x182B00, vtable 0x2EB090): the daytime heat shimmer.  The
// frame is redrawn over itself, a quarter strong, through an 11 × 8 grid
// whose inner points — texture coordinates in 1/16 pixel — are a mesh of
// springs kicked once at random and left to wobble; drawn at a z near the
// far plane, so that only what is far away shimmers.
struct Shimmer {
    bool exists = false, on = false;
    s32 at[88][2] = {}, speed[88][2] = {};
} g_shimmer;
int Rand_Next();
static void Shimmer_Init() {                        // 0x1826D0
    const s32 dx = (*gScreenW << 4) / 10, dy = (*gScreenH << 4) / 7;
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 11; col++) {
            g_shimmer.at[col + row * 11][0] = col * dx;
            g_shimmer.at[col + row * 11][1] = row * dy;
            g_shimmer.speed[col + row * 11][0] = g_shimmer.speed[col + row * 11][1] = 0;
        }
    for (int row = 1; row < 7; row++)
        for (int col = 1; col < 10; col++) {
            const s32 x = (s32)((u32)Rand_Next() & 0x1FF), y = (s32)((u32)Rand_Next() & 0x1FF);
            g_shimmer.speed[col + row * 11][0] += x - 0x100;
            g_shimmer.speed[col + row * 11][1] += y - 0x100;
        }
    g_shimmer.exists = true;
}
static void Shimmer_Update() {                      // 0x182280
    if (!g_shimmer.on)
        return;
    s32 was[88][2];
    memcpy(was, g_shimmer.at, sizeof was);
    for (int row = 1; row < 7; row++)
        for (int col = 1; col < 10; col++) {
            const int i = col + row * 11;
            s32* v = g_shimmer.speed[i];
            for (int k = 0; k < 2; k++) {
                const s32 around = was[i + 11][k] + was[i - 11][k] + was[i + 1][k] + was[i - 1][k];
                v[k] += (s32)((float)around * 0.25f) - was[i][k];           // 0x2EDAE8
            }
            if (262144.0f < (float)(v[0] * v[0] + v[1] * v[1])) {
                v[0] = (s32)((float)v[0] * 0.9f);                           // 0x2EDAEC
                v[1] = (s32)((float)v[1] * 0.9f);
            }
            g_shimmer.at[i][0] += (s32)((float)v[0] * 0.0625f);             // 0x2EDAF0
            g_shimmer.at[i][1] += (s32)((float)v[1] * 0.0625f);
        }
}
static void Shimmer_Draw(void* dev) {               // 0x182030
    if (!g_shimmer.on || !g_shimmer.exists)
        return;
    struct Vertex { s16 u, v; u32 _4; u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 tex0, prim; Vertex v[22]; } packet;
    const s32 left = (s32)(Screen_X(0, -1) & 0xFFFF), top = (s32)(Screen_Y(0, -1) & 0xFFFF);
    const s32 step = ((s32)(Screen_X(0, 1) & 0xFFFF) - left) / 10;
    hal::gs::set_blend(hal::gs::Blend::Normal);
    for (int row = 0; row < 7; row++) {
        memset(&packet, 0, sizeof packet);
        packet.tex0 = (u64)(*gScreenW >> 6) << 14 | 0x268100000ull;        // the frame buffer as a texture
        packet.prim = 0x15C;
        int n = 0;
        for (int col = 0; col < 11; col++)
            for (int r = row; r < row + 2; r++, n++) {
                packet.v[n].u = (s16)g_shimmer.at[col + r * 11][0];
                packet.v[n].v = (s16)g_shimmer.at[col + r * 11][1];
                packet.v[n].rgba = 0x20808080u;
                packet.v[n].q = 1.0f;
                packet.v[n].x = (u16)(left + col * step);
                packet.v[n].y = (u16)(top + r * step);
                packet.v[n].z = 0x10000;            // near the far plane: only what is distant passes
            }
        GsDevice_DrawTexturedPrim(dev, &packet, n);
    }
}
// Effect 2 (class 0x268390, vtable 0x2ECBA0; the scripts' Effect.Monotone):
// the picture without its colours.  Modes 1–5 read the frame back every
// frame, change it and put it back (0x267720…: 1 grey tinted red, 2 grey,
// 3 red alone, 4 the red as grey, 5 the red with the rest at three quarters);
// only a test script asks for those.  Mode 6, the endings' (0x2682B0 →
// 0x267E20): the frame as it is *when asked* is kept — its red as an 8-bit
// picture with a grey palette — and shown as a still (0x267AB0) from then
// on, under the narration.  Mode 0 lets go.
static struct {
    u32 mode = 0, tex = 0;
} g_mono;
static void Mono_Change(std::vector<u8>& px, u32 mode) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        u8* p = &px[i];
        const u32 lum = (p[2] * 0x132u + p[0] * 0x75u + p[1] * 0x259u) >> 10;      // as written: red and blue the wrong way round
        switch (mode) {
        case 1: p[0] = (u8)lum; p[1] = p[2] = (u8)(lum * 3 >> 2); break;
        case 2: p[0] = p[1] = p[2] = (u8)lum; break;
        case 3: p[1] = p[2] = 0; break;
        case 4: p[1] = p[2] = p[0]; break;
        case 5: p[1] = p[2] = (u8)(p[0] - (p[0] >> 2)); break;
        case 6: {                                   // palette entry = index / 2, drawn at colour 0xFF
            const u32 g = (p[0] >> 1) * 0xFFu >> 7;
            p[0] = p[1] = p[2] = (u8)(g > 0xFF ? 0xFF : g);
            break;
        }
        }
        p[3] = 0xFF;
    }
}
static void Mono_Keep(u32 mode) {
    std::vector<u8> px;
    hal::gs::read_frame(px);
    Mono_Change(px, mode);
    if (g_mono.tex != 0)
        hal::gs::destroy_texture(g_mono.tex);
    g_mono.tex = hal::gs::create_texture(px.data(), *gScreenW, *gScreenH);
}
static void Mono_Start(u32 mode) {                  // 0x268210
    if (mode == g_mono.mode)
        return;
    g_mono.mode = mode;
    if (mode == 6) {
        Mono_Keep(6);
    } else if (g_mono.tex != 0 && mode == 0) {
        hal::gs::destroy_texture(g_mono.tex);
        g_mono.tex = 0;
    }
}
static void Mono_Draw() {                           // 0x267070
    if (g_mono.mode == 0 || g_mono.mode > 6)
        return;
    if (g_mono.mode != 6)
        Mono_Keep(g_mono.mode);
    if (g_mono.tex == 0)
        return;
    hal::gs::set_blend(hal::gs::Blend::Normal);
    hal::gs::set_alpha_ref(0);
    const float w = (float)*gScreenW, h = (float)*gScreenH;
    hal::gs::set_full_width();          // (the still is of the whole picture, however wide)
    hal::gs::draw_sprite(g_mono.tex, *gScreenW, *gScreenH, 0, 0, w, h, 0, 0, w, h, 0x80808080u);
}

struct OverText {
    u16 x = 0, y = 0, centred = 0;
    u32 z = 0, rgba = 0, counter = 0;
    int frames = 0;
    std::vector<u16> text;
};
static std::vector<OverText> g_overText;

int RenderMgr_TriggerEffect(void* mgr, u32 type, u32 arg1, u32 arg2) {
    if (type >= 14)         // the world's: decompiled (src/game/world_effects.cpp)
        return (int)guest::addr(RenderMgr_AddWorldEffect(mgr, (int)type, arg1, arg2));
    if (type == 8) {
        g_bars.on = arg1 != 0;
        return 0;
    }
    if (type == 13) {
        struct Desc { u16 x, y; u32 z, rgba; u16 centred, _e; u32 rep, refs; };
        const Desc* d = guest::ptr<Desc>(arg1);
        OverText t;
        t.x = d->x; t.y = d->y; t.z = d->z; t.rgba = d->rgba; t.centred = d->centred;
        t.counter = arg2;
        t.frames = 1;
        if (d->rep != 0)
            for (const u16* c = guest::ptr<u16>(*guest::ptr<u32>(d->rep)); *c != 0; c++)
                t.text.push_back(*c);
        t.text.push_back(0);
        g_overText.clear();                         // (0x195DD0: one at a time)
        g_overText.push_back(std::move(t));
        return 0;
    }
    if (type == 12) {                               // 0x17F1E0
        if (arg1 != g_wash.mode) {
            g_wash.mode = (u8)arg1;
            g_wash.timer = g_wash.total = (s32)arg2;
        }
        return 0;
    }
    if (type == 4) {                                // 0x1826C0: on / off
        if (!g_shimmer.exists)
            Shimmer_Init();
        g_shimmer.on = (u8)arg1 != 0;
        return 0;
    }
    if (type == 2) {
        Mono_Start(arg1);
        return 0;
    }
    if (type != 11) {
        printf("[stub] render effect %u ignored\n", type);
        return 0;
    }
    // 0x17FE40
    // WOTS2_TRACE_FADE=1 (debug): every request for the wipe, and the mode it found.
    static const bool traceFade = getenv("WOTS2_TRACE_FADE") != nullptr;
    if (traceFade)
        printf("[fade] asked %u (%u frames), was %s mode %u\n", arg1, arg2, g_fade.exists ? "there," : "gone,", g_fade.mode);
    g_fade.exists = true;
    const u8 now = g_fade.mode;
    if (arg1 != now && !(now == 2 && arg1 == 1) && !(now == 3 && arg1 == 0)) {
        g_fade.mode = (u8)arg1;
        g_fade.timer = g_fade.total = arg2 != 0 ? (s32)arg2 : 15;
        if (arg1 == 0) {                    // the loading text goes with the black
            *guest::ptr<s32>(0x2EE6C0u) = 0;
            *guest::ptr<s32>(0x2EE6BCu) = 0;
            *guest::ptr<s32>(0x2EE6B8u) = 0;
            *guest::ptr<s32>(0x2EE6B4u) = 0;
        }
    }
    return 0;
}

void RenderMgr_UpdateEffects(void*) {
    TextRoll_Update();
    if (g_wash.timer != 0)                          // 0x17F1C0
        g_wash.timer--;
    if (g_fade.exists) {                            // 0x17FDC0
        if (g_fade.mode == 2) {
            g_fade.timer = 1;
        } else {
            if (g_fade.timer != 0)
                g_fade.timer--;
            if (g_fade.timer == 0) {
                if (g_fade.mode == 1) {
                    g_fade.mode = 2;
                    g_fade.timer = 1;
                } else if (g_fade.mode == 0) {
                    g_fade = {};
                }
            }
        }
    }
    if (g_bars.on) {
        if (g_bars.level < 0x1E)
            g_bars.level++;
    } else if (g_bars.level != 0) {
        g_bars.level--;
    }
    Shimmer_Update();
}

// 0x14F580: the effects numbered below 10 — the letterbox — are drawn by
// the world, before its HUD (a shop's signboard and the money plate sit on
// the bars); the rest (0x14F490: the fade, text) by the main loop, last.
void RenderMgr_Draw(void* mgr) {
    RenderMgr_DrawWorld(mgr);
    if (!getenv("WOTS2_NO_SHIMMER"))
        Shimmer_Draw(guest::ptr(*guest::ptr<u32>(0x2EE670u)));
    Mono_Draw();
    if (g_bars.level == 0)
        return;
    hal::gs::set_blend(hal::gs::Blend::Normal);
    hal::gs::set_alpha_ref(0);
    const u32 black = (u32)(g_bars.level * 0x80 / 0x1E) << 24;
    hal::gs::fill_rect(0, 0, (float)*gScreenW, 62.0f, black);
    hal::gs::fill_rect(0, (float)*gScreenH - 85.0f, (float)*gScreenW, 85.0f, black);
}

void RenderMgr_DrawEffects(void*) {
    hal::gs::set_blend(hal::gs::Blend::Normal);
    hal::gs::set_alpha_ref(0);
    TextRoll_Draw();
    hal::gs::set_blend(hal::gs::Blend::Normal);
    for (auto it = g_overText.begin(); it != g_overText.end();) {
        const bool live = it->counter != 0 ? *guest::ptr<s32>(it->counter) != 0 : it->frames-- > 0;
        if (!live) {
            it = g_overText.erase(it);
            continue;
        }
        // The text has to be in guest memory for the font code.
        static u32 buffer = 0;
        if (buffer == 0)
            buffer = guest::addr(guest::alloc(0x1000));
        const size_t n = it->text.size() < 0x800 ? it->text.size() : 0x800;
        memcpy(guest::ptr(buffer), it->text.data(), n * 2);
        guest::ptr<u16>(buffer)[n - 1] = 0;
        Font* font = guest::ptr<Font>(*guest::ptr<u32>(0x2EE67Cu));
        const u16* text = guest::ptr<const u16>(buffer);
        const u32 old = Font_SetColor(font, 0xFF000515u);
        if (it->centred == 1) {
            Font_DrawCentered(font, text, it->x + 0x20, it->y + 0x10, it->z, 1.2f, 0.9f);
            Font_SetColor(font, it->rgba);
            Font_DrawCentered(font, text, it->x, it->y, it->z, 1.2f, 0.9f);
        } else {
            Font_DrawString(font, text, it->x + 0x20, it->y + 0x10, it->z, 1.2f, 0.9f);
            Font_SetColor(font, it->rgba);
            Font_DrawString(font, text, it->x, it->y, it->z, 1.2f, 0.9f);
        }
        Font_SetColor(font, old);
        ++it;
    }
    // Render effect 12 (0x17EFA0): black from the top of the screen down —
    // the top edge darkens first, then the bottom follows.
    {
        s32 level = 0;
        if (g_wash.mode == 3)
            level = 0x80 - (g_wash.total != 0 ? (g_wash.timer << 7) / g_wash.total : 0);
        else if (g_wash.mode == 2)
            level = g_wash.total != 0 ? (g_wash.timer << 7) / g_wash.total : 0;
        else if (g_wash.mode == 1)
            level = 0x80;
        if (level != 0) {
            const s32 top = level < 0x40 ? level * 2 : 0x80;
            const s32 bottom = level < 0x40 ? 0 : (level - 0x40) * 2;
            hal::gs::set_blend(hal::gs::Blend::Normal);
            hal::gs::set_alpha_ref(0);
            const int strips = 56;
            const float h = (float)*gScreenH / strips;
            for (int i = 0; i < strips; i++) {
                const s32 a = top + (bottom - top) * (2 * i + 1) / (2 * strips);
                hal::gs::fill_rect(0, h * i, (float)*gScreenW, h, (u32)a << 24);
            }
        }
    }
    // 0x17FB10: the leading edge darkens first, then the other follows —
    // covering comes from the left, clearing leaves to the right.
    const s32 level = g_fade.exists ? g_fade.level() : 0;
    if (level == 0)
        return;
    const s32 lead = level < 0x40 ? level * 2 : 0x80, trail = level < 0x40 ? 0 : (level - 0x40) * 2;
    const s32 left = g_fade.mode == 1 ? lead : trail, right = g_fade.mode == 1 ? trail : lead;
    hal::gs::set_blend(hal::gs::Blend::Normal);
    hal::gs::set_alpha_ref(0);
    const int strips = 80;
    // (Native: across the whole picture when it is wider than the game's screen.)
    const float margin = hal::gs::wide_margin();
    const float w = ((float)*gScreenW + 2.0f * margin) / strips;
    for (int i = 0; i < strips; i++) {
        const s32 a = left + (right - left) * (2 * i + 1) / (2 * strips);
        hal::gs::fill_rect(w * i - margin, 0, w, (float)*gScreenH, (u32)a << 24);
    }
}

void RenderMgr_ClearEffects(void* mgr) {
    RenderMgr_ClearWorld(mgr);
    TextRoll_Stop();
    g_fade = {};
    g_wash = {};
    g_bars = {};
    g_shimmer = {};
    Mono_Start(0);
    g_overText.clear();
}
int  RenderMgr_FindEffect(void*, u32 type) {
    if (type == 8)
        return g_bars.level;
    return type == 11 && g_fade.exists;
}
void fn_14e7e0(void*) { TextRoll_Stop(); }     // 0x14E7E0: a text roll in progress goes

// ── Per-frame services with nothing behind them yet ───────────────────────
void Camera_Apply(void*, void*) {}
void Profiler_Begin(void*) {}
void Profiler_End(void*) {}

// ── Sound ─────────────────────────────────────────────────────────────────
// The sound-effect driver proper talks to the IOP's SPU2 voices.  Here the
// bank (sound/common.vgd) is decoded on first use and samples go to the mixer.
//   "VGS1" u32 count, u32 headerSize, u32 bodySize,
//   then {u16 index, u16 rate, u32 offset, u32 size} per sample; bodies are VAG ADPCM.
void SeDriver_Play(void*, int id, u32 pitch, int volumeL, int volumeR) {
    struct Sample { u32 rate = 0; std::vector<s16> pcm; };
    static std::vector<Sample> bank;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::vector<u8> file;
        if (archive::read_asset(0x0B, file) && file.size() > 16 && memcmp(file.data(), "VGS1", 4) == 0) {
            u32 count, headerSize;
            memcpy(&count, &file[4], 4);
            memcpy(&headerSize, &file[8], 4);
            bank.resize(count);
            for (u32 i = 0; i < count && 16 + (i + 1) * 12 <= file.size(); i++) {
                u16 rate; u32 offset, size;
                memcpy(&rate, &file[16 + i * 12 + 2], 2);
                memcpy(&offset, &file[16 + i * 12 + 4], 4);
                memcpy(&size, &file[16 + i * 12 + 8], 4);
                if ((size_t)headerSize + offset + size > file.size())
                    continue;
                bank[i].rate = rate;
                hal::spu2::decode_adpcm(&file[headerSize + offset], size, bank[i].pcm);
            }
            printf("[sound] sound/common.vgd: %u samples\n", count);
        } else {
            fprintf(stderr, "[sound] sound/common.vgd could not be read\n");
        }
    }
    if (id < 0 || (size_t)id >= bank.size() || bank[id].pcm.empty())
        return;
    hal::spu2::sample_play(bank[id].pcm.data(), (u32)bank[id].pcm.size(), bank[id].rate, pitch,
                           volumeL, volumeR);
}

// ── 0x180C70: how loud a sound from a place in the world is in each ear.
// The place is turned into the camera's space (the game: the world-view
// matrix at GsDevice+0x360; here the native camera's): the volume falls off
// from `nearDist` to nothing at `farDist` (a negative near distance: no
// falling off), and the ear on the far side loses by how far to the side
// the sound is.  The menus pass no place, which leaves the volumes alone.
bool hle_world_camera(float* view);
float Sound_ApplyPan(float nearDist, float farDist, int* left, int* right, int positional) {
    // (Some callers pass a small number that is not a place — a null object's
    // +0x80, say; on the PS2 that reads low memory, here it is left alone.)
    if (positional == 0 || (u32)positional < 0x100000u || (u32)positional >= 0x2000000u)
        return 1.0f;
    float view[16];
    if (!hle_world_camera(view))
        return 1.0f;
    const float* p = guest::ptr<float>((u32)positional);
    float v[3];
    for (int k = 0; k < 3; k++)
        v[k] = p[0] * view[k] + p[1] * view[4 + k] + p[2] * view[8 + k] + view[12 + k];
    const float distance = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float side = distance > 0 ? v[0] / distance : 0.0f;
    float gain = 1.0f;
    if (nearDist >= 0.0f) {
        const float slope = -1.0f / (farDist - nearDist);
        gain = slope * distance + (1.0f - slope * nearDist);
        gain = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
    }
    if (*guest::ptr<s32>(0x2EE5E4u) != 0)               // mono output
        side = 0.0f;
    *left = (int)((float)*left * (side < 0.0f ? gain * (side + 1.0f) : gain));
    *right = (int)((float)*right * (side > 0.0f ? gain * (1.0f - side) : gain));
    return gain;
}

// ── Streaming driver (BGM) ────────────────────────────────────────────────
// The game queues commands for the IOP module ToraSt.irx; here they go
// straight to the audio HAL.  "cdrom0:\\BGM\\BGM.TRS;1" becomes <disc>/BGM/BGM.TRS.
static std::string host_path(const char* cdrom) {
    std::string p = cdrom ? cdrom : "";
    size_t colon = p.find(':');
    if (colon != std::string::npos) p = p.substr(colon + 1);
    size_t semi = p.find(';');
    if (semi != std::string::npos) p = p.substr(0, semi);
    for (char& c : p)
        if (c == '\\') c = '/';
    return hle::disc_root() + p;
}

void Stream_Open(void*, const char* path, int voice, u32 fileOffset) {
    hal::spu2::stream_open(voice, host_path(path).c_str(), fileOffset);
}
void Stream_Start(void*, int voice, int loop, int fadeIn) {
    hal::spu2::stream_start(voice, loop != 0, fadeIn != 0);
}
void Stream_Stop(void*, int voice, int fadeOut) { hal::spu2::stream_stop(voice, fadeOut != 0); }
void Stream_Close(void*, int voice)             { hal::spu2::stream_close(voice); }
int  Stream_SetVolume(void*, int voice, u32 left, u32 right) {
    hal::spu2::stream_set_volume(voice, (int)(left & 0x3FFF), (int)(right & 0x3FFF));
    return 0;
}
int  Stream_TogglePause(void*, int voice) { hal::spu2::stream_toggle_pause(voice); return 0; }
void Stream_Update(void*) {}      // the timed stop after a fade-out is done by the HAL
int  Stream_GetStatus(void*, int voice) { return hal::spu2::stream_active(voice) ? 2 : 0; }

// ── Player data and friends ───────────────────────────────────────────────
void* fn_1d6db0() { return nullptr; }
void  fn_1d6a80(void*, int) {}
void  fn_1d6d80() {}
int   Rand_Next() {
    sRandState = sRandState * 6364136223846793005ull + 1;
    return (int)((sRandState >> 32) & 0x7FFFFFFF);
}
void  fn_1656f0() {}
void  fn_21d8f0(void* hud) { if (hud != nullptr && guest::addr(hud) != 0) Hud_Reset(hud); }   // 0x21D8F0, from the scene base's destructor

// ── The native save file ──────────────────────────────────────────────────
// NOT the game's memory card code (the card screens, 0x25E550–0x260050 and
// the title's modes 0 / 1, are not decompiled).  Saving — "Save" on the area
// map's pause menu — writes the whole player data block to a file; pressing
// Start on the title reads it back, and because the block says a game is in
// progress (its first word is 2) "Start" then carries straight on at the
// area map, as the game does after loading from a card.
// WOTS2_SAVE=<file> names the file (default wots2.sav in the current
// directory); WOTS2_NEW_GAME=1 ignores it.
static const char* save_path() {
    const char* path = getenv("WOTS2_SAVE");
    return path != nullptr && *path != 0 ? path : "wots2.sav";
}
static const u32 kSaveMagic = 0x32535457u;                  // "WTS2"
static const u32 kSaveSize = 0x7000;
bool hle_load_save() {
    // (Every time Start is pressed on the title, as a card would be read: so
    // after a death the last save comes back.)
    if (getenv("WOTS2_NEW_GAME") != nullptr)
        return false;
    FILE* f = fopen(save_path(), "rb");
    if (f == nullptr)
        return false;
    u32 magic = 0;
    std::vector<u8> block(kSaveSize);
    const bool ok = fread(&magic, 4, 1, f) == 1 && magic == kSaveMagic && fread(block.data(), kSaveSize, 1, f) == 1;
    fclose(f);
    if (!ok) {
        printf("[save] %s is not a save file of this build\n", save_path());
        return false;
    }
    memcpy(guest::ptr<u8>(*gPlayerData), block.data(), kSaveSize);
    printf("[save] loaded %s (day %d, period %d, area %d)\n", save_path(), block[0x100] + 1, block[0x101], block[0x103]);
    return true;
}

// ── GameScene mode objects ────────────────────────────────────────────────
// (Mode 1 — the load screen — is src/game/mcard.cpp.)
// Debug: WOTS2_GIVE_ITEMS=n puts n of every item kind in stock, as if a save
// with them had been loaded.
void hle_debug_after_load() {
    if (const char* give = getenv("WOTS2_GIVE_ITEMS")) {
        u8* pd = guest::ptr<u8>(*gPlayerData);
        for (int i = 0; i < 21; i++) {
            pd[0x6CC0 + i] = (u8)atoi(give);
            pd[0x6CD5 + i] = 1;
        }
    }
}
// (Mode 0 — the boot check of the memory card — is src/game/mcard.cpp.)


// The resource manager's constructor and loader are not decompiled (they also
// set up 9.5 MB of memory pools and the parameter and motion lists); this
// builds just enough of the object for the decompiled character-list code.
CharaResources* CharaResources_Get() {
    static CharaResources* instance = nullptr;
    if (!instance) {
        instance = static_cast<CharaResources*>(guest::alloc(sizeof(CharaResources)));
        GuestList_Init(&instance->charas);
        GuestList_Init(&instance->params);
        GuestList_Init(&instance->templates);
        GuestList_Init(&instance->motionNames);
        // The motions every pack shares, read at boot in the original
        // (0x1E1890: asset 0x8B into the table at 0x30A6E0).
        std::vector<u8> common;
        if (archive::read_asset(0x8B, common)) {
            common.resize((common.size() + 7) & ~(size_t)3);
            CommonMotions_Load(reinterpret_cast<const u32*>(common.data()));
        }
        CharaResources_InitMotions(instance);
        // The fight AI's tables, also read at boot (0x1E1878 / 0x29A7D0: asset 0x85).
        std::vector<u8> battle;
        if (archive::read_asset(0x85, battle)) {
            battle.resize(battle.size() + 16);
            BattleAi_Create(battle.data());
        }
        *guest::ptr<u32>(0x2EE63C) = guest::addr(instance);
    }
    return instance;
}
void CharaResources_Load(CharaResources* self, int force) {
    if (!force && self->loaded)
        return;
    std::vector<u8> file;
    if (archive::read_asset(0x28, file)) {                 // Common/chara/list
        file.resize((file.size() + 7) & ~(size_t)3);
        CharaResources_ParseList(self, reinterpret_cast<const u32*>(file.data()));
    }
    if (archive::read_asset(0x29, file)) {                 // chara/paramlist
        file.resize((file.size() + 7) & ~(size_t)3);
        CharaResources_ParseParams(self, reinterpret_cast<const u32*>(file.data()));
    }
    if (archive::read_asset(0x2A, file)) {                 // Common/motion/list
        file.resize((file.size() + 7) & ~(size_t)3);
        CharaResources_ParseMotionList(self, reinterpret_cast<const u32*>(file.data()));
    }
    self->loaded = 1;
}

static void mat_mul(const float* a, const float* b, float* out) {      // out = a × b, row vectors
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof(r));
}

// ── World → screen, through the stand-in world's camera ───────────────────
// (The real 0x12F7E0 uses the device's own matrices.)  `out` = {x, y} in GS
// primitive coordinates (1/16 pixel); returns 0 when the point is not in view.
static bool project_point(const float* world, float* px, float* py, float* w) {
    float m[16];
    if (!hle_world_last_view(m))
        return false;
    const float x = world[0], y = world[1], z = world[2];
    const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
    const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
    const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
    *w = cw;
    if (cw < 0.0001f)
        return false;
    *px = (cx / cw * 0.5f + 0.5f) * 640.0f;
    *py = (0.5f - cy / cw * 0.5f) * 448.0f;
    return true;
}
// (out[2] is the depth as the GS has it — larger = nearer — times 16, like
// the real one's; GsDevice_DrawTexturedPrim turns it back.)
// 0x172160: the effects' "face the viewer" matrix at 0x309E50 — the view's
// rotation turned back (the real one inverts the device's world-view matrix).
void Fx_PrepareView() {
    float view[16];
    float* out = guest::ptr<float>(0x309E50u);
    memset(out, 0, 64);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
    if (!hle_world_camera(view))
        return;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i * 4 + j] = view[j * 4 + i];
}
int GsDevice_Project(void*, s32* out, const float* world) {
    float px, py, w;
    if (!project_point(world, &px, &py, &w))
        return 0;
    out[0] = (s32)((px + kOriginX) * 16.0f);
    out[1] = (s32)((py + kOriginY) * 16.0f);
    {
        float m[16];
        hle_world_last_view(m);
        float d = (world[0] * m[2] + world[1] * m[6] + world[2] * m[10] + m[14]) / w;
        d = d < -1.0f ? -1.0f : d > 1.0f ? 1.0f : d;
        out[2] = (s32)((1.0 - (d * 0.5 + 0.5)) * 16777215.0) << 4;
        out[3] = 0;
    }
    return px >= 0.0f && px <= 640.0f && py >= 0.0f && py <= 448.0f;
}
int hle_project_extent(const float* world, float radius, s32* rect) {
    float px, py, w, qx, qy, qw;
    const float above[3] = {world[0], world[1] + radius, world[2]};
    if (!project_point(world, &px, &py, &w) || !project_point(above, &qx, &qy, &qw))
        return 0;
    const float r = fabsf(py - qy);
    rect[0] = (s32)((px - r + kOriginX) * 16.0f);
    rect[1] = (s32)((py - r + kOriginY) * 16.0f);
    rect[2] = (s32)((px + r + kOriginX) * 16.0f);
    rect[3] = (s32)((py + r + kOriginY) * 16.0f);
    return 1;
}

// 0x166920: the lighting object's lights — four directions and four colours
// (row 3 the ambient).  Its upload to VU1 (0x166910) is the native model
// draw's business: the lights go to the native renderer here.
void Light_SetMatrices(void*, const void* directions, const void* colours) {
    model::set_scene_light(static_cast<const float (*)[4]>(colours), static_cast<const float (*)[4]>(directions));
}
void Light_Upload(void*, void*) {}

// Sony libvu0 matrices: 4×4 floats, row-vector convention.
void Matrix_Identity(void* m) {
    float* f = static_cast<float*>(m);
    memset(f, 0, 64);
    f[0] = f[5] = f[10] = f[15] = 1.0f;
}
// dst = src turned by `angle` about the vertical axis.  (Which axis the
// original turns about has not been verified; the game only uses it to turn
// characters where they stand.)
void Matrix_Rotate(void* dst, const void* src, float angle) {
    const float c = cosf(angle), s = sinf(angle);
    const float r[16] = {c, 0, -s, 0,  0, 1, 0, 0,  s, 0, c, 0,  0, 0, 0, 1};
    mat_mul(static_cast<const float*>(src), r, static_cast<float*>(dst));
}
// dst = b × a: b is applied first.
void Matrix_Multiply(void* dst, const void* a, const void* b) {
    mat_mul(static_cast<const float*>(b), static_cast<const float*>(a), static_cast<float*>(dst));
}

// ── Item icons: none yet ──────────────────────────────────────────────────

// ── Character-setup sub-screens: not decompiled ───────────────────────────
// Their constructors fail, which the screen treats as "could not open" and
// returns to its menu.
static void* missing_screen(void* mem, const char* what) {
    printf("[scene] %s is not decompiled yet\n", what);
    guest::free(mem);
    return nullptr;
}

}  // extern "C"

