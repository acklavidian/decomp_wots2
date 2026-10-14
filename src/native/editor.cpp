// The level editor — native, not part of the game.  F2 in the world opens
// it over the picture (Dear ImGui) and holds the game still; F2 again closes
// it and the game goes on.
//
// What a level is made of here, and what can be edited:
//   - the map's model (Common/map/*.mdl): not edited — it is the 3D artwork;
//   - its placement file (object/iom/*.Iom, named by object/ObjectIom.Lst):
//     the crates, pots, pickups, scenery, sliding doors, effects and sounds
//     of the place — moved, turned, added, removed, changed;
//   - its collision file (Common/collision/*.col): the grid of cells that says
//     where one can walk, where the walls, doors and exits are and how high
//     the ground is — cells painted with any of the map's cell types, and the
//     types themselves changed; and the named points in it (where one
//     arrives through each way in) moved.
// Edits are saved as a mod, mods/level_edits/files/<the file's path>, which
// the mod loader lays over the game's own files (see mods/README.md); the
// disc's files are never touched.
//
// Controls: right mouse held + move = look, W A S D = fly, Q / E = down / up,
// Shift = faster.  Left click picks (objects, points) or paints (cells);
// dragging moves what was picked over the ground.  Delete removes, Ctrl+D
// copies, R / Shift+R turns by 15°, Ctrl+Z undoes, Ctrl+S saves.

#include "native/editor.h"
#include "native/editor_hooks.h"
#include "native/archive.h"
#include "native/guest.h"
#include "game/map_collision.h"
#include "hal/gs.h"
#include "hal/pad.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.14159265f;

// ── What is being edited ───────────────────────────────────────────────────
struct Placement {
    float m[16];        // row vectors: x, y, z axes, then the place
    s32 id;             // object kind (object/ObjectName.Lst)
    u8 tail[12];        // the rest of the record, kept as it was
};

struct Level {
    int map = -1;
    u8* colFile = nullptr;          // (to see when the game has loaded another map)
    std::string iomPath;            // "" = the map has no placement file
    u8 iomHeader[12] = {};
    std::vector<Placement> items;
    std::vector<u8> iomTrail;       // what follows the records the count names (unread by the game; kept)
    bool iomDirty = false, colDirty = false;
};
Level sLevel;
EditorMap sMap;

struct Snapshot {
    std::vector<Placement> items;
    std::vector<u8> col;
    bool iomDirty, colDirty;
};
std::deque<Snapshot> sUndo;

enum Mode { kObjects, kCells, kPoints };

bool sReady = false;        // ImGui set up
bool sOpen = false;
bool sPause = true;
int  sMode = kObjects;
std::string sStatus;

// The editor's camera.
bool  sCamSet = false;
float sEye[3] = {0, 5, 0}, sYaw = 0, sPitch = -0.4f;
float sSpeed = 6.0f;            // units a second

// Picking and dragging.
int   sSelected = -1;           // placement, or point
bool  sDragging = false;
float sDragLift = 0;            // how far above the ground the dragged thing was
float sDragOffset[2] = {0, 0};  // and how far (x, z) from where the mouse took hold of it
int   sAddKind = 3;             // what "Add" puts down
bool  sPlacing = false;         // the next click puts one down
char  sKindFilter[64] = "";

// Cells.
bool  sShowCells = true;
float sCellRadius = 6.0f;
int   sBrush = 1;               // cells across
int   sPaintType = -1;          // type id
bool  sStroke = false;          // a paint stroke under way (one undo step)

// The mouse's ray and where it meets the ground.
float sRayFrom[3], sRayDir[3];
bool  sHit = false;
float sHitPos[3];

// ── Small maths ────────────────────────────────────────────────────────────
bool invert(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f)
        return false;
    for (int i = 0; i < 16; i++)
        out[i] = inv[i] / det;
    return true;
}

// The frame's view-projection as drawn (widescreen narrows clip x).
void view_proj(float out[16]) {
    memcpy(out, sMap.viewProj, sizeof sMap.viewProj);
    const float k = hal::gs::clip_x_scale();
    for (int r = 0; r < 4; r++)
        out[r * 4] *= k;
}

