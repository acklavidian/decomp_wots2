#pragma once
#include "hal/ps2_types.h"

#include <string>

// The memory card as the game's card class (0x136EB0–0x137420) sees it,
// backed by a directory on the host.  See src/native/mcard_native.cpp.
namespace mc {

struct Entry {
    u32 size;
    std::string name;
};

u32  status(int port);                      // 0x137410
void set_flags(int port, u32 flags);        // 0x1373F0
void clear_flags(int port, u32 flags);      // 0x1373D0
s32  result();                              // 0x1373C0  what the last call answered
s32  free_kb(int port);                     // 0x1373A0
bool busy();                                // (sceMcSync: never, here)

void get_info(int port);                    // 0x137320
void get_dir(int port, const char* pattern);    // 0x136EB0
const Entry* entry(int index);              // 0x1373B0
int  open(int port, const char* path, int mode);    // 0x1370B0  → 0 = the call was taken
bool is_open();
int  read(void* buffer, u32 size);          // 0x136F60
int  write(const void* buffer, u32 size);   // 0x136EF0
int  seek(u32 offset);                      // 0x136FD0
int  close();                               // 0x137040
int  make_dir(int port, const char* path);  // 0x1371D0
int  remove(int port, const char* path);    // 0x137160
void format(int port);                      // 0x1372B0
void unformat(int port);                    // 0x137240

}  // namespace mc
