// Character resource manager: the character list — decompiled from
// 0x1D4E00–0x1D4F00, 0x1D6500, 0x1D6630 and 0x1D68A0–0x1D6A70.

#include "game/chara_resources.h"
#include "native/guest.h"

#include <cstddef>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);       // 0x100820
    void  MemFree(void* p);         // 0x100650
}

// 0x1D6A00 / 0x1D6990 / 0x1D6920 / 0x1D68B0: copy a counted string into a
// new, terminated buffer; an empty one leaves the field alone.
static void StoreString(u32* field, const u8* text, s32 length) {
    if (length == 0)
        return;
    char* copy = static_cast<char*>(MemAlloc((u32)length + 1));
    memcpy(copy, text, (size_t)length);
    copy[length] = 0;
    *field = guest::addr(copy);
}

// ── 0x1D6500 ──────────────────────────────────────────────────────────────
void CharaResources_FreeList(CharaResources* self) {
    GuestListNode* node = GuestList_Begin(&self->charas);
    while (node != GuestList_End(&self->charas)) {
        CharaInfo* info = guest::ptr<CharaInfo>(node->value);
        if (info) {
            for (int i = 0; i < 3; i++) {
                if (info->bodyPath[i]) MemFree(guest::ptr(info->bodyPath[i]));
                if (info->headPath[i]) MemFree(guest::ptr(info->headPath[i]));
            }
            if (info->name)        MemFree(guest::ptr(info->name));
            if (info->texturePath) MemFree(guest::ptr(info->texturePath));
            MemFree(info);
        }
        node = GuestList_Erase(&self->charas, node);
    }
}

// ── 0x1D6630 ──────────────────────────────────────────────────────────────
// File: u16 count, u16 version, then per character
//   u32 id, name, 3 body paths, 3 head paths, [version > 1: texture path,]
//   u32 value, [version > 2: two floats]
// where every string is a u32 length followed by that many characters.
void CharaResources_ParseList(CharaResources* self, const u32* file) {
    CharaResources_FreeList(self);
    const u32 count = file[0] & 0xFFFF, version = file[0] >> 16;
    const u8* p = reinterpret_cast<const u8*>(file + 1);

    for (u32 i = 0; i < count; i++) {
        CharaInfo* info = static_cast<CharaInfo*>(MemAlloc(sizeof(CharaInfo)));
        if (info) {
            memset(info->bodyPath, 0, sizeof(info->bodyPath));
            memset(info->headPath, 0, sizeof(info->headPath));
            info->name = 0;
            info->texturePath = 0;
            info->pack = -1;
            info->id = -1;
            info->param[0] = 0;
            info->param[1] = 0;
        }
        if (version != 0) {
            s32 length;
            info->id = (s8)p[0];                                // 0x1D6A70
            memcpy(&length, p + 4, 4);
            StoreString(&info->name, p + 8, length);
            p += 8 + length;
            memcpy(&length, p, 4);
            p += 4;
            for (int k = 0; k < 3; k++) {
                StoreString(&info->bodyPath[k], p, length);
                p += length;
                memcpy(&length, p, 4);
                p += 4;
            }
            for (int k = 0; k < 3; k++) {
                StoreString(&info->headPath[k], p, length);
                p += length;
                memcpy(&length, p, 4);
                p += 4;
            }
            if (version > 1) {
                StoreString(&info->texturePath, p, length);
                p += length;
                memcpy(&length, p, 4);
                p += 4;
            }
            info->pack = length;                                // 0x1D68A0
            if (version > 2) {
                memcpy(&info->param[0], p, 4);
                memcpy(&info->param[1], p + 4, 4);
                p += 8;
            }
        }
        GuestList_Insert(&self->charas, GuestList_End(&self->charas), guest::addr(info));
    }
}

// The search every getter below repeats inline.
const CharaInfo* CharaResources_Find(CharaResources* self, int id) {
    for (GuestListNode* n = GuestList_Begin(&self->charas); n != GuestList_End(&self->charas);
         n = GuestList_Next(n)) {
        const CharaInfo* info = guest::ptr<CharaInfo>(n->value);
        if (info->id == id)
            return info;
    }
    return nullptr;
}

// ── 0x1D4F00 ──────────────────────────────────────────────────────────────
const char* CharaResources_GetTexturePath(CharaResources* self, int id) {
    const CharaInfo* info = CharaResources_Find(self, id);
    return info ? guest::ptr<const char>(info->texturePath) : nullptr;
}

