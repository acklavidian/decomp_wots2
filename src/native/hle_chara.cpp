// What still stands in under the decompiled character class
// (src/game/chara.cpp, chara_body.cpp, chara_control.cpp):
//
//   - models: the body and head files are read and drawn by the native model
//     code (shared between look-alikes); the character's skeleton is the real
//     one, built from the body file's bone table and posed by the real motion
//     player (hair and cloth swing with it: Skeleton_Swing);
//   - the voice, head turning, effects: empty; swords are not drawn;
//   - what the two classes add to the base class's blow methods (the player's
//     statistics and rumble, the report of a death).

#include "engine/pad.h"
#include "game/chara.h"
#include "game/chara_mgr.h"
#include "game/map_collision.h"
#include "game/sword.h"
#include "hal/gs.h"
#include "native/archive.h"
#include "native/guest.h"

bool hle_make_sword(int type, void* out);       // hle_gameplay.cpp (debug)
void hle_carry_sword(const void* sword);
#include "native/hle.h"
#include "native/model.h"
#include "native/editor_hooks.h"
#include "native/sprites.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" void Accessories_Refresh();      // src/game/accessories.cpp
extern "C" {
    int   Lights_At(const float* pos, float dirs[3][4], float colours[3][4]);    // 0x165D30 (point_lights.cpp)
    void* RenderMgr_FindWorldEffect(void* mgr, int type, int n);       // 0x14EB50
    void* RenderMgr_AddWorldEffect(void* mgr, int type, u32 a, u32 b); // (src/game/world_effects.cpp)
    void* PadMgr_Get();                                 // 0x13A920
    u16   Pad_GetPressed(void* mgr, int port);          // 0x13A0C0
    void  Matrix_FaceTowards(void* matrix, const void* target);     // 0x1C8DB0
    void  Chara_StepMap(Chara* self, MapCollision* map);            // 0x1CE580
    void  hle_world_follow(float* matrix);
}

static bool g_whereAsked = false;
extern "C" void hle_ask_where() { g_whereAsked = true; }

namespace {

// The models of one entry of Common/chara/list, shared by its users.
struct ModelSet {
    model::Model body, head;
    sprites::Sheet shared;
    bool loaded = false, hasHead = false, hasShared = false;
    int users = 0;
    int type = 0;                   // the body file's: 1 = one skinned model
    std::vector<u8> bones;          // the body file's bone table (0xA0 bytes each)
};
std::map<int, ModelSet*> g_models;

struct NativeChara {
    ModelSet* models = nullptr;
    int modelId = -1;
};
std::map<u32, NativeChara> g_native;        // by the guest address of the Chara

ModelSet* acquire_models(int id) {
    auto it = g_models.find(id);
    if (it != g_models.end()) {
        it->second->users++;
        return it->second;
    }
    auto* set = new ModelSet;
    set->users = 1;
    g_models[id] = set;
    // File names come from the game's own character list (decompiled).
    CharaResources* res = CharaResources_Get();
    CharaResources_Load(res, 0);
    if (const CharaInfo* info = CharaResources_Find(res, id)) {
        const char* body = CharaResources_GetBodyPath(res, id, 1);
        const char* head = CharaResources_GetHeadPath(res, id, 1);
        const char* pim = CharaResources_GetTexturePath(res, id);
        std::vector<u8> file;
        if (body && archive::read(body, file)) {
            set->loaded = set->body.load(file);
            if (file.size() > 0x20) {
                const u8* data = file.data() + 0x10;
                set->type = file[4];
                s16 count;
                u32 at;
                memcpy(&count, data + 2, 2);
                memcpy(&at, data + (set->type == 1 ? 8 : 0xC), 4);
                if (count > 0 && 0x10 + (size_t)at + (size_t)count * 0xA0 <= file.size())
                    set->bones.assign(data + at, data + at + (size_t)count * 0xA0);
            }
        }
        if (head && archive::read(head, file))
            set->hasHead = set->head.load(file);
        if (pim && archive::read(pim, file))
            set->hasShared = set->shared.load(file);
        printf("[chara] %d %s\n", info->id, guest::ptr<const char>(info->name));
    }
    if (!set->loaded || set->bones.empty())
        fprintf(stderr, "[chara] character %d could not be loaded\n", id);
    return set;
}

void release_models(NativeChara& n) {
    if (!n.models)
        return;
    if (--n.models->users == 0) {
        n.models->body.release();
        n.models->head.release();
        n.models->shared.release();
        g_models.erase(n.modelId);
        delete n.models;
    }
    n.models = nullptr;
}

void mat_mul(const float* a, const float* b, float* out) {      // out = a × b, row vectors
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof(r));
}

inline CharaParams* params_of(Chara* c) { return guest::ptr<CharaParams>(c->params); }
inline CharaBody*   body_of(Chara* c)   { return guest::ptr<CharaBody>(c->body); }

Chara* find_player() {
    for (auto& entry : g_native) {
        Chara* c = guest::ptr<Chara>(entry.first);
        if (c->id == 0)
            return c;
    }
    return nullptr;
}

