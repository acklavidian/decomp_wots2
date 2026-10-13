#pragma once
#include "engine/containers.h"
#include "game/chara.h"
#include "game/chara_mgr.h"
#include "hal/ps2_types.h"

// An actor's task (0x1C0F20–0x1C6980): the program that decides what it does.
// A task file (see tools/task.py) holds eleven programs, one per *priority
// level*; level 0 is what the actor normally does and the others are entered
// when something happens to it (kTaskCrash … kTaskDead).  Each frame the
// interpreter (Task_Step) runs the current command of the current level:
// commands that only touch variables or set up watches are done at once, the
// others are handed back to the task manager to carry out.

enum : s32 {
    kTaskNormal = 0,
    kTaskCrash = 1,         // somebody bumped into it
    kTaskFindTarget = 2,    // somebody it looks for came into view
    kTaskBattle = 3,        // a fight nearby
    kTaskPartner = 4,       // its partner needs it
    kTaskWeapon = 5,        // somebody nearby drew a sword
    kTaskCrime = 6,         // somebody nearby is a wanted criminal
    kTaskDamage = 7,        // it was hit
    kTaskDead = 8,
    kTaskLevels = 11,
};

// Who a command is about (TaskCommand::who1 / who2); any other value is a
// character id.
enum : s32 { kWhoPlayer = 0, kWhoSelf = 1, kWhoTarget = 2, kWhoPartner = 6, kWhoOther = 7 };

// Whom a watch list entry matches: anyone, the level's target, the player,
// a faction (3–5) or a role (6 + CharaParams::role).
enum : s32 { kMatchAnyone = 0, kMatchTarget = 1, kMatchPlayer = 2, kMatchFaction = 3, kMatchRole = 6 };

enum : s32 {
    kOpJump = 0x00, kOpWait = 0x01, kOpPlayMotion = 0x02, kOpSetAI = 0x03, kOpPlaySE = 0x04,
    kOpPlayVoice = 0x05, kOpSetGlobal = 0x06, kOpFindTargetA = 0x08, kOpFindTargetB = 0x09,
    kOpLostTargetA = 0x0A, kOpLostTargetB = 0x0B, kOpIf = 0x0D, kOpSetLocal = 0x0E,
    kOpDrawSerif = 0x10, kOpReturn = 0x11, kOpSetPos = 0x13, kOpSetAngle = 0x14,
    kOpFindRange = 0x15, kOpRange16 = 0x16, kOpFindWho = 0x17, kOpSubGlobal = 0x18,
    kOpAddGlobal = 0x19, kOpSetChara = 0x1A, kOpSubChara = 0x1C, kOpAddChara = 0x1D,
    kOpWeaponRange = 0x1E, kOpBattleRange = 0x1F, kOpWeaponWho = 0x20, kOpCrashWho = 0x21,
    kOpBattleWho = 0x22, kOpSetDamageTargetID = 0x23, kOpDrawTalkSelect = 0x24, kOpMapOut = 0x25,
    kOpSetBattleMode = 0x26, kOpIfTarget = 0x27, kOpSetTalkCameraOff = 0x28, kOpAddLocal = 0x2A,
    kOpSubLocal = 0x2B, kOpCheckTalkStart = 0x2C, kOpDrawInfoPlate = 0x2D, kOpCrimeWho = 0x2E,
    kOpCrimeRange = 0x2F, kOpSetRoomIn = 0x30, kOpSetPartnerID = 0x31, kOpCheckTargetLife = 0x32,
    kOpCheckCollisionTarget = 0x33, kOpSetWeaponOff = 0x34, kOpSetDirection = 0x35,
    kOpSetDayFlag = 0x36, kOpSubDayFlag = 0x38, kOpAddDayFlag = 0x39, kOpCheckPriorityTarget = 0x3A,
    kOpSetPrision = 0x3B, kOpChrReserve = 0x3C, kOpSetFlag = 0x3E, kOpSetWorkMessage = 0x3F,
    kOpSetTaskA = 0x40, kOpStopActionCount = 0x43, kOpSetTaskB = 0x44, kOpSetWorkName = 0x46,
    kOpCheckItem = 0x47, kOpWatch48 = 0x48, kOpDropItem = 0x49, kOpDeleteWorkItem = 0x4C,
    kOpStopPriorityTask = 0x4D, kOpCheckTargetChange = 0x4E, kOpSetItem = 0x4F, kOpSetItemPos = 0x50,
    kOpAddFlag = 0x51, kOpSubFlag = 0x52, kOpSetDamageStopFlag = 0x53, kOpSetCounter = 0x55,
    kOpAddCounter = 0x56, kOpSetWorkOut = 0x57, kOpSetOtherMessage = 0x58, kOpCheckCatchTarget = 0x5A,
    kOpDeleteNormalItem = 0x5B,
};

