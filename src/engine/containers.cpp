// std::vector growth as the game's library does it: capacity doubles from 1.

#include "engine/containers.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);       // 0x100820
    void  MemFree(void* p);         // 0x100650
}

void* GuestVector_PushBack(GuestVector* v, const void* element, u32 elementSize) {
    if (v->capacity < v->count + 1) {
        u32 capacity = v->capacity ? v->capacity : 1;
        while (capacity < v->count + 1)
            capacity <<= 1;
        void* old = guest::ptr(v->data);
        void* grown = MemAlloc(capacity * elementSize);
        if (old) {
            memcpy(grown, old, v->count * elementSize);
            MemFree(old);
        }
        v->data = guest::addr(grown);
        v->capacity = capacity;
    }
    void* slot = guest::ptr<u8>(v->data) + v->count * elementSize;
    memcpy(slot, element, elementSize);
    v->count++;
    return slot;
}

void VectorU32_PushBack(GuestVector* v, u32 value) {
    GuestVector_PushBack(v, &value, 4);
}

// ── std::list ─────────────────────────────────────────────────────────────
// 0x13EF00
void GuestList_Init(GuestList* list) {
    list->size = 0;
    list->endNext = guest::addr(GuestList_End(list));
    list->endPrev = guest::addr(GuestList_End(list));
}

// 0x13EE00
GuestListNode* GuestList_Insert(GuestList* list, GuestListNode* pos, u32 value) {
    GuestListNode* node = static_cast<GuestListNode*>(MemAlloc(sizeof(GuestListNode)));
    node->value = value;
    guest::ptr<GuestListNode>(pos->prev)->next = guest::addr(node);
    node->prev = pos->prev;
    pos->prev = guest::addr(node);
    node->next = guest::addr(pos);
    list->size++;
    return node;
}

// 0x13ED80
GuestListNode* GuestList_Erase(GuestList* list, GuestListNode* node) {
    GuestListNode* next = guest::ptr<GuestListNode>(node->next);
    guest::ptr<GuestListNode>(node->prev)->next = node->next;
    next->prev = node->prev;
    MemFree(node);
    list->size--;
    return next;
}

// 0x13ECC0 (begin … end)
void GuestList_Clear(GuestList* list) {
    GuestListNode* node = GuestList_Begin(list);
    while (node != GuestList_End(list))
        node = GuestList_Erase(list, node);
}

// ══ std::map<int, T> ══════════════════════════════════════════════════════
static inline GuestMapNode* N(u32 a) { return guest::ptr<GuestMapNode>(a); }
static inline u32 A(const GuestMapNode* n) { return guest::addr(n); }
static inline GuestMapNode* Parent(const GuestMapNode* n) { return N(n->parent & ~1u); }
static inline bool IsRed(const GuestMapNode* n) { return (n->parent & 1) != 0; }

// ── 0x194550 ──────────────────────────────────────────────────────────────
void GuestIntMap_Init(GuestIntMap* map) {
    map->count = 0;
    map->root = 0;
    map->flag = 0;
    map->leftmost = guest::addr(GuestIntMap_End(map));
}

// ── 0x170B90 ──────────────────────────────────────────────────────────────
GuestMapNode* GuestIntMap_Find(GuestIntMap* map, s32 key) {
    GuestMapNode* end = GuestIntMap_End(map);
    GuestMapNode* best = end;
    for (GuestMapNode* n = N(map->root); n;) {
        if (n->key < key) {
            n = N(n->right);
        } else {
            best = n;
            n = N(n->left);
        }
    }
    return (best == end || key < best->key) ? end : best;
}

// ── 0x14E6C0 ──────────────────────────────────────────────────────────────
static void RotateRight(GuestMapNode* n, GuestMapNode** root) {
    GuestMapNode* child = N(n->left);
    if (*root == n)
        *root = child;
    n->left = child->right;
    if (child->right)
        N(child->right)->parent = A(n) | (N(child->right)->parent & 1);
    child->parent = (n->parent & ~1u) | (child->parent & 1);
    GuestMapNode* parent = Parent(n);
    if (A(n) == parent->left)
        parent->left = A(child);
    else
        parent->right = A(child);
    child->right = A(n);
    n->parent = A(child) | (n->parent & 1);
}

