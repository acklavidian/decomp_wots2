// AI moves — decompiled from 0x18BFD0–0x18D350 (the factory and the walking
// base), 0x155120–0x155210 (standing), and the small classes at 0x21ABE0,
// 0x220C40, 0x222990, 0x2683E0, 0x275420 and 0x279E30, with the queries they
// make at 0x175210–0x175940.
//
// Mode 7 (the onlooker, 0x278390–0x279D80) is here too; modes 9 and 10 and
// the fight (mode 1) are in ai_fight.cpp.

#include "game/ai_moves.h"
#include "game/chara_mgr.h"
#include "game/map_collision.h"
#include "game/map_db.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    int   Rand_Next();                                  // 0x1200F0
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Matrix_Identity(void* m);                     // 0x107428
    void  Matrix_Rotate(void* dst, const void* src, float angle);   // 0x107618
    void  Matrix_FaceTowards(float* matrix, const float* point);    // 0x1C8DB0
    u32   MapState_GetMap(void* self);                  // 0x14B640
    int   Collision_CanSee(int idA, int idB, int a);    // 0x175600
    u32   AiFight_Create(const void* order);            // 0x18E630
    u32   AiWait_Create();                              // 0x26A150
    u32   AiHold_Create();                              // 0x274410
}

static_assert(sizeof(AiWalk) == 0xD4 && offsetof(AiWalk, path) == 0xC0 && offsetof(AiWalk, target) == 0x80, "AiWalk");

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);
static u32* const gCharaMgr = guest::ptr<u32>(0x2EE5C4u);
static u32* const gMapState = guest::ptr<u32>(0x2EE5C8u);

enum : u32 {
    kVtMove = 0x2EAD40u, kVtStand = 0x2EAD20u, kVtWalk = 0x2EB230u, kVtToPoint = 0x2EC380u, kVtToChara = 0x2EC580u,
    kVtPath = 0x2EC4B0u, kVtLeave = 0x2ECBC0u, kVtFollow = 0x2ECDD0u, kVtWalkIn = 0x2ECE20u,
};
enum : u32 { kMoveStart = 0x0C };

static inline CharaParams* Params(CharaBody* b) { return P<CharaParams>(b->params); }
static inline float* Position(CharaBody* b) { return &Params(b)->matrix[12]; }
static inline void Copy(float* d, const float* s) { memcpy(d, s, 16); }
static float FlatDistance(const float* a, const float* b) {
    const float dx = a[0] - b[0], dz = a[2] - b[2], dw = a[3] - b[3];
    return sqrtf(dx * dx + dz * dz + dw * dw);
}

// ═══ Queries ══════════════════════════════════════════════════════════════

// ── 0x1C8BC0 ──────────────────────────────────────────────────────────────
float Matrix_FacingCosine(const float* matrix, const float* point) {
    float to[3] = {point[0] - matrix[12], 0.0f, point[2] - matrix[14]};
    const float length = sqrtf(to[0] * to[0] + to[2] * to[2]);
    if (length != 0.0f) {
        to[0] /= length;
        to[2] /= length;
    }
    return matrix[8] * to[0] + matrix[10] * to[2];
}

// ── 0x175510 / 0x175580 ───────────────────────────────────────────────────
const float* Query_LineBlocked(const float* from, const float* to, int) {
    if (*gCharaMgr == 0 || *gMapState == 0)
        return nullptr;
    return Map_LineBlocked(P<MapCollision>(MapState_GetMap(P(*gMapState))), from, to, 0);
}
const float* Query_LineBlockedFrom(int id, const float* to, int) {
    if (*gCharaMgr == 0 || *gMapState == 0)
        return nullptr;
    MapCollision* map = P<MapCollision>(MapState_GetMap(P(*gMapState)));
    return Map_LineBlocked(map, CharaMgr_GetPosition(P<CharaMgr>(*gCharaMgr), id), to, 0);
}
// ── 0x175940 ──────────────────────────────────────────────────────────────
float Query_Distance(int idA, int idB) {
    if (*gCharaMgr == 0)
        return 0.0f;
    CharaMgr* mgr = P<CharaMgr>(*gCharaMgr);
    return Vec_Distance(CharaMgr_GetPosition(mgr, idA), CharaMgr_GetPosition(mgr, idB));
}

static Chara* FindChara(CharaMgr* mgr, int id) {                // 0x145F90
    for (GuestListNode* n = GuestList_Begin(&mgr->charas); n != GuestList_End(&mgr->charas); n = GuestList_Next(n))
        if (P<Chara>(n->value)->id == id)
            return P<Chara>(n->value);
    return nullptr;
}
// Is `other` near `self` and in front of it?
static bool Ahead(Chara* self, Chara* other, float within, float cosine) {
    if (other == self || other->active == 0)
        return false;
    const CharaParams* a = P<CharaParams>(self->params);
    const CharaParams* b = P<CharaParams>(other->params);
    return Vec_Distance(&a->matrix[12], &b->matrix[12]) < within && cosine < Matrix_FacingCosine(a->matrix, &b->matrix[12]);
}
// ── 0x175370 ──────────────────────────────────────────────────────────────
// (The original first asks an object at *0x2EE5CC whether the character is
// in a zone of kind 3 or 4, and says "yes" there; that object is not
// decompiled and does not exist yet.)
int Query_SomeoneAhead(float within, float cosine, int id) {
    CharaMgr* mgr = P<CharaMgr>(*gCharaMgr);
    Chara* self = FindChara(mgr, id);
    if (!self)
        return -1;
    for (GuestListNode* n = GuestList_Begin(&mgr->charas); n != GuestList_End(&mgr->charas); n = GuestList_Next(n))
        if (Ahead(self, P<Chara>(n->value), within, cosine))
            return 1;
    return 0;
}

