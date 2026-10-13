// The in-world HUD — decompiled from 0x21D4D0–0x220630.  See include/game/hud.h.
//
// Decompiled here: the manager and the text widgets (hints, area name, date,
// notices) with their frames.  The gauges, death screen, message window and
// item-drop markers are other widget classes, still stand-ins.

#include "game/hud.h"
#include "game/chara_resources.h"
#include "engine/bgm.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <cstring>
extern "C" void Native_HudAnchor(int on);     // (native: see hle.cpp)

struct SpriteSet;

extern "C" {
    int   ItemDrops_fn242550(void* drops, const void* matrix, void* swordDb);  // 0x242550  what is within reach
    const u16* ItemDrops_ObjectName(int id);                                   // 0x21C240
    int   ItemDrops_BagItem(int id);                                           // 0x21B0E0
    int   PlayerData_HasFreeItemSlot(struct PlayerData* self, int home);       // 0x177760
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void  GsDevice_SetBlend(GsDevice* dev, u64 alpha);          // 0x130C50
    void  GsDevice_ClearScreenColor(GsDevice* dev, u32 rgba);   // 0x12F1F0  a blended full-screen sprite
    int   RenderMgr_FindEffect(void* mgr, u32 type);            // 0x14F0F0
    SpriteSet* SpriteSet_Ctor(void* mem);                       // 0x17C630
    char  SpriteSet_Load(SpriteSet* set, int assetId, int assetId2);    // 0x17C430
    int   SpriteSet_Bind(SpriteSet* set);                       // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);             // 0x17C570
    void  SpriteSet_DrawEx(SpriteSet* set, int index, const void* params);             // 0x17ADF0
    void* SpriteSet_GetSprite(SpriteSet* set, int index);       // 0x17BDF0
    void  SpriteSet_Draw(SpriteSet* set, int index, int x, int y, u32 z, u32 rgba);     // 0x17B780
    void  SpriteSet_DrawRotated(SpriteSet* set, int index, const void* params);         // 0x179A40
}

static_assert(sizeof(HudBox) == 0x14 && sizeof(HudText) == 0x50 && sizeof(HudNotice) == 0x60, "HudText");
static_assert(offsetof(HudText, time) == 0x38 && offsetof(HudText, state) == 0x44, "HudText");
static_assert(offsetof(Hud, hint) == 0x20 && offsetof(Hud, sprites) == 0x48 && sizeof(Hud) == 0x54, "Hud");

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const GuestPtrVar<Font>     gFont{0x2EE67Cu};
static const GuestPtrVar<void>     gPlayerDataHud{0x2EE688u};
// The game asks the render manager's own accessor (0x14EBC0, which creates it
// on first use); by now it always exists.
static const GuestPtrVar<void>     gRenderMgr{0x2EE684u};
static s32& sNoticeStack = *guest::ptr<s32>(0x2EE73Cu);     // height of the notices so far this frame

enum : u32 {
    kWidgetVtable = 0x2EC410u, kNoticeVtable = 0x2EC450u, kTextVtable = 0x2EC470u,
    kStringVtable = 0x2EAF20u, kLabelVtable = 0x2EAF30u,
};
enum { kFadeFrames = 5, kCloseFrames = 6 };
enum { kAppearing = 0, kLeaving = 1, kFlashing = 2 };

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline bool EventEffect() { return *gRenderMgr != nullptr && RenderMgr_FindEffect(*gRenderMgr, 8) != 0; }
static inline bool Slides(int type) { return type == 5 || type == 3 || type == 4 || type == 2; }

struct BoxRect { u32 flags; s32 x, y; u32 z; s32 w, h; u32 rgba; };

// ── Frames (0x21E040, 0x21F990–0x220630) ─────────────────────────────────

// 0x21E040 — a flat rectangle.
static void Box_Fill(const BoxRect* r) {
    struct { u64 prim; u32 rgba0, q0; u16 x0, y0; u32 z0; u32 rgba1, q1; u16 x1, y1; u32 z1; } packet{};
    packet.prim = 0x14E;
    packet.x0 = (u16)r->x;
    packet.y0 = (u16)r->y;
    packet.z0 = r->z;
    packet.rgba0 = r->rgba;
    packet.x1 = (u16)(r->x + r->w);
    packet.y1 = (u16)(r->y + r->h);
    packet.z1 = r->z;
    packet.rgba1 = r->rgba;
    GsDevice_DrawPrim(*gGsDevice, &packet, 2);
}

static inline u32 BoxAlpha(const HudBox* box) { return (u32)(int)(box->alpha * 128.0f) << 24; }

// 0x21F990 / 0x21FC00 — a backing colour, a stretched centre and two end caps.
static void Box_DrawCapped(HudBox* box, SpriteSet* set, const s32* pos, u32 backing, int centre, int cap) {
    const s32 w = box->w + 0xB0, h = box->h + 0x40;
    const s32 x = pos[0] - w / 2, y = pos[1] - h / 2;
    const u32 z = (u32)pos[2];
    const u32 alpha = BoxAlpha(box);
    BoxRect r{4, x - 0x20, y - 0x20, z, w + 0x40, h + 0x40, alpha + backing};
    Box_Fill(&r);
    const u32 rgba = alpha + 0x808080u;
    r = {4, x, y, z, w, h, rgba};
    SpriteSet_DrawEx(set, centre, &r);
    r = {3, x, y, z, -1, h, rgba};
    SpriteSet_DrawEx(set, cap, &r);
    const u8* sprite = static_cast<const u8*>(SpriteSet_GetSprite(set, cap));
    r = {0x23, x + w - (s32)*reinterpret_cast<const u16*>(sprite + 0xC) * 16, y, z, -1, h, rgba};
    SpriteSet_DrawEx(set, cap, &r);
}

// 0x220040 / 0x21FE70 — a backing colour and one stretched sprite.
static void Box_DrawFlat(HudBox* box, SpriteSet* set, const s32* pos, u32 backing, int sprite) {
    const s32 w = box->w + 0xB0, h = box->h + 0x40;
    const s32 x = pos[0] - w / 2, y = pos[1] - h / 2;
    const u32 z = (u32)pos[2];
    const u32 alpha = BoxAlpha(box);
    BoxRect r{4, x - 0x20, y - 0x20, z, w + 0x40, h + 0x40, alpha + backing};
    Box_Fill(&r);
    r = {4, x, y, z, w, h, alpha + 0x808080u};
    SpriteSet_DrawEx(set, sprite, &r);
}

// 0x220210 — an additive glow around the frame: four corners and four edges.
static void Box_DrawGlow(HudBox* box, SpriteSet* set, const s32* pos) {
    GsDevice* dev = *gGsDevice;
    const s32 w = box->w + 0x80, h = box->h;
    const s32 x = pos[0] - w / 2, y = pos[1] - h / 2;
    const u32 z = (u32)pos[2];
    const u32 rgba = BoxAlpha(box) + 0x808080u;
    const u8* corner = static_cast<const u8*>(SpriteSet_GetSprite(set, 0x2C));
    const s32 cw = *reinterpret_cast<const u16*>(corner + 0xC) * 16;
    const s32 ch = *reinterpret_cast<const u16*>(corner + 0xE) * 16;
    GsDevice_SetBlend(dev, 0x8000000048ull);
    BoxRect r{1, x - cw, y - ch, z, -1, -1, rgba};
    SpriteSet_DrawEx(set, 0x2C, &r);
    r = {0x21, x + w, y - ch, z, -1, -1, rgba};
    SpriteSet_DrawEx(set, 0x2C, &r);
    r = {0x21, x, y - ch, z, w, -1, rgba};
    SpriteSet_DrawEx(set, 0x2D, &r);
    r = {0x41, x, y + h, z, w, -1, rgba};
    SpriteSet_DrawEx(set, 0x2D, &r);
    r = {1, x - cw, y, z, -1, h, rgba};
    SpriteSet_DrawEx(set, 0x2E, &r);
    r = {0x21, x + w, y, z, -1, h, rgba};
    SpriteSet_DrawEx(set, 0x2E, &r);
    r = {0x41, x - cw, y + h, z, -1, -1, rgba};
    SpriteSet_DrawEx(set, 0x2C, &r);
    r = {0x61, x + w, y + h, z, -1, -1, rgba};
    SpriteSet_DrawEx(set, 0x2C, &r);
    GsDevice_SetBlend(dev, 0x8000000044ull);
}

