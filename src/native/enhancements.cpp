// The "Enhancements" page (native, not the game's): R1 on the Settings screen
// opens it over the screen.  The port's own settings — the picture's shape and
// lines, the frame rate, an HD texture pack, the modern lighting — in the
// game's font, driven by the pad like the game's menus: Up / Down choose,
// Left / Right (or Cross) change, Triangle goes back.  Changes apply at once;
// leaving writes them into the mods' mod.ini files (editor::save_graphics).

#include "engine/bgm.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "hal/gs.h"
#include "native/editor.h"
#include "native/guest.h"
#include "native/texture_pack.h"

#include <cmath>
#include <cstdio>
#include <vector>

extern "C" void GsDevice_ClearScreenColor(GsDevice* dev, u32 rgba);    // 0x12F1F0

namespace {

const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
const GuestPtrVar<Font>     gFont{0x2EE67Cu};

enum : u32 {
    kZPanel = 0xFFFFC0, kZText = 0xFFFFC4,
    kWhite = 0x80FFFFFFu, kGold = 0x8060D0FFu, kGrey = 0x80909090u,
};
enum : u16 { kUp = 0x1000, kRight = 0x2000, kDown = 0x4000, kLeft = 0x8000, kCross = 0x40, kTriangle = 0x10 };

struct Row {
    const char* label;
    std::vector<const char*> choices;
    int  (*get)();              // the choice that is in effect now
    void (*set)(int choice);
    bool (*shown)() = nullptr;  // greyed out when false
};

int nearest(float value, std::initializer_list<float> options) {
    int best = 0, i = 0;
    float gap = 1e9f;
    for (float o : options) {
        if (fabsf(o - value) < gap) { gap = fabsf(o - value); best = i; }
        i++;
    }
    return best;
}

hal::gs::Lighting L() { return hal::gs::lighting(); }
void put(const hal::gs::Lighting& l) { hal::gs::set_lighting(l); }

const float kAspect[] = {0.0f, -1.0f, 16.0f / 9.0f, 21.0f / 9.0f};
const int   kLines[]  = {448, 0, 720, 1080, 1440, 2160};
const int   kRates[]  = {0, 60, 120, 144, -1};
const float kHalf[]   = {0.0f, 0.5f, 1.0f};
const float kDust[]   = {0.0f, 0.5f, 1.0f, 2.0f};
const float kBloom[]  = {0.0f, 0.3f, 0.58f, 0.9f};

const std::vector<Row>& rows() {
    static const std::vector<Row> r = {
        {"Picture shape", {"4:3", "Window", "16:9", "21:9"},
         [] { const float a = hal::gs::video().aspect; return a < 0.0f ? 1 : nearest(a, {0.0f, -9.0f, 16.0f / 9.0f, 21.0f / 9.0f}); },
         [](int c) { hal::gs::Video v = hal::gs::video(); v.aspect = kAspect[c]; v.hudEdges = c != 0;
                     v.windowWidth = v.windowHeight = 0; hal::gs::set_video(v); }},
        {"Resolution", {"448 (PS2)", "Window", "720", "1080", "1440", "2160"},
         [] { const int h = hal::gs::video().height; return h <= 0 ? 1 : nearest((float)h, {448, -1e6f, 720, 1080, 1440, 2160}); },
         [](int c) { hal::gs::Video v = hal::gs::video(); v.height = kLines[c]; v.windowWidth = v.windowHeight = 0;
                     hal::gs::set_video(v); }},
        {"Frame rate", {"30", "60", "120", "144", "Display"},
         [] { const int f = hal::gs::video().fps; return f < 0 ? 4 : nearest((float)f, {0, 60, 120, 144, -1e6f}); },
         [](int c) { hal::gs::set_rate(kRates[c], hal::gs::video().vsync); }},
        {"HD textures", {"Off", "On"},
         [] { return texture_pack::enabled() ? 1 : 0; },
         [](int c) { texture_pack::set_enabled(c == 1); },
         [] { return texture_pack::active(); }},
        {"Modern lighting", {"Off", "On"},
         [] { return hal::gs::lighting_held() ? 0 : 1; },
         [](int c) { if ((c == 1) == hal::gs::lighting_held()) hal::gs::toggle_lighting(); }},
        {"Sun shadows", {"Off", "On"},
         [] { return L().shadows ? 1 : 0; },
         [](int c) { auto l = L(); l.shadows = c == 1; put(l); }},
        {"Characters in shade", {"Off", "Half", "Full"},
         [] { return nearest(L().groundLight, {0.0f, 0.5f, 1.0f}); },
         [](int c) { auto l = L(); l.groundLight = kHalf[c]; put(l); }},
        {"Weather", {"By itself", "Clear", "Rain"},
         [] { return L().weather; },
         [](int c) { auto l = L(); l.weather = c; put(l); }},
        {"Dust", {"Off", "Light", "Normal", "Dusty"},
         [] { return nearest(L().dust, {0.0f, 0.5f, 1.0f, 2.0f}); },
         [](int c) { auto l = L(); l.dust = kDust[c]; put(l); }},
        {"Bloom", {"Off", "Low", "Normal", "High"},
         [] { return nearest(L().bloom, {0.0f, 0.3f, 0.58f, 0.9f}); },
         [](int c) { auto l = L(); l.bloom = kBloom[c]; put(l); }},
        {"Moods", {"Off", "On"},
         [] { return L().moods ? 1 : 0; },
         [](int c) { auto l = L(); l.moods = c == 1; put(l); }},
        {"Back", {}, nullptr, nullptr},
    };
    return r;
}

int  sCursor = 0;
bool sChanged = false;

std::vector<u16> wide(const char* text) {
    std::vector<u16> out;
    for (const char* p = text; *p; p++)
        out.push_back((u16)(u8)*p);
    out.push_back(0);
    return out;
}
void text(const char* s, int x, int y, u32 colour, float scale = 1.0f) {
    Font* font = *gFont;
    const std::vector<u16> w = wide(s);
    Font_SetColor(font, colour);
    Font_DrawString(font, w.data(), (int)Screen_X(x * 16, 0), (int)Screen_Y(y * 16, 0), kZText, scale, 0.0f);
}
int text_width(const char* s, float scale = 1.0f) {
    Font* font = *gFont;
    const std::vector<u16> w = wide(s);
    Font_Measure(font, w.data(), scale);
    return Font_GetExtent(font)->w / 16;
}
void box(int x0, int y0, int x1, int y1, u32 rgba) {     // pixels from the centre, y down
    FrameRect r{x0, -y0, x1, -y1, kZPanel, 1, rgba};
    Gs_FillRect2(&r);
}

const int kTop = -170, kStep = 24, kLabelX = -230, kValueX = 60;

}  // namespace

