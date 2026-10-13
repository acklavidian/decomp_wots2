// Screen parameter files — decompiled from 0x223560–0x2240F0.

#include "engine/param_file.h"
#include "native/guest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                                   // 0x100820
    void  MemFree(void* p);                                     // 0x100650
    void* AssetRef_Ctor(void* mem);                             // 0x174EA0
    void  AssetRef_Dtor(void* ref, int flag);                   // 0x174E30
    char  AssetQueue_Request(void* ref, int assetId);           // 0x174C90
    char  AssetQueue_RequestByName(void* ref, const char* name);// 0x174D50
    void* AssetRef_GetData(void* ref);                          // 0x174A00
    s32   AssetRef_GetSize(void* ref);                          // 0x1749F0
}

enum : u32 {
    kVtBufferReader = 0x2EC5C0u,
    kVtAssetBuffer  = 0x2EC5B0u,
};

// ── 0x223830 ──────────────────────────────────────────────────────────────
static void BufferReader_Set(BufferReader* r, void* data, s32 size) {
    if (data && size >= 0) {
        r->start = guest::addr(data);
        r->cur = guest::addr(data);
        r->size = size;
        r->remaining = (u32)size;
    }
}

// ── 0x2238B0 ──────────────────────────────────────────────────────────────
static void BufferReader_Ctor(BufferReader* r) {
    r->vtable = kVtBufferReader;
    r->start = 0;
    r->cur = 0;
    r->size = 0;
    r->remaining = 0;
}

// ── 0x224020 ──────────────────────────────────────────────────────────────
ParamFile* ParamFile_Ctor(ParamFile* self) {
    BufferReader_Ctor(&self->out);
    BufferReader_Ctor(&self->asset);          // 0x2237F0
    self->asset.vtable = kVtAssetBuffer;
    self->assetRef = 0;
    BufferReader_Ctor(&self->text);
    self->values.capacity = 0;
    self->values.count = 0;
    self->values.data = 0;
    return self;
}