void other_step_map(Chara* c, MapCollision* map) {
    // WOTS2_TRACE_CHARA="id[,every]" (debug): where that character is, where
    // its motion wants it, and what the map and the collision world say.
    static const char* trace = getenv("WOTS2_TRACE_CHARA");
    static const int traceId = trace ? atoi(trace) : -1;
    static const int traceEvery = trace && strchr(trace, ',') ? atoi(strchr(trace, ',') + 1) : 20;
    const bool show = trace != nullptr && c->id == traceId && hal::gs::frame_count() % traceEvery == 0;
    float before[3] = {0, 0, 0}, wanted[3] = {0, 0, 0};
    if (show) {
        memcpy(before, body_of(c)->lastPosition, sizeof before);
        memcpy(wanted, body_of(c)->position, sizeof wanted);
    }
    Chara_StepMap(c, map);
    if (show) {
        const float* m = params_of(c)->matrix;
        const CharaBody* b = body_of(c);
        printf("[chara %d] frame %d: was %.2f %.2f %.2f wanted %.2f %.2f %.2f now %.2f %.2f %.2f facing %.2f %.2f, action %#x flags %#x cell %#x/%d found %#x\n",
               c->id, hal::gs::frame_count(), before[0], before[1], before[2], wanted[0], wanted[1], wanted[2],
               m[12], m[13], m[14], m[8], m[10], b->action, b->flags, c->cellFlags,
               c->cellIndex, c->flagHandle != 0 ? guest::ptr<u32>(c->flagHandle)[0x174 / 4] : 0u);
    }
}

