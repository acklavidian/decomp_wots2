// Accessories on the character: the masks, hats, instruments and stranger
// things from the items menu, drawn on whoever wears them (the object at
// World+0xFC, also *0x2EE7B8, which older notes called "targeting" or the
// "lock-on object"; 0x2AB350–0x2AD710).
//
// Data: Accessory/Accessory.Lst (asset 0x86): u32 count (33), u32, then
// 0x20-byte records {u32, u32 kind, u16 bone[4], u32 model path offsets[4]}
// — up to four models, each riding a bone of the wearer's skeleton (20 the
// head, 8 the back, 13 / 17 the shoulders, 7, 15, 19, 29 …);
// Common/Accessory/AccessoryID.TBL (0x87): u8 count, then the record of
// each accessory item (item id − 0x20).  Kinds: 1 two candle flames at the
// temples, 2 trails of sparks, 3 a halo, 4 not worn at all — it floats
// after the wearer (the ghost).
//
// Kept host-side: the two tables (the original holds the files and their
// load state in a 0x10-byte object) and the one accessory in use (0x80
// bytes with up to four model objects of class 0x2ED670 and an effect
// object); models are drawn natively (hle_object_draw), so nothing waits to
// load.

#include "engine/containers.h"
#include "game/chara_resources.h"
#include "game/map_collision.h"
#include "native/archive.h"
#include "native/guest.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    int   Rand_Next();                                          // 0x1200F0
    void  Matrix_Copy(void* dst, const void* src);              // 0x1073C0
    void  hle_object_draw(const char* path, const float* matrix);
    // src/game/world_effects.cpp
    void  Fx_AccHalo(const float* head);
    void  Fx_AccFlames(const float* head, const float* heights, const u32* colours);
    void  Fx_AccTrail(const float (*left)[4], const float (*right)[4], u32 newest, u32 oldest);
}

