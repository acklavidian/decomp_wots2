// The eye-catch: the twelve seconds of animation that open a new day, before
// the area map (the HUD's slot +0x44, which older notes took for a message
// window; 0x29BD90–0x29FE90, widget 0x28 bytes, vtable 0x2ED350; sprites
// asset 0x6E, ui/GameUI/EyeCatch.Bpm; tune 2).
//
// A pond: a dark red gradient whose brightness ripples where four drops fall,
// the shadows of two umbrellas crossing it, four petals drifting down and settling, a
// lotus that opens once a black band has closed over the picture; then a
// fade to black.  Start skips to the end over a second.
//
// The widget keeps a list of *parts*, each a small object with update(time
// in thirtieths of a second) → 0 when it is done with, and draw(sprites).
// Here the parts are one host-side record (the original allocates fifteen
// little classes, vtables 0x2ED370–0x2ED590, and steps them through the
// list); what each does and in what order is as in the original.
//
// The pond is the PS2 texturing from its own frame buffer: a 64 × 48 grid of
// 10-pixel squares re-draws the picture so far, each corner's brightness
// being the height of a simulated water surface (0x29EFA0 / 0x29F620).

#include "engine/bgm.h"
#include "engine/containers.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "game/hud.h"
#include "native/guest.h"

#include <cstring>
#include <memory>

struct SpriteSet;

extern "C" {
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    void  GsDevice_DrawTexturedPrim(void* dev, const void* packet, int count);  // 0x12FAF0
    void  GsDevice_SetBlend(void* dev, u64 alpha);                      // 0x130C50
    SpriteSet* SpriteSet_Ctor(void* mem);                               // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    int   SpriteSet_Bind(SpriteSet* set);                               // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);                     // 0x17C570
    void  SpriteSet_DrawRotated(SpriteSet* set, int index, const void* params);        // 0x179A40
    void* SpriteSet_GetSprite(SpriteSet* set, int index);               // 0x17BDF0
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);

const u32 kZ = 0xFFFFF0u;
enum : int { kGridW = 65, kGridH = 49 };

inline s32 Left()   { return (s32)(Screen_X(0, -1) & 0xFFFF); }
inline s32 Right()  { return (s32)(Screen_X(0, 1) & 0xFFFF); }
inline s32 Top()    { return (s32)(Screen_Y(0, -1) & 0xFFFF); }
inline s32 Bottom() { return (s32)(Screen_Y(0, 1) & 0xFFFF); }
inline s32 MidX()   { return (s32)(Screen_X(0, 0) & 0xFFFF); }
inline s32 MidY()   { return (s32)(Screen_Y(0, 0) & 0xFFFF); }
inline float Frames(float n) { return (float)*gFrameRate * n / 30.0f; }
inline u32 Grey(float alpha) { return (u32)(s32)alpha << 24 | 0x808080; }