// ── Debug hooks around the player's map step ──────────────────────────────
void player_step_map(Chara* c, MapCollision* map) {
    const u32 mapAddr = guest::addr(map);
    // WOTS2_FIND_DOORS=1 lists where the map's door cells are.
    static u32 scanned = 0;
    if (scanned != mapAddr && getenv("WOTS2_FIND_DOORS")) {
        scanned = mapAddr;
        s32 last = -1;
        for (float z = -100.0f; z <= 100.0f; z += 0.5f)
            for (float x = -100.0f; x <= 100.0f; x += 0.5f) {
                const float p[4] = {x, 0.0f, z, 1.0f};
                s32 door = 0;
                if ((hle_cell_at(mapAddr, p, &door) & 8) && door != last) {
                    printf("[doors] door %d at %.1f, %.1f\n", door, x, z);
                    last = door;
                }
            }
    }
    if (hle_world_active())
        hle_world_follow(params_of(c)->matrix);         // WOTS2_WARP, and the stand-in camera
    Chara_StepMap(c, map);
    // WOTS2_TRACE_PLAYER=n prints where the player is every n frames.
    static const int trace = getenv("WOTS2_TRACE_PLAYER") ? atoi(getenv("WOTS2_TRACE_PLAYER")) : 0;
    if (trace > 0 && hal::gs::frame_count() % trace == 0) {
        const float* m = params_of(c)->matrix;
        const CharaBody* b = body_of(c);
        printf("[player] frame %d: at %.2f %.2f %.2f facing %.2f %.2f, action %#x flags %#x act %#x state %#x cell %#x/%d\n",
               hal::gs::frame_count(), m[12], m[13], m[14], m[8], m[10], b->action, b->flags, b->actionFlags,
               b->state, c->cellFlags, c->cellIndex);
    }
    // F4: where the player is, and the camera (to report a place that looks wrong).
    if (g_whereAsked) {
        g_whereAsked = false;
        const float* m = params_of(c)->matrix;
        EditorMap map{};
        if (Editor_CurrentMap(&map))
            printf("[where] map %d %s: player at %.2f %.2f %.2f facing %.2f %.2f; camera at %.2f %.2f %.2f looking %.2f %.2f %.2f\n",
                   map.index, map.modelPath.c_str(), m[12], m[13], m[14], m[8], m[10], map.eye[0], map.eye[1], map.eye[2],
                   map.dir[0], map.dir[1], map.dir[2]);
        else
            printf("[where] player at %.2f %.2f %.2f\n", m[12], m[13], m[14]);
        fflush(stdout);
    }
    // WOTS2_KILL_PLAYER=frame (debug): the player drops dead then, to see
    // what dying does.
    static const int killAt = getenv("WOTS2_KILL_PLAYER") ? atoi(getenv("WOTS2_KILL_PLAYER")) : -1;
    if (killAt >= 0 && hal::gs::frame_count() == killAt) {
        params_of(c)->hp = 0;
        body_of(c)->flags |= kBodyDead;
    }
    // WOTS2_WEAK_NPCS=frame (debug): from then everybody else is down to
    // their last point of health — one blow kills (to see what the dead drop).
    static const int weakAt = getenv("WOTS2_WEAK_NPCS") ? atoi(getenv("WOTS2_WEAK_NPCS")) : -1;
    if (weakAt >= 0 && hal::gs::frame_count() >= weakAt)
        for (auto& entry : g_native) {
            Chara* other = guest::ptr<Chara>(entry.first);
            if (other != c && other->params != 0 && params_of(other)->hp > 1)
                params_of(other)->hp = 1;
        }
    // WOTS2_WEAR="frame:item" (debug): from then the player owns and wears
    // that accessory (items 0x21–0x3C, i.e. 33–60).
    static int wearAt = -1, wearItem = 0;
    static bool wearRead = false;
    if (!wearRead) {
        wearRead = true;
        if (const char* spec = getenv("WOTS2_WEAR"))
            sscanf(spec, "%d:%i", &wearAt, &wearItem);
    }
    if (wearAt >= 0 && hal::gs::frame_count() == wearAt) {
        u8* pd = guest::ptr<u8>(*guest::ptr<u32>(0x2EE688u));
        pd[0x6CEA] = (u8)wearItem;
        const u16 worn = (u16)wearItem;
        memcpy(pd + 0x11A, &worn, 2);
        Accessories_Refresh();
    }
    // WOTS2_HURT_PLAYER="frame:health" (debug): the player's health is that
    // from then, to try food and medicine on.
    static int hurtAt = -1, hurtTo = 0;
    static bool hurtRead = false;
    if (!hurtRead) {
        hurtRead = true;
        if (const char* spec = getenv("WOTS2_HURT_PLAYER"))
            sscanf(spec, "%d:%d", &hurtAt, &hurtTo);
    }
    if (hurtAt >= 0 && hal::gs::frame_count() == hurtAt)
        params_of(c)->hp = (s16)hurtTo;
    // WOTS2_TOUGH=1 (debug): the player's health is put back to full every
    // frame it is still alive — for scripted runs that must outlast a fight.
    static const bool tough = getenv("WOTS2_TOUGH") != nullptr;
    if (tough && params_of(c)->hp > 0)
        params_of(c)->hp = params_of(c)->maxHp;
    // WOTS2_GIVE_SWORDS="frame:n" (debug): n more swords (kinds 5, 10, …)
    // are carried from then, to try the sword menu on.
    static int swordsAt = -1, swordsMore = 0;
    static bool swordsRead = false;
    if (!swordsRead) {
        swordsRead = true;
        if (const char* spec = getenv("WOTS2_GIVE_SWORDS"))
            sscanf(spec, "%d:%d", &swordsAt, &swordsMore);
    }
    if (swordsAt >= 0 && hal::gs::frame_count() == swordsAt) {
        void* sword = guest::alloc(0x100);
        for (int i = 1; i <= swordsMore; i++) {
            memset(sword, 0, 0x100);
            if (hle_make_sword(i * 5, sword))
                hle_carry_sword(sword);
        }
        guest::free(sword);
    }
    // WOTS2_CARRY="frame:item,item…" (debug): those items are in the bag
    // from then (13–15 are the scrolls that teach a technique).
    static int carryAt = -1;
    static std::vector<int> carry;
    static bool carryRead = false;
    if (!carryRead) {
        carryRead = true;
        if (const char* spec = getenv("WOTS2_CARRY")) {
            carryAt = atoi(spec);
            for (const char* p = strchr(spec, ':'); p != nullptr; p = strchr(p + 1, ','))
                carry.push_back(atoi(p + 1));
        }
    }
    if (carryAt >= 0 && hal::gs::frame_count() == carryAt) {
        u8* bag = guest::ptr<u8>(*guest::ptr<u32>(0x2EE688u)) + 0x150;
        for (size_t i = 0; i < carry.size() && i < 8; i++)
            bag[i] = (u8)carry[i];
    }
    // Debug shortcut (WOTS2_DEBUG_KEYS=1): R2 leaves the area from anywhere.
    static const bool debugKeys = getenv("WOTS2_DEBUG_KEYS") != nullptr;
    if (debugKeys && hle_world_active() && (Pad_GetPressed(PadMgr_Get(), 0) & kPadR2)) {
        c->cellFlags = 4;
        c->cellIndex = 0;
    }
    // WOTS2_TOUR (debug; see the area map): after eight seconds free in an
    // area the player leaves it — at home (Otsuka), where WOTS2_WARP walks
    // it into its house, only if that has not happened in a minute.
    static const bool tour = getenv("WOTS2_TOUR") != nullptr;
    if (tour && hle_world_active() && c->id == 0) {
        const int area = guest::ptr<s8>(*guest::ptr<u32>(0x2EE688u))[0x103];
        if (hle_world_settled() >= (area == 6 ? 1800 : 240)) {
            c->cellFlags = 4;
            c->cellIndex = 0;
        }
    }
}

void* zeroed(u32 size) {
    void* p = guest::alloc(size);
    memset(p, 0, size);
    return p;
}

int chara_zero(Chara*) { return 0; }

}  // namespace

bool hle_chara_alive(u32 chara) { return g_native.count(chara) != 0; }

// In the game an interior's script places the player at its entrance and
// gives the controls back when it is done; no script runs yet.
void hle_chara_enter_interior() {
    float stand[4], face[4];
    if (!hle_interior_entrance(stand, face))
        return;
    Chara* c = find_player();
    if (!c)
        return;
    float* matrix = params_of(c)->matrix;
    memcpy(matrix + 12, stand, 12);
    memcpy(body_of(c)->position, stand, 12);
    memcpy(body_of(c)->lastPosition, stand, 12);
    Matrix_FaceTowards(matrix, face);
    Chara_SetAction(c, CharaAction_Make(0, 0, nullptr));
    guest::ptr<CharaAction>(c->action)->released = 1;
}