// ═══ Standing (0x155120–0x155210; modes 0 and anything unknown) ═══════════
struct AiStand { AiMove base; s32 _08; u32 _0c; s32 _10; u8 _14[0x4C]; };

static AiMove* Move_Dtor(AiMove* self, s16 flag) {              // 0x155210
    if (self) {
        self->vtable = kVtMove;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
static AiMove* Stand_Dtor(AiMove* self, s16 flag) {             // 0x155190
    if (self)
        self->vtable = kVtStand;
    return Move_Dtor(self, flag);
}
static int Stand_Start(AiStand* self) {                         // 0x155170
    self->base.kind = 0;
    self->_08 = 0;
    self->_10 = 0;
    return 0;
}
static int Move_Nothing(AiMove*, CharaBody*) { return 0; }      // 0x155160, 0x155120 and their copies
static int Stand_Result(AiMove*, CharaBody* body) {             // 0x155130
    if (body)
        CharaBody_Idle(body);
    return 0;
}

// ═══ Walking (0x18C1A0–0x18CFE0) ══════════════════════════════════════════

// ── 0x18CFE0 ──────────────────────────────────────────────────────────────
static AiWalk* Walk_Ctor(AiWalk* self, u32 route, u32 links, u32 routes) {
    memset(self, 0, sizeof(AiWalk));
    self->base.vtable = kVtWalk;
    self->route = route;
    self->links = links;
    self->routes = routes;
    self->viaRoute = -1;
    self->atPoint = 1;
    self->replan = 1;
    self->findEnd = 1;
    self->stopDistance = self->stopNow = 0.8f;
    self->avoidOthers = 1;
    for (float* v : {self->lastCheck, self->target, self->heading, self->from})
        v[3] = 1.0f;
    self->_3c = -1;
    return self;
}
// ── 0x18CF40 ──────────────────────────────────────────────────────────────
static AiMove* Walk_DtorBase(AiWalk* self, s16 flag) {
    if (!self)
        return nullptr;
    self->base.vtable = kVtWalk;
    self->path.count = 0;
    if (self->path.data != 0)
        MemFree(P(self->path.data));
    return Move_Dtor(&self->base, flag);
}
static AiMove* Walk_Dtor(AiWalk* self, s16 flag) {              // 0x21ACD0 and its copies
    if (!self)
        return nullptr;
    Walk_DtorBase(self, 0);
    if (flag > 0)
        MemFree(self);
    return &self->base;
}

// ── 0x18CBB0 ──────────────────────────────────────────────────────────────
// The route points to pass, from the one nearest itself to the one nearest
// the goal, read off the table of hops.
static void BuildPath(AiWalk* self) {
    self->path.count = 0;
    GuestIntMap* routes = P<GuestIntMap>(self->routes);
    GuestMapNode* to = GuestIntMap_Find(routes, self->nearGoal);
    if (to == GuestIntMap_End(routes))
        return;
    GuestIntMap* hops = reinterpret_cast<GuestIntMap*>(&to->value);
    GuestMapNode* hop = GuestIntMap_Find(hops, self->nearSelf);
    if (hop == GuestIntMap_End(hops)) {
        if (self->nearSelf != -1) {
            VectorU32_PushBack(&self->path, (u32)self->nearSelf);
            VectorU32_PushBack(&self->path, (u32)self->nearSelf);
        }
        return;                     // (and a debug message: no way there)
    }
    VectorU32_PushBack(&self->path, (u32)hop->key);
    while (hop->key != (s32)hop->value) {
        hop = GuestIntMap_Find(hops, (s32)hop->value);
        if (hop == GuestIntMap_End(hops))
            return;
        VectorU32_PushBack(&self->path, (u32)hop->key);
    }
}

// The route point nearest a place that the place can see, or −1.
static s32 NearestPoint(const PointSet* route, const float* place) {
    s32 nearest = -1;
    float least = 1000.0f;
    const float* points = P<float>(route->points);
    for (s32 i = 0; i < route->count; i++) {
        const float d = Vec_Distance(place, points + i * 4);
        if (d < least && Query_LineBlocked(place, points + i * 4, 2) == nullptr) {
            nearest = i;
            least = d;
        }
    }
    return nearest;
}

// ── 0x18CD50 ──────────────────────────────────────────────────────────────
static int Plan(AiWalk* self, CharaBody* body) {
    if (self->route == 0) {
        self->_48 = 0;
        return -1;
    }
    const PointSet* route = P<PointSet>(self->route);
    const float* goal = P<float>(self->goal);
    float to[4];
    for (int i = 0; i < 4; i++)
        to[i] = goal[i] + self->goalOffset[i];
    if (self->findEnd) {
        self->nearGoal = NearestPoint(route, to);
        self->findEnd = 0;
        if (self->nearGoal != -1)
            Copy(self->target, P<float>(route->points) + self->nearGoal * 4);
    }
    if (body)
        Copy(self->from, Position(body));
    self->nearSelf = NearestPoint(route, self->from);
    self->_4c = self->nearSelf;
    self->pathAt = 0;
    BuildPath(self);
    return 0;
}

// ── 0x18C1A0 ──────────────────────────────────────────────────────────────
// Walk at what it is heading for.  → true when it has reached the goal itself.
static bool Advance(AiWalk* self, CharaBody* body) {
    self->stopNow = self->viaRoute == 1 ? 0.8f : self->stopDistance;
    float here[4], there[4];
    Copy(here, Position(body));
    Copy(there, self->target);
    here[1] = there[1] = 0.0f;
    if (Vec_Distance(here, there) < self->stopNow) {
        CharaBody_Idle(body);
        self->atPoint = 1;
        return self->viaRoute == 0;
    }
    self->atPoint = 0;
    CharaBody_Walk(body, self->run);
    if (self->avoiding == 0) {
        alignas(16) float m[16];
        Matrix_Copy(m, Params(body)->matrix);
        Matrix_FaceTowards(m, self->target);
        Copy(self->heading, &m[8]);
        Copy(body->input, self->heading);
    }
    return false;
}

// A route point, with a little scatter so that people do not walk in file.
static void HeadFor(AiWalk* self, const float* point) {
    float to[4];
    Copy(to, point);
    const int degrees = Rand_Next() % 180;
    const int side = Rand_Next() & 1;
    float angle = (float)degrees * 3.1415927f / 180.0f;
    if (side == 0)
        angle = -angle;
    const int reach = Rand_Next() % 450 + 1;
    alignas(16) float m[16];
    Matrix_Rotate(m, P<float>(0x2D1A80u), angle);
    const float scale = (float)reach / 1000.0f;
    for (int i = 0; i < 4; i++)
        to[i] += m[8 + i] * scale;
    Copy(self->target, to);
}

// ── 0x18C2B0 / 0x18C440 ───────────────────────────────────────────────────
// Someone is in the way: turn 45° aside for three quarters of a unit.
static void Avoid(AiWalk* self, CharaBody* body) {
    CharaParams* p = Params(body);
    self->crowded = Query_SomeoneAhead(1.0f, 0.65f, p->id);
    if (self->crowded == 1 && self->avoiding == 0) {
        self->avoiding = 1;
        // Of those drawn this frame who are near and ahead, the one most to
        // the side decides which way: away from it.
        CharaMgr* mgr = P<CharaMgr>(*gCharaMgr);
        Chara* me = FindChara(mgr, p->id);
        float least = 10.0f;
        int turn = 45;
        if (me) {
            const float* m = p->matrix;
            for (GuestListNode* n = GuestList_Begin(&mgr->drawn); n != GuestList_End(&mgr->drawn); n = GuestList_Next(n)) {
                Chara* other = P<Chara>(n->value);
                if (!Ahead(me, other, 1.0f, 0.65f))
                    continue;
                const float* at = &P<CharaParams>(other->params)->matrix[12];
                const float d[3] = {at[0] - m[12], at[1] - m[13], at[2] - m[14]};
                const float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const float forward = (m[8] * d[0] + m[9] * d[1] + m[10] * d[2]) / length;
                const float right = (m[0] * d[0] + m[1] * d[1] + m[2] * d[2]) / length;
                if (forward < least) {
                    least = forward;
                    turn = right > 0.0f ? -45 : 45;
                }
            }
        }
        alignas(16) float r[16];
        Matrix_Identity(r);
        Matrix_Rotate(r, r, (float)turn * 3.1415927f / 180.0f);
        const float* f = &p->matrix[8];
        for (int j = 0; j < 4; j++)
            body->input[j] = f[0] * r[j] + f[1] * r[4 + j] + f[2] * r[8 + j] + f[3] * r[12 + j];
        Copy(self->avoidStart, Position(body));
    }
    if (self->avoiding == 1 && Vec_Distance(Position(body), self->avoidStart) > 0.75f)
        self->avoiding = 0;
}

// ── 0x18C720 ──────────────────────────────────────────────────────────────
// One frame of finding the way: straight for the goal while it can be seen,
// else from route point to route point.
static int Walk_Step(AiWalk* self, CharaBody* body) {
    CharaParams* p = Params(body);
    const PointSet* route = P<PointSet>(self->route);
    const float* goal = P<float>(self->goal);
    float to[4];
    for (int i = 0; i < 4; i++)
        to[i] = goal[i] + self->goalOffset[i];
    const bool hidden = Query_LineBlockedFrom(p->id, to, 2) != nullptr;
    if (self->replan) {
        Copy(self->from, &p->matrix[12]);
        Plan(self, nullptr);
        self->replan = 0;
        self->atPoint = 1;
        self->avoiding = 0;
    }
    const u32* path = P<u32>(self->path.data);
    const float* points = route ? P<float>(route->points) : nullptr;
    // A corner can be cut when the point after next is already in sight.
    if (hidden && self->atPoint == 0 && self->pathAt + 1 < self->path.count) {
        const float* next = points + path[self->pathAt + 1] * 4;
        if (Query_LineBlockedFrom(p->id, next, 2) == nullptr) {
            self->pathAt++;
            HeadFor(self, next);
        }
    }
    if (!hidden) {
        self->viaRoute = 0;
        Copy(self->target, to);
    } else if (self->atPoint != 0) {
        self->viaRoute = 1;
        if (self->pathAt < self->path.count)
            HeadFor(self, points + path[self->pathAt++] * 4);
        else
            self->replan = 1;
    }
    if (self->atPoint == 0) {
        // Standing on a blocked cell, or not getting anywhere: think again.
        if (p->link[2] != 0) {
            const u32 cell = P<u32>(p->link[2])[1];
            if ((cell & 2) && (cell & 1) == 0)
                self->replan = 1;
        }
        if (self->stuckTimer++ >= *gFrameRate >> 1) {
            if (Vec_Distance(self->lastCheck, Position(body)) < 0.25f) {
                self->stuck = 1;
                self->replan = 1;
            } else {
                self->stuck = 0;
            }
            self->stuckTimer = 0;
            Copy(self->lastCheck, Position(body));
        }
    }
    if (self->avoidOthers)
        Avoid(self, body);
    return 0;
}
static int Walk_StepV(AiWalk* self, CharaBody* body) {          // 0x21AC30 and its copies
    if (body)
        Walk_Step(self, body);
    return 0;
}
static int Walk_Result(AiWalk* self, CharaBody* body) {         // 0x21ABF0 and its copies
    return body && Advance(self, body);
}

// ── Mode 2: to a place (0x21ABE0–0x21AD30) ────────────────────────────────
struct AiToPoint {
    AiWalk walk;
    u8    _d4[0xC];
    u32   route;            // +0xE0
    u8    _e4[0xC];
    float place[4];         // +0xF0
    float stop;             // +0x100
    s32   run;              // +0x104
    u8    avoid;            // +0x108
};
static int ToPoint_Start(AiToPoint* self) {                     // 0x21AC70
    self->walk.stopDistance = self->stop;
    self->walk.avoidOthers = self->avoid;
    self->walk.findEnd = self->walk.replan = 1;
    self->walk.goal = guest::addr(self->place);
    self->walk.run = self->run != 0;
    Plan(&self->walk, nullptr);
    return 0;
}
static AiMove* ToPoint_Ctor(void* mem, const u32* data) {       // 0x21AD30
    AiToPoint* self = static_cast<AiToPoint*>(mem);
    Walk_Ctor(&self->walk, data[0], data[1], data[2]);
    self->walk.base.vtable = kVtToPoint;
    self->route = data[0];
    memcpy(&self->stop, &data[8], 4);
    self->run = (s32)data[9];
    self->avoid = (u8)data[10];
    self->walk.base.kind = 2;
    memcpy(self->place, &data[4], 16);
    return &self->walk.base;
}

// ── Mode 3: to someone (0x222990–0x222C00) ────────────────────────────────
struct AiToChara {
    AiWalk walk;
    u8    _d4[0xC];
    s32   run;              // +0xE0
    float lastDistance;     // +0xE4
    u8    _e8[8];
    float lastPlace[4];     // +0xF0  where they were when the way was planned
    u32   route;            // +0x100
    u8    _104[8];
    u32   who;              // +0x10C  CharaParams*
    float stop;             // +0x110
    u32   _114;
    s32   pace;             // +0x118  0 walk, 1 run, −1: run when far
    u8    avoid;            // +0x11C
};
// Walk when within three units, run beyond (looked at again each time the
// distance has changed by a unit).
template <typename T> static void PickPace(T* self, float distance) {
    if (fabsf(distance - self->lastDistance) < 1.0f)
        return;
    const s32 pace = (self->run == 1 || distance <= 3.0f) ? 0 : 1;
    self->lastDistance = distance;
    self->run = self->pace == -1 ? pace : self->pace;
    self->walk.run = self->run;
}
static int ToChara_Start(AiToChara* self) {                     // 0x222B40
    const float* where = &P<CharaParams>(self->who)->matrix[12];
    self->walk.avoidOthers = self->avoid;
    self->walk.findEnd = self->walk.replan = 1;
    self->walk.goal = guest::addr(where);
    self->walk.stopDistance = self->stop;
    Plan(&self->walk, nullptr);
    Copy(self->lastPlace, where);
    return 0;
}
static int ToChara_Step(AiToChara* self, CharaBody* body) {     // 0x2229E0
    if (!body)
        return 0;
    const float* where = &P<CharaParams>(self->who)->matrix[12];
    // They have moved out of sight of where they were: plan afresh.
    if (Vec_Distance(self->lastPlace, where) > 1.0f && Query_LineBlocked(self->lastPlace, where, 2) != nullptr)
        VirtualCall<int, AiToChara*, int>(self->walk.base.vtable, kMoveStart, self, 0);
    PickPace(self, Vec_Distance(Position(body), where));
    Walk_Step(&self->walk, body);
    return 0;
}
static AiMove* ToChara_Ctor(void* mem, const u32* data) {       // 0x222C00
    AiToChara* self = static_cast<AiToChara*>(mem);
    Walk_Ctor(&self->walk, data[0], data[1], data[2]);
    self->walk.base.vtable = kVtToChara;
    self->route = data[0];
    self->who = data[3];
    self->pace = (s32)data[6];
    self->avoid = (u8)data[7];
    memcpy(&self->stop, &data[4], 4);
    self->run = self->pace == -1 ? 0 : self->pace;
    self->lastDistance = 0.0f;
    self->walk.base.kind = 3;
    return &self->walk.base;
}

// ── Mode 4: wandering between the points of a path (0x220C40–0x220E70) ────
struct AiPath {
    AiWalk walk;
    u8    _d4[0xC];
    s32   index;            // +0xE0  the point it is going to
    s32   lastIndex;        // +0xE4
    u32   _e8;
    s32   wait;             // +0xEC  frames to stand there
    s32   waiting;          // +0xF0
    u32   route;            // +0xF4
    u8    _f8[8];
    s32   count;            // +0x100
    u32   points;           // +0x104 float[4][]
    s32   run;              // +0x108
    u8    avoid;            // +0x10C
};
static int Path_Start(AiPath* self) {                           // 0x220DA0
    self->walk.avoidOthers = self->avoid;
    self->walk.findEnd = self->walk.replan = 1;
    self->walk.goal = self->points + (u32)self->index * 0x10;
    self->walk.run = self->run != 0;
    self->wait = 0;
    self->waiting = 0;
    Plan(&self->walk, nullptr);
    return 0;
}
// It never finishes: at each point it waits up to two seconds and picks
// another.
static int Path_Result(AiPath* self, CharaBody* body) {         // 0x220C50
    if (!body || !Advance(&self->walk, body))
        return 0;
    if (self->waiting == 0) {
        do {
            self->index = Rand_Next() % self->count;
        } while (self->index == self->lastIndex && self->count > 1);    // (the original has no way out with one point)
        self->lastIndex = self->index;
        self->wait = (*gFrameRate / 2) * (Rand_Next() % 5);
        self->waiting = 1;
    }
    if (self->wait-- < 1) {
        self->walk.findEnd = self->walk.replan = 1;
        self->walk.goal = self->points + (u32)self->index * 0x10;
        self->waiting = 0;
    }
    return 0;
}
static AiMove* Path_Ctor(void* mem, const u32* data) {          // 0x220E70
    AiPath* self = static_cast<AiPath*>(mem);
    Walk_Ctor(&self->walk, data[0], data[1], data[2]);
    self->walk.base.vtable = kVtPath;
    self->route = data[0];
    self->count = (s32)data[3];
    self->points = data[4];
    self->run = (s32)data[5];
    self->avoid = (u8)data[6];
    self->index = self->lastIndex = Rand_Next() % self->count;
    self->wait = 0;
    self->waiting = 0;
    self->walk.base.kind = 4;
    return &self->walk.base;
}

// ── Mode 5: out of the area (0x2683E0–0x268530) ───────────────────────────
struct AiLeave {
    AiWalk walk;
    u8    _d4[0xC];
    u32   route;            // +0xE0
    u8    _e4[0xC];
    float place[4];         // +0xF0
    s32   run;              // +0x100
    u8    avoid;            // +0x104
    u8    _105[0xB];
};
static int Leave_Start(AiLeave* self) {                         // 0x268470
    self->walk.avoidOthers = self->avoid;
    self->walk.findEnd = self->walk.replan = 1;
    self->walk.goal = guest::addr(self->place);
    self->walk.run = self->run != 0;
    Plan(&self->walk, nullptr);
    return 0;
}
static AiMove* Leave_Ctor(void* mem, const u32* data) {         // 0x268530
    AiLeave* self = static_cast<AiLeave*>(mem);
    Walk_Ctor(&self->walk, data[0], data[1], data[2]);
    self->walk.base.vtable = kVtLeave;
    self->route = data[0];
    self->run = (s32)data[8];
    self->avoid = (u8)data[9];
    memcpy(self->place, &data[4], 16);
    self->walk.base.kind = 5;
    self->walk.stopDistance = 3.0f;
    return &self->walk.base;
}

// ── Mode 7: an onlooker (0x278390–0x279D80) ───────────────────────────────
// Somebody who watches a fight from a distance: it backs off when the thing
// it watches comes nearer than `nearest`, comes closer when that is further
// than `furthest`, and in between stands facing it, playing `motion` (a
// frightened or excited gesture) if it was given one.  What it watches is
// `how`: 0 the nearest fighter, 1 the middle of the nearest duel between two
// sides, 2 the back of the nearest fighter of its own side, 3 fighters near a
// place it keeps to, 4 the back of one named character.
struct AiWatch {
    AiWalk walk;
    u8    _d4[0x1C];
    float place[4];         // +0xF0
    s32   motion;           // +0x100  −1 none
    s32   run;              // +0x104
    s32   how;              // +0x108
    s32   whom;             // +0x10C  (how 4) a character id
    float nearest;          // +0x110
    float furthest;         // +0x114
    float behind;           // +0x118  (how 2, 4) how far behind them it stands
    u8    _11c;
    u8    _11d[3];
    s32   stepFrames;       // +0x120
    u8    stepping;         // +0x124  walking: nothing is decided for ten frames
    u8    gesturing;        // +0x125  its motion is playing
    u8    _126[2];
};
static_assert(sizeof(AiWatch) == 0x128 && offsetof(AiWatch, place) == 0xF0 && offsetof(AiWatch, stepping) == 0x124, "AiWatch");
enum : u32 { kVtWatch = 0x2ECE00u };

extern "C" GuestIntMap* Fighters_Get();                         // 0x174F30 (pairs.cpp)
int Matrix_TurnTowards(float degrees, float* matrix, const float* point);   // 0x1C8C40 (task_actions.cpp)

static inline CharaParams* FighterOf(GuestMapNode* n) { return P<CharaParams>(n->value); }
static inline u32 TargetFlags(const CharaParams* p) { return p->link[8] ? *P<u32>(p->link[8]) : 0; }

// Walks a step from `from` toward `to`.
static void Watch_Walk(AiWatch* self, CharaBody* body, const float* to, const float* from) {
    if (self->gesturing == 1) {
        CharaBody_StopScript(body);
        self->gesturing = 0;
    }
    float dir[4] = {to[0] - from[0], to[1] - from[1], to[2] - from[2], to[3] - from[3]};
    const float length = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (length != 0.0f)
        for (int i = 0; i < 3; i++)
            dir[i] /= length;
    CharaBody_WalkTo(body, self->run, dir);
    self->stepping = 1;
}
// Stands and faces `point`.
static void Watch_Face(AiWatch* self, CharaBody* body, const float* point) {
    CharaBody_Idle(body);
    alignas(16) float m[16];
    Matrix_Copy(m, Params(body)->matrix);
    Matrix_TurnTowards(10.0f, m, point);
    Matrix_Copy(Params(body)->matrix, m);
    if (self->motion != -1 && self->gesturing == 0) {
        CharaBody_PlayScripted(body, self->motion, 1, -1, -1, -1, -1);
        self->gesturing = 1;
    }
}
// The place `behind` units behind the character with matrix `m`.
static void Watch_Behind(const AiWatch* self, const float* m, float out[4]) {
    out[0] = m[12] + -m[8] * self->behind;
    out[1] = m[13];
    out[2] = m[14] + -m[10] * self->behind;
    out[3] = 1.0f;
}
// 0x278390: on its way behind someone it must not cut across their front:
// within 3.5 and in front of them it steps off at 45° instead (→ false).
static bool Watch_Clear(AiWatch* self, CharaBody* body, const float* theirs) {
    const float* mine = Params(body)->matrix;
    const float* at = &mine[12];
    float to[3] = {at[0] - theirs[12], at[1] - theirs[13], at[2] - theirs[14]};
    const float front = sqrtf(theirs[8] * theirs[8] + theirs[9] * theirs[9] + theirs[10] * theirs[10]);
    const float away = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    const float cosine = (theirs[8] * to[0] + theirs[9] * to[1] + theirs[10] * to[2]) / (front * away);
    if (!(cosine >= 0.0f) || Vec_Distance(at, &theirs[12]) > 3.5f)
        return true;
    const float side = -(mine[0] * to[0] + mine[1] * to[1] + mine[2] * to[2]);
    float dir[4] = {-theirs[8], theirs[9], -theirs[10], theirs[11]};
    float length = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (length != 0.0f)
        for (int i = 0; i < 3; i++) dir[i] /= length;
    alignas(16) float turn[16];
    Matrix_Identity(turn);
    Matrix_Rotate(turn, turn, side > 0.0f ? -0.7853982f : 0.7853982f);
    float r[4];
    for (int i = 0; i < 4; i++)
        r[i] = turn[i] * dir[0] + turn[4 + i] * dir[1] + turn[8 + i] * dir[2] + turn[12 + i] * dir[3];
    length = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (length != 0.0f)
        for (int i = 0; i < 3; i++) r[i] /= length;
    const float step[4] = {at[0] + r[0], at[1] + r[1], at[2] + r[2], at[3] + r[3]};
    Watch_Walk(self, body, step, at);
    return false;
}
// Nearer than `nearest`: back off; further than `furthest`: come closer
// (`toward`); between them: stand and look at `look`.
static void Watch_Keep(AiWatch* self, CharaBody* body, float apart, const float* toward, const float* look) {
    const float* at = Position(body);
    if (apart == 1000.0f)
        CharaBody_Idle(body);
    else if (apart < self->nearest)
        Watch_Walk(self, body, at, toward);
    else if (apart > self->furthest)
        Watch_Walk(self, body, toward, at);
    else
        Watch_Face(self, body, look);
}

// 0x2798C0
static void Watch_Nearest(AiWatch* self, CharaBody* body) {
    GuestIntMap* all = Fighters_Get();
    float best = 1000.0f;
    s32 key = 0;
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        if (n->value == 0)
            continue;
        const float apart = Vec_Distance(Position(body), &FighterOf(n)->matrix[12]);
        if (apart < best) {
            best = apart;
            key = n->key;
        }
    }
    GuestMapNode* found = GuestIntMap_Find(all, key);
    if (found == GuestIntMap_End(all))
        return;
    const float* there = &FighterOf(found)->matrix[12];
    if (best == 1000.0f)
        Watch_Face(self, body, there);
    else
        Watch_Keep(self, body, best, there, there);
}
// 0x279720 / 0x279350
static void Watch_Duel(AiWatch* self, CharaBody* body) {
    GuestIntMap* all = Fighters_Get();
    float best = 1000.0f;
    float middle[4] = {0, 0, 0, 0};
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        const CharaParams* f = FighterOf(n);
        if ((TargetFlags(f) & 1) == 0 || f->link[3] == 0)
            continue;
        const CharaParams* o = P<CharaParams>(f->link[3]);
        if ((TargetFlags(o) & 1) == 0 || f->faction == o->faction)
            continue;
        float m[4];
        for (int i = 0; i < 3; i++)
            m[i] = o->matrix[12 + i] + (f->matrix[12 + i] - o->matrix[12 + i]) / 2.0f;
        m[3] = 1.0f;
        const float apart = Vec_Distance(Position(body), m);
        if (apart < best) {
            best = apart;
            Copy(middle, m);
        }
    }
    Watch_Keep(self, body, best, middle, middle);
}
// 0x278EA0
static void Watch_Ally(AiWatch* self, CharaBody* body) {
    GuestIntMap* all = Fighters_Get();
    const CharaParams* me = Params(body);
    float best = 1000.0f;
    float spot[4] = {0, 0, 0, 0}, there[4] = {0, 0, 0, 0};
    alignas(16) float theirs[16];
    Matrix_Identity(theirs);
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        const CharaParams* f = FighterOf(n);
        if (me->faction != f->faction)
            continue;
        float s[4];
        Watch_Behind(self, f->matrix, s);
        const float apart = Vec_Distance(Position(body), s);
        if (apart < best) {
            best = apart;
            Copy(spot, s);
            Copy(there, &f->matrix[12]);
            Matrix_Copy(theirs, f->matrix);
        }
    }
    if (best == 1000.0f)
        CharaBody_Idle(body);
    else if (best < self->nearest)
        Watch_Walk(self, body, Position(body), spot);
    else if (best > self->furthest) {
        if (Watch_Clear(self, body, theirs))
            Watch_Walk(self, body, spot, Position(body));
    } else
        Watch_Face(self, body, there);
}
// 0x278A00
static void Watch_Place(AiWatch* self, CharaBody* body) {
    GuestIntMap* all = Fighters_Get();
    const float* at = Position(body);
    float best = 1000.0f;
    bool any = false;
    float there[4] = {0, 0, 0, 0};
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        const float* his = &FighterOf(n)->matrix[12];
        const float fromPlace = Vec_Distance(his, self->place);
        const float fromMe = Vec_Distance(his, at);
        if (fromPlace < self->furthest || fromMe < self->nearest) {
            any = true;
            const float least = fromPlace <= fromMe ? fromPlace : fromMe;
            if (least < best) {
                best = least;
                Copy(there, his);
            }
        }
    }
    if (any) {
        if (best == 1000.0f)
            CharaBody_Idle(body);
        else if (Vec_Distance(at, there) < self->nearest)
            Watch_Walk(self, body, at, there);
        else
            Watch_Face(self, body, there);
        return;
    }
    if (Vec_Distance(at, self->place) > self->furthest) {
        Watch_Walk(self, body, self->place, at);
        return;
    }
    CharaBody_Idle(body);
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        const float apart = Vec_Distance(at, &FighterOf(n)->matrix[12]);
        if (apart < best) {
            best = apart;
            Copy(there, &FighterOf(n)->matrix[12]);
        }
    }
    if (best != 1000.0f)
        Watch_Face(self, body, there);
}
// 0x278650
static void Watch_One(AiWatch* self, CharaBody* body) {
    GuestIntMap* all = Fighters_Get();
    for (GuestMapNode* n = GuestIntMap_Begin(all); n != GuestIntMap_End(all); n = GuestIntMap_Next(n)) {
        const CharaParams* f = FighterOf(n);
        if (f->id != self->whom)
            continue;
        float spot[4];
        Watch_Behind(self, f->matrix, spot);
        const float apart = Vec_Distance(Position(body), spot);
        if (apart < self->nearest)
            Watch_Walk(self, body, Position(body), spot);
        else if (apart > self->furthest) {
            if (Watch_Clear(self, body, f->matrix))
                Watch_Walk(self, body, spot, Position(body));
        } else
            Watch_Face(self, body, &f->matrix[12]);
        return;
    }
    CharaBody_Idle(body);
}
static int Watch_Result(AiWatch* self, CharaBody* body) {       // 0x279BF0
    if (!body || body->params == 0)
        return 0;
    if (self->stepping == 1) {
        if (++self->stepFrames > 9) {
            self->stepFrames = 0;
            self->stepping = 0;
        }
        return 0;
    }
    switch (self->how) {
    case 4: Watch_One(self, body); break;
    case 3: Watch_Place(self, body); break;
    case 2: Watch_Ally(self, body); break;
    case 1: Watch_Duel(self, body); break;
    default: Watch_Nearest(self, body); break;
    }
    return 0;
}
static AiMove* Watch_Ctor(void* mem, const u32* data) {         // 0x279D80
    AiWatch* self = static_cast<AiWatch*>(mem);
    Walk_Ctor(&self->walk, data[0], data[1], data[2]);
    self->walk.base.vtable = kVtWatch;
    memcpy(self->place, &data[4], 16);
    self->motion = (s32)data[8];
    self->run = (s32)data[9];
    self->_11c = (u8)data[15];
    self->how = (s32)data[10];
    self->whom = (s32)data[11];
    memcpy(&self->nearest, &data[12], 4);
    memcpy(&self->furthest, &data[13], 4);
    memcpy(&self->behind, &data[14], 4);
    self->walk.base.kind = 7;
    self->stepFrames = 0;
    self->stepping = 0;
    self->gesturing = 0;
    return &self->walk.base;
}

