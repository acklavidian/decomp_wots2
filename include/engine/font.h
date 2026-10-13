#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"

// Bitmap font (Font/Samu2.Fnt + Font/Samu2.Pim) and the menu text table.
//
// The Font object itself only holds a stack of saved colours; the glyph table,
// the texture descriptors and the current colours are static members (globals
// at 0x2EE534–0x2EE54C, 0x2EDA58 and 0x2EDA5C).

struct GsDevice;

// A std::vector<u32> of colours pushed by the "set colour" escape code.
struct Font {
    GuestVector colors;   // +0x00
};

// One entry of the .Fnt file, sorted by character code.
struct FontGlyph {
    u16 code;       // +0  UTF-16 character
    u16 u, v;       // +2  top-left texel
    u8  texture;    // +6  low nibble: texture descriptor; high nibble: uses the fixed colour
    u8  clut;       // +7  added to the descriptor's palette block
    u8  width;      // +8  texels
    u8  height;     // +9
};

// Text metrics, in 1/16 pixel.  There are two of these: one describing the
// last string drawn or measured (Font_GetExtent), one the last glyph looked up
// (where w / h are whole pixels).
struct FontExtent {
    u16 _0;
    s16 x, y;           // +2  where the string started
    s16 endX, endY;     // +6  where it ended
    u16 w, h;           // +A  size
};

// Escape codes inside strings: 0x1B 0x01 <u32 rgba> sets the colour (saving
// the old one), 0x1B 0x02 restores it.
enum : u16 { kTextEscape = 0x1B, kTextPushColor = 1, kTextPopColor = 2 };

// Text laid out in a box (Font_DrawBox).  Coordinates in 1/16 pixel.
struct TextBox {
    u32   text;         // +0x00  const u16*
    float scale;        // +0x04
    u16   x, y;         // +0x08
    u32   z;            // +0x0C
    u16   w, h;         // +0x10
    u32   rgba;         // +0x14
    u16   flags;        // +0x18
};
enum : u16 {
    kTextBoxWrap = 1,       // start a new line at the right edge
    kTextBoxClip = 2,       // stop at the bottom edge
    kTextBoxCentered = 4,   // centred on (x, y)
    kTextBoxRight = 0x10,   // against the right edge
    kTextBoxColor = 0x20,   // drawn in `rgba`
    kTextBoxFixedA = 0x40,  // every character as wide as the widest
    kTextBoxFixedB = 0x80,
};

extern "C" {
    int   Font_DrawBox(Font* self, TextBox* box);               // 0x1536E0
    Font* Font_Ctor(Font* self, GsDevice* dev);                 // 0x154FD0
    void  Font_SetTexture(Font* self, void* pim);               // 0x153630
    void  Font_SetGlyphs(Font* self, const u16* fnt);           // 0x1535D0
    u32   Font_SetColor(Font* self, u32 rgba);                  // 0x1536D0  → previous colour
    // The colour of glyphs that carry their own (icons); → the previous one.
    u32   Font_SetFixedColor(Font* self, u32 rgba);             // 0x154F90
    u16   Font_ParseEscape(Font* self, const u16** text);       // 0x1534B0
    const FontGlyph* Font_FindGlyph(Font* self, u16 code);      // 0x154E80
    int   Font_DrawGlyph(Font* self, u16 code, int x, int y, u32 z, float scale);   // 0x154990
    // x / y in 1/16 pixel (GS primitive coordinates).  lineScale 0 = use scale.
    int   Font_DrawString(Font* self, const u16* text, int x, int y, u32 z,
                          float scale, float lineScale);        // 0x154510

    // Sets the extent to the size of `text` without drawing it.
    int   Font_Measure(Font* self, const u16* text, float scale);           // 0x154320
    FontExtent* Font_GetExtent(Font* self);                                 // 0x1536C0
    // Each line centred on x; the block centred on y.
    int   Font_DrawCentered(Font* self, const u16* text, int x, int y, u32 z,
                            float scale, float lineScale);                  // 0x153E90
    // Unscaled variants (the centred one sits 4 pixels left of centre).
    int   Font_DrawGlyphPlain(Font* self, u16 code, s16 x, s16 y, u32 z);   // 0x154CC0
    int   Font_DrawStringPlain(Font* self, const u16* text, int x, int y, u32 z);    // 0x1547E0
    int   Font_DrawCenteredPlain(Font* self, const u16* text, int x, int y, u32 z);  // 0x154100

    // 16-bit strings
    // printf into a 16-bit string; only digits and "+-,." survive.  → length, or −1.
    int   WStr_Format(u16* dst, u16 maxLength, const char* format, ...);    // 0x16E400
    int   WStr_Length(const u16* s);                            // 0x16E260  −1 if over 1023
    int   WStr_Copy(u16* dst, const u16* src);                  // 0x16E120
    int   WStr_Append(u16* dst, const u16* src, s16 maxLength); // 0x16E160

    // The table of menu strings (menu/menutext.txt): one string per line,
    // numbered from 0.
    struct TextTable;
    TextTable* TextTable_Get();                                 // 0x176D90
    void       TextTable_Load(TextTable* table);                // 0x176C70
    void       TextTable_Parse(TextTable* table, u16* text, int bytes);   // 0x176AF0
    const u16* TextTable_Find(TextTable* table, int id);        // inlined map lookup (0x170B90)
}
