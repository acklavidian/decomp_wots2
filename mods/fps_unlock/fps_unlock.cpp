// FPS unlock: asks for more pictures a second than the game's thirty.  The
// work is the game's graphics layer's (hal::gs::Video::fps): it shows the
// frames in between by moving everything part of the way.
#include "wots2_mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wots2_mod_init(const Wots2Api* api) {
    Wots2Video video;
    api->get_video(&video);
    const char* fps = api->setting(api, "fps", "display");
    int n = 0;
    if (strcmp(fps, "display") == 0 || strcmp(fps, "auto") == 0)
        video.fps = -1;
    else if (sscanf(fps, "%d", &n) == 1 && n >= 30 && n <= 480)
        video.fps = n;
    else
        api->log(api, "fps = %s: not a rate (display, 60, 120 ...): left as it is", fps);
    video.vsync = atoi(api->setting(api, "vsync", "1")) != 0;
    api->set_video(&video);
    if (video.fps < 0)
        api->log(api, "the display's rate%s", video.vsync ? "" : ", not waiting for it");
    else
        api->log(api, "%d pictures a second%s", video.fps, video.vsync ? "" : ", not waiting for the display");
    return 0;
}
