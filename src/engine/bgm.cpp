// Background music — decompiled from 0x17FFA0–0x181110.

#include "engine/bgm.h"
#include "engine/config.h"
#include "engine/containers.h"
#include "native/guest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const GuestPtrVar<StreamDriver> gStream{0x2EE5DCu};
static const GuestPtrVar<Config>       gConfig{0x2EE68Cu};
static const GuestPtrVar<void>         gSeDriver{0x2EE5E0u};

extern "C" {
    int   Rand_Next();                                                              // 0x124B10
    float Sound_ApplyPan(float nearDist, float farDist, int* left, int* right, int positional);  // 0x180C70
    void  SeDriver_Play(void* driver, int id, u32 pitch, int volumeL, int volumeR); // 0x15B290
}
static GuestVector& sPlaying   = *guest::ptr<GuestVector>(0x309F80u);   // BgmPlaying
static s32& gSeVolume   = *guest::ptr<s32>(0x2EDAD0u);
static s32& gSeVolume2  = *guest::ptr<s32>(0x2EDAD4u);
static s32& gBgmVolume  = *guest::ptr<s32>(0x2EDAD8u);   // 0–10
static s32& gCurrentBgm = *guest::ptr<s32>(0x2EDADCu);   // −1 = none
static s32& gMono       = *guest::ptr<s32>(0x2EE5E4u);

static const u32* const  kBgmOffsets = guest::ptr<const u32>(0x2D3A50u);   // into BGM.TRS
static const char* const kBgmPath    = guest::ptr<const char>(0x2E0920u);  // "cdrom0:\BGM\BGM.TRS;1"

static inline BgmPlaying* Begin() { return guest::ptr<BgmPlaying>(sPlaying.data); }
static inline BgmPlaying* End()   { return Begin() + sPlaying.count; }

// 0x1812F0 — vector erase; returns the entry now in that position
static BgmPlaying* Erase(BgmPlaying* entry) {
    memmove(entry, entry + 1, (size_t)((u8*)End() - (u8*)(entry + 1)));
    sPlaying.count--;
    return entry;
}

static int ScaledVolume() {
    int v = gBgmVolume * 0x3FFF / 10;
    return v > 0x3FFF ? 0x3FFF : v;
}

// ── 0x180B90 ──────────────────────────────────────────────────────────────
// A free voice, or failing that the one whose tune has been fading longest.
static int Bgm_PickVoice(int layer) {
    int layers[3] = {999, 999, 999};
    for (BgmPlaying* e = Begin(); e != End(); e++)
        layers[e->voice] = e->layer;

    int best = -1;
    for (int voice = 0; voice < 3; voice++) {
        if (layer <= layers[voice]) {
            if (layers[voice] == 999)
                return voice;
            layer = layers[voice] + 1;
            best = voice;
        }
    }
    return best;
}

// ── 0x1808C0 ──────────────────────────────────────────────────────────────
static void Bgm_StartStream(const char* path, u32 offset, int layer, int id,
                            int volumeL, int volumeR, int loop, int fadeIn) {
    int voice = Bgm_PickVoice(layer);
    if (voice == -1)
        return;
    Stream_Close(*gStream, voice);
    for (BgmPlaying* e = Begin(); e != End(); e++) {
        if (e->voice == voice) {
            Erase(e);
            break;
        }
    }
    BgmPlaying entry = {id, voice, layer};
    GuestVector_PushBack(&sPlaying, &entry, sizeof(entry));
    Stream_Open(*gStream, path, voice, offset);
    Stream_SetVolume(*gStream, voice, (u32)volumeR, (u32)volumeL);
    Stream_Start(*gStream, voice, loop, fadeIn);
}

// ── 0x1802C0 ──────────────────────────────────────────────────────────────
static void Bgm_Start(int id, int loop, int fadeIn, int volume) {
    int v = volume * gBgmVolume / 10;
    if (v > 0x3FFF)
        v = 0x3FFF;
    Bgm_StartStream(kBgmPath, kBgmOffsets[id], 0, id, v, v, loop, fadeIn);
    gCurrentBgm = id;
}

// ── 0x180A80 ──────────────────────────────────────────────────────────────
// The tune that was playing fades out underneath the new one.
void Bgm_Play(int id, int loop, int fadeIn, int volume) {
    if (id < 0 || id >= kBgmCount)
        return;
    Bgm_StopLayer(0, 1);
    Bgm_Start(id, loop, fadeIn, volume);
}

