#include "hal/spu2.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <vector>

// SPU2 → SDL audio.  Streams (BGM) are decoded and mixed in the audio callback;
// voice synthesis for sound effects is not implemented yet.

static SDL_AudioDeviceID s_audio_dev = 0;
static SDL_AudioSpec     s_spec      = {};

// ── Streams ───────────────────────────────────────────────────────────────
// A stream is read into memory when opened and decoded in the audio callback.
// Its body is PS-ADPCM (16-byte blocks of 28 samples) with the channels
// interleaved in chunks of `interleave` bytes.
struct Stream {
    std::vector<u8> body;
    int  channels = 0, interleave = 0;
    int  rate = 48000;           // BGM is 48 kHz, voices 44.1 kHz
    double phase = 0.0;          // how far between two of its samples the output is
    size_t chunk = 0;            // next chunk group to decode
    std::vector<s16> pcm;        // decoded samples of the current group, interleaved
    size_t pcmPos = 0;
    int  hist[2][2] = {};
    bool open = false, playing = false, loop = false, paused = false, ended = false;
    float volume[2] = {1.0f, 1.0f};
    float gain = 1.0f, gainStep = 0.0f;     // fades
};
static Stream s_streams[hal::spu2::kStreamSlots];

static void decode_block(const u8* block, int hist[2], s16* out, int stride) {
    static const int kCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    int shift = block[0] & 0x0F, filter = block[0] >> 4;
    if (filter > 4) filter = 0;
    for (int i = 0; i < 28; i++) {
        int nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 0x0F;
        int sample = (s16)(nibble << 12) >> shift;
        sample += (hist[0] * kCoef[filter][0] + hist[1] * kCoef[filter][1]) >> 6;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        hist[1] = hist[0];
        hist[0] = sample;
        out[i * stride] = (s16)sample;
    }
}

// Decodes the next chunk of every channel into st.pcm.  False at the end.
static bool decode_next(Stream& st) {
    const size_t group = (size_t)st.interleave * st.channels;
    const int blocks = st.interleave / 16;
    for (;;) {
        size_t base = st.chunk * group;
        if (base + group > st.body.size()) {
            if (!st.loop || st.chunk == 0)
                return false;
            st.chunk = 0;
            continue;
        }
        st.pcm.assign((size_t)blocks * 28 * 2, 0);
        bool end = false;
        for (int ch = 0; ch < st.channels && ch < 2; ch++) {
            const u8* p = &st.body[base + (size_t)ch * st.interleave];
            for (int b = 0; b < blocks; b++) {
                decode_block(p + b * 16, st.hist[ch], &st.pcm[(size_t)b * 28 * 2 + ch], 2);
                if (p[b * 16 + 1] & 1)
                    end = true;
            }
        }
        if (st.channels == 1)
            for (size_t i = 0; i < st.pcm.size(); i += 2)
                st.pcm[i + 1] = st.pcm[i];
        st.pcmPos = 0;
        st.chunk++;
        if (end) {
            // The block flagged as the end is the last one with sound in it.
            if (st.loop) st.chunk = 0;
            else         st.chunk = st.body.size() / group + 1;
        }
        return true;
    }
}

// Debug aid: WOTS2_AUDIO_DUMP=file writes everything mixed (raw s16 stereo, 48 kHz).
static FILE* s_dump = nullptr;

static void mix(s16* dst, int frames);

// Master volume: WOTS2_VOLUME=0…100, default 35.  The game's own full volume
// is full scale, which is far too loud on a desktop.
static float s_master = 0.35f;

static void audio_callback(void*, Uint8* out, int bytes) {
    memset(out, 0, (size_t)bytes);
    mix(reinterpret_cast<s16*>(out), bytes / 4);
    s16* samples = reinterpret_cast<s16*>(out);
    for (int i = 0; i < bytes / 2; i++)
        samples[i] = (s16)(samples[i] * s_master);
    if (s_dump)
        fwrite(out, 1, (size_t)bytes, s_dump);
}

// ── One-shot samples ──────────────────────────────────────────────────────
struct Shot {
    const s16* pcm = nullptr;
    u32 frames = 0;
    double pos = 0, step = 0;
    float volume[2] = {};
    bool held = false;          // standing still (the game paused it)
};
static Shot s_shots[24];

