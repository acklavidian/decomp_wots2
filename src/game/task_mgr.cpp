// The task manager — decompiled from 0x1BC3C0–0x1C0030.  See include/game/task_mgr.h.

#include "game/task_mgr.h"
#include "engine/archive_file.h"
#include "engine/bgm.h"
#include "engine/font.h"
#include "engine/mt_random.h"
#include "engine/pad.h"
#include "game/day_mgr.h"
#include "game/hud.h"
#include "game/player_data.h"
#include "game/spawner.h"
#include "game/sword.h"
#include "game/world.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* MemAllocAligned(u32 align, u32 size);         // 0x1228C8
    void  MemFreeAligned(void* p);                      // 0x122B50
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Matrix_Identity(void* m);                     // 0x107428
    void* PadMgr_Get();                                 // 0x13A920
    u16   Pad_GetHeld(void* mgr, int port);             // 0x13A0E0
    u16   Pad_GetPressed(void* mgr, int port);          // 0x13A0C0
    void* FollowCam_Ctor(void* mem, void* target, const void* config);     // 0x152470
    void  CamList_Push(void* list, void* controller);   // 0x150E60
    void  ItemDrops_Drop(void* self, int item, const void* matrix);        // 0x21C0E0

    // Character manager (src/game/chara_mgr.cpp).
    u32   CharaMgr_GetFlags(CharaMgr* mgr, int id);
    int   CharaMgr_SetState(CharaMgr* mgr, int id, int a, u32 b, u32 c);
    void  CharaMgr_SetActive(CharaMgr* mgr, int id, int active);
    void  CharaMgr_Set68(CharaMgr* mgr, int id, int value);
    void  CharaMgr_Set6C(CharaMgr* mgr, int id, int value);
    void  CharaMgr_Set70(CharaMgr* mgr, int id, int value);
    int   CharaMgr_Get70(CharaMgr* mgr, int id);
    int   CharaMgr_GetPartner(CharaMgr* mgr, int id);
    void  CharaMgr_SetPartner(CharaMgr* mgr, int id, int other);
    void  CharaMgr_Release(CharaMgr* mgr, int id);
    int   CharaMgr_IsHeld(CharaMgr* mgr, int id);
    void  CharaMgr_ClearB5(CharaMgr* mgr);
    void  CharaMgr_fn143140(CharaMgr* mgr, int id);
    void  CharaMgr_SetOpponent(CharaMgr* mgr, int id, int other, int kind);
    void  CharaMgr_Sheathe(CharaMgr* mgr, int id, int quiet);
    void  CharaMgr_RestoreSword(CharaMgr* mgr, void* swordDb, int id);

    // Not decompiled (stand-ins in src/native/hle_tasks.cpp).
    void  Effects_EndTalkOf(void* talk, int id);                    // 0x18F770
    void  Effects_EndAll(void* talk, int id);                       // 0x18F7D0  id −1 = every conversation
    void  Effects_fn18e730(void* talk, int id);                     // 0x18E730
    void  Effects_fn18e890(void* talk, int handle);                 // 0x18E890
    void  Sound_Cry(const void* charaParams, int cry);              // 0x180F70 (bgm.cpp)
    void  Hud_ShowTalkPrompt(void* hud, const void* info);          // 0x21CD60
    void  ItemDrops_Place(void* drops, int item, const void* matrix);       // 0x21BAB0
    int   ItemDrops_ItemId(void* drops, int item);                  // 0x21C1A0
}

static_assert(sizeof(TaskMgr) == 0x58 && offsetof(TaskMgr, talkStarts) == 0x4C, "TaskMgr");

static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
static const GuestPtrVar<void>       gHud{0x2EE69Cu};
static const GuestPtrVar<CharaMgr>   gCollisionCharas{0x2EE5C4u};
static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);
static s32& gIndoors   = *guest::ptr<s32>(0x2EE518u);
static u32& sLoaded    = *guest::ptr<u32>(0x2EE638u);
static GuestList* const sArgs = guest::ptr<GuestList>(0x30A5C0u);       // the argument table ($n in task files)
static const u32* const sPositions = guest::ptr<const u32>(0x309F90u);  // three tables of places, by area

struct PlaceSet { s32 count; u32 points; };

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline Task* TaskOf(TaskMgr* mgr, int id) {
    GuestMapNode* n = GuestIntMap_Find(&mgr->tasks, id);
    return n == GuestIntMap_End(&mgr->tasks) ? nullptr : P<Task>(n->value);
}
static inline const PlaceSet* Places(int table, int area) { return P<PlaceSet>(sPositions[table]) + area; }
static inline CharaMgr* Charas(World* w) { return P<CharaMgr>(w->charaMgr); }
static inline void* Talk(World* w) { return P(w->effects); }
static inline void* CamList(World* w) { return P<u8>(w->stage) + 0x60; }

