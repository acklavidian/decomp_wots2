#pragma once
#include <vector>
#include "ps2_types.h"

// GS (Graphics Synthesizer) → OpenGL abstraction.
//
// For now this is a 2D layer: textured and flat quads in a 640×448 logical
// screen, with the handful of GS blend modes the menus use.  Colours use the
// GS convention: on textured draws 0x80 means 1.0 (RGB 0x80 leaves a texel
// unchanged); untextured draws take RGB as it is.  Alpha 0x80 is always 1.0.

struct SDL_Window;

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
// A block-compressed texture (texture packs: DDS files), its mipmaps from
// the largest down.
enum class Compressed { DXT1, DXT3, DXT5 };
struct Level { int width, height; const u8* data; size_t size; };
u32  create_texture_compressed(Compressed format, const Level* levels, int count);
// Says that a texture is a cut-out (every texel clear or opaque; worked out by
// itself for create_texture): in 3D it is drawn opaque where it is not cut.
void set_cutout(u32 tex, bool on);
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
// What a view draws, for the modern lighting (set_lighting): the map and the
// models in it (characters, swords, objects) are in world coordinates under
// the world's view; everything else is left as it is.
enum class View : u8 { Other, Sky, Map, Model };
// `who` names what is drawn (a character, an object…) for the pictures made
// in between frames: the view is paired only with the same `who` of the
// frame before (0 = not said: paired by what its meshes are and their order).
void begin_3d(const float* mvp, View kind = View::Other, u32 who = 0);
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
// Only the rate and vsync (F6, the panel): the window is left where it is.
void  set_rate(int fps, bool vsync);
// Photo mode: the frame as the game drew it (no overlay) saved as a PNG at
// the end of this frame; and "the game is held still" (the dust stops too).
void  save_picture(const char* path);
void  set_frozen(bool frozen);
Video video();
void  toggle_fullscreen();
// How far (logical pixels) the picture now reaches beyond each side of the
// game's 640-wide screen: 0 unless a wider shape was asked for.
float wide_margin();
// The next 2D draw, if it spans the game's screen, is stretched to the edges
// of a wider picture, texture and all (flat ones and ones textured from the
// frame are without asking).
void  set_full_width();

// ── Modern lighting (a mod's choice; all off by default).  Played on top of
// what the game draws, in the world only:
//   shadows      the sun's shadows of characters, swords and objects on the
//                map and on each other (a shadow map from the map's first light)
//   ao           ambient occlusion from the depth buffer: corners, the feet
//                on the ground, under eaves
//   lanterns     the map's point lights (lanterns at night, the flash of a
//                blow) light the ground round them, per pixel
//   bloom        bright things glow
//   grade        a gentle contrast curve, saturation, warmth and vignette
struct Lighting {
    bool  shadows = false;
    bool  mapShadows = false;       // the map's own geometry casts them too (its lighting is baked)
    float modelShadow = 0.6f;       // how dark the sun's shadow falls on characters and things, against the map's
    float bump = 0.0f;              // bump mapping of the map's surfaces, heights from brightness: 0 = off
    float bumpDepth = 4.0f;         // how steep a step of brightness is
    float dust = 0.0f;
    float unbake = 0.0f;            // the map relit (ambient + sun + real shadows) instead of its baked light: 0 … 1
    float unbakeExposure = 0.44f;    // brightness of the relit map
    float dof = 0.0f;               // depth of field (photo mode): 0 = off, 1 = strong
    float dofFocus = 0.0f;          // distance in focus; 0 = whatever is in the middle of the picture              // wind and dust: motes in the air, dust swept up by gusts; 0 = off
    float shadowStrength = 0.55f;
    int   shadowSize = 2048;        // texels each way
    float shadowReach = 16.0f;      // units from the middle of the view each way
    bool  ao = false;
    float aoStrength = 1.0f, aoRadius = 0.35f;
    bool  lanterns = false;
    float lanternStrength = 1.0f, lanternRadius = 5.0f;
    bool  lanternShadows = false;   // the lanterns nearest the player cast characters' shadows, and light them
    float bloom = 0.0f;             // 0 = off
    float bloomThreshold = 0.78f;
    bool  grade = false;
    float contrast = 0.25f, saturation = 1.1f, warmth = 0.0f, vignette = 0.25f;
};
void     set_lighting(const Lighting& lighting);
Lighting lighting();
// F3: hold the lighting back (or let it be again) without forgetting it → on now.
bool     toggle_lighting();
bool     lighting_held();       // F3 has it held back
// The world's view this frame (MapState_Draw): its world→camera and
// camera→clip matrices (row-vector convention), the direction towards the
// sun and its colour, and the point lights: n × {x, y, z, radius, r, g, b, 0}.
void set_world(const float* view, const float* proj, const float* sunDir, const float* sunColour,
               const float* lights, int n,
               const float* ambient = nullptr);
// What the wind and dust need of a world frame (MapState_Draw): the game's
// wind (the gust vector it gives cloth and smoke), where the player is, and
// whether this is an interior.
void set_ambience(const float* wind, const float* focus, bool indoors, bool night = false);
// The world is drawn (World_Draw, before the HUD): ambient occlusion, bloom
// and the grade are applied here.
void world_post();
// True when the sun's shadows are being drawn in this frame (the game's own blob and
// projected shadows then step aside).
bool real_shadows();

int frame_count();   // frames presented so far

// For a tool drawn over the picture (the level editor, native/editor.cpp):
// the window and its GL context; a function called when each picture is on
// the window, just before it is shown; where on the window (in drawable
// pixels: x0, y0, x1, y1) the frame lies; and by how much 3D clip x is
// narrowed in a frame wider than the game's screen.
SDL_Window* window();
void* gl_context();
void  set_overlay(void (*draw)());
void  present_rect(int out[4]);
float clip_x_scale();

// Screen dimensions (PS2 default: 640×448 for NTSC)
int screen_width();
int screen_height();

}  // namespace hal::gs
