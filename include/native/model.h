#pragma once
#include "hal/gs.h"
#include "hal/ps2_types.h"
#include "native/sprites.h"
#include <string>
#include <vector>

// Native reader and renderer for A2MF character models (see tools/mdl.py and
// the notes in the project notes for the format).  This is NOT decompiled code: it
// stands where the game's model class and its VU1 programs will be until
// those are decompiled, and it backs the debug model viewer.

namespace model {

struct Bone {
    float rest[16];     // model space, row-vector convention
    float local[16];    // relative to the parent
    int   parent;       // -1 = root
};

// One vertex with up to two bone influences:
//   position = p1 × R(b1) + T(b1) × w1  +  p2 × R(b2) + T(b2) × w2
// (rigid vertices have w1 = 1 and no second influence).
struct Vertex {
    float p1[3], w1;
    float p2[3], w2;
    s16   b1, b2;
    float u, v;
    u32   rgba;
    float n[3] = {0, 0, 0};     // normal in its (first) bone's space; zero = none (maps)
};

// The light models are drawn in (a map's: up to three directions and their
// colours over an ambient level) and what the model itself is tinted by (a
// character: the light of the map cell it stands on).  1.0 = as the texture is.
struct Lighting {
    float dir[3][3] = {};       // towards each light
    float colour[3][3] = {};
    float ambient[3] = {1, 1, 1};
    float tint[3] = {1, 1, 1};
};
// The map's light, used by everything drawn until the next map is loaded.
void set_scene_light(const float dirs[4][4], const float colours[4][4]);
const Lighting& scene_light();

struct Strip {
    u16 texture, clut;          // texture descriptor index and palette offset
    std::vector<Vertex> verts;  // a triangle strip, or (list) separate triangles
    bool list = false;
    bool textured = true;
    // Maps: the mesh's place in its cell's list (later ones lie on top of
    // earlier ones where they share a plane), and whether it is one of the
    // cell's second group, drawn after everything else.
    u16 order = 0;
    bool overlay = false;
};

struct Model {
    int type = -1;
    std::vector<Bone>  bones;
    std::vector<Strip> strips;
    sprites::Sheet     ownTextures;     // textures embedded in the file, if any
    bool               hasOwnTextures = false;

    float origin[3] = {}, extent[3] = {};   // maps: the box the grid of cells covers

    bool load(const std::vector<u8>& file);
    bool load_map(const u8* data, size_t size);
    // Draws inside hal::gs::begin_3d / end_3d.  `pose` is one model-space
    // matrix per bone (null = rest pose); `shared` supplies textures for a
    // model without its own.
    // `light`: lit by it (null = drawn as it is: maps, whose lighting is baked).
    void draw(const float* pose, sprites::Sheet* shared, const Lighting* light = nullptr);
    void release();

    // What a draw sends: runs of triangles that share a texture and a depth
    // bias.  A model drawn in its rest pose and unlit (a map) is the same
    // every frame, so its runs are made once and kept.
    struct Batch {
        u32 tex = 0;
        int bias = 0;
        int tile = 0;               // a map's: which 16 × 16 square it lies in (so that it can be culled)
        bool overlay = false;
        std::vector<hal::gs::Vtx3> verts;
    };
    std::vector<Batch> fixedBatches;
    bool fixedBuilt = false;
    int  fixedGeneration = 0;   // texture_pack::generation() when they were made
};

// A motion pack (.mtp) — see tools/mtp.py for the format.
struct MotionPack {
    struct Motion {
        u32 id = 0;
        int boneCount = 0, frames = 0;
        std::vector<u8> data;       // the motion proper; track offsets are relative to it
    };
    std::vector<Motion> motions;

    bool load(const std::vector<u8>& file);
    // Fills `out` with one model-space matrix (16 floats) per bone of `model`.
    void pose(const Model& model, int motion, float frame, std::vector<float>& out) const;
};

// One entry of Common/chara/list.
struct CharaEntry {
    int id;
    std::string name, body, head, pim;
};
bool load_chara_list(std::vector<CharaEntry>& out);

// Debug model viewer (WOTS2_MODEL_VIEW=<name or index>): shows characters on
// a turntable.  Left/Right change character, Esc quits.
int run_viewer(const char* which);

// Model paths of every map in Common/map/list.
bool load_map_list(std::vector<std::string>& out);
// Debug map viewer (WOTS2_MAP_VIEW=<index or part of a name>): fly through a map.
int run_map_viewer(const char* which);

}  // namespace model