// 0x17BED0: a sprite of a set by its eight-character name.
int FindSprite(SpriteSet* set, u32 name) {
    for (int i = 0; i < 16; i++) {
        const void* record = SpriteSet_GetSprite(set, i);
        if (record == nullptr)
            break;
        if (memcmp(record, P(name), 8) == 0)
            return i;
    }
    return -1;
}
// A flat-shaded strip of points {x, y, colour}.
struct Point { s32 x, y; u32 rgba; };
void DrawFlat(u64 prim, const Point* points, int count) {
    struct { u64 prim; struct { u32 rgba, q; u16 x, y; u32 z; } v[6]; } packet;
    memset(&packet, 0, sizeof packet);
    packet.prim = prim;
    for (int i = 0; i < count; i++) {
        packet.v[i].rgba = points[i].rgba;
        packet.v[i].x = (u16)points[i].x;
        packet.v[i].y = (u16)points[i].y;
        packet.v[i].z = kZ;
    }
    GsDevice_DrawPrim(*gGsDevice, &packet, count);
}
void DrawRotated(SpriteSet* set, u32 name, u32 flags, s32 x, s32 y, u32 rgba, float scale, float angle) {
    struct { u32 flags; s32 x, y; u32 z; u32 rgba; float scaleX, scaleY, angle; } q = {flags, x, y, kZ, rgba, scale, scale, angle};
    SpriteSet_DrawRotated(set, FindSprite(set, name), &q);
}
// 0x181C60: a point on the smooth curve through four points (p[1] → p[2]).
struct Pt { s32 x, y; };
Pt Curve(float t, const Pt* p) {
    const float t2 = t * t, t3 = t2 * t;
    const float a = t + (t3 - t2 * 2.0f), b = t3 * -2.0f + t2 * 3.0f, c = (t3 * 2.0f - t2 * 3.0f) + 1.0f, d = t3 - t2;
    const auto axis = [&](int p0, int p1, int p2, int p3) {
        const int in = (int)((float)((p1 - p0) + (p2 - p1)) * 0.5f);
        const int out = (int)((float)((p3 - p2) + (p2 - p1)) * 0.5f);
        return (int)((float)p2 * b) + (int)((float)in * a) + (int)((float)p1 * c) + (int)((float)out * d);
    };
    return Pt{axis(p[0].x, p[1].x, p[2].x, p[3].x), axis(p[0].y, p[1].y, p[2].y, p[3].y)};
}

// A petal, or an umbrella's shadow.
struct Mover {
    bool alive = true;
    s32 x = 0, y = 0;
    float angle = 0.0f, scale = 1.0f;
    u32 rgba = 0x80808080u;
};
// A petal's way: three stretches of one curve through six points.
void AlongPath(Mover* m, const Pt* path, float t, float t1, float t2, float d0, float d1, float d2) {
    Pt at;
    if (t < t1)
        at = Curve(t / d0, path);
    else if (t < t2)
        at = Curve((t - t1) / d1, path + 1);
    else
        at = Curve((t - t2) / d2, path + 2);
    m->x = at.x + Left();
    m->y = at.y + Top();
}

struct Parts {
    // The pond's surface and how fast each point of it is moving.
    float height[kGridW * kGridH] = {};
    float speed[kGridW * kGridH] = {};
    bool drop[4] = {true, true, true, true};
    Mover hat[2];                   // 0x2ED4D0, 0x2ED4B0  the umbrellas ("kasa")
    Mover petal[4];                 // 0x2ED470, 0x2ED450, 0x2ED430, 0x2ED410
    bool band = true;               // 0x2ED3D0  the black band that closes from the middle
    u32 bandEdge = 0, bandMiddle = 0;
    u32 bud = 0;                    // 0x2ED3B0  "hasu_1"
    u32 bloom = 0;                  // 0x2ED390  "hasu_2"
    bool cover = true;              // 0x2ED370  a black patch over the lotus that lifts
    u32 coverBottom = 0, coverTop = 0;
    u32 fade = 0;                   // 0x2ED570  the whole picture to black
    bool skipping = false;          // 0x2ED550  … and the same when skipped
    s32 skipEnd = 0;
    u32 skipFade = 0;
};

