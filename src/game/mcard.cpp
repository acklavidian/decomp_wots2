// The memory-card screens — decompiled from 0x25E540–0x2668F0 (the common
// part, the boot check and the save screen; the load screen, 0x257000–
// 0x25E520, is not done yet and is stood in for at the end of this file).
//
// One class, three faces.  The object (0x150E8 bytes and up) holds a whole
// copy of the save file:
//
//   +0x00010  u32 100, s32 the slot used last
//   +0x00018  three slots of 0x6D40 bytes — the first 0x6D40 bytes of the
//             player data, with a checksum of its own at +0xEC
//   +0x147D8  three copies of the settings (0x40 bytes), one per slot
//   +0x14898  u32 checksum (a byte for the header and one per slot), u32 size
//
// and then how far it has got:
//
//   +0x148A0  how the message is shown (0 a window, 1 / 2 a line of the load screen)
//   +0x148A4  the slot being written      +0x148A8 / +0x148AC  the message file / the icon
//   +0x148B0  state, +0x148B4 step within it      +0x148B8 face (1 = the load screen)
//   +0x148BC  port      +0x148C0  → the operation in progress, in a table
//   +0x148C8  0 / 1 loading the two files, 2 idle, 3 an operation table is running
//   +0x148CC  its result      +0x148D8 / +0x148DC  the operation and its step
//   +0x148E0  message, +0x148E4 question, +0x148E8 / +0x148EC the two answers (indices)
//   +0x148F0  where the cursor starts     +0x148F4 / +0x14900  the two windows
//   +0x148F8  the answer (0 = the first)  +0x14904 the button that ended a wait
//   +0x1490C  KB a save needs             +0x14910  icon.sys as it should be (0x3C4 bytes)
//
// A state does its work by starting an **operation table** — pairs {operation,
// argument} in the game's data at 0x2DA900…, ended by operation 0x16 — and
// waiting for it: 0 ask the card what it is, 1 look for the save's three files
// and check them, 3 make them, 4 read the save, 5 write one slot, 6 format,
// 0x13 delete, 8–0xD and 0x14 / 0x15 wait for a button or an answer (some
// while watching for the card being pulled out), 0xE–0x12 set the message,
// its window, the two answers and the question.  Results are 0, or
// 0x80000000 | what is wrong; 0x1000000 = not finished.
//
// The strings (`mcard/message.dat`: 16-bit, one after another) carry `%s` the
// slot's number, `%n` the name, `%d` the day, `%p` the period, each followed
// by asterisks that reserve room; `＆` stands for the KB needed.  The original
// writes the values into the file's text in place; here each message is
// made anew when it is shown.
//
// The card itself is native (native/mcard_native.cpp): a directory.

#include "engine/config.h"
#include "engine/containers.h"
#include "engine/font.h"
#include "engine/pad.h"
#include "engine/window.h"
#include "game/player_data.h"
#include "native/guest.h"
#include "native/mcard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* AssetRef_Ctor(void* mem);                     // 0x174C00
    void  AssetRef_Dtor(void* ref, int flag);
    char  AssetQueue_RequestByName(void* ref, const char* name);
    void* AssetRef_GetData(void* ref);                  // 0x174A00
    s32   AssetRef_GetSize(void* ref);
    void* PadMgr_Get();                                 // 0x13A920
    u16   Pad_GetPressed(void* mgr, int port);          // 0x13A0C0
    u16   Pad_GetRepeat(void* mgr, int port);           // 0x139F50
    u16   Pad_GetHeld(void* mgr, int port);             // 0x13A0E0
    u16   Pad_GetStickHeld(void* mgr, int port);        // 0x139F90
    u32   Screen_X(int v, int align);                   // 0x12E5E0
    u32   Screen_Y(int v, int align);                   // 0x12E550
    Window* WindowMgr_OpenPassive(WindowMgr* self, WindowDesc* desc);   // 0x13D810
}

namespace {

template <typename T = void> inline T* P(u32 a) { return guest::ptr<T>(a); }
inline s32& F(void* self, u32 at) { return *reinterpret_cast<s32*>(static_cast<u8*>(self) + at); }
inline u8*  B(void* self, u32 at) { return static_cast<u8*>(self) + at; }

const GuestPtrVar<u8>        gPlayerData{0x2EE688u};
const GuestPtrVar<u8>        gConfigData{0x2EE68Cu};
const GuestPtrVar<WindowMgr> gWindowMgr{0x2EE680u};
const GuestPtrVar<Font>      gFont{0x2EE67Cu};
s32* const gFrameRate   = guest::ptr<s32>(0x2EDA00u);
s32* const gCurrentSlot = guest::ptr<s32>(0x2EE768u);       // the slot the game in memory belongs to
s32* const gWindowY     = guest::ptr<s32>(0x2EE764u);       // where the message window goes
s32* const gLiveObjects = guest::ptr<s32>(0x2EE760u);

enum : u32 {
    kStyle = 0x148A0, kWriteSlot = 0x148A4, kMsgFile = 0x148A8, kIconFile = 0x148AC, kState = 0x148B0,
    kStep = 0x148B4, kFace = 0x148B8, kPort = 0x148BC, kOpAt = 0x148C0, kPhase = 0x148C8, kResult = 0x148CC,
    kKept = 0x148D0, kRead = 0x148D4, kOp = 0x148D8, kOpStep = 0x148DC, kMessage = 0x148E0, kQuestion = 0x148E4,
    kAnswer0 = 0x148E8, kAnswer1 = 0x148EC, kCursor = 0x148F0, kMsgWindow = 0x148F4, kAnswer = 0x148F8,
    kAskWindow = 0x14900, kButton = 0x14904, kCardWas = 0x14908, kNeeded = 0x1490C, kIconSys = 0x14910,
    kIconSysRead = 0x14CD4, kIconRead = 0x15098, kDiffer = 0x1509C,
    // (four strings in the original; here four buffers in guest memory)
    kMsgText = 0x150A0, kAskText = 0x150B0, kAns0Text = 0x150C0, kAns1Text = 0x150D0,
    kCountdown = 0x150E0,
    kData = 0x10,
};
enum : s32 {
    kBusy = 0x1000000,
    kNoCard = (s32)0x80000008, kNotPs2 = (s32)0x80000400, kUnformattedA = (s32)0x80000010,
    kUnformatted = (s32)0x80000020, kNoFile = (s32)0x80000040, kNoRoom = (s32)0x80000080,
    kBroken = (s32)0x80000100, kFailed = (s32)0x80000200,
};
enum : u32 { kSlotSize = 0x6D40, kSaveSize = 0x14890, kTextRoom = 0x400 };

// ═══ The save data (0x265990–0x266EA0) ════════════════════════════════════
// `d` is the object + 0x10.
inline u8* Slot(u8* d, int i) { return d + 8 + (u32)i * kSlotSize; }
inline u8* SlotConfig(u8* d, int i) { return d + 0x147C8 + (u32)i * 0x40; }
inline s32& SlotSum(u8* d, int i) { return *reinterpret_cast<s32*>(Slot(d, i) + 0xEC); }

u32 Sum(const u8* p, u32 n) {
    u32 sum = 0;
    for (u32 i = 0; i < n; i++)
        sum += p[i];
    return sum;
}
// 0x265990: a slot and its settings, the stored sum left out.
s32 SlotChecksum(u8* d, int i) {
    const s32 kept = SlotSum(d, i);
    SlotSum(d, i) = 0;
    const u32 sum = Sum(Slot(d, i), kSlotSize) + Sum(SlotConfig(d, i), 0x40);
    SlotSum(d, i) = kept;
    return (s32)sum;
}
// 0x2664A0: → 0 if the slot can be believed.
s32 SlotCheck(u8* d, int i) {
    const s32 kept = SlotSum(d, i);
    if (kept != 0 && kept != SlotChecksum(d, i))
        return kBroken;
    const u8* s = Slot(d, i);
    const auto h = [s](u32 at) { s16 v; memcpy(&v, s + at, 2); return v; };
    if ((s8)s[0x100] < 0 || (s8)s[0x101] < 0 || h(0x106) < 0 || h(0x116) < 0)
        return kBroken;
    if (h(0x10E) < 0 || h(0x110) < 0 || h(0x108) < 0 || h(0x10A) < 0)
        return kBroken;
    return 0;
}
// 0x266460 / 0x266440: is there a game in it — and is it one not yet begun?
bool SlotUsed(u8* d, int i) {
    s32 head[2];
    memcpy(head, Slot(d, i), 8);
    return head[0] != 3 || head[1] != 0;
}
bool SlotFresh(u8* d, int i) {
    s32 state;
    memcpy(&state, Slot(d, i), 4);
    return state == 1;
}
// What a byte lane of the file's checksum holds: the header's, or a slot's.
u32 FileChecksum(u8* d, u32* size) {
    u32 sum = Sum(d, 8) & 0xFF;
    u32 total = 4 + 8;
    for (int i = 0; i < 3; i++) {
        const u32 shift = (u32)(i + 1) * 8;
        const u32 lane = (Sum(Slot(d, i), kSlotSize) + Sum(SlotConfig(d, i), 0x40)) & 0xFF;
        sum = (sum & ~(0xFFu << shift)) | (((((sum >> shift) & 0xFF) + lane) & 0xFF) << shift);
        total += kSlotSize + 0x40;
    }
    *size = total;
    return sum;
}
// 0x2668F0: the sums, made afresh.
void Reseal(u8* d) {
    for (int i = 0; i < 3; i++)
        if (SlotCheck(d, i) == 0)
            SlotSum(d, i) = SlotChecksum(d, i);
    u32 size;
    const u32 sum = FileChecksum(d, &size);
    memcpy(d + 0x14888, &sum, 4);
    memcpy(d + 0x1488C, &size, 4);
}
// 0x1788A0: an empty slot — a game never begun.
void BlankSlot(u8* slot) {
    static u32 scratch = 0;
    if (scratch == 0)
        scratch = guest::addr(guest::alloc(0x7000));
    u8* blank = P<u8>(scratch);
    memset(blank, 0, 0x7000);
    const s32 two = 2, capacity = 0x14, empty = 3;
    memcpy(blank + 0x14, &two, 4);
    PlayerData_ResetGame(reinterpret_cast<PlayerData*>(blank), 1);
    memset(blank + 0x880, 0, 0x6440);
    memset(blank + 0x6CC0, 0, 0x80);
    memcpy(blank + 0x6C84, &capacity, 2);
    memcpy(blank, &empty, 4);
    memcpy(slot, blank, kSlotSize);
}
void BlankConfig(u8* cfg) {                             // 0x265940
    memset(cfg, 0, 0x40);
    const s32 v[9] = {10, 10, 0, 1, 1, 0, 0, 1, 1};     // 0x1836A0, 0x1836C0
    memcpy(cfg, v, sizeof v);
}
// 0x266DD0: nothing saved at all.
void BlankData(u8* d) {
    const s32 head[2] = {100, 0};
    memcpy(d, head, 8);
    for (int i = 0; i < 3; i++) {
        BlankSlot(Slot(d, i));
        BlankConfig(SlotConfig(d, i));
    }
    Reseal(d);
}
// 0x266570: what was read — → 0, or broken beyond use.
s32 VerifyData(u8* d) {
    s32 head[2];
    memcpy(head, d, 8);
    if (head[0] != 100 || head[1] < 0 || head[1] > 2) {
        const s32 fresh[2] = {100, 0};
        memcpy(d, fresh, 8);
    }
    u32 size, stored, storedSize;
    const u32 sum = FileChecksum(d, &size);
    memcpy(&stored, d + 0x14888, 4);
    memcpy(&storedSize, d + 0x1488C, 4);
    if (stored == 0 || (stored == sum && storedSize == size))
        return 0;
    int bad = 0;
    for (int i = 0; i < 3; i++)
        bad += SlotCheck(d, i) != 0;
    return bad == 3 ? kBroken : 0;
}
// 0x265AF0: the sums of the slots that check out are cleared (a slot that
// does not keeps its wrong one, and so stays "corrupt").
void DropBadSums(u8* d) {
    for (int i = 0; i < 3; i++)
        if (SlotCheck(d, i) == 0)
            SlotSum(d, i) = 0;
}
// 0x266C20: the current slot → the game in memory.
void SlotToGame(u8* d) {
    const s32 slot = *gCurrentSlot;
    memcpy(d + 4, &slot, 4);
    SlotSum(d, slot) = 0;
    memcpy(*gPlayerData, Slot(d, slot), kSlotSize);
    memcpy(*gConfigData, SlotConfig(d, slot), 0x40);
}
// 0x266EA0: the game in memory → the current slot.
int GameToSlot(u8* d) {
    const s32 slot = *gCurrentSlot;
    if (slot < 0 || slot > 2)
        return 0;
    memcpy(d + 4, &slot, 4);
    memcpy(Slot(d, slot), *gPlayerData, kSlotSize);
    SlotSum(d, slot) = 0;
    memcpy(SlotConfig(d, slot), *gConfigData, 0x40);
    Reseal(d);
    return 1;
}

// ═══ Messages ═════════════════════════════════════════════════════════════
struct Extra {
    std::vector<u32> messages;      // → the strings of mcard/message.dat
    std::map<int, std::u16string> made;     // those with values filled in
};
std::map<void*, Extra> sExtra;

const u16* Text(int id) { return TextTable_Find(TextTable_Get(), id); }
std::u16string Raw(void* self, int index) {
    const Extra& e = sExtra[self];
    const auto made = e.made.find(index);
    if (made != e.made.end())
        return made->second;
    if (index < 0 || (size_t)index >= e.messages.size())
        return {};
    const u16* s = P<u16>(e.messages[(size_t)index]);
    return std::u16string(reinterpret_cast<const char16_t*>(s), (size_t)WStr_Length(s));
}
// 0x261800: the file is strings one after another.
void ReadMessages(void* self) {
    Extra& e = sExtra[self];
    e.messages.clear();
    void* ref = P(F(self, kMsgFile));
    const u16* text = static_cast<const u16*>(AssetRef_GetData(ref));
    const s32 size = AssetRef_GetSize(ref);
    const u16* start = text;
    for (s32 at = 0; at + 1 < size; at += 2, text++)
        if (*text == 0) {
            e.messages.push_back(guest::addr(start));
            start = text + 1;
        }
}
std::u16string Number(int n) {
    char digits[16];
    snprintf(digits, sizeof digits, "%d", n);
    return std::u16string(digits, digits + strlen(digits));
}
// 0x260F10: the values of a slot — the game in memory's, or a save's — put
// where the message asks for them.
void Fill(void* self, int index, int slot, bool fromSave) {
    Extra& e = sExtra[self];
    e.made.erase(index);
    const std::u16string in = Raw(self, index);
    u8* d = B(self, kData);
    const u8* from = fromSave ? Slot(d, slot) : *gPlayerData;
    std::u16string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != u'%' || i + 1 >= in.size()) {
            out += in[i];
            continue;
        }
        const char16_t what = in[++i];
        if (what == u's') {
            out += Number(slot + 1);
        } else if (what == u'p') {
            if (const u16* period = Text(0x1E + (s8)from[0x101]))
                out += reinterpret_cast<const char16_t*>(period);
        } else if (what == u'd') {
            out += Number((s8)from[0x100] + 1);
        } else if (what == u'n') {
            const u16* name = reinterpret_cast<const u16*>(from + 0x480);
            if (fromSave && !SlotUsed(d, slot))
                name = Text(0x11B);                     // 0x266300
            if (name != nullptr)
                out += reinterpret_cast<const char16_t*>(name);
        }
        while (i + 1 < in.size() && in[i + 1] == u'*')  // the room kept for it
            i++;
    }
    e.made[index] = out;
}
void Put(void* self, u32 field, const std::u16string& text) {
    u16* room = P<u16>((u32)F(self, field));
    const size_t n = text.size() < kTextRoom - 1 ? text.size() : kTextRoom - 1;
    memcpy(room, text.data(), n * 2);
    room[n] = 0;
}
// 0x261F00: a message into the string that is shown — `＃` becomes the card
// slot's number and `＆` the KB a save needs.
void SetMessage(void* self, int index) {
    F(self, kMessage) = index;
    if (index < 0)
        return;
    std::u16string text = Raw(self, index), out;
    for (char16_t c : text) {
        if (c == 0xFF03)
            out += F(self, kPort) == 0 ? u'1' : u'2';
        else if (c == 0xFF06)
            out += Number(F(self, kNeeded));
        else
            out += c;
    }
    Put(self, kMsgText, out);
}

