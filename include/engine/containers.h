#pragma once
#include "hal/ps2_types.h"

// The game's std::vector<T> as it sits in memory (Metrowerks MSL):
// capacity, size, data.  `data` is a guest address.
struct GuestVector {
    u32 capacity;
    u32 count;
    u32 data;
};

// The game's std::list<T> for 4-byte T: a size and a sentinel node; nodes are
// {prev, next, value}, 12 bytes each, allocated with MemAlloc.
struct GuestListNode {
    u32 prev, next;
    u32 value;
};
struct GuestList {
    u32 size;           // +0x00
    u32 endPrev;        // +0x04  the sentinel's prev (last node)
    u32 endNext;        // +0x08  the sentinel's next (first node)
};

// The game's std::map<int, T> for 4-byte T: a red-black tree.  The word after
// the count doubles as the end node (its `left` is the root); a node's parent
// word carries its colour in bit 0 (1 = red).
struct GuestMapNode {
    u32 left, right;
    u32 parent;         // | colour
    s32 key;
    u32 value;
};
struct GuestIntMap {
    u32 count;          // +0x00
    u32 root;           // +0x04  = end node's left
    u8  flag;           // +0x08
    u32 leftmost;       // +0x0C  first node in order (the end node when empty)
};

extern "C" {
    void GuestIntMap_Init(GuestIntMap* map);                                // 0x194550
    // Returns the node with `key`, or the end node.
    GuestMapNode* GuestIntMap_Find(GuestIntMap* map, s32 key);              // 0x170B90
    // Inserts unless the key is present; returns the node either way.
    GuestMapNode* GuestIntMap_Insert(GuestIntMap* map, s32 key, u32 value); // 0x194480 (and its copies)
    // The same for a value of any size (the game has one copy of the insert
    // code per value type); the node is 0x10 bytes + the value.
    GuestMapNode* GuestIntMap_InsertBytes(GuestIntMap* map, s32 key, const void* value, u32 size);
    GuestMapNode* GuestIntMap_Next(GuestMapNode* node);                     // 0x14C680
    void GuestIntMap_Erase(GuestIntMap* map, GuestMapNode* node);           // 0x1C0690 (and its copies)
    // Frees every node (not what the values point to).
    void GuestIntMap_Clear(GuestIntMap* map);                               // 0x194440

    void GuestList_Init(GuestList* list);                                   // 0x13EF00
    // Inserts before `pos` (a node, or the end); returns the new node.
    GuestListNode* GuestList_Insert(GuestList* list, GuestListNode* pos, u32 value);   // 0x13EE00
    // Removes a node; returns the one after it.
    GuestListNode* GuestList_Erase(GuestList* list, GuestListNode* node);  // 0x13ED80
    void GuestList_Clear(GuestList* list);                                  // 0x13ECC0 over the whole list

    // push_back for 4-byte elements.  (The game calls the library's
    // insert(pos, n, value) at 0x102BA0 with pos = end and n = 1.)
    void VectorU32_PushBack(GuestVector* v, u32 value);
    // push_back for elements of any size.
    void* GuestVector_PushBack(GuestVector* v, const void* element, u32 elementSize);
}

#include "native/guest.h"
inline GuestListNode* GuestList_End(GuestList* list)   { return reinterpret_cast<GuestListNode*>(&list->endPrev); }
inline GuestListNode* GuestList_Begin(GuestList* list) { return guest::ptr<GuestListNode>(list->endNext); }
inline GuestListNode* GuestList_Next(GuestListNode* n) { return guest::ptr<GuestListNode>(n->next); }
inline GuestMapNode* GuestIntMap_End(GuestIntMap* map)   { return reinterpret_cast<GuestMapNode*>(&map->root); }
inline GuestMapNode* GuestIntMap_Begin(GuestIntMap* map) { return guest::ptr<GuestMapNode>(map->leftmost); }
