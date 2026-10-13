// Pop-up windows — decompiled from 0x13D0C0–0x13F330 and 0x13F8A0–0x13FA80.
// See include/engine/window.h.

#include "engine/window.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/pad.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void  Sound_PlaySE(u32 id, u32 positional, u32 volume);     // 0x180E70
}

static_assert(sizeof(Window) == 0xB4 && offsetof(Window, frame) == 0x70, "Window");
static_assert(sizeof(WindowDesc) == 0x38 && sizeof(WindowMgr) == 0x58, "WindowMgr");

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
// How far the far corners of the two gradients are shaded.
static float& gFrameShade = *guest::ptr<float>(0x2EDA1Cu);
static float& gPanelShade = *guest::ptr<float>(0x2EDA20u);

struct OptionNode {
    u32 prev, next;
    TextLabel label;
};
static inline OptionNode* OptionsEnd(Window* w) { return reinterpret_cast<OptionNode*>(&w->options.endPrev); }
static inline OptionNode* OptionsBegin(Window* w) { return guest::ptr<OptionNode>(w->options.endNext); }
static inline OptionNode* Next(OptionNode* n) { return guest::ptr<OptionNode>(n->next); }

// ══ Shapes ════════════════════════════════════════════════════════════════
struct ShapePacket {
    u64 prim;
    struct { u32 rgba; float q; u16 x, y; u32 z; } v[4];
};

// ── 0x13FA80 ──────────────────────────────────────────────────────────────
void Gs_FillGradient(const s32* r) {
    GsDevice* dev = *gGsDevice;
    if (!dev)
        return;
    ShapePacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.prim = ((u64)(u32)r[5] & 0x3FFFFFF) << 6 | 0xD;      // triangle fan, shaded
    const s32 corner[4][2] = {{r[0], r[1]}, {r[2], r[1]}, {r[2], r[3]}, {r[0], r[3]}};
    for (int i = 0; i < 4; i++) {
        packet.v[i].rgba = (u32)r[6 + i];
        packet.v[i].q = 1.0f;
        packet.v[i].x = (u16)Screen_XAligned(corner[i][0] << 4);
        packet.v[i].y = (u16)Screen_XAligned(corner[i][1] * -16);   // the x conversion, as in the original
        packet.v[i].z = (u32)r[4];
    }
    GsDevice_DrawPrim(dev, &packet, 4);
}

// ── 0x13F8A0 ──────────────────────────────────────────────────────────────
void AnimRect_Fill(const AnimRect* r) {
    GsDevice* dev = *gGsDevice;
    if (!dev)
        return;
    ShapePacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.prim = ((u64)r->blend & 0x3FFFFFF) << 6 | 5;
    const s32 x0 = r->x + r->rect[0], y0 = r->y + r->rect[1];
    const s32 x1 = r->x + r->rect[2], y1 = r->y + r->rect[3];
    const s32 corner[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; i++) {
        packet.v[i].rgba = r->rgba;
        packet.v[i].q = 1.0f;
        packet.v[i].x = (u16)Screen_XAligned(corner[i][0] * 16);
        packet.v[i].y = (u16)Screen_XAligned(corner[i][1] * -16);
        packet.v[i].z = r->z;
    }
    GsDevice_DrawPrim(dev, &packet, 4);
}

// ── 0x13F330 ──────────────────────────────────────────────────────────────
void AnimRect_SetTarget(AnimRect* self, const s32* target, int duration) {
    self->duration = duration;
    self->frame = 0;
    memcpy(self->target, target, sizeof(self->target));
}

// ── 0x13EF20 ──────────────────────────────────────────────────────────────
// Covers an equal share of what is left each frame.
int AnimRect_Step(AnimRect* self) {
    if (self->duration < ++self->frame)
        return 0;
    if (self->frame < self->duration) {
        const float share = 1.0f / (float)(self->duration - self->frame);
        self->x += (int)((float)(self->target[0] - self->x) * share);
        self->y += (int)((float)(self->target[1] - self->y) * share);
        for (int i = 0; i < 4; i++)
            self->rect[i] += (int)((float)(self->target[2 + i] - self->rect[i]) * share);
        return 1;
    }
    self->x = self->target[0];
    self->y = self->target[1];
    memcpy(self->rect, &self->target[2], sizeof(self->rect));
    return 0;
}

