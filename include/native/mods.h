#pragma once

// The mod loader (src/native/mods.cpp; what a mod sees is include/mod/wots2_mod.h).
namespace mods {

// Loads every enabled mod of the mods directory ("mods", or WOTS2_MODS=<dir>;
// WOTS2_MODS=0 = none).  A run steered by debug hooks loads none unless
// WOTS2_MODS says where from: scripted runs stay what they were.
void load_all();
// Once for every frame of the game, before it is shown.
void frame();
// The directory of a loaded mod (by its directory name), or "" if none is.
const char* dir_of(const char* id);

}  // namespace mods
