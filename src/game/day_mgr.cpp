// The calendar — decompiled from 0x232040–0x2332E0.  See include/game/day_mgr.h.

#include "game/day_mgr.h"
#include "game/chara_resources.h"
#include "game/player_data.h"
#include "native/guest.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" {
    void* MemAlloc(u32 size);                               // 0x100820
    void  MemFree(void* p);                                 // 0x100650
    void  Matrix_Copy(void* dst, const void* src);          // 0x1073C0
}

static u32& gDayMgr = *guest::ptr<u32>(0x2EE74Cu);
static const GuestPtrVar<PlayerData> gPlayerData{0x2EE688u};
// Which set of maps each period uses: 0 day, 1 night.
static const s32* const kPeriodNight = guest::ptr<const s32>(0x2DA160u);
enum { kPeriods = 5, kLastDay = 9, kPeriodLength = 900 };

// The player's live parameter block.
struct PlayerParams {
    u8* p;
    PlayerParams() : p(reinterpret_cast<u8*>(CharaResources_GetParams(CharaResources_Get(), 0))) {}
    s16& health()    { return *reinterpret_cast<s16*>(p + 0x40); }
    s16& healthMax() { return *reinterpret_cast<s16*>(p + 0x42); }
    bool alive()     { return *guest::ptr<s32>(*reinterpret_cast<u32*>(p + 0xA0)) != 0; }
    // Time spent in the world since it was last banked.
    s32* clock()     { return guest::ptr<s32>(*reinterpret_cast<u32*>(p + 0xD0)); }
    s32  time()      { return clock() ? *clock() : 0; }
    void resetTime() { if (clock()) *clock() = 0; }
};

static void SetHealth(PlayerData* pd, s16 value) {
    const s16 before = pd->health;
    pd->health = value;
    s16& lost = *reinterpret_cast<s16*>(reinterpret_cast<u8*>(pd) + 0x854);
    if (pd->health - before < 0)
        lost = (s16)(lost + abs(pd->health - before));
}
static void SetVigour(PlayerData* pd, int value) {
    if (value < 1)
        value = 0;
    if (pd->_10a <= value)
        value = pd->_10a;
    pd->_108 = (s16)value;
}
static s16& DaysPassed(PlayerData* pd)  { return *reinterpret_cast<s16*>(reinterpret_cast<u8*>(pd) + 0x864); }
static s16& TimePassed(PlayerData* pd)  { return *reinterpret_cast<s16*>(reinterpret_cast<u8*>(pd) + 0x850); }
static s16& TaskKind(PlayerData* pd)    { return *reinterpret_cast<s16*>(reinterpret_cast<u8*>(pd) + 0x11E); }
static s16& TaskActive(PlayerData* pd)  { return *reinterpret_cast<s16*>(reinterpret_cast<u8*>(pd) + 0x11C); }

// Stores a day and period that may have run past the end of either.
static void SetDate(PlayerData* pd, PlayerParams& params, int day, int period) {
    pd->_6e80[0] = 0;
    pd->time = (s8)(period > 5 ? 4 : period);
    pd->day = (s8)(day > 10 ? 9 : day);
    pd->night = (s8)kPeriodNight[period];
    pd->periodTime = 0;
    params.resetTime();
}

// ── 0x2332E0 ──────────────────────────────────────────────────────────────
DayMgr* DayMgr_Get() {
    if (gDayMgr == 0) {
        DayMgr* mgr = static_cast<DayMgr*>(MemAlloc(sizeof(DayMgr)));
        gDayMgr = guest::addr(mgr);
        if (mgr)
            memset(mgr, 0, sizeof(*mgr));
    }
    return guest::ptr<DayMgr>(gDayMgr);
}

// ── 0x2332A0 ──────────────────────────────────────────────────────────────
void DayMgr_Release() {
    if (gDayMgr != 0)
        MemFree(guest::ptr(gDayMgr));
    gDayMgr = 0;
}

// ── 0x232040 ──────────────────────────────────────────────────────────────
// Each of the four story lines can have a deadline (day, period, outcome).
// Once it passes, the line is over (99) with that outcome.
void DayMgr_CheckDeadlines(DayMgr*) {
    PlayerData* pd = *gPlayerData;
    u8* b = reinterpret_cast<u8*>(pd);
    const int day = pd->day + 1, period = pd->time + 1;
    for (int line = 0; line < 4; line++) {
        const int deadline = 0xDB + line * 6;           // flags: day, period, outcome, result
        const s8 dueDay = (s8)b[0x740 + deadline], duePeriod = (s8)b[0x740 + deadline + 1];
        const u8 outcome = b[0x740 + deadline + 2];
        if ((s8)b[0x740 + 0xBC + line] >= 99 || outcome == 0)
            continue;
        if (dueDay < day || (day == dueDay && duePeriod <= period)) {
            *PlayerData_Flag(pd, 0xBC + line) = 99;
            *PlayerData_Flag(pd, deadline + 3) = outcome;
            *PlayerData_Flag(pd, deadline) = 0;
            *PlayerData_Flag(pd, deadline + 1) = 0;
            *PlayerData_Flag(pd, deadline + 2) = 0;
        }
    }
    PlayerData_ClearFinishedTask(pd);
}

