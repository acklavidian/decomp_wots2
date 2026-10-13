// Decompiled from 0x224FE0 – 0x2252E0 (Track) and 0x26D300 – 0x26E2F0 (TitleBg)
//
// TitleBg — the title screen's background and its 200-frame intro: the
// backdrop darkens, two pattern layers slide in, mist drifts across, the logo
// fades up with a glow, and waves scroll along the top and bottom edges.
// Everything is driven by twelve keyframe tracks stepped once per frame.

#include "game/title_menu.h"
#include "native/guest.h"

// ── External functions ─────────────────────────────────────────────────────
struct GsDevice;
extern "C" {
    void* MemAlloc(u32 size);                               // 0x100820
    void  MemFree(void* p);                                 // 0x100650

    int   SpriteSet_Bind(void* set);                        // 0x17BFD0
    void  SpriteSet_Draw(void* set, int index, int x, int y, u32 z, u32 rgba);  // 0x17B780
    void  SpriteSet_DrawEx(void* set, int index, void* params);                // 0x17ADF0
    void  Gs_FillRect(const s32* rect, u32 z, u32 rgba);    // 0x225CD0  {x, y, w, h} in 1/16 px
    void  GsDevice_SetAlphaRef(GsDevice* dev, int ref);     // 0x12EDC0  alpha-test reference
    void  GsDevice_SetBlend(GsDevice* dev, u64 alphaReg);   // 0x130C50  GS ALPHA register value
    u32   Screen_X(int px, int align);                      // 0x12E5E0
    u32   Screen_Y(int px, int align);                      // 0x12E550
}

// ── Globals ───────────────────────────────────────────────────────────────
static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const s32* const gFrameRate  = reinterpret_cast<const s32*>(0x2EDA00u);
static const s32* const gScreenW    = reinterpret_cast<const s32*>(0x2EE4C0u);
static const s32* const gScreenH    = reinterpret_cast<const s32*>(0x2EE4C4u);

// GS ALPHA register values
static const u64 kBlendNormal   = 0x8000000044ull;   // (Cs − Cd) · As + Cd
static const u64 kBlendAdditive = 0x8000000048ull;   // Cs · As + Cd
static const u64 kBlendSubtract = 0x142ull;          // Cd − Cs · As   (| fix alpha << 32)

static const u32 kZBack  = 0xFFFF82u;
static const u32 kZFront = 0xFFFFE6u;
static const u32 kWhite  = 0x80808080u;

template <typename T = void>
static T* P(u32 addr) { return reinterpret_cast<T*>(static_cast<uintptr_t>(addr)); }
static u32 A(void* p) { return static_cast<u32>(reinterpret_cast<uintptr_t>(p)); }

// The track indices used below, in the order they sit in TitleBg::tracks.
enum {
    kTrackMistAlpha,      //  0  (+0x14)
    kTrackMistSpread,     //  1  (+0x34)
    kTrackDarken,         //  2  (+0x54)
    kTrackPatternAlpha,   //  3  (+0x74)
    kTrackPatternSlide,   //  4  (+0x94)
    kTrackLogoSlide,      //  5  (+0xB4)
    kTrackLogoAlpha,      //  6  (+0xD4)
    kTrackLogoGlow,       //  7  (+0xF4)   loops
    kTrackCopyright,      //  8  (+0x114)
    kTrackCrestSlide,     //  9  (+0x134)
    kTrackCrestAlpha,     // 10  (+0x154)
    kTrackWaveScroll,     // 11  (+0x174)  loops
};

// 0–1 → GS alpha in the top byte over mid-grey RGB.
static u32 AlphaColor(float t) { return ((u32)(s32)(t * 128.0f) << 24) | 0x808080u; }

// ─────────────────────────────────────────────────────────────────────────
// Track
// ─────────────────────────────────────────────────────────────────────────

// std::vector<TrackKey>::push_back — the original inlines insert() (0x225350).
static void Track_PushKey(Track* track, s32 frame, float value) {
    if (track->count >= track->capacity) {
        u32 cap = track->capacity ? track->capacity * 2 : 1;
        TrackKey* keys = static_cast<TrackKey*>(MemAlloc(cap * sizeof(TrackKey)));
        TrackKey* old = P<TrackKey>(track->keys);
        for (u32 i = 0; i < track->count; i++)
            keys[i] = old[i];
        if (old)
            MemFree(old);
        track->capacity = cap;
        track->keys = A(keys);
    }
    P<TrackKey>(track->keys)[track->count++] = {frame, value};
}

// ── 0x225210 Track_Init ───────────────────────────────────────────────────
// Reset to a single key at frame 0.  `length` is in 1/30 s.
void Track_Init(float value, Track* track, int length) {
    track->count = 0;
    Track_PushKey(track, 0, value);
    track->length  = (length * *gFrameRate) / 30;
    track->frame   = 0;
    track->keyA    = 0;
    track->keyB    = 0;
    track->loopKey = -1;
}

