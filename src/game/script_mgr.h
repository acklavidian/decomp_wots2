// Script Manager — Way of the Samurai 2 event/cutscene scripting system.
//
// Data layout discovered at 0x2E7100 in SLUS_208.93:
//   0x2E7100: ScriptConfig struct (8×u32)
//   0x2E7120: function pointer table A (7 entries, type ScriptGroupFn*)
//   0x2E7140: function pointer table B (7 entries)
//   0x2E7160: command name string table (8-byte-aligned null-terminated strings)
//
// Command strings found (8-byte aligned):
//   Init, Start, End, AddActor, DelActorAll, Exclude, Enable, Disable,
//   SetTime, SetAreaOut, SetActorHP_Val, SetActorHP_Per, SetActorCount,
//   SetActorStrike, BreakRuleAllClear, GetBreakID, SetSeeCharacter,
//   GetGroupID, SetGroupID, SetName, ...

#pragma once
#include "hal/ps2_types.h"

// Opaque context passed to script command handlers
struct ScriptContext;

// Script command handler function type (used in the indirect dispatch table)
using ScriptGroupFn = void (*)(ScriptContext*);

// Config block at 0x2E7100 — appears to hold frame count thresholds
// (values: 0, 0x640000, 0x01F4012C, 0x051402BC, 0x09C40640, 0x0FA00AF0, 0x1194, 0)
struct ScriptConfig {
    u32 pad0;
    u32 field_04;    // 0x640000
    u32 field_08;    // 0x01F4012C (pair: 500, 300?)
    u32 field_0C;    // 0x051402BC
    u32 field_10;    // 0x09C40640
    u32 field_14;    // 0x0FA00AF0
    u32 field_18;    // 0x00001194 (4500?)
    u32 pad1;
};

// Script module descriptor — each module has Init/Start/End + command handlers
struct ScriptModule {
    ScriptGroupFn fn_table_a[7];  // at 0x2E7120 — group A handlers
    u32           pad_a;
    ScriptGroupFn fn_table_b[7];  // at 0x2E7140 — group B handlers
    u32           pad_b;
    // 0x2E7160+: command name string table follows (not a struct member,
    // accessed by scanning for null terminator, 8-byte aligned)
};

// Look up a command handler by name (strcmp scan of the string table)
ScriptGroupFn ScriptMgr_FindCommand(const char* name);

// Execute the named command in the given context
void ScriptMgr_Exec(const char* cmd, ScriptContext* ctx);
