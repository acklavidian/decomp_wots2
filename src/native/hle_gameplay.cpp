// Stand-ins for the parts of the Gameplay scene that are not decompiled yet.
// NOT decompiled code.  The scene's shell, the world class, the pause menu,
// the area map, the calendar and the map database are real; what they drive
// is replaced here by the least that lets them run:
//
//   MapState     loads the map model of an area and draws it with the native
//                model code, from a camera that follows the player
//   CharaMgr     (in hle.cpp) one character; here: the d-pad walks the player
//                over the map's collision grid (decompiled; blocked cells stop
//                it; exit zones, doors and hint spots come from its cell types),
//                L1 / R1 turn the camera, R2 leaves the area from anywhere (with WOTS2_DEBUG_KEYS=1).
//                No NPCs or fighting.
//   EventPlayer  never has a script to run, so story scenes are skipped
//   the HUD's remaining widgets, the save screen and the rest do nothing
//
// Each reports itself once on stdout.  Delete a stand-in when the real thing
// is decompiled.

#include "game/gameplay_scene.h"
#include "game/ai_moves.h"
#include "game/map_collision.h"
#include "game/map_db.h"
#include "game/chara_resources.h"
#include "game/player_data.h"
#include "game/spawner.h"
#include "engine/containers.h"
#include "engine/pad.h"
#include "native/archive.h"
#include "native/guest.h"
#include "native/editor_hooks.h"
#include "native/hle.h"
#include "native/model.h"
#include "hal/gs.h"
#include "hal/pad.h"
#include "hal/spu2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
    void* PadMgr_Get();
    u16   Pad_GetHeld(void* mgr, int port);
    u16   Pad_GetPressed(void* mgr, int port);
    int   RenderMgr_TriggerEffect(void* mgr, u32 type, u32 arg1, u32 arg2);
}

namespace {

void note_once(const char* what) {
    static std::set<std::string> seen;
    if (seen.insert(what).second)
        printf("[stub] %s\n", what);
}

u32 make_dtor_vtable(void (*dtor)(void*, int)) {
    u32* vt = static_cast<u32*>(guest::alloc(0x10));
    vt[2] = guest::fake_address();
    guest::bind(vt[2], dtor, "native");
    return guest::addr(vt);
}

void free_object(void* self, int flag) {
    if (self && (s16)flag > 0)
        guest::free(self);
}

void* zeroed(u32 size) {
    void* p = guest::alloc(size);
    memset(p, 0, size);
    return p;
}

void mul(const float* a, const float* b, float* out) {      // out = a × b, row vectors
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof(r));
}

// The loaded maps, by MapState object.
struct NativeMap {
    model::Model model;
    bool loaded = false;
    u32  clearColor = 0x80000000u;
    float fog[4] = {0, 0, 0, 0};
    u32  collision = 0;         // MapCollision (decompiled), in guest memory
    u32  collisionFile = 0;
    u32  collisionSize = 0;
    int  index = -1;            // (for the level editor)
    std::string modelPath, collisionPath;
    // The sky: a dome that goes where the camera goes and turns very slowly
    // (Sky_Draw 0x179500; the class is 0x179470–0x179A40).
    model::Model sky;
    bool  hasSky = false;
    float skyScale = 1.0f;
    float skyAngle = 0.0f;      // degrees, 0–180…
    bool  skyTurned = false;    // …and then the other half of the circle
    // The water: a level surface under the map, seen where the ground is
    // open (moats, rivers).  Water_Draw 0x158210; the class is 0x158210–0x158D00.
    sprites::Sheet water;       // Common/map/etc/wateranime.pim: thirty frames
    bool  hasWater = false;
    float waterHeight = -0.5f;
    int   waterFrame = 0;
    float waterScroll = 0.0f;
};
std::map<void*, NativeMap*> g_maps;
NativeMap* g_currentMap = nullptr;

// The camera: behind and above the player.
struct View {
    int   frame = -1;           // frame the matrix below was built in
    float viewProj[16];
    float view[16];             // world → camera (x right, y up, the viewer looks down −z)
    float eye[3], dir[3];
} g_view;

PlayerData* player() { return guest::ptr<PlayerData>(*guest::ptr<u32>(0x2EE688u)); }

const MapInfo* map_info(int index) {
    MapDb* db = MapDb_Get();
    MapDb_Load(db, 0);
    int i = 0;
    for (GuestListNode* n = GuestList_Begin(&db->maps); n != GuestList_End(&db->maps); n = GuestList_Next(n), i++)
        if (i == index)
            return guest::ptr<MapInfo>(n->value);
    return nullptr;
}

