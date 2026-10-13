#pragma once
#include "hal/ps2_types.h"
#include "engine/containers.h"

// Screen layouts (Layout/*.lif + *.pim): the static artwork of a menu screen
// as a list of flat polygons and textured sprites, authored with the screen
// centre as origin and y pointing up.  `layout/list` (asset 0x2C) names the
// ten layouts; a ScreenLayout is created with an index into that list.
//
// .lif file:  u32 itemCount, itemCount × LayoutItem,
//             u32 animCount, animCount × { s32 duration, s32 frameCount,
//                                          frameCount × LayoutFrame }

enum LayoutId {
    kLayoutNameInput = 0,
    kLayoutStatusMenuCommon,
    kLayoutWeaponSelectMenu,
    kLayoutItemMenu,
    kLayoutAreaMapMenu,
    kLayoutWeaponSheet,
    kLayoutSafeArea,
    kLayoutOptionScreen,
    kLayoutCharacterSetup,
    kLayoutMcardSelect,
};

struct LayoutVertex {
    u32 rgba;       // +0x00
    s32 x, y;       // +0x04  pixels from the screen centre, y up
    u32 z;          // +0x0C
    s32 u, v;       // +0x10  texels, 12.4 fixed
};

struct LayoutItem {
    u32 flags;      // +0x00  low nibble: 0 line, 1 triangle, 2 quad, 3 sprite; 0x10 additive, 0x20 subtractive
    s32 groupA;     // +0x04
    s32 groupB;     // +0x08
    u32 anim;       // +0x0C  sprites: which LayoutAnim picks the texture
    LayoutVertex v[4];   // sprites use v[0] and v[3]
};

struct LayoutFrame {
    s32 texture;    // descriptor index in the .pim
    s32 clut;       // palette offset
    s32 start;      // first frame it is shown on
};

// 0x14 bytes.  (The original keeps the frames in a std::list.)
struct LayoutAnim {
    s32 frame;
    s32 duration;
    s32 frameCount;
    u32 frames;     // LayoutFrame[]
    u32 _10;
};

struct LayoutData {
    u32 itemCount;      // +0x00
    u32 items;          // +0x04  LayoutItem[]
    GuestVector anims;  // +0x08  LayoutAnim
};

struct ScreenLayout {
    s32 id;             // +0x00  LayoutId
    u32 data;           // +0x04  LayoutData*
    u32 _08;
    u8  ready;          // +0x0C
    u32 file;           // +0x10  ArchiveFile* while loading
    u32 fileBuffer;     // +0x14
    u32 _18;            // +0x18  std::vector<bool> of visible items:
    u32 visibleCount;   // +0x1C    number of bits
    u32 visibleBits;    // +0x20    u32[]
    u32 offsets;        // +0x24  {s32 x, y}[] per item
    u32 spriteSet;      // +0x28
    u32 blend;          // +0x2C  blend bits of the item last drawn
};

extern "C" {
    ScreenLayout* ScreenLayout_Ctor(ScreenLayout* self);                // 0x168B10
    ScreenLayout* ScreenLayout_Dtor(ScreenLayout* self, s16 flag);      // 0x168A10
    void ScreenLayout_Init(ScreenLayout* self);                         // 0x168A00  empty
    // Drives the load; true once the layout can be drawn.
    bool ScreenLayout_Poll(ScreenLayout* self);                         // 0x1688A0
    void ScreenLayout_SetDepth(ScreenLayout* self, int z);              // 0x168180
    // Shows or hides every item whose groupB (byGroupA = 0) or groupA matches.
    void ScreenLayout_SetGroupVisible(ScreenLayout* self, int byGroupA, bool visible, int group);  // 0x168190
    void ScreenLayout_ShowGroupB(ScreenLayout* self, int group);        // 0x1682C0
    void ScreenLayout_HideGroupB(ScreenLayout* self, int group);        // 0x1682B0
    void ScreenLayout_ShowGroup(ScreenLayout* self, int group);         // 0x1682E0  by groupA
    void ScreenLayout_HideGroup(ScreenLayout* self, int group);         // 0x1682D0  by groupA
    // Where the screen's code should put things: the first item of `group`
    // (groupA; `sub` additionally selects by groupB unless −1) marks a spot.
    // Coordinates are the layout's: pixels from the screen centre, y up.
    void ScreenLayout_GetGroupOrigin(s32 out[2], ScreenLayout* self, int group, int sub);   // 0x1682F0  first corner
    void ScreenLayout_GetGroupCenter(s32 out[2], ScreenLayout* self, int group, int sub);   // 0x1683D0
    void ScreenLayout_GetGroupRect(s32 out[4], ScreenLayout* self, int group, int sub);     // 0x168470  x0, y0, x1, y1
    // Moves the first item of the group by `offset` pixels when drawn.
    void ScreenLayout_SetGroupOffset(ScreenLayout* self, const s32 offset[2], int group, int sub);  // 0x167E90
    // Moves every item whose groupB matches.
    void ScreenLayout_SetGroupBOffset(ScreenLayout* self, const s32 offset[2], int groupB);         // 0x167FC0
    void ScreenLayout_SetGroupDepth(ScreenLayout* self, u32 z, int groupB);     // 0x1680E0
    void ScreenLayout_Draw(ScreenLayout* self);                         // 0x167C60
    // For screens that place the layout's pieces themselves:
    const LayoutItem* ScreenLayout_FindItem(ScreenLayout* self, int group);                         // 0x13BCF0
    void ScreenLayout_DrawSprite(ScreenLayout* self, const LayoutItem* item, const s32 offset[2]);  // 0x167A10
    void ScreenLayout_DrawSpriteAt(ScreenLayout* self, const LayoutItem* item, const s32 at[2]);    // 0x167660
    void ScreenLayout_FillItem(ScreenLayout* self, const LayoutItem* item, const s32 at[2], u32 rgba);  // 0x167500
}