static void DestroyAction(u32 action) {
    if (action != 0)
        VirtualCall<void*, void*, int>(P<TaskAction>(action)->vtable, 0x08, P(action), 1);
}
// Removes what `id` has running in one of the three maps.
static void DropAction(GuestIntMap* map, int id) {
    GuestMapNode* n = GuestIntMap_Find(map, id);
    if (n != GuestIntMap_End(map)) {
        DestroyAction(n->value);
        GuestIntMap_Erase(map, n);
    }
}
static void ReplaceAction(GuestIntMap* map, int id, TaskAction* action) {
    DropAction(map, id);
    GuestIntMap_Insert(map, id, guest::addr(action));
}
static void DropTalkStarts(TaskMgr* mgr, int actor) {
    GuestListNode* n = GuestList_Begin(&mgr->talkStarts);
    while (n != GuestList_End(&mgr->talkStarts)) {
        TalkStart* t = P<TalkStart>(n->value);
        if (t->actor == actor) {
            MemFree(t);
            n = GuestList_Erase(&mgr->talkStarts, n);
        } else {
            n = GuestList_Next(n);
        }
    }
}

// 0x1754C0 — who last hit this character, −1 nobody.
int Collision_AttackerOf(int id) {
    Chara* c = CharaMgr_Find(*gCollisionCharas, id);
    const s32* attacker = c != nullptr ? P<s32>(c->_1dc) : nullptr;
    return attacker != nullptr ? *attacker : -1;
}

// 0x1C8BC0 — how squarely a character (by its matrix) faces a point: the
// cosine of the angle, on the ground plane.
float Chara_FacingTowards(const void* matrix, const void* point) {
    const float* m = static_cast<const float*>(matrix);
    const float* p = static_cast<const float*>(point);
    const float dx = p[0] - m[12], dz = p[2] - m[14];
    const float length = __builtin_sqrtf(dx * dx + dz * dz);
    // (0x136D50 normalises on VU0, where dividing by zero gives a huge number, not a fault.)
    const float inv = length != 0.0f ? 1.0f / length : 3.4028235e38f;
    return m[8] * dx * inv + m[10] * dz * inv;
}

// ── The player is given the controls back (after being hit, or when a
// conversation it was held in has nobody left) ─────────────────────────────
static bool FreePlayer(World* world, CharaMgr* charas) {
    if (CharaMgr_IsHeld(charas, 0) == 0)
        return false;
    void* mem = MemAlloc(0xC0);
    void* cam = nullptr;
    if (mem != nullptr)
        cam = FollowCam_Ctor(mem, CharaResources_GetParams(CharaResources_Get(), 0),
                             P(gIndoors == 1 ? 0x2E0070u : 0x2E0020u));
    CamList_Push(CamList(world), cam);
    CharaMgr_Release(charas, 0);
    CharaMgr_ClearB5(charas);
    Effects_fn18e730(Talk(world), 0);
    return true;
}

// 0x190010 belongs to the conversation manager: ends the talk camera between
// two characters.  With the stand-in manager there never is one.
extern "C" void Effects_EndTalkCamera(void* talk, void* camList, CharaMgr* charas, int a, int b);

// ── 0x1BF140 ──────────────────────────────────────────────────────────────
// Everything `id` has going stops; its current command will be issued again.
static void StopActions(TaskMgr* mgr, int id, CharaMgr*, void* talk) {
    Effects_EndTalkOf(talk, id);
    DropAction(&mgr->motions, id);
    DropAction(&mgr->talks, id);
    DropAction(&mgr->ais, id);
    DropTalkStarts(mgr, id);
    if (Task* task = TaskOf(mgr, id)) {
        task->waiting[task->level] = 0;
        task->started[task->level] = 0;
        for (TaskWatch& w : task->watch) {
            w.on = 0;
            w.line = 0;
            w.distance = w.facing = 0.0f;
            GuestList_Clear(&w.who);
        }
        for (TaskCheck& c : task->check) {
            c.on = 0;
            c.line = 0;
            c.level = -1;
            GuestList_Clear(&c.who);
        }
        task->checkLife.on = task->checkLife.line = task->checkLife.life = 0;
        GuestList_Clear(&task->checkLife.who);
        task->checkCatch = {0, 0, 0, -1};
        task->checkChange = {0, 0, 0, 0};
        task->watch48 = {0, 0, 0};
    }
}

// ── 0x1BF540 ──────────────────────────────────────────────────────────────
// The same for its motion and speech only.
static void StopTalk(TaskMgr* mgr, int id, CharaMgr*, void* talk) {
    Effects_EndTalkOf(talk, id);
    DropAction(&mgr->motions, id);
    DropAction(&mgr->talks, id);
    DropTalkStarts(mgr, id);
    if (Task* task = TaskOf(mgr, id)) {
        task->waiting[task->level] = 0;
        task->started[task->level] = 0;
    }
}

