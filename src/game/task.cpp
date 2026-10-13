// Actor tasks — decompiled from 0x1C0F20–0x1C6980.  See include/game/task.h.

#include "game/task.h"
#include "engine/mt_random.h"
#include "game/day_mgr.h"
#include "game/player_data.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* MemAllocAligned(u32 align, u32 size);         // 0x1228C8
    void  MemFreeAligned(void* p);                      // 0x122B50
    int   CharaMgr_GetFlags(CharaMgr* mgr, int id);     // 0x1467B0
    int   CharaMgr_GetPartner(CharaMgr* mgr, int id);   // 0x146B00
    int   CharaMgr_Get6C(CharaMgr* mgr, int id);        // 0x146BC0
    void  CharaMgr_Set6C(CharaMgr* mgr, int id, int value);     // 0x1460A0
}

static_assert(sizeof(TaskCommand) == 0x4C && sizeof(TaskWatch) == 0x1C && sizeof(TaskCheck) == 0x18, "TaskCommand");
static_assert(offsetof(Task, line) == 0xB4 && offsetof(Task, target) == 0x190 && offsetof(Task, local) == 0x1BC, "Task");
static_assert(offsetof(Task, findRange) == 0x3BC && offsetof(Task, findWho) == 0x3EC, "Task");
static_assert(offsetof(Task, checkLife) == 0x434 && offsetof(Task, checkCatch) == 0x44C, "Task");
static_assert(offsetof(Task, check) == 0x478 && offsetof(Task, watch) == 0x4A8 && sizeof(Task) == 0x518, "Task");

static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);
// Whom the level-raising events react to unless a program says otherwise.
static const s32* const kDefaultWho = guest::ptr<const s32>(0x2EDCBCu);    // crash, weapon, battle, damage, crime

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline void Append(GuestList* list, u32 value) { GuestList_Insert(list, GuestList_End(list), value); }
static inline CharaParams* Params(Chara* c) { return P<CharaParams>(c->params); }
static inline u32 Flags(Chara* c) { return P<CharaBody>(c->body)->flags; }
static inline const float* Position(Chara* c) { return &Params(c)->matrix[12]; }
static inline TaskCommand* CommandAt(Task* t, int level, u32 line) {
    return P<TaskCommand>(P<u32>(t->programs[level].data)[line]);
}
static inline Task* TaskOf(GuestIntMap* tasks, int id) {
    GuestMapNode* n = GuestIntMap_Find(tasks, id);
    return n == GuestIntMap_End(tasks) ? nullptr : P<Task>(n->value);
}
static int ClampIndex(int index, int count) { return (index < 0 || index >= count) ? 0 : index; }      // 0x1772D0

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

// ══ Loading ═══════════════════════════════════════════════════════════════

// ── 0x1C5AB0 ──────────────────────────────────────────────────────────────
// File: u32, then for each of the eleven levels a count and its commands
// {index, op, float seconds, who1, who2, text bytes, [text], 5 × {type, value}}.
void Task_Load(Task* self, const void* data, int size) {
    const s32* in = static_cast<const s32*>(data) + 1;
    const s32* const begin = in;
    for (int level = 0; level < kTaskLevels; level++) {
        const s32 count = *in++;
        if (count < 1)
            continue;       // (a negative count is reported: "Task CommandNum Error")
        bool last = false;
        do {
            TaskCommand* cmd = static_cast<TaskCommand*>(MemAlloc(sizeof(TaskCommand)));
            if (cmd != nullptr) {
                memset(cmd, 0, sizeof *cmd);
                for (int k = 0; k < 5; k++)
                    cmd->argType[k] = cmd->arg[k] = -1;
            }
            cmd->index = in[0];
            cmd->op = in[1];
            float seconds;
            memcpy(&seconds, in + 2, 4);
            cmd->wait = (seconds == 0.0f || seconds == -1.0f) ? (s32)seconds : (s32)(seconds * (float)gFrameRate);
            cmd->who1 = in[3];
            cmd->who2 = in[4];
            cmd->textLength = in[5];
            in += 6;
            if (cmd->textLength > 0) {
                u8* text = static_cast<u8*>(MemAllocAligned(0x10, (u32)cmd->textLength + 2));
                cmd->text = guest::addr(text);
                memset(text, 0, (size_t)cmd->textLength + 2);
                memcpy(text, in, (size_t)cmd->textLength);
                in = reinterpret_cast<const s32*>(reinterpret_cast<const u8*>(in) + (cmd->textLength & ~3));
            }
            for (int k = 0; k < 5; k++) {
                cmd->argType[k] = in[0];
                if (cmd->argType[k] == 1)
                    cmd->arg[k] = in[1] < self->argCount ? P<s32>(self->args)[in[1]] : 0;
                else
                    cmd->arg[k] = in[1];
                in += 2;
            }
            VectorU32_PushBack(&self->programs[level], guest::addr(cmd));
            if (cmd->index == count - 1)
                last = true;
        } while (!last && (reinterpret_cast<const u8*>(in) - reinterpret_cast<const u8*>(begin)) < size - 4);
    }
}

