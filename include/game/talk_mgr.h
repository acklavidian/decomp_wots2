#pragma once
#include "engine/containers.h"
#include "game/chara_mgr.h"
#include "game/speech.h"
#include "hal/ps2_types.h"

// The conversation manager (0x18E730–0x190450; `World+0xF4`, called
// "effects" in older code): what is being said by whom, the choice on offer,
// and the camera that frames two people talking.  Its functions keep the
// `Effects_` prefix they were given before their purpose was known.

struct TalkEntry {
    u32 balloon;            // +0x00  SpeechBalloon*
    u32 choice;             // +0x04  ChoiceWindow*
    u32 remark;             // +0x08  SpeechBalloon* (speaker = listener)
    u32 speaker;            // +0x0C  CharaParams*
    u32 listener;           // +0x10
    u32 text;               // +0x14  u16*, its own copy
    u32 handle;             // +0x18  (its own address)
    s32 direction;          // +0x1C  balloon side to prefer, −1
    s32 frames;             // +0x20  until it closes by itself; −1 never
    s32 started;            // +0x24
    s32 waitsForButton;     // +0x28
    u32 _2c;
    s32 _30;
    u32 _34;
    s32 style;              // +0x38
    s32 choices;            // +0x3C  lines to choose from; < 1 = plain speech
    s32 closeNow;           // +0x40
    s32 endCamera;          // +0x44  the talk camera goes when this closes (and is not started for it)
    s32 delay;              // +0x48  frames before input counts
    s32 closeDelay;         // +0x4C
    s32 closing;            // +0x50
    s32 actor;              // +0x54  −1
};

struct TalkMgr {
    u32 list;               // +0x00  SpeechList*
    GuestList entries;      // +0x04  TalkEntry*
    s32 cameraOn;           // +0x10
    s32 selection;          // +0x14  the choice made: line from 1, 0x100 timed out, 0x101 cancelled
    u32 _18;
    s32 choiceShown;        // +0x1C
    s32 chooser;            // +0x20  who the finished choice was put to (id), −1
    u32 choiceHandle;       // +0x24
    s32 cameraA, cameraB;   // +0x28  the two the camera frames (ids), −1
    s32 lastDirection;      // +0x30
    s32 lastA, lastB;       // +0x34
};

extern "C" {
    void* Effects_Ctor(void* mem);                                  // 0x190450
    void  Effects_Dtor(void* self, int flag);                       // 0x1902D0
    void  Effects_Update(void* self, void* camList, void* charas, void* hud);   // 0x18F880
    void  Effects_Draw(void* self);                                 // 0x18F860
    int   Effects_IsBusy(void* self, int id);                       // 0x18E830
    int   Effects_PartnerOf(void* self, int id);                    // 0x18E790
    void  Effects_EndTalkOf(void* self, int id);                    // 0x18F770
    void  Effects_EndAll(void* self, int handle);                   // 0x18F7D0  −1 = everything
    void  Effects_fn18e730(void* self, int id);                     // 0x18E730  closes what is said to `id`
    void  Effects_fn18e890(void* self, int handle);                 // 0x18E890  lets an entry start
    int   Talk_Exists(void* self, u32 handle);                      // 0x18E8D0
    void  Effects_EndTalkCamera(void* self, void* camList, CharaMgr* charas, int a, int b);   // 0x190010
    // A line of speech; returns its handle.
    u32   Talk_Say(void* self, CharaParams* speaker, CharaParams* listener, const u16* text, int style,
                   int frames, int actor);                          // 0x18EBE0
    // A choice of `choices` lines.
    u32   Talk_Select(void* self, CharaParams* speaker, CharaParams* listener, const u16* text, int style,
                      int choices, int frames);                     // 0x18E920

    // What event scripts use (UI.SetSerif / UI.SetTalkSelect / UI.DeleteSelect).
    int   Talk_ScriptSayAt(void* self, const u32* desc);                // 0x18F460
    int   Talk_ScriptSay(void* self, CharaParams* speaker, CharaParams* listener, const u16* text, int style,
                         int frames);                               // 0x18F1D0
    void  Talk_ScriptSelect(void* self, int choices, const SpeechParams* desc);     // 0x18EF00
    void  Talk_CloseChoice(void* self);                             // 0x18F7C0

    // The camera on two people talking (0x18BFA0, 0x30 bytes).
    void* TalkCam_Ctor(void* mem);                                  // 0x18BFA0
    void  TalkCam_Set(float distance, float degrees, void* cam, CharaParams* a, CharaParams* b);   // 0x18BF60
}