// 0x29EFA0: four drops, and a frame of the surface.
void Pond_Update(Parts* p, float t) {
    static const struct { float at; int cell; } kDrops[4] = {
        {43.0f, 21 * kGridW + 23}, {64.0f, 31 * kGridW + 22}, {76.0f, 12 * kGridW + 43}, {91.0f, 33 * kGridW + 34}};
    for (int i = 0; i < 4; i++) {
        if (!(kDrops[i].at < t) || !p->drop[i])
            continue;
        p->drop[i] = false;
        float* h = &p->height[kDrops[i].cell];
        h[0] += 0.05f;
        h[1] += 0.025f;
        h[-1] += 0.025f;
        h[kGridW] += 0.025f;
        h[-kGridW] += 0.025f;
        h[kGridW + 1] += 0.0125f;
        h[-kGridW + 1] += 0.0125f;
        h[kGridW - 1] += 0.0125f;
        h[-kGridW - 1] += 0.0125f;
    }
    for (int x = 1; x < kGridW - 1; x++)
        for (int y = 1; y < kGridH - 1; y++) {
            const float* h = &p->height[x + y * kGridW];
            p->speed[x + y * kGridW] -= h[0] - (h[-kGridW] + h[kGridW] + h[1] + h[-1]) * 0.2f
                                      - (h[-kGridW - 1] + h[-kGridW + 1] + h[kGridW + 1] + h[kGridW - 1]) * 0.05f;
        }
    for (int x = 1; x < kGridW - 1; x++)
        for (int y = 1; y < kGridH - 1; y++) {
            const int i = x + y * kGridW;
            p->height[i] += p->speed[i] * 0.5f;
            p->speed[i] *= 0.99f;
        }
}
// 0x29F620: the picture so far, redrawn through the surface.
void Pond_Draw(const Parts* p) {
    GsDevice* dev = *gGsDevice;
    // TEX0 naming the frame buffer being drawn to: 32-bit, as wide as the
    // screen (the original: its address from GsDevice+0x170 / +0x80 by which
    // buffer is showing; the native layer knows it by the format and width).
    const u64 tex0 = (u64)(*P<s32>(0x2EE4C0u) >> 6) << 14 | 0x268100000ull;
    const auto shade = [p](int i) {
        float v = p->height[i] * 4096.0f + 128.0f;
        // (The original lets values past 255 spill into the next channel;
        // they are clamped here.)
        const u32 c = v < 0.0f ? 0u : v > 255.0f ? 255u : (u32)v;
        return c | c << 8 | c << 16 | 0x80000000u;
    };
    GsDevice_Begin2D(dev);
    const s32 left = Left(), top = Top();
    for (int row = 0; row < kGridH - 1; row++)
        for (int col = 0; col < kGridW - 1; col++) {
            struct { u64 tex0, prim; struct { s16 u, v; u32 _4; u32 rgba; u32 q; u16 x, y; u32 z; } v[4]; } packet;
            memset(&packet, 0, sizeof packet);
            packet.tex0 = tex0;
            packet.prim = 0x15C;
            const s32 u = col * 0xA0, v = row * 0xA0;
            const s32 us[4] = {u, u + 0xA0, u, u + 0xA0}, vs[4] = {v, v, v + 0xA0, v + 0xA0};
            const int cells[4] = {col + row * kGridW, col + 1 + row * kGridW, col + (row + 1) * kGridW, col + 1 + (row + 1) * kGridW};
            for (int k = 0; k < 4; k++) {
                packet.v[k].u = (s16)us[k];
                packet.v[k].v = (s16)vs[k];
                packet.v[k].rgba = shade(cells[k]);
                packet.v[k].x = (u16)(left + us[k]);
                packet.v[k].y = (u16)(top + vs[k]);
                packet.v[k].z = kZ;
            }
            GsDevice_DrawTexturedPrim(dev, &packet, 4);
        }
    GsDevice_End2D(dev);
}

