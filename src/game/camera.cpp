// The view camera: the stage, its camera list and the camera controllers.
// Decompiled from 0x150340–0x1512D0 (the list), 0x151430–0x1517F0 (the free
// camera), 0x151E60–0x1530D0 (the follow camera and keeping walls out of the
// way), 0x157510–0x157DE0 (easing), 0x1653C0–0x1654B0 (the stage) and
// 0x18BB50–0x18BFA0 (the camera on two people talking).
//
// The *stage* (World+0xE0, 0xD0 bytes) holds what the frame is drawn from:
// an eye, a direction and an up vector.  They come from the *camera list*
// inside it (0x70 bytes, also at *0x2EE528), which is not a list at all: it
// has one *controller* at a time.  Each frame the controller says where it
// would like the camera — a point to look at, a pivot and an eye given in a
// frame of its own — the list moves the eye to where the map lets it be seen
// from, and eases the camera there.
//
// Keeping walls out of the way works on the map's collision cells: the line
// from the pivot to the eye is walked in quarter-unit steps until a cell says
// no (a wall, a roof overhead, or the edge of the map).  If the eye cannot be
// at least as far as the controller insists on, sixteen directions round the
// pivot are tried and the one nearest the present camera that is long enough
// wins; failing that the camera stays where it is.

#include "engine/pad.h"
#include "game/chara_resources.h"
#include "game/map_collision.h"
#include "native/guest.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    int   Rand_Next();                                  // 0x1200F0
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Matrix_Identity(void* m);                     // 0x107428
    void  Matrix_Rotate(void* dst, const void* src, float angle);       // 0x107618  about y
    void  Matrix_Multiply(void* dst, const void* a, const void* b);     // 0x107130
    void* PadMgr_Get();                                 // 0x13A920
    u16   Pad_GetHeld(void* mgr, int port);             // 0x13A0E0
    int   Pad_GetRightStick(void* mgr, int port, u8* x, u8* y);         // 0x139F10

    void* FollowCam_Ctor(void* mem, void* target, const void* config);  // 0x152470
    void  CamList_Push(void* list, void* controller);                   // 0x150E60
}

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);
static u32* const gCamList = guest::ptr<u32>(0x2EE528u);
static const GuestPtrVar<u8> gConfig{0x2EE68Cu};
static const float* const kIdentity = guest::ptr<const float>(0x2D1A80u);