// ══ Window ════════════════════════════════════════════════════════════════
// 0x225E20 — places a span of `size` inside one of `room` centred on `centre`
static int Align(int centre, int room, int size, u32 align) {
    return centre + ((int)(align & 3) - 1) * ((room - size) >> 1);
}

// 0x13EAD0 over the whole list
static void Window_ClearOptions(Window* self) {
    OptionNode* node = OptionsBegin(self);
    while (node != OptionsEnd(self)) {
        OptionNode* next = Next(node);
        TextLabel_Dtor(&node->label, -1);
        MemFree(node);
        self->options.size--;
        node = next;
    }
    self->options.endPrev = self->options.endNext = guest::addr(OptionsEnd(self));
}

// ── 0x13E970 ──────────────────────────────────────────────────────────────
static Window* Window_Ctor(Window* self) {
    TextLabel_Ctor(&self->title);
    self->options.size = 0;                                     // 0x13ECA0
    self->options.endPrev = self->options.endNext = guest::addr(OptionsEnd(self));
    self->x = self->y = 0;
    self->halfWidth = self->halfHeight = 0;
    memset(&self->frame, 0, sizeof(self->frame));
    self->frame.z = 0xFFFFF0;
    self->frame.rgba = 0x80808080u;
    self->kind = kWindowTitledMenu;
    self->state = kWindowOpening;
    self->_3c = 0.4f;
    self->cursor = 0;
    self->titleAlign = 0x11;
    self->optionAlign = 0x11;
    self->flags = 0;
    return self;
}

static void Window_Delete(Window* self) {
    Window_ClearOptions(self);
    TextLabel_Dtor(&self->title, -1);
    MemFree(self);
}

// 0x13EBA0 — a copy of `label` at the end of the list
static OptionNode* Window_AddOption(Window* self, const TextLabel* label) {
    OptionNode* node = static_cast<OptionNode*>(MemAlloc(sizeof(OptionNode)));
    TextLabel_Copy(&node->label, label);
    OptionNode* end = OptionsEnd(self);
    OptionNode* last = guest::ptr<OptionNode>(end->prev);
    last->next = guest::addr(node);
    node->prev = end->prev;
    end->prev = guest::addr(node);
    node->next = guest::addr(end);
    self->options.size++;
    return node;
}

// ── 0x13E490 ──────────────────────────────────────────────────────────────
// Positions the title and the options inside the window.
static void Window_Layout(Window* self, int contentHeight, u32 titleAlign, u32 optionAlign) {
    const int width = self->halfWidth << 1;
    int tx = Align(self->x, width, self->title.width + 16, titleAlign);
    int ty = Align(self->y + (self->halfHeight - ((self->titleHeight + 8) >> 1)),
                   -self->titleHeight, -(self->title.height + 8), titleAlign >> 4);
    if ((titleAlign & 0xF) == 0) {
        tx = self->x - self->halfWidth + 16;
        ty = self->y + self->halfHeight - 8;
    }
    self->title.x = tx;
    self->title.y = ty;

    int y = (self->y + self->halfHeight - self->titleHeight)
          - (int)((optionAlign >> 4) & 0xF) * ((self->halfHeight * 2 - contentHeight) >> 1);
    for (OptionNode* node = OptionsBegin(self); node != OptionsEnd(self); node = Next(node)) {
        const int height = node->label.height + 8;
        node->label.x = Align(self->x, width, node->label.width + 16, optionAlign);
        node->label.y = y - (height >> 1);
        y -= height;
    }
    self->titleAlign = titleAlign;
    self->optionAlign = optionAlign;
}