// ═══ Windows (0x261990, 0x261B60) ═════════════════════════════════════════
void CloseWindow(void* self, u32 field) {
    if (F(self, field) != 0) {
        WindowMgr_Close(*gWindowMgr, P<Window>((u32)F(self, field)));
        F(self, field) = 0;
    }
}
// 0x261B60: shows the message — in a window, or (the load screen) as a line.
void ShowMessage(void* self) {
    const s32 face = F(self, kFace);
    if (face == 2 || face == 1) {
        F(self, kStyle) = face;
        return;
    }
    F(self, kStyle) = 0;
    CloseWindow(self, kMsgWindow);
    WindowDesc desc;
    memset(&desc, 0, sizeof desc);
    WindowDesc_Init(&desc);
    desc.x = 0;
    desc.y = *gWindowY;
    desc.title = F(self, kMessage) < 0 ? 0 : (u32)F(self, kMsgText);
    desc.z = 0xFFFFE6;
    desc.titleHeight = 0;
    desc.titleAlign = 0;
    desc.priority = 2;
    F(self, kMsgWindow) = (s32)guest::addr(WindowMgr_OpenPassive(*gWindowMgr, &desc));
}
// 0x261990: asks — the question over two answers.
void Ask(void* self) {
    WindowDesc desc;
    memset(&desc, 0, sizeof desc);
    WindowDesc_Init(&desc);
    if (F(self, kAnswer0) >= 0)
        VectorU32_PushBack(&desc.options, (u32)F(self, kAns0Text));
    if (F(self, kAnswer1) >= 0)
        VectorU32_PushBack(&desc.options, (u32)F(self, kAns1Text));
    desc.x = 0;
    desc.y = F(self, kFace) == 0 ? -100 : 0;
    desc.title = F(self, kQuestion) < 0 ? 0 : (u32)F(self, kAskText);
    desc.priority = 2;
    desc.titleHeight = 0;
    desc.z = 0xFFFFE7;
    desc.cursor = F(self, kCursor);
    F(self, kAskWindow) = (s32)guest::addr(WindowMgr_Open(*gWindowMgr, &desc));
    if (desc.options.data != 0)
        MemFree(P(desc.options.data));
}
void SetState(void* self, int state) {                  // 0x262450
    F(self, kState) = state;
    F(self, kStep) = 0;
}
void SetStep(void* self, int step) { F(self, kStep) = step; }      // 0x262440
// 0x2623B0: a message at once, outside a table.
void Say(void* self, int index) {
    SetMessage(self, index);
    ShowMessage(self);
}

// ═══ The card (0x2649E0) ══════════════════════════════════════════════════
// What is wrong with the card, among the things `ask` names.
s32 CardCheck(void* self, u32 ask) {
    const int port = F(self, kPort);
    const u32 status = mc::status(port);
    const s32 result = mc::result();
    if (status & 0x800)
        mc::set_flags(port, 0x400);
    if ((ask & 0x80000008u) == 0x80000008u && (status & 0x800))
        return kNoCard;
    if ((ask & 0x80000400u) == 0x80000400u && (status & 0xF000) != 0x2000)
        return kNotPs2;
    if ((ask & 0x80000010u) == 0x80000010u && (status & 0x80))
        return kUnformattedA;
    if ((ask & 0x80000020u) == 0x80000020u && (status & 0x200))
        return kUnformatted;
    if ((ask & 0x80000080u) == 0x80000080u && mc::free_kb(port) < F(self, kNeeded) && (status & 0x200) == 0)
        return kNoRoom;
    if ((result == -3 || result == -4) && (ask & 0x80000100u) == 0x80000100u)
        return kBroken;
    if (result < -2)
        return kFailed;
    return 0;
}

const char kDir[] = "/BASLUS-20893";
const char kDirAll[] = "/BASLUS-20893/*";
const char kIconSysPath[] = "/BASLUS-20893/icon.sys";
const char kIconPath[] = "/BASLUS-20893/samurai2.ico";
const char kDataPath[] = "/BASLUS-20893/BASLUS-20893";