// World → ImGui's coordinates (window points).  False behind the eye.
bool project(const float p[3], ImVec2* out) {
    float m[16];
    view_proj(m);
    float c[4];
    for (int j = 0; j < 4; j++)
        c[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
    if (c[3] <= 0.05f)
        return false;
    int r[4];
    hal::gs::present_rect(r);
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const float x = r[0] + (c[0] / c[3] + 1.0f) * 0.5f * (float)(r[2] - r[0]);
    const float y = r[1] + (1.0f - c[1] / c[3]) * 0.5f * (float)(r[3] - r[1]);
    *out = ImVec2(x / (scale.x > 0 ? scale.x : 1), y / (scale.y > 0 ? scale.y : 1));
    return true;
}

// The ray under the mouse.
bool mouse_ray(float from[3], float dir[3]) {
    float m[16], inv[16];
    view_proj(m);
    if (!invert(m, inv))
        return false;
    int r[4];
    hal::gs::present_rect(r);
    const ImGuiIO& io = ImGui::GetIO();
    const float px = io.MousePos.x * io.DisplayFramebufferScale.x, py = io.MousePos.y * io.DisplayFramebufferScale.y;
    if (r[2] <= r[0] || r[3] <= r[1])
        return false;
    const float nx = (px - r[0]) / (float)(r[2] - r[0]) * 2.0f - 1.0f;
    const float ny = 1.0f - (py - r[1]) / (float)(r[3] - r[1]) * 2.0f;
    float pts[2][3];
    for (int k = 0; k < 2; k++) {
        const float c[4] = {nx, ny, k == 0 ? -1.0f : 1.0f, 1.0f};
        float w[4];
        for (int j = 0; j < 4; j++)
            w[j] = c[0] * inv[j] + c[1] * inv[4 + j] + c[2] * inv[8 + j] + c[3] * inv[12 + j];
        if (w[3] == 0.0f)
            return false;
        for (int j = 0; j < 3; j++)
            pts[k][j] = w[j] / w[3];
    }
    float len = 0;
    for (int j = 0; j < 3; j++) {
        from[j] = pts[0][j];
        dir[j] = pts[1][j] - pts[0][j];
        len += dir[j] * dir[j];
    }
    len = sqrtf(len);
    if (len <= 0.0f)
        return false;
    for (int j = 0; j < 3; j++)
        dir[j] /= len;
    return true;
}

// ── The collision file ─────────────────────────────────────────────────────
Collision* col() { return static_cast<Collision*>(sMap.collision); }
const ColHeader* header() { return col() ? guest::ptr<ColHeader>(col()->header) : nullptr; }
ColCellType* types() { return header() ? reinterpret_cast<ColCellType*>(const_cast<ColHeader*>(header()) + 1) : nullptr; }
s16* cells() { return col() ? guest::ptr<s16>(col()->cells) : nullptr; }

ColCellType* type_by_id(int id) {
    if (!header())
        return nullptr;
    for (s32 i = 0; i < header()->typeCount; i++)
        if (types()[i].id == id)
            return &types()[i];
    return nullptr;
}

// The cell a place lies in, as the game finds it (0x16BA90).
bool cell_index(float x, float z, int* cx, int* cz) {
    const ColHeader* h = header();
    if (!h || h->width <= 0)
        return false;
    *cx = (int)(x * 100.0f / h->cellSize) + h->offsetX;
    *cz = (int)(z * 100.0f / h->cellSize) + h->offsetZ;
    return *cx >= 0 && *cz >= 0 && *cx < h->width && *cz < h->cellCount / h->width;
}
s16* cell_at(int cx, int cz) { return cells() + (cx + cz * header()->width) * 2; }

// Where a cell lies (the game's (int) rounds toward zero, so cell 0 of an
// axis is two cells wide).
void cell_span(int c, int offset, float* lo, float* hi) {
    const float s = header()->cellSize / 100.0f;
    const int k = c - offset;
    if (k > 0) { *lo = k * s; *hi = (k + 1) * s; }
    else if (k < 0) { *lo = (k - 1) * s; *hi = k * s; }
    else { *lo = -s; *hi = s; }
}

bool ground_at(float x, float z, float* y) {
    int cx, cz;
    if (!cell_index(x, z, &cx, &cz))
        return false;
    const ColCellType* t = type_by_id(cell_at(cx, cz)[1]);
    if (!t)
        return false;
    *y = t->height;
    return true;
}

// The spawn points: in the file (s32 count, {s32 name, float x, y, z}[]) and
// in the copy the collision object made of them.
s32 point_count() { return sMap.collisionFile ? *reinterpret_cast<s32*>(sMap.collisionFile) : 0; }
float* point_pos(int i) { return reinterpret_cast<float*>(sMap.collisionFile + 4 + i * 16 + 4); }
s32 point_name(int i) { return *reinterpret_cast<s32*>(sMap.collisionFile + 4 + i * 16); }
void point_moved(int i) {
    if (col() && col()->spots)
        memcpy(guest::ptr<ColSpot>(col()->spots)[i].pos, point_pos(i), 12);
    sLevel.colDirty = true;
}

// ── The placement file ─────────────────────────────────────────────────────
std::vector<u8> iom_bytes() {
    std::vector<u8> out(16 + sLevel.items.size() * 0x50);
    out.insert(out.end(), sLevel.iomTrail.begin(), sLevel.iomTrail.end());
    const s32 n = (s32)sLevel.items.size();
    memcpy(out.data(), &n, 4);
    memcpy(out.data() + 4, sLevel.iomHeader, 12);
    for (size_t i = 0; i < sLevel.items.size(); i++) {
        u8* r = out.data() + 16 + i * 0x50;
        memcpy(r, sLevel.items[i].m, 64);
        memcpy(r + 0x40, &sLevel.items[i].id, 4);
        memcpy(r + 0x44, sLevel.items[i].tail, 12);
    }
    return out;
}

void objects_changed() {
    sLevel.iomDirty = true;
    Objects_ReloadPlacements(iom_bytes());
}

void load_level() {
    sLevel = Level();
    sUndo.clear();
    sSelected = -1;
    sLevel.map = sMap.index;
    sLevel.colFile = sMap.collisionFile;
    sLevel.iomPath = Objects_PlacementFile(sMap.index);
    std::vector<u8> file;
    if (!sLevel.iomPath.empty() && archive::read(sLevel.iomPath, file) && file.size() >= 16) {
        s32 count;
        memcpy(&count, file.data(), 4);
        memcpy(sLevel.iomHeader, file.data() + 4, 12);
        for (s32 i = 0; i < count && 16 + (size_t)(i + 1) * 0x50 <= file.size(); i++) {
            Placement p;
            const u8* r = file.data() + 16 + (size_t)i * 0x50;
            memcpy(p.m, r, 64);
            memcpy(&p.id, r + 0x40, 4);
            memcpy(p.tail, r + 0x44, 12);
            sLevel.items.push_back(p);
        }
        const size_t end = 16 + sLevel.items.size() * 0x50;
        if (file.size() > end)
            sLevel.iomTrail.assign(file.begin() + (long)end, file.end());
    }
    if (header() && (sPaintType < 0 || !type_by_id(sPaintType)))
        sPaintType = header()->typeCount > 0 ? types()[0].id : -1;
    printf("[editor] map %d: %s, %zu placements (%s), collision %s\n", sMap.index, sMap.modelPath.c_str(), sLevel.items.size(),
           sLevel.iomPath.empty() ? "no placement file" : sLevel.iomPath.c_str(), sMap.collisionPath.c_str());
}

void remember() {
    Snapshot s{sLevel.items, {}, sLevel.iomDirty, sLevel.colDirty};
    if (sMap.collisionFile)
        s.col.assign(sMap.collisionFile, sMap.collisionFile + sMap.collisionSize);
    sUndo.push_back(std::move(s));
    if (sUndo.size() > 50)
        sUndo.pop_front();
}

void undo() {
    if (sUndo.empty()) {
        sStatus = "Nothing to undo.";
        return;
    }
    Snapshot s = std::move(sUndo.back());
    sUndo.pop_back();
    const bool placements = s.items.size() != sLevel.items.size() ||
                            memcmp(s.items.data(), sLevel.items.data(), s.items.size() * sizeof(Placement)) != 0;
    sLevel.items = std::move(s.items);
    if (sMap.collisionFile && s.col.size() == sMap.collisionSize) {
        memcpy(sMap.collisionFile, s.col.data(), s.col.size());
        for (int i = 0; i < point_count(); i++)
            point_moved(i);
    }
    sLevel.colDirty = s.colDirty;
    if (placements)
        Objects_ReloadPlacements(iom_bytes());
    sLevel.iomDirty = s.iomDirty;
    if (sSelected >= (sMode == kPoints ? point_count() : (int)sLevel.items.size()))
        sSelected = -1;
    sStatus = "Undone.";
}

std::string mod_dir() {
    const char* where = getenv("WOTS2_MODS");
    const std::string root = where != nullptr && strcmp(where, "1") != 0 && strcmp(where, "0") != 0 ? where : "mods";
    return root + "/level_edits";
}

bool write_file(const std::string& path, const u8* data, size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

void save() {
    const std::string dir = mod_dir();
    std::string saved;
    bool ok = true;
    if (sLevel.iomDirty && !sLevel.iomPath.empty()) {
        const std::vector<u8> bytes = iom_bytes();
        const std::string path = dir + "/files/" + archive::full_path(sLevel.iomPath);
        if (write_file(path, bytes.data(), bytes.size())) {
            sLevel.iomDirty = false;
            saved += " " + archive::full_path(sLevel.iomPath);
        } else {
            ok = false;
        }
    }
    if (sLevel.colDirty && sMap.collisionFile) {
        const std::string path = dir + "/files/" + archive::full_path(sMap.collisionPath);
        if (write_file(path, sMap.collisionFile, sMap.collisionSize)) {
            sLevel.colDirty = false;
            saved += " " + archive::full_path(sMap.collisionPath);
        } else {
            ok = false;
        }
    }
    if (!std::filesystem::exists(dir + "/mod.ini")) {
        const char ini[] =
            "; Made by the level editor (F2 in the world): the files under files/ take\n"
            "; the place of the game's own.  Delete a file to go back to the original.\n"
            "[mod]\nname = Level edits\ndescription = Maps changed with the level editor\nenabled = 1\n";
        ok = write_file(dir + "/mod.ini", reinterpret_cast<const u8*>(ini), sizeof ini - 1) && ok;
    }
    // (In a run that loaded no mods the overlay is not there yet.)
    archive::add_overlay((dir + "/files").c_str());
    sStatus = !ok ? "Could not write to " + dir + "." : saved.empty() ? "Nothing to save." : "Saved to " + dir + "/files:" + saved;
    printf("[editor] %s\n", sStatus.c_str());
}

void discard() {
    // Back to what the files say (the saved edits, if any, or the disc's).
    std::vector<u8> file;
    if (sMap.collisionFile && archive::read(sMap.collisionPath, file) && file.size() == sMap.collisionSize) {
        remember();
        memcpy(sMap.collisionFile, file.data(), file.size());
        for (int i = 0; i < point_count(); i++)
            point_moved(i);
    }
    const int map = sLevel.map;
    load_level();
    sLevel.map = map;
    Objects_ReloadPlacements(iom_bytes());
    sStatus = "Edits since the last save thrown away.";
}

// ── Objects ────────────────────────────────────────────────────────────────
std::string utf8(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xE0 | c >> 12); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string kind_label(int id) {
    EditorObjectKind k;
    if (!Objects_Kind(id, &k))
        return "#" + std::to_string(id) + " (unknown)";
    std::string model = k.model;
    const size_t slash = model.find_last_of("/\\");
    if (slash != std::string::npos)
        model = model.substr(slash + 1);
    static const char* const kTypes[] = {"crate", "door", "scenery", "pickup", "plain", "dropped",
                                         "held", "dropped", "effect", "sound"};
    char buf[200];
    // (The font has no Japanese: such names are left out.)
    bool ascii = true;
    for (char16_t c : k.name)
        ascii = ascii && c < 0x80;
    snprintf(buf, sizeof buf, "#%d %s  [%s] %s", id, ascii ? utf8(k.name).c_str() : "", k.type < 10 ? kTypes[k.type] : "?",
             model.c_str());
    return buf;
}

u32 kind_colour(int id) {
    EditorObjectKind k;
    if (!Objects_Kind(id, &k))
        return IM_COL32(255, 255, 255, 255);
    switch (k.type) {
    case 0: return IM_COL32(230, 160, 60, 255);     // crates
    case 1: return IM_COL32(120, 200, 255, 255);    // doors
    case 2: return IM_COL32(150, 230, 150, 255);    // scenery
    case 3: return IM_COL32(255, 230, 80, 255);     // pickups
    case 8: return IM_COL32(255, 90, 60, 255);      // effects
    case 9: return IM_COL32(200, 120, 255, 255);    // sounds
    default: return IM_COL32(230, 230, 230, 255);
    }
}

void turn(Placement& p, float angle) {
    const float c = cosf(angle), s = sinf(angle);
    for (int row = 0; row < 3; row++) {
        float* v = p.m + row * 4;
        const float x = v[0] * c + v[2] * s, z = -v[0] * s + v[2] * c;
        v[0] = x;
        v[2] = z;
    }
}
float yaw_of(const Placement& p) { return atan2f(p.m[8], p.m[10]); }

int pick_object() {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int best = -1;
    float bestD = 18.0f * 18.0f;
    for (size_t i = 0; i < sLevel.items.size(); i++) {
        ImVec2 s;
        if (!project(sLevel.items[i].m + 12, &s))
            continue;
        const float d = (s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y);
        if (d < bestD) {
            bestD = d;
            best = (int)i;
        }
    }
    return best;
}

int pick_point() {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int best = -1;
    float bestD = 18.0f * 18.0f;
    for (int i = 0; i < point_count(); i++) {
        ImVec2 s;
        if (!project(point_pos(i), &s))
            continue;
        const float d = (s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void add_at(const float pos[3], int kind) {
    Placement p{};
    p.m[0] = p.m[5] = p.m[10] = p.m[15] = 1.0f;
    EditorObjectKind k;
    const float lift = Objects_Kind(kind, &k) ? k.size[1] * 0.5f : 0.0f;
    p.m[12] = pos[0];
    p.m[13] = pos[1] + lift;
    p.m[14] = pos[2];
    p.id = kind;
    remember();
    sLevel.items.push_back(p);
    sSelected = (int)sLevel.items.size() - 1;
    objects_changed();
}

// ── Each picture ───────────────────────────────────────────────────────────
void fly() {
    ImGuiIO& io = ImGui::GetIO();
    const float dt = io.DeltaTime > 0 && io.DeltaTime < 0.25f ? io.DeltaTime : 1.0f / 30.0f;
    if (!io.WantCaptureMouse && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        sYaw -= io.MouseDelta.x * 0.005f;
        sPitch -= io.MouseDelta.y * 0.005f;
        sPitch = std::clamp(sPitch, -1.55f, 1.55f);
    }
    if (io.WantCaptureKeyboard || io.KeyCtrl)
        return;
    const u8* keys = SDL_GetKeyboardState(nullptr);
    const float speed = sSpeed * (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] ? 4.0f : 1.0f) * dt;
    const float fwd[3] = {-sinf(sYaw) * cosf(sPitch), sinf(sPitch), -cosf(sYaw) * cosf(sPitch)};
    const float right[3] = {cosf(sYaw), 0, -sinf(sYaw)};
    float move[3] = {0, 0, 0};
    const auto add = [&](const float* v, float k) { for (int j = 0; j < 3; j++) move[j] += v[j] * k; };
    if (keys[SDL_SCANCODE_W]) add(fwd, 1);
    if (keys[SDL_SCANCODE_S]) add(fwd, -1);
    if (keys[SDL_SCANCODE_D]) add(right, 1);
    if (keys[SDL_SCANCODE_A]) add(right, -1);
    if (keys[SDL_SCANCODE_E]) move[1] += 1;
    if (keys[SDL_SCANCODE_Q]) move[1] -= 1;
    for (int j = 0; j < 3; j++)
        sEye[j] += move[j] * speed;
}

// Where the mouse's ray first goes below the ground (or y = 0 off the grid).
void find_hit() {
    sHit = false;
    if (!mouse_ray(sRayFrom, sRayDir))
        return;
    float prev[3] = {sRayFrom[0], sRayFrom[1], sRayFrom[2]};
    for (float t = 0.1f; t < 250.0f; t += 0.1f) {
        float p[3];
        for (int j = 0; j < 3; j++)
            p[j] = sRayFrom[j] + sRayDir[j] * t;
        float g;
        if (!ground_at(p[0], p[2], &g))
            g = 0.0f;
        if (p[1] <= g) {
            memcpy(sHitPos, prev, sizeof sHitPos);
            sHitPos[1] = g;
            // (one step's accuracy is enough: 0.1 along the ray)
            sHitPos[0] = p[0];
            sHitPos[2] = p[2];
            sHit = true;
            return;
        }
        memcpy(prev, p, sizeof prev);
    }
}

void paint(int cx, int cz) {
    const ColHeader* h = header();
    if (!h || !type_by_id(sPaintType))
        return;
    const int r0 = -(sBrush - 1) / 2, r1 = sBrush / 2;
    for (int dz = r0; dz <= r1; dz++)
        for (int dx = r0; dx <= r1; dx++) {
            const int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= h->width || z >= h->cellCount / h->width)
                continue;
            s16* c = cell_at(x, z);
            if (c[1] != sPaintType) {
                c[1] = (s16)sPaintType;
                sLevel.colDirty = true;
            }
        }
}

void handle_mouse() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            sDragging = false;
            sStroke = false;
        }
        return;
    }
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!down) {
        sDragging = false;
        sStroke = false;
    }
    if (sMode == kObjects) {
        if (click && sPlacing && sHit) {
            add_at(sHitPos, sAddKind);
            sPlacing = false;
            return;
        }
        if (click) {
            sSelected = pick_object();
            if (sSelected >= 0 && sHit) {
                float g = 0;
                const float* p = sLevel.items[(size_t)sSelected].m + 12;
                if (!ground_at(p[0], p[2], &g))
                    g = 0;
                sDragLift = p[1] - g;
                sDragOffset[0] = p[0] - sHitPos[0];
                sDragOffset[1] = p[2] - sHitPos[2];
                sDragging = true;
                remember();
            }
        } else if (sDragging && down && sHit && sSelected >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            float* p = sLevel.items[(size_t)sSelected].m + 12;
            p[0] = sHitPos[0] + sDragOffset[0];
            p[2] = sHitPos[2] + sDragOffset[1];
            float g;
            p[1] = (ground_at(p[0], p[2], &g) ? g : sHitPos[1]) + sDragLift;
            objects_changed();
        }
    } else if (sMode == kPoints) {
        if (click) {
            sSelected = pick_point();
            sDragging = sSelected >= 0;
            if (sDragging)
                remember();
        } else if (sDragging && down && sHit && sSelected >= 0 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            memcpy(point_pos(sSelected), sHitPos, 12);
            point_moved(sSelected);
        }
    } else if (sMode == kCells && sHit && down) {
        int cx, cz;
        if (!cell_index(sHitPos[0], sHitPos[2], &cx, &cz))
            return;
        if (io.KeyCtrl) {                       // Ctrl + click: take the type under the mouse
            if (click)
                sPaintType = cell_at(cx, cz)[1];
            return;
        }
        if (!sStroke) {
            remember();
            sStroke = true;
        }
        paint(cx, cz);
    }
}