// ── 0x13E650 ──────────────────────────────────────────────────────────────
static bool Window_Init(Window* self, const WindowDesc* desc) {
    self->x = desc->x;
    self->y = desc->y;
    self->halfWidth = desc->halfWidth;
    self->halfHeight = desc->halfHeight;
    self->titleHeight = desc->titleHeight;
    self->cursor = desc->cursor;
    self->z = desc->z;
    self->priority = desc->priority;
    self->titleAlign = desc->titleAlign;
    self->optionAlign = desc->optionAlign;

    const u32 count = desc->options.count;
    const u32* texts = guest::ptr<const u32>(desc->options.data);
    if (desc->title == 0) {
        if (count == 0)
            return false;
        self->titleHeight = 0;
        self->kind = kWindowMenu;
    } else {
        TextLabel_SetString(&self->title, guest::ptr<const u16>(desc->title));
        if (count == 0) {
            self->kind = kWindowMessage;
            self->titleHeight = self->halfHeight << 1;
        } else {
            self->kind = kWindowTitledMenu;
        }
    }
    for (u32 i = 0; i < count; i++) {
        if (texts[i] == 0)
            continue;
        TextLabel blank;
        TextLabel_Ctor(&blank);
        OptionNode* node = Window_AddOption(self, &blank);
        TextLabel_Dtor(&blank, -1);
        TextLabel_SetString(&node->label, guest::ptr<const u16>(texts[i]));
    }
    if (self->kind != kWindowMenu && self->titleHeight == 0)
        self->titleHeight = self->title.height + 8;

    int height = self->titleHeight;
    int width = self->title.width + 16;
    OptionNode* node = OptionsBegin(self);
    for (u32 i = 0; i < count; i++, node = Next(node)) {
        height += node->label.height + 8;
        if (width < node->label.width + 16)
            width = node->label.width + 16;
    }
    if (self->halfWidth == 0)
        self->halfWidth = (width >> 1) + 4;
    if (self->halfHeight == 0)
        self->halfHeight = (height >> 1) + 4;

    // The frame grows out of the centre.
    self->frame.z = desc->z;
    self->frame.x = self->x;
    self->frame.y = self->y;
    const s32 target[6] = {self->x, self->y, -self->halfWidth, self->halfHeight,
                           self->halfWidth, -self->halfHeight};
    AnimRect_SetTarget(&self->frame, target, 3);
    Window_Layout(self, height, desc->titleAlign, desc->optionAlign);
    return true;
}

// ── 0x13DD70 ──────────────────────────────────────────────────────────────
static void Window_DrawText(Window* self) {
    if (self->kind != kWindowMenu) {
        if ((self->titleAlign & 0xF) == 0)
            TextLabel_Draw(&self->title, 0x7F828286u, self->z);
        else
            TextLabel_DrawCentered(&self->title, 0x7F828286u, self->z);
    }
    if (self->kind == kWindowMessage)
        return;
    int index = 0;
    for (OptionNode* node = OptionsBegin(self); node != OptionsEnd(self); node = Next(node), index++) {
        if (index != self->cursor) {
            TextLabel_DrawCentered(&node->label, 0x7F000000u, self->z);
            continue;
        }
        TextLabel shadow;
        TextLabel_Copy(&shadow, &node->label);
        shadow.x += 2;
        shadow.y -= 2;
        TextLabel_DrawCentered(&shadow, 0x7F000000u, self->z);
        if (self->kind == kWindowMenu)
            TextLabel_DrawCentered(&node->label, 0x7F95A8C8u, self->z);
        else if (self->kind == kWindowTitledMenu)
            TextLabel_DrawCentered(&node->label, 0x7F3636A0u, self->z);
        TextLabel_Dtor(&shadow, -1);
    }
}

// A colour moved by `shade` times a per-channel amount.
static u32 Shaded(float shade, int r, int dr, int g, int dg, int b, int db) {
    return (u32)(((int)(shade * (float)dr) + r) & 0xFF)
         | (u32)(((int)(shade * (float)dg) + g) & 0xFF) << 8
         | (u32)(((int)(shade * (float)db) + b) & 0xFF) << 16
         | (u32)((int)(shade * 0.0f) + 0x7F) << 24;
}

// ── 0x13DF00 ──────────────────────────────────────────────────────────────
static void Window_Draw(Window* self, GsDevice* dev, Font*) {
    const AnimRect& f = self->frame;
    const bool titled = self->kind == kWindowMessage || self->kind == kWindowTitledMenu;
    if (titled) {
        self->frame.rgba = 0x7F464246u;
        AnimRect_Fill(&self->frame);
    } else {
        const u32 far = Shaded(gFrameShade, 0x5A, -24, 0x23, -18, 0x22, -17);
        const s32 rect[10] = {f.x + f.rect[0], f.y + f.rect[1], f.x + f.rect[2], f.y + f.rect[3],
                              (s32)self->z, 0, 0x7F22235A, (s32)far, 0x7F111142, (s32)far};
        Gs_FillGradient(rect);
    }
    if (titled) {
        const u32 far = Shaded(gPanelShade, 0x2E, -14, 0x2D, -14, 0x3C, -18);
        const s32 rect[10] = {f.x + f.rect[0] + 2, f.y + f.rect[1] - 2, f.x + f.rect[2] - 2,
                              f.y + f.rect[3] + 2, (s32)f.z, 0,
                              0x7F3C2D2E, (s32)far, 0x7F2A1F20, (s32)far};
        Gs_FillGradient(rect);
    } else {
        const FrameRect rect = {f.x + f.rect[0] + 4, f.y + f.rect[1] - 4, f.x + f.rect[2] - 4,
                                f.y + f.rect[3] + 4, f.z, 0, 0x7F000000u};
        Gs_DrawFrameRect(&rect, 2);
    }
    if (self->state != kWindowOpen)
        return;
    GsDevice_Begin2D(dev);
    Window_DrawText(self);
    GsDevice_End2D(dev);
    if (self->kind != kWindowMenu && self->options.size != 0) {
        // a rule under the title
        const int y = self->y + self->halfHeight - 4 - self->titleHeight;
        const FrameRect rule = {self->x - self->halfWidth + 10, y + 1, self->x + self->halfWidth - 10,
                                y - 1, 0xFFFFF0, 0, 0x7F464246u};
        Gs_FillRect2(&rule);
    }
}

