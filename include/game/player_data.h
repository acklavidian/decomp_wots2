#pragma once
#include "hal/ps2_types.h"
#include "game/sword.h"

// The player / save data object at *0x2EE688.  Decompiled from
// 0x177400–0x1788A0.  Three parts: what survives between games (+0x000),
// the game in progress (+0x100, cleared by PlayerData_ResetGame) and the
// storehouse (+0x880).

enum { kCarriedSwords = 3, kStoredSwords = 100, kCarriedItems = 8, kHomeItems = 32,
       kItemKinds = 21, kAccessorySlots = 28 };

struct PlayerData {
    s32 state;              // +0x0000  2 = a game is in progress
    s32 _004;
    s32 score;              // +0x0008  total score: unlocks difficulties, heads, clothes
    s32 _00c, _010;
    s32 _014;               // +0x0014  2
    u8  _018[0x14];
    u8  seenA[30];          // +0x002C
    u8  _04a[2];
    u8  swordSeen[0x5F];    // +0x004C  by sword type
    u8  accessorySeen[0x55];// +0x00AB  by accessory id (0x21–0x3C)

    // ── the game in progress ──
    s8  day;                // +0x0100  0–9: the game lasts ten days
    s8  time;               // +0x0101  0–4: five periods a day (a new game starts at 1)
    s8  night;              // +0x0102  from the period (table 0x2DA160): picks the night maps
    s8  area;               // +0x0103  where the player is (10 = leaving the province)
    u8  _104;
    u8  difficulty;         // +0x0105
    s16 _106;
    s16 _108;               // +0x0108
    s16 _10a;               // +0x010A  10
    s16 periodTime;         // +0x010C  time used up in this period (900 ends it)
    s16 health;             // +0x010E
    s16 healthMax;          // +0x0110
    s16 chara;              // +0x0112  character shown (from the setup screen)
    s16 secret;             // +0x0114
    s16 head;               // +0x0116
    s16 _118;
    u16 accessory;          // +0x011A  accessory worn, 0 = none
    u8  _11c[0x10];
    s32 _12c[3];            // +0x012C  30 each
    s32 _138[3];            // +0x0138  30 each
    s32 _144[3];            // +0x0144  50 each
    u8  carried[kCarriedItems];     // +0x0150  item ids, 0 = empty
    u8  home[kHomeItems];           // +0x0158  kept sorted
    s16 swordCount;         // +0x0178
    s16 swordOrder[kCarriedSwords]; // +0x017A  0 = readied, -1 = empty slot
    Sword sword[kCarriedSwords];    // +0x0180
    u16 name[16];           // +0x0480
    u8  _4a0[0x30];
    u8  _4d0[0xF0];         // +0x04D0
    u8  _5c0[0x100];        // +0x05C0
    u8  _6c0[0x146];
    u8  _806[16];           // +0x0806  50 each
    u8  _816[0x2A];

    u8  _840[0xC];          // +0x0840
    u16 _84c;               // +0x084C
    u16 _84e;               // +0x084E
    u8  _850[0x18];
    s16 _868;               // +0x0868  -1
    u8  _86a[0x16];

    // ── the storehouse ──
    Sword stored[kStoredSwords];    // +0x0880
    s16 storedCount;        // +0x6C80
    s16 _6c82;
    s16 storedMax;          // +0x6C84  20
    u8  _6c86[0x3A];
    u8  stock[kItemKinds];          // +0x6CC0  number of each item (max 99)
    u8  itemSeen[kItemKinds];       // +0x6CD5
    u8  accessories[kAccessorySlots];   // +0x6CEA  accessory ids owned, 0 = empty
    u8  _6d06[0x3A];
    u8  _6d40[0x100];       // +0x6D40
    float _6e40[16];        // +0x6E40  matrix
    u32 _6e80[3];
};

extern "C" {
    void PlayerData_Reset(PlayerData* self, int newName);           // 0x178170
    void PlayerData_ResetGame(PlayerData* self, int newName);       // 0x1782E0
    void PlayerData_SetDefaultName(PlayerData* self);               // 0x178280

    // Swords carried.
    int    PlayerData_CarrySword(PlayerData* self, const Sword* sword);         // 0x177E80
    int    PlayerData_SetSword(PlayerData* self, const Sword* sword, int slot); // 0x177FD0
    int    PlayerData_RemoveSword(PlayerData* self, int slot);                  // 0x177E00
    int    PlayerData_GetReadiedSword(PlayerData* self);                        // 0x1780C0
    Sword* PlayerData_ReadySword(PlayerData* self, int slot);                   // 0x178100
    void   PlayerData_ClearSwords(PlayerData* self);                            // 0x177DE0
    void   PlayerData_ClearSwordMarks(PlayerData* self, int slot);              // 0x177D80
    bool   PlayerData_CanCarryAnother(PlayerData* self);                        // 0x177DD0
    // Swords in the storehouse.
    int    PlayerData_StoreSword(PlayerData* self, const Sword* sword);         // 0x177C00
    int    PlayerData_RemoveStoredSword(PlayerData* self, int index);           // 0x177B90

    // Items: `home` = 0 the eight carried, otherwise the 32 kept at home.
    int  PlayerData_AddItem(PlayerData* self, int home, u8 item);               // 0x177A30
    int  PlayerData_AddItemSorted(PlayerData* self, int home, u8 item);         // 0x177920
    int  PlayerData_RemoveItem(PlayerData* self, int home, u32 slot);           // 0x177860
    int  PlayerData_RemoveCarriedItem(PlayerData* self, u32 item);              // 0x1778E0
    int  PlayerData_SwapItems(PlayerData* self, int home, u32 a, u32 b);        // 0x1776F0
    int  PlayerData_HasFreeItemSlot(PlayerData* self, int home);                // 0x177760
    int  PlayerData_CountItems(PlayerData* self, int home);                     // 0x1777E0
    int  PlayerData_HasItem(PlayerData* self, int home, u32 item);              // 0x177AC0
    int  PlayerData_SortItems(PlayerData* self, int home);                      // 0x1779B0
    int  PlayerData_DropQuestItems(PlayerData* self);                           // 0x177B40
    // Items in stock, by kind.
    u8   PlayerData_GetItemCount(PlayerData* self, int item);                   // 0x177680
    void PlayerData_SetItemCount(PlayerData* self, int item, u8 count);         // 0x177610
    bool PlayerData_IsItemSeen(PlayerData* self, int item);                     // 0x1775A0

    // Story flags: two byte arrays, indexed with wrap-to-0 on a bad index.
    u8*  PlayerData_Flag(PlayerData* self, int index);                          // inlined: +0x740[0x100]
    u8*  PlayerData_DayFlag(PlayerData* self, int index);                       // inlined: +0x6C0[0x80]
    // Forgets the current task's description once the task is finished.
    void PlayerData_ClearFinishedTask(PlayerData* self);                        // 0x177300

    // Accessories.
    int  PlayerData_HasAccessory(PlayerData* self, u32 id);                     // 0x177440
    int  PlayerData_WornAccessorySlot(PlayerData* self);                        // 0x177480
    void PlayerData_SwapAccessories(PlayerData* self, int a, int b);            // 0x1774E0
    int  PlayerData_AddAccessory(PlayerData* self, u32 id);                     // 0x177500
    int  PlayerData_CountAccessoriesSeen(PlayerData* self);                     // 0x177D00
    int  PlayerData_CountSeenA(PlayerData* self);                               // 0x177D40
}
