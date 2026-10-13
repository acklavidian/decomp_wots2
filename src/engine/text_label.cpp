// Shared 16-bit string and text label — decompiled from 0x16CF80–0x16DC20,
// 0x189CC0–0x189DF0 and 0x224920–0x224F60.  See include/engine/text_label.h.

#include "engine/text_label.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* MemAllocAligned(u32 align, u32 size);         // 0x1228C8
    void  MemFreeAligned(void* p);                      // 0x122B50
}

static_assert(sizeof(TextLabel) == 0x2C && offsetof(TextLabel, text) == 0x18, "TextLabel");

static const GuestPtrVar<Font> gFont{0x2EE67Cu};
static s32& gLabelOffsetX = *guest::ptr<s32>(0x2EE740u);
static s32& gLabelOffsetY = *guest::ptr<s32>(0x2EE744u);
enum : u32 { kVtRep = 0x2EAF10, kVtString = 0x2EAF20, kVtLabel = 0x2EAF30 };

// ══ WString ═══════════════════════════════════════════════════════════════
static inline WStringRep* Rep(const WString* s) { return guest::ptr<WStringRep>(s->rep); }
static inline s32& Refs(const WString* s) { return *guest::ptr<s32>(s->refs); }

// The original calls the C library's realloc (0x124BD8).
static void Rep_SetCapacity(WStringRep* rep, int capacity) {
    if (capacity == 0) {
        if (rep->data) {
            MemFreeAligned(guest::ptr(rep->data));
            rep->capacity = 0;
            rep->data = 0;
        }
        return;
    }
    u16* data = static_cast<u16*>(MemAllocAligned(0x10, (u32)capacity << 1));
    if (!data)
        return;
    if (rep->data) {
        const int keep = rep->capacity < capacity ? rep->capacity : capacity;
        memcpy(data, guest::ptr(rep->data), (size_t)keep << 1);
        MemFreeAligned(guest::ptr(rep->data));
    }
    rep->data = guest::addr(data);
    rep->capacity = capacity;
}

// 0x16D6D0 — room for twice what is asked (at least 20); shrinks when four times too big
static void Rep_Resize(WStringRep* rep, int size) {
    int want = size << 1;
    if (want < 20)
        want = 20;
    if (size < rep->size) {
        if (rep->capacity >= want << 1)
            Rep_SetCapacity(rep, want);
    } else if (rep->capacity < size) {
        Rep_SetCapacity(rep, want);
    }
    rep->size = size;
}

static WStringRep* Rep_New() {
    WStringRep* rep = static_cast<WStringRep*>(MemAlloc(sizeof(WStringRep)));
    if (rep) {
        rep->vtable = kVtRep;
        rep->data = 0;
        rep->capacity = 0;
        rep->size = 0;
    }
    return rep;
}

// 0x16D4D0 with a null or new rep
static void WString_Adopt(WString* self, WStringRep* rep) {
    WString_Release(self);
    self->rep = guest::addr(rep);
    if (rep) {
        s32* refs = static_cast<s32*>(MemAlloc(4));
        if (refs)
            *refs = 1;
        self->refs = guest::addr(refs);
    }
}

void WString_Release(WString* self) {
    if (self->rep == 0)
        return;
    if (--Refs(self) == 0) {
        WStringRep* rep = Rep(self);
        rep->vtable = kVtRep;
        MemFreeAligned(guest::ptr(rep->data));
        MemFree(rep);
        MemFree(guest::ptr(self->refs));
    }
    self->rep = 0;
}

void WString_Share(WString* self, const WString* other) {
    if (self == other || self->rep == other->rep)
        return;
    WString_Release(self);
    self->rep = other->rep;
    if (self->rep != 0) {
        self->refs = other->refs;
        Refs(self)++;
    }
}

// 0x16CF90 — a private copy before writing
static void WString_Unshare(WString* self) {
    if (self->rep == 0 || Refs(self) == 1)
        return;
    const WStringRep* old = Rep(self);
    WStringRep* rep = Rep_New();
    if (rep) {
        Rep_Resize(rep, old->size);
        memcpy(guest::ptr(rep->data), guest::ptr(old->data), (size_t)old->size << 1);
    }
    WString_Adopt(self, rep);
}

// ── 0x16D800 ──────────────────────────────────────────────────────────────
WString* WString_Ctor(WString* self) {
    self->vtable = kVtString;
    WStringRep* rep = Rep_New();
    self->rep = guest::addr(rep);
    if (rep) {
        s32* refs = static_cast<s32*>(MemAlloc(4));
        if (refs)
            *refs = 1;
        self->refs = guest::addr(refs);
    }
    return self;
}

// ── 0x16DC20 ──────────────────────────────────────────────────────────────
void WString_Resize(WString* self, int size) {
    WString_Unshare(self);
    Rep_Resize(Rep(self), size);
}

// ── 0x16DBC0 ──────────────────────────────────────────────────────────────
u16* WString_At(WString* self, int index) {
    WString_Unshare(self);
    return guest::ptr<u16>(Rep(self)->data) + index;
}

// ── 0x189D70 ──────────────────────────────────────────────────────────────
u16* WString_Begin(WString* self) {
    WString_Unshare(self);
    return guest::ptr<u16>(Rep(self)->data);
}

