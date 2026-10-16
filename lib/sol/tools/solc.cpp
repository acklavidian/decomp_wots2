// solc: compile Sol scripts.
//
//   solc in.sol [-o out.sx] [-I dir]     compile one script
//   solc --check dir                     compile every dir/**/*.sol and compare
//                                        with the .sx beside it, byte for byte
//
// #include "x" is looked up beside the script, then in each -I directory.
#include "sol/compiler.h"

#include <cctype>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// The game's archive ignores case ("FlagID" is flagID.sol): so do these lookups.
static fs::path nocase(const fs::path& dir, const fs::path& rel) {
    fs::path at = dir;
    for (const auto& part : rel) {
        fs::path exact = at / part;
        if (fs::exists(exact)) { at = exact; continue; }
        std::string want = part.string();
        for (auto& c : want) c = (char)tolower((unsigned char)c);
        bool found = false;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(at, ec)) {
            std::string n = e.path().filename().string();
            for (auto& c : n) c = (char)tolower((unsigned char)c);
            if (n == want) { at = e.path(); found = true; break; }
        }
        if (!found) return dir / rel;
    }
    return at;
}

static bool read_file(const fs::path& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static sol::IncludeLoader loader(std::vector<fs::path> dirs) {
    return [dirs](const std::string& name, std::vector<uint8_t>* bytes) {
        for (const auto& d : dirs)
            if (read_file(nocase(d, name), bytes)) return true;
        return false;
    };
}

int main(int argc, char** argv) {
    std::vector<fs::path> inc;
    std::string in, out, check;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "-I" && i + 1 < argc) inc.push_back(argv[++i]);
        else if (a == "--check" && i + 1 < argc) check = argv[++i];
        else in = a;
    }
    if (!check.empty()) {
        int total = 0, same = 0;
        for (const auto& e : fs::recursive_directory_iterator(check)) {
            if (e.path().extension() != ".sol") continue;
            fs::path sx = e.path();
            sx.replace_extension(".sx");
            std::vector<uint8_t> src, want;
            if (!read_file(e.path(), &src) || !read_file(sx, &want)) continue;
            total++;
            std::vector<fs::path> dirs{e.path().parent_path(), check};
            dirs.insert(dirs.end(), inc.begin(), inc.end());
            const auto r = sol::compile(src, e.path().filename().string(), loader(dirs));
            const auto rel = fs::relative(e.path(), check).string();
            if (!r.ok) { printf("FAIL %s: %s\n", rel.c_str(), r.error.c_str()); continue; }
            const auto got = sol::make_sx(r.code);
            // The disc's padding after the code is whatever was in memory: compare the header and the code.
            const size_t meaningful = 0x14 + r.code.size();
            if (got.size() == want.size() && std::equal(got.begin(), got.begin() + (long)meaningful, want.begin())) {
                same++;
                continue;
            }
            size_t at = 0;
            while (at < got.size() && at < want.size() && got[at] == want[at]) at++;
            printf("DIFF %s: at code offset 0x%zx (ours %zu bytes, disc %zu)\n", rel.c_str(),
                   at >= 0x14 ? at - 0x14 : at, got.size(), want.size());
        }
        printf("%d of %d identical\n", same, total);
        return same == total ? 0 : 1;
    }
    if (in.empty()) {
        fprintf(stderr, "usage: solc in.sol [-o out.sx] [-I dir] | solc --check dir\n");
        return 2;
    }
    std::vector<uint8_t> src;
    if (!read_file(in, &src)) { fprintf(stderr, "solc: cannot read %s\n", in.c_str()); return 1; }
    std::vector<fs::path> dirs{fs::path(in).parent_path()};
    dirs.insert(dirs.end(), inc.begin(), inc.end());
    const auto r = sol::compile(src, fs::path(in).filename().string(), loader(dirs));
    if (!r.ok) { fprintf(stderr, "%s\n", r.error.c_str()); return 1; }
    if (out.empty()) out = fs::path(in).replace_extension(".sx").string();
    const auto sx = sol::make_sx(r.code);
    std::ofstream(out, std::ios::binary).write((const char*)sx.data(), (std::streamsize)sx.size());
    return 0;
}
