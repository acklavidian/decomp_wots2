// The conversation manager — decompiled from 0x18E730–0x190450.
// See include/game/talk_mgr.h.

#include "game/talk_mgr.h"
#include "engine/bgm.h"
#include "engine/font.h"
#include "engine/pad.h"
#include "native/guest.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* MemAllocAligned(u32 align, u32 size);         // 0x1228C8
    void  MemFreeAligned(void* p);                      // 0x122B50
    void* PadMgr_Get();                                 // 0x13A920
    void  PadMgr_Poll(void* mgr);                       // 0x13A7B0
    void  PadMgr_Reset(void* mgr);                      // 0x13A820
    u16   Pad_GetPressed(void* mgr, int port);          // 0x13A0C0
    void* FollowCam_Ctor(void* mem, void* target, const void* config);     // 0x152470
    void  CamList_Push(void* list, void* controller);   // 0x150E60
    void  CharaMgr_SetIdle(CharaMgr* mgr, int id);      // 0x144E60
    void  CharaMgr_Release(CharaMgr* mgr, int id);      // 0x144E00
    void  CharaMgr_ClearB5(CharaMgr* mgr);              // 0x146640
    void  CharaMgr_fn146740(CharaMgr* mgr, int id, const float* point);    // 0x146740  look at
}

static_assert(sizeof(TalkEntry) == 0x58 && offsetof(TalkEntry, style) == 0x38, "TalkEntry");
static_assert(sizeof(TalkMgr) == 0x3C && offsetof(TalkMgr, cameraA) == 0x28, "TalkMgr");

static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);
static s32& gIndoors   = *guest::ptr<s32>(0x2EE518u);

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline TalkEntry* EntryOf(GuestListNode* n) { return P<TalkEntry>(n->value); }
static inline s32 IdOf(u32 params) { return P<CharaParams>(params)->id; }
#define EACH_ENTRY(mgr, e) \
    for (GuestListNode* n_ = GuestList_Begin(&(mgr)->entries); n_ != GuestList_End(&(mgr)->entries); \
         n_ = GuestList_Next(n_)) \
        if (TalkEntry* e = EntryOf(n_))

static TalkEntry* NewEntry(CharaParams* speaker, CharaParams* listener, const u16* text, int style) {
    TalkEntry* e = static_cast<TalkEntry*>(MemAlloc(sizeof(TalkEntry)));
    memset(e, 0, sizeof *e);
    e->direction = -1;
    e->frames = -1;
    e->started = 1;
    e->waitsForButton = 1;
    e->choices = -1;
    e->delay = gFrameRate / 2;
    e->closeDelay = gFrameRate / 5;
    e->actor = -1;
    e->speaker = guest::addr(speaker);
    e->listener = guest::addr(listener);
    e->style = style;
    e->handle = guest::addr(e);
    if (text != nullptr) {
        const int length = WStr_Length(text);
        u16* copy = static_cast<u16*>(MemAllocAligned(0x10, (u32)length * 2 + 2));
        memcpy(copy, text, (size_t)length * 2);
        copy[length] = 0;
        e->text = guest::addr(copy);
    }
    return e;
}

template <typename Widget>
static Widget* ShowWidget(TalkMgr* mgr, Widget* widget, const TalkEntry* e, int direction) {
    if (widget == nullptr)
        return widget;
    SpeechParams params{};
    params.speaker = e->speaker;
    params.listener = e->listener;
    params.frames = e->frames;
    params.style = e->style;
    params.direction = direction;
    params.text = e->text;
    VirtualCall<int, void*, const SpeechParams*>(widget->w.vtable, 0x0C, widget, &params);
    GuestList* list = &P<SpeechList>(mgr->list)->widgets;
    GuestList_Insert(list, GuestList_End(list), guest::addr(widget));
    return widget;
}
static SpeechBalloon* NewBalloon(TalkMgr* mgr, const TalkEntry* e, bool remark, int direction) {
    void* mem = MemAlloc(sizeof(SpeechBalloon));
    return ShowWidget(mgr, mem ? (remark ? SpeechRemark_Ctor(mem) : SpeechBalloon_Ctor(mem)) : nullptr, e, direction);
}
static ChoiceWindow* NewChoice(TalkMgr* mgr, const TalkEntry* e) {
    void* mem = MemAlloc(sizeof(ChoiceWindow));
    return ShowWidget(mgr, mem ? ChoiceWindow_Ctor(mem) : nullptr, e, -1);
}

