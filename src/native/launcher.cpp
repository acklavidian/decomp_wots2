// The launcher (native, not decompiled): finds the disc's files, unpacking
// them from a disc image the first time.
//
//   wots2                      the directory `extracted` (or WOTS2_DATA) if the game
//                              is there; otherwise asks for the disc image
//   wots2 <directory>          the files are in that directory (as before)
//   wots2 <image or archive>   unpacks it into `extracted` (or WOTS2_DATA), then plays
//
// A disc image is read with libarchive, which knows ISO 9660 as well as zip,
// 7z, rar and tar with their compressions — so an .iso, an archive holding
// the .iso, and an archive of the files themselves all go the same way.
#include "native/launcher.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#ifdef WOTS2_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace launcher {

namespace {

const char* const kExecutable = "SLUS_208.93";
// What of the disc the game reads (the rest is firmware and 100 MB of padding).
const char* const kWanted[] = {"SLUS_208.93", "VOLUME.DAT", "SYSTEM.CNF", "BGM", "MOVIE", "MODULES"};

namespace fs = std::filesystem;

bool is_dir(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool is_file(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
void remove_tree(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return is_dir(path);
}

#ifdef WOTS2_LIBARCHIVE
// An entry's path as the game spells it: no "./", no ISO version suffix, upper case.
std::string normal(const char* raw) {
    std::string s = raw ? raw : "";
    while (!s.compare(0, 2, "./"))
        s.erase(0, 2);
    while (!s.empty() && s[0] == '/')
        s.erase(0, 1);
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    const size_t semi = s.rfind(';');
    if (semi != std::string::npos && s.find('/', semi) == std::string::npos)
        s.erase(semi);
    for (char& c : s)
        c = (char)toupper((unsigned char)c);
    return s;
}
std::string base_name(const std::string& s) {
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}
bool ends_with(const std::string& s, const char* tail) {
    const size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}
// "SLES_123.45": some PlayStation 2 disc's executable.
bool looks_like_ps2_exe(const std::string& name) {
    return name.size() == 11 && name[0] == 'S' && (name[1] == 'L' || name[1] == 'C') && name[4] == '_' &&
           name[8] == '.' && isdigit((unsigned char)name[5]) && isdigit((unsigned char)name[10]);
}

struct Listed {
    std::string raw;        // as the archive has it
    std::string path;       // normal()
    int64_t size;
};

archive* open_archive(const std::string& path, std::string* error) {
    archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), 1 << 20) != ARCHIVE_OK) {
        *error = std::string("cannot read it: ") + (archive_error_string(a) ? archive_error_string(a) : "unknown format");
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

bool list_archive(const std::string& path, std::vector<Listed>* out, std::string* error) {
    archive* a = open_archive(path, error);
    if (a == nullptr)
        return false;
    archive_entry* e;
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (archive_entry_filetype(e) != AE_IFREG)
            continue;
        const char* name = archive_entry_pathname(e);
        out->push_back({name ? name : "", normal(name), (int64_t)archive_entry_size(e)});
    }
    const bool ok = r == ARCHIVE_EOF;
    if (!ok)
        *error = std::string("cannot read it: ") + (archive_error_string(a) ? archive_error_string(a) : "damaged file");
    archive_read_free(a);
    return ok || !out->empty();
}

// Writes the entries `pick` gives a destination for.  `total` bytes in all.
bool unpack(const std::string& path, const std::function<std::string(const char* raw)>& pick, int64_t total,
            const char* what, double from, double share, const Progress& progress, std::string* error) {
    archive* a = open_archive(path, error);
    if (a == nullptr)
        return false;
    std::vector<char> buffer(1 << 20);
    int64_t done = 0;
    archive_entry* e;
    int r;
    bool ok = true;
    while (ok && ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN)) {
        if (archive_entry_filetype(e) != AE_IFREG)
            continue;
        const std::string to = pick(archive_entry_pathname(e));
        if (to.empty())
            continue;
        const size_t slash = to.rfind('/');
        if (slash != std::string::npos && !make_dirs(to.substr(0, slash))) {
            *error = "cannot create " + to.substr(0, slash);
            ok = false;
            break;
        }
        FILE* f = fopen(to.c_str(), "wb");
        if (f == nullptr) {
            *error = "cannot write " + to + ": " + strerror(errno);
            ok = false;
            break;
        }
        for (;;) {
            const la_ssize_t n = archive_read_data(a, buffer.data(), buffer.size());
            if (n == 0)
                break;
            if (n < 0) {
                *error = std::string("the file is damaged: ") +
                         (archive_error_string(a) ? archive_error_string(a) : "read error");
                ok = false;
                break;
            }
            if (fwrite(buffer.data(), 1, (size_t)n, f) != (size_t)n) {
                *error = std::string("cannot write ") + to + ": " + strerror(errno);
                ok = false;
                break;
            }
            done += n;
            if (progress && !progress(what, from + share * (total > 0 ? (double)done / (double)total : 0.0))) {
                *error = "cancelled";
                ok = false;
                break;
            }
        }
        if (fclose(f) != 0 && ok) {
            *error = std::string("cannot write ") + to + ": " + strerror(errno);
            ok = false;
        }
    }
    archive_read_free(a);
    return ok;
}