// ── 0x1BF720 ──────────────────────────────────────────────────────────────
static void Remove(TaskMgr* mgr, void* talk, int id) {
    Effects_EndTalkOf(talk, id);
    DropAction(&mgr->motions, id);
    DropAction(&mgr->talks, id);
    DropAction(&mgr->ais, id);
    GuestMapNode* n = GuestIntMap_Find(&mgr->tasks, id);
    if (n != GuestIntMap_End(&mgr->tasks)) {
        if (n->value != 0)
            Task_Dtor(P<Task>(n->value), 1);
        GuestIntMap_Erase(&mgr->tasks, n);
    }
    DropTalkStarts(mgr, id);
}

static void ToLevelZero(Task* task, int line) {
    task->level = 0;
    for (int i = 0; i < kTaskLevels; i++)
        task->line[i] = task->prevLine[i] = task->frames[i] = task->started[i] = task->waiting[i] = task->target[i] = 0;
    task->line[0] = line;
}

// ── 0x1BC3C0 ──────────────────────────────────────────────────────────────
// Return: back to the normal program, at `line`.
static void ReturnTo(TaskMgr* mgr, int line, int id, CharaMgr* charas, void* talk) {
    if (Task* task = TaskOf(mgr, id)) {
        ToLevelZero(task, line);
        Task_SetBattleMode(task, 0);
        if (task->weaponOff == 0)
            task->weaponOff = 1;
    }
    CharaMgr_Set6C(charas, id, 0);
    StopActions(mgr, id, charas, talk);
}

// ── 0x1BC600 ──────────────────────────────────────────────────────────────
// Ends `id`'s pairing with its partner.
static int Unpair(TaskMgr* mgr, int id, CharaMgr* charas, void* talk, int force) {
    Task* task = TaskOf(mgr, id);
    if (task == nullptr || (force == 0 && task->level == kTaskPartner))
        return 0;
    if (CharaMgr_GetPartner(charas, id) != -1 && CharaMgr_Get70(charas, id) != 0) {
        if (task->level < kTaskWeapon)
            ToLevelZero(task, 0);
        CharaMgr_Set6C(charas, id, 0);
        StopActions(mgr, id, charas, talk);
    }
    CharaMgr_SetPartner(charas, id, -1);
    CharaMgr_Set70(charas, id, 0);
    return 1;
}

// ── 0x1BC540 ──────────────────────────────────────────────────────────────
// The level changed: start the new program clean.
static void LevelChanged(TaskMgr* mgr, int id, CharaMgr* charas, void* talk) {
    CharaMgr_Set6C(charas, id, 0);
    StopActions(mgr, id, charas, talk);
    const int partner = CharaMgr_GetPartner(charas, id);
    if (Unpair(mgr, id, charas, talk, 0) != 0 && partner != -1)
        Unpair(mgr, partner, charas, talk, 1);
}

// ── 0x1BE4B0 ──────────────────────────────────────────────────────────────
// Polls what the actor has running in one map; when it is finished the task
// moves on.  Returns what its Update returned.
static int PollAction(GuestIntMap* map, int id, Task* task) {
    GuestMapNode* n = GuestIntMap_Find(map, id);
    if (n == GuestIntMap_End(map))
        return 0;
    TaskAction* action = P<TaskAction>(n->value);
    const int done = VirtualCall<int, TaskAction*>(action->vtable, 0x0C, action);
    if (done == 0)
        return 0;
    if (action->nextLine != -1) {
        task->prevLine[task->level] = task->line[task->level];
        task->line[task->level] = action->nextLine;
        task->waiting[task->level] = 0;
        task->started[task->level] = 0;
    }
    DestroyAction(n->value);
    GuestIntMap_Erase(map, n);
    return done;
}