extern "C" {

// ── 0x190450 ──────────────────────────────────────────────────────────────
void* Effects_Ctor(void* mem) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    GuestList_Init(&mgr->entries);
    void* list = MemAlloc(sizeof(SpeechList));
    mgr->list = list ? guest::addr(SpeechList_Ctor(list)) : 0;
    mgr->cameraA = mgr->cameraB = mgr->lastDirection = mgr->lastA = mgr->lastB = -1;
    mgr->choiceShown = 0;
    mgr->cameraOn = 0;
    return mgr;
}

static void FreeEntry(TalkEntry* e) {
    if (e == nullptr)
        return;
    if (e->text != 0) {
        MemFreeAligned(P(e->text));
        e->text = 0;
    }
    if (e->balloon != 0)
        SpeechWidget_Close(P<SpeechWidget>(e->balloon));
    if (e->choice != 0)
        SpeechWidget_Close(P<SpeechWidget>(e->choice));
    e->remark = 0;
    MemFree(e);
}

// ── 0x1902D0 ──────────────────────────────────────────────────────────────
void Effects_Dtor(void* mem, int flag) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    if (mgr == nullptr)
        return;
    GuestListNode* n = GuestList_Begin(&mgr->entries);
    while (n != GuestList_End(&mgr->entries)) {
        FreeEntry(EntryOf(n));
        n = GuestList_Erase(&mgr->entries, n);
    }
    if (mgr->list != 0)
        SpeechList_Dtor(P<SpeechList>(mgr->list), 1);
    mgr->list = 0;
    if ((s16)flag > 0)
        MemFree(mgr);
}

// ── 0x18F860 ──────────────────────────────────────────────────────────────
void Effects_Draw(void* mem) { SpeechList_Draw(P<SpeechList>(static_cast<TalkMgr*>(mem)->list)); }

// ── 0x18E830 ──────────────────────────────────────────────────────────────
int Effects_IsBusy(void* mem, int id) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    EACH_ENTRY(mgr, e)
        if (id == IdOf(e->speaker) || id == IdOf(e->listener))
            return 1;
    return mgr->cameraOn;
}

// ── 0x18E790 ──────────────────────────────────────────────────────────────
int Effects_PartnerOf(void* mem, int id) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    EACH_ENTRY(mgr, e) {
        if (id == IdOf(e->speaker))
            return IdOf(e->listener);
        if (id == IdOf(e->listener))
            return IdOf(e->speaker);
    }
    if (mgr->cameraOn != 0) {
        if (mgr->cameraA == id)
            return mgr->cameraB;
        if (mgr->cameraB == id)
            return mgr->cameraA;
    }
    return -1;
}

// ── 0x18E730 ──────────────────────────────────────────────────────────────
void Effects_fn18e730(void* mem, int id) {
    EACH_ENTRY(static_cast<TalkMgr*>(mem), e)
        if (e->started != 0 && e->listener != 0 && id == IdOf(e->listener))
            e->closeNow = 1;
}

// ── 0x18E890 ──────────────────────────────────────────────────────────────
void Effects_fn18e890(void* mem, int handle) {
    EACH_ENTRY(static_cast<TalkMgr*>(mem), e)
        if ((u32)handle == e->handle) {
            e->started = 1;
            return;
        }
}

