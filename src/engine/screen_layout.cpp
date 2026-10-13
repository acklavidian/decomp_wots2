// Screen layouts — decompiled from 0x1678B0–0x168B50 and 0x13BD90–0x13C310.

#include "engine/screen_layout.h"
#include "engine/archive_file.h"
#include "engine/gs_device.h"
#include "native/guest.h"

#include <cstring>
#include <initializer_list>

struct SpriteSet;
extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    SpriteSet* SpriteSet_Ctor(void* mem);                       // 0x17C630
    char  SpriteSet_LoadByName(SpriteSet* set, const char* name, const char* name2);  // 0x17C2F0
    int   SpriteSet_Bind(SpriteSet* set);                       // 0x17BFD0
    void  SpriteSet_Dtor(SpriteSet* set, int flag);             // 0x17C570
    void  GsDevice_SetBlend(GsDevice* dev, u64 alphaReg);       // 0x130C50
}

static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};

enum { kAssetLayoutList = 0x2C };   // layout/list

// GS ALPHA register values; the top byte is the fixed alpha.
static const u64 kBlendNormal      = 0x8000000044ull;
static const u64 kBlendAdditive    = 0x8000000048ull;
static const u64 kBlendSubtractive = 0x42ull;

// ── Layout list (singleton at *0x2EE5B8) ──────────────────────────────────
// The original holds a std::vector of pairs of std::string; here the names
// are plain C strings in guest memory.
struct LayoutNames { u32 lif; u32 pim; };
static u32& sLayoutList = *guest::ptr<u32>(0x2EE5B8u);

// ── 0x168B50 ──────────────────────────────────────────────────────────────
// File: u32 count, then per layout two length-prefixed names (no terminator).
static void LayoutList_Load(GuestVector* list) {
    ArchiveFile* file = ArchiveFile_Create();
    ArchiveFile_Open(file, kAssetLayoutList);
    u32 size = ArchiveFile_GetSize(file);
    if (size != 0) {
        u8* buffer = static_cast<u8*>(MemAlloc(size));
        ArchiveFile_Read(file, buffer, size);
        u32 count;
        memcpy(&count, buffer, 4);
        const u8* p = buffer + 4;
        for (u32 i = 0; i < count; i++) {
            LayoutNames names;
            for (u32* out : {&names.lif, &names.pim}) {
                u32 word;
                memcpy(&word, p, 4);
                s8 length = (s8)word;
                char* name = static_cast<char*>(MemAlloc((u32)length + 1));
                memcpy(name, p + 4, (size_t)length);
                name[length] = 0;
                *out = guest::addr(name);
                p += 4 + length;
            }
            GuestVector_PushBack(list, &names, sizeof(names));
        }
        MemFree(buffer);
    }
    ArchiveFile_Close(file);
    ArchiveFile_Destroy(file);
}

static GuestVector* LayoutList_Get() {
    if (!sLayoutList) {
        GuestVector* list = static_cast<GuestVector*>(MemAlloc(sizeof(GuestVector)));
        if (list) {
            list->capacity = 0;
            list->count = 0;
            list->data = 0;
            LayoutList_Load(list);
        }
        sLayoutList = guest::addr(list);
    }
    return guest::ptr<GuestVector>(sLayoutList);
}

static const LayoutNames* LayoutList_Find(GuestVector* list, u32 id) {
    return id < list->count ? guest::ptr<LayoutNames>(list->data) + id : nullptr;
}

