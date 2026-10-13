#include "hal/pad.h"
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "hal/gs.h"

// Current button state per port (PS2 convention: 0 = pressed)
static u16 s_buttons[2] = { 0xFFFF, 0xFFFF };
static u8  s_lx[2] = { 128, 128 }, s_ly[2] = { 128, 128 };
static u8  s_rx[2] = { 128, 128 }, s_ry[2] = { 128, 128 };

// Keyboard → PS2 button map (port 0)
struct KeyMap { SDL_Keycode key; hal::pad::Button btn; };
static constexpr KeyMap KEY_MAP[] = {
    { SDLK_RETURN,    hal::pad::START    },
    { SDLK_RSHIFT,    hal::pad::SELECT   },
    { SDLK_UP,        hal::pad::UP       },
    { SDLK_DOWN,      hal::pad::DOWN     },
    { SDLK_LEFT,      hal::pad::LEFT     },
    { SDLK_RIGHT,     hal::pad::RIGHT    },
    { SDLK_z,         hal::pad::CROSS    },
    { SDLK_x,         hal::pad::CIRCLE   },
    { SDLK_a,         hal::pad::SQUARE   },
    { SDLK_s,         hal::pad::TRIANGLE },
    { SDLK_q,         hal::pad::L1       },
    { SDLK_w,         hal::pad::R1       },
    { SDLK_e,         hal::pad::L2       },
    { SDLK_r,         hal::pad::R2       },
};

// Debug aid: the left stick held at (x, y) for the next two polls.
static u8  s_debugStick[2] = {0x80, 0x80};
static int s_debugStickFrames = 0;
namespace hal::pad {
void debug_stick(u8 x, u8 y) {
    s_debugStick[0] = x;
    s_debugStick[1] = y;
    s_debugStickFrames = 2;
}
}  // namespace hal::pad

// Debug aid: WOTS2_INPUT="350:start,420:down,450:cross" presses each button
// for three frames starting at the given frame; "600-700:up" holds one down
// over a range of frames.
struct ScriptedPress { int frame, end; hal::pad::Button btn; };
static std::vector<ScriptedPress> s_script;

static void load_script() {
    const char* e = getenv("WOTS2_INPUT");
    if (!e)
        return;
    static const struct { const char* name; hal::pad::Button btn; } kNames[] = {
        {"start", hal::pad::START}, {"select", hal::pad::SELECT}, {"up", hal::pad::UP},
        {"down", hal::pad::DOWN}, {"left", hal::pad::LEFT}, {"right", hal::pad::RIGHT},
        {"cross", hal::pad::CROSS}, {"circle", hal::pad::CIRCLE}, {"square", hal::pad::SQUARE},
        {"triangle", hal::pad::TRIANGLE}, {"l1", hal::pad::L1}, {"r1", hal::pad::R1},
        {"l2", hal::pad::L2}, {"r2", hal::pad::R2},
    };
    std::string spec = e;
    if (spec.size() > 1 && spec[0] == '@') {            // "@file": the list is in a file (a long run's does not fit the environment)
        std::string text;
        if (FILE* f = fopen(spec.c_str() + 1, "rb")) {
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
                text.append(buf, n);
            fclose(f);
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        spec = text;
    }
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        std::string item = spec.substr(pos, end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            for (const auto& n : kNames)
                if (item.substr(colon + 1) == n.name) {
                    const int from = atoi(item.c_str());
                    const size_t dash = item.find('-');
                    const int to = dash != std::string::npos && dash < colon ? atoi(item.c_str() + dash + 1) + 1 : from + 3;
                    s_script.push_back({from, to, n.btn});
                }
        pos = end + 1;
    }
}

// Game controllers (SDL's "game controller" layout, so any pad SDL knows is a
// DualShock here: the buttons by their place, not their letters).  Up to
// two; opened as they are plugged in.
static SDL_GameController* s_pads[2] = {nullptr, nullptr};
static int s_scanTimer = 0;

static void scan_controllers() {
    // A scripted run (debug) is steered by its script alone: a controller
    // that happens to be plugged in — and in somebody's hands — stays out.
    static const bool scripted = getenv("WOTS2_INPUT") || getenv("WOTS2_TOUR") || getenv("WOTS2_EXIT_FRAME");
    if (scripted)
        return;
    for (SDL_GameController*& pad : s_pads)
        if (pad && !SDL_GameControllerGetAttached(pad)) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
        }
    if (s_pads[0] && s_pads[1])
        return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i))
            continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool open = false;
        for (SDL_GameController* pad : s_pads)
            if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id)
                open = true;
        if (open)
            continue;
        for (SDL_GameController*& pad : s_pads)
            if (!pad) {
                pad = SDL_GameControllerOpen(i);
                if (pad)
                    printf("[PAD] port %d: %s\n", (int)(&pad - s_pads), SDL_GameControllerName(pad));
                break;
            }
    }
}

