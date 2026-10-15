#include "native/sprites.h"
#include "hal/gs.h"
#include "native/texture_pack.h"

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

// PCSX2's hash of a texture (GSTextureCache's HashTextureLevel): XXH3-64
// over the 256-byte GS blocks the texture covers, in its own format, block
// row by block row — the raw bytes, as the upload left them.  `mem` starts
// at the texture's base; `pagesPerRow` is how the decoder above lays out
// pages.  A texture smaller than a block is hashed as its texels instead
// (indices one byte each, 32-bit colour four), row by row.
u64 pack_hash(const std::vector<u8>& mem, int psm, int log2w, int log2h, u32 pagesPerRow,
              const std::vector<u8>& texels) {
    const int w = 1 << log2w, h = 1 << log2h;
    const int bw = psm == 0x14 ? 32 : psm == 0x13 ? 16 : 8, bh = psm == 0x00 ? 8 : 16;
    if (w < bw || h < bh)
        return texture_pack::hash(texels.data(), texels.size());
    std::vector<u8> blocks;
    blocks.reserve((size_t)(w / bw) * (size_t)(h / bh) * 256);
    for (int by = 0; by < h / bh; by++)
        for (int bx = 0; bx < w / bw; bx++) {
            size_t addr;
            if (psm == 0x13)        // 128 × 64 pages
                addr = ((size_t)(by * 16 / 64) * pagesPerRow + (size_t)(bx * 16 / 128)) * 8192 + kBlock32[by % 4][bx % 8] * 256;
            else if (psm == 0x14)   // 128 × 128 pages
                addr = ((size_t)(by * 16 / 128) * pagesPerRow + (size_t)(bx * 32 / 128)) * 8192 + kBlock4[by % 8][bx % 4] * 256;
            else                    // 32-bit: 64 × 32 pages
                addr = ((size_t)(by * 8 / 32) * pagesPerRow + (size_t)(bx * 8 / 64)) * 8192 + kBlock32[by % 4][bx % 8] * 256;
            for (size_t i = 0; i < 256; i++)
                blocks.push_back(addr + i < mem.size() ? mem[addr + i] : 0);
        }
    return texture_pack::hash(blocks.data(), blocks.size());
}

