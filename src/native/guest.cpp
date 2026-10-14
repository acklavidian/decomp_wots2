#include "native/guest.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

namespace {

constexpr u32 kRamBase  = 0x00100000;
constexpr u32 kRamEnd   = 0x02000000;
constexpr u32 kHeapBase = 0x00400000;
constexpr u32 kHeapEnd  = 0x01F00000;

struct Bound { void* fn; guest::Invoker invoke; const char* name; };
// Function-local so that GUEST_BIND registrations, which run during static
// initialisation in other files, always find it constructed.
std::unordered_map<u32, Bound>& registry() {
    static std::unordered_map<u32, Bound> map;
    return map;
}
std::set<u32> g_reported;
u32 g_nextFake = 0xF0000000;

// First-fit heap.  Every block starts with a 16-byte header so payloads stay
// 16-byte aligned (the game stores 128-bit values in heap objects).
struct Block {
    u32 size;    // payload bytes
    u32 used;
    u32 _pad[2];
};
bool g_heapReady = false;

Block* first_block() { return guest::ptr<Block>(kHeapBase); }
Block* next_block(Block* b) {
    u8* n = reinterpret_cast<u8*>(b + 1) + b->size;
    return n < guest::ptr<u8>(kHeapEnd) ? reinterpret_cast<Block*>(n) : nullptr;
}

bool map_fixed(u32 base, u32 size) {
    void* want = guest::ptr(base);
#ifdef _WIN32
    void* got = VirtualAlloc(want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
#ifndef MAP_FIXED_NOREPLACE             // (macOS: a hint, checked below)
#define MAP_FIXED_NOREPLACE 0
#endif
    void* got = mmap(want, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    if (got != want) {
        fprintf(stderr, "[guest] cannot map %#x bytes at %#x\n", size, base);
        return false;
    }
    return true;
}

}  // namespace

namespace guest {

bool init(const char* elf_path) {
    if (!map_fixed(kRamBase, kRamEnd - kRamBase) ||
        !map_fixed(0x10000000, 0x10000) ||     // EE timers / INTC / DMAC
        !map_fixed(0x12000000, 0x10000) ||     // GS privileged registers
        !map_fixed(0x70000000, 0x4000))        // scratchpad
        return false;

    FILE* f = fopen(elf_path, "rb");
    if (!f) {
        fprintf(stderr, "[guest] cannot open %s\n", elf_path);
        return false;
    }
    std::vector<u8> elf;
    fseek(f, 0, SEEK_END);
    elf.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(elf.data(), 1, elf.size(), f);
    fclose(f);
    if (got != elf.size() || elf.size() < 0x34 || memcmp(elf.data(), "\x7f" "ELF", 4) != 0) {
        fprintf(stderr, "[guest] %s is not an ELF file\n", elf_path);
        return false;
    }

    // ELF32 program headers: load every PT_LOAD segment at its virtual address.
    u32 phoff;  u16 phentsize, phnum;
    memcpy(&phoff, &elf[0x1C], 4);
    memcpy(&phentsize, &elf[0x2A], 2);
    memcpy(&phnum, &elf[0x2C], 2);
    for (u16 i = 0; i < phnum; i++) {
        u32 ph[8];
        memcpy(ph, &elf[phoff + i * phentsize], sizeof(ph));
        u32 type = ph[0], offset = ph[1], vaddr = ph[2], filesz = ph[4], memsz = ph[5];
        if (type != 1 || memsz == 0)
            continue;
        if (vaddr < kRamBase || vaddr + memsz > kHeapBase || offset + filesz > elf.size()) {
            fprintf(stderr, "[guest] segment %u out of range\n", i);
            return false;
        }
        memcpy(ptr(vaddr), &elf[offset], filesz);   // the rest (BSS) is already zero
        printf("[guest] loaded %#x bytes at %#x (%#x in memory)\n", filesz, vaddr, memsz);
    }

    Block* b = first_block();
    b->size = kHeapEnd - kHeapBase - sizeof(Block);
    b->used = 0;
    g_heapReady = true;
    return true;
}

void* alloc(u32 size) {
    if (!g_heapReady)
        return nullptr;
    size = (size + 15) & ~15u;
    if (size == 0)
        size = 16;
    for (Block* b = first_block(); b; b = next_block(b)) {
        if (b->used)
            continue;
        // Merge with any free blocks that follow.
        for (Block* n = next_block(b); n && !n->used; n = next_block(b))
            b->size += sizeof(Block) + n->size;
        if (b->size < size)
            continue;
        if (b->size >= size + sizeof(Block) + 16) {
            Block* rest = reinterpret_cast<Block*>(reinterpret_cast<u8*>(b + 1) + size);
            rest->size = b->size - size - sizeof(Block);
            rest->used = 0;
            b->size = size;
        }
        b->used = 1;
        memset(b + 1, 0, b->size);
        return b + 1;
    }
    fprintf(stderr, "[guest] heap exhausted allocating %u bytes\n", size);
    return nullptr;
}

void free(void* p) {
    if (!p)
        return;
    Block* b = static_cast<Block*>(p) - 1;
    if (addr(b) < kHeapBase || addr(b) >= kHeapEnd || !b->used) {
        fprintf(stderr, "[guest] bad free of %p\n", p);
        return;
    }
    b->used = 0;
}

void bind(u32 address, const Fn& fn, const char* name) {
    registry()[address] = {fn.fn, fn.invoke, name};
}

bool find(u32 address, void** fn, Invoker* invoke) {
    auto it = registry().find(address);
    if (it == registry().end())
        return false;
    *fn = it->second.fn;
    *invoke = it->second.invoke;
    return true;
}

void* lookup(u32 address) {
    auto it = registry().find(address);
    return it == registry().end() ? nullptr : it->second.fn;
}

void report_unbound(u32 address) {
    if (g_reported.insert(address).second)
        fprintf(stderr, "[guest] call to unported function %#x ignored\n", address);
}

u32 fake_address() {
    g_nextFake += 4;
    return g_nextFake;
}

}  // namespace guest
