// Settings menu widgets — decompiled from 0x2833A0–0x289F60.
//
// See include/game/menu_widgets.h for the classes.  Virtual methods are
// registered at their PS2 addresses (GUEST_BIND) and called through the
// vtables in the game's data segment.

#include "game/menu_widgets.h"
#include "engine/config.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "native/guest.h"

#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void* PadMgr_Get();                                         // 0x13A920
    u16   Pad_GetPressed(void* mgr, int port);                  // 0x13A0C0
    int   Pad_SetVibration(void* mgr, int port, const void* pattern);   // 0x139FF0
    void  Rand_Seed(u32 seed);                                  // 0x124AF8  libc srand
    int   Rand_Next();                                          // 0x124B10  libc rand

    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);    // 0x17B780
    void  SpriteSet_DrawEx(SpriteSet* set, int index, void* params);                   // 0x17ADF0
    void  SpriteSet_DrawRotated(SpriteSet* set, int index, const void* params);        // 0x179A40
    void* SpriteSet_GetSprite(SpriteSet* set, int index);                              // 0x17BDF0

    void  Sound_PlaySE(u32 id, u32 randomPitch, u32 volume);    // 0x180E70
    void  Sound_ApplyConfig();                                  // 0x17FFA0  volumes and mono from the config
    void  Bgm_Play(int id, int loop, int fade, int volume);     // 0x180A80
    void  Bgm_Stop(int id);                                     // 0x1805E0
    void  Bgm_TogglePause();                                    // 0x1804B0
    void  Bgm_ApplyVolume();                                    // 0x180B00  from gBgmVolume
}

// ── Globals ───────────────────────────────────────────────────────────────
static const GuestPtrVar<Font>   gFont{0x2EE67Cu};
static const GuestPtrVar<Config> gConfig{0x2EE68Cu};
static s32& gFrameRate  = *guest::ptr<s32>(0x2EDA00u);
static s32& gSeVolume   = *guest::ptr<s32>(0x2EDAD0u);
static s32& gSeVolume2  = *guest::ptr<s32>(0x2EDAD4u);
static s32& gBgmVolume  = *guest::ptr<s32>(0x2EDAD8u);

// Label appearance, read from menu/OptionScreen/debug.txt.
static u32&   sLabelSelected = *guest::ptr<u32>(0x2EE784u);
static u32&   sLabelNormal   = *guest::ptr<u32>(0x2EE788u);
static u32&   sLabelUnused   = *guest::ptr<u32>(0x2EE78Cu);
static float& sLabelScale    = *guest::ptr<float>(0x2EE790u);

enum : u32 {
    kVtMenuItem    = 0x2ED040u,
    kVtMenuRow     = 0x2ED000u,
    kVtMenuButton  = 0x2ECFC0u,
    kVtMenuValue   = 0x2EB290u,
    kVtChoiceValue = 0x2ED0D0u,
    kVtGaugeValue  = 0x2ED0F0u,
    kVtNumberValue = 0x2ED0B0u,
    kVtPanelBase   = 0x2ECFA8u,
    kVtPanel       = 0x2ECF98u,
};

enum : u32 { kMenuDepth = 0xFFFFBE };

// Sprites of menu/OptionScreen/Img.bpm
enum {
    kSpritePinwheel = 0, kSpritePinwheelStick = 1,
    kSpriteRow = 2, kSpriteRowSelected = 3,
    kSpriteButton = 4, kSpriteButtonSelected = 5,
    kSpriteGaugeEmpty = 6, kSpriteGaugeFull = 7,
    kSpriteArrowB = 8, kSpriteArrowA = 9,
};

// menutext.txt lines
enum {
    kTextColorBars = 0x6A, kTextScreenPos = 0x6B, kTextCamera = 0x6C, kTextNormal = 0x6D,
    kTextReverse = 0x6E, kTextVibrate = 0x6F, kTextVibrateOff = 0x70, kTextVibrateOn = 0x71,
    kTextSubtitles = 0x72, kTextOn = 0x73, kTextOff = 0x74, kTextBlood = 0x75, kTextSound = 0x76,
    kTextAudio = 0x77, kTextStereo = 0x78, kTextMono = 0x79, kTextBgmTest = 0x7A,
    kTextBgmVolume = 0x7B, kTextSeTest = 0x7C, kTextSeVolume = 0x7D, kTextEnd = 0x7E,
    kTextSoundEnd = 0x7F, kTextReset = 0x80,
};

enum { kSeMenuOpen = 1, kSeCursor = 2, kSeChange = 3, kSeConfirm = 5 };

static inline MenuContext* ContextOf(const MenuItem* item) { return guest::ptr<MenuContext>(item->context); }
static inline SpriteSet*   SpritesOf(const MenuItem* item) { return guest::ptr<SpriteSet>(ContextOf(item)->spriteSet); }
static inline int Frames(int at30fps) { return gFrameRate * at30fps / 30; }
static inline u32 FadeColor(float level) { return (u32)(int)(level * 127.0f) << 24 | 0x808080u; }

template <typename R, typename... Args>
static inline R ItemCall(MenuItem* item, MenuItemSlot slot, Args... args) {
    return VirtualCall<R, MenuItem*, Args...>(item->vtable, slot, item, args...);
}

// The two position getters return a pair, so the result pointer comes first.
static inline void ItemGetPos(MenuItem* item, MenuItemSlot slot, s32* out) {
    VirtualCall<void, s32*, MenuItem*>(item->vtable, slot, out, item);
}

// ══ Values ════════════════════════════════════════════════════════════════

static void MenuValue_Ctor(MenuValue* v, u32 vtable) {
    v->vtable = kVtMenuValue;
    v->x = 0;
    v->y = 0;
    v->offsetX = 0;
    v->offsetY = 0;
    v->blink = 0;
    v->vtable = vtable;
}