// ── 0x180740 ──────────────────────────────────────────────────────────────
void Bgm_StopLayer(int layer, int fadeOut) {
    for (BgmPlaying* e = Begin(); e != End();) {
        if (e->layer != layer) {
            e++;
            continue;
        }
        if (e->layer == 0 && gCurrentBgm == e->id)
            gCurrentBgm = -1;
        if (fadeOut == 0) {
            Stream_Close(*gStream, e->voice);
            e = Erase(e);
        } else {
            Stream_Stop(*gStream, e->voice, fadeOut);
            e->layer++;
            e++;
        }
    }
}

// ── 0x1805E0 ──────────────────────────────────────────────────────────────
void Bgm_Stop(int id) {
    if (id == -1) {
        Bgm_StopLayer(0, 0);
        return;
    }
    BgmPlaying* e = Begin();
    while (e != End() && (e->id != id || e->layer != 0))
        e++;
    if (e == End())
        return;
    if (e->layer == 0 && gCurrentBgm == e->id)
        gCurrentBgm = -1;
    Stream_Close(*gStream, e->voice);
    Erase(e);
}

// ── 0x180350 ──────────────────────────────────────────────────────────────
void Bgm_StopAll(int fadeOut) {
    for (BgmPlaying* e = Begin(); e != End();) {
        if (e->layer == 0 && gCurrentBgm == e->id)
            gCurrentBgm = -1;
        if (fadeOut == 0) {
            Stream_Close(*gStream, e->voice);
            e = Erase(e);
        } else {
            Stream_Stop(*gStream, e->voice, fadeOut);
            e->layer++;
            e++;
        }
    }
    gCurrentBgm = -1;
}

// ── 0x181110 ──────────────────────────────────────────────────────────────
void Bgm_ReapFinished() {
    Stream_Update(*gStream);
    for (BgmPlaying* e = Begin(); e != End();) {
        int status = Stream_GetStatus(*gStream, e->voice);
        if (status == 0 || status == 1) {
            Stream_Close(*gStream, e->voice);
            e = Erase(e);
        } else {
            e++;
        }
    }
}

// ── 0x180B00 ──────────────────────────────────────────────────────────────
void Bgm_ApplyVolume() {
    int v = ScaledVolume();
    for (int voice = 0; voice < 3; voice++)
        Stream_SetVolume(*gStream, voice, (u32)v, (u32)v);
}

// ── 0x1804B0 ──────────────────────────────────────────────────────────────
void Bgm_TogglePause() {
    for (int voice = 0; voice < 3; voice++)
        Stream_TogglePause(*gStream, voice);
}

// ── 0x17FFA0 ──────────────────────────────────────────────────────────────
void Sound_ApplyConfig() {
    const Config* config = *gConfig;
    if (!config)
        return;
    gBgmVolume = config->bgmVolume;
    Bgm_ApplyVolume();
    gSeVolume = config->seVolume;
    gMono = config->mono;
    gSeVolume2 = gSeVolume;
}

// ── 0x180E70 ──────────────────────────────────────────────────────────────
// `positional` non-zero attenuates by the listener's distance and adds a
// little random pitch; the menus pass 0.
void Sound_PlaySE(int id, int positional, int volume) {
    if (id == -1)
        return;
    static const bool trace = getenv("WOTS2_TRACE_SE") != nullptr;      // (native debug aid)
    if (trace)
        printf("[se] %d%s\n", id, positional ? " (placed)" : "");
    int right = volume * gSeVolume / 10;
    int left = right;
    Sound_ApplyPan(4.6f, 14.0f, &left, &right, positional);
    if (left <= 10 && right <= 10)
        return;
    u32 pitch = positional == 0 ? 0x1000u : ((u32)Rand_Next() & 0xFF) + 0xF81;
    SeDriver_Play(*gSeDriver, id, pitch, left, right);
}

