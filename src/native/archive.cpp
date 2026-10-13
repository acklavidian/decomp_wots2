#include "native/archive.h"
#include "native/guest.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

struct Entry { u32 hash, offset, size, flags, storedEnd, nameLen; };

FILE* g_file = nullptr;
u32 g_dataStart = 0;
std::vector<Entry> g_entries;
std::string g_prefix = "Us/";

constexpr u32 kPathTable = 0x2D1AF0;
constexpr u32 kPathCount = 141;

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

// 0x141530: uppercase, '/' -> '\\', h = c + h * 19 with signed chars.
u32 name_hash(const std::string& path) {
    u32 h = 0;
    for (char raw : path) {
        char c = (char)toupper((unsigned char)raw);
        if (c == '/')
            c = '\\';
        h = (u32)(s32)(signed char)c + h * 19;
    }
    return h;
}

bool starts_with_nocase(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)s[i]) != toupper((unsigned char)prefix[i]))
            return false;
    return true;
}

// 0x1420B0: Okumura LZSS, 4 KB zero-filled window, write position 0xFEE.
void lzss_decompress(const std::vector<u8>& src, std::vector<u8>& out, u32 size) {
    u8 window[4096] = {};
    u32 wpos = 0xFEE, ip = 0, flags = 0;
    out.clear();
    out.reserve(size);
    while (out.size() < size) {
        flags >>= 1;
        if (!(flags & 0x100)) {
            if (ip >= src.size()) break;
            flags = src[ip++] | 0xFF00;
        }
        if (flags & 1) {
            if (ip >= src.size()) break;
            u8 c = src[ip++];
            out.push_back(c);
            window[wpos] = c;
            wpos = (wpos + 1) & 0xFFF;
        } else {
            if (ip + 1 >= src.size()) break;
            u8 lo = src[ip], hi = src[ip + 1];
            ip += 2;
            u32 pos = lo | ((hi & 0xF0) << 4);
            u32 len = (hi & 0x0F) + 3;
            for (u32 i = 0; i < len; i++) {
                u8 c = window[(pos + i) & 0xFFF];
                out.push_back(c);
                window[wpos] = c;
                wpos = (wpos + 1) & 0xFFF;
            }
        }
    }
    out.resize(size);
}

// 0x142430: LZO1X.
bool lzo1x_decompress(const std::vector<u8>& src, std::vector<u8>& out, u32 size) {
    out.clear();
    out.reserve(size + 16);
    size_t ip = 0;
    const size_t n = src.size();

    auto copy_match = [&](size_t dist, size_t len) {
        if (dist == 0 || dist > out.size())
            return false;
        size_t start = out.size() - dist;
        for (size_t i = 0; i < len; i++)
            out.push_back(out[start + i]);
        return true;
    };
    auto literals = [&](size_t count) {
        if (ip + count > n)
            return false;
        out.insert(out.end(), src.begin() + ip, src.begin() + ip + count);
        ip += count;
        return true;
    };
    auto extended = [&](size_t t, size_t base) {
        while (ip < n && src[ip] == 0) { t += 255; ip++; }
        t += base + src[ip++];
        return t;
    };

    enum State { Start, FirstLiteralRun, Match, MatchDone, MatchNext } state = Start;
    size_t t = src[ip];
    if (t > 17) {
        ip++;
        t -= 17;
        if (t < 4) {
            state = MatchNext;
        } else {
            if (!literals(t)) return false;
            state = FirstLiteralRun;
        }
    }

    for (;;) {
        if (ip >= n)
            return false;
        switch (state) {
        case Start:
            t = src[ip++];
            if (t >= 16) { state = Match; break; }
            if (t == 0) t = extended(t, 15);
            if (!literals(t + 3)) return false;
            state = FirstLiteralRun;
            break;

        case FirstLiteralRun:
            t = src[ip++];
            if (t >= 16) { state = Match; break; }
            if (!copy_match(1 + 0x800 + (t >> 2) + ((size_t)src[ip] << 2), 3)) return false;
            ip++;
            state = MatchDone;
            break;

        case Match:
            if (t >= 64) {
                size_t dist = 1 + ((t >> 2) & 7) + ((size_t)src[ip++] << 3);
                if (!copy_match(dist, (t >> 5) - 1 + 2)) return false;
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) t = extended(t, 31);
                size_t dist = 1 + (src[ip] >> 2) + ((size_t)src[ip + 1] << 6);
                ip += 2;
                if (!copy_match(dist, t + 2)) return false;
            } else if (t >= 16) {
                size_t dist = (t & 8) << 11;
                t &= 7;
                if (t == 0) t = extended(t, 7);
                dist += (src[ip] >> 2) + ((size_t)src[ip + 1] << 6);
                ip += 2;
                if (dist == 0)                       // end-of-stream marker
                    return out.size() == size;
                if (!copy_match(dist + 0x4000, t + 2)) return false;
            } else {
                size_t dist = 1 + (t >> 2) + ((size_t)src[ip++] << 2);
                if (!copy_match(dist, 2)) return false;
            }
            state = MatchDone;
            break;

        case MatchDone:
            t = src[ip - 2] & 3;
            state = t ? MatchNext : Start;
            break;

        case MatchNext:
            if (!literals(t)) return false;
            t = src[ip++];
            state = Match;
            break;
        }
    }
}

}  // namespace