// ── 0x1BEB60 ──────────────────────────────────────────────────────────────
// Offers the nearest conversation that is waiting for the player (name plate
// on the HUD); Circle starts it.
static void OfferTalk(TaskMgr* mgr, CharaMgr* charas, void* talk) {
    if (Effects_IsBusy(talk, 0) != 0)
        return;
    void* pad = PadMgr_Get();
    float bestDistance = 1000.0f, bestFacing = 0.0f;
    TalkStart* best = nullptr;
    for (GuestListNode* n = GuestList_Begin(&mgr->talkStarts); n != GuestList_End(&mgr->talkStarts);
         n = GuestList_Next(n)) {
        TalkStart* t = P<TalkStart>(n->value);
        const CharaParams* a = CharaMgr_GetParams(charas, t->who1);
        const CharaParams* b = CharaMgr_GetParams(charas, t->who2);
        if (a == nullptr || b == nullptr)
            continue;           // (the original assumes both exist)
        const float distance = Vec_Distance(&a->matrix[12], &b->matrix[12]);
        if (getenv("WOTS2_DUMP_TALK"))          // debug hook, not in the original
            printf("[talk] %d waits for %d: distance %.2f, facing %.2f, sight %d\n", t->who1, t->who2, distance,
                   Chara_FacingTowards(b->matrix, &a->matrix[12]), Collision_CanSee(t->who1, t->who2, 2));
        if (!(distance < 3.0f))
            continue;
        const float facing = Chara_FacingTowards(b->matrix, &a->matrix[12]);
        if (0.6f <= facing && Collision_CanSee(t->who1, t->who2, 2) != 0 && distance < bestDistance
            && bestFacing < facing) {
            best = t;
            bestDistance = distance;
            bestFacing = facing;
        }
    }
    if (best == nullptr || (CharaMgr_GetFlags(charas, 0) & 0x100) != 0)
        return;
    const CharaParams* who = CharaMgr_GetParams(charas, best->who1);
    struct { u32 params, name; s32 kind; u32 flag; } info;
    info.params = guest::addr(who);
    info.name = P<WStringRep>(who->name.rep)->data;
    info.kind = 2;
    info.flag = who->_14 != 0;
    Hud_ShowTalkPrompt(*gHud, &info);
    if ((Pad_GetPressed(pad, 0) & kPadCircle) == 0 || (Pad_GetHeld(pad, 0) & kPadR1) != 0)
        return;
    if (Task* task = TaskOf(mgr, best->actor)) {
        Task_Goto(task, best->line);
        // ("TalkStart")
        StopTalk(mgr, best->who1, charas, talk);
        *reinterpret_cast<s16*>(reinterpret_cast<u8*>(*gPlayerData) + 0x85E) += 1;     // conversations had
    }
    GuestListNode* n = GuestList_Begin(&mgr->talkStarts);
    while (n != GuestList_End(&mgr->talkStarts)) {
        if (P<TalkStart>(n->value) == best) {
            MemFree(best);
            n = GuestList_Erase(&mgr->talkStarts, n);
        } else {
            n = GuestList_Next(n);
        }
    }
}

// ── 0x1BEEC0 ──────────────────────────────────────────────────────────────
// The events that raise an actor's level, most urgent first.
static bool CheckEvents(TaskMgr* mgr, int id, Task* task, CharaMgr* charas, void* talk) {
    Chara* chara = CharaMgr_Find(charas, id);
    bool changed = Task_CheckDead(task, charas, chara) != 0;        // ("ChangePriorityTask -> Dead!!")
    if (task->stopped != 0)
        return false;
    if (!changed)
        changed = Task_CheckDamage(task, charas, chara) != 0
               || Task_CheckCrime(task, charas, chara) != 0
               || Task_CheckWeapon(task, charas, chara) != 0
               || Task_CheckPartner(task, charas, chara) != 0
               || Task_CheckBattle(task, charas, chara) != 0
               || Task_CheckFindTarget(task, &mgr->tasks, charas, talk, chara) != 0
               || Task_CheckCrash(task, charas, chara) != 0;
    if (changed)
        LevelChanged(mgr, id, charas, talk);
    return changed;
}

static int ResolveWho(s32 who, int self, const TaskCommand* cmd, CharaMgr* charas) {
    switch (who) {
    case kWhoOther:   return cmd->other;
    case kWhoPartner: {
        const int partner = CharaMgr_GetPartner(charas, self);
        return partner != -1 ? partner : self;
    }
    case kWhoTarget:  return cmd->target;
    case kWhoPlayer:  return 0;
    case kWhoSelf:    return self;
    default:          return who;
    }
}

