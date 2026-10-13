#pragma once
#include "ps2_types.h"
#include <vector>

// SPU2 (Sound Processing Unit 2) → SDL2 Audio abstraction.
// PS2 audio is ADPCM-encoded (VAG format) played via DMA to SPU2.

namespace hal::spu2 {

void init();
void shutdown();

// Voice control (mirrors SPU2 register interface)
void key_on(u32 voice_mask);
void key_off(u32 voice_mask);
void set_voice_addr(u32 voice, u32 spu2_addr);
void set_voice_pitch(u32 voice, u16 pitch);
void set_voice_volume(u32 voice, u16 vol_left, u16 vol_right);

// DMA transfer (upload ADPCM data to SPU2 RAM)
void dma_write(u32 spu2_addr, const void* data, u32 size_bytes);

// Streaming audio: Sony "SShd"/"SSbd" PS-ADPCM streams, several of which are
// packed back to back in the BGM/*.TRS files.  `slot` is one of kStreamSlots
// independent players; volumes run 0–0x3FFF.
constexpr int kStreamSlots = 3;
bool stream_open(int slot, const char* path, u64 offset);
void stream_start(int slot, bool loop, bool fade_in);
void stream_stop(int slot, bool fade_out);     // fade_out: silent after about two seconds
void stream_close(int slot);
void stream_set_volume(int slot, int left, int right);
void stream_toggle_pause(int slot);
bool stream_active(int slot);                  // opened and not yet finished

// One-shot samples (sound effects).  `pcm` must stay valid; it is mono s16 at
// `rate` Hz.  pitch 0x1000 plays at that rate; volumes run 0–0x3FFF.
void sample_play(const s16* pcm, u32 frames, u32 rate, u32 pitch, int left, int right);
void sample_stop(const s16* pcm);     // silences whatever is playing out of that buffer
void samples_hold(bool held);         // the samples playing now stand still (silent) until let go
// PS-ADPCM (VAG body) → mono s16.  Stops at the block flagged as the end.
void decode_adpcm(const u8* data, u32 size, std::vector<s16>& out);

}  // namespace hal::spu2
