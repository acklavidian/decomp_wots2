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
    const bool mapShadows = flag("map_shadows", 1);
    if (api->version >= 3 && api->set_map_shadows)
        api->set_map_shadows(mapShadows);
    const float bump = number("bump", 0.0f);
    if (api->version >= 3 && api->set_bump)
        api->set_bump(bump, number("bump_depth", 4.0f));
    if (api->version >= 3 && api->set_dust)
        api->set_dust(number("dust", 1.0f));
    if (api->version >= 8 && api->set_character_bump) {
        api->set_character_bump(number("character_bump", 0.5f));
        api->set_god_rays(number("god_rays", 0.6f));
    }
    if (api->version >= 7 && api->set_weather) {
        const char* w = api->setting(api, "weather", "auto");
        const int mode = strcmp(w, "rain") == 0 ? 2 : strcmp(w, "clear") == 0 ? 1 : 0;
        api->set_weather(mode, number("rain_chance", 0.3f), number("rain_amount", 1.0f));
    }
    if (api->version >= 6 && api->set_lantern_shadows)
        api->set_lantern_shadows(flag("lantern_shadows", 1));
    if (api->version >= 5 && api->set_model_shadow)
        api->set_model_shadow(number("character_shadow", 0.6f));
    if (api->version >= 4 && api->set_unbake)
        api->set_unbake(number("unbake", 0.0f), number("unbake_exposure", 0.44f));
    api->log(api, "shadows %s (the map's too: %s), ambient occlusion %s, lanterns %s, bloom %.2f, grade %s",
             l.shadows ? "on" : "off", mapShadows ? "yes" : "no", l.ao ? "on" : "off", l.lanterns ? "on" : "off", l.bloom, l.grade ? "on" : "off");
    return 0;
}
