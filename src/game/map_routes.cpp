// Route-point connectivity — decompiled from 0x14AAF0 (search), 0x14AF50
// (table) and 0x14B280 (links).  These are methods of the map state, which is
// otherwise still a stand-in (src/native/hle_gameplay.cpp); they use only its
// three tables at +0x20 / +0x40 / +0x60 and the map it has loaded.

#include "game/ai_moves.h"
#include "game/map_collision.h"
#include "game/map_db.h"
#include "native/guest.h"

#include <cmath>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                   // 0x100820
    void  MemFree(void* p);                     // 0x100650
    u32   MapState_GetMap(void* self);          // 0x14B640
}

static const GuestPtrVar<u8> gPlayerData{0x2EE688u};
static s32* const gIndoors = guest::ptr<s32>(0x2EE518u);

static inline RouteTables* Tables(void* mapState) {
    return reinterpret_cast<RouteTables*>(static_cast<u8*>(mapState) + 0x20);
}
static inline const PointSet* RoutePoints() {
    return MapDb_GetRoutePoints(MapDb_Get(), (s8)(*gPlayerData)[0x103]);
}
struct SearchEntry { float distance; s32 previous; };
template <typename T> static inline T* Value(GuestMapNode* n) { return reinterpret_cast<T*>(&n->value); }

static void FreeNodes(GuestIntMap* map, void (*freeValue)(GuestMapNode*)) {
    for (GuestMapNode* n = GuestIntMap_Begin(map); n != GuestIntMap_End(map); n = GuestIntMap_Next(n))
        if (freeValue)
            freeValue(n);
    GuestIntMap_Clear(map);
}

void RouteTables_Init(RouteTables* tables) {
    for (int i = 0; i < 2; i++) {
        GuestIntMap_Init(&tables->links[i]);
        GuestIntMap_Init(&tables->search[i]);
        GuestIntMap_Init(&tables->routes[i]);
    }
}

// ── 0x14B280 (and the scripts' copy 0x1FBD50) ─────────────────────────────
// Two route points are linked when nothing on the map lies between them.
void Routes_Link(GuestIntMap* links, const PointSet* set, void* collision) {
    FreeNodes(links, [](GuestMapNode* n) {
        GuestVector* v = Value<GuestVector>(n);
        if (v->data != 0)
            MemFree(guest::ptr(v->data));
    });
    MapCollision* map = static_cast<MapCollision*>(collision);
    if (!set || !map)
        return;
    const float* points = guest::ptr<float>(set->points);
    for (s32 a = 0; a < set->count; a++) {
        for (s32 b = 0; b < set->count; b++) {
            if (a == b || Map_LineBlocked(map, points + a * 4, points + b * 4, 0) != nullptr)
                continue;
            GuestMapNode* node = GuestIntMap_Find(links, a);
            if (node == GuestIntMap_End(links)) {
                const GuestVector empty{0, 0, 0};
                node = GuestIntMap_InsertBytes(links, a, &empty, sizeof empty);
            }
            VectorU32_PushBack(Value<GuestVector>(node), (u32)b);
        }
    }
}
void MapState_LinkRoutePoints(void* mapState) {
    Routes_Link(&Tables(mapState)->links[*gIndoors], RoutePoints(), guest::ptr(MapState_GetMap(mapState)));
}

