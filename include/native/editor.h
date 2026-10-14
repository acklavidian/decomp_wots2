#pragma once
#include <SDL.h>

// The level editor (native, not part of the game): F2 in the world.  See
// "The level editor" in the project notes and mods/README.md.
namespace editor {

// Every window event goes here first; true = the editor took it.
bool event(const SDL_Event& ev);

}  // namespace editor