static void ClearWatch(TaskWatch* w) {              // 0x1BF4F0
    w->on = 0;
    w->line = 0;
    w->distance = 0.0f;
    w->facing = 0.0f;
    GuestList_Clear(&w->who);
}
static void ClearCheck(TaskCheck* c) {              // 0x1BF4A0
    c->on = 0;
    c->line = 0;
    c->level = -1;
    GuestList_Clear(&c->who);
}

// ── 0x1C5DB0 ──────────────────────────────────────────────────────────────
void Task_ResetState(Task* self) {
    self->stopped = 0;
    self->level = 0;
    self->active = 0;
    self->restart = 1;
    self->_20 = 0;
    self->weaponOff = 0;
    self->lastRandom = -1;
    memset(self->local, 0, sizeof self->local);
    for (int i = 0; i < kTaskLevels; i++)
        self->line[i] = self->prevLine[i] = self->frames[i] = self->started[i] = self->waiting[i] = self->target[i] = 0;
    self->checkChange = {0, 0, 0, 0};
    self->checkLife.on = self->checkLife.line = self->checkLife.life = 0;
    GuestList_Clear(&self->checkLife.who);
    self->checkCatch = {0, 0, 0, -1};
    self->watch48 = {0, 0, 0};
    for (TaskWatch& w : self->watch)
        ClearWatch(&w);
    for (TaskCheck& c : self->check)
        ClearCheck(&c);
    self->other = -1;
    self->battleMode = 0;
    self->_00 = 0;
    self->findRange[0] = 0.0f;  self->findRange[1] = 5.0f;  self->findRange[2] = 0.3f;
    self->range16[0] = self->range16[1] = self->range16[2] = 0.0f;
    self->weaponRange[0] = 2.4f;  self->weaponRange[1] = 0.0f;
    self->battleRange[0] = 4.0f;  self->battleRange[1] = 0.0f;
    self->crimeRange[0] = 8.0f;   self->crimeRange[1] = 0.3f;
    GuestList_Clear(&self->weaponWho);
    GuestList_Clear(&self->crashWho);
    GuestList_Clear(&self->battleWho);
    GuestList_Clear(&self->damageWho);
    Append(&self->crashWho, (u32)kDefaultWho[0]);
    Append(&self->weaponWho, (u32)kDefaultWho[1]);
    Append(&self->battleWho, (u32)kDefaultWho[2]);
    Append(&self->damageWho, (u32)kDefaultWho[3]);
    Append(&self->crimeWho, (u32)kDefaultWho[4]);   // (not cleared first: it grows with every restart)
}

// ── 0x1C6170 ──────────────────────────────────────────────────────────────
void Task_Restart(Task* self) {
    Mt_Seed(Timer1_Count());
    Task_ResetState(self);
    self->level = 0;
    for (int i = 0; i < kTaskLevels; i++)
        self->line[i] = self->prevLine[i] = self->frames[i] = self->started[i] = self->waiting[i] = self->target[i] = 0;
}

// ── 0x1C6980 ──────────────────────────────────────────────────────────────
Task* Task_Ctor(Task* self, const void* data, int size, GuestList* args) {
    for (GuestVector& v : self->programs)
        v = GuestVector{};
    for (GuestList* list : {&self->findWho, &self->weaponWho, &self->crashWho, &self->battleWho,
                            &self->damageWho, &self->crimeWho, &self->checkLife.who})
        GuestList_Init(list);
    self->checkLife.on = self->checkLife.line = self->checkLife.life = 0;
    self->checkCatch = {0, 0, 0, -1};
    self->checkChange = {0, 0, 0, 0};
    self->watch48 = {0, 0, 0};
    for (TaskCheck& c : self->check) {              // 0x1C6BD0
        GuestList_Init(&c.who);
        ClearCheck(&c);
    }
    for (TaskWatch& w : self->watch) {              // 0x1C6B70
        GuestList_Init(&w.who);
        ClearWatch(&w);
    }
    Task_ResetState(self);
    self->argCount = (s32)args->size;
    s32* table = static_cast<s32*>(MemAlloc((u32)self->argCount * 4));
    self->args = guest::addr(table);
    for (GuestListNode* n = GuestList_Begin(args); n != GuestList_End(args); n = GuestList_Next(n))
        *table++ = (s32)n->value;
    Task_Load(self, data, size);
    return self;
}

// ── 0x1C6260 ──────────────────────────────────────────────────────────────
Task* Task_Dtor(Task* self, s16 flag) {
    if (self == nullptr)
        return self;
    for (GuestVector& v : self->programs) {
        for (u32 i = 0; i < v.count; i++) {
            TaskCommand* cmd = P<TaskCommand>(P<u32>(v.data)[i]);
            if (cmd == nullptr)
                continue;
            if (cmd->text != 0) {
                MemFreeAligned(P(cmd->text));
                cmd->text = 0;
            }
            MemFree(cmd);
        }
        v.count = 0;
    }
    if (self->args != 0)
        MemFree(P(self->args));
    self->args = 0;
    self->checkChange = {0, 0, 0, 0};
    self->checkLife.on = self->checkLife.line = self->checkLife.life = 0;
    GuestList_Clear(&self->checkLife.who);
    self->checkCatch = {0, 0, 0, -1};
    self->watch48 = {0, 0, 0};
    for (TaskWatch& w : self->watch)
        ClearWatch(&w);
    for (TaskCheck& c : self->check)
        ClearCheck(&c);
    for (GuestList* list : {&self->weaponWho, &self->crashWho, &self->battleWho, &self->damageWho,
                            &self->crimeWho, &self->findWho})
        GuestList_Clear(list);
    for (GuestVector& v : self->programs)           // 0x1C6790
        if (v.data != 0)
            MemFree(P(v.data));
    if (flag > 0)
        MemFree(self);
    return self;
}

