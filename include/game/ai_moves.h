#pragma once
#include "engine/containers.h"
#include "game/chara.h"
#include "hal/ps2_types.h"

// What an AI routine steps (see chara.h): the moves a task's SetAI asks for,
// 0x18BFD0–0x18D350 with one small class per kind of move.
//
// Finding the way.  Each area has *route points* (MapDb: PathPoint).  The
// map state keeps, per map, which points can see which (`links`) and, for
// every point as a destination, the next hop from every other (`routes`).
// A walker heads straight for its goal while nothing is in the way;
// otherwise it finds the route points nearest itself and the goal that each
// can see, and follows the hops between them.

// The map state's three tables (MapState+0x20, +0x40, +0x60; one of each
// outdoors and one indoors, by *0x2EE518).
struct PointSet;

struct RouteTables {
    GuestIntMap links[2];   // +0x20  point → vector<int>: the points it can see
    GuestIntMap search[2];  // +0x40  scratch: point → {float distance, int previous}
    GuestIntMap routes[2];  // +0x60  destination → map(point → the point to go to next)
};

struct AiMove { u32 vtable; s32 kind; };

// A move that walks somewhere (0xD4 bytes; 0x18C1A0–0x18CFE0).
struct AiWalk {
    AiMove base;
    u32   route;            // +0x08  PointSet*: the area's route points
    u32   goal;             // +0x0C  float[4]*: where it is going (may move)
    s32   replan;           // +0x10
    s32   findEnd;          // +0x14  the goal's nearest route point is to be looked up again
    s32   viaRoute;         // +0x18  1: heading for a route point; 0: straight for the goal
    s32   run;              // +0x1C
    s32   atPoint;          // +0x20  reached what it was heading for
    s32   _24;
    s32   stuckTimer;       // +0x28
    s32   stuck;            // +0x2C
    u32   pathAt;           // +0x30
    s32   nearSelf;         // +0x34  route point nearest itself
    s32   nearGoal;         // +0x38  …and nearest the goal
    s32   _3c;
    s32   crowded;          // +0x40
    s32   avoiding;         // +0x44  stepping round someone
    s32   _48, _4c;
    u8    avoidOthers;      // +0x50
    u8    _51[3];
    float stopDistance;     // +0x54  how near the goal counts as there
    float stopNow;          // +0x58
    u32   _5c;
    float lastCheck[4];     // +0x60
    float heading[4];       // +0x70
    float target[4];        // +0x80  what it is heading for now
    float from[4];          // +0x90
    float goalOffset[4];    // +0xA0
    float avoidStart[4];    // +0xB0
    GuestVector path;       // +0xC0  route point indices
    u32   links;            // +0xCC  GuestIntMap*
    u32   routes;           // +0xD0  GuestIntMap*
};

extern "C" {
    // ── The map state's route tables (0x14AAF0–0x14B5C0) ──────────────────
    void RouteTables_Init(RouteTables* tables);
    void MapState_LinkRoutePoints(void* mapState);          // 0x14B280
    void MapState_SearchRoutes(void* mapState, int from);   // 0x14AAF0
    void MapState_BuildRouteTable(void* mapState);          // 0x14AF50
    // The same over any set of points (the scripts' paths: 0x1FB5E0, 0x1FBD50, 0x1FB8D0).
    void Routes_Link(GuestIntMap* links, const PointSet* set, void* collision);
    void Routes_Search(GuestIntMap* links, GuestIntMap* found, const PointSet* set, int from);
    void Routes_Build(GuestIntMap* routes, GuestIntMap* links, GuestIntMap* found, const PointSet* set);
    void Routes_BuildFor(GuestIntMap* routes, const PointSet* set, void* collision);

    // ── Queries about the characters around (0x175210–0x175940) ───────────
    int  Query_SomeoneAhead(float within, float cosine, int id);            // 0x175370
    const float* Query_LineBlocked(const float* from, const float* to, int kind);       // 0x175510
    const float* Query_LineBlockedFrom(int id, const float* to, int kind);              // 0x175580
    float Query_Distance(int idA, int idB);                                 // 0x175940
    // Cosine of the angle between where a matrix faces and a point.
    float Matrix_FacingCosine(const float* matrix, const float* point);     // 0x1C8BC0
}