// A cut-out texture (every texel either clear or opaque) is given the
// colour of its opaque neighbours in the clear texels next to them.  Drawn
// with smooth filtering, an edge pixel mixes the two; the colour hidden under
// the clear texels is whatever the artist left there (the grass of the
// magistrate's yard: a paler green), which showed as a dotted fringe.  Only
// pixels that mix in a clear texel change.
void bleed_cutout(std::vector<u8>& rgba, int w, int h) {
    const size_t n = (size_t)w * h;
    size_t clear = 0;
    for (size_t i = 0; i < n; i++) {
        u8 a = rgba[i * 4 + 3];
        if (a != 0 && a != 255) return;
        clear += a == 0;
    }
    if (clear == 0 || clear == n) return;
    std::vector<u8> filled(n), next;
    for (size_t i = 0; i < n; i++) filled[i] = rgba[i * 4 + 3] != 0;
    for (int pass = 0; pass < 4; pass++) {
        next = filled;
        bool any = false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const size_t i = (size_t)y * w + x;
                if (filled[i]) continue;
                u32 sum[3] = {0, 0, 0}, k = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int xx = x + dx, yy = y + dy;
                        if ((dx | dy) == 0 || xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        const size_t j = (size_t)yy * w + xx;
                        if (!filled[j]) continue;
                        for (int c = 0; c < 3; c++) sum[c] += rgba[j * 4 + c];
                        k++;
                    }
                if (!k) continue;
                for (int c = 0; c < 3; c++) rgba[i * 4 + c] = (u8)(sum[c] / k);
                next[i] = 1;
                any = true;
            }
        filled.swap(next);
        if (!any) break;
    }
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
        // Pages per row when the texture is *read*: its own buffer width
        // (TEX0's TBW, +0x06, in 64 texels) over the indexed page's 128.  It
        // need not be the upload's: a 4-bit 128 × 256 texture is uploaded as
        // a 32-bit image two pages wide but read one page wide, two down —
        // taking the upload's width put its lower half on a page that is not
        // there (black wall panels, scrambled clothes).
        const u32 tbw = rd16(d + 6);

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

        const u32 readPpr = (t.psm == 0x13 || t.psm == 0x14) && tbw >= 2 && tbw * 64 >= (u32)t.width
                          ? tbw / 2 : uploadBw;
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
            if (texture_pack::active() && memBw && pixels + pixSize <= &blob[0] + blob.size()) {
                std::vector<u8> whole = gs_memory_from_ct32(pixels, pixSize, memBw, w);
                std::vector<u8> mem(whole.begin() + (memStart < whole.size() ? memStart : whole.size()), whole.end());
                t.packHash = pack_hash(mem, 0x00, rd16(d + 8), rd16(d + 10), memBw, t.direct);
                t.packBits = (u32)rd16(d + 8) << 6 | (u32)rd16(d + 10) << 10;
                t.packHashed = true;
            }
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
                        u32 addr = ((y / 64) * readPpr + x / 128) * 8192
                                 + kBlock32[(y / 16) % 4][(x / 16) % 8] * 256
                                 + column_index(x % 16, y % 16, 4);
                        if (addr < mem.size()) value = mem[addr];
                    } else if (t.psm == 0x14) {
                        // 4-bit: 128×128 pages, 32×16 blocks, addressed in nibbles
                        u32 nib = ((y / 128) * readPpr + x / 128) * 16384
                                + kBlock4[(y / 16) % 8][(x / 32) % 4] * 512
                                + column_index(x % 32, y % 16, 8);
                        if (nib / 2 < mem.size())
                            value = (nib & 1) ? mem[nib / 2] >> 4 : mem[nib / 2] & 0x0F;
                    }
                    t.indices[(size_t)y * t.width + x] = value;
                }
            }
            if (texture_pack::active()) {
                t.packHash = pack_hash(mem, t.psm, rd16(d + 8), rd16(d + 10), readPpr, t.indices);
                t.packBits = (u32)t.psm | (u32)rd16(d + 8) << 6 | (u32)rd16(d + 10) << 10;
                t.packHashed = true;
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
    // (Cached by palette and by whether the texture pack is on, so that
    // switching it — F9 — finds both kinds ready.)
    const int packOn = texture_pack::enabled() ? 0x10000 : 0;
    if (!t.direct.empty()) {
        auto it = t.glTextures.find(packOn);
        if (it != t.glTextures.end())
            return it->second;
        if (t.packHashed)
            if (u32 tex = texture_pack::find(t.packHash, 0, t.packBits))
                return t.glTextures[packOn] = tex;
        std::vector<u8> rgba = t.direct;
        for (size_t i = 3; i < rgba.size(); i += 4) {
            u32 a = rgba[i] * 255u / 128u;          // GS alpha: 0x80 is opaque
            rgba[i] = (u8)(a > 255 ? 255 : a);
        }
        bleed_cutout(rgba, t.width, t.height);
        return t.glTextures[packOn] = hal::gs::create_texture(rgba.data(), t.width, t.height);
    }
    if (t.indices.empty())
        return 0;

    const bool eightBit = t.psm == 0x13 || t.psm == 0x1B;
    const int clut = eightBit ? rect.clutOffset / 4 : rect.clutOffset;
    const int key = clut | packOn;
    auto it = t.glTextures.find(key);
    if (it != t.glTextures.end())
        return it->second;

    const size_t entries = eightBit ? 256 : 16;
    const size_t clutBytes = entries * 4;
    if ((size_t)(clut + 1) * clutBytes > t.clutRaw.size()) {
        fprintf(stderr, "[sprites] palette %d out of range\n", clut);
        return t.glTextures[key] = 0;
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
    }
    // A texture pack's replacement: named by the palette as the GS holds it
    // (index order, its own alpha).
    if (t.packHashed && t.psm != 0x1B)
        if (u32 tex = texture_pack::find(t.packHash, texture_pack::hash(palette, entries * 4), t.packBits)) {
            // A cut-out in the game is one in the pack too.
            bool used[256] = {}, clear = false, opaque = false, between = false;
            for (u8 i : t.indices) used[i & (entries - 1)] = true;
            for (size_t i = 0; i < entries; i++)
                if (used[i]) {
                    clear |= palette[i][3] == 0;
                    opaque |= palette[i][3] >= 0x80;
                    between |= palette[i][3] != 0 && palette[i][3] < 0x80;
                }
            hal::gs::set_cutout(tex, clear && opaque && !between);
            return t.glTextures[key] = tex;
        }
    for (size_t i = 0; i < entries; i++) {
        u32 a = palette[i][3] * 255u / 128u;       // GS alpha: 0x80 is opaque
        palette[i][3] = (u8)(a > 255 ? 255 : a);
    }

    std::vector<u8> rgba((size_t)t.width * t.height * 4);
    for (size_t i = 0; i < t.indices.size(); i++)
        memcpy(&rgba[i * 4], palette[t.indices[i] & (entries - 1)], 4);
    bleed_cutout(rgba, t.width, t.height);
    u32 tex = hal::gs::create_texture(rgba.data(), t.width, t.height);
    t.glTextures[key] = tex;
    return tex;
}

void Sheet::release() {
    for (Texture& t : textures) {
        for (auto& [clut, tex] : t.glTextures)
            if (tex && !texture_pack::owns(tex)) hal::gs::destroy_texture(tex);
        t.glTextures.clear();
    }
}

}  // namespace sprites