// A stick as the two bytes a DualShock 2 gives (0x80 = centred).  A modern
// stick moves in a circle: pushed home on a diagonal each axis reads only
// 71 %, short of the 75 % the game takes for "run" (it tests the axes one by
// one, 0x1C76B0), where the PS2's stick reads nearly full on both.  So the
// circle is stretched toward the square, by up to 1.3.
static void stick_bytes(SDL_GameController* pad, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, u8* bx, u8* by) {
    float x = SDL_GameControllerGetAxis(pad, ax) / 32767.0f, y = SDL_GameControllerGetAxis(pad, ay) / 32767.0f;
    const float len = sqrtf(x * x + y * y);
    *bx = *by = 0x80;
    if (len < 0.21f)                    // a worn stick does not rest at the centre
        return;
    const float most = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y);
    float gain = len / most;
    if (gain > 1.3f) gain = 1.3f;
    x *= gain; y *= gain;
    x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
    y = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
    *bx = (u8)(0x80 + (int)lroundf(x * 127.0f));
    *by = (u8)(0x80 + (int)lroundf(y * 127.0f));
}

static u8 axis_byte(SDL_GameController* pad, SDL_GameControllerAxis axis) {
    const int v = SDL_GameControllerGetAxis(pad, axis);
    if (v > -7000 && v < 7000)          // a worn stick does not rest at the centre
        return 0x80;
    return (u8)((v + 32768) >> 8);
}