static void mix(s16* dst, int frames) {
    for (Shot& sh : s_shots) {
        if (!sh.pcm || sh.held)
            continue;
        for (int i = 0; i < frames; i++) {
            u32 index = (u32)sh.pos;
            if (index >= sh.frames) { sh.pcm = nullptr; break; }
            for (int ch = 0; ch < 2; ch++) {
                int mixed = dst[i * 2 + ch] + (int)(sh.pcm[index] * sh.volume[ch]);
                dst[i * 2 + ch] = (s16)(mixed > 32767 ? 32767 : mixed < -32768 ? -32768 : mixed);
            }
            sh.pos += sh.step;
        }
    }
    for (Stream& st : s_streams) {
        if (!st.open || !st.playing || st.paused || st.ended)
            continue;
        for (int i = 0; i < frames; i++) {
            if (st.pcmPos >= st.pcm.size() && !decode_next(st)) {
                st.ended = true;
                break;
            }
            st.gain += st.gainStep;
            if (st.gain >= 1.0f) { st.gain = 1.0f; st.gainStep = 0.0f; }
            if (st.gain <= 0.0f && st.gainStep < 0.0f) { st.ended = true; break; }
            for (int ch = 0; ch < 2; ch++) {
                int mixed = dst[i * 2 + ch]
                          + (int)(st.pcm[st.pcmPos + ch] * st.volume[ch] * st.gain);
                dst[i * 2 + ch] = (s16)(mixed > 32767 ? 32767 : mixed < -32768 ? -32768 : mixed);
            }
            // Step through the stream at its own rate (the output is 48 kHz).
            st.phase += (double)st.rate / 48000.0;
            while (st.phase >= 1.0) {
                st.phase -= 1.0;
                st.pcmPos += 2;
            }
        }
    }
}

// SPU2 RAM (2MB, mirroring PS2 hardware)
static constexpr u32 SPU2_RAM_SIZE = 2 * 1024 * 1024;
static u8 s_spu2_ram[SPU2_RAM_SIZE] = {};

namespace hal::spu2 {

void init() {
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (const char* v = getenv("WOTS2_VOLUME")) {
        int percent = atoi(v);
        s_master = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f;
    }
    if (const char* path = getenv("WOTS2_AUDIO_DUMP"))
        s_dump = fopen(path, "wb");

    SDL_AudioSpec want = {};
    want.freq     = 48000;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = audio_callback;

    s_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &s_spec, 0);
    if (!s_audio_dev)
        printf("[SPU2] failed to open audio: %s\n", SDL_GetError());
    else
        SDL_PauseAudioDevice(s_audio_dev, 0);

    printf("[SPU2] init @ %d Hz %d-ch\n", s_spec.freq, s_spec.channels);
}

void shutdown() {
    if (s_audio_dev) SDL_CloseAudioDevice(s_audio_dev);
    if (s_dump) { fclose(s_dump); s_dump = nullptr; }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void key_on(u32)  {}  // TODO: implement voice mixing
void key_off(u32) {}

void set_voice_addr(u32, u32)       {}
void set_voice_pitch(u32, u16)      {}
void set_voice_volume(u32, u16, u16){}

void dma_write(u32 spu2_addr, const void* data, u32 size_bytes) {
    if (spu2_addr + size_bytes > SPU2_RAM_SIZE) {
        printf("[SPU2] dma_write OOB: addr=%x size=%u\n", spu2_addr, size_bytes);
        return;
    }
    memcpy(s_spu2_ram + spu2_addr, data, size_bytes);
}

bool stream_open(int slot, const char* path, u64 offset) {
    if (slot < 0 || slot >= kStreamSlots)
        return false;
    Stream fresh;
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("[SPU2] cannot open %s\n", path);
        return false;
    }
    // "SShd" size {format, rate, channels, interleave, …}  "SSbd" size <body>
    u8 head[8];
    u32 info[4] = {};
    bool ok = fseeko(f, (off_t)offset, SEEK_SET) == 0 && fread(head, 1, 8, f) == 8
           && memcmp(head, "SShd", 4) == 0;
    u32 headSize = 0;
    if (ok) {
        memcpy(&headSize, head + 4, 4);
        ok = headSize >= 16 && fread(info, 4, 4, f) == 4
          && fseeko(f, (off_t)(offset + 8 + headSize), SEEK_SET) == 0
          && fread(head, 1, 8, f) == 8 && memcmp(head, "SSbd", 4) == 0;
    }
    if (ok) {
        u32 bodySize;
        memcpy(&bodySize, head + 4, 4);
        fresh.rate = info[1] >= 8000 && info[1] <= 48000 ? (int)info[1] : 48000;
        fresh.channels = (int)info[2];
        fresh.interleave = (int)info[3];
        ok = fresh.channels >= 1 && fresh.channels <= 2 && fresh.interleave >= 16
          && fresh.interleave % 16 == 0;
        if (ok) {
            fresh.body.resize(bodySize);
            ok = fread(fresh.body.data(), 1, bodySize, f) == bodySize;
        }
    }
    fclose(f);
    if (!ok) {
        printf("[SPU2] no stream at %s + %#llx\n", path, (unsigned long long)offset);
        return false;
    }
    fresh.open = true;
    SDL_LockAudioDevice(s_audio_dev);
    s_streams[slot] = std::move(fresh);
    SDL_UnlockAudioDevice(s_audio_dev);
    return true;
}