// ── 0x225160 Track_AddKey ─────────────────────────────────────────────────
void Track_AddKey(float value, Track* track, int time) {
    Track_PushKey(track, (time * *gFrameRate) / 30, value);
    if (track->count == 2)
        track->keyB = 1;
}

// ── 0x225070 Track_Step ───────────────────────────────────────────────────
void Track_Step(Track* track) {
    if (track->frame >= track->length)
        return;

    const TrackKey* keys = P<TrackKey>(track->keys);
    if (keys[track->keyB].frame <= track->frame) {
        track->keyA = track->keyB;
        if (++track->keyB >= track->count)
            track->keyB--;
    }
    if (++track->frame >= track->length && track->loopKey != -1) {
        track->keyA  = track->loopKey;
        track->keyB  = track->loopKey;
        track->frame = keys[track->loopKey].frame;
    }
}

// ── 0x224FE0 Track_Value ──────────────────────────────────────────────────
float Track_Value(const Track* track) {
    const TrackKey* keys = P<TrackKey>(track->keys);
    const TrackKey& a = keys[track->keyA];
    const TrackKey& b = keys[track->keyB];
    if (track->frame < a.frame || b.frame < track->frame)
        return a.value;
    float start = (float)a.frame;
    return a.value + (b.value - a.value) * (((float)track->frame - start) / ((float)b.frame - start));
}

// ── 0x225130 Track_SkipToEnd ──────────────────────────────────────────────
void Track_SkipToEnd(Track* track) {
    track->frame = track->length - 1;
    track->keyB  = track->count - 1;
    track->keyA  = track->count - 1;
}

// ── 0x2252E0 Track_Dtor ───────────────────────────────────────────────────
void Track_Dtor(Track* track, s16 flag) {
    if (!track)
        return;
    track->count = 0;
    if (track->keys)
        MemFree(P(track->keys));
    track->keys = 0;
    track->capacity = 0;
    if (flag > 0)
        MemFree(track);
}

// ─────────────────────────────────────────────────────────────────────────
// TitleBg
// ─────────────────────────────────────────────────────────────────────────

// ── 0x26DD90 TitleBg_SetupTracks ──────────────────────────────────────────
// Key times are in 1/30 s; the intro runs for 200 of them.
static void TitleBg_SetupTracks(TitleBg* self) {
    Track* t = self->tracks;

    Track_Init(0.75f, &t[kTrackDarken], 200);
    Track_AddKey(0.75f, &t[kTrackDarken], 30);
    Track_AddKey(0.0f,  &t[kTrackDarken], 120);

    Track_Init(0.3f, &t[kTrackMistAlpha], 200);
    Track_AddKey(0.3f, &t[kTrackMistAlpha], 60);
    Track_AddKey(0.0f, &t[kTrackMistAlpha], 115);

    Track_Init(0.0f, &t[kTrackMistSpread], 200);
    Track_AddKey(0.0f, &t[kTrackMistSpread], 30);
    Track_AddKey(1.0f, &t[kTrackMistSpread], 165);

    Track_Init(1.0f, &t[kTrackPatternSlide], 200);
    Track_AddKey(1.0f, &t[kTrackPatternSlide], 50);
    Track_AddKey(0.0f, &t[kTrackPatternSlide], 125);

    Track_Init(0.0f, &t[kTrackPatternAlpha], 200);
    Track_AddKey(0.0f,  &t[kTrackPatternAlpha], 85);
    Track_AddKey(0.75f, &t[kTrackPatternAlpha], 135);

    Track_Init(1.0f, &t[kTrackLogoSlide], 200);
    Track_AddKey(1.0f, &t[kTrackLogoSlide], 132);
    Track_AddKey(0.0f, &t[kTrackLogoSlide], 142);

    Track_Init(0.0f, &t[kTrackLogoAlpha], 200);
    Track_AddKey(0.0f, &t[kTrackLogoAlpha], 132);
    Track_AddKey(1.0f, &t[kTrackLogoAlpha], 160);

    // The glow flashes once, then pulses between 0.2 and 0.7 forever.
    Track_Init(0.0f, &t[kTrackLogoGlow], 250);
    Track_AddKey(0.0f, &t[kTrackLogoGlow], 160);
    Track_AddKey(1.0f, &t[kTrackLogoGlow], 172);
    Track_AddKey(0.2f, &t[kTrackLogoGlow], 195);
    Track_AddKey(0.2f, &t[kTrackLogoGlow], 200);
    Track_AddKey(0.7f, &t[kTrackLogoGlow], 225);
    Track_AddKey(0.2f, &t[kTrackLogoGlow], 250);
    t[kTrackLogoGlow].loopKey = 3;

    Track_Init(0.0f, &t[kTrackCopyright], 200);
    Track_AddKey(0.0f, &t[kTrackCopyright], 155);
    Track_AddKey(1.0f, &t[kTrackCopyright], 160);

    Track_Init(1.0f, &t[kTrackCrestSlide], 200);
    Track_AddKey(1.0f, &t[kTrackCrestSlide], 29);
    Track_AddKey(0.0f, &t[kTrackCrestSlide], 132);

    Track_Init(0.0f, &t[kTrackCrestAlpha], 200);
    Track_AddKey(0.0f, &t[kTrackCrestAlpha], 125);
    Track_AddKey(1.0f, &t[kTrackCrestAlpha], 152);

    Track_Init(0.0f, &t[kTrackWaveScroll], 162);
    Track_AddKey(1.0f, &t[kTrackWaveScroll], 162);
    t[kTrackWaveScroll].loopKey = 0;
}