// ── 0x18E8D0 ──────────────────────────────────────────────────────────────
int Talk_Exists(void* mem, u32 handle) {
    EACH_ENTRY(static_cast<TalkMgr*>(mem), e)
        if (handle == e->handle)
            return 1;
    return 0;
}

// ── 0x18F770 ──────────────────────────────────────────────────────────────
void Effects_EndTalkOf(void* mem, int id) {
    EACH_ENTRY(static_cast<TalkMgr*>(mem), e)
        if (id == IdOf(e->speaker)) {
            e->endCamera = 1;
            e->closeNow = 1;
        }
}

// ── 0x18F7D0 ──────────────────────────────────────────────────────────────
void Effects_EndAll(void* mem, int handle) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    if (handle == -1) {
        EACH_ENTRY(mgr, e)
            e->closeNow = 1;
        mgr->cameraOn = 0;
        return;
    }
    EACH_ENTRY(mgr, e)
        if ((u32)handle == e->handle) {
            e->closeNow = 1;
            return;
        }
}

// ── 0x190010 ──────────────────────────────────────────────────────────────
// The camera goes back to following the player and both are free to move.
void Effects_EndTalkCamera(void* mem, void* camList, CharaMgr* charas, int a, int b) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    if (mgr->cameraOn == 0 || mgr->cameraA != a || mgr->cameraB != b)
        return;
    void* block = MemAlloc(0xC0);
    void* cam = nullptr;
    if (block != nullptr)
        cam = FollowCam_Ctor(block, CharaResources_GetParams(CharaResources_Get(), 0),
                             P(gIndoors == 1 ? 0x2E0070u : 0x2E0020u));
    CamList_Push(camList, cam);
    CharaMgr_Release(charas, a);
    CharaMgr_Release(charas, b);
    CharaMgr_ClearB5(charas);
    mgr->cameraA = mgr->cameraB = -1;
    mgr->lastA = mgr->cameraA;
    mgr->lastB = mgr->cameraB;
    mgr->cameraOn = 0;
}

// ── 0x18EBE0 ──────────────────────────────────────────────────────────────
u32 Talk_Say(void* mem, CharaParams* speaker, CharaParams* listener, const u16* text, int style, int frames,
             int actor) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    TalkEntry* e = NewEntry(speaker, listener, text, style);
    e->frames = frames;
    e->delay = gFrameRate / 10;
    e->closeDelay = gFrameRate / 10;
    e->actor = actor;
    if (mgr->lastA == speaker->id)
        e->direction = mgr->lastDirection;      // the same person again: the balloon stays on its side
    if (speaker == listener) {
        e->started = 1;
        e->waitsForButton = 0;
        e->remark = guest::addr(NewBalloon(mgr, e, true, -1));
    } else {
        e->started = 0;                         // until Effects_fn18e890
        e->waitsForButton = 1;
        mgr->choiceShown = 0;
    }
    GuestList_Insert(&mgr->entries, GuestList_End(&mgr->entries), guest::addr(e));
    return e->handle;
}

// ── 0x18E920 ──────────────────────────────────────────────────────────────
u32 Talk_Select(void* mem, CharaParams* speaker, CharaParams* listener, const u16* text, int style, int choices,
                int frames) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    TalkEntry* e = NewEntry(speaker, listener, text, style);
    e->frames = frames;
    e->choices = choices;
    e->waitsForButton = 0;
    if (speaker == listener) {
        e->started = 1;
        ChoiceWindow* window = NewChoice(mgr, e);
        e->choice = guest::addr(window);
        ChoiceWindow_SetSelection(window, 0);
    } else {
        e->started = 0;
    }
    GuestList_Insert(&mgr->entries, GuestList_End(&mgr->entries), guest::addr(e));
    mgr->selection = 1;
    mgr->chooser = -1;
    mgr->choiceShown = 0;
    return e->handle;
}

// ── 0x18F7C0: closes the choice on offer ──────────────────────────────────
void Talk_CloseChoice(void* mem) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    Effects_EndAll(mem, (int)mgr->choiceHandle);
}

