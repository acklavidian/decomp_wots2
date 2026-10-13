#pragma once
#include "hal/ps2_types.h"

// The parts of the display device (GsDevice, *0x2EE670) that 2D code draws
// through.  The device itself is not decompiled; these are implemented by the
// native runtime (src/native/hle.cpp).

struct GsDevice;

// A textured sprite as the device takes it: the GS TEX0 and PRIM register
// values, then UV / RGBAQ / XYZ2 for the two corners.
struct GsSpritePacket {
    u64 tex0;
    u64 prim;           // 0x156 = sprite, textured, alpha-blended, UV coordinates
    struct Corner {
        s16 u, v;       // texels, 12.4 fixed
        u32 _uvHi;
        u32 rgba;       // 0x80 = unmodified
        u32 q;
        u16 x, y;       // 1/16 pixel, offset by the GS primitive origin
        u32 z;
    } corner[2];
};

// A rectangle in layout coordinates (pixels from the screen centre, y up).
struct FrameRect {
    s32 x0, y0, x1, y1;
    u32 z;
    u32 blend;          // non-zero: alpha-blended
    u32 rgba;
};

extern "C" {
    void  Gs_FillRect2(const FrameRect* rect);                              // 0x13F6D0  filled
    void  Gs_DrawFrameRect(const FrameRect* rect, int thickness);           // 0x13F370  outline

    void  GsDevice_BeginFrame(GsDevice* dev);                               // 0x131470
    void  GsDevice_EndFrame(GsDevice* dev);                                 // 0x131430
    int   sceGsSyncPath(int mode, u32 timeout);                             // 0x104BD0
    // Sends every texture and palette of a relocated .Pim to GS memory.
    int   GsDevice_UploadTextures(GsDevice* dev, void* pim);                // 0x1306D0
    void  GsDevice_DrawSprite(GsDevice* dev, const GsSpritePacket* packet); // 0x12FD30
    // Untextured geometry: the PRIM value, then RGBAQ + XYZ2 per vertex.
    void  GsDevice_DrawPrim(GsDevice* dev, const void* packet, int vertexCount);  // 0x12FC30
    // Bracket a run of 2D drawing (depth test off / back on).
    void  GsDevice_Begin2D(GsDevice* dev);                                  // 0x130D10
    void  GsDevice_End2D(GsDevice* dev);                                    // 0x130DC0
    void* Pim_Relocate(void* pim);                                          // 0x132AA0

    // v is in 1/16 pixel; align −1 / 0 / 1 measures from the left (top) edge,
    // the centre, or the right (bottom) edge.
    u32   Screen_X(int v, int align);                                       // 0x12E5E0
    u32   Screen_Y(int v, int align);                                       // 0x12E550
    // The same, aligned as the globals at 0x2EE4D0 / 0x2EE4D4 say (normally centre).
    u32   Screen_XAligned(int v);                                           // 0x12E4C0
    u32   Screen_YAligned(int v);                                           // 0x12E430
}
