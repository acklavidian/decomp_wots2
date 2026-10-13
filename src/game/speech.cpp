// Speech balloons and the choice window — decompiled from 0x188250–0x18BB10
// and 0x1910A0–0x191B10.  See include/game/speech.h.

#include "game/speech.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "native/guest.h"
#include "native/hle.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    int   Rand_Next();                                  // 0x1200F0
}

static_assert(offsetof(BalloonShape, scale) == 0x20 && sizeof(BalloonShape) == 0x28, "BalloonShape");
static_assert(offsetof(SpeechWidget, anchorX) == 0x24 && offsetof(SpeechWidget, text) == 0x40, "SpeechWidget");
static_assert(sizeof(SpeechWidget) == 0x58 && sizeof(SpeechBalloon) == 0x68 && sizeof(ChoiceWindow) == 0x5C, "SpeechWidget");

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const GuestPtrVar<Font>     gFont{0x2EE67Cu};
static u32& sCurrentList = *guest::ptr<u32>(0x2EE604u);         // for keeping balloons off each other
static s32& sScreenW = *guest::ptr<s32>(0x2EE4C0u);
static s32& sScreenH = *guest::ptr<s32>(0x2EE4C4u);
// Where a balloon may sit around the speaker: directions (degrees) and the
// order they are tried in, for a speaker left / right of the listener.
static const float* const kAngles = guest::ptr<const float>(0x2D80C0u);     // [9]
static const s32* const   kOrder  = guest::ptr<const s32>(0x2D80F0u);       // [2][11]; slot 0 = last time's

enum : u32 {
    kShapeVtable = 0x2EB1A0u, kRoughVtable = 0x2EB1C0u, kCloudVtable = 0x2EB1E0u,
    kWidgetVtable = 0x2EB140u, kBalloonVtable = 0x2EB160u, kChoiceVtable = 0x2EB270u, kRemarkVtable = 0x2ECAE0u,
};
enum : u32 { kInk = 0x80576365u, kPaper = 0x809FAEB3u, kChoiceInk = 0x80111314u, kChoicePaper = 0x803F494Du };

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
struct Pt { s32 x, y; };

// ══ Balloon outlines ══════════════════════════════════════════════════════

// 0x18A8B0 is vector<Pt>::insert; only ever used to append.
static void Push(BalloonShape* s, Pt p) { GuestVector_PushBack(&s->points, &p, sizeof p); }
static inline Pt* Points(BalloonShape* s) { return P<Pt>(s->points.data); }

// Pushes every second point outwards by a random amount, which is what makes
// the outline look drawn by hand; then closes the fan.
static void Roughen(BalloonShape* s, float length, int base, int spread, float amount) {
    Pt* pts = Points(s);
    const u32 n = s->points.count;
    for (u32 i = 1; i + 2 < n; i += 2) {
        const float dx = (float)(pts[i].x - pts[i + 2].x), dy = (float)(pts[i].y - pts[i + 2].y);
        const float d = sqrtf(dx * dx + dy * dy);
        const int nx = (int)(dx * length / d), ny = (int)(dy * length / d);
        pts[i + 1].x = (int)((float)pts[i + 1].x - (float)ny * (float)(Rand_Next() % spread + base) * amount);
        pts[i + 1].y = (int)((float)pts[i + 1].y + (float)nx * (float)(Rand_Next() % spread + base) * amount);
    }
    Push(s, Points(s)[1]);
}

// 0x18A310 / 0x18AEE0 — a rectangle a little larger than the text, its sides
// cut into uneven pieces.  (The two differ in how the pieces carry over a
// corner and in the roughness.)
static void BuildRect(BalloonShape* s, int w, int h, bool carry, float length, int base, int spread, float amount) {
    s->points.count = 0;
    const int hw = (w >> 1) + 0x2C, hh = (h >> 1) + 0x2C;
    const Pt corner[5] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}, {-hw, -hh}};
    Push(s, Pt{0, 0});
    float t = 0.0f;
    int r = 0;
    for (int side = 0; side < 4; side++) {
        const int dx = corner[side + 1].x - corner[side].x, dy = corner[side + 1].y - corner[side].y;
        for (; t < 1.0f; t += (128.0f / sqrtf((float)(dx * dx + dy * dy))) * (float)(r % 0x50 + 0x50) * 0.02f) {
            Push(s, Pt{corner[side].x + (int)((float)dx * t), corner[side].y + (int)((float)dy * t)});
            r = Rand_Next();
        }
        t = carry ? t - 1.0f : 0.0f;
    }
    if (carry)
        Push(s, corner[4]);
    Roughen(s, length, base, spread, amount);
}

static int Shape_Build(BalloonShape* s, int w, int h) {         // 0x18A310
    BuildRect(s, w, h, true, 128.0f, 0x28, 0x50, 0.009f);
    return 1;
}
static int RoughShape_Build(BalloonShape* s, int w, int h) {    // 0x18AEE0
    BuildRect(s, w, h, false, 96.0f, 0x28, 0x28, 0.01f);
    return 1;
}