// ── 0x1BC7E0 ──────────────────────────────────────────────────────────────
// Carries out the command the interpreter handed back.  −1, or a request for
// the scene.  (The names in quotes are the game's own debug output.)
static int Execute(TaskMgr* mgr, int self, Task* task, TaskCommand* cmd, World* world, void* spawner) {
    CharaMgr* charas = Charas(world);
    void* talk = Talk(world);
    void* drops = P(world->itemDrops);
    PlayerData* player = *gPlayerData;
    u8* pd = reinterpret_cast<u8*>(player);
    const int who1 = ResolveWho(cmd->who1, self, cmd, charas);
    const int who2 = ResolveWho(cmd->who2, self, cmd, charas);
    Chara* whoChara = CharaMgr_Find(charas, who1);
    const int area = player->area;
    const s32* arg = cmd->arg;
    const u16* text = P<const u16>(cmd->text);

    switch (cmd->op) {
    case kOpSetPos: {                           // "SetPos ID[%d] POSID[%d]"
        CharaMgr_SetActive(charas, self, 1);
        Mt_Seed(Timer1_Count());
        const PlaceSet* set = Places(arg[0], area);
        int place = arg[1];
        if (place == -1) {
            // Any place nobody has been given yet.
            const u32 drawn = Mt_Below((u32)set->count);
            auto used = [mgr](u32 index) {
                for (GuestListNode* n = GuestList_Begin(&mgr->usedPlaces); n != GuestList_End(&mgr->usedPlaces);
                     n = GuestList_Next(n))
                    if (n->value == index)
                        return true;
                return false;
            };
            bool isFree = !used(drawn);
            u32 pick = drawn;
            if (!isFree) {
                for (u32 i = 0; (s32)i < set->count; i++) {
                    isFree = !used(i);
                    pick = i;
                    if (isFree)
                        break;
                    pick = drawn;
                }
                if ((u32)set->count < mgr->usedPlaces.size)
                    pick = Mt_Below((u32)set->count);
            }
            if (isFree)
                GuestList_Insert(&mgr->usedPlaces, GuestList_End(&mgr->usedPlaces), pick);
            place = (int)pick;
        }
        CharaMgr_SetPosition(charas, self, P<float>(set->points) + place * 4);
        break;
    }
    case kOpStopPriorityTask:
        if (who1 != -1)
            if (Task* other = TaskOf(mgr, who1))
                other->stopped = arg[0];
        break;
    case kOpSetOtherMessage:
        WStr_Copy(reinterpret_cast<u16*>(pd + 0x6D40), text);
        break;
    case kOpSetWorkOut:
        DayMgr_EndPeriodNow(DayMgr_Get());
        break;
    case kOpSetItemPos: {
        alignas(16) float matrix[16];
        Matrix_Identity(matrix);
        memcpy(&matrix[12], P<float>(Places(arg[1], area)->points) + arg[2] * 4, 16);
        ItemDrops_Place(drops, arg[0], matrix);
        break;
    }
    case kOpSetItem:
        if (PlayerData_AddItem(player, arg[0], (u8)ItemDrops_ItemId(drops, arg[1])) == 0)
            if (Task* own = TaskOf(mgr, self))
                Task_Goto(own, arg[2]);         // no room: "Jump -> Line[%d]"
        break;
    case kOpDropItem:
        if (who1 != -1) {
            alignas(16) float matrix[16];
            Matrix_Identity(matrix);
            memcpy(&matrix[12], CharaMgr_GetPosition(charas, who1), 16);
            ItemDrops_Drop(drops, arg[0], matrix);
        }
        break;
    case kOpDeleteNormalItem:
        PlayerData_RemoveCarriedItem(player, (u32)ItemDrops_ItemId(drops, arg[0]));
        break;
    case kOpDeleteWorkItem:
        PlayerData_DropQuestItems(player);
        break;
    case kOpCheckItem:
        if (PlayerData_HasItem(player, arg[0], (u32)ItemDrops_ItemId(drops, arg[1])) != 0)
            if (Task* own = TaskOf(mgr, self))
                Task_Goto(own, arg[2]);
        break;
    case kOpSetWorkName:                        // the job taken, shown as a plate
        memset(pd + 0x4A0, 0, 0x30);
        if (WStr_Length(text) < 0x18)
            WStr_Copy(reinterpret_cast<u16*>(pd + 0x4A0), text);
        Hud_ShowDate(*gHud, text, (int)((float)gFrameRate * 3.5f), 0);
        DayMgr_StartJob(DayMgr_Get());
        break;
    case kOpSetDamageStopFlag:
        if (who1 != -1)
            if (CharaParams* p = CharaResources_GetParams(CharaResources_Get(), who1))
                p->_48 = (s16)arg[0];
        break;
    case kOpStopActionCount:
        DayMgr_SetEvent(DayMgr_Get(), arg[0]);
        break;
    case kOpSetWorkMessage:
        memset(pd + 0x4D0, 0, 0xF0);
        if (cmd->textLength < 0xF0)
            WStr_Copy(reinterpret_cast<u16*>(pd + 0x4D0), text);
        break;
    case kOpChrReserve:
        CharaMgr_SetActive(charas, self, 1);
        break;
    case kOpSetPrision: {                       // the player is arrested
        const int partner = Effects_PartnerOf(talk, 0);
        if (partner != -1) {
            Effects_EndTalkCamera(talk, CamList(world), charas, partner, 0);
            Effects_EndTalkOf(talk, partner);
        }
        Effects_EndAll(talk, -1);
        return 0x80;
    }
    case kOpSetDirection:
        if (who1 != -1) {
            alignas(16) float point[4] = {};
            if (arg[0] != -1)
                memcpy(point, P<float>(Places(2, area)->points) + arg[0] * 4, 16);
            void* mem = MemAlloc(0x40);
            TaskAction* action = mem != nullptr ? TaskDirection_Ctor(mem, self, cmd, charas, point) : nullptr;
            ReplaceAction(&mgr->motions, self, action);
        }
        break;
    case kOpSetWeaponOff:
        if (who1 != -1)
            if (Task* other = TaskOf(mgr, who1))
                if (other->weaponOff == 0)
                    other->weaponOff = 1;
        break;
    case kOpSetPartnerID:
        if (who1 != -1 && who2 != -1) {
            int partner = CharaMgr_GetPartner(charas, who1);
            Unpair(mgr, who1, charas, talk, 1);
            Unpair(mgr, partner, charas, talk, 1);
            partner = CharaMgr_GetPartner(charas, who2);
            Unpair(mgr, who2, charas, talk, 1);
            Unpair(mgr, partner, charas, talk, 1);
            if (arg[0] != 0) {
                CharaMgr_SetPartner(charas, who1, who2);
                CharaMgr_SetPartner(charas, who2, who1);
                CharaMgr_Set70(charas, who2, 1);
            }
        }
        break;
    case kOpSetRoomIn:                          // the player is taken through an exit
        Effects_EndAll(talk, -1);
        return arg[0];
    case kOpDrawInfoPlate:
        Hud_ShowDate(*gHud, text, (int)((float)arg[0] / 1000.0f * (float)gFrameRate), 0);
        break;
    case kOpSetTalkCameraOff:
        if (who1 != -1 && who2 != -1) {
            Effects_EndTalkCamera(talk, CamList(world), charas, who1, who2);
            Effects_EndTalkOf(talk, who1);
        }
        break;
    case kOpSetBattleMode:
        if (who1 != -1)
            if (Task* other = TaskOf(mgr, who1))
                Task_SetBattleMode(other, arg[0]);
        break;
    case kOpMapOut:
        Spawner_ComeBack(spawner, charas, self);
        break;
    case kOpSetDamageTargetID:
        if (who1 != -1)
            if (Task* other = TaskOf(mgr, who1 == 0 ? self : who1))
                Task_SetDamageWho(other, cmd);
        break;
    case kOpReturn:
        ReturnTo(mgr, arg[0], self, charas, talk);
        break;
    case kOpSetAngle:
        if (who1 != -1) {
            alignas(16) float matrix[16], point[4];
            Matrix_Copy(matrix, CharaMgr_GetTransform(charas, self));
            memcpy(point, P<float>(Places(2, area)->points) + arg[0] * 4, 16);
            // (facing the spot it stands on is reported as an error)
            if (!(point[0] == matrix[12] && point[1] == matrix[13] && point[2] == matrix[14])) {
                Matrix_FaceTowards(matrix, point);
                CharaMgr_SetTransform(charas, self, matrix);
            }
        }
        break;
    case kOpJump:
        StopTalk(mgr, self, charas, talk);
        break;
    case kOpPlayVoice:
        if (who1 != -1 && whoChara != nullptr && arg[0] != -1)
            Sound_Cry(P(whoChara->params), arg[0]);
        break;
    case kOpPlaySE:
        if (who1 != -1 && whoChara != nullptr)
            Sound_PlaySE(arg[0], (int)(whoChara->params + 0x80), 0x3FFF);
        break;
    case kOpSetAI:                              // "SetAI ID1[%d] ID2[%d] ARG[%d][%d]"
        if (who1 != 0 && who1 != -1 && who2 != -1 && whoChara != nullptr) {
            void* mem = MemAlloc(0x18);
            TaskAction* action = mem != nullptr ? TaskAI_Ctor(mem, self, cmd, whoChara, P(world->mapState)) : nullptr;
            if (arg[0] == 1) {                  // fight
                if (Task* other = TaskOf(mgr, who1))
                    Task_SetBattleMode(other, 1);
                CharaMgr_SetOpponent(charas, who1, who1 == who2 ? 0 : who2, 0);
            } else {
                CharaMgr_fn143140(charas, who1);
            }
            ReplaceAction(&mgr->ais, who1, action);
        }
        break;
    case kOpPlayMotion:
        if (who1 != -1) {
            void* mem = MemAlloc(0x38);
            ReplaceAction(&mgr->motions, self, mem != nullptr ? TaskMotion_Ctor(mem, self, cmd, charas) : nullptr);
        }
        break;
    case kOpDrawTalkSelect:
    case kOpDrawSerif:
    case kOpCheckTalkStart:
        if (who1 == -1 || who2 == -1) {
            FreePlayer(world, charas);          // "BaseChara Error [%d] [%d]"
            break;
        }
        if (cmd->op == kOpCheckTalkStart) {
            TalkStart* start = static_cast<TalkStart*>(MemAlloc(sizeof(TalkStart)));
            *start = {self, arg[0], who1, who2};
            for (GuestListNode* n = GuestList_Begin(&mgr->talkStarts); n != GuestList_End(&mgr->talkStarts);
                 n = GuestList_Next(n))
                if (P<TalkStart>(n->value)->actor == self) {
                    MemFree(P(n->value));
                    GuestList_Erase(&mgr->talkStarts, n);
                    break;
                }
            GuestList_Insert(&mgr->talkStarts, GuestList_End(&mgr->talkStarts), guest::addr(start));
        } else {
            Chara* other = CharaMgr_Find(charas, who2);
            const bool select = cmd->op == kOpDrawTalkSelect;
            // The line can name where to go when the listener is (for a
            // choice: is not) busy with something else.
            if (arg[3] != -1 && other != nullptr
                && (Task_CharaBusy(task, other, talk, &mgr->tasks, who1) != 0) != select) {
                Effects_EndTalkCamera(talk, CamList(world), charas, who1, who2);
                Effects_EndTalkOf(talk, who1);
                Task_Goto(task, arg[3]);        // "JUMP -> [%d]"
                break;
            }
            void* mem = MemAlloc(0x24);
            TaskAction* action = nullptr;
            if (mem != nullptr)
                action = select ? TaskTalkSelect_Ctor(mem, self, cmd, charas, talk)
                                : TaskSerif_Ctor(mem, self, cmd, charas, talk);
            Effects_fn18e890(talk, action->handle);
            ReplaceAction(&mgr->talks, self, action);
            if (!select && arg[2] != -1 && whoChara != nullptr)
                Sound_Cry(P(whoChara->params), arg[2]);          // the cry that goes with the line
        }
        break;
    default:
        break;
    }
    return -1;
}

