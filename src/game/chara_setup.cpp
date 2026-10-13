// Character setup (new-game screen) — decompiled from 0x24CCE0–0x24ECF0 and
// 0x254300–0x256760.  See include/game/chara_setup.h.

#include "game/chara_setup.h"
#include "game/player_data.h"
#include "game/item_select.h"
#include "game/name_input.h"
#include "engine/archive_file.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "engine/screen_layout.h"
#include "native/guest.h"

#include <cstdio>
#include <cstring>
#include <string>

struct SpriteSet;
extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void  MemSet(void* dst, int value, u32 size);               // 0x123880
    void* PadMgr_Get();                                         // 0x13A920
    u16   Pad_GetHeld(void* mgr, int port);                     // 0x13A0E0
    u16   Pad_GetPressed(void* mgr, int port);                  // 0x13A0C0
    u16   Pad_GetRepeat(void* mgr, int port);                   // 0x139F50
    u16   Pad_GetNewPress(void* mgr, int port);                 // 0x139F70
    void  Sound_PlaySE(u32 id, u32 positional, u32 volume);     // 0x180E70
    int   RenderMgr_TriggerEffect(void* mgr, u32 type, u32 arg1, u32 arg2);  // 0x14F190
    void  GsDevice_SwapBuffers(GsDevice* dev, u32 mode);        // 0x131240

    // Sony libvu0
    void  Matrix_Copy(void* dst, const void* src);              // 0x1073C0
    void  Matrix_Identity(void* m);                             // 0x107428
    void  Matrix_Rotate(void* dst, const void* src, float angle);         // 0x107618
    void  Matrix_Multiply(void* dst, const void* a, const void* b);       // 0x107130

    // Character manager (not decompiled; id is always 0 here)
    void  CharaMgr_Update(CharaMgr* mgr, int load);             // 0x145BE0
    int   CharaMgr_IsLoaded(CharaMgr* mgr, int id);             // 0x1441F0
    char  CharaMgr_IsPlayerReady(CharaMgr* mgr);                // 0x143E50
    void  CharaMgr_Remove(CharaMgr* mgr, int id);               // 0x145DF0
    void  CharaMgr_Add(CharaMgr* mgr, int kind, int id, int charaId, int arg);   // 0x146DA0
    void  CharaMgr_QueueLoad(CharaMgr* mgr, int id);            // 0x145790
    void* CharaMgr_GetTransform(CharaMgr* mgr, int id);         // 0x146D10
    void  CharaMgr_SetTransform(CharaMgr* mgr, int id, const void* matrix);      // 0x146A20
    void  CharaMgr_SetPosition(CharaMgr* mgr, int id, const void* color);           // 0x1468F0
    void  CharaMgr_Draw(CharaMgr* mgr, int pass);               // 0x145830

    // Lighting for the preview (the object at *0x2EE678)
    void  Light_SetMatrices(void* light, const void* directions, const void* colors);   // 0x166920
    void  Light_Upload(void* light, GsDevice* dev);             // 0x166910

    // Item icons (not decompiled)

    // Sub-screens (not decompiled)
    void* SwordSelect_Ctor(void* mem);                          // 0x26F320
    char  SwordSelect_Start(void* self, int mode);              // 0x26F150
    void  SwordSelect_Load(void* self);                         // 0x26EE90
    void  SwordSelect_Update(s32 out[2], void* self);           // 0x26F040
    void  SwordSelect_Draw(void* self);                         // 0x26EA40
    void  SwordSelect_Dtor(void* self, int flag);               // 0x26F270
    void* SwordInfo_Ctor(void* mem);                            // 0x2379C0
    int   SwordInfo_Start(void* self, int sword);               // 0x237840
    void  SwordInfo_Update(void* self);                         // 0x237700
    void  SwordInfo_Draw(void* self);                           // 0x237330
    void  SwordInfo_Dtor(void* self, int flag);                 // 0x237950

    // Player data (not decompiled)
}

// ── Globals ───────────────────────────────────────────────────────────────
static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const GuestPtrVar<void>     gLight{0x2EE678u};
static const GuestPtrVar<Font>     gFont{0x2EE67Cu};
static const GuestPtrVar<void>     gRenderMgr{0x2EE684u};
static const GuestPtrVar<u8>       gPlayerData{0x2EE688u};
static u32& gClearColor      = *guest::ptr<u32>(0x2EE6A8u);
static u32& gLoadingActive   = *guest::ptr<u32>(0x2EE6B4u);
static u32& gLoadingDotTimer = *guest::ptr<u32>(0x2EE6B8u);
static u32& gLoadingDots     = *guest::ptr<u32>(0x2EE6BCu);
static u32& gLoadingFrames   = *guest::ptr<u32>(0x2EE6C0u);
static s32& gFrameRate       = *guest::ptr<s32>(0x2EDA00u);

// Player data fields used here
static inline PlayerData* PD(u8* pd) { return reinterpret_cast<PlayerData*>(pd); }
static inline s32& Score(u8* pd)          { return *reinterpret_cast<s32*>(pd + 0x08); }
static inline s16& SavedChara(u8* pd)     { return *reinterpret_cast<s16*>(pd + 0x112); }
static inline s16& SavedSecret(u8* pd)    { return *reinterpret_cast<s16*>(pd + 0x114); }
static inline s16& SavedHead(u8* pd)      { return *reinterpret_cast<s16*>(pd + 0x116); }
static inline s16& SwordCount(u8* pd)     { return *reinterpret_cast<s16*>(pd + 0x178); }
static inline s16* SwordSlots(u8* pd)     { return reinterpret_cast<s16*>(pd + 0x17A); }
static inline Sword* SwordAt(u8* pd, int i) { return reinterpret_cast<PlayerData*>(pd)->sword + i; }
static inline u8*  ItemSlots(u8* pd)      { return pd + 0x150; }
static inline u16* PlayerName(u8* pd)     { return reinterpret_cast<u16*>(pd + 0x480); }

// ── Tables ────────────────────────────────────────────────────────────────
// Which character a head and a set of clothes make: [head][clothes], −1 = none.
static const s16 (*const kCharaTable)[13] = guest::ptr<const s16[13]>(0x2E6FF0u);
static const s8*  const kHeadGender     = guest::ptr<const s8>(0x2E70C0u);    // per head: 0 male, 1 female
static const s8*  const kClothesGender  = guest::ptr<const s8>(0x2E70C8u);    // per clothes
static const s16* const kDefaultClothes = guest::ptr<const s16>(0x2E70E0u);   // per head
static const s16* const kHeadScore      = guest::ptr<const s16>(0x2E70F0u);   // score needed, per head
static const s16* const kClothesScore   = guest::ptr<const s16>(0x2E7100u);   // per clothes
static const s8*  const kCoverGender    = guest::ptr<const s8>(0x2E7490u);    // per clothes
// The secret characters: {character, head} pairs, −1 ends.
struct SecretChara { s16 chara, head; };
static const SecretChara* const kSecretCharas = guest::ptr<const SecretChara>(0x2DA480u);
// The button sequence that opens them (on "Name"): L1 R2 R1 L2 L1 R2 R1 L2 Square, −1 ends.
static const u32* const kSecretCode = guest::ptr<const u32>(0x2DA570u);

enum : u32 { kVtGridPicker = 0x2ECAC0u, kVtHeadPicker = 0x2ECAA0u, kVtClothesPicker = 0x2ECA80u };