// ── 0x1D4EB0 ──────────────────────────────────────────────────────────────
const char* CharaResources_GetBodyPath(CharaResources* self, int id, int lod) {
    const CharaInfo* info = CharaResources_Find(self, id);
    return info ? guest::ptr<const char>(info->bodyPath[lod]) : nullptr;
}

// ── 0x1D4E60 ──────────────────────────────────────────────────────────────
const char* CharaResources_GetHeadPath(CharaResources* self, int id, int lod) {
    const CharaInfo* info = CharaResources_Find(self, id);
    return info ? guest::ptr<const char>(info->headPath[lod]) : nullptr;
}

// ── 0x1D4E00 ──────────────────────────────────────────────────────────────
float CharaResources_GetParam(CharaResources* self, int id, int which) {
    const CharaInfo* info = CharaResources_Find(self, id);
    return info ? (float)(int)info->param[which] : 0.0f;
}

// ══ Parameter templates (chara/paramlist) ═════════════════════════════════
extern "C" void Matrix_Identity(void* m);       // 0x107428

static_assert(sizeof(CharaParams) == 0xE0 && offsetof(CharaParams, name) == 0xA0, "CharaParams");
static_assert(offsetof(CharaParams, hp) == 0x40 && offsetof(CharaParams, matrix) == 0x50, "CharaParams");
static_assert(offsetof(CharaResources, templates) == 0x50 && sizeof(CharaResources) == 0xC4, "CharaResources");

// ── 0x1C8AA0 ──────────────────────────────────────────────────────────────
CharaParamsRef* CharaParams_New(CharaParamsRef* ref, int id) {
    CharaParams* p = static_cast<CharaParams*>(MemAlloc(sizeof(CharaParams)));
    if (p) {
        p->nameVtable = 0x2EAF30u;                  // 0x194900: an empty string
        WString_Ctor(&p->name);
        WString_Resize(&p->name, 1);
        *WString_At(&p->name, 0) = 0;
        p->id = 0;
        p->chara = 0;
        p->_08 = -1;
        p->role = 0;
        p->_20 = 0;
        p->_24 = 0;
        p->hp = 0;
        p->maxHp = 0;
        p->_48 = -1;
        p->sword = 0;
        p->faction = 0;
        p->_18 = 0;
        p->_28 = 0;
        p->_3c = 0;
        p->unique = 0;
        p->child = 0;
        p->_14 = 0;
        p->_b0 = 0;
        p->link[3] = 0;
        p->_d8[0] = 0;
        p->link[0] = p->link[1] = p->link[2] = 0;   // 0x1C8B90
        p->link[4] = p->link[5] = p->link[6] = p->link[7] = p->link[8] = 0;
        Matrix_Identity(p->matrix);
        p->scale[0] = p->scale[1] = p->scale[2] = p->scale[3] = 1.0f;
        p->stat[0] = p->stat[1] = p->stat[2] = p->stat[3] = 0;
    }
    ref->params = guest::addr(p);
    p->id = id;
    return ref;
}

// ── 0x1C8990 ──────────────────────────────────────────────────────────────
CharaParamsRef* CharaParams_Delete(CharaParamsRef* ref, s16 flag) {
    if (!ref)
        return ref;
    if (CharaParams* p = guest::ptr<CharaParams>(ref->params)) {
        WString_Release(&p->name);
        MemFree(p);
    }
    ref->params = 0;
    if (flag > 0)
        MemFree(ref);
    return ref;
}

// ── 0x1C8670 ──────────────────────────────────────────────────────────────
// Entry: id, health, sword, faction, then by file version (9 on this disc)
//   1: model   2: ?   3: unique   4: role, ?   5: ?   6: child   7: ?
//   8: flag (−1 = clear)   9: ?
// and the name: a byte count and that many bytes of 16-bit characters.
const u8* CharaParams_Parse(CharaParamsRef* ref, const u8* data, u16 version) {
    CharaParams* p = guest::ptr<CharaParams>(ref->params);
    auto next = [&data]() { s32 v; memcpy(&v, data, 4); data += 4; return v; };
    p->id = next();
    p->maxHp = (s16)next();
    p->hp = p->maxHp;
    p->sword = (s16)next();
    p->faction = (s16)next();
    if (version > 0) p->chara = next();
    if (version > 1) p->_08 = next();
    if (version > 2) p->unique = next();
    if (version > 3) { p->role = next(); p->_20 = next(); }
    if (version > 4) p->_24 = next();
    if (version > 5) p->child = next();
    if (version > 6) p->_28 = next();
    if (version > 7 && next() != -1) p->_14 = 1;
    if (version > 8) p->_48 = (s16)next();
    const s32 bytes = next();
    const u16* text = reinterpret_cast<const u16*>(data);       // 0x1C8920
    WString_Assign(&p->name, text, text + bytes / 2);
    return data + bytes;
}

