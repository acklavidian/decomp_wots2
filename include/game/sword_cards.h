#pragma once
#include "hal/ps2_types.h"

// A sword's card (the weapon column, 0x28A070–0x28BC70; src/game/swords_page.cpp)
// for the screens that show them: the status menu's carousel and the sword
// safe.  A card is 0xE0 bytes; the first 0xC4 are the class itself and the
// rest belongs to whoever lays the cards out.

// What a layout sets on a card before it is drawn.
struct CardHead {
    u16 x, y;           // +0x00  1/16 px
    u32 z;              // +0x04
    float alpha;        // +0x08
    u8  alphaByte;      // +0x0C
    u8  shade;          // +0x0D  a dark sheet over it (0 = lit)
    u8  marked;         // +0x0E  the "readied" mark
};

namespace swordcards {
    // The base of a card holder (0x28A370; 0x138 bytes: the card layout out
    // of the weapon column's numbers, then the mark's records and a vtable).
    void  HolderInit(void* holder);
    void  HolderFree(void* holder);                     // its mark records
    void  Mark(void* card);                             // 0x28B4A0: readied — the mark is stamped on
    void  Unmark(void* card);                           // 0x28B480
    bool  Ready();                                      // 0x28A150: the column's sprites are in
    void  BindSprites();
    void* NewCard(void* holder, u32 sword, u32 vtable); // 0x28BC70 (+ 0x28A070); 0xE0 bytes, the tail zeroed
    void  FreeCard(void* card);                         // vtable +0x08
    void  SetTexts(void* card);                         // 0x28B500
    void  Update(void* card);                           // 0x28B3C0
    void  Draw(void* card);                             // 0x28B2D0  all but the picture
    void  DrawPicture(void* card);                      // 0x28A6B0

    // The sword pictures (Common/weaponimage/list): table = {entries, count},
    // an entry (8 bytes) is one picture, read when first drawn.
    u32*  ImageTable();                                 // 0x2A37B0
    void  ImageDraw(void* image, void* params);         // 0x2A3990  SpriteSet_DrawEx params
    bool  ImageLoaded(void* image);                     // 0x2A3890
    void  ImageFree(void* image);                       // 0x2A3A70
    void  ImageFreeAll();                               // 0x2A3720
}