// ── 0x16D3E0 ──────────────────────────────────────────────────────────────
void WString_Reserve(WString* self, int length) {
    if (length < 0)
        return;
    WString_Resize(self, length + 1);
    *WString_At(self, length) = 0;
}

// ── 0x189CC0 ──────────────────────────────────────────────────────────────
WString* WString_Assign(WString* self, const u16* begin, const u16* end) {
    const int length = (int)(end - begin);
    if (length >= 0) {
        WString_Resize(self, length + 1);
        *WString_At(self, length) = 0;
    }
    u16* out = WString_Begin(self);
    for (; begin != end; begin++)
        *out++ = *begin;
    return self;
}

// ══ TextLabel ═════════════════════════════════════════════════════════════
// ── 0x225DC0 ──────────────────────────────────────────────────────────────
bool Text_MeasurePixels(const u16* text, s32* size) {
    Font_Measure(*gFont, text, 1.0f);
    const FontExtent* extent = Font_GetExtent(*gFont);
    size[0] = extent->w >> 4;
    size[1] = extent->h >> 4;
    return true;
}

// ── 0x224F60 ──────────────────────────────────────────────────────────────
TextLabel* TextLabel_Ctor(TextLabel* self) {
    self->x = 0;
    self->y = 0;
    self->vtable = kVtLabel;
    WString_Ctor(&self->string);
    WString_Reserve(&self->string, 0);
    self->width = 0;
    self->height = 0;
    self->scale = 1.0f;
    self->text = 0;
    self->useString = 1;
    return self;
}

// ── 0x224E00 ──────────────────────────────────────────────────────────────
TextLabel* TextLabel_Copy(TextLabel* self, const TextLabel* other) {
    self->x = 0;
    self->y = 0;
    self->vtable = kVtLabel;
    WString_Ctor(&self->string);
    WString_Reserve(&self->string, 0);
    self->width = 0;
    self->height = 0;
    WString_Share(&self->string, &other->string);
    self->text = other->text;
    self->width = other->width;
    self->height = other->height;
    self->x = other->x;
    self->y = other->y;
    self->scale = other->scale;
    self->useString = other->useString;
    return self;
}

// ── 0x224CD0 ──────────────────────────────────────────────────────────────
void TextLabel_Clear(TextLabel* self) {
    WString_Resize(&self->string, 1);
    *WString_At(&self->string, 0) = 0;
}

// ── 0x224D10 ──────────────────────────────────────────────────────────────
TextLabel* TextLabel_Dtor(TextLabel* self, s16 flag) {
    if (!self)
        return self;
    TextLabel_Clear(self);
    self->vtable = kVtLabel;
    self->string.vtable = kVtString;
    WString_Release(&self->string);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x224BD0 ──────────────────────────────────────────────────────────────
bool TextLabel_SetText(TextLabel* self, const u16* text) {
    if (!text)
        return false;
    self->text = guest::addr(text);
    if (!Text_MeasurePixels(text, &self->width))
        return false;
    self->useString = 0;
    self->scale = 1.0f;
    return true;
}

// ── 0x224C40 ──────────────────────────────────────────────────────────────
bool TextLabel_SetString(TextLabel* self, const u16* text) {
    if (!text)
        return false;
    int length = 0;                                     // 0x189DF0
    while (text[length] != 0)
        length++;
    WString_Assign(&self->string, text, text + length);
    if (!Text_MeasurePixels(guest::ptr<const u16>(Rep(&self->string)->data), &self->width))
        return false;
    self->useString = 1;
    self->scale = 1.0f;
    return true;
}

static const u16* LabelText(const TextLabel* self) {
    return guest::ptr<const u16>(self->useString == 0 ? self->text : Rep(&self->string)->data);
}

// ── 0x224B00 ──────────────────────────────────────────────────────────────
bool TextLabel_SetScale(TextLabel* self, float scale) {
    const u16* text = LabelText(self);
    if (!text || !Text_MeasurePixels(text, &self->width))
        return false;
    self->scale = scale;
    self->width = (int)(((float)self->width + 0.5f) * scale);
    self->height = (int)(((float)self->height + 0.5f) * scale);
    return true;
}

static void DrawLabel(TextLabel* self, u32 rgba, u32 z, bool centered) {
    if (Rep(&self->string)->size == 1 && self->text == 0)
        return;
    Font* font = *gFont;
    Font_SetColor(font, rgba);
    const u16* text = LabelText(self);
    if (!text)
        return;
    int x = (int)Screen_X((self->x + gLabelOffsetX) * 16, 0);
    int y = (int)Screen_Y((self->y + gLabelOffsetY) * -16, 0);
    if (centered)
        Font_DrawCentered(font, text, x, y, z, self->scale, 0.0f);
    else
        Font_DrawString(font, text, x, y, z, self->scale, 0.0f);
}

// ── 0x224A10 ──────────────────────────────────────────────────────────────
void TextLabel_Draw(TextLabel* self, u32 rgba, u32 z) { DrawLabel(self, rgba, z, false); }

// ── 0x224920 ──────────────────────────────────────────────────────────────
void TextLabel_DrawCentered(TextLabel* self, u32 rgba, u32 z) { DrawLabel(self, rgba, z, true); }