// Vtable slots of the two character classes that are not decompiled.
void hle_chara_bind() {
    printf("[stub] characters: models are read and drawn by native code (lit by the map's lights)\n");
    guest::bind(0x269B70, player_step_map, "Chara_StepMap + debug hooks");
    guest::bind(0x2692A0, other_step_map, "Chara_StepMap + map edge");
}

extern "C" {
    void* CharaVoice_Ctor(void* mem, u32 chara);        // 0x26B600  (chara_face.cpp)
    void  CharaVoice_Dtor(void* face);                  // 0x26B420
    void* CharaState_Ctor(void* mem);                   // 0x201E90  (chara_look.cpp)
}

extern "C" {

// ── Parts ─────────────────────────────────────────────────────────────────
void CharaParts_Create(Chara* c) {
    c->effects = guest::addr(zeroed(4));
    c->look = guest::addr(CharaState_Ctor(guest::alloc(0x40)));      // head turning, feet (chara_look.cpp)
    c->voice = guest::addr(CharaVoice_Ctor(guest::alloc(0x1D0), guest::addr(c)));     // (the face, really)
    c->sounds = guest::addr(zeroed(4));
    c->sheath = guest::addr(zeroed(4));
    c->swords = guest::addr(SwordHolder_Ctor(guest::alloc(sizeof(CharaSwords))));
    c->_1d8 = guest::addr(zeroed(4));
    NativeChara& n = g_native[guest::addr(c)];
    n = NativeChara();
    n.modelId = c->model;
}
void CharaParts_Destroy(Chara* c) {
    if (c->swords != 0)
        SwordHolder_Dtor(guest::ptr(c->swords), 1);
    c->swords = 0;
    if (c->voice != 0)
        CharaVoice_Dtor(guest::ptr(c->voice));
    for (u32* part : {&c->effects, &c->look, &c->sounds, &c->sheath, &c->_1d8, &c->voice}) {
        if (*part != 0)
            guest::free(guest::ptr(*part));
        *part = 0;
    }
}

// ── Models ────────────────────────────────────────────────────────────────
// Phase 0 reads everything at once; phase 1 (the body) makes the skeleton.
int CharaModels_Start(Chara* c, int phase) {
    if (phase > 2)
        return 0;                               // (other levels of detail: none)
    NativeChara& n = g_native[guest::addr(c)];
    if (!n.models && n.modelId >= 0)
        n.models = acquire_models(n.modelId);
    if (!n.models || n.models->bones.empty())
        return 0;
    c->bodyType = n.models->type;
    CharaParams* p = params_of(c);
    if (phase == 1 && p->link[1] == 0) {
        const std::vector<u8>& raw = n.models->bones;
        const int count = (int)(raw.size() / 0xA0);
        Bone* source = static_cast<Bone*>(guest::alloc((u32)raw.size()));
        memcpy(source, raw.data(), raw.size());
        for (int i = 0; i < count; i++) {       // parent: an index in the file, a pointer once relocated
            const s32 parent = (s32)source[i].parent;
            source[i].parent = parent < 0 ? 0 : guest::addr(&source[parent]);
        }
        Skeleton* skeleton = Skeleton_Ctor(static_cast<Skeleton*>(guest::alloc(sizeof(Skeleton))), count, source);
        guest::free(source);
        p->link[1] = guest::addr(skeleton);
        CharaBody_SetSkeleton(body_of(c), skeleton);
    }
    c->loadPhase++;
    return 1;
}
void CharaModels_Release(Chara* c) {
    auto it = g_native.find(guest::addr(c));
    if (it == g_native.end())
        return;
    release_models(it->second);
    g_native.erase(it);
}

// The world → clip matrix characters are drawn with this frame.
static void current_view(float* mvp) {
    if (hle_world_active() && hle_world_view(mvp))
        return;
    // The character setup screen: its camera sits at the origin looking
    // along +z.  (The real matrices are in asset 0x21, not used yet.)
    const float view[16] = {-1, 0, 0, 0,  0, 1, 0, 0,  0, 0, -1, 0,  0, 0, 0, 1};
    const float zn = 0.2f, zf = 40.0f;
    const float proj[16] = {2 * 512.0f / 640.0f, 0, 0, 0,  0, 2 * 512.0f / 448.0f, 0, 0,
                            0, 0, (zf + zn) / (zn - zf), -1,  0, 0, 2 * zf * zn / (zn - zf), 0};
    mat_mul(view, proj, mvp);
}

// Who is being drawn (for the pictures in between: see begin_3d's `who`).
// The sword parts drawn after a character's body are taken as its own.
static u32 g_drawing = 0;
// The ground's painted light under each character (hle_gameplay.cpp), eased.
extern "C" float hle_ground_light_at(const float* p);
static std::map<u32, float> g_ground;
static float ground_eased(u32 who, const float* feet) {
    const float want = hle_ground_light_at(feet);
    auto it = g_ground.find(who);
    if (it == g_ground.end() || !hal::gs::ground_light())
        return g_ground[who] = want;
    return it->second += (want - it->second) * 0.25f;
}
static float ground_of(u32 who) {
    auto it = g_ground.find(who);
    return it != g_ground.end() ? it->second : 1.0f;
}
static u32 who_of(u32 a, u32 b) { return (a * 0x9E3779B1u) ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2)) | 1u; }

