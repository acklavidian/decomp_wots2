// The mod loader.  Not decompiled code: nothing like it is in the game.
//
// A mod is a directory: mods/<name>/mod.ini, and optionally a shared library
// (mods/<name>/<name>.so, entered through wots2_mod_init with the table of
// include/mod/wots2_mod.h) and a files/ directory whose contents take the
// place of the game's own files of the same path.  Mods are loaded in the
// order of their directories' names, after the game's functions are bound
// and the window is up, before the game starts.
#include "native/mods.h"
#include "mod/wots2_mod.h"
#include "hal/gs.h"
#include "native/archive.h"
#include "native/guest.h"
#include "native/texture_pack.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#ifdef _WIN32
#include <windows.h>
static void* dlopen(const char* path, int) { return LoadLibraryA(path); }
static void* dlsym(void* library, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
}
static const char* dlerror() { return "the library could not be loaded"; }
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#define WOTS2_MOD_SUFFIX ".dll"
#else
#include <dlfcn.h>
#ifdef __APPLE__
#define WOTS2_MOD_SUFFIX ".dylib"
#else
#define WOTS2_MOD_SUFFIX ".so"
#endif
#endif
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

struct Mod {
    Wots2Api api{};                     // (first: the mod's api pointer is the mod)
    std::string dir, id, name;
    std::map<std::string, std::string> settings;
    void* library = nullptr;
};
std::vector<Mod*> sMods;
std::string sTexturePackDir;     // the mod that brought a texture pack
struct FrameHook { void (*fn)(void*); void* user; };
std::vector<FrameHook> sFrameHooks;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
bool is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool is_file(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// mod.ini: "[section]" lines and "key = value" lines; ';' or '#' starts a
// comment.  → "section.key" → value (keys in lower case).
bool read_ini(const std::string& path, std::map<std::string, std::string>& out) {
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr)
        return false;
    char line[1024];
    std::string section;
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        const size_t cut = s.find_first_of(";#");
        if (cut != std::string::npos)
            s.erase(cut);
        s = trim(s);
        if (s.empty())
            continue;
        if (s.front() == '[' && s.back() == ']') {
            section = lower(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        const size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        out[section + "." + lower(trim(s.substr(0, eq)))] = trim(s.substr(eq + 1));
    }
    fclose(f);
    return true;
}

Mod* mod_of(const Wots2Api* api) { return reinterpret_cast<Mod*>(const_cast<Wots2Api*>(api)); }

void api_log(const Wots2Api* api, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    printf("[mod %s] %s\n", mod_of(api)->id.c_str(), text);
}
const char* api_setting(const Wots2Api* api, const char* key, const char* fallback) {
    Mod* mod = mod_of(api);
    std::string env = "WOTS2_MOD_" + mod->id + "_" + key;
    for (char& c : env) c = (char)toupper((unsigned char)c);
    if (const char* v = getenv(env.c_str()))
        return v;
    const auto it = mod->settings.find(lower(key));
    return it != mod->settings.end() ? it->second.c_str() : fallback;
}
void api_get_video(Wots2Video* out) {
    const hal::gs::Video v = hal::gs::video();
    *out = {v.aspect, v.height, v.fps, v.vsync, v.fullscreen, v.windowWidth, v.windowHeight, v.hudEdges};
}
void api_set_video(const Wots2Video* in) {
    hal::gs::Video v;
    v.aspect = in->aspect;
    v.height = in->height;
    v.fps = in->fps;
    v.vsync = in->vsync != 0;
    v.fullscreen = in->fullscreen != 0;
    v.windowWidth = in->window_width;
    v.windowHeight = in->window_height;
    v.hudEdges = in->hud_edges != 0;
    hal::gs::set_video(v);
}
void api_get_lighting(Wots2Lighting* out) {
    const hal::gs::Lighting l = hal::gs::lighting();
    *out = {l.shadows, l.shadowStrength, l.shadowSize, l.shadowReach, l.ao, l.aoStrength, l.aoRadius,
            l.lanterns, l.lanternStrength, l.lanternRadius, l.bloom, l.bloomThreshold,
            l.grade, l.contrast, l.saturation, l.warmth, l.vignette};
}
void api_set_lighting(const Wots2Lighting* in) {
    hal::gs::Lighting l;
    l.shadows = in->shadows != 0;
    l.shadowStrength = in->shadow_strength;
    l.shadowSize = in->shadow_size;
    l.shadowReach = in->shadow_reach;
    l.ao = in->ao != 0;
    l.aoStrength = in->ao_strength;
    l.aoRadius = in->ao_radius;
    l.lanterns = in->lanterns != 0;
    l.lanternStrength = in->lantern_strength;
    l.lanternRadius = in->lantern_radius;
    l.bloom = in->bloom;
    l.bloomThreshold = in->bloom_threshold;
    l.grade = in->grade != 0;
    l.contrast = in->contrast;
    l.saturation = in->saturation;
    l.warmth = in->warmth;
    l.vignette = in->vignette;
    l.mapShadows = hal::gs::lighting().mapShadows;      // (version 3's, set apart)
    l.bump = hal::gs::lighting().bump;
    l.bumpDepth = hal::gs::lighting().bumpDepth;
    l.dust = hal::gs::lighting().dust;
    l.unbake = hal::gs::lighting().unbake;
    l.modelShadow = hal::gs::lighting().modelShadow;
    l.lanternShadows = hal::gs::lighting().lanternShadows;
    l.weather = hal::gs::lighting().weather;
    l.rainChance = hal::gs::lighting().rainChance;
    l.rainAmount = hal::gs::lighting().rainAmount;
    l.characterBump = hal::gs::lighting().characterBump;
    l.godRays = hal::gs::lighting().godRays;
    l.moods = hal::gs::lighting().moods;
    memcpy(l.mood, hal::gs::lighting().mood, sizeof l.mood);
    l.unbakeExposure = hal::gs::lighting().unbakeExposure;
    hal::gs::set_lighting(l);
}
void api_set_bump(float strength, float depth) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.bump = strength > 0.0f ? strength : 0.0f;
    l.bumpDepth = depth > 0.0f ? depth : 4.0f;
    hal::gs::set_lighting(l);
}
void api_set_moods(int on) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.moods = on != 0;
    hal::gs::set_lighting(l);
}
void api_set_mood(int which, const float values[10]) {
    if (which < 0 || which >= hal::gs::kMoodCount)
        return;
    hal::gs::Lighting l = hal::gs::lighting();
    memcpy(&l.mood[which].brightness, values, 10 * sizeof(float));
    hal::gs::set_lighting(l);
}
void api_set_character_bump(float share) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.characterBump = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
    hal::gs::set_lighting(l);
}
void api_set_god_rays(float strength) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.godRays = strength < 0.0f ? 0.0f : strength > 2.0f ? 2.0f : strength;
    hal::gs::set_lighting(l);
}
void api_set_weather(int mode, float chance, float amount) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.weather = mode < 0 || mode > 2 ? 0 : mode;
    l.rainChance = chance < 0.0f ? 0.0f : chance > 1.0f ? 1.0f : chance;
    l.rainAmount = amount < 0.0f ? 0.0f : amount > 2.0f ? 2.0f : amount;
    hal::gs::set_lighting(l);
}
void api_set_lantern_shadows(int on) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.lanternShadows = on != 0;
    hal::gs::set_lighting(l);
}
void api_set_model_shadow(float share) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.modelShadow = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
    hal::gs::set_lighting(l);
}
void api_set_unbake(float amount, float exposure) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.unbake = amount < 0.0f ? 0.0f : amount > 1.0f ? 1.0f : amount;
    l.unbakeExposure = exposure > 0.0f ? exposure : 0.44f;
    hal::gs::set_lighting(l);
}
void api_set_dust(float amount) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.dust = amount > 0.0f ? amount : 0.0f;
    hal::gs::set_lighting(l);
}
void api_set_map_shadows(int on) {
    hal::gs::Lighting l = hal::gs::lighting();
    l.mapShadows = on != 0;
    hal::gs::set_lighting(l);
}
void* api_guest_ptr(uint32_t address) { return guest::ptr<void>(address); }
void* api_find(uint32_t address) { return guest::lookup(address); }
void* api_hook(uint32_t address, void* fn, const char* name) {
    void* was = guest::lookup(address);
    guest::bind(address, guest::Fn(fn), name != nullptr ? name : "mod hook");
    return was;
}
void api_on_frame(void (*fn)(void*), void* user) { sFrameHooks.push_back({fn, user}); }
uint16_t api_pad_buttons(int port) {
    return port >= 0 && port < 2 ? *guest::ptr<u16>(0x307140u + (u32)port * 0x80u + 0x08u) : 0;
}