// ── 0x223F70 ──────────────────────────────────────────────────────────────
ParamFile* ParamFile_Dtor(ParamFile* self, s16 flag) {
    if (self) {
        if (self->values.data)
            MemFree(guest::ptr(self->values.data));
        // 0x223750: the asset buffer releases its reference
        self->asset.start = self->asset.cur = 0;
        self->asset.size = 0;
        self->asset.remaining = 0;
        if (self->assetRef)
            AssetRef_Dtor(guest::ptr(self->assetRef), 1);
        self->assetRef = 0;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x2236B0 / 0x223610: drop any previous file and ask for a new one.
static void* NewAssetRef(ParamFile* self) {
    self->asset.start = self->asset.cur = 0;
    self->asset.size = 0;
    self->asset.remaining = 0;
    if (self->assetRef)
        AssetRef_Dtor(guest::ptr(self->assetRef), 1);
    self->assetRef = 0;
    void* mem = MemAlloc(0xC);
    self->assetRef = mem ? guest::addr(AssetRef_Ctor(mem)) : 0;
    return guest::ptr(self->assetRef);
}

// ── 0x223F20 ──────────────────────────────────────────────────────────────
bool ParamFile_Request(ParamFile* self, int assetId) {
    void* ref = NewAssetRef(self);
    return ref && AssetQueue_Request(ref, assetId) != 0;
}

// ── 0x223ED0 ──────────────────────────────────────────────────────────────
bool ParamFile_RequestByName(ParamFile* self, const char* name) {
    void* ref = NewAssetRef(self);
    return ref && AssetQueue_RequestByName(ref, name) != 0;
}

// ── 0x2238E0 ──────────────────────────────────────────────────────────────
// Skips white space and comments; returns the next character, or −1 at the
// end.  A line comment only ends at a CR LF pair.
static int SkipBlanks(ParamFile* self) {
    BufferReader* t = &self->text;
    bool blockComment = false, lineComment = false;
    for (;;) {
        const char* p = guest::ptr<const char>(t->cur);
        int c = (s32)t->remaining > 0 ? p[0] : -1;
        if (c == -1)
            return -1;
        int next = t->remaining == 0 ? 0 : p[1];

        if (c == '\r' && next == '\n') {
            lineComment = false;
            t->cur += 2;
            t->remaining -= 2;
            continue;
        }
        if (!lineComment) {
            if (blockComment) {
                if (c == '*' && next == '/')
                    blockComment = false;
                t->cur += 1;
                t->remaining -= 1;
                continue;
            }
            if (c == '/' && next == '*') {
                blockComment = true;
                continue;
            }
            if (c == '/' && next == '/') {
                lineComment = true;
                continue;
            }
            if (!(c == ' ' || (c >= '\t' && c <= '\r')))
                return c;
        }
        t->cur += 1;
        t->remaining -= 1;
    }
}

// ── 0x223AD0 ──────────────────────────────────────────────────────────────
// "Color[r,g,b,a]"
static void ParseColor(ParamFile* self, char* token) {
    char prefix[8];
    strncpy(prefix, token, 6);
    prefix[6] = 0;
    if (strcmp(prefix, "Color[") != 0)
        return;

    static const char kSeparators[4] = {',', ',', ',', ']'};
    char* p = token + 6;
    for (int i = 0; i < 4; i++) {
        char* q = strchr(p, kSeparators[i]);
        if (!q || q - p < 1)
            return;
        *q = ' ';
        p = q + 1;
    }
    int r = 0, g = 0, b = 0, a = 0;
    sscanf(token + 6, "%d %d %d %d", &r, &g, &b, &a);
    VectorU32_PushBack(&self->values,
                       (u32)(r & 0xFF) | (u32)(g & 0xFF) << 8 | (u32)(b & 0xFF) << 16 | (u32)a << 24);
}

// ── 0x223C00 ──────────────────────────────────────────────────────────────
static void ParseText(ParamFile* self) {
    self->values.count = 0;
    while (SkipBlanks(self) != -1) {
        char token[0x90];
        token[0] = 0;
        sscanf(guest::ptr<const char>(self->text.cur), "%127s", token);
        u32 length = (u32)strlen(token);
        self->text.cur += length;
        self->text.remaining -= length;

        char c = token[0];
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (alpha || c == '_') {
            if (strcmp(token, "BLEND_ON") == 0)
                VectorU32_PushBack(&self->values, 1);
            else if (strcmp(token, "BLEND_OFF") == 0)
                VectorU32_PushBack(&self->values, 0);
            else
                ParseColor(self, token);
        } else if (digit || c == '+' || c == '-') {
            if (strchr(token, '.') == nullptr) {
                VectorU32_PushBack(&self->values, (u32)atoi(token));       // 0x2240B0
            } else {
                float value = (float)atof(token);                           // 0x224070
                u32 bits;
                memcpy(&bits, &value, 4);
                VectorU32_PushBack(&self->values, bits);
            }
        }
    }
    BufferReader_Set(&self->out, guest::ptr(self->values.data), (s32)(self->values.count << 2));
}

// ── 0x223DB0 ──────────────────────────────────────────────────────────────
int ParamFile_Poll(ParamFile* self) {
    // 0x223560: pick the data up once the asset has arrived
    if (self->assetRef != 0 && self->asset.start == 0) {
        void* data = AssetRef_GetData(guest::ptr(self->assetRef));
        if (!data)
            return 1;
        BufferReader_Set(&self->asset, data, AssetRef_GetSize(guest::ptr(self->assetRef)));
    }

    int first = (s32)self->asset.remaining > 0 ? *guest::ptr<const char>(self->asset.cur) : -1;
    if (first == 'T') {
        BufferReader_Set(&self->text, guest::ptr(self->asset.cur), (s32)self->asset.remaining);
        self->text.cur += 1;
        self->text.remaining -= 1;
        ParseText(self);
    } else {
        BufferReader_Set(&self->out, guest::ptr(self->asset.cur), (s32)self->asset.remaining);
    }
    return 0;
}

// ── 0x223E70 ──────────────────────────────────────────────────────────────
// The same from data already in memory (part of a pack).
void ParamFile_SetData(ParamFile* self, const char* data, int size) {
    if (*data == 'T') {
        BufferReader_Set(&self->text, const_cast<char*>(data) + 1, size - 1);
        ParseText(self);
    } else {
        BufferReader_Set(&self->out, const_cast<char*>(data), size);
    }
}

// ══ AssetBuffer on its own ════════════════════════════════════════════════
// 0x2237F0
AssetBuffer* AssetBuffer_Ctor(AssetBuffer* self) {
    BufferReader_Ctor(&self->reader);
    self->reader.vtable = kVtAssetBuffer;
    self->assetRef = 0;
    return self;
}

// 0x223750
AssetBuffer* AssetBuffer_Dtor(AssetBuffer* self, s16 flag) {
    if (self) {
        self->reader.start = self->reader.cur = 0;
        self->reader.size = 0;
        self->reader.remaining = 0;
        if (self->assetRef)
            AssetRef_Dtor(guest::ptr(self->assetRef), 1);
        self->assetRef = 0;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// 0x2236B0
bool AssetBuffer_Request(AssetBuffer* self, int assetId) {
    self->reader.start = self->reader.cur = 0;
    self->reader.size = 0;
    self->reader.remaining = 0;
    if (self->assetRef)
        AssetRef_Dtor(guest::ptr(self->assetRef), 1);
    self->assetRef = 0;
    void* mem = MemAlloc(0xC);
    self->assetRef = mem ? guest::addr(AssetRef_Ctor(mem)) : 0;
    return self->assetRef && AssetQueue_Request(guest::ptr(self->assetRef), assetId) != 0;
}

// 0x223560
int AssetBuffer_Poll(AssetBuffer* self) {
    if (self->assetRef != 0 && self->reader.start == 0) {
        void* data = AssetRef_GetData(guest::ptr(self->assetRef));
        if (!data)
            return 1;
        BufferReader_Set(&self->reader, data, AssetRef_GetSize(guest::ptr(self->assetRef)));
    }
    return 0;
}

// 0x2234D0
int WText_FindBlankLine(const BufferReader* r) {
    if (r->remaining <= 1)
        return -1;
    const u16* p = guest::ptr<const u16>(r->cur);
    const int chars = (int)r->remaining >> 1;
    for (int i = 0; i < chars - 3; i++)
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n')
            return i + 2;
    return chars;
}

// 0x223450
int WText_CountLineBreaks(const BufferReader* r, int chars) {
    if ((u32)(chars << 1) > r->remaining)
        return 0;
    const u16* p = guest::ptr<const u16>(r->cur);
    int count = 0;
    for (int i = 0; i < chars - 1; i++)
        if (p[i] == '\r' && p[i + 1] == '\n')
            count++;
    return count;
}