// 0x181C60 — a point on the smooth curve through four corners (p[1] → p[2]).
static Pt Curve(float t, const Pt* p) {
    const float t2 = t * t, t3 = t2 * t;
    const float a = t + (t3 - t2 * 2.0f), b = t3 * -2.0f + t2 * 3.0f, c = (t3 * 2.0f - t2 * 3.0f) + 1.0f, d = t3 - t2;
    auto axis = [&](int p0, int p1, int p2, int p3) {
        const int in = (int)((float)((p1 - p0) + (p2 - p1)) * 0.5f);
        const int out = (int)((float)((p3 - p2) + (p2 - p1)) * 0.5f);
        return (int)((float)p2 * b) + (int)((float)in * a) + (int)((float)p1 * c) + (int)((float)out * d);
    };
    return Pt{axis(p[0].x, p[1].x, p[2].x, p[3].x), axis(p[0].y, p[1].y, p[2].y, p[3].y)};
}

// 0x18B5B0 — a cloud: the curve through the text's corners, bulging.
static int CloudShape_Build(BalloonShape* s, int w, int h) {
    s->points.count = 0;
    Push(s, Pt{0, 0});
    const int hw = w >> 1, hh = h >> 1;
    float t = 0.0f;
    for (int turn = 4; turn > 0; turn--) {
        Pt p[4];
        p[turn & 3] = Pt{-hw, -hh};
        p[(turn + 1) & 3] = Pt{hw, -hh};
        p[(turn + 2) & 3] = Pt{hw, hh};
        p[(turn + 3) & 3] = Pt{-hw, hh};
        const int dx = p[2].x - p[1].x, dy = p[2].y - p[1].y;
        float step = 256.0f / sqrtf((float)(dx * dx + dy * dy));
        if (step > 0.12f) {
            step *= 0.6f;
            if (step > 0.12f)
                step *= 0.8f;
            if (step > 0.12f)
                step *= 0.9f;
        }
        int r = 0;
        for (; t < 1.0f; t += step * (float)(r % 0x14 + 0x50) * 0.01f) {
            Push(s, Curve(t, p));
            r = Rand_Next();
        }
        t -= 1.0f;
    }
    Roughen(s, 256.0f, 100, 0x32, 0.005f);
    return 1;
}

// A triangle fan of the outline at `pos` {x, y, z}.
static int DrawFan(BalloonShape* s, const s32* pos, u32 rgba, int grow, int zOffset, u32 limit) {
    struct Vertex { u32 rgba, q; u16 x, y; u32 z; };
    struct { u64 prim; Vertex v[100]; } packet;
    packet.prim = 0x1CD;
    const Pt* pts = Points(s);
    u32 n = 0;
    for (; n < s->points.count && n < limit; n++) {
        int x = (int)((float)pts[n].x * s->scale), y = (int)((float)pts[n].y * s->scale);
        x += x < 0 ? -grow : grow;
        y += y < 0 ? -grow : grow;
        packet.v[n] = {rgba, 0, (u16)(x + pos[0]), (u16)(y + pos[1]), (u32)(pos[2] + zOffset)};
    }
    GsDevice_DrawPrim(*gGsDevice, &packet, (int)n);
    return 1;
}
static int Shape_Draw(BalloonShape* s, const s32* pos, u32 rgba)      { return DrawFan(s, pos, rgba, 0, -1, 100); }    // 0x18A1B0
static int RoughShape_Draw(BalloonShape* s, const s32* pos, u32 rgba) { return DrawFan(s, pos, rgba, 0, 0, 100); }     // 0x18AD80
static int CloudShape_Draw(BalloonShape* s, const s32* pos, u32 rgba) { return DrawFan(s, pos, rgba, 0x10, 0, 100); }  // 0x18B450

static BalloonShape* Shape_Dtor(BalloonShape* s, s16 flag) {    // 0x18A800 (and 0x18B3B0, 0x18BAB0)
    if (s == nullptr)
        return s;
    s->points.count = 0;
    if (s->points.data != 0)
        MemFree(P(s->points.data));
    if (flag > 0)
        MemFree(s);
    return s;
}
static BalloonShape* NewShape(u32 vtable) {                     // 0x18A880 + 0x18B410 / 0x18BB10
    BalloonShape* s = static_cast<BalloonShape*>(MemAlloc(0x30));
    if (s == nullptr)
        return s;
    s->points = GuestVector{};
    s->scale = 1.0f;
    s->vtable = vtable;
    return s;
}
GUEST_BIND(0x18A800, Shape_Dtor);
GUEST_BIND(0x18A310, Shape_Build);
GUEST_BIND(0x18A1B0, Shape_Draw);
static BalloonShape* RoughShape_Dtor(BalloonShape* s, s16 flag) { return Shape_Dtor(s, flag); }
GUEST_BIND(0x18B3B0, RoughShape_Dtor);
GUEST_BIND(0x18AEE0, RoughShape_Build);
GUEST_BIND(0x18AD80, RoughShape_Draw);
static BalloonShape* CloudShape_Dtor(BalloonShape* s, s16 flag) { return Shape_Dtor(s, flag); }
GUEST_BIND(0x18BAB0, CloudShape_Dtor);
GUEST_BIND(0x18B5B0, CloudShape_Build);
GUEST_BIND(0x18B450, CloudShape_Draw);

