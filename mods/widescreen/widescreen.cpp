// Widescreen: asks for a picture of another shape than the PS2's 4:3.
// The work is the game's graphics layer's (hal::gs::Video::aspect).
#include "wots2_mod.h"
#include <stdio.h>
#include <string.h>

int wots2_mod_init(const Wots2Api* api) {
    const char* text = api->setting(api, "aspect", "auto");
    float aspect = -1.0f;                       // the window's
    float w = 0.0f, h = 0.0f;
    if (strcmp(text, "auto") == 0)
        aspect = -1.0f;
    else if (sscanf(text, "%f:%f", &w, &h) == 2 && h > 0.0f)
        aspect = w / h;
    else if (sscanf(text, "%f", &w) == 1 && w > 0.0f)
        aspect = w;
    else
        api->log(api, "aspect = %s: not a shape (auto, 16:9, 1.7778 ...): using the window's", text);
    if (aspect > 0.0f && aspect < 4.0f / 3.0f) {
        api->log(api, "aspect = %s is narrower than 4:3: using 4:3", text);
        aspect = 4.0f / 3.0f;
    }
    Wots2Video video;
    api->get_video(&video);
    video.aspect = aspect;
    video.hud_edges = strcmp(api->setting(api, "hud", "edges"), "edges") == 0;
    // (A window of the right shape to begin with, if nobody chose one.)
    if (video.window_width == 0 && !video.fullscreen) {
        video.window_height = 720;
        video.window_width = (int)(720.0f * (aspect > 0.0f ? aspect : 16.0f / 9.0f) + 0.5f);
    }
    api->set_video(&video);
    if (aspect > 0.0f)
        api->log(api, "aspect %.4f", aspect);
    else
        api->log(api, "the window's shape");
    return 0;
}