void load_map(void* state, int index) {
    NativeMap* m = g_maps[state];
    if (!m)
        return;
    m->model.release();
    m->model = model::Model();
    m->loaded = false;
    m->index = index;
    const MapInfo* info = map_info(index);
    m->modelPath = info && info->model ? guest::ptr<const char>(info->model) : "";
    m->collisionPath = info && info->collision ? guest::ptr<const char>(info->collision) : "";
    std::vector<u8> file;
    if (info && archive::read(guest::ptr<const char>(info->model), file) && m->model.load(file)) {
        m->loaded = true;
        m->clearColor = 0x80000000u | info->color[0] | (u32)info->color[1] << 8 | (u32)info->color[2] << 16;
        memcpy(m->fog, info->fog, sizeof m->fog);
        printf("[world] map %d: %s\n", index, guest::ptr<const char>(info->model));
        m->water.release();
        m->hasWater = false;
        std::vector<u8> water;
        if (info->water && archive::read(guest::ptr<const char>(info->water), water) && m->water.load(water) &&
            !m->water.textures.empty()) {
            m->hasWater = true;
            memcpy(&m->waterHeight, &info->waterParam, 4);
            printf("[world] water: %s (%zu frames) at %.2f\n", guest::ptr<const char>(info->water),
                   m->water.textures.size(), m->waterHeight);
        }
        m->sky.release();
        m->hasSky = false;
        std::vector<u8> sky;
        if (info->sky && archive::read(guest::ptr<const char>(info->sky), sky) && m->sky.load(sky)) {
            m->hasSky = true;
            memcpy(&m->skyScale, &info->skyParam, 4);
            if (!(m->skyScale > 0.0f))
                m->skyScale = 1.0f;
            printf("[world] sky: %s (scale %.2f, %zu bones)\n", guest::ptr<const char>(info->sky), m->skyScale,
                   m->sky.bones.size());
        }
        model::set_scene_light(info->light, info->lightColor);
        if (getenv("WOTS2_DUMP_LIGHT"))
            printf("[light] fog %.3f %.3f %.3f %.3f  far %.1f  clear %d %d %d\n", info->fog[0], info->fog[1], info->fog[2],
                   info->fog[3], info->_18, info->color[0], info->color[1], info->color[2]);
        if (getenv("WOTS2_DUMP_LIGHT"))
            for (int i = 0; i < 4; i++)
                printf("[light] %d dir %.2f %.2f %.2f %.2f  colour %.2f %.2f %.2f %.2f\n", i, info->light[i][0], info->light[i][1],
                       info->light[i][2], info->light[i][3], info->lightColor[i][0], info->lightColor[i][1],
                       info->lightColor[i][2], info->lightColor[i][3]);
    } else {
        fprintf(stderr, "[world] map %d could not be loaded\n", index);
    }
    // The collision file is read by the decompiled code.
    MapCollision* holder = guest::ptr<MapCollision>(m->collision);
    if (holder->collision)
        Collision_Dtor(guest::ptr<Collision>(holder->collision), 1);
    holder->collision = 0;
    if (m->collisionFile)
        guest::free(guest::ptr(m->collisionFile));
    m->collisionFile = 0;
    if (info && info->collision && archive::read(guest::ptr<const char>(info->collision), file)) {
        void* copy = guest::alloc((u32)file.size());
        memcpy(copy, file.data(), file.size());
        m->collisionFile = guest::addr(copy);
        m->collisionSize = (u32)file.size();
        MapCollision_SetFile(holder, static_cast<const s32*>(copy));
    }
    g_currentMap = m;
    // Which route points can see which, and the ways between them (the real
    // loader does this as its last step).  This stand-in keeps one map, so
    // the tables for outdoors and indoors are both made from it.
    //
    // The tables are made from the route points of the player's area
    // (PlayerData+0x103).  When a script loads a map of another area
    // (World_GoToMap) the area is only set after the load is begun — the real
    // loader is still reading then and makes the tables later, with the new
    // area; this one is done at once, so it must look the map's area up
    // itself (the world's table at 0x2D8F00), or people on that map would be
    // given the ways of the area the player came from and find none.
    u8* pd = *guest::ptr<u32>(0x2EE688u) ? guest::ptr<u8>(*guest::ptr<u32>(0x2EE688u)) : nullptr;
    const u8 savedArea = pd ? pd[0x103] : 0;
    if (pd != nullptr) {
        const s16* areaMaps = guest::ptr<const s16>(0x2D8F00u);
        for (int area = 0; area < 11; area++)
            for (const s16* entry = areaMaps + area * 0x14; *entry != -1; entry++)
                if (*entry == index)
                    pd[0x103] = (u8)area;
    }
    s32* indoors = guest::ptr<s32>(0x2EE518u);
    const s32 saved = *indoors;
    for (s32 i = 0; i < 2; i++) {
        *indoors = i;
        MapState_LinkRoutePoints(state);
        MapState_BuildRouteTable(state);
    }
    *indoors = saved;
    if (pd != nullptr)
        pd[0x103] = savedArea;
}

}  // namespace

