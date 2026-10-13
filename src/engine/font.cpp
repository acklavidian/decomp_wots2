// Bitmap font — decompiled from 0x1534B0–0x154FD0.
//
// Glyphs come from Font/Samu2.Fnt (a sorted table of FontGlyph) and are drawn
// as textured sprites from the 8-bit textures of Font/Samu2.Pim.

#include "engine/font.h"
#include "engine/gs_device.h"
#include "native/guest.h"

#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);       // 0x100820
    void  MemFree(void* p);         // 0x100650
}

// ── Static members ────────────────────────────────────────────────────────
static const GuestPtrVar<GsDevice>  sDevice{0x2EE534u};
static u16&  sCharHeight   = *guest::ptr<u16>(0x2EE538u);   // 22 px in 1/16 px; not read here
static u16&  sLineHeight   = *guest::ptr<u16>(0x2EE53Au);   // 22 px in 1/16 px
static u32&  sTextures     = *guest::ptr<u32>(0x2EE544u);   // copy of the .Pim descriptors
static u16&  sGlyphCount   = *guest::ptr<u16>(0x2EE548u);
static u32&  sGlyphs       = *guest::ptr<u32>(0x2EE54Cu);   // FontGlyph[]
static u32&  sColor        = *guest::ptr<u32>(0x2EDA58u);   // current text colour
static u32&  sFixedColor   = *guest::ptr<u32>(0x2EDA5Cu);   // for glyphs with their own colours
static FontExtent& sExtent = *guest::ptr<FontExtent>(0x308DE0u);   // last string drawn
static FontExtent& sGlyph  = *guest::ptr<FontExtent>(0x308DF0u);   // last glyph looked up

// Texture descriptor fields (see the .Pim notes in the project notes).
struct PimDesc {
    u8  _0[2];
    u8  psm;            // +0x02
    u8  _3;
    u16 tbp;            // +0x04  texture base pointer
    u16 tbw;            // +0x06  buffer width / 64
    u16 log2w;          // +0x08
    u16 log2h;          // +0x0A
    u8  _c[0x12];
    u16 cbp;            // +0x1E  palette base pointer
    u8  _20[8];
    u16 remaining;      // +0x28
    u8  _2a[6];
};
static_assert(sizeof(PimDesc) == 0x30);

// ── 0x154FD0 ──────────────────────────────────────────────────────────────
Font* Font_Ctor(Font* self, GsDevice* dev) {
    self->colors.capacity = 0;
    self->colors.count = 0;
    self->colors.data = 0;

    memset(&sExtent, 0, sizeof(FontExtent));
    *guest::ptr<u16>(0x2EE53Cu) = 0;
    *guest::ptr<u16>(0x2EE53Eu) = 0;
    sTextures = 0;
    sGlyphCount = 0;
    sGlyphs = 0;
    *guest::ptr<u16>(0x2EE540u) = 0;
    *guest::ptr<u16>(0x2EE542u) = 0;
    *guest::ptr<u32>(sDevice.address) = guest::addr(dev);
    sGlyph = sExtent;

    self->colors.count = 0;
    sColor = 0x80808080u;
    sFixedColor = 0x80FFFFFFu;
    sCharHeight = 0x160;
    sLineHeight = 0x160;
    return self;
}

// ── 0x153630 ──────────────────────────────────────────────────────────────
// Uploads the font textures and keeps a copy of their descriptors; the file
// buffer itself can be freed afterwards.
void Font_SetTexture(Font*, void* pim) {
    pim = Pim_Relocate(pim);
    GsDevice_BeginFrame(*sDevice);
    GsDevice_UploadTextures(*sDevice, pim);
    GsDevice_EndFrame(*sDevice);
    GsDevice_BeginFrame(*sDevice);
    sceGsSyncPath(0, 0);

    u32 bytes = static_cast<PimDesc*>(pim)->remaining * 0x30u;
    void* copy = MemAlloc(bytes);
    sTextures = guest::addr(copy);
    memcpy(copy, pim, bytes);
}

// ── 0x1535D0 ──────────────────────────────────────────────────────────────
// `fnt` is the .Fnt file: a glyph count followed by the glyph table.
void Font_SetGlyphs(Font*, const u16* fnt) {
    sGlyphCount = fnt[0];
    void* copy = MemAlloc(sGlyphCount * 10u);
    sGlyphs = guest::addr(copy);
    memcpy(copy, fnt + 1, sGlyphCount * 10u);
}

