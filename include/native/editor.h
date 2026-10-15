#pragma once
#include <SDL.h>

// The level editor (native, not part of the game): F2 in the world.  See
// "The level editor" in the project notes and mods/README.md.
namespace editor {

// Every window event goes here first; true = the editor took it.
bool event(const SDL_Event& ev);
// Writes the graphics settings (picture, FPS, texture pack, lighting) into
// the mods' mod.ini files, as the F5 panel's Save buttons do.
void save_graphics();

}  // namespace editor
