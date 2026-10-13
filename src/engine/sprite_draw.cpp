// SpriteSet_DrawEx (0x17ADF0): the general way to draw a sprite of a set —
// stretched to a size, flipped, or tiled to fill a rectangle.
//
// params: {flags, x, y, z, w, h, rgba}; x / y / w / h in 1/16 pixel, w / h
// −1 = the sprite's own.  flags: low nibble 0 / 1 stretch, 2 tile across,
// 3 tile down, 4 tile both ways; 0x10 use the sprite's whole last texel (the
// default stops at its centre, so a one-texel sprite stretches to a flat
// colour); 0x20 / 0x40 flip.

#include "hal/ps2_types.h"
#include "native/guest.h"

#include <cstring>
#include <utility>

struct GsDevice;
extern "C" {
    void* AssetRef_GetData(void* ref);                                              // 0x174A00
    void  GsDevice_DrawTexturedPrim(GsDevice* dev, const void* packet, int count);  // 0x12FAF0
}

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};

struct DrawExParams { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; };
struct SpriteVertex { s16 u, v; u32 _04; u32 rgba; u32 _0c; s16 x, y; u32 z; };    // UV, RGBAQ, XYZ2
struct SpritePacket { u64 tex0, prim; SpriteVertex v[1]; };

static inline u16 U16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }

extern "C" int SpriteSet_DrawEx(void* set, int index, void* params) {
    u32* handle = static_cast<u32*>(set);
    DrawExParams* p = static_cast<DrawExParams*>(params);
    u32 desc = handle[2];
    if (desc == 0)
        return 0;
    if (handle[3] == 0) {
        // Not looked at yet: find the sprite records in the file.
        if (handle[1] == 0)
            return 0;
        const s32* file = static_cast<const s32*>(AssetRef_GetData(guest::ptr(handle[1])));
        if (file == nullptr)
            return 0;
        handle[4] = (u32)file[1];
        handle[3] = guest::addr(file + 4);
    }
    if (index < 0 || (s32)handle[4] < index)
        return 0;
    const u8* sprite = guest::ptr<const u8>(handle[3] + (u32)index * 0x20);
    if (sprite[0x19] != 0)
        desc += (u32)sprite[0x19] * 0x30;
    const u8* d = guest::ptr<const u8>(desc);
    const u64 tex0 = (u64)U16(d + 10) << 30 | (u64)U16(d + 8) << 26 | (u64)d[2] << 20 | (u64)U16(d + 4)
                   | (u64)U16(d + 6) << 14 | 0x400000000ull
                   | (u64)(s64)(s32)((u32)U16(d + 0x1E) + (u32)sprite[0x18]) << 37 | 0x2000000000000000ull;

    const u32 flags = p->flags;
    const s16 inset = (flags & 0x10) ? 0 : -0x10;
    const s16 w16 = (s16)(U16(sprite + 0xC) * 0x10), h16 = (s16)(U16(sprite + 0xE) * 0x10);
    s16 u0 = (s16)(U16(sprite + 8) * 0x10 + 8), v0 = (s16)(U16(sprite + 10) * 0x10 + 8);
    s16 u1 = (s16)(u0 + w16 + inset), v1 = (s16)(v0 + h16 + inset);
    const u16 tileW = (u16)(w16 + inset), tileH = (u16)(h16 + inset);
    if (flags & 0x20)
        std::swap(u0, u1);
    if (flags & 0x40)
        std::swap(v0, v1);

    SpritePacket* packet = guest::ptr<SpritePacket>(0x70000000u);      // the scratchpad
    packet->tex0 = tex0;
    packet->prim = 0x15E;
    SpriteVertex* out = packet->v;
    auto put = [&](s16 ua, s16 va, s16 xa, s16 ya, s16 ub, s16 vb, s16 xb, s16 yb) {
        out[0].u = ua; out[0].v = va; out[0].rgba = p->rgba; out[0].x = xa; out[0].y = ya; out[0].z = p->z;
        out[1].u = ub; out[1].v = vb; out[1].rgba = p->rgba; out[1].x = xb; out[1].y = yb; out[1].z = p->z;
        out += 2;
    };
    if (p->h < 0)
        p->h = (s32)U16(sprite + 0xE) << 4;
    if (p->w < 0)
        p->w = (s32)U16(sprite + 0xC) << 4;

    switch (flags & 0xF) {
    case 2: {                                   // tiled across
        const s32 step = tileW;
        u16 x = (u16)p->x;
        const s16 y = (s16)p->y, bottom = (s16)(y + (s16)p->h);
        for (s32 n = p->w / step; n != 0; n--) {
            put(u0, v0, (s16)x, y, u1, v1, (s16)(x + step), bottom);
            x = (u16)(x + step);
        }
        const s16 rest = (s16)(p->w % step);
        if (rest != 0)
            put(u0, v0, (s16)x, y, (s16)(u0 + rest), v1, (s16)(x + rest), bottom);
        break;
    }
    case 3: {                                   // tiled down
        const s32 step = tileH;
        u16 y = (u16)p->y;
        const s16 x = (s16)p->x, right = (s16)(x + (s16)p->w);
        for (s32 n = p->h / step; n != 0; n--) {
            put(u0, v0, x, (s16)y, u1, v1, right, (s16)(y + step));
            y = (u16)(y + step);
        }
        const s16 rest = (s16)(p->h % step);
        if (rest != 0)
            put(u0, v0, x, (s16)y, u1, (s16)(v0 + rest), right, (s16)(y + rest));
        break;
    }
    case 4: {                                   // tiled both ways
        const u32 height = (u16)p->h, width = (u16)p->w;
        for (u32 yy = 0; yy < height; yy = (yy + tileH) & 0xFFFF) {
            for (u32 xx = 0; xx < width; xx = (xx + tileW) & 0xFFFF) {
                const u16 cw = width < xx + tileW ? (u16)(width - xx) : tileW;
                const u16 ch = height < yy + tileH ? (u16)(height - yy) : tileH;
                put(u0, v0, (s16)(p->x + xx), (s16)(p->y + yy),
                    (s16)(u0 + cw), (s16)(v0 + ch), (s16)(p->x + xx + cw), (s16)(p->y + yy + ch));
            }
        }
        break;
    }
    default:
        put(u0, v0, (s16)p->x, (s16)p->y, u1, v1, (s16)(p->x + p->w), (s16)(p->y + p->h));
        break;
    }
    GsDevice_DrawTexturedPrim(*gGsDevice, packet, (int)(out - packet->v));
    return 1;
}
