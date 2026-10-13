// The memory card — native.  This is hardware: the game reaches a card
// through a small class of its own (0x136EB0–0x137420, object `*0x2EE75C`)
// over Sony's libmc, whose calls are asynchronous (start one, then poll
// sceMcSync until it is done).  Here the card is a directory on the host —
// `wots2_card` in the current directory, or what WOTS2_CARD names — and
// every call is done by the time it returns.
//
// What the game puts on it (see src/game/mcard.cpp):
//   BASLUS-20893/icon.sys        0x3C4 bytes: the browser's title and colours
//   BASLUS-20893/samurai2.ico    the browser's icon (a copy of mcard/icon1.ico)
//   BASLUS-20893/BASLUS-20893    0x14890 bytes: three save slots
//
// The card is always there, is always a PlayStation 2 card, is formatted and
// has room; WOTS2_NEW_GAME=1 makes it look empty (and keeps writes off it).

#include "native/mcard.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mc {

namespace {

u32  sStatus = 0;           // as the game keeps it: see status()
s32  sResult = 0;
FILE* sFile = nullptr;
std::vector<Entry> sEntries;

std::string Root() {
    const char* dir = getenv("WOTS2_CARD");
    return dir != nullptr && *dir != 0 ? dir : "wots2_card";
}
bool Hidden() { return getenv("WOTS2_NEW_GAME") != nullptr; }
std::string Host(const char* path) { return Root() + path; }

}  // namespace

// Bits of a port's status word: low 7 bits the command in progress, 0x80 /
// 0x200 unformatted, 0x100 a card is there, 0x400 | 0x800 no card / changed,
// 0xF000 the kind of card (2 = PlayStation 2).
u32  status(int) { return sStatus; }
void set_flags(int, u32 flags) { sStatus |= flags; }
void clear_flags(int, u32 flags) { sStatus &= ~flags; }
s32  result() { return sResult; }
s32  free_kb(int) { return 8000; }
bool busy() { return false; }

void get_info(int) {
    sStatus = 0x2100;
    sResult = 0;
}

// sceMcGetDir: → the number of entries (at most eight), −4 no such path.
void get_dir(int, const char* pattern) {
    sEntries.clear();
    std::string path = pattern;
    const bool inside = path.size() >= 2 && path.compare(path.size() - 2, 2, "/*") == 0;
    if (inside)
        path.resize(path.size() - 2);
    struct stat st;
    if (Hidden() || stat(Host(path.c_str()).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        sResult = inside ? -4 : 0;
        return;
    }
    if (!inside) {
        sEntries.push_back({0, path.substr(path.rfind('/') + 1)});
        sResult = 1;
        return;
    }
    sEntries.push_back({0, "."});
    sEntries.push_back({0, ".."});
    if (DIR* dir = opendir(Host(path.c_str()).c_str())) {
        while (dirent* e = readdir(dir)) {
            if (e->d_name[0] == '.' || sEntries.size() >= 8)
                continue;
            struct stat file;
            const std::string full = Host(path.c_str()) + "/" + e->d_name;
            if (stat(full.c_str(), &file) == 0 && S_ISREG(file.st_mode))
                sEntries.push_back({(u32)file.st_size, e->d_name});
        }
        closedir(dir);
    }
    sResult = (s32)sEntries.size();
}
const Entry* entry(int index) {
    static const Entry none{0, ""};
    return index >= 0 && (size_t)index < sEntries.size() ? &sEntries[(size_t)index] : &none;
}

// sceMcOpen: mode 1 read, 2 write, 0x200 create.  → 0 if the call was taken.
int open(int, const char* path, int mode) {
    if (sFile != nullptr) {
        fclose(sFile);
        sFile = nullptr;
    }
    const bool writing = (mode & 0x202) != 0;
    if (Hidden() && writing) {
        // (kept off the disc: reads and writes then go nowhere)
        sFile = fopen("/dev/null", "r+b");
    } else if (Hidden()) {
        sFile = nullptr;
    } else if (mode & 0x200) {
        sFile = fopen(Host(path).c_str(), "w+b");
    } else {
        sFile = fopen(Host(path).c_str(), writing ? "r+b" : "rb");
    }
    sResult = sFile != nullptr ? 0 : -4;
    return 0;
}
bool is_open() { return sFile != nullptr; }
int read(void* buffer, u32 size) {
    if (sFile == nullptr)
        return -1;
    sResult = (s32)fread(buffer, 1, size, sFile);
    return 0;
}
int write(const void* buffer, u32 size) {
    if (sFile == nullptr)
        return -1;
    sResult = (s32)fwrite(buffer, 1, size, sFile);
    return 0;
}
int seek(u32 offset) {
    if (sFile == nullptr)
        return -1;
    sResult = fseek(sFile, (long)offset, SEEK_SET) == 0 ? (s32)offset : -1;
    return 0;
}
int close() {
    if (sFile == nullptr)
        return -1;
    fclose(sFile);
    sFile = nullptr;
    sResult = 0;
    return 0;
}
int make_dir(int, const char* path) {
    if (!Hidden()) {
        mkdir(Root().c_str(), 0777);
        mkdir(Host(path).c_str(), 0777);
    }
    sResult = 0;
    return 0;
}
int remove(int, const char* path) {
    if (Hidden()) {
        sResult = -4;
        return 0;
    }
    const std::string host = Host(path);
    sResult = (unlink(host.c_str()) == 0 || rmdir(host.c_str()) == 0) ? 0 : -4;
    return 0;
}
void format(int) { sResult = 0; }
void unformat(int) { sResult = 0; }

}  // namespace mc