void CharaModels_Draw(Chara* c, int) {
    if (getenv("WOTS2_DUMP_ACTORS") && hal::gs::frame_count() % 200 == 0) {     // debug hook
        const float* m = params_of(c)->matrix;
        const CharaBody* b = body_of(c);
        const u32 foe = params_of(c)->link[3];
        printf("[chara] frame %d: id %d active %d visible %d at %.1f %.1f %.1f action %#x flags %#x hp %d"
               " target %#x foe %d routine %d\n",
               hal::gs::frame_count(), c->id, c->active, c->visible, m[12], m[13], m[14], b->action, b->flags,
               params_of(c)->hp, b->targetFlags, foe ? guest::ptr<CharaParams>(foe)->id : -1,
               (int)CharaControl_Kind(guest::ptr<CharaControl>(c->action)));
    }
    auto it = g_native.find(guest::addr(c));
    if (it == g_native.end() || !it->second.models || !it->second.models->loaded)
        return;
    ModelSet* set = it->second.models;
    const float* matrix = params_of(c)->matrix;
    if (matrix[13] < -40.0f)
        return;                 // parked below the map until its task places it
    if (params_of(c)->link[1] == 0)
        return;
    const Skeleton* skeleton = guest::ptr<Skeleton>(params_of(c)->link[1]);
    const Bone* bones = guest::ptr<Bone>(skeleton->bones);

    // World matrices for the native renderer.  Bones the game moves with its
    // cloth code (top flag bits) hang rigidly off their parents here.
    static std::vector<float> pose, headPose;
    pose.resize((size_t)skeleton->count * 16);
    for (int i = 0; i < skeleton->count; i++) {
        float* out = &pose[(size_t)i * 16];
        if ((bones[i].flags & 0xE0000000u) && bones[i].parent != 0) {
            const int parent = (int)((bones[i].parent - skeleton->bones) / sizeof(Bone));
            mat_mul(bones[i].local, &pose[(size_t)parent * 16], out);
        } else {
            memcpy(out, bones[i].world, 64);
        }
    }
    const float* bodyPose = pose.data();
    if (set->body.bones.size() > (size_t)skeleton->count)
        bodyPose = nullptr;
    // The head shares the 28 bones of the common skeleton; its own extra
    // bones (hair) keep their rest pose on top of those.
    if (set->hasHead) {
        const size_t count = set->head.bones.size();
        headPose.resize(count * 16);
        for (size_t i = 0; i < count; i++) {
            float* out = &headPose[i * 16];
            if (i < 28 && i < (size_t)skeleton->count) {
                memcpy(out, &pose[i * 16], 64);
            } else {
                const int parent = set->head.bones[i].parent;
                if (parent >= 0 && (size_t)parent < i)
                    mat_mul(set->head.bones[i].local, &headPose[(size_t)parent * 16], out);
                else
                    mat_mul(set->head.bones[i].local, matrix, out);
            }
        }
    }

    float mvp[16];
    current_view(mvp);
    g_drawing = guest::addr(c);
    hal::gs::begin_3d(mvp, hal::gs::View::Model, who_of(g_drawing, 0));
    // Lit by the map's lights, dimmed by the light of the cell it stands on
    // (Chara's light, 127 = full; it eases from cell to cell).
    model::Lighting light = model::scene_light();
    // (Modern lighting: …and by the light painted into the ground under it,
    // eased so that stepping into a shaded doorway darkens it gradually.)
    const float ground = ground_eased(guest::addr(c), &matrix[12]);
    static const int traceGround = getenv("WOTS2_TRACE_GROUND") ? atoi(getenv("WOTS2_TRACE_GROUND")) : 0;
    if (traceGround > 0 && c->id == 0 && hal::gs::frame_count() % traceGround == 0)     // (debug)
        printf("[ground] frame %d: at %.2f %.2f %.2f ground light %.2f\n", hal::gs::frame_count(), matrix[12],
               matrix[13], matrix[14], ground);
    for (int k = 0; k < 3; k++)
        light.tint[k] = c->light[k] / 127.0f * ground;
    {   // …and by the point lights near it (point_lights.cpp).
        float dirs[3][4] = {}, colours[3][4] = {};
        const int changed = Lights_At(&matrix[12], dirs, colours);
        for (int slot = 1; slot < 3; slot++)
            if (changed & (1 << slot))
                for (int k = 0; k < 3; k++) {
                    light.dir[slot][k] = dirs[slot][k];
                    light.colour[slot][k] = colours[slot][k];
                }
    }
    if (bodyPose)
        set->body.draw(bodyPose, set->hasShared ? &set->shared : nullptr, &light);
    if (set->hasHead)
        set->head.draw(headPose.data(), set->hasShared ? &set->shared : nullptr, &light);
    hal::gs::end_3d();
}