// The parts' updates, in the order of the list.
void Parts_Update(Parts* p, float t) {
    Pond_Update(p, t);
    // 0x29ED10 / 0x29E990: the umbrellas' shadows slide across, turning.
    if (Mover* m = &p->hat[0]; m->alive) {
        const float k = *P<float>(0x2EE3FCu);
        m->x = 0xDC0 - (s32)((float)(s32)(t * 3104.0f) * k) + Left();
        m->y = 0xDC0 - (s32)((float)(s32)(t * 5632.0f) * k) + Top();
        m->angle = t * 0.008695652f * 100.0f;
        if (70.0f < t && t < 110.0f)
            m->rgba = Grey(128.0f - (t - 70.0f) * 128.0f / 40.0f);
        m->alive = t <= 115.0f;
    }
    if (Mover* m = &p->hat[1]; m->alive) {
        const float k = *P<float>(0x2EE400u);
        m->x = 0x2300 - (s32)((float)(s32)(t * -640.0f) * k) + Left();
        m->y = 0xC80 - (s32)((float)(s32)(t * -3904.0f) * k) + Top();
        if (55.0f < t && t < 95.0f)
            m->rgba = Grey(128.0f - (t - 55.0f) * 128.0f / 40.0f);
        m->angle = t * 0.01f * -90.0f;
        m->alive = t <= 100.0f;
    }
    // 0x29E5D0
    if (Mover* m = &p->petal[0]; m->alive) {
        static const Pt kPath[6] = {{0xEE0, 0x1C80}, {0xEE0, 0x1C80}, {0xCA0, 0x14F0}, {0xEA0, 0xD70}, {0x1020, 0xBD0}, {0x1020, 0xBD0}};
        AlongPath(m, kPath, t, 28.0f, 44.0f, 28.0f, 16.0f, 31.0f);
        m->scale = t < 27.0f ? 1.2f - t / 27.0f * 0.7f : 0.5f;
        if (t < 44.0f)
            m->angle = 90.0f - t * 200.0f / 44.0f;
        else if (t < 75.0f)
            m->angle = -110.0f - (t - 44.0f) * 20.0f / 31.0f;
        if (44.0f < t)
            m->rgba = t < 68.0f ? Grey(128.0f - (t - 44.0f) * 128.0f / 24.0f) : 0;
        m->alive = t <= 75.0f;
    }
    // 0x29E160
    if (Mover* m = &p->petal[1]; m->alive) {
        if (t < 12.0f) {
            m->rgba = 0;
        } else {
            static const Pt kPath[6] = {{0x14C0, -0x880}, {0x14C0, -0x880}, {0x1F70, 0xB70}, {0xFB0, 0x12C0}, {0xD60, 0x1270}, {0xD60, 0x1270}};
            const float u = t - 12.0f;
            m->rgba = 0x80808080u;
            AlongPath(m, kPath, u, 30.0f, 49.0f, 30.0f, 19.0f, 31.0f);
            if (u < 24.0f)
                m->scale = 2.5f - u / 24.0f;
            else if (u < 38.0f)
                m->scale = 1.5f - (u - 24.0f) * 0.5f / 14.0f;
            else if (u < 49.0f)
                m->scale = 1.0f - (u - 38.0f) * 0.5f / 11.0f;
            if (u < 49.0f)
                m->angle = -(u * 300.0f / 49.0f);
            else if (u < 75.0f)
                m->angle = -300.0f - (u - 49.0f) * 20.0f / 31.0f;
            if (49.0f < u)
                m->rgba = u < 80.0f ? Grey(128.0f - (u - 49.0f) * 128.0f / 31.0f) : 0;
            m->alive = u <= 79.0f;
        }
    }
    // 0x29DD10
    if (Mover* m = &p->petal[2]; m->alive) {
        if (t < 12.0f) {
            m->rgba = 0;
        } else {
            static const Pt kPath[6] = {{-0x640, 0xD20}, {-0x640, 0xD20}, {0xCA0, 0x480}, {0x1A30, 0x750}, {0x1C60, 0x8D0}, {0x1C60, 0x8D0}};
            m->rgba = 0x80808080u;
            AlongPath(m, kPath, t - 12.0f, 40.0f, 65.0f, 40.0f, 25.0f, 31.0f);
            if (t < 52.0f)
                m->scale = 2.5f - (t - 12.0f) / 40.0f * 1.5f;
            else if (t < 77.0f)
                m->scale = 1.0f - (t - 52.0f) / 25.0f * 0.5f;
            else
                m->scale = 0.5f;
            m->angle = t < 77.0f ? (t - 12.0f) / 65.0f * 300.0f : (t - 77.0f) / 31.0f * 20.0f + 300.0f;
            if (77.0f < t)
                m->rgba = t < 108.0f ? Grey(128.0f - (t - 77.0f) * 128.0f / 31.0f) : 0;
            m->alive = t <= 108.0f;
        }
    }
    // 0x29D810
    if (Mover* m = &p->petal[3]; m->alive) {
        if (t < 53.0f) {
            m->rgba = 0;
        } else {
            static const Pt kPath[6] = {{0x2F20, 0xDA0}, {0x2F20, 0xDA0}, {0x2810, 0x1090}, {0x17D0, 0x14A0}, {0x1540, 0x14A0}, {0x1540, 0x14A0}};
            m->rgba = 0x80808080u;
            AlongPath(m, kPath, t - 53.0f, 11.0f, 36.0f, 11.0f, 25.0f, 31.0f);
            if (t < 68.0f)
                m->scale = 2.5f - (t - 53.0f) / 15.0f * 0.9f;
            else if (t < 89.0f)
                m->scale = 1.6f - (t - 68.0f) / 21.0f * 1.1f;
            else
                m->scale = 0.5f;
            if (t < 89.0f)
                m->angle = -((t - 53.0f) * 300.0f / 36.0f);
            else if (t < 120.0f)
                m->angle = -300.0f - (t - 89.0f) * 20.0f / 31.0f;
            else
                m->angle = -320.0f;
            if (89.0f < t)
                m->rgba = t < 115.0f ? Grey(128.0f - (t - 89.0f) * 128.0f / 26.0f) : 0;
            m->alive = t <= 120.0f;
        }
    }
    // 0x29D2C0: black closes over the picture from the middle.
    if (p->band) {
        if (t < 110.0f) {
            p->bandEdge = p->bandMiddle = 0;
        } else if (t < 135.0f) {
            const u32 v = (u32)((t - 110.0f) * 192.0f / 25.0f);
            p->bandEdge = (v < 0x41 ? 0 : v - 0x40) << 24;
            p->bandMiddle = (v > 0x80 ? 0x80 : v) << 24;
        } else if (t < 400.0f) {
            p->bandEdge = p->bandMiddle = 0x80000000u;
        } else {
            p->band = false;
        }
    }
    // 0x29D130 / 0x29CF10: the lotus.
    p->bud = t < 135.0f ? 0 : 0x80808080u;
    p->bloom = t < 215.0f ? 0 : t < 290.0f ? Grey((t - 215.0f) * 128.0f / 75.0f) : 0x80808080u;
    // 0x29CBE0: the patch over it lifts from the bottom up.
    if (p->cover) {
        if (t < 135.0f) {
            p->coverBottom = p->coverTop = 0;
        } else if (t < 155.0f) {
            p->coverBottom = p->coverTop = 0x80000000u;
        } else if (t < 215.0f) {
            const u32 v = (u32)((t - 155.0f) * 192.0f / 60.0f);
            const u32 top = v < 0x41 ? 0 : v - 0x40, bottom = v > 0x80 ? 0x80 : v;
            p->coverBottom = (0x80 - bottom) << 24;
            p->coverTop = (0x80 - top) << 24;
        } else {
            p->cover = false;
        }
    }
    // 0x29FE90: out of black, and back into it at the end.
    if (t < 15.0f)
        p->fade = (u32)(s32)(128.0f - t * 128.0f / 15.0f) << 24;
    else if (t < 290.0f)
        p->fade = 0;
    else if (t < 360.0f)
        p->fade = (u32)(s32)((t - 290.0f) * 128.0f / 70.0f) << 24;
    else
        p->fade = 0x80000000u;
    // 0x29FC80: the fade of a skip, over a second.
    if (p->skipping) {
        if (t < (float)p->skipEnd) {
            const float second = Frames(30.0f);
            p->skipFade = (u32)(s32)((second - ((float)p->skipEnd - t)) * 128.0f / second) << 24;
        } else {
            p->skipFade = 0x80000000u;
        }
    }
}
void FullScreen(u32 rgba) {                                     // 0x29FD30
    if (rgba == 0)
        return;
    const Point quad[4] = {{Left(), Top(), rgba}, {Right(), Top(), rgba}, {Left(), Bottom(), rgba}, {Right(), Bottom(), rgba}};
    DrawFlat(0x14C, quad, 4);
}
void Parts_Draw(const Parts* p, SpriteSet* set) {
    // 0x29FAF0: the pond's ground, near black to red, between the bars.
    {
        const s32 top = Top() + 0x3F0, bottom = Bottom() - 0x3F0;
        const Point quad[4] = {{Left(), top, 0x8006052Eu}, {Right(), top, 0x8006052Eu},
                               {Left(), bottom, 0x801A167Cu}, {Right(), bottom, 0x801A167Cu}};
        DrawFlat(0x14C, quad, 4);
    }
    Pond_Draw(p);
    for (const Mover& m : p->hat)                               // 0x29EBC0: subtracted from the picture
        if (m.alive) {
            GsDevice_SetBlend(*gGsDevice, 0x42);
            DrawRotated(set, 0x2E8588u, 0x11, m.x, m.y, m.rgba, 16.0f, m.angle);
            GsDevice_SetBlend(*gGsDevice, 0x8000000044ull);
        }
    for (const Mover& m : p->petal)                             // 0x29DC10
        if (m.alive)
            DrawRotated(set, 0x2E8578u, 1, m.x, m.y, m.rgba, m.scale * 16.0f, m.angle);
    // 0x29D640: the bars above and below (two flat sprites).
    {
        const Point top[2] = {{Left(), Top(), 0x80000000u}, {Right(), Top() + 0x400, 0x80000000u}};
        DrawFlat(0x14E, top, 2);
        const Point bottom[2] = {{Left(), Bottom(), 0x80000000u}, {Right(), Bottom() - 0x400, 0x80000000u}};
        DrawFlat(0x14E, bottom, 2);
    }
    if (p->band) {                                              // 0x29D420
        const Point strip[6] = {{Left(), Top(), p->bandEdge}, {Right(), Top(), p->bandEdge},
                                {Left(), MidY(), p->bandMiddle}, {Right(), MidY(), p->bandMiddle},
                                {Left(), Bottom(), p->bandEdge}, {Right(), Bottom(), p->bandEdge}};
        DrawFlat(0x14C, strip, 6);
    }
    if (p->bud != 0)                                            // 0x29D170
        DrawRotated(set, 0x2E8568u, 1, MidX() + 0x10, Top() + 0xF80, p->bud, 16.0f, 0.0f);
    if (p->bloom != 0)                                          // 0x29CFF0
        DrawRotated(set, 0x2E8558u, 1, MidX(), Top() + 0xE10, p->bloom, 16.0f, 0.0f);
    if (p->cover) {                                             // 0x29CD30
        const s32 x0 = Left() + 0x1300, x1 = Left() + 0x14F0, y0 = Top() + 0xC90, y1 = Top() + 0x1270;
        const Point quad[4] = {{x0, y0, p->coverTop}, {x1, y0, p->coverTop}, {x0, y1, p->coverBottom}, {x1, y1, p->coverBottom}};
        DrawFlat(0x14C, quad, 4);
    }
    FullScreen(p->fade);
    if (p->skipping)
        FullScreen(p->skipFade);
}