// ── 0x1800F0: a voice effect — a shout, a cry, a shopkeeper's line — from
// BGM/EFFECT.TRS (2,436 streams; offsets at 0x2D3AF0), heard from a place ──
void Voice_Play(int id, int positional, int volume, int loop) {
    if (id == -1)
        return;
    int right = volume * gSeVolume / 10;
    int left = right;
    Sound_ApplyPan(4.6f, 14.0f, &left, &right, positional);
    if (left <= 10 && right <= 10)
        return;
    Bgm_StartStream("cdrom0:\\BGM\\EFFECT.TRS;1", guest::ptr<u32>(0x2D3AF0u)[id], 2, id, left, right, loop, 0);
}
// ── 0x1801F0: a line of story dialogue from BGM/SOUND.TRS (offsets at
// 0x2D6100), at the voice volume (0x2EDAD4) ───────────────────────────────
void Voice_Start(int id, int positional, int volume) {
    int right = volume * *guest::ptr<s32>(0x2EDAD4u) / 10;
    int left = right;
    Sound_ApplyPan(-1.0f, -1.0f, &left, &right, positional);
    if (left <= 10 && right <= 10)
        return;
    Bgm_StartStream("cdrom0:\\BGM\\SOUND.TRS;1", guest::ptr<u32>(0x2D6100u)[id], 1, id, left, right, 0, 0);
}
// 0x1801E0
void Voice_Stop() { Bgm_StopLayer(1, 0); }

// ── 0x180F70: a character's cry.  `who` is its CharaParams (its voice set
// at +0x24; 0 = the player's, by the head chosen); cries 0–5 are short
// samples of the effect bank (table 0x2D22C0, 6 per set), the rest voice
// clips (table 0x2D2584, 45 per set) ──────────────────────────────────────
void Sound_Cry(const void* who, int cry) {
    const u8* params = static_cast<const u8*>(who);
    s32 set = 0;
    if (params != nullptr) {
        memcpy(&set, params + 0x24, 4);
        if (set == 0)
            set = *reinterpret_cast<const s16*>(guest::ptr<u8>(*guest::ptr<u32>(0x2EE688u)) + 0x116);
    }
    const int place = params != nullptr ? (int)guest::addr(params + 0x80) : 0x80;
    if (params == nullptr || cry > 5) {
        Voice_Play(*guest::ptr<s16>(0x2D2584u + (u32)(cry * 2 + set * 0x5A)), place, 0x3FFF, 0);
        return;
    }
    const s16 sample = *guest::ptr<s16>(0x2D22C0u + (u32)(cry * 2 + set * 0xC));
    if (sample == -1)
        return;
    int right = gSeVolume * 0x3FFF / 10;
    int left = right;
    Sound_ApplyPan(4.6f, 14.0f, &left, &right, place);
    if (left <= 10 && right <= 10)
        return;
    SeDriver_Play(*gSeDriver, sample, ((u32)Rand_Next() & 0xFF) + 0xF81, left, right);
}
// ── 0x2A2880: one of the cries of a kind, at random (0 … 11: hurt, dying,
// attacking…; tables 0x2DB3C0 / 0x2DB420).  Older notes call this
// "CharaFace_Set". ──────────────────────────────────────────────────────
int CharaFace_Set(void*, const void* params, int kind) {
    if (kind < 0 || kind > 11)
        return -1;
    const s32* range = guest::ptr<s32>(0x2DB420u) + kind * 2;       // {first, count}
    const s32 cry = guest::ptr<s32>(0x2DB3C0u)[range[0] + Rand_Next() % range[1]];
    if (cry != -1)
        Sound_Cry(params, cry);
    return cry;
}

// ── 0x180060 ──────────────────────────────────────────────────────────────
bool Bgm_IsPlaying(int id) {
    BgmPlaying* e = Begin();
    while (e != End() && (e->id != id || e->layer != 0))
        e++;
    return e != End();
}

// ── 0x1805F0 ──────────────────────────────────────────────────────────────
void Bgm_StopFade(int id, int fadeOut) {
    if (id == -1) {
        Bgm_StopLayer(0, fadeOut);
        return;
    }
    BgmPlaying* e = Begin();
    while (e != End() && (e->id != id || e->layer != 0))
        e++;
    if (e == End())
        return;
    if (e->layer == 0 && gCurrentBgm == e->id)
        gCurrentBgm = -1;
    if (fadeOut == 0) {
        Stream_Close(*gStream, e->voice);
        Erase(e);
    } else {
        Stream_Stop(*gStream, e->voice, fadeOut);
        e->layer++;
    }
}

// ── 0x1800E0 ──────────────────────────────────────────────────────────────
void Bgm_StopVoiceLayer(int fadeOut) {
    Bgm_StopLayer(2, fadeOut);
}