// ── Mode 6: after someone, and out when they are lost (0x275420–0x2757F0) ─
struct AiFollow {
    AiWalk walk;
    u8    _d4[0xC];
    s32   run;              // +0xE0
    s32   leaving;          // +0xE4  lost sight of them: making for an entrance
    float lastDistance;     // +0xE8
    u32   route;            // +0xEC
    u8    _f0[8];
    u32   who;              // +0xF8  CharaParams*
    u32   entrances;        // +0xFC  PointSet*: the area's target points (the first ten are its entrances)
    s32   pace;             // +0x100
    u8    avoid;            // +0x104
    u8    _105[0xB];
};
static int Follow_Start(AiFollow* self) {                       // 0x275730
    self->walk.avoidOthers = self->avoid;
    self->walk.findEnd = self->walk.replan = 1;
    self->walk.goal = guest::addr(&P<CharaParams>(self->who)->matrix[12]);
    self->walk.run = self->pace;
    Plan(&self->walk, nullptr);
    self->leaving = 0;
    return 0;
}
static int Follow_Step(AiFollow* self, CharaBody* body) {       // 0x275620
    if (!body)
        return 0;
    PickPace(self, Vec_Distance(Position(body), &P<CharaParams>(self->who)->matrix[12]));
    Walk_Step(&self->walk, body);
    return 0;
}
static int Follow_Result(AiFollow* self, CharaBody* body) {     // 0x275430
    if (!body)
        return 0;
    const float* entrances = P<float>(P<PointSet>(self->entrances)->points);
    if (self->leaving == 0 && Collision_CanSee(Params(body)->id, P<CharaParams>(self->who)->id, 2) == 0) {
        int nearest = -1;
        float least = 1000.0f;
        for (int i = 0; i < 10; i++) {
            const float d = FlatDistance(Position(body), entrances + i * 4);
            if (d < least) {
                least = d;
                nearest = i;
            }
        }
        if (nearest != -1) {
            self->walk.findEnd = self->walk.replan = 1;
            self->walk.goal = guest::addr(entrances + nearest * 4);
            self->leaving = 1;
        }
    }
    if (Advance(&self->walk, body))
        for (int i = 0; i < 10; i++)
            if (FlatDistance(Position(body), entrances + i * 4) < 4.0f)
                return 1;
    return 0;
}
static AiMove* Follow_Ctor(void* mem, const u32* data) {        // 0x2757F0
    AiFollow* self = static_cast<AiFollow*>(mem);
    Walk_Ctor(&self->walk, data[0], data[1], data[2]);
    self->walk.base.vtable = kVtFollow;
    self->who = data[3];
    self->route = data[0];
    self->entrances = data[4];
    self->pace = (s32)data[5];
    self->avoid = (u8)data[6];
    self->lastDistance = 0.0f;
    self->run = self->pace == -1 ? 0 : self->pace;
    self->walk.base.kind = 6;
    self->walk.stopDistance = 0.8f;
    return &self->walk.base;
}

