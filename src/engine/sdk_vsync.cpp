// Decompiled from 0x1107F0 - 0x11087F
//
// Sony SDK library code (libgraph internals), not game logic.
// Blocks until the next vertical blank by acknowledging the VBLANK-start bit
// in the EE interrupt status register and polling until it is raised again.
// Called from sceGsSyncV (0x104B38) when no vsync callback is installed.
//
// Native port: not needed — the HAL buffer swap provides vsync.

#include "hal/ps2_types.h"

// ── PS2 EE hardware registers ─────────────────────────────────────────────
// INTC_STAT: write 1 to a bit to acknowledge it.  Bit 2 = VBLANK start.
static volatile u32& INTC_STAT = *reinterpret_cast<volatile u32*>(0x1000F000u);
static const u32 kIntcVblankStart = 4;

// ── libkernel interrupt helpers (CP0 Status.EIE, not syscalls) ────────────
// DI: disables EE interrupts; returns non-zero if they were enabled.
// EI: re-enables EE interrupts.
extern "C" int  DI();  // 0x118FA0
extern "C" int  EI();  // 0x118FE8

// ── 0x1107F0 ──────────────────────────────────────────────────────────────
void GsWaitVSync() {
    // Acknowledge any pending VBLANK-start
    int wasEnabled = DI();
    INTC_STAT = kIntcVblankStart;
    __sync_synchronize();  // MIPS: sync
    if (wasEnabled)
        EI();

    // Wait for the next one
    while (!(INTC_STAT & kIntcVblankStart)) {}

    // Acknowledge it
    wasEnabled = DI();
    INTC_STAT = kIntcVblankStart;
    __sync_synchronize();  // MIPS: sync
    if (wasEnabled)
        EI();
}