static inline void ShapeBuild(u32 shape, int w, int h) {
    VirtualCall<int, BalloonShape*, int, int>(P<BalloonShape>(shape)->vtable, 0x0C, P<BalloonShape>(shape), w, h);
}
static inline void ShapeDraw(u32 shape, const s32* pos, u32 rgba) {
    VirtualCall<int, BalloonShape*, const s32*, u32>(P<BalloonShape>(shape)->vtable, 0x10, P<BalloonShape>(shape), pos, rgba);
}
// The outline, four times in ink around where it goes, then in paper on top.
static void DrawOutlined(u32 shape, int x, int y, u32 z, u32 ink, u32 paper) {
    s32 pos[4] = {x + 0x20, y, (s32)z, 0};
    ShapeDraw(shape, pos, ink);
    pos[0] -= 0x40;
    ShapeDraw(shape, pos, ink);
    pos[0] += 0x20;
    pos[1] += 0x20;
    ShapeDraw(shape, pos, ink);
    pos[1] -= 0x40;
    ShapeDraw(shape, pos, ink);
    pos[1] += 0x20;
    ShapeDraw(shape, pos, paper);
}

// ══ The widget base (0x188250–0x188480) ═══════════════════════════════════

static const u16* TextOf(SpeechWidget* w) { return P<const u16>(P<WStringRep>(w->text.rep)->data); }

static SpeechWidget* Widget_Ctor(SpeechWidget* w) {             // 0x188480
    w->vtable = kWidgetVtable;
    w->textVtable = 0x2EAF30u;
    WString_Ctor(&w->text);
    WString_Reserve(&w->text, 0);
    w->speaker = 0;
    w->listener = 0;
    w->shape = 0;
    w->appear = 0;
    w->closing = 0;
    w->kind = 0;
    w->scale = 1.0f;
    return w;
}
static SpeechWidget* Widget_Dtor(SpeechWidget* w, s16 flag) {   // 0x188360
    if (w == nullptr)
        return w;
    w->vtable = kWidgetVtable;
    if (w->shape != 0)
        VirtualCall<void*, BalloonShape*, int>(P<BalloonShape>(w->shape)->vtable, 0x08, P<BalloonShape>(w->shape), 1);
    w->shape = 0;
    WString_Release(&w->text);
    if (flag > 0)
        MemFree(w);
    return w;
}
static int Widget_Show(SpeechWidget* w, const SpeechParams* p) {    // 0x188340
    w->speaker = p->speaker;
    w->listener = p->listener;
    w->_30 = p->frames;
    return 1;
}
static int Widget_Update(SpeechWidget*) { return 1; }           // 0x188330
static int Widget_Draw(SpeechWidget*) { return 1; }             // 0x188320
GUEST_BIND(0x188360, Widget_Dtor);
GUEST_BIND(0x188340, Widget_Show);
GUEST_BIND(0x188330, Widget_Update);
GUEST_BIND(0x188320, Widget_Draw);

void SpeechWidget_Close(SpeechWidget* w) { w->closing = 1; }    // 0x188310

// The speaker's head / chest: a point of its skeleton (CharaParams+0xB8 →
// model object → +4 → +0x5D0 / +0x490).
static const float* BodyPoint(u32 params, u32 offset) {
    if (params == 0)
        return nullptr;
    const u32 model = P<CharaParams>(params)->link[1];
    if (model == 0 || P<u32>(model)[1] == 0)
        return nullptr;
    return P<float>(P<u32>(model)[1] + offset);
}

// ── 0x188A50 ──────────────────────────────────────────────────────────────
// Where a character's head is on screen, kept inside it.
static Pt HeadOnScreen(SpeechBalloon* self, u32 params) {
    self->onScreen = 0;
    Pt out{0, 0};
    const float* head = BodyPoint(params, 0x5D0);
    if (head == nullptr)
        return out;
    const float point[4] = {head[0], head[1] + 0.06f, head[2], 1.0f};
    const s32 left = (s32)(Screen_X(0, -1) & 0xFFFF), top = (s32)(Screen_Y(0, -1) & 0xFFFF);
    const s32 right = (s32)(Screen_X(0, 1) & 0xFFFF), bottom = (s32)(Screen_Y(0, 1) & 0xFFFF);
    s32 xy[4] = {};
    self->onScreen = GsDevice_Project(*gGsDevice, xy, point) != 0;
    out.x = xy[0] < left ? left : xy[0] > right ? right : xy[0];
    out.y = xy[1] < top ? top : xy[1] > bottom ? bottom : xy[1];
    return out;
}

// ── 0x1888E0 ──────────────────────────────────────────────────────────────
// The screen rectangle a head covers (0.18 to each side of it, as the camera
// sees it), which balloons keep clear of.  The original turns the two corner
// offsets by the inverse of the view matrix; the native camera does that part.
static int HeadRect(u32 params, s32* rect) {
    const float* head = BodyPoint(params, 0x5D0);
    if (head == nullptr)
        return 0;
    const float point[4] = {head[0], head[1] + 0.06f, head[2], 1.0f};
    return hle_project_extent(point, 0.18f, rect);
}

// ── 0x188500 ──────────────────────────────────────────────────────────────
// Would a balloon centred here cover another one?
static bool Overlaps(SpeechList* list, SpeechWidget* self, Pt at) {
    for (GuestListNode* n = GuestList_Begin(&list->widgets); n != GuestList_End(&list->widgets); n = GuestList_Next(n)) {
        SpeechWidget* o = P<SpeechWidget>(n->value);
        if (o == self)
            continue;
        const int dx = abs(o->x - at.x), dy = abs(o->y - at.y);
        if (dx < o->halfW || dy < self->halfH)
            return true;
    }
    return false;
}