// ── 0x289800 ──────────────────────────────────────────────────────────────
// The blinking arrows either side of a selected value.  `which`: 1 = left
// arrow, 2 = right arrow.  `halfWidth` is in 1/16 pixel.
static void MenuValue_DrawArrows(MenuValue* v, u32 which, u32 halfWidth, int alpha, SpriteSet* set) {
    u32 x = Screen_X((v->x + v->offsetX) * 16, -1);
    u32 y = Screen_Y((v->y + v->offsetY) * 16, -1);
    const u8* arrow = static_cast<const u8*>(SpriteSet_GetSprite(set, kSpriteArrowA));
    if (!arrow)
        return;

    const int period = Frames(10);
    if (++v->blink > period * 2)
        v->blink = 0;
    const int sprite = v->blink < period ? kSpriteArrowA : kSpriteArrowB;

    u16 w, h;
    memcpy(&w, arrow + 0xC, 2);
    memcpy(&h, arrow + 0xE, 2);
    struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } params;
    params.y = (s32)((y & 0xFFFF) - h * 8);
    params.rgba = (u32)alpha << 24 | 0x808080u;
    params.z = kMenuDepth;
    params.w = -1;
    params.h = -1;

    params.flags = 0x20;                                   // mirrored
    params.x = (s32)((x & 0xFFFF) - (halfWidth & 0xFFFF) - w * 8);
    if (which & 1)
        SpriteSet_DrawEx(set, sprite, &params);

    params.flags = 1;
    params.x = (s32)((x & 0xFFFF) + (halfWidth & 0xFFFF) - w * 8);
    if (which & 2)
        SpriteSet_DrawEx(set, sprite, &params);
}

// ── ChoiceValue ───────────────────────────────────────────────────────────
// 0x289250
static ChoiceValue* ChoiceValue_Ctor(ChoiceValue* self) {
    MenuValue_Ctor(&self->base, kVtChoiceValue);
    self->entries.capacity = 0;
    self->entries.count = 0;
    self->entries.data = 0;
    self->index = 0;
    return self;
}