// 0x220630
static void Box_Draw(HudBox* box, SpriteSet* set, const s32* pos, int type) {
    if (SpriteSet_GetSprite(set, 0x2C) == nullptr)
        return;     // (not in the original: its sprites are always loaded by now)
    if (box->frame > 5 && box->frame < 0x13) {
        const float alpha = box->alpha;
        box->alpha = sinf((float)((box->frame - 6) * 180 / 12) * 3.14f / 180.0f);
        Box_DrawGlow(box, set, pos);
        box->alpha = alpha;
    }
    switch (type) {
    case 0: Box_DrawCapped(box, set, pos, 0x7C827Cu, 0x2F, 0x2A); break;
    case 1:
        box->frame++;
        Box_DrawCapped(box, set, pos, 0x0D0C0Du, 0x30, 0x2B);
        break;
    case 2:
        box->alpha = 1.0f;
        Box_DrawFlat(box, set, pos, 0x21252Au, 0x32);
        break;
    case 3:
        box->alpha = 1.0f;
        Box_DrawFlat(box, set, pos, 0x152D3Cu, 0x31);
        break;
    case 4: Box_DrawFlat(box, set, pos, 0x21252Au, 0x32); break;
    case 5: Box_DrawFlat(box, set, pos, 0x152D3Cu, 0x31); break;
    default: Box_DrawCapped(box, set, pos, 0x0D0C0Du, 0x30, 0x2B); break;
    }
}

// ── Text widget (0x21ED00–0x21F900) ──────────────────────────────────────

static inline const u16* HudText_String(HudText* self) {
    return P<const u16>(P<WStringRep>(self->string.rep)->data);
}

// 0x21F900
static HudText* HudText_Ctor(HudText* self) {
    self->vtable = kWidgetVtable;       // base constructor, 0x21E140
    self->kind = 0;
    self->vtable = kTextVtable;
    self->labelVtable = kLabelVtable;
    WString_Ctor(&self->string);
    WString_Reserve(&self->string, 0);
    self->kind = 1;
    HudBox* box = static_cast<HudBox*>(MemAlloc(sizeof(HudBox)));
    if (box != nullptr)
        box->alpha = 0.7f;
    self->box = guest::addr(box);
    self->fade = kFadeFrames;
    self->state = kAppearing;
    return self;
}

// 0x21ECC0
static HudNotice* HudNotice_Ctor(HudNotice* self) {
    HudText_Ctor(&self->text);
    self->text.vtable = kNoticeVtable;
    return self;
}

// 0x21F7D0 (and 0x21EAC0, the notice's, which only differs in the vtable it restores first)
static HudText* HudText_Dtor(HudText* self, s16 flag) {
    if (self == nullptr)
        return self;
    self->vtable = kTextVtable;
    if (self->box != 0)
        MemFree(P(self->box));
    self->box = 0;
    self->labelVtable = kLabelVtable;
    self->string.vtable = kStringVtable;
    WString_Release(&self->string);
    self->vtable = kWidgetVtable;
    if (flag > 0)
        MemFree(self);
    return self;
}

// 0x21ED00 — starts it leaving (or ends it at once if it has already faded out).
static int HudText_Close(HudText* self) {
    if (self->state == kLeaving && self->fade == 0) {
        self->time = 0;
    } else {
        self->time = kCloseFrames;
        self->state = kLeaving;
        self->fade = kFadeFrames;
    }
    return 1;
}

// 0x21F4A0
static void HudText_FadeOut(HudText* self) {
    if (self->state == kLeaving)
        return;
    self->fade = self->state == kAppearing ? kFadeFrames - self->fade : kFadeFrames;
    self->state = kLeaving;
}

// 0x21F4F0
static void HudText_FadeIn(HudText* self) {
    if (self->state == kAppearing)
        return;
    if (self->state == kLeaving) {
        self->fade = kFadeFrames - self->fade;
    } else {
        self->fade = kFadeFrames;
        Sound_PlaySE(Slides(self->type) ? 0 : 1, 0, 0x3FFF);
    }
    self->state = kAppearing;
}

// 0x21F5A0
static void HudText_Flash(HudText* self) {
    if (self->state != kFlashing) {
        self->state = kFlashing;
        self->fade = kFadeFrames;
    }
}

// 0x21F5D0 — vtable +0x0C
static int HudText_Show(HudText* self, HudTextParams* params, int) {
    if (params->type == kHudDefault)
        params->type = kHudPlain;
    const u16* text = P<const u16>(params->text);
    WString_Assign(&self->string, text, text + WStr_Length(text));
    self->type = params->type;
    self->time = params->time;

    Font* font = *gFont;
    Native_HudAnchor(1);                // (native: the edge of a wider picture, if asked for)
    const u32 right = Screen_X(0, 1), bottom = Screen_Y(0, 1);
    Native_HudAnchor(0);
    Font_Measure(font, HudText_String(self), 1.0f);
    const FontExtent* extent = Font_GetExtent(font);
    self->x = (s32)((right & 0xFFFF) - (u32)(extent->w >> 1)) - 0x220;
    self->y = (s32)((bottom & 0xFFFF) - (u32)(extent->h >> 1)) - 0x1E0;
    self->z = 0xFFFFF0u;
    HudBox* box = P<HudBox>(self->box);
    box->w = extent->w;
    box->h = extent->h;
    box->alpha = 0.7f;
    box->frame = 0;
    self->halfW = extent->w >> 1;
    self->halfH = extent->h >> 1;

    if (self->fade == kFadeFrames && self->state == kAppearing)
        Sound_PlaySE(Slides(self->type) ? 0 : 1, 0, 0x3FFF);
    if (self->type == kHudDark && EventEffect()) {
        self->time = 0;
        self->fade = 0;
        self->state = kLeaving;
    }
    return 1;
}

// 0x21F2F0 — vtable +0x10
static int HudText_Update(HudText* self) {
    if (self->state == kFlashing) {
        HudBox* box = P<HudBox>(self->box);
        if (box->frame < 6)
            box->frame = 6;
        box->frame++;
        if (box->frame > 0x12)
            HudText_Close(self);
        return 1;
    }
    if (self->type == kHudDark && self->state != kLeaving && EventEffect()) {
        HudText_Close(self);
        return 1;
    }
    if (self->fade == 0) {
        if (self->time == kCloseFrames) {
            self->fade = kFadeFrames;
            self->state = kLeaving;
        }
    } else {
        self->fade--;
    }
    if (self->time == -1)
        return 1;
    if (self->time == 0)
        return 0;
    self->time--;
    return 1;
}

// 0x21ED50 — vtable +0x14
static int HudText_Draw(HudText* self, SpriteSet* sprites) {
    s32 pos[4] = {self->x, self->y, (s32)self->z, (s32)self->_2c};
    HudBox* box = P<HudBox>(self->box);
    u32 rgba = 0x80ACB2BEu;
    int alpha = 0x80;
    box->alpha = 0.7f;

    const int type = self->type;
    const bool slides = type == 2 || type == 3 || type == 4 || type == 5;
    if (type == kHudDark)
        rgba = 0x8093B1C7u;
    if (!slides) {
        const int t = self->state == kAppearing ? self->fade : kFadeFrames - self->fade;
        alpha = 0x80 - (t << 7) / kFadeFrames;
        box->alpha = 0.7f - (float)t / 5.0f * 0.7f;
    } else {
        if (type == 4 || type == 5)
            box->alpha = 0.5f;
        if (self->state == kAppearing) {
            const int t = self->fade;
            pos[0] += t * 0x220 / kFadeFrames + t * self->halfW * 4 / kFadeFrames;
        } else {
            const int t = kFadeFrames - self->fade;
            pos[0] += t * 0x1E0 / kFadeFrames + t * self->halfW * 4 / kFadeFrames;
        }
    }

    if (box->alpha == 0.0f || self->time == 0)
        return 1;
    Box_Draw(box, sprites, pos, type);

    Font* font = *gFont;
    const u16* text = HudText_String(self);
    const u32 oldColor = Font_SetColor(font, (u32)alpha << 24);
    const u32 oldFixed = Font_SetFixedColor(font, 0);
    Font_DrawString(font, text, pos[0] - self->halfW + 0xB, pos[1] - self->halfH + 0xB, (u32)pos[2], 1.0f, 0.0f);
    Font_SetFixedColor(font, oldFixed);
    Font_SetColor(font, (rgba & 0xFFFFFFu) | (u32)alpha << 24);
    if (type == 4 || type == 5)
        Font_SetColor(font, (rgba & 0xFFFFFFu) | (u32)(int)((float)(rgba >> 24) * 0.5f) << 24);
    Font_DrawString(font, text, pos[0] - self->halfW - 0xB, pos[1] - self->halfH - 0xB, (u32)pos[2], 1.0f, 0.0f);
    if (type == 4 || type == 5) {
        // A dark wash over the whole widget.
        box->alpha = 0.15f;
        const s32 w = box->w + 0xB0, h = box->h + 0x40;
        const BoxRect r{4, pos[0] - w / 2 - 0x20, pos[1] - h / 2 - 0x20, (u32)pos[2], w + 0x40, h + 0x40,
                        BoxAlpha(box) + 0x21252Au};
        Box_Fill(&r);
    }
    Font_SetColor(font, oldColor);
    return 1;
}

// 0x21EC80 — vtable +0x10 of the notice: claims its place in the stack first.
static int HudNotice_Update(HudNotice* self) {
    self->stackY = sNoticeStack;
    sNoticeStack += P<HudBox>(self->text.box)->h + 0xA0;
    return HudText_Update(&self->text);
}