// ═══ Operations (0x262860) ════════════════════════════════════════════════
// One step of the operation in progress.  → 0 done (on to the next of the
// table), 0x1000000 not yet, anything else: the table stops with that.
s32 RunOperation(void* self) {
    void* pads = PadMgr_Get();
    const u16 pressed = (u16)(Pad_GetPressed(pads, 0) | Pad_GetRepeat(pads, 0));
    const u16 held = (u16)(Pad_GetHeld(pads, 0) | Pad_GetStickHeld(pads, 0));
    const int port = F(self, kPort);
    const u32 card = mc::status(port);
    u8* d = B(self, kData);
    void* icon = P((u32)F(self, kIconFile));
    s32& step = F(self, kOpStep);
    s32 result = kBusy;
    const auto closeAsk = [self] { CloseWindow(self, kAskWindow); };
    // A button ends a wait: Cross (not while the cursor is being moved), or
    // Triangle, which also answers "the second".
    const auto answered = [&](bool triangleToo, bool takeCursor) {
        if ((pressed & kPadCross) && (held & (kPadUp | kPadDown)) == 0) {
            F(self, kButton) = kPadCross;
            if (takeCursor) {
                F(self, kAnswer) = WindowMgr_GetCursor(*gWindowMgr, P<Window>((u32)F(self, kAskWindow)));
                closeAsk();
            }
            return true;
        }
        if (triangleToo && (pressed & kPadTriangle)) {
            F(self, kButton) = kPadTriangle;
            if (takeCursor) {
                F(self, kAnswer) = 1;
                closeAsk();
            }
            return true;
        }
        return false;
    };
    // The card pulled out, or one of another kind put in.
    const auto watch = [&](s32 now) -> s32 {
        if (card & 0x800)
            return kNoCard;
        if ((card & 0xF000) != 0x2000)
            return kNotPs2;
        return now;
    };

    switch (F(self, kOp)) {
    case 0:                                             // what is in the slot?
        if (step == 1)
            result = CardCheck(self, 0x80000438u);
        else if (step == 0)
            mc::get_info(port);
        step++;
        break;
    case 1: {                                           // the save's files: there, and right?
        switch (step) {
        case 0:
            mc::get_dir(port, kDir);
            break;
        case 1:
            if (mc::result() == 0)
                result = CardCheck(self, 0x80000080u) == kNoRoom ? kNoRoom : kNoFile;
            else
                mc::get_dir(port, kDirAll);
            break;
        case 2: {
            const s32 count = mc::result();
            if (count == -4) {
                result = CardCheck(self, 0x80000080u) == kNoRoom ? kNoRoom : kNoFile;
            } else if ((result = CardCheck(self, 0x80000438u)) == 0) {
                if (count < 5) {
                    result = kBroken;
                } else {
                    int i = 0;
                    for (; i < count; i++) {
                        const mc::Entry* e = mc::entry(i);
                        if (e->name == "icon.sys") {
                            if (e->size != 0x3C4) break;
                        } else if (e->name == "samurai2.ico") {
                            if ((s32)e->size != AssetRef_GetSize(icon)) break;
                        } else if (e->name == "BASLUS-20893") {
                            if (e->size != kSaveSize) break;
                        }
                    }
                    result = i == count ? kBusy : kBroken;
                }
            }
            break;
        }
        case 3:
            if (mc::open(port, kIconSysPath, 1) != 0)
                result = kBroken;
            break;
        case 4:
            if ((result = CardCheck(self, 0x80000538u)) == 0) {
                mc::read(B(self, kIconSysRead), 0x3C4);
                result = kBusy;
            }
            break;
        case 5:
            F(self, kKept) = CardCheck(self, 0x80000538u);
            mc::close();
            F(self, kDiffer) = memcmp(B(self, kIconSys), B(self, kIconSysRead), 0x3C4);
            break;
        case 6:
            if ((result = CardCheck(self, 0x80000538u)) == 0) {
                result = F(self, kKept) != 0 ? F(self, kKept) : kBusy;
                if (F(self, kDiffer) != 0)
                    result = kBroken;
            }
            break;
        case 7:
            result = F(self, kKept);
            if (result == 0)
                result = mc::open(port, kIconPath, 1) != 0 ? kBroken : kBusy;
            break;
        case 8:
            if ((result = CardCheck(self, 0x80000538u)) == 0) {
                const u32 size = (u32)AssetRef_GetSize(icon);
                F(self, kIconRead) = (s32)guest::addr(guest::alloc(size));
                mc::read(P((u32)F(self, kIconRead)), size);
                result = kBusy;
            }
            break;
        case 9:
            F(self, kKept) = CardCheck(self, 0x80000538u);
            mc::close();
            F(self, kDiffer) = memcmp(P((u32)F(self, kIconRead)), AssetRef_GetData(icon), (size_t)AssetRef_GetSize(icon));
            if (F(self, kIconRead) != 0)
                guest::free(P((u32)F(self, kIconRead)));
            F(self, kIconRead) = 0;
            break;
        case 10:
            result = CardCheck(self, 0x80000538u);
            if (F(self, kKept) != 0)
                result = F(self, kKept);
            if (F(self, kDiffer) != 0)
                result = kBroken;
            break;
        }
        step++;
        break;
    }
    case 2:
        result = CardCheck(self, 0x80000488u);
        break;
    case 3: {                                           // make the save's files
        const auto next = [&](bool ok) { result = ok ? kBusy : kFailed; };
        switch (step) {
        case 0:
            next(mc::make_dir(port, kDir) == 0);
            break;
        case 1:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0) next(mc::open(port, kDataPath, 0x200) == 0);
            break;
        case 2:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0) next(mc::write(d, kSaveSize) == 0);
            break;
        case 3:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0) next(mc::close() == 0);
            break;
        case 4:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0) next(mc::open(port, kIconPath, 0x200) == 0);
            break;
        case 5:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0)
                next(mc::write(AssetRef_GetData(icon), (u32)AssetRef_GetSize(icon)) == 0);
            break;
        case 6:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0) next(mc::close() == 0);
            break;
        case 7:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0) next(mc::open(port, kIconSysPath, 0x200) == 0);
            break;
        case 8:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0) next(mc::write(B(self, kIconSys), 0x3C4) == 0);
            break;
        case 9:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0) next(mc::close() == 0);
            break;
        default:
            if ((result = CardCheck(self, 0xFFFFFFFFu)) == 0)
                mc::clear_flags(port, 0x400);
            break;
        }
        step++;
        break;
    }
    case 4:                                             // read the save
        if (step == 0) {
            F(self, kRead) = 0;
            mc::open(port, kDataPath, 1);
        } else if (step == 1) {
            if ((result = CardCheck(self, 0x80000538u)) == 0) {
                mc::read(d, kSaveSize);
                result = kBusy;
            }
        } else if (step == 2) {
            F(self, kKept) = CardCheck(self, 0x80000538u);
            F(self, kRead) = mc::result();
            mc::close();
        } else if (step == 3) {
            const s32 now = CardCheck(self, 0x80000538u);
            if (now == kNoCard || F(self, kKept) == kNoCard) {
                result = kNoCard;
            } else if (F(self, kKept) != 0) {
                result = F(self, kKept);
            } else if (F(self, kRead) != (s32)kSaveSize) {
                result = kFailed;
            } else if (VerifyData(d) != 0) {
                result = kBroken;
            } else {
                DropBadSums(d);
                result = now;
            }
        }
        step++;
        break;
    case 5: {                                           // write one slot (and the file's ends)
        const u32 slot = (u32)F(self, kWriteSlot);
        const auto part = [&](bool ok) {
            if ((result = CardCheck(self, 0x80000438u)) == 0)
                result = ok ? kBusy : kFailed;
            else
                mc::close();
        };
        switch (step) {
        case 0: mc::open(port, kDataPath, 2); break;
        case 1: part(mc::write(d, 8) == 0); break;
        case 2: part(mc::seek(slot * kSlotSize + 8) == 0); break;
        case 3: part(mc::write(Slot(d, (int)slot), kSlotSize) == 0); break;
        case 4: part(mc::seek(slot * 0x40 + 0x147C8) == 0); break;
        case 5: part(mc::write(SlotConfig(d, (int)slot), 0x40) == 0); break;
        case 6: part(mc::seek(0x14888) == 0); break;
        case 7: part(mc::write(d + 0x14888, 8) == 0); break;
        case 8:
            F(self, kKept) = CardCheck(self, 0x80000438u);
            mc::close();
            break;
        case 9:
            result = CardCheck(self, 0x80000438u);
            if (F(self, kKept) == 0 && result == 0)
                mc::clear_flags(port, 0x400);
            if (F(self, kKept) != 0)
                result = F(self, kKept);
            break;
        }
        step++;
        break;
    }
    case 6:
        if (step == 1) result = CardCheck(self, 0x80000438u);
        else if (step == 0) mc::format(port);
        step++;
        break;
    case 7:
        if (step == 1) result = CardCheck(self, 0x80000438u);
        else if (step == 0) mc::unformat(port);
        step++;
        break;
    case 8:                                             // Cross or Triangle
        if (step == 1) {
            if (pressed & kPadCross) { F(self, kButton) = kPadCross; result = 0; }
            else if (pressed & kPadTriangle) { F(self, kButton) = kPadTriangle; result = 0; }
        } else if (step == 0) {
            step = 1;
            F(self, kButton) = 0;
        }
        break;
    case 9:                                             // Cross — or the card changes
        if (step == 1) {
            if (pressed & kPadCross) { result = 0; F(self, kButton) = kPadCross; }
        } else if (step == 0) {
            F(self, kCardWas) = (s32)card;
            F(self, kButton) = 0;
            step++;
        }
        if ((card & 0xF000) != ((u32)F(self, kCardWas) & 0xF000))
            result = 1;
        break;
    case 10:                                            // Cross or Triangle, the card watched
        if (step == 1) {
            if (pressed & (kPadCross | kPadTriangle)) { result = 0; F(self, kButton) = pressed & (kPadCross | kPadTriangle); }
        } else if (step == 0) {
            F(self, kCardWas) = (s32)card;
            F(self, kButton) = 0;
            step++;
        }
        result = watch(result);
        break;
    case 0xB:                                           // a question
        if (step == 1) {
            if (answered(true, true)) result = 0;
        } else if (step == 0) {
            Ask(self);
            step++;
            F(self, kButton) = 0;
        }
        break;
    case 0xC:                                           // a question — or the card changes
        if (step == 1) {
            if ((card & 0xF000) == ((u32)F(self, kCardWas) & 0xF000)) {
                if (answered(true, true)) result = 0;
            } else {
                result = 1;
                closeAsk();
            }
        } else if (step == 0) {
            F(self, kCardWas) = (s32)card;
            Ask(self);
            F(self, kButton) = 0;
            step++;
        }
        break;
    case 0xD:                                           // a question, the card watched
        if (step == 1) {
            if (answered(true, true)) result = 0;
        } else if (step == 0) {
            Ask(self);
            F(self, kButton) = 0;
            step++;
        }
        if (watch(result) != result) {
            result = watch(result);
            closeAsk();
        }
        break;
    case 0xE:
        SetMessage(self, P<s32>((u32)F(self, kOpAt))[1]);
        result = 0;
        break;
    case 0xF:
        ShowMessage(self);
        result = 0;
        break;
    case 0x10:
    case 0x11: {                                        // the two answers (0x261DA0)
        const bool second = F(self, kOp) == 0x11;
        const s32 index = P<s32>((u32)F(self, kOpAt))[1];
        F(self, second ? kAnswer1 : kAnswer0) = index;
        F(self, kCursor) = second ? 1 : 0;
        if (index >= 0)
            Put(self, second ? kAns1Text : kAns0Text, Raw(self, index));
        result = 0;
        break;
    }
    case 0x12: {                                        // the question (0x261CF0)
        const s32 index = P<s32>((u32)F(self, kOpAt))[1];
        F(self, kQuestion) = index;
        Put(self, kAskText, Raw(self, index));
        result = 0;
        break;
    }
    case 0x13: {                                        // delete the save's files
        static const char* const kPaths[4] = {kIconSysPath, kIconPath, kDataPath, kDir};
        if (step == 0) {
            mc::remove(port, kPaths[0]);
        } else if (step <= 3) {
            result = CardCheck(self, 0x80000438u);
            if (result == 0 || result == kFailed) {
                mc::remove(port, kPaths[step]);
                result = kBusy;
            }
        } else {
            result = CardCheck(self, 0x80000438u);
            if (result == 0 || result == kFailed)
                result = 0;
        }
        step++;
        break;
    }
    case 0x14:                                          // Cross or Triangle
        if (step == 1) {
            if (answered(true, false)) result = 0;
        } else if (step == 0) {
            step = 1;
            F(self, kButton) = 0;
        }
        break;
    case 0x15:                                          // …the card watched
        if (step == 1) {
            if (answered(true, false)) result = 0;
        } else if (step == 0) {
            F(self, kCardWas) = (s32)card;
            F(self, kButton) = 0;
            step++;
        }
        result = watch(result);
        break;
    case 0x16:
        result = 0;
        break;
    }
    return result;
}

// 0x264BE0: starts a table of operations.  → 0 if there is nothing to run.
int StartTable(void* self, u32 table) {
    if (F(self, kPhase) != 2)
        return 0;
    F(self, kOpAt) = (s32)table;
    F(self, kResult) = kBusy;
    F(self, kQuestion) = F(self, kAnswer0) = F(self, kAnswer1) = -1;
    if (table == 0)
        return 0;
    F(self, kOp) = *P<s32>(table);
    F(self, kOpStep) = 0;
    if (F(self, kOp) == 0x16)
        return 0;
    F(self, kPhase) = 3;
    return 1;
}

// 0x2625E0: once a frame — the two files first, then the table in progress.
void StepCommon(void* self) {
    switch (F(self, kPhase)) {
    case 3: {
        const s32 result = RunOperation(self);
        if (result == kBusy)
            break;
        if (result == 0) {
            F(self, kOpAt) += 8;
            F(self, kOp) = *P<s32>((u32)F(self, kOpAt));
            F(self, kOpStep) = 0;
            if (F(self, kOp) == 0x16) {
                F(self, kPhase) = 2;
                F(self, kResult) = 0;
            }
        } else {
            F(self, kResult) = result;
            F(self, kPhase) = 2;
        }
        break;
    }
    case 1:
        if (F(self, kIconFile) == 0) {
            void* ref = AssetRef_Ctor(MemAlloc(0xC));
            F(self, kIconFile) = (s32)guest::addr(ref);
            AssetQueue_RequestByName(ref, "mcard/icon1.ico");
        } else if (AssetRef_GetData(P((u32)F(self, kIconFile))) != nullptr) {
            F(self, kPhase) = 2;
            F(self, kNeeded) = (((s32)kSaveSize + 0x3FF) >> 10) + ((AssetRef_GetSize(P((u32)F(self, kIconFile))) + 0x3FF) >> 10) + 5;
        }
        break;
    case 0:
        if (F(self, kMsgFile) == 0) {
            void* ref = AssetRef_Ctor(MemAlloc(0xC));
            F(self, kMsgFile) = (s32)guest::addr(ref);
            AssetQueue_RequestByName(ref, "mcard/message.dat");
        } else if (AssetRef_GetData(P((u32)F(self, kMsgFile))) != nullptr) {
            ReadMessages(self);
            F(self, kPhase) = 1;
        }
        break;
    }
}

// ═══ Construction (0x2654F0, 0x2656A0, 0x264D00) ══════════════════════════
enum : u32 { kVtBase = 0x2ECB88u, kVtBoot = 0x2ECB78u, kVtSave = 0x2ECB68u };

void BaseCtor(void* self) {
    *static_cast<u32*>(self) = kVtBase;
    BlankData(B(self, kData));
    u16* room = static_cast<u16*>(guest::alloc(kTextRoom * 2 * 4));
    memset(room, 0, kTextRoom * 2 * 4);
    for (int i = 0; i < 4; i++)
        F(self, kMsgText + (u32)i * 0x10) = (s32)guest::addr(room + (size_t)i * kTextRoom);
    F(self, kWriteSlot) = *gCurrentSlot;
    ++*gLiveObjects;
    sExtra[self] = Extra();
    // 0x2654F0
    for (u32 at : std::initializer_list<u32>{kPhase, kPort, kResult, kOpAt, kMsgFile, kIconFile, kAnswer, kCursor,
                                             kMsgWindow, kStyle, kAskWindow, kFace, kNeeded, kButton, 0x148FCu,
                                             kIconRead})
        F(self, at) = 0;
    F(self, kMessage) = F(self, kAnswer0) = F(self, kAnswer1) = F(self, kQuestion) = -1;
    // What icon.sys must say: the header and colours, the title, the icon's name three times.
    u8* sys = B(self, kIconSys);
    memset(sys, 0, 0x3C4);
    memcpy(sys, P(0x2DAD70u), 0xC0);
    strcpy(reinterpret_cast<char*>(sys + 0xC0), P<const char>(0x2E77A0u));
    for (u32 at : {0x104u, 0x144u, 0x184u})
        strcpy(reinterpret_cast<char*>(sys + at), "samurai2.ico");
}
void BaseDtor(void* self) {
    *static_cast<u32*>(self) = kVtBase;
    --*gLiveObjects;
    F(self, kOpAt) = 0;
    CloseWindow(self, kMsgWindow);
    CloseWindow(self, kAskWindow);
    for (u32 at : std::initializer_list<u32>{kMsgFile, kIconFile}) {
        if (F(self, at) != 0)
            AssetRef_Dtor(P((u32)F(self, at)), 1);
        F(self, at) = 0;
    }
    if (F(self, kIconRead) != 0)
        guest::free(P((u32)F(self, kIconRead)));
    F(self, kIconRead) = 0;
    guest::free(P((u32)F(self, kMsgText)));
    sExtra.erase(self);
    // (0x154FA0: the font's spacing back to its own)
    *guest::ptr<u16>(0x2EE538u) = 0x160;
    *guest::ptr<u16>(0x2EE53Au) = 0x160;
}
void* Destroy(void* self, int flag) {                   // 0x260E50, 0x25FFB0
    if (self != nullptr) {
        BaseDtor(self);
        if ((s16)flag > 0)
            MemFree(self);
    }
    return self;
}

