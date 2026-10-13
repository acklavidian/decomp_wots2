#pragma once
#include "hal/ps2_types.h"

#include <map>
#include <vector>

// Skill sheets as the player sees them — see src/game/skill_sheets.cpp.

// One technique on a chart (the original's map value, 0x20 bytes: these
// eight words and a ref-counted string).
struct SkillCell {
    s16 column = 0;     // +0x00
    s16 row = 0;        // +0x02
    s16 kind = 0;       // +0x04  0 plain; 1 Float, 2 Break, 3 Crush, 4 Throw, 5: the word printed on it
    u16 buttons = 0;    // +0x06  0x1000 Triangle, 0x2000 Square, 0x4000 R1 + Triangle, 0x8000 in the air; | direction bits
    s16 tally = 0;      // +0x08  what the sword must have done to learn it
    s16 special = 0;    // +0x0A  1: name in gold
    s16 frame = 0;      // +0x0C  which border (0 / 1 / 2)
    s16 follows = 0;    // +0x0E  the technique it is a later form of, 0 none
    u32 name = 0;       // +0x10  16-bit string in guest memory
};

struct SkillLink {
    s16 row0, column0, row1, column1;   // the two cells a line joins
    s16 from, to;                       // their techniques
};

// One chart (0x1D0 bytes in the original).
struct SkillChart {
    u8  loaded = 0;                             // +0x00
    s16 columns = 0, rows = 0;                  // +0x02 / +0x04
    s32 sheet = -1;                             // +0x08
    std::map<s16, SkillCell> techniques;        // +0x0C
    std::vector<std::vector<s16>> groups;       // +0x1C  cells that are one technique
    std::vector<SkillLink> links;               // +0x28
    std::vector<std::vector<s16>> groups2;      // +0x34  (version-1 files only)
};

extern "C" {
    void* SkillSheets_Get();                                        // 0x23DC20
    void  SkillSheets_Boot();                                       // (in App_CreateDevices)
    void  SkillSheets_SetFiles(void* self, const u8* list);         // 0x23DA20
    // slot: 0–2 a carried sword, 4 the "Check moves" screen
    int   SkillSheets_Request(void* self, int sheet, int slot);     // 0x23D870  1 there, 2 reading, 3 busy, −1 no
    int   SkillSheets_Loaded(void* self, int slot);                 // 0x23D750
    int   SkillSheets_Poll(void* self, int slot);                   // 0x23D7D0  4 nothing asked, 0 not yet, 1 done
    int   SkillSheets_Free(void* self, int slot);                   // 0x23DEC0
    void  SkillSheets_Reset(void* self);                            // 0x23DB90
    const SkillChart* SkillSheets_Chart(void* self, int slot);      // 0x23C580 … 0x23C800
    const u16* SkillSheets_MakeNotice(void* self, const void* sword, int slot, int technique, int next);    // 0x23BA90
    void  SwordRecords_Learnt(void* self, void* sword, int slot, int technique, int next);                  // 0x23B7F0
    void  SkillSheets_Announce(void* self, const void* sword, int slot, int technique, int next);           // 0x23C4C0
}
