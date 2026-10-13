// Native A2MF model reader / renderer and the debug model viewer.
// Not decompiled code — see include/native/model.h.

#include "native/model.h"
#include "native/archive.h"
#include "native/hle.h"
#include "engine/motion.h"
#include "hal/gs.h"
#include "hal/pad.h"

#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace model {
namespace {

u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
u32 rd32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
float rdf(const u8* p) { float f; memcpy(&f, p, 4); return f; }

// The mesh data is a VIF stream meant for VU1.  Only what the models use is
// understood: DIRECT (sets the texture), UNPACK into a small buffer, and
// MSCNT / MSCAL, which is where one triangle strip is complete.
//
// In a map's batches the fourth word of a position is a flag: 1 = this vertex
// completes a triangle with the two before it, 0 = it does not (the strip
// starts again there).  Drawing every triangle of the strip regardless adds
// about as many stray ones as there are real, lying in the same planes.
void read_vif(const u8* d, size_t start, size_t end, int bone, int bone2, std::vector<Strip>& out,
              bool kickFlags = false) {
    std::map<u32, const u8*> buffers;
    u16 texture = 0, clut = 0;
    size_t p = start;
    while (p + 4 <= end) {
        u32 word = rd32(d + p);
        p += 4;
        u32 cmd = (word >> 24) & 0x7F, num = (word >> 16) & 0xFF, imm = word & 0xFFFF;
        if (cmd == 0x50 || cmd == 0x51) {                   // DIRECT
            if (imm >= 2) {
                texture = rd16(d + p + 24);
                clut = rd16(d + p + 26);
            }
            p += (size_t)imm * 16;
        } else if (cmd == 0x17 || cmd == 0x14) {            // MSCNT / MSCAL
            auto tags = buffers.find(0);
            if (tags != buffers.end()) {
                u32 n = rd32(tags->second) & 0x7FFF;
                const u8* pos = buffers.count(2) ? buffers[2] : nullptr;
                const u8* nrm = buffers.count(2 + n) ? buffers[2 + n] : nullptr;
                const u8* col = buffers.count(2 + 2 * n) ? buffers[2 + 2 * n] : nullptr;
                const u8* uv  = buffers.count(2 + 3 * n) ? buffers[2 + 3 * n] : nullptr;
                // Map meshes have no normals (their lighting is baked into the
                // colours), and some batches have no texture coordinates.
                if (pos) {
                    Strip strip;
                    strip.texture = texture;
                    strip.clut = clut;
                    std::vector<Vertex> all;
                    for (u32 i = 0; i < n; i++) {
                        Vertex v{};
                        for (int k = 0; k < 3; k++)
                            v.p1[k] = rdf(pos + i * 16 + k * 4);
                        v.w1 = 1.0f;
                        // On joint meshes the normal's w picks the bone.
                        bool second = bone != bone2 && nrm && (s16)rd16(nrm + i * 8 + 6) != 0;
                        v.b1 = (s16)(second ? bone2 : bone);
                        v.b2 = -1;
                        if (uv) {
                            v.u = (s16)rd16(uv + i * 8) / 4096.0f;
                            v.v = (s16)rd16(uv + i * 8 + 2) / 4096.0f;
                        }
                        v.rgba = col ? rd32(col + i * 4) : 0x80808080u;
                        if (nrm && !kickFlags)
                            for (int k = 0; k < 3; k++)
                                v.n[k] = (s16)rd16(nrm + i * 8 + k * 2) / 4096.0f;
                        all.push_back(v);
                    }
                    if (kickFlags) {
                        strip.list = true;
                        for (u32 i = 2; i < n; i++) {
                            if (rd32(pos + i * 16 + 12) == 0)
                                continue;
                            strip.verts.push_back(all[i - 2]);
                            strip.verts.push_back(all[i - 1]);
                            strip.verts.push_back(all[i]);
                        }
                    } else {
                        strip.verts = std::move(all);
                    }
                    strip.textured = uv != nullptr;
                    out.push_back(std::move(strip));
                }
            }
            buffers.clear();
        } else if ((cmd & 0x60) == 0x60) {                  // UNPACK
            static const int kBytes[4] = {4, 2, 1, 2};
            u32 vn = (cmd >> 2) & 3, vl = cmd & 3, n = num ? num : 256;
            size_t size = ((size_t)(vn + 1) * kBytes[vl] * n + 3) & ~(size_t)3;
            buffers[imm & 0x3FF] = d + p;
            p += size;
        } else if (cmd == 0x20) {
            p += 4;
        } else if (cmd == 0x30 || cmd == 0x31) {
            p += 16;
        }
    }
}

// The "extra block": vertices blended between two bones on the CPU.
void read_skinned(const u8* d, size_t e, std::vector<Strip>& out) {
    u32 count = rd16(d + e), count2 = rd16(d + e + 2);
    u32 meshes = rd16(d + e + 8) + rd16(d + e + 10);
    const u8* pos1 = d + e + rd32(d + e + 0x0C);
    const u8* pos2 = d + e + rd32(d + e + 0x14);
    const u8* uvs  = d + e + rd32(d + e + 0x1C);
    const u8* cols = d + e + rd32(d + e + 0x20);
    const u8* nrm1 = d + e + rd32(d + e + 0x10);

    // Which bone each vertex of an array follows: runs of {bone, count}.
    auto bones_of = [&](u32 runs, u32 n) {
        std::vector<s16> bones(n, 0);
        const u8* r = d + e + runs;
        u32 i = 0;
        for (;; r += 4) {
            s16 bone = (s16)rd16(r), run = (s16)rd16(r + 2);
            if (bone < 0)
                break;
            for (int k = 0; k < run && i < n; k++)
                bones[i++] = bone;
        }
        return bones;
    };
    std::vector<s16> bone1 = bones_of(rd32(d + e + 0x24), count);
    std::vector<s16> bone2 = bones_of(rd32(d + e + 0x2C), count2);

    for (u32 m = 0; m < meshes; m++) {
        size_t mesh = e + rd32(d + e + 0x34 + m * 4);
        u16 texture = rd16(d + mesh + 8), clut = rd16(d + mesh + 10);
        u32 strips = rd16(d + mesh + 0x0E);
        for (u32 s = 0; s < strips; s++) {
            const u8* strip = d + mesh + rd32(d + mesh + 0x10 + s * 4);
            u32 n = rd32(strip) & 0x3FFF;
            Strip result;
            result.texture = texture;
            result.clut = clut;
            for (u32 i = 0; i < n; i++) {
                const u8* index = strip + 0x10 + i * 8;
                u32 pi = rd16(index), ni = rd16(index + 2), ci = rd16(index + 4), ui = rd16(index + 6);
                if (pi >= count)
                    continue;
                Vertex v{};
                for (int k = 0; k < 3; k++)
                    v.p1[k] = rdf(pos1 + pi * 16 + k * 4);
                v.w1 = rdf(pos1 + pi * 16 + 12);
                v.b1 = bone1[pi];
                v.b2 = -1;
                if (pi < count2) {
                    for (int k = 0; k < 3; k++)
                        v.p2[k] = rdf(pos2 + pi * 16 + k * 4);
                    v.w2 = rdf(pos2 + pi * 16 + 12);
                    v.b2 = bone2[pi];
                }
                v.u = (s16)rd16(uvs + ui * 8) / 4096.0f;
                v.v = (s16)rd16(uvs + ui * 8 + 2) / 4096.0f;
                v.rgba = rd32(cols + ci * 4);
                if (ni < count)                     // (its first influence's normal, in that bone's space)
                    for (int k = 0; k < 3; k++)
                        v.n[k] = rdf(nrm1 + ni * 16 + k * 4);
                result.verts.push_back(v);
            }
            out.push_back(std::move(result));
        }
    }
}

// result = a × b (row-vector convention: apply a, then b)
void mul(const float* a, const float* b, float* out) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j]
                         + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof(r));
}

}  // namespace