enum {
    kTextSwordLabel = 0x2E, kTextMenuFirst = 0xA6, kTextChoose = 0xAD, kTextDoneHint = 0xAE,
    kTextHeadHint = 0xAF, kTextClothesHint = 0xB0, kTextNeedSword = 0x141,
};
enum { kAssetSetupLighting = 0x21 };
enum { kSeCursor = 2, kSeChange = 3, kSeConfirm = 5, kSeDecide = 6, kSeCancel = 7, kSeSecret = 0x27 };

static inline ScreenLayout* LayoutOf(u32 context) {
    return guest::ptr<ScreenLayout>(guest::ptr<SetupContext>(context)->layout);
}
static inline int TransitionFrames() { return gFrameRate * 32 / 30; }

static void ShowLoading(bool on) {
    if (on) {
        if (!gLoadingActive) {
            gLoadingFrames = 0;
            gLoadingDots = 0;
            gLoadingDotTimer = 0;
        }
        gLoadingActive = 1;
    } else {
        gLoadingFrames = 0;
        gLoadingDots = 0;
        gLoadingDotTimer = 0;
        gLoadingActive = 0;
    }
}

// ══ Hint ══════════════════════════════════════════════════════════════════
// 0x255DA0
static void SetupHint_SetText(SetupHint* self, int textId) {
    self->text = guest::addr(TextTable_Find(TextTable_Get(), textId));
}

// 0x255E00
static void SetupHint_Place(SetupHint* self, ScreenLayout* layout) {
    s32 pos[2];
    ScreenLayout_GetGroupCenter(pos, layout, 0, -1);
    self->x = pos[0];
    self->y = pos[1];
}

// 0x255CE0
static void SetupHint_Draw(SetupHint* self) {
    Font* font = *gFont;
    GsDevice* dev = *gGsDevice;
    if (self->text == 0)
        return;
    GsDevice_End2D(dev);
    Font_SetColor(font, 0x803E3E5Au);
    Font_DrawString(font, guest::ptr<const u16>(self->text), (int)Screen_X(self->x << 4, 0),
                    (int)Screen_Y(self->y * -16, 0), 0, 1.0f, 0.0f);
    GsDevice_Begin2D(dev);
}

// ══ Menu ══════════════════════════════════════════════════════════════════
// 0x256650 (+ 0x256460)
static SetupMenu* SetupMenu_Ctor(SetupMenu* self, SetupContext* context) {
    PadRepeatPair_Ctor(&self->upDown);
    self->upDown.vtable = 0x2EAA00u;
    PadRepeatPair_SetMasks(&self->upDown, kPadUp, kPadDown);
    self->context = guest::addr(context);
    self->cursor = 0;
    self->slideMax = 20;
    for (SetupMenuEntry& e : self->entry) {
        e.slideStep = 0;
        e.slide = 0;
        e.lit = 0;
        e.label = 0;
        e.x = 0;
        e.y = 0;
    }
    PadRepeatPair_SetTiming(&self->upDown, 10, 5, 4);

    static const int kLabels[6] = {0xA6, 0xA7, 0xA8, 0xAA, 0xAB, 0xAC};   // Name Head Clothes Sword Items Done
    TextTable* text = TextTable_Get();
    for (int i = 0; i < 6; i++)
        self->entry[i].label = guest::addr(TextTable_Find(text, kLabels[i]));
    return self;
}

// Entry i has two pieces of layout art: plain (groups 10+i and 0x11+i) and
// selected (0x1E+i and 0x25+i).
static void SetupMenu_Light(SetupMenu* self, int i, bool lit) {
    ScreenLayout* layout = LayoutOf(self->context);
    if (lit) {
        ScreenLayout_ShowGroup(layout, i + 0x1E);
        ScreenLayout_ShowGroup(layout, i + 0x25);
        ScreenLayout_HideGroup(layout, i + 10);
        ScreenLayout_HideGroup(layout, i + 0x11);
    } else {
        ScreenLayout_ShowGroup(layout, i + 10);
        ScreenLayout_ShowGroup(layout, i + 0x11);
        ScreenLayout_HideGroup(layout, i + 0x1E);
        ScreenLayout_HideGroup(layout, i + 0x25);
    }
}

// 0x256340
static void SetupMenu_Place(SetupMenu* self) {
    ScreenLayout* layout = LayoutOf(self->context);
    ScreenLayout_HideGroupB(layout, 3);
    for (int i = 0; i < 6; i++) {
        s32 pos[2];
        ScreenLayout_GetGroupCenter(pos, layout, i + 10, -1);
        self->entry[i].x = pos[0];
        self->entry[i].y = pos[1];
    }
    self->entry[0].slideStep = 5;
    self->entry[0].lit = 1;
    SetupMenu_Light(self, 0, true);
    s32 plain[2], selected[2];
    ScreenLayout_GetGroupOrigin(plain, layout, 10, -1);
    ScreenLayout_GetGroupOrigin(selected, layout, 0x1E, -1);
    self->slideMax = (s16)(plain[0] - selected[0]);
}

// 0x255FE0 — true when the cursor moved
static bool SetupMenu_Update(SetupMenu* self) {
    bool moved = false;
    PadRepeat* up = &self->upDown.a;
    PadRepeat* down = &self->upDown.b;
    PadRepeat_Update(up);
    PadRepeat_Update(down);

    int next = -1;
    if (!PadRepeat_IsHeld(down) && up->fired != 0)
        next = self->cursor - 1 < 0 ? 5 : self->cursor - 1;
    else if (!PadRepeat_IsHeld(up) && down->fired != 0)
        next = self->cursor + 1 > 5 ? 0 : self->cursor + 1;
    if (next >= 0) {
        self->entry[self->cursor].slideStep = -5;
        self->cursor = (s16)next;
        self->entry[next].slideStep = 5;
        self->entry[next].lit = 1;
        SetupMenu_Light(self, next, true);
        Sound_PlaySE(kSeCursor, 0, 0x3FFF);
        moved = true;
    }

    ScreenLayout* layout = LayoutOf(self->context);
    for (int i = 0; i < 6; i++) {
        SetupMenuEntry& e = self->entry[i];
        if (e.slideStep == 0)
            continue;
        e.slide = (s16)(e.slide + e.slideStep);
        if (e.slide < 1) {
            e.slide = 0;
            e.slideStep = 0;
            e.lit = 0;
            SetupMenu_Light(self, i, false);
        } else {
            if (e.slide >= self->slideMax) {
                e.slide = self->slideMax;
                e.slideStep = 0;
            }
            const s32 offset[2] = {e.slide, 0};
            ScreenLayout_SetGroupOffset(layout, offset, i + 0x1E, -1);
            ScreenLayout_SetGroupOffset(layout, offset, i + 0x25, -1);
        }
    }
    return moved;
}

// 0x255EC0
static void SetupMenu_Draw(SetupMenu* self) {
    Font* font = *gFont;
    GsDevice* dev = *gGsDevice;
    GsDevice_End2D(dev);
    Font_SetColor(font, 0x80605F95u);
    for (int i = 0; i < 6; i++) {
        const SetupMenuEntry& e = self->entry[i];
        if (i == self->cursor)
            Font_SetColor(font, 0x80ACB2BEu);
        Font_DrawCentered(font, guest::ptr<const u16>(e.label), (int)Screen_X((e.x + e.slide) * 16, 0),
                          (int)Screen_Y(e.y * -16, 0), 0, 1.0f, 0.0f);
        if (i == self->cursor)
            Font_SetColor(font, 0x80605F95u);
    }
    GsDevice_Begin2D(dev);
}