// ── 0x26E1B0 TitleBg_Ctor ─────────────────────────────────────────────────
TitleBg* TitleBg_Ctor(TitleBg* self, u32* bg, u32* mist, u32* menu, u32* pattern) {
    for (Track& track : self->tracks) {
        track.capacity = 0;
        track.count = 0;
        track.keys = 0;
        Track_Init(0.0f, &track, 0);
    }
    self->bgSprites      = A(bg);
    self->mistSprites    = A(mist);
    self->menuSprites    = A(menu);
    self->patternSprites = A(pattern);
    self->frame = 0;
    TitleBg_SetupTracks(self);
    return self;
}

// ── 0x26E0A0 TitleBg_Reset ────────────────────────────────────────────────
void TitleBg_Reset(TitleBg* self) {
    TitleBg_SetupTracks(self);
    self->frame = 0;
}

// ── 0x26DCE0 TitleBg_Update ───────────────────────────────────────────────
// Returns non-zero while the intro is still playing.
int TitleBg_Update(TitleBg* self) {
    for (Track& track : self->tracks)
        Track_Step(&track);
    s32 frame = self->frame;
    if (frame < 200)
        self->frame = frame + 1;
    return frame < 200;
}

// ── 0x26D300 TitleBg_DrawWaves ────────────────────────────────────────────
// A 264-pixel wave tile repeated five times along the top edge (upside down)
// and five times along the bottom, scrolling in opposite directions.
static void TitleBg_DrawWaves(TitleBg* self) {
    GsDevice* dev = *gGsDevice;
    u32 left   = Screen_X(0, -1) & 0xFFFF;
    u32 top    = Screen_Y(0, -1) & 0xFFFF;
    u32 right  = Screen_X(0, 1) & 0xFFFF;
    s16 bottom = (s16)Screen_Y(0, 1);
    void* set = P(*P<u32>(self->patternSprites));
    const s32 kTile = 264 * 16;
    const u32 kWaveColor = 0x08808080u;

    GsDevice_SetBlend(dev, kBlendAdditive);

    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } params =
        { 0x40 /* flip vertically */, 0, 0, kZBack, -1, -1, kWaveColor };
    params.x = (s32)((left - kTile) & 0xFFFF) + (s32)(Track_Value(&self->tracks[kTrackWaveScroll]) * 4224.0f);
    params.y = (s32)top;
    for (int i = 0; i < 5; i++) {
        SpriteSet_DrawEx(set, 3, &params);
        params.x += kTile;
    }

    u32 x = right + (s32)(Track_Value(&self->tracks[kTrackWaveScroll]) * -4224.0f);
    for (int i = 0; i < 5; i++) {
        SpriteSet_Draw(set, 3, x & 0xFFFF, bottom - 0x580, kZBack, kWaveColor);
        x = (x & 0xFFFF) - kTile;
    }

    GsDevice_SetBlend(dev, kBlendNormal);
}

// ── 0x26D4F0 TitleBg_DrawPatterns ─────────────────────────────────────────
static void TitleBg_DrawPatterns(TitleBg* self) {
    GsDevice* dev = *gGsDevice;
    u32 left = Screen_X(0, -1) & 0xFFFF;
    u32 top  = Screen_Y(0, -1) & 0xFFFF;
    u32 right  = (left + *gScreenW * 16) & 0xFFFF;
    u32 bottom = (top + *gScreenH * 16) & 0xFFFF;
    void* set = P(*P<u32>(self->patternSprites));
    const Track* t = self->tracks;

    // Two pattern panels slide in from the left and the right.
    u32 color = AlphaColor(Track_Value(&t[kTrackPatternAlpha]));
    float slide = Track_Value(&t[kTrackPatternSlide]);
    SpriteSet_Draw(set, 0, (left + (s32)(slide * -566.0f) * 16) & 0xFFFF, top, kZFront, color);
    slide = Track_Value(&t[kTrackPatternSlide]);
    SpriteSet_Draw(set, 1, (right - 0x14C0 + (s32)(slide * 330.0f) * 16) & 0xFFFF,
                   (bottom - 0xB70) & 0xFFFF, kZFront, color);

    // The crest slides in from the right.
    float crest = Track_Value(&t[kTrackCrestSlide]);
    SpriteSet_Draw(set, 2, (right - 0x1B30 + (s32)(crest * 450.0f) * 16) & 0xFFFF,
                   (bottom - 0x11E0) & 0xFFFF, kZFront, AlphaColor(Track_Value(&t[kTrackCrestAlpha])));

    GsDevice_SetAlphaRef(dev, 0);
    TitleBg_DrawWaves(self);
    GsDevice_SetAlphaRef(dev, 8);
}