extern "C" {

void Native_EnhOpen() {
    sCursor = 0;
    sChanged = false;
    Sound_PlaySE(5, 0, 0x3FFF);
}

// → 0 once the page is left.
int Native_EnhUpdate() {
    void* pad = PadMgr_Get();
    const u16 press = Pad_GetPressed(pad, 0) | Pad_GetRepeat(pad, 0);
    const auto& r = rows();
    const int count = (int)r.size();
    if (press & kTriangle) {
        if (sChanged)
            editor::save_graphics();
        Sound_PlaySE(7, 0, 0x3FFF);
        return 0;
    }
    if (press & (kUp | kDown)) {
        sCursor = (sCursor + ((press & kUp) ? count - 1 : 1)) % count;
        Sound_PlaySE(2, 0, 0x3FFF);
        return 1;
    }
    const Row& row = r[(size_t)sCursor];
    if (row.set == nullptr) {               // "Back"
        if (press & kCross) {
            if (sChanged)
                editor::save_graphics();
            Sound_PlaySE(7, 0, 0x3FFF);
            return 0;
        }
        return 1;
    }
    if (row.shown && !row.shown())
        return 1;
    int step = (press & kLeft) ? -1 : (press & (kRight | kCross)) ? 1 : 0;
    if (step != 0) {
        const int n = (int)row.choices.size();
        row.set((row.get() + step + n) % n);
        sChanged = true;
        Sound_PlaySE(3, 0, 0x3FFF);
    }
    return 1;
}

void Native_EnhDraw() {
    GsDevice_ClearScreenColor(*gGsDevice, 0x60000000u);
    box(-260, kTop - 50, 260, kTop + kStep * (int)rows().size() + 40, 0x60201810u);
    text("Enhancements", -text_width("Enhancements", 1.2f) / 2, kTop - 40, kWhite, 1.2f);
    const auto& r = rows();
    for (size_t i = 0; i < r.size(); i++) {
        const Row& row = r[i];
        const int y = kTop + (int)i * kStep + (row.set == nullptr ? 10 : 0);
        const bool here = (int)i == sCursor;
        const bool live = !row.shown || row.shown();
        if (here)
            box(-250, y - 2, 250, y + kStep - 4, 0x40306080u);
        const u32 colour = !live ? kGrey : here ? kGold : kWhite;
        text(row.label, kLabelX, y, colour);
        if (row.set != nullptr) {
            char value[48];
            if (!live) snprintf(value, sizeof value, "Not installed");
            else snprintf(value, sizeof value, "%s  %s  %s", here ? "<" : " ", row.choices[(size_t)row.get()], here ? ">" : " ");
            text(value, kValueX, y, colour);
        }
    }
    text("Up/Down: choose   Left/Right: change   Triangle: back", -text_width("Up/Down: choose   Left/Right: change   Triangle: back", 0.8f) / 2,
         kTop + kStep * (int)r.size() + 18, kGrey, 0.8f);
}

// The line on the Settings screen that says the page is there.
void Native_EnhHint() {
    const char* s = "R1: Enhancements";
    text(s, 300 - text_width(s, 0.9f), -210, 0x7F141414u, 0.9f);
}

}  // extern "C"
