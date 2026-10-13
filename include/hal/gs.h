#pragma once
#include <vector>
#include "ps2_types.h"

// GS (Graphics Synthesizer) → OpenGL abstraction.
//
// For now this is a 2D layer: textured and flat quads in a 640×448 logical
// screen, with the handful of GS blend modes the menus use.  Colours use the
// GS convention: on textured draws 0x80 means 1.0 (RGB 0x80 leaves a texel
// unchanged); untextured draws take RGB as it is.  Alpha 0x80 is always 1.0.

namespace hal::gs {

void init(int width, int height, const char* title);
void shutdown();

void begin_frame();
void end_frame();   // swap buffers, hold the frame rate
void set_frame_rate(int fps);
void clear(u32 rgba);

enum class Blend {
    Normal,        // GS ALPHA 0x44: (Cs - Cd) * As + Cd
    Additive,      // GS ALPHA 0x48: Cs * As + Cd
    Subtractive,   // GS ALPHA 0x42: Cd - Cs * As
};
void set_blend(Blend mode);
// The GS depth (24 bits, larger = nearer) of the next 2D primitive, which is
// then tested against the frame's depth buffer ("greater or equal", the
// game's standing TEST setting) and written to it.  It applies to one draw:
// what does not say is drawn over everything, untested.
void set_z(u32 z);
void set_alpha_ref(int ref);   // pixels with alpha below ref/128 are discarded

u32  create_texture(const u8* rgba, int width, int height);
void destroy_texture(u32 tex);
// A copy of the frame as drawn so far, as an opaque texture of the screen's
// size (its texels are the frame's pixels, top row first).  What the PS2
// does when a game uses its frame buffer as a texture: ripples, heat haze.
u32  grab_frame();
// The pixels of the frame shown last (the one on the screen while the next
// is being made): RGBA, top row first.  For effects that work on the picture itself.
void read_frame(std::vector<u8>& rgba);
// Draws into a square texture of its own instead of the frame (cleared to
// transparent black, with its own depth buffer; no fog) until end_target,
// which gives the texture and goes back to the frame.
void begin_target(int size);
u32  end_target();

// x/y/w/h in logical pixels; u/v in texels of a tex_w × tex_h texture.
void draw_sprite(u32 tex, int tex_w, int tex_h,
                 float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, u32 rgba);
void fill_rect(float x, float y, float w, float h, u32 rgba);

// Arbitrary primitives.  u/v are in texels of a tex_w × tex_h texture; pass
// tex 0 for flat-shaded geometry.
struct Vtx { float x, y, u, v; u32 rgba; };
enum class Prim { Lines, Triangles, TriangleStrip, TriangleFan };
void draw_prim(u32 tex, int tex_w, int tex_h, Prim prim, const Vtx* verts, int count);
// The same, but tested against the frame's 3D depth buffer (not written to
// it): `depth` is one value per vertex, −1 nearest … 1 farthest, as the 3D
// path leaves them.  What the game draws into the world as screen-space
// primitives it projected itself: shadows, blood, dust.
void draw_prim_depth(u32 tex, int tex_w, int tex_h, Prim prim, const Vtx* verts, const float* depth, int count);

// 3D: depth-tested, perspective geometry.  `mvp` is 16 floats in the game's
// row-vector convention (clip = position × mvp), clip space as in OpenGL.
// u/v are normalised (0–1 across the texture).  Texels with alpha below one
// half are discarded.
struct Vtx3 { float x, y, z, u, v; u32 rgba; };
void begin_3d(const float* mvp);
void draw_mesh(u32 tex, const Vtx3* verts, int count, Prim prim);
void end_3d();
// Between begin_3d and end_3d.  A depth bias (in the smallest steps the depth
// buffer resolves; negative = nearer) settles which of two surfaces in one
// plane shows; an overlay is blended over what is there without hiding
// anything drawn later.
void set_depth_bias(int steps);
// Depth fog for what is drawn in 3D from now on (see gs.cpp).
void set_fog(bool on, float fogAt = 0, float clearAt = 0, float nearDepth = 0, float farDepth = 0, u32 rgb = 0);
void set_overlay(bool on);

// ── What mods may change about the picture (all off by default: the game as
// it was on the PS2).  None of it touches the game: every frame the game
// draws is recorded as a list of draws and played into a frame of the size
// asked for, and for a higher frame rate played again in between with every
// position part of the way from the frame before (see gs.cpp).
struct Video {
    float aspect = 0.0f;   // 0: the 4:3 frame, stretched to the window; > 0: that shape (16:9 = 1.7778); < 0: the window's
    int   height = 448;    // lines drawn; 0 = as many as the window has
    int   fps = 0;         // pictures shown a second: 0 = the game's own 30; < 0 = the display's rate; else that many
    bool  vsync = true;
    bool  fullscreen = false;
    int   windowWidth = 0, windowHeight = 0;    // 0 = leave the window as it is
    bool  hudEdges = false; // a wider picture: the in-world HUD keeps to its edges, not to the 4:3 middle
};
void  set_video(const Video& video);
Video video();
void  toggle_fullscreen();
// How far (logical pixels) the picture now reaches beyond each side of the
// game's 640-wide screen: 0 unless a wider shape was asked for.
float wide_margin();
// The next 2D draw, if it spans the game's screen, is stretched to the edges
// of a wider picture, texture and all (flat ones and ones textured from the
// frame are without asking).
void  set_full_width();

int frame_count();   // frames presented so far

// Screen dimensions (PS2 default: 640×448 for NTSC)
int screen_width();
int screen_height();

}  // namespace hal::gs