// ── 0x1C1160 ──────────────────────────────────────────────────────────────
// Entering or leaving battle forgets the watches.
void Task_SetBattleMode(Task* self, int mode) {
    self->battleMode = mode;
    for (TaskWatch& w : self->watch)
        ClearWatch(&w);
    for (TaskCheck& c : self->check)
        ClearCheck(&c);
    self->watch48 = {0, 0, 0};
}

// ── 0x1C0F20 ──────────────────────────────────────────────────────────────
void Task_SetDamageWho(Task* self, const TaskCommand* cmd) {
    GuestList_Clear(&self->damageWho);
    for (int k = 0; k < 5; k++)
        if (cmd->arg[k] != -1)
            Append(&self->damageWho, (u32)cmd->arg[k]);
}

// ══ State of the characters around ════════════════════════════════════════

// ── 0x1C0FD0 ──────────────────────────────────────────────────────────────
int Task_CharaBusy(Task*, Chara* chara, void* talk, GuestIntMap* tasks, int with) {
    chara->_68 = 0;
    const int id = chara->id;
    if (const Task* task = TaskOf(tasks, id))
        if (task->level > 0)
            chara->_68 = 1;
    if (Effects_IsBusy(talk, id) != 0 && Effects_PartnerOf(talk, id) != with)
        chara->_68 = 1;
    if ((Flags(chara) & 0x100) != 0 && Params(chara)->link[3] != 0)
        chara->_68 = 1;
    return chara->_68;
}

// ── 0x1C10B0 ──────────────────────────────────────────────────────────────
// SetWeaponOff: the sword goes back once the body allows it.
void Task_StepWeaponOff(Task* self, Chara* chara) {
    if (self->weaponOff == 0)
        return;
    CharaBody* body = P<CharaBody>(chara->body);
    if ((body->flags & 0x100) == 0) {
        self->weaponOff = 0;
    } else if ((body->actionFlags & 0x1000) == 0) {
        if (self->weaponOff == 1) {
            CharaBody_Refresh(body);
            self->weaponOff = 2;
        }
        if (self->weaponOff == 2) {
            CharaBody_fn1d1c90(body);
            self->weaponOff = 2;
        }
    }
}

// Does a watch list entry describe this character?
static bool Matches(s32 selector, const CharaParams* p) {
    if (selector == kMatchAnyone)
        return true;
    if (selector == kMatchPlayer)
        return p->id == 0;
    if (selector < 3 || selector > 5)
        return p->role == selector - kMatchRole;
    return p->faction == selector - kMatchFaction;
}

// The scans below share a shape: among the characters a selector describes
// (everyone, the player, a faction or a role), the nearest one that is most
// squarely in front and passes `test`, which also reports the distance and
// the facing.  The player, asked for by name, only has to pass the test.
template <typename Test>
static Chara* Nearest(CharaMgr* charas, s32 selector, float bestFacing, Test test) {
    float d, f;
    if (selector == kMatchPlayer) {
        Chara* c = CharaMgr_Find(charas, 0);
        return c != nullptr && test(c, true, d, f) ? c : nullptr;
    }
    Chara* best = nullptr;
    float bestDistance = 1000.0f;
    for (GuestListNode* n = GuestList_Begin(&charas->charas); n != GuestList_End(&charas->charas);
         n = GuestList_Next(n)) {
        Chara* c = P<Chara>(n->value);
        if (selector != kMatchAnyone) {
            const CharaParams* p = Params(c);
            if (selector < 3 || selector > 5 ? p->role != selector - kMatchRole : p->faction != selector - kMatchFaction)
                continue;
        }
        if (test(c, false, d, f) && d < bestDistance && bestFacing < f) {
            best = c;
            bestDistance = d;
            bestFacing = f;
        }
    }
    return best;
}