// ── 0x188BE0 ──────────────────────────────────────────────────────────────
// Finds the balloon a place: around the speaker's head at `offset`, trying
// the directions in order (last time's first), on screen, clear of the
// listener's head and of other balloons; failing all, under the head.
static Pt Place(SpeechBalloon* self, Pt offset, const s32* avoid) {
    SpeechWidget* w = &self->w;
    const s32 marginX = ((sScreenW - 0x10) / 10) * 8, marginY = ((sScreenH - 0x10) / 10) * 8;
    const s32 left = (s32)(Screen_X(0, -1) & 0xFFFF) + marginX, right = (s32)(Screen_X(0, 1) & 0xFFFF) - marginX;
    const s32 top = (s32)(Screen_Y(0, -1) & 0xFFFF) + marginY, bottom = (s32)(Screen_Y(0, 1) & 0xFFFF) - marginY;
    Pt head = HeadOnScreen(self, w->speaker);
    const Pt other = HeadOnScreen(self, w->listener);
    s32 order[11];
    memcpy(order, kOrder + (head.x < other.x ? 11 : 0), sizeof order);
    order[0] = self->direction;
    const s32 hw = w->halfW, hh = w->halfH;
    const s32 avoidX = (avoid[0] + avoid[2]) / 2, avoidY = (avoid[1] + avoid[3]) / 2;
    const s32 avoidW = (avoid[2] - avoid[0]) / 2, avoidH = (avoid[3] - avoid[1]) / 2;
    for (int i = 0; i < 11; i++) {
        const s32 dir = order[i];
        if (dir == -1)
            continue;
        const float a = kAngles[dir] * 3.1415927f / 180.0f;
        const s32 ax = (int)((float)offset.x * cosf(a) - (float)offset.y * sinf(a)) + head.x;
        const s32 ay = (int)((float)offset.x * sinf(a) + (float)offset.y * cosf(a)) + head.y;
        w->anchorX = ax;
        w->anchorY = ay;
        Pt at{ax, ay};
        switch (dir) {
        case 0: case 1: at = {ax + hw, ay - hh}; break;
        case 2: case 3: at = {ax + hw, ay + hh}; break;
        case 4: case 5: at = {ax - hw, ay + hh}; break;
        case 6: case 7: at = {ax - hw, ay - hh}; break;
        case 8:         at = {ax - hw, ay + hh}; break;
        case 9:         at = {ax + hw, ay + hh}; break;
        }
        if (!(left < at.x - hw && at.x + hw < right && top < at.y - hh && at.y + hh < bottom))
            continue;
        if (abs(at.x - avoidX) < hw + avoidW && abs(at.y - avoidY) < hh + avoidH)
            continue;
        if (sCurrentList != 0 && Overlaps(P<SpeechList>(sCurrentList), w, at))
            continue;
        self->direction = dir;
        return at;
    }
    head.y += (avoid[3] - avoid[1]) / 2;
    if (head.x - hw < left)   head.x = left + hw;
    if (right < head.x + hw)  head.x = right - hw;
    if (head.y - hh < top)    head.y = top + hh;
    if (bottom < head.y + hh) head.y = bottom - hh;
    w->anchorX = head.x;
    w->anchorY = head.y;
    return head;
}

// ══ The speech balloon (0x189370–0x189E90) ════════════════════════════════