struct TaskCommand {
    s32 index;              // +0x00  its line number
    s32 op;                 // +0x04
    s32 wait;               // +0x08  frames before the next line; −1 = until told to go on
    s32 who1, who2;         // +0x0C
    s32 textLength;         // +0x14  bytes
    u32 text;               // +0x18  u16*
    s32 argType[5];         // +0x1C  1 = came from the argument table
    s32 arg[5];             // +0x30
    s32 target;             // +0x44  the level's target when the command was reached
    s32 other;              // +0x48
};

// FindTarget / LostTarget (ops 8–11): go to `line` when somebody of `who`
// comes within `distance` and in front (or, for the Lost pair, stops being so).
struct TaskWatch {
    s32 on;
    s32 line;
    float distance;
    float facing;           // least cosine between its heading and the direction to them
    GuestList who;
};

// CheckCollisionTarget / CheckPriorityTarget.
struct TaskCheck {
    s32 on;
    s32 line;
    s32 level;              // −1
    GuestList who;          // character ids
};

struct Task {
    u32   _00;
    u32   args;             // +0x004  s32[]: the argument table (task/…, asset 0x41)
    s32   argCount;         // +0x008
    s32   stopped;          // +0x00C  StopPriorityTask: events no longer change its level
    s32   level;            // +0x010
    s32   battleMode;       // +0x014
    s32   active;           // +0x018  what Chara::active becomes when the task (re)starts
    s32   restart;          // +0x01C
    s32   _20;
    s32   weaponOff;        // +0x024  1, 2: putting its sword away
    s32   other;            // +0x028  who a watch found, −1
    s32   lastRandom;       // +0x02C
    GuestVector programs[kTaskLevels];  // +0x030  TaskCommand*[]
    s32   line[kTaskLevels];        // +0x0B4
    s32   prevLine[kTaskLevels];    // +0x0E0
    s32   frames[kTaskLevels];      // +0x10C  spent on the current line
    s32   started[kTaskLevels];     // +0x138  the current line's command has been issued
    s32   waiting[kTaskLevels];     // +0x164  it waits to be told to go on
    s32   target[kTaskLevels];      // +0x190  who caused the level
    s32   local[128];               // +0x1BC
    float findRange[3];     // +0x3BC  least distance, greatest distance, least facing
    float range16[3];       // +0x3C8
    float weaponRange[2];   // +0x3D4  distance, facing
    float battleRange[2];   // +0x3DC
    float crimeRange[2];    // +0x3E4
    GuestList findWho;      // +0x3EC
    GuestList weaponWho;    // +0x3F8
    GuestList crashWho;     // +0x404
    GuestList battleWho;    // +0x410
    GuestList damageWho;    // +0x41C
    GuestList crimeWho;     // +0x428
    struct { s32 on, line, life; GuestList who; } checkLife;        // +0x434
    struct { s32 on, line, life, who; } checkCatch;                 // +0x44C
    struct { s32 on, line, who, opponent; } checkChange;            // +0x45C
    struct { s32 on, value, who; } watch48;                         // +0x46C
    TaskCheck check[2];     // +0x478  collision, priority
    TaskWatch watch[4];     // +0x4A8  ops 8–11
};