const u32 kCheck = 0x2DA920u, kCreateAsk = 0x2DAB10u, kCreate = 0x2DAB90u, kFormatAsk = 0x2DAC80u,
          kFormat = 0x2DACB0u, kNoSaveA = 0x2DA980u, kNoSaveB = 0x2DA9D0u, kNoSaveC = 0x2DAA20u,
          kNotice = 0x2DACF0u, kNoticeWatched = 0x2DACD0u, kRecreateAsk = 0x2DAB50u, kRecreate = 0x2DABC0u,
          kProbe = 0x2DA970u, kOverwriteAsk = 0x2DAAE0u, kWrite = 0x2DAD10u, kRewrite = 0x2DABF0u,
          kCancelAsk = 0x2DAAB0u, kQuitAskB = 0x2DAA80u, kQuitAsk = 0x2DAA50u, kWaitGone = 0x2DA900u;
// The message a notice table shows is its first operation's argument.
void SetNotice(u32 table, s32 message) { P<s32>(table)[1] = message; }

// One of the things the first look at the card can find, shared by the boot
// check and the save screen: → true if it was one of the plain refusals.
bool Refused(void* self, s32 result, int noticeState, s32 removedMessage) {
    if (result == kNoRoom) { SetNotice(kNoticeWatched, 9); SetState(self, noticeState); return true; }
    if (result == kUnformattedA) { SetNotice(kNoticeWatched, 0x2E); SetState(self, noticeState); return true; }
    if (result == kNotPs2) { SetNotice(kNoticeWatched, 3); SetState(self, noticeState); return true; }
    if (result == kNoCard && removedMessage >= 0) { SetNotice(kNoticeWatched, removedMessage); SetState(self, noticeState); return true; }
    return false;
}
bool IsCorrupt(s32 r) {
    return r == kFailed || r == (s32)0x80000004 || r == (s32)0x80000002 || r == (s32)0x80000001 || r == kBroken;
}

}  // namespace

extern "C" {

// ═══ The boot check (0x260100–0x260EB0) ═══════════════════════════════════
void* Mode0_Ctor(void* mem) {                           // 0x260EB0
    BaseCtor(mem);
    *static_cast<u32*>(mem) = kVtBoot;
    *B(mem, 0x150E4) = 0;
    F(mem, kPort) = 0;
    SetState(mem, 0);
    return mem;
}
void Mode0_Start(void* self) {                          // 0x260E20
    F(self, kPort) = 0;
    SetState(self, 0);
}
// → 0 when it is over.
int Mode0_Update(void* self) {                          // 0x260100
    StepCommon(self);
    const s32 step = F(self, kStep);
    const bool idle = F(self, kPhase) == 2;
    const s32 result = F(self, kResult);
    const auto begin = [self](u32 table, int next) {
        if (StartTable(self, table) == 0)
            return 0;
        SetStep(self, next);
        return 1;
    };
    switch (F(self, kState)) {
    case 0:
        if (idle)
            SetState(self, 1);
        break;
    case 1:                                             // look at the card
        if (step == 1) {
            if (!idle)
                break;
            if (Refused(self, result, 10, -1)) {
            } else if (result == kNoFile || result == kUnformatted) {
                SetState(self, 6);
            } else if (result == kNoCard) {
                SetState(self, 5);
            } else if (IsCorrupt(result)) {
                if (mc::free_kb(F(self, kPort)) < F(self, kNeeded)) {
                    SetNotice(kNoticeWatched, 0x32);
                    SetState(self, 10);
                } else {
                    *B(self, 0x150E4) = 1;
                    BlankData(B(self, kData));
                    SetState(self, 3);
                }
            } else if (result == 0) {
                SetState(self, 3);
            } else {
                SetNotice(kNoticeWatched, 0x18);
                SetState(self, 10);
            }
        } else if (step == 0) {
            *B(self, 0x150E4) = 0;
            return begin(kCheck, 1);
        }
        break;
    case 2:                                             // make the save file
        if (step == 3) {
            if (--F(self, kCountdown) < 0)
                SetState(self, 3);
        } else if (step == 2) {
            Say(self, 0xD);
            F(self, kCountdown) = 0x1E;
            SetStep(self, 3);
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                SetStep(self, 2);
            } else {
                SetNotice(kNotice, 0xE);
                SetState(self, 9);
            }
        } else if (step == 0) {
            return begin(kCreate, 1);
        }
        break;
    case 3:
        return 0;
    case 4:                                             // format?
        if (step == 2) {
            if (!idle)
                break;
            if (result == 0) {
                SetState(self, 2);
            } else {
                SetNotice(kNotice, 8);
                SetState(self, 9);
            }
        } else if (step == 1) {
            if (!idle || result == kBusy)
                break;
            if (result != 0)
                SetState(self, 1);
            else if (F(self, kAnswer) == 0)
                return begin(kFormat, 2);
            else
                SetState(self, 8);
        } else if (step == 0) {
            return begin(kFormatAsk, 1);
        }
        break;
    case 5:                                             // no card: go on without saving?
        if (step == 1) {
            if (!idle)
                break;
            if (result == 0)
                SetState(self, F(self, kAnswer) == 0 ? 3 : 1);
            else if (result == 1)
                SetState(self, 1);
        } else if (step == 0) {
            return begin(kNoSaveA, 1);
        }
        break;
    case 6:                                             // no save file: make one?
        if (step == 2) {
            if (!idle)
                break;
            if (result == kUnformatted) SetState(self, 4);
            else if (result == 0) SetState(self, 2);
            else SetState(self, 1);
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                if (F(self, kAnswer) == 0)
                    return begin(kProbe, 2);
                SetState(self, 8);
            } else if (result == kNoRoom) {
                SetState(self, 7);
            } else if (result == kNoCard) {
                SetState(self, 1);
            }
        } else if (step == 0) {
            return begin(kCreateAsk, 1);
        }
        break;
    case 7:
        if (step == 1) {
            if (!idle)
                break;
            if (result == 0)
                SetState(self, F(self, kAnswer) == 0 ? 3 : 1);
            else if (result == kNoCard)
                SetState(self, 1);
        } else if (step == 0) {
            return begin(kNoSaveB, 1);
        }
        break;
    case 8:                                             // go on without saving?
        if (step == 1) {
            if (idle && result == 0)
                SetState(self, F(self, kAnswer) == 0 ? 3 : 1);
        } else if (step == 0) {
            return begin(kNoSaveC, 1);
        }
        break;
    case 9:                                             // a notice, then look again
        if (step == 1) {
            if (idle)
                SetState(self, 1);
        } else if (step == 0) {
            StartTable(self, kNotice);
            SetStep(self, 1);
        }
        break;
    case 10:                                            // a notice while the card is watched
        if (step == 1) {
            if (idle)
                SetState(self, result == 1 ? 1 : 8);
        } else if (step == 0) {
            StartTable(self, kNoticeWatched);
            SetStep(self, 1);
        }
        break;
    case 0xB:                                           // the file is bad: make it again?
        if (step == 4) {
            if (--F(self, kCountdown) < 0)
                SetState(self, 3);
        } else if (step == 3) {
            Say(self, 0xD);
            F(self, kCountdown) = 0x1E;
            SetStep(self, 4);
        } else if (step == 2) {
            if (!idle)
                break;
            if (result == 0) {
                SetStep(self, 3);
            } else {
                SetNotice(kNotice, 0xE);
                SetState(self, 9);
            }
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                if (F(self, kAnswer) == 0) {
                    StartTable(self, kRecreate);
                    SetStep(self, 2);
                } else {
                    SetState(self, 8);
                }
            } else if (result == kNoCard) {
                SetState(self, 1);
            }
        } else if (step == 0) {
            return begin(kRecreateAsk, 1);
        }
        break;
    }
    return 1;
}
// 0x262470: the message as a line of the load screen (in a window otherwise).
void Mode0_Draw(void* self) {
    if (F(self, kStyle) == 0 || F(self, kMessage) < 0)
        return;
    Font* font = *gFont;
    const bool first = F(self, kStyle) == 1;
    const u32 spacing = *P<u32>(first ? 0x2EE368u : 0x2EE36Cu);
    *guest::ptr<u16>(0x2EE538u) = (u16)spacing;
    *guest::ptr<u16>(0x2EE53Au) = (u16)(spacing >> 16);
    Font_SetColor(font, 0x809498A0u);
    Font_DrawString(font, P<u16>((u32)F(self, kMsgText)), (int)Screen_X(first ? -0x1120 : -0x1220, 0),
                    (int)Screen_Y(first ? 0x410 : -0x320, 0), 0xFFFFE6, 0.9f, 0.0f);
}

