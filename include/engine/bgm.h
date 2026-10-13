#pragma once
#include "hal/ps2_types.h"

// Background music.  The 37 tunes are streams inside BGM/BGM.TRS, started by
// byte offset.  Up to three play at once so one can fade out under the next.

enum { kBgmCount = 37, kBgmTitle = 0, kBgmMenu = 24 };

// One entry of the list of playing tunes (std::vector at 0x309F80).
struct BgmPlaying {
    s32 id;
    s32 voice;      // stream voice, 0–2
    s32 layer;      // 0 = the current tune; counts up each time it is faded out
};

extern "C" {
    // Volume is 0–0x3FFF before the BGM volume setting is applied.
    void Bgm_Play(int id, int loop, int fadeIn, int volume);    // 0x180A80
    void Bgm_Stop(int id);                                      // 0x1805E0  −1 = whatever is current
    void Bgm_StopLayer(int layer, int fadeOut);                 // 0x180740
    void Bgm_StopAll(int fadeOut);                              // 0x180350
    void Bgm_ReapFinished();                                    // 0x181110  once a frame
    void Bgm_ApplyVolume();                                     // 0x180B00  gBgmVolume → all voices
    void Bgm_TogglePause();                                     // 0x1804B0
    bool Bgm_IsPlaying(int id);                                 // 0x180060
    void Bgm_StopFade(int id, int fadeOut);                     // 0x1805F0
    void Bgm_StopVoiceLayer(int fadeOut);                       // 0x1800E0
    void Voice_Play(int id, int positional, int volume, int loop);  // 0x1800F0  EFFECT.TRS: shouts, cries, shop lines
    void Voice_Start(int id, int positional, int volume);       // 0x1801F0  SOUND.TRS: story dialogue
    void Voice_Stop();                                          // 0x1801E0
    void Sound_Cry(const void* charaParams, int cry);           // 0x180F70
    int  CharaFace_Set(void* face, const void* params, int kind);   // 0x2A2880  (really: a cry of that kind)
    void Sound_ApplyConfig();                                   // 0x17FFA0
    void Sound_PlaySE(int id, int positional, int volume);      // 0x180E70  volume 0–0x3FFF

    // The streaming driver (an object at *0x2EE5DC that queues commands for
    // the IOP module ToraSt.irx).  Implemented by the native runtime.
    struct StreamDriver;
    void Stream_Open(StreamDriver* drv, const char* path, int voice, u32 fileOffset);  // 0x288260
    void Stream_Start(StreamDriver* drv, int voice, int loop, int fadeIn);             // 0x287B10
    void Stream_Stop(StreamDriver* drv, int voice, int fadeOut);                       // 0x287820
    void Stream_Close(StreamDriver* drv, int voice);                                   // 0x2880D0
    int  Stream_SetVolume(StreamDriver* drv, int voice, u32 left, u32 right);          // 0x287680
    int  Stream_TogglePause(StreamDriver* drv, int voice);                             // 0x286FB0
    void Stream_Update(StreamDriver* drv);                                             // 0x287400
    // 0 or 1 once the voice has stopped; anything else while it plays.
    int  Stream_GetStatus(StreamDriver* drv, int voice);                               // 0x2874B0
}
