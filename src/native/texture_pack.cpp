// Texture packs made for PCSX2 — see include/native/texture_pack.h.
#include "native/texture_pack.h"
#include "hal/gs.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
#include <vector>

namespace texture_pack {
namespace {

using Key = std::tuple<u64, u64, u32>;
struct Entry {
    std::string path;
    u32 tex = 0;
    bool tried = false;
};
std::map<Key, Entry> sEntries;

// A name as PCSX2 writes it (TEXTURE_FILENAME_CLUT_FORMAT_STRING and
// TEXTURE_FILENAME_FORMAT_STRING): hex numbers without leading zeros, the
// last one of eight digits.  Region and mipmap names are not used here.
bool parse(const std::string& stem, Key& key) {
    u64 a = 0, b = 0;
    u32 bits = 0;
    char extra = 0;
    if (std::sscanf(stem.c_str(), "%" SCNx64 "-%" SCNx64 "-%08x%c", &a, &b, &bits, &extra) == 3) {
        key = Key(a, b, bits & 0x3FFF);     // (bit 14 was TCC in older versions; TEXA bits are not ours)
        return true;
    }
    if (std::sscanf(stem.c_str(), "%" SCNx64 "-%08x%c", &a, &bits, &extra) == 2) {
        key = Key(a, 0, bits & 0x3FFF);
        return true;
    }
    return false;
}

u32 rd32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

// A DDS of DXT1 / DXT3 / DXT5 to the GPU as it is, mipmaps and all.  The
// pictures keep the GS's alpha (0x80 = opaque), as PCSX2 does; the game's
// textures here are made with 0xFF = opaque, so DXT5's alpha end points
// are doubled (the six-point mode, a0 <= a1, keeps its order).
u32 load_dds(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<u8> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) {
        printf("[textures] not a DDS: %s\n", path.c_str());
        return 0;
    }
    const int height = (int)rd32(&d[12]), width = (int)rd32(&d[16]);
    int levels = (int)rd32(&d[28]);
    if (levels < 1) levels = 1;
    const u32 fourcc = rd32(&d[84]);
    int blockBytes = 16;
    bool dxt5 = false;
    hal::gs::Compressed format;
    if (fourcc == 0x35545844u) {            // "DXT5"
        format = hal::gs::Compressed::DXT5;
        dxt5 = true;
    } else if (fourcc == 0x33545844u) {     // "DXT3"
        format = hal::gs::Compressed::DXT3;
    } else if (fourcc == 0x31545844u) {     // "DXT1"
        format = hal::gs::Compressed::DXT1;
        blockBytes = 8;
    } else {
        printf("[textures] %s: only DXT1 / DXT3 / DXT5 are read\n", path.c_str());
        return 0;
    }
    std::vector<hal::gs::Level> mips;
    size_t at = 128;
    for (int i = 0, w = width, h = height; i < levels; i++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) {
        const size_t size = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * (size_t)blockBytes;
        if (at + size > d.size())
            break;
        if (dxt5)
            for (size_t b = at; b < at + size; b += 16) {
                const int a0 = d[b], a1 = d[b + 1];
                int s0 = a0 * 255 / 128, s1 = a1 * 255 / 128;
                s0 = s0 > 255 ? 255 : s0;
                s1 = s1 > 255 ? 255 : s1;
                if (a0 > a1 && s0 == s1)
                    s1 = s0 - 1;
                d[b] = (u8)s0;
                d[b + 1] = (u8)s1;
            }
        mips.push_back({w, h, &d[at], size});
        at += size;
    }
    if (mips.empty())
        return 0;
    return hal::gs::create_texture_compressed(format, mips.data(), (int)mips.size());
}

}  // namespace

u64 hash(const void* data, size_t size) { return XXH3_64bits(data, size); }

int add_dir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    int count = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::follow_directory_symlink, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".dds")
            continue;
        Key key;
        if (!parse(it->path().stem().string(), key))
            continue;
        sEntries[key].path = it->path().string();
        count++;
    }
    return count;
}

bool active() { return !sEntries.empty(); }

static bool sOn = true;
static int sGeneration = 0;
bool enabled() { return sOn && !sEntries.empty(); }
void set_enabled(bool on) {
    if (on != sOn)
        sGeneration++;
    sOn = on;
}
int generation() { return sGeneration; }

u32 find(u64 textureHash, u64 paletteHash, u32 bits) {
    if (!sOn)
        return 0;
    static const bool trace = getenv("WOTS2_TRACE_TEXTURES") != nullptr;
    auto it = sEntries.find(Key(textureHash, paletteHash, bits & 0x3FFF));
    if (trace)
        printf("[textures] %" PRIx64 "-%" PRIx64 "-%08x %s\n", textureHash, paletteHash, bits,
               it != sEntries.end() ? "replaced" : "-");
    if (it == sEntries.end())
        return 0;
    Entry& e = it->second;
    if (!e.tried) {
        e.tried = true;
        e.tex = load_dds(e.path);
    }
    return e.tex;
}

bool owns(u32 tex) {
    if (tex == 0)
        return false;
    for (const auto& [key, e] : sEntries)
        if (e.tex == tex)
            return true;
    return false;
}

}  // namespace texture_pack