// ── 0x14E750 ──────────────────────────────────────────────────────────────
static void RotateLeft(GuestMapNode* n, GuestMapNode** root) {
    GuestMapNode* child = N(n->right);
    if (*root == n)
        *root = child;
    n->right = child->left;
    if (child->left)
        N(child->left)->parent = A(n) | (N(child->left)->parent & 1);
    child->parent = (n->parent & ~1u) | (child->parent & 1);
    GuestMapNode* parent = Parent(n);
    if (A(n) == parent->left)
        parent->left = A(child);
    else
        parent->right = A(child);
    child->left = A(n);
    n->parent = A(child) | (n->parent & 1);
}

// ── 0x14E4A0 ──────────────────────────────────────────────────────────────
// Restores the red-black rules after `n` is linked in.
static void Rebalance(GuestMapNode* n, GuestMapNode* root) {
    n->parent |= 1;
    while (n != root) {
        GuestMapNode* parent = Parent(n);
        if (!IsRed(parent))
            break;
        GuestMapNode* grand = Parent(parent);
        if (A(parent) == grand->left) {
            GuestMapNode* uncle = N(grand->right);
            if (!uncle || !IsRed(uncle)) {
                if (A(n) == parent->right) {
                    RotateLeft(parent, &root);
                    n = parent;
                }
                Parent(n)->parent &= ~1u;
                Parent(Parent(n))->parent |= 1;
                RotateRight(Parent(Parent(n)), &root);
            } else {
                parent->parent &= ~1u;
                uncle->parent &= ~1u;
                n = Parent(Parent(n));
                n->parent |= 1;
            }
        } else {
            GuestMapNode* uncle = N(grand->left);
            if (!uncle || !IsRed(uncle)) {
                if (A(n) == parent->left) {
                    RotateRight(parent, &root);
                    n = parent;
                }
                Parent(n)->parent &= ~1u;
                Parent(Parent(n))->parent |= 1;
                RotateLeft(Parent(Parent(n)), &root);
            } else {
                parent->parent &= ~1u;
                uncle->parent &= ~1u;
                n = Parent(Parent(n));
                n->parent |= 1;
            }
        }
    }
    root->parent &= ~1u;
}

// ── 0x194480 + 0x194570 ───────────────────────────────────────────────────
GuestMapNode* GuestIntMap_InsertBytes(GuestIntMap* map, s32 key, const void* value, u32 size) {
    GuestMapNode* parent = GuestIntMap_End(map);
    GuestMapNode* notLess = nullptr;        // last node whose key is <= key
    bool asLeft = true, leftmost = true;
    for (GuestMapNode* n = N(map->root); n;) {
        parent = n;
        if (n->key <= key) {
            notLess = n;
            leftmost = false;
            n = N(n->right);
        } else {
            n = N(n->left);
        }
        asLeft = parent->key > key;
    }
    if (notLess && !(notLess->key < key))
        return notLess;

    GuestMapNode* node = static_cast<GuestMapNode*>(MemAlloc((u32)offsetof(GuestMapNode, value) + size));
    node->key = key;
    memcpy(&node->value, value, size);
    node->left = node->right = 0;
    node->parent = A(parent);
    if (asLeft)
        parent->left = A(node);
    else
        parent->right = A(node);
    map->count++;
    Rebalance(node, N(map->root));
    if (leftmost)
        map->leftmost = A(node);
    return node;
}
GuestMapNode* GuestIntMap_Insert(GuestIntMap* map, s32 key, u32 value) {
    return GuestIntMap_InsertBytes(map, key, &value, 4);
}

// ── 0x14C680 ──────────────────────────────────────────────────────────────
GuestMapNode* GuestIntMap_Next(GuestMapNode* node) {
    if (node->right) {
        GuestMapNode* n = N(node->right);
        while (n->left)
            n = N(n->left);
        return n;
    }
    GuestMapNode* parent = Parent(node);
    while (A(node) != parent->left) {
        node = parent;
        parent = Parent(node);
    }
    return parent;
}