bool enough_room(const std::string& near, int64_t bytes, std::string* error) {
    std::error_code ec;
    fs::path dir = fs::path(near).parent_path();
    if (dir.empty())
        dir = ".";
    const fs::space_info space = fs::space(dir, ec);
    if (ec)
        return true;
    const int64_t room = (int64_t)space.available;
    if (room >= bytes + (64 << 20))
        return true;
    char text[160];
    snprintf(text, sizeof text, "not enough disk space: %.1f GB needed, %.1f GB free", bytes / 1e9, room / 1e9);
    *error = text;
    return false;
}

// One level: `source` holds the game's files, or an image that does.
bool extract_level(const std::string& source, const std::string& out, int depth, double from, double share,
                   const Progress& progress, std::string* error) {
    std::vector<Listed> list;
    if (!list_archive(source, &list, error))
        return false;

    // The game's files: everything beside the executable.
    const Listed* exe = nullptr;
    for (const Listed& l : list)
        if (base_name(l.path) == kExecutable && (exe == nullptr || l.path.size() < exe->path.size()))
            exe = &l;
    if (exe != nullptr) {
        const std::string prefix = exe->path.substr(0, exe->path.size() - strlen(kExecutable));
        auto wanted = [&](const std::string& path) -> std::string {
            if (path.compare(0, prefix.size(), prefix) != 0)
                return "";
            const std::string rel = path.substr(prefix.size());
            if (rel.empty() || rel.find("..") != std::string::npos)
                return "";
            const std::string top = rel.substr(0, rel.find('/'));
            for (const char* w : kWanted)
                if (top == w)
                    return rel;
            return "";
        };
        int64_t total = 0;
        for (const Listed& l : list)
            if (!wanted(l.path).empty())
                total += l.size;
        if (!enough_room(out, total, error))
            return false;
        return unpack(source, [&](const char* raw) {
                          const std::string rel = wanted(normal(raw));
                          return rel.empty() ? std::string() : out + "/" + rel;
                      },
                      total, "Extracting the game's files", from, share, progress, error);
    }

    for (const Listed& l : list)
        if (looks_like_ps2_exe(base_name(l.path))) {
            *error = "this is a different game or release (" + base_name(l.path) +
                     "); the North American disc, SLUS-208.93, is needed";
            return false;
        }

    // An image inside an archive: the largest thing that is, or could be, one.
    const Listed* image = nullptr;
    if (depth < 2) {
        for (const Listed& l : list) {
            const bool named = ends_with(l.path, ".ISO") || ends_with(l.path, ".IMG") || ends_with(l.path, ".BIN") ||
                               ends_with(l.path, ".MDF") || ends_with(l.path, ".UDF");
            if ((named || l.size > (300ll << 20)) && (image == nullptr || l.size > image->size))
                image = &l;
        }
    }
    if (image == nullptr) {
        *error = "Way of the Samurai 2 (SLUS-208.93) is not in it";
        return false;
    }
    if (!enough_room(out, image->size * 2, error))
        return false;
    const std::string temp = out + ".image";
    const std::string raw = image->raw;
    bool ok = unpack(source, [&](const char* name) { return name && raw == name ? temp : std::string(); },
                     image->size, "Unpacking the disc image", from, share * 0.5, progress, error);
    if (ok)
        ok = extract_level(temp, out, depth + 1, from + share * 0.5, share * 0.5, progress, error);
    remove_tree(temp);
    return ok;
}
#endif  // WOTS2_LIBARCHIVE

