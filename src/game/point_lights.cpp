// Point lights — decompiled from the lighting object's list (0x165D30,
// 0x165FD0, 0x166040, 0x166200, 0x1663E0, 0x1664A0, 0x166B80; the object is
// `*0x2EE678`, called "Camera" in older notes, with the list at +0x180).
//
// Two kinds.  A map's own (flags 6: permanent), from the file
// Common/Light/LightFile.lst (asset 0x89: "LgPs", u32 count, then a name per
// map id, "NOP" = none) names — `Common/light/Amakaze_Night.Lst` and the
// like: 16 bytes of header with the count at +4, then {float pos[4],
// colour[4]} each — the lanterns of a few night maps.  And an effect's
// (flags 9: fading), for a few frames: the red of a blood spray, the white
// of a blow that lands, a parried blow, the ring of a hit.
//
// Something drawn at a place takes at most one of each: the nearest effect
// light within 2 units replaces the map's second light, the nearest map
// light within √6 its third, each growing fainter with the distance.
//
// The rest of the lighting object is still native (native/model.cpp), and
// nothing else reads the list, so it is kept on the host side.

#include "native/archive.h"
#include "native/guest.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct PointLight {
    float pos[4];
    float colour[4];
    u32   flags;        // 1 an effect's, 2 | 4 the map's (kept), 8 fades as it runs out
    s32   left, total;
};
std::vector<PointLight> sLights;
u32 sSwitches = 0;      // +0x198: bit 0 = the map's lights count
s32 sPendingMap = -1;   // +0x194: the map whose file is still to be read
std::vector<std::string> sFiles;
bool sFilesRead = false;

void ReadFileList() {                                   // 0x166D00 / 0x166BF0
    sFilesRead = true;
    std::vector<u8> list;
    if (!archive::read_asset(0x89, list) || list.size() < 8)
        return;
    u32 count;
    memcpy(&count, &list[4], 4);
    size_t at = 8;
    for (u32 i = 0; i < count && at < list.size(); i++) {
        const char* name = reinterpret_cast<const char*>(&list[at]);
        const size_t length = strnlen(name, list.size() - at);
        sFiles.emplace_back(name, length);
        at += length + 1;
    }
}
void LoadMapLights(const std::string& path) {           // 0x166200
    std::vector<u8> file;
    if (!archive::read(path.c_str(), file) || file.size() < 0x10)
        return;
    u32 count;
    memcpy(&count, &file[4], 4);
    for (u32 i = 0; i < count && 0x10 + (size_t)(i + 1) * 0x20 <= file.size(); i++) {
        PointLight light;
        memcpy(light.pos, &file[0x10 + (size_t)i * 0x20], 16);
        memcpy(light.colour, &file[0x20 + (size_t)i * 0x20], 16);
        light.flags = 6;
        light.left = light.total = 10;
        sLights.push_back(light);
    }
}

}  // namespace