// 0x21EC00 — vtable +0x14 of the notice
static int HudNotice_Draw(HudNotice* self, SpriteSet* sprites) {
    const s32 x = self->text.x, y = self->text.y;
    const u32 z = self->text.z, w = self->text._2c;
    self->text.y -= self->stackY;
    HudText_Draw(&self->text, sprites);
    self->text.x = x;
    self->text.y = y;
    self->text.z = z;
    self->text._2c = w;
    return 1;
}

GUEST_BIND(0x21F7D0, HudText_Dtor);
GUEST_BIND(0x21F5D0, HudText_Show);
GUEST_BIND(0x21F2F0, HudText_Update);
GUEST_BIND(0x21ED50, HudText_Draw);
static HudText* HudNotice_Dtor(HudText* self, s16 flag) { return HudText_Dtor(self, flag); }   // 0x21EAC0
GUEST_BIND(0x21EAC0, HudNotice_Dtor);
GUEST_BIND(0x21EC80, HudNotice_Update);
GUEST_BIND(0x21EC00, HudNotice_Draw);

// ── Talk prompt (0x21E160–0x21EA80) ──────────────────────────────────────

extern "C" int GsDevice_Project(void* dev, s32* out, const float* world);   // 0x12F7E0  (native camera)

static_assert(sizeof(HudTalkPrompt) == 0x24, "HudTalkPrompt");

// 0x21E230 — vtable +0x0C
static int TalkPrompt_Show(HudTalkPrompt* self, const HudTalkInfo* info, int) {
    self->params = info->params;
    self->name = info->name;
    self->frames = info->frames;
    self->special = info->special;
    return 1;
}

// 0x21E160 — where the person's head is (a point of the skeleton, see speech.cpp).
static int TalkPrompt_Locate(HudTalkPrompt* self, s32* out) {
    if (self->params == 0)
        return 0;
    const u32 model = P<CharaParams>(self->params)->link[1];
    if (model == 0 || P<u32>(model)[1] == 0)
        return 0;
    const float* head = P<float>(P<u32>(model)[1] + 0x5D0);
    const float point[4] = {head[0], head[1] + 0.06f, head[2], 1.0f};
    s32 xy[4] = {};
    if (GsDevice_Project(*gGsDevice, xy, point) == 0)
        return 0;
    out[0] = xy[0];
    out[1] = xy[1];
    return 1;
}

// 0x21E9A0 — vtable +0x10
static int TalkPrompt_Update(HudTalkPrompt* self) {
    if (self->frames == -1)
        return 0;
    self->onScreen = TalkPrompt_Locate(self, &self->x) != 0;
    if (self->frames != 0)
        self->frames--;
    return 1;
}

// 0x21E260 — vtable +0x14: a plate as wide as the name above the head, with
// two layers of shadow, and the name on it.
static int TalkPrompt_Draw(HudTalkPrompt* self, SpriteSet*) {
    if (self->frames == 0 || self->onScreen == 0)
        return 1;
    Font* font = *gFont;
    const u16* name = P<const u16>(self->name);
    const s32 x = self->x, y = self->y;
    const s16 top = (s16)y - 0x210;
    Font_Measure(font, name, 1.0f);
    const s16 width = (s16)Font_GetExtent(font)->w;
    struct Corner { s32 x, y, ox, oy; u32 rgba; };
    Corner plate[6];
    memcpy(plate, P<Corner>(self->special == 1 ? 0x2D9800u : 0x2D9780u), sizeof plate);
    const int half = (width >> 5) + 0xB;
    plate[3].x = plate[2].x = -half;
    plate[5].x = plate[4].x = plate[1].x = half;
    struct Vertex { u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 prim; Vertex v[6]; } packet;
    packet.prim = 0x14D;
    for (int layer = 2; layer >= 0; layer--) {
        for (int i = 0; i < 6; i++) {
            const Corner& c = plate[i];
            const u32 rgba = layer == 2 ? 0x80000000u : layer == 1 ? 0x80787878u : c.rgba;
            packet.v[i] = {rgba, 1.0f, (u16)((s16)x + (s16)(c.x + c.ox * layer) * 0x10),
                           (u16)(top + (s16)(c.y + c.oy * layer) * 0x10), 0xFFFFF0u};
        }
        GsDevice_DrawPrim(*gGsDevice, &packet, 6);
    }
    const u32 old = Font_SetColor(font, 0x80222222u);
    const s32 left = x - (width >> 1);
    Font_DrawStringPlain(font, name, left + 0x10, y - 0x2B0, 0xFFFFF0u);
    Font_SetColor(font, 0x80BEBEBEu);
    Font_DrawStringPlain(font, name, left, y - 0x2C0, 0xFFFFF0u);
    Font_SetColor(font, old);
    return 1;
}