// ── 0x1D5280 ──────────────────────────────────────────────────────────────
void CharaResources_FreeParams(CharaResources* self) {
    GuestListNode* node = GuestList_Begin(&self->templates);
    while (node != GuestList_End(&self->templates)) {
        if (node->value)
            CharaParams_Delete(guest::ptr<CharaParamsRef>(node->value), 1);
        node->value = 0;
        node = GuestList_Erase(&self->templates, node);
    }
}

// ── 0x1D6400 ──────────────────────────────────────────────────────────────
// File: u16 count, u16 version, then the entries.
void CharaResources_ParseParams(CharaResources* self, const u32* file) {
    CharaResources_FreeParams(self);
    const u32 header = *file;
    const u8* data = reinterpret_cast<const u8*>(file + 1);
    const u32 count = header & 0xFFFF;
    for (u32 i = 0; i < count; i++) {
        void* mem = MemAlloc(sizeof(CharaParamsRef));
        CharaParamsRef* ref = mem ? CharaParams_New(static_cast<CharaParamsRef*>(mem), (int)i) : nullptr;
        data = CharaParams_Parse(ref, data, (u16)(header >> 16));
        GuestList_Insert(&self->templates, GuestList_End(&self->templates), guest::addr(ref));
    }
}

static inline CharaParams* TemplateOf(GuestListNode* node) {
    return guest::ptr<CharaParams>(guest::ptr<CharaParamsRef>(node->value)->params);
}

// ── 0x1D5230 ──────────────────────────────────────────────────────────────
CharaParams* CharaResources_FindParamTemplate(CharaResources* self, int id) {
    for (GuestListNode* n = GuestList_Begin(&self->templates); n != GuestList_End(&self->templates);
         n = GuestList_Next(n))
        if (TemplateOf(n)->id == id)
            return TemplateOf(n);
    return nullptr;
}

// ── 0x1D4F50 ──────────────────────────────────────────────────────────────
void CharaResources_ListByRole(CharaResources* self, int role, GuestList* out) {
    GuestList_Clear(out);
    for (GuestListNode* n = GuestList_Begin(&self->templates); n != GuestList_End(&self->templates);
         n = GuestList_Next(n))
        if (role == -1 || role == TemplateOf(n)->role)
            GuestList_Insert(out, GuestList_End(out), (u32)TemplateOf(n)->id);
}

// ── 0x1D5030 ──────────────────────────────────────────────────────────────
void CharaResources_ListByFaction(CharaResources* self, s16 faction, int role, GuestList* out) {
    GuestList_Clear(out);
    for (GuestListNode* n = GuestList_Begin(&self->templates); n != GuestList_End(&self->templates);
         n = GuestList_Next(n)) {
        const CharaParams* p = TemplateOf(n);
        if ((faction == -1 || faction == p->faction) && (role == -1 || role == p->role))
            GuestList_Insert(out, GuestList_End(out), (u32)p->id);
    }
}

// ── 0x1D6D10 ──────────────────────────────────────────────────────────────
// How much model memory an area's characters may use.
int CharaResources_MemoryBudget(CharaResources*) { return 0x320000; }

// ── 0x1D5330 ──────────────────────────────────────────────────────────────
// The live parameters of character `id` (0 = the player), made on first use.
CharaParams* CharaResources_GetParams(CharaResources* self, int id) {
    for (GuestListNode* n = GuestList_Begin(&self->params); n != GuestList_End(&self->params);
         n = GuestList_Next(n))
        if (TemplateOf(n)->id == id)
            return TemplateOf(n);
    void* mem = MemAlloc(sizeof(CharaParamsRef));
    CharaParamsRef* ref = mem ? CharaParams_New(static_cast<CharaParamsRef*>(mem), id) : nullptr;
    GuestList_Insert(&self->params, GuestList_End(&self->params), guest::addr(ref));
    return guest::ptr<CharaParams>(ref->params);
}