// ── 0x1C1240 ──────────────────────────────────────────────────────────────
// FindTarget (watches 0, 1) and LostTarget (2, 3).
int Task_CheckWatches(Task* self, GuestIntMap* tasks, CharaMgr* charas, void* talk, Chara* chara) {
    int fired = 0;
    for (int i = 0; i < 4; i++) {
        TaskWatch* w = &self->watch[i];
        if (w->on == 0)
            continue;
        for (GuestListNode* e = GuestList_Begin(&w->who); e != GuestList_End(&w->who); e = GuestList_Next(e)) {
            const s32 selector = (s32)e->value;
            Chara* found = nullptr;
            if (i < 2) {
                auto inView = [&](Chara* c, bool player, float& d, float& f) {
                    if ((Flags(c) & 0x200) != 0 || (!player && c->active == 0))
                        return false;
                    d = Vec_Distance(Position(chara), Position(c));
                    if (!(d < w->distance))
                        return false;
                    f = Chara_FacingTowards(Params(chara)->matrix, Position(c));
                    return w->facing < f;
                };
                if (selector == kMatchTarget) {
                    float d, f;
                    Chara* c = CharaMgr_Find(charas, self->target[self->level]);
                    if (c != nullptr && inView(c, false, d, f))
                        found = c;
                } else {
                    found = Nearest(charas, selector, -1.0f, inView);
                }
            } else if (selector == kMatchTarget || selector == kMatchPlayer) {
                Chara* c = CharaMgr_Find(charas, selector == kMatchTarget ? self->target[self->level] : 0);
                if (c == nullptr)
                    continue;       // (the original assumes it exists)
                bool lost = (Flags(c) & 0x200) != 0;
                if (Task_CharaBusy(self, c, talk, tasks, chara->id) != 0
                    && (self->level == kTaskFindTarget || self->level == kTaskPartner))
                    lost = true;
                if (!lost) {
                    if (Collision_CanSee(chara->id, c->id, 2) == 0) {
                        lost = true;
                    } else {
                        if (w->distance < Vec_Distance(Position(chara), Position(c)))
                            lost = true;
                        if (Chara_FacingTowards(Params(chara)->matrix, Position(c)) < w->facing)
                            lost = true;
                    }
                }
                if (lost)
                    found = c;
            }
            if (found != nullptr) {
                // ("Get's!!Target" / "Lost!!Target")
                fired = 1;
                Task_Goto(self, w->line);
                w->on = 0;
                self->other = found->id;
            }
        }
    }
    return fired;
}

// ── 0x1C1BA0 ──────────────────────────────────────────────────────────────
int Task_CheckPriorityTarget(Task* self, GuestIntMap* tasks, CharaMgr*) {
    TaskCheck* c = &self->check[1];
    if (c->on != 0)
        for (GuestListNode* e = GuestList_Begin(&c->who); e != GuestList_End(&c->who); e = GuestList_Next(e)) {
            const Task* other = TaskOf(tasks, (int)e->value);
            if (other != nullptr && c->level != other->level) {
                Task_Goto(self, c->line);
                c->on = 0;
                c->level = -1;
            }
        }
    return 0;
}

// ── 0x1C1CE0 ──────────────────────────────────────────────────────────────
int Task_CheckCollisionTarget(Task* self, CharaMgr* charas) {
    TaskCheck* c = &self->check[0];
    int fired = 0;
    if (c->on != 0)
        for (GuestListNode* e = GuestList_Begin(&c->who); e != GuestList_End(&c->who); e = GuestList_Next(e))
            if (CharaMgr_GetFlags(charas, (int)e->value) & 0x20000) {
                fired = 1;
                Task_Goto(self, c->line);
                c->on = 0;
            }
    return fired;
}

// ── 0x1C1E10 ──────────────────────────────────────────────────────────────
int Task_CheckTargetLife(Task* self, int, CharaMgr* charas) {
    int fired = 0;
    if (self->checkLife.on != 0)
        for (GuestListNode* e = GuestList_Begin(&self->checkLife.who); e != GuestList_End(&self->checkLife.who);
             e = GuestList_Next(e)) {
            const CharaParams* p = CharaMgr_GetParams(charas, (int)e->value);
            if (p != nullptr && p->hp <= self->checkLife.life) {
                fired = 1;
                Task_Goto(self, self->checkLife.line);
                self->checkLife.on = 0;
            }
        }
    return fired;
}

// ── 0x1C1F40 ──────────────────────────────────────────────────────────────
int Task_CheckCatchTarget(Task* self, int, CharaMgr* charas) {
    if (self->checkCatch.on == 0)
        return 0;
    const CharaParams* p = CharaMgr_GetParams(charas, self->checkCatch.who);
    if (p == nullptr || p->hp > self->checkCatch.life || (CharaMgr_GetFlags(charas, self->checkCatch.who) & 6) == 0)
        return 0;
    Task_Goto(self, self->checkCatch.line);
    self->checkCatch.on = 0;
    return 1;
}

// ── 0x1C2060 ──────────────────────────────────────────────────────────────
int Task_CheckTargetChange(Task* self, CharaMgr* charas) {
    if (self->checkChange.on == 0)
        return 0;
    const CharaParams* p = CharaMgr_GetParams(charas, self->checkChange.who);
    const s32* opponent = p != nullptr ? P<s32>(p->link[3]) : nullptr;
    if (opponent == nullptr || self->checkChange.opponent == *opponent)
        return 0;
    Task_Goto(self, self->checkChange.line);
    self->checkChange.on = 0;
    return 1;
}

// ══ The events that raise the level ═══════════════════════════════════════

// ── 0x1C2140 ──────────────────────────────────────────────────────────────
int Task_CheckDead(Task* self, CharaMgr*, Chara* chara) {
    if (self->programs[kTaskDead].count == 0 || (Flags(chara) & 0x200) == 0)
        return 0;
    const int killer = Collision_AttackerOf(chara->id);
    if (self->level >= kTaskDead)
        return 0;
    self->target[kTaskDead] = killer;
    self->level = kTaskDead;
    return 1;
}

