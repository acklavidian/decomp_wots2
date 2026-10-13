#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"

// Screen parameter files (*.tp, debug.txt): a list of 4-byte values that the
// menus read in order — positions, colours, flags, floats.  A file is either
// the raw values, or text starting with 'T' that is converted on load:
//
//   123  -4        integers
//   0.95f          floats (anything with a '.')
//   Color[r,g,b,a] packed as r | g<<8 | b<<16 | a<<24
//   BLEND_ON / BLEND_OFF   1 / 0
//   // … and /* … */       comments

struct BufferReader {
    u32 start;       // +0x00
    u32 cur;         // +0x04
    s32 size;        // +0x08
    u32 remaining;   // +0x0C
    u32 vtable;      // +0x10
};

struct ParamFile {
    BufferReader out;      // +0x00  the values, ready to be read
    BufferReader asset;    // +0x14  the file as loaded
    u32          assetRef; // +0x28
    BufferReader text;     // +0x2C  cursor while converting a text file
    GuestVector  values;   // +0x40  converted values
};

// Copies `size` bytes out of a reader, or nothing if fewer remain.
inline void BufferReader_Read(BufferReader* r, void* dst, u32 size) {
    u32 taken = 0;
    if (r->remaining >= size) {
        __builtin_memcpy(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(r->cur)), size);
        taken = size;
    }
    r->cur += taken;
    r->remaining -= taken;
}

// A file being loaded, readable through a BufferReader once it has arrived.
struct AssetBuffer {
    BufferReader reader;   // +0x00
    u32          assetRef; // +0x14
};

inline u16 BufferReader_Peek16(const BufferReader* r) {                 // 0x278360
    return r->remaining > 1 ? *reinterpret_cast<const u16*>(static_cast<uintptr_t>(r->cur)) : 0;
}
inline u16 BufferReader_Read16(BufferReader* r) {                       // 0x278320
    u16 value = BufferReader_Peek16(r);
    r->cur += 2;
    r->remaining -= 2;
    return value;
}

extern "C" {
    AssetBuffer* AssetBuffer_Ctor(AssetBuffer* self);               // 0x2237F0
    AssetBuffer* AssetBuffer_Dtor(AssetBuffer* self, s16 flag);     // 0x223750
    bool AssetBuffer_Request(AssetBuffer* self, int assetId);       // 0x2236B0
    // 1 while the file is still loading.
    int  AssetBuffer_Poll(AssetBuffer* self);                       // 0x223560
    // In a 16-bit text: the length in characters up to the first blank line
    // (CR LF CR LF), counting its first CR LF; and the CR LF pairs in a span.
    int  WText_FindBlankLine(const BufferReader* r);                // 0x2234D0
    int  WText_CountLineBreaks(const BufferReader* r, int chars);   // 0x223450

    ParamFile* ParamFile_Ctor(ParamFile* self);                     // 0x224020
    ParamFile* ParamFile_Dtor(ParamFile* self, s16 flag);           // 0x223F70
    bool ParamFile_Request(ParamFile* self, int assetId);           // 0x223F20
    bool ParamFile_RequestByName(ParamFile* self, const char* name);// 0x223ED0
    // 1 while the file is still loading; 0 once `out` is ready.
    void ParamFile_SetData(ParamFile* self, const char* data, int size);   // 0x223E70
    int  ParamFile_Poll(ParamFile* self);                           // 0x223DB0
}