// The player's shadow (the hardware side of 0x269940).  The original draws
// the player's body once more, black, into a small texture as seen from the
// light (a view 1.2 units to each side of the hips), and has the map's
// ground near the player drawn again with that texture laid over it
// (0x135A40 → 0x149580), `strength` × 64 / 128 dark.  Here the same
// silhouette is laid on the level ground under the player.
extern bool hle_world_has_ground();     // hle_gameplay.cpp
void hle_chara_shadow(Chara* c, float strength) {
    auto it = g_native.find(guest::addr(c));
    if (it == g_native.end() || !it->second.models || !it->second.models->loaded || getenv("WOTS2_NO_SHADOW") ||
        hal::gs::real_shadows() || !hle_world_has_ground())
        return;
    ModelSet* set = it->second.models;
    const CharaParams* p = params_of(c);
    if (p->link[1] == 0)
        return;
    const Skeleton* skeleton = guest::ptr<Skeleton>(p->link[1]);
    const Bone* bones = guest::ptr<Bone>(skeleton->bones);
    std::vector<float> pose((size_t)skeleton->count * 16), headPose;
    for (int i = 0; i < skeleton->count; i++)
        memcpy(&pose[(size_t)i * 16], bones[i].world, 64);
    if (set->body.bones.size() > (size_t)skeleton->count)
        return;
    if (set->hasHead) {
        const size_t count = set->head.bones.size();
        headPose.resize(count * 16);
        for (size_t i = 0; i < count; i++) {
            float* out = &headPose[i * 16];
            if (i < 28 && i < (size_t)skeleton->count) {
                memcpy(out, &pose[i * 16], 64);
            } else {
                const int parent = set->head.bones[i].parent;
                if (parent >= 0 && (size_t)parent < i)
                    mat_mul(set->head.bones[i].local, &headPose[(size_t)parent * 16], out);
                else
                    mat_mul(set->head.bones[i].local, p->matrix, out);
            }
        }
    }

    // Towards the light; never so low that the shadow runs off to the horizon.
    const model::Lighting& scene = model::scene_light();
    float L[3] = {scene.dir[0][0], scene.dir[0][1], scene.dir[0][2]};
    float len = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    if (len < 0.001f) { L[0] = 0.3f; L[1] = 1.0f; L[2] = 0.2f; len = sqrtf(1.13f); }
    for (float& v : L) v /= len;
    if (L[1] < 0.5f) {
        L[1] = 0.5f;
        len = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        for (float& v : L) v /= len;
    }
    const float C[3] = {p->matrix[12], bones[0].world[13], p->matrix[14]};
    const float F[3] = {-L[0], -L[1], -L[2]};               // the way the light looks
    float R[3] = {F[2], 0.0f, -F[0]};                       // (0, 1, 0) × F
    len = sqrtf(R[0] * R[0] + R[2] * R[2]);
    if (len < 0.001f) { R[0] = 1.0f; R[2] = 0.0f; len = 1.0f; }
    for (float& v : R) v /= len;
    const float U[3] = {F[1] * R[2] - F[2] * R[1], F[2] * R[0] - F[0] * R[2], F[0] * R[1] - F[1] * R[0]};
    const float e = 1.2f, deep = 10.0f;
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float fromLight[16] = {R[0] / e, U[0] / e, F[0] / deep, 0,  R[1] / e, U[1] / e, F[1] / deep, 0,
                                 R[2] / e, U[2] / e, F[2] / deep, 0,
                                 -dot(C, R) / e, -dot(C, U) / e, -dot(C, F) / deep, 1};
    model::Lighting black;
    for (int k = 0; k < 3; k++)
        black.ambient[k] = black.tint[k] = 0.0f;
    hal::gs::begin_target(256);
    hal::gs::begin_3d(fromLight);
    set->body.draw(pose.data(), set->hasShared ? &set->shared : nullptr, &black);
    if (set->hasHead)
        set->head.draw(headPose.data(), set->hasShared ? &set->shared : nullptr, &black);
    hal::gs::end_3d();
    const u32 silhouette = hal::gs::end_target();

    // The view's four corners, carried along the light onto the ground.
    const float ground = p->matrix[13] - c->aboveGround + 0.03f;
    const u32 rgba = (u32)(strength * 64.0f > 127.0f ? 127.0f : strength * 64.0f) << 24;
    hal::gs::Vtx3 corner[4];
    static const float kCorner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        float at[3];
        for (int k = 0; k < 3; k++)
            at[k] = C[k] + R[k] * kCorner[i][0] * e + U[k] * kCorner[i][1] * e;
        const float s = (ground - at[1]) / L[1];
        // (The target's rows are top first, as the frame's are: its v runs
        // down U.  Taken the other way the shadow was the player's mirror
        // image, and turned the wrong way when the player turned.)
        corner[i] = {at[0] + L[0] * s, ground, at[2] + L[2] * s, (kCorner[i][0] + 1) / 2, (1 - kCorner[i][1]) / 2, rgba};
    }
    const hal::gs::Vtx3 quad[6] = {corner[0], corner[1], corner[2], corner[2], corner[1], corner[3]};
    float mvp[16];
    current_view(mvp);
    hal::gs::set_blend(hal::gs::Blend::Normal);
    hal::gs::begin_3d(mvp);
    hal::gs::set_overlay(true);
    hal::gs::set_depth_bias(-2);
    hal::gs::draw_mesh(silhouette, quad, 6, hal::gs::Prim::Triangles);
    hal::gs::set_depth_bias(0);
    hal::gs::set_overlay(false);
    hal::gs::end_3d();
}