static int RaiseForAttacker(Task* self, CharaMgr* charas, Chara* chara, int level, GuestList* who) {
    const int attacker = Collision_AttackerOf(chara->id);
    const CharaParams* p = CharaMgr_GetParams(charas, attacker);
    if (p == nullptr)
        return 0;           // (the original assumes there is one)
    for (GuestListNode* e = GuestList_Begin(who); e != GuestList_End(who); e = GuestList_Next(e)) {
        if (!Matches((s32)e->value, p))
            continue;
        if (self->level >= level)
            return 0;
        self->target[level] = attacker;
        self->level = level;
        return 1;
    }
    return 0;
}

// ── 0x1C21C0 ──────────────────────────────────────────────────────────────
int Task_CheckDamage(Task* self, CharaMgr* charas, Chara* chara) {
    if (self->programs[kTaskDamage].count == 0 || self->battleMode != 0 || (Flags(chara) & 0x10000) == 0)
        return 0;
    return RaiseForAttacker(self, charas, chara, kTaskDamage, &self->damageWho);
}

// ── 0x1C3970 ──────────────────────────────────────────────────────────────
int Task_CheckCrash(Task* self, CharaMgr* charas, Chara* chara) {
    if (self->programs[kTaskCrash].count == 0 || self->battleMode != 0 || (Flags(chara) & 0x20000) == 0)
        return 0;
    return RaiseForAttacker(self, charas, chara, kTaskCrash, &self->crashWho);
}

// Somebody in `who`, near, in front and in sight, that passes `extra`.
template <typename Extra>
static int RaiseForNearby(Task* self, CharaMgr* charas, Chara* chara, int level, GuestList* who,
                          const float* range, Extra extra) {
    for (GuestListNode* e = GuestList_Begin(who); e != GuestList_End(who); e = GuestList_Next(e)) {
        Chara* found = Nearest(charas, (s32)e->value, -1.0f, [&](Chara* c, bool player, float& d, float& f) {
            if (!player && (c == chara || (Flags(c) & 0x200) != 0))
                return false;
            if (!extra(c))
                return false;
            d = Vec_Distance(Position(chara), Position(c));
            if (!(d < range[0]))
                return false;
            f = Chara_FacingTowards(Params(chara)->matrix, Position(c));
            return range[1] < f && Collision_CanSee(chara->id, c->id, 2) != 0;
        });
        if (found != nullptr && self->level < level) {
            self->target[level] = found->id;
            self->level = level;
            return 1;
        }
    }
    return 0;
}

// ── 0x1C2320 ──────────────────────────────────────────────────────────────
int Task_CheckCrime(Task* self, CharaMgr* charas, Chara* chara) {
    if (self->programs[kTaskCrime].count == 0 || self->battleMode != 0)
        return 0;
    return RaiseForNearby(self, charas, chara, kTaskCrime, &self->crimeWho, self->crimeRange,
                          [](Chara* c) { return c->_6c != 0; });
}

// ── 0x1C27C0 ──────────────────────────────────────────────────────────────
int Task_CheckWeapon(Task* self, CharaMgr* charas, Chara* chara) {
    if (self->programs[kTaskWeapon].count == 0 || self->battleMode != 0)
        return 0;
    return RaiseForNearby(self, charas, chara, kTaskWeapon, &self->weaponWho, self->weaponRange,
                          [](Chara* c) { return (Flags(c) & 0x100) != 0; });
}

// ── 0x1C2CE0 ──────────────────────────────────────────────────────────────
int Task_CheckBattle(Task* self, CharaMgr* charas, Chara* chara) {
    if (self->programs[kTaskBattle].count == 0 || self->battleMode != 0)
        return 0;
    return RaiseForNearby(self, charas, chara, kTaskBattle, &self->battleWho, self->battleRange,
                          [](Chara* c) { return Params(c)->link[3] != 0 && (Flags(c) & 0x100) != 0; });
}

// ── 0x1C2C70 ──────────────────────────────────────────────────────────────
int Task_CheckPartner(Task* self, CharaMgr*, Chara* chara) {
    if (self->programs[kTaskPartner].count == 0 || (Flags(chara) & 0x200) != 0)
        return 0;
    if (chara->_70 == 0 || self->level == kTaskPartner)
        return 0;
    self->target[kTaskPartner] = chara->partner;
    self->level = kTaskPartner;
    return 1;
}

// ── 0x1C31B0 ──────────────────────────────────────────────────────────────
int Task_CheckFindTarget(Task* self, GuestIntMap* tasks, CharaMgr* charas, void* talk, Chara* chara) {
    if (self->programs[kTaskFindTarget].count == 0)
        return 0;
    for (GuestListNode* e = GuestList_Begin(&self->findWho); e != GuestList_End(&self->findWho);
         e = GuestList_Next(e)) {
        Chara* found = Nearest(charas, (s32)e->value, 0.0f, [&](Chara* c, bool player, float& d, float& f) {
            if (!player && c == chara)
                return false;
            if (Task_CharaBusy(self, c, talk, tasks, chara->id) != 0)
                return false;
            if (!player && (Flags(c) & 0x200) != 0)
                return false;
            d = Vec_Distance(Position(chara), Position(c));
            if (!(self->findRange[0] < d && d < self->findRange[1]))
                return false;
            f = Chara_FacingTowards(Params(chara)->matrix, Position(c));
            return self->findRange[2] < f && Collision_CanSee(chara->id, c->id, 2) != 0;
        });
        if (found != nullptr && self->level < kTaskFindTarget) {
            self->target[kTaskFindTarget] = found->id;
            self->level = kTaskFindTarget;
            return 1;
        }
    }
    return 0;
}