extern "C" {

// ══ Map state ═════════════════════════════════════════════════════════════
void* MapState_Ctor(void* mem) {
    note_once("world: the map is loaded and drawn by native code (the map state is a stand-in)");
    memset(mem, 0, 0x80);
    static_cast<s32*>(mem)[3] = -1;                 // no load pending (outdoors / indoors)
    static_cast<s32*>(mem)[4] = -1;
    // As the original's (0x14C3C0): a new map state starts out of doors.
    // Without it, a world made while the flag was still set — after resting
    // in a house, say, which leaves by way of the area map — never ended its
    // walk-in (World_IntroDone answers 0 indoors) and kept the bars for good.
    *guest::ptr<s32>(0x2EE518u) = 0;
    RouteTables_Init(reinterpret_cast<RouteTables*>(static_cast<u8*>(mem) + 0x20));
    NativeMap* m = new NativeMap;
    m->collision = guest::addr(MapCollision_Ctor(static_cast<MapCollision*>(guest::alloc(sizeof(MapCollision)))));
    g_maps[mem] = m;
    return mem;
}
void MapState_Dtor(void* self, int flag) {
    auto it = g_maps.find(self);
    if (it != g_maps.end()) {
        if (g_currentMap == it->second)
            g_currentMap = nullptr;
        it->second->model.release();
        it->second->sky.release();
        it->second->water.release();
        MapCollision_Dtor(guest::ptr<MapCollision>(it->second->collision), 1);
        if (it->second->collisionFile)
            guest::free(guest::ptr(it->second->collisionFile));
        delete it->second;
        g_maps.erase(it);
    }
    free_object(self, flag);
}
void MapState_Load(void* self, int map)     { load_map(self, map); }
void MapState_SetCurrent(void*, int)        {}
int  MapState_IsLoaded(void*)               { return 1; }
void MapState_LoadNext(void* self, int map) { load_map(self, map); }
void MapState_SetNext(void*, int)           {}
void MapState_Return(void* self, int map)   { load_map(self, map); }
void MapState_BeginDraw(void*, void*)       {}
void MapState_EndDraw(void*)                {}
u32  MapState_GetMap(void* self) {
    NativeMap* m = g_maps[self];
    return m ? m->collision : 0;
}

extern "C" int Lights_Gather(float* out, int max);     // point_lights.cpp
// The world is drawn (World_Draw): the modern lighting's last part.
extern "C" void Native_WorldPost() { hal::gs::world_post(); }
extern "C" int Native_RealShadows() { return hal::gs::real_shadows() ? 1 : 0; }

void MapState_Draw(void* self, const float*, void* stage, void*) {
    NativeMap* m = g_maps[self];
    if (!m)
        return;
    hal::gs::clear(m->clearColor);
    // The map's fog (MapInfo+0xB0 = what the game hands GsDevice_SetFogParams),
    // toward the clear colour.  WOTS2_NO_FOG=1 (debug) switches it off.
    static const bool noFog = getenv("WOTS2_NO_FOG") != nullptr;
    hal::gs::set_fog(!noFog && m->fog[3] > m->fog[2], m->fog[0], m->fog[1], m->fog[2], m->fog[3], m->clearColor);
    // The view is the decompiled stage's: an eye, the way it looks, and up —
    // unless the level editor is flying its own camera.
    const float* cam = static_cast<const float*>(stage);
    float editorCam[12];
    if (Editor_Camera(editorCam, editorCam + 4, editorCam + 8))
        cam = editorCam;
    // Debug: WOTS2_VIEW="ex,ey,ez,dx,dy,dz" draws the world from there (an F4
    // line's camera), for screenshots of a reported spot.
    static float fixedCam[12];
    static const bool fixedView = [] {
        const char* v = getenv("WOTS2_VIEW");
        if (v == nullptr)
            return false;
        float* c = fixedCam;
        const int n = sscanf(v, "%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[4], &c[5], &c[6]);
        c[3] = c[7] = 1.0f;
        c[8] = 0.0f; c[9] = 1.0f; c[10] = 0.0f; c[11] = 0.0f;
        return n == 6;
    }();
    if (fixedView)
        cam = fixedCam;
    memcpy(g_view.eye, cam, sizeof g_view.eye);
    memcpy(g_view.dir, cam + 4, sizeof g_view.dir);
    const float* eye = cam;
    float z[3] = {-cam[4], -cam[5], -cam[6]};
    float x[3] = {cam[9] * z[2] - cam[10] * z[1], cam[10] * z[0] - cam[8] * z[2], cam[8] * z[1] - cam[9] * z[0]};
    const float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (float& c : x) c /= xl > 0 ? xl : 1;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const float view[16] = {x[0], y[0], z[0], 0,  x[1], y[1], z[1], 0,  x[2], y[2], z[2], 0,
                            -(eye[0] * x[0] + eye[1] * x[1] + eye[2] * x[2]),
                            -(eye[0] * y[0] + eye[1] * y[1] + eye[2] * y[2]),
                            -(eye[0] * z[0] + eye[1] * z[1] + eye[2] * z[2]), 1};
    // The game's projection (GsDevice_SetProjection 0x12F8A0 with the numbers of
    // 0x1315D0): the screen is 512 away, 640 × 448 with y scaled by 0.94 for
    // NTSC; near plane 0.1.  Its far plane is 64, hidden by fog; there is no
    // fog here yet, so the native one is further out.
    const float zn = 0.1f, zf = 400.0f;
    const float zoom = *guest::ptr<float>(0x2EDD30u) > 0 ? *guest::ptr<float>(0x2EDD30u) : 1.0f;    // (Camera.SetFOV)
    const float proj[16] = {zoom * 512.0f / 320.0f, 0, 0, 0,  0, zoom * 512.0f * 0.94f / 224.0f, 0, 0,
                            0, 0, (zf + zn) / (zn - zf), -1,  0, 0, 2 * zf * zn / (zn - zf), 0};
    mul(view, proj, g_view.viewProj);
    memcpy(g_view.view, view, sizeof view);
    g_view.frame = hal::gs::frame_count();
    {   // For the modern lighting (a mod's): the view, the sun, the point lights.
        const model::Lighting& sun = model::scene_light();
        static float lights[64 * 8];
        const int n = Lights_Gather(lights, 64);
        hal::gs::set_world(view, proj, sun.dir[0], sun.colour[0], lights, n, sun.ambient);
        static const bool traceLight = getenv("WOTS2_TRACE_LIGHT") != nullptr;
        if (traceLight)
            printf("[light] sun %.2f %.2f %.2f from %.2f %.2f %.2f, ambient %.2f %.2f %.2f\n", sun.colour[0][0],
                   sun.colour[0][1], sun.colour[0][2], sun.dir[0][0], sun.dir[0][1], sun.dir[0][2], sun.ambient[0],
                   sun.ambient[1], sun.ambient[2]);
        // For the wind and dust: the game's gust (the render manager's wind,
        // RenderMgr+0x50, set by World_Step from the gust generator) and the player.
        const u32 renderMgr = *guest::ptr<u32>(0x2EE684u);
        const CharaParams* player = CharaResources_GetParams(CharaResources_Get(), 0);
        if (renderMgr != 0 && player != nullptr) {
            const float* wind = guest::ptr<float>(renderMgr + 0x50);
            const u32 playerData = *guest::ptr<u32>(0x2EE688u);
            const bool night = playerData != 0 && *guest::ptr<s8>(playerData + 0x102) != 0;
            hal::gs::set_ambience(wind, &player->matrix[12], *guest::ptr<s32>(0x2EE518u) == 1, night);
            static const bool traceWind = getenv("WOTS2_TRACE_WIND") != nullptr;
            if (traceWind)
                printf("[wind] %.4f %.4f %.4f\n", wind[0], wind[1], wind[2]);
        }
    }
    if (m->hasSky && !getenv("WOTS2_NO_SKY")) {
        // 0.002° a frame; no fog; behind everything (the original gives it a
        // projection of its own with the far plane a long way off — here its
        // depth is simply pinned to the far plane).
        m->skyAngle += 0.002f;
        if (m->skyAngle >= 180.0f) {
            m->skyAngle = 0.0f;
            m->skyTurned = !m->skyTurned;
        }
        const float a = (m->skyAngle + (m->skyTurned ? 180.0f : 0.0f)) * 3.14159265f / 180.0f;
        const float c = cosf(a) * m->skyScale, s = sinf(a) * m->skyScale;
        const float world[16] = {c, 0, -s, 0,  0, m->skyScale, 0, 0,  s, 0, c, 0,  eye[0], eye[1], eye[2], 1};
        std::vector<float> pose(m->sky.bones.size() * 16);
        for (size_t i = 0; i < m->sky.bones.size(); i++)
            mul(m->sky.bones[i].rest, world, &pose[i * 16]);
        float skyProj[16];
        memcpy(skyProj, proj, sizeof proj);
        skyProj[10] = -0.99999f;
        skyProj[14] = 0.0f;
        float skyViewProj[16];
        mul(view, skyProj, skyViewProj);
        hal::gs::set_fog(false);
        hal::gs::begin_3d(skyViewProj, hal::gs::View::Sky);
        m->sky.draw(pose.empty() ? nullptr : pose.data(), nullptr);
        hal::gs::end_3d();
        hal::gs::set_fog(!noFog && m->fog[3] > m->fog[2], m->fog[0], m->fog[1], m->fog[2], m->fog[3], m->clearColor);
    }
    if (m->loaded) {
        hal::gs::begin_3d(g_view.viewProj, hal::gs::View::Map);
        m->model.draw(nullptr, nullptr);
        hal::gs::end_3d();
    }
    if (m->loaded && m->hasWater && !getenv("WOTS2_NO_WATER")) {
        // The original draws the frame of the animation (a new one every
        // frame, thirty of them) into a texture, sliding it 0.002 a frame,
        // and lays that over the plane y = height, repeating every 8 units,
        // out to where the fog takes it — two quads built from the view
        // frustum (0x1594F0, 0x158F10).  Here it is a field of 8-unit tiles
        // round the camera, behind the map by the depth test.
        if (++m->waterFrame > 0x1D)
            m->waterFrame = 0;
        m->waterScroll += 0.002f;
        if (m->waterScroll > 1.0f)
            m->waterScroll -= 1.0f;
        const int frame = m->waterFrame % (int)m->water.textures.size();
        const u32 tex = m->water.texture_for(sprites::Rect{0, 0, 0, 0, 0, (u8)frame});
        if (tex != 0) {
            const float reach = m->fog[3] > m->fog[2] ? m->fog[3] : 96.0f;
            const int tiles = (int)(reach / 8.0f) + 1;
            const float shift = m->waterScroll * 8.0f;
            const float x0 = floorf(eye[0] / 8.0f) * 8.0f - shift, z0 = floorf(eye[2] / 8.0f) * 8.0f;
            std::vector<hal::gs::Vtx3> verts;
            verts.reserve((size_t)(tiles * 2 + 1) * (tiles * 2) * 6);
            const float y = m->waterHeight;
            for (int i = -tiles; i <= tiles; i++)
                for (int j = -tiles; j < tiles; j++) {
                    const float xa = x0 + i * 8.0f, xb = xa + 8.0f, za = z0 + j * 8.0f, zb = za + 8.0f;
                    const hal::gs::Vtx3 q[4] = {{xa, y, za, 0, 0, 0x80808080u}, {xb, y, za, 1, 0, 0x80808080u},
                                                {xa, y, zb, 0, 1, 0x80808080u}, {xb, y, zb, 1, 1, 0x80808080u}};
                    verts.insert(verts.end(), {q[0], q[1], q[2], q[2], q[1], q[3]});
                }
            hal::gs::begin_3d(g_view.viewProj);
            hal::gs::draw_mesh(tex, verts.data(), (int)verts.size(), hal::gs::Prim::Triangles);
            hal::gs::end_3d();
        }
    }
}

// The most recent world-to-clip matrix, whichever frame it was made in (for
// code that places things on screen before the frame is drawn).
bool hle_world_last_view(float* viewProj) {
    if (!g_currentMap || g_view.frame < 0)
        return false;
    memcpy(viewProj, g_view.viewProj, sizeof(g_view.viewProj));
    return true;
}

// The most recent world-to-camera matrix (for sounds heard from a place).
bool hle_world_camera(float* view) {
    if (!g_currentMap || g_view.frame < 0)
        return false;
    memcpy(view, g_view.view, sizeof(g_view.view));
    return true;
}

bool hle_world_view(float* viewProj) {
    if (g_view.frame != hal::gs::frame_count())
        return false;
    memcpy(viewProj, g_view.viewProj, sizeof(g_view.viewProj));
    return true;
}

// Walks the character whose model matrix this is: d-pad relative to the
// camera; L1 / R1 turn the camera.
static float g_playerPos[4] = {0, 0, 0, 1};
static bool  g_playerMoving = false;

// The collision cell the player stands on, or null.
static ColCellType* cell_under_player() {
    if (!g_currentMap)
        return nullptr;
    MapCollision* holder = guest::ptr<MapCollision>(g_currentMap->collision);
    if (!holder->collision)
        return nullptr;
    return Collision_CellAt(guest::ptr<Collision>(holder->collision), g_playerPos);
}

// Called with the player's matrix before its map step each frame.  The
// camera swings round behind the way the player walks (the game's follow
// camera is not decompiled).
extern "C" int RenderMgr_FindEffect(void* mgr, u32 type);
extern "C" void Camera_GetViewRotation(float* matrix);
static const GuestPtrVar<void> gWarpRenderMgr{0x2EE684u};
static int g_settledOutdoors = 0;     // frames the player has been free out of doors
int hle_world_settled() { return g_settledOutdoors; }
void hle_world_follow(float* matrix) {
    // Debug: WOTS2_WARP="frame:x,z" moves the player there at that frame;
    // several, separated by ';', and "every N:x,z" (from frame N, every N
    // frames) for runs that go round and round.  "…:x,z>tx,tz" then walks
    // it toward (tx, tz) for 45 frames — pad directions go by the camera,
    // so a scripted run cannot simply hold one to reach a door.
    struct Warp { int frame; bool every; float x, z; bool walk; float tx, tz; };
    static std::vector<Warp> warps;
    static bool parsed = false;
    static int walkFrames = 0;
    static float walkTo[2];
    if (!parsed) {
        parsed = true;
        if (const char* spec = getenv("WOTS2_WARP")) {
            while (*spec != 0) {
                Warp w{-1, false, 0, 0, false, 0, 0};
                if (strncmp(spec, "every ", 6) == 0) {
                    w.every = true;
                    spec += 6;
                }
                const int got = sscanf(spec, "%d:%f,%f>%f,%f", &w.frame, &w.x, &w.z, &w.tx, &w.tz);
                w.walk = got == 5;
                if (got >= 3 && w.frame > 0)
                    warps.push_back(w);
                const char* next = strchr(spec, ';');
                if (next == nullptr)
                    break;
                spec = next + 1;
            }
        }
    }
    // A repeating warp waits until the player has been free out of doors
    // for three seconds — moved during the walk-in on arrival it may never
    // reach where it is walking to — and, with WOTS2_WARP_AREA=n, in area n.
    static const int warpArea = getenv("WOTS2_WARP_AREA") ? atoi(getenv("WOTS2_WARP_AREA")) : -1;
    static int settled = 0;
    const bool free = RenderMgr_FindEffect(*gWarpRenderMgr, 8) == 0 && RenderMgr_FindEffect(*gWarpRenderMgr, 11) == 0 &&
                      *guest::ptr<s32>(0x2EE518u) == 0 &&
                      (warpArea < 0 || (*guest::ptr<u32>(0x2EE688u) != 0 && guest::ptr<s8>(*guest::ptr<u32>(0x2EE688u))[0x103] == warpArea));
    settled = free ? settled + 1 : 0;
    g_settledOutdoors = RenderMgr_FindEffect(*gWarpRenderMgr, 8) == 0 && RenderMgr_FindEffect(*gWarpRenderMgr, 11) == 0 &&
                        *guest::ptr<s32>(0x2EE518u) == 0 ? g_settledOutdoors + 1 : 0;
    for (const Warp& w : warps) {
        const int frame = (int)hal::gs::frame_count();
        if (w.every ? frame % w.frame == 0 && settled >= 90 : frame == w.frame) {
            matrix[12] = w.x;
            matrix[14] = w.z;
            if (w.walk) {
                walkFrames = 45;
                walkTo[0] = w.tx;
                walkTo[1] = w.tz;
            }
        }
    }
    if (walkFrames > 0) {
        walkFrames--;
        const float wx = walkTo[0] - matrix[12], wz = walkTo[1] - matrix[14];
        const float far = sqrtf(wx * wx + wz * wz);
        if (far > 0.2f && far < 6.0f && walkFrames < 35 && *guest::ptr<u32>(0x2EE528u) != 0) {
            // The pad routine turns (−x, 0, −y) of the stick by the camera's
            // heading; the other way round here.
            alignas(16) float m[16];
            Camera_GetViewRotation(m);
            const float lx = (wx * m[0] + wz * m[2]) / far, lz = (wx * m[8] + wz * m[10]) / far;
            const float big = fabsf(lx) > fabsf(lz) ? fabsf(lx) : fabsf(lz);
            const int sx = 0x80 - (int)(lx / big * 127.0f), sy = 0x80 - (int)(lz / big * 127.0f);
            hal::pad::debug_stick((u8)(sx < 1 ? 1 : sx > 255 ? 255 : sx), (u8)(sy < 1 ? 1 : sy > 255 ? 255 : sy));
        }
    }
    const float dx = matrix[12] - g_playerPos[0], dz = matrix[14] - g_playerPos[2];
    const float moved = sqrtf(dx * dx + dz * dz);
    g_playerMoving = moved > 0.001f && moved < 1.0f;
    memcpy(g_playerPos, matrix + 12, 12);
}

// ══ What the stand-in characters (hle_chara.cpp) ask of the stand-in world ══
bool hle_world_active() { return g_currentMap != nullptr; }
bool hle_player_moving() { return g_playerMoving; }

// The real character class looks up the collision cell it stands on each
// frame and keeps the cell's flags and index (Chara+0x154 / +0x15C).
u32 hle_cell_at(u32 map, const float* position, s32* index) {
    *index = 0;
    MapCollision* holder = guest::ptr<MapCollision>(map);
    if (!holder || !holder->collision)
        return 0;
    float p[4] = {position[0], position[1], position[2], 1.0f};
    ColCellType* cell = Collision_CellAt(guest::ptr<Collision>(holder->collision), p);
    if (!cell)
        return 0;
    *index = cell->_0c;
    return cell->flags;
}

// Where the player appears inside an interior: its entrance (points 1 and 2).
// (In the game the interior's script does this.)
bool hle_interior_entrance(float* stand, float* face) {
    if (!g_currentMap)
        return false;
    MapCollision* map = guest::ptr<MapCollision>(g_currentMap->collision);
    GuestList* found = Map_FindPoints(map, 1);
    if (found->size == 0)
        return false;
    memcpy(stand, guest::ptr<float>(GuestList_Begin(found)->value), 16);
    found = Map_FindPoints(map, 2);
    if (found->size == 0)
        return false;
    memcpy(face, guest::ptr<float>(GuestList_Begin(found)->value), 16);
    return true;
}

// ══ Stage, camera and the small world objects ═════════════════════════════
void  Camera_Update(void*, GsDevice*) {}
void  Camera_Release(void*) {}
void  GsDevice_ApplyView(GsDevice*) {}


// The real one (0x1759B0) resolves characters against each other and the
// map; it also remembers who to ask (0x2EE5C4 / 0x2EE5C8), which the
// line-of-sight test uses.
void  Fighters_Clear();                     // pairs.cpp
void  Collision_Release() { Fighters_Clear(); }
void  Collision_Update(void* charas, void* mapState, void*) {
    Collision_Release();
    *guest::ptr<u32>(0x2EE5C4u) = guest::addr(charas);
    *guest::ptr<u32>(0x2EE5C8u) = guest::addr(mapState);
}
// Nobody to fight: closes the HUD's lock-on marker (Hud+0x24, 0x21CF60 →
// 0x222600; that widget is not decompiled).  The enemy plates go by themselves.
void  Hud_HideEnemies(void*) {}
// 0x14EBB0: the wind of the moment, kept at RenderMgr+0x50 (what reads it there has not been found).
void  RenderMgr_SetFlash(void* mgr, const float* wind) { memcpy(static_cast<u8*>(mgr) + 0x50, wind, 16); }


void  SwordDb_Unload(void*) {}
void  CharaResources_Prepare(void*) {}
void* WorldLink_Ctor(void* mem, World*) {
    static u32 vtable = make_dtor_vtable(free_object);
    memset(mem, 0, 8);
    *static_cast<u32*>(mem) = vtable;
    return mem;
}

// ══ Event (script) player ═════════════════════════════════════════════════
// The stand-in used until the real one (src/game/event_player.cpp, over the
// script VM) has enough of the scripts' built-ins to run shops and the story.
EventPlayer* hle_EventPlayer_Ctor(EventPlayer* mem) { memset(mem, 0, sizeof(EventPlayer)); return mem; }
void hle_EventPlayer_StartDay(EventPlayer*, int, int) {
    note_once("event scripts are not decompiled: story scenes (the opening included) are skipped");
}
// A shop's script would run the shop and send the player out again.  Here it
// "runs" until the player has walked onto the interior's exit cells.
static bool g_shopOpen = false, g_shopLeftDoor = false;
static int  g_shopFrames = 0;
void hle_EventPlayer_StartScript(EventPlayer*, const char* script, int) {
    printf("[stub] script %s is not run; walk to the exit to leave\n", script);
    g_shopOpen = true;
    g_shopLeftDoor = false;
    g_shopFrames = 0;
}
int  hle_EventPlayer_Update(EventPlayer*, int) {
    if (!g_shopOpen)
        return 0;
    // The scene has by now given the player a routine that stands still for
    // the script; ours places the player at the door and hands the pad back.
    if (g_shopFrames == 0)
        hle_chara_enter_interior();
    // While a script runs the scene leaves the fades to it: fade in ourselves.
    if (++g_shopFrames == 8)
        RenderMgr_TriggerEffect(guest::ptr(*guest::ptr<u32>(0x2EE684u)), 11, 0, 0);
    ColCellType* cell = cell_under_player();
    const bool onExit = cell && (cell->flags & kCellEventOnly);
    if (!onExit)
        g_shopLeftDoor = true;
    else if (g_shopLeftDoor)
        g_shopOpen = false;
    return g_shopOpen;
}


// (The status menu and its three pages are decompiled: src/game/status_menu.cpp,
// status_page.cpp, items_page.cpp, swords_page.cpp.)

// ══ The rest ══════════════════════════════════════════════════════════════
// The save screen (0x25E550–0x260050: choosing a memory card and a file) is
// not decompiled.  The stand-in writes the native save file at once, says so
// on the HUD and goes back to the area map.
// (The save screen is src/game/mcard.cpp.)


void* Hud2_Ctor(void* mem) { guest::free(mem); return nullptr; }
void  Hud2_Set(void* hud) { *guest::ptr<u32>(0x2EE690u) = guest::addr(hud); }
void  CharaResources_Release() {}
void  Voice_Free() {}
// 0x180C50 / 0x180C60: every sound-effect voice is held where it is while
// the pause menu or the area map is up (0x15A9B0 / 0x15AC00 send each of the
// 38 voices a command).
void  Se_Resume() { hal::spu2::samples_hold(false); }
void  Se_Pause()  { hal::spu2::samples_hold(true); }

}  // extern "C"

// Debug (WOTS2_GIVE_SWORDS): a new sword of a kind, and into the player's hands.
bool hle_make_sword(int type, void* out) {
    return SwordDb_MakeSword(SwordDb_Get(), type, static_cast<Sword*>(out)) != 0;
}
void hle_carry_sword(const void* sword) {
    PlayerData_CarrySword(guest::ptr<PlayerData>(*guest::ptr<u32>(0x2EE688u)), static_cast<const Sword*>(sword));
}

// For the level editor (native/editor.cpp).
bool Editor_CurrentMap(EditorMap* out) {
    if (g_currentMap == nullptr || g_view.frame < 0)
        return false;
    NativeMap* m = g_currentMap;
    out->index = m->index;
    out->modelPath = m->modelPath;
    out->collisionPath = m->collisionPath;
    const MapCollision* holder = guest::ptr<MapCollision>(m->collision);
    out->collision = holder->collision ? guest::ptr<void>(holder->collision) : nullptr;
    out->collisionFile = m->collisionFile ? guest::ptr<u8>(m->collisionFile) : nullptr;
    out->collisionSize = m->collisionSize;
    memcpy(out->viewProj, g_view.viewProj, sizeof out->viewProj);
    memcpy(out->eye, g_view.eye, sizeof out->eye);
    memcpy(out->dir, g_view.dir, sizeof out->dir);
    return true;
}