// ── Sword models ──────────────────────────────────────────────────────────
// A sword style (a 0x3C-byte record of weapon/totaldata) names a texture
// file, a pack of small one-bone models (Common/weapon/wmp/*.wmp: u32 1,
// u32 count, {s32 id, s32 size, A2MF model}[]) and which of the pack's models
// are its parts: six that hang on the body (s16 ids at +0x0C, the bones they
// hang from at +0x18) and four held in the hand (ids at +0x24, bones +0x34).
// In the game the sword database reads these in the background (0x15D4F0,
// 0x15D2C0, 0x15E310, 0x15E1F0) and SwordModel_Draw* hand a part to the model
// class (0x15A610 / 0x15A6F0 → 0x12DD40, the VU1 boundary).  Here they are
// read on first use and drawn by the native model code.
namespace {
struct SwordPack {
    std::map<int, model::Model> parts;
};
std::map<int, SwordPack> g_swordPacks;
std::map<int, sprites::Sheet> g_swordTextures;

model::Model* sword_part(SwordDb* db, int pack, int id) {
    auto it = g_swordPacks.find(pack);
    if (it == g_swordPacks.end()) {
        SwordPack& made = g_swordPacks[pack];
        std::vector<u8> file;
        const char* path = pack >= 0 && pack < db->motionNameCount
                               ? guest::ptr<const char>(guest::ptr<u32>(db->motionNames)[pack]) : nullptr;
        if (path && archive::read(path, file) && file.size() >= 8) {
            u32 count;
            memcpy(&count, &file[4], 4);
            size_t at = 8;
            for (u32 i = 0; i < count && at + 8 <= file.size(); i++) {
                s32 partId, size;
                memcpy(&partId, &file[at], 4);
                memcpy(&size, &file[at + 4], 4);
                if (size < 0 || at + 8 + (size_t)size > file.size())
                    break;
                std::vector<u8> part(file.begin() + at + 8, file.begin() + at + 8 + size);
                model::Model m;
                if (m.load(part))
                    made.parts[partId] = std::move(m);
                at += 8 + (size_t)size;
            }
        } else {
            fprintf(stderr, "[sword] model pack %d could not be read\n", pack);
        }
        it = g_swordPacks.find(pack);
    }
    auto part = it->second.parts.find(id);
    return part == it->second.parts.end() ? nullptr : &part->second;
}

sprites::Sheet* sword_texture(SwordDb* db, int index) {
    auto it = g_swordTextures.find(index);
    if (it == g_swordTextures.end()) {
        sprites::Sheet& sheet = g_swordTextures[index];
        std::vector<u8> file;
        const char* path = index >= 0 && index < db->textureNameCount
                               ? guest::ptr<const char>(guest::ptr<u32>(db->textureNames)[index]) : nullptr;
        if (!(path && archive::read(path, file) && sheet.load(file)))
            fprintf(stderr, "[sword] texture %d could not be read\n", index);
        it = g_swordTextures.find(index);
    }
    return &it->second;
}

int draw_sword_part(const s16* swordModel, int idsAt, int slot, const float* matrix, bool own) {
    SwordDb* db = SwordDb_Get();
    if (!db || !swordModel)
        return 0;
    const u8* record = guest::ptr<u8>(db->styles) + (size_t)swordModel[0] * 0x3C;
    s32 texture, pack;
    s16 id;
    memcpy(&texture, record, 4);
    memcpy(&pack, record + 4, 4);
    memcpy(&id, record + idsAt + slot * 2, 2);
    if (id == -1)
        return 0;
    model::Model* part = sword_part(db, pack, id);
    if (!part || part->bones.empty())
        return 0;
    float pose[16], mvp[16];
    if (own)
        mat_mul(part->bones[0].rest, matrix, pose);
    else
        memcpy(pose, matrix, sizeof pose);
    current_view(mvp);
    hal::gs::begin_3d(mvp, hal::gs::View::Model, who_of(g_drawing, (u32)idsAt << 8 | (u32)slot));
    model::Lighting light = model::scene_light();
    const float ground = ground_of(g_drawing);          // (its character's)
    for (float& t : light.tint) t *= ground;
    part->draw(pose, sword_texture(db, texture), &light);
    hal::gs::end_3d();
    return 1;
}
}  // namespace

