#pragma once
#include "hal/ps2_types.h"

// ── Guest memory image ────────────────────────────────────────────────────
// The decompiled code still refers to PS2 addresses directly (globals such
// as 0x2EE688, vtables, data tables) and stores pointers in 32-bit fields.
// Instead of rewriting all of that up front, the native build maps the PS2's
// RAM at its real addresses and loads the game ELF's data into it:
//
//   0x00100000 – 0x02000000   main RAM (ELF image, then the heap)
//   0x10000000, 0x12000000    dummy pages for EE / GS hardware registers
//   0x70000000                scratchpad
//
// Code cannot be shared that way, so every call through a guest address (a
// vtable slot, a constructor table) goes through GuestCall, which looks the
// address up in a registry of native implementations.
namespace guest {

bool init(const char* elf_path);

// Heap inside guest RAM, so allocations fit in 32-bit pointer fields.
void* alloc(u32 size);   // zero-filled; null on exhaustion
void  free(void* p);

// Register the native implementation of the function at `addr`.
void  bind(u32 addr, void* fn, const char* name);
void* lookup(u32 addr);
void  report_unbound(u32 addr);

// A unique address in an unmapped range, for native-only functions that need
// to appear in guest vtables.
u32 fake_address();

template <typename T = void>
inline T* ptr(u32 addr) { return reinterpret_cast<T*>(static_cast<uintptr_t>(addr)); }
inline u32 addr(const void* p) { return static_cast<u32>(reinterpret_cast<uintptr_t>(p)); }

}  // namespace guest

// A PS2 global that holds a 32-bit pointer.  `*gFoo` reads the four bytes at
// the global's address and widens them to a native pointer — declaring such a
// global as `T**` would read eight bytes on a 64-bit host.
template <typename T>
struct GuestPtrVar {
    u32 address;
    T* operator*() const { return guest::ptr<T>(*guest::ptr<u32>(address)); }
};

// Call the function at a guest address.  The native implementation must take
// the same arguments; an unbound address is reported once and returns R().
template <typename R, typename... Args>
inline R GuestCall(u32 addr, Args... args) {
    void* fn = guest::lookup(addr);
    if (!fn) {
        guest::report_unbound(addr);
        return R();
    }
    return reinterpret_cast<R (*)(Args...)>(fn)(args...);
}

// Registers a decompiled function at its PS2 address from the file that
// defines it, for functions reached through vtables:
//     GUEST_BIND(0x286390, Row_Update);
namespace guest {
struct Binder {
    Binder(u32 address, void* fn, const char* name) { bind(address, fn, name); }
};
}  // namespace guest
#define GUEST_BIND(address, fn) \
    static const guest::Binder guest_bind_##fn{address, reinterpret_cast<void*>(fn), #fn}

// Virtual call through a guest vtable: `vtable` is the vtable's address and
// `slot` the byte offset of the method in it.
template <typename R, typename... Args>
inline R VirtualCall(u32 vtable, u32 slot, Args... args) {
    return GuestCall<R, Args...>(*guest::ptr<u32>(vtable + slot), args...);
}
