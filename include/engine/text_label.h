#pragma once
#include "hal/ps2_types.h"

// A shared, copy-on-write 16-bit string (0x16CF80–0x16DC20) and the text
// label built on it (0x224920–0x224F60).

struct WStringRep {
    u32 data;           // u16[]
    s32 capacity;       // characters
    u32 vtable;         // 0x2EAF10
    s32 size;           // characters in use, counting the terminator
};

struct WString {
    u32 rep;            // WStringRep*, shared
    u32 refs;           // s32*: how many strings share it
    u32 vtable;         // 0x2EAF20
};

// A piece of text at a layout position: either a fixed string (`text`) or its
// own copy (`string`).
struct TextLabel {
    s32   x, y;         // +0x00  layout coordinates
    WString string;     // +0x08
    u32   vtable;       // +0x14  0x2EAF30
    u32   text;         // +0x18  const u16*
    s32   width, height;// +0x1C  pixels
    float scale;        // +0x24
    u32   useString;    // +0x28
};

extern "C" {
    WString* WString_Ctor(WString* self);                               // 0x16D800
    void     WString_Reserve(WString* self, int length);                // 0x16D3E0
    void     WString_Resize(WString* self, int size);                   // 0x16DC20
    u16*     WString_At(WString* self, int index);                      // 0x16DBC0
    u16*     WString_Begin(WString* self);                              // 0x189D70
    WString* WString_Assign(WString* self, const u16* begin, const u16* end);   // 0x189CC0
    void     WString_Share(WString* self, const WString* other);        // inlined
    void     WString_Release(WString* self);                            // inlined

    TextLabel* TextLabel_Ctor(TextLabel* self);                         // 0x224F60
    TextLabel* TextLabel_Copy(TextLabel* self, const TextLabel* other); // 0x224E00
    TextLabel* TextLabel_Dtor(TextLabel* self, s16 flag);               // 0x224D10
    void TextLabel_Clear(TextLabel* self);                              // 0x224CD0
    // Uses `text` where it is; it must outlive the label.
    bool TextLabel_SetText(TextLabel* self, const u16* text);           // 0x224BD0
    // Keeps a copy of `text`.
    bool TextLabel_SetString(TextLabel* self, const u16* text);         // 0x224C40
    bool TextLabel_SetScale(TextLabel* self, float scale);              // 0x224B00
    void TextLabel_Draw(TextLabel* self, u32 rgba, u32 z);              // 0x224A10
    void TextLabel_DrawCentered(TextLabel* self, u32 rgba, u32 z);      // 0x224920
    bool Text_MeasurePixels(const u16* text, s32* size);                // 0x225DC0
}