bool Model::load(const std::vector<u8>& file) {
    fixedBatches.clear();
    fixedBuilt = false;
    if (file.size() < 0x40 || memcmp(file.data(), "A2MF", 4) != 0)
        return false;
    type = file[4];
    const u8* d = file.data() + 0x10;
    const size_t size = file.size() - 0x10;
    if (type == 3)
        return load_map(d, size);
    u32 boneCount = rd16(d + 2);
    u32 meshCount, boneOff, texOff, extraOff, table;
    if (type == 1) {
        meshCount = rd16(d + 4) + rd16(d + 6);
        boneOff = rd32(d + 8); texOff = rd32(d + 0x10); extraOff = rd32(d + 0x14);
        table = 0x18;
    } else if (type == 0) {
        meshCount = rd16(d + 4) + rd16(d + 6) + rd16(d + 8) + rd16(d + 10);
        boneOff = rd32(d + 0x0C); texOff = rd32(d + 0x14); extraOff = rd32(d + 0x18);
        table = 0x1C;
    } else {
        return false;
    }

    bones.resize(boneCount);
    for (u32 i = 0; i < boneCount; i++) {
        const u8* b = d + boneOff + i * 0xA0;
        memcpy(bones[i].rest, b, 64);
        memcpy(bones[i].local, b + 0x40, 64);
        bones[i].parent = (s32)rd32(b + 0x80);
    }
    for (u32 i = 0; i < meshCount; i++) {
        size_t mesh = rd32(d + table + i * 4);
        u32 qwords = rd32(d + mesh + (type == 1 ? 4 : 8));
        u32 data = rd32(d + mesh + (type == 1 ? 8 : 12));
        size_t start = mesh + data, end = start + (size_t)qwords * 16;
        if (end > size)
            continue;
        read_vif(d, start, end, d[mesh], d[mesh + 1], strips);
    }
    if (extraOff)
        read_skinned(d, extraOff, strips);
    if (texOff) {
        std::vector<u8> pim(d + texOff, d + size);
        hasOwnTextures = ownTextures.load(pim);
    }
    return true;
}