// ── 0x1536D0 ──────────────────────────────────────────────────────────────
u32 Font_SetColor(Font*, u32 rgba) {
    u32 previous = sColor;
    sColor = rgba;
    return previous;
}

// ── 0x154F90 ──────────────────────────────────────────────────────────────
u32 Font_SetFixedColor(Font*, u32 rgba) {
    u32 previous = sFixedColor;
    sFixedColor = rgba;
    return previous;
}

// ── 0x1534B0 ──────────────────────────────────────────────────────────────
// Consumes one escape sequence at *text, if there is one, and returns the
// character that follows.
u16 Font_ParseEscape(Font* self, const u16** text) {
    if (**text == kTextEscape) {
        (*text)++;
        if (**text == kTextPopColor) {
            (*text)++;
            if (self->colors.count != 0) {
                sColor = guest::ptr<u32>(self->colors.data)[self->colors.count - 1];
                self->colors.count--;
            }
            // (the original prints an error when the stack is empty)
        } else if (**text == kTextPushColor) {
            (*text)++;
            u32 previous = sColor;
            memcpy(&sColor, *text, 4);
            VectorU32_PushBack(&self->colors, previous);
            *text += 2;
        }
    }
    return **text;
}

// ── 0x154E80 ──────────────────────────────────────────────────────────────
// Binary search of the glyph table.  Also records the glyph's size.
const FontGlyph* Font_FindGlyph(Font*, u16 code) {
    if (code > 0x1F) {
        const FontGlyph* glyphs = guest::ptr<FontGlyph>(sGlyphs);
        u32 low = 0, high = sGlyphCount;
        while (low < high) {
            u32 mid = (low + high) >> 1;
            u16 c = glyphs[mid].code;
            if (c == code) {
                sGlyph.w = glyphs[mid].width;
                sGlyph.h = glyphs[mid].height;
                return &glyphs[mid];
            }
            if (c < code)
                low = mid + 1;
            else
                high = mid;
        }
    }
    sGlyph.h = 0;
    sGlyph.w = 0;
    return nullptr;
}

// ── 0x154990 ──────────────────────────────────────────────────────────────
// Draws one character with its top-left corner at (x, y).  Returns 0 if the
// font has no such glyph.
int Font_DrawGlyph(Font* self, u16 code, int x, int y, u32 z, float scale) {
    const FontGlyph* g = Font_FindGlyph(self, code);
    if (!g)
        return 0;

    const PimDesc* tex = guest::ptr<PimDesc>(sTextures) + (g->texture & 0x0F);
    GsSpritePacket p;
    memset(&p, 0, sizeof(p));
    p.prim = 0x156;
    p.tex0 = (u64)tex->tbp
           | (u64)tex->tbw << 14
           | (u64)tex->psm << 20
           | (u64)tex->log2w << 26
           | (u64)tex->log2h << 30
           | 0x400000000ull                               // texture has alpha
           | (u64)(u32)(tex->cbp + g->clut) << 37
           | 0x2000000000000000ull;                       // load the palette

    u32 rgba = (g->texture & 0xF0) == 0 ? sColor : sFixedColor;
    p.corner[0].u = (s16)(g->u * 16);
    p.corner[0].v = (s16)(g->v * 16);
    p.corner[0].rgba = rgba;
    p.corner[0].x = (u16)x;
    p.corner[0].y = (u16)y;
    p.corner[0].z = z;
    p.corner[1].u = (s16)(p.corner[0].u + g->width * 16);
    p.corner[1].v = (s16)(p.corner[0].v + g->height * 16);
    p.corner[1].rgba = rgba;
    p.corner[1].x = (u16)(int)((float)x + scale * (float)(g->width << 4));
    p.corner[1].y = (u16)(int)((float)y + scale * (float)(g->height << 4));
    p.corner[1].z = z;
    GsDevice_DrawSprite(*sDevice, &p);

    sGlyph.w = (u16)(int)((float)sGlyph.w * scale);
    sGlyph.h = (u16)(int)((float)sGlyph.h * scale);
    return 1;
}