// ── 0x18F1D0: a line said by an event script: shown at once, no talk camera
// (the script frames its own shots) ───────────────────────────────────────
int Talk_ScriptSay(void* mem, CharaParams* speaker, CharaParams* listener, const u16* text, int style, int frames) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    TalkEntry* e = NewEntry(speaker, listener, text, style);
    e->frames = frames;
    {
        const s32 shown = e->frames;
        e->frames = -1;                         // the balloon itself never times out
        e->balloon = guest::addr(NewBalloon(mgr, e, false, -1));
        e->frames = shown;
    }
    e->started = 1;
    e->endCamera = 1;
    Sound_PlaySE(0, 0, 0x3FFF);
    GuestList_Insert(&mgr->entries, GuestList_End(&mgr->entries), guest::addr(e));
    mgr->choiceShown = 0;
    return 1;
}

// ── 0x18F460: a line said by an event script in a balloon at a place the
// script names (desc: {flags (1 = at x, y), speaker, listener, text, style,
// frames, x, y}) ──────────────────────────────────────────────────────────
int Talk_ScriptSayAt(void* mem, const u32* desc) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    TalkEntry* e = NewEntry(P<CharaParams>(desc[1]), P<CharaParams>(desc[2]),
                            desc[3] ? P<const u16>(desc[3]) : nullptr, (int)desc[4]);
    e->frames = (s32)desc[5];
    void* block = MemAlloc(sizeof(PlacedBalloon));
    PlacedBalloon* balloon = block ? SpeechPlaced_Ctor(block) : nullptr;
    if (balloon != nullptr) {
        SpeechParams params{};
        params.speaker = desc[1];
        params.listener = desc[2];
        params.frames = -1;
        params.style = (s32)desc[4];
        params.direction = -1;
        params.text = e->text;
        params.flags = desc[0] & 1;
        if (desc[0] & 1) {
            params.x = (s32)desc[6];
            params.y = (s32)desc[7];
        }
        VirtualCall<int, void*, const SpeechParams*>(balloon->b.w.vtable, 0x0C, balloon, &params);
        GuestList* list = &P<SpeechList>(mgr->list)->widgets;
        GuestList_Insert(list, GuestList_End(list), guest::addr(balloon));
    }
    e->balloon = guest::addr(balloon);
    e->started = 1;
    e->endCamera = 1;
    Sound_PlaySE(0, 0, 0x3FFF);
    GuestList_Insert(&mgr->entries, GuestList_End(&mgr->entries), guest::addr(e));
    mgr->choiceShown = 0;
    return 1;
}

// ── 0x18EF00: a choice put by an event script ─────────────────────────────
void Talk_ScriptSelect(void* mem, int choices, const SpeechParams* desc) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    Talk_CloseChoice(mem);
    TalkEntry* e = NewEntry(P<CharaParams>(desc->speaker), P<CharaParams>(desc->listener),
                            desc->text ? P<const u16>(desc->text) : nullptr, desc->style);
    e->frames = desc->frames;
    e->choices = choices;
    e->waitsForButton = 0;
    e->started = 1;
    e->endCamera = 1;
    void* block = MemAlloc(sizeof(ChoiceWindow));
    ChoiceWindow* window = block ? ChoiceWindow_Ctor(block) : nullptr;
    if (window != nullptr) {
        SpeechParams params = *desc;
        params.text = e->text;
        VirtualCall<int, void*, const SpeechParams*>(window->w.vtable, 0x0C, window, &params);
        GuestList* list = &P<SpeechList>(mgr->list)->widgets;
        GuestList_Insert(list, GuestList_End(list), guest::addr(window));
    }
    e->choice = guest::addr(window);
    ChoiceWindow_SetSelection(window, 0);
    Sound_PlaySE(1, 0, 0x3FFF);
    GuestList_Insert(&mgr->entries, GuestList_End(&mgr->entries), guest::addr(e));
    mgr->choiceHandle = e->handle;
    mgr->selection = 1;
    mgr->choiceShown = 0;
    mgr->chooser = -1;
}