// ── 0x14AAF0 (0x1FB8D0) ───────────────────────────────────────────────────
// The shortest way from one point to every other over the links: for each
// point reached, how far it is and which point comes before it.
void Routes_Search(GuestIntMap* links, GuestIntMap* found, const PointSet* set, int from) {
    GuestIntMap_Clear(found);
    GuestVector frontier{0, 0, 0}, next{0, 0, 0};
    VectorU32_PushBack(&frontier, (u32)from);
    const SearchEntry start{0.0f, from};
    GuestIntMap_InsertBytes(found, from, &start, sizeof start);

    while (frontier.count != 0) {
        next.count = 0;
        const s32* at = guest::ptr<s32>(frontier.data);
        for (u32 i = 0; i < frontier.count; i++) {
            GuestMapNode* node = GuestIntMap_Find(links, at[i]);
            if (node == GuestIntMap_End(links))
                continue;
            const GuestVector* seen = Value<GuestVector>(node);
            const s32* to = guest::ptr<s32>(seen->data);
            for (u32 k = 0; k < seen->count; k++) {
                GuestMapNode* known = GuestIntMap_Find(found, to[k]);
                float distance = 0.0f;
                if (set) {
                    GuestMapNode* here = GuestIntMap_Find(found, at[i]);
                    if (here != GuestIntMap_End(found) && at[i] < set->count && to[k] < set->count &&
                        at[i] >= 0 && to[k] >= 0) {
                        const float* points = guest::ptr<float>(set->points);
                        distance = Vec_Distance(points + at[i] * 4, points + to[k] * 4) +
                                   Value<SearchEntry>(here)->distance;
                    }
                }
                if (known == GuestIntMap_End(found)) {
                    VectorU32_PushBack(&next, (u32)to[k]);
                    const SearchEntry entry{distance, at[i]};
                    GuestIntMap_InsertBytes(found, to[k], &entry, sizeof entry);
                } else if (distance < Value<SearchEntry>(known)->distance) {
                    Value<SearchEntry>(known)->distance = distance;
                    Value<SearchEntry>(known)->previous = at[i];
                    VectorU32_PushBack(&next, (u32)to[k]);
                }
            }
        }
        // (0x14C5E0: frontier = next)
        frontier.count = 0;
        const u32* add = guest::ptr<u32>(next.data);
        for (u32 i = 0; i < next.count; i++)
            VectorU32_PushBack(&frontier, add[i]);
    }
    if (next.data != 0)
        MemFree(guest::ptr(next.data));
    if (frontier.data != 0)
        MemFree(guest::ptr(frontier.data));
}
void MapState_SearchRoutes(void* mapState, int from) {
    RouteTables* t = Tables(mapState);
    Routes_Search(&t->links[*gIndoors], &t->search[*gIndoors], RoutePoints(), from);
}

// ── 0x14AF50 (0x1FB5E0) ───────────────────────────────────────────────────
// routes[destination][point] = the point to make for next.
void Routes_Build(GuestIntMap* routes, GuestIntMap* links, GuestIntMap* found, const PointSet* set) {
    FreeNodes(routes, [](GuestMapNode* n) { GuestIntMap_Clear(Value<GuestIntMap>(n)); });
    if (!set || links->count == 0)
        return;
    for (s32 i = 0; i < set->count; i++) {
        Routes_Search(links, found, set, i);
        const GuestIntMap blank{0, 0, 0, 0};
        GuestMapNode* node = GuestIntMap_InsertBytes(routes, i, &blank, sizeof blank);
        GuestIntMap* hops = Value<GuestIntMap>(node);
        GuestIntMap_Init(hops);
        for (GuestMapNode* n = GuestIntMap_Begin(found); n != GuestIntMap_End(found); n = GuestIntMap_Next(n))
            GuestIntMap_Insert(hops, n->key, (u32)Value<SearchEntry>(n)->previous);
    }
    GuestIntMap_Clear(found);
}
void MapState_BuildRouteTable(void* mapState) {
    RouteTables* t = Tables(mapState);
    Routes_Build(&t->routes[*gIndoors], &t->links[*gIndoors], &t->search[*gIndoors], RoutePoints());
}

// The scripts' own paths (0x1FB5E0): the same three steps over a handful of
// points; only the table of hops is kept.
void Routes_BuildFor(GuestIntMap* routes, const PointSet* set, void* collision) {
    // (on the stack in the original; a map's end node is its own header, so
    // here they have to be in guest memory)
    GuestIntMap* scratch = static_cast<GuestIntMap*>(MemAlloc(2 * sizeof(GuestIntMap)));
    GuestIntMap* links = &scratch[0];
    GuestIntMap* found = &scratch[1];
    GuestIntMap_Init(links);
    GuestIntMap_Init(found);
    Routes_Link(links, set, collision);
    Routes_Build(routes, links, found, set);
    FreeNodes(links, [](GuestMapNode* n) {
        GuestVector* v = Value<GuestVector>(n);
        if (v->data != 0)
            MemFree(guest::ptr(v->data));
    });
    MemFree(scratch);
}