// 0x21EA20 — vtable +0x08
static HudTalkPrompt* TalkPrompt_Dtor(HudTalkPrompt* self, s16 flag) {
    if (self != nullptr && flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x21EA20, TalkPrompt_Dtor);
GUEST_BIND(0x21E230, TalkPrompt_Show);
GUEST_BIND(0x21E9A0, TalkPrompt_Update);
GUEST_BIND(0x21E260, TalkPrompt_Draw);

// ── The gauges (0x225E40–0x227490, vtable 0x2EC5D0, 0x80 bytes) ───────────
// Bottom left: the clock (a sun that sets through the period), health with a
// trail where it has just been lost, the sword's segments with its heat, and
// the beads of vigour.  Shown again every frame by World_UpdateGauges.
struct HudGaugeParams {
    s32 x, y; u32 z; u32 _0c, _10;
    s32 special;                // PlayerData+0x6E80: the health bar is drawn in one flat colour
    s32 healthMax, health, vigourMax, vigour;
    s32 swordSegments, swordHeat;
    s32 time, timeMax, period;
};
struct HudGauges {
    s32 kind; u32 vtable;       // +0x00
    u32 _08[2];
    s32 x, y; u32 z;            // +0x10
    u32 _1c;
    s32 shown;                  // +0x20  frames left to draw (set to 2 by every Show)
    s32 special;                // +0x24
    s32 healthMax, health;      // +0x28  (0 … 2000)
    s32 healthTrail;            // +0x30  falls to the health by 4 a frame
    s32 vigourMax, vigour;      // +0x34  (10 … 15)
    s32 swordSegments;          // +0x3C  (0 … 5)
    s32 swordHeat;              // +0x40  600 = one segment
    s32 time, timeMax;          // +0x44
    s32 period;                 // +0x4C
    u32 _50;
    s32 glow, glowLength;       // +0x54  the clock pulses once the period's time is up
    u32 glowColor;              // +0x5C
    s32 glowWait;               // +0x60
    s32 spark;                  // +0x64  a spinning flash
    u32 sparkColor;             // +0x68
    float sparkAngle, sparkScale;   // +0x6C
    s32 flash;                  // +0x74  "the day is over": 0x2C frames
    u32 flashColor;             // +0x78
    float flashScale;           // +0x7C
};
static_assert(sizeof(HudGauges) == 0x80 && offsetof(HudGauges, flash) == 0x74, "HudGauges");

struct GaugePart { u16 sprite; s16 x, y; };     // where a piece goes, from the gauges' corner
static const GaugePart* const kGaugeBack   = guest::ptr<const GaugePart>(0x2D9A50u);   // 2
static const GaugePart* const kGaugeClock  = guest::ptr<const GaugePart>(0x2D9A5Cu);   // 6
static const GaugePart* const kGaugeFx     = guest::ptr<const GaugePart>(0x2D9A80u);   // glow, spark, flash
static const GaugePart* const kGaugeHealth = guest::ptr<const GaugePart>(0x2D9A92u);   // 5
static const GaugePart* const kGaugeSword  = guest::ptr<const GaugePart>(0x2D9AB0u);   // 5
static const GaugePart* const kGaugeVigour = guest::ptr<const GaugePart>(0x2D9ACEu);   // 5

static void Gauge_Part(HudGauges* self, SpriteSet* set, const GaugePart& part, s32 dx, s32 w, u32 rgba = 0x80808080u) {
    BoxRect r{1, self->x + part.x + dx, self->y + part.y, self->z, w, -1, rgba};
    SpriteSet_DrawEx(set, part.sprite, &r);
}
static s32 Gauge_Step(SpriteSet* set, const GaugePart& part) {
    return (s32)*reinterpret_cast<u16*>(static_cast<u8*>(SpriteSet_GetSprite(set, part.sprite)) + 0xC) * 0x10 + 0x10;
}

// 0x225E40 — vigour: a bead for each point it can hold, the filled ones over them.
static int Gauges_DrawVigour(HudGauges* self, SpriteSet* set) {
    const GaugePart* t = kGaugeVigour;
    Gauge_Part(self, set, t[0], 0, -1);
    const s32 step = Gauge_Step(set, t[1]);
    for (s32 i = 0; i < self->vigourMax; i++)
        Gauge_Part(self, set, t[1], step * i, -1);
    for (s32 i = 0; i < self->vigour; i++)
        Gauge_Part(self, set, t[2], step * i, -1);
    s32 x = 0;
    for (s32 i = 0; i < self->vigourMax; i++) {
        x += step;
        Gauge_Part(self, set, t[3], x - 0x10, -1);
    }
    Gauge_Part(self, set, t[4], x - 0x10, -1);
    return 1;
}

// 0x226180 — the sword: its segments, and how hot it has run.
static int Gauges_DrawSword(HudGauges* self, SpriteSet* set) {
    const GaugePart* t = kGaugeSword;
    Gauge_Part(self, set, t[0], 0, -1);
    const s32 step = Gauge_Step(set, t[1]);
    for (s32 i = 0; i < self->swordSegments; i++)
        Gauge_Part(self, set, t[1], step * i, -1);
    Gauge_Part(self, set, t[2], 0, step * self->swordHeat / 600);
    s32 x = 0;
    for (s32 i = 0; i < self->swordSegments; i++) {
        x += step;
        Gauge_Part(self, set, t[3], x - 0x10, -1);
    }
    Gauge_Part(self, set, t[4], x - 0x10, -1);
    return 1;
}

// 0x2264B0 — health: 240 pixels for 2000.
static int Gauges_DrawHealth(HudGauges* self, SpriteSet* set) {
    const GaugePart* t = kGaugeHealth;
    const s32 max = self->healthMax;
    const s32 full = max * 0xF00 / 2000;
    const s32 now = full * self->health / max;
    Gauge_Part(self, set, t[0], 0, -1);
    Gauge_Part(self, set, t[1], 0, full);
    Gauge_Part(self, set, t[2], 0, full * self->healthTrail / max);
    if (self->special == 0) {
        Gauge_Part(self, set, t[3], 0, now);
    } else {
        BoxRect r{1, self->x + t[3].x, self->y + t[3].y + 0x20, self->z, now, -1, 0x80337AD2u};
        SpriteSet_DrawEx(set, 0x33, &r);
    }
    Gauge_Part(self, set, t[4], full, -1);
    return 1;
}

// 0x2267F0 — the clock: the pieces left of the day, the current one fading
// as its time runs out.
static int Gauges_DrawClock(HudGauges* self, SpriteSet* set) {
    const s32* count = guest::ptr<const s32>(0x2D9AF0u);        // by period: 6, 5, 4, 3, 1
    u32 rgba = 0x80808080u;
    const GaugePart* part = kGaugeClock;
    for (s32 i = count[self->period]; i != 0; i--, part++) {
        if (i == 1 || (self->period == 3 && i == 2))
            rgba = (u32)(0x80 - (self->time << 7) / self->timeMax) << 24 | 0x808080u;
        Gauge_Part(self, set, *part, 0, -1, rgba);
    }
    return 1;
}

// 0x226980 — the additive lights over them.
static int Gauges_DrawLights(HudGauges* self, SpriteSet* set) {
    GsDevice* dev = *gGsDevice;
    GsDevice_SetBlend(dev, 0x8000000048ull);
    if (self->glow != 0)
        Gauge_Part(self, set, kGaugeFx[0], 0, -1, self->glowColor);
    struct { u32 flags; s32 x, y; u32 z; u32 rgba; float scaleX, scaleY, angle; } p;
    if (self->spark != 0) {
        p = {1, self->x + kGaugeFx[1].x, self->y + kGaugeFx[1].y, self->z, self->sparkColor,
             self->sparkScale * 16.0f, self->sparkScale * 16.0f, self->sparkAngle};
        SpriteSet_DrawRotated(set, kGaugeFx[1].sprite, &p);
    }
    if (self->flash != 0) {
        p = {1, self->x + kGaugeFx[2].x, self->y + kGaugeFx[2].y, self->z, self->flashColor,
             self->flashScale * 16.0f, self->flashScale * 16.0f, 0.0f};
        SpriteSet_DrawRotated(set, kGaugeFx[2].sprite, &p);
    }
    GsDevice_SetBlend(dev, 0x8000000044ull);
    return 1;
}

// 0x226B50 — vtable +0x14
static int Gauges_Draw(HudGauges* self, SpriteSet* set) {
    if (self->shown == 0)
        return 1;
    self->special = *reinterpret_cast<s32*>(reinterpret_cast<u8*>(*gPlayerDataHud) + 0x6E80);
    for (int i = 0; i < 2; i++)
        SpriteSet_Draw(set, kGaugeBack[i].sprite, self->x + kGaugeBack[i].x, self->y + kGaugeBack[i].y, self->z,
                       0x80808080u);
    Gauges_DrawClock(self, set);
    Gauges_DrawHealth(self, set);
    Gauges_DrawSword(self, set);
    Gauges_DrawVigour(self, set);
    Gauges_DrawLights(self, set);
    return 1;
}

// 0x226F10 — the spark: 0x22 frames, turning as it swells and fades.
static int Gauges_StepSpark(HudGauges* self) {
    const s32 t = self->spark;
    if (t == 0) {
        self->sparkColor = 0;
        return 1;
    }
    if (t < 0x14) {
        const float f = (float)t / 19.0f;
        self->sparkColor = (u32)(s32)(f * 51.0f) << 24 | 0x808080u;
        self->sparkScale = f * 0.4f + 0.6f;
        self->sparkAngle -= 6.3157897f;
    } else {
        if (t > 0x26)
            self->spark = 0x26;
        const float f = (float)(0x22 - self->spark) / 15.0f;
        self->sparkColor = (u32)(s32)(f * 51.0f) << 24 | 0x808080u;
        self->sparkScale = f * 0.4f + 0.6f;
        self->sparkAngle -= 6.6666665f;
    }
    if (self->sparkAngle < 0.0f)
        self->sparkAngle += 360.0f;
    self->spark--;
    return 1;
}

// 0x227110 — the flash when the day is over (it waits for the clock's glow
// to finish, and sets off the spark on its way).
static int Gauges_StepFlash(HudGauges* self) {
    const s32 t = self->flash;
    if (t == 0) {
        self->flashColor = 0;
        return 1;
    }
    if (t == 0x2C && self->glow != 0)
        return 1;
    if (t == 0x27) {
        self->spark = 0x22;
        self->sparkScale = 0.6f;
    }
    const s32 u = t < 0x17 ? t : 0x2C - t;
    self->flashColor = (u32)((u << 7) / 0x16) << 24 | 0x808080u;
    self->flashScale = (float)u * 0.6f / 22.0f + 0.5f;
    self->flash--;
    return 1;
}

// 0x226C90 — vtable +0x10
static int Gauges_Update(HudGauges* self) {
    if (self->healthTrail < self->health) {
        self->healthTrail = self->health;
    } else if (self->health < self->healthTrail) {
        self->healthTrail -= 4;
        if (self->healthTrail < self->health)
            self->healthTrail = self->health;
    }
    Gauges_StepFlash(self);
    Gauges_StepSpark(self);
    if (self->glow != 0) {
        self->glow--;
    } else if (self->glowWait != 0) {
        self->glowWait--;
    } else if (self->time >= 900) {
        const s32 left = self->timeMax - self->time;
        self->glow = self->glowLength = left * 0x10 / 0x5A + 0x10;
        self->glowWait = left * 5 / 0x5A;
    }
    const s32 half = self->glowLength / 2;
    const float level = self->glow < half ? (float)self->glow * 87.04f / (float)half
                                          : (float)(self->glowLength - self->glow) * 87.04f / (float)half;
    self->glowColor = (u32)(s32)level << 24 | 0x808080u;
    if (self->shown != 0)
        self->shown--;
    return 1;
}

// 0x227280 — vtable +0x0C
static int Gauges_Show(HudGauges* self, const HudGaugeParams* p, int) {
    self->x = p->x;
    self->y = p->y;
    self->z = p->z;
    self->special = p->special;
    self->healthMax = p->healthMax;
    self->health = p->health;
    self->vigourMax = p->vigourMax;
    self->vigour = p->vigour;
    self->swordSegments = p->swordSegments;
    self->swordHeat = p->swordHeat;
    self->time = p->time;
    self->timeMax = p->timeMax;
    self->period = p->period;
    if (self->healthTrail == 0)
        self->healthTrail = self->health;
    self->healthMax = self->healthMax > 2000 ? 2000 : self->healthMax < 0 ? 0 : self->healthMax;
    if (self->healthMax < self->health)
        self->health = self->healthMax;
    else if (self->health < 0)
        self->health = 0;
    self->swordSegments = self->swordSegments > 5 ? 5 : self->swordSegments < 0 ? 0 : self->swordSegments;
    if (self->swordHeat < 0)
        self->swordHeat = 0;
    self->vigourMax = self->vigourMax > 0xF ? 0xF : self->vigourMax < 10 ? 10 : self->vigourMax;
    if (self->vigourMax < self->vigour)
        self->vigour = self->vigourMax;
    else if (self->vigour < 0)
        self->vigour = 0;
    self->shown = 2;
    return 1;
}

// 0x227490
static HudGauges* Gauges_Ctor(HudGauges* self) {
    memset(self, 0, sizeof *self);          // the original leaves what it does not set
    self->kind = 0;                         // base constructor, 0x21E140
    self->vtable = 0x2EC5D0u;
    self->healthMax = 2000;
    self->vigourMax = 10;
    self->swordSegments = 3;
    self->timeMax = 900;
    self->glow = 1;
    self->glowLength = 1;
    self->time = 800;
    return self;
}

// 0x227430 — vtable +0x08
static HudGauges* Gauges_Dtor(HudGauges* self, s16 flag) {
    if (self != nullptr) {
        self->vtable = kWidgetVtable;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
GUEST_BIND(0x227430, Gauges_Dtor);
GUEST_BIND(0x227280, Gauges_Show);
GUEST_BIND(0x226C90, Gauges_Update);
GUEST_BIND(0x226B50, Gauges_Draw);

// ── The enemy gauges (0x242610–0x242D80, vtable 0x2EC850, 0x84 bytes) ─────
// A row of plates along the bottom, right of the player's gauges: the name
// and health of the player's opponent (bright) and of up to five others who
// are after the player (dim).  Each has to be shown again every frame
// (Pairs_UpdateHud); a plate slides to its place in the row.
struct EnemySlot {
    u32 params;         // +0x00  CharaParams*
    u16 flags;          // +0x04  1 glow behind it, 2 the opponent (drawn at full strength)
    u16 kind;           // +0x06  0 same faction, 1 an enemy, 3 a named enemy
    u16 x, wantX;       // +0x08
    s16 frames;         // +0x0C  left to show
    u16 _0e;
};
struct HudEnemies {
    s32 kind; u32 vtable;       // +0x00
    EnemySlot slot[6];          // +0x08
    EnemySlot extra;            // +0x68  (never filled in what has been read)
    u32 state;                  // +0x78  2 hidden, 4 delete
    s32 count;                  // +0x7C  shown so far this frame
    s32 mode;                   // +0x80
};
static_assert(sizeof(HudEnemies) == 0x84 && sizeof(EnemySlot) == 0x10, "HudEnemies");

// 0x242C20 — vtable +0x0C
static int Enemies_Show(HudEnemies* self, u32 params, u16 kind) {
    self->state |= 1;
    if (params == 0)
        return 1;
    EnemySlot* slot = nullptr;
    for (int i = 0; i < 6; i++) {
        if (self->slot[i].frames == 0) {
            slot = &self->slot[i];
        } else if (self->slot[i].params == params) {
            slot = &self->slot[i];
            break;
        }
    }
    if (slot == nullptr)
        return 1;
    slot->flags = 0;
    if (self->count < self->mode && self->mode != 1)
        slot->flags = 1;
    slot->kind = kind;
    if (self->count == 0 && self->mode != 0)
        slot->flags |= 2;
    slot->params = params;
    slot->wantX = (u16)((s16)self->count * 0x640);
    slot->x = slot->wantX;
    slot->frames = 2;
    self->count++;
    return 1;
}

// 0x242B10
static void Enemies_StepSlot(EnemySlot* slot) {
    if (slot->x < slot->wantX) {
        const int half = (slot->wantX - slot->x) / 2;
        slot->x = (u16)(slot->x + ((half & 0xFFFF) > 0xF0 ? 0xF0 : half));
    } else if (slot->wantX < slot->x) {
        const int half = (slot->x - slot->wantX) / 2;
        slot->x = (u16)(slot->x - ((half & 0xFFFF) > 0xF0 ? 0xF0 : half));
    }
    if (slot->frames != 0)
        slot->frames--;
}
// 0x242BB0 — vtable +0x10
static int Enemies_Update(HudEnemies* self) {
    self->count = 0;
    for (int i = 0; i < 6; i++)
        Enemies_StepSlot(&self->slot[i]);
    return (self->state & 4) == 0;
}

// 0x242620
static void Enemies_DrawSlot(HudEnemies*, SpriteSet* set, EnemySlot* slot) {
    GsDevice* dev = *gGsDevice;
    Font* font = *gFont;
    const u32 left = Screen_X(0, -1) & 0xFFFF, top = Screen_Y(0, -1) & 0xFFFF;
    if (slot->params == 0 || slot->frames == 0)
        return;
    const CharaParams* who = P<CharaParams>(slot->params);
    const u32 rgba = (slot->flags & 2) ? 0x80808080u : 0x40808080u;
    const s32 x = (s32)(((u32)slot->x + left + 0x1C0) & 0xFFFFFFF0u), y = (s32)top;
    BoxRect r{0x11, x + 0x10, y + 0x290, 0xFFF000u, -1, -1, 0x40000000u};
    SpriteSet_DrawEx(set, 0xC, &r);
    r = {0x11, x, y + 0x280, 0xFFF000u, -1, -1, rgba};
    SpriteSet_DrawEx(set, 0xC, &r);
    if (slot->flags & 1) {
        GsDevice_SetBlend(dev, 0x8000000048ull);
        r = {0x11, x - 0x70, y + 0x210, 0xFFF000u, -1, -1, 0x80804040u};
        SpriteSet_DrawEx(set, 10, &r);
        GsDevice_SetBlend(dev, 0x8000000044ull);
    }
    const s32 max = who->maxHp, hp = who->hp < max ? who->hp : max;
    const s32 width = hp * 0x520 / max;
    r = {1, x + 0x20, y + 0x3A0, 0xFFF000u, 0x520, -1, rgba};
    SpriteSet_DrawEx(set, 0xD, &r);
    if (width > 0) {
        r = {1, x + 0x20, y + 0x3A0, 0xFFF000u, width, -1, rgba};
        SpriteSet_DrawEx(set, (slot->kind & 1) == 0 ? 0xF : (slot->kind & 2) == 0 ? 0xE : 0xB, &r);
    }
    const u16* name = P<const u16>(who->_b0);
    Font_Measure(font, name, 1.0f);
    const FontExtent* extent = Font_GetExtent(font);
    const s32 tx = x + 0x2C0 - (s32)((u16)extent->w >> 1), ty = y + 0x350 - (s32)((u16)extent->h >> 1);
    Font_SetColor(font, 0x80000000u);
    Font_DrawStringPlain(font, name, tx + 0x10, ty + 0x10, 0xFFF000u);
    Font_DrawStringPlain(font, name, tx + 0x10, ty - 0x10, 0xFFF000u);
    Font_DrawStringPlain(font, name, tx - 0x10, ty + 0x10, 0xFFF000u);
    Font_DrawStringPlain(font, name, tx - 0x10, ty - 0x10, 0xFFF000u);
    Font_SetColor(font, 0x808A8A8Au);
    Font_DrawStringPlain(font, name, tx, ty, 0xFFF000u);
}
// 0x242A60 — vtable +0x14
static int Enemies_Draw(HudEnemies* self, SpriteSet* set) {
    if (self->state & 2)
        return 1;
    for (int i = 0; i < 6; i++)
        Enemies_DrawSlot(self, set, &self->slot[i]);
    Enemies_DrawSlot(self, set, &self->extra);
    return 1;
}
// 0x242D80
static HudEnemies* Enemies_Ctor(HudEnemies* self) {
    memset(self, 0, sizeof *self);
    self->kind = 0;                         // base constructor, 0x21E140
    self->vtable = 0x2EC850u;
    self->state = 1;
    return self;
}
// 0x242D20 — vtable +0x08
static HudEnemies* Enemies_Dtor(HudEnemies* self, s16 flag) {
    if (self != nullptr) {
        self->vtable = kWidgetVtable;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
GUEST_BIND(0x242D20, Enemies_Dtor);
GUEST_BIND(0x242C20, Enemies_Show);
GUEST_BIND(0x242BB0, Enemies_Update);
GUEST_BIND(0x242A60, Enemies_Draw);

// ── Manager ──────────────────────────────────────────────────────────────

// ── The game-over screen (0x242050–0x2424E0, vtable 0x2EC830, 0x14 bytes) ─
// Drawn alone while it exists: the picture fades to black over two seconds,
// then the brush strokes of the "game over" calligraphy appear one after
// another (sprites 1–5 of the HUD sheet).
struct HudDeath {
    s32 kind;           // +0x00
    u32 vtable;         // +0x04
    s32 timer;          // +0x08  counts down from half a second + two seconds + 45 frames
    s32 phase;          // +0x0C  1 running, 3 over
    s32 shown;          // +0x10  −1 not started, 0 fading, 1 black, 2 has been up long enough
};
static_assert(sizeof(HudDeath) == 0x14, "HudDeath");

static int Death_Show(HudDeath*, void*, int) { return 1; }             // 0x242470
static int Death_Update(HudDeath* self) {                               // 0x2423D0
    if (self->phase == 3)
        return 0;
    if (self->phase == 1 && self->shown != -1) {
        if (self->timer == 0)
            self->shown = 2;
        else
            self->timer--;
    }
    return 1;
}
// 0x242060.  NOT drawn: the two slanted, fading strokes the original draws
// first (sprite 0 through the four-corner draws 0x17AA80 / 0x17A710).
static int Death_Draw(HudDeath* self, SpriteSet* set) {
    const s32 t = self->timer;
    const s32 fps = *reinterpret_cast<const s32*>(guest::ptr(0x2EDA00u));
    if (t > fps * 2 + 0x2D)
        return 1;
    if (t >= 0x2D) {                                        // the world darkens
        GsDevice_ClearScreenColor(*gGsDevice, (u32)(0x80 - ((t - 0x2D) * 0x80) / (fps * 2)) << 24);
        return 1;
    }
    GsDevice_ClearScreenColor(*gGsDevice, 0x80000000u);
    if (t == 0x2B)
        self->shown = 1;
    const s32 x = (s32)(Screen_X(0, -1) & 0xFFFF), y = (s32)(Screen_Y(0, -1) & 0xFFFF);
    if (t < 0x18) SpriteSet_Draw(set, 1, x + 0x1310, y + 0xD50, 0xFFFFF0u, 0x80808080u);
    if (t < 0x10) SpriteSet_Draw(set, 2, x + 0x1360, y + 0xF50, 0xFFFFF0u, 0x80808080u);
    if (t < 0x0C) SpriteSet_Draw(set, 3, x + 0x1460, y + 0xFE0, 0xFFFFF0u, 0x80808080u);
    if (t < 0x0A) SpriteSet_Draw(set, 4, x + 0x1520, y + 0x1150, 0xFFFFF0u, 0x80808080u);
    SpriteSet_Draw(set, 5, x + 0x1320, y + 0xCF0, 0xFFFFF0u, 0x80808080u);
    return 1;
}
static HudDeath* Death_Dtor(HudDeath* self, s16 flag) {                 // 0x242480
    if (self != nullptr) {
        self->vtable = kWidgetVtable;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
GUEST_BIND(0x242480, Death_Dtor);
GUEST_BIND(0x242470, Death_Show);
GUEST_BIND(0x2423D0, Death_Update);
GUEST_BIND(0x242060, Death_Draw);
static HudDeath* Death_Ctor(HudDeath* self) {                           // 0x2424E0
    const s32 fps = *reinterpret_cast<const s32*>(guest::ptr(0x2EDA00u));
    self->kind = 0;
    self->vtable = 0x2EC830u;
    self->timer = fps / 2 + fps * 2 + 0x2D;
    self->phase = 1;
    self->shown = -1;
    return self;
}

// ── A shop's signboard (0x245A30–0x245FD0, vtable 0x2EC8A0, 0x1C bytes) ───
// Slides in from the left over ten frames; number 0 (the player's own house)
// then slides two paper doors apart over a name plate.
struct HudKanban {
    s32 kind;           // +0x00
    u32 vtable;         // +0x04
    s32 state;          // +0x08  1 = leaving
    s32 timer;          // +0x0C  frames of sliding left
    s32 doors;          // +0x10  frames of opening left
    s32 which;          // +0x14
    u32 sprites;        // +0x18  SpriteSet*: asset 0x73 + which
};
static_assert(sizeof(HudKanban) == 0x1C, "HudKanban");

// 0x17BED0 — a sprite of a set by its name (eight characters).
static int SpriteByName(SpriteSet* set, const char* name) {
    char key[8] = {};
    strncpy(key, name, 8);
    const u32* words = reinterpret_cast<const u32*>(set);
    const u8* records = P<u8>(words[3]);
    for (int i = 0; i < (int)words[4]; i++)
        if (memcmp(records + i * 0x20, key, 8) == 0)
            return i;
    return -1;
}
// 0x245EB0 — vtable +0x0C
static int Kanban_Show(HudKanban* self, int which, int) {
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    self->which = which;
    self->timer = 10;
    self->doors = 5;
    void* mem = MemAlloc(0x14);
    self->sprites = mem != nullptr ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    SpriteSet_Load(P<SpriteSet>(self->sprites), 0x73 + which, -1);    // table 0x2DA390
    return 1;
}
// 0x245E10 — vtable +0x10
static int Kanban_Update(HudKanban* self) {
    if (self->timer == 0 && self->doors != 0)
        self->doors--;
    if (self->timer != 0)
        self->timer--;
    return !(self->state == 1 && self->timer == 0);
}
// 0x245CB0 (with 0x245A30) — vtable +0x14
static int Kanban_Draw(HudKanban* self, SpriteSet* hudSprites) {
    const s32 slide = self->state == 1 ? 10 - self->timer : self->timer;
    const s32 x = (s32)(Screen_X(0, -1) & 0xFFFF) + slide * -0x1E0;
    const s32 y = (s32)(Screen_Y(0, -1) & 0xFFFF);
    SpriteSet* set = P<SpriteSet>(self->sprites);
    if (self->which >= 0 && self->which <= 9 && SpriteSet_Bind(set) != 0) {
        BoxRect r{0x11, x + 69 * 16, y + 26 * 16, 0xFFF000u, -1, -1, 0x80808080u};      // 0x2DA3C0
        if (self->which != 0) {
            SpriteSet_DrawEx(set, 0, &r);
        } else {
            SpriteSet_DrawEx(set, SpriteByName(set, "my_plate"), &r);
            const int door = SpriteByName(set, "my_syoji");
            r = BoxRect{0x11, x + 71 * 16 + (self->doors * (123 - 71) * 16) / 5, y + 30 * 16, 0xFFF000u, -1, -1,
                        0x80808080u};
            SpriteSet_DrawEx(set, door, &r);
            r.x = x + (233 * 16 - (self->doors * (233 - 181) * 16) / 5);
            SpriteSet_DrawEx(set, door, &r);
        }
    }
    SpriteSet_Bind(hudSprites);
    return 1;
}
// 0x245F50 — vtable +0x08
static HudKanban* Kanban_Dtor(HudKanban* self, s16 flag) {
    if (self == nullptr)
        return self;
    self->vtable = 0x2EC8A0u;
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    self->vtable = kWidgetVtable;
    if (flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x245F50, Kanban_Dtor);
GUEST_BIND(0x245EB0, Kanban_Show);
GUEST_BIND(0x245E10, Kanban_Update);
GUEST_BIND(0x245CB0, Kanban_Draw);
static HudKanban* Kanban_Ctor(HudKanban* self) {                    // 0x245FD0
    self->kind = 0;
    self->vtable = 0x2EC8A0u;
    self->state = 0;
    self->timer = 10;
    self->sprites = 0;
    return self;
}

// ── The player's money (0x282F90–0x283320, vtable 0x2ECF80, 0x18 bytes) ───
struct HudMoney {
    s32 kind;           // +0x00
    u32 vtable;         // +0x04
    s32 active;         // +0x08
    s32 x, y;           // +0x0C  1/16 pixel from the top left
    u32 sprites;        // +0x14  SpriteSet*: asset 0x6D
};
static_assert(sizeof(HudMoney) == 0x18, "HudMoney");

static int Money_Show(HudMoney* self, int x, int y) {               // 0x283270
    self->x = x << 4;
    self->y = y << 4;
    return 1;
}
static int Money_Update(HudMoney* self) { return self->active != 0; }  // 0x283260
// 0x282FA0 — a plate, "Money" and the amount.
static int Money_Draw(HudMoney* self, SpriteSet* hudSprites) {
    Font* font = *gFont;
    const s32 x = self->x + (s32)(Screen_X(0, -1) & 0xFFFF);
    const s32 y = self->y + (s32)(Screen_Y(0, -1) & 0xFFFF);
    const s16 money = *reinterpret_cast<const s16*>(static_cast<const u8*>(*gPlayerDataHud) + 0x106);
    SpriteSet* set = P<SpriteSet>(self->sprites);
    if (SpriteSet_Bind(set) != 0) {
        BoxRect r{1, x + 2 * 16, y + 2 * 16, 0xFFF000u, 141 << 4, 26 << 4, 0x2A080404u};       // 0x2DB2A0
        Box_Fill(&r);
        r = BoxRect{0x11, x, y, 0xFFF000u, -16, -16, 0x80808080u};
        SpriteSet_DrawEx(set, 0, &r);
        TextTable* table = TextTable_Get();
        Font_SetColor(font, 0x80BEC8D2u);
        Font_DrawCentered(font, TextTable_Find(table, 0x1B), x + 0x220, y + 0xD0, 0xFFF000u, 0.82f, 0.0f);
        u16 amount[0x100];
        WStr_Format(amount, 0x100, "%d", (int)money);
        WStr_Append(amount, TextTable_Find(table, 5), 0x100);
        Font_DrawCentered(font, amount, x + 0x690, y + 0xD0, 0xFFF000u, 0.82f, 0.0f);
    }
    SpriteSet_Bind(hudSprites);
    return 1;
}
static HudMoney* Money_Dtor(HudMoney* self, s16 flag) {             // 0x2832A0
    if (self == nullptr)
        return self;
    if (self->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(self->sprites), 1);
    self->sprites = 0;
    self->vtable = kWidgetVtable;
    if (flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x2832A0, Money_Dtor);
GUEST_BIND(0x283270, Money_Show);
GUEST_BIND(0x283260, Money_Update);
GUEST_BIND(0x282FA0, Money_Draw);
static HudMoney* Money_Ctor(HudMoney* self) {                       // 0x283320
    self->kind = 0;
    self->vtable = 0x2ECF80u;
    void* mem = MemAlloc(0x14);
    self->sprites = mem != nullptr ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    SpriteSet_Load(P<SpriteSet>(self->sprites), 0x6D, -1);
    self->active = 1;
    self->x = 0x1400;
    self->y = 0xE00;
    return self;
}

static inline u32  VtableOf(u32 widget) { return P<u32>(widget)[1]; }
static inline void Widget_Destroy(u32 w) { VirtualCall<void*, void*, int>(VtableOf(w), 0x08, P(w), 1); }
static inline void Widget_Show(u32 w, HudTextParams* p) {
    VirtualCall<int, void*, void*, int>(VtableOf(w), 0x0C, P(w), static_cast<void*>(p), 0);
}
static inline int  Widget_Update(u32 w) { return VirtualCall<int, void*>(VtableOf(w), 0x10, P(w)); }
static inline void Widget_Draw(u32 w, u32 sprites) {
    VirtualCall<int, void*, void*>(VtableOf(w), 0x14, P(w), P(sprites));
}

static u32 NewText() {
    void* mem = MemAlloc(sizeof(HudText));
    return mem != nullptr ? guest::addr(HudText_Ctor(static_cast<HudText*>(mem))) : 0;
}
static inline void Append(Hud* hud, u32 widget) {
    GuestList_Insert(&hud->widgets, GuestList_End(&hud->widgets), widget);
}

extern "C" {

// 0x21D8A0
static void Hud_ClearSlots(Hud* hud) {
    hud->_0c[0] = hud->_0c[1] = hud->_0c[2] = 0;
    hud->date = hud->prompt = hud->hint = 0;
    hud->_24 = hud->gauges = hud->overlay = hud->talkPrompt = hud->death = 0;
    hud->_38[0] = hud->_38[1] = hud->_38[2] = 0;
    hud->message = 0;
    hud->hintTimer = 0;
}

// 0x21DD70
void* Hud_Ctor(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    GuestList_Init(&hud->widgets);
    hud->_4c = 0;
    Hud_ClearSlots(hud);
    hud->sprites = 0;
    return hud;
}

// 0x21DD10
void Hud_Load(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->sprites == 0) {
        void* mem = MemAlloc(0x14);
        hud->sprites = mem != nullptr ? guest::addr(SpriteSet_Ctor(mem)) : 0;
        SpriteSet_Load(P<SpriteSet>(hud->sprites), 0x68, 0);
    }
}

// 0x21D8F0
void Hud_Reset(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    GuestListNode* end = GuestList_End(&hud->widgets);
    for (GuestListNode* n = GuestList_Begin(&hud->widgets); n != end; n = GuestList_Next(n))
        if (n->value != 0)
            Widget_Destroy(n->value);
    GuestList_Clear(&hud->widgets);
    if (hud->overlay != 0)
        Widget_Destroy(hud->overlay);
    hud->overlay = 0;
    Hud_ClearSlots(hud);
    if (hud->sprites != 0)
        SpriteSet_Dtor(P<SpriteSet>(hud->sprites), 1);
    hud->sprites = 0;
}

// 0x21D9E0
void Hud_Draw(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    const u32 sprites = hud->sprites;
    if (sprites == 0)
        return;
    if (SpriteSet_Bind(P<SpriteSet>(sprites)) != 0) {
        if (hud->death == 0) {
            GuestListNode* end = GuestList_End(&hud->widgets);
            for (GuestListNode* n = GuestList_Begin(&hud->widgets); n != end; n = GuestList_Next(n)) {
                // (Native: in a wider picture these may keep to its edges —
                // all but the eye-catch, which is a whole screen.)
                Native_HudAnchor(n->value != hud->message);
                Widget_Draw(n->value, sprites);
                Native_HudAnchor(0);
            }
        } else {
            Widget_Draw(hud->death, sprites);
        }
    }
    if (hud->overlay != 0)
        Widget_Draw(hud->overlay, sprites);
}

// 0x21DDB0 — only one line of text shows at a time: a one-off line beats the
// date, which beats the prompt, which beats the hint.  During effect 8 the
// prompt and the hint are hidden.
static void Hud_ArrangeText(Hud* hud) {
    const bool event = EventEffect();
    const HudText* other = nullptr;
    GuestListNode* end = GuestList_End(&hud->widgets);
    for (GuestListNode* n = GuestList_Begin(&hud->widgets); n != end; n = GuestList_Next(n)) {
        const u32 w = n->value;
        if (*P<s32>(w) == 1 && w != hud->prompt && w != hud->hint && w != hud->date) {
            other = P<HudText>(w);
            break;
        }
    }
    HudText* date = P<HudText>(hud->date);
    HudText* prompt = P<HudText>(hud->prompt);
    HudText* hint = P<HudText>(hud->hint);
    if (other != nullptr) {
        if (hud->date != 0)   HudText_FadeOut(date);
        if (hud->prompt != 0) HudText_FadeOut(prompt);
        if (hud->hint != 0)   HudText_FadeOut(hint);
    } else if (hud->date != 0) {
        HudText_FadeIn(date);
        if (hud->prompt != 0) HudText_FadeOut(prompt);
        if (hud->hint != 0)   HudText_FadeOut(hint);
    } else if (event) {
        if (hud->prompt != 0) HudText_FadeOut(prompt);
        if (hud->hint != 0)   HudText_FadeOut(hint);
    } else if (hud->prompt != 0) {
        HudText_FadeIn(prompt);
        if (hud->hint != 0)   HudText_FadeOut(hint);
    } else if (hud->hint != 0) {
        HudText_FadeIn(hint);
    }
}

// 0x21DAB0
void Hud_Update(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    sNoticeStack = 0;                   // 0x21ECB0
    if (hud->hintTimer != -1) {
        if (hud->hintTimer == 0) {
            if (hud->hint != 0) {
                HudText_Close(P<HudText>(hud->hint));
                hud->hint = 0;
            }
        } else {
            hud->hintTimer--;
        }
    }
    GuestListNode* end = GuestList_End(&hud->widgets);
    for (GuestListNode* n = GuestList_Begin(&hud->widgets); n != end;) {
        if (Widget_Update(n->value) == 0) {
            if (n->value != 0)
                Widget_Destroy(n->value);
            n = GuestList_Erase(&hud->widgets, n);
        } else {
            n = GuestList_Next(n);
        }
    }
    Hud_ArrangeText(hud);
    if (hud->overlay != 0 && Widget_Update(hud->overlay) == 0) {
        Widget_Destroy(hud->overlay);
        hud->overlay = 0;
    }
}

// 0x21D720
void Hud_ShowHint(void* self, const u16* text, int type) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->hint != 0 && hud->hintTimer == -1)
        return;
    hud->hintTimer = -1;
    HudTextParams params{type, -1, guest::addr(text)};
    if (hud->hint == 0) {
        hud->hint = NewText();
        Append(hud, hud->hint);
    }
    Widget_Show(hud->hint, &params);
}

// 0x21D7E0 — a hint that leaves by itself; a real hint replaces it.
void Hud_ShowAreaName(void* self, const u16* text, int frames, int type) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->hint != 0 && hud->hintTimer != -1)
        return;
    hud->hintTimer = frames;
    HudTextParams params{type, -1, guest::addr(text)};
    if (hud->hint == 0) {
        hud->hint = NewText();
        Append(hud, hud->hint);
    }
    Widget_Show(hud->hint, &params);
}

// 0x21D6E0
void Hud_HideHint(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->hint != 0) {
        HudText_Close(P<HudText>(hud->hint));
        hud->hint = 0;
    }
}

// 0x21D510 — frames −1: the standing date line (one at a time, closed with
// Hud_ClearChoice); otherwise a one-off line.
void Hud_ShowDate(void* self, const u16* text, int frames, int type) {
    Hud* hud = static_cast<Hud*>(self);
    HudTextParams params{type, frames, guest::addr(text)};
    if (frames == -1) {
        if (hud->date == 0) {
            hud->date = NewText();
            Widget_Show(hud->date, &params);
            Append(hud, hud->date);
        } else {
            Widget_Show(hud->date, &params);
        }
    } else {
        const u32 widget = NewText();
        Widget_Show(widget, &params);
        Append(hud, widget);
    }
}

// 0x21D4D0
void Hud_ClearChoice(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->date != 0) {
        HudText_Close(P<HudText>(hud->date));
        hud->date = 0;
    }
}

// 0x21D630
void Hud_ShowNotice(void* self, const u16* text, int frames, int type) {
    Hud* hud = static_cast<Hud*>(self);
    HudTextParams params{type, frames == -1 ? 0x1E : frames, guest::addr(text)};
    void* mem = MemAlloc(sizeof(HudNotice));
    const u32 widget = mem != nullptr ? guest::addr(HudNotice_Ctor(static_cast<HudNotice*>(mem))) : 0;
    Widget_Show(widget, &params);
    Append(hud, widget);
}

// 0x21CBC0 — an enemy's plate (see Pairs_UpdateHud); every frame.
void Hud_ShowEnemy(void* self, CharaParams* who, int kind) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->_38[0] == 0) {
        void* mem = MemAlloc(sizeof(HudEnemies));
        hud->_38[0] = mem != nullptr ? guest::addr(Enemies_Ctor(static_cast<HudEnemies*>(mem))) : 0;
        Append(hud, hud->_38[0]);
    }
    VirtualCall<int, void*, u32, u16>(VtableOf(hud->_38[0]), 0x0C, P(hud->_38[0]), guest::addr(who), (u16)kind);
}
// 0x21CFA0 — every frame in the free world: the prompt for what lies within
// the player's reach (`Hud+0x1C`): a thing's name (type 2; 4 = it will not
// fit in the bag), or a sword's (3; 5 = three are carried already).
// → 0 when it could not be taken.  `Hud+0x10` / `+0x14` remember which.
int Hud_UpdateDrops(void* self, void* drops, const void* matrix, void* swordDb) {
    Hud* hud = static_cast<Hud*>(self);
    u8* pd = static_cast<u8*>(*gPlayerDataHud);
    const s16 carried = *reinterpret_cast<const s16*>(pd + 0x178);
    const u32* found = P<u32>((u32)ItemDrops_fn242550(drops, matrix, swordDb));
    const auto closeAll = [hud] {
        if (hud->_0c[0] != 0) {                 // (a widget nothing makes in this release; 0x220C30)
            *P<s32>(hud->_0c[0] + 0x24) = 2;
            hud->_0c[0] = 0;
        }
        if (hud->prompt != 0) {
            HudText_Close(P<HudText>(hud->prompt));
            hud->prompt = 0;
        }
    };
    const auto show = [hud](s32 type, const u16* text) {
        HudTextParams params{type, -1, guest::addr(text)};
        if (hud->prompt == 0) {
            hud->prompt = NewText();
            Append(hud, hud->prompt);
        }
        Widget_Show(hud->prompt, &params);
    };
    if (found[0] == 0) {
        if (found[1] == 0) {
            closeAll();
            hud->_0c[2] = 0;
        } else if (hud->_0c[2] != found[1]) {   // a sword on the ground
            closeAll();
            hud->_0c[2] = found[1];
            show(carried < 3 ? 3 : 5, P<const u16>(*P<u32>(found[1] + 4)));
        }
        hud->_0c[1] = 0;
    } else {
        if (hud->_0c[1] != found[0]) {
            closeAll();
            hud->_0c[1] = found[0];
            const u8* object = P<u8>(found[0]);
            const s32 id = *reinterpret_cast<const s16*>(object + 0x68);
            s32 type = 2;
            if (ItemDrops_BagItem(id) != 0 && PlayerData_HasFreeItemSlot(static_cast<PlayerData*>(*gPlayerDataHud), 0) == 0 &&
                *reinterpret_cast<const s32*>(object + 0x6C) != 2)
                type = 4;
            show(type, ItemDrops_ObjectName(id));
        }
        hud->_0c[2] = 0;
    }
    if (found[0] == 0)
        return !(found[1] != 0 && carried > 2);
    return PlayerData_HasFreeItemSlot(static_cast<PlayerData*>(*gPlayerDataHud), 0) == 0;
}

// 0x21CB20 / 0x21CAF0 (→ 0x2425E0): the plate of someone on the player's
// side (the scripts' UI.FriendLife) — the widget's seventh slot; null hides it.
void Hud_SetFriend(void* self, CharaParams* who) {
    Hud* hud = static_cast<Hud*>(self);
    if (who != nullptr && hud->_38[0] == 0) {
        void* mem = MemAlloc(sizeof(HudEnemies));
        hud->_38[0] = mem != nullptr ? guest::addr(Enemies_Ctor(static_cast<HudEnemies*>(mem))) : 0;
        Append(hud, hud->_38[0]);
    }
    if (hud->_38[0] == 0)
        return;
    EnemySlot* slot = &P<HudEnemies>(hud->_38[0])->extra;
    slot->params = guest::addr(who);
    slot->flags = 2;
    slot->kind = 0;
    slot->x = slot->wantX = 8000;
    slot->frames = 2;
}

// 0x21CC60 / 0x242610
void Hud_EnemiesShown(void* self, int mode) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->_38[0] != 0)
        P<HudEnemies>(hud->_38[0])->mode = mode;
}

