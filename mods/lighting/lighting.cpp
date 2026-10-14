// Modern lighting: reads its settings and hands them to the game's graphics
// layer (hal::gs::Lighting), which does the work.
#include "wots2_mod.h"
#include <stdlib.h>
#include <string.h>

static const Wots2Api* game;
static float number(const char* key, float fallback) {
    const char* text = game->setting(game, key, 0);
    return text != 0 ? (float)atof(text) : fallback;
}
static int flag(const char* key, int fallback) {
    const char* text = game->setting(game, key, 0);
    if (text == 0)
        return fallback;
    return strcmp(text, "1") == 0 || strcmp(text, "on") == 0 || strcmp(text, "yes") == 0 || strcmp(text, "true") == 0;
}

int wots2_mod_init(const Wots2Api* api) {
    game = api;
    if (api->version < 2 || api->set_lighting == 0) {
        api->log(api, "this game is too old for modern lighting");
        return 1;
    }
    Wots2Lighting l;
    api->get_lighting(&l);
    l.shadows = flag("shadows", 1);
    l.shadow_strength = number("shadow_strength", 0.55f);
    const char* quality = api->setting(api, "shadow_quality", "high");
    l.shadow_size = strcmp(quality, "ultra") == 0 ? 4096 : strcmp(quality, "low") == 0 ? 1024
                  : strcmp(quality, "high") == 0 ? 2048 : atoi(quality) > 0 ? atoi(quality) : 2048;
    l.ao = flag("ao", 1);
    l.ao_strength = number("ao_strength", 1.0f);
    l.ao_radius = number("ao_radius", 0.35f);
    l.lanterns = flag("lanterns", 1);
    l.lantern_strength = number("lantern_strength", 0.8f);
    l.bloom = number("bloom", 0.35f);
    l.bloom_threshold = number("bloom_threshold", 0.78f);
    l.grade = flag("grade", 1);
    l.contrast = number("contrast", 0.25f);
    l.saturation = number("saturation", 1.1f);
    l.warmth = number("warmth", 0.0f);
    l.vignette = number("vignette", 0.25f);
    api->set_lighting(&l);
    api->log(api, "shadows %s, ambient occlusion %s, lanterns %s, bloom %.2f, grade %s",
             l.shadows ? "on" : "off", l.ao ? "on" : "off", l.lanterns ? "on" : "off", l.bloom, l.grade ? "on" : "off");
    return 0;
}