extern "C" {

// ── 0x1663E0: the map's lights go (an effect's run out by themselves) ─────
void Camera_Reset(void*) {
    for (size_t i = 0; i < sLights.size();)
        if ((sLights[i].flags & 1) == 0)
            sLights.erase(sLights.begin() + (long)i);
        else
            i++;
}
// ── 0x1664A0 ──────────────────────────────────────────────────────────────
void Camera_SetMap(void*, int map) {
    sSwitches = 0xF;
    sPendingMap = map;
}
// ── 0x165C60 (the scripts' Effect.PointLightEnable) ───────────────────────
void Lights_EnableMap(int on) { sSwitches = on ? (sSwitches | 1) : (sSwitches & ~1u); }

// ── 0x166040: once a frame ────────────────────────────────────────────────
void Camera_Finish(void*) {
    if (!sFilesRead)
        ReadFileList();
    if (sPendingMap != -1) {
        if (sPendingMap >= 0 && (size_t)sPendingMap < sFiles.size()) {
            const std::string& name = sFiles[(size_t)sPendingMap];
            if (name.size() >= 3 && name.compare(name.size() - 3, 3, "Lst") == 0)
                LoadMapLights(name);
        }
        sPendingMap = -1;
    }
    for (size_t i = 0; i < sLights.size();) {
        PointLight& light = sLights[i];
        if ((light.flags & 4) == 0) {
            if (light.left == 0) {
                sLights.erase(sLights.begin() + (long)i);
                continue;
            }
            light.left--;
        }
        i++;
    }
}

// ── 0x165FD0 + 0x166B80: a light for `frames` frames ──────────────────────
void Lights_Add(const float* pos, const float* colour, u32 flags, s32 frames) {
    PointLight light;
    memcpy(light.pos, pos, 16);
    memcpy(light.colour, colour, 16);
    light.flags = flags;
    light.left = light.total = frames;
    sLights.push_back(light);
}

// ── 0x165D30 ──────────────────────────────────────────────────────────────
// What the point lights do to something at `pos`: `dirs` and `colours` are
// the three lights it is lit by (rows of four).  → bit 1 / 2 set when the
// second / third was replaced.
int Lights_At(const float* pos, float dirs[3][4], float colours[3][4]) {
    float nearMap = 6.0f, nearEffect = 4.0f;
    const PointLight* map = nullptr;
    const PointLight* effect = nullptr;
    float toMap[3] = {}, toEffect[3] = {};
    for (const PointLight& light : sLights) {
        const float d[3] = {light.pos[0] - pos[0], light.pos[1] - pos[1], light.pos[2] - pos[2]};
        if (fabsf(d[0]) > 6.0f || fabsf(d[1]) > 6.0f || fabsf(d[2]) > 6.0f)
            continue;
        const float square = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if ((light.flags & 1) == 0) {
            if ((sSwitches & 1) != 0 && square < nearMap) {
                nearMap = square;
                map = &light;
                memcpy(toMap, d, sizeof d);
            }
        } else if (square < nearEffect) {
            nearEffect = square;
            effect = &light;
            memcpy(toEffect, d, sizeof d);
        }
    }
    const auto put = [](const PointLight* light, const float* to, float fade, float* dir, float* colour) {
        const float length = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        for (int k = 0; k < 3; k++)
            dir[k] = length != 0.0f ? to[k] / length : 0.0f;
        const float strength = (light->flags & 8) ? (float)light->left / (float)light->total : 1.0f;
        for (int k = 0; k < 3; k++)
            colour[k] = light->colour[k] * strength * fade;
    };
    int changed = 0;
    if (map != nullptr) {
        put(map, toMap, 1.0f - nearMap * nearMap * 0.027777778f, dirs[2], colours[2]);
        changed |= 4;
    }
    if (effect != nullptr) {
        put(effect, toEffect, 1.0f - nearEffect * nearEffect * 0.0625f, dirs[1], colours[1]);
        changed |= 2;
    }
    return changed;
}

// Native, for the modern lighting (hal::gs::set_world): the lights that
// shine now, n × {x, y, z, radius, r, g, b, 0} — the map's while they are
// switched on, an effect's fading with its frames.
int Lights_Gather(float* out, int max) {
    int n = 0;
    for (const PointLight& light : sLights) {
        if (n >= max)
            break;
        const bool effect = (light.flags & 1) != 0;
        if (!effect && (sSwitches & 1) == 0)
            continue;
        const float strength = (light.flags & 8) && light.total > 0 ? (float)light.left / (float)light.total : 1.0f;
        float* o = out + (size_t)n * 8;
        memcpy(o, light.pos, 3 * sizeof(float));
        o[3] = effect ? 3.0f : 5.0f;
        for (int k = 0; k < 3; k++)
            o[4 + k] = light.colour[k] * strength;
        o[7] = 0.0f;
        n++;
    }
    return n;
}

}  // extern "C"
