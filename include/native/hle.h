#pragma once
#include <string>
#include <vector>

namespace hle {

// Register the native implementation of every PS2 code address the
// decompiled code can reach through a vtable or constructor table.
void bind_all();

// Directory holding the files from the game disc; "cdrom0:" paths resolve here.
void set_disc_root(const char* root);
const std::string& disc_root();

// Runs the decompiled model classes (construct, relocate, bind textures) on a
// copy of an A2MF file and checks the result is self-consistent.  Used by the
// debug viewers so that code is exercised on every model.  Returns a short
// description of the first problem, or an empty string.
std::string check_model(const std::vector<unsigned char>& file);

}  // namespace hle

// Between the stand-in characters (src/native/hle_chara.cpp) and the
// stand-in world.
extern "C" bool hle_world_active();
extern "C" bool hle_player_moving();
extern "C" u32  hle_cell_at(u32 map, const float* position, s32* index);
extern "C" bool hle_interior_entrance(float* stand, float* face);
extern "C" void hle_chara_enter_interior();
extern "C" bool hle_world_last_view(float* viewProj);
// The screen rectangle {x0, y0, x1, y1} (1/16 pixel, GS coordinates) that a
// sphere of `radius` around a world point covers; 0 if it is behind the camera.
extern "C" int  hle_project_extent(const float* world, float radius, s32* rect);
bool hle_chara_alive(u32 chara);     // is this guest address a live stand-in character

void hle_chara_bind();               // binds the character methods that are not decompiled

// The stand-in world (src/native/hle_gameplay.cpp): is told where the player
// is each frame (its camera follows), and supplies this frame's
// world-to-clip matrix (false when no world was drawn this frame).
extern "C" void hle_world_follow(float* matrix);
extern "C" int  hle_world_settled();         // frames the player has been free out of doors
extern "C" bool hle_world_view(float* viewProj);

// The native save file (src/native/hle.cpp): the player data block, written
// by the stand-in save screen and read when Start is pressed on the title.
extern "C" bool hle_load_save();