// Type 3, a map: origin and cell size, a grid of cells at +0x30 ({offset,
// size} each), every cell a list of meshes with world-space vertices.
bool Model::load_map(const u8* d, size_t size) {
    fixedBatches.clear();
    fixedBuilt = false;
    Bone root{};
    root.rest[0] = root.rest[5] = root.rest[10] = root.rest[15] = 1.0f;
    memcpy(root.local, root.rest, sizeof(root.rest));
    root.parent = -1;
    bones.assign(1, root);
    for (int k = 0; k < 3; k++) {
        origin[k] = rdf(d + k * 4);
        extent[k] = rdf(d + 0x10 + k * 4) * (float)(s32)rd32(d + 0x20 + k * 4);
    }
    u32 cells = rd32(d + 0x20) * rd32(d + 0x24) * rd32(d + 0x28);
    u32 texOff = rd32(d + 0x2C);
    for (u32 i = 0; i < cells; i++) {
        size_t cell = rd32(d + 0x30 + i * 8);
        if (rd32(d + 0x34 + i * 8) == 0 || cell + 4 > size)
            continue;
        const u32 first = rd16(d + cell);
        u32 count = first + rd16(d + cell + 2);
        u16 sequence = 0;
        for (u32 m = 0; m < count; m++) {
            size_t mesh = cell + rd32(d + cell + 4 + m * 4);
            size_t start = mesh + rd32(d + mesh + 8), end = start + (size_t)rd32(d + mesh + 4) * 16;
            const size_t before = strips.size();
            if (end <= size)
                read_vif(d, start, end, 0, 0, strips, true);
            for (size_t k = before; k < strips.size(); k++) {
                strips[k].order = sequence < 8 ? sequence++ : sequence;
                strips[k].overlay = m >= first;
            }
        }
    }
    if (texOff) {
        std::vector<u8> pim(d + texOff, d + size);
        hasOwnTextures = ownTextures.load(pim);
    }
    return true;
}

namespace { Lighting g_sceneLight, g_mapLight; }
// A map's light block: rows 0–2 of the first matrix are the lights' colours
// and row 3 the ambient level; rows 0–2 of the second the directions to them.
void set_scene_light(const float colours[4][4], const float dirs[4][4]) {
    Lighting l;
    for (int i = 0; i < 3; i++) {
        float length = 0;
        for (int k = 0; k < 3; k++)
            length += dirs[i][k] * dirs[i][k];
        length = sqrtf(length);
        for (int k = 0; k < 3; k++) {
            l.dir[i][k] = length > 0 ? dirs[i][k] / length : 0.0f;
            l.colour[i][k] = length > 0 ? colours[i][k] : 0.0f;
        }
    }
    for (int k = 0; k < 3; k++)
        l.ambient[k] = colours[3][k];
    g_sceneLight = g_mapLight = l;
}
const Lighting& scene_light() { return g_sceneLight; }

}  // namespace model

