#pragma once
#include "engine/containers.h"
#include "game/task.h"
#include "hal/ps2_types.h"

struct World;

// The task manager (0x1BC3C0–0x1C0030; `GameplayScene+0xCC`): every actor's
// task, the things tasks have started (a motion, speech, an "AI" such as
// walking somewhere — one of each per actor), and the commands that act on
// the world.

// What a command leaves running.  The classes are not decompiled; all start
// like this.  Update returns non-zero when it is finished (−1 has a special
// meaning per kind), and the task then goes to `nextLine` unless that is −1.
struct TaskAction {
    s32 handle;             // +0x00
    s32 actor;              // +0x04
    s32 nextLine;           // +0x08
    u32 command;            // +0x0C  TaskCommand*
    u32 vtable;             // +0x10  +0x08 Destroy(self, flag), +0x0C Update(self)
};

// CheckTalkStart: the player may start a conversation with `who1`.
struct TalkStart {
    s32 actor;              // whose task continues…
    s32 line;               // …at this line
    s32 who1, who2;
};

struct TaskMgr {
    GuestList usedPlaces;   // +0x00  places SetPos has handed out
    GuestIntMap tasks;      // +0x0C  actor id → Task*
    GuestIntMap motions;    // +0x1C  actor id → TaskAction* (PlayMotion, SetDirection)
    GuestIntMap talks;      // +0x2C  (DrawSerif, DrawTalkSelect)
    GuestIntMap ais;        // +0x3C  (SetAI)
    GuestList talkStarts;   // +0x4C  TalkStart*
};

extern "C" {
    void* TaskMgr_Ctor(void* mem);                                  // 0x1C0030
    void  TaskMgr_Dtor(void* self, int flag);                       // 0x1BFC20
    void  TaskMgr_ReleaseAll();                                     // 0x1BFFE0
    // Gives actor `id` the task in `data` (or restarts the one it has).
    void  TaskMgr_Add(void* self, u32 id, u32 data, u32 size);      // 0x1BF960
    // Coming back out of an interior: the people in it start over.
    void  TaskMgr_Reset(void* self, World* world);                  // 0x1BFA30
    // One frame.  −1, or what a command asked of the scene: an exit to take
    // (SetRoomIn) or 0x80 (the player was arrested).
    int   TaskMgr_Update(void* self, World* world, void* spawner);  // 0x1BE5E0

    // The classes commands create (stand-ins in src/native/hle_tasks.cpp).
    TaskAction* TaskMotion_Ctor(void* mem, int actor, TaskCommand* cmd, CharaMgr* charas);                 // 0x210BF0  0x38 bytes
    TaskAction* TaskDirection_Ctor(void* mem, int actor, TaskCommand* cmd, CharaMgr* charas, const float* point);  // 0x275AD0  0x40
    TaskAction* TaskAI_Ctor(void* mem, int actor, TaskCommand* cmd, Chara* chara, void* mapState);         // 0x222E70  0x18
    TaskAction* TaskSerif_Ctor(void* mem, int actor, TaskCommand* cmd, CharaMgr* charas, void* talk);      // 0x20FB10  0x24
    TaskAction* TaskTalkSelect_Ctor(void* mem, int actor, TaskCommand* cmd, CharaMgr* charas, void* talk); // 0x2107B0  0x24
}
