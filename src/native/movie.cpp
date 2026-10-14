// Movies (MOVIE/*/*.PSS) — native.  This is hardware: the game's player
// (0x1613D0 → 0x1641E0 → 0x163F00) feeds the PS2's MPEG decoder (the IPU)
// and the streaming sound driver.  Here the picture is decoded by an
// `ffmpeg` process (if one is installed) and the sound, which is plain PCM
// in the file's private stream, is read directly.
//
// A .PSS is an MPEG program stream: video in stream 0xE0 (MPEG-2, 640 × 480
// or so, 30 frames a second), sound in private stream 1 (0xBD), each packet's
// payload starting with four bytes (FF A0 00 00) and the whole being
//   "SShd" u32 0x18, u32 format (1 = 16-bit PCM), u32 rate, u32 channels,
//          u32 interleave (0x200), …   "SSbd" u32 size, samples
// with the channels alternating every `interleave` bytes.
//
// What is kept from the original: Start, Circle or Cross (newly pressed, not
// together with Square) stop the movie once ten frames have been shown, and
// the answer is the pad's buttons at the end.

#include "hal/gs.h"
#include "hal/pad.h"
#include "hal/spu2.h"
#include "native/hle.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

extern char** environ;

extern "C" {
    void* PadMgr_Get();                 // 0x13A920
    void  PadMgr_Poll(void* mgr);       // 0x13A7B0  (pumps the window's events too)
}

namespace {

std::string HostPath(const char* cdrom) {
    std::string p = cdrom ? cdrom : "";
    const size_t colon = p.find(':');
    if (colon != std::string::npos) p = p.substr(colon + 1);
    const size_t semi = p.find(';');
    if (semi != std::string::npos) p = p.substr(0, semi);
    for (char& c : p)
        if (c == '\\') c = '/';
    return hle::disc_root() + p;
}

// Movies play in an ordinary run.  A run steered by debug hooks skips them,
// so that scripted input keeps its frame numbers; WOTS2_MOVIES=1 / 0 decides
// either way, and WOTS2_MOVIES=STAFF plays only the movies so named.
bool Wanted(const std::string& file) {
    if (const char* v = getenv("WOTS2_MOVIES")) {
        if (strcmp(v, "0") == 0 || strcmp(v, "1") == 0)
            return v[0] == '1';
        return file.find(v) != std::string::npos;       // only those with this in their name
    }
    for (char** e = environ; *e; e++) {
        if (strncmp(*e, "WOTS2_", 6) != 0)
            continue;
        // Settings a player may use: they do not make a run scripted.
        static const char* const kPlayer[] = {"WOTS2_VOLUME=", "WOTS2_SAVE=", "WOTS2_NEW_GAME=", "WOTS2_DATA=",
                                              "WOTS2_CARD=", "WOTS2_MODS=", "WOTS2_MOD_", "WOTS2_VIDEO="};
        bool player = false;
        for (const char* p : kPlayer)
            player = player || strncmp(*e, p, strlen(p)) == 0;
        if (player)
            continue;
        return false;
    }
    return true;
}

// The sound track: every 0xBD packet's payload, then the SShd / SSbd body.
struct Sound {
    std::vector<s16> left, right;
    u32 rate = 48000;
};
void ReadSound(const std::string& path, Sound& out) {
    out.left.clear();
    out.right.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return;
    std::vector<u8> body;
    u8 head[16];
    for (;;) {
        if (fread(head, 1, 4, f) != 4)
            break;
        if (head[0] != 0 || head[1] != 0 || head[2] != 1) {     // resynchronise
            fseek(f, -3, SEEK_CUR);
            continue;
        }
        const u8 id = head[3];
        if (id == 0xB9)
            break;
        if (id == 0xBA) {                                       // pack header
            if (fread(head, 1, 10, f) != 10)
                break;
            fseek(f, head[9] & 7, SEEK_CUR);
            continue;
        }
        if (fread(head, 1, 2, f) != 2)
            break;
        const u32 length = (u32)head[0] << 8 | head[1];
        if (id != 0xBD) {
            fseek(f, (long)length, SEEK_CUR);
            continue;
        }
        std::vector<u8> packet(length);
        if (fread(packet.data(), 1, length, f) != length)
            break;
        if (length < 3)
            continue;
        const u32 skip = 3 + packet[2] + 4;                     // PES header, then FF A0 00 00
        if (skip < length)
            body.insert(body.end(), packet.begin() + skip, packet.end());
    }
    fclose(f);
    if (body.size() < 0x28 || memcmp(body.data(), "SShd", 4) != 0)
        return;
    u32 headSize, format, rate, channels, interleave;
    memcpy(&headSize, &body[4], 4);
    memcpy(&format, &body[8], 4);
    memcpy(&rate, &body[12], 4);
    memcpy(&channels, &body[16], 4);
    memcpy(&interleave, &body[20], 4);
    const size_t data = 8 + headSize + 8;
    if (format != 1 || channels != 2 || interleave == 0 || data > body.size())
        return;
    out.rate = rate;
    const size_t frames = (body.size() - data) / 4;
    out.left.reserve(frames);
    out.right.reserve(frames);
    for (size_t at = data; at + interleave * 2 <= body.size(); at += interleave * 2) {
        const s16* l = reinterpret_cast<const s16*>(&body[at]);
        const s16* r = reinterpret_cast<const s16*>(&body[at + interleave]);
        out.left.insert(out.left.end(), l, l + interleave / 2);
        out.right.insert(out.right.end(), r, r + interleave / 2);
    }
}

Sound sSound;       // (kept: the mixer plays out of these buffers)

// A whole frame from the decoder's pipe (a read may be cut short by a signal).
bool ReadFrame(FILE* pipe, std::vector<u8>& frame, const char** why) {
    const int fd = fileno(pipe);
    size_t got = 0;
    while (got < frame.size()) {
        const ssize_t n = read(fd, frame.data() + got, frame.size() - got);
        if (n > 0) {
            got += (size_t)n;
        } else if (n == 0) {
            *why = "the end";
            return false;
        } else if (errno != EINTR && errno != EAGAIN) {
            *why = strerror(errno);
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
// The decoder without a console window (_popen would open one for cmd.exe in
// a windowed program).  Windows looks for ffmpeg.exe beside the program
// first, then along the PATH.
HANDLE sDecoder = nullptr;

FILE* OpenDecoder(const char* command) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20))
        return nullptr;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::string line = command;
    const BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return nullptr;
    }
    CloseHandle(pi.hThread);
    sDecoder = pi.hProcess;
    return _fdopen(_open_osfhandle((intptr_t)rd, _O_RDONLY | _O_BINARY), "rb");
}