extern "C" {

// ── 0x1BE5E0 ──────────────────────────────────────────────────────────────
int TaskMgr_Update(void* mem, World* world, void* spawner) {
    TaskMgr* mgr = static_cast<TaskMgr*>(mem);
    const s32 indoors = gIndoors;
    CharaMgr* charas = Charas(world);
    void* talk = Talk(world);
    int result = -1;

    // An actor whose death program has run out is gone (one a frame).
    for (GuestMapNode* n = GuestIntMap_Begin(&mgr->tasks); n != GuestIntMap_End(&mgr->tasks);
         n = GuestIntMap_Next(n)) {
        const Task* task = P<Task>(n->value);
        if (task->level == kTaskDead && task->programs[kTaskDead].count <= (u32)task->line[kTaskDead]) {
            CharaMgr_fn143140(charas, n->key);
            Remove(mgr, talk, n->key);
            break;
        }
    }
    OfferTalk(mgr, charas, talk);

    for (GuestMapNode* n = GuestIntMap_Begin(&mgr->tasks); n != GuestIntMap_End(&mgr->tasks);
         n = GuestIntMap_Next(n)) {
        const int id = n->key;
        Task* task = P<Task>(n->value);
        Chara* chara = CharaMgr_Find(charas, id);
        // Only those loaded and in the place the player is in.
        if (task == nullptr || chara == nullptr || chara->loaded == 0 || indoors != chara->indoors
            || (chara->indoors != 0 && chara->interior != world->doorIndex))
            continue;
        if (task->restart != 0) {
            chara->active = task->active;
            CharaMgr_SetState(charas, id, 2, 0, 0);
            task->restart = 0;
        }
        Task_StepWeaponOff(task, chara);
        PollAction(&mgr->motions, id, task);
        if (PollAction(&mgr->talks, id, task) == -1)
            Effects_EndTalkOf(talk, id);
        const int ai = PollAction(&mgr->ais, id, task);
        if (ai == -1)
            Spawner_ComeBack(spawner, charas, id);      // "MapOutMoveEnd!": it walked out of the area
        if (CheckEvents(mgr, id, task, charas, talk)) {
            Effects_EndTalkCamera(talk, CamList(world), charas, id, 0);
            Effects_EndTalkOf(talk, id);
        }
        const int a = Task_CheckWatches(task, &mgr->tasks, charas, talk, chara);
        const int b = Task_CheckTargetLife(task, id, charas);
        const int c = Task_CheckCatchTarget(task, id, charas);
        const int d = Task_CheckCollisionTarget(task, charas);
        const int e = Task_CheckPriorityTarget(task, &mgr->tasks, charas);
        const int f = Task_CheckTargetChange(task, charas);
        if (f != 0 || e != 0 || d != 0 || c != 0 || b != 0 || a != 0 || ai == -1)
            StopActions(mgr, id, charas, talk);
        // Its opponent (someone other than the player) is down: back to normal.
        if ((P<CharaBody>(chara->body)->flags & 0x100) != 0) {
            const CharaParams* opponent = P<CharaParams>(P<CharaParams>(chara->params)->link[3]);
            if (opponent != nullptr && opponent->id != 0 && opponent->hp < 1 && task->weaponOff == 0) {
                CharaMgr_fn143140(charas, id);
                ReturnTo(mgr, 0, id, charas, talk);     // "TargetCharacter BattleEnd"
            }
        }
        if (TaskCommand* cmd = Task_Step(task, id, &mgr->tasks, charas)) {
            const int request = Execute(mgr, id, task, cmd, world, spawner);
            if (request != -1)
                result = request;
        }
    }

    // "Plyaer Damage Reset"
    if (CharaMgr_GetFlags(charas, 0) & 0x10000)
        FreePlayer(world, charas);
    return result;
}

// ── 0x1BF960 ──────────────────────────────────────────────────────────────
void TaskMgr_Add(void* mem, u32 id, u32 data, u32 size) {
    TaskMgr* mgr = static_cast<TaskMgr*>(mem);
    if (Task* task = TaskOf(mgr, (int)id)) {
        Task_Restart(task);
        task->restart = 1;
        task->active = 1;
        return;
    }
    Task* task = static_cast<Task*>(MemAlloc(sizeof(Task)));
    if (task != nullptr)
        Task_Ctor(task, P(data), (int)size, sArgs);
    GuestIntMap_Insert(&mgr->tasks, (s32)id, guest::addr(task));
}

// ── 0x1BFA30 ──────────────────────────────────────────────────────────────
void TaskMgr_Reset(void* mem, World* world) {
    TaskMgr* mgr = static_cast<TaskMgr*>(mem);
    CharaMgr* charas = Charas(world);
    if (charas == nullptr || world->mapState == 0 || gIndoors != 1)
        return;
    GuestListNode* n = GuestList_Begin(&charas->charas);
    while (n != GuestList_End(&charas->charas)) {
        GuestListNode* next = GuestList_Next(n);
        Chara* c = P<Chara>(n->value);
        const int id = c->id;
        Task* task = id != 0 && c->indoors == 1 && c->interior == world->doorIndex ? TaskOf(mgr, id) : nullptr;
        if (task != nullptr) {
            if (task->level == kTaskDead) {
                Remove(mgr, Talk(world), id);
            } else {
                StopActions(mgr, id, charas, Talk(world));
                Task_Restart(task);
                Chara_Reset(c);
                c->active = 1;
                c->_68 = 0;
                c->_6c = 0;
                c->_70 = 0;
                CharaMgr_RestoreSword(charas, SwordDb_Get(), id);
                if (c->swords != 0)
                    CharaSwords_fn2454b0(P<CharaSwords>(c->swords));
                if (P<CharaBody>(c->body)->flags & 0x100)
                    CharaMgr_Sheathe(charas, id, 0);
                const int partner = c->partner;
                CharaMgr_SetPartner(charas, id, -1);
                if (partner != -1) {
                    CharaMgr_Set70(charas, partner, 0);
                    CharaMgr_SetPartner(charas, partner, -1);
                }
            }
        }
        n = next;
    }
}

// ── 0x1C0030 ──────────────────────────────────────────────────────────────
void* TaskMgr_Ctor(void* mem) {
    TaskMgr* mgr = static_cast<TaskMgr*>(mem);
    GuestList_Init(&mgr->usedPlaces);
    GuestIntMap_Init(&mgr->tasks);
    GuestIntMap_Init(&mgr->motions);
    GuestIntMap_Init(&mgr->talks);
    GuestIntMap_Init(&mgr->ais);
    GuestList_Init(&mgr->talkStarts);
    // (The argument table is a global object; the original constructs it with
    // the other static constructors before main.)
    if (sArgs->endNext == 0)
        GuestList_Init(sArgs);
    if (sLoaded == 0) {
        // Asset 0x41: a count and that many ints, which task files refer to by index.
        GuestList_Clear(sArgs);
        ArchiveFile* file = ArchiveFile_Create();
        ArchiveFile_Open(file, 0x41);
        const u32 size = ArchiveFile_GetSize(file);
        s32* data = static_cast<s32*>(MemAllocAligned(0x40, size));
        ArchiveFile_Read(file, data, size);
        ArchiveFile_Destroy(file);
        for (s32 i = 0; i < data[0]; i++)
            GuestList_Insert(sArgs, GuestList_End(sArgs), (u32)data[1 + i]);
        MemFreeAligned(data);
        sLoaded = 1;
    }
    return mgr;
}

// ── 0x1BFC20 ──────────────────────────────────────────────────────────────
void TaskMgr_Dtor(void* mem, int flag) {
    TaskMgr* mgr = static_cast<TaskMgr*>(mem);
    if (mgr == nullptr)
        return;
    GuestList_Clear(&mgr->usedPlaces);
    for (GuestMapNode* n = GuestIntMap_Begin(&mgr->tasks); n != GuestIntMap_End(&mgr->tasks);
         n = GuestIntMap_Next(n))
        if (n->value != 0)
            Task_Dtor(P<Task>(n->value), 1);
    GuestIntMap_Clear(&mgr->tasks);
    for (GuestIntMap* map : {&mgr->motions, &mgr->talks, &mgr->ais}) {
        for (GuestMapNode* n = GuestIntMap_Begin(map); n != GuestIntMap_End(map); n = GuestIntMap_Next(n))
            DestroyAction(n->value);
        GuestIntMap_Clear(map);
    }
    GuestListNode* n = GuestList_Begin(&mgr->talkStarts);
    while (n != GuestList_End(&mgr->talkStarts)) {
        if (n->value != 0)
            MemFree(P(n->value));
        n = GuestList_Erase(&mgr->talkStarts, n);
    }
    if ((s16)flag > 0)
        MemFree(mgr);
}

// ── 0x1BFFE0 ──────────────────────────────────────────────────────────────
void TaskMgr_ReleaseAll() {
    sLoaded = 0;
    if (sArgs->endNext != 0)
        GuestList_Clear(sArgs);
}

}  // extern "C"
