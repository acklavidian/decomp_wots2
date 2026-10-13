#pragma once
#include <cstdint>
#include <cstddef>

// PS2 primitive types (matching EE ABI)
using s8  = int8_t;   using u8  = uint8_t;
using s16 = int16_t;  using u16 = uint16_t;
using s32 = int32_t;  using u32 = uint32_t;
using s64 = int64_t;  using u64 = uint64_t;

// R5900 128-bit quad-word (for MMI instructions)
struct alignas(16) u128 {
    u64 lo, hi;
};

// PS2 GS primitive types
struct GsRgba    { u8 r, g, b, a; };
struct GsXYZF    { u16 x, y; u32 z; u8 f; u8 pad[3]; };
struct GsUV      { u16 u, v; };
struct GsPrim    { u16 prim; };

// PS2 vector (VU / PS2 math)
struct Vec4 {
    float x, y, z, w;
};
struct Vec3 {
    float x, y, z;
};
struct Mat4 {
    float m[4][4];
};

// Memory map constants (PS2 EE address space)
static constexpr u32 PS2_RAM_BASE   = 0x00000000;
static constexpr u32 PS2_RAM_SIZE   = 32 * 1024 * 1024;  // 32 MB
static constexpr u32 PS2_SCRATCHPAD = 0x70000000;
static constexpr u32 PS2_GS_BASE    = 0x12000000;  // GS privileged registers