extern "C" {
// A world object's model (the game: a class around an A2MF model with its
// own textures, 0x2B8830; its draw is the VU1 boundary) at a world matrix.
// How the next object is lit (the game's three set-ups before a draw,
// 0x1667E0 / 0x166730 / 0x166870): by the map's lights with its ambient
// level scaled, or — `glow` — by a level of its own alone (lanterns).
static float g_objectLevel[3] = {1, 1, 1};
static bool g_objectGlow = false, g_objectLit = false;
void hle_object_light(const float* level, int glow) {
    memcpy(g_objectLevel, level, sizeof g_objectLevel);
    g_objectGlow = glow != 0;
    g_objectLit = true;
}
static void object_draw(const char* path, const float* matrix, const char* textures, bool inHand = true) {
    static std::map<std::string, model::Model> models;
    auto it = models.find(path);
    if (it == models.end()) {
        model::Model m;
        std::vector<u8> file;
        if (!archive::read(path, file) || !m.load(file))
            printf("[objects] cannot read %s\n", path);
        it = models.emplace(path, std::move(m)).first;
    }
    model::Model& m = it->second;
    if (m.bones.empty())
        return;
    // Every bone of the object sits where the object does.
    std::vector<float> pose(m.bones.size() * 16);
    for (size_t i = 0; i < m.bones.size(); i++)
        mat_mul(m.bones[i].rest, matrix, &pose[i * 16]);
    // Things that can be carried are modelled about their own middle, and
    // their one bone's rest pose is where that middle sits from the hand
    // (0.2 or so along it; the crate's: 0.23, −0.19, 0.39).  Lying or
    // standing in the world they are where their matrix says — which is
    // where their collision box is, and half their height over the ground
    // in the maps' placement files.
    if (!inHand && m.bones.size() == 1)
        memcpy(pose.data(), matrix, 16 * sizeof(float));
    float mvp[16];
    current_view(mvp);
    // (An object is known by where its matrix lives: the object's own, or
    // the accessory's / splinter's — the same from frame to frame.)
    u32 name = 0;
    for (const char* p = path; *p; p++)
        name = name * 31u + (u8)*p;
    hal::gs::begin_3d(mvp, hal::gs::View::Model, who_of((u32)(uintptr_t)matrix, name));
    sprites::Sheet* shared = nullptr;
    if (textures != nullptr) {
        static std::map<std::string, sprites::Sheet> sheets;
        auto found = sheets.find(textures);
        if (found == sheets.end()) {
            sprites::Sheet& sheet = sheets[textures];
            std::vector<u8> data;
            if (!(archive::read(textures, data) && sheet.load(data)))
                printf("[objects] cannot read %s\n", textures);
            found = sheets.find(textures);
        }
        shared = &found->second;
    }
    model::Lighting light = model::scene_light();
    if (!g_objectGlow) {
        const float ground = hle_ground_light_at(&matrix[12]);
        for (float& t : light.tint) t *= ground;
    }
    if (g_objectLit) {
        for (int k = 0; k < 3; k++)
            light.ambient[k] = g_objectGlow ? g_objectLevel[k] : light.ambient[k] * g_objectLevel[k];
        if (g_objectGlow)
            memset(light.colour, 0, sizeof light.colour);
        g_objectLit = false;
    }
    m.draw(pose.data(), shared, &light);
    hal::gs::end_3d();
}
void hle_object_draw(const char* path, const float* matrix) { object_draw(path, matrix, nullptr); }
void hle_object_draw_loose(const char* path, const float* matrix) { object_draw(path, matrix, nullptr, false); }
// The same for a model without textures of its own (the splinters of a
// broken crate use the effect sprites' textures).
void hle_object_draw_with(const char* path, const float* matrix, const char* textures) {
    object_draw(path, matrix, textures);
}

int hle_sword_part_matrix(void* swordModel, int slot, const float* hand, float* out) {
    SwordDb* db = SwordDb_Get();
    if (!db || !swordModel || slot < 0 || slot >= 4)
        return 0;
    const s16* head = static_cast<const s16*>(swordModel);
    const u8* record = guest::ptr<u8>(db->styles) + (size_t)head[0] * 0x3C;
    s32 pack;
    s16 id;
    memcpy(&pack, record + 4, 4);
    memcpy(&id, record + 0x24 + slot * 2, 2);
    if (id == -1)
        return 0;
    model::Model* part = sword_part(db, pack, id);
    if (!part || part->bones.empty())
        return 0;
    mat_mul(part->bones[0].rest, hand, out);
    return 1;
}
// 0x15A6F0: one of the six parts that hang on the body.
int SwordModel_DrawMount(void* swordModel, int slot, const float* matrix) {
    if (slot < 0 || slot >= 6)
        return 0;
    return draw_sword_part(static_cast<const s16*>(swordModel), 0x0C, slot, matrix, true);
}
// 0x15A610: one of the four parts held in the hand.
int SwordModel_DrawHand(void* swordModel, int slot, const float* matrix, int own) {
    if (slot < 0 || slot >= 4)
        return 0;
    return draw_sword_part(static_cast<const s16*>(swordModel), 0x24, slot, matrix, own != 0);
}
}

// ── Voice, face, effects, sheath, head turning ────────────────────────────

// Culling is left to the GPU.
int   Frustum_Outside(const void*, const void*, const void*, int) { return 0; }
void  Frustum_LoadPlanes(const void*, const void*, void* planes, int count) { memset(planes, 0, (size_t)count * 16); }
void  Frustum_MakeMatrix(float, float, float, float, float, int, void* out) {
    float* m = static_cast<float*>(out);
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
float Vec_Distance(const void* a, const void* b) {
    const float* p = static_cast<const float*>(a);
    const float* q = static_cast<const float*>(b);
    const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
void  CharaResources_fn1d2b20(CharaResources*, u32) {}
void  CharaResources_fn1d2bb0(CharaResources*) {}
void  CharaResources_fn1d6300(CharaResources*) {}
void  Debug_Print(const char*) {}

}  // extern "C"