// ── 0x26D780 TitleBg_DrawLogo ─────────────────────────────────────────────
static void TitleBg_DrawLogo(TitleBg* self) {
    GsDevice* dev = *gGsDevice;
    u32 left = Screen_X(0, -1) & 0xFFFF;
    s16 top  = (s16)Screen_Y(0, -1);
    void* set = P(*P<u32>(self->menuSprites));
    const Track* t = self->tracks;
    u32 logoX = left + 0x430;

    // Glow behind the logo, additive.
    GsDevice_SetBlend(dev, kBlendAdditive);
    SpriteSet_Draw(set, 1, logoX & 0xFFFF, top + 0x150, kZFront, AlphaColor(Track_Value(&t[kTrackLogoGlow])));
    GsDevice_SetBlend(dev, kBlendNormal);

    // Logo, settling 12 pixels to the right as it fades in.
    float slide = Track_Value(&t[kTrackLogoSlide]);
    SpriteSet_Draw(set, 0, (logoX + (s32)(slide * -12.0f) * 16) & 0xFFFF, top + 0x1A0, kZFront,
                   AlphaColor(Track_Value(&t[kTrackLogoAlpha])));

    // Copyright line.
    SpriteSet_Draw(set, 2, (left + 0x540) & 0xFFFF, top + 0x17E0, 0xFFFFEBu,
                   AlphaColor(Track_Value(&t[kTrackCopyright])));
}

// ── 0x26DA10 TitleBg_Draw ─────────────────────────────────────────────────
void TitleBg_Draw(TitleBg* self) {
    const Track* t = self->tracks;

    // Backdrop, with a black overlay that lifts during the intro.
    void* bg = P(*P<u32>(self->bgSprites));
    SpriteSet_Bind(bg);
    u32 left = Screen_X(0, -1) & 0xFFFF;
    u32 top  = Screen_Y(0, -1) & 0xFFFF;
    SpriteSet_Draw(bg, 0, left, top, kZBack, kWhite);
    const s32 screen[4] = { (s32)left, (s32)top, *gScreenW << 4, *gScreenH << 4 };
    Gs_FillRect(screen, kZBack, (u32)(s32)(Track_Value(&t[kTrackDarken]) * 128.0f) << 24);

    SpriteSet_Bind(P(*P<u32>(self->patternSprites)));
    TitleBg_DrawPatterns(self);

    // Mist: two layers drifting apart, subtracted from the picture.
    void* mist = P(*P<u32>(self->mistSprites));
    SpriteSet_Bind(mist);
    GsDevice* dev = *gGsDevice;
    left = Screen_X(0, -1) & 0xFFFF;
    u32 topY = Screen_Y(0, -1);
    s32 alpha = (s32)(Track_Value(&t[kTrackMistAlpha]) * 128.0f);
    GsDevice_SetAlphaRef(dev, 0);
    GsDevice_SetBlend(dev, ((u64)(alpha & 0xFF) << 32) | kBlendSubtract);
    u32 color = ((u32)alpha << 24) | 0x808080u;
    float spread = Track_Value(&t[kTrackMistSpread]);
    SpriteSet_Draw(mist, 0, (left + (s32)(spread * -820.0f) * 16) & 0xFFFF, topY, kZFront, color);
    spread = Track_Value(&t[kTrackMistSpread]);
    SpriteSet_Draw(mist, 1, (left + (s32)(spread * 820.0f) * 16) & 0xFFFF, topY, kZFront, color);
    GsDevice_SetBlend(dev, kBlendNormal);
    GsDevice_SetAlphaRef(dev, 8);

    SpriteSet_Bind(P(*P<u32>(self->menuSprites)));
    TitleBg_DrawLogo(self);
}

// ── 0x26E0D0 TitleBg_Dtor ─────────────────────────────────────────────────
TitleBg* TitleBg_Dtor(TitleBg* self, s16 flag) {
    if (!self)
        return self;
    for (int i = 11; i >= 0; i--)
        Track_Dtor(&self->tracks[i], -1);
    if (flag > 0)
        MemFree(self);
    return self;
}