// 0x21CCD0 — the game-over screen: made on the first call (→ 0), afterwards
// its state: 1 once the picture has gone black, 2 when it has shown long
// enough.  0x21CC90 ends it.
int Hud_DeathState(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->death == 0) {
        HudDeath* death = static_cast<HudDeath*>(MemAlloc(sizeof(HudDeath)));
        hud->death = death != nullptr ? guest::addr(Death_Ctor(death)) : 0;
        Append(hud, hud->death);
        if (death != nullptr && death->shown == -1)         // 0x242440
            death->shown = 0;
        return 0;
    }
    return P<HudDeath>(hud->death)->shown;
}
void Hud_EndDeath(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->death != 0) {
        P<HudDeath>(hud->death)->phase = 3;                 // 0x242050: the list drops it
        hud->death = 0;
    }
}

// 0x21CA60 / 0x21CA20 — a shop's signboard (UI.ShowKanban / HideKanban).
void Hud_ShowKanban(void* self, int which) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->_38[1] == 0) {
        void* mem = MemAlloc(sizeof(HudKanban));
        hud->_38[1] = mem != nullptr ? guest::addr(Kanban_Ctor(static_cast<HudKanban*>(mem))) : 0;
        Append(hud, hud->_38[1]);
    }
    VirtualCall<int, void*, int, int>(VtableOf(hud->_38[1]), 0x0C, P(hud->_38[1]), which, 0);
}
void Hud_HideKanban(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->_38[1] != 0) {
        HudKanban* k = P<HudKanban>(hud->_38[1]);      // 0x245C90: slides away, then goes
        k->state = 1;
        k->timer = 10;
        hud->_38[1] = 0;
    }
}
// 0x21DC60 / 0x21DC20 — the player's money (UI.Syojikin).
void Hud_ShowMoney(void* self, int x, int y) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->_38[2] != 0)
        return;
    void* mem = MemAlloc(sizeof(HudMoney));
    const u32 widget = mem != nullptr ? guest::addr(Money_Ctor(static_cast<HudMoney*>(mem))) : 0;
    VirtualCall<int, void*, int, int>(VtableOf(widget), 0x0C, P(widget), x, y);
    hud->_38[2] = widget;
    Append(hud, widget);
}
void Hud_HideMoney(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->_38[2] != 0) {
        P<HudMoney>(hud->_38[2])->active = 0;           // 0x282F90
        hud->_38[2] = 0;
    }
}

