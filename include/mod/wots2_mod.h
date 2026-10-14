/* Way of the Samurai 2 (native) — the interface a mod is given.
 *
 * A mod is a directory under mods/ holding a mod.ini and, if it wants any of
 * these, a shared library and a files/ directory:
 *
 *   mods/<name>/mod.ini      [mod] name, description, enabled (1 / 0), library
 *                            [settings] whatever the mod reads with setting()
 *   mods/<name>/<name>.so    code: exports  int wots2_mod_init(const Wots2Api*)
 *   mods/<name>/files/...    files that take the place of the game's own, by
 *                            the path the game asks for (Us/menu/menutext.txt,
 *                            Common/chara/list, ...)
 *
 * The library is built against this header alone.  Everything it may do goes
 * through the table of functions below; `api` stays valid for the whole run
 * and is the mod's own (its settings, its directory).
 */
#ifndef WOTS2_MOD_H
#define WOTS2_MOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WOTS2_MOD_API_VERSION 6      /* 2: get_lighting / set_lighting; 3: set_map_shadows, set_bump, set_dust; 4: set_unbake; 5: set_model_shadow; 6: set_lantern_shadows */

/* What of the picture a mod may change (see include/hal/gs.h, Video). */
typedef struct Wots2Video {
    float aspect;           /* 0: the PS2's 4:3 frame stretched over the window; > 0: that shape
                               (16:9 = 1.7778); < 0: the shape of the window */
    int   height;           /* lines drawn (the PS2: 448); 0 = as many as the window has */
    int   fps;              /* pictures shown a second: 0 = the game's own 30; < 0 = the display's
                               rate; else that many.  The game itself always runs at 30: the
                               pictures in between are made by moving everything part of the way. */
    int   vsync;            /* 1 = wait for the display */
    int   fullscreen;
    int   window_width, window_height;      /* 0 = leave the window alone */
    int   hud_edges;        /* a wider picture: 1 = gauges, enemy plates and the HUD's lines keep to
                               its edges; 0 = they stay where the 4:3 screen had them */
} Wots2Video;

/* Modern lighting, drawn over the game's own in the world (see
 * include/hal/gs.h, Lighting).  Strengths are 0 = none, 1 = as meant. */
typedef struct Wots2Lighting {
    int   shadows;          /* the sun's shadows of characters and things */
    float shadow_strength;  /* how dark (0.55) */
    int   shadow_size;      /* texels of the shadow map each way (2048) */
    float shadow_reach;     /* units round the middle of the view that get shadows (16) */
    int   ao;               /* ambient occlusion */
    float ao_strength, ao_radius;           /* (1, 0.35 units) */
    int   lanterns;         /* the map's point lights light the ground */
    float lantern_strength, lantern_radius; /* (1, 5 units) */
    float bloom;            /* glow of bright things: 0 = off (0.35) */
    float bloom_threshold;  /* (0.78) */
    int   grade;            /* contrast, saturation, warmth, vignette below */
    float contrast, saturation, warmth, vignette;
} Wots2Lighting;

typedef struct Wots2Api Wots2Api;
struct Wots2Api {
    int version;                        /* WOTS2_MOD_API_VERSION of the game */
    const char* mod_dir;                /* this mod's directory */

    void (*log)(const Wots2Api* api, const char* format, ...);

    /* A value from [settings] of the mod's mod.ini (or of the environment:
     * WOTS2_MOD_<NAME>_<KEY>, upper case), else `fallback`. */
    const char* (*setting)(const Wots2Api* api, const char* key, const char* fallback);

    /* The picture. */
    void (*get_video)(Wots2Video* out);
    void (*set_video)(const Wots2Video* video);

    /* The game.  Its memory is the PS2's, at the PS2's addresses: guest_ptr
     * turns one into a pointer.  Its functions are known by their PS2
     * addresses too (config/functions.json): `hook` puts `fn` in
     * the place of the function at `address` and answers what was there, to
     * call on to (0 if nothing was).  The signature is the original's, with
     * guest pointers as host pointers — see the decompiled source. */
    void* (*guest_ptr)(uint32_t address);
    void* (*hook)(uint32_t address, void* fn, const char* name);
    void* (*find)(uint32_t address);

    /* `fn(user)` once for every frame of the game (thirty a second), before
     * the frame is shown. */
    void (*on_frame)(void (*fn)(void* user), void* user);

    /* The pad as the game sees it: the buttons held (bit set = down; the
     * masks of include/engine/pad.h) and the sticks (0x80 = centred). */
    uint16_t (*pad_buttons)(int port);

    /* Version 2: modern lighting. */
    void (*get_lighting)(Wots2Lighting* out);
    void (*set_lighting)(const Wots2Lighting* lighting);

    /* Version 3: the map's own geometry (carts, eaves, trees…) casts the sun's
     * shadows too, not only characters and things.  Its lighting is baked, so
     * this darkens what it already shades. */
    void (*set_map_shadows)(int on);
    /* Bump mapping of the map's surfaces, their heights read from the
     * textures' brightness: strength 0 = off (about 1 = as meant), depth how
     * steep a step of brightness is (4). */
    void (*set_bump)(float strength, float depth);
    /* Wind and dust: motes in the air and dust swept up by the game's gusts;
     * 0 = none, 1 = as meant. */
    void (*set_dust)(float amount);
    /* Version 4: the map relit (its ambient, its sun, the real shadows)
     * instead of the light painted into it: 0 = baked … 1 = relit; exposure
     * its brightness (0.44).  By day and out of doors only. */
    void (*set_unbake)(float amount, float exposure);
    /* Version 5: how dark the sun's shadow falls on characters and things,
     * against on the map (0.6; 1 = as dark as the map's). */
    void (*set_model_shadow)(float share);
    /* Version 6: the lanterns nearest the player cast the characters' shadows
     * on the ground and light the characters too. */
    void (*set_lantern_shadows)(int on);
};

/* Exported by the mod.  0 = loaded; anything else: the mod is left out. */
int wots2_mod_init(const Wots2Api* api);

#ifdef __cplusplus
}
#endif
#endif