// ── 0x190190 ──────────────────────────────────────────────────────────────
// The two look at each other (at eye height, CharaParams+0x3C).
static void FaceEachOther(CharaMgr* charas, const TalkEntry* e) {
    auto eyes = [](const CharaParams* p, float* out) {
        memcpy(out, &p->matrix[12], 16);
        float height;
        memcpy(&height, &p->_3c, 4);
        out[1] += height;
    };
    const CharaParams* speaker = P<CharaParams>(e->speaker);
    const CharaParams* listener = e->actor == -1 ? P<CharaParams>(e->listener) : CharaMgr_GetParams(charas, e->actor);
    if (listener == nullptr)
        return;
    alignas(16) float point[4];
    eyes(listener, point);
    CharaMgr_fn146740(charas, speaker->id, point);
    eyes(speaker, point);
    CharaMgr_fn146740(charas, listener->id, point);
}

// ── 0x18F880 ──────────────────────────────────────────────────────────────
void Effects_Update(void* mem, void* camList, void* charaMgr, void*) {
    TalkMgr* mgr = static_cast<TalkMgr*>(mem);
    CharaMgr* charas = static_cast<CharaMgr*>(charaMgr);
    void* pad = PadMgr_Get();
    // Debug hook, not in the original: WOTS2_TEST_SAY="frames:id" makes that
    // character speak to the player (and remark to itself) after so many
    // frames in the area, then offer a choice once the line is dismissed.
    if (const char* spec = getenv("WOTS2_TEST_SAY")) {
        static int frames = 0, at = -1, who = 0, stage = 0;
        if (at < 0)
            sscanf(spec, "%d:%d", &at, &who);
        frames++;
        CharaParams* speaker = CharaMgr_GetParams(charas, who);
        CharaParams* player = CharaMgr_GetParams(charas, 0);
        static u16 line[64], remark[64], choices[64];
        auto put = [](u16* out, const char* text) { while ((*out++ = (u8)*text++) != 0) {} };
        if (speaker != nullptr && player != nullptr) {
            if (stage == 0 && frames >= at && strstr(spec, ":r") != nullptr) {
                // "frames:id:r": only a remark, by that character to nobody.
                put(remark, "Fine weather\nfor a walk.");
                Talk_Say(mgr, speaker, speaker, remark, 0, gFrameRate * 8, who);
                stage = 3;
            } else if (stage == 0 && frames >= at) {
                put(line, "You there! State your\nbusiness in Amahara.");
                put(remark, "Hmph.");
                Effects_fn18e890(mgr, (int)Talk_Say(mgr, speaker, player, line, 0, -1, who));
                Talk_Say(mgr, player, player, remark, 1, gFrameRate * 4, 0);
                stage = 1;
            } else if (stage == 1 && mgr->entries.size == 0) {
                put(choices, "Just passing through.\nNone of your concern.\n(Say nothing)");
                Effects_fn18e890(mgr, (int)Talk_Select(mgr, speaker, player, choices, 0, 3, gFrameRate * 20));
                stage = 2;
            }
        }
    }
    GuestListNode* n = GuestList_Begin(&mgr->entries);
    while (n != GuestList_End(&mgr->entries)) {
        TalkEntry* e = EntryOf(n);
        bool done = false;
        if (e->started != 0) {
            if (e->closing != 0) {
                done = e->closeDelay-- < 1;
            } else {
                if (e->choices < 1) {
                    // Speech: any face button moves on.
                    if (e->waitsForButton != 0 && e->delay-- < 1
                        && (Pad_GetPressed(pad, 0) & (kPadTriangle | kPadCircle | kPadCross | kPadSquare)) != 0) {
                        Sound_PlaySE(5, 0, 0x3FFF);
                        e->closing = 1;
                    }
                } else {
                    if (mgr->choiceShown == 0 && e->delay-- < 1) {
                        mgr->choiceShown = 1;
                        ChoiceWindow_SetSelection(P<ChoiceWindow>(e->choice), mgr->selection);
                    }
                    if (mgr->choiceShown != 0) {
                        const u16 pressed = Pad_GetPressed(pad, 0) | Pad_GetRepeat(pad, 0);
                        int step = 0;
                        if (e->choices > 1) {
                            if (pressed & kPadUp) {
                                Sound_PlaySE(2, 0, 0x3FFF);
                                step = -1;
                            }
                            if (pressed & kPadDown) {
                                Sound_PlaySE(2, 0, 0x3FFF);
                                step = 1;
                            }
                        }
                        mgr->selection += step;
                        if (mgr->selection < 1)
                            mgr->selection = e->choices;
                        if (e->choices < mgr->selection)
                            mgr->selection = 1;
                        ChoiceWindow_SetSelection(P<ChoiceWindow>(e->choice), mgr->selection);
                        if ((pressed & kPadCross) && mgr->selection != 0) {
                            Sound_PlaySE(5, 0, 0x3FFF);
                            e->closing = 1;
                        }
                        if (e->endCamera == 0 && (pressed & kPadTriangle)) {    // walk away
                            Sound_PlaySE(7, 0, 0x3FFF);
                            e->_30 = 0;
                            e->closing = 1;
                            mgr->selection = -1;
                            PadMgr_Poll(pad);
                            PadMgr_Reset(pad);
                        }
                    }
                }
                if (e->frames >= 0 && --e->frames == 0) {
                    e->closing = 1;
                    mgr->selection = 0x100;
                }
            }
            // First frame: put it on screen.
            if (e->balloon == 0 && e->choice == 0 && e->remark == 0) {
                if (e->choices < 1) {
                    SpeechBalloon* balloon = NewBalloon(mgr, e, false, e->direction);
                    e->balloon = guest::addr(balloon);
                    mgr->lastDirection = balloon->direction;
                    Sound_PlaySE(0, 0, 0x3FFF);
                } else {
                    ChoiceWindow* window = NewChoice(mgr, e);
                    e->choice = guest::addr(window);
                    Sound_PlaySE(1, 0, 0x3FFF);
                    ChoiceWindow_SetSelection(window, 0);
                }
            }
            // Two people: the camera frames them and they stand still.
            if (e->speaker != e->listener && e->endCamera == 0) {
                if (mgr->cameraOn == 0) {
                    mgr->cameraA = IdOf(e->speaker);
                    mgr->cameraB = IdOf(e->listener);
                    mgr->lastA = mgr->cameraA;
                    mgr->lastB = mgr->cameraB;
                    void* block = MemAlloc(0x30);
                    void* cam = block != nullptr ? TalkCam_Ctor(block) : nullptr;
                    TalkCam_Set(1.2f, 10.0f, cam, P<CharaParams>(e->speaker), P<CharaParams>(e->listener));
                    CamList_Push(camList, cam);
                    CharaMgr_SetIdle(charas, mgr->cameraA);
                    CharaMgr_SetIdle(charas, mgr->cameraB);
                    mgr->cameraOn = 1;
                }
                FaceEachOther(charas, e);
            }
        }
        if (done || e->closeNow != 0) {
            if (e->endCamera != 0)
                Effects_EndTalkCamera(mgr, camList, charas, IdOf(e->speaker), IdOf(e->listener));
            if (e->choice != 0) {
                if (mgr->selection == -1)
                    mgr->selection = 0x101;
                mgr->choiceShown = 0;
                mgr->chooser = IdOf(e->speaker);
            }
            FreeEntry(e);
            n = GuestList_Erase(&mgr->entries, n);
        } else {
            n = GuestList_Next(n);
        }
    }
    SpeechList_Update(P<SpeechList>(mgr->list));
}

}  // extern "C"