extern "C" {
    // `data` / `size`: an unpacked task file; `args`: the argument table (a list of ints).
    Task* Task_Ctor(Task* self, const void* data, int size, GuestList* args);   // 0x1C6980
    Task* Task_Dtor(Task* self, s16 flag);                          // 0x1C6260
    void  Task_Load(Task* self, const void* data, int size);        // 0x1C5AB0
    void  Task_ResetState(Task* self);                              // 0x1C5DB0
    void  Task_Restart(Task* self);                                 // 0x1C6170
    void  Task_SetBattleMode(Task* self, int mode);                 // 0x1C1160
    void  Task_SetDamageWho(Task* self, const TaskCommand* cmd);    // 0x1C0F20
    // Is `chara` busy (talking, a higher task level, fighting)?  Also leaves the answer in Chara+0x68.
    int   Task_CharaBusy(Task* self, Chara* chara, void* talk, GuestIntMap* tasks, int with);   // 0x1C0FD0
    void  Task_StepWeaponOff(Task* self, Chara* chara);             // 0x1C10B0
    // The watches a program set up; each returns non-zero when it fired.
    int   Task_CheckWatches(Task* self, GuestIntMap* tasks, CharaMgr* charas, void* talk, Chara* chara);   // 0x1C1240
    int   Task_CheckPriorityTarget(Task* self, GuestIntMap* tasks, CharaMgr* charas);   // 0x1C1BA0
    int   Task_CheckCollisionTarget(Task* self, CharaMgr* charas);  // 0x1C1CE0
    int   Task_CheckTargetLife(Task* self, int id, CharaMgr* charas);   // 0x1C1E10
    int   Task_CheckCatchTarget(Task* self, int id, CharaMgr* charas);  // 0x1C1F40
    int   Task_CheckTargetChange(Task* self, CharaMgr* charas);     // 0x1C2060
    // The events that raise the level; each returns non-zero when it did.
    int   Task_CheckDead(Task* self, CharaMgr* charas, Chara* chara);       // 0x1C2140
    int   Task_CheckDamage(Task* self, CharaMgr* charas, Chara* chara);     // 0x1C21C0
    int   Task_CheckCrime(Task* self, CharaMgr* charas, Chara* chara);      // 0x1C2320
    int   Task_CheckWeapon(Task* self, CharaMgr* charas, Chara* chara);     // 0x1C27C0
    int   Task_CheckPartner(Task* self, CharaMgr* charas, Chara* chara);    // 0x1C2C70
    int   Task_CheckBattle(Task* self, CharaMgr* charas, Chara* chara);     // 0x1C2CE0
    int   Task_CheckFindTarget(Task* self, GuestIntMap* tasks, CharaMgr* charas, void* talk, Chara* chara);  // 0x1C31B0
    int   Task_CheckCrash(Task* self, CharaMgr* charas, Chara* chara);      // 0x1C3970
    // One frame of the interpreter.  Returns the command to carry out, or null.
    TaskCommand* Task_Step(Task* self, int id, GuestIntMap* tasks, CharaMgr* charas);   // 0x1C3AC0

    // Not decompiled (stand-ins in src/native/hle_tasks.cpp).
    float Chara_FacingTowards(const void* matrix, const void* point);       // 0x1C8BC0  cosine
    int   Collision_CanSee(int idA, int idB, int a);                        // 0x175600
    int   Collision_AttackerOf(int id);                                     // 0x1754C0  −1 nobody
    // The conversation manager (World+0xF4, called "effects" elsewhere).
    int   Effects_IsBusy(void* talk, int id);                               // 0x18E830  is `id` in a conversation
    int   Effects_PartnerOf(void* talk, int id);                            // 0x18E790  −1 nobody
}

// Sends the current level to a line of its program.
inline void Task_Goto(Task* task, int line) {
    task->waiting[task->level] = 0;
    task->started[task->level] = 0;
    task->line[task->level] = line;
    task->frames[task->level] = 0;
}
