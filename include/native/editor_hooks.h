#pragma once
#include "hal/ps2_types.h"

#include <string>
#include <vector>

// What the level editor (native/editor.cpp) asks of the game's side.  Native
// additions, not functions of the original.

// ── src/game/world_objects.cpp ──────────────────────────────────────────────
struct EditorObjectKind {
    std::string model;
    std::u16string name;
    float size[3];          // its box, whole extents (its origin is at the middle)
    u8 type, a, b, param;   // type: 0 crate, 1 door, 2 scenery, 3 pickup, 4 plain, 5 / 7 dropped,
                            // 6 held in scenes, 8 an effect of the place, 9 a sound of the place
};
int  Objects_KindCount();
bool Objects_Kind(int id, EditorObjectKind* out);
// The map's placement file (object/ObjectIom.Lst), or "" if it has none.
std::string Objects_PlacementFile(int map);
// The map's objects made again from (edited) placement file contents.
void Objects_ReloadPlacements(const std::vector<u8>& file);

// ── src/native/hle_gameplay.cpp ─────────────────────────────────────────────
struct EditorMap {
    int index = -1;                 // in Common/map/list
    std::string modelPath;
    std::string collisionPath;      // the .col
    void* collision = nullptr;      // Collision* (decompiled), or null
    u8*   collisionFile = nullptr;  // the file in guest memory (the Collision points into it)
    u32   collisionSize = 0;
    float viewProj[16];             // of the frame drawn last (row vectors, before the widescreen narrowing)
    float eye[3];                   // where the camera was
    float dir[3];                   // and the way it looked
};
bool Editor_CurrentMap(EditorMap* out);

// ── src/native/editor.cpp ───────────────────────────────────────────────────
// While the editor flies its own camera: its eye, the way it looks and up.
bool Editor_Camera(float eye[3], float dir[3], float up[3]);
