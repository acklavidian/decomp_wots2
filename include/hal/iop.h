#pragma once
#include "ps2_types.h"
#include <cstdio>

// IOP (I/O Processor) → POSIX file I/O abstraction.
// The PS2 IOP handles CD/DVD reads via IRX modules.
// Here we redirect those reads to the extracted VOLUME.DAT / asset files.

namespace hal::iop {

void init(const char* game_data_root);  // path to extracted/

// File I/O (mirrors PS2 sceIo* API)
int  open(const char* path, int flags);
int  close(int fd);
int  read(int fd, void* buf, u32 size);
int  write(int fd, const void* buf, u32 size);
long seek(int fd, long offset, int whence);
long tell(int fd);

// Async read (mirrors sceCdRead / DMA)
void async_read(const char* path, u32 offset, void* buf, u32 size,
                void (*callback)(void* user), void* user);
void async_poll();  // call once per frame to fire pending callbacks

// Volume.dat asset access (resolves internal paths to VOLUME.DAT entries)
bool volume_open(u32 name_crc, const void** data_out, u32* size_out);

}  // namespace hal::iop