// ── 0x1C0690 (and its copies) + 0x16C610 ──────────────────────────────────
// Unlinks and frees a node.  (The usual red-black removal; written from the
// algorithm, not compared with 0x16C610 instruction by instruction — only the
// tree's shape could differ, never its contents or order.)
void GuestIntMap_Erase(GuestIntMap* map, GuestMapNode* z) {
    GuestMapNode* end = GuestIntMap_End(map);
    if (A(z) == map->leftmost)
        map->leftmost = A(GuestIntMap_Next(z));
    auto setParent = [](GuestMapNode* n, GuestMapNode* parent) { n->parent = A(parent) | (n->parent & 1); };
    auto setRed = [](GuestMapNode* n, bool red) { n->parent = (n->parent & ~1u) | (red ? 1u : 0u); };
    auto replaceIn = [](GuestMapNode* parent, GuestMapNode* old, GuestMapNode* with) {
        if (parent->left == A(old))
            parent->left = A(with);
        else
            parent->right = A(with);
    };

    GuestMapNode* y = z;
    GuestMapNode* x;
    GuestMapNode* xParent;
    if (!y->left) {
        x = N(y->right);
    } else if (!y->right) {
        x = N(y->left);
    } else {
        y = N(y->right);
        while (y->left)
            y = N(y->left);
        x = N(y->right);
    }
    bool removedRed;
    if (y != z) {
        // z has two children: its successor y takes its place.
        removedRed = IsRed(y);
        setParent(N(z->left), y);
        y->left = z->left;
        if (y != N(z->right)) {
            xParent = Parent(y);
            if (x)
                setParent(x, xParent);
            xParent->left = A(x);
            y->right = z->right;
            setParent(N(z->right), y);
        } else {
            xParent = y;
        }
        replaceIn(Parent(z), z, y);
        setRed(y, IsRed(z));
        setParent(y, Parent(z));
    } else {
        removedRed = IsRed(y);
        xParent = Parent(y);
        if (x)
            setParent(x, xParent);
        replaceIn(xParent, z, x);
    }

    if (!removedRed) {
        GuestMapNode* root = N(end->left);
        while (x != N(end->left) && (!x || !IsRed(x))) {
            if (x == N(xParent->left)) {
                GuestMapNode* w = N(xParent->right);
                if (IsRed(w)) {
                    setRed(w, false);
                    setRed(xParent, true);
                    RotateLeft(xParent, &root);
                    w = N(xParent->right);
                }
                const bool leftBlack = !w->left || !IsRed(N(w->left));
                const bool rightBlack = !w->right || !IsRed(N(w->right));
                if (leftBlack && rightBlack) {
                    setRed(w, true);
                    x = xParent;
                    xParent = Parent(xParent);
                } else {
                    if (rightBlack) {
                        if (w->left)
                            setRed(N(w->left), false);
                        setRed(w, true);
                        RotateRight(w, &root);
                        w = N(xParent->right);
                    }
                    setRed(w, IsRed(xParent));
                    setRed(xParent, false);
                    if (w->right)
                        setRed(N(w->right), false);
                    RotateLeft(xParent, &root);
                    break;
                }
            } else {
                GuestMapNode* w = N(xParent->left);
                if (IsRed(w)) {
                    setRed(w, false);
                    setRed(xParent, true);
                    RotateRight(xParent, &root);
                    w = N(xParent->left);
                }
                const bool leftBlack = !w->left || !IsRed(N(w->left));
                const bool rightBlack = !w->right || !IsRed(N(w->right));
                if (leftBlack && rightBlack) {
                    setRed(w, true);
                    x = xParent;
                    xParent = Parent(xParent);
                } else {
                    if (leftBlack) {
                        if (w->right)
                            setRed(N(w->right), false);
                        setRed(w, true);
                        RotateLeft(w, &root);
                        w = N(xParent->left);
                    }
                    setRed(w, IsRed(xParent));
                    setRed(xParent, false);
                    if (w->left)
                        setRed(N(w->left), false);
                    RotateRight(xParent, &root);
                    break;
                }
            }
        }
        if (x)
            setRed(x, false);
    }
    MemFree(z);
    map->count--;
    if (map->count == 0)
        map->leftmost = A(end);
}

static void FreeSubtree(GuestMapNode* n) {      // 0x1941D0
    if (!n)
        return;
    FreeSubtree(N(n->left));
    FreeSubtree(N(n->right));
    MemFree(n);
}

// ── 0x194440 ──────────────────────────────────────────────────────────────
void GuestIntMap_Clear(GuestIntMap* map) {
    if (map->root == 0)
        return;
    FreeSubtree(N(map->root));
    map->count = 0;
    map->root = 0;
    map->leftmost = guest::addr(GuestIntMap_End(map));
}
