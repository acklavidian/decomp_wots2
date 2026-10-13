#pragma once
#include "hal/ps2_types.h"

// The game's second random number generator: MT19937 (the 1998 reference
// version), state at 0x3075A0.  Callers reseed it from a hardware timer
// before nearly every use.
extern "C" {
    void Mt_Seed(u64 seed);         // 0x13B680
    u64  Mt_Next();                 // 0x13AE10
}

// How callers use it: `__umoddi3(Mt_Next(), n)` (0x12B8F8).
inline u32 Mt_Below(u32 n) { return n != 0 ? (u32)(Mt_Next() % n) : 0; }

// EE timer 1's counter, the usual seed.
inline u32 Timer1_Count() { return *reinterpret_cast<volatile u32*>(0x10000800u); }