// ── 0x2328F0 ──────────────────────────────────────────────────────────────
void DayMgr_Tick(DayMgr*) {
    PlayerData* pd = *gPlayerData;
    TaskActive(pd) = 0;
    TaskKind(pd) = 0;
    for (int flag : {0x2E, 0x32, 0x33, 0x34, 0x35, 0x2A, 0x2B, 0x2C, 0x22, 0x23, 0x24, 0x5C, 0x21})
        *PlayerData_DayFlag(pd, flag) = 0;
    PlayerData_DropQuestItems(pd);
}

// ── 0x232390 ──────────────────────────────────────────────────────────────
u32 DayMgr_GetChoiceFlags(DayMgr* self, int periods) {
    PlayerData* pd = *gPlayerData;
    PlayerParams params;
    int vigour = pd->_108, health = pd->health, period = pd->time, day = pd->day;
    u32 result = kDayChanged;
    if (periods >= 1 && periods <= 5) {
        vigour += periods * 2;
        health += periods * 200;
        period += periods;
    }
    if (period > 4) {
        if (day == kLastDay)
            *PlayerData_Flag(pd, 0xFF) = 1;
        period -= kPeriods;
        if (period < 1)
            period = 0;
        day = pd->day + 1;
        result = kDayChanged | kDayNewDay;
        if (day == 5)
            period = 4;
        if (day > kLastDay)
            day = kLastDay;
    }
    if (pd->_10a <= vigour)
        vigour = pd->_10a;
    if (pd->healthMax <= health)
        health = pd->healthMax;
    self->countdown = 0;
    params.health() = (s16)health < params.healthMax() ? (s16)health : params.healthMax();
    SetHealth(pd, (s16)(pd->healthMax <= health ? pd->healthMax : health));
    SetVigour(pd, vigour);
    SetDate(pd, params, day, period);
    DayMgr_Tick(self);
    DaysPassed(pd)++;
    TimePassed(pd) = (s16)(periods * 5 + TimePassed(pd));
    DayMgr_CheckDeadlines(self);
    return result;
}

// ── 0x2326C0 ──────────────────────────────────────────────────────────────
void DayMgr_SetEvent(DayMgr* self, int inEvent) {
    if (self->inEvent == inEvent)
        return;
    self->inEvent = inEvent;
    PlayerData* pd = *gPlayerData;
    PlayerParams params;
    if (inEvent != 0)
        pd->periodTime = (s16)(pd->periodTime + (s16)params.time());
    params.resetTime();
}

// ── 0x232670 ──────────────────────────────────────────────────────────────
// A job ends the period (task command SetWorkOut).
void DayMgr_EndPeriodNow(DayMgr*) {
    PlayerParams params;
    (*gPlayerData)->periodTime = 900;
    params.resetTime();
}

// ── 0x232C30 ──────────────────────────────────────────────────────────────
// Has the period run out?  (What task scripts test as global 15.)
int DayMgr_PeriodOver(DayMgr*) {
    PlayerParams params;
    return 899 < (*gPlayerData)->periodTime + params.time();
}

// ── 0x2331B0 ──────────────────────────────────────────────────────────────
// A job was taken (task command SetWorkName): if too little of the period is
// left for it — 30 % (45 % on Easy) — the timed-task countdown starts.
void DayMgr_StartJob(DayMgr* self) {
    PlayerData* pd = *gPlayerData;
    if (self->countdown != 0)
        return;
    PlayerParams params;
    const float needed = pd->difficulty == 0 ? 0.45f : 0.3f;
    if ((1.0f - needed) * 900.0f <= (float)(pd->periodTime + params.time())) {
        self->countdown = 1;
        self->elapsed = 0;
        self->periodTime = pd->periodTime + params.time();
    }
}

// ── 0x232750 ──────────────────────────────────────────────────────────────
void DayMgr_Save(DayMgr* self) {
    self->inEvent = 0;
    PlayerData* pd = *gPlayerData;
    u8* b = reinterpret_cast<u8*>(pd);
    PlayerParams params;
    if (self->collapsed != 0) {
        SetVigour(pd, pd->_10a < 1 ? 0 : pd->_10a);
        self->collapsed = 0;
    }
    if (params.alive()) {
        pd->periodTime = (s16)(pd->periodTime + (s16)params.time());
        pd->healthMax = params.healthMax() > 1999 ? 2000 : params.healthMax();
        SetHealth(pd, pd->healthMax <= params.health() ? pd->healthMax : params.health());
        params.resetTime();
    }
    if (TaskKind(pd) == 4) {
        self->countdown = 0;
        self->elapsed = 0;
        TaskActive(pd) = 0;
        TaskKind(pd) = 0;
        PlayerData_DropQuestItems(pd);
    }
    const int done = (s8)b[0x6F0], total = (s8)b[0x6EF];
    *reinterpret_cast<s16*>(b + 0x848) = (s16)(done < 1 ? 0 : done);
    *reinterpret_cast<s16*>(b + 0x84A) = (s16)(total - done < 1 ? 0 : total - done);
    pd->night = (s8)kPeriodNight[pd->time];
}