// ── The window ──────────────────────────────────────────────────────────────

struct Window {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    Uint32 drawn = 0;
    bool closed = false;

    bool open(const char* title) {
        if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
            return false;
        window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 560, 150, 0);
        if (window == nullptr)
            return false;
        renderer = SDL_CreateRenderer(window, -1, 0);
        SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
        return true;
    }
    void close() {
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        renderer = nullptr;
        window = nullptr;
    }
    // A bar `fraction` full; a negative fraction draws the empty frame only
    // (the "drop a file here" state).
    void draw(double fraction) {
        if (renderer == nullptr)
            return;
        SDL_SetRenderDrawColor(renderer, 0x17, 0x14, 0x12, 0xFF);
        SDL_RenderClear(renderer);
        const SDL_Rect frame = {40, 60, 480, 30};
        SDL_SetRenderDrawColor(renderer, 0xB0, 0xA0, 0x80, 0xFF);
        SDL_RenderDrawRect(renderer, &frame);
        if (fraction > 0.0) {
            const SDL_Rect bar = {43, 63, (int)(474 * std::min(fraction, 1.0)), 24};
            SDL_SetRenderDrawColor(renderer, 0xA2, 0x23, 0x23, 0xFF);
            SDL_RenderFillRect(renderer, &bar);
        }
        SDL_RenderPresent(renderer);
    }
    // → a file dropped on the window, if any.
    std::string pump() {
        std::string dropped;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
                closed = true;
            if (e.type == SDL_DROPFILE && e.drop.file != nullptr) {
                dropped = e.drop.file;
                SDL_free(e.drop.file);
            }
        }
        return dropped;
    }
};

// The desktop's own file chooser, if there is one.  → false if there is none.
bool choose_file(std::string* out) {
    out->clear();
#ifdef _WIN32
    char file[MAX_PATH * 4] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrTitle = "Way of the Samurai 2: choose your disc image";
    ofn.lpstrFilter = "Disc images and archives\0*.iso;*.img;*.bin;*.zip;*.7z;*.rar;*.gz;*.xz;*.zst;*.tar\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (GetOpenFileNameA(&ofn))
        *out = file;
    return true;
#else
    struct Chooser { const char* program; const char* command; };
    const Chooser choosers[] = {
#ifdef __APPLE__
        {"osascript",
         "osascript -e 'POSIX path of (choose file with prompt \"Way of the Samurai 2: choose your disc image\")' "
         "2>/dev/null"},
#endif
        {"zenity",
         "zenity --file-selection --title='Way of the Samurai 2: choose your disc image' "
         "--file-filter='Disc images and archives | *.iso *.ISO *.img *.IMG *.bin *.BIN *.zip *.ZIP *.7z *.7Z "
         "*.rar *.RAR *.gz *.xz *.zst *.tar' --file-filter='All files | *' 2>/dev/null"},
        {"kdialog",
         "kdialog --title 'Way of the Samurai 2: choose your disc image' --getopenfilename . "
         "'Disc images and archives (*.iso *.img *.bin *.zip *.7z *.rar *.gz *.xz *.zst *.tar);;All files (*)' "
         "2>/dev/null"},
    };
    for (const Chooser& c : choosers) {
        const std::string probe = std::string("command -v ") + c.program + " >/dev/null 2>&1";
        if (system(probe.c_str()) != 0)
            continue;
        if (FILE* p = popen(c.command, "r")) {
            char line[4096];
            if (fgets(line, sizeof line, p) != nullptr) {
                *out = line;
                while (!out->empty() && (out->back() == '\n' || out->back() == '\r'))
                    out->pop_back();
            }
            pclose(p);
        }
        return true;
    }
    return false;
#endif
}

