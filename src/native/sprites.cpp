#include "native/sprites.h"
#include "hal/gs.h"

#include <cstdio>
#include <algorithm>
#include <cstring>

namespace {

// GS local-memory layout tables.  A page is 8192 bytes; 32-bit pixels are
// stored in 8×8 blocks, 8-bit in 16×16 blocks, 4-bit in 32×16 blocks.
const u8 kBlock32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
const u8 kColumn32[8][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
const u8 kBlock4[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};

// Position of pixel (x, y) inside its block, counted in pixels of that format.
// `unit` is 4 for 8-bit (16-wide blocks) and 8 for 4-bit (32-wide blocks).
u32 column_index(u32 x, u32 y, u32 unit) {
    u32 column = y / 4, row = y % 4;
    u32 group = (x % 8) / 2;
    if ((row >= 2) != (column % 2 == 1))
        group = (group + 2) % 4;
    return column * unit * 16 + group * unit * 4 + (x % 2) * unit + (row % 2) * unit * 2
         + (x / 8) * 2 + row / 2;
}

u16 rd16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
u32 rd32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

// The game uploads indexed pixel data as a 32-bit image and then samples it
// in the indexed format, so the file's bytes are in 32-bit block order.
// Rebuild GS memory as that upload leaves it.
// The image is `w` pixels wide — half the width of the indexed texture it
// becomes, which for small textures is less than the buffer width.
std::vector<u8> gs_memory_from_ct32(const u8* data, size_t size, u32 uploadBw, u32 w) {
    if (w == 0) w = uploadBw * 64;
    u32 h = (u32)(size / 4 / w);
    // Whole pages: a pixel's address depends on its page and block, not on
    // how many bytes came before it (an image 96 × 40 in a buffer two pages
    // wide reaches into the second row of pages).
    const size_t pages = ((size_t)(h + 31) / 32) * (uploadBw ? uploadBw : 1);
    std::vector<u8> mem(std::max((size_t)w * h * 4 + 8192, pages * 8192), 0);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u32 addr = ((y / 32) * uploadBw + x / 64) * 8192
                     + kBlock32[(y / 8) % 4][(x / 8) % 8] * 256 + kColumn32[y % 8][x % 8] * 4;
            if (addr + 4 <= mem.size())
                memcpy(&mem[addr], data + ((size_t)y * w + x) * 4, 4);
        }
    }
    return mem;
}

}  // namespace