// ══ Grid pickers ══════════════════════════════════════════════════════════
template <typename R, typename... Args>
static inline R PickerCall(GridPicker* p, GridPickerSlot slot, Args... args) {
    return VirtualCall<R, GridPicker*, Args...>(p->vtable, slot, p, args...);
}

// 0x255B80
static GridPicker* GridPicker_Ctor(GridPicker* self, SetupContext* context, int count, int columns,
                                   int rows, int panelGroup, int choiceGroup, int cellGroup,
                                   int lockGroup) {
    self->vtable = kVtGridPicker;
    self->count = count;
    self->columns = columns;
    self->rows = rows;
    self->panelGroup = panelGroup;
    self->choiceGroup = choiceGroup;
    self->cellGroup = cellGroup;
    self->lockGroup = lockGroup;
    self->cursor = {0, 0, 0, 0, 0xFFFFF0, 0, 0x80808080u};
    PadRepeatPair_Ctor(&self->upDown);
    PadRepeatPair_SetMasks(&self->upDown, kPadUp, kPadDown);
    PadRepeatPair_Ctor(&self->rightLeft);
    PadRepeatPair_SetMasks(&self->rightLeft, kPadRight, kPadLeft);
    self->upDown.vtable = 0x2EBA60u;
    self->rightLeft.vtable = 0x2EBA6Cu;
    self->context = guest::addr(context);
    self->chosen = 0;
    self->chosenColumn = 0;
    self->chosenRow = 0;
    self->column = 0;
    self->row = 0;
    self->disabled = 0;
    self->cursor.blend = 1;
    self->cursor.z = 0;
    self->cursor.rgba = 0x801010E8u;
    PadRepeatPair_SetTiming(&self->upDown, 13, 6, 7);
    PadRepeatPair_SetTiming(&self->rightLeft, 13, 6, 7);
    return self;
}

