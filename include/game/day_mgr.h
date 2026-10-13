#pragma once
#include "hal/ps2_types.h"

// The calendar (0x232040–0x2332E0; singleton at *0x2EE74C, 0x18 bytes).
// The game lasts ten days of five periods each; the day and period are
// PlayerData+0x100 / +0x101.  A period ends when the time spent in the world
// reaches 900, when the player rests, or when a story scene says so.

struct DayMgr {
    s32 inEvent;        // +0x00  a story scene is running: time stands still
    s32 collapsed;      // +0x04  the player ran out of vigour: it is refilled on leaving
    s32 countdown;      // +0x08  a timed task is running
    s32 elapsed;        // +0x0C  time into the timed task
    s32 periodTime;     // +0x10  period time when the task began
    s32 pending;        // +0x14  periods a story scene asked to skip
};

// What a change of period means for the scene.
enum : u32 { kDayChanged = 1, kDayNewDay = 0x10, kDayCollapsed = 0x20 };

extern "C" {
    DayMgr* DayMgr_Get();                                       // 0x2332E0
    void DayMgr_Release();                                      // 0x2332A0
    // Leaving the world: bank the time spent there.
    void DayMgr_Save(DayMgr* self);                             // 0x232750
    // Resting for 1–5 periods: restores vigour and health.
    u32  DayMgr_GetChoiceFlags(DayMgr* self, int periods);      // 0x232390
    void DayMgr_SetEvent(DayMgr* self, int inEvent);            // 0x2326C0
    void DayMgr_EndPeriodNow(DayMgr* self);                     // 0x232670
    int  DayMgr_PeriodOver(DayMgr* self);                       // 0x232C30
    void DayMgr_StartJob(DayMgr* self);                         // 0x2331B0
    // Applies the periods a story scene skipped; 0 if there were none.
    u32  DayMgr_Advance(DayMgr* self, int fromEvent);           // 0x232FF0
    // Thrown in jail: the rest of the day is gone.
    void DayMgr_Arrest(DayMgr* self);                           // 0x232A60
    // The period is over: costs a point of vigour.
    u32  DayMgr_EndDay(DayMgr* self);                           // 0x232E10
    // True when the period's time has run out.
    int  DayMgr_IsNight(DayMgr* self);                          // 0x232CA0
    // Clears what only lasts for one period.
    void DayMgr_Tick(DayMgr* self);                             // 0x2328F0
    void DayMgr_CheckDeadlines(DayMgr* self);                   // 0x232040
}