// ── Mode 8: the few steps in on arriving (0x279E30–0x279F60) ──────────────
struct AiWalkIn { AiMove base; u32 _08, _0c; float place[4]; s32 run; u8 _24[0xC]; };
static int WalkIn_Result(AiWalkIn* self, CharaBody* body) {     // 0x279E40
    const float distance = Vec_Distance(Position(body), self->place);
    if (distance < 0.5f) {
        CharaBody_Idle(body);
    } else {
        CharaBody_Walk(body, self->run);
        alignas(16) float m[16];
        Matrix_Copy(m, Params(body)->matrix);
        Matrix_FaceTowards(m, self->place);
        Copy(body->input, &m[8]);
    }
    return distance < 0.5f;
}
static AiMove* WalkIn_Dtor(AiMove* self, s16 flag) {            // 0x279F00
    if (self)
        self->vtable = kVtWalkIn;
    return Move_Dtor(self, flag);
}
static AiMove* WalkIn_Ctor(void* mem, const u32* data) {        // 0x279F60
    AiWalkIn* self = static_cast<AiWalkIn*>(mem);
    self->base.vtable = kVtWalkIn;
    memcpy(self->place, data, 16);
    self->run = (s32)data[4];       // (as the original: the word after the point it is given)
    return &self->base;
}