static SpeechBalloon* Balloon_Dtor(SpeechBalloon* self, s16 flag) {     // 0x189E30
    if (self == nullptr)
        return self;
    self->w.vtable = kBalloonVtable;
    Widget_Dtor(&self->w, 0);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x189A70 ──────────────────────────────────────────────────────────────
static int Balloon_Show(SpeechBalloon* self, const SpeechParams* p) {
    SpeechWidget* w = &self->w;
    if (w->shape != 0) {
        VirtualCall<void*, BalloonShape*, int>(P<BalloonShape>(w->shape)->vtable, 0x08, P<BalloonShape>(w->shape), 1);
        w->shape = 0;
    }
    if (p->style == 1)
        w->shape = guest::addr(NewShape(kCloudVtable));
    else if (p->style == 0)
        w->shape = guest::addr(NewShape(kRoughVtable));
    const u16* text = P<const u16>(p->text);
    WString_Assign(&w->text, text, text + WStr_Length(text));
    w->speaker = p->speaker;
    w->listener = p->listener;
    if (w->shape != 0) {
        Font_Measure(*gFont, TextOf(w), w->scale);
        const FontExtent* extent = Font_GetExtent(*gFont);
        w->halfW = extent->w >> 1;
        w->halfH = extent->h >> 1;
        ShapeBuild(w->shape, extent->w, extent->h);
    }
    self->frames = p->frames;
    s32 own[4] = {}, theirs[4] = {};
    HeadRect(w->speaker, own);
    HeadRect(w->listener, theirs);
    // The tail starts about a head away from the head, further for a shout.
    Pt offset{0, -(((own[2] - own[0]) + (own[3] - own[1])) / 2)};
    if (p->style == 1)
        offset.y = (int)((float)offset.y * 1.414f);
    self->style = p->style;
    self->direction = p->direction;
    const Pt at = Place(self, offset, theirs);
    w->x = at.x;
    w->y = at.y;
    return 1;
}

// ── 0x189A00 ──────────────────────────────────────────────────────────────
static int Balloon_Update(SpeechBalloon* self) {
    if (self->w.appear < 3)
        self->w.appear++;
    if (self->w.closing == 1)
        return 0;
    if (self->frames != -1) {
        if (self->frames == 0)
            return 0;
        self->frames--;
    }
    return 1;
}

// 0x189370 — the tail: two thin triangles from around `base` to `tip`.
static void DrawTail(SpeechWidget* w, Pt tip, Pt base, u32 rgba) {
    struct Vertex { u32 rgba, q; u16 x, y; u32 z; };
    struct { u64 prim; Vertex v[3]; } packet;
    packet.prim = 0x4C;
    packet.v[2] = {rgba, 0, (u16)tip.x, (u16)tip.y, w->z};
    packet.v[0] = {rgba, 0, (u16)(base.x + 0x40), (u16)base.y, w->z};
    packet.v[1] = {rgba, 0, (u16)(base.x - 0x40), (u16)base.y, w->z};
    GsDevice_DrawPrim(*gGsDevice, &packet, 3);
    packet.v[0] = {rgba, 0, (u16)base.x, (u16)(base.y + 0x40), w->z};
    packet.v[1] = {rgba, 0, (u16)base.x, (u16)(base.y - 0x40), w->z};
    GsDevice_DrawPrim(*gGsDevice, &packet, 3);
}

// ── 0x1894E0 ──────────────────────────────────────────────────────────────
static int Balloon_Draw(SpeechBalloon* self) {
    SpeechWidget* w = &self->w;
    const float grown = (float)w->appear / 3.0f;
    if (grown == 0.0f)
        return 1;
    // It grows out of the end of its tail.
    const Pt anchor{w->anchorX, w->anchorY};
    const Pt centre{anchor.x + (int)((float)(w->x - anchor.x) * grown), anchor.y + (int)((float)(w->y - anchor.y) * grown)};
    const Pt head = HeadOnScreen(self, w->speaker);
    const Pt tip{head.x + (int)((float)(anchor.x - head.x) * 0.5f), head.y + (int)((float)(anchor.y - head.y) * 0.5f)};
    if (self->onScreen != 0) {
        DrawTail(w, Pt{tip.x + 0x20, tip.y}, Pt{anchor.x - 0x20, anchor.y}, kInk);
        DrawTail(w, Pt{tip.x - 0x20, tip.y}, Pt{anchor.x + 0x20, anchor.y}, kInk);
        DrawTail(w, Pt{tip.x, tip.y + 0x20}, Pt{anchor.x, anchor.y - 0x20}, kInk);
        DrawTail(w, Pt{tip.x, tip.y - 0x20}, Pt{anchor.x, anchor.y + 0x20}, kInk);
    }
    if (w->shape != 0) {
        P<BalloonShape>(w->shape)->scale = grown;
        DrawOutlined(w->shape, centre.x, centre.y, w->z, kInk, kPaper);
    }
    if (self->onScreen != 0)
        DrawTail(w, tip, anchor, kPaper);
    if (w->appear == 3) {
        Font_SetColor(*gFont, 0x802E3539u);
        Font_DrawString(*gFont, TextOf(w), centre.x - w->halfW, centre.y - w->halfH, w->z, w->scale, 0.0f);
    }
    return 1;
}
GUEST_BIND(0x189E30, Balloon_Dtor);
GUEST_BIND(0x189A70, Balloon_Show);
GUEST_BIND(0x189A00, Balloon_Update);
GUEST_BIND(0x1894E0, Balloon_Draw);

// ── 0x189E90 ──────────────────────────────────────────────────────────────
SpeechBalloon* SpeechBalloon_Ctor(void* mem) {
    SpeechBalloon* self = static_cast<SpeechBalloon*>(mem);
    Widget_Ctor(&self->w);
    self->w.vtable = kBalloonVtable;
    self->w.kind = 1;
    self->w.shape = 0;
    self->direction = -1;
    self->onScreen = 1;
    self->w.x = (s32)(Screen_X(0, 0) & 0xFFFF);
    self->w.y = (s32)(Screen_Y(0, 0) & 0xFFFF);
    return self;
}

// ── 0x256FB0 ──────────────────────────────────────────────────────────────
// What a character says to nobody in particular.  It is shown like any
// balloon (0x256D60 is the plain balloon's show over again, and 0x256C00 its
// draw), but it **follows its speaker**: every frame it is put by the head
// again, and it is gone as soon as the speaker cannot be seen — something
// between it and the player, or its head off the screen.
extern "C" int Collision_CanSee(int idA, int idB, int kind);       // 0x175600
static SpeechBalloon* Remark_Dtor(SpeechBalloon* self, s16 flag) { return Balloon_Dtor(self, flag); }   // 0x256770
static int Remark_Show(SpeechBalloon* self, const SpeechParams* p) { return Balloon_Show(self, p); }    // 0x256D60
// 0x2567D0: by the head in the direction it had (else the first of the
// order; tables 0x2DA610 / 0x2DA640, the same numbers as the balloon's) —
// no looking for room on the screen or among the other balloons.
static Pt Remark_Place(SpeechBalloon* self, Pt offset) {
    SpeechWidget* w = &self->w;
    const float* angles = P<const float>(0x2DA610u);
    const Pt head = HeadOnScreen(self, w->speaker);
    const Pt other = HeadOnScreen(self, w->listener);
    s32 order[11];
    memcpy(order, P<const s32>(0x2DA640u) + (head.x < other.x ? 11 : 0), sizeof order);
    order[0] = self->direction;
    for (int i = 0; i < 11; i++) {
        const s32 dir = order[i];
        if (dir == -1)
            continue;
        const float a = angles[dir] * 3.1415927f / 180.0f;
        const s32 ax = (int)((float)offset.x * cosf(a) - (float)offset.y * sinf(a)) + head.x;
        const s32 ay = (int)((float)offset.x * sinf(a) + (float)offset.y * cosf(a)) + head.y;
        w->anchorX = ax;
        w->anchorY = ay;
        const s32 hw = w->halfW, hh = w->halfH;
        Pt at{ax, ay};
        switch (dir) {
        case 0: case 1: at = {ax + hw, ay - hh}; break;
        case 2: case 3: at = {ax + hw, ay + hh}; break;
        case 4: case 5: at = {ax - hw, ay + hh}; break;
        case 6: case 7: at = {ax - hw, ay - hh}; break;
        case 8:         at = {ax - hw, ay + hh}; break;
        case 9:         at = {ax + hw, ay + hh}; break;
        }
        self->direction = dir;
        return at;
    }
    w->anchorX = head.x;
    w->anchorY = head.y;
    return head;
}
static int Remark_Update(SpeechBalloon* self) {                    // 0x256C10
    SpeechWidget* w = &self->w;
    int seen = 1;
    if (w->speaker != 0)
        seen = Collision_CanSee(P<CharaParams>(w->speaker)->id, 0, 2);
    s32 own[4] = {}, theirs[4] = {};
    HeadRect(w->listener, theirs);
    if (seen == 0 || HeadRect(w->speaker, own) == 0)
        return 0;
    Pt offset{0, -(((own[2] - own[0]) + (own[3] - own[1])) / 2)};
    if (self->style == 1)
        offset.y = (int)((float)offset.y * 1.414f);
    const Pt at = Remark_Place(self, offset);
    w->x = at.x;
    w->y = at.y;
    return Balloon_Update(self);
}
static int Remark_Draw(SpeechBalloon* self) { return Balloon_Draw(self); }     // 0x256C00
GUEST_BIND(0x256770, Remark_Dtor);
GUEST_BIND(0x256D60, Remark_Show);
GUEST_BIND(0x256C10, Remark_Update);
GUEST_BIND(0x256C00, Remark_Draw);

SpeechBalloon* SpeechRemark_Ctor(void* mem) {
    SpeechBalloon* self = SpeechBalloon_Ctor(mem);
    self->w.vtable = kRemarkVtable;
    self->w.kind = 3;
    return self;
}

// ══ A balloon where the script puts it (0x23A210–0x23A340; UI.SetSerif2) ═══
static const u32 kPlacedVtable = 0x2EC730u;

static PlacedBalloon* Placed_Dtor(PlacedBalloon* self, s16 flag) {      // 0x23A2E0
    if (self == nullptr)
        return self;
    self->b.w.vtable = kPlacedVtable;
    Balloon_Dtor(&self->b, 0);
    if (flag > 0)
        MemFree(self);
    return self;
}
static int Placed_Show(PlacedBalloon* self, const SpeechParams* p) {    // 0x23A260
    Balloon_Show(&self->b, p);
    if (p->flags & 1) {
        self->x = p->x * 16 + (s32)(Screen_X(0, -1) & 0xFFFF);
        self->y = p->y * 16 + (s32)(Screen_Y(0, -1) & 0xFFFF);
    }
    return 1;
}
static int Placed_Update(PlacedBalloon* self) {                         // 0x23A250
    self->b.w.z = 0xFFF001u;
    return Balloon_Update(&self->b);
}
static int Placed_Draw(PlacedBalloon* self) {                           // 0x23A210
    SpeechWidget* w = &self->b.w;
    w->x = self->x;
    w->y = self->y;
    w->anchorX = w->x;
    w->anchorY = w->y;
    Balloon_Draw(&self->b);
    return 1;
}
GUEST_BIND(0x23A2E0, Placed_Dtor);
GUEST_BIND(0x23A260, Placed_Show);
GUEST_BIND(0x23A250, Placed_Update);
GUEST_BIND(0x23A210, Placed_Draw);

PlacedBalloon* SpeechPlaced_Ctor(void* mem) {                           // 0x23A340
    PlacedBalloon* self = static_cast<PlacedBalloon*>(mem);
    SpeechBalloon_Ctor(&self->b);
    self->b.w.vtable = kPlacedVtable;
    self->b.w.kind = 4;
    self->x = (s32)(Screen_X(0xA00, 0) & 0xFFFF);
    self->y = (s32)(Screen_Y(0, 0) & 0xFFFF);
    return self;
}

// ══ The choice window (0x1914F0–0x191B10) ═════════════════════════════════

static ChoiceWindow* Choice_Dtor(ChoiceWindow* self, s16 flag) {    // 0x191A30
    if (self == nullptr)
        return self;
    self->w.vtable = kChoiceVtable;
    Widget_Dtor(&self->w, 0);
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x191830 ──────────────────────────────────────────────────────────────
static int Choice_Show(ChoiceWindow* self, const SpeechParams* p) {
    SpeechWidget* w = &self->w;
    w->shape = guest::addr(NewShape(kRoughVtable));
    const u16* text = P<const u16>(p->text);
    WString_Assign(&w->text, text, text + WStr_Length(text));
    w->speaker = p->speaker;
    w->listener = p->listener;
    if (w->shape != 0) {
        Font_Measure(*gFont, TextOf(w), w->scale);
        const FontExtent* extent = Font_GetExtent(*gFont);
        w->halfW = extent->w >> 1;
        w->halfH = extent->h >> 1;
        ShapeBuild(w->shape, extent->w + 0x100, extent->h);
    }
    w->_30 = 4;
    w->appear = 0;
    if (const float* chest = BodyPoint(w->speaker, 0x490)) {
        s32 xy[4] = {};
        GsDevice_Project(*gGsDevice, xy, chest);
        w->anchorX = xy[0];
        w->anchorY = xy[1];
    }
    // Bottom middle of the screen, unless told where.
    w->x = (s32)(Screen_X(0, 0) & 0xFFFF);
    w->y = (s32)(Screen_Y(0, 1) & 0xFFFF) - (w->halfH * 2 + 0x80);
    if (p->flags & 1) {
        w->x = p->x * 16 + (s32)(Screen_X(0, -1) & 0xFFFF);
        w->y = p->y * 16 + (s32)(Screen_Y(0, -1) & 0xFFFF);
    }
    return 1;
}

static int Choice_Update(ChoiceWindow* self) {                  // 0x1917F0
    if (self->w.appear < 3)
        self->w.appear++;
    return self->w.closing != 1;
}

// 0x191B10 — the bar behind the highlighted line, fading out at both ends.
static void DrawBar(s16 x, s16 y, u32 z, s16 w, s16 h, u32 rgba) {
    struct Vertex { u32 rgba; float q; u16 x, y; u32 z; };
    struct { u64 prim; Vertex v[8]; } packet;
    packet.prim = 0x4C;
    const u32 faint = (rgba & 0xFFFFFFu) | 0x20000000u;
    const s16 xs[4] = {x, (s16)(x + 0x200), (s16)(x + w - 0x200), (s16)(x + w)};
    const u32 colours[4] = {faint, rgba, rgba, faint};
    for (int i = 0; i < 4; i++) {
        packet.v[i * 2] = {colours[i], 1.0f, (u16)xs[i], (u16)y, z};
        packet.v[i * 2 + 1] = {colours[i], 1.0f, (u16)xs[i], (u16)(y + h), z};
    }
    GsDevice_DrawPrim(*gGsDevice, &packet, 8);
}

// ── 0x1914F0 ──────────────────────────────────────────────────────────────
static int Choice_Draw(ChoiceWindow* self) {
    SpeechWidget* w = &self->w;
    const int steps = w->_30 < 3 ? w->_30 : w->appear;
    const float grown = (float)steps / 3.0f;
    if (grown == 0.0f)
        return 1;
    const s32 x = w->x - w->halfW, y = w->y - w->halfH;
    if (w->shape != 0) {
        P<BalloonShape>(w->shape)->scale = grown;
        DrawOutlined(w->shape, w->x, w->y, w->z, kChoiceInk, kChoicePaper);
    }
    if (w->_30 > 2 && w->appear == 3) {
        if (self->selection != 0) {
            const u32 lineHeight = (u32)(w->scale * (float)*P<u16>(0x2EE53Au));
            DrawBar((s16)x, (s16)(lineHeight * (u32)self->selection + (u32)y - lineHeight), w->z,
                    (s16)((w->halfW & 0x7FFF) << 1), (s16)lineHeight, 0x80292E30u);
        }
        Font_SetColor(*gFont, 0x80ADC5CCu);
        Font_DrawString(*gFont, TextOf(w), x, y, w->z, w->scale, 0.0f);
    }
    return 1;
}
GUEST_BIND(0x191A30, Choice_Dtor);
GUEST_BIND(0x191830, Choice_Show);
GUEST_BIND(0x1917F0, Choice_Update);
GUEST_BIND(0x1914F0, Choice_Draw);

// ── 0x191A90 ──────────────────────────────────────────────────────────────
ChoiceWindow* ChoiceWindow_Ctor(void* mem) {
    ChoiceWindow* self = static_cast<ChoiceWindow*>(mem);
    Widget_Ctor(&self->w);
    self->w.vtable = kChoiceVtable;
    self->w.kind = 2;
    self->w.shape = 0;
    self->w.x = (s32)(Screen_X(0, 0) & 0xFFFF);
    self->w.y = (s32)(Screen_Y(0, 0) & 0xFFFF);
    self->selection = 0;
    return self;
}

int ChoiceWindow_SetSelection(ChoiceWindow* self, int line) {   // 0x1917E0
    self->selection = line;
    return 1;
}

// ══ The list (0x1885E0–0x188870) ══════════════════════════════════════════

SpeechList* SpeechList_Ctor(void* mem) {                        // 0x188870
    SpeechList* self = static_cast<SpeechList*>(mem);
    GuestList_Init(&self->widgets);
    sCurrentList = guest::addr(self);
    memset(self->_0c, 0, sizeof self->_0c);
    return self;
}

SpeechList* SpeechList_Dtor(SpeechList* self, s16 flag) {       // 0x188760
    if (self == nullptr)
        return self;
    for (GuestListNode* n = GuestList_Begin(&self->widgets); n != GuestList_End(&self->widgets); n = GuestList_Next(n))
        if (n->value != 0)
            VirtualCall<void*, void*, int>(P<SpeechWidget>(n->value)->vtable, 0x08, P(n->value), 1);
    GuestList_Clear(&self->widgets);
    sCurrentList = 0;
    if (flag > 0)
        MemFree(self);
    return self;
}

int SpeechList_Draw(SpeechList* self) {                         // 0x1885E0
    for (GuestListNode* n = GuestList_Begin(&self->widgets); n != GuestList_End(&self->widgets); n = GuestList_Next(n))
        VirtualCall<int, void*>(P<SpeechWidget>(n->value)->vtable, 0x14, P(n->value));
    return 1;
}

// ── 0x188640 ──────────────────────────────────────────────────────────────
// Later widgets are drawn over earlier ones; remarks go under everything.
int SpeechList_Update(SpeechList* self) {
    u32 z = 0xFFF001u;
    GuestListNode* n = GuestList_Begin(&self->widgets);
    while (n != GuestList_End(&self->widgets)) {
        SpeechWidget* w = P<SpeechWidget>(n->value);
        w->z = w->kind == 3 ? 0xFFF000u : z++;
        if (VirtualCall<int, void*>(w->vtable, 0x10, w) == 0) {
            if (n->value != 0)
                VirtualCall<void*, void*, int>(w->vtable, 0x08, w, 1);
            n = GuestList_Erase(&self->widgets, n);
        } else {
            n = GuestList_Next(n);
        }
    }
    return 1;
}

// ══ The shop menu's yes / no box (0x239D50–0x23A190, vtable 0x2EC710) ═════
// A choice window in the shop's colours at a fixed place; the shop menu
// itself moves the bar (shop_menu.cpp).
static const u32 kShopMessageVtable = 0x2EC710u;

static ChoiceWindow* ShopMessage_Dtor(ChoiceWindow* self, s16 flag) {   // 0x23A130
    if (self == nullptr)
        return self;
    self->w.vtable = kShopMessageVtable;
    Widget_Dtor(&self->w, 0);
    if (flag > 0)
        MemFree(self);
    return self;
}
static int ShopMessage_Show(ChoiceWindow* self, const SpeechParams* p) {    // 0x239FE0
    SpeechWidget* w = &self->w;
    if (w->shape == 0)
        w->shape = guest::addr(NewShape(kRoughVtable));
    const u16* text = P<const u16>(p->text);
    WString_Assign(&w->text, text, text + WStr_Length(text));
    w->speaker = p->speaker;
    w->listener = p->listener;
    if (w->shape != 0) {
        Font_Measure(*gFont, TextOf(w), w->scale);
        const FontExtent* extent = Font_GetExtent(*gFont);
        w->halfW = extent->w >> 1;
        w->halfH = extent->h >> 1;
        ShapeBuild(w->shape, extent->w + 0x100, extent->h);
    }
    w->_30 = 4;
    w->appear = 0;
    w->x = w->halfW + (s32)(Screen_X(0, -1) & 0xFFFF) + 0x1550;
    w->y = (s32)(Screen_Y(0, -1) & 0xFFFF) + 0x1570;
    w->z = 0xFFF001u;
    return 1;
}
static int ShopMessage_Update(ChoiceWindow* self) { return self->w.closing != 1; }  // 0x239FD0
static int ShopMessage_Draw(ChoiceWindow* self) {                       // 0x239D60
    SpeechWidget* w = &self->w;
    const s32 x = w->x - w->halfW, y = w->y - w->halfH;
    if (w->shape != 0) {
        P<BalloonShape>(w->shape)->scale = 1.0f;
        DrawOutlined(w->shape, w->x, w->y, w->z, 0x80111314u, 0x803F494Du);
    }
    if (self->selection != 0) {
        const u32 lineHeight = (u32)(w->scale * (float)*P<u16>(0x2EE53Au));
        DrawBar((s16)x, (s16)(lineHeight * (u32)self->selection + (u32)y - lineHeight), w->z,
                (s16)((w->halfW & 0x7FFF) << 1), (s16)lineHeight, 0x80292E30u);
    }
    Font_SetColor(*gFont, 0x80ADC5CCu);
    Font_DrawString(*gFont, TextOf(w), x, y, w->z, w->scale, 0.0f);
    return 1;
}
GUEST_BIND(0x23A130, ShopMessage_Dtor);
GUEST_BIND(0x239FE0, ShopMessage_Show);
GUEST_BIND(0x239FD0, ShopMessage_Update);
GUEST_BIND(0x239D60, ShopMessage_Draw);

ChoiceWindow* ShopMessage_Ctor(void* mem) {                             // 0x23A190
    ChoiceWindow* self = static_cast<ChoiceWindow*>(mem);
    Widget_Ctor(&self->w);
    self->w.vtable = kShopMessageVtable;
    self->w.kind = 5;
    self->w.shape = 0;
    self->w.x = (s32)(Screen_X(0, 0) & 0xFFFF);
    self->w.y = (s32)(Screen_Y(0, 0) & 0xFFFF);
    self->selection = 0;
    return self;
}
void ShopMessage_SetSelection(ChoiceWindow* self, int line) { self->selection = line; }    // 0x239D50