void CloseDecoder(FILE* f) {
    fclose(f);
    if (sDecoder != nullptr) {
        if (WaitForSingleObject(sDecoder, 2000) == WAIT_TIMEOUT)
            TerminateProcess(sDecoder, 0);
        CloseHandle(sDecoder);
        sDecoder = nullptr;
    }
}
#else
FILE* OpenDecoder(const char* command) { return popen(command, "r"); }
void CloseDecoder(FILE* f) { pclose(f); }
#endif

}  // namespace

// Is this (a movie, or the logo screen) shown in this run?
extern "C" int Movie_Wanted(const char* name) { return Wanted(name ? name : "") ? 1 : 0; }

extern "C" int Movie_Play(const char* path) {
    const std::string file = HostPath(path);
    if (!Wanted(file)) {
        printf("[movie] skipped (a scripted run): %s\n", file.c_str());
        return 0;
    }
    const int w = hal::gs::screen_width(), h = hal::gs::screen_height();
    char command[1024];
#ifdef _WIN32
    snprintf(command, sizeof command,
             "ffmpeg -v quiet -nostdin -i \"%s\" -an -vf scale=%d:%d -r 30 -pix_fmt rgba -f rawvideo -",
             file.c_str(), w, h);
    FILE* video = OpenDecoder(command);
#else
    std::string quoted;
    for (char c : file)
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    snprintf(command, sizeof command,
             "ffmpeg -v quiet -nostdin -i '%s' -an -vf scale=%d:%d -r 30 -pix_fmt rgba -f rawvideo - 2>/dev/null",
             quoted.c_str(), w, h);
    FILE* video = OpenDecoder(command);
#endif
    std::vector<u8> frame((size_t)w * h * 4);
    const char* why = "";
    if (!video || !ReadFrame(video, frame, &why)) {
        if (video)
            CloseDecoder(video);
        printf("[movie] cannot be shown (is ffmpeg installed?): %s\n", file.c_str());
        return 0;
    }

    hal::spu2::sample_stop(sSound.left.data());
    hal::spu2::sample_stop(sSound.right.data());
    ReadSound(file, sSound);
    printf("[movie] %s (%.1f s of sound)\n", file.c_str(), sSound.left.size() / (double)sSound.rate);
    if (!sSound.left.empty()) {
        hal::spu2::sample_play(sSound.left.data(), (u32)sSound.left.size(), sSound.rate, 0x1000, 0x3FFF, 0);
        hal::spu2::sample_play(sSound.right.data(), (u32)sSound.right.size(), sSound.rate, 0x1000, 0, 0x3FFF);
    }

    void* pads = PadMgr_Get();
    u16 before = 0, held = 0;
    int shown = 0;
    for (;; shown++) {
        PadMgr_Poll(pads);
        const u16 hw = (u16)~hal::pad::get_buttons(0);
        held = (u16)((hw << 8) | (hw >> 8));
        const u16 pressed = held & (before ^ held);
        before = held;
        if ((pressed & 0x8E0) != 0 && shown > 10 && (pressed & 0x80) == 0)
            break;

        hal::gs::begin_frame();
        hal::gs::clear(0);
        hal::gs::set_blend(hal::gs::Blend::Normal);
        hal::gs::set_z(0xFFFFFF);
        const u32 tex = hal::gs::create_texture(frame.data(), w, h);
        hal::gs::draw_sprite(tex, w, h, 0, 0, (float)w, (float)h, 0, 0, (float)w, (float)h, 0x80808080u);
        hal::gs::end_frame();
        hal::gs::destroy_texture(tex);

        if (!ReadFrame(video, frame, &why))
            break;
    }
    CloseDecoder(video);
    printf("[movie] %d frames shown (%s)\n", shown, *why ? why : "stopped by the pad");
    hal::spu2::sample_stop(sSound.left.data());
    hal::spu2::sample_stop(sSound.right.data());
    return held;
}