// ── 0x232A60 ──────────────────────────────────────────────────────────────
void DayMgr_Arrest(DayMgr* self) {
    PlayerData* pd = *gPlayerData;
    PlayerParams params;
    int day = pd->day, period = pd->time + kPeriods;
    self->collapsed = 1;
    if (period > 4) {
        period = pd->time < 1 ? 0 : pd->time;
        day = pd->day + 1;
        if (day == 5)
            period = 4;
        if (day > kLastDay)
            day = kLastDay;
    }
    self->countdown = 0;
    SetDate(pd, params, day, period);
    DayMgr_Tick(self);
    SetHealth(pd, pd->healthMax);
    params.health() = pd->healthMax < params.healthMax() ? pd->healthMax : params.healthMax();
    DaysPassed(pd)++;
    TimePassed(pd) = (s16)(TimePassed(pd) + 25);
    DayMgr_CheckDeadlines(self);
}

// ── 0x232CA0 ──────────────────────────────────────────────────────────────
int DayMgr_IsNight(DayMgr* self) {
    if (self->inEvent != 0)
        return 0;
    PlayerData* pd = *gPlayerData;
    PlayerParams params;
    if (self->countdown == 0)
        return pd->periodTime + params.time() >= kPeriodLength;
    // A timed task: the period does not run out while it lasts, and it lasts
    // a fraction of a period (longer on Easy).
    self->elapsed += params.time();
    pd->periodTime = (s16)self->periodTime;
    params.resetTime();
    const float share = pd->difficulty == 0 ? 0.45f : 0.3f;
    if (TaskKind(pd) == 4 || (float)self->periodTime + share * 900.0f <= (float)self->elapsed) {
        pd->periodTime = (s16)self->periodTime;
        params.resetTime();
        self->elapsed = 0;
        self->countdown = 0;
    }
    return 0;
}

// ── 0x232E10 ──────────────────────────────────────────────────────────────
u32 DayMgr_EndDay(DayMgr* self) {
    PlayerData* pd = *gPlayerData;
    PlayerParams params;
    int day = pd->day, period;
    u32 result = kDayChanged;
    const int vigour = pd->_108 - 1;
    if (vigour < 1) {
        // Nothing left: collapse and lose the rest of the day.
        pd->_108 = pd->_10a < 1 ? pd->_10a : 0;
        period = pd->time + kPeriods;
        self->collapsed = 1;
        result = kDayChanged | kDayCollapsed;
        DaysPassed(pd)++;
        TimePassed(pd) = (s16)(TimePassed(pd) + 25);
    } else {
        SetVigour(pd, vigour);
        self->collapsed = 0;
        period = pd->time + 1;
    }
    if (period > 4) {
        if (day == kLastDay)
            *PlayerData_Flag(pd, 0xFF) = 1;
        period -= kPeriods;
        if (period < 1)
            period = 0;
        day = pd->day + 1;
        result |= kDayNewDay;
        if (day == 5)
            period = 4;
        if (day > kLastDay)
            day = kLastDay;
    }
    self->countdown = 0;
    SetDate(pd, params, day, period);
    DayMgr_CheckDeadlines(self);
    return result;
}

// ── 0x232FF0 ──────────────────────────────────────────────────────────────
u32 DayMgr_Advance(DayMgr* self, int fromEvent) {
    if (self->pending == 0)
        return 0;
    PlayerData* pd = *gPlayerData;
    PlayerParams params;
    u32 result = kDayChanged;
    int day = pd->day, period = pd->time + self->pending;
    self->collapsed = 0;
    if (period > 4) {
        if (day == kLastDay)
            *PlayerData_Flag(pd, 0xFF) = 1;
        int left;
        do {
            left = period - kPeriods;
            if (left > -1)
                period = left;
            day++;
        } while (left > 5);
        result = kDayChanged | kDayNewDay;
        if (day == 5)
            period = 4;
        if (day > kLastDay)
            day = kLastDay;
    }
    SetDate(pd, params, day, period);
    DayMgr_Tick(self);
    if (fromEvent == 0) {
        DaysPassed(pd)++;
        TimePassed(pd) = (s16)(self->pending * 5 + TimePassed(pd));
    }
    Matrix_Copy(pd->_6e40, guest::ptr(0x2D1A80u));
    self->pending = 0;
    DayMgr_CheckDeadlines(self);
    return result;
}