void handle_keys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard)
        return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
        undo();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
        save();
    if (sMode != kObjects || sSelected < 0 || sSelected >= (int)sLevel.items.size())
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        remember();
        sLevel.items.erase(sLevel.items.begin() + sSelected);
        sSelected = -1;
        objects_changed();
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
        remember();
        Placement p = sLevel.items[(size_t)sSelected];
        p.m[12] += 0.5f;
        sLevel.items.push_back(p);
        sSelected = (int)sLevel.items.size() - 1;
        objects_changed();
    } else if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R)) {
        remember();
        turn(sLevel.items[(size_t)sSelected], (io.KeyShift ? -15.0f : 15.0f) * kPi / 180.0f);
        objects_changed();
    }
}

void line3(ImDrawList* dl, const float a[3], const float b[3], u32 colour, float width = 1.5f) {
    ImVec2 sa, sb;
    if (project(a, &sa) && project(b, &sb))
        dl->AddLine(sa, sb, colour, width);
}

void draw_scene() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // Cells round the mouse (or round the point the camera looks at).
    if (sShowCells && sMode == kCells && header()) {
        float centre[3];
        if (sHit) memcpy(centre, sHitPos, sizeof centre);
        else for (int j = 0; j < 3; j++) centre[j] = sEye[j] + sRayDir[j] * 8.0f;
        const ColHeader* h = header();
        const int span = (int)(sCellRadius * 100.0f / h->cellSize);
        int ccx, ccz;
        cell_index(centre[0], centre[2], &ccx, &ccz);
        const int rows = h->cellCount / h->width;
        for (int z = std::max(0, ccz - span); z <= std::min(rows - 1, ccz + span); z++)
            for (int x = std::max(0, ccx - span); x <= std::min(h->width - 1, ccx + span); x++) {
                const ColCellType* t = type_by_id(cell_at(x, z)[1]);
                if (!t)
                    continue;
                u32 colour;
                if (t->flags & 8) colour = IM_COL32(60, 120, 255, 110);           // door
                else if (t->flags & 4) colour = IM_COL32(255, 220, 40, 110);      // exit
                else if (t->flags & kCellBlocked) colour = IM_COL32(255, 50, 50, 90);
                else if (t->flags & 0x80) colour = IM_COL32(200, 80, 255, 100);   // hint spot
                else colour = IM_COL32(60, 230, 90, 60);
                if (t->id == sPaintType && sMode == kCells)
                    colour = (colour & 0x00FFFFFF) | 0xA0000000u;
                float x0, x1, z0, z1;
                cell_span(x, h->offsetX, &x0, &x1);
                cell_span(z, h->offsetZ, &z0, &z1);
                const float y = t->height + 0.02f;
                const float c[4][3] = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
                ImVec2 s[4];
                bool ok = true;
                for (int k = 0; k < 4 && ok; k++)
                    ok = project(c[k], &s[k]);
                if (ok)
                    dl->AddQuadFilled(s[0], s[1], s[2], s[3], colour);
            }
    }
    // The brush.
    if (sMode == kCells && sHit && header()) {
        int cx, cz;
        if (cell_index(sHitPos[0], sHitPos[2], &cx, &cz)) {
            const ColHeader* h = header();
            float x0, x1, z0, z1, a, b;
            cell_span(cx - (sBrush - 1) / 2, h->offsetX, &x0, &a);
            cell_span(cx + sBrush / 2, h->offsetX, &b, &x1);
            cell_span(cz - (sBrush - 1) / 2, h->offsetZ, &z0, &a);
            cell_span(cz + sBrush / 2, h->offsetZ, &b, &z1);
            const float y = sHitPos[1] + 0.03f;
            const float c[4][3] = {{x0, y, z0}, {x1, y, z0}, {x1, y, z1}, {x0, y, z1}};
            for (int k = 0; k < 4; k++)
                line3(dl, c[k], c[(k + 1) % 4], IM_COL32(255, 255, 255, 255), 2.0f);
        }
    }
    // Placements.
    for (size_t i = 0; i < sLevel.items.size(); i++) {
        const Placement& p = sLevel.items[i];
        ImVec2 s;
        if (!project(p.m + 12, &s))
            continue;
        const bool sel = (int)i == sSelected && sMode == kObjects;
        const u32 colour = kind_colour(p.id);
        dl->AddCircleFilled(s, sel ? 6.0f : 4.0f, colour);
        dl->AddCircle(s, sel ? 7.0f : 5.0f, IM_COL32(0, 0, 0, 200));
        if (sel || sMode == kObjects) {
            // which way it faces
            const float tip[3] = {p.m[12] + p.m[8] * 0.6f, p.m[13] + p.m[9] * 0.6f, p.m[14] + p.m[10] * 0.6f};
            line3(dl, p.m + 12, tip, colour);
        }
        if (sel) {
            EditorObjectKind k;
            if (Objects_Kind(p.id, &k)) {
                // its box, about its origin (the sizes are whole extents: a crate's are 1.05 × 0.7 × 0.7
                // and its origin stands 0.36 up)
                const float hx = std::max(k.size[0], 0.1f) * 0.5f, hz = std::max(k.size[2], 0.1f) * 0.5f;
                const float hi = std::max(k.size[1], 0.05f) * 0.5f, lo = -hi;
                float c[8][3];
                for (int n = 0; n < 8; n++) {
                    const float lx = (n & 1) ? hx : -hx, ly = (n & 2) ? hi : lo, lz = (n & 4) ? hz : -hz;
                    for (int j = 0; j < 3; j++)
                        c[n][j] = p.m[12 + j] + lx * p.m[j] + ly * p.m[4 + j] + lz * p.m[8 + j];
                }
                static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (const auto& e : edges)
                    line3(dl, c[e[0]], c[e[1]], IM_COL32(255, 255, 255, 255), 2.0f);
            }
            dl->AddText(ImVec2(s.x + 9, s.y - 7), IM_COL32(255, 255, 255, 255), kind_label(p.id).c_str());
        }
    }
    // Spawn points.
    if (sMode == kPoints || sMode == kObjects)
        for (int i = 0; i < point_count(); i++) {
            ImVec2 s;
            if (!project(point_pos(i), &s))
                continue;
            const bool sel = i == sSelected && sMode == kPoints;
            dl->AddTriangleFilled(ImVec2(s.x, s.y - 9), ImVec2(s.x - 6, s.y + 3), ImVec2(s.x + 6, s.y + 3),
                                  sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(80, 220, 255, 230));
            if (sMode == kPoints) {
                char label[16];
                snprintf(label, sizeof label, "%d", point_name(i));
                dl->AddText(ImVec2(s.x + 8, s.y - 8), IM_COL32(255, 255, 255, 255), label);
            }
        }
    if (sMode == kObjects && sPlacing && sHit) {
        ImVec2 s;
        if (project(sHitPos, &s))
            dl->AddCircle(s, 10.0f, IM_COL32(255, 255, 0, 255), 0, 2.0f);
    }
}

