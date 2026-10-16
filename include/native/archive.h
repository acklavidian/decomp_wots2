#pragma once
#include "hal/ps2_types.h"
#include <string>
#include <vector>

// VOLUME.DAT reader — see tools/volume.py for the format notes.
namespace archive {

bool open(const char* volume_path);

// Language directory prepended to paths outside "Common/" ("Us/").
void set_prefix(const char* prefix);

// Read and decompress a file by game path, e.g. "menu/TitleScreen/BG.bpm".
bool read(const std::string& path, std::vector<u8>& out);

// A directory whose files take the place of the archive's (mods): looked in
// first, by the full path the game asks for ("Us/menu/menutext.txt",
// "Common/chara/list"), as written or in lower case.  Later ones come first.
void add_overlay(const char* dir);
// The same file from the overlays alone (false if no mod has it).
bool read_overlay(const std::string& path, std::vector<u8>& out);
// The path an overlay file takes the place of: with the language directory
// for paths outside "Common/", and '/' for '\\'.
std::string full_path(const std::string& path);

// Read by asset id: an index into the game's path table at 0x2D1AF0.
bool read_asset(u32 id, std::vector<u8>& out);
const char* asset_path(u32 id);

}  // namespace archive
