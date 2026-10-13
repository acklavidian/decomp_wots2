// Player data: belongings and reset — decompiled from 0x177440–0x1788A0.
// See include/game/player_data.h.

#include "game/player_data.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

extern "C" {
    int  WStr_Length(const u16* s);                 // 0x16E260
    void WStr_Copy(u16* dst, const u16* src);       // 0x16E120
    void Matrix_Copy(void* dst, const void* src);   // 0x1073C0
}

static_assert(sizeof(Sword) == 0x100, "Sword");
static_assert(offsetof(PlayerData, difficulty) == 0x105 && offsetof(PlayerData, carried) == 0x150, "PlayerData");
static_assert(offsetof(PlayerData, swordCount) == 0x178 && offsetof(PlayerData, sword) == 0x180, "PlayerData");
static_assert(offsetof(PlayerData, name) == 0x480 && offsetof(PlayerData, _806) == 0x806, "PlayerData");
static_assert(offsetof(PlayerData, _868) == 0x868 && offsetof(PlayerData, stored) == 0x880, "PlayerData");
static_assert(offsetof(PlayerData, storedCount) == 0x6C80 && offsetof(PlayerData, stock) == 0x6CC0, "PlayerData");
static_assert(offsetof(PlayerData, accessories) == 0x6CEA && offsetof(PlayerData, _6e40) == 0x6E40, "PlayerData");

static const u16* const kDefaultName = guest::ptr<const u16>(0x2D2240u);    // "MIFUNE"

// ══ Story flags ═══════════════════════════════════════════════════════════
static int ClampIndex(int index, int count) { return (index < 0 || index >= count) ? 0 : index; }   // 0x1772D0
u8* PlayerData_Flag(PlayerData* self, int index) {
    return reinterpret_cast<u8*>(self) + 0x740 + ClampIndex(index, 0x100);
}
u8* PlayerData_DayFlag(PlayerData* self, int index) {
    return reinterpret_cast<u8*>(self) + 0x6C0 + ClampIndex(index, 0x80);
}

// ── 0x177300 ──────────────────────────────────────────────────────────────
// Flags 0xBC–0xBF hold the progress of the four story lines; 99 = over.
void PlayerData_ClearFinishedTask(PlayerData* self) {
    u8* b = reinterpret_cast<u8*>(self);
    const s8 task = (s8)b[0x83E];
    bool done = false;
    switch (task) {
    case 5: done = b[0x7FE] == 99 || b[0x7FF] == 99; break;
    case 4: done = b[0x7FF] == 99; break;
    case 3: done = b[0x7FE] == 99; break;
    case 2: done = b[0x7FD] == 99; break;
    case 1: done = b[0x7FC] == 99; break;
    }
    if (done) {
        memset(self->_5c0, 0, sizeof(self->_5c0));
        b[0x83E] = 0;
    }
}

// ══ Accessories ═══════════════════════════════════════════════════════════
// ── 0x177440 ──────────────────────────────────────────────────────────────
int PlayerData_HasAccessory(PlayerData* self, u32 id) {
    for (int i = 0; i < kAccessorySlots; i++)
        if (self->accessories[i] == id)
            return 1;
    return 0;
}

// ── 0x177480 ──────────────────────────────────────────────────────────────
int PlayerData_WornAccessorySlot(PlayerData* self) {
    if (self->accessory != 0)
        for (int i = 0; i < kAccessorySlots; i++)
            if (self->accessories[i] == self->accessory)
                return i;
    return -1;
}

// ── 0x1774E0 ──────────────────────────────────────────────────────────────
void PlayerData_SwapAccessories(PlayerData* self, int a, int b) {
    u8 t = self->accessories[a];
    self->accessories[a] = self->accessories[b];
    self->accessories[b] = t;
}

// ── 0x177500 ──────────────────────────────────────────────────────────────
// Accessory ids are 0x21–0x3C; one of each at most.
int PlayerData_AddAccessory(PlayerData* self, u32 id) {
    if ((s32)id < 0x21 || (s32)id > 0x3C)
        return 0;
    int free = -1;
    for (int i = 0; i < kAccessorySlots; i++) {
        if (self->accessories[i] == id)
            return 0;
        if (free == -1 && self->accessories[i] == 0)
            free = i;
    }
    if (free == -1)
        return 0;
    self->accessories[free] = (u8)id;
    reinterpret_cast<u8*>(self)[0xAB + id] = 1;
    return 1;
}

