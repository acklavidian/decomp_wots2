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

## The four that come with the game

| Mod | What it does | Settings |
|-----|--------------|----------|
| `widescreen` | The 3D view opened out to the sides — the same height of view, more to the left and right. | `aspect` = `auto` (the window's shape), `16:9`, `21:9`, a number; `hud` = `edges` (gauges, enemy plates and the HUD's text keep to the picture's edges) or `center` |
| `resolution` | Draws the picture with more lines than the PS2's 448. | `internal` = `window` (as many as the window has), a number of lines (`1080`), or `2x`, `3x`…; `window` = `1920x1080`; `fullscreen` = `1` |
| `fps_unlock` | Shows more than thirty pictures a second. | `fps` = `display` or a number (`60`, `120`…); `vsync` = `1` / `0` |
| `lighting` | Modern lighting in the world: the sun's shadows of characters, swords and things (in place of the dark blots under their feet), ambient occlusion, the lanterns of the night maps lighting the ground, bloom, a colour grade. | `shadows`, `map_shadows` (the map's own carts, eaves and trees cast too), `character_shadow` (how dark the sun's shadow falls on characters, 0.6; 1 = like the map), `ao`, `bump` (relief on the map's surfaces, heights from the textures' brightness: 0 off, 1, 2 strong), `bump_depth`, `dust` (motes in the air and dust swept up by the game's own gusts: 0 off, 1, 2 a dusty day), `unbake` (0 the map's painted-in light … 1 relit by its sun, ambient and the real shadows — by day, out of doors), `unbake_exposure`, `lanterns`, `grade` = `1` / `0`; `shadow_strength`, `shadow_quality` (`low`, `high`, `ultra`); `ao_strength`, `ao_radius`; `lantern_strength`; `bloom` (0 = off), `bloom_threshold`; `contrast`, `saturation`, `warmth`, `vignette` |

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
- **F3** switches the modern lighting off and on while playing, to compare.
- **F7** is photo mode (in the world): the game holds still, the HUD goes,
  and you fly a free camera — right mouse looks, W A S D / Q E fly (Shift:
  faster), the wheel zooms; the panel has the field of view, roll, depth of
  field (with the modern lighting on) and the speed.  **F12** saves the picture,
  without the panel, as a PNG in `screenshots/` beside the game.  Esc or F7
  goes back.
- **F6** switches the FPS unlock off (the game's own 30 pictures a second)
  and on again.
- **F5** opens a panel with the FPS unlock (on / off, the rate, vsync) and
  every setting of the modern lighting: move a slider and the picture changes
  at once (the game goes on underneath; Esc
  or F5 closes the panel).  Each part's **Save** writes its values into its
  mod's `mod.ini` (`mods/fps_unlock/`, `mods/lighting/`), so the next start
  uses them.
- **F4** prints where you are (the map, your position, the camera) on the
  terminal — handy for reporting something that looks wrong.
- `lighting` only touches the 3D world: menus, the character setup screen,
  story text and movies are as they were.  Its shadows come from the map's
  own sun (at night, the moon: fainter).  Characters and things cast them, and
  with `map_shadows` (on by default) so does the map itself — carts, eaves,
  fences, trees; the map's baked lighting is kept underneath, so with it off
  only characters and things cast.
- All four only change how the finished frame is drawn.  With them off the
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

### Texture packs

HD texture packs made for **PCSX2** work as they are.  Put the pack's
replacement files (the `.dds` files named like
`1011ff5a8e3dfa33-f89beb808c422937-00005dd4.dds`, usually in a folder
`SLUS-20893/replacements`) under a mod's `textures/` folder, any layout:

```
mods/hd_textures/mod.ini
mods/hd_textures/textures/SLUS-20893/replacements/*.dds
```

or point at them from the `mod.ini`: `[mod]` `textures = /path/to/replacements`.
The game works out PCSX2's name for every texture it loads — a hash of the
texture as it lies in the PS2's video memory, of its palette, and its
format — and uses the pack's picture when there is one.  DDS files in DXT1,
DXT3 or DXT5 are read (what PCSX2 packs use); PNG is not yet.  A pack only
replaces what its author replaced; `WOTS2_TRACE_TEXTURES=1` prints every
texture's name and whether the pack had it.

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
| `get_video`, `set_video` | the shape, size and rate of the picture (what the first three mods above use) |
| `get_lighting`, `set_lighting` | the modern lighting (`Wots2Lighting`; API version 2) |
| `set_map_shadows`, `set_bump`, `set_dust` | whether the map's geometry casts the sun's shadows too; bump mapping of its surfaces; wind and dust (API version 3) |
| `set_unbake` | the map relit instead of its painted-in light (API version 4) |
| `set_model_shadow` | how dark the sun's shadow falls on characters and things (API version 5) |
| `guest_ptr(address)` | the game's memory: the PS2's addresses, as `config/globals.json` gives them |
| `on_frame(fn, user)` | a call once every frame of the game |
| `pad_buttons(port)` | the buttons held, as the game sees them |
| `hook(address, fn, name)`, `find(address)` | put a function in the place of one of the game's, known by its PS2 address (`config/functions.json`); answers the one that was there |

A caution about `hook`: it replaces what is *called by address* — virtual
methods, the constructors of scenes and screens, everything the game reaches
through a table.  A decompiled function that calls another directly, as
ordinary C++, does not go through it.  For those, change the source.

## The level editor

Press **F2** while walking about in the world (not in a menu or a story scene)
and the level editor opens over the picture. The game holds still while it is
open (untick *Hold the game still* to watch people walk about); F2 or Esc
closes it and the game goes on.

A map here is three things, and the editor changes the two that make it play
differently:

| | What | In the editor |
|---|---|---|
| model | the 3D artwork (`Common/map/*.mdl`) | not edited |
| placement file | crates, pots, things to pick up, scenery, sliding doors, smoke and fire, sounds of the place (`object/Iom/*.Iom`) | **Objects** tab: pick, drag, turn, copy, delete, add any of the 126 kinds |
| collision file | the grid of 25 cm cells: walkable ground, walls, doors, exits, hint spots, the ground's height, and the points one arrives at (`Common/collision/*.col`) | **Cells** tab: paint cells with any of the map's cell types, change a type (every cell of it changes); **Points** tab: move the arrival points |

Controls:

| | |
|---|---|
| Right mouse held + move | look round |
| W A S D, Q / E | fly; down / up (Shift: faster) |
| Left click / drag | pick and move a dot (Objects, Points), paint (Cells) |
| Ctrl + click (Cells) | take the type under the mouse |
| R / Shift+R | turn the picked object 15° |
| Ctrl+D, Delete | copy, remove it |
| Ctrl+Z, Ctrl+S | undo, save |

Objects are shown as dots: orange crates, blue doors, green scenery, yellow
things to pick up, red smoke and fire, purple sounds. Changes show at once,
except for smoke, fire and sounds, which take their new places the next time
the map is entered.

**Saving writes a mod**, `mods/level_edits/`: the changed files go under its
`files/`, so they take the place of the game's own the next time the map is
loaded (the disc's files are never changed). To undo everything, delete
`mods/level_edits`; to undo one map, delete its file there; to share your
edits, share the folder.

Not possible yet: changing the 3D model itself, making a placement file for a
map that has none, and changing which people appear where (that is in the
task files and the event scripts).