// What story scripts change (the game: the lighting object's first light,
// 0x165BE0 direction / 0x165B90 colour; 0x165C10 puts the map's back).
extern "C" void hle_light_set_dir(float x, float y, float z) {
    const float length = sqrtf(x * x + y * y + z * z);
    if (length > 0) {
        model::g_sceneLight.dir[0][0] = x / length;
        model::g_sceneLight.dir[0][1] = y / length;
        model::g_sceneLight.dir[0][2] = z / length;
    }
}
extern "C" void hle_light_set_colour(float r, float g, float b) {
    model::g_sceneLight.colour[0][0] = r;
    model::g_sceneLight.colour[0][1] = g;
    model::g_sceneLight.colour[0][2] = b;
}
extern "C" void hle_light_restore() { model::g_sceneLight = model::g_mapLight; }

namespace model {

void Model::draw(const float* pose, sprites::Sheet* shared, const Lighting* light) {
    sprites::Sheet* sheet = hasOwnTextures ? &ownTextures : shared;
    // Two passes: the solid meshes, each a hair nearer than the ones before
    // it in its cell (on the PS2 the later of two surfaces in one plane
    // simply wins); then the overlays.
    // WOTS2_VIEW_STRIPS="first-last" (debug): draw only those strips.
    static int onlyFirst = 0, onlyLast = 1 << 30;
    static bool asked = false;
    if (!asked) {
        asked = true;
        if (const char* only = getenv("WOTS2_VIEW_STRIPS"))
            sscanf(only, "%d-%d", &onlyFirst, &onlyLast);
    }
    // The strips are gathered into runs of triangles with one texture and one
    // depth bias: a draw call apiece costs far more than the triangles do.
    // A map (rest pose, unlit) gives the same runs every frame — and there
    // strips of different cells may share a run, since each triangle keeps
    // its own bias — so those are made once.
    const bool fixed = pose == nullptr && light == nullptr && onlyFirst == 0 && onlyLast == (1 << 30);
    static std::vector<Batch> scratch;
    std::vector<Batch>& batches = fixed ? fixedBatches : scratch;
    if (!fixed || !fixedBuilt) {
        batches.clear();
        for (int pass = 0; pass < 2; pass++)
        for (const Strip& strip : strips) {
            if (strip.overlay != (pass == 1))
                continue;
            const int number = (int)(&strip - strips.data());
            if (number < onlyFirst || number > onlyLast)
                continue;
            if (onlyFirst == onlyLast && hal::gs::frame_count() == 1)
                printf("[viewer] strip %d: texture %u clut %u, %zu vertices, bone %d / %d, first at %.3f %.3f %.3f uv %.3f %.3f rgba %08x%s\n",
                       number, strip.texture, strip.clut, strip.verts.size(), strip.verts[0].b1, strip.verts[0].b2,
                       strip.verts[0].p1[0], strip.verts[0].p1[1], strip.verts[0].p1[2], strip.verts[0].u, strip.verts[0].v,
                       strip.verts[0].rgba, strip.textured ? "" : " (untextured)");
            const int bias = -(int)strip.order - (pass == 1 ? 10 : 0);
            u32 tex = 0;
            if (sheet && strip.textured && strip.texture < sheet->textures.size()) {
                sprites::Rect rect{};
                rect.texture = (u8)strip.texture;
                rect.clutOffset = (u8)strip.clut;
                tex = sheet->texture_for(rect);
            }
            // The run this strip joins: the last one (a moving model keeps its
            // order of drawing), or for a map any with the same key.
            Batch* batch = nullptr;
            if (fixed) {
                for (Batch& b : batches)
                    if (b.tex == tex && b.bias == bias && b.overlay == (pass == 1)) {
                        batch = &b;
                        break;
                    }
            } else if (!batches.empty() && batches.back().tex == tex && batches.back().bias == bias &&
                       batches.back().overlay == (pass == 1)) {
                batch = &batches.back();
            }
            if (batch == nullptr) {
                batches.emplace_back();
                batch = &batches.back();
                batch->tex = tex;
                batch->bias = bias;
                batch->overlay = pass == 1;
            }
            hal::gs::Vtx3 window[2];        // the last two points of a strip
            int index = 0;
            for (const Vertex& v : strip.verts) {
                float p[3] = {0, 0, 0};
                const struct { const float* q; float w; int bone; } part[2] = {
                    {v.p1, v.w1, v.b1}, {v.p2, v.w2, v.b2}};
                for (const auto& in : part) {
                    if (in.bone < 0 || in.bone >= (int)bones.size())
                        continue;
                    const float* m = pose ? pose + in.bone * 16 : bones[in.bone].rest;
                    for (int k = 0; k < 3; k++)
                        p[k] += in.q[0] * m[k] + in.q[1] * m[4 + k] + in.q[2] * m[8 + k] + in.w * m[12 + k];
                }
                // Untextured map geometry takes its colour as it is; a model whose
                // texture is missing gets a neutral grey.
                u32 rgba = tex ? v.rgba : (strip.textured ? 0xFF909090u : v.rgba);
                if (light && (v.n[0] != 0 || v.n[1] != 0 || v.n[2] != 0) && v.b1 >= 0 && v.b1 < (int)bones.size()) {
                    const float* m = pose ? pose + v.b1 * 16 : bones[v.b1].rest;
                    float n[3], length = 0;
                    for (int k = 0; k < 3; k++) {
                        n[k] = v.n[0] * m[k] + v.n[1] * m[4 + k] + v.n[2] * m[8 + k];
                        length += n[k] * n[k];
                    }
                    length = sqrtf(length);
                    float lit[3] = {light->ambient[0], light->ambient[1], light->ambient[2]};
                    if (length > 0)
                        for (int i = 0; i < 3; i++) {
                            const float facing = (n[0] * light->dir[i][0] + n[1] * light->dir[i][1] + n[2] * light->dir[i][2]) / length;
                            if (facing > 0)
                                for (int k = 0; k < 3; k++)
                                    lit[k] += facing * light->colour[i][k];
                        }
                    u32 out = rgba & 0xFF000000u;
                    for (int k = 0; k < 3; k++) {
                        float c = (float)((rgba >> (k * 8)) & 0xFF) * lit[k] * light->tint[k];
                        out |= (u32)(c > 255.0f ? 255.0f : c) << (k * 8);
                    }
                    rgba = out;
                }
                const hal::gs::Vtx3 point = {p[0], p[1], p[2], v.u, v.v, rgba};
                if (strip.list) {
                    batch->verts.push_back(point);
                } else {
                    if (index >= 2) {
                        batch->verts.push_back(window[0]);
                        batch->verts.push_back(window[1]);
                        batch->verts.push_back(point);
                    }
                    window[0] = window[1];
                    window[1] = point;
                }
                index++;
            }
        }
        if (fixed)
            fixedBuilt = true;
    }
    for (const Batch& batch : batches) {
        if (batch.verts.empty())
            continue;
        hal::gs::set_depth_bias(batch.bias);
        hal::gs::set_overlay(batch.overlay);
        hal::gs::draw_mesh(batch.tex, batch.verts.data(), (int)batch.verts.size(), hal::gs::Prim::Triangles);
    }
    hal::gs::set_depth_bias(0);
    hal::gs::set_overlay(false);
}

void Model::release() {
    fixedBatches.clear();
    fixedBuilt = false;
    ownTextures.release();
}

// ── Motions ───────────────────────────────────────────────────────────────
bool MotionPack::load(const std::vector<u8>& file) {
    motions.clear();
    if (file.size() < 12 || rd32(file.data()) < 5)
        return false;
    u32 count = rd32(&file[4]);
    size_t p = 8;
    for (u32 i = 0; i < count && p + 8 <= file.size(); i++) {
        u32 id = rd32(&file[p]), size = rd32(&file[p + 4]);
        if (p + 8 + size > file.size())
            break;
        if (id != 0xFFFFFFFFu && size >= 24) {
            Motion m;
            m.id = id;
            m.data.assign(&file[p + 16], &file[p + 8] + size);     // skip {8, size}
            m.boneCount = (s16)rd16(&m.data[2]);
            m.frames = (s16)rd16(&m.data[6]);
            motions.push_back(std::move(m));
        }
        p += 8 + size;
    }
    return !motions.empty();
}

// Poses with the game's own samplers (src/engine/motion.cpp).
void MotionPack::pose(const Model& model, int motion, float frame, std::vector<float>& out) const {
    out.assign(model.bones.size() * 16, 0.0f);
    if (motion < 0 || motion >= (int)motions.size())
        return;
    const Motion& m = motions[motion];
    auto track = [&](size_t index) -> const u8* {
        if (8 + (index + 1) * 4 > m.data.size())
            return nullptr;
        u32 offset = rd32(&m.data[8 + index * 4]);
        return offset && offset + 8 <= m.data.size() ? &m.data[offset] : nullptr;
    };
    for (size_t i = 0; i < model.bones.size(); i++) {
        float local[16];
        memcpy(local, model.bones[i].local, sizeof(local));
        if ((int)i < m.boneCount) {
            if (const u8* rotation = track(3 + i)) {
                float q[4], turned[16];
                Motion_SampleRotation(frame, nullptr, q, rotation, 1);
                Quat_ToMatrix(q, turned);
                memcpy(local, turned, 12 * sizeof(float));      // keep the rest translation
            }
            // Track 1 places the root bone (hips); track 0 is how far the whole
            // character travels over the move, which is left out here.
            const u8* hips = i == 0 ? track(1) : nullptr;
            if (hips) {
                float p[4];
                Motion_SamplePosition(frame, nullptr, p, hips, 1);
                memcpy(&local[12], p, 3 * sizeof(float));
            }
        }
        int parent = model.bones[i].parent;
        if (parent < 0 || parent >= (int)i)
            memcpy(&out[i * 16], local, sizeof(local));
        else
            mul(local, &out[(size_t)parent * 16], &out[i * 16]);
    }
}

// Common/chara/list: u16 count, u16 version, then per character
//   u32 id, name, 3 body paths, 3 head paths, [version > 1: texture path],
//   u32 value, [version > 2: two floats]        (strings are u32 length + bytes;
// only the middle body and head paths are ever filled in).
bool load_chara_list(std::vector<CharaEntry>& out) {
    std::vector<u8> d;
    if (!archive::read("Common/chara/list", d) || d.size() < 4)
        return false;
    u32 count = rd16(d.data()), version = rd16(d.data() + 2);
    size_t p = 4;
    auto text = [&]() {
        u32 length = rd32(&d[p]);
        std::string s(reinterpret_cast<const char*>(&d[p + 4]), length);
        p += 4 + length;
        return s;
    };
    for (u32 i = 0; i < count && p + 8 <= d.size(); i++) {
        CharaEntry e;
        e.id = (int)rd32(&d[p]);
        p += 4;
        e.name = text();
        std::string body[3], head[3];
        for (auto& s : body) s = text();
        for (auto& s : head) s = text();
        if (version > 1) e.pim = text();
        p += 4;
        if (version > 2) p += 8;
        e.body = body[1];
        e.head = head[1];
        out.push_back(e);
    }
    return !out.empty();
}

// ── Debug model viewer ────────────────────────────────────────────────────
int run_viewer(const char* which) {
    std::vector<CharaEntry> list;
    if (!load_chara_list(list)) {
        fprintf(stderr, "[viewer] Common/chara/list could not be read\n");
        return 1;
    }
    int index = 0;
    bool found = false;
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].name == which) { index = (int)i; found = true; }
    if (!found)
        index = atoi(which) % (int)list.size();

    // WOTS2_MOTION=<pack name> picks the motion pack (default ka08, a sword
    // style); Up / Down step through its motions, Triangle toggles the rest pose.
    MotionPack pack;
    {
        const char* name = getenv("WOTS2_MOTION");
        std::vector<u8> file;
        std::string path = std::string("Common/motion/") + (name ? name : "ka08") + ".mtp";
        if (!archive::read(path, file) || !pack.load(file))
            fprintf(stderr, "[viewer] motion pack %s could not be loaded\n", path.c_str());
        else
            printf("[viewer] %s: %zu motions\n", path.c_str(), pack.motions.size());
    }
    int motion = 0;
    float frame = 0;
    bool animate = !pack.motions.empty();
    std::vector<float> pose, headPose;

    Model body, head;
    sprites::Sheet shared;
    bool haveShared = false, haveHead = false;
    auto load = [&]() {
        body.release(); head.release(); shared.release();
        body = Model(); head = Model(); shared = sprites::Sheet();
        haveShared = haveHead = false;
        const CharaEntry& e = list[index];
        std::vector<u8> file;
        auto check = [&](const std::string& path) {
            std::string problem = hle::check_model(file);
            if (!problem.empty())
                fprintf(stderr, "[viewer] decompiled model code: %s: %s\n", path.c_str(), problem.c_str());
        };
        if (!archive::read(e.body, file) || !body.load(file))
            fprintf(stderr, "[viewer] %s: body %s could not be loaded\n", e.name.c_str(), e.body.c_str());
        else
            check(e.body);
        if (!e.head.empty() && archive::read(e.head, file)) {
            haveHead = head.load(file);
            check(e.head);
        }
        if (!e.pim.empty() && archive::read(e.pim, file))
            haveShared = shared.load(file);
        size_t verts = 0;
        for (const Strip& s : body.strips) verts += s.verts.size();
        printf("[viewer] %d %s: %zu bones, %zu strips, %zu vertices%s\n", e.id, e.name.c_str(),
               body.bones.size(), body.strips.size(), verts, haveHead ? ", separate head" : "");
    };
    load();

    u16 previous = 0xFFFF;
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                return 0;
        hal::pad::poll();
        u16 buttons = hal::pad::get_buttons(0);
        u16 pressed = (u16)(previous & ~buttons);      // active low
        previous = buttons;
        int step = (pressed & hal::pad::RIGHT) ? 1 : (pressed & hal::pad::LEFT) ? -1 : 0;
        if (step) {
            index = (index + step + (int)list.size()) % (int)list.size();
            load();
        }
        if (!pack.motions.empty()) {
            int change = (pressed & hal::pad::UP) ? 1 : (pressed & hal::pad::DOWN) ? -1 : 0;
            if (change) {
                motion = (motion + change + (int)pack.motions.size()) % (int)pack.motions.size();
                frame = 0;
                printf("[viewer] motion %d (id %u, %d frames)\n", motion, pack.motions[motion].id,
                       pack.motions[motion].frames);
            }
            if (pressed & hal::pad::TRIANGLE)
                animate = !animate;
            frame += 1.0f;
            if (frame > (float)pack.motions[motion].frames)
                frame = 0;
            if (animate) {
                pack.pose(body, motion, frame, pose);
                if (haveHead)
                    pack.pose(head, motion, frame, headPose);
            }
        }

        // Turntable: model × view × projection, row-vector convention.
        float angle = hal::gs::frame_count() * 0.012f;
        if (const char* at = getenv("WOTS2_VIEW_AT")) {
            float y, z, a = -100.0f;
            if (sscanf(at, "%f,%f,%f", &y, &z, &a) == 3)
                angle = a;
        }
        const float c = cosf(angle), s = sinf(angle);
        const float turn[16] = {c, 0, -s, 0,  0, 1, 0, 0,  s, 0, c, 0,  0, 0, 0, 1};
        // WOTS2_VIEW_AT="height,distance[,angle]" (debug) looks closer at a part.
        static float lookY = 0.9f, lookZ = 3.0f, fixed = -100.0f;
        static bool placed = false;
        if (!placed) {
            placed = true;
            if (const char* at = getenv("WOTS2_VIEW_AT"))
                sscanf(at, "%f,%f,%f", &lookY, &lookZ, &fixed);
        }
        const float view[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, -lookY, -lookZ, 1};
        const float f = 1.0f / tanf(0.5f * 38.0f * 3.14159265f / 180.0f);
        const float zn = 0.1f, zf = 50.0f, aspect = 4.0f / 3.0f;
        const float proj[16] = {f / aspect, 0, 0, 0,  0, f, 0, 0,
                                0, 0, (zf + zn) / (zn - zf), -1,  0, 0, 2 * zf * zn / (zn - zf), 0};
        float mvp[16];
        mul(turn, view, mvp);
        mul(mvp, proj, mvp);

        hal::gs::begin_frame();
        hal::gs::clear(0x80402820u);
        hal::gs::begin_3d(mvp);
        body.draw(animate ? pose.data() : nullptr, haveShared ? &shared : nullptr);
        if (haveHead)
            head.draw(animate ? headPose.data() : nullptr, haveShared ? &shared : nullptr);
        hal::gs::end_3d();
        hal::gs::end_frame();
    }
}