// 0x2891D0
static ChoiceValue* ChoiceValue_Dtor(ChoiceValue* self, s16 flag) {
    if (self) {
        self->base.vtable = kVtChoiceValue;
        self->entries.count = 0;
        if (self->entries.data)
            MemFree(guest::ptr(self->entries.data));
        self->base.vtable = kVtMenuValue;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x289150 (the text is measured by 0x225DC0)
static void ChoiceValue_Add(ChoiceValue* self, const u16* text) {
    Font_Measure(*gFont, text, 1.0f);
    const FontExtent* extent = Font_GetExtent(*gFont);
    ChoiceEntry entry;
    entry.text = guest::addr(text);
    entry.width = (s16)((extent->w >> 4) + 0x18);
    entry.height = (s16)(extent->h >> 4);
    GuestVector_PushBack(&self->entries, &entry, sizeof(entry));
}

// 0x289140
static void ChoiceValue_Set(ChoiceValue* self, s32 index) { self->index = index; }

// 0x289100
static bool ChoiceValue_Next(ChoiceValue* self) {
    bool can = (u32)self->index < self->entries.count - 1;
    if (can)
        self->index++;
    return can;
}

// 0x2890D0
static bool ChoiceValue_Prev(ChoiceValue* self) {
    bool can = self->index > 0;
    if (can)
        self->index--;
    return can;
}

// 0x288F70
static void ChoiceValue_Draw(ChoiceValue* self, bool selected, u8 alpha, SpriteSet* set) {
    Font* font = *gFont;
    const ChoiceEntry* entry = guest::ptr<ChoiceEntry>(self->entries.data) + self->index;
    u32 x = Screen_X((self->base.x + self->base.offsetX) * 16, -1);
    u32 y = Screen_Y((self->base.y + self->base.offsetY) * 16, -1);

    Font_SetColor(font, (selected ? 0xCCCC6Eu : 0x93B1C2u) | (u32)alpha << 24);
    Font_DrawCentered(font, guest::ptr<const u16>(entry->text), (int)x, (int)y, kMenuDepth, 1.0f, 0.0f);
    if (selected) {
        u32 arrows = (self->index > 0 ? 1 : 0) | ((u32)self->index < self->entries.count - 1 ? 2 : 0);
        MenuValue_DrawArrows(&self->base, arrows, ((u32)entry->width & 0x1FFF) << 3, alpha, set);
    }
}

GUEST_BIND(0x2891D0, ChoiceValue_Dtor);
GUEST_BIND(0x289100, ChoiceValue_Next);
GUEST_BIND(0x2890D0, ChoiceValue_Prev);
GUEST_BIND(0x288F70, ChoiceValue_Draw);

// ── GaugeValue ────────────────────────────────────────────────────────────
// 0x2897C0
static GaugeValue* GaugeValue_Ctor(GaugeValue* self) {
    MenuValue_Ctor(&self->base, kVtGaugeValue);
    self->value = 0;
    self->shown = 0;
    return self;
}

// 0x289760
static GaugeValue* GaugeValue_Dtor(GaugeValue* self, s16 flag) {
    if (self) {
        self->base.vtable = kVtMenuValue;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x2896F0
static void GaugeValue_Set(GaugeValue* self, s32 value) {
    if (self->value < value) {
        self->shown = self->value;
        self->timer = 0;
    } else if (value < self->value) {
        self->shown = self->value;
        self->timer = Frames(7);
    }
    self->value = value;
}

// 0x2896B0
static bool GaugeValue_Next(GaugeValue* self) {
    bool can = self->value < 10;
    if (can) {
        self->shown = self->value;
        self->timer = 0;
        self->value++;
    }
    return can;
}

// 0x289640
static bool GaugeValue_Prev(GaugeValue* self) {
    bool can = self->value > 0;
    if (can) {
        self->shown = self->value;
        self->timer = Frames(7);
        self->value--;
    }
    return can;
}

// 0x2892A0 — ten pips; the ones between the old and the new value fade.
static void GaugeValue_Draw(GaugeValue* self, bool selected, u8 alpha8, SpriteSet* set) {
    const u32 alpha = alpha8;
    s16 x = (s16)(Screen_X((self->base.x + self->base.offsetX) * 16, -1) - 0x3F0);
    s16 y = (s16)(Screen_Y((self->base.y + self->base.offsetY) * 16, -1) - 0xD0);
    const s32 low  = self->value < self->shown ? self->value : self->shown;
    const s32 high = self->value < self->shown ? self->shown : self->value;

    int i = 0;
    for (; i < low; i++, x += 0xD0)
        SpriteSet_Draw(set, kSpriteGaugeFull, x, y, kMenuDepth, alpha << 24 | 0x808080u);
    for (; i < high; i++, x += 0xD0) {
        float fading = (float)alpha * ((float)self->timer / (float)Frames(7));
        SpriteSet_Draw(set, kSpriteGaugeFull, x, y, kMenuDepth, (u32)(int)fading << 24 | 0x808080u);
    }
    for (; i < 10; i++, x += 0xD0)
        SpriteSet_Draw(set, kSpriteGaugeEmpty, x, y, kMenuDepth, alpha << 24 | 0x808080u);

    if (self->value != self->shown) {
        if (self->shown < self->value) {
            if (++self->timer > Frames(7))
                self->shown = self->value;
        } else if (--self->timer < 0) {
            self->shown = self->value;
        }
    }
    if (selected) {
        u32 arrows = (self->value > 0 ? 1 : 0) | (self->value < 10 ? 2 : 0);
        MenuValue_DrawArrows(&self->base, arrows, 0x4E0, (int)alpha, set);
    }
}

GUEST_BIND(0x289760, GaugeValue_Dtor);
GUEST_BIND(0x2896B0, GaugeValue_Next);
GUEST_BIND(0x289640, GaugeValue_Prev);
GUEST_BIND(0x2892A0, GaugeValue_Draw);

// ── NumberValue ───────────────────────────────────────────────────────────
// 0x288F20
static NumberValue* NumberValue_Ctor(NumberValue* self) {
    MenuValue_Ctor(&self->base, kVtNumberValue);
    self->value = 0;
    self->max = 10;
    self->top = 0;
    return self;
}

// 0x283D10
static NumberValue* NumberValue_Dtor(NumberValue* self, s16 flag) {
    if (self) {
        self->base.vtable = kVtMenuValue;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x288EF0
static void NumberValue_Set(NumberValue* self, s32 value) {
    self->value = value;
    self->top = self->value;
    if (self->max <= self->top + 5)
        self->top = self->max - 5;
}

// 0x288E90 — wraps to the start
static bool NumberValue_Next(NumberValue* self) {
    if (self->value < self->max) {
        self->value++;
        if (self->value - self->top > 4)
            self->top++;
    } else {
        self->value = 0;
        self->top = 0;
    }
    return true;
}

// 0x288E30 — wraps to the end
static bool NumberValue_Prev(NumberValue* self) {
    if (self->value < 1) {
        self->value = self->max;
        self->top = self->max - 4;
    } else {
        self->value--;
        if (self->value < self->top)
            self->top--;
    }
    return true;
}

// 0x288C50 — five numbers in a row, the chosen one highlighted
static void NumberValue_Draw(NumberValue* self, bool selected, u8 alpha, SpriteSet* set) {
    Font* font = *gFont;
    s16 x = (s16)(Screen_X((self->base.x + self->base.offsetX) * 16, -1) - 0x3E0);
    u32 y = Screen_Y((self->base.y + self->base.offsetY) * 16, -1);

    for (int i = 1; i < 6; i++, x += 0x1E0) {
        u16 text[4] = {};
        WStr_Format(text, 4, "%d", self->top + i);
        const bool current = selected && self->top + i - 1 == self->value;
        Font_SetColor(font, (u32)alpha << 24 | (current ? 0xCCCC6Eu : 0x93B1C2u));
        Font_DrawCenteredPlain(font, text, x, (int)y, kMenuDepth);
    }
    if (selected) {
        u32 arrows = (self->value > 0 ? 1 : 0) | (self->value < self->max ? 2 : 0);
        MenuValue_DrawArrows(&self->base, arrows, 0x570, alpha, set);
    }
}

GUEST_BIND(0x283D10, NumberValue_Dtor);
GUEST_BIND(0x288E90, NumberValue_Next);
GUEST_BIND(0x288E30, NumberValue_Prev);
GUEST_BIND(0x288C50, NumberValue_Draw);

// ── SettingsValues ────────────────────────────────────────────────────────
// 0x283A90
SettingsValues* SettingsValues_Ctor(SettingsValues* self) {
    ChoiceValue_Ctor(&self->camera);
    ChoiceValue_Ctor(&self->vibration);
    ChoiceValue_Ctor(&self->subtitles);
    ChoiceValue_Ctor(&self->blood);
    ChoiceValue_Ctor(&self->audio);
    GaugeValue_Ctor(&self->bgmVolume);
    GaugeValue_Ctor(&self->seVolume);
    NumberValue_Ctor(&self->bgmTest);
    NumberValue_Ctor(&self->seTest);

    TextTable* text = TextTable_Get();
    ChoiceValue_Add(&self->camera,    TextTable_Find(text, kTextNormal));
    ChoiceValue_Add(&self->camera,    TextTable_Find(text, kTextReverse));
    ChoiceValue_Add(&self->vibration, TextTable_Find(text, kTextVibrateOff));
    ChoiceValue_Add(&self->vibration, TextTable_Find(text, kTextVibrateOn));
    ChoiceValue_Add(&self->subtitles, TextTable_Find(text, kTextOff));
    ChoiceValue_Add(&self->subtitles, TextTable_Find(text, kTextOn));
    ChoiceValue_Add(&self->blood,     TextTable_Find(text, kTextOff));
    ChoiceValue_Add(&self->blood,     TextTable_Find(text, kTextOn));
    ChoiceValue_Add(&self->audio,     TextTable_Find(text, kTextStereo));
    ChoiceValue_Add(&self->audio,     TextTable_Find(text, kTextMono));
    self->bgmTest.max = 26;
    self->seTest.max = 98;
    return self;
}

void SettingsValues_Dtor(SettingsValues* self) {
    NumberValue_Dtor(&self->seTest, -1);
    NumberValue_Dtor(&self->bgmTest, -1);
    GaugeValue_Dtor(&self->seVolume, -1);
    GaugeValue_Dtor(&self->bgmVolume, -1);
    ChoiceValue_Dtor(&self->audio, -1);
    ChoiceValue_Dtor(&self->blood, -1);
    ChoiceValue_Dtor(&self->subtitles, -1);
    ChoiceValue_Dtor(&self->vibration, -1);
    ChoiceValue_Dtor(&self->camera, -1);
}

// 0x283600 — nine positions, in the order the parameter file lists them
void SettingsValues_LoadPositions(SettingsValues* self, ParamFile* params) {
    MenuValue* const order[9] = {
        &self->camera.base, &self->vibration.base, &self->subtitles.base, &self->blood.base,
        &self->audio.base, &self->bgmTest.base, &self->bgmVolume.base, &self->seTest.base,
        &self->seVolume.base,
    };
    s32 pos[2] = {0, 0};
    for (MenuValue* value : order) {
        BufferReader_Read(&params->out, pos, 8);
        value->x = pos[0];
        value->y = pos[1];
    }
}

// The part of 0x2839B0 / 0x283450 / 0x2833A0 that copies the sound settings.
static void SetSoundValues(SettingsValues* self, const Config* config) {
    ChoiceValue_Set(&self->audio, config->mono != 0);
    GaugeValue_Set(&self->bgmVolume, (u32)config->bgmVolume > 10 ? 10 : config->bgmVolume);
    GaugeValue_Set(&self->seVolume, (u32)config->seVolume > 10 ? 10 : config->seVolume);
    NumberValue_Set(&self->bgmTest, 0);
    NumberValue_Set(&self->seTest, 0);
}

// 0x2839B0
void SettingsValues_FromConfig(SettingsValues* self) {
    const Config* config = *gConfig;
    ChoiceValue_Set(&self->camera, config->cameraMode != 1);
    ChoiceValue_Set(&self->vibration, config->vibration != 0);
    ChoiceValue_Set(&self->subtitles, config->subtitles != 0);
    ChoiceValue_Set(&self->blood, config->blood != 0);
    SetSoundValues(self, config);
}

// 0x283950
void SettingsValues_ToConfig(SettingsValues* self) {
    Config* config = *gConfig;
    config->cameraMode = self->camera.index != 0 ? 2 : 1;
    config->vibration = self->vibration.index != 0;
    config->subtitles = self->subtitles.index != 0;
    config->blood = self->blood.index != 0;
    config->mono = self->audio.index != 0;
    config->bgmVolume = self->bgmVolume.value;
    config->seVolume = self->seVolume.value;
    Sound_ApplyConfig();
}

// 0x283450 — "Reset": everything back to its default, the screen position too
void SettingsValues_ResetAll(SettingsValues* self) {
    Config* config = *gConfig;
    Display_Set(Display_GetX() - (u32)config->screenX, Display_GetY() - (u32)config->screenY);
    Config_SetDefaults(config);
    SettingsValues_FromConfig(self);
    Sound_ApplyConfig();
    // (the original prints the display position here)
}

// 0x2833A0
void SettingsValues_ResetVolumes(SettingsValues* self) {
    Config_SetDefaultVolumes(*gConfig);
    SetSoundValues(self, *gConfig);
    Sound_ApplyConfig();
}

// ══ Items ═════════════════════════════════════════════════════════════════

// ── MenuItem (base) ───────────────────────────────────────────────────────
static void MenuItem_Ctor(MenuItem* item, MenuContext* context, u32 vtable, s32 width, s32 height) {
    item->vtable = kVtMenuItem;
    item->x = 0;
    item->y = 0;
    item->width = 0;
    item->height = 0;
    item->context = guest::addr(context);
    item->label = 0;
    item->value = 0;
    item->vtable = vtable;
    item->width = width;
    item->height = height;
}

// 0x285AB0
static void MenuItem_GetPos(s32* out, MenuItem* self) {
    out[0] = self->x;
    out[1] = self->y;
}

// 0x2868C0
static MenuItem* MenuItem_Dtor(MenuItem* self, s16 flag) {
    if (self) {
        self->vtable = kVtMenuItem;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x284FA0
static void MenuItem_SetLabel(MenuItem* self, const u16* text) {
    self->label = guest::addr(text);
}

// 0x286800
static void MenuItem_Load(MenuItem* self) {
    ParamFile* params = guest::ptr<ParamFile>(ContextOf(self)->params);
    BufferReader_Read(&params->out, &self->x, 4);
    BufferReader_Read(&params->out, &self->y, 4);
}

// 0x2866A0 — the label, centred in the item's box
static void MenuItem_DrawLabel(MenuItem* self, int alpha) {
    Font* font = *gFont;
    s32 pos[2];
    ItemGetPos(self, kItemGetPos, pos);
    if (self->label == 0)
        return;
    u32 x = Screen_X((pos[0] + (int)((float)self->width * 0.5f)) * 16, -1);
    u32 y = Screen_Y((pos[1] + (int)((float)self->height * 0.5f)) * 16, -1);
    bool selected = ItemCall<bool>(self, kItemIsSelected);
    Font_SetColor(font, ((selected ? sLabelSelected : sLabelNormal) & 0xFFFFFFu) | (u32)alpha << 24);
    Font_DrawCentered(font, guest::ptr<const u16>(self->label), (int)x, (int)y, kMenuDepth,
                      sLabelScale, 0.0f);
}

static void MenuItem_Update(MenuItem*)            {}                 // 0x286AD0
static void MenuItem_Draw(MenuItem* self)         { MenuItem_DrawLabel(self, 0x80); }   // 0x286AE0
static void MenuItem_Focus(MenuItem*)             {}                 // 0x2846C0
static void MenuItem_Blur(MenuItem*)              {}                 // 0x2846D0
static void MenuItem_Hide(MenuItem*, bool)        {}                 // 0x284170
static void MenuItem_Show(MenuItem*, bool)        {}                 // 0x284160
static int  MenuItem_Confirm(MenuItem*)           { return 0; }      // 0x284180
static bool MenuItem_IsSelected(MenuItem*)        { return false; }  // 0x2867F0

GUEST_BIND(0x285AB0, MenuItem_GetPos);
GUEST_BIND(0x2868C0, MenuItem_Dtor);
GUEST_BIND(0x284FA0, MenuItem_SetLabel);
GUEST_BIND(0x286800, MenuItem_Load);
GUEST_BIND(0x286AD0, MenuItem_Update);
GUEST_BIND(0x286AE0, MenuItem_Draw);
GUEST_BIND(0x2846C0, MenuItem_Focus);
GUEST_BIND(0x2846D0, MenuItem_Blur);
GUEST_BIND(0x284170, MenuItem_Hide);
GUEST_BIND(0x284160, MenuItem_Show);
GUEST_BIND(0x284180, MenuItem_Confirm);
GUEST_BIND(0x2867F0, MenuItem_IsSelected);

// ── MenuRow ───────────────────────────────────────────────────────────────
static MenuRow* MenuRow_Ctor(MenuRow* self, MenuContext* context) {
    MenuItem_Ctor(&self->base, context, kVtMenuRow, 0x78, 0x20);
    self->slide = 0;
    self->slideTarget = 0;
    self->timer = 0;
    self->state = kRowIdle;
    return self;
}

// 0x2861E0
static void MenuRow_GetPos(s32* out, MenuRow* self) {
    out[0] = self->base.x + self->slide;
    out[1] = self->base.y;
}

// 0x285860
static MenuRow* MenuRow_Dtor(MenuRow* self, s16 flag) {
    if (self) {
        self->base.vtable = kVtMenuItem;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x286390
static void MenuRow_Update(MenuRow* self) {
    if (self->state == kRowIdle || self->state == kRowHidden || self->state == kRowSelected)
        return;
    self->slide += (self->slideTarget - self->slide) / self->timer;
    if (--self->timer < 1) {
        switch (self->state) {
        case kRowHiding:                       self->state = kRowHidden;   break;
        case kRowShowing:    case kRowBlurring: self->state = kRowIdle;     break;
        case kRowShowingSel: case kRowFocusing: self->state = kRowSelected; break;
        }
    }
}

// 0x285F60 — both backgrounds cross-faded, the label, and the value
static void MenuRow_DrawParts(MenuRow* self, float plain, float lit, float text) {
    s32 pos[2];
    ItemGetPos(&self->base, kItemGetPos, pos);
    u32 x = Screen_X(pos[0] << 4, -1);
    u32 y = Screen_Y(pos[1] << 4, -1);
    SpriteSet* set = SpritesOf(&self->base);
    SpriteSet_Draw(set, kSpriteRow, (int)x, (int)y, kMenuDepth, FadeColor(plain));
    SpriteSet_Draw(set, kSpriteRowSelected, (int)x, (int)y, kMenuDepth, FadeColor(lit));
    const int alpha = (int)(text * 127.0f) & 0xFF;
    MenuItem_DrawLabel(&self->base, alpha);

    if (self->base.value != 0) {
        MenuValue* value = guest::ptr<MenuValue>(self->base.value);
        // The value follows the row's slide except while the row is appearing
        // or disappearing.
        const bool follows = (self->state & (kRowFocusing | kRowBlurring | kRowSelected | kRowShowingSel)) != 0;
        value->offsetX = follows ? self->slide : 0;
        value->offsetY = 0;
        const bool selected = (self->state & (kRowFocusing | kRowSelected)) != 0;
        VirtualCall<void, MenuValue*, bool, u8, SpriteSet*>(value->vtable, kValueDraw, value,
                                                            selected, (u8)alpha, set);
    }
}

// 0x286200
static void MenuRow_Draw(MenuRow* self) {
    if (self->state == kRowHidden)
        return;
    const float t = (float)self->timer / (float)self->duration;
    switch (self->state) {
    case kRowShowingSel: MenuRow_DrawParts(self, 0.0f, 1.0f - t, 1.0f - t); break;
    case kRowShowing:    MenuRow_DrawParts(self, 1.0f - t, 0.0f, 1.0f - t); break;
    case kRowHiding:     MenuRow_DrawParts(self, t, 0.0f, t);               break;
    case kRowSelected:   MenuRow_DrawParts(self, 0.0f, 1.0f, 1.0f);         break;
    case kRowBlurring:   MenuRow_DrawParts(self, 1.0f - t, t, 1.0f);        break;
    case kRowFocusing:   MenuRow_DrawParts(self, t, 1.0f - t, 1.0f);        break;
    case kRowIdle:       MenuRow_DrawParts(self, 1.0f, 0.0f, 1.0f);         break;
    }
}

// 0x286610
static void MenuRow_Focus(MenuRow* self) {
    self->state = kRowFocusing;
    self->timer = Frames(5);
    self->duration = Frames(5);
    self->slideTarget = 0x10;
    if (self->base.value != 0)
        guest::ptr<MenuValue>(self->base.value)->blink = 0;
}

// 0x2865A0
static void MenuRow_Blur(MenuRow* self) {
    self->state = kRowBlurring;
    self->timer = Frames(5);
    self->duration = Frames(5);
    self->slideTarget = 0;
}

// 0x286500
static void MenuRow_Hide(MenuRow* self, bool atOnce) {
    if (!atOnce) {
        self->state = kRowHiding;
        self->timer = Frames(10);
        self->duration = Frames(10);
        self->slideTarget = -0x20;
    } else {
        self->state = kRowHidden;
        self->duration = self->slideTarget;     // as in the original
    }
}

// 0x286460
static void MenuRow_Show(MenuRow* self, bool selected) {
    if (!selected) {
        self->state = kRowShowing;
        self->slideTarget = 0;
    } else {
        self->state = kRowShowingSel;
        self->slideTarget = 0x10;
    }
    self->timer = Frames(10);
    self->duration = Frames(10);
}

// 0x286AC0
static bool MenuRow_IsSelected(MenuRow* self) {
    return (self->state & (kRowFocusing | kRowSelected)) != 0;
}

// 0x286370
static void MenuRow_GetCursorPos(s32* out, MenuRow* self) {
    out[0] = self->base.x + self->slide;
    out[1] = self->base.y + 10;
}

GUEST_BIND(0x2861E0, MenuRow_GetPos);
GUEST_BIND(0x285860, MenuRow_Dtor);
GUEST_BIND(0x286390, MenuRow_Update);
GUEST_BIND(0x286200, MenuRow_Draw);
GUEST_BIND(0x286610, MenuRow_Focus);
GUEST_BIND(0x2865A0, MenuRow_Blur);
GUEST_BIND(0x286500, MenuRow_Hide);
GUEST_BIND(0x286460, MenuRow_Show);
GUEST_BIND(0x286AC0, MenuRow_IsSelected);
GUEST_BIND(0x286370, MenuRow_GetCursorPos);

// ── MenuButton ────────────────────────────────────────────────────────────
static MenuButton* MenuButton_Ctor(MenuButton* self, MenuContext* context) {
    MenuItem_Ctor(&self->base, context, kVtMenuButton, 0x7C, 0x24);
    self->state = kButtonIdle;
    self->timer = 0;
    self->duration = 0;
    return self;
}

// 0x285F00
static MenuButton* MenuButton_Dtor(MenuButton* self, s16 flag) {
    if (self) {
        self->base.vtable = kVtMenuItem;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x285E60
static void MenuButton_Update(MenuButton* self) {
    if ((self->state & (kButtonSelected | kButtonHidden)) != 0)
        return;
    if (--self->timer != 0)
        return;
    switch (self->state) {
    case kButtonFocusing:                       self->state = kButtonSelected; break;
    case kButtonHiding:                         self->state = kButtonHidden;   break;
    case kButtonBlurring: case kButtonShowing:  self->state = kButtonIdle;     break;
    case kButtonPressed:                        self->state = kButtonSelected; break;
    }
}

// 0x2858C0
static void MenuButton_DrawParts(MenuButton* self, float plain, float lit, float text) {
    s32 pos[2];
    ItemGetPos(&self->base, kItemGetPos, pos);
    u32 x = Screen_X(pos[0] << 4, -1);
    u32 y = Screen_Y(pos[1] << 4, -1);
    SpriteSet* set = SpritesOf(&self->base);
    SpriteSet_Draw(set, kSpriteButton, (int)x, (int)y, kMenuDepth, FadeColor(plain));
    SpriteSet_Draw(set, kSpriteButtonSelected, (int)x, (int)y, kMenuDepth, FadeColor(lit));
    MenuItem_DrawLabel(&self->base, (int)(u32)(text * 127.0f));
}

// 0x285AD0
static void MenuButton_Draw(MenuButton* self) {
    if (self->state == kButtonHidden)
        return;
    const float t = (float)self->timer / (float)self->duration;
    switch (self->state) {
    case kButtonSelected: MenuButton_DrawParts(self, 0.0f, 1.0f, 1.0f);         break;
    case kButtonBlurring: MenuButton_DrawParts(self, 1.0f - t, t, 1.0f);        break;
    case kButtonFocusing: MenuButton_DrawParts(self, t, 1.0f - t, 1.0f);        break;
    case kButtonShowing:  MenuButton_DrawParts(self, 1.0f - t, 0.0f, 1.0f - t); break;
    case kButtonHiding:   MenuButton_DrawParts(self, t, 0.0f, t);               break;
    case kButtonPressed:  MenuButton_DrawParts(self, t, 1.0f - t, 1.0f);        break;
    case kButtonIdle:     MenuButton_DrawParts(self, 1.0f, 0.0f, 1.0f);         break;
    }
}

// 0x285DD0
static void MenuButton_Focus(MenuButton* self) {
    self->state = kButtonFocusing;
    self->timer = Frames(5);
    self->duration = Frames(5);
}

// 0x285D60
static void MenuButton_Blur(MenuButton* self) {
    self->state = kButtonBlurring;
    self->timer = Frames(5);
    self->duration = Frames(5);
}

// 0x285CD0
static void MenuButton_Hide(MenuButton* self, bool atOnce) {
    if (!atOnce) {
        self->state = kButtonHiding;
        self->timer = Frames(10);
        self->duration = Frames(10);
    } else {
        self->state = kButtonHidden;
    }
}

// 0x285C50
static void MenuButton_Show(MenuButton* self, bool) {
    self->state = kButtonShowing;
    self->timer = Frames(10);
    self->duration = Frames(10);
}

// 0x285E40 — a short flash
static int MenuButton_Confirm(MenuButton* self) {
    self->state = kButtonPressed;
    self->timer = 10;
    self->duration = 10;
    return 0;
}

// 0x286AB0
static bool MenuButton_IsSelected(MenuButton* self) {
    return (self->state & (kButtonSelected | kButtonBlurring | kButtonFocusing | kButtonPressed)) != 0;
}

// 0x285C30
static void MenuButton_GetCursorPos(s32* out, MenuButton* self) {
    out[0] = self->base.x;
    out[1] = self->base.y + 10;
}

GUEST_BIND(0x285F00, MenuButton_Dtor);
GUEST_BIND(0x285E60, MenuButton_Update);
GUEST_BIND(0x285AD0, MenuButton_Draw);
GUEST_BIND(0x285DD0, MenuButton_Focus);
GUEST_BIND(0x285D60, MenuButton_Blur);
GUEST_BIND(0x285CD0, MenuButton_Hide);
GUEST_BIND(0x285C50, MenuButton_Show);
GUEST_BIND(0x285E40, MenuButton_Confirm);
GUEST_BIND(0x286AB0, MenuButton_IsSelected);
GUEST_BIND(0x285C30, MenuButton_GetCursorPos);

// ══ Cursor ════════════════════════════════════════════════════════════════

// 0x286F50
static MenuCursor* MenuCursor_Ctor(MenuCursor* self) {
    self->timer = Frames(120);
    self->accel = 0.0f;
    self->speed = 5.0f;
    self->angle = 0.0f;
    self->kicked = 0;
    return self;
}

// 0x286C00 — every four seconds the pinwheel picks a new random drift
static void MenuCursor_Update(MenuCursor* self) {
    if (--self->timer < 1) {
        self->timer = Frames(120);
        self->kicked = 0;
        Rand_Seed((u32)self->angle);
        self->accel = ((float)gFrameRate * 0.5f / 30.0f) * ((float)(Rand_Next() % 101 - 50) / 100.0f);
    }
    self->speed += self->accel;
    if (!self->kicked) {
        const float limit = (float)gFrameRate * 30.0f / 30.0f;
        if (self->speed > limit) {
            self->speed = limit;
            self->timer = 0;
        } else if (self->speed < -limit) {
            self->speed = -limit;
            self->timer = 0;
        }
    }
    self->angle += self->speed;
    if (self->angle >= 360.0f)
        self->angle -= 360.0f;
    else if (self->angle < 0.0f)
        self->angle += 360.0f;
}

// 0x286DD0 — a push in the direction it is already turning
static void MenuCursor_Kick(MenuCursor* self) {
    self->kicked = 1;
    self->accel = 0.0f;
    self->timer = Frames(120) >> 1;
    const float push = (float)gFrameRate * 4.0f / 30.0f;
    self->speed += self->speed >= 0.0f ? push : -push;
    const float limit = (float)gFrameRate * 30.0f / 30.0f + push;
    if (self->speed > limit)
        self->speed = limit;
    else if (self->speed < -limit)
        self->speed = -limit;
}

// 0x286AF0
static void MenuCursor_Draw(MenuCursor* self, const s32* pos, SpriteSet* set) {
    u32 x = Screen_X(pos[0] << 4, -1);
    u32 y = Screen_Y(pos[1] << 4, -1);
    struct { u32 flags; s32 x, y; u32 z; u32 rgba; float scaleX, scaleY, angle; } params;
    params.flags = 0;
    params.z = kMenuDepth;
    params.rgba = 0x80808080u;
    params.scaleX = 16.0f;
    params.scaleY = 16.0f;

    params.x = (s32)((x & 0xFFFF) + 0xB0);
    params.y = (s32)((y & 0xFFFF) + 0xC0);
    params.angle = -45.0f;
    SpriteSet_DrawRotated(set, kSpritePinwheelStick, &params);

    params.x = (s32)(x & 0xFFFF);
    params.y = (s32)(y & 0xFFFF);
    params.angle = self->angle;
    SpriteSet_DrawRotated(set, kSpritePinwheel, &params);
}

// ══ Panel ═════════════════════════════════════════════════════════════════

static inline MenuItem* ItemAt(SettingsPanel* self, int index) {
    return guest::ptr<MenuItem>(self->items[index]);
}

static inline MenuItem* Current(SettingsPanel* self) {
    return guest::ptr<MenuItem>(guest::ptr<u32>(self->list)[self->cursor]);
}

static inline SettingsValues* ValuesOf(SettingsPanel* self) {
    return guest::ptr<SettingsValues>(guest::ptr<MenuContext>(self->context)->values);
}

// 0x286910 — label colours and scale from menu/OptionScreen/debug.txt
static void LoadLabelStyle() {
    ParamFile params;
    ParamFile_Ctor(&params);
    ParamFile_RequestByName(&params, "menu/OptionScreen/debug.txt");
    while (ParamFile_Poll(&params) != 0) {}
    BufferReader_Read(&params.out, &sLabelSelected, 4);
    BufferReader_Read(&params.out, &sLabelNormal, 4);
    BufferReader_Read(&params.out, &sLabelUnused, 4);
    BufferReader_Read(&params.out, &sLabelScale, 4);
    ParamFile_Dtor(&params, -1);
}

// 0x284C10
static void SettingsPanel_SetLabels(SettingsPanel* self) {
    static const int kLabels[kSettingsItemCount] = {
        kTextCamera, kTextVibrate, kTextSubtitles, kTextBlood, kTextSound,
        kTextScreenPos, kTextColorBars, kTextReset, kTextEnd,
        kTextAudio, kTextBgmTest, kTextBgmVolume, kTextSeTest, kTextSeVolume, kTextSoundEnd,
    };
    TextTable* text = TextTable_Get();
    for (int i = 0; i < kSettingsItemCount; i++)
        ItemCall<void>(ItemAt(self, i), kItemSetLabel, TextTable_Find(text, kLabels[i]));
}

// 0x2852A0 (base) + 0x2849B0
SettingsPanel* SettingsPanel_Ctor(SettingsPanel* self, MenuContext* context) {
    self->vtable = kVtPanelBase;
    MenuRow_Ctor(&self->camera, context);
    MenuRow_Ctor(&self->vibration, context);
    MenuRow_Ctor(&self->subtitles, context);
    MenuRow_Ctor(&self->blood, context);
    MenuRow_Ctor(&self->sound, context);
    MenuRow_Ctor(&self->screenPos, context);
    MenuRow_Ctor(&self->colorBars, context);
    MenuButton_Ctor(&self->reset, context);
    MenuButton_Ctor(&self->end, context);
    MenuRow_Ctor(&self->audio, context);
    MenuRow_Ctor(&self->bgmTest, context);
    MenuRow_Ctor(&self->bgmVolume, context);
    MenuRow_Ctor(&self->seTest, context);
    MenuRow_Ctor(&self->seVolume, context);
    MenuButton_Ctor(&self->soundEnd, context);
    self->context = guest::addr(context);

    void* const items[kSettingsItemCount] = {
        &self->camera, &self->vibration, &self->subtitles, &self->blood, &self->sound,
        &self->screenPos, &self->colorBars, &self->reset, &self->end,
        &self->audio, &self->bgmTest, &self->bgmVolume, &self->seTest, &self->seVolume,
        &self->soundEnd,
    };
    for (int i = 0; i < kSettingsItemCount; i++)
        self->items[i] = guest::addr(items[i]);
    SettingsPanel_SetLabels(self);

    self->vtable = kVtPanel;
    PadRepeatPair_Ctor(&self->upDown);
    self->upDown.vtable = 0x2EAA00u;
    PadRepeatPair_SetMasks(&self->upDown, kPadUp, kPadDown);
    PadRepeatPair_Ctor(&self->rightLeft);
    self->rightLeft.vtable = 0x2EBA50u;
    PadRepeatPair_SetMasks(&self->rightLeft, kPadRight, kPadLeft);
    MenuCursor_Ctor(&self->pinwheel);

    self->cursor = 0;
    self->_33c = 0;
    self->list = guest::addr(&self->items[kItemCamera]);
    self->listCount = 9;
    ItemCall<void>(Current(self), kItemFocus);
    self->testBgm = -1;
    self->savedBgm = -1;
    PadRepeatPair_SetTiming(&self->upDown, 10, 5, 5);
    PadRepeatPair_SetTiming(&self->rightLeft, 15, 8, 7);
    for (int i = kItemAudio; i < kSettingsItemCount; i++)
        ItemCall<void>(ItemAt(self, i), kItemHide, true);
    LoadLabelStyle();
    return self;
}

// 0x2848F0 (+ base 0x285000)
SettingsPanel* SettingsPanel_Dtor(SettingsPanel* self, s16 flag) {
    if (self) {
        self->vtable = kVtPanelBase;
        for (int i = 0; i < kSettingsItemCount; i++)
            ItemAt(self, i)->vtable = kVtMenuItem;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x284FB0
void SettingsPanel_LinkValues(SettingsPanel* self) {
    SettingsValues* values = ValuesOf(self);
    self->camera.base.value    = guest::addr(&values->camera);
    self->vibration.base.value = guest::addr(&values->vibration);
    self->subtitles.base.value = guest::addr(&values->subtitles);
    self->blood.base.value     = guest::addr(&values->blood);
    self->audio.base.value     = guest::addr(&values->audio);
    self->bgmTest.base.value   = guest::addr(&values->bgmTest);
    self->bgmVolume.base.value = guest::addr(&values->bgmVolume);
    self->seTest.base.value    = guest::addr(&values->seTest);
    self->seVolume.base.value  = guest::addr(&values->seVolume);
}

// 0x284B00
void SettingsPanel_LoadPositions(SettingsPanel* self) {
    for (int i = 0; i < kSettingsItemCount; i++)
        ItemCall<void>(ItemAt(self, i), kItemLoad);
}

// 0x283EC0
int SettingsPanel_GetHint(SettingsPanel* self) {
    MenuItem* item = Current(self);
    if (item == &self->bgmTest.base || item == &self->seTest.base)
        return 2;
    return item->value == 0 ? 1 : 0;
}

// The music volume as the gauge shows it, applied at once.
static void ApplyBgmVolume(SettingsPanel* self) {
    gBgmVolume = (int)(((float)ValuesOf(self)->bgmVolume.value / 10.0f) * 10.0f + 0.5f);
    Bgm_ApplyVolume();
}

// 0x284190 — back from the Sound sub-menu
static void SettingsPanel_LeaveSound(SettingsPanel* self) {
    self->list = guest::addr(&self->items[kItemCamera]);
    self->listCount = 9;
    self->cursor = kItemSound;
    for (int i = kItemAudio; i < kSettingsItemCount; i++)
        ItemCall<void>(ItemAt(self, i), kItemHide, false);
    for (int i = 0; i < 9; i++)
        ItemCall<void>(ItemAt(self, i), kItemShow, i == self->cursor);
    Sound_PlaySE(kSeMenuOpen, 0, 0x3FFF);
    if (self->testBgm != -1) {
        Bgm_Stop(self->testBgm);
        self->testBgm = -1;
    }
    if (self->savedBgm != -1) {
        Bgm_Play(self->savedBgm, 1, 0, 0x3FFF);
        Bgm_TogglePause();
        self->savedBgm = -1;
    }
}

// 0x283F10 — Cross on the current item
static int SettingsPanel_Confirm(SettingsPanel* self) {
    MenuItem* item = Current(self);
    ItemCall<int>(item, kItemConfirm);

    if (item == &self->sound.base) {
        self->list = guest::addr(&self->items[kItemAudio]);
        self->listCount = 6;
        self->cursor = 0;
        for (int i = 0; i < 9; i++)
            ItemCall<void>(ItemAt(self, i), kItemHide, false);
        for (int i = 0; i < 6; i++)
            ItemCall<void>(ItemAt(self, kItemAudio + i), kItemShow, i == self->cursor);
        Sound_PlaySE(kSeMenuOpen, 0, 0x3FFF);
    } else if (item == &self->soundEnd.base) {
        SettingsPanel_LeaveSound(self);
    } else {
        if (item == &self->end.base)       return kSettingsEnd;
        if (item == &self->reset.base)     return kSettingsReset;
        if (item == &self->screenPos.base) return kSettingsScreenPos;
        if (item == &self->colorBars.base) return kSettingsColorBars;
        if (item == &self->bgmTest.base) {
            if (self->testBgm != -1)
                Bgm_Stop(self->testBgm);
            self->testBgm = ValuesOf(self)->bgmTest.value;
            Bgm_Play(self->testBgm, 1, 0, 0x3FFF);
            ApplyBgmVolume(self);
            return kSettingsNone;
        }
        if (item == &self->seTest.base)
            Sound_PlaySE((u32)ValuesOf(self)->seTest.value, 0, 0x3FFF);
        return kSettingsNone;
    }
    Sound_PlaySE(kSeConfirm, 0, 0x3FFF);
    return kSettingsNone;
}

// 0x2842A0 — left / right change the current item's value
static void SettingsPanel_UpdateValue(SettingsPanel* self) {
    PadRepeat* right = &self->rightLeft.a;
    PadRepeat* left = &self->rightLeft.b;
    PadRepeat_Update(right);
    PadRepeat_Update(left);

    bool changed = false;
    MenuValue* value = guest::ptr<MenuValue>(Current(self)->value);
    if (!PadRepeat_IsHeld(left) && right->fired != 0) {
        if (value)
            changed = VirtualCall<bool, MenuValue*>(value->vtable, kValueNext, value);
    } else if (!PadRepeat_IsHeld(right) && left->fired != 0) {
        if (value)
            changed = VirtualCall<bool, MenuValue*>(value->vtable, kValuePrev, value);
    }
    if (!changed)
        return;

    Sound_PlaySE(kSeChange, 0, 0x3FFF);
    SettingsValues* values = ValuesOf(self);
    MenuItem* item = Current(self);
    if (item == &self->vibration.base && values->vibration.index == 1) {
        // Turning vibration on gives a short rumble.
        u8 pattern[0x24];
        memcpy(pattern, guest::ptr<const u8>(0x2DB300u), sizeof(pattern));
        Pad_SetVibration(PadMgr_Get(), 0, pattern);
    }
    if (item == &self->bgmVolume.base) {
        ApplyBgmVolume(self);
    } else if (item == &self->seVolume.base) {
        gSeVolume = (int)(((float)values->seVolume.value / 10.0f) * 10.0f + 0.5f);
        gSeVolume2 = gSeVolume;
    }
}

// 0x284510 — up / down move the cursor, wrapping
static void SettingsPanel_UpdateCursor(SettingsPanel* self) {
    PadRepeat* up = &self->upDown.a;
    PadRepeat* down = &self->upDown.b;
    PadRepeat_Update(up);
    PadRepeat_Update(down);

    const bool goDown = !PadRepeat_IsHeld(up) && down->fired != 0;
    const bool goUp   = !PadRepeat_IsHeld(down) && up->fired != 0;
    if (goUp || goDown)
        ItemCall<void>(Current(self), kItemBlur);
    if (goDown) {
        if (++self->cursor >= self->listCount)
            self->cursor = 0;
    } else if (goUp) {
        if (--self->cursor < 0)
            self->cursor = self->listCount - 1;
    }
    if (goUp || goDown) {
        ItemCall<void>(Current(self), kItemFocus);
        Sound_PlaySE(kSeCursor, 0, 0x3FFF);
    }
}

// 0x2846E0
int SettingsPanel_Update(SettingsPanel* self) {
    u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);
    if (pressed & kPadR2)
        LoadLabelStyle();
    for (int i = 0; i < kSettingsItemCount; i++)
        ItemCall<void>(ItemAt(self, i), kItemUpdate);
    MenuCursor_Update(&self->pinwheel);

    if (pressed & kPadCross) {
        MenuCursor_Kick(&self->pinwheel);
        return SettingsPanel_Confirm(self);
    }
    if ((pressed & kPadTriangle) && self->cursor != self->listCount - 1) {
        // Cancel jumps to the last entry ("End").
        ItemCall<void>(Current(self), kItemBlur);
        self->cursor = self->listCount - 1;
        ItemCall<void>(Current(self), kItemFocus);
        return kSettingsNone;
    }
    SettingsPanel_UpdateCursor(self);
    SettingsPanel_UpdateValue(self);
    return kSettingsNone;
}

// 0x283D70
void SettingsPanel_Draw(SettingsPanel* self) {
    for (int i = 0; i < kSettingsItemCount; i++)
        ItemCall<void>(ItemAt(self, i), kItemDraw);
    s32 pos[2];
    MenuItem* item = Current(self);
    ItemGetPos(item, kItemGetCursorPos, pos);
    MenuCursor_Draw(&self->pinwheel, pos,
                    guest::ptr<SpriteSet>(guest::ptr<MenuContext>(self->context)->spriteSet));
}