namespace hal::pad {

void init()     { SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER); load_script(); }
void shutdown() {
    for (SDL_GameController*& pad : s_pads)
        if (pad) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
        }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void poll() {
    if (s_scanTimer-- <= 0) {
        s_scanTimer = 60;
        scan_controllers();
    }
    const u8* keys = SDL_GetKeyboardState(nullptr);
    u16 pressed = 0xFFFF;
    for (const auto& km : KEY_MAP) {
        SDL_Scancode sc = SDL_GetScancodeFromKey(km.key);
        if (keys[sc])
            pressed &= ~static_cast<u16>(km.btn);  // 0 = pressed
    }
    int frame = hal::gs::frame_count();
    for (const auto& sp : s_script)
        if (frame >= sp.frame && frame < sp.end)
            pressed &= ~static_cast<u16>(sp.btn);
    s_buttons[0] = pressed;
    s_buttons[1] = 0xFFFF;

    // The sticks from the keyboard: T / G / F / H tilt the left one part of
    // the way (all the way with Left Shift), J / L and I / K push the right.
    for (int port = 0; port < 2; port++)
        s_lx[port] = s_ly[port] = s_rx[port] = s_ry[port] = 0x80;
    const int tilt = keys[SDL_SCANCODE_LSHIFT] ? 0x7F : 0x48;
    if (keys[SDL_SCANCODE_F]) s_lx[0] = (u8)(0x80 - tilt);
    if (keys[SDL_SCANCODE_H]) s_lx[0] = (u8)(0x80 + tilt);
    if (keys[SDL_SCANCODE_T]) s_ly[0] = (u8)(0x80 - tilt);
    if (keys[SDL_SCANCODE_G]) s_ly[0] = (u8)(0x80 + tilt);
    if (keys[SDL_SCANCODE_J]) s_rx[0] = 0x00;
    if (keys[SDL_SCANCODE_L]) s_rx[0] = 0xFF;
    if (keys[SDL_SCANCODE_I]) s_ry[0] = 0x00;
    if (keys[SDL_SCANCODE_K]) s_ry[0] = 0xFF;
    // Debug: WOTS2_LSTICK="x,y" (0–255) holds the left stick somewhere.
    // ("from:x,y" only from that frame on.)
    if (const char* fixed = getenv("WOTS2_LSTICK")) {
        int from = 0, fx = 0x80, fy = 0x80;
        if (strchr(fixed, ':'))
            sscanf(fixed, "%d:%d,%d", &from, &fx, &fy);
        else
            sscanf(fixed, "%d,%d", &fx, &fy);
        if (frame >= from) {
            s_lx[0] = (u8)fx;
            s_ly[0] = (u8)fy;
        }
    }

    // Debug: a stick position some hook asked for (see debug_stick).
    if (s_debugStickFrames > 0) {
        s_debugStickFrames--;
        s_lx[0] = s_debugStick[0];
        s_ly[0] = s_debugStick[1];
    }

    static constexpr struct { SDL_GameControllerButton from; Button to; } kButtons[] = {
        {SDL_CONTROLLER_BUTTON_A, CROSS}, {SDL_CONTROLLER_BUTTON_B, CIRCLE},
        {SDL_CONTROLLER_BUTTON_X, SQUARE}, {SDL_CONTROLLER_BUTTON_Y, TRIANGLE},
        {SDL_CONTROLLER_BUTTON_BACK, SELECT}, {SDL_CONTROLLER_BUTTON_START, START},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, L3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, R3},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, L1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, R1},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RIGHT},
    };
    for (int port = 0; port < 2; port++) {
        SDL_GameController* pad = s_pads[port];
        if (!pad)
            continue;
        for (const auto& b : kButtons)
            if (SDL_GameControllerGetButton(pad, b.from))
                s_buttons[port] &= ~static_cast<u16>(b.to);
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000)
            s_buttons[port] &= ~static_cast<u16>(L2);
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000)
            s_buttons[port] &= ~static_cast<u16>(R2);
        u8 lx, ly, rx, ry;
        stick_bytes(pad, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &lx, &ly);
        stick_bytes(pad, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &rx, &ry);
        if (lx != 0x80) s_lx[port] = lx;        // (a key held wins over a stick at rest)
        if (ly != 0x80) s_ly[port] = ly;
        if (rx != 0x80) s_rx[port] = rx;
        if (ry != 0x80) s_ry[port] = ry;
    }
}

u16 get_buttons(u32 port) { return port < 2 ? s_buttons[port] : 0xFFFF; }
u8  get_left_x(u32 port)  { return port < 2 ? s_lx[port] : 128; }
u8  get_left_y(u32 port)  { return port < 2 ? s_ly[port] : 128; }
u8  get_right_x(u32 port) { return port < 2 ? s_rx[port] : 128; }
u8  get_right_y(u32 port) { return port < 2 ? s_ry[port] : 128; }

bool connected(u32 port) { return port == 0 || (port < 2 && s_pads[port] != nullptr); }

void rumble(u32 port, bool small, u8 big) {
    if (port >= 2 || !s_pads[port])
        return;
    SDL_GameControllerRumble(s_pads[port], (Uint16)(big * 257), small ? 0xFFFF : 0, small || big ? 250 : 0);
}

}  // namespace hal::pad