// The camera a few units back from a place, looking down at it.
void look_at(const float* p) {
    sPitch = -0.6f;
    const float back = 5.0f;
    sEye[0] = p[0] + sinf(sYaw) * cosf(sPitch) * back;
    sEye[1] = p[1] - sinf(sPitch) * back;
    sEye[2] = p[2] + cosf(sYaw) * cosf(sPitch) * back;
}

void flag_box(const char* label, u32* flags, u32 bit) {
    bool on = (*flags & bit) != 0;
    if (ImGui::Checkbox(label, &on)) {
        remember();
        *flags = on ? (*flags | bit) : (*flags & ~bit);
        sLevel.colDirty = true;
    }
}

void draw_panel() {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("Level editor (F2)");
    ImGui::Text("Map %d  %s", sMap.index, sMap.modelPath.c_str());
    ImGui::TextDisabled("%s", sMap.collisionPath.c_str());
    ImGui::TextDisabled("%s", sLevel.iomPath.empty() ? "(no placement file)" : sLevel.iomPath.c_str());
    ImGui::Checkbox("Hold the game still", &sPause);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderFloat("speed", &sSpeed, 1.0f, 30.0f, "%.0f");
    if (ImGui::Button(sLevel.iomDirty || sLevel.colDirty ? "Save*" : "Save"))
        save();
    ImGui::SameLine();
    if (ImGui::Button("Undo"))
        undo();
    ImGui::SameLine();
    if (ImGui::Button("Throw away edits"))
        discard();
    if (!sStatus.empty())
        ImGui::TextWrapped("%s", sStatus.c_str());
    ImGui::Separator();

    if (ImGui::BeginTabBar("modes")) {
        if (ImGui::BeginTabItem("Objects")) {
            if (sMode != kObjects) sSelected = -1;
            sMode = kObjects;
            if (sLevel.iomPath.empty()) {
                ImGui::TextWrapped("This map has no placement file, so nothing can be placed on it.");
            } else {
                ImGui::TextWrapped("Click a dot to pick it, drag to move it. R / Shift+R turn, Ctrl+D copy, Delete remove. "
                                   "Effects and sounds of the place (red / purple) take their new places when the map "
                                   "is next entered.");
                // What to add.
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##kind", kind_label(sAddKind).c_str())) {
                    ImGui::InputText("filter", sKindFilter, sizeof sKindFilter);
                    for (int id = 0; id < Objects_KindCount(); id++) {
                        const std::string label = kind_label(id);
                        if (sKindFilter[0] && label.find(sKindFilter) == std::string::npos)
                            continue;
                        if (ImGui::Selectable(label.c_str(), id == sAddKind))
                            sAddKind = id;
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button(sPlacing ? "Click where it goes..." : "Add"))
                    sPlacing = !sPlacing;
                ImGui::SameLine();
                ImGui::Text("%zu placed", sLevel.items.size());

                if (sSelected >= 0 && sSelected < (int)sLevel.items.size()) {
                    Placement& p = sLevel.items[(size_t)sSelected];
                    ImGui::SeparatorText("Selected");
                    ImGui::TextWrapped("%s", kind_label(p.id).c_str());
                    float pos[3] = {p.m[12], p.m[13], p.m[14]};
                    if (ImGui::DragFloat3("place", pos, 0.02f)) {
                        if (ImGui::IsItemActivated()) remember();
                        memcpy(p.m + 12, pos, sizeof pos);
                        objects_changed();
                    }
                    float yaw = yaw_of(p) * 180.0f / kPi;
                    if (ImGui::DragFloat("facing", &yaw, 1.0f, -180.0f, 180.0f, "%.0f°")) {
                        if (ImGui::IsItemActivated()) remember();
                        turn(p, yaw * kPi / 180.0f - yaw_of(p));
                        objects_changed();
                    }
                    if (ImGui::Button("Make it the kind above")) {
                        remember();
                        p.id = sAddKind;
                        objects_changed();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Go there"))
                        look_at(p.m + 12);
                    ImGui::SameLine();
                    if (ImGui::Button("On the ground")) {
                        float g;
                        EditorObjectKind k;
                        if (ground_at(p.m[12], p.m[14], &g)) {
                            remember();
                            p.m[13] = g + (Objects_Kind(p.id, &k) ? k.size[1] * 0.5f : 0.0f);
                            objects_changed();
                        }
                    }
                }
                ImGui::SeparatorText("All (double-click: go there)");
                if (ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                    for (size_t i = 0; i < sLevel.items.size(); i++) {
                        char row[240];
                        snprintf(row, sizeof row, "%3zu  %s", i, kind_label(sLevel.items[i].id).c_str());
                        if (ImGui::Selectable(row, (int)i == sSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                            sSelected = (int)i;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                look_at(sLevel.items[i].m + 12);
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Cells")) {
            if (sMode != kCells) sSelected = -1;
            sMode = kCells;
            if (!header()) {
                ImGui::TextWrapped("This map has no collision file.");
            } else {
                const ColHeader* h = header();
                ImGui::TextWrapped("Left drag paints the type picked below; Ctrl+click takes the type under the mouse. "
                                   "Green walkable, red wall, yellow exit, blue door, purple hint spot.");
                ImGui::Text("%d × %d cells of %.2f", h->width, h->cellCount / std::max(h->width, 1), h->cellSize / 100.0f);
                ImGui::Checkbox("Show cells", &sShowCells);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("around", &sCellRadius, 2.0f, 20.0f, "%.0f");
                ImGui::SetNextItemWidth(120);
                ImGui::SliderInt("brush", &sBrush, 1, 12);
                if (ColCellType* t = type_by_id(sPaintType)) {
                    ImGui::SeparatorText("Type to paint");
                    ImGui::Text("Type %d (flags %#x): changes here apply to every cell of it", t->id, t->flags);
                    flag_box("walkable (1)", &t->flags, 1);
                    ImGui::SameLine();
                    flag_box("wall (2)", &t->flags, 2);
                    ImGui::SameLine();
                    flag_box("exit (4)", &t->flags, 4);
                    flag_box("door (8)", &t->flags, 8);
                    ImGui::SameLine();
                    flag_box("no NPCs (0x10)", &t->flags, 0x10);
                    ImGui::SameLine();
                    flag_box("hint (0x80)", &t->flags, 0x80);
                    int index = t->_0c;
                    ImGui::SetNextItemWidth(120);
                    if (ImGui::InputInt("exit / door / hint number", &index)) {
                        remember();
                        t->_0c = index;
                        sLevel.colDirty = true;
                    }
                    ImGui::SetNextItemWidth(120);
                    if (ImGui::DragFloat("ground height", &t->height, 0.01f)) {
                        if (ImGui::IsItemActivated()) remember();
                        sLevel.colDirty = true;
                    }
                }
                ImGui::SeparatorText("The map's cell types");
                std::vector<int> used((size_t)h->typeCount, 0);
                for (s32 i = 0; i < h->cellCount; i++)
                    for (s32 k = 0; k < h->typeCount; k++)
                        if (cells()[i * 2 + 1] == types()[k].id) {
                            used[(size_t)k]++;
                            break;
                        }
                if (ImGui::BeginChild("types", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                    for (s32 k = 0; k < h->typeCount; k++) {
                        const ColCellType& t = types()[k];
                        char row[160];
                        snprintf(row, sizeof row, "%4d  flags %#5x  #%d  height %.2f   %d cells", t.id, t.flags, t._0c,
                                 t.height, used[(size_t)k]);
                        if (ImGui::Selectable(row, t.id == sPaintType))
                            sPaintType = t.id;
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Points")) {
            if (sMode != kPoints) sSelected = -1;
            sMode = kPoints;
            ImGui::TextWrapped("Where one arrives: each way into the map has a pair: the place (odd number) and a "
                               "point it faces (the next number): 1 / 2 the area's entrance, 3 / 4 exit 0, 5 / 6 exit 1... "
                               "Click one and drag it.");
            if (sSelected >= 0 && sSelected < point_count()) {
                float* p = point_pos(sSelected);
                ImGui::Text("Point %d", point_name(sSelected));
                ImGui::SameLine();
                if (ImGui::Button("Go there"))
                    look_at(p);
                if (ImGui::DragFloat3("place", p, 0.02f)) {
                    if (ImGui::IsItemActivated()) remember();
                    point_moved(sSelected);
                }
            }
            if (ImGui::BeginChild("points", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                for (int i = 0; i < point_count(); i++) {
                    const float* p = point_pos(i);
                    char row[96];
                    snprintf(row, sizeof row, "%4d   %7.2f %7.2f %7.2f", point_name(i), p[0], p[1], p[2]);
                    if (ImGui::Selectable(row, i == sSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        sSelected = i;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            look_at(p);
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::TextDisabled("camera at %.1f %.1f %.1f", sEye[0], sEye[1], sEye[2]);
    if (sHit)
        ImGui::TextDisabled("mouse on the ground at %.2f %.2f %.2f", sHitPos[0], sHitPos[1], sHitPos[2]);
    ImGui::End();
}

void overlay() {
    if (!sOpen)
        return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (Editor_CurrentMap(&sMap)) {
        if (sMap.index != sLevel.map || sMap.collisionFile != sLevel.colFile)
            load_level();
        fly();
        find_hit();
        handle_keys();
        handle_mouse();
        draw_scene();
        draw_panel();
    } else {
        ImGui::Begin("Level editor (F2)");
        ImGui::Text("No map is loaded.");
        ImGui::End();
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void open_editor() {
    EditorMap map;
    if (!Editor_CurrentMap(&map)) {
        printf("[editor] F2: there is no map to edit here (walk into the world first)\n");
        return;
    }
    if (!sReady) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().Alpha = 0.95f;
        ImGui_ImplSDL2_InitForOpenGL(hal::gs::window(), hal::gs::gl_context());
        ImGui_ImplOpenGL3_Init("#version 330 core");
        sReady = true;
    }
    // Start where the game's camera is.
    memcpy(sEye, map.eye, sizeof sEye);
    const float len = sqrtf(map.dir[0] * map.dir[0] + map.dir[1] * map.dir[1] + map.dir[2] * map.dir[2]);
    const float d[3] = {map.dir[0] / (len > 0 ? len : 1), map.dir[1] / (len > 0 ? len : 1), map.dir[2] / (len > 0 ? len : 1)};
    sYaw = atan2f(-d[0], -d[2]);
    sPitch = asinf(std::clamp(d[1], -1.0f, 1.0f));
    printf("[editor] camera at %.2f %.2f %.2f, looking %.2f %.2f %.2f\n", sEye[0], sEye[1], sEye[2], d[0], d[1], d[2]);
    sCamSet = true;
    sOpen = true;
    hal::pad::set_blocked(true);
    hal::gs::set_overlay(overlay);
    SDL_ShowCursor(SDL_ENABLE);
    printf("[editor] open\n");
}

void close_editor() {
    sOpen = false;
    sCamSet = false;
    sDragging = false;
    hal::pad::set_blocked(false);
    hal::gs::set_overlay(nullptr);
    if (sLevel.iomDirty || sLevel.colDirty)
        printf("[editor] closed with edits not saved (they stay until the map is loaded again)\n");
    printf("[editor] closed\n");
}

}  // namespace

namespace editor {

bool event(const SDL_Event& ev) {
    if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0 && ev.key.keysym.sym == SDLK_F2) {
        if (sOpen) close_editor();
        else open_editor();
        return true;
    }
    if (!sOpen)
        return false;
    ImGui_ImplSDL2_ProcessEvent(&ev);
    switch (ev.type) {
    case SDL_QUIT:
        return false;
    case SDL_KEYDOWN:
        if (ev.key.keysym.sym == SDLK_F11)
            return false;
        if (ev.key.keysym.sym == SDLK_ESCAPE && !ImGui::GetIO().WantCaptureKeyboard) {
            close_editor();                     // (Esc closes the editor rather than the game)
            return true;
        }
        return true;
    case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
        return true;
    default:
        return false;
    }
}

}  // namespace editor

bool Editor_Camera(float eye[3], float dir[3], float up[3]) {
    if (!sOpen || !sCamSet)
        return false;
    memcpy(eye, sEye, sizeof sEye);
    dir[0] = -sinf(sYaw) * cosf(sPitch);
    dir[1] = sinf(sPitch);
    dir[2] = -cosf(sYaw) * cosf(sPitch);
    up[0] = 0;
    up[1] = 1;
    up[2] = 0;
    return true;
}

extern "C" int Native_EditorPaused() { return sOpen && sPause ? 1 : 0; }
