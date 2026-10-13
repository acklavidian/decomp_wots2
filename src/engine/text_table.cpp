// Menu text table and 16-bit string helpers — decompiled from 0x16E120,
// 0x16E260, 0x176AF0, 0x176C70 and 0x176D90.

#include "engine/font.h"
#include "engine/archive_file.h"
#include "native/guest.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

extern "C" {
    void* MemAllocAligned(u32 align, u32 size);   // 0x1228C8  libc memalign
    void  MemFreeAligned(void* p);                // 0x122B50  libc free
}

// 0x14 bytes in the original: the file being read, then a std::map from line
// number to string.  The map is a native one here, so the object lives outside
// guest memory; nothing reaches into it except through these functions.
struct TextTable {
    ArchiveFile* file;
    std::map<s32, const u16*> strings;
};

// The original keeps the singleton at *0x2EE5D0.
static TextTable* sTextTable = nullptr;

enum { kAssetMenuText = 0x46 };   // menu/menutext.txt

// ── 0x16E260 ──────────────────────────────────────────────────────────────
int WStr_Length(const u16* s) {
    int n = 0;
    while (*s != 0) {
        n++;
        s++;
        if (n > 0x3FF)
            return -1;
    }
    return n;
}

// ── 0x16E120 ──────────────────────────────────────────────────────────────
int WStr_Copy(u16* dst, const u16* src) {
    while (*src != 0)
        *dst++ = *src++;
    *dst = 0;
    return 0;
}

// ── 0x16E160 ──────────────────────────────────────────────────────────────
// Appends if the result fits in `maxLength` characters; −1 otherwise.
int WStr_Append(u16* dst, const u16* src, s16 maxLength) {
    int length = WStr_Length(dst), added = WStr_Length(src);
    if (length < 1 || added < 1 || length + added > maxLength)
        return -1;
    memcpy(dst + length, src, (size_t)added * 2);
    dst[length + added] = 0;
    return 0;
}

// ── 0x16E400 ──────────────────────────────────────────────────────────────
// Used for numbers: the formatted text is converted character by character
// through a 14-entry table, and anything that is not a digit or one of
// "+-,." is dropped.
int WStr_Format(u16* dst, u16 maxLength, const char* format, ...) {
    static const u16* const kWide = guest::ptr<const u16>(0x2D2090u);   // "0123456789+-,．"
    static const char* const kExtra = guest::ptr<const char>(0x2D20BAu); // "+-,."

    if ((s16)maxLength >= 0x400)
        return -1;
    char buffer[0x400];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (strlen(buffer) >= maxLength)
        return -1;

    int length = 0;
    for (const char* p = buffer; *p != 0; p++) {
        u8 c = (u8)*p;
        int index;
        if (c >= '0' && c <= '9') {
            index = c - '0';
        } else {
            index = -1;
            for (int i = 0; i < 4; i++)
                if (c == (u8)kExtra[i]) { index = 10 + i; break; }
        }
        if (index >= 0) {
            *dst++ = kWide[index];
            length++;
        }
    }
    *dst = 0;
    return length;
}

// ── 0x176D90 ──────────────────────────────────────────────────────────────
TextTable* TextTable_Get() {
    if (!sTextTable) {
        sTextTable = new TextTable;
        sTextTable->file = nullptr;
    }
    return sTextTable;
}

// The game inlines std::map::find at every use (helper at 0x170B90) and takes
// a missing id as a null string.
const u16* TextTable_Find(TextTable* table, int id) {
    auto it = table->strings.find(id);
    return it == table->strings.end() ? nullptr : it->second;
}

// ── 0x176AF0 ──────────────────────────────────────────────────────────────
// `text` is UTF-16 with a byte-order mark; lines end with CR (or CR LF).  The
// two characters `\n` inside a line are turned into a real line break.
void TextTable_Parse(TextTable* table, u16* text, int bytes) {
    if (!text || text[0] != 0xFEFF)
        return;

    u16* p = text + 1;
    u16* lineStart = p;
    int line = 0;
    int lineBytes = 0;
    for (int offset = 0; offset < bytes - 2;) {
        if (*p == '\r') {
            u16* next = p + 1;
            offset += 2;
            u16* copy = static_cast<u16*>(MemAllocAligned(4, lineBytes + 2));
            memset(copy, 0, lineBytes + 2);
            memcpy(copy, lineStart, lineBytes);
            table->strings.insert({line, copy});
            line++;
            if (*next == '\n') {
                next = p + 2;
                offset += 2;
            }
            lineBytes = 0;
            lineStart = next;
            p = next;
        } else if (p[0] == '\\' && p[1] == 'n') {
            p[0] = '\r';
            p[1] = '\n';
            offset += 4;
            p += 2;
            lineBytes += 4;
        } else {
            offset += 2;
            p += 1;
            lineBytes += 2;
        }
    }
}

// ── 0x176C70 ──────────────────────────────────────────────────────────────
void TextTable_Load(TextTable* table) {
    table->file = ArchiveFile_Create();
    ArchiveFile_Open(table->file, kAssetMenuText);
    u16* buffer = static_cast<u16*>(MemAllocAligned(0x40, ArchiveFile_GetSize(table->file)));
    ArchiveFile_Read(table->file, buffer, ArchiveFile_GetSize(table->file));
    TextTable_Parse(table, buffer, (int)ArchiveFile_GetSize(table->file));
    MemFreeAligned(buffer);
    ArchiveFile_Close(table->file);
    if (table->file)
        ArchiveFile_Destroy(table->file);
    table->file = nullptr;
}