// ── 0x18BFD0 ──────────────────────────────────────────────────────────────
u32 AiMove_Create(u32 mode, const void* data) {
    const u32* d = static_cast<const u32*>(data);
    AiMove* move = nullptr;
    switch (mode) {
    case 1: return AiFight_Create(data);                // ai_fight.cpp
    case 2: move = ToPoint_Ctor(MemAlloc(0x110), d); break;
    case 3: move = ToChara_Ctor(MemAlloc(0x120), d); break;
    case 4: move = Path_Ctor(MemAlloc(0x110), d); break;
    case 5: move = Leave_Ctor(MemAlloc(0x110), d); break;
    case 6: move = Follow_Ctor(MemAlloc(0x110), d); break;
    case 7: move = Watch_Ctor(MemAlloc(0x128), d); break;
    case 8: move = WalkIn_Ctor(MemAlloc(0x30), d); break;
    case 9: return AiWait_Create();                     // ai_fight.cpp
    case 10: return AiHold_Create();
    default: {
        // 0 is standing.
        AiStand* stand = static_cast<AiStand*>(MemAlloc(sizeof(AiStand)));
        memset(stand, 0, sizeof(AiStand));
        stand->base.vtable = kVtStand;
        move = &stand->base;
        break;
    }
    }
    return guest::addr(move);
}