// ══ Manager ═══════════════════════════════════════════════════════════════
static inline Window* WindowOf(GuestListNode* node) { return guest::ptr<Window>(node->value); }

// ── 0x13DC20 ──────────────────────────────────────────────────────────────
WindowMgr* WindowMgr_Ctor(WindowMgr* self) {
    GuestList_Init(&self->windows);
    PadRepeatPair_Ctor(&self->upDown);
    PadRepeatPair_SetMasks(&self->upDown, kPadUp, kPadDown);
    self->active = 0;
    self->locked = 0;
    PadRepeatPair_SetTiming(&self->upDown, 18, 8, 10);
    return self;
}

// ── 0x13D410 ──────────────────────────────────────────────────────────────
// The open window with the highest priority; the newest of equals.
static Window* WindowMgr_FindTop(WindowMgr* self) {
    Window* top = nullptr;
    for (GuestListNode* n = GuestList_Begin(&self->windows); n != GuestList_End(&self->windows);
         n = GuestList_Next(n)) {
        Window* w = WindowOf(n);
        if (w->state == kWindowOpen && (w->flags & 1) == 0
            && (!top || (top->priority & 0xF) <= (w->priority & 0xF)))
            top = w;
    }
    return top;
}

static GuestListNode* WindowMgr_Find(WindowMgr* self, Window* window) {
    GuestListNode* n = GuestList_Begin(&self->windows);
    while (n != GuestList_End(&self->windows) && n->value != guest::addr(window))
        n = GuestList_Next(n);
    return n;
}

// ── 0x13D150 ──────────────────────────────────────────────────────────────
void WindowMgr_Update(WindowMgr* self) {
    GuestListNode* n = GuestList_Begin(&self->windows);
    while (n != GuestList_End(&self->windows)) {
        Window* w = WindowOf(n);
        if (w->state == kWindowClosing) {
            if (!AnimRect_Step(&w->frame))
                w->state = kWindowClosed;
        } else if (w->state == kWindowOpening) {
            if (!AnimRect_Step(&w->frame))
                w->state = kWindowOpen;
        }
        if (w->state == kWindowClosed) {
            Window_Delete(w);
            n->value = 0;
            n = GuestList_Erase(&self->windows, n);
        } else {
            n = GuestList_Next(n);
        }
    }

    Window* active = guest::ptr<Window>(self->active);
    if (self->locked != 0 || !active || active->state != kWindowOpen)
        return;
    PadRepeat_Update(&self->upDown.a);
    PadRepeat_Update(&self->upDown.b);
    if (!PadRepeat_IsHeld(&self->upDown.b) && self->upDown.a.fired != 0) {
        if (active->kind != kWindowMessage)
            Sound_PlaySE(2, 0, 0x3FFF);
        if (--active->cursor < 0)
            active->cursor = (s32)active->options.size - 1;
    } else if (!PadRepeat_IsHeld(&self->upDown.a) && self->upDown.b.fired != 0) {
        if (active->kind != kWindowMessage)
            Sound_PlaySE(2, 0, 0x3FFF);
        if ((u32)++active->cursor >= active->options.size)
            active->cursor = 0;
    }
}

// ── 0x13D0C0 ──────────────────────────────────────────────────────────────
void WindowMgr_Draw(WindowMgr* self, GsDevice* dev, Font* font) {
    GsDevice* device = *gGsDevice;
    GsDevice_Begin2D(device);
    for (GuestListNode* n = GuestList_Begin(&self->windows); n != GuestList_End(&self->windows);
         n = GuestList_Next(n))
        Window_Draw(WindowOf(n), dev, font);
    GsDevice_End2D(device);
}