// ── 0x177D00 ──────────────────────────────────────────────────────────────
int PlayerData_CountAccessoriesSeen(PlayerData* self) {
    int missing = 0;
    for (const u8* p = reinterpret_cast<u8*>(self) + 0xCC; p != reinterpret_cast<u8*>(self) + 0xE8; p++)
        if (*p == 0)
            missing++;
    return 28 - missing;
}

// ── 0x177D40 ──────────────────────────────────────────────────────────────
int PlayerData_CountSeenA(PlayerData* self) {
    int missing = 0;
    for (u8 seen : self->seenA)
        if (seen == 0)
            missing++;
    return 30 - missing;
}

// ══ Items in stock ════════════════════════════════════════════════════════
// Item ids skip 0 and 10.
static int StockIndex(int item) {
    if (item == 0 || item == 10)
        return -1;
    int index = item < 10 ? item - 1 : item - 2;
    return index < kItemKinds ? index : -1;
}

// ── 0x1775A0 ──────────────────────────────────────────────────────────────
bool PlayerData_IsItemSeen(PlayerData* self, int item) {
    int index = StockIndex(item);
    return index != -1 && self->itemSeen[index] != 0;
}

// ── 0x177610 ──────────────────────────────────────────────────────────────
void PlayerData_SetItemCount(PlayerData* self, int item, u8 count) {
    int index = StockIndex(item);
    if (index == -1)
        return;
    if (count > 99)
        count = 99;
    self->stock[index] = count;
    if (count != 0)
        self->itemSeen[index] = 1;
}

// ── 0x177680 ──────────────────────────────────────────────────────────────
u8 PlayerData_GetItemCount(PlayerData* self, int item) {
    int index = StockIndex(item);
    return index == -1 ? 0 : self->stock[index];
}

// ══ Item slots ════════════════════════════════════════════════════════════
static inline u8* Slots(PlayerData* self, int home) { return home == 0 ? self->carried : self->home; }
static inline u32 SlotCount(int home) { return home == 0 ? kCarriedItems : kHomeItems; }

// ── 0x1776F0 ──────────────────────────────────────────────────────────────
int PlayerData_SwapItems(PlayerData* self, int home, u32 a, u32 b) {
    u8* slots = Slots(self, home);
    const u32 count = SlotCount(home);
    if ((a & 0xFF) >= count || (b & 0xFF) >= count)
        return 0;
    u8 t = slots[a & 0xFF];
    slots[a & 0xFF] = slots[b & 0xFF];
    slots[b & 0xFF] = t;
    return 1;
}

// ── 0x177760 ──────────────────────────────────────────────────────────────
int PlayerData_HasFreeItemSlot(PlayerData* self, int home) {
    const u8* slots = Slots(self, home);
    for (u32 i = 0; i < SlotCount(home); i++)
        if (slots[i] == 0)
            return 1;
    return 0;
}

// ── 0x1777E0 ──────────────────────────────────────────────────────────────
int PlayerData_CountItems(PlayerData* self, int home) {
    const u8* slots = Slots(self, home);
    int count = 0;
    for (u32 i = 0; i < SlotCount(home); i++)
        if (slots[i] != 0)
            count++;
    return count;
}

// ── 0x1779B0 ──────────────────────────────────────────────────────────────
// Items kept at home are sorted by id with the empty slots last.
int PlayerData_SortItems(PlayerData* self, int home) {
    if (home != 0) {
        for (int pass = 0; pass < kHomeItems; pass++) {
            for (int i = 1; i < kHomeItems; i++) {
                u8 next = self->home[i], here = self->home[i - 1];
                if ((next < here || here == 0) && next != 0) {
                    self->home[i - 1] = next;
                    self->home[i] = here;
                }
            }
        }
    }
    return 1;
}

// ── 0x177860 ──────────────────────────────────────────────────────────────
int PlayerData_RemoveItem(PlayerData* self, int home, u32 slot) {
    u8* slots = Slots(self, home);
    if (SlotCount(home) < (slot & 0xFF))        // (sic: the count itself is let through)
        return 0;
    if (slots[slot & 0xFF] == 0)
        return 0;
    slots[slot & 0xFF] = 0;
    PlayerData_SortItems(self, home);
    return 1;
}

// ── 0x1778E0 ──────────────────────────────────────────────────────────────
int PlayerData_RemoveCarriedItem(PlayerData* self, u32 item) {
    for (int i = 0; i < kCarriedItems; i++) {
        if (self->carried[i] == item) {
            self->carried[i] = 0;
            return 1;
        }
    }
    return 0;
}