// ── 0x13C310 / 0x13BE70 ───────────────────────────────────────────────────
static LayoutData* LayoutData_Parse(LayoutData* data, const u8* file) {
    data->anims.capacity = 0;
    data->anims.count = 0;
    data->anims.data = 0;

    memcpy(&data->itemCount, file, 4);
    const u8* p = file + 4;
    void* items = MemAlloc(data->itemCount * sizeof(LayoutItem));
    data->items = guest::addr(items);
    memcpy(items, p, data->itemCount * sizeof(LayoutItem));
    p += data->itemCount * sizeof(LayoutItem);

    s32 animCount;
    memcpy(&animCount, p, 4);
    p += 4;
    for (s32 i = 0; i < animCount; i++) {
        LayoutAnim anim;
        anim.frame = 0;
        anim._10 = 0;
        memcpy(&anim.duration, p, 4);
        memcpy(&anim.frameCount, p + 4, 4);
        p += 8;
        void* frames = MemAlloc((u32)anim.frameCount * sizeof(LayoutFrame));
        memcpy(frames, p, (size_t)anim.frameCount * sizeof(LayoutFrame));
        p += (size_t)anim.frameCount * sizeof(LayoutFrame);
        anim.frames = guest::addr(frames);
        GuestVector_PushBack(&data->anims, &anim, sizeof(anim));
    }
    return data;
}

// ── 0x13C1F0 ──────────────────────────────────────────────────────────────
static void LayoutData_Dtor(LayoutData* data) {
    if (!data)
        return;
    if (data->items)
        MemFree(guest::ptr(data->items));
    LayoutAnim* anims = guest::ptr<LayoutAnim>(data->anims.data);
    for (u32 i = 0; i < data->anims.count; i++)
        MemFree(guest::ptr(anims[i].frames));
    if (data->anims.data)
        MemFree(anims);
    MemFree(data);
}

// ── 0x13BE20 ──────────────────────────────────────────────────────────────
static LayoutAnim* LayoutData_GetAnim(LayoutData* data, u32 index) {
    if (data->anims.count == 0 || index >= data->anims.count)
        return nullptr;
    return guest::ptr<LayoutAnim>(data->anims.data) + index;
}

// ── 0x168B10 ──────────────────────────────────────────────────────────────
ScreenLayout* ScreenLayout_Ctor(ScreenLayout* self) {
    self->_18 = 0;
    self->visibleCount = 0;
    self->visibleBits = 0;
    self->id = -1;
    self->data = 0;
    self->_08 = 0;
    self->ready = 0;
    self->file = 0;
    self->fileBuffer = 0;
    self->offsets = 0;
    self->spriteSet = 0;
    return self;
}

// ── 0x168A00 ──────────────────────────────────────────────────────────────
void ScreenLayout_Init(ScreenLayout*) {}