// ── 0x154510 ──────────────────────────────────────────────────────────────
// Draws a string, left-aligned at (x, y).  CR, LF or a pair of them starts a
// new line.  Always returns 0.
int Font_DrawString(Font* self, const u16* text, int x, int y, u32 z,
                    float scale, float lineScale) {
    int penX = x, penY = y;

    while (*text != 0 && Font_ParseEscape(self, &text) != 0) {
        bool newline = false;
        if (*text == '\n' || *text == '\r') {
            if (text[1] == '\n' || text[1] == '\r')
                text += 2;
            else
                text += 1;
            newline = true;
        }
        if (newline) {
            float step = lineScale == 0.0f ? scale : lineScale;
            penY = (int)((float)penY + (float)sLineHeight * step);
            penX = x;
        } else {
            if (Font_DrawGlyph(self, *text, penX, penY, z, scale))
                penX += sGlyph.w * 16;
            text++;
        }
    }

    sExtent.x = (s16)x;
    sExtent.y = (s16)y;
    sExtent.endX = (s16)(penX + sGlyph.w * 16);
    sExtent.endY = (s16)(int)((float)penY + (float)sLineHeight * scale);
    return 0;
}

// True (and steps over it) when *text is at a line break: CR, LF or a pair.
static bool SkipNewline(const u16** text) {
    const u16* p = *text;
    if (*p != '\n' && *p != '\r')
        return false;
    *text = (p[1] == '\n' || p[1] == '\r') ? p + 2 : p + 1;
    return true;
}

// ── 0x154320 ──────────────────────────────────────────────────────────────
int Font_Measure(Font* self, const u16* text, float scale) {
    const int lineHeight = (int)((float)sLineHeight * scale);
    int width = 0, widest = 0, height = lineHeight;

    while (*text != 0 && Font_ParseEscape(self, &text) != 0) {
        if (SkipNewline(&text)) {
            height += lineHeight;
            if (widest < width) widest = width;
            width = 0;
        } else {
            if (Font_FindGlyph(self, *text))
                width = (int)((float)width + scale * (float)(sGlyph.w << 4));
            text++;
        }
    }
    if (widest < width) widest = width;

    sExtent.x = 0;
    sExtent.y = 0;
    sExtent.endX = (s16)widest;
    sExtent.w = (u16)widest;
    sExtent.endY = (s16)height;
    sExtent.h = (u16)height;
    return 0;
}

// ── 0x1536C0 ──────────────────────────────────────────────────────────────
FontExtent* Font_GetExtent(Font*) {
    return &sExtent;
}

// Copies one line (at most 64 characters) and steps past its line break.
static void TakeLine(const u16** text, u16* line) {
    int n = 0;
    do {
        if (**text == 0 || SkipNewline(text))
            break;
        line[n++] = *(*text)++;
    } while (n < 0x40);
    line[n] = 0;
}

// ── 0x153E90 ──────────────────────────────────────────────────────────────
int Font_DrawCentered(Font* self, const u16* text, int x, int y, u32 z,
                      float scale, float lineScale) {
    Font_Measure(self, text, scale);
    const FontExtent whole = sExtent;
    const int halfHeight = whole.h >> 1;

    while (*text != 0 && Font_ParseEscape(self, &text) != 0) {
        u16 line[0x48];
        TakeLine(&text, line);
        Font_Measure(self, line, scale);
        Font_DrawString(self, line, x - (sExtent.w >> 1), y - halfHeight, z, scale, lineScale);
        y = (int)((float)y + (float)sLineHeight * scale);
    }
    sExtent = whole;
    return 0;
}

// ── 0x154CC0 ──────────────────────────────────────────────────────────────
int Font_DrawGlyphPlain(Font* self, u16 code, s16 x, s16 y, u32 z) {
    return Font_DrawGlyph(self, code, (u16)x, (u16)y, z, 1.0f);
}

// ── 0x1547E0 ──────────────────────────────────────────────────────────────
int Font_DrawStringPlain(Font* self, const u16* text, int x, int y, u32 z) {
    int penX = x, penY = y;

    while (*text != 0 && Font_ParseEscape(self, &text) != 0) {
        if (SkipNewline(&text)) {
            penY += sLineHeight;
            penX = x;
        } else {
            if (Font_DrawGlyphPlain(self, *text, (s16)penX, (s16)penY, z))
                penX += sGlyph.w * 16;
            text++;
        }
    }
    sExtent.x = (s16)x;
    sExtent.y = (s16)y;
    sExtent.endY = (s16)(penY + sLineHeight);
    sExtent.endX = (s16)(penX + sGlyph.w * 16);
    sExtent.h = (u16)(sExtent.endY - (s16)y);
    sExtent.w = (u16)(sExtent.endX - (s16)x);
    return 0;
}