// ═══ The save screen (0x25E550–0x260050) ══════════════════════════════════
// +0x150E4 where it was opened (1: after the results), +0x150E8 the answer
// for the scene (1 = back to the title), +0x150EC the file was made anew,
// +0x150F0 the line that says what is being saved.
void* SaveScreen_Ctor(void* mem, int mode) {            // 0x260050
    BaseCtor(mem);
    *static_cast<u32*>(mem) = kVtSave;
    F(mem, 0x150E8) = 0;
    F(mem, 0x150E4) = mode;
    *B(mem, 0x150EC) = 0;
    F(mem, 0x150F0) = 0;
    F(mem, 0x150F4) = 0;
    *gWindowY = 0x14;
    F(mem, kPort) = 0;
    SetState(mem, 0);
    return mem;
}
void SaveScreen_Start(void* self) {                     // 0x25FF80
    F(self, kPort) = 0;
    SetState(self, 0);
}
// → 0 when it is over.
int SaveScreen_Update(void* self) {                     // 0x25E550
    StepCommon(self);
    void* pads = PadMgr_Get();
    const u16 pressed = (u16)(Pad_GetPressed(pads, 0) | Pad_GetRepeat(pads, 0));
    u8* d = B(self, kData);
    const s32 step = F(self, kStep);
    const bool idle = F(self, kPhase) == 2;
    const s32 result = F(self, kResult);
    const s32 slot = *gCurrentSlot;
    const bool afterResults = F(self, 0x150E4) != 0;
    const auto begin = [self](u32 table, int next) {
        if (StartTable(self, table) == 0)
            return 0;
        SetStep(self, next);
        return 1;
    };
    const auto afterSaving = [&] { SetState(self, afterResults ? 5 : 0xB); };
    const s32 half = (*gFrameRate * 30) / 30;           // (as the original spells one second)
    switch (F(self, kState)) {
    case 0:                                             // "Data to be saved: …"
        if (idle) {
            Fill(self, 0x33, slot, false);
            if (*reinterpret_cast<s32*>(*gPlayerData) == 1) {
                // A game not begun: the name, then "From the beginning".
                std::u16string text = sExtra[self].made[0x33];
                const std::u16string name(reinterpret_cast<const char16_t*>(*gPlayerData + 0x480));
                const size_t at = text.find(name);
                if (at != std::u16string::npos)
                    text = text.substr(0, at + name.size()) + u" " + Raw(self, 0x4E) + u"\"";
                sExtra[self].made[0x33] = text;
            }
            // (kept in the question's buffer: nothing is asked while it shows)
            static u32 line = 0;
            if (line == 0)
                line = guest::addr(guest::alloc(kTextRoom * 2));
            const std::u16string text = Raw(self, 0x33);
            const size_t n = text.size() < kTextRoom - 1 ? text.size() : kTextRoom - 1;
            memcpy(P(line), text.data(), n * 2);
            P<u16>(line)[n] = 0;
            WindowDesc desc;
            memset(&desc, 0, sizeof desc);
            WindowDesc_Init(&desc);
            desc.x = 0;
            desc.y = 0x78;
            desc.title = line;
            desc.z = 0xFFFFE6;
            desc.priority = 2;
            F(self, 0x150F0) = (s32)guest::addr(WindowMgr_OpenPassive(*gWindowMgr, &desc));
            SetState(self, 1);
        }
        break;
    case 1:                                             // look at the card
        if (step == 1) {
            if (!idle)
                break;
            if (Refused(self, result, 10, 2)) {
            } else if (result == kNoFile || result == kUnformatted) {
                SetState(self, 7);
            } else if (IsCorrupt(result)) {
                if (mc::free_kb(F(self, kPort)) < F(self, kNeeded)) {
                    *gWindowY = -30;
                    SetNotice(kNoticeWatched, 0x32);
                    SetState(self, 10);
                } else {
                    *B(self, 0x150EC) = 1;
                    BlankData(d);
                    WindowMgr_Clear(*gWindowMgr);
                    F(self, kMsgWindow) = F(self, kAskWindow) = F(self, 0x150F0) = 0;
                    SetState(self, 0xE);
                    static u32 line = 0;
                    if (line == 0)
                        line = guest::addr(guest::alloc(kTextRoom * 2));
                    const std::u16string text = Raw(self, 0x56);
                    memcpy(P(line), text.c_str(), (text.size() + 1) * 2);
                    WindowDesc desc;
                    memset(&desc, 0, sizeof desc);
                    WindowDesc_Init(&desc);
                    desc.title = line;
                    WindowMgr_Open(*gWindowMgr, &desc);
                }
            } else if (result == 0) {
                // Which question: the card was changed or not; the slot holds
                // rubbish, a game, or nothing.
                const bool changed = (mc::status(F(self, kPort)) & 0x400) != 0;     // 0x264CD0
                const bool fresh = *B(self, 0x150EC) != 0;
                s32 message;
                if (!fresh && SlotCheck(d, slot) != 0)
                    message = changed ? 0x14 : 0x2D;
                else if (!fresh && SlotUsed(d, slot))
                    message = changed ? (SlotFresh(d, slot) ? 0x16 : 0x10) : (SlotFresh(d, slot) ? 0x1A : 0x1B);
                else
                    message = changed ? 0x11 : 0x19;
                SetNotice(kOverwriteAsk, message);
                Fill(self, message, slot, true);
                SetState(self, 3);
            } else {
                SetNotice(kNoticeWatched, 0x18);
                SetState(self, 10);
            }
        } else if (step == 0) {
            *B(self, 0x150EC) = 0;
            return begin(kCheck, 1);
        }
        break;
    case 2:                                             // make the file, with the game in it
        if (step == 3) {
            if (--F(self, kCountdown) < 0)
                afterSaving();
        } else if (step == 2) {
            Say(self, 0xD);
            F(self, kCountdown) = half;
            SetStep(self, 3);
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                SetStep(self, 2);
            } else {
                SetNotice(kNotice, 0xE);
                SetState(self, 9);
            }
        } else if (step == 0) {
            BlankData(d);
            GameToSlot(d);
            return begin(kCreate, 1);
        }
        break;
    case 3:                                             // overwrite?
        if (step == 2) {
            if (!idle)
                break;
            if (result == kUnformatted) {
                SetState(self, 6);
            } else if (result == 0) {
                SetState(self, 2);
            } else {
                SetNotice(kNotice, 0x1E);
                SetState(self, 9);
            }
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                if (F(self, kAnswer) == 0)
                    SetState(self, *B(self, 0x150EC) == 0 ? 4 : 0xD);
                else
                    SetState(self, 8);
            } else if (result == kNotPs2 || result == kNoCard) {
                SetState(self, 1);
            }
        } else if (step == 0 && StartTable(self, kOverwriteAsk) != 0) {
            SetStep(self, 1);
        }
        break;
    case 4:                                             // write the slot
        if (step == 3) {
            if (--F(self, kCountdown) < 0)
                afterSaving();
        } else if (step == 2) {
            Say(self, 0x1D);
            F(self, kCountdown) = half;
            SetStep(self, 3);
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                SetStep(self, 2);
            } else {
                SetNotice(kNotice, 0x1E);
                SetState(self, 9);
            }
        } else if (step == 0) {
            GameToSlot(d);
            F(self, kWriteSlot) = slot;
            StartTable(self, kWrite);
            SetStep(self, 1);
        }
        break;
    case 5:
        return 0;
    case 6:                                             // format?
        if (step == 2) {
            if (!idle)
                break;
            if (result == 0) {
                SetState(self, 2);
            } else {
                SetNotice(kNotice, 8);
                SetState(self, 9);
            }
        } else if (step == 1) {
            if (!idle || result == kBusy)
                break;
            if (result != 0)
                SetState(self, 1);
            else if (F(self, kAnswer) == 0)
                return begin(kFormat, 2);
            else
                SetState(self, 8);
        } else if (step == 0) {
            return begin(kFormatAsk, 1);
        }
        break;
    case 7:                                             // no save file: make one?
        if (step == 2) {
            if (!idle)
                break;
            if (result == kUnformatted) SetState(self, 6);
            else if (result == 0) SetState(self, 2);
            else SetState(self, 1);
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                if (F(self, kAnswer) == 0)
                    return begin(kProbe, 2);
                SetState(self, 8);
            } else if (result == kNoRoom) {
                SetNotice(kNoticeWatched, 9);
                SetState(self, 10);
            } else if (result == kNoCard) {
                SetState(self, 1);
            }
        } else if (step == 0) {
            return begin(kCreateAsk, 1);
        }
        break;
    case 8:                                             // "Cancel save?" / "Quit without saving?"
        if (step == 1) {
            if (idle && result == 0)
                SetState(self, F(self, kAnswer) == 0 ? 5 : 1);
        } else if (step == 0) {
            StartTable(self, afterResults ? kQuitAskB : kCancelAsk);
            SetStep(self, 1);
        }
        break;
    case 9:
        if (step == 1) {
            if (idle)
                SetState(self, 1);
        } else if (step == 0) {
            StartTable(self, kNotice);
            SetStep(self, 1);
        }
        break;
    case 10:
        if (step == 1) {
            if (idle)
                SetState(self, result == 1 ? 1 : 8);
        } else if (step == 0) {
            *gWindowY = 0x14;
            StartTable(self, kNoticeWatched);
            SetStep(self, 1);
        }
        break;
    case 0xB:                                           // saved: "Quit game and return to title screen?"
        if (step == 1) {
            if (idle && result == 0) {
                F(self, 0x150E8) = F(self, kAnswer) == 0 ? 1 : 0;
                SetState(self, 5);
            }
        } else if (step == 0) {
            CloseWindow(self, 0x150F0);
            return begin(kQuitAsk, 1);
        }
        break;
    case 0xC:                                           // the file is bad: make it again?
        if (step == 4) {
            if (--F(self, kCountdown) < 0)
                afterSaving();
        } else if (step == 3) {
            Say(self, 0xD);
            F(self, kCountdown) = half;
            SetStep(self, 4);
        } else if (step == 2) {
            if (!idle)
                break;
            if (result == 0) {
                SetStep(self, 3);
            } else {
                SetNotice(kNotice, 0xE);
                SetState(self, 9);
            }
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                if (F(self, kAnswer) == 0) {
                    StartTable(self, kRecreate);
                    SetStep(self, 2);
                } else {
                    SetState(self, 8);
                }
            } else if (result == kNoCard) {
                SetState(self, 1);
            }
        } else if (step == 0) {
            return begin(kRecreateAsk, 1);
        }
        break;
    case 0xD:                                           // the file again, with the game in it
        if (step == 3) {
            if (--F(self, kCountdown) < 0)
                afterSaving();
        } else if (step == 2) {
            Say(self, 0x1D);
            F(self, kCountdown) = half;
            SetStep(self, 3);
        } else if (step == 1) {
            if (!idle)
                break;
            if (result == 0) {
                *B(self, 0x150EC) = 0;
                SetStep(self, 2);
            } else {
                SetNotice(kNotice, 0x1E);
                SetState(self, 9);
            }
        } else if (step == 0) {
            BlankData(d);
            GameToSlot(d);
            StartTable(self, kRewrite);
            SetStep(self, 1);
        }
        break;
    case 0xE:                                           // "the save file is corrupt": a button
        if (step == 1) {
            if (!idle)
                break;
            if (pressed & (kPadCross | kPadTriangle)) {
                WindowMgr_Clear(*gWindowMgr);
                F(self, kMsgWindow) = F(self, kAskWindow) = 0;
                SetState(self, 0xC);
            }
            if (result == kNoCard) {
                WindowMgr_Clear(*gWindowMgr);
                F(self, kMsgWindow) = F(self, kAskWindow) = 0;
                SetState(self, 1);
            }
        } else if (step == 0) {
            StartTable(self, kWaitGone);
            SetStep(self, 1);
        }
        break;
    }
    return 1;
}

}  // extern "C"