// ══ The interpreter ═══════════════════════════════════════════════════════

// "Random" arguments: mode 0x12 = below `value`; 0x5C = the same but not the
// number drawn last time.
static int RandomArg(Task* self, int value, int mode) {
    if (mode == 0x12)
        return (int)Mt_Below((u32)value);
    if (mode == 0x5C) {
        int r = (int)Mt_Below((u32)value);
        if (r == self->lastRandom && --r == 0)
            r = value - 1;
        self->lastRandom = r;
        return r;
    }
    return value;
}

// The player-data values tasks read and write by number (ops 6, 0x18, 0x19).
static int GetGlobal(const u8* pd, int which) {
    auto i32 = [pd](int o) { return *reinterpret_cast<const s32*>(pd + o); };
    auto i16 = [pd](int o) { return (int)*reinterpret_cast<const s16*>(pd + o); };
    switch (which) {
    case 0: return i32(0x124);
    case 1: case 2: case 3: return i32(0x144 + (which - 1) * 4);
    case 4: case 5: case 6: return i32(0x12C + (which - 4) * 4);
    case 7: return i16(0x106);
    case 8: return i16(0x108);
    case 9: return i32(0x128);
    }
    return 0;
}
static void SetGlobal(u8* pd, int which, int value, bool assign) {
    auto i32 = [pd](int o) -> s32& { return *reinterpret_cast<s32*>(pd + o); };
    auto i16 = [pd](int o) -> s16& { return *reinterpret_cast<s16*>(pd + o); };
    auto u16at = [pd](int o) -> u16& { return *reinterpret_cast<u16*>(pd + o); };
    const int percent = value < 1 ? 0 : value > 100 ? 100 : value;
    switch (which) {
    case 0: i32(0x124) = value; break;
    case 1: case 2: case 3: i32(0x144 + ClampIndex(which - 1, 3) * 4) = percent; break;    // faction strengths
    case 4: case 5: case 6: i32(0x12C + ClampIndex(which - 4, 3) * 4) = percent; break;
    case 7: {
        // Money (0–9999), with running totals of what was gained and lost.
        const u32 v = (u32)value & 0xFFFF;
        const s16 before = i16(0x106);
        i16(0x106) = v == 0 ? 0 : v > 9999 ? 9999 : (s16)value;
        const int change = i16(0x106) - before;
        u16& total = change < 1 ? u16at(0x84E) : u16at(0x84C);
        const int sum = total + abs(change);
        total = (u16)(sum > 0xFFFF ? 0xFFFF : sum);
        break;
    }
    case 8:
        if (assign) {
            i32(0x124) = value;         // (so the original: SetGlobal 8 writes value 0's slot)
        } else {
            const int v = value < 1 ? 0 : value;
            i16(0x108) = (s16)(i16(0x10A) <= v ? i16(0x10A) : v);
        }
        break;
    case 9: i32(0x128) = value; break;
    }
}

// A character's values (ops 0x1A, 0x1C, 0x1D): 0 health, 1 CharaParams+0x18,
// 2 Chara+0x6C (wanted for a crime), 3–6 CharaParams::stat.
static int GetCharaValue(CharaMgr* charas, const CharaParams* p, int id, int which) {
    switch (which) {
    case 0: return p->hp;
    case 1: return p->_18;
    case 2: return CharaMgr_Get6C(charas, id);
    case 3: case 4: case 5: case 6: return p->stat[which - 3];
    }
    return 0;
}
static void SetCharaValue(CharaMgr* charas, CharaParams* p, int id, int which, int value) {
    switch (which) {
    case 0: p->hp = p->maxHp <= (s16)value ? p->maxHp : (s16)value; break;
    case 1: p->_18 = value; break;
    case 2: CharaMgr_Set6C(charas, id, value); break;
    case 3: case 4: case 5: case 6: p->stat[which - 3] = value; break;
    }
}