// Unpacks `source` behind a progress window (or with lines on the terminal
// when there is no display).  → false with a message shown.
bool extract_shown(const std::string& source, const std::string& dest, bool interactive) {
    Window w;
    const bool shown = w.open("Way of the Samurai 2: extracting…");
    int lastPercent = -1;
    std::string error;
    const bool ok = extract(source, dest, [&](const char* what, double fraction) {
        const int percent = (int)(fraction * 100.0);
        if (shown) {
            const Uint32 now = SDL_GetTicks();
            if (now - w.drawn >= 33 || percent != lastPercent) {
                w.drawn = now;
                w.pump();
                if (percent != lastPercent) {
                    char title[160];
                    snprintf(title, sizeof title, "%s… %d%%", what, percent);
                    SDL_SetWindowTitle(w.window, title);
                }
                w.draw(fraction);
            }
        }
        if (percent != lastPercent && (percent % 10 == 0 || !shown)) {
            fprintf(stderr, "\r[launcher] %s… %d%%", what, percent);
            fflush(stderr);
        }
        lastPercent = percent;
        return !w.closed;
    }, &error);
    fprintf(stderr, "\n");
    w.close();
    if (ok) {
        fprintf(stderr, "[launcher] the game's files are in %s\n", dest.c_str());
        return true;
    }
    fprintf(stderr, "[launcher] %s: %s\n", source.c_str(), error.c_str());
    if (interactive && error != "cancelled")
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Way of the Samurai 2",
                                 ("That file could not be used:\n\n" + error).c_str(), nullptr);
    return false;
}

[[noreturn]] void usage(int code) {
    fprintf(stderr,
            "usage: wots2 [directory with the disc's files | disc image (.iso) | archive (.zip, .7z, .rar, …)]\n"
            "       Without an argument the files are looked for in ./extracted (or $WOTS2_DATA);\n"
            "       given an image or an archive, they are unpacked there first.\n");
    exit(code);
}

// Asks for the disc image until one could be unpacked.  → the directory with
// the game's files; does not come back without them.
std::string ask_and_extract(const std::string& dest) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        usage(1);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose file…"},
    };
    const SDL_MessageBoxData box = {
        SDL_MESSAGEBOX_INFORMATION, nullptr, "Way of the Samurai 2",
        "The game's files have not been set up yet.\n\n"
        "Choose your copy of the disc (SLUS-208.93): an .iso image, or a\n"
        ".zip / .7z / .rar archive that holds it. Its files are unpacked once,\n"
        "into the folder \"extracted\" (about 1.5 GB).",
        2, buttons, nullptr};
    for (;;) {
        int pressed = 0;
        if (SDL_ShowMessageBox(&box, &pressed) != 0)
            usage(1);                           // no display to ask on
        if (pressed != 1)
            exit(0);
        std::string file;
        if (!choose_file(&file)) {
            // No file chooser on this desktop: a window to drop the file on.
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Way of the Samurai 2",
                                     "Drag the disc image onto the next window (Esc quits).", nullptr);
            Window w;
            if (!w.open("Way of the Samurai 2: drop your disc image here"))
                usage(1);
            while (file.empty() && !w.closed) {
                file = w.pump();
                w.draw(-1.0);
                SDL_Delay(30);
            }
            w.close();
            if (file.empty())
                exit(0);
        }
        if (file.empty())
            continue;                           // the chooser was cancelled
        if (is_dir(file) && has_game(file))
            return file;                        // (a folder that already holds the files was dropped)
        if (extract_shown(file, dest, true))
            return dest;
    }
}

}  // namespace

