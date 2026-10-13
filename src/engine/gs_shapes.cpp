// Flat rectangles in layout coordinates — decompiled from 0x13F370 and 0x13F6D0.

#include "engine/gs_device.h"
#include "native/guest.h"

#include <cstring>

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};

// ── 0x13F6D0 (0x13FA80 is the same routine again) ─────────────────────────
void Gs_FillRect2(const FrameRect* rect) {
    GsDevice* dev = *gGsDevice;
    if (!dev)
        return;
    struct {
        u64 prim;
        struct { u32 rgba; float q; u16 x, y; u32 z; } v[4];
    } packet;
    memset(&packet, 0, sizeof(packet));
    packet.prim = ((u64)rect->blend & 0x3FFFFFF) << 6 | 5;      // triangle fan, optionally blended
    const s32 corner[4][2] = {{rect->x0, rect->y0}, {rect->x1, rect->y0},
                              {rect->x1, rect->y1}, {rect->x0, rect->y1}};
    for (int i = 0; i < 4; i++) {
        packet.v[i].rgba = rect->rgba;
        packet.v[i].q = 1.0f;
        packet.v[i].x = (u16)Screen_XAligned(corner[i][0] << 4);
        packet.v[i].y = (u16)Screen_XAligned(corner[i][1] * -16);   // the x conversion, as in the original
        packet.v[i].z = rect->z;
    }
    GsDevice_DrawPrim(dev, &packet, 4);
}

// ── 0x13F370 ──────────────────────────────────────────────────────────────
// Four bars centred on the rectangle's edges.
void Gs_DrawFrameRect(const FrameRect* rect, int thickness) {
    const int inner = thickness >> 1;
    const int outer = inner + (thickness & 1);
    FrameRect bar = *rect;

    bar.x0 = rect->x0 - inner; bar.y0 = rect->y0; bar.x1 = rect->x0 + outer; bar.y1 = rect->y1;   // left
    Gs_FillRect2(&bar);
    bar.x0 = rect->x0; bar.y0 = rect->y0 + inner; bar.x1 = rect->x1; bar.y1 = rect->y0 - outer;   // top
    Gs_FillRect2(&bar);
    bar.x0 = rect->x1 - outer; bar.y0 = rect->y0; bar.x1 = rect->x1 + inner; bar.y1 = rect->y1;   // right
    Gs_FillRect2(&bar);
    bar.x0 = rect->x0; bar.y0 = rect->y1 + outer; bar.x1 = rect->x1; bar.y1 = rect->y1 - inner;   // bottom
    Gs_FillRect2(&bar);
}