// ── 0x154100 ──────────────────────────────────────────────────────────────
int Font_DrawCenteredPlain(Font* self, const u16* text, int x, int y, u32 z) {
    Font_Measure(self, text, 1.0f);
    const FontExtent whole = sExtent;
    const int halfHeight = whole.h >> 1;

    while (*text != 0 && Font_ParseEscape(self, &text) != 0) {
        u16 line[0x48];
        TakeLine(&text, line);
        Font_Measure(self, line, 1.0f);
        Font_DrawStringPlain(self, line, x - ((sExtent.w >> 1) + 0x40), y - halfHeight, z);
        y += sLineHeight;
    }
    sExtent = whole;
    return 0;
}

// ── 0x132AA0 ──────────────────────────────────────────────────────────────
// Turns the data offsets of a .Pim's descriptors into addresses.
void* Pim_Relocate(void* pim) {
    u32 base = guest::addr(pim);
    for (u8* d = static_cast<u8*>(pim);; d += 0x30) {
        u32* pixels = reinterpret_cast<u32*>(d + 0x18);
        u32* palette = reinterpret_cast<u32*>(d + 0x24);
        if (*pixels != 0)  *pixels += base;
        if (*palette != 0) *palette += base;
        if (*reinterpret_cast<u16*>(d + 0x28) < 2)
            break;
    }
    return pim;
}

// ── 0x153800 ──────────────────────────────────────────────────────────────
// Lays `box->text` out inside the box, drawing it if asked.  Returns how many
// characters were used; the extent gets the size.
static int Font_LayoutBox(Font* self, const TextBox* box, bool draw) {
    static u16& sFixedWidth = *guest::ptr<u16>(0x2EE538u);
    const u16* text = guest::ptr<const u16>(box->text);
    const u16* const start = text;
    const float scale = box->scale;
    u32 maxX = 0xFFFF, maxY = 0xFFFF;
    if (box->flags & kTextBoxWrap)
        maxX = (u32)(box->x + box->w) & 0xFFFF;
    if (box->flags & kTextBoxClip)
        maxY = (u32)(box->y + box->h) & 0xFFFF;
    u32 penX = box->x, penY = box->y, widest = 0;
    s16 count = 0;

    auto newLine = [&]() {
        if (widest < penX)
            widest = penX;
        penX = box->x;
        penY = (u32)(int)((float)penY + (float)sLineHeight * scale) & 0xFFFF;
        return maxY >= penY;
    };
    while (*text != 0 && Font_ParseEscape(self, &text) != 0) {
        if (SkipNewline(&text) || maxX < penX) {
            if (!newLine())
                break;
            continue;
        }
        if (Font_FindGlyph(self, *text)) {
            u32 advance = (u32)(int)(scale * (float)((u32)sGlyph.w << 4)) & 0xFFFF;
            if (box->flags & (kTextBoxFixedA | kTextBoxFixedB))
                advance = (u32)(int)((float)sFixedWidth * scale) & 0xFFFF;
            if (maxX < penX + advance && !newLine())
                break;
            if (draw)
                Font_DrawGlyph(self, *text, (int)penX, (int)penY, box->z, scale);
            penX = (penX + advance) & 0xFFFF;
            count++;
        }
        text++;
    }
    sExtent._0 = (u16)count;
    sExtent.x = (s16)box->x;
    sExtent.y = (s16)box->y;
    sExtent.endX = (s16)(widest < penX ? penX : widest);
    sExtent.endY = (s16)(int)((float)penY + (float)sLineHeight * scale);
    sExtent.w = (u16)(sExtent.endX - (s16)box->x);
    sExtent.h = (u16)(int)((float)(int)(penY - box->y) + (float)sLineHeight * scale);
    return (int)(text - start);
}

// ── 0x1536E0 ──────────────────────────────────────────────────────────────
int Font_DrawBox(Font* self, TextBox* box) {
    const u32 color = sColor;
    const u16 x = box->x, y = box->y;
    if (box->flags & kTextBoxColor)
        sColor = box->rgba;
    if (box->flags & kTextBoxCentered) {
        Font_LayoutBox(self, box, false);
        box->x = (u16)(box->x - (sExtent.w >> 1));
        box->y = (u16)(box->y - (sExtent.h >> 1));
    } else if (box->flags & kTextBoxRight) {
        Font_LayoutBox(self, box, false);
        box->x = (u16)(box->x + (box->w - sExtent.w));
    }
    Font_LayoutBox(self, box, true);
    box->x = x;
    box->y = y;
    sColor = color;
    return 1;
}