namespace {

template <typename T = void> T* P(u32 a) { return guest::ptr<T>(a); }

struct AccRecord {
    u32 kind = 0;
    u16 bone[4] = {};
    std::string model[4];
};
std::vector<AccRecord> sRecords;
std::vector<u8> sByItem;        // AccessoryID.TBL
bool sTablesRead = false;

void ReadTables() {                                             // (0x2AD5C0 / 0x2AD490)
    if (sTablesRead)
        return;
    sTablesRead = true;
    std::vector<u8> d;
    if (archive::read("Accessory/Accessory.Lst", d) && d.size() >= 8) {
        u32 count;
        memcpy(&count, d.data(), 4);
        for (u32 i = 0; i < count && 8 + (i + 1) * 0x20 <= d.size(); i++) {
            const u8* r = &d[8 + i * 0x20];
            AccRecord rec;
            memcpy(&rec.kind, r + 4, 4);
            memcpy(rec.bone, r + 8, 8);
            for (int k = 0; k < 4; k++) {
                u32 at;
                memcpy(&at, r + 0x10 + k * 4, 4);
                if (at < d.size())
                    rec.model[k] = reinterpret_cast<const char*>(&d[at]);
            }
            sRecords.push_back(std::move(rec));
        }
    }
    archive::read("Common/Accessory/AccessoryID.TBL", sByItem);
}
// 0x2AD560: the record of an accessory item, −1 if it has none.
int RecordOfItem(int item) {
    ReadTables();
    if (sByItem.empty() || (u32)sByItem[0] <= (u32)(item - 0x21))
        return -1;
    return sByItem[(size_t)(item - 0x20)];
}

// The accessory in use (the original's 0x80-byte object, 0x2AC2D0).
struct Worn {
    int record = -1;
    // The ghost: where it is, how it is turned, its bobbing.
    float at[4] = {0, 0, 0, 1};
    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    int phase = 0;
    bool fresh = true;
    // The flames.
    float heights[2] = {0.06f, 0.06f};
    u32 colours[2] = {0x60808080u, 0x60808080u};
    // The trails: the last 32 places of two points of the head.
    float left[32][4] = {}, right[32][4] = {};
    u32 newest = 0, oldest = 0;
    bool moved = false;
};
std::unique_ptr<Worn> sWorn;

struct Wearers {            // 0x18 bytes (World+0xFC)
    u32 capacity, count, list;  // +0x00  CharaParams* of who wears it (the player, and its double in scenes)
    u32 tables;             // +0x0C  (the original's table object)
    u32 worn;               // +0x10  (… and its accessory object; here: non-zero while there is one)
    s32 state;              // +0x14
};
static_assert(sizeof(Wearers) == 0x18, "Wearers");

inline const float* BoneOf(const CharaParams* who, u32 bone) {
    return P<float>(*P<u32>(who->link[1] + 4) + bone * 0xA0);
}
inline bool Active(const CharaParams* who) { return who->link[4] != 0 && *P<u32>(who->link[4]) != 0; }  // +0xC4
inline void ApplyRow(float* o, const float* m, const float* v) {
    float r[4];
    for (int j = 0; j < 4; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(o, r, sizeof r);
}

// 0x2AB3D0: the accessory object for a record (kept if it is that one already).
void SetRecord(Wearers* self, int record) {
    ReadTables();
    if (record < 0 || (size_t)record >= sRecords.size()) {
        sWorn.reset();
        self->worn = 0;
        return;
    }
    if (sWorn && sWorn->record == record)
        return;
    sWorn = std::make_unique<Worn>();
    sWorn->record = record;
    self->worn = 1;
}
// 0x2AB4C0: what the player wears now (PlayerData+0x11A).
void Refresh(Wearers* self) {
    const u16 item = *reinterpret_cast<const u16*>(P<u8>(*P<u32>(0x2EE688u)) + 0x11A);
    SetRecord(self, RecordOfItem(item));
}

// 0x2AC050: a frame of the accessory for one wearer.
void Worn_Update(Worn* w, const CharaParams* who) {
    const AccRecord& rec = sRecords[(size_t)w->record];
    if (rec.kind == 4 && who->link[1] != 0) {
        // The ghost drifts after a point just behind the wearer, faster the
        // further it has fallen behind, and keeps facing them.
        const float* root = BoneOf(who, 0);
        static const float kBehind[4] = {0.0f, 0.0f, -0.1f, 1.0f};          // 0x2DB720
        float goal[4];
        ApplyRow(goal, root, kBehind);
        if (w->fresh) {
            memcpy(w->at, goal, sizeof goal);
            w->fresh = false;
        }
        float to[4];
        for (int k = 0; k < 4; k++)
            to[k] = goal[k] - w->at[k];
        float far = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (far > 0.0f)
            for (int k = 0; k < 3; k++)
                to[k] /= far;
        if (far > 0.01f) {
            far -= 0.01f;
            if (far > 2.0f)
                far *= 9.0f;
            for (int k = 0; k < 4; k++)
                w->at[k] += to[k] * far * 0.1f;
            w->at[3] = 1.0f;
        }
        w->at[1] = root[13];
        // 0x2B2E40: turned to look at the wearer.
        float dir[3] = {root[12] - w->at[0], root[13] - w->at[1], root[14] - w->at[2]};
        const float length = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (length > 0.0f)
            for (float& c : dir)
                c /= length;
        float* m = w->matrix;
        m[0] = dir[2];                      // up × dir
        m[1] = 0.0f;
        m[2] = -dir[0];
        m[4] = dir[1] * m[2] - dir[2] * m[1];
        m[5] = dir[2] * m[0] - dir[0] * m[2];
        m[6] = dir[0] * m[1] - dir[1] * m[0];
        const float upLength = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        if (upLength > 0.0f)
            for (int k = 4; k < 7; k++)
                m[k] /= upLength;
        m[8] = dir[0];
        m[9] = dir[1];
        m[10] = dir[2];
        m[3] = m[7] = m[11] = 0.0f;
        memcpy(&m[12], w->at, 4 * sizeof(float));
        w->phase = (w->phase + (Rand_Next() & 1) + 2) % 0x168;
    }
    if (rec.kind == 2) {
        w->moved = true;                                        // 0x2ACFC0
    } else if (rec.kind == 1) {                                 // 0x2AD280: the flames flicker
        for (int i = 0; i < 2; i++)
            w->heights[i] = (float)(Rand_Next() % 0x1E + 0x32) * 0.001f;
        for (int i = 0; i < 2; i++)
            w->colours[i] = (u32)((Rand_Next() % 0x1E + 0x46) * 0x80 / 100) << 24 | 0x808080;
    }
}
// 0x2ABD80
void Worn_Draw(Worn* w, const CharaParams* who, MapCollision* map) {
    if (who == nullptr || who->link[1] == 0)
        return;
    const AccRecord& rec = sRecords[(size_t)w->record];
    for (int i = 0; i < 4; i++) {
        if (rec.model[i].empty())
            continue;
        alignas(16) float m[16];
        if (rec.kind == 4) {                                    // the ghost bobs over the ground under it
            Matrix_Copy(m, w->matrix);
            if (map != nullptr)
                m[13] = Map_CellAt(map, &m[12])->height + sinf((float)w->phase / 180.0f * 3.1415927f) * 0.02f;
        } else {
            Matrix_Copy(m, BoneOf(who, rec.bone[i]));
        }
        hle_object_draw(rec.model[i].c_str(), m);               // (model vtable +0x14 / +0x18: the VU1 boundary)
    }
    const float* head = BoneOf(who, 20);
    if (rec.kind == 3) {
        Fx_AccHalo(head);
    } else if (rec.kind == 1) {
        Fx_AccFlames(head, w->heights, w->colours);
    } else if (rec.kind == 2) {                                 // 0x2ACCB0
        if (w->moved) {
            static const float kLeft[4] = {0.03f, -0.16f, 0.072f, 1.0f}, kRight[4] = {-0.03f, -0.16f, 0.072f, 1.0f};    // 0x2DB5B0
            const auto step = [w] {
                w->newest = (w->newest + 1) & 0x1F;
                if (w->oldest == w->newest)
                    w->oldest = (w->oldest + 1) & 0x1F;
            };
            if (w->oldest == w->newest) {
                ApplyRow(w->left[w->newest], head, kLeft);
                ApplyRow(w->right[w->newest], head, kRight);
                step();
            } else {
                // Ten points along the way from where they last were.
                float a[4], b[4];
                ApplyRow(a, head, kLeft);
                ApplyRow(b, head, kRight);
                const u32 last = (w->newest + 0x1F) & 0x1F;
                float fromA[4], fromB[4];
                memcpy(fromA, w->left[last], sizeof fromA);
                memcpy(fromB, w->right[last], sizeof fromB);
                for (int n = 1; n <= 10; n++) {
                    const float share = 0.1f * (float)n;
                    for (int k = 0; k < 3; k++) {
                        w->left[w->newest][k] = fromA[k] + (a[k] - fromA[k]) * share;
                        w->right[w->newest][k] = fromB[k] + (b[k] - fromB[k]) * share;
                    }
                    w->left[w->newest][3] = w->right[w->newest][3] = 1.0f;
                    step();
                }
            }
            w->moved = false;
        }
        Fx_AccTrail(w->left, w->right, w->newest, w->oldest);
    }
}

}  // namespace

extern "C" {

void* Targeting_Ctor(void* mem) {                               // 0x2ABA50
    Wearers* self = static_cast<Wearers*>(mem);
    memset(self, 0, sizeof *self);
    *P<u32>(0x2EE7B8u) = guest::addr(self);
    sWorn.reset();
    ReadTables();
    return self;
}
void Targeting_Dtor(void* mem, int flag) {                      // 0x2AB9A0
    Wearers* self = static_cast<Wearers*>(mem);
    if (self == nullptr)
        return;
    *P<u32>(0x2EE7B8u) = 0;
    sWorn.reset();
    if (self->list != 0)
        MemFree(P(self->list));
    if ((s16)flag > 0)
        MemFree(self);
}
// 0x2AB880 / 0x2AB910: someone who wears what the player wears.
void Targeting_SetPlayer(void* mem, u8* params) {
    Wearers* self = static_cast<Wearers*>(mem);
    const u32 who = guest::addr(params);
    for (u32 i = 0; i < self->count; i++)
        if (P<u32>(self->list)[i] == who)
            return;
    if (self->count == self->capacity) {
        const u32 capacity = self->capacity ? self->capacity * 2 : 1;
        u32* list = static_cast<u32*>(MemAlloc(capacity * 4));
        if (self->list != 0) {
            memcpy(list, P(self->list), self->count * 4);
            MemFree(P(self->list));
        }
        self->list = guest::addr(list);
        self->capacity = capacity;
    }
    P<u32>(self->list)[self->count++] = who;
}
// 0x2AB7D0: … no longer.
void Accessories_RemoveWearer(const void* params) {
    Wearers* self = P<Wearers>(*P<u32>(0x2EE7B8u));
    if (*P<u32>(0x2EE7B8u) == 0)
        return;
    const u32 who = guest::addr(params);
    u32* list = P<u32>(self->list);
    for (u32 i = 0; i < self->count; i++)
        if (list[i] == who) {
            memmove(&list[i], &list[i + 1], (self->count - i - 1) * 4);
            self->count--;
            return;
        }
}
void Accessories_AddWearer(void* params) {                      // 0x2AB910
    if (*P<u32>(0x2EE7B8u) != 0)
        Targeting_SetPlayer(P(*P<u32>(0x2EE7B8u)), static_cast<u8*>(params));
}
// 0x2AB4C0: what the player wears has changed (the items menu).
void Accessories_Refresh() {
    if (*P<u32>(0x2EE7B8u) != 0)
        Refresh(P<Wearers>(*P<u32>(0x2EE7B8u)));
}
// 0x2AB600 (with 0x2AB6D0): once a frame — are the models in; and a frame of
// the accessory for everyone active who wears it.
u8 Targeting_IsReady(void* mem) {
    Wearers* self = static_cast<Wearers*>(mem);
    if (self->state == 0) {
        self->state = 1;
        return 0;
    }
    if (!sWorn && self->state == 1) {
        Refresh(self);
        self->state = 3;
        return 0;
    }
    if (sWorn)
        for (u32 i = 0; i < self->count; i++) {
            const CharaParams* who = P<CharaParams>(P<u32>(self->list)[i]);
            if (Active(who))
                Worn_Update(sWorn.get(), who);
        }
    return 1;
}
void Targeting_Draw(void* mem, u32 map) {                       // 0x2AB530
    Wearers* self = static_cast<Wearers*>(mem);
    if (!sWorn)
        return;
    for (u32 i = 0; i < self->count; i++) {
        const CharaParams* who = P<CharaParams>(P<u32>(self->list)[i]);
        if (Active(who))
            Worn_Draw(sWorn.get(), who, map != 0 ? P<MapCollision>(map) : nullptr);
    }
}

}  // extern "C"
