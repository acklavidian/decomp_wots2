// Resolution: asks for the picture to be drawn with more lines than the
// PS2's 448, and sets the window up.  The work is the game's graphics
// layer's (hal::gs::Video::height).
#include "wots2_mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wots2_mod_init(const Wots2Api* api) {
    Wots2Video video;
    api->get_video(&video);

    const char* internal = api->setting(api, "internal", "window");
    int lines = 0, times = 0;
    char x = 0;
    if (strcmp(internal, "window") == 0 || strcmp(internal, "auto") == 0)
        video.height = 0;
    else if (sscanf(internal, "%d%c", &times, &x) == 2 && (x == 'x' || x == 'X') && times >= 1 && times <= 8)
        video.height = 448 * times;
    else if (sscanf(internal, "%d", &lines) == 1 && lines >= 224 && lines <= 4320)
        video.height = lines;
    else
        api->log(api, "internal = %s: not a size (window, 1080, 2x ...): left as it is", internal);

    const char* window = api->setting(api, "window", "default");
    int w = 0, h = 0;
    if (sscanf(window, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 224) {
        video.window_width = w;
        video.window_height = h;
    } else if (strcmp(window, "default") != 0) {
        api->log(api, "window = %s: not WIDTHxHEIGHT: left as it is", window);
    }
    video.fullscreen = atoi(api->setting(api, "fullscreen", "0")) != 0;
    api->set_video(&video);
    if (video.height > 0)
        api->log(api, "%d lines", video.height);
    else
        api->log(api, "as many lines as the window has");
    return 0;
}