namespace {

// ═══ The load screen (0x257000–0x25E270) ══════════════════════════════════
// What Start on the title leads to: the three slots of the save file, with
// what is in the one under the cursor; Start / Copy / Delete / Back for a
// slot; and the questions on the way (no save file: make one?  play without
// saving?).  It ends with its answer in the byte at +0x15151: 1 = on to the
// main menu (with the chosen slot's game in memory), 0 = back.
//
// The screen is layout 9 (McardSelect) under fifteen lines of text — the
// table at 0x2DA6A0: 0–2 the slots, 3–6 the menu, 7–11 the slot's details
// (8, the money, is never drawn), 12 / 13 Yes / No, 14 the button hints —
// whose colours pulse where the cursor is.
enum : u32 {
    kCur = 0x150E0, kListShown = 0x150E4, kCurColour = 0x150E8, kCurDir = 0x150EC, kCurT = 0x150F0,
    kCopyTo = 0x150F4, kCopyColour = 0x150F8, kCopyDir = 0x150FC, kCopyT = 0x15100,
    kMenu = 0x15104, kMenuShown = 0x15108, kMenuColour = 0x1510C, kMenuDir = 0x15110, kMenuT = 0x15114,
    kYesNo = 0x15118, kYesNoShown = 0x1511C, kYesNoColour = 0x15120, kYesNoDir = 0x15124, kYesNoT = 0x15128,
    kDetailsShown = 0x1512C, kAccessing = 0x15130, kHintShown = 0x15134, kLayout = 0x15138, kListAt = 0x1513C,
    kLayoutReady = 0x15150, kAnswerByte = 0x15151, kFileFresh = 0x15154,
    kVtLoad = 0x2ECAF8u,
};
struct Line {
    u32  text = 0;          // 16-bit string in guest memory
    bool shown = false, single = true, shadow = false;
    float x = 0, y = 0;
    u32  colour = 0x80808080u;
};
std::map<void*, std::vector<Line>> sLines;
std::map<void*, bool> sInstant;     // (native: a scripted run skips the screen)
int sBlink = 0;                     // 0x2EE758

extern "C" {
    void  Sound_PlaySE(int id, int positional, int volume);     // 0x180E70
    int   Movie_Wanted(const char* name);                       // native/movie.cpp
    bool  hle_load_save();                                      // the single file older builds wrote
    void  hle_debug_after_load();                               // WOTS2_GIVE_ITEMS
}
#include "engine/screen_layout.h"

inline float& Ff(void* self, u32 at) { return *reinterpret_cast<float*>(static_cast<u8*>(self) + at); }
inline ScreenLayout* Layout(void* self) { return P<ScreenLayout>((u32)F(self, kLayout)); }
inline Line& L(void* self, int i) { return sLines[self][(size_t)i]; }
void SetLine(void* self, int i, const std::u16string& text) {
    u16* room = P<u16>(L(self, i).text);
    const size_t n = text.size() < kTextRoom - 1 ? text.size() : kTextRoom - 1;
    memcpy(room, text.data(), n * 2);
    room[n] = 0;
}
std::u16string GetLine(void* self, int i) {
    return std::u16string(reinterpret_cast<const char16_t*>(P<u16>(L(self, i).text)));
}
inline u32 Rgb(float r, float g, float b) { return (u32)(int)r | (u32)(int)g << 8 | (u32)(int)b << 16 | 0x80000000u; }
// t runs up and down between two ends; → the new t.
void Bounce(void* self, u32 dirAt, u32 tAt, float step, float low) {
    if (F(self, dirAt) < 1) {
        Ff(self, tAt) -= step;
        if (Ff(self, tAt) < low) {
            Ff(self, tAt) = low;
            F(self, dirAt) = 1;
        }
    } else {
        Ff(self, tAt) += step;
        if (Ff(self, tAt) > 1.0f) {
            Ff(self, tAt) = 1.0f;
            F(self, dirAt) = -1;
        }
    }
}
int UpDown() {
    void* pads = PadMgr_Get();
    const u16 pressed = (u16)(Pad_GetPressed(pads, 0) | Pad_GetRepeat(pads, 0));
    return (pressed & kPadUp) ? -1 : (pressed & kPadDown) ? 1 : 0;
}
// 0x25DBA0: a slot's colour — 0 under the cursor, 1 its dim end, 2 otherwise.
u32 SlotColour(void* self, int kind, int slot) {
    const bool ok = SlotCheck(B(self, kData), slot) == 0;
    if (kind == 1) return ok ? 0x80AFAAA0u : 0x805A50A0u;
    if (kind == 0) return ok ? 0x80D7E4D3u : 0x805A50F0u;
    return ok ? 0x806A615Au : 0x80464678u;
}
// The panel behind the slot under the cursor (layout group 4): where, and how bright.
void ListPanel(void* self, int slot, int level) {
    s32* at = reinterpret_cast<s32*>(B(self, kListAt));
    at[0] = 8;
    at[1] = slot * -0x1E - 0x30;
    ScreenLayout_SetGroupOffset(Layout(self), at, 4, -1);
    if (level < 0)
        return;
    if (LayoutItem* item = const_cast<LayoutItem*>(ScreenLayout_FindItem(Layout(self), 4)))
        for (LayoutVertex& v : item->v)
            v.rgba = (u32)level;
}
void SetAccessing(void* self, bool on) {
    if (F(self, kAccessing) != 0) {
        F(self, kAccessing) = 0;
        ScreenLayout_HideGroup(Layout(self), 3);
    }
    if (on) {
        F(self, kAccessing) = 1;
        ScreenLayout_ShowGroup(Layout(self), 3);
    }
}
void HideHint(void* self) {                             // 0x25A450
    if (F(self, kHintShown) != 0) {
        F(self, kHintShown) = 0;
        L(self, 14).shown = false;
    }
}
void ShowHint(void* self) {                             // 0x25A4E0
    HideHint(self);
    F(self, kHintShown) = 1;
    L(self, 14).shown = true;
}
void HideDetails(void* self) {                          // 0x25A570
    if (F(self, kDetailsShown) == 0)
        return;
    F(self, kDetailsShown) = 0;
    ScreenLayout_HideGroup(Layout(self), 0);
    for (int i = 0; i < 8; i++)
        ScreenLayout_HideGroup(Layout(self), i + 0xB);
    ScreenLayout_HideGroup(Layout(self), 10);
    for (int i = 0; i < 5; i++)
        L(self, i + 7).shown = false;
}
std::u16string Padded(int n) {
    const std::u16string digits = Number(n);
    return (digits.size() < 4 ? std::u16string(4 - digits.size(), u'　') : std::u16string()) + digits;
}
// 0x25A680: what is in a slot — the day, the sword, health and vigour, the face.
void ShowDetails(void* self, int slot) {
    HideDetails(self);
    u8* d = B(self, kData);
    if (!SlotUsed(d, slot) || F(self, kFileFresh) != 0 || SlotCheck(d, slot) != 0)
        return;
    F(self, kDetailsShown) = 1;
    const u8* s = Slot(d, slot);
    const auto h = [s](u32 at) { s16 v; memcpy(&v, s + at, 2); return (int)v; };
    int head = h(0x116);
    if (h(0x114) != 0)
        head = 8;                                       // (a secret character has no picture)
    for (int i = 0; i < 8; i++) {
        if (i == head) ScreenLayout_ShowGroup(Layout(self), i + 0xB);
        else ScreenLayout_HideGroup(Layout(self), i + 0xB);
    }
    if (head < 8)
        ScreenLayout_ShowGroup(Layout(self), 10);
    ScreenLayout_ShowGroup(Layout(self), 0);
    for (int i = 0; i < 5; i++)
        L(self, i + 7).shown = true;
    if (!SlotFresh(d, slot)) {
        sExtra[self].made[0x44] = Raw(self, 0x44);      // "Day %d  %p"
        sExtra[self].made.erase(0x44);
        Fill(self, 0x44, slot, true);
        SetLine(self, 7, Raw(self, 0x3C) + Raw(self, 0x44));
        sExtra[self].made.erase(0x44);
    } else {
        SetLine(self, 7, Raw(self, 0x4E));
    }
    SetLine(self, 8, Raw(self, 0x3D) + Padded(h(0x106)) + Raw(self, 0x4A));
    // (0x266250) the sword in hand, by name
    const u16* sword = Text(0x11C);
    for (int i = 0; i < 3; i++)
        if (h(0x17A + (u32)i * 2) == 0) {
            sword = reinterpret_cast<const u16*>(s + 0x180 + (u32)i * 0x100);
            break;
        }
    SetLine(self, 9, sword ? std::u16string(reinterpret_cast<const char16_t*>(sword)) : std::u16string());
    SetLine(self, 10, Raw(self, 0x3F) + Padded(h(0x10E)) + Raw(self, 0x4B) + Padded(h(0x110)));
    SetLine(self, 11, Raw(self, 0x40) + Padded(h(0x108)) + Raw(self, 0x4B) + Padded(h(0x10A)));
}
void HideList(void* self) {                             // 0x25DC80
    if (F(self, kListShown) == 0)
        return;
    F(self, kListShown) = 0;
    ScreenLayout_HideGroup(Layout(self), 4);
    ScreenLayout_HideGroup(Layout(self), 5);
    for (int i = 0; i < 3; i++)
        L(self, i).shown = false;
}
void ShowList(void* self) {                             // 0x25D350
    HideList(self);
    u8* d = B(self, kData);
    const int cur = F(self, kCur);
    ScreenLayout_ShowGroup(Layout(self), 5);
    ScreenLayout_ShowGroup(Layout(self), 4);
    ListPanel(self, cur, -1);
    F(self, kListShown) = 1;
    F(self, kCurColour) = (s32)SlotColour(self, 0, cur);
    F(self, kCurDir) = 1;
    Ff(self, kCurT) = 1.0f;
    for (int i = 0; i < 3; i++) {
        std::u16string text = Raw(self, 0x41 + i);
        if (SlotCheck(d, i) == 0) {
            const u16* name = SlotUsed(d, i) ? reinterpret_cast<const u16*>(Slot(d, i) + 0x480) : Text(0x11B);
            if (name != nullptr)
                text += reinterpret_cast<const char16_t*>(name);
        } else {
            text += Raw(self, 0x4F);
        }
        SetLine(self, i, text);
        L(self, i).shown = true;
        L(self, i).colour = SlotColour(self, i == cur ? 0 : 2, i);
    }
    ShowDetails(self, cur);
}
void MenuColours(void* self, int first, int count, int cursor, u32 colour) {
    for (int i = 0; i < count; i++) {
        Line& line = L(self, first + i);
        line.shown = true;
        line.colour = i == cursor ? colour : 0x80000000u;
        line.shadow = i == cursor;
    }
}
void HideMenu(void* self) {                             // 0x25C590
    if (F(self, kMenuShown) == 0)
        return;
    F(self, kMenuShown) = 0;
    ScreenLayout_HideGroup(Layout(self), 1);
    ScreenLayout_HideGroup(Layout(self), 2);
    for (int i = 0; i < 4; i++)
        L(self, i + 3).shown = false;
    HideHint(self);
}
void ShowMenu(void* self) {                             // 0x25C270
    HideMenu(self);
    F(self, kMenuShown) = 1;
    ScreenLayout_ShowGroup(Layout(self), 1);
    MenuColours(self, 3, 4, F(self, kMenu), (u32)F(self, kMenuColour));
    Sound_PlaySE(0, 0, 0x3FFF);
    ShowHint(self);
}
void MenuCursor(void* self) {                           // 0x25BD50
    if (const int move = UpDown()) {
        F(self, kMenuColour) = (s32)0x8095A8C8u;
        F(self, kMenuDir) = 1;
        Ff(self, kMenuT) = 1.0f;
        F(self, kMenu) = (F(self, kMenu) + move + 4) % 4;
        Sound_PlaySE(2, 0, 0x3FFF);
    }
    const float t = Ff(self, kMenuT);
    F(self, kMenuColour) = (s32)Rgb(t * 200.0f, t * 168.0f, t * 149.0f);
    Bounce(self, kMenuDir, kMenuT, 0.05f, 0.2f);
    MenuColours(self, 3, 4, F(self, kMenu), (u32)F(self, kMenuColour));
}
void HideYesNo(void* self) {                            // 0x25BC70
    if (F(self, kYesNoShown) == 0)
        return;
    F(self, kYesNoShown) = 0;
    ScreenLayout_HideGroup(Layout(self), 2);
    for (int i = 0; i < 2; i++)
        L(self, i + 12).shown = false;
    HideHint(self);
}
void ShowYesNo(void* self) {                            // 0x25B950
    HideYesNo(self);
    F(self, kYesNoShown) = 1;
    ScreenLayout_ShowGroup(Layout(self), 2);
    MenuColours(self, 12, 2, F(self, kYesNo), (u32)F(self, kYesNoColour));
    Sound_PlaySE(0, 0, 0x3FFF);
    ShowHint(self);
}
void YesNoCursor(void* self) {                          // 0x25B440
    if (const int move = UpDown()) {
        F(self, kYesNoColour) = (s32)0x80D7E4D3u;
        F(self, kYesNoDir) = 1;
        Ff(self, kYesNoT) = 1.0f;
        F(self, kYesNo) = (F(self, kYesNo) + move + 2) % 2;
        Sound_PlaySE(2, 0, 0x3FFF);
    }
    const float t = Ff(self, kYesNoT);
    F(self, kYesNoColour) = (s32)Rgb(t * 211.0f, t * 228.0f, t * 215.0f);
    Bounce(self, kYesNoDir, kYesNoT, 0.05f, 0.0f);
    MenuColours(self, 12, 2, F(self, kYesNo), (u32)F(self, kYesNoColour));
}
// 0x25CC40: the cursor on the slots.
void ListCursor(void* self) {
    if (const int move = UpDown()) {
        F(self, kCur) = (F(self, kCur) + move + 3) % 3;
        F(self, kCurColour) = (s32)SlotColour(self, 0, F(self, kCur));
        F(self, kCurDir) = 1;
        Ff(self, kCurT) = 1.0f;
        F(self, kWriteSlot) = F(self, kCur);
        Sound_PlaySE(2, 0, 0x3FFF);
        ShowDetails(self, F(self, kCur));
    }
    const int cur = F(self, kCur);
    const u32 bright = SlotColour(self, 0, cur), dim = SlotColour(self, 1, cur);
    const float t = Ff(self, kCurT), u = 1.0f - t;
    const auto mix = [&](int shift) { return (float)((dim >> shift) & 0xFF) * u + (float)((bright >> shift) & 0xFF) * t; };
    F(self, kCurColour) = (s32)Rgb(mix(0), mix(8), mix(16));
    Bounce(self, kCurDir, kCurT, 0.025f, 0.0f);
    for (int i = 0; i < 3; i++) {
        L(self, i).shown = true;
        L(self, i).colour = i == cur ? (u32)F(self, kCurColour) : SlotColour(self, 2, i);
    }
    const float now = Ff(self, kCurT), rest = 1.0f - now, lit = now * 128.0f;
    ListPanel(self, cur, (int)((u32)(int)(lit + rest * 58.0f) | (u32)(int)(lit + rest * 56.0f) << 8 |
                               (u32)(int)(lit + rest * 64.0f) << 16 | (u32)(int)(lit + rest * 128.0f) << 24));
}
// 0x25C690: …and the one that picks where a copy goes.
void CopyCursor(void* self) {
    if (const int move = UpDown()) {
        F(self, kCopyColour) = (s32)0x80B4C8F0u;
        F(self, kCopyDir) = 1;
        Ff(self, kCopyT) = 1.0f;
        F(self, kCopyTo) = (F(self, kCopyTo) + move + 3) % 3;
        Sound_PlaySE(2, 0, 0x3FFF);
        ShowDetails(self, F(self, kCopyTo));
    }
    const float t = Ff(self, kCopyT), u = 1.0f - t;
    F(self, kCopyColour) = (s32)Rgb(u * 90.0f + t * 240.0f, u * 97.0f + t * 200.0f, u * 106.0f + t * 180.0f);
    Bounce(self, kCopyDir, kCopyT, 0.05f, 0.0f);
    for (int i = 0; i < 3; i++) {
        L(self, i).shown = true;
        if (i == F(self, kCur)) L(self, i).colour = SlotColour(self, 0, i);
        else if (i == F(self, kCopyTo)) L(self, i).colour = (u32)F(self, kCopyColour);
        else L(self, i).colour = SlotColour(self, 2, i);
    }
    ListPanel(self, F(self, kCopyTo), -1);
}
void HideAll(void* self) {                              // 0x25A3E0
    HideList(self);
    HideMenu(self);
    HideDetails(self);
    SetAccessing(self, false);
}
// 0x25D900: the line that says what to do, with the slot's difficulty after it.
void StatusLine(void* self, int message, int slot) {
    Extra& e = sExtra[self];
    e.made.erase(message);
    std::u16string text = Raw(self, message);
    size_t at = text.find(u'.');
    at = (at == std::u16string::npos ? text.size() : at) + 3;
    if (text.size() < at)
        text.resize(at, u' ');
    text.resize(at);
    u8* d = B(self, kData);
    if (!SlotFresh(d, slot) && SlotUsed(d, slot) && F(self, kFileFresh) == 0 && SlotCheck(d, slot) == 0) {
        const u8* s = Slot(d, slot);
        int difficulty = (s8)s[0x105];
        if (difficulty < 0 || difficulty > 2)
            difficulty = 0;
        if (s[0x104] != 0)
            difficulty = 3;
        text += Raw(self, 0x50) + Raw(self, 0x51 + difficulty);
    }
    e.made[message] = text;
}
// The colours of two slots and the panel back as they are at rest.
void RestColours(void* self) {
    L(self, F(self, kCopyTo)).colour = SlotColour(self, 2, F(self, kCopyTo));
    L(self, F(self, kCur)).colour = SlotColour(self, 0, F(self, kCur));
    ListPanel(self, F(self, kCur), (int)0x80808080u);
}
// 0x265BF0 / 0x265B50: a slot copied onto another; a slot emptied.
void CopySlot(u8* d, int from, int to) {
    memcpy(Slot(d, to), Slot(d, from), kSlotSize);
    memcpy(SlotConfig(d, to), SlotConfig(d, from), 0x40);
    Reseal(d);
}
void ClearSlot(u8* d, int slot) {
    BlankSlot(Slot(d, slot));
    BlankConfig(SlotConfig(d, slot));
    Reseal(d);
}
void OpenNotice(void* self, int message) {
    static u32 line = 0;
    if (line == 0)
        line = guest::addr(guest::alloc(kTextRoom * 2));
    const std::u16string text = Raw(self, message);
    const size_t n = text.size() < kTextRoom - 1 ? text.size() : kTextRoom - 1;
    memcpy(P(line), text.data(), n * 2);
    P<u16>(line)[n] = 0;
    WindowDesc desc;
    memset(&desc, 0, sizeof desc);
    WindowDesc_Init(&desc);
    desc.title = line;
    WindowMgr_Open(*gWindowMgr, &desc);
}
void ClearWindows(void* self) {
    WindowMgr_Clear(*gWindowMgr);
    F(self, kMsgWindow) = F(self, kAskWindow) = 0;
}
// The slot used last off the card into the game, without the screen.
int LoadLast() {
    static u32 room = 0;
    if (room == 0)
        room = guest::addr(guest::alloc(kSaveSize));
    u8* d = P<u8>(room);
    mc::get_info(0);
    mc::open(0, kDataPath, 1);
    if (!mc::is_open())
        return -1;
    mc::read(d, kSaveSize);
    const bool whole = mc::result() == (s32)kSaveSize;
    mc::close();
    if (!whole || VerifyData(d) != 0)
        return -1;
    s32 slot;
    memcpy(&slot, d + 4, 4);
    if (slot < 0 || slot > 2 || SlotCheck(d, slot) != 0 || !SlotUsed(d, slot))
        return -1;
    *gCurrentSlot = slot;
    SlotToGame(d);
    return slot;
}

}  // namespace

