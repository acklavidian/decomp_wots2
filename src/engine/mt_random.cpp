// MT19937 — decompiled from 0x13AE10 and 0x13B680.  See include/engine/mt_random.h.

#include "engine/mt_random.h"
#include "native/guest.h"

enum { kN = 624, kM = 397 };

static u64* const sState = guest::ptr<u64>(0x3075A0u);          // u64[kN], 32 bits used
static s32& sIndex       = *guest::ptr<s32>(0x2EDA18u);         // kN + 1 = never seeded
static const u64* const sMag = guest::ptr<const u64>(0x2D1AE0u);    // {0, 0x9908B0DF}

// ── 0x13B680 ──────────────────────────────────────────────────────────────
void Mt_Seed(u64 seed) {
    for (int i = 0; i < kN; i++) {
        sState[i] = seed & 0xFFFF0000u;
        seed = seed * 69069 + 1;
        sState[i] |= (seed & 0xFFFF0000u) >> 16;
        seed = seed * 69069 + 1;
    }
    sIndex = kN;
}

// ── 0x13AE10 ──────────────────────────────────────────────────────────────
u64 Mt_Next() {
    if (sIndex >= kN) {
        if (sIndex == kN + 1)
            Mt_Seed(0x1105);
        int i = 0;
        for (; i < kN - kM; i++) {
            const u64 y = (sState[i] & 0x80000000u) | (sState[i + 1] & 0x7FFFFFFFu);
            sState[i] = sState[i + kM] ^ (y >> 1) ^ sMag[sState[i + 1] & 1];
        }
        for (; i < kN - 1; i++) {
            const u64 y = (sState[i] & 0x80000000u) | (sState[i + 1] & 0x7FFFFFFFu);
            sState[i] = sState[i + kM - kN] ^ (y >> 1) ^ sMag[sState[i + 1] & 1];
        }
        const u64 y = (sState[kN - 1] & 0x80000000u) | (sState[0] & 0x7FFFFFFFu);
        sState[kN - 1] = sState[kM - 1] ^ (y >> 1) ^ sMag[sState[0] & 1];
        sIndex = 0;
    }
    u64 y = sState[sIndex++];
    y ^= y >> 11;
    y ^= (y & 0x013A58ADu) << 7;
    y ^= (y & 0x0001DF8Cu) << 15;
    return y ^ (y >> 18);
}