namespace sprites {

bool Sheet::load(const std::vector<u8>& blob) {
    rects.clear();
    textures.clear();
    if (blob.size() < 0x40)
        return false;

    size_t base = 0;
    if (memcmp(&blob[8], "BPMFile", 7) == 0) {
        u32 count = rd32(&blob[4]);
        if (0x10 + (size_t)count * 0x20 > blob.size())
            return false;
        for (u32 i = 0; i < count; i++) {
            const u8* r = &blob[0x10 + i * 0x20];
            rects.push_back({(s16)rd16(r + 8), (s16)rd16(r + 10), (s16)rd16(r + 12),
                             (s16)rd16(r + 14), r[0x18], r[0x19]});
        }
        base = 0x10 + (size_t)count * 0x20;
    }

    // Texture descriptors; their data offsets are relative to the first one.
    const size_t origin = base;
    const u8* prevPixels = nullptr;
    size_t prevSize = 0;
    u32 prevBw = 0, prevW = 0, prevMemBw = 0;
    size_t prevStart = 0;
    for (;;) {
        if (base + 0x30 > blob.size())
            return false;
        const u8* d = &blob[base];
        Texture t;
        t.psm = d[2];
        t.width = 1 << rd16(d + 8);
        t.height = 1 << rd16(d + 10);
        u32 pixSize = rd32(d + 0x14), pixOff = rd32(d + 0x18);
        u32 clutSize = rd32(d + 0x20), clutOff = rd32(d + 0x24);
        u32 remaining = rd16(d + 0x28);
        u32 uploadBw = rd16(d + 0x2E);
        // Width of the 32-bit image that is uploaded: normally half the
        // texture's width, less when the picture does not fill the texture.
        u32 uploadW = rd16(d + 0x10);

        const u8* pixels = &blob[0] + origin + pixOff;
        // The buffer width the pixels were uploaded with, and where in that
        // upload this texture starts.
        u32 memBw = uploadBw;
        size_t memStart = 0;
        if (pixSize == 0) {
            // No pixels of its own: it lies inside the previous descriptor's
            // upload, from GS block +0x2A on (256 bytes each), and is read in
            // its own format and buffer width.
            pixels = prevPixels;
            pixSize = (u32)prevSize;
            memBw = prevMemBw;
            memStart = prevStart + (size_t)rd16(d + 0x2A) * 256;
            if (!uploadBw) uploadBw = prevBw;
            uploadW = prevW;
        }
        // The size field can overstate what is there (4-bit sheets), so clamp.
        if (origin + clutOff < blob.size()) {
            size_t avail = blob.size() - (origin + clutOff);
            size_t take = clutSize < avail ? clutSize : avail;
            t.clutRaw.assign(&blob[origin + clutOff], &blob[origin + clutOff] + take);
        }

        if (pixels && t.psm == 0x00) {
            // 32-bit colour, no palette: the bytes are the pixels.
            u32 w = uploadW ? uploadW : (u32)t.width;
            u32 h = (u32)(pixSize / 4 / (w ? w : 1));
            t.indices.clear();
            t.direct.assign((size_t)t.width * t.height * 4, 0);
            for (u32 y = 0; y < h && y < (u32)t.height; y++)
                if (pixels + ((size_t)y + 1) * w * 4 <= &blob[0] + blob.size())
                    memcpy(&t.direct[(size_t)y * t.width * 4], pixels + (size_t)y * w * 4,
                           (size_t)(w < (u32)t.width ? w : (u32)t.width) * 4);
        } else if (pixels && t.psm == 0x1B) {
            // 8-bit indices in the high byte of 32-bit pixels: uploaded in
            // that format, so the file holds them in plain row order.
            size_t count = (size_t)t.width * t.height;
            if (pixels + count <= &blob[0] + blob.size())
                t.indices.assign(pixels, pixels + count);
        } else if (pixels && t.psm == 0x13 && uploadW && uploadW * rd16(d + 0x12) == pixSize
                   && pixels + pixSize <= &blob[0] + blob.size()) {
            // Uploaded as it is: a rectangle of 8-bit pixels in row order.
            const u32 uploadH = rd16(d + 0x12);
            t.indices.assign((size_t)t.width * t.height, 0);
            for (u32 y = 0; y < uploadH && y < (u32)t.height; y++)
                memcpy(&t.indices[(size_t)y * t.width], pixels + (size_t)y * uploadW,
                       uploadW < (u32)t.width ? uploadW : (u32)t.width);
        } else if (pixels && pixels + pixSize <= &blob[0] + blob.size() && uploadBw) {
            std::vector<u8> whole = gs_memory_from_ct32(pixels, pixSize, memBw, uploadW ? uploadW : (u32)t.width / 2);
            std::vector<u8> mem(whole.begin() + (memStart < whole.size() ? memStart : whole.size()), whole.end());
            t.indices.resize((size_t)t.width * t.height);
            for (int y = 0; y < t.height; y++) {
                for (int x = 0; x < t.width; x++) {
                    u8 value = 0;
                    if (t.psm == 0x13) {
                        // 8-bit: 128×64 pages, 16×16 blocks (same block order as 32-bit)
                        u32 addr = ((y / 64) * uploadBw + x / 128) * 8192
                                 + kBlock32[(y / 16) % 4][(x / 16) % 8] * 256
                                 + column_index(x % 16, y % 16, 4);
                        if (addr < mem.size()) value = mem[addr];
                    } else if (t.psm == 0x14) {
                        // 4-bit: 128×128 pages, 32×16 blocks, addressed in nibbles
                        u32 nib = ((y / 128) * uploadBw + x / 128) * 16384
                                + kBlock4[(y / 16) % 8][(x / 32) % 4] * 512
                                + column_index(x % 32, y % 16, 8);
                        if (nib / 2 < mem.size())
                            value = (nib & 1) ? mem[nib / 2] >> 4 : mem[nib / 2] & 0x0F;
                    }
                    t.indices[(size_t)y * t.width + x] = value;
                }
            }
        } else if (pixels && t.psm == 0x13 && pixSize >= (u32)(t.width * t.height)) {
            t.indices.assign(pixels, pixels + (size_t)t.width * t.height);
        }

        prevPixels = pixels;
        prevSize = pixSize;
        prevBw = uploadBw;
        prevW = uploadW;
        prevMemBw = memBw;
        prevStart = memStart;
        textures.push_back(std::move(t));
        if (remaining < 2)
            break;
        base += 0x30;
    }
    return true;
}

u32 Sheet::texture_for(const Rect& rect) {
    if (rect.texture >= textures.size())
        return 0;
    Texture& t = textures[rect.texture];
    if (!t.direct.empty()) {
        auto it = t.glTextures.find(0);
        if (it != t.glTextures.end())
            return it->second;
        std::vector<u8> rgba = t.direct;
        for (size_t i = 3; i < rgba.size(); i += 4) {
            u32 a = rgba[i] * 255u / 128u;          // GS alpha: 0x80 is opaque
            rgba[i] = (u8)(a > 255 ? 255 : a);
        }
        return t.glTextures[0] = hal::gs::create_texture(rgba.data(), t.width, t.height);
    }
    if (t.indices.empty())
        return 0;

    const bool eightBit = t.psm == 0x13 || t.psm == 0x1B;
    const int clut = eightBit ? rect.clutOffset / 4 : rect.clutOffset;
    auto it = t.glTextures.find(clut);
    if (it != t.glTextures.end())
        return it->second;

    const size_t entries = eightBit ? 256 : 16;
    const size_t clutBytes = entries * 4;
    if ((size_t)(clut + 1) * clutBytes > t.clutRaw.size()) {
        fprintf(stderr, "[sprites] palette %d out of range\n", clut);
        return t.glTextures[clut] = 0;
    }
    u8 palette[256][4];
    const u8* raw = &t.clutRaw[(size_t)clut * clutBytes];
    for (size_t i = 0; i < entries; i++) {
        // 8-bit palettes use the GS CSM1 order: entries 8–15 and 16–23 of
        // every 32 are swapped.
        size_t src = i;
        if (eightBit) {
            if ((i & 0x18) == 0x08) src = i + 8;
            else if ((i & 0x18) == 0x10) src = i - 8;
        }
        memcpy(palette[i], raw + src * 4, 4);
        u32 a = palette[i][3] * 255u / 128u;       // GS alpha: 0x80 is opaque
        palette[i][3] = (u8)(a > 255 ? 255 : a);
    }

    std::vector<u8> rgba((size_t)t.width * t.height * 4);
    for (size_t i = 0; i < t.indices.size(); i++)
        memcpy(&rgba[i * 4], palette[t.indices[i] & (entries - 1)], 4);
    u32 tex = hal::gs::create_texture(rgba.data(), t.width, t.height);
    t.glTextures[clut] = tex;
    return tex;
}

void Sheet::release() {
    for (Texture& t : textures) {
        for (auto& [clut, tex] : t.glTextures)
            if (tex) hal::gs::destroy_texture(tex);
        t.glTextures.clear();
    }
}

}  // namespace sprites