void stream_start(int slot, bool loop, bool fade_in) {
    if (slot < 0 || slot >= kStreamSlots) return;
    SDL_LockAudioDevice(s_audio_dev);
    Stream& st = s_streams[slot];
    if (st.open) {
        st.loop = loop;
        st.playing = true;
        st.gain = fade_in ? 0.0f : 1.0f;
        st.gainStep = fade_in ? 1.0f / 48000.0f : 0.0f;
    }
    SDL_UnlockAudioDevice(s_audio_dev);
}

void stream_stop(int slot, bool fade_out) {
    if (slot < 0 || slot >= kStreamSlots) return;
    SDL_LockAudioDevice(s_audio_dev);
    Stream& st = s_streams[slot];
    if (fade_out && st.playing && !st.ended)
        st.gainStep = -1.0f / (2.0f * 48000.0f);
    else
        st.ended = true;
    SDL_UnlockAudioDevice(s_audio_dev);
}

void stream_close(int slot) {
    if (slot < 0 || slot >= kStreamSlots) return;
    SDL_LockAudioDevice(s_audio_dev);
    s_streams[slot] = Stream{};
    SDL_UnlockAudioDevice(s_audio_dev);
}

void stream_set_volume(int slot, int left, int right) {
    if (slot < 0 || slot >= kStreamSlots) return;
    SDL_LockAudioDevice(s_audio_dev);
    s_streams[slot].volume[0] = (float)left / 16383.0f;
    s_streams[slot].volume[1] = (float)right / 16383.0f;
    SDL_UnlockAudioDevice(s_audio_dev);
}

void stream_toggle_pause(int slot) {
    if (slot < 0 || slot >= kStreamSlots) return;
    SDL_LockAudioDevice(s_audio_dev);
    s_streams[slot].paused = !s_streams[slot].paused;
    SDL_UnlockAudioDevice(s_audio_dev);
}

bool stream_active(int slot) {
    if (slot < 0 || slot >= kStreamSlots) return false;
    SDL_LockAudioDevice(s_audio_dev);
    bool active = s_streams[slot].open && !s_streams[slot].ended;
    SDL_UnlockAudioDevice(s_audio_dev);
    return active;
}

void sample_play(const s16* pcm, u32 frames, u32 rate, u32 pitch, int left, int right) {
    if (!pcm || frames == 0)
        return;
    SDL_LockAudioDevice(s_audio_dev);
    for (Shot& sh : s_shots) {
        if (sh.pcm)
            continue;
        sh.pcm = pcm;
        sh.held = false;
        sh.frames = frames;
        sh.pos = 0;
        sh.step = (double)rate * pitch / 4096.0 / 48000.0;
        sh.volume[0] = (float)left / 16383.0f;
        sh.volume[1] = (float)right / 16383.0f;
        break;
    }
    SDL_UnlockAudioDevice(s_audio_dev);
}

void samples_hold(bool held) {
    // (What is playing now: a sound started afterwards plays — the menus'.)
    SDL_LockAudioDevice(s_audio_dev);
    for (Shot& sh : s_shots)
        sh.held = held && sh.pcm != nullptr;
    SDL_UnlockAudioDevice(s_audio_dev);
}

void sample_stop(const s16* pcm) {
    if (!pcm)
        return;
    SDL_LockAudioDevice(s_audio_dev);
    for (Shot& sh : s_shots)
        if (sh.pcm == pcm)
            sh.pcm = nullptr;
    SDL_UnlockAudioDevice(s_audio_dev);
}

void decode_adpcm(const u8* data, u32 size, std::vector<s16>& out) {
    int hist[2] = {};
    out.clear();
    for (u32 offset = 0; offset + 16 <= size; offset += 16) {
        const u8 flags = data[offset + 1];
        if (flags == 7)                 // end marker block, no sound
            break;
        size_t at = out.size();
        out.resize(at + 28);
        decode_block(data + offset, hist, &out[at], 1);
        if (flags & 1)
            break;
    }
}

}  // namespace hal::spu2