// Common/map/list names every map's files; rather than depend on its record
// layout (not decoded yet), this picks out the model paths: strings are
// stored as a u32 length followed by the characters.
bool load_map_list(std::vector<std::string>& out) {
    std::vector<u8> d;
    if (!archive::read("Common/map/list", d))
        return false;
    for (size_t p = 0; p + 16 < d.size(); p++) {
        u32 length = rd32(&d[p]);
        if (length < 12 || length > 80 || p + 4 + length > d.size())
            continue;
        if (memcmp(&d[p + 4], "Common/map/", 11) != 0)
            continue;
        std::string s(reinterpret_cast<const char*>(&d[p + 4]), length);
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".mdl") == 0 && s.find("/etc/") == std::string::npos) {
            bool seen = false;
            for (const std::string& o : out) seen = seen || o == s;
            if (!seen) out.push_back(s);
        }
        p += 3 + length;
    }
    return !out.empty();
}

// ── Debug map viewer ──────────────────────────────────────────────────────
// WOTS2_MAP_VIEW=<index or part of a name>.  Up / Down move, Left / Right
// turn, L1 / R1 (Q / W) sink / rise, L2 / R2 (E / R) look down / up,
// Circle / Square (X / A) next / previous map, Esc quits.
int run_map_viewer(const char* which) {
    std::vector<std::string> maps;
    if (!load_map_list(maps)) {
        fprintf(stderr, "[viewer] Common/map/list could not be read\n");
        return 1;
    }
    int index = -1;
    for (size_t i = 0; i < maps.size(); i++)
        if (index < 0 && maps[i].find(which) != std::string::npos) index = (int)i;
    if (index < 0)
        index = atoi(which) % (int)maps.size();

    Model map;
    float eye[3] = {0, 0, 0}, yaw = 0, pitch = -0.35f;
    auto load = [&]() {
        map.release();
        map = Model();
        std::vector<u8> file;
        bool ok = archive::read(maps[index], file) && map.load(file);
        if (ok) {
            std::string problem = hle::check_model(file);
            if (!problem.empty())
                fprintf(stderr, "[viewer] decompiled model code: %s: %s\n", maps[index].c_str(), problem.c_str());
        }
        size_t verts = 0;
        for (const Strip& s : map.strips) verts += s.verts.size();
        printf("[viewer] map %d %s: %zu strips, %zu vertices%s\n", index, maps[index].c_str(),
               map.strips.size(), verts, ok ? "" : " (failed)");
        // Start above the middle, looking across it.
        // Start above one edge, looking down across the middle.  (Ground level
        // is around y = 0 in every map looked at so far.)
        eye[0] = map.origin[0] + map.extent[0] * 0.5f;
        eye[1] = 18.0f;
        eye[2] = map.origin[2] + map.extent[2] * 0.5f + 34.0f;
        yaw = 0;
        pitch = -0.5f;
    };
    load();

    u16 previous = 0xFFFF;
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                return 0;
        hal::pad::poll();
        u16 buttons = hal::pad::get_buttons(0);
        u16 held = (u16)~buttons, pressed = (u16)(previous & ~buttons);
        previous = buttons;
        int step = (pressed & hal::pad::CIRCLE) ? 1 : (pressed & hal::pad::SQUARE) ? -1 : 0;
        if (step) {
            index = (index + step + (int)maps.size()) % (int)maps.size();
            load();
        }
        const float speed = 0.35f;
        if (held & hal::pad::LEFT)  yaw += 0.04f;
        if (held & hal::pad::RIGHT) yaw -= 0.04f;
        if (held & hal::pad::L2)    pitch -= 0.03f;
        if (held & hal::pad::R2)    pitch += 0.03f;
        float forward = (held & hal::pad::UP) ? speed : (held & hal::pad::DOWN) ? -speed : 0.0f;
        eye[0] += -sinf(yaw) * forward;
        eye[2] += -cosf(yaw) * forward;
        if (held & hal::pad::L1) eye[1] -= speed;
        if (held & hal::pad::R1) eye[1] += speed;

        // view = translate(−eye) × turn(−yaw) × tilt(−pitch), row vectors
        const float cy = cosf(-yaw), sy = sinf(-yaw), cp = cosf(-pitch), sp = sinf(-pitch);
        const float move[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  -eye[0], -eye[1], -eye[2], 1};
        const float turn[16] = {cy, 0, -sy, 0,  0, 1, 0, 0,  sy, 0, cy, 0,  0, 0, 0, 1};
        const float tilt[16] = {1, 0, 0, 0,  0, cp, sp, 0,  0, -sp, cp, 0,  0, 0, 0, 1};
        const float f = 1.0f / tanf(0.5f * 55.0f * 3.14159265f / 180.0f);
        const float zn = 0.2f, zf = 400.0f, aspect = 4.0f / 3.0f;
        const float proj[16] = {f / aspect, 0, 0, 0,  0, f, 0, 0,
                                0, 0, (zf + zn) / (zn - zf), -1,  0, 0, 2 * zf * zn / (zn - zf), 0};
        float mvp[16];
        mul(move, turn, mvp);
        mul(mvp, tilt, mvp);
        mul(mvp, proj, mvp);

        hal::gs::begin_frame();
        hal::gs::clear(0x80D8B890u);
        hal::gs::begin_3d(mvp);
        map.draw(nullptr, nullptr);
        hal::gs::end_3d();
        hal::gs::end_frame();
    }
}

}  // namespace model