bool has_game(const std::string& dir) {
    return is_file(dir + "/" + kExecutable) && is_file(dir + "/VOLUME.DAT");
}

bool extract(const std::string& source, const std::string& dest, const Progress& progress, std::string* error) {
#ifdef WOTS2_LIBARCHIVE
    std::string none;
    if (error == nullptr)
        error = &none;
    if (!is_file(source)) {
        *error = "there is no such file";
        return false;
    }
    // Into a directory beside the destination, moved into place when whole.
    const std::string partial = dest + ".partial";
    remove_tree(partial);
    if (!make_dirs(partial)) {
        *error = "cannot create " + partial + ": " + strerror(errno);
        return false;
    }
    bool ok = extract_level(source, partial, 0, 0.0, 1.0, progress, error);
    if (ok && !has_game(partial)) {
        *error = "the disc's files are incomplete (no VOLUME.DAT)";
        ok = false;
    }
    if (ok) {
        if (is_dir(dest) && !has_game(dest)) {
            std::error_code ec;
            fs::remove(dest, ec);               // an empty leftover
        }
        if (rename(partial.c_str(), dest.c_str()) != 0) {
            *error = "cannot move the files to " + dest + ": " + strerror(errno);
            ok = false;
        }
    }
    if (!ok)
        remove_tree(partial);
    return ok;
#else
    (void)source; (void)dest; (void)progress;
    if (error)
        *error = "this build cannot unpack disc images (it was built without libarchive)";
    return false;
#endif
}

std::string locate(int argc, char* argv[]) {
    SDL_SetMainReady();
    const char* data = getenv("WOTS2_DATA");
    std::string dest = data && *data ? data : "extracted";

    if (argc > 1) {
        const std::string given = argv[1];
        if (given == "-h" || given == "--help")
            usage(0);
        if (is_dir(given))
            return given;                       // as before: the game says so itself if it is not there
        if (!is_file(given)) {
            fprintf(stderr, "[launcher] %s: no such file or directory\n", given.c_str());
            usage(1);
        }
        if (has_game(dest)) {
            fprintf(stderr, "[launcher] %s already holds the game's files; not unpacking again\n", dest.c_str());
            return dest;
        }
        if (!extract_shown(given, dest, false))
            exit(1);
    } else {
        if (has_game(dest))
            return dest;
        // A scripted run never asks.
        if (getenv("WOTS2_INPUT") || getenv("WOTS2_TOUR") || getenv("WOTS2_EXIT_FRAME")) {
            fprintf(stderr, "[launcher] the game's files are not in %s\n", dest.c_str());
            usage(1);
        }
        dest = ask_and_extract(dest);
    }

    // Start afresh with the directory: the game maps its memory at fixed low
    // addresses and wants to be first there.
    SDL_Quit();
#ifdef _WIN32
    char self[MAX_PATH * 4];
    if (GetModuleFileNameA(nullptr, self, sizeof self) != 0) {
        const std::string quoted = "\"" + dest + "\"";
        const intptr_t code = _spawnl(_P_WAIT, self, "wots2", quoted.c_str(), nullptr);
        if (code != -1)
            exit((int)code);
    }
#elif defined(__APPLE__)
    {
        char* const again[] = {argv[0], const_cast<char*>(dest.c_str()), nullptr};
        execv(argv[0], again);
    }
#else
    {
        char* const again[] = {argv[0], const_cast<char*>(dest.c_str()), nullptr};
        execv("/proc/self/exe", again);
    }
#endif
    return dest;                                // (it could not be started again: carry on in this process)
}

}  // namespace launcher