// ── 0x13D4E0 ──────────────────────────────────────────────────────────────
void WindowMgr_Clear(WindowMgr* self) {
    GuestListNode* n = GuestList_Begin(&self->windows);
    while (n != GuestList_End(&self->windows)) {
        if (n->value)
            Window_Delete(WindowOf(n));
        n = GuestList_Erase(&self->windows, n);
    }
    self->active = 0;
}

// ── 0x13D600 ──────────────────────────────────────────────────────────────
int WindowMgr_Close(WindowMgr* self, Window* window) {
    GuestListNode* n = WindowMgr_Find(self, window);
    if (n == GuestList_End(&self->windows))
        return -1;
    Window* w = WindowOf(n);
    w->state = kWindowClosing;
    const s32 target[6] = {w->x, w->y, 0, 0, 0, 0};     // back into the centre
    AnimRect_SetTarget(&w->frame, target, 2);
    self->active = guest::addr(WindowMgr_FindTop(self));
    return w->cursor;
}

// ── 0x13D6D0 ──────────────────────────────────────────────────────────────
int WindowMgr_GetCursor(WindowMgr* self, Window* window) {
    GuestListNode* n = WindowMgr_Find(self, window);
    return n == GuestList_End(&self->windows) ? -1 : WindowOf(n)->cursor;
}

// ── 0x13EA60 ──────────────────────────────────────────────────────────────
void WindowDesc_Init(WindowDesc* desc) {
    desc->x = desc->y = 0;
    desc->halfWidth = desc->halfHeight = 0;
    desc->titleHeight = 0;
    desc->z = 0xFFF000;
    desc->title = 0;
    desc->options.count = 0;
    desc->titleAlign = 0x11;
    desc->optionAlign = 0x11;
    desc->cursor = 0;
    desc->priority = 0;
}

// 0x13D7B0 — at most 16 windows
static Window* WindowMgr_NewWindow(WindowMgr* self) {
    if (self->windows.size >= 0x11)
        return nullptr;
    void* mem = MemAlloc(sizeof(Window));
    return mem ? Window_Ctor(static_cast<Window*>(mem)) : nullptr;
}

// ── 0x13D8E0 ──────────────────────────────────────────────────────────────
Window* WindowMgr_Open(WindowMgr* self, WindowDesc* desc) {
    Window* window = WindowMgr_NewWindow(self);
    if (!window)
        return nullptr;
    Window* active = guest::ptr<Window>(self->active);
    if (desc->priority == 0 && active)
        desc->priority = active->priority & 0xF;
    if (!Window_Init(window, desc))
        return nullptr;                                 // (the window is not freed)
    if (!active || (s32)(active->priority & 0xF) <= (s32)desc->priority)
        self->active = guest::addr(window);
    GuestList_Insert(&self->windows, GuestList_End(&self->windows), guest::addr(window));
    return window;
}

// ── 0x13D810 ──────────────────────────────────────────────────────────────
// A window that never takes the input: something to read while another
// window asks, or while nothing does.
Window* WindowMgr_OpenPassive(WindowMgr* self, WindowDesc* desc) {
    Window* window = WindowMgr_NewWindow(self);
    if (!window)
        return nullptr;
    Window* active = guest::ptr<Window>(self->active);
    if (desc->priority == 0 && active)
        desc->priority = active->priority & 0xF;
    if (!Window_Init(window, desc))
        return nullptr;
    window->flags = 1;
    GuestList_Insert(&self->windows, GuestList_End(&self->windows), guest::addr(window));
    return window;
}

// ── 0x13D9D0 ──────────────────────────────────────────────────────────────
Window* WindowMgr_OpenMessage(WindowMgr* self, int x, int y, u32 z, u32 priority,
                              int halfWidth, int halfHeight, const u16* text, u32 titleAlign) {
    WindowDesc desc;
    memset(&desc, 0, sizeof(desc));
    WindowDesc_Init(&desc);
    desc.x = x;
    desc.y = y;
    desc.halfWidth = halfWidth;
    desc.halfHeight = halfHeight;
    desc.z = z;
    desc.title = guest::addr(text);
    desc.titleAlign = titleAlign;
    desc.priority = priority;
    return WindowMgr_Open(self, &desc);
}