// 0x255AC0
static GridPicker* GridPicker_Dtor(GridPicker* self, s16 flag) {
    if (self) {
        self->vtable = kVtGridPicker;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// The "chosen" marker: the frame pieces (two groups before choice 0's) and
// the marker of one choice.
static void GridPicker_MarkChosen(GridPicker* self, int choice) {
    ScreenLayout* layout = LayoutOf(self->context);
    if (layout->ready) {
        ScreenLayout_HideGroup(layout, self->choiceGroup + self->chosen);
        ScreenLayout_ShowGroup(layout, self->choiceGroup - 2);
        ScreenLayout_ShowGroup(layout, self->choiceGroup - 1);
        ScreenLayout_ShowGroup(layout, self->choiceGroup + choice);
    }
    self->chosen = choice;
}

// 0x255980
static void GridPicker_Place(GridPicker* self) {
    ScreenLayout* layout = LayoutOf(self->context);
    if (!self->disabled) {
        for (int i = 0; i < self->count; i++)
            if (i != self->chosen)
                ScreenLayout_HideGroup(layout, self->choiceGroup + i);
        GridPicker_MarkChosen(self, self->chosen);
    } else {
        for (int i = -2; i < self->count; i++)
            ScreenLayout_HideGroup(layout, self->choiceGroup + i);
    }
    ScreenLayout_HideGroupB(layout, self->panelGroup);
}

// 0x255300
static void GridPicker_Disable(GridPicker* self) {
    self->disabled = 1;
    ScreenLayout* layout = LayoutOf(self->context);
    if (layout->ready)
        for (int i = -2; i < self->count; i++)
            ScreenLayout_HideGroup(layout, self->choiceGroup + i);
}

static void GridPicker_FrameCurrent(GridPicker* self) {
    int choice = PickerCall<int>(self, kPickerCurrent);
    s32 rect[4];
    ScreenLayout_GetGroupRect(rect, LayoutOf(self->context), self->cellGroup + choice, -1);
    self->cursor.x0 = rect[0];
    self->cursor.y0 = rect[1];
    self->cursor.x1 = rect[2];
    self->cursor.y1 = rect[3];
}

// 0x255880
static void GridPicker_Open(GridPicker* self) {
    ScreenLayout* layout = LayoutOf(self->context);
    PickerCall<void>(self, kPickerSetHint);
    ScreenLayout_ShowGroupB(layout, self->panelGroup);
    for (int i = 0; i < self->count; i++)
        if (PickerCall<bool>(self, kPickerIsAvailable, i))
            ScreenLayout_HideGroup(layout, self->lockGroup + i);
    self->column = self->chosenColumn;
    self->row = self->chosenRow;
    GridPicker_FrameCurrent(self);
}

// 0x2557B0
static void GridPicker_Close(GridPicker* self, bool accept) {
    ScreenLayout_HideGroupB(LayoutOf(self->context), self->panelGroup);
    if (accept) {
        GridPicker_MarkChosen(self, PickerCall<int>(self, kPickerCurrent));
        self->chosenColumn = self->column;
        self->chosenRow = self->row;
    }
}

static void WrapColumn(GridPicker* self) {
    if (self->column < 0)              self->column = self->columns - 1;
    if (self->column >= self->columns) self->column = 0;
}

// 0x255380 — moves the cursor, skipping the empty cells of a ragged grid
static bool GridPicker_MoveCursor(GridPicker* self) {
    PickerCall<int>(self, kPickerCurrent);
    int dx = 0, dy = 0;
    if (!PadRepeat_IsHeld(&self->rightLeft.a) && self->rightLeft.b.fired != 0) dx = -1;
    if (!PadRepeat_IsHeld(&self->rightLeft.b) && self->rightLeft.a.fired != 0) dx = 1;
    if (!PadRepeat_IsHeld(&self->upDown.b) && self->upDown.a.fired != 0)       dy = -1;
    if (!PadRepeat_IsHeld(&self->upDown.a) && self->upDown.b.fired != 0)       dy = 1;

    self->column += dx;
    self->row += dy;
    WrapColumn(self);
    if (self->row < 0)           self->row = self->rows - 1;
    if (self->row >= self->rows) self->row = 0;
    if (dx != 0 || dy != 0)
        Sound_PlaySE(kSeCursor, 0, 0x3FFF);

    if (dx != 0) {
        while ((PickerCall<int>(self, kPickerCurrent) & 0xFF00) != 0) {
            self->column += dx;
            WrapColumn(self);
        }
        return true;
    }
    if (dy != 0) {
        // An empty cell says which way its nearest real neighbour is.
        int marker = PickerCall<int>(self, kPickerCurrent) & 0xFF00;
        if (marker != 0) {
            if (marker == 0x100) self->column -= 1;
            if (marker == 0x200) self->column += 1;
            WrapColumn(self);
        }
        return true;
    }
    return false;
}

// 0x255670 — 1 = a choice was made, 2 = cancelled
static int GridPicker_Update(GridPicker* self) {
    u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);
    if (pressed & kPadCross) {
        int choice = PickerCall<int>(self, kPickerCurrent);
        if (PickerCall<bool>(self, kPickerIsAvailable, choice))
            return 1;
        Sound_PlaySE(kSeCancel, 0, 0x3FFF);
    }
    if (pressed & kPadTriangle) {
        Sound_PlaySE(kSeCancel, 0, 0x3FFF);
        return 2;
    }
    PadRepeat_Update(&self->upDown.a);
    PadRepeat_Update(&self->upDown.b);
    PadRepeat_Update(&self->rightLeft.a);
    PadRepeat_Update(&self->rightLeft.b);
    if (GridPicker_MoveCursor(self))
        GridPicker_FrameCurrent(self);
    return 0;
}

// Marks `choice` as the one in effect and puts the cursor on its cell.
static void GridPicker_Select(GridPicker* self, const s32* cells, int choice) {
    for (int row = 0; row < 2; row++) {
        bool found = false;
        for (int col = 0; col < self->columns; col++) {
            if (cells[row * self->columns + col] == choice) {
                self->chosenColumn = col;
                self->chosenRow = row;
                found = true;
                break;
            }
        }
        if (found)
            break;
    }
    GridPicker_MarkChosen(self, choice);
}

// ── HeadPicker ────────────────────────────────────────────────────────────
// 0x255220
static HeadPicker* HeadPicker_Ctor(HeadPicker* self, SetupContext* context) {
    GridPicker_Ctor(&self->base, context, 8, 5, 2, 4, 100, 140, 150);
    self->base.vtable = kVtHeadPicker;
    memcpy(self->choice, guest::ptr<const s32>(0x2DA5A0u), sizeof(self->choice));   // 0 1 2 3 4 / → 5 6 7 ←
    return self;
}

static void HeadPicker_SetHint(HeadPicker* self) {              // 0x2550E0
    SetupHint* hint = guest::ptr<SetupHint>(guest::ptr<SetupContext>(self->base.context)->hint);
    hint->text = guest::addr(TextTable_Find(TextTable_Get(), kTextHeadHint));
}
static bool HeadPicker_IsAvailable(HeadPicker* self, int choice) { return self->available[choice] != 0; }   // 0x256760
static HeadPicker* HeadPicker_Dtor(HeadPicker* self, s16 flag) {    // 0x255150
    return reinterpret_cast<HeadPicker*>(GridPicker_Dtor(&self->base, flag));
}
static int HeadPicker_Current(HeadPicker* self) {              // 0x24E7B0
    return self->choice[self->base.row][self->base.column];
}
// 0x254FF0
static void HeadPicker_Select(HeadPicker* self, int head) {
    GridPicker_Select(&self->base, &self->choice[0][0], head);
}
// 0x2552F0
static void HeadPicker_Draw(HeadPicker* self) {
    Gs_DrawFrameRect(&self->base.cursor, 2);
}

GUEST_BIND(0x2550E0, HeadPicker_SetHint);
GUEST_BIND(0x256760, HeadPicker_IsAvailable);
GUEST_BIND(0x255150, HeadPicker_Dtor);
GUEST_BIND(0x24E7B0, HeadPicker_Current);

// ── ClothesPicker ─────────────────────────────────────────────────────────
// 0x254F00
static ClothesPicker* ClothesPicker_Ctor(ClothesPicker* self, SetupContext* context) {
    GridPicker_Ctor(&self->base, context, 13, 7, 2, 5, 120, 180, 180);
    self->base.vtable = kVtClothesPicker;
    for (FrameRect& r : self->cover)
        r = {0, 0, 0, 0, 0xFFFFF0, 0, 0x80808080u};
    memcpy(self->choice, guest::ptr<const s32>(0x2DA5D0u), sizeof(self->choice));   // 0…6 / 7…12 ←
    return self;
}

static void ClothesPicker_SetHint(ClothesPicker* self) {        // 0x254D00
    SetupHint* hint = guest::ptr<SetupHint>(guest::ptr<SetupContext>(self->base.context)->hint);
    hint->text = guest::addr(TextTable_Find(TextTable_Get(), kTextClothesHint));
}
static bool ClothesPicker_IsAvailable(ClothesPicker* self, int choice) { return self->available[choice] != 0; }   // 0x2542F0
static ClothesPicker* ClothesPicker_Dtor(ClothesPicker* self, s16 flag) {    // 0x254E30
    return reinterpret_cast<ClothesPicker*>(GridPicker_Dtor(&self->base, flag));
}
static int ClothesPicker_Current(ClothesPicker* self) {        // 0x24E780
    return self->choice[self->base.row][self->base.column];
}
// 0x254C10
static void ClothesPicker_Select(ClothesPicker* self, int clothes) {
    GridPicker_Select(&self->base, &self->choice[0][0], clothes);
}
// 0x254D70
static void ClothesPicker_Place(ClothesPicker* self) {
    GridPicker_Place(&self->base);
    ScreenLayout* layout = LayoutOf(self->base.context);
    for (int i = 0; i < 13; i++) {
        s32 rect[4];
        ScreenLayout_GetGroupRect(rect, layout, 0xB4 + i, -1);
        self->cover[i].x0 = rect[0];
        self->cover[i].y0 = rect[1];
        self->cover[i].x1 = rect[2];
        self->cover[i].y1 = rect[3];
        self->cover[i].blend = 1;
        self->cover[i].rgba = 0x40000000u;
    }
}
// 0x254B70 — clothes for the other gender are greyed out
static void ClothesPicker_Draw(ClothesPicker* self) {
    Gs_DrawFrameRect(&self->base.cursor, 2);
    u8 gender = guest::ptr<SetupContext>(self->base.context)->gender;
    for (int i = 0; i < 13; i++)
        if (PickerCall<bool>(&self->base, kPickerIsAvailable, i) && kCoverGender[i] != (s8)gender)
            Gs_FillRect2(&self->cover[i]);
}

GUEST_BIND(0x254D00, ClothesPicker_SetHint);
GUEST_BIND(0x2542F0, ClothesPicker_IsAvailable);
GUEST_BIND(0x254E30, ClothesPicker_Dtor);
GUEST_BIND(0x24E780, ClothesPicker_Current);

// ══ Swords and items ══════════════════════════════════════════════════════
// 0x2548A0
static void SetupSwords_PlaceSlot(SetupSwords* self, int slot, int nameGroup, int labelGroup, int countGroup) {
    ScreenLayout* layout = LayoutOf(self->context);
    s32 first[4], last[4];
    ScreenLayout_GetGroupRect(first, layout, nameGroup, -1);
    ScreenLayout_GetGroupRect(last, layout, countGroup, -1);
    self->rect[slot][0] = first[0] - 1;
    self->rect[slot][1] = first[1] + 1;
    self->rect[slot][2] = last[2] + 1;
    self->rect[slot][3] = last[3] - 1;
    ScreenLayout_GetGroupOrigin(self->namePos[slot], layout, nameGroup, -1);
    ScreenLayout_GetGroupCenter(self->labelPos[slot], layout, labelGroup, -1);
    ScreenLayout_GetGroupCenter(self->countPos[slot], layout, countGroup, -1);
}

// 0x254A40
static void SetupSwords_Place(SetupSwords* self) {
    SetupSwords_PlaceSlot(self, 0, 0x32, 0x33, 0x34);
    SetupSwords_PlaceSlot(self, 1, 0x35, 0x36, 0x37);
}

// 0x254570
static void SetupSwords_DrawSlot(SetupSwords* self, int slot, int sword) {
    Font* font = *gFont;
    u8* pd = *gPlayerData;
    int x = (int)(Screen_X(0, 0) & 0xFFFF);
    int y = (int)(Screen_Y(0, 0) & 0xFFFF);

    Font_SetColor(font, 0x80ACB2BEu);
    Font_DrawCentered(font, TextTable_Find(TextTable_Get(), kTextSwordLabel),
                      x + self->labelPos[slot][0] * 16, y + self->labelPos[slot][1] * -16, 0, 1.0f, 0.0f);
    Font_DrawString(font, SwordAt(pd, sword)->name,
                    x + self->namePos[slot][0] * 16 + 0x40, y + self->namePos[slot][1] * -16 + 0x20,
                    0, 1.0f, 0.0f);

    Font_SetColor(font, 0x8064443Au);
    u16 number[16];
    WStr_Format(number, 16, "%d", SwordAt(pd, sword)->durability);
    Font_DrawCentered(font, number, x + self->countPos[slot][0] * 16 - 0x20,
                      y + self->countPos[slot][1] * -16, 0, 1.0f, 0.0f);
}

// 0x2547F0 — the first two swords carried
static void SetupSwords_Draw(SetupSwords* self) {
    GsDevice* dev = *gGsDevice;
    const s16* slots = SwordSlots(*gPlayerData);
    GsDevice_End2D(dev);
    int shown = 0;
    for (int i = 0; i < 3; i++) {
        if (slots[i] != -1)
            SetupSwords_DrawSlot(self, shown++, i);
        if (shown > 1)
            break;
    }
    GsDevice_Begin2D(dev);
}

// 0x254460
static void SetupItems_Place(SetupItems* self) {
    ScreenLayout* layout = LayoutOf(self->context);
    for (int i = 0; i < 8; i++)
        ScreenLayout_GetGroupRect(self->rect[i], layout, 0x3C + i, -1);
}

// 0x254300
static void SetupItems_Draw(SetupItems* self) {
    IconSet* icons = guest::ptr<IconSet>(guest::ptr<SetupContext>(self->context)->iconSet);
    const u8* items = ItemSlots(*gPlayerData);
    int x = (int)(Screen_X(0, 0) & 0xFFFF);
    int y = (int)(Screen_Y(0, 0) & 0xFFFF);
    for (int i = 0; i < 8; i++) {
        if (items[i] == 0)
            continue;
        ItemInfo* icon = IconSet_Get(icons, items[i]);
        if (!icon)
            continue;
        const s32* r = self->rect[i];
        struct { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; } params =
            {1, x + r[0] * 16, y + r[1] * -16, 0, (r[2] - r[0]) * 16, (r[1] - r[3]) * 16, 0x80808080u};
        Icon_Draw(icon, &params);
    }
}

// ══ The screen ════════════════════════════════════════════════════════════

static inline CharaMgr* Mgr(CharaSetup* self) { return guest::ptr<CharaMgr>(self->charaMgr); }

// Stores what is on screen as the player's character.
static void SaveChoice(CharaSetup* self) {
    u8* pd = *gPlayerData;
    SavedChara(pd) = (s16)self->charaId;
    SavedHead(pd) = (s16)self->head;
    SavedSecret(pd) = 0;
    if (self->secret)
        SavedSecret(pd) = (s16)(self->secretIndex + 1);
}

static int SecretCount() {
    int n = 0;
    while (kSecretCharas[n].chara != -1)
        n++;
    return n;
}

// ── 0x24ECF0 ──────────────────────────────────────────────────────────────
CharaSetup* CharaSetup_Ctor(CharaSetup* self) {
    u8* pd = *gPlayerData;
    MemSet(&self->charaMgr, 0, 4);
    self->hint = {0, 0, 0};
    SetupMenu_Ctor(&self->menu, &self->context);
    HeadPicker_Ctor(&self->heads, &self->context);
    ClothesPicker_Ctor(&self->clothes, &self->context);
    self->swords.context = guest::addr(&self->context);
    memset(self->swords.rect, 0, 0x50);
    self->items.context = guest::addr(&self->context);
    memset(self->items.rect, 0, 0x80);

    self->phase = 0;
    self->transition = kTransNone;
    self->sub = kSubMenu;
    self->charaId = SavedChara(pd);
    self->head = SavedHead(pd);
    self->needSword = 0;
    self->secret = 0;
    self->nameInput = 0;
    self->swordSelect = 0;
    self->itemSelect = 0;
    self->swordInfo = 0;
    self->codeTimer = 0;
    self->codeStep = 0;
    self->secretOpen = SavedSecret(pd) != 0;
    self->secretIndex = SavedSecret(pd) - 1;
    if (self->secretIndex < 0 || self->secretIndex >= SecretCount())
        self->secretIndex = 0;
    MemSet(&self->context, 0, 0x10);
    self->context.hint = guest::addr(&self->hint);
    self->context.iconSet = 0;
    return self;
}

// ── 0x24CF10 ──────────────────────────────────────────────────────────────
// Asset 0x21: background colour, light colours, light directions.
static void CharaSetup_LoadLighting() {
    ArchiveFile* file = ArchiveFile_Create();
    ArchiveFile_Open(file, kAssetSetupLighting);
    u8* data = static_cast<u8*>(ArchiveFile_ReadAll(file, 0));
    ArchiveFile_Close(file);

    u32 background = 0;
    alignas(16) u8 colors[0x40], directions[0x40];
    memcpy(&background, data, 4);
    memcpy(colors, data + 4, 0x40);
    memcpy(directions, data + 0x44, 0x40);

    GsDevice_BeginFrame(*gGsDevice);
    gClearColor = background;
    Light_SetMatrices(*gLight, directions, colors);
    Light_Upload(*gLight, *gGsDevice);
    GsDevice_EndFrame(*gGsDevice);
    sceGsSyncPath(0, 0);
    GsDevice_SwapBuffers(*gGsDevice, 0);
    MemFree(data);
    ArchiveFile_Destroy(file);
}

// ── 0x24D8A0 ──────────────────────────────────────────────────────────────
// Shows the character for the current head and clothes (or the current
// secret character).  With fromSave, starts from the saved character instead.
static void CharaSetup_ApplySelection(CharaSetup* self, bool fromSave) {
    u8* pd = *gPlayerData;
    if (!self->charaMgr)
        return;

    int chara = 0;
    if (!fromSave) {
        if (!CharaMgr_IsLoaded(Mgr(self), 0)) {
            self->transition = kTransApply;       // try again once the load finishes
            return;
        }
        if (!self->secretOpen) {
            chara = kCharaTable[self->heads.base.chosen][self->clothes.base.chosen];
            if (self->charaId == chara)
                return;
            self->secret = 0;
        } else {
            GridPicker_Disable(&self->heads.base);
            GridPicker_Disable(&self->clothes.base);
            self->secret = 1;
        }
        CharaMgr_Remove(Mgr(self), 0);
    } else {
        bool found = false;
        for (int head = 0; head < 8 && !found; head++) {
            for (int clothes = 0; clothes < 13; clothes++) {
                if (kCharaTable[head][clothes] == SavedChara(pd)) {
                    HeadPicker_Select(&self->heads, head);
                    ClothesPicker_Select(&self->clothes, clothes);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            GridPicker_Disable(&self->heads.base);
            GridPicker_Disable(&self->clothes.base);
            self->secret = 1;
        }
        chara = SavedChara(pd);
    }

    self->context.gender = (u8)kHeadGender[self->heads.base.chosen];
    if (!self->secretOpen) {
        self->charaId = chara;
        self->head = self->heads.base.chosen;
    } else {
        self->charaId = kSecretCharas[self->secretIndex].chara;
        self->head = kSecretCharas[self->secretIndex].head;
    }

    CharaMgr_Add(Mgr(self), 0, 0, self->charaId, -1);
    CharaMgr_QueueLoad(Mgr(self), 0);

    // Stand it facing the camera, turned ten degrees.
    alignas(16) float pose[16], turn[16];
    Matrix_Copy(pose, CharaMgr_GetTransform(Mgr(self), 0));
    Matrix_Identity(turn);
    Matrix_Rotate(turn, turn, 0.17453292f);
    Matrix_Multiply(pose, pose, turn);
    Matrix_Identity(turn);
    Matrix_Rotate(turn, turn, 3.1415927f);
    Matrix_Multiply(pose, pose, turn);
    CharaMgr_SetTransform(Mgr(self), 0, pose);
    CharaMgr_SetPosition(Mgr(self), 0, guest::ptr(0x2DA560u));
}

// ── 0x24E7E0 ──────────────────────────────────────────────────────────────
void CharaSetup_Attach(CharaSetup* self, u32* charaMgr) {
    u8* pd = *gPlayerData;
    SwordDb_Get();
    self->charaMgr = *charaMgr;
    CharaSetup_LoadLighting();

    void* mem = MemAlloc(sizeof(ScreenLayout));
    self->context.layout = mem ? guest::addr(ScreenLayout_Ctor(static_cast<ScreenLayout*>(mem))) : 0;
    if (self->context.layout) {
        ScreenLayout_Init(guest::ptr<ScreenLayout>(self->context.layout));
        guest::ptr<ScreenLayout>(self->context.layout)->id = kLayoutCharacterSetup;
    }
    self->phase = 1;

    // Heads and clothes unlock with the player's total score.
    for (int i = 0; i < 8; i++)
        self->heads.available[i] = kHeadScore[i] <= Score(pd);
    for (int i = 0; i < 13; i++)
        self->clothes.available[i] = kClothesScore[i] <= Score(pd);

    mem = MemAlloc(0x20);
    self->context.iconSet = mem ? guest::addr(IconSet_Ctor(static_cast<IconSet*>(mem))) : 0;
    IconSet_Load(guest::ptr<IconSet>(self->context.iconSet));
    CharaSetup_ApplySelection(self, true);

    // A new character starts with the default belongings.
    if (SwordCount(pd) == 0 && *reinterpret_cast<s16*>(pd + 0x6C80) == 0) {
        Sword sword;
        Sword_Ctor(&sword);
        SwordDb_MakeSword(SwordDb_Get(), 0, &sword);
        PlayerData_CarrySword(reinterpret_cast<PlayerData*>(pd), &sword);
        Sword_Dtor(&sword, -1);
    }
    // More than two swords: one that is not readied goes into storage.
    if (*reinterpret_cast<s16*>(pd + 0x6C80) < *reinterpret_cast<s16*>(pd + 0x6C84) && SwordCount(pd) > 2) {
        int readied = PlayerData_GetReadiedSword(PD(pd));
        for (int i = 0; i < 3; i++) {
            if (i != readied && SwordSlots(pd)[i] != -1) {
                PlayerData_StoreSword(PD(pd), SwordAt(pd, i));
                PlayerData_RemoveSword(PD(pd), i);
                break;
            }
        }
    }
    // Items carried go back into stock.
    for (int i = 0; i < 8; i++) {
        s8 item = (s8)ItemSlots(pd)[i];
        if (item != 0 && PlayerData_GetItemCount(PD(pd), item) < 99) {
            PlayerData_SetItemCount(PD(pd), item, (u8)(PlayerData_GetItemCount(PD(pd), item) + 1));
            PlayerData_RemoveItem(PD(pd), 0, (u32)i);
        }
    }
}

// ── 0x24D280 ──────────────────────────────────────────────────────────────
// Screen is black: drop the old sub-screen and create the next.
static void CharaSetup_EnterSub(CharaSetup* self) {
    u8* pd = *gPlayerData;
    switch (self->sub) {
    case kSubSwordDetail:
        if (self->swordInfo) SwordInfo_Dtor(guest::ptr(self->swordInfo), 1);
        self->swordInfo = 0;
        break;
    case kSubItems:
        if (self->itemSelect) ItemSelect_Dtor(guest::ptr<ItemSelect>(self->itemSelect), 1);
        self->itemSelect = 0;
        break;
    case kSubSwords:
        if (self->nextSub != kSubSwordDetail) {     // kept while a sword's details are shown
            if (self->swordSelect) SwordSelect_Dtor(guest::ptr(self->swordSelect), 1);
            self->swordSelect = 0;
        }
        break;
    case kSubNameInput:
        if (self->nameInput) NameInput_Dtor(guest::ptr<NameInput>(self->nameInput), 1);
        self->nameInput = 0;
        break;
    }

    self->sub = self->nextSub;
    void* mem;
    switch (self->sub) {
    case kSubSwordDetail:
        mem = MemAlloc(0x14);
        self->swordInfo = mem ? guest::addr(SwordInfo_Ctor(mem)) : 0;
        if (!self->swordInfo) {
            self->sub = kSubMenu;
        } else if (!SwordInfo_Start(guest::ptr(self->swordInfo), self->swordDetail)) {
            SwordInfo_Dtor(guest::ptr(self->swordInfo), 1);
            self->swordInfo = 0;
            self->sub = kSubMenu;
        }
        break;
    case kSubItems:
        mem = MemAlloc(0x16C);
        self->itemSelect = mem ? guest::addr(ItemSelect_Ctor(static_cast<ItemSelect*>(mem))) : 0;
        if (!self->itemSelect) {
            self->sub = kSubMenu;
        } else if (!ItemSelect_Start(guest::ptr<ItemSelect>(self->itemSelect))) {
            ItemSelect_Dtor(guest::ptr<ItemSelect>(self->itemSelect), 1);
            self->itemSelect = 0;
            self->sub = kSubMenu;
        }
        break;
    case kSubSwords:
        if (!self->swordSelect) {
            mem = MemAlloc(0x464);
            self->swordSelect = mem ? guest::addr(SwordSelect_Ctor(mem)) : 0;
            if (!self->swordSelect) {
                self->sub = kSubMenu;
            } else if (!SwordSelect_Start(guest::ptr(self->swordSelect), 2)) {
                SwordSelect_Dtor(guest::ptr(self->swordSelect), 1);
                self->swordSelect = 0;
                self->sub = kSubMenu;
            }
        }
        break;
    case kSubNameInput:
        mem = MemAlloc(0x9B8);
        self->nameInput = mem ? guest::addr(NameInput_Ctor(static_cast<NameInput*>(mem))) : 0;
        if (!self->nameInput)
            self->sub = kSubMenu;
        else
            NameInput_Setup(guest::ptr<NameInput>(self->nameInput), PlayerName(pd), 2);
        break;
    default:
        break;
    }
    self->transition = kTransLoading;
    ShowLoading(true);
}

static void BeginFadeIn(CharaSetup* self) {
    self->transition = kTransFadeIn;
    self->countdown = TransitionFrames();
    RenderMgr_TriggerEffect(*gRenderMgr, 11, 0, 0);
    ShowLoading(false);
}

// ── 0x24D580 ──────────────────────────────────────────────────────────────
static void CharaSetup_UpdateTransition(CharaSetup* self) {
    switch (self->transition) {
    case kTransFadeOut:
        if (--self->countdown == 0)
            CharaSetup_EnterSub(self);
        break;
    case kTransFadeIn:
        if (--self->countdown == 0)
            self->transition = kTransNone;
        break;
    case kTransLoading:
        switch (self->sub) {
        case kSubSwordDetail:
            if (self->swordInfo) {
                SwordInfo_Update(guest::ptr(self->swordInfo));
                if (guest::ptr<u8>(self->swordInfo)[0xD] != 0)
                    BeginFadeIn(self);
            }
            break;
        case kSubItems:
            ItemSelect_Update(guest::ptr<ItemSelect>(self->itemSelect));
            if (*guest::ptr<s32>(self->itemSelect) == 4)
                BeginFadeIn(self);
            break;
        case kSubSwords:
            if (self->swordSelect) {
                SwordSelect_Load(guest::ptr(self->swordSelect));
                if (*guest::ptr<s32>(self->swordSelect) == 5)
                    BeginFadeIn(self);
            }
            break;
        case kSubNameInput:
            if (self->nameInput) {
                NameInput_Update(guest::ptr<NameInput>(self->nameInput));
                if (*guest::ptr<s32>(self->nameInput) == 4)
                    BeginFadeIn(self);
            }
            break;
        default:
            BeginFadeIn(self);
            break;
        }
        break;
    default:
        break;
    }
}

// ── 0x24DB70 ──────────────────────────────────────────────────────────────
// Left / Right turn the character.
static void CharaSetup_UpdateTurn(CharaSetup* self) {
    if (self->sub != kSubClothes && self->sub != kSubHead && self->sub != kSubMenu)
        return;
    u16 held = Pad_GetNewPress(PadMgr_Get(), 0);
    int degrees;
    if (held & kPadRight)
        degrees = 120 / gFrameRate;
    else if (held & kPadLeft)
        degrees = -(120 / gFrameRate);
    else
        return;

    alignas(16) float pose[16], turn[16];
    Matrix_Copy(pose, CharaMgr_GetTransform(Mgr(self), 0));
    Matrix_Identity(turn);
    Matrix_Rotate(turn, turn, (float)degrees * 3.1415927f / 180.0f);
    Matrix_Multiply(pose, pose, turn);
    CharaMgr_SetTransform(Mgr(self), 0, pose);
}

// ── 0x24CCE0 ──────────────────────────────────────────────────────────────
// On "Name": watches for the secret button sequence, and once it has been
// entered lets Left / Right step through the secret characters.
static void CharaSetup_UpdateSecret(CharaSetup* self) {
    if (self->codeTimer++ >= gFrameRate >> 2)
        self->codeStep = 0;                     // too slow: start over
    void* pad = PadMgr_Get();
    Pad_GetHeld(pad, 0);
    u16 pressed = Pad_GetPressed(pad, 0);
    if ((kSecretCode[self->codeStep] & pressed) == 0) {
        if (Pad_GetPressed(pad, 0) != 0) {
            self->codeTimer = 0;
            self->codeStep = 0;
        }
    } else {
        self->codeTimer = 0;
        self->codeStep++;
        if (kSecretCode[self->codeStep] == 0xFFFFFFFFu) {
            self->secretOpen = 1;
            Sound_PlaySE(kSeSecret, 0, 0x3FFF);
            self->codeTimer = 0;
            self->codeStep = 0;
        }
    }

    if (!self->secretOpen)
        return;
    const int count = SecretCount();
    u16 buttons = (u16)(Pad_GetPressed(pad, 0) | Pad_GetRepeat(pad, 0));
    if (buttons & kPadRight) {
        if (CharaMgr_IsLoaded(Mgr(self), 0)) {
            if (++self->secretIndex > count - 1)
                self->secretIndex = 0;
            CharaSetup_ApplySelection(self, false);
            Sound_PlaySE(kSeChange, 0, 0x3FFF);
        }
    } else if (buttons & kPadLeft) {
        if (CharaMgr_IsLoaded(Mgr(self), 0)) {
            if (--self->secretIndex < 0)
                self->secretIndex = count - 1;
            CharaSetup_ApplySelection(self, false);
            Sound_PlaySE(kSeChange, 0, 0x3FFF);
        }
    }
}

static void FadeOutTo(CharaSetup* self, int sub) {
    self->transition = kTransFadeOut;
    self->countdown = TransitionFrames();
    self->nextSub = sub;
    RenderMgr_TriggerEffect(*gRenderMgr, 11, 1, 0);
}

// ── 0x24DCD0 ──────────────────────────────────────────────────────────────
static int CharaSetup_UpdateMain(CharaSetup* self) {
    u8* pd = *gPlayerData;
    u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);
    if (SetupMenu_Update(&self->menu))
        self->needSword = 0;
    if (self->needSword)
        SetupHint_SetText(&self->hint, kTextNeedSword);
    else
        SetupHint_SetText(&self->hint, self->menu.cursor == kSetupDone ? kTextDoneHint : kTextChoose);

    if (pressed & kPadTriangle) {
        SaveChoice(self);
        if (!CharaMgr_IsLoaded(Mgr(self), 0)) {
            self->transition = kTransLeave;
            self->result = kSetupBack;
            return kSetupRunning;
        }
        Sound_PlaySE(kSeCancel, 0, 0x3FFF);
        return kSetupBack;
    }
    if (!CharaMgr_IsLoaded(Mgr(self), 0))
        return kSetupRunning;
    if (!(pressed & kPadCross)) {
        if (self->menu.cursor == kSetupName)
            CharaSetup_UpdateSecret(self);
        return kSetupRunning;
    }

    switch (self->menu.cursor) {
    case kSetupName:
        Sound_PlaySE(kSeConfirm, 0, 0x3FFF);
        FadeOutTo(self, kSubNameInput);
        break;
    case kSetupHead:
        self->secretOpen = 0;
        Sound_PlaySE(kSeConfirm, 0, 0x3FFF);
        GridPicker_Open(&self->heads.base);
        self->sub = kSubHead;
        break;
    case kSetupClothes:
        if (!self->secret) {
            self->secretOpen = 0;
            Sound_PlaySE(kSeConfirm, 0, 0x3FFF);
            GridPicker_Open(&self->clothes.base);
            self->sub = kSubClothes;
        } else {
            Sound_PlaySE(kSeCancel, 0, 0x3FFF);
        }
        break;
    case kSetupSword:
        Sound_PlaySE(kSeConfirm, 0, 0x3FFF);
        FadeOutTo(self, kSubSwords);
        break;
    case kSetupItems:
        Sound_PlaySE(kSeConfirm, 0, 0x3FFF);
        FadeOutTo(self, kSubItems);
        break;
    case kSetupDone:
        if (SwordCount(pd) < 1) {
            self->needSword = 1;
            Sound_PlaySE(kSeCancel, 0, 0x3FFF);
        } else {
            Sound_PlaySE(kSeDecide, 0, 0x3FFF);
            SaveChoice(self);
            if (CharaMgr_IsPlayerReady(Mgr(self)))
                return kSetupStartGame;
            self->transition = kTransLeave;
            self->result = kSetupStartGame;
        }
        break;
    }
    return kSetupRunning;
}

// ── 0x24E0E0 ──────────────────────────────────────────────────────────────
int CharaSetup_Update(CharaSetup* self) {
    u8* pd = *gPlayerData;
    if (!self->context.layout)
        return kSetupBack;

    if (self->phase != 3) {
        if (self->phase == 2) {
            if (!IconSet_IsLoading(guest::ptr<IconSet>(self->context.iconSet)))
                self->phase = 3;
        } else if (self->phase == 1 && ScreenLayout_Poll(guest::ptr<ScreenLayout>(self->context.layout))) {
            ScreenLayout* layout = guest::ptr<ScreenLayout>(self->context.layout);
            SetupHint_Place(&self->hint, layout);
            SetupMenu_Place(&self->menu);
            GridPicker_Place(&self->heads.base);
            ClothesPicker_Place(&self->clothes);
            SetupSwords_Place(&self->swords);
            SetupItems_Place(&self->items);
            ScreenLayout_HideGroupB(layout, 0);
            ScreenLayout_SetDepth(layout, 0);
            self->phase = 2;
        }
        return kSetupRunning;
    }

    if (self->transition == kTransApply || self->transition == kTransLeave) {
        CharaMgr_Update(Mgr(self), 1);
        if (!CharaMgr_IsLoaded(Mgr(self), 0))
            return kSetupRunning;
        if (self->transition != kTransApply)
            return self->result;
        self->transition = kTransNone;
        CharaSetup_ApplySelection(self, false);
    }
    if (self->transition != kTransNone) {
        CharaSetup_UpdateTransition(self);
        if (self->transition != kTransFadeIn)
            return kSetupRunning;
    }
    if (self->charaMgr)
        CharaMgr_Update(Mgr(self), 1);
    CharaSetup_UpdateTurn(self);
    u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);

    switch (self->sub) {
    case kSubMenu:
        return CharaSetup_UpdateMain(self);

    case kSubNameInput:
        if (self->nameInput && NameInput_Update(guest::ptr<NameInput>(self->nameInput))) {
            // The typed name is at +0x8C0, its length at +0x8E2.
            u16* typed = guest::ptr<u16>(self->nameInput + 0x8C0);
            typed[*guest::ptr<s16>(self->nameInput + 0x8E2)] = 0;
            if (WStr_Length(typed) == 0)
                PlayerData_SetDefaultName(PD(pd));
            else if (WStr_Length(typed) < 16)
                WStr_Copy(PlayerName(pd), typed);
            FadeOutTo(self, kSubMenu);
        }
        break;

    case kSubHead: {
        int r = GridPicker_Update(&self->heads.base);
        if (r == 2) {
            GridPicker_Close(&self->heads.base, false);
            if (!self->secret)
                CharaSetup_ApplySelection(self, false);
            self->sub = kSubMenu;
        } else if (r == 1) {
            // A head of the other gender (or leaving a secret character)
            // also switches to that head's default clothes.
            int head = PickerCall<int>(&self->heads.base, kPickerCurrent);
            if ((s8)self->context.gender != kHeadGender[head] || self->secret)
                ClothesPicker_Select(&self->clothes, kDefaultClothes[head]);
            Sound_PlaySE(kSeConfirm, 0, 0x3FFF);
            GridPicker_Close(&self->heads.base, true);
            CharaSetup_ApplySelection(self, false);
            self->sub = kSubMenu;
        }
        break;
    }
    case kSubClothes: {
        int r = GridPicker_Update(&self->clothes.base);
        if (r == 2) {
            GridPicker_Close(&self->clothes.base, false);
            CharaSetup_ApplySelection(self, false);
            self->sub = kSubMenu;
        } else if (r == 1) {
            int clothes = PickerCall<int>(&self->clothes.base, kPickerCurrent);
            if ((s8)self->context.gender == kClothesGender[clothes]) {
                Sound_PlaySE(kSeConfirm, 0, 0x3FFF);
                GridPicker_Close(&self->clothes.base, true);
                CharaSetup_ApplySelection(self, false);
                self->sub = kSubMenu;
            } else {
                Sound_PlaySE(kSeCancel, 0, 0x3FFF);
            }
        }
        break;
    }
    case kSubSwords: {
        s32 result[2];
        SwordSelect_Update(result, guest::ptr(self->swordSelect));
        if (result[0] == 2) {
            self->swordDetail = result[1];
            FadeOutTo(self, kSubSwordDetail);
        } else if (result[0] == 1) {
            FadeOutTo(self, kSubMenu);
        }
        break;
    }
    case kSubItems:
        if (self->itemSelect && ItemSelect_Update(guest::ptr<ItemSelect>(self->itemSelect)))
            FadeOutTo(self, kSubMenu);
        break;
    case kSubSwordDetail:
        if (self->swordInfo) {
            SwordInfo_Update(guest::ptr(self->swordInfo));
            if (pressed & kPadTriangle)
                FadeOutTo(self, kSubSwords);
        }
        break;
    default:
        return kSetupBack;
    }
    return kSetupRunning;
}

// ── 0x24D0B0 ──────────────────────────────────────────────────────────────
void CharaSetup_Draw(CharaSetup* self) {
    u8* pd = *gPlayerData;
    GsDevice* dev = *gGsDevice;
    if (!self->context.layout || self->phase != 3)
        return;

    GsDevice_Begin2D(dev);
    switch (self->sub) {
    case kSubSwordDetail: SwordInfo_Draw(guest::ptr(self->swordInfo));     return;
    case kSubItems:       ItemSelect_Draw(guest::ptr<ItemSelect>(self->itemSelect));   return;
    case kSubSwords:      SwordSelect_Draw(guest::ptr(self->swordSelect)); return;
    case kSubNameInput:   NameInput_Draw(guest::ptr<NameInput>(self->nameInput));     return;
    default: break;
    }

    ScreenLayout* layout = guest::ptr<ScreenLayout>(self->context.layout);
    ScreenLayout_Draw(layout);
    SetupMenu_Draw(&self->menu);
    SetupSwords_Draw(&self->swords);
    SetupItems_Draw(&self->items);
    SetupHint_Draw(&self->hint);
    if (self->sub == kSubClothes)
        ClothesPicker_Draw(&self->clothes);
    else if (self->sub == kSubHead)
        HeadPicker_Draw(&self->heads);
    if (self->charaMgr)
        CharaMgr_Draw(Mgr(self), 0);
    GsDevice_End2D(dev);

    // The player's name, on its plate.
    s32 pos[2];
    ScreenLayout_GetGroupCenter(pos, layout, 0x50, -1);
    Font_SetColor(*gFont, 0x80C0C0C0u);
    Font_DrawCentered(*gFont, PlayerName(pd), (int)(Screen_X(pos[0] << 4, 0) & 0xFFFF) + 0x40,
                      (int)Screen_X(pos[1] * -16, 0), 0, 0.82f, 0.0f);
}

// ── 0x24EBB0 ──────────────────────────────────────────────────────────────
CharaSetup* CharaSetup_Dtor(CharaSetup* self, s16 flag) {
    if (!self)
        return self;
    if (self->charaMgr)
        CharaMgr_Remove(Mgr(self), 0);
    if (self->context.layout)
        ScreenLayout_Dtor(guest::ptr<ScreenLayout>(self->context.layout), 1);
    self->context.layout = 0;
    if (self->context.iconSet)
        IconSet_Dtor(guest::ptr<IconSet>(self->context.iconSet), 1);
    self->context.iconSet = 0;
    if (self->nameInput)   NameInput_Dtor(guest::ptr<NameInput>(self->nameInput), 1);
    self->nameInput = 0;
    if (self->swordSelect) SwordSelect_Dtor(guest::ptr(self->swordSelect), 1);
    self->swordSelect = 0;
    if (self->itemSelect)  ItemSelect_Dtor(guest::ptr<ItemSelect>(self->itemSelect), 1);
    self->itemSelect = 0;
    if (self->swordInfo)   SwordInfo_Dtor(guest::ptr(self->swordInfo), 1);
    self->swordInfo = 0;
    ClothesPicker_Dtor(&self->clothes, -1);
    HeadPicker_Dtor(&self->heads, -1);
    if (flag > 0)
        MemFree(self);
    return self;
}
