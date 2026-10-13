// Decompiled from 0x118728 – 0x118E70
//
// Sony SDK library code (libkernel TLB setup), not game logic.  Kept as a
// record of what these addresses are; none of it is needed by the native port.
//
//   InitTLB            0x118A00 (identical copy at 0x119550)
//   InitTLB32MB        0x118A40
//   SetTLBHandler      0x118728
//   TLB exception stub 0x118C40 / 0x118D40 / 0x118D80
//
// The stub at 0x118C40 is registered for TLB exception causes 1/2/3 (TLB
// modified, TLB load miss, TLB store miss).  It saves the full CPU context to
// 0x2F3F80, returns to user mode at 0x118D40, calls the handler stored at
// 0x2CD858 on a private stack (0x2F7F80), then restores the context via
// syscall -84.  It is NOT a VBlank handler and does not drive the game loop:
// the main loop is App_Run (0x1E09F0), called from main (0x1001E0).

#include "hal/ps2_types.h"

// ── EE kernel syscall stubs ───────────────────────────────────────────────
extern "C" {
    u32  GetMemorySize();                                              // 0x1107C0  syscall 127
    void KernelInitTLB();                                              // 0x1107D0  syscall 130
    void PutTLBEntry(u32 index, u32 pagemask, u32 entryhi,
                     u32 entrylo0, u32 entrylo1);                      // 0x118960  syscall 86
    void* SetVTLBRefillHandler(u32 cause, void* handler);              // 0x110010  syscall 13
    int  scePrintf(const char* fmt, ...);                              // 0x1123C8
    void Exit(int code);                                               // 0x119610  InitTLB + _Exit
}

// ── Static data ───────────────────────────────────────────────────────────
// 0x2DCCF0 "# TLB spad=0 kernel=1:%d default=%d:%d extended=%d:%d\n"
// 0x2DCD28 "# TLB over flow (1)"   0x2DCD40 "... (2)"   0x2DCD58 "... (3)"
static const char* const kMsgLayout    = reinterpret_cast<const char*>(0x2DCCF0u);
static const char* const kMsgOverflow1 = reinterpret_cast<const char*>(0x2DCD28u);
static const char* const kMsgOverflow2 = reinterpret_cast<const char*>(0x2DCD40u);
static const char* const kMsgOverflow3 = reinterpret_cast<const char*>(0x2DCD58u);

static const u32 kTlbEntries = 48;

// ── TLB layout table at 0x2CDB58 ──────────────────────────────────────────
struct TlbDescriptor {
    u32 pagemask;
    u32 entryhi;
    u32 entrylo0;
    u32 entrylo1;
};

struct TlbLayout {
    s32            kernel_count;    // +0x00  13 wired kernel entries
    s32            default_count;   // +0x04  18 wired default entries
    s32            extended_count;  // +0x08  8 non-wired extended entries
    s32            wired;           // +0x0C  written by InitTLB32MB
    TlbDescriptor* kernel;          // +0x10
    TlbDescriptor* defaults;        // +0x14
    TlbDescriptor* extended;        // +0x18
};
static TlbLayout* const gTlbLayout = reinterpret_cast<TlbLayout*>(0x2CDB58u);

void InitTLB32MB();

// ── 0x118A00 InitTLB ──────────────────────────────────────────────────────
void InitTLB() {
    if (GetMemorySize() == 0x02000000u)   // retail 32 MB console
        InitTLB32MB();
    else
        KernelInitTLB();
}

static s32 InstallGroup(s32 idx, s32 end, const TlbDescriptor* desc, const char* overflowMsg) {
    if (end > (s32)kTlbEntries) {
        scePrintf(overflowMsg);
        Exit(1);
    }
    for (; idx < end; idx++, desc++)
        PutTLBEntry(idx, desc->pagemask, desc->entryhi, desc->entrylo0, desc->entrylo1);
    return idx;
}

// ── 0x118A40 InitTLB32MB ──────────────────────────────────────────────────
void InitTLB32MB() {
    TlbLayout* t = gTlbLayout;

    s32 kernelEnd  = t->kernel_count;
    s32 defaultEnd = kernelEnd + t->default_count;
    scePrintf(kMsgLayout, kernelEnd - 1, kernelEnd, defaultEnd - 1,
              defaultEnd, defaultEnd + t->extended_count - 1);

    // (asm: mtc0 zero, c0_wired; sync.p)
    s32 idx = 0;
    idx = InstallGroup(idx, t->kernel_count, t->kernel, kMsgOverflow1);
    idx = InstallGroup(idx, idx + t->default_count, t->defaults, kMsgOverflow2);

    // (asm: mtc0 idx, c0_wired; sync.p)
    t->wired = idx;

    if (t->extended_count > 0)
        idx = InstallGroup(idx, idx + t->extended_count, t->extended, kMsgOverflow3);

    // Fill the remaining slots with invalid entries at distinct addresses.
    for (u32 vaddr = 0xE0000000u + (u32)idx * 0x2000u; idx < (s32)kTlbEntries; idx++, vaddr += 0x2000u)
        PutTLBEntry(idx, 0, vaddr, 0, 0);
}

// ── 0x118728 SetTLBHandler ────────────────────────────────────────────────
// Stores `handler` as the user TLB-exception callback and points the kernel's
// three TLB exception vectors at the context-saving stub.  Returns `handler`.
using TlbHandler = void (*)();
static TlbHandler* const gTlbHandler = reinterpret_cast<TlbHandler*>(0x2CD858u);

TlbHandler SetTLBHandler(TlbHandler handler) {
    *gTlbHandler = handler;

    void* const kStub = reinterpret_cast<void*>(0x118C40u);
    SetVTLBRefillHandler(1, kStub);
    SetVTLBRefillHandler(2, kStub);
    SetVTLBRefillHandler(3, kStub);

    return handler;
}