// The methods, by the addresses the vtables hold.
static const struct MoveBinder {
    MoveBinder() {
        guest::bind(0x155210, Move_Dtor, "Move_Dtor");
        guest::bind(0x155190, Stand_Dtor, "Stand_Dtor");
        guest::bind(0x155170, Stand_Start, "Stand_Start");
        guest::bind(0x155160, Move_Nothing, "Move_Nothing");
        guest::bind(0x155130, Stand_Result, "Stand_Result");
        guest::bind(0x155120, Move_Nothing, "Move_Nothing");
        guest::bind(0x21ACD0, Walk_Dtor, "Walk_Dtor");
        guest::bind(0x21AC70, ToPoint_Start, "ToPoint_Start");
        guest::bind(0x21AC30, Walk_StepV, "Walk_StepV");
        guest::bind(0x21ABF0, Walk_Result, "Walk_Result");
        guest::bind(0x21ABE0, Move_Nothing, "Move_Nothing");
        guest::bind(0x222BA0, Walk_Dtor, "Walk_Dtor");
        guest::bind(0x222B40, ToChara_Start, "ToChara_Start");
        guest::bind(0x2229E0, ToChara_Step, "ToChara_Step");
        guest::bind(0x2229A0, Walk_Result, "Walk_Result");
        guest::bind(0x222990, Move_Nothing, "Move_Nothing");
        guest::bind(0x220E10, Walk_Dtor, "Walk_Dtor");
        guest::bind(0x220DA0, Path_Start, "Path_Start");
        guest::bind(0x220D60, Walk_StepV, "Walk_StepV");
        guest::bind(0x220C50, Path_Result, "Path_Result");
        guest::bind(0x220C40, Move_Nothing, "Move_Nothing");
        guest::bind(0x2684D0, Walk_Dtor, "Walk_Dtor");
        guest::bind(0x268470, Leave_Start, "Leave_Start");
        guest::bind(0x268430, Walk_StepV, "Walk_StepV");
        guest::bind(0x2683F0, Walk_Result, "Walk_Result");
        guest::bind(0x2683E0, Move_Nothing, "Move_Nothing");
        guest::bind(0x275790, Walk_Dtor, "Walk_Dtor");
        guest::bind(0x275730, Follow_Start, "Follow_Start");
        guest::bind(0x275620, Follow_Step, "Follow_Step");
        guest::bind(0x275430, Follow_Result, "Follow_Result");
        guest::bind(0x275420, Move_Nothing, "Move_Nothing");
        guest::bind(0x279D20, Walk_Dtor, "Watch_Dtor");
        guest::bind(0x279D10, Move_Nothing, "Watch_Start");
        guest::bind(0x279D00, Move_Nothing, "Watch_Step");
        guest::bind(0x279BF0, Watch_Result, "Watch_Result");
        guest::bind(0x279BE0, Move_Nothing, "Watch_Nothing");
        guest::bind(0x279F00, WalkIn_Dtor, "WalkIn_Dtor");
        guest::bind(0x279EF0, Move_Nothing, "Move_Nothing");
        guest::bind(0x279EE0, Move_Nothing, "Move_Nothing");
        guest::bind(0x279E40, WalkIn_Result, "WalkIn_Result");
        guest::bind(0x279E30, Move_Nothing, "Move_Nothing");
    }
} sMoveBinder;