// ── 0x177920 ──────────────────────────────────────────────────────────────
int PlayerData_AddItemSorted(PlayerData* self, int home, u8 item) {
    if (item == 0)
        return 0;
    u8* slots = Slots(self, home);
    for (u32 i = 0; i < SlotCount(home); i++) {
        if (slots[i] == 0) {
            slots[i] = item;
            PlayerData_SortItems(self, home);
            return 1;
        }
    }
    return 0;
}

// ── 0x177A30 ──────────────────────────────────────────────────────────────
int PlayerData_AddItem(PlayerData* self, int home, u8 item) {
    u8* slots = Slots(self, home);
    for (u32 i = 0; i < SlotCount(home); i++) {
        if (slots[i] == 0) {
            slots[i] = item;
            return 1;
        }
    }
    return 0;
}

// ── 0x177AC0 ──────────────────────────────────────────────────────────────
int PlayerData_HasItem(PlayerData* self, int home, u32 item) {
    const u8* slots = Slots(self, home);
    for (u32 i = 0; i < SlotCount(home); i++)
        if (slots[i] == item)
            return 1;
    return 0;
}

// ── 0x177B40 ──────────────────────────────────────────────────────────────
// Items 0x17–0x1D do not stay at home.
int PlayerData_DropQuestItems(PlayerData* self) {
    for (u8& item : self->home)
        if (item > 0x16 && item < 0x1E)
            item = 0;
    return 1;
}

// ══ Swords ════════════════════════════════════════════════════════════════
// ── 0x177B90 ──────────────────────────────────────────────────────────────
int PlayerData_RemoveStoredSword(PlayerData* self, int index) {
    if (index < 0 || index >= self->storedCount)
        return 0;
    memmove(&self->stored[index], &self->stored[index + 1],
            (size_t)(self->storedCount - index - 1) * sizeof(Sword));
    self->storedCount--;
    return 1;
}

// ── 0x177C00 ──────────────────────────────────────────────────────────────
int PlayerData_StoreSword(PlayerData* self, const Sword* sword) {
    if (self->storedCount >= self->storedMax)
        return 0;
    self->stored[self->storedCount] = *sword;
    self->storedCount++;
    return 1;
}

// ── 0x177D80 ──────────────────────────────────────────────────────────────
// (Only -1, "all", does anything: the test for a single slot can never pass.)
void PlayerData_ClearSwordMarks(PlayerData* self, int slot) {
    if (slot == -1) {
        for (Sword& sword : self->sword)
            sword._80 = 0;
    }
}

// ── 0x177DD0 ──────────────────────────────────────────────────────────────
bool PlayerData_CanCarryAnother(PlayerData* self) {
    return self->swordCount + 1 < 4;
}

// ── 0x177DE0 ──────────────────────────────────────────────────────────────
void PlayerData_ClearSwords(PlayerData* self) {
    self->swordCount = 0;
    self->swordOrder[0] = self->swordOrder[1] = self->swordOrder[2] = -1;
}

// ── 0x177E00 ──────────────────────────────────────────────────────────────
int PlayerData_RemoveSword(PlayerData* self, int slot) {
    const s16 order = self->swordOrder[slot];
    for (s16& other : self->swordOrder)
        if (order < other)
            other--;
    self->swordOrder[slot] = -1;
    self->swordCount--;
    return 1;
}

// ── 0x177E80 ──────────────────────────────────────────────────────────────
// Puts the sword in the first empty slot, last in the order.
int PlayerData_CarrySword(PlayerData* self, const Sword* sword) {
    if (self->swordCount >= kCarriedSwords)
        return 0;
    int slot = 0;
    for (; slot < kCarriedSwords; slot++)
        if (self->swordOrder[slot] == -1)
            break;
    self->sword[slot] = *sword;
    self->swordOrder[slot] = self->swordCount;
    self->swordCount++;
    self->swordSeen[sword->type == 0 ? 12 : sword->type] = 1;
    return 1;
}

// ── 0x177FD0 ──────────────────────────────────────────────────────────────
int PlayerData_SetSword(PlayerData* self, const Sword* sword, int slot) {
    self->sword[slot] = *sword;
    return 1;
}

// ── 0x1780C0 ──────────────────────────────────────────────────────────────
int PlayerData_GetReadiedSword(PlayerData* self) {
    for (int i = 0; i < kCarriedSwords; i++)
        if (self->swordOrder[i] == 0)
            return i;
    return -1;
}

