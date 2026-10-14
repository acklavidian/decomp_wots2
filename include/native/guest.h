#pragma once
#include "hal/ps2_types.h"

#include <cstring>
#include <type_traits>
#include <utility>

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

// How a call through the registry reaches a function whose parameters it does
// not know: its integer-class arguments in order in `ints`, its float-class
// ones in `floats` (each slot holds the register's bits), the result put back
// the same way.  See `Fn` below.
using Invoker = void (*)(void* fn, const u64* ints, const u64* floats, u64* ret);

// A native function as the registry keeps it: the pointer, and — when its type
// was known where it was registered — how to call it from packed arguments.
struct Fn;

// Register the native implementation of the function at `addr`.
void  bind(u32 addr, const Fn& fn, const char* name);
void* lookup(u32 addr);
// The registry's entry: the function and its invoker (null when untyped).
bool  find(u32 addr, void** fn, Invoker* invoke);
void  report_unbound(u32 addr);

// A unique address in an unmapped range, for native-only functions that need
// to appear in guest vtables.
u32 fake_address();

template <typename T = void>
inline T* ptr(u32 addr) { return reinterpret_cast<T*>(static_cast<uintptr_t>(addr)); }
inline u32 addr(const void* p) { return static_cast<u32>(reinterpret_cast<uintptr_t>(p)); }

}  // namespace guest


// ── Calls through the registry on Win64 ──────────────────────────────────
// A call through a guest address is made with the *call site's* signature.
// The decompiled code does not always declare a callee exactly as it is
// defined: most often float and integer parameters come in another order
// (`f(float, Chara*, int)` called as `f(Chara*, int, float)`).  The System V
// ABI (Linux, macOS) and the PS2's both pass the two kinds in separate
// registers, so such a call still works there; Win64 passes arguments by
// position, so it does not.  So on Windows a call goes through the callee's
// invoker, which hands its n-th integer parameter the call's n-th integer
// argument and its n-th float parameter the n-th float one — what the
// registers do elsewhere.  (`WOTS2_FORCE_INVOKER` does the same on Linux, to
// test it.)
namespace guest {
namespace detail {

constexpr int kSlots = 16;

template <typename T>
constexpr bool is_float = std::is_floating_point_v<std::remove_cv_t<std::remove_reference_t<T>>>;

template <typename T>
inline u64 to_slot(const T& v) {
    using D = std::remove_cv_t<std::remove_reference_t<T>>;
    u64 out = 0;
    if constexpr (std::is_floating_point_v<D>) {
        std::memcpy(&out, &v, sizeof(D));
    } else if constexpr (std::is_pointer_v<D>) {
        out = static_cast<u64>(reinterpret_cast<uintptr_t>(v));
    } else if constexpr (std::is_null_pointer_v<D>) {
        out = 0;
    } else if constexpr (std::is_enum_v<D>) {
        out = static_cast<u64>(static_cast<std::underlying_type_t<D>>(v));
    } else if constexpr (std::is_integral_v<D>) {
        out = static_cast<u64>(v);
    } else {
        static_assert(sizeof(D) == 0, "a registry call passes something that is not a number or a pointer");
    }
    return out;
}

template <typename T>
inline T from_slot(const u64* slot) {
    if constexpr (std::is_reference_v<T>) {
        return *reinterpret_cast<std::remove_reference_t<T>*>(static_cast<uintptr_t>(*slot));
    } else {
        using D = std::remove_cv_t<T>;
        static_assert(std::is_arithmetic_v<D> || std::is_pointer_v<D> || std::is_enum_v<D> ||
                          std::is_null_pointer_v<D>,
                      "a registered function takes something that is not a number or a pointer");
        D v;
        std::memcpy(&v, slot, sizeof(D));
        return v;
    }
}

// The index of parameter k among the parameters of its own kind.
template <typename... A>
constexpr int class_index(size_t k) {
    constexpr bool fp[] = {is_float<A>..., false};
    int n = 0;
    for (size_t j = 0; j < k; j++)
        if (fp[j] == fp[k])
            n++;
    return n;
}

template <typename R, typename... A>
struct Invoke {
    template <size_t... K>
    static void run(void* fn, const u64* ints, const u64* floats, u64* ret, std::index_sequence<K...>) {
        auto f = reinterpret_cast<R (*)(A...)>(fn);
        if constexpr (std::is_void_v<R>) {
            f(from_slot<A>((is_float<A> ? floats : ints) + class_index<A...>(K))...);
        } else {
            *ret = to_slot(f(from_slot<A>((is_float<A> ? floats : ints) + class_index<A...>(K))...));
        }
    }
    static void call(void* fn, const u64* ints, const u64* floats, u64* ret) {
        static_assert(sizeof...(A) <= kSlots, "too many parameters");
        run(fn, ints, floats, ret, std::index_sequence_for<A...>{});
    }
};

}  // namespace detail

struct Fn {
    void* fn = nullptr;
    Invoker invoke = nullptr;
    Fn() = default;
    Fn(std::nullptr_t) {}
    // Untyped (a mod's hook): called with the call site's signature as it is.
    explicit Fn(void* f) : fn(f) {}
    template <typename R, typename... A>
    Fn(R (*f)(A...)) : fn(reinterpret_cast<void*>(f)), invoke(&detail::Invoke<R, A...>::call) {}
    template <typename R, typename... A>
    Fn(R (*f)(A..., ...)) : fn(reinterpret_cast<void*>(f)) {}
};

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
#if defined(_WIN32) || defined(WOTS2_FORCE_INVOKER)
    void* fn = nullptr;
    guest::Invoker invoke = nullptr;
    if (!guest::find(addr, &fn, &invoke) || !fn) {
        guest::report_unbound(addr);
        return R();
    }
    if (invoke) {
        using namespace guest::detail;
        u64 ints[kSlots] = {}, floats[kSlots] = {}, ret = 0;
        int ni = 0, nf = 0;
        (((is_float<Args> ? floats[nf++] : ints[ni++]) = to_slot(args)), ...);
        (void)ni, (void)nf;
        invoke(fn, ints, floats, &ret);
        if constexpr (!std::is_void_v<R>)
            return from_slot<R>(&ret);
        else
            return;
    }
#else
    void* fn = guest::lookup(addr);
    if (!fn) {
        guest::report_unbound(addr);
        return R();
    }
#endif
    return reinterpret_cast<R (*)(Args...)>(fn)(args...);
}

// Registers a decompiled function at its PS2 address from the file that
// defines it, for functions reached through vtables:
//     GUEST_BIND(0x286390, Row_Update);
namespace guest {
struct Binder {
    Binder(u32 address, const Fn& fn, const char* name) { bind(address, fn, name); }
};
}  // namespace guest
#define GUEST_BIND(address, fn) \
    static const guest::Binder guest_bind_##fn{address, fn, #fn}

// Virtual call through a guest vtable: `vtable` is the vtable's address and
// `slot` the byte offset of the method in it.
template <typename R, typename... Args>
inline R VirtualCall(u32 vtable, u32 slot, Args... args) {
    return GuestCall<R, Args...>(*guest::ptr<u32>(vtable + slot), args...);
}
