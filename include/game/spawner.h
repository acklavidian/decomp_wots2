#pragma once
#include "engine/containers.h"
#include "hal/ps2_types.h"

struct World;

// Who is in an area (0x1837C0–0x186980).  When an area is entered the spawner
// picks actor templates that apply to this area, day and period, chooses a
// character for each and lists them as `ActorDesc`s for the world; what they
// then do is up to their task (see the task manager).

// One entry of task/DataList (asset 0x40): a kind of actor.
struct ActorCompare { s32 op, value; };     // op 0 ==, 1 <, 2 >; value −1 = unused
struct ActorTemplate {
    u32 task;               // +0x000  char*: "task/….bin", the behaviour
    s32 minStrength;        // +0x004  its faction must be at least this strong
    s32 repeatable;         // +0x008  may be used again when the crowd's templates run out
    s32 faction;            // +0x00C  0–2, −1 = any
    s32 role;               // +0x010  CharaParams::role wanted
    s32 chosen;             // +0x014  CharaParams id picked for it this time
    s32 fixed;              // +0x018  > 0: always present (shopkeepers, …)
    s32 grouped;            // +0x01C
    s32 group;              // +0x020  −1 = a group of its own
    s32 story;              // +0x024  appears only while its conditions hold
    s32 areas[10];          // +0x028
    s32 periods[50];        // +0x050  [day × 5 + period]
    struct { s32 index, op, value; } stat[2];   // +0x118  against PlayerData+0x806[index]
    s32 taskA;              // +0x130  == PlayerData+0x11E, −1 = unused
    s32 taskB;              // +0x134  == PlayerData+0x11C
    ActorCompare compare[4][2];     // +0x138  against PlayerData+0x7FC[i]
    GuestList flags[2];     // +0x178  PlayerData+0x740 flags that must be 0 / 1
    GuestList flagsB[2];    // +0x190  the same for PlayerData+0x7E5
    GuestList dayFlags[2];  // +0x1A8  the same for PlayerData+0x6C0
};

// A decompressed task file.
struct TaskData { u32 data; u32 size; };

// How many actors an area holds (Common/GroupAI/mapdata), by time of day.
struct AreaCapacity {
    s32 total;
    struct { s32 min, max; } faction[3];
};

struct Spawner {
    s32   memoryUsed;       // +0x00  against CharaResources_MemoryBudget
    s32   crowd;            // +0x04  places left after the story actors
    s32   strength;         // +0x08  the three factions' strengths together
    float share[3];         // +0x0C
    s32   count[3];         // +0x18  crowd members wanted per faction
    GuestList actors;       // +0x24  ActorDesc*
    GuestIntMap tasks;      // +0x30  hash of the task name → TaskData*
    GuestList usedCharas;   // +0x40  CharaParams ids
    GuestList usedModels;   // +0x4C  chara ids
};

extern "C" {
    void* Spawner_Ctor(void* mem);                                  // 0x1863F0
    void  Spawner_Dtor(void* self, int flag);                       // 0x186250
    // Chooses the area's actors (PlayerData says which area and when).
    void  Spawner_Load(void* self);                                 // 0x184B10
    // Puts actors that asked for it back into the area somewhere else.
    void  Spawner_Update(void* self, World* world, void* taskMgr);  // 0x1848B0
    void  Spawner_ComeBack(void* self, void* charas, int id);       // 0x184840
    void  Spawner_ReleaseAll();                                     // 0x186060  frees the tables
}