void load_mod(const std::string& root, const std::string& id) {
    const std::string dir = root + "/" + id;
    std::map<std::string, std::string> ini;
    if (!read_ini(dir + "/mod.ini", ini))
        return;
    const auto get = [&](const char* key, const std::string& fallback) {
        const auto it = ini.find(key);
        return it != ini.end() ? it->second : fallback;
    };
    // (WOTS2_MOD_<NAME>=1 / 0 decides for one run whatever the file says.)
    std::string env = "WOTS2_MOD_" + id;
    for (char& c : env) c = (char)toupper((unsigned char)c);
    const char* forced = getenv(env.c_str());
    if (atoi(forced != nullptr ? forced : get("mod.enabled", "1").c_str()) == 0) {
        printf("[mods] %s: off\n", id.c_str());
        return;
    }
    Mod* mod = new Mod;
    mod->dir = dir;
    mod->id = id;
    mod->name = get("mod.name", id);
    for (const auto& [key, value] : ini)
        if (key.compare(0, 9, "settings.") == 0)
            mod->settings[key.substr(9)] = value;
    mod->api = {WOTS2_MOD_API_VERSION, mod->dir.c_str(), api_log, api_setting, api_get_video, api_set_video,
                api_guest_ptr, api_hook, api_find, api_on_frame, api_pad_buttons, api_get_lighting, api_set_lighting,
                api_set_map_shadows, api_set_bump, api_set_dust, api_set_unbake, api_set_model_shadow,
                api_set_lantern_shadows, api_set_weather,
                api_set_character_bump, api_set_god_rays, api_set_moods, api_set_mood};

    int parts = 0;
    if (is_dir(dir + "/files")) {
        archive::add_overlay((dir + "/files").c_str());
        parts++;
    }
    // A texture pack made for PCSX2: its folder of replacement .dds files
    // here as textures/, or wherever [mod] textures = says (relative to the mod).
    {
        std::string textures = get("mod.textures", "textures");
        if (!textures.empty() && textures[0] != '/' && !(textures.size() > 1 && textures[1] == ':'))
            textures = dir + "/" + textures;
        if (is_dir(textures)) {
            const int n = texture_pack::add_dir(textures);
            printf("[mods] %s: %d replacement textures\n", id.c_str(), n);
            parts += n > 0;
            if (n > 0) {
                sTexturePackDir = dir;
                // [settings] on = 0: the pack is there but a run starts without it (F9 switches).
                const char* onEnv = getenv((env + "_ON").c_str());     // (WOTS2_MOD_<NAME>_ON, as for a mod's settings)
                const std::string on = onEnv != nullptr ? onEnv : get("settings.on", "1");
                texture_pack::set_enabled(!(on == "0" || on == "off" || on == "no"));
            }
        }
    }
    const std::string library = dir + "/" + get("mod.library", id + WOTS2_MOD_SUFFIX);
    if (is_file(library)) {
        mod->library = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (mod->library == nullptr) {
            printf("[mods] %s: %s\n", id.c_str(), dlerror());
            delete mod;
            return;
        }
        auto init = reinterpret_cast<int (*)(const Wots2Api*)>(dlsym(mod->library, "wots2_mod_init"));
        const int answer = init != nullptr ? init(&mod->api) : -1;
        if (answer != 0) {
            printf("[mods] %s: %s\n", id.c_str(), init ? "its wots2_mod_init refused" : "no wots2_mod_init in its library");
            delete mod;         // (the library stays loaded: it may have hooked something already)
            return;
        }
        parts++;
    } else if (ini.count("mod.library")) {
        printf("[mods] %s: %s is not there (not built?)\n", id.c_str(), library.c_str());
    }
    if (parts == 0)
        printf("[mods] %s: nothing to load (no library, no files/, no textures/)\n", id.c_str());
    else
        printf("[mods] %s: loaded (%s)\n", id.c_str(), mod->name.c_str());
    sMods.push_back(mod);
}

}  // namespace

namespace mods {

void load_all() {
    const char* where = getenv("WOTS2_MODS");
    if (where != nullptr && (strcmp(where, "0") == 0 || where[0] == 0))
        return;
    if (where == nullptr && (getenv("WOTS2_INPUT") || getenv("WOTS2_TOUR") || getenv("WOTS2_EXIT_FRAME")))
        return;
    const std::string root = where != nullptr && strcmp(where, "1") != 0 ? where : "mods";
    DIR* d = opendir(root.c_str());
    if (d == nullptr)
        return;
    std::vector<std::string> ids;
    while (const dirent* e = readdir(d))
        if (e->d_name[0] != '.' && is_dir(root + "/" + e->d_name))
            ids.push_back(e->d_name);
    closedir(d);
    std::sort(ids.begin(), ids.end());
    for (const std::string& id : ids)
        load_mod(root, id);
}

void frame() {
    for (const FrameHook& hook : sFrameHooks)
        hook.fn(hook.user);
}

const char* texture_pack_dir() { return sTexturePackDir.c_str(); }

const char* dir_of(const char* id) {
    for (const Mod* mod : sMods)
        if (mod->id == id)
            return mod->dir.c_str();
    return "";
}

}  // namespace mods