// ── 0x1C3AC0 ──────────────────────────────────────────────────────────────
TaskCommand* Task_Step(Task* self, int id, GuestIntMap* tasks, CharaMgr* charas) {
    PlayerData* player = *gPlayerData;
    u8* pd = reinterpret_cast<u8*>(player);
    const int level = self->level;
    const u32 line = (u32)self->line[level];
    if (self->programs[level].count <= line)
        return nullptr;
    TaskCommand* cmd = CommandAt(self, level, line);

    // Who "target" means on this line: its opponent, if it has one.
    const CharaParams* own = CharaMgr_GetParams(charas, id);
    if (own != nullptr && own->link[3] != 0)
        cmd->target = *P<s32>(own->link[3]);
    else if (own != nullptr && self->weaponOff == 0 && (CharaMgr_GetFlags(charas, id) & 0x100) != 0)
        cmd->target = -1;
    else
        cmd->target = self->target[level];
    cmd->other = self->other == -1 ? cmd->target : self->other;

    const int alreadyStarted = self->started[level];
    if (alreadyStarted == 0) {
        const int who1 = ResolveWho(cmd->who1, id, cmd, charas);
        const int who2 = ResolveWho(cmd->who2, id, cmd, charas);
        const s32* arg = cmd->arg;
        self->started[level] = 1;
        switch (cmd->op) {
        case kOpIf: {
            s64 value = arg[0];
            switch (arg[0]) {
            case 0x59: value = *reinterpret_cast<s16*>(pd + 0x116); break;
            case 0x54: value = *reinterpret_cast<s16*>(pd + 0x6C84); break;
            case 0x29: value = self->active; break;
            case 0x1B:
                if (who1 == -1) {
                    value = 0;                  // ("BaseChara Error")
                } else if (const CharaParams* p = CharaMgr_GetParams(charas, who1)) {
                    switch (arg[1]) {
                    case 0: value = p->hp; break;
                    case 1: value = p->_18; break;
                    case 2: value = CharaMgr_GetFlags(charas, who1) & 0x100; break;
                    case 3: value = CharaMgr_Get6C(charas, who1); break;
                    case 4: value = p->faction; break;
                    case 5: case 6: case 7: case 8: value = p->stat[arg[1] - 5]; break;
                    case 9: {
                        const Task* other = TaskOf(tasks, who1);
                        value = other != nullptr ? other->level : 0;
                        break;
                    }
                    }
                }
                break;
            case 0x3D: value = (s8)pd[0x740 + arg[1]]; break;
            case 0x41: value = *reinterpret_cast<s16*>(pd + 0x11E); break;
            case 0x45: value = *reinterpret_cast<s16*>(pd + 0x11C); break;
            case 0x07:
                switch (arg[1]) {
                case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
                    value = GetGlobal(pd, arg[1]);
                    break;
                case 9:  value = player->area; break;
                case 10: value = player->time; break;
                case 11: value = player->day; break;
                case 12: case 13: case 14: value = *reinterpret_cast<s32*>(pd + 0x138 + (arg[1] - 12) * 4); break;
                case 15: value = DayMgr_PeriodOver(DayMgr_Get()); break;
                }
                break;
            case 0x0F: value = self->local[arg[1]]; break;
            case 0x37: value = (s8)pd[0x6C0 + arg[1]]; break;
            }
            const s64 against = arg[3];
            bool holds = false;
            switch (arg[2]) {
            case 3: holds = value != against; break;
            case 2: holds = value == against; break;
            case 1: holds = value < against; break;
            case 0: holds = against < value; break;
            }
            if (holds)
                self->line[level] = arg[4];
            else
                self->line[level]++;
            self->started[level] = 0;
            self->frames[level] = 0;
            break;
        }
        case kOpCheckPriorityTarget:
        case kOpCheckCollisionTarget: {
            TaskCheck* c = &self->check[cmd->op == kOpCheckCollisionTarget ? 0 : 1];
            ClearCheck(c);
            if (arg[0] != -1 && who1 != -1) {
                c->on = 1;
                c->line = arg[0];
                Append(&c->who, (u32)who1);
                if (cmd->op == kOpCheckPriorityTarget)
                    if (const Task* other = TaskOf(tasks, who1))
                        c->level = other->level;
            }
            break;
        }
        case kOpWatch48:
            self->watch48 = {0, 0, 0};
            if (arg[0] != -1 && who1 != -1)
                self->watch48 = {1, arg[0], who1};
            break;
        case kOpCheckTargetChange:
            self->checkChange = {0, 0, 0, 0};
            if (arg[0] != -1 && who1 != -1 && who2 != -1)
                self->checkChange = {1, arg[0], who1, who2};
            break;
        case kOpCheckTargetLife:
            self->checkLife.on = self->checkLife.line = self->checkLife.life = 0;
            GuestList_Clear(&self->checkLife.who);
            if (arg[0] != -1 && who2 != -1) {
                self->checkLife.on = 1;
                self->checkLife.life = arg[0];
                self->checkLife.line = arg[1];
                Append(&self->checkLife.who, (u32)who2);
            }
            break;
        case kOpCheckCatchTarget:
            self->checkCatch = {0, 0, 0, -1};
            if (arg[0] != -1 && who2 != -1)
                self->checkCatch = {1, arg[1], arg[0], who2};
            break;
        case kOpFindTargetA:
            self->other = -1;
            [[fallthrough]];
        case kOpFindTargetB:
        case kOpLostTargetA:
        case kOpLostTargetB: {
            TaskWatch* w = &self->watch[cmd->op - kOpFindTargetA];
            ClearWatch(w);
            if (arg[3] != -1) {
                w->on = 1;
                w->line = arg[0];
                w->distance = (float)arg[1] / 1000.0f;
                w->facing = (float)arg[2] / 1000.0f;
                Append(&w->who, (u32)arg[3]);
            }
            break;
        }
        case kOpAddChara:
        case kOpSubChara:
        case kOpSetChara:
            if (who1 == -1)
                break;                          // ("BaseChara Error")
            if (CharaParams* p = CharaMgr_GetParams(charas, who1)) {
                int value;
                if (cmd->op == kOpSetChara)
                    value = RandomArg(self, arg[1], arg[2]);
                else if (cmd->op == kOpAddChara)
                    value = GetCharaValue(charas, p, who1, arg[0]) + arg[1];
                else
                    value = GetCharaValue(charas, p, who1, arg[0]) - arg[1];
                SetCharaValue(charas, p, who1, arg[0], value);
            }
            break;
        case kOpAddGlobal: SetGlobal(pd, arg[0], GetGlobal(pd, arg[0]) + arg[1], false); break;
        case kOpSubGlobal: SetGlobal(pd, arg[0], GetGlobal(pd, arg[0]) - arg[1], false); break;
        case kOpSetGlobal: SetGlobal(pd, arg[0], RandomArg(self, arg[1], arg[2]), true); break;
        case kOpCrimeWho:
        case kOpBattleWho:
        case kOpCrashWho:
        case kOpWeaponWho:
        case kOpFindWho: {
            GuestList* list = cmd->op == kOpCrimeWho ? &self->crimeWho : cmd->op == kOpBattleWho ? &self->battleWho
                            : cmd->op == kOpCrashWho ? &self->crashWho : cmd->op == kOpWeaponWho ? &self->weaponWho
                            : &self->findWho;
            GuestList_Clear(list);
            for (int k = 0; k < 5; k++)
                if (arg[k] != -1)
                    Append(list, (u32)arg[k]);
            break;
        }
        case kOpCrimeRange:
        case kOpBattleRange:
        case kOpWeaponRange: {
            float* range = cmd->op == kOpCrimeRange ? self->crimeRange : cmd->op == kOpBattleRange ? self->battleRange
                         : self->weaponRange;
            range[0] = (float)arg[0] / 1000.0f;
            range[1] = (float)arg[1] / 1000.0f;
            break;
        }
        case kOpRange16:
        case kOpFindRange: {
            float* range = cmd->op == kOpRange16 ? self->range16 : self->findRange;
            for (int k = 0; k < 3; k++)
                range[k] = (float)arg[k] / 1000.0f;
            break;
        }
        case kOpSetFlag:
            pd[0x740 + ClampIndex(arg[0], 0x100)] = (u8)RandomArg(self, arg[1], arg[2]);
            PlayerData_ClearFinishedTask(player);
            break;
        case kOpSubFlag:
        case kOpAddFlag: {
            const s8 current = (s8)pd[0x740 + arg[0]];
            pd[0x740 + ClampIndex(arg[0], 0x100)] = (u8)(cmd->op == kOpAddFlag ? current + arg[1] : current - arg[1]);
            PlayerData_ClearFinishedTask(player);
            break;
        }
        case kOpSetTaskB: *reinterpret_cast<s16*>(pd + 0x11C) = (s16)arg[0]; break;
        case kOpSetTaskA: *reinterpret_cast<s16*>(pd + 0x11E) = (s16)arg[0]; break;
        case kOpSetLocal: self->local[arg[0]] = RandomArg(self, arg[1], arg[2]); break;
        case kOpSubLocal: self->local[arg[0]] -= arg[1]; break;
        case kOpAddLocal: self->local[arg[0]] += arg[1]; break;
        case kOpAddCounter: *reinterpret_cast<s16*>(pd + 0x6C84) += (s16)arg[0]; break;
        case kOpSetCounter: *reinterpret_cast<s16*>(pd + 0x6C84) = (s16)arg[0]; break;
        case kOpSubDayFlag:
        case kOpAddDayFlag: {
            const s8 current = (s8)pd[0x6C0 + arg[0]];
            pd[0x6C0 + ClampIndex(arg[0], 0x80)] = (u8)(cmd->op == kOpAddDayFlag ? current + arg[1] : current - arg[1]);
            break;
        }
        case kOpSetDayFlag:
            pd[0x6C0 + ClampIndex(arg[0], 0x80)] = (u8)RandomArg(self, arg[1], arg[2]);
            break;
        case kOpIfTarget:                       // is the target the player?
            self->started[level] = 0;
            self->frames[level] = 0;
            self->line[level] = cmd->target == 0 ? arg[0] : arg[1];
            break;
        case kOpJump:
            self->started[level] = 0;
            self->line[level] = arg[0];
            self->frames[level] = 0;
            break;
        case kOpSetAI:
            if (arg[0] == 1)
                self->weaponOff = 0;
            break;
        default:
            break;
        }
        if (cmd->wait == -1)
            self->waiting[level] = 1;
    }

    // Move on when the line's time is up (unless it waits to be told).
    if (self->waiting[level] == 0) {
        const int frames = self->frames[level]++;
        if (cmd->wait < frames) {
            self->prevLine[level] = self->line[level];
            self->line[level]++;
            self->frames[level] = 0;
            self->started[level] = 0;
        }
    }
    return alreadyStarted != 0 ? nullptr : cmd;
}
