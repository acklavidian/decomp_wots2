#include "hal/iop.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <unistd.h>   // ::close, ::read, ::write, lseek

static std::string s_game_root;

// Async read queue
struct AsyncRead {
    std::string path;
    u32 offset;
    void* buf;
    u32 size;
    std::function<void(void*)> callback;
    void* user;
};
static std::vector<AsyncRead> s_async_queue;

namespace hal::iop {

void init(const char* game_data_root) {
    s_game_root = game_data_root;
    printf("[IOP] game root: %s\n", game_data_root);
}

int open(const char* path, int flags) {
    std::string full = s_game_root + "/" + path;
    const char* mode = (flags & 1) ? "rb" : "rb";  // simplified
    FILE* f = fopen(full.c_str(), mode);
    if (!f) {
        printf("[IOP] open failed: %s\n", full.c_str());
        return -1;
    }
    return fileno(f);  // use POSIX fd
}

int close(int fd) { return ::close(fd); }

int read(int fd, void* buf, u32 size) {
    return ::read(fd, buf, size);
}

int write(int fd, const void* buf, u32 size) {
    return ::write(fd, buf, size);
}

long seek(int fd, long offset, int whence) {
    return lseek(fd, offset, whence);
}

long tell(int fd) {
    return lseek(fd, 0, SEEK_CUR);
}

void async_read(const char* path, u32 offset, void* buf, u32 size,
                void (*callback)(void*), void* user) {
    // For now, perform synchronously and queue the callback
    int fd = open(path, 0);
    if (fd >= 0) {
        seek(fd, offset, SEEK_SET);
        read(fd, buf, size);
        close(fd);
    }
    s_async_queue.push_back({path, offset, buf, size,
        [callback](void* u){ callback(u); }, user});
}

void async_poll() {
    for (auto& req : s_async_queue)
        req.callback(req.user);
    s_async_queue.clear();
}

bool volume_open(u32 name_crc, const void** data_out, u32* size_out) {
    // TODO: look up name_crc in VOLUME.DAT entry table and return pointer to data
    (void)name_crc;
    *data_out = nullptr;
    *size_out = 0;
    return false;
}

}  // namespace hal::iop
