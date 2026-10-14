#pragma once
#include <string>
#include "hal/ps2_types.h"

// Texture packs made for PCSX2 (native, not the game's): a folder of
// replacement images named the way PCSX2's texture replacement names them,
//   <texture hash>-<palette hash>-<format>.dds   (palette textures)
//   <texture hash>-<format>.dds                  (the others)
// where the texture hash is XXH3-64 of the GS memory blocks the texture
// covers, the palette hash XXH3-64 of its 16 or 256 colours (32-bit, in
// index order, the GS's own alpha) and the format PSM | TW << 6 | TH << 10.
// sprites::Sheet works out the same names for the textures it decodes and
// asks here for a replacement.  See "Texture packs" in mods/README.md.
namespace texture_pack {

// Index the replacement files under `dir` (any depth).  → how many.
int add_dir(const std::string& dir);
bool active();

// XXH3-64 (seed 0), as PCSX2 hashes.
u64 hash(const void* data, size_t size);

// The replacement's GL texture for that name (loaded on first use, kept for
// the rest of the run), or 0.  `bits` = PSM | TW << 6 | TH << 10.
u32 find(u64 textureHash, u64 paletteHash, u32 bits);
// Whether a GL texture is one of the pack's (they are shared: never deleted).
bool owns(u32 tex);

}  // namespace texture_pack