// ── 0x178100 ──────────────────────────────────────────────────────────────
// Makes `slot` the readied sword; the others follow in slot order.
Sword* PlayerData_ReadySword(PlayerData* self, int slot) {
    s16 order = 0;
    for (int i = 0; i < kCarriedSwords; i++) {
        s16& entry = self->swordOrder[(i + slot) % 3];
        if (entry != -1)
            entry = order++;
    }
    return &self->sword[slot];
}

// ══ Reset ═════════════════════════════════════════════════════════════════
// ── 0x178280 ──────────────────────────────────────────────────────────────
void PlayerData_SetDefaultName(PlayerData* self) {
    u16 name[16];
    memcpy(name, kDefaultName, sizeof(name));
    if (WStr_Length(name) < 16)
        WStr_Copy(self->name, name);
}

// ── 0x1782E0 ──────────────────────────────────────────────────────────────
// Clears the game in progress.  Unless `newName`, the name and the setup
// choices are kept, and — if the last character was alive — what was carried.
void PlayerData_ResetGame(PlayerData* self, int newName) {
    u16 name[16];
    memcpy(name, self->name, sizeof(name));
    const s16 health = self->health;
    const s16 chara = self->chara, secret = self->secret, head = self->head;
    const s16 swordCount = self->swordCount;
    Sword swords[kCarriedSwords];
    s16 order[kCarriedSwords];
    u8 carried[kCarriedItems];
    for (int i = 0; i < kCarriedSwords; i++) {
        Sword_Ctor(&swords[i]);
        order[i] = self->swordOrder[i];
        swords[i] = self->sword[i];
    }
    memcpy(carried, self->carried, sizeof(carried));

    memset(&self->day, 0, 0x740);
    for (int i = 0; i < 3; i++) {
        self->_144[i] = 50;
        self->_12c[i] = 30;
        self->_138[i] = 30;
    }
    self->_10a = 10;
    self->_108 = 10;
    self->healthMax = 1000;
    self->health = 1000;
    memset(self->carried, 0, sizeof(self->carried));
    memset(self->home, 0, sizeof(self->home));
    PlayerData_ClearSwords(self);
    self->time = 1;
    self->night = 0;
    self->_108 = self->_10a < 5 ? self->_10a : 4;
    {   // an inlined setter for +0x106 that tallies the change either way
        const s16 before = self->_106;
        self->_106 = 0;
        int delta = self->_106 - before;
        if (delta < 1) {
            int total = self->_84e + abs(delta);
            self->_84e = (u16)(total > 0xFFFF ? 0xFFFF : total);
        } else {
            int total = self->_84c + delta;
            self->_84c = (u16)(total > 0xFFFF ? 0xFFFF : total);
        }
    }
    self->accessory = 0;
    memset(self->_4d0, 0, sizeof(self->_4d0));
    memset(self->_5c0, 0, sizeof(self->_5c0));

    if (newName == 0) {
        if (WStr_Length(name) < 16)
            WStr_Copy(self->name, name);
        self->chara = chara;
        self->secret = secret;
        self->head = head;
        if (health > 0) {
            self->swordCount = swordCount;
            for (int i = 0; i < kCarriedSwords; i++) {
                self->swordOrder[i] = order[i];
                self->sword[i] = swords[i];
            }
            memcpy(self->carried, carried, sizeof(carried));
        }
    } else {
        PlayerData_SetDefaultName(self);
    }
    memset(self->_840, 0, 0x40);
    self->_868 = -1;
    memset(self->_806, 50, sizeof(self->_806));
    for (int i = kCarriedSwords - 1; i >= 0; i--)
        Sword_Dtor(&swords[i], -1);
}

// ── 0x178170 ──────────────────────────────────────────────────────────────
void PlayerData_Reset(PlayerData* self, int newName) {
    memset(self, 0, 0x100);
    self->_014 = 2;
    PlayerData_ResetGame(self, newName);
    memset(self->stored, 0, 0x6440);
    memset(self->stock, 0, 0x80);
    self->_6c82 = 0;
    self->storedMax = 20;
    Matrix_Copy(self->_6e40, guest::ptr(0x2D1A80u));
    memset(self->_6d40, 0, sizeof(self->_6d40));
    self->_6e80[0] = self->_6e80[1] = self->_6e80[2] = 0;
}
