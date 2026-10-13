# Mods

Each directory here is a mod.  The game loads every enabled one when it starts
(in the order of their names) and says so on the terminal:

```
[mod widescreen] the window's shape
[mods] widescreen: loaded (Widescreen)
```

Switch a mod off with `enabled = 0` in its `mod.ini`; change what it does under
`[settings]` there.  `WOTS2_MODS=0 ./build/wots2 extracted` runs without any,
`WOTS2_MODS=<directory>` takes them from somewhere else.

## The three that come with the game

| Mod | What it does | Settings |
|-----|--------------|----------|
| `widescreen` | The 3D view opened out to the sides — the same height of view, more to the left and right. | `aspect` = `auto` (the window's shape), `16:9`, `21:9`, a number; `hud` = `edges` (gauges, enemy plates and the HUD's text keep to the picture's edges) or `center` |
| `resolution` | Draws the picture with more lines than the PS2's 448. | `internal` = `window` (as many as the window has), a number of lines (`1080`), or `2x`, `3x`…; `window` = `1920x1080`; `fullscreen` = `1` |
| `fps_unlock` | Shows more than thirty pictures a second. | `fps` = `display` or a number (`60`, `120`…); `vsync` = `1` / `0` |

**F11** switches between the window and the whole screen at any time.

Good to know:

- **The game itself always runs at thirty frames a second** — its speed, its
  physics and its timing are tied to that.  `fps_unlock` makes the pictures in
  between by moving every character, the camera and the HUD part of the way
  from one frame to the next.  The price: what you see is a thirtieth of a
  second behind what you pressed.  Turn it off if that bothers you.
- With `widescreen`, menus, story text and movies stay as the 4:3 screens they
  were drawn as, in the middle; only the 3D world (and fades, the pause
  menu's dimming, the letterbox bars) fills the width.
- All three only change how the finished frame is drawn.  With them off the
  picture is pixel for pixel what it was.

## Making a mod

```
mods/mymod/mod.ini         required
mods/mymod/mymod.cpp       optional: code, built into mymod.so by `cd build && cmake .. && ninja`
mods/mymod/files/...       optional: files that replace the game's own
```

`mod.ini`:

```ini
[mod]
name = My mod
enabled = 1
; library = something.so     (default: <directory name>.so)

[settings]
anything = the mod reads with api->setting(api, "anything", "default")
```

A setting can be overridden for one run from the environment:
`WOTS2_MOD_<DIRECTORY>_<KEY>=value` (upper case), and `WOTS2_MOD_<DIRECTORY>=0`
/ `1` switches the mod off or on.

### Replacing files

A file under `files/` takes the place of the game's file of the same path —
the path the game asks `VOLUME.DAT` for, language directory included:

```
mods/mymod/files/Us/menu/menutext.txt        the menus' text (UTF-16)
mods/mymod/files/Common/chara/list           the list of characters
mods/mymod/files/Us/script/CTX/Opening.sx    an event script
```

`tools/volume.py list` prints the paths, `tools/volume.py extract <path> -o
<file>` gets the original to start from; the comments in `src/` describe the formats.
(Not replaceable this way: the music and voice streams in `BGM/` and the
movies, which are read straight from the disc directory.)

### Code

The library exports one function and is built against
[`include/mod/wots2_mod.h`](../include/mod/wots2_mod.h) alone:

```cpp
#include "wots2_mod.h"

static const Wots2Api* game;
static void every_frame(void*) {
    // PlayerData is at *0x2EE688; the player's money is a s16 at +0x106.
    uint32_t playerData = *(uint32_t*)game->guest_ptr(0x2EE688);
    if (playerData && (game->pad_buttons(0) & 0x0001))          // L2 held
        *(int16_t*)game->guest_ptr(playerData + 0x106) = 9999;
}

int wots2_mod_init(const Wots2Api* api) {
    game = api;
    api->log(api, "hello, %s", api->setting(api, "greeting", "world"));
    api->on_frame(every_frame, 0);
    return 0;               // anything else: the mod is left out
}
```

What the table gives a mod:

| | |
|---|---|
| `log`, `setting` | a line on the terminal; a value from its `mod.ini` |
| `get_video`, `set_video` | the shape, size and rate of the picture (what the three mods above use) |
| `guest_ptr(address)` | the game's memory: the PS2's addresses, as `config/globals.json` gives them |
| `on_frame(fn, user)` | a call once every frame of the game |
| `pad_buttons(port)` | the buttons held, as the game sees them |
| `hook(address, fn, name)`, `find(address)` | put a function in the place of one of the game's, known by its PS2 address (`config/functions.json`); answers the one that was there |

A caution about `hook`: it replaces what is *called by address* — virtual
methods, the constructors of scenes and screens, everything the game reaches
through a table.  A decompiled function that calls another directly, as
ordinary C++, does not go through it.  For those, change the source.