// ── The widget ────────────────────────────────────────────────────────────
struct EyeCatch {
    s32 kind;               // +0x00
    u32 vtable;             // +0x04
    s32 time;               // +0x08  frames; twelve seconds in all
    s32 state;              // +0x0C  0 over, 1 loading, 2 a pause before the tune, 3 running, 4 skipping
    s32 delay;              // +0x10
    s32 skip;               // +0x14
    u32 sprites;            // +0x18
    GuestList parts;        // +0x1C  (the original's list of part objects; unused here)
};
static_assert(sizeof(EyeCatch) == 0x28, "EyeCatch");
std::unique_ptr<Parts> sParts;      // of the one eye-catch there can be

inline s32 Length() { return (s32)(Frames(30.0f) * 12.0f); }

int EyeCatch_Show(EyeCatch*, int, int) { return 1; }            // 0x29C2E0
int EyeCatch_Update(EyeCatch* self) {                           // 0x29BF00
    if (self->state == 4) {
        if (self->skip == 0) {
            self->time = Length();
            return 1;
        }
        self->skip--;
    } else if (self->state != 0 && (Pad_GetPressed(PadMgr_Get(), 0) & kPadStart) != 0 &&
               (float)self->time < Frames(290.0f)) {
        self->state = 4;
        self->skip = (s32)Frames(30.0f);
        if (sParts) {
            sParts->skipping = true;
            sParts->skipEnd = (s32)(Frames(30.0f) + (float)self->time);
        }
        Bgm_StopAll(0);
    }
    if (self->state == 0) {
        Bgm_StopAll(0);
        return 0;
    }
    if (self->state != 1) {
        if (sParts)
            Parts_Update(sParts.get(), (float)self->time * 30.0f / (float)*gFrameRate);
        if (self->state != 2 && ++self->time > Length())
            self->time = Length();
    }
    return 1;
}
int EyeCatch_Draw(EyeCatch* self, SpriteSet* hudSprites) {      // 0x29BDF0
    SpriteSet* set = P<SpriteSet>(self->sprites);
    if (SpriteSet_Bind(set) == 0)
        return 1;
    if (self->state == 1) {
        self->state = 2;
        Bgm_StopAll(0);
        return 1;
    }
    if (self->state == 2) {
        if (self->delay == 0) {
            Bgm_Play(2, 0, 0, 0x3FFF);
            self->state = 3;
        } else {
            self->delay--;
        }
    }
    if (sParts)
        Parts_Draw(sParts.get(), set);
    SpriteSet_Bind(hudSprites);
    return 1;
}
EyeCatch* EyeCatch_Dtor(EyeCatch* self, s16 flag) {             // 0x29C2F0
    if (self == nullptr)
        return nullptr;
    sParts.reset();
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    self->vtable = 0x2EC410u;                                   // the widget base's
    if (flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x29C2F0, EyeCatch_Dtor);
GUEST_BIND(0x29C2E0, EyeCatch_Show);
GUEST_BIND(0x29BF00, EyeCatch_Update);
GUEST_BIND(0x29BDF0, EyeCatch_Draw);

EyeCatch* EyeCatch_Ctor(void* mem) {                            // 0x29C430
    EyeCatch* self = static_cast<EyeCatch*>(mem);
    memset(self, 0, sizeof *self);
    self->vtable = 0x2ED350u;
    GuestList_Init(&self->parts);
    self->state = 1;
    self->delay = 0x1E;
    void* set = MemAlloc(0x14);
    self->sprites = set != nullptr ? guest::addr(SpriteSet_Ctor(set)) : 0;
    SpriteSet_Load(P<SpriteSet>(self->sprites), 0x6E, -1);
    sParts = std::make_unique<Parts>();
    for (Mover& m : sParts->petal) {            // (they start in the middle of the screen)
        m.x = MidX();
        m.y = MidY();
    }
    return self;
}

}  // namespace