extern "C" {

void Mode1_Start(void* self) {                          // 0x25DD70
    F(self, kPort) = 0;
    SetState(self, 0);
    F(self, kFace) = 1;
    F(self, kMenu) = 0;
    F(self, kMenuColour) = (s32)0x8095A8C8u;
    F(self, kMenuDir) = 1;
    Ff(self, kMenuT) = 1.0f;
    F(self, kCur) = 0;
    F(self, kCurColour) = (s32)0x80D7E4D3u;
    F(self, kCurDir) = 1;
    Ff(self, kCurT) = 1.0f;
    F(self, kYesNo) = 0;
    F(self, kYesNoColour) = (s32)0x80D7E4D3u;
    F(self, kYesNoDir) = 1;
    Ff(self, kYesNoT) = 1.0f;
    if (F(self, kLayout) == 0) {
        void* mem = MemAlloc(0x30);
        F(self, kLayout) = mem ? (s32)guest::addr(ScreenLayout_Ctor(static_cast<ScreenLayout*>(mem))) : 0;
    }
    if (F(self, kLayout) != 0) {
        ScreenLayout_Init(Layout(self));
        Layout(self)->id = 9;
    }
    *B(self, kLayoutReady) = 0;
}
void* Mode1_Ctor(void* mem) {                           // 0x25E270
    BaseCtor(mem);
    *static_cast<u32*>(mem) = kVtLoad;
    for (u32 at : std::initializer_list<u32>{kListShown, kMenuShown, kYesNoShown, kDetailsShown, kHintShown,
                                             kAccessing, kLayout, kFileFresh})
        F(mem, at) = 0;
    *B(mem, kAnswerByte) = 1;
    sLines[mem].clear();
    // (Native: a run steered by debug hooks goes without the screen, like the
    // movies, so that scripted input keeps its frame numbers: the slot saved
    // last is loaded — or the single file older builds wrote.)
    sInstant[mem] = Movie_Wanted("LOADSCREEN") == 0;
    if (sInstant[mem]) {
        const int slot = LoadLast();
        if (slot >= 0)
            printf("[save] loaded slot %d from the memory card directory\n", slot + 1);
        else
            hle_load_save();
        hle_debug_after_load();
        return mem;
    }
    Mode1_Start(mem);
    return mem;
}
void* Mode1_Dtor(void* self, int flag) {                // 0x25DEE0
    if (self == nullptr)
        return self;
    if (!sInstant[self]) {
        for (Line& line : sLines[self])
            guest::free(P(line.text));
        if (F(self, kLayout) != 0)
            ScreenLayout_Dtor(Layout(self), 1);
        F(self, kLayout) = 0;
    }
    sLines.erase(self);
    sInstant.erase(self);
    BaseDtor(self);
    if ((s16)flag > 0)
        MemFree(self);
    return self;
}

// → 0 when it is over.
int Mode1_Update(void* self) {                          // 0x257550
    if (sInstant[self]) {
        *B(self, kAnswerByte) = 1;
        return 0;
    }
    StepCommon(self);
    if (*B(self, kLayoutReady) == 0) {
        if (!Layout(self)->ready) {
            ScreenLayout_Poll(Layout(self));
        } else {
            *B(self, kLayoutReady) = 1;
            for (int group : {5, 3, 0, 1, 2, 4, 10})
                ScreenLayout_HideGroup(Layout(self), group);
            for (int i = 0; i < 8; i++)
                ScreenLayout_HideGroup(Layout(self), i + 0xB);
            if (LayoutItem* item = const_cast<LayoutItem*>(ScreenLayout_FindItem(Layout(self), 4)))
                item->flags = (item->flags & 0xFFFFFF0Fu) | 0x10;       // 0x168360: the panel is additive
        }
    } else if (F(self, kState) != 0 && F(self, kLayout) != 0) {
        ScreenLayout_Poll(Layout(self));
    }

    u8* d = B(self, kData);
    const s32 step = F(self, kStep);
    const bool idle = F(self, kPhase) == 2;
    const s32 result = F(self, kResult);
    const u32 button = (u32)F(self, kButton);
    const int cur = F(self, kCur);
    const bool confirmed = (button & kPadCross) != 0 && F(self, kYesNo) == 0;
    const auto begin = [self](u32 table, int next) {
        if (StartTable(self, table) == 0)
            return false;
        SetStep(self, next);
        return true;
    };
    const auto se = [](int id) { Sound_PlaySE(id, 0, 0x3FFF); };
    const u32 kWait = 0x2DA8D0u, kWaitList = 0x2DA8E0u, kWaitMenu = 0x2DA8F0u, kClear = 0x2DA950u,
              kCreateAsk2 = 0x2DA880u, kFormatAsk2 = 0x2DA8B0u, kNoSaveAsk = 0x2DA860u, kCopy = 0x2DAD30u,
              kCopyNew = 0x2DAC20u, kDelete = 0x2DAD50u, kDeleteNew = 0x2DAC50u;

    switch (F(self, kState)) {
    case 0:                                             // the lines of text
        if (idle && *B(self, kLayoutReady) != 0) {
            SetState(self, 1);
            std::vector<Line>& lines = sLines[self];
            lines.clear();
            const s32* row = P<s32>(0x2DA6A0u);
            for (int i = 0; row[0] != -999; row += 7, i++) {
                Line line;
                line.text = guest::addr(guest::alloc(kTextRoom * 2));
                line.shown = row[6] & 0xFF;
                line.x = (float)(row[4] - 0x140);
                line.y = (float)(row[5] - 0xE0);
                line.colour = (u32)row[3];
                line.shadow = ((row[6] >> 8) & 0xFF) != 0;
                line.single = ((row[6] >> 16) & 0xFF) != 0;
                lines.push_back(line);
                std::u16string text = Raw(self, row[2]);
                if (i < 3)
                    text += Raw(self, SlotCheck(d, i) == 0 ? 0x3B : 0x45);
                SetLine(self, i, text);
            }
        }
        break;
    case 1:                                             // look at the card
        if (step == 1) {
            if (!idle)
                break;
            SetAccessing(self, false);
            if (Refused(self, result, 0x12, 2)) {
            } else if (result == kNoFile || result == kUnformatted) {
                SetState(self, 0xF);
            } else if (IsCorrupt(result)) {
                if (mc::free_kb(F(self, kPort)) < F(self, kNeeded)) {
                    SetNotice(kNoticeWatched, 0x34);
                    SetState(self, 0x12);
                } else {
                    F(self, kFileFresh) = 1;
                    BlankData(d);
                    SetState(self, 3);
                    OpenNotice(self, 0x56);
                }
            } else if (result == 0) {
                if (SlotCheck(d, 0) == 0 && SlotCheck(d, 1) == 0 && SlotCheck(d, 2) == 0) {
                    SetState(self, 4);
                    return 1;
                }
                SetState(self, 2);
                OpenNotice(self, 0x55);
            } else {
                SetNotice(kNoticeWatched, 0x18);
                SetState(self, 0x12);
            }
        } else if (step == 0) {
            BlankData(d);
            HideAll(self);
            SetAccessing(self, true);
            F(self, kFileFresh) = 0;
            StartTable(self, kCheck);
            SetStep(self, 1);
        }
        break;
    case 2:                                             // "data may be corrupt": a button
    case 3:
        if (step == 1) {
            if (!idle)
                break;
            if (button & (kPadCross | kPadTriangle)) {
                ClearWindows(self);
                SetState(self, F(self, kState) == 2 ? 4 : 0x16);
            }
            if (result == kNoCard) {
                ClearWindows(self);
                SetState(self, 1);
            }
        } else if (step == 0) {
            StartTable(self, kWaitGone);
            SetStep(self, 1);
        }
        break;
    case 4:
        if (step == 1) {
            if (idle) {
                SetState(self, 5);
                s32 last;
                memcpy(&last, d + 4, 4);
                if (last < 0 || last > 2)
                    last = -1;
                F(self, kCur) = F(self, kWriteSlot) = last < 0 ? 0 : last;
            }
        } else if (step == 0) {
            StartTable(self, kClear);
            SetStep(self, 1);
        }
        break;
    case 5:                                             // choose a slot
        if (step == 1) {
            ListCursor(self);
            StatusLine(self, 0x12, F(self, kCur));
            Say(self, 0x12);
            if (!idle)
                break;
            HideHint(self);
            if (button & kPadCross) {
                se(5);
                L(self, F(self, kCur)).colour = SlotColour(self, 0, F(self, kCur));
                ListPanel(self, F(self, kCur), (int)0x80808080u);
                SetState(self, 6);
            } else if (button & kPadTriangle) {
                se(7);
                L(self, F(self, kCur)).colour = 0x806A615Au;
                SetState(self, 0x13);
            } else {
                se(7);
                SetState(self, 1);
            }
        } else if (step == 0) {
            HideDetails(self);
            SetAccessing(self, false);
            HideMenu(self);
            ShowList(self);
            ShowHint(self);
            StartTable(self, kWaitList);
            StatusLine(self, 0x12, cur);
            Say(self, 0x12);
            SetStep(self, 1);
        }
        break;
    case 6:                                             // Start / Copy / Delete / Back
        if (step == 2) {
            HideMenu(self);
            SetState(self, 5);
            F(self, kMenu) = 0;
        } else if (step == 1) {
            MenuCursor(self);
            if (!idle)
                break;
            if ((button & kPadCross) == 0) {
                se(7);
                if ((button & kPadTriangle) == 0) {
                    F(self, kMenu) = 0;
                    SetState(self, 1);
                } else {
                    HideMenu(self);
                    SetStep(self, 2);
                }
            } else {
                HideMenu(self);
                const int choice = F(self, kMenu);
                L(self, choice + 3).colour = 0x8095A8C8u;
                if (choice == 3) {
                    SetStep(self, 2);
                    se(7);
                } else {
                    SetState(self, choice == 2 ? 9 : choice == 1 ? 8 : 7);
                    se(5);
                }
            }
        } else if (step == 0) {
            Say(self, 0x13);
            ShowMenu(self);
            StartTable(self, kWaitMenu);
            SetStep(self, 1);
        }
        break;
    case 7:                                             // "Start game with this data?"
        if (step == 1) {
            YesNoCursor(self);
            if (!idle)
                break;
            HideYesNo(self);
            if (result == 0) {
                se(confirmed ? 5 : 7);
                SetState(self, confirmed ? 10 : 6);
            } else {
                HideAll(self);
                F(self, kMenu) = 0;
                SetState(self, 1);
                se(7);
            }
        } else if (step == 0) {
            if (SlotCheck(d, cur) == 0) {
                SetAccessing(self, false);
                F(self, kYesNo) = 0;
                ShowYesNo(self);
                Say(self, 0x31);
                StartTable(self, kWait);
                SetStep(self, 1);
            } else {
                HideMenu(self);
                HideHint(self);
                SetNotice(kNoticeWatched, 10);
                SetState(self, 0x15);
            }
        }
        break;
    case 8:                                             // copy: where to?
        switch (step) {
        case 0:
            if (SlotCheck(d, cur) == 0) {
                F(self, kCopyTo) = cur;
                StartTable(self, kWaitList);
                SetStep(self, 1);
                ShowHint(self);
                StatusLine(self, 0x1F, cur);
                Say(self, 0x1F);
            } else {
                HideMenu(self);
                HideHint(self);
                SetNotice(kNoticeWatched, 0x30);
                SetState(self, 0x15);
            }
            break;
        case 1:
            CopyCursor(self);
            StatusLine(self, 0x1F, F(self, kCopyTo));
            Say(self, 0x1F);
            if (!idle)
                break;
            HideHint(self);
            if ((button & kPadCross) == 0) {
                if ((button & kPadTriangle) == 0) {
                    F(self, kMenu) = 0;
                    SetState(self, 1);
                } else {
                    SetState(self, 6);
                }
                RestColours(self);
                se(7);
                ShowDetails(self, cur);
            } else {
                se(5);
                L(self, F(self, kCopyTo)).colour = 0x80B4C8F0u;
                SetStep(self, F(self, kCopyTo) == cur ? 0 : 2);
            }
            break;
        case 2:
            F(self, kYesNo) = 1;
            ShowYesNo(self);
            Say(self, 0x20);
            StartTable(self, kWait);
            SetStep(self, 3);
            break;
        case 3:
            YesNoCursor(self);
            if (!idle)
                break;
            HideYesNo(self);
            if (result == 0) {
                if (!confirmed) {
                    SetState(self, 6);
                    RestColours(self);
                    se(7);
                    ShowDetails(self, cur);
                } else {
                    se(5);
                    SetStep(self, 4);
                }
            } else {
                F(self, kMenu) = 0;
                se(7);
                SetState(self, 1);
            }
            break;
        case 4:
            CopySlot(d, cur, F(self, kCopyTo));
            F(self, kWriteSlot) = F(self, kCopyTo);
            StartTable(self, F(self, kFileFresh) == 0 ? kCopy : kCopyNew);
            F(self, kFileFresh) = 0;
            SetStep(self, 5);
            SetAccessing(self, true);
            break;
        case 5:
            if (!idle)
                break;
            F(self, kWriteSlot) = cur;
            SetAccessing(self, false);
            if (result == 0) {
                SetState(self, 6);
                RestColours(self);
                ShowDetails(self, cur);
                ShowList(self);
            } else {
                F(self, kMenu) = 0;
                HideAll(self);
                SetNotice(kNotice, 0x23);
                SetState(self, 0x11);
            }
            break;
        }
        break;
    case 9:                                             // delete?
        if (step == 3) {
            if (!idle)
                break;
            SetAccessing(self, false);
            if (result == 0) {
                ShowDetails(self, cur);
                ShowList(self);
                SetState(self, 6);
            } else {
                F(self, kMenu) = 0;
                HideAll(self);
                SetNotice(kNotice, 0x27);
                SetState(self, 0x11);
            }
        } else if (step == 2) {
            ClearSlot(d, cur);
            StartTable(self, F(self, kFileFresh) == 0 ? kDelete : kDeleteNew);
            F(self, kFileFresh) = 0;
            SetStep(self, 3);
            SetAccessing(self, true);
        } else if (step == 1) {
            YesNoCursor(self);
            if (!idle)
                break;
            HideYesNo(self);
            if (result == 0) {
                se(confirmed ? 5 : 7);
                if (confirmed) SetStep(self, 2);
                else SetState(self, 6);
            } else {
                F(self, kMenu) = 0;
                HideMenu(self);
                HideList(self);
                SetState(self, 1);
                se(7);
            }
        } else if (step == 0) {
            F(self, kYesNo) = 1;
            ShowYesNo(self);
            Say(self, 0x24);
            StartTable(self, kWait);
            SetStep(self, 1);
        }
        break;
    case 10:                                            // the slot's game into memory
        *gCurrentSlot = cur;
        F(self, kWriteSlot) = cur;
        SlotToGame(d);
        mc::clear_flags(F(self, kPort), 0x400);
        SetState(self, 0xC);
        break;
    case 0xB:                                           // make the save file
        if (step == 1) {
            if (!idle)
                break;
            SetAccessing(self, false);
            if (result == 0) {
                SetState(self, 4);
            } else {
                SetNotice(kNotice, 0xE);
                SetState(self, 0x11);
            }
        } else if (step == 0) {
            if (!begin(kCreate, 1))
                return 0;
            SetAccessing(self, true);
        }
        break;
    case 0xC:
        *B(self, kAnswerByte) = 1;
        hle_debug_after_load();
        return 0;
    case 0xD:                                           // format?
        if (step == 2) {
            if (!idle)
                break;
            SetAccessing(self, false);
            if (result == 0) {
                SetState(self, 0xB);
            } else {
                SetNotice(kNotice, 8);
                SetState(self, 0x11);
            }
        } else if (step == 1) {
            YesNoCursor(self);
            if (!idle)
                break;
            HideYesNo(self);
            if (result == 0) {
                if (!confirmed) {
                    SetState(self, 0x10);
                    se(7);
                } else {
                    se(5);
                    StartTable(self, kFormat);
                    SetStep(self, 2);
                    SetAccessing(self, true);
                }
            } else {
                se(7);
                SetState(self, 1);
            }
        } else if (step == 0) {
            if (!begin(kFormatAsk2, 1))
                return 0;
            F(self, kYesNo) = 1;
            ShowYesNo(self);
        }
        break;
    case 0xF:                                           // no save file: make one?
    case 0x16:                                          // the file is bad: make it again?
        if (step == 2) {
            if (!idle)
                break;
            if (F(self, kState) == 0x16) {
                F(self, kFileFresh) = 0;
                SetAccessing(self, false);
                if (result == 0) {
                    SetState(self, 4);
                } else {
                    SetNotice(kNotice, 0xE);
                    SetState(self, 0x11);
                }
            } else if (result == kUnformatted) {
                SetState(self, 0xD);
            } else {
                SetState(self, result == 0 ? 0xB : 1);
            }
        } else if (step == 1) {
            YesNoCursor(self);
            if (!idle)
                break;
            HideYesNo(self);
            if (result == 0) {
                if (!confirmed) {
                    SetState(self, 0x10);
                    se(7);
                } else if (F(self, kState) == 0x16) {
                    SetAccessing(self, true);
                    StartTable(self, kRecreate);
                    SetStep(self, 2);
                    se(5);
                } else {
                    if (!begin(kProbe, 2))
                        return 0;
                    se(5);
                }
            } else if (result == kNoRoom) {
                SetNotice(kNoticeWatched, 9);
                SetState(self, 0x12);
            } else if (result == kNoCard) {
                se(7);
                SetState(self, 1);
            }
        } else if (step == 0) {
            if (StartTable(self, kCreateAsk2) == 0)
                return 0;
            F(self, kYesNo) = 0;
            ShowYesNo(self);
            SetStep(self, 1);
        }
        break;
    case 0x10:                                          // play without saving?
        if (step == 1) {
            YesNoCursor(self);
            if (!idle)
                break;
            HideYesNo(self);
            if (result != 0)
                break;
            if (!confirmed) {
                se(7);
                SetState(self, 0x13);
            } else {
                se(5);
                F(self, kWriteSlot) = 0;
                *gCurrentSlot = 0;
                SetMessage(self, -1);
                SetState(self, 0x14);
            }
        } else if (step == 0) {
            F(self, kYesNo) = 1;
            ShowYesNo(self);
            if (!begin(kNoSaveAsk, 1))
                return 0;
        }
        break;
    case 0x11:                                          // a notice, then look again
        if (step == 1) {
            if (idle)
                SetState(self, 1);
        } else if (step == 0) {
            StartTable(self, kNotice);
            SetStep(self, 1);
        }
        break;
    case 0x12:
    case 0x15:
        if (step == 1) {
            if (idle)
                SetState(self, result == 1 ? 1 : F(self, kState) == 0x12 ? 0x10 : 6);
        } else if (step == 0) {
            StartTable(self, kNoticeWatched);
            SetStep(self, 1);
        }
        break;
    case 0x13:
        *B(self, kAnswerByte) = 0;
        return 0;
    case 0x14:                                          // "…you will still be able to save…"
        if (step == 1) {
            if (idle)
                SetState(self, 0xC);
        } else if (step == 0) {
            F(self, kFace) = 2;
            SetMessage(self, -1);
            ShowMessage(self);
            SetNotice(kNotice, 0x28);
            StartTable(self, kNotice);
            SetStep(self, 1);
        }
        break;
    }
    return 1;
}

void Mode1_Draw(void* self) {                           // 0x257000
    if (sInstant[self])
        return;
    if (F(self, kLayout) != 0 && F(self, kState) != 0) {
        ScreenLayout_Draw(Layout(self));
        Font* font = *gFont;
        std::vector<Line>& lines = sLines[self];
        for (size_t i = 0; i < lines.size(); i++) {
            const Line& line = lines[i];
            if (i == 8 || !line.shown)                  // (the money is made but never shown)
                continue;
            // 0x261310
            const int x = (int)line.x * 16, y = (int)line.y * 16;
            const u32 spacing = *P<u32>(line.single ? 0x2EE370u : 0x2EE374u);
            *guest::ptr<u16>(0x2EE538u) = (u16)spacing;
            *guest::ptr<u16>(0x2EE53Au) = (u16)(spacing >> 16);
            if (line.single) {
                if (line.shadow) {
                    Font_SetColor(font, 0x7F010101u);
                    Font_DrawString(font, P<u16>(line.text), (int)Screen_X(x + 0x20, 0), (int)Screen_Y(y + 0x20, 0),
                                    0xFFFFE5, 0.9f, 0.0f);
                }
                Font_SetColor(font, line.colour);
                Font_DrawString(font, P<u16>(line.text), (int)Screen_X(x, 0), (int)Screen_Y(y, 0), 0xFFFFE6, 0.9f, 0.0f);
            } else {
                TextBox box;
                memset(&box, 0, sizeof box);
                box.text = line.text;
                box.scale = 0.9f;
                box.w = 0x280;
                box.h = 0x100;
                box.flags = 0x40;
                if (line.shadow) {
                    box.x = (u16)Screen_X(x + 0x20, 0);
                    box.y = (u16)Screen_Y(y + 0x20, 0);
                    box.z = 0xFFFFE5;
                    box.rgba = 0x7F010101u;
                    Font_DrawBox(font, &box);
                }
                box.x = (u16)Screen_X(x, 0);
                box.y = (u16)Screen_Y(y, 0);
                box.z = 0xFFFFE6;
                box.rgba = line.colour;
                Font_DrawBox(font, &box);
            }
        }
        if (F(self, kAccessing) != 0) {                 // "ACCESSING!!", blinking
            if (++sBlink > 5)
                sBlink = 0;
            if (sBlink < 3) {
                static u32 words = 0;
                if (words == 0)
                    words = guest::addr(guest::alloc(0x80));
                const std::u16string text = Raw(self, 0x4D);
                memcpy(P(words), text.c_str(), (text.size() + 1) * 2);
                Font_SetColor(font, 0x805050FAu);
                Font_DrawCentered(font, P<u16>(words), (int)Screen_X(0, 0), (int)Screen_Y(0x140, 0), 0xFFFFE6, 0.85f,
                                  0.0f);
            }
        }
    }
    Mode0_Draw(self);
}

// Bound from hle::bind_all (the destructors are reached through the vtables).
void McScreens_Bind() {
    guest::bind(0x260E50, reinterpret_cast<void*>(Destroy), "Mode0_Dtor");
    guest::bind(0x25FFB0, reinterpret_cast<void*>(Destroy), "SaveScreen_Dtor");
    guest::bind(0x264D00, reinterpret_cast<void*>(Destroy), "McScreen_Dtor");
    guest::bind(0x25DEE0, reinterpret_cast<void*>(Mode1_Dtor), "Mode1_Dtor");
}

}  // extern "C"