namespace archive {

bool open(const char* volume_path) {
    g_file = fopen(volume_path, "rb");
    if (!g_file) {
        fprintf(stderr, "[archive] cannot open %s\n", volume_path);
        return false;
    }
    u8 header[20];
    if (fread(header, 1, 20, g_file) != 20 || memcmp(header, "\xFA\xDE\xBA\xBE", 4) != 0) {
        fprintf(stderr, "[archive] %s is not a FADEBABE archive\n", volume_path);
        return false;
    }
    u32 count = be32(header + 4);
    g_dataStart = be32(header + 12);
    std::vector<u8> raw(count * 24);
    if (fread(raw.data(), 1, raw.size(), g_file) != raw.size())
        return false;
    g_entries.resize(count);
    for (u32 i = 0; i < count; i++) {
        const u8* p = &raw[i * 24];
        g_entries[i] = {be32(p), be32(p + 4), be32(p + 8), be32(p + 12), be32(p + 16), be32(p + 20)};
    }
    printf("[archive] %u entries\n", count);
    return true;
}

void set_prefix(const char* prefix) { g_prefix = prefix; }

static std::vector<std::string> g_overlays;
void add_overlay(const char* dir) { g_overlays.push_back(dir); }

bool read(const std::string& path, std::vector<u8>& out) {
    std::string full = path;
    if (!starts_with_nocase(full, "Common/") && !starts_with_nocase(full, g_prefix.c_str()))
        full = g_prefix + full;

    for (auto dir = g_overlays.rbegin(); dir != g_overlays.rend(); ++dir) {
        std::string name = full, low;
        for (char& c : name)
            if (c == '\\') c = '/';
        for (char c : name) low += (char)tolower((unsigned char)c);
        for (const std::string& tried : {*dir + "/" + name, *dir + "/" + low})
            if (FILE* f = fopen(tried.c_str(), "rb")) {
                fseek(f, 0, SEEK_END);
                const long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                out.resize(size > 0 ? (size_t)size : 0);
                const bool ok = fread(out.data(), 1, out.size(), f) == out.size();
                fclose(f);
                if (ok)
                    return true;
            }
    }

    u32 hash = name_hash(full);
    const Entry* e = nullptr;
    for (const Entry& cand : g_entries)
        if (cand.hash == hash) { e = &cand; break; }
    if (!e || !g_file) {
        fprintf(stderr, "[archive] not found: %s\n", full.c_str());
        return false;
    }

    std::vector<u8> stored(e->storedEnd - e->offset);
    fseek(g_file, (long)g_dataStart + e->offset, SEEK_SET);
    if (fread(stored.data(), 1, stored.size(), g_file) != stored.size())
        return false;

    if (e->flags & 2) {
        if (!lzo1x_decompress(stored, out, e->size)) {
            fprintf(stderr, "[archive] LZO error in %s\n", full.c_str());
            return false;
        }
    } else if (e->flags & 4) {
        lzss_decompress(stored, out, e->size);
    } else {
        stored.resize(e->size);
        out.swap(stored);
    }
    return true;
}

const char* asset_path(u32 id) {
    if (id >= kPathCount)
        return nullptr;
    return guest::ptr<const char>(guest::ptr<u32>(kPathTable)[id]);
}

bool read_asset(u32 id, std::vector<u8>& out) {
    const char* path = asset_path(id);
    if (!path) {
        fprintf(stderr, "[archive] bad asset id %#x\n", id);
        return false;
    }
    return read(path, out);
}

}  // namespace archive