// ── 0x168A10 ──────────────────────────────────────────────────────────────
ScreenLayout* ScreenLayout_Dtor(ScreenLayout* self, s16 flag) {
    if (self) {
        if (self->data)
            LayoutData_Dtor(guest::ptr<LayoutData>(self->data));
        self->data = 0;
        if (self->file)
            ArchiveFile_Destroy(guest::ptr<ArchiveFile>(self->file));
        self->file = 0;
        if (self->fileBuffer)
            MemFree(guest::ptr(self->fileBuffer));
        self->fileBuffer = 0;
        if (self->offsets)
            MemFree(guest::ptr(self->offsets));
        self->offsets = 0;
        if (self->spriteSet)
            SpriteSet_Dtor(guest::ptr<SpriteSet>(self->spriteSet), 1);
        self->spriteSet = 0;
        if (self->visibleBits)
            MemFree(guest::ptr(self->visibleBits));
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// ── 0x1686A0 ──────────────────────────────────────────────────────────────
// Starts reading the .lif and requests the .pim.
static void ScreenLayout_StartLoad(ScreenLayout* self) {
    GuestVector* list = LayoutList_Get();
    if (self->ready == 1 || self->file != 0 || self->spriteSet != 0)
        return;
    const LayoutNames* names = LayoutList_Find(list, (u32)self->id);

    ArchiveFile* file = ArchiveFile_Create();
    if (file)
        ArchiveFile_OpenByName(file, names ? guest::ptr<const char>(names->lif) : nullptr);
    self->file = guest::addr(file);
    u32 size = ArchiveFile_GetSize(file);
    self->fileBuffer = guest::addr(MemAlloc(size));
    ArchiveFile_ReadAsync(file, guest::ptr(self->fileBuffer), size);

    void* mem = MemAlloc(0x14);
    self->spriteSet = mem ? guest::addr(SpriteSet_Ctor(mem)) : 0;
    SpriteSet_LoadByName(guest::ptr<SpriteSet>(self->spriteSet),
                         names ? guest::ptr<const char>(names->pim) : nullptr, nullptr);
}

// ── 0x168500 ──────────────────────────────────────────────────────────────
// First the .lif is parsed once its read completes; then the layout becomes
// ready when its textures are.
static void ScreenLayout_FinishLoad(ScreenLayout* self) {
    if (self->ready == 1)
        return;
    if (self->file == 0) {
        if (SpriteSet_Bind(guest::ptr<SpriteSet>(self->spriteSet)))
            self->ready = 1;
        return;
    }
    ArchiveFile* file = guest::ptr<ArchiveFile>(self->file);
    if (ArchiveFile_IsBusy(file))
        return;
    ArchiveFile_Close(file);
    ArchiveFile_Destroy(file);
    self->file = 0;

    LayoutData* data = static_cast<LayoutData*>(MemAlloc(sizeof(LayoutData)));
    if (data) {
        memset(data, 0, sizeof(LayoutData));
        LayoutData_Parse(data, guest::ptr<const u8>(self->fileBuffer));
    }
    self->data = guest::addr(data);
    if (self->fileBuffer)
        MemFree(guest::ptr(self->fileBuffer));
    self->fileBuffer = 0;

    // Every item starts visible, with no offset.
    u32 count = data->itemCount;
    u32 words = (count + 31) >> 5;
    u32* bits = static_cast<u32*>(MemAlloc(words * 4 + 4));
    for (u32 i = 0; i < count; i++)
        bits[i >> 5] |= 1u << (i & 31);
    self->visibleBits = guest::addr(bits);
    self->visibleCount = count;
    self->offsets = guest::addr(MemAlloc(count * 8 + 8));
    memset(guest::ptr(self->offsets), 0, count * 8);
}

// ── 0x1688A0 ──────────────────────────────────────────────────────────────
bool ScreenLayout_Poll(ScreenLayout* self) {
    if (self->ready != 1) {
        ScreenLayout_StartLoad(self);
        ScreenLayout_FinishLoad(self);
    }
    return self->ready == 1;
}

// ── 0x168180 ──────────────────────────────────────────────────────────────
void ScreenLayout_SetDepth(ScreenLayout* self, int z) {
    LayoutData* data = guest::ptr<LayoutData>(self->data);
    LayoutItem* items = guest::ptr<LayoutItem>(data->items);
    for (u32 i = 0; i < data->itemCount; i++)
        for (int k = 0; k < 4; k++)
            items[i].v[k].z = (u32)(int)(float)z;
}

// ── 0x168E80 / 0x168EB0 ───────────────────────────────────────────────────
static bool IsVisible(ScreenLayout* self, u32 index) {
    return (guest::ptr<u32>(self->visibleBits)[index >> 5] & (1u << (index & 31))) != 0;
}

static void SetVisible(ScreenLayout* self, u32 index, bool visible) {
    u32* word = &guest::ptr<u32>(self->visibleBits)[index >> 5];
    if (visible) *word |= 1u << (index & 31);
    else         *word &= ~(1u << (index & 31));
}

// ── 0x168190 ──────────────────────────────────────────────────────────────
void ScreenLayout_SetGroupVisible(ScreenLayout* self, int byGroupA, bool visible, int group) {
    LayoutData* data = guest::ptr<LayoutData>(self->data);
    const LayoutItem* items = guest::ptr<LayoutItem>(data->items);
    for (u32 i = 0; i < data->itemCount; i++) {
        if (i == self->visibleCount)
            return;
        if ((byGroupA ? items[i].groupA : items[i].groupB) == group)
            SetVisible(self, i, visible);
    }
}

void ScreenLayout_ShowGroupB(ScreenLayout* self, int group) { ScreenLayout_SetGroupVisible(self, 0, true, group); }
void ScreenLayout_HideGroupB(ScreenLayout* self, int group) { ScreenLayout_SetGroupVisible(self, 0, false, group); }
void ScreenLayout_ShowGroup(ScreenLayout* self, int group)  { ScreenLayout_SetGroupVisible(self, 1, true, group); }
void ScreenLayout_HideGroup(ScreenLayout* self, int group)  { ScreenLayout_SetGroupVisible(self, 1, false, group); }

// ── 0x13BCF0 / 0x13BC70 ───────────────────────────────────────────────────
static LayoutItem* FindItem(ScreenLayout* self, int group, int sub) {
    LayoutData* data = guest::ptr<LayoutData>(self->data);
    LayoutItem* items = guest::ptr<LayoutItem>(data->items);
    for (u32 i = 0; i < data->itemCount; i++)
        if (items[i].groupA == group && (sub == -1 || items[i].groupB == sub))
            return &items[i];
    return nullptr;
}

// ── 0x1682F0 ──────────────────────────────────────────────────────────────
void ScreenLayout_GetGroupOrigin(s32 out[2], ScreenLayout* self, int group, int sub) {
    const LayoutItem* item = FindItem(self, group, sub);
    out[0] = item ? item->v[0].x : 0;
    out[1] = item ? item->v[0].y : 0;
}

// ── 0x1683D0 ──────────────────────────────────────────────────────────────
void ScreenLayout_GetGroupCenter(s32 out[2], ScreenLayout* self, int group, int sub) {
    const LayoutItem* item = FindItem(self, group, sub);
    if (!item) {
        out[0] = out[1] = 0;
        return;
    }
    out[0] = (item->v[0].x + item->v[3].x) / 2;
    out[1] = (item->v[0].y + item->v[3].y) / 2;
}

// ── 0x168470 ──────────────────────────────────────────────────────────────
void ScreenLayout_GetGroupRect(s32 out[4], ScreenLayout* self, int group, int sub) {
    const LayoutItem* item = FindItem(self, group, sub);
    if (!item) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = item->v[0].x;
    out[1] = item->v[0].y;
    out[2] = item->v[3].x;
    out[3] = item->v[3].y;
}

// ── 0x167E90 ──────────────────────────────────────────────────────────────
void ScreenLayout_SetGroupOffset(ScreenLayout* self, const s32 offset[2], int group, int sub) {
    LayoutData* data = guest::ptr<LayoutData>(self->data);
    const LayoutItem* items = guest::ptr<LayoutItem>(data->items);
    for (u32 i = 0; i < data->itemCount; i++) {
        if (items[i].groupA == group && (sub == -1 || items[i].groupB == sub)) {
            s32* slot = guest::ptr<s32>(self->offsets) + i * 2;
            slot[0] = offset[0];
            slot[1] = offset[1];
            return;
        }
    }
}

// ── 0x167FC0 ──────────────────────────────────────────────────────────────
void ScreenLayout_SetGroupBOffset(ScreenLayout* self, const s32 offset[2], int groupB) {
    LayoutData* data = guest::ptr<LayoutData>(self->data);
    const LayoutItem* items = guest::ptr<LayoutItem>(data->items);
    for (u32 i = 0; i < data->itemCount; i++) {
        if (items[i].groupB == groupB) {
            s32* slot = guest::ptr<s32>(self->offsets) + i * 2;
            slot[0] = offset[0];
            slot[1] = offset[1];
        }
    }
}

// ── 0x1680E0 ──────────────────────────────────────────────────────────────
// The depth of every item of a group (ScreenLayout_SetDepth does all of them).
void ScreenLayout_SetGroupDepth(ScreenLayout* self, u32 z, int groupB) {
    LayoutData* data = guest::ptr<LayoutData>(self->data);
    LayoutItem* items = guest::ptr<LayoutItem>(data->items);
    for (u32 i = 0; i < data->itemCount; i++)
        if (items[i].groupB == groupB)
            for (LayoutVertex& v : items[i].v)
                v.z = z;
}

// ── 0x1678B0 ──────────────────────────────────────────────────────────────
// A line, triangle or quad with a colour per vertex.
static void DrawPolygon(ScreenLayout*, const LayoutItem* item, const s32* offset) {
    struct Packet {
        u64 prim;
        struct { u32 rgba; u32 q; u16 x, y; u32 z; } v[4];
    } packet;
    memset(&packet, 0, sizeof(packet));

    int count;
    switch (item->flags & 0xF) {
    case 0: packet.prim = 0x49; count = 2; break;   // line, Gouraud, blended
    case 1: packet.prim = 0x4B; count = 3; break;   // triangle
    case 2: packet.prim = 0x4C; count = 4; break;   // triangle strip
    default: return;
    }
    for (int i = 0; i < count; i++) {
        const LayoutVertex& v = item->v[i];
        packet.v[i].x = (u16)Screen_XAligned((v.x + offset[0]) * 16);
        // (the original runs y through the x conversion as well; the two
        // only differ when the alignment globals do)
        packet.v[i].y = (u16)Screen_XAligned((v.y + offset[1]) * -16);
        packet.v[i].z = v.z;
        packet.v[i].rgba = v.rgba;
    }
    GsDevice_DrawPrim(*gGsDevice, &packet, count);
}

// ── 0x167A10 ──────────────────────────────────────────────────────────────
// A textured sprite; its animation picks which texture and palette to use.
static void DrawSprite(ScreenLayout* self, const LayoutItem* item, const s32* offset) {
    if ((item->flags & 0xF) != 3)
        return;

    LayoutAnim* anim = LayoutData_GetAnim(guest::ptr<LayoutData>(self->data), item->anim);
    s32 texture = 0, clut = 0;
    const LayoutFrame* frames = guest::ptr<LayoutFrame>(anim->frames);
    for (s32 i = 0; i < anim->frameCount; i++) {
        if (frames[i].start <= anim->frame) {
            clut = frames[i].clut;
            texture = frames[i].texture;
            break;
        }
    }
    if (++anim->frame >= anim->duration)
        anim->frame = 0;

    // Texture descriptor `texture` of the layout's .pim (set + 8).
    const u8* desc = guest::ptr<const u8>(guest::ptr<u32>(self->spriteSet)[2]) + texture * 0x30;
    u16 tbp, tbw, log2w, log2h, cbp;
    memcpy(&tbp, desc + 4, 2);
    memcpy(&tbw, desc + 6, 2);
    memcpy(&log2w, desc + 8, 2);
    memcpy(&log2h, desc + 10, 2);
    memcpy(&cbp, desc + 0x1E, 2);

    GsSpritePacket p;
    memset(&p, 0, sizeof(p));
    p.prim = 0x156;
    p.tex0 = (u64)tbp | (u64)tbw << 14 | (u64)desc[2] << 20 | (u64)log2w << 26 | (u64)log2h << 30
           | 0x400000000ull | (u64)(u32)(cbp + clut) << 37 | 0x2000000000000000ull;

    const LayoutVertex& a = item->v[0];
    const LayoutVertex& b = item->v[3];
    p.corner[0].x = (u16)Screen_XAligned((a.x + offset[0]) * 16);
    p.corner[0].y = (u16)Screen_YAligned((a.y + offset[1]) * -16);
    p.corner[0].z = a.z;
    p.corner[0].u = (s16)(a.u + 8);
    p.corner[0].v = (s16)(a.v + 8);
    p.corner[0].rgba = a.rgba;
    p.corner[1].x = (u16)Screen_XAligned((b.x + offset[0]) * 16);
    p.corner[1].y = (u16)Screen_YAligned((b.y + offset[1]) * -16);
    p.corner[1].z = b.z;
    p.corner[1].u = (s16)(b.u + 8);
    p.corner[1].v = (s16)(b.v + 8);
    p.corner[1].rgba = a.rgba;
    GsDevice_DrawSprite(*gGsDevice, &p);
}

// ── 0x167C60 ──────────────────────────────────────────────────────────────
void ScreenLayout_Draw(ScreenLayout* self) {
    if (self->ready != 1 || !SpriteSet_Bind(guest::ptr<SpriteSet>(self->spriteSet)))
        return;

    GsDevice* dev = *gGsDevice;
    LayoutData* data = guest::ptr<LayoutData>(self->data);
    GsDevice_Begin2D(dev);
    self->blend = 0;
    GsDevice_SetBlend(dev, kBlendNormal);

    for (u32 i = 0; i < data->itemCount; i++) {
        if (i == self->visibleCount)
            break;
        if (!IsVisible(self, i))
            continue;
        LayoutItem item = guest::ptr<LayoutItem>(data->items)[i];   // 0x13BD90 copies it out

        u32 blend = item.flags & 0xF0;
        if (self->blend != blend) {
            self->blend = blend;
            if (blend == 0)         GsDevice_SetBlend(dev, kBlendNormal);
            else if (blend == 0x20) GsDevice_SetBlend(dev, kBlendSubtractive);
            else if (blend == 0x10) GsDevice_SetBlend(dev, kBlendAdditive);
        }
        const s32* offset = guest::ptr<s32>(self->offsets) + i * 2;
        if ((item.flags & 0xF) == 3)
            DrawSprite(self, &item, offset);
        else
            DrawPolygon(self, &item, offset);
    }

    GsDevice_End2D(dev);
    self->blend = 0;
    GsDevice_SetBlend(dev, kBlendNormal);
}

// ── 0x13BCF0: the first item of a group, for screens that draw the layout's
// pieces themselves ───────────────────────────────────────────────────────
const LayoutItem* ScreenLayout_FindItem(ScreenLayout* self, int group) { return FindItem(self, group, -1); }

// ── 0x167A10 (public) ─────────────────────────────────────────────────────
void ScreenLayout_DrawSprite(ScreenLayout* self, const LayoutItem* item, const s32 offset[2]) {
    DrawSprite(self, item, offset);
}

// ── 0x167660: an item's sprite with its first corner at a place (layout
// coordinates) instead of where the layout has it ────────────────────────
void ScreenLayout_DrawSpriteAt(ScreenLayout* self, const LayoutItem* item, const s32 at[2]) {
    const s32 offset[2] = {at[0] - item->v[0].x, at[1] - item->v[0].y};
    DrawSprite(self, item, offset);
}

// ── 0x167500: a flat rectangle the size of an item, at a place ────────────
void ScreenLayout_FillItem(ScreenLayout*, const LayoutItem* item, const s32 at[2], u32 rgba) {
    struct Packet {
        u64 prim;
        struct { u32 rgba; u32 q; u16 x, y; u32 z; } v[4];
    } packet;
    memset(&packet, 0, sizeof(packet));
    packet.prim = 0x4C;
    const s32 w = item->v[1].x - item->v[0].x, h = item->v[3].y - item->v[0].y;
    const s32 xs[4] = {at[0], at[0] + w, at[0], at[0] + w};
    const s32 ys[4] = {at[1], at[1], at[1] + h, at[1] + h};
    for (int i = 0; i < 4; i++) {
        packet.v[i].x = (u16)Screen_XAligned(xs[i] * 16);
        packet.v[i].y = (u16)Screen_XAligned(ys[i] * -16);   // (x conversion for y too, as in the original)
        packet.v[i].rgba = rgba;
    }
    GsDevice_DrawPrim(*gGsDevice, &packet, 4);
}