extern "C" {

// ── 0x21C990: the eye-catch begins ────────────────────────────────────────
void Hud_OpenMessage(void* mem) {
    Hud* hud = static_cast<Hud*>(mem);
    if (hud->message == 0) {
        void* widget = MemAlloc(sizeof(EyeCatch));
        hud->message = widget != nullptr ? guest::addr(EyeCatch_Ctor(widget)) : 0;
        if (hud->message == 0)
            return;
        GuestList_Insert(&hud->widgets, GuestList_End(&hud->widgets), hud->message);
    }
    VirtualCall<int, void*, int, int>(P<EyeCatch>(hud->message)->vtable, 0x0C, P(hud->message), 0, 0);
}
// ── 0x21C900 (→ 0x29BDA0): it is still playing ────────────────────────────
int Hud_MessageBusy(void* mem) {
    const Hud* hud = static_cast<const Hud*>(mem);
    return hud->message != 0 && (float)P<EyeCatch>(hud->message)->time < Frames(30.0f) * 12.0f;
}
// ── 0x21C950 (→ 0x29BD90): over — the HUD frees it on its next update ─────
void Hud_CloseMessage(void* mem) {
    Hud* hud = static_cast<Hud*>(mem);
    if (hud->message != 0) {
        P<EyeCatch>(hud->message)->state = 0;
        hud->message = 0;
    }
}

}  // extern "C"
