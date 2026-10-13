#pragma once
#include "hal/ps2_types.h"
#include <map>
#include <vector>

// Decoder for .bpm sprite sheets — see tools/bpm.py for the format notes.
namespace sprites {

struct Rect {
    s16 u, v, w, h;
    u8  clutOffset;
    u8  texture;
};

struct Texture {
    int width = 0, height = 0;
    int psm = 0;                 // 0x13 = 8-bit indexed, 0x14 = 4-bit, 0x1B = 8-bit in the high byte, 0 = 32-bit colour
    std::vector<u8> indices;     // width * height palette indices
    std::vector<u8> direct;      // or, for 32-bit textures (psm 0), RGBA pixels
    std::vector<u8> clutRaw;     // palettes as stored in the file
    std::map<int, u32> glTextures;   // palette number → GL texture
};

struct Sheet {
    std::vector<Rect> rects;
    std::vector<Texture> textures;

    bool load(const std::vector<u8>& blob);
    // GL texture for a sprite (created on first use); 0 if it cannot be built.
    u32 texture_for(const Rect& rect);
    void release();
};

}  // namespace sprites