// ═══ Vectors (Sony libvu0; four floats, the matrix convention is row vectors) ═
static inline void Copy(float* d, const float* s) { memcpy(d, s, 16); }
static inline void Set(float* d, float x, float y, float z, float w) { d[0] = x; d[1] = y; d[2] = z; d[3] = w; }
static inline void Sub(float* d, const float* a, const float* b) { for (int i = 0; i < 4; i++) d[i] = a[i] - b[i]; }
static inline void Add(float* d, const float* a, const float* b) { for (int i = 0; i < 4; i++) d[i] = a[i] + b[i]; }
static inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float Dist(const float* a, const float* b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}
// 0x1071C0
static inline void Normalize(float* d, const float* s) {
    const float q = 1.0f / sqrtf(Dot(s, s));
    for (int i = 0; i < 4; i++) d[i] = s[i] * q;
}
// 0x136E?? (Vec_Normalize): the same, and → the length it had.
static inline float NormalizeLen(float* d, const float* s) {
    const float len = sqrtf(Dot(s, s));
    if (len > 0.0f)
        for (int i = 0; i < 4; i++) d[i] = s[i] / len;
    return len;
}
// 0x107178: a × b
static inline void Cross(float* d, const float* a, const float* b) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    Set(d, x, y, z, 0.0f);
}
// 0x107C68: a × t + b × (1 − t)
static inline void Inter(float* d, const float* a, const float* b, float t) {
    for (int i = 0; i < 4; i++) d[i] = a[i] * t + b[i] * (1.0f - t);
}
// sceVu0ApplyMatrix: v × m
static inline void Apply(float* d, const float* m, const float* v) {
    float r[4];
    for (int j = 0; j < 4; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    Copy(d, r);
}
// 0x107570: dst = src turned by `angle` about x.
static void RotateX(float* dst, const float* src, float angle) {
    const float c = cosf(angle), s = sinf(angle);
    alignas(16) const float r[16] = {1, 0, 0, 0,  0, c, s, 0,  0, -s, c, 0,  0, 0, 0, 1};
    Matrix_Multiply(dst, r, src);
}
// 0x11BC98 as the camera uses it: the heading of (x, z).
static inline float Heading(float x, float z) { return atan2f(x, z); }

// ── 0x152FD0: an angle brought into −π … π ────────────────────────────────
static float WrapAngle(float a) {
    if (a < -3.1415927f || 3.1415927f < a) {
        const float f = (a + 31.415928f) * 1000.0f;
        const u32 u = f < 2.1474836e+09f ? (u32)(s32)f : ((u32)(s32)(f - 2.1474836e+09f) | 0x80000000u);
        a = (float)(u32)((s32)u % 0x188B) / 1000.0f - 3.1415927f;
    }
    return a;
}

// ── 0x1530D0: a stick axis with its dead zone taken out (−80 … 79) ────────
static int StickAxis(int v) {
    if (v - 0x80 < 0x31) {
        if (v - 0x80 < -0x30)
            return v - 0x50;
        return 0;
    }
    return v - 0xB0;
}

// ═══ Layouts ══════════════════════════════════════════════════════════════
struct EaseHolder { u32 current, next; };       // what CamList+0x60 points to

struct CamState {           // 0x50 bytes
    u32   controller;       // +0x00
    u32   _04[3];
    float dir[4];           // +0x10  the way the camera looks
    s32   turnFrames;       // +0x20  after a cut: frames over which pad directions follow the new view
    u32   _24[3];
    float padDir[4];        // +0x30  the heading pad directions are read by
    s32   shake;            // +0x40  frames of shaking left
    float shakeAmp;         // +0x44
    float shakeStep;        // +0x48
    u32   _4c;
};

struct CamList {            // 0x70 bytes
    float target[4];        // +0x00  where it wants to be…
    float eye[4];           // +0x10
    float up[4];            // +0x20
    float curTarget[4];     // +0x30  …and where it is
    float curEye[4];        // +0x40
    float curUp[4];         // +0x50
    u32   ease;             // +0x60  EaseHolder*
    u32   map;              // +0x64  MapCollision* (set by World_Step)
    s32   cuts;             // +0x68  controllers still to be cut to rather than eased to
    u32   state;            // +0x6C  CamState*
};

struct Stage {              // 0xD0 bytes
    float eye[4], dir[4], up[4];                // +0x00  what the frame is drawn from
    float nextEye[4], nextDir[4], nextUp[4];    // +0x30
    CamList cams;                               // +0x60
};

// What a controller asks for.
struct CamWish {
    float look[4];          // +0x00  the point to look at
    float nearLook[4];      // +0x10  …and the one to drift to as the eye is pushed in
    float eye[4];           // +0x20  the eye and the pivot it swings about, in the frame below
    float pivot[4];         // +0x30
    float frame[16];        // +0x40
    float roll;             // +0x80
};

// How far a line through the map got.
struct LineResult {
    float pos[4];
    float reached, wanted;
    s32   kind;             // what stopped it: 1 a wall, 2 a roof overhead, 3 an edge
};

// Controller vtable slots (the vtable pointer is the object's first word).
enum : u32 {
    kCamDtor = 0x08, kCamCompute = 0x0C, kCamAttach = 0x10, kCamDistance = 0x14, kCamAfter = 0x18,
    kCamRecentre = 0x1C, kCamDetach = 0x20, kCamStep = 0x24, kCamFlags = 0x28,
};
static inline u32 Vt(void* object) { return *static_cast<u32*>(object); }
static inline CamState* State(CamList* l) { return P<CamState>(l->state); }
static inline MapCollision* MapOf(CamList* l) { return l->map ? P<MapCollision>(l->map) : nullptr; }

// ═══ Easing ═══════════════════════════════════════════════════════════════
// Each gives the share of the remaining way to cover this frame.
enum : u32 { kVtEase = 0x2EAD80u, kVtEaseShaky = 0x2EADA0u, kVtEaseFrames = 0x2EADC0u, kVtEaseFixed = 0x2EADE0u,
             kVtEaseFollow = 0x2EAE00u };

struct EaseFollow {         // 0x30 bytes: speeds up gradually, slows when the goal changes direction
    u32 vtable; u32 _04[3];
    float dir[4];           // +0x10
    float speed;            // +0x20
    float share;            // +0x24  0.2
    float gain;             // +0x28  0.02
    u32 _2c;
};

extern "C" {

// ── 0x157670 / 0x157CB0 / 0x157D10 / 0x157D70 / 0x157B20 ──────────────────
void* Ease_Dtor(void* self, int flag) {
    if (self != nullptr) {
        *static_cast<u32*>(self) = kVtEase;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
// ── 0x157550 / 0x157DD0 ───────────────────────────────────────────────────
void Ease_ResetNone(void*) {}

// ── 0x1578A0: `frames` to get there, unsteadily ───────────────────────────
void* EaseShaky_Ctor(void* mem, int frames) {
    u32* e = static_cast<u32*>(mem);
    e[0] = kVtEaseShaky;
    if (frames < 0)
        frames = 0;
    e[1] = (u32)frames;
    e[2] = (u32)frames;
    return mem;
}
// ── 0x1578F0 ──────────────────────────────────────────────────────────────
float EaseShaky_Share(void* self) {
    s32* e = static_cast<s32*>(self);
    float* const last = P<float>(0x2EE550u);
    const s32 half = *gFrameRate / 2;
    const s32 left = e[2];
    if (*gFrameRate / 5 <= left) {
        const float share = 1.0f / (float)left;
        const s32 gone = e[1] - left;
        if (gone < half)
            *last = share * ((float)gone / (float)half);
        else
            *last = share + share * ((float)(Rand_Next() % 0x28 - 0x14) / 100.0f);
        e[2]--;
    }
    return *last;
}
// ── 0x157A20: there in exactly `frames` ───────────────────────────────────
void* EaseFrames_Ctor(void* mem, int frames) {
    u32* e = static_cast<u32*>(mem);
    e[0] = kVtEaseFrames;
    if (frames < 0)
        frames = 0;
    e[1] = (u32)frames;
    return mem;
}
// ── 0x157A70 ──────────────────────────────────────────────────────────────
float EaseFrames_Share(void* self) {
    s32* e = static_cast<s32*>(self);
    const s32 left = e[1];
    if (left == 0)
        return 1.0f;
    e[1] = left - 1;
    return 1.0f / (float)left;
}
// ── 0x157AC0: the same share every frame ──────────────────────────────────
void* EaseFixed_Ctor(float share, void* mem) {
    u32* e = static_cast<u32*>(mem);
    e[0] = kVtEaseFixed;
    *reinterpret_cast<float*>(&e[1]) = share < 0.0f ? 0.0f : (share <= 1.0f ? share : 1.0f);
    return mem;
}
// ── 0x157AB0 ──────────────────────────────────────────────────────────────
float EaseFixed_Share(void* self) { return static_cast<float*>(self)[1]; }

// ── 0x157B80 ──────────────────────────────────────────────────────────────
float EaseFollow_Share(EaseFollow* self, const float* wanted, const float* current) {
    float v[4];
    Sub(v, current, wanted);
    const float length = NormalizeLen(v, v);
    const float limit = length * self->share;
    if (!(0.0f < length)) {
        self->speed = limit;
        return 0.2f;
    }
    self->speed *= (Dot(v, self->dir) + 1.0f) / 2.0f;
    Copy(self->dir, v);
    if (self->speed < limit) {
        self->speed += limit * self->gain;
        if (limit < self->speed)
            self->speed = limit;
    } else {
        self->speed = limit;
    }
    const float share = self->speed / length;
    return 0.2f < share ? 0.2f : share;
}
// ── 0x157DE0 ──────────────────────────────────────────────────────────────
void EaseFollow_Reset(EaseFollow* self) { self->speed = 0.0f; }

GUEST_BIND(0x157670, Ease_Dtor);
GUEST_BIND(0x157550, Ease_ResetNone);
GUEST_BIND(0x1578F0, EaseShaky_Share);
GUEST_BIND(0x157A70, EaseFrames_Share);
GUEST_BIND(0x157AB0, EaseFixed_Share);
GUEST_BIND(0x157B80, EaseFollow_Share);
GUEST_BIND(0x157DE0, EaseFollow_Reset);

}  // extern "C"

static const struct EaseBinds {
    EaseBinds() {
        for (u32 address : {0x157CB0u, 0x157D10u, 0x157D70u, 0x157B20u})
            guest::bind(address, Ease_Dtor, "Ease_Dtor");
        guest::bind(0x157DD0u, Ease_ResetNone, "Ease_ResetNone");
    }
} sEaseBinds;

static u32 NewEaseFollow() {
    EaseFollow* e = static_cast<EaseFollow*>(MemAlloc(0x30));
    if (e != nullptr) {
        e->vtable = kVtEaseFollow;
        e->share = 0.2f;
        e->gain = 0.02f;
        Set(e->dir, 0.0f, 0.0f, 0.0f, 1.0f);
        e->speed = 0.0f;
    }
    return guest::addr(e);
}
static void ReplaceEase(EaseHolder* h, u32 ease) {
    if (h->current != 0)
        VirtualCall<void*, void*, int>(Vt(P(h->current)), 0x08, P(h->current), 1);
    h->current = ease;
}

extern "C" {

// ── 0x1577D0 ──────────────────────────────────────────────────────────────
void EaseHolder_Ctor(u32* slot) {
    EaseHolder* h = static_cast<EaseHolder*>(MemAlloc(8));
    if (h != nullptr) {
        h->current = 0;
        h->next = 0;
    }
    *slot = guest::addr(h);
    ReplaceEase(h, NewEaseFollow());
}
// ── 0x157710 ──────────────────────────────────────────────────────────────
void EaseHolder_Dtor(u32* slot) {
    EaseHolder* h = P<EaseHolder>(*slot);
    if (*slot == 0)
        return;
    if (h->next != 0)
        VirtualCall<void*, void*, int>(Vt(P(h->next)), 0x08, P(h->next), 1);
    if (h->current != 0)
        VirtualCall<void*, void*, int>(Vt(P(h->current)), 0x08, P(h->current), 1);
    MemFree(h);
}
// ── 0x1576C0: the easing the next controller will be brought in with ──────
void EaseHolder_SetNext(u32* slot, void* ease) {
    EaseHolder* h = P<EaseHolder>(*slot);
    if (h->next != 0)
        VirtualCall<void*, void*, int>(Vt(P(h->next)), 0x08, P(h->next), 1);
    h->next = guest::addr(ease);
}
// ── 0x157560: a new controller: take the easing set for it, else the usual ─
void EaseHolder_Begin(u32* slot) {
    EaseHolder* h = P<EaseHolder>(*slot);
    const u32 next = h->next;
    if (next == 0) {
        ReplaceEase(h, NewEaseFollow());
    } else {
        h->next = 0;
        ReplaceEase(h, next);
    }
    VirtualCall<void, void*>(Vt(P(h->current)), 0x10, P(h->current));
}
// ── 0x157510 / 0x157530 ───────────────────────────────────────────────────
static float Ease_Share(u32* slot, const float* wanted, const float* current) {
    EaseHolder* h = P<EaseHolder>(*slot);
    return VirtualCall<float, void*, const float*, const float*>(Vt(P(h->current)), 0x0C, P(h->current), wanted, current);
}
static void Ease_Reset(u32* slot) {
    EaseHolder* h = P<EaseHolder>(*slot);
    VirtualCall<void, void*>(Vt(P(h->current)), 0x10, P(h->current));
}

}  // extern "C"

// ═══ Lines through the map ════════════════════════════════════════════════

// ── 0x152D20: may the camera be here?  0 yes ──────────────────────────────
static int Cam_CellTest(MapCollision* map, const float* pos) {
    const ColCellType* cell = Map_CellAt(map, pos);
    if (cell == nullptr)
        return 1;
    const float y = pos[1];
    if (y < cell->height)
        return 1;
    const u8 bits = cell->_16;
    if ((bits & 2) == 0) {
        if (bits & 4)
            return 3;
        if (cell->flags & 2)
            return 1;
    }
    if ((bits & 1) == 0 || y <= cell->height + 2.2f)
        return 0;
    return 2;
}

// ── 0x152DF0: walk from `from` towards `to`; → 1 if something stopped it ──
static int Cam_Line(MapCollision* map, const float* from, const float* to, LineResult* out) {
    LineResult scratch;
    float step[4], p[4];
    Sub(step, to, from);
    const float length = NormalizeLen(step, step);
    if (out == nullptr)
        out = &scratch;
    Copy(out->pos, from);
    out->reached = 0.0f;
    out->wanted = length;
    out->kind = 0;
    if (length <= 1e+10f) {
        int n = (int)(length / 0.25f);
        if (n == 0)
            n = 1;
        const float scale = length / (float)n;
        for (int i = 0; i < 4; i++)
            step[i] *= scale;
        Copy(p, from);
        float gone = 0.0f;
        for (; n > 0; n--) {
            gone += 0.25f;
            Add(p, p, step);
            const int hit = Cam_CellTest(map, p);
            if (hit == 3) {
                if (length * 0.6f < gone) {
                    out->kind = 3;
                    return 1;
                }
            } else {
                if (hit != 0) {
                    out->kind = hit;
                    return 1;
                }
                out->reached = gone;
                Copy(out->pos, p);
            }
        }
    }
    return 0;
}
// A line stopped by a roof is tried again level with where it starts.
static void Cam_LineLevel(MapCollision* map, const float* from, const float* to, LineResult* out) {
    if (Cam_Line(map, from, to, out) && out->kind == 2) {
        float level[4] = {to[0], from[1], to[2], 1.0f};
        Cam_Line(map, from, level, out);
    }
}

// ── 0x152830: where the eye can be.  → 0 where it wanted, 1 somewhere else
// round the pivot, −1 nowhere at least `want` away ─────────────────────────
static int Cam_Avoid(float want, MapCollision* map, const float* pivot, const float* eye, LineResult* out) {
    if (map == nullptr) {
        Copy(out->pos, eye);
        out->reached = out->wanted = Dist(pivot, eye);
        out->kind = 0;
    } else {
        Cam_LineLevel(map, pivot, eye, out);
    }
    const float need = want <= out->wanted ? want : out->wanted;
    if (!(out->reached < need))
        return 0;

    float best = 1e+10f;
    const float* current = P<CamList>(*gCamList)->curEye;
    float v[4], flat[4];
    Sub(v, eye, pivot);
    v[3] = 1.0f;
    Copy(flat, v);
    flat[1] = 0.0f;
    const float heading = Heading(flat[0], flat[2]);
    const float radius = sqrtf(Dot(flat, flat));
    // The nearest eighth of a turn.
    const int degrees = (int)(heading * 180.0f / 3.1415927f);
    const int rounded = (degrees > 0 ? degrees + 0x16 : degrees - 0x16) / 0x2D * 0x2D;
    const float snapped = (float)rounded * 3.1415927f / 180.0f;
    v[0] = radius * sinf(snapped);
    v[2] = radius * cosf(snapped);
    v[3] = 1.0f;
    Add(v, v, pivot);
    v[3] = 1.0f;
    for (int i = 0x10; i > 0; i--) {
        LineResult r;
        if (map == nullptr) {
            Copy(r.pos, v);
            r.reached = r.wanted = Dist(pivot, v);
            r.kind = 0;
        } else {
            Cam_LineLevel(map, pivot, v, &r);
        }
        if (need <= r.reached) {
            const float away = (float)(int)Dist(r.pos, current);
            if (away < best) {
                *out = r;
                best = away;
            }
        }
        // A sixteenth of a turn on, about the pivot.
        alignas(16) float turn[16];
        Matrix_Identity(turn);
        Sub(&turn[12], &turn[12], pivot);
        Matrix_Rotate(turn, turn, 0.3926991f);
        Add(&turn[12], &turn[12], pivot);
        turn[15] = 1.0f;
        Apply(v, turn, v);
    }
    return need <= out->reached ? 1 : -1;
}

// ── 0x1525B0: should the camera cut rather than ease?  (It has fallen far
// behind, the view would swing through the target, or the eye is in a wall.)
static int Cam_ShouldCut(MapCollision* map, const float* target, const float* eye, const float* wanted, float* out) {
    const float apart = Dist(eye, wanted);
    if (8.0f < apart) {
        Copy(out, wanted);
        return 1;
    }
    if (1.5f < apart && map != nullptr) {
        float a[4], b[4];
        Sub(a, target, wanted);
        Sub(b, target, eye);
        Normalize(a, a);
        Normalize(b, b);
        if (Dot(a, b) < -0.95f) {
            Copy(out, wanted);
            return 1;
        }
        if (Cam_CellTest(map, eye) != 0) {
            LineResult r;
            Cam_LineLevel(map, wanted, eye, &r);
            Copy(out, r.pos);
            return 1;
        }
    }
    return 0;
}

// ── 0x1512D0: `m` turned by `angle` about an axis ─────────────────────────
static void RotateAbout(float angle, float* out, const float* m, const float* axis) {
    const float q = 1.0f / sqrtf(axis[2] * axis[2] + axis[0] * axis[0] + axis[1] * axis[1]);
    const float x = axis[0] * q, y = axis[1] * q, z = axis[2] * q;
    const float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
    alignas(16) float r[16];
    Matrix_Identity(r);
    r[0] = t * x * x + c;
    r[1] = t * x * y - z * s;
    r[2] = t * x * z - y * s;
    r[4] = r[1];
    r[5] = t * y * y + c;
    r[6] = t * y * z + x * s;
    r[8] = t * x * z + y * s;
    r[9] = t * y * z - x * s;
    r[10] = t * z * z + c;
    Matrix_Multiply(out, r, m);
}

// ═══ The camera list ══════════════════════════════════════════════════════

static void MarkCut(CamList* self) {
    CamState* s = State(self);
    Set(s->padDir, s->dir[0], 0.0f, s->dir[2], 1.0f);
    Normalize(s->padDir, s->padDir);
    State(self)->turnFrames = 0xF;
}

// ── 0x1508F0: one frame: ask the controller, mind the walls, ease ─────────
static int CamList_Solve(CamList* self) {
    void* ctrl = P(State(self)->controller);
    if (State(self)->controller != 0) {
        CamWish wish;
        LineResult got;
        Set(wish.eye, 0.0f, 0.0f, 0.0f, 1.0f);
        Copy(wish.pivot, wish.eye);
        Copy(wish.look, wish.eye);
        Copy(wish.nearLook, wish.eye);
        Matrix_Identity(wish.frame);
        wish.roll = 0.0f;
        if (VirtualCall<int, void*, CamList*, CamWish*>(Vt(ctrl), kCamCompute, ctrl, self, &wish) != 0) {
            const float want = VirtualCall<float, void*>(Vt(ctrl), kCamDistance, ctrl);
            if (0.0f < want) {
                float eye[4], pivot[4];
                Apply(eye, wish.frame, wish.eye);
                Apply(pivot, wish.frame, wish.pivot);
                int found;
                if (1.0f < Dist(self->curEye, self->curTarget)) {
                    found = Cam_Avoid(want, MapOf(self), pivot, eye, &got);
                    if (found < 0)
                        found = Cam_Avoid(want, MapOf(self), pivot, eye, &got);
                } else {
                    found = Cam_Avoid(want, MapOf(self), pivot, eye, &got);
                }
                if (found < 0) {
                    if (got.reached <= 0.75f) {
                        Copy(got.pos, self->curEye);
                        got.reached = Dist(got.pos, pivot);
                        if (got.wanted < got.reached)
                            got.reached = got.wanted;
                    }
                } else if (found > 0) {
                    got.reached = 0.0f;
                }
            } else {
                Apply(got.pos, wish.frame, wish.eye);
                got.reached = got.wanted = Dist(wish.eye, wish.pivot);
            }
            Copy(self->eye, got.pos);
            if (0.0f < got.wanted)
                Inter(self->target, wish.look, wish.nearLook, got.reached / got.wanted);
            else
                Copy(self->target, wish.look);
            Apply(self->target, wish.frame, self->target);
            float view[4];
            alignas(16) float rolled[16];
            Sub(view, self->target, self->eye);
            RotateAbout(wish.roll, rolled, kIdentity, view);
            Copy(self->up, &rolled[4]);
            VirtualCall<void, void*, CamList*>(Vt(ctrl), kCamAfter, ctrl, self);
        }
        const float share = Ease_Share(&self->ease, self->eye, self->curEye);
        Inter(self->curEye, self->eye, self->curEye, share);
        Inter(self->curTarget, self->target, self->curTarget, share);
        Inter(self->curUp, self->up, self->curUp, share);
        Normalize(self->curUp, self->curUp);
        if (Cam_ShouldCut(MapOf(self), self->curTarget, self->curEye, self->eye, self->curEye) != 0) {
            MarkCut(self);
            Copy(self->curTarget, self->target);
        }
    } else {
        const float share = Ease_Share(&self->ease, self->eye, self->curEye);
        Inter(self->curEye, self->eye, self->curEye, share);
        Inter(self->curTarget, self->target, self->curTarget, share);
        Inter(self->curUp, self->up, self->curUp, share);
        Normalize(self->curUp, self->curUp);
    }
    CamState* s = State(self);
    Sub(s->dir, self->curTarget, self->curEye);
    Normalize(s->dir, s->dir);
    return 1;
}

static void DropController(CamList* self) {
    CamState* s = State(self);
    if (s->controller != 0)
        VirtualCall<void, void*, CamList*>(Vt(P(s->controller)), kCamDetach, P(s->controller), self);
    s = State(self);
    if (s->controller != 0)
        VirtualCall<void*, void*, int>(Vt(P(s->controller)), kCamDtor, P(s->controller), 1);
    s->controller = 0;
}

extern "C" {

// ── 0x1505E0: be where it wants to be, now ────────────────────────────────
void CamList_Apply(void* mem) {
    CamList* self = static_cast<CamList*>(mem);
    memcpy(self->curTarget, self->target, 0x30);
    Ease_Reset(&self->ease);
}

// ── 0x150E60: a new controller takes over (the old one is destroyed) ──────
void CamList_Push(void* mem, void* controller) {
    CamList* self = static_cast<CamList*>(mem);
    DropController(self);
    State(self)->controller = guest::addr(controller);
    if (controller == nullptr)
        return;
    VirtualCall<void, void*, CamList*>(Vt(controller), kCamAttach, controller, self);
    EaseHolder_Begin(&self->ease);
    CamList_Solve(self);
    if (0 < self->cuts) {
        MarkCut(self);
        memcpy(self->curTarget, self->target, 0x30);
        Ease_Reset(&self->ease);
        self->cuts--;
    }
}

// ── 0x1507B0 / 0x150850 / 0x1506B0: no controller; look at / from / up ────
void CamList_SetTarget(void* mem, const float* v) {
    CamList* self = static_cast<CamList*>(mem);
    DropController(self);
    Copy(self->target, v);
}
void CamList_SetEye(void* mem, const float* v) {
    CamList* self = static_cast<CamList*>(mem);
    DropController(self);
    Copy(self->eye, v);
}
void CamList_SetUp(void* mem, const float* v) {
    CamList* self = static_cast<CamList*>(mem);
    DropController(self);
    Copy(self->up, v);
}

// ── 0x1503A0: shake for `frames`, dying away ──────────────────────────────
void CamList_Shake(float amount, void* mem, int frames) {
    CamState* s = State(static_cast<CamList*>(mem));
    s->shake = frames;
    s->shakeAmp = amount;
    s->shakeStep = amount * 0.3f / (float)frames;
}

// ── 0x1503F0 ──────────────────────────────────────────────────────────────
u32 CamList_GetFlags(void* mem) {
    CamState* s = State(static_cast<CamList*>(mem));
    if (s->controller == 0)
        return 0;
    return VirtualCall<u32, void*>(Vt(P(s->controller)), kCamFlags, P(s->controller));
}

// ── 0x150650: the controller starts over (behind its character) ───────────
void CamList_Update(void* mem) {
    CamList* self = static_cast<CamList*>(mem);
    CamState* s = State(self);
    if (s->controller != 0) {
        VirtualCall<void, void*>(Vt(P(s->controller)), kCamRecentre, P(s->controller));
        CamList_Solve(self);
    }
}

// ── 0x150DC0: every frame ─────────────────────────────────────────────────
int CamList_Step(void* mem) {
    CamList* self = static_cast<CamList*>(mem);
    CamState* s = State(self);
    if (0 < s->turnFrames)
        s->turnFrames--;
    if (s->shake != 0) {
        s->shakeAmp -= s->shakeStep;
        s->shake--;
    }
    if (s->controller != 0)
        VirtualCall<void, void*, CamList*>(Vt(P(s->controller)), kCamStep, P(s->controller), self);
    return CamList_Solve(self);
}

// ── 0x150530: what to draw from: the eye, the way it looks and up ─────────
void CamList_Get(void* mem, float* eye, float* dir, float* up) {
    CamList* self = static_cast<CamList*>(mem);
    Copy(eye, self->curEye);
    Copy(dir, State(self)->dir);
    Copy(up, self->curUp);
    const CamState* s = State(self);
    if (0 < s->shake) {
        dir[1] += (s->shake & 1) ? s->shakeAmp : -s->shakeAmp;
        Normalize(dir, dir);
    }
}

// ── 0x150440: the rotation that turns pad directions (up = +z) into the
// world: the camera's heading (eased in after a cut) ───────────────────────
void Camera_GetViewRotation(float* matrix) {
    CamState* s = State(P<CamList>(*gCamList));
    float d[4] = {s->dir[0], 0.0f, s->dir[2], 1.0f};
    Normalize(d, d);
    if (0 < s->turnFrames) {
        const float f = (float)s->turnFrames;
        Inter(d, d, s->padDir, (f - 1.0f) / f);
        Normalize(d, d);
        Copy(s->padDir, d);
    }
    Matrix_Identity(matrix);
    Copy(&matrix[8], d);
    matrix[0] = d[2];
    matrix[2] = -d[0];
}

}  // extern "C"

// ═══ Controllers ══════════════════════════════════════════════════════════
enum : u32 { kVtCam = 0x2EAC40u, kVtFreeCam = 0x2EAC70u, kVtFollowCam = 0x2EACA0u, kVtTalkCam = 0x2EB200u };

extern "C" {

// ── 0x150750 / 0x151430 / 0x151E60 / 0x18BB50 ─────────────────────────────
void* Cam_Dtor(void* self, int flag) {
    if (self != nullptr) {
        *static_cast<u32*>(self) = kVtCam;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
// ── The base class's methods ──────────────────────────────────────────────
float Cam_NoDistance(void*) { return 0.0f; }            // 0x150DB0
void  Cam_Nothing(void*, void*) {}                      // 0x150DA0 / 0x1506A0 / 0x1507A0 / 0x150E50
u32   Cam_NoFlags(void*) { return 0; }                  // 0x150430

GUEST_BIND(0x150750, Cam_Dtor);
GUEST_BIND(0x150DB0, Cam_NoDistance);
GUEST_BIND(0x150DA0, Cam_Nothing);
GUEST_BIND(0x150430, Cam_NoFlags);

}  // extern "C"

// ─── The free camera (the list's own; a debug camera flown with the right
// stick: R1 held slides it, R2 held moves it along the ground) ─────────────
struct FreeCam {            // 0x50 bytes
    u32 vtable; u32 _04[3];
    float matrix[16];       // +0x10
};

static void FreeCam_Square(FreeCam* self) {
    float* m = self->matrix;
    Normalize(&m[8], &m[8]);
    Set(&m[4], 0.0f, 1.0f, 0.0f, 1.0f);
    Cross(&m[0], &m[4], &m[8]);
    Normalize(&m[0], &m[0]);
    Cross(&m[4], &m[8], &m[0]);
    Normalize(&m[4], &m[4]);
}

extern "C" {

// ── 0x151490 ──────────────────────────────────────────────────────────────
int FreeCam_Compute(FreeCam* self, CamList*, CamWish* wish) {
    void* pads = PadMgr_Get();
    u8 sx, sy;
    Pad_GetRightStick(pads, 0, &sx, &sy);
    const u16 held = Pad_GetHeld(pads, 0);
    float* m = self->matrix;
    if (held & kPadR1) {
        float v[4] = {-(float)StickAxis(sx) / 128.0f * 0.08f, -(float)StickAxis(sy) / 128.0f * 0.08f, 0.0f, 1.0f};
        Apply(v, m, v);
        Copy(&m[12], v);
    } else if (held & kPadR2) {
        float v[4] = {-(float)StickAxis(sx) / 128.0f * 0.08f, 0.0f, -(float)StickAxis(sy) / 128.0f * 0.08f, 1.0f};
        Apply(v, m, v);
        Copy(&m[12], v);
    } else {
        const float turn = -(float)StickAxis(sx) / 128.0f * 0.04f;
        const float tilt = -(float)StickAxis(sy) / 128.0f * 0.04f;
        float at[4];
        alignas(16) float tilted[16];
        Copy(at, &m[12]);
        Set(&m[12], 0.0f, 0.0f, 0.0f, 1.0f);
        FreeCam_Square(self);
        Matrix_Rotate(m, m, turn);
        RotateX(tilted, kIdentity, tilt);
        Matrix_Multiply(m, m, tilted);
        Copy(&m[12], at);
    }
    Matrix_Copy(wish->frame, m);
    Set(wish->look, 0.0f, 0.0f, 0.0f, 1.0f);
    Copy(wish->nearLook, wish->look);
    Set(wish->eye, 0.0f, 0.0f, -1.0f, 1.0f);
    Copy(wish->pivot, wish->eye);
    return 1;
}
// ── 0x1517F0: starts from where the camera is ─────────────────────────────
void FreeCam_Attach(FreeCam* self, CamList* list) {
    float* m = self->matrix;
    Copy(&m[12], list->curEye);
    Sub(&m[8], list->curTarget, list->curEye);
    FreeCam_Square(self);
    m[3] = 0.0f;
    m[7] = 0.0f;
    m[11] = 0.0f;
    m[15] = 1.0f;
}

GUEST_BIND(0x151490, FreeCam_Compute);
GUEST_BIND(0x1517F0, FreeCam_Attach);

}  // extern "C"

// ─── The follow camera ────────────────────────────────────────────────────
// Stays behind its character by trailing it: the frame it places the eye in
// looks from where the eye last was to where the character is now.  The
// right stick swings it round and tilts it; R3 puts it back behind.
struct FollowConfig {       // 0x48 bytes (0x2E0020 outdoors, 0x2E0070 indoors)
    float eye[4];           // (0, 2.8, −3.6)   (0, 1.5, −2.5)
    float look[4];          // (0, 1, 0.3)
    float pivot[4];         // (0, 1.5, 0)      (0, 1.3, 0)
    float nearLook[4];      // (0, 1.4, 0.05)
    float tiltMin, tiltMax; // −0.5, 0.2        −0.5, 0.6
};
struct FollowCam {          // 0xC0 bytes
    u32 vtable; u32 _04[3];
    FollowConfig config;    // +0x10
    u32 _58[2];
    float tilt;             // +0x60
    u32 _64[3];
    float anchor[4];        // +0x70  where the eye last was
    float stickFrom[4];     // +0x80  where the character stood when the stick was last touched
    u8  stick;              // +0x90  the stick is being used
    u8  _91[3];
    u32 mode;               // +0x94  the Camera Mode setting: bit 0 tilt stays, bit 1 reversed
    u32 _98[2];
    u32 target;             // +0xA0  CharaParams*
    u32 _a4[3];
    float _b0[4];
};

static const float* Follow_Matrix(FollowCam* self) {    // 0x152580
    return self->target ? P<CharaParams>(self->target)->matrix : kIdentity;
}

extern "C" {
    int   FollowCam_Compute(FollowCam* self, CamList*, CamWish* wish);  // 0x151F90
    void  FollowCam_Attach(FollowCam* self, CamList* list);             // 0x152420
    void  FollowCam_After(FollowCam* self, CamList* list);              // 0x151F50
    void  FollowCam_Recentre(FollowCam* self);                          // 0x151ED0
}

// ─── The three cameras the follow camera hands over to ────────────────────
// What they look at through a character's parameters (CharaParams): +0xC0
// its opponent, +0xC8 → its body's flags, +0xCC → its state, +0xD4 → its
// target flags, +0xD8 who struck it last.
static inline u32 Linked(const void* params, u32 at) {
    const u32 ptr = *reinterpret_cast<const u32*>(static_cast<const u8*>(params) + at);
    return ptr != 0 ? *P<u32>(ptr) : 0;
}
static inline u32 ParamAt(const void* params, u32 at) {
    return *reinterpret_cast<const u32*>(static_cast<const u8*>(params) + at);
}
static inline const float* MatrixOf(const void* params) { return static_cast<const CharaParams*>(params)->matrix; }
// 0x17E770: it stands facing an opponent.
static bool Cam_Duelling(const void* params) {
    return params != nullptr && ParamAt(params, 0xC0) != 0 && (Linked(params, 0xD4) & 0x1000000) != 0;
}
// 0x2442F0
static bool Cam_Dying(const void* params) {
    return params != nullptr && ((Linked(params, 0xCC) & 0x200) != 0 || (Linked(params, 0xC8) & 0x100000) != 0);
}
// 0x27A0E0: the cell it stands on is marked 0x40.
static bool Cam_InSpot(const void* params) {
    const u32 list = *gCamList;
    const u32 map = list != 0 ? P<CamList>(list)->map : 0;
    return map != 0 && (Map_CellAt(P<MapCollision>(map), &MatrixOf(params)[12])->flags & 0x40) != 0;
}
// 0x2BBEC0: a follow camera for where it is (indoors a closer one).
static void* FollowCam_NewFor(void* params) {
    void* mem = MemAlloc(0xC0);
    return mem ? FollowCam_Ctor(mem, params, P(*P<s32>(0x2EE518u) == 1 ? 0x2E0070u : 0x2E0020u)) : nullptr;
}
// 0x152780: an upright frame whose z runs from the second point back to the
// first (sceVu0SubVector(z, first, second) = first − second).
static void LookFrame(float* m, const float* from, const float* to) {
    Sub(&m[8], from, to);
    Normalize(&m[8], &m[8]);
    m[11] = 0.0f;
    Set(&m[4], 0.0f, 1.0f, 0.0f, 1.0f);
    Cross(&m[0], &m[4], &m[8]);
    Normalize(&m[0], &m[0]);
    m[3] = 0.0f;
    Cross(&m[4], &m[8], &m[0]);
    Normalize(&m[4], &m[4]);
    m[7] = 0.0f;
    Set(&m[12], 0.0f, 0.0f, 0.0f, 1.0f);
}

// ── The duel camera (0x17E700–0x17EC10, vtable 0x2EB000, 0x30 bytes): off to
// one side of the line between the two — the nearer they are, the more side
// on — whichever side the camera is already nearer to. ───────────────────
struct DuelCam {
    u32 vtable; u32 _04[3];
    u32 target;             // +0x10  CharaParams*
    u32 _14[3];
    float _20[4];
};
static_assert(sizeof(DuelCam) == 0x30, "DuelCam");
static void* DuelCam_New(void* params) {                                // 0x17EC10
    DuelCam* self = static_cast<DuelCam*>(MemAlloc(sizeof(DuelCam)));
    if (self == nullptr)
        return nullptr;
    self->vtable = 0x2EB000u;
    self->target = guest::addr(params);
    Set(self->_20, 0.0f, 0.0f, 0.0f, 1.0f);
    return self;
}
static int DuelCam_Compute(DuelCam* self, CamList* list, CamWish* wish) {   // 0x17E7E0
    if (self->target == 0)
        return 0;
    const float* me = MatrixOf(P(self->target));
    const u32 foeAt = ParamAt(P(self->target), 0xC0);
    const float* foe = foeAt != 0 ? MatrixOf(P(foeAt)) : kIdentity;        // 0x152550
    float to[4];
    Sub(to, &foe[12], &me[12]);
    float half = NormalizeLen(to, to) / 2.0f;
    if (half < 1.5f) {
        to[1] = 0.0f;
        if (Dot(to, to) == 0.0f) {
            Copy(to, &me[8]);
            half = 0.5f;
        }
    }
    static const float kOrigin[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    LookFrame(wish->frame, kOrigin, to);
    Copy(&wish->frame[12], &me[12]);
    const float back = half < 0.5f ? 0.5f : half > 1.2f ? 1.2f : half;
    float angle = half * -0.6108653f + 2.0071287f;
    angle = angle < 0.17453294f ? 0.17453294f : angle > 1.3962635f ? 1.3962635f : angle;
    alignas(16) float turn[16];
    float side[2][4];
    Set(side[0], 0.0f, 1.5f, 4.0f, 1.0f);
    Matrix_Rotate(turn, kIdentity, angle);
    Apply(side[0], turn, side[0]);
    Set(side[1], -side[0][0], side[0][1], side[0][2], 1.0f);
    float a[4], b[4];
    Apply(a, wish->frame, side[0]);
    Apply(b, wish->frame, side[1]);
    Copy(wish->eye, side[Dist(list->curEye, a) < Dist(list->curEye, b) ? 0 : 1]);
    Set(wish->pivot, 0.0f, 1.5f, 0.0f, 1.0f);
    Set(wish->look, 0.0f, 1.0f, -back, 1.0f);
    Set(wish->nearLook, 0.0f, 1.3f, -0.3f, 1.0f);
    return 1;
}
static float DuelCam_Distance(DuelCam*) { return 1.5f; }                // 0x17E7C0
static void DuelCam_Attach(DuelCam*, CamList*) {}                       // 0x17E7D0
static u32 Cam_Flags2(void*) { return 2; }                              // 0x17E760, 0x2442E0
static void* SpecialCam_Dtor(void* self, int flag) {                    // 0x17E700, 0x244280
    if (self != nullptr && (s16)flag > 0)
        MemFree(self);
    return self;
}
static void* DeathCam_New(void* params);
static void* SpotCam_New(void* params);
static void DuelCam_Step(DuelCam* self, CamList* list) {                // 0x17EAD0
    void* who = P(self->target);
    if (!Cam_Duelling(who))
        CamList_Push(list, FollowCam_NewFor(who));
    else if (Cam_Dying(who))
        CamList_Push(list, DeathCam_New(who));
    else if (Cam_InSpot(who))
        CamList_Push(list, SpotCam_New(who));
}
GUEST_BIND(0x17E700, SpecialCam_Dtor);
GUEST_BIND(0x17E7E0, DuelCam_Compute);
GUEST_BIND(0x17E7D0, DuelCam_Attach);
GUEST_BIND(0x17E7C0, DuelCam_Distance);
GUEST_BIND(0x17EAD0, DuelCam_Step);
GUEST_BIND(0x17E760, Cam_Flags2);

// ── The death camera (0x244280–0x244910, vtable 0x2EC870, 0xB0 bytes): cuts
// to one of a few close views of the one going down — from the side of
// whoever struck — zoomed and a little tilted, and gives the picture back as
// it was a moment after. ───────────────────────────────────────────────────
struct DeathCam {
    u32 vtable;
    u32 target;             // +0x04  CharaParams*
    u32 striker;            // +0x08  CharaParams* (its +0xD8)
    u32 _0c;
    float eye[4];           // +0x10  in the frame below
    float lookUp;           // +0x20
    float zoom;             // +0x24
    float roll;             // +0x28
    u32 _2c;
    s32 frames;             // +0x30  to stay once the dying is over
    u32 _34[3];
    float saved[3][4];      // +0x40  the camera as it was
    float frame[16];        // +0x70  (its last row, +0xA0: where the one dying is)
};
static_assert(sizeof(DeathCam) == 0xB0, "DeathCam");
static void* DeathCam_New(void* params) {                               // 0x244910
    DeathCam* self = static_cast<DeathCam*>(MemAlloc(sizeof(DeathCam)));
    if (self == nullptr)
        return nullptr;
    memset(self, 0, sizeof *self);
    self->vtable = 0x2EC870u;
    self->target = guest::addr(params);
    self->striker = params != nullptr ? ParamAt(params, 0xD8) : 0;
    return self;
}
static void DeathCam_Attach(DeathCam* self, CamList* list) {            // 0x244410
    memcpy(self->saved, list->curTarget, sizeof self->saved);
    if (self->target != 0) {
        // Views of 0x20 bytes {eye[4], look up, zoom, roll}: two for one lying
        // (0x2DA2E0), three for one still on its feet (0x2DA320).
        const bool down = (Linked(P(self->target), 0xC8) & 0x100000) != 0;
        const float* view = down ? P<float>(0x2DA2E0u) + (Rand_Next() & 1) * 8 : P<float>(0x2DA320u) + (Rand_Next() % 3) * 8;
        Copy(self->eye, view);
        self->lookUp = view[4];
        self->zoom = view[5];
        self->roll = view[6];
        self->frames = down ? 0xF : 7;
        *P<float>(0x2EDD30u) = self->zoom;
        list->cuts = 1;
    }
    if (self->striker != 0) {
        const float* m = MatrixOf(P(self->target));
        float from[4];
        Sub(from, list->curEye, &m[12]);
        if (Dot(from, &m[0]) < 0.0f)
            self->eye[0] = -self->eye[0];
    }
    Matrix_Copy(self->frame, MatrixOf(P(self->target)));
}
static int DeathCam_Compute(DeathCam* self, CamList*, CamWish* wish) {  // 0x244650
    const float* me = MatrixOf(P(self->target));
    float* at = &self->frame[12];
    Copy(at, &me[12]);
    float look[4];
    if (self->striker == 0) {
        Set(look, 0.0f, 1.3f, 1.0f, 1.0f);
        Apply(look, self->frame, look);
    } else {
        const float* striker = MatrixOf(P(self->striker));
        float share = 0.5f;
        if (2.0f < Dist(&me[12], &striker[12])) {
            share = 0.1f;
            LookFrame(self->frame, &striker[12], &me[12]);
            Copy(at, &me[12]);
        }
        Inter(look, at, &striker[12], share);
        look[1] += self->lookUp;
    }
    Apply(wish->eye, self->frame, self->eye);
    Set(wish->pivot, at[0], 1.7f, at[2], 1.0f);
    Copy(wish->look, look);
    Copy(wish->nearLook, look);
    wish->roll = self->roll;
    return 1;
}
static float DeathCam_Distance(DeathCam*) { return 2.0f; }              // 0x244400
static void DeathCam_Detach(DeathCam* self, CamList* list) {            // 0x244360
    *P<float>(0x2EDD30u) = 1.0f;
    memcpy(list->target, self->saved, sizeof self->saved);
    CamList_Apply(list);
    list->cuts = 1;
}
static void DeathCam_Step(DeathCam* self, CamList* list) {              // 0x2447D0
    void* who = P(self->target);
    if (self->target != 0 && Cam_Dying(who))
        return;
    if (self->frames >= 1) {
        self->frames--;
        return;
    }
    CamList_Push(list, Cam_Duelling(who) ? DuelCam_New(who) : FollowCam_NewFor(who));
}
static const guest::Binder guest_bind_DeathCam_Dtor{0x244280, SpecialCam_Dtor, "DeathCam_Dtor"};
GUEST_BIND(0x244650, DeathCam_Compute);
GUEST_BIND(0x244410, DeathCam_Attach);
GUEST_BIND(0x244400, DeathCam_Distance);
GUEST_BIND(0x244360, DeathCam_Detach);
GUEST_BIND(0x2447D0, DeathCam_Step);
static const guest::Binder guest_bind_DeathCam_Flags{0x2442E0, Cam_Flags2, "DeathCam_Flags"};

// ── The spot camera (0x279FC0–0x27A520, vtable 0x2ECE40, 0xF0 bytes): in
// cells marked 0x40 the camera stops following.  It was made to cut to
// fixed viewpoints — a list of {eye, look} pairs at *0x2EE770 — but nothing
// in this release fills that list, so what it does is stay where it is and
// turn to keep its character in view; until it has settled it is a follow
// camera of its own (config 0x2E79C0: eight units back). ──────────────────
struct SpotCam {
    u32 vtable;
    u32 spot;               // +0x04  {float eye[4], look[4]}, or 0
    u8  settled;            // +0x08
    u8  _09[7];
    u32 target;             // +0x10  CharaParams*
    u32 _14[3];
    float _20[4];
    FollowCam follow;       // +0x30
};
static_assert(sizeof(SpotCam) == 0xF0, "SpotCam");
static void* SpotCam_New(void* params) {                                // 0x27A520
    SpotCam* self = static_cast<SpotCam*>(MemAlloc(sizeof(SpotCam)));
    if (self == nullptr)
        return nullptr;
    memset(self, 0, sizeof *self);
    self->vtable = 0x2ECE40u;
    FollowCam_Ctor(&self->follow, params, P(0x2E79C0u));
    self->target = guest::addr(params);
    Set(self->_20, 0.0f, 0.0f, 0.0f, 1.0f);
    return self;
}
static void* SpotCam_Dtor(SpotCam* self, int flag) {                    // 0x279FC0
    if (self != nullptr && (s16)flag > 0)
        MemFree(self);
    return self;
}
static void SpotCam_Attach(SpotCam* self, CamList* list) {              // 0x27A330
    FollowCam_Attach(&self->follow, list);
    list->cuts = 1;
    const float* me = MatrixOf(P(self->target));
    const u32 spots = *P<u32>(0x2EE770u);
    const s32 count = *P<s32>(0x2EE774u);
    self->spot = 0;
    for (s32 i = 0; i < count; i++) {
        const float* spot = P<float>(spots + (u32)i * 0x20);
        float to[4], view[4];
        Sub(to, &me[12], spot);
        Sub(view, spot + 4, spot);
        const float far = NormalizeLen(to, to);
        Normalize(view, view);
        if (Dot(to, view) >= 0.8f && far <= 18.0f) {
            self->spot = spots + (u32)i * 0x20;
            break;
        }
    }
}
static int SpotCam_Compute(SpotCam* self, CamList* list, CamWish* wish) {   // 0x27A140
    if (self->spot != 0) {
        const float* spot = P<float>(self->spot);
        Copy(wish->eye, spot);
        Copy(wish->look, spot + 4);
    } else {
        if (!self->settled)
            return FollowCam_Compute(&self->follow, list, wish);
        const float* me = MatrixOf(P(self->target));
        float to[4], view[4];
        Sub(to, &me[12], list->eye);
        to[1] = 0.0f;
        if (NormalizeLen(to, to) < 1.2f)
            return FollowCam_Compute(&self->follow, list, wish);
        Sub(view, list->target, list->eye);
        view[1] = 0.0f;
        Normalize(view, view);
        if (Dot(view, to) < 0.95f) {
            Set(wish->look, me[12], list->target[1], me[14], 1.0f);
            Inter(wish->look, wish->look, list->target, 0.05f);
        } else {
            Copy(wish->look, list->target);
        }
        Copy(wish->eye, list->eye);
    }
    Copy(wish->pivot, wish->eye);
    Copy(wish->nearLook, wish->look);
    return 1;
}
static float SpotCam_Distance(SpotCam* self) { return self->spot != 0 ? 0.0f : 7.0f; }     // 0x27A0B0
static void SpotCam_After(SpotCam* self, CamList* list) {               // 0x27A070
    FollowCam_After(&self->follow, list);
    self->settled = 1;
}
static void SpotCam_Recentre(SpotCam* self) { FollowCam_Recentre(&self->follow); }         // 0x27A050
static void SpotCam_Step(SpotCam* self, CamList* list) {                // 0x27A470
    void* who = P(self->target);
    const u32 map = list->map;
    if (map != 0) {
        const u32 flags = Map_CellAt(P<MapCollision>(map), &MatrixOf(who)[12])->flags;
        if ((flags & 0x40) != 0 || (flags & 4) != 0)
            return;
    }
    CamList_Push(list, FollowCam_NewFor(who));
}
GUEST_BIND(0x279FC0, SpotCam_Dtor);
GUEST_BIND(0x27A140, SpotCam_Compute);
GUEST_BIND(0x27A330, SpotCam_Attach);
GUEST_BIND(0x27A0B0, SpotCam_Distance);
GUEST_BIND(0x27A070, SpotCam_After);
GUEST_BIND(0x27A050, SpotCam_Recentre);
GUEST_BIND(0x27A470, SpotCam_Step);

extern "C" {

// ── 0x152470 ──────────────────────────────────────────────────────────────
void* FollowCam_Ctor(void* mem, void* target, const void* config) {
    FollowCam* self = static_cast<FollowCam*>(mem);
    self->vtable = kVtFollowCam;
    memcpy(&self->config, config, sizeof(FollowConfig));
    self->tilt = 0.0f;
    self->mode = 1;
    self->target = guest::addr(target);
    Set(self->_b0, 0.0f, 0.0f, 0.0f, 1.0f);
    return mem;
}

// ── 0x151F90 ──────────────────────────────────────────────────────────────
int FollowCam_Compute(FollowCam* self, CamList*, CamWish* wish) {
    self->mode = *reinterpret_cast<u32*>(*gConfig + 0x10);
    if (self->target == 0)
        return 0;
    const CharaParams* who = P<CharaParams>(self->target);
    void* pads = PadMgr_Get();
    if (Pad_GetHeld(pads, 0) & 0x400) {                 // R3: back behind
        Copy(self->anchor, self->config.eye);
        Apply(self->anchor, Follow_Matrix(self), self->anchor);
        self->tilt = 0.0f;
    }
    Apply(wish->look, who->matrix, self->config.look);
    Apply(wish->nearLook, who->matrix, self->config.nearLook);
    // The frame: at the character, looking the way the camera trails it.
    alignas(16) float frame[16];
    Matrix_Identity(frame);
    Copy(&frame[12], &who->matrix[12]);
    Sub(&frame[8], &frame[12], self->anchor);
    frame[9] = 0.0f;
    Normalize(&frame[8], &frame[8]);
    Cross(&frame[0], &frame[4], &frame[8]);

    u8 sx, sy;
    Pad_GetRightStick(pads, 0, &sx, &sy);
    float swing = -(float)StickAxis(sx) / 128.0f * 0.1f;
    const float tilt = -(float)StickAxis(sy) / 128.0f * 0.1f;
    if (swing == 0.0f && tilt == 0.0f) {
        self->stick = 0;
    } else if (self->stick == 0) {
        Copy(self->stickFrom, &frame[12]);
        self->stick = 1;
    }
    if (self->stick == 0 && 0.25f < Dist(&frame[12], self->stickFrom))
        self->tilt = 0.0f;
    if ((self->mode & 1) == 0)
        self->tilt = tilt * 8.5f;
    else
        self->tilt += tilt / 2.0f;
    if (self->tilt < self->config.tiltMin)
        self->tilt = self->config.tiltMin;
    else if (self->config.tiltMax < self->tilt)
        self->tilt = self->config.tiltMax;
    if (self->mode & 2)
        swing = -swing;
    alignas(16) float turn[16];
    Matrix_Identity(turn);
    RotateX(turn, turn, self->tilt);
    Matrix_Rotate(turn, turn, swing);
    Apply(wish->eye, turn, self->config.eye);
    Apply(wish->eye, frame, wish->eye);
    Apply(wish->pivot, turn, self->config.pivot);
    Apply(wish->pivot, frame, wish->pivot);
    return 1;
}

// ── 0x152420: takes over from where the camera is ─────────────────────────
void FollowCam_Attach(FollowCam* self, CamList* list) {
    float v[4];
    Sub(v, list->eye, list->target);
    Add(self->anchor, &Follow_Matrix(self)[12], v);
    *P<float>(0x2EDD30u) = 1.0f;
}
// ── 0x151F60: how far from the pivot it insists on being ──────────────────
float FollowCam_Distance(FollowCam* self) { return self->stick == 0 ? 2.5f : 0.5f; }
// ── 0x151F50: the eye the list settled on is where it trails from next ────
void FollowCam_After(FollowCam* self, CamList* list) { Copy(self->anchor, list->eye); }
// ── 0x151ED0: start over, seeing the character from its side ──────────────
void FollowCam_Recentre(FollowCam* self) {
    alignas(16) float turn[16];
    Copy(self->anchor, self->config.eye);
    Matrix_Identity(turn);
    Matrix_Rotate(turn, turn, -1.5707964f);
    Apply(self->anchor, turn, self->anchor);
    Apply(self->anchor, Follow_Matrix(self), self->anchor);
    self->tilt = 0.0f;
}
// ── 0x1522E0: hands over to a special camera when its character duels
// (0x17E770 → the duel camera), stands in a cell marked for a still camera
// (0x27A0E0 → the spot camera) or is dying (0x2442F0 → the death camera).
// (Pushing a controller destroys this one.) ────────────────────────────────
void FollowCam_Step(FollowCam* self, CamList* list) {
    void* who = P(self->target);
    if (Cam_Duelling(who)) {
        CamList_Push(list, DuelCam_New(who));
    } else if (Cam_InSpot(who)) {
        CamList_Push(list, SpotCam_New(who));
    } else if (Cam_Dying(who)) {
        CamList_Push(list, DeathCam_New(who));
    }
}
// ── 0x151EC0 ──────────────────────────────────────────────────────────────
u32 FollowCam_Flags(FollowCam*) { return 1; }

GUEST_BIND(0x151F90, FollowCam_Compute);
GUEST_BIND(0x152420, FollowCam_Attach);
GUEST_BIND(0x151F60, FollowCam_Distance);
GUEST_BIND(0x151F50, FollowCam_After);
GUEST_BIND(0x151ED0, FollowCam_Recentre);
GUEST_BIND(0x1522E0, FollowCam_Step);
GUEST_BIND(0x151EC0, FollowCam_Flags);

}  // extern "C"

// ─── The camera on two people talking ─────────────────────────────────────
// Looks at the point half way between them from a little above, staying
// between 30° and 150° off the line that joins them.
struct TalkCam {            // 0x30 bytes
    u32 vtable;
    u32 speaker, listener;  // +0x04  CharaParams*
    float tilt;             // +0x0C
    float heading;          // +0x10
    float height;           // +0x14
    u8  moved;              // +0x18
    u8  _19[7];
    float from[4];          // +0x20
};

// ── 0x18BE30 ──────────────────────────────────────────────────────────────
static float Talk_Heading(float heading, float line) {
    float off = WrapAngle(heading - line);
    const bool positive = 0.0f < off;
    if (!positive)
        off = -off;
    if (2.6179938f < off)
        off = 2.6179938f - off;
    else if (off < 0.5235988f)
        off = 0.5235988f - off;
    else
        off = 0.0f;
    return WrapAngle(positive ? heading + off : heading - off);
}
static float ViewHeading() {
    if (*gCamList == 0)
        return 0.0f;
    const CamList* l = P<CamList>(*gCamList);
    return Heading(l->curTarget[0] - l->curEye[0], l->curTarget[2] - l->curEye[2]);
}

extern "C" {

// ── 0x18BFA0 ──────────────────────────────────────────────────────────────
void* TalkCam_Ctor(void* mem) {
    TalkCam* self = static_cast<TalkCam*>(mem);
    self->vtable = kVtTalkCam;
    self->speaker = 0;
    self->listener = 0;
    return mem;
}
// ── 0x18BF60 ──────────────────────────────────────────────────────────────
void TalkCam_Set(float height, float degrees, void* cam, CharaParams* a, CharaParams* b) {
    TalkCam* self = static_cast<TalkCam*>(cam);
    self->speaker = guest::addr(a);
    self->listener = guest::addr(b);
    self->height = height;
    self->tilt = degrees * 3.1415927f / 180.0f;
}

// ── 0x18BC10 ──────────────────────────────────────────────────────────────
int TalkCam_Compute(TalkCam* self, CamList* list, CamWish* wish) {
    const u32 speaker = self->speaker;
    u32 other = self->listener;
    if (other == 0)
        other = speaker ? P<CharaParams>(speaker)->link[3] : 0;
    bool follow = other == 0;
    if (!follow) {
        const CharaParams* a = P<CharaParams>(speaker);
        const CharaParams* b = P<CharaParams>(other);
        float half[4];
        Sub(half, &b->matrix[12], &a->matrix[12]);
        for (int i = 0; i < 4; i++)
            half[i] *= 0.5f;
        const float apart = sqrtf(Dot(half, half));
        self->heading = Talk_Heading(self->heading, Heading(half[0], half[2]));
        if (self->moved == 0 || Dist(self->from, list->curEye) <= 1.0f) {
            const float back = 4.0f <= apart * 2.4f ? apart * 2.4f : 4.0f;
            RotateX(wish->frame, kIdentity, self->tilt);
            Matrix_Rotate(wish->frame, wish->frame, self->heading);
            Add(&wish->frame[12], &a->matrix[12], half);
            wish->frame[13] += self->height;
            wish->frame[15] = 1.0f;
            Set(wish->eye, 0.0f, 0.0f, -back, 1.0f);
            return 1;
        }
        follow = true;
    }
    // Nobody to look at any more: back to following the speaker.  (This
    // destroys the talk camera.)
    void* mem = MemAlloc(0xC0);
    void* cam = mem ? FollowCam_Ctor(mem, P(speaker), P(0x2E0020u)) : nullptr;
    CamList_Push(list, cam);
    return 0;
}
// ── 0x18BF00 / 0x18BBB0 ───────────────────────────────────────────────────
void TalkCam_Attach(TalkCam* self, CamList*) {
    self->moved = 0;
    self->heading = ViewHeading();
}
void TalkCam_After(TalkCam* self, CamList*) { self->heading = ViewHeading(); }
// ── 0x18BC00 ──────────────────────────────────────────────────────────────
float TalkCam_Distance(TalkCam*) { return 1.5f; }

GUEST_BIND(0x18BC10, TalkCam_Compute);
GUEST_BIND(0x18BF00, TalkCam_Attach);
GUEST_BIND(0x18BBB0, TalkCam_After);
GUEST_BIND(0x18BC00, TalkCam_Distance);

}  // extern "C"

static const struct CamBinds {
    CamBinds() {
        for (u32 address : {0x151430u, 0x151E60u, 0x18BB50u})
            guest::bind(address, Cam_Dtor, "Cam_Dtor");
        for (u32 address : {0x1506A0u, 0x1507A0u, 0x150E50u})
            guest::bind(address, Cam_Nothing, "Cam_Nothing");
    }
} sCamBinds;

// ═══ The list's own life, and the stage ═══════════════════════════════════
extern "C" {

// ── 0x151060 ──────────────────────────────────────────────────────────────
void* CamList_Ctor(void* mem) {
    CamList* self = static_cast<CamList*>(mem);
    EaseHolder_Ctor(&self->ease);
    *gCamList = guest::addr(self);
    self->map = 0;
    Set(self->target, 0.0f, 0.0f, 1.0f, 1.0f);
    Set(self->eye, 0.0f, 0.0f, 0.0f, 1.0f);
    Set(self->up, 0.0f, 1.0f, 0.0f, 1.0f);
    Set(self->curTarget, 0.0f, 1.0f, 1.0f, 1.0f);
    Set(self->curEye, 0.0f, 1.0f, -2.0f, 1.0f);
    Set(self->curUp, 0.0f, 1.0f, 0.0f, 1.0f);
    self->cuts = 0;
    CamState* s = static_cast<CamState*>(MemAlloc(0x50));
    if (s != nullptr)
        s->controller = 0;
    self->state = guest::addr(s);
    s->turnFrames = 0;
    s->shake = 0;
    s->shakeAmp = 0.0f;
    s->shakeStep = 0.0f;
    u32* free = static_cast<u32*>(MemAlloc(0x50));
    if (free != nullptr)
        *free = kVtFreeCam;
    CamList_Push(self, free);
    return mem;
}

// ── 0x150FC0 ──────────────────────────────────────────────────────────────
void* CamList_Dtor(void* mem, int flag) {
    CamList* self = static_cast<CamList*>(mem);
    if (self == nullptr)
        return mem;
    if (self->state != 0) {
        CamState* s = State(self);
        if (s->controller != 0)
            VirtualCall<void*, void*, int>(Vt(P(s->controller)), kCamDtor, P(s->controller), 1);
        MemFree(s);
    }
    *gCamList = 0;
    EaseHolder_Dtor(&self->ease);
    if (flag > 0)
        MemFree(self);
    return mem;
}

// ── 0x1654B0 ──────────────────────────────────────────────────────────────
void* Stage_Ctor(void* mem) {
    Stage* self = static_cast<Stage*>(mem);
    CamList_Ctor(&self->cams);
    Copy(self->nextEye, P<const float>(0x306900u));
    Copy(self->nextUp, P<const float>(0x2D1A90u));
    Copy(self->nextDir, P<const float>(0x2D1AA0u));
    memcpy(self->eye, self->nextEye, 0x30);
    return mem;
}
// ── 0x165450 ──────────────────────────────────────────────────────────────
void* Stage_Dtor(void* mem, int flag) {
    if (mem != nullptr) {
        CamList_Dtor(&static_cast<Stage*>(mem)->cams, -1);
        if (flag > 0)
            MemFree(mem);
    }
    return mem;
}
// ── 0x1653E0 ──────────────────────────────────────────────────────────────
void Stage_Update(void* mem) {
    Stage* self = static_cast<Stage*>(mem);
    if (CamList_Step(&self->cams) != 0)
        CamList_Get(&self->cams, self->nextEye, self->nextDir, self->nextUp);
    memcpy(self->eye, self->nextEye, 0x30);
}
// ── 0x165430: the world → view matrix (0x107738, sceVu0CameraMatrix) ──────
void Stage_Apply(void* mem, void* matrix) {
    const Stage* self = static_cast<const Stage*>(mem);
    alignas(16) float m[16];
    float x[4], y[4], z[4];
    Cross(x, self->up, self->dir);
    Normalize(x, x);
    Normalize(z, self->dir);
    Cross(y, z, x);
    // The inverse of the camera's own placement {x, y, z, eye}.
    const float* e = self->eye;
    Set(&m[0], x[0], y[0], z[0], 0.0f);
    Set(&m[4], x[1], y[1], z[1], 0.0f);
    Set(&m[8], x[2], y[2], z[2], 0.0f);
    Set(&m[12], -Dot(e, x), -Dot(e, y), -Dot(e, z), 1.0f);
    memcpy(matrix, m, sizeof m);
}

}  // extern "C"
