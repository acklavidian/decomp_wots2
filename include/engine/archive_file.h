#pragma once
#include "hal/ps2_types.h"

// A file inside VOLUME.DAT, opened by asset id (index into the path table at
// 0x2D1AF0).  The class is 0xC bytes {vtable, handle, vtable} and is not
// decompiled; the native runtime implements it (src/native/hle.cpp).

struct ArchiveFile;

extern "C" {
    ArchiveFile* ArchiveFile_Create();                       // inlined: MemAlloc(0xC) + constructor
    void  ArchiveFile_Open(ArchiveFile* file, int assetId);  // 0x142AA0
    void  ArchiveFile_OpenByName(ArchiveFile* file, const char* path);    // 0x142B40
    u32   ArchiveFile_GetSize(ArchiveFile* file);            // vtable +0x1C
    void  ArchiveFile_ReadAsync(ArchiveFile* file, void* dst, u32 size);  // vtable +0x28
    int   ArchiveFile_IsBusy(ArchiveFile* file);             // vtable +0x2C
    void  ArchiveFile_Read(ArchiveFile* file, void* dst, u32 size);   // vtable +0x0C
    void* ArchiveFile_ReadAll(ArchiveFile* file, u32 size);  // 0x142980  size 0 = whole file; MemAlloc'd
    void  ArchiveFile_Close(ArchiveFile* file);              // 0x142A90  (vtable +0x24)
    void  ArchiveFile_Destroy(ArchiveFile* file);            // virtual destructor, deleting
}