// 0x21CED0 — the gauges, made on first use; has to be called every frame.
void Hud_SetGauges(void* self, const void* gauges) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->gauges == 0) {
        void* mem = MemAlloc(sizeof(HudGauges));
        hud->gauges = mem != nullptr ? guest::addr(Gauges_Ctor(static_cast<HudGauges*>(mem))) : 0;
        Append(hud, hud->gauges);
    }
    VirtualCall<int, void*, const void*, int>(VtableOf(hud->gauges), 0x0C, P(hud->gauges), gauges, 0);
}

// 0x21CE70 / 0x226C70 — "the day is over": the flash at the clock.
void Hud_ShowGameOver(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->gauges != 0)
        P<HudGauges>(hud->gauges)->flash = 0x2C;
}
// 0x21CEA0 / 0x226C80
int Hud_GameOverBusy(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    return hud->gauges != 0 && P<HudGauges>(hud->gauges)->flash != 0;
}

// 0x21CD60 — the name plate over someone the player can talk to.
void Hud_ShowTalkPrompt(void* self, const void* info) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->talkPrompt == 0) {
        HudTalkPrompt* prompt = static_cast<HudTalkPrompt*>(MemAlloc(sizeof(HudTalkPrompt)));
        if (prompt != nullptr) {
            prompt->kind = 0;               // 0x21EA80
            prompt->vtable = 0x2EC430u;
            prompt->frames = 1;
            prompt->onScreen = 0;
        }
        hud->talkPrompt = guest::addr(prompt);
        Append(hud, hud->talkPrompt);
    }
    VirtualCall<int, void*, const void*, int>(VtableOf(hud->talkPrompt), 0x0C, P(hud->talkPrompt), info, 0);
}

// 0x21DFA0 — flashes the prompt (or, without one, the first one-off line)
// before it leaves.  Non-zero if there was something to flash.
int Hud_FlashPrompt(void* self) {
    Hud* hud = static_cast<Hud*>(self);
    if (hud->prompt != 0) {
        HudText_Flash(P<HudText>(hud->prompt));
        hud->prompt = 0;
        return 1;
    }
    GuestListNode* end = GuestList_End(&hud->widgets);
    for (GuestListNode* n = GuestList_Begin(&hud->widgets); n != end; n = GuestList_Next(n)) {
        const u32 w = n->value;
        if (*P<s32>(w) == 1 && w != hud->hint && w != hud->date) {
            HudText_Flash(P<HudText>(w));
            return 1;
        }
    }
    return 0;
}

}  // extern "C"
