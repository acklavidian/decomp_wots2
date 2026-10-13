// The event (script) player — GameplayScene+0xD4, 0x18 bytes, 0x1E2E20–
// 0x1E4DF0 — and the built-ins it gives the scripts.  See "Event scripts" in
// the project notes; the VM is src/game/script_vm.cpp.
//
// WORK IN PROGRESS.  Of the 234 built-ins (config/script_builtins.json) only
// the ones below are decompiled; a script that calls another gets a stub that
// reports itself once and answers 0.  WOTS2_EVENTS=0 (debug) goes back to
// the stand-in player (src/native/hle_gameplay.cpp), which runs no scripts.

#include "game/chara_resources.h"
#include "game/map_collision.h"
#include "game/day_mgr.h"
#include "engine/font.h"
#include "engine/bgm.h"
#include "engine/motion.h"
#include "game/gameplay_scene.h"
#include "game/hud.h"
#include "game/map_db.h"
#include "game/ai_moves.h"
#include "game/world.h"
#include "game/script_vm.h"
#include "native/archive.h"
#include "native/guest.h"

#include <array>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern "C" int Pad_SetVibration(void* mgr, int port, const void* pattern);   // 0x139FF0
extern "C" void Camera_Reset(void* lighting);     // 0x1663E0 (point_lights.cpp)
extern "C" void Lights_EnableMap(int on);          // 0x165C60
extern "C" {
    void  CharaResources_Purge(void* res);              // 0x1D2AC0
    int   CharaResources_PickSword(void* res, int kind);    // 0x1D2AF0 (sword_table.cpp)
    void  CharaResources_UseSword(void* res, int sword);    // 0x1D2A90
    void  RenderMgr_TriggerEffect(void* mgr, int type, int a, int b);   // 0x14F190
    void  Matrix_Multiply(void* dst, const void* a, const void* b);     // 0x107130
    // The camera (src/game/camera.cpp).
    void  CamList_SetEye(void* list, const float* v);                   // 0x150850
    void  CamList_SetTarget(void* list, const float* v);                // 0x1507B0
    void  CamList_Apply(void* list);                                    // 0x1505E0
    void  CamList_SetUp(void* list, const float* v);                    // 0x1506B0
    // The forge (src/game/forge.cpp).
    int   Forge_CheckHardness(int n);
    int   Forge_CheckQuality(int n);
    int   Forge_CheckSuccess(int n);
    int   Forge_CheckJudge(int n);
    int   Forge_GetKoudo(int n);
    int   Forge_GetUpParam(int which);
    int   Forge_Penalty(int n);
    int   Forge_Secret(int n);                                          // 0x28D920
    void  Forge_Hardness(int n);
    void  Forge_Quality(int n, int kind, int gamble);
    int   Forge_Judge(int n, int judged);
    const char16_t* Forge_TitleName(int title);
    void  Forge_SetTitle(int n, int title);
    const u16* Forge_SwordName(int n);
    // World effects (src/game/world_effects.cpp).
    void  Fx_BloodSpray(u32 params, const float* dir);
    void  Fx_BloodPool(const float* pos, const float* dir);
    int   RenderMgr_DeleteMarks(void* mgr);                             // 0x14FC00
    void* RenderMgr_FindWorldEffect(void* mgr, int type, int n);        // 0x14EB50
    int   RenderMgr_AddEnvSound(void* mgr, int id, int volume, int interval);  // 0x14EA50
    int   RenderMgr_RemoveEnvSound(void* mgr, int handle);              // 0x14E920
    void  Shadow_Release(void* effect);                                 // 0x172090
    int   CharaMgr_Get1DC(void* mgr, int id);                           // 0x146AA0
    void  CharaMgr_fn1444d0(void* mgr, int id);                         // 0x1444D0
    int   ItemDrops_ObjectType(int id);                                 // 0x21C230
    int   ItemDrops_ItemId(void* drops, int id);                        // 0x21C1A0
    int   PlayerData_AddItemSorted(void* self, int home, u8 item);      // 0x177920
    int   PlayerData_HasFreeItemSlot(void* self, int home);             // 0x177760
    void  CamList_Push(void* list, void* controller);                   // 0x150E60
    void* FollowCam_Ctor(void* mem, void* target, const void* config);  // 0x152470
    void* EaseShaky_Ctor(void* mem, int frames);                        // 0x1578A0
    void* EaseFrames_Ctor(void* mem, int frames);                       // 0x157A20
    void* EaseFixed_Ctor(float share, void* mem);                       // 0x157AC0
    void  EaseHolder_SetNext(u32* slot, void* ease);                    // 0x1576C0
    void  EaseHolder_Begin(u32* slot);                                  // 0x157560
    void* MemAlloc(u32 size);                                           // 0x100820
    void  MemFree(void* p);                                             // 0x100650
    void  Sound_PlaySE(int id, int positional, int volume);             // 0x180E70
    void  Voice_Play(int id, int positional, int volume, int a);        // 0x1800F0
    void  Voice_Start(int id, int positional, int volume);              // 0x1801F0
    void  Voice_Stop();                                                 // 0x1801E0
    void  hle_light_set_dir(float x, float y, float z);                 // native: the light models are lit by
    void  hle_light_set_colour(float r, float g, float b);
    void  hle_light_restore();
    void* PadMgr_Get();                                                 // 0x13A920
    u32   Screen_X(int v, int align);                                   // 0x12E5E0
    u32   Screen_Y(int v, int align);                                   // 0x12E550
    u16   Pad_GetPressed(void* mgr, int port);                          // 0x13A0C0
    // The conversation manager (talk_mgr.cpp), untyped as the world holds it.
    int   Talk_ScriptSay(void* self, void* speaker, void* listener, const u16* text, int style, int frames);  // 0x18F1D0
    int   Talk_ScriptSayAt(void* self, const u32* desc);                // 0x18F460
    int   Hud_OpenShop(void* hud, const u32* params);                   // 0x21CE00
    int   Hud_ShopOpen(void* hud);                                      // 0x21CDF0
    int   Hud_ShopResult(void* hud);
    void  Talk_ScriptSelect(void* self, int choices, const void* desc); // 0x18EF00
    void  Talk_CloseChoice(void* self);                                 // 0x18F7C0
    void  Effects_EndAll(void* self, int handle);                       // 0x18F7D0
    int   Talk_Exists(void* self, u32 handle);                          // 0x18E8D0
    void  CharaMgr_ResetTint(void* mgr, int id);                        // 0x143030
    void  Accessories_AddWearer(void* params);                          // 0x2AB910 (accessories.cpp)
    void  Accessories_RemoveWearer(const void* params);                 // 0x2AB7D0
    void  CharaMgr_fn1443f0(void* mgr, int id);                         // 0x1443F0  its sword is broken
    int   CharaMgr_fn144460(void* mgr, int id);                         // 0x144460  is its sword broken
    int   CharaMgr_IsLoaded(void* mgr, int id);                         // 0x1441F0
    void  CharaMgr_fn143bf0(void* mgr, int id, u32 request);            // 0x143BF0  play a motion (Chara 0x1CCCC0)
    void  CharaMgr_fn146740(void* mgr, int id, const float* point);     // 0x146740  look at a point
    int   CharaMgr_GetHealthShown(void* mgr, int id);                   // 0x143A90  (misnamed: the motion's current frame)
    void  CharaMgr_fn143b80(void* mgr, int id);                         // 0x143B80  hold the motion where it is
    int   CharaMgr_GetB5(void* mgr, int id);                            // 0x146680  is it looking at something
    void  CharaMgr_Renumber(void* mgr, int id, int newId);              // 0x146030
    int   CharaMgr_IdAt(void* mgr, int index);                          // 0x145FE0
    void  CharaMgr_fn143310(void* mgr, int id);                         // 0x143310  eyes: blink by itself
    void  CharaMgr_fn143380(void* mgr, int id, u32 value);              // 0x143380  eyes: 0 open, 1 half, 2 shut
    void  CharaMgr_fn1433f0(void* mgr, int id, u32 value);              // 0x1433F0  expression
    void  CharaMgr_fn143280(void* mgr, int id, u32 arg);                // 0x143280
    void  CharaMgr_fn1434a0(void* mgr, int id, u32 arg);                // 0x1434A0  mouth
    void  CharaMgr_Kill(void* mgr, int id);                             // 0x146100
    int   CharaMgr_fn1438d0(void* mgr, u32 object, int id);             // 0x1438D0  what it holds
    // World objects (world_objects.cpp).
    void* ItemDrops_Place(void* self, int object, const void* matrix);  // 0x21BAB0
    int   ItemDrops_Has(void* self, const void* object);                // 0x21B860
    int   ItemDrops_Remove(void* self, void* object);                   // 0x21B7A0
    void  ItemDrops_Action(void* self, int id, int nth, int how);       // 0x21B310
    float* WorldObject_Matrix(void* object);
    u16*   WorldObject_Flags(void* object);
    u32    WorldObject_Vtable(void* object);
    void  CharaMgr_fn143cd0(void* mgr, int id);                         // 0x143CD0  sword out of hand (at once)
    void  CharaMgr_fn143d40(void* mgr, int id);                         // 0x143D40  sword in hand (at once)
    int   CharaMgr_fn1437d0(void* mgr, int id);                         // 0x1437D0
    void  CharaMgr_Draw_Sword(void* mgr, int id, int quiet);            // 0x146380
    void  CharaMgr_SetOpponent(void* mgr, int id, int other, int kind); // 0x1431A0
    void  CharaMgr_SetFlags(void* mgr, int id, u32 bits);               // 0x1461D0
    void  CharaMgr_ClearFlags(void* mgr, int id, u32 bits);             // 0x146160
    // The stand-in player (hle_gameplay.cpp).
    EventPlayer* hle_EventPlayer_Ctor(EventPlayer* mem);
    void   hle_EventPlayer_StartDay(EventPlayer* self, int a, int b);
    void   hle_EventPlayer_StartScript(EventPlayer* self, const char* script, int a);
    int    hle_EventPlayer_Update(EventPlayer* self, int a);
}

int Matrix_TurnTowards(float degrees, float* matrix, const float* point);    // 0x1C8C40 (task_actions.cpp)
bool TextRoll_Start(const char* path, float speed, int total);                // text_roll.cpp
void TextRoll_Stop();

extern "C" void SwordDb_FreeModels(void* db);                   // 0x15C780

namespace {

using sol::Frame;
using sol::Ref;

// The guest object (the scene reads `state` and `result`).
struct Player {
    u32 scene;          // +0x00
    u32 vm;             // +0x04  (the VM is a host object here: sVm)
    s32 frames;         // +0x08
    s32 state;          // +0x0C  1 idle, 2 a script is running, 3 the config script failed
    s32 result;         // +0x10
    s32 budget;         // +0x14  ops per thread per frame
};
static_assert(sizeof(Player) == 0x18 && sizeof(EventPlayer) == 0x18, "EventPlayer");

std::unique_ptr<sol::VM> sVm;
Player* sSelf = nullptr;                                // *0x2EE6DC
const GuestPtrVar<u8> gPlayerData{0x2EE688u};
const GuestPtrVar<void> gHud{0x2EE69Cu};
s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);
s32& gLoadingActive = *guest::ptr<s32>(0x2EE6B4u);

bool Enabled() {
    // On unless WOTS2_EVENTS=0 (debug: the stand-in, which drops the player
    // straight into the first area with no story).
    static const bool on = getenv("WOTS2_EVENTS") == nullptr || atoi(getenv("WOTS2_EVENTS")) != 0;
    return on;
}
inline Player* P(EventPlayer* p) { return reinterpret_cast<Player*>(p); }

// ── 0x1E4640 / 0x1E48E0: a script by name.  (The original falls back to
// compiling the .sol; the disc has every .sx.) ─────────────────────────────
Ref LoadScript(const std::string& name) {
    std::vector<u8> file;
    if (!archive::read(("script/" + name + ".sx").c_str(), file))
        return nullptr;
    return sVm->load(file, name);
}

// ── 0x1EB470 / 0x1EB400 + 0x1EB6B0: a built-in is not started with fewer
// than `least` arguments. ─────────────────────────────────────────────────
// (WOTS2_TRACE_CALLS=1, debug: every built-in a script calls, with its numbers.)
void TraceCall(const std::string& name, Frame& f) {
    static const bool on = getenv("WOTS2_TRACE_CALLS") != nullptr;
    if (!on)
        return;
    printf("[call %d] %s(", sSelf ? sSelf->frames : 0, name.c_str());
    for (size_t i = 0; i < sol::argCount(f); i++) {
        s32 n;
        float x;
        if (sol::argInt(f, i, &n)) printf("%s%d", i ? ", " : "", n);
        else if (sol::argNumber(f, i, &x)) printf("%s%g", i ? ", " : "", x);
        else printf("%s…", i ? ", " : "");
    }
    printf(")\n");
}
using Fn = std::function<int(Player*, Frame&)>;
// EventPlayer_ProbeStory (debug, below): while a probe runs, built-ins that
// only look are let through and the rest do nothing; the first story script
// StartUp2 would start is noted instead of started.
int sProbeMap = -1;
std::string sProbeHit;
int ProbeCall(const std::string& name, Frame& f, const Fn& fn) {
    if (name == "GetMapID") {
        f.thread->result = sVm->newInt(sProbeMap);
        return 1;
    }
    if (name == "GetDay" || name == "GetPhase" || name == "GetFlag" || name == "GetTrustPoint" ||
        name == "GetPowerPoint" || name == "GetStaminaPoint")
        return fn(sSelf, f);
    if (name == "StartScript" || name == "Require") {
        const std::u16string* script = sol::argString(f, 0);
        const std::string narrow = script ? sol::narrow(*script) : std::string();
        if (name == "Require" && narrow.compare(0, 4, "CTX/") != 0)
            return fn(sSelf, f);
        if (sProbeHit.empty())
            sProbeHit = narrow;
    }
    f.thread->result = sVm->newInt(0);
    return 0;
}
void Reg(const Ref& object, const char* name, int least, Fn fn) {
    // The function's answer is the call's: 1 keeps its frame for one more
    // step (so that whatever it called — Require's script — runs first).
    sVm->def(object, name, [least, fn, name = std::string(name)](Frame& f) {
        TraceCall(name, f);
        if ((int)sol::argCount(f) < least)
            return 0;
        if (sProbeMap >= 0)
            return ProbeCall(name, f, fn);
        return fn(sSelf, f);
    });
}
void RegWait(const Ref& object, const char* name, int least, std::function<int(Frame&)> start,
             std::function<int(Frame&)> step) {
    sVm->def(object, name, [least, start, name = std::string(name)](Frame& f) {
        TraceCall(name, f);
        if ((int)sol::argCount(f) < least)
            return 0;
        if (sProbeMap >= 0) {
            f.thread->result = sVm->newInt(0);
            return 0;
        }
        return start ? start(f) : 1;
    }, std::move(step));
}
inline s32 IntArg(Frame& f, size_t i, s32 otherwise = 0) {
    s32 v = otherwise;
    sol::argInt(f, i, &v);
    return v;
}
inline void Answer(Frame& f, s32 v) { f.thread->result = sVm->newInt(v); }

// ═══ Root functions (0x1E79D0) ════════════════════════════════════════════
void BuildRoot(const Ref& root) {
    // Wait(seconds [, 0]) / Wait(frames): 0x1EBE20 / 0x1EC1C0.  It also puts
    // the script back to its normal speed.
    RegWait(root, "Wait", 0, [](Frame& f) {
        sSelf->budget = 10;
        s32 frames;
        float seconds;
        if (sol::argInt(f, 0, &frames)) {
            f.phase = frames;
        } else if (sol::argNumber(f, 0, &seconds)) {
            s32 mode = 1;
            const bool given = sol::argCount(f) > 1 && sol::argInt(f, 1, &mode);
            f.phase = (int)(seconds * (given && mode == 0 ? 30.0f : (float)*gFrameRate));
        } else {
            return 0;
        }
        return 1;
    }, [](Frame& f) { return f.phase-- < 1 ? sol::kEnd : sol::kYield; });

    // WaitFor(def): calls it; while it answers 0, waits eight frames and
    // asks again (0x1EB970 / 0x1EBA10).
    RegWait(root, "WaitFor", 0, [](Frame& f) {
        if (sol::argCount(f) == 0)
            return 0;
        f.phase = 0;
        return 1;
    }, [](Frame& f) {
        if (f.phase == 10) {
            f.phase = 0;
            return (int)sol::kGoOn;
        }
        if (f.phase == 1) {
            s32 value = 0;
            const Ref r = f.thread->result;
            if (!r || r->type() != sol::Type::Int)
                return (int)sol::kError;
            value = static_cast<sol::Int*>(r.get())->value;
            if (value != 0)
                return (int)sol::kEnd;
            f.phase++;
            return (int)sol::kGoOn;
        }
        if (f.phase != 0) {
            f.phase++;
            return (int)sol::kYield;
        }
        f.phase = 1;
        sol::call(*f.thread, f.receiver, sol::arg(f, 0), sVm->newList(), f.scope);
        return (int)sol::kGoOn;
    });

    // Rapid([ops]): run faster until the next Wait (0x1E9A50; default *0x2EDD50 = 100).
    Reg(root, "Rapid", 0, [](Player* self, Frame& f) {
        self->budget = IntArg(f, 0, 100);
        return 1;
    });

    // Require(name): run another script here, in this thread (0x1EA100).
    Reg(root, "Require", 1, [](Player*, Frame& f) {
        const std::u16string* name = sol::argString(f, 0);
        const Ref code = name ? LoadScript(sol::narrow(*name)) : nullptr;
        if (!code) {
            sVm->error("script not found: " + (name ? sol::narrow(*name) : std::string("?")));
            return 0;
        }
        sol::call(*f.thread, sVm->root, code, sVm->newList(), sVm->newObject());
        return 1;
    });

    // StartScript(name): a new script on a new thread (0x1EA8C0).
    Reg(root, "StartScript", 1, [](Player* self, Frame& f) {
        const std::u16string* name = sol::argString(f, 0);
        CharaResources_Purge(CharaResources_Get());
        sVm->root->attrs.erase("OnCancelEvent");                // 0x1E2D90
        const Ref code = name ? LoadScript(sol::narrow(*name)) : nullptr;
        if (!code) {
            sVm->error("script not found: " + (name ? sol::narrow(*name) : std::string("?")));
            return 0;
        }
        printf("[event] StartScript %s\n", sol::narrow(*name).c_str());
        sVm->start(sVm->root, code, sVm->newList(), sVm->newObject());
        if (gLoadingActive == 0)
            gLoadingActive = 1;                         // (0x1E0700 also restarts the dots)
        self->budget = 9999;
        if (self->scene != 0 && guest::ptr<s32>(self->scene)[2] == 0 && *gPlayerData != nullptr)
            memset(*gPlayerData + 0x5C0, 0, 0x100);
        return 1;
    });
}

// ═══ EventFlag (0x2460E0): the calendar and the story flags ═══════════════
void BuildEventFlag(const Ref& o) {
    Reg(o, "GetDay", 0, [](Player*, Frame& f) {                 // 0x24BBB0
        Answer(f, (*gPlayerData)[0x100] + 1);
        return 1;
    });
    Reg(o, "SetDay", 1, [](Player*, Frame& f) {                 // 0x24B9F0
        const s32 day = IntArg(f, 0) - 1;
        if (day >= 0 && day < 10)
            (*gPlayerData)[0x100] = (u8)day;
        return 1;
    });
    Reg(o, "GetPhase", 0, [](Player*, Frame& f) {               // 0x24B870
        Answer(f, (*gPlayerData)[0x101] + 1);
        return 1;
    });
    Reg(o, "SetPhase", 1, [](Player*, Frame& f) {               // 0x24B6B0
        const s32 phase = IntArg(f, 0) - 1;
        if (phase >= 0 && phase < 5)
            (*gPlayerData)[0x101] = (u8)phase;
        return 1;
    });
    Reg(o, "AddPhase", 1, [](Player*, Frame& f) {               // 0x24B550
        reinterpret_cast<s32*>(DayMgr_Get())[5] += IntArg(f, 0);
        return 1;
    });
    Reg(o, "GetMapID", 0, [](Player*, Frame& f) {               // 0x24B3D0
        const u8* pd = *gPlayerData;
        Answer(f, MapDb_GetAreaMap(MapDb_Get(), pd[0x103], pd[0x102]));
        return 1;
    });
    Reg(o, "SetMapID", 1, [](Player*, Frame& f) {               // 0x24B260
        (*gPlayerData)[0x103] = (u8)IntArg(f, 0);
        return 1;
    });
    Reg(o, "GetFlag", 1, [](Player*, Frame& f) {                // 0x24BD10
        Answer(f, (*gPlayerData)[0x740 + (IntArg(f, 0) & 0xFF)]);
        return 1;
    });
    Reg(o, "SetFlag", 2, [](Player*, Frame& f) {                // 0x24BF80
        s32 index = IntArg(f, 0);
        index = index < 0 ? 0 : index > 0xFF ? 0xFF : index;    // (ClampIndex)
        (*gPlayerData)[0x740 + index] = (u8)IntArg(f, 1);
        return 1;
    });
}

// ═══ Locator (0x216C20): a place and a facing — a 4×4 matrix ══════════════
struct Locator : sol::Obj {
    alignas(16) float m[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    sol::Type type() const override { return sol::Type::Other; }
    Ref clone() const override {
        auto copy = std::make_shared<Locator>(*this);
        copy->attrs.clear();
        copy->cloneAttrs(*this);
        return copy;
    }
};
inline Locator* AsLocator(const Ref& r) { return dynamic_cast<Locator*>(r.get()); }
// Number arguments of the built-ins are floats; anything else reads as the default.
inline float FloatArg(Frame& f, size_t i, float otherwise = 0.0f) {
    const Ref a = sol::arg(f, i);
    return a && a->type() == sol::Type::Float ? static_cast<sol::Float*>(a.get())->value : otherwise;
}
float sLocatorOrigin[4] = {0, 0, 0, 1};                 // 0x30AA20

void BuildLocator(const Ref& o) {
    Reg(o, "SetXYZ", 0, [](Player*, Frame& f) {                 // 0x217F80
        if (Locator* l = AsLocator(f.receiver)) {
            static const float kDefault[4] = {0, 0, 0, 1};
            for (int i = 0; i < 4; i++)
                l->m[12 + i] = FloatArg(f, (size_t)i, kDefault[i]);
        }
        f.thread->result = f.receiver;
        return 1;
    });
    Reg(o, "SetOriginXYZ", 0, [](Player*, Frame& f) {           // 0x217C30
        static const float kDefault[4] = {0, 0, 0, 1};
        for (int i = 0; i < 4; i++)
            sLocatorOrigin[i] = FloatArg(f, (size_t)i, kDefault[i]);
        return 1;
    });
    // SetDirection(locator): face that place, upright (0x217910).
    Reg(o, "SetDirection", 0, [](Player*, Frame& f) {
        Locator* l = AsLocator(f.receiver);
        if (l == nullptr)
            return 1;
        float target[4] = {0, 0, 0, 1};
        if (Locator* to = AsLocator(sol::arg(f, 0)))
            memcpy(target, &to->m[12], sizeof target);
        float d[3] = {target[0] - l->m[12], target[1] - l->m[13], target[2] - l->m[14]};
        const float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (length == 0.0f)
            return 1;
        for (float& c : d)
            c /= length;
        float* x = &l->m[0];
        float* y = &l->m[4];
        float* z = &l->m[8];
        z[0] = d[0]; z[1] = d[1]; z[2] = d[2]; z[3] = 0;
        y[0] = 0; y[1] = 1; y[2] = 0; y[3] = 0;
        x[0] = y[1] * z[2] - y[2] * z[1]; x[1] = y[2] * z[0] - y[0] * z[2]; x[2] = y[0] * z[1] - y[1] * z[0]; x[3] = 0;
        y[0] = z[1] * x[2] - z[2] * x[1]; y[1] = z[2] * x[0] - z[0] * x[2]; y[2] = z[0] * x[1] - z[1] * x[0];
        return 1;
    });
    Reg(o, "SetScale", 0, [](Player*, Frame& f) {               // 0x217570
        if (Locator* l = AsLocator(f.receiver))
            for (int row = 0; row < 3; row++) {
                float* v = &l->m[row * 4];
                const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                const float scale = FloatArg(f, (size_t)row, 1.0f) / (length > 0 ? length : 1);
                for (int i = 0; i < 4; i++)
                    v[i] *= scale;
            }
        return 1;
    });
    Reg(o, "AddXYZ", 0, [](Player*, Frame& f) {                 // 0x217100
        if (Locator* l = AsLocator(f.receiver))
            for (int i = 0; i < 3; i++)
                l->m[12 + i] += FloatArg(f, (size_t)i);
        f.thread->result = f.receiver;
        return 1;
    });
    Reg(o, "Pos", 0, [](Player*, Frame& f) {                    // 0x216DF0
        if (Locator* l = AsLocator(f.receiver))
            f.thread->result = sVm->newFloat(l->m[12 + (IntArg(f, 0) & 3)]);
        return 1;
    });
}
// L(x, y, z): a new locator there (0x1E9BB0).
Ref NewLocator(float x, float y, float z) {
    const Ref made = sol::getAttr(*sVm, sVm->root, "Locator")->clone();
    if (Locator* l = AsLocator(made)) {
        static const Locator kFresh;
        memcpy(l->m, kFresh.m, sizeof l->m);
        l->m[12] = x;
        l->m[13] = y;
        l->m[14] = z;
    }
    return made;
}

// ═══ Map (0x1E4EA0) ═══════════════════════════════════════════════════════
inline World* TheWorld(Player* self) { return guest::ptr<World>(self->scene); }
// 0x1E2AC0: the world's map state; its load counter for where the player is.
inline s32* MapStateOf(Player* self) { return guest::ptr<s32>(TheWorld(self)->mapState); }
inline bool MapLoaded(Player* self) {
    const s32* state = MapStateOf(self);
    return (*guest::ptr<s32>(0x2EE518u) == 0 ? state[3] : state[4]) == -1;
}

void BuildMap(const Ref& o) {
    // Load(map): go to another map.  If one is still coming in, wait for it
    // and ask again (0x1E5310).
    Reg(o, "Load", 1, [](Player* self, Frame& f) {
        const s32 map = IntArg(f, 0);
        if (map == MapStateOf(self)[1])
            return 1;
        if (MapLoaded(self)) {
            World* world = TheWorld(self);
            World_GoToMap(world, (s16)map);
            *reinterpret_cast<float*>(reinterpret_cast<u8*>(world) + 0xB0) = 0.1f;
            return 1;
        }
        sol::call(*f.thread, f.receiver, sol::getAttr(*sVm, f.receiver, "Load"), f.args, sVm->newObject());
        sol::call(*f.thread, f.receiver, sol::getAttr(*sVm, f.receiver, "WaitLoading"), sVm->newList(),
                  sVm->newObject());
        return 1;
    });
    // WaitLoading(): until the map is in (0x1E5890).
    // SetWindGain(strength): World+0xB0 (1 outdoors, 0.1 indoors by default;
    // 0x1E51A0, which otherwise only prints it).  Nothing decompiled so far
    // reads it: the wind the swing takes (CharaMgr+0x20) stays still.
    Reg(o, "SetWindGain", 0, [](Player* self, Frame& f) {
        if (self->scene != 0)
            *reinterpret_cast<float*>(reinterpret_cast<u8*>(TheWorld(self)) + 0xB0) = FloatArg(f, 0);
        return 1;
    });
    // (Native loading is done at once, but a real load never ends in the
    // frame it is asked for — and the scene counts on that: a door's script
    // starts a frame before the scene blacks the screen "while loading", so
    // a script that got as far as its Wipe(0) at once would have it undone.)
    RegWait(o, "WaitLoading", 0, nullptr, [](Frame& f) {
        if (f.phase == 0) {
            f.phase = 1;
            return (int)sol::kYield;
        }
        return MapLoaded(sSelf) ? (int)sol::kEnd : (int)sol::kYield;
    });
}

// ═══ Character (0x1F1780) ═════════════════════════════════════════════════
// Script characters are ordinary characters of the world, made through
// World_AddActor.  One that carries a sword comes as two: `id` unarmed and
// `id + 1000` armed, only one of them switched on.  Id 0 is the player and
// 100 its double for acted scenes ("performance"), which looks like the
// player.
inline void* Charas(Player* self) {                     // 0x1E2AA0
    return self->scene && TheWorld(self)->charaMgr ? guest::ptr(TheWorld(self)->charaMgr) : nullptr;
}
inline const CharaParams* ParamsOf(void* charas, s32 id) {
    return reinterpret_cast<const CharaParams*>(CharaMgr_GetParams(charas, id));
}
inline s32 ArmedId(s32 id) { return id < 1000 && id != 100 && id != 0 ? id + 1000 : id; }
// The one of the pair that is switched on.
s32 ActiveId(Player* self, s32 id) {
    void* charas = Charas(self);
    if (charas == nullptr)
        return id;
    const s32 armed = ArmedId(id);
    const CharaParams* p = ParamsOf(charas, armed);
    if (p != nullptr && p->link[4] != 0 && *guest::ptr<s32>(p->link[4]) != 0)
        return armed;
    return id;
}
std::vector<s32> sScriptCharas;                         // 0x30A980: made by the running script
// Deleted by a script but kept in the world, renumbered id + 10000, until the
// next script has loaded its own cast and calls FormerCleanUp (so that what
// the two scenes share is not unloaded and read again).
std::vector<s32> sKeptCharas;                           // 0x30A960
void ForgetChara(s32 id) {                              // 0x1F9CC0
    for (auto* list : {&sScriptCharas, &sKeptCharas})
        for (auto it = list->begin(); it != list->end();)
            it = *it == id ? list->erase(it) : it + 1;
}

// 0x1F9DE0: `to` takes over from `from` — its place, its being switched on,
// whether the sword is out, its side and its health; with `motion`, the
// motion it is in the middle of too.
void SwapBody(Player* self, s32 to, s32 from, bool motion) {
    void* charas = Charas(self);
    if (charas == nullptr || to == from)
        return;
    CharaParams* a = reinterpret_cast<CharaParams*>(CharaMgr_GetParams(charas, to));
    CharaParams* b = reinterpret_cast<CharaParams*>(CharaMgr_GetParams(charas, from));
    if (a == nullptr || b == nullptr)
        return;
    memcpy(a->matrix, b->matrix, sizeof a->matrix);
    if (a->link[4] != 0)
        *guest::ptr<s32>(a->link[4]) = 1;
    if (b->link[4] != 0)
        *guest::ptr<s32>(b->link[4]) = 0;
    if ((CharaMgr_GetFlags(charas, from) & 0x100) == 0)
        CharaMgr_Sheathe(charas, to, 0);
    else
        CharaMgr_Draw_Sword(charas, to, 0);
    a->faction = b->faction;
    if (to != 0) {
        a->hp = b->hp < a->maxHp ? b->hp : a->maxHp;
        a->maxHp = b->maxHp;
    }
    if (motion) {
        static u32 request = 0;
        if (request == 0)
            request = guest::addr(guest::alloc(0x18));
        s32* r = guest::ptr<s32>(request);
        r[0] = b->link[0] != 0 ? *guest::ptr<s32>(b->link[0]) : -1;
        r[1] = 0;
        r[2] = 0;
        r[3] = CharaMgr_GetHealthShown(charas, from);       // (the frame its motion is at)
        r[4] = -1;
        r[5] = 0;
        CharaMgr_fn143bf0(charas, to, request);
    }
}
// 0x1F9FB0: which of a pair is the one in the scene — the unarmed body or
// the armed one.
void UseBody(Player* self, s32 id, bool armed) {
    void* charas = Charas(self);
    if (charas == nullptr || id >= 1000)
        return;
    const s32 other = ArmedId(id);
    const CharaParams* p = ParamsOf(charas, other);
    if (p == nullptr)
        return;
    const bool on = p->link[4] != 0 && *guest::ptr<s32>(p->link[4]) != 0;
    if (armed && !on)
        SwapBody(self, other, id, true);
    else if (!armed && on)
        SwapBody(self, id, other, false);
}

void BuildCharacter(const Ref& o) {
    // Load(id, parameters, sword, motion pack, flag) — 0x1F8F80.  `sword`:
    // −1 none, −2 none either, 0 its own (the player's for the double), else
    // that sword.
    Reg(o, "Load", 2, [](Player* self, Frame& f) {
        const s32 id = IntArg(f, 0), kind = IntArg(f, 1), swordArg = IntArg(f, 2, -1);
        const s32 pack = IntArg(f, 3, -1), flag = IntArg(f, 4);
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        if (static_cast<s32*>(charas)[0x30 / 4] == 1)                   // only the player is there: a fresh scene
            sScriptCharas.clear();
        if (CharaMgr_GetParams(charas, id) != nullptr)
            return 1;                                   // already there
        const CharaParams* tmpl = CharaResources_FindParamTemplate(CharaResources_Get(), kind);
        if (tmpl == nullptr) {
            sVm->error("Character.Load: no such parameters " + std::to_string(kind));
            return 1;
        }
        s32 model = tmpl->chara;
        s32 sword = swordArg == -2 ? -1 : swordArg, other = -1;
        if (swordArg != -1) {
            other = tmpl->sword;
            if (other == -1)
                other = 0;
        }
        if (id == 0 || id == 100) {
            model = *reinterpret_cast<s16*>(*gPlayerData + 0x112);
            if (id == 100 && sword == 0)
                if (const CharaParams* player = ParamsOf(charas, 0))
                    sword = player->sword;
        }
        if (sword == 0) {       // not named: the sword table's choice, else the template's
            const int picked = CharaResources_PickSword(CharaResources_Get(), kind);
            sword = other = picked != -1 ? picked : tmpl->sword;
        }
        CharaResources_UseSword(CharaResources_Get(), sword);
        CharaResources_UseSword(CharaResources_Get(), other);
        World* world = TheWorld(self);
        sScriptCharas.push_back(id);
        ActorDesc desc;
        memcpy(&desc, guest::ptr(0x2D9540u), sizeof desc);      // parked at (0, −20, 0), state 2
        desc.id = id;
        desc.kind = kind;
        desc.chara = model;
        desc.sword = sword;
        World_AddActor(world, &desc, pack, flag);
        CharaMgr_ResetTint(charas, id);
        if (id != 100 && swordArg != -1 && flag == 0) {
            const s32 armed = ArmedId(id);
            memcpy(&desc, guest::ptr(0x2D9580u), sizeof desc);
            desc.id = armed;
            desc.kind = kind;
            desc.chara = model;
            desc.sword = other;
            World_AddActor(world, &desc, -1, 0);
            CharaMgr_SetActive(charas, armed, 0);
            CharaMgr_ResetTint(charas, armed);
        }
        if (id == 100 && CharaMgr_fn144460(charas, 0) == 1)
            CharaMgr_fn1443f0(charas, id);              // the player's sword is broken: so is the double's
        // The player and its double wear the player's accessory (0x2AB910).
        if (id == 0 || id == 100)
            if (void* params = CharaMgr_GetParams(charas, id))
                Accessories_AddWearer(params);
        return 1;
    });

    // WaitLoading(id): until both of the pair are loaded (0x1FE780 / 0x1FE9A0).
    RegWait(o, "WaitLoading", 0, [](Frame& f) {
        f.phase = IntArg(f, 0);
        f.value = ArmedId(f.phase);
        void* charas = Charas(sSelf);
        if (charas != nullptr && CharaMgr_GetParams(charas, f.value) == nullptr)
            f.value = f.phase;
        return 1;
    }, [](Frame& f) {
        void* charas = Charas(sSelf);
        if (charas == nullptr)
            return (int)sol::kError;
        if (CharaMgr_GetParams(charas, f.phase) == nullptr)
            return (int)sol::kEnd;
        if (CharaMgr_IsLoaded(charas, f.phase) == 0 || CharaMgr_IsLoaded(charas, f.value) == 0)
            return (int)sol::kYield;
        return (int)sol::kEnd;
    });

    // PlayMotion(id, motion, a, b, c, d, flag) — 0x1FCE70 / 0x1FD420: asks
    // the character for the motion as soon as it is loaded; does not wait
    // for the motion to end (WaitMotion does).
    RegWait(o, "PlayMotion", 0, [](Frame& f) {
        static const s32 kDefault[5] = {0, 1, -1, -1, -1};
        auto req = std::make_shared<sol::List>();           // (just a box for the numbers)
        for (int i = 0; i < 5; i++)
            req->items.push_back(sVm->newInt(IntArg(f, (size_t)i + 1, kDefault[i])));
        req->items.push_back(sVm->newInt(IntArg(f, 6)));
        f.keep = req;
        f.phase = IntArg(f, 0);
        f.value = 0;
        return 1;
    }, [](Frame& f) {
        void* charas = Charas(sSelf);
        if (f.value != 0 || charas == nullptr || CharaMgr_GetParams(charas, f.phase) == nullptr)
            return (int)sol::kEnd;
        if (CharaMgr_IsLoaded(charas, f.phase) == 0)
            return (int)sol::kYield;
        UseBody(sSelf, f.phase, false);                     // 0x1F9FB0(id, 0)
        // The request, in guest memory for the character code: five ints and a flag.
        static u32 request = 0;
        if (request == 0)
            request = guest::addr(guest::alloc(0x18));
        const sol::List* req = static_cast<const sol::List*>(f.keep.get());
        for (int i = 0; i < 5; i++)
            guest::ptr<s32>(request)[i] = static_cast<sol::Int*>(req->items[(size_t)i].get())->value;
        guest::ptr<u8>(request)[0x14] = (u8)static_cast<sol::Int*>(req->items[5].get())->value;
        CharaMgr_fn143bf0(charas, f.phase, request);
        f.value = 1;
        return (int)sol::kEnd;
    });

    // Move(id, how, x, y, z [, avoid, speed]) — 0x1F5330: send it walking
    // there.  how: 0 walk / 1 run along the map's routes (AI move 2), 2 / 3
    // the same in a straight line (AI move 8).
    Reg(o, "Move", 5, [](Player* self, Frame& f) {
        const s32 id = IntArg(f, 0), how = IntArg(f, 1);
        alignas(16) float point[4] = {FloatArg(f, 2) + sLocatorOrigin[0], FloatArg(f, 3) + sLocatorOrigin[1],
                                      FloatArg(f, 4) + sLocatorOrigin[2], 1.0f};
        UseBody(self, id, false);
        void* charas = Charas(self);
        if (charas == nullptr || TheWorld(self)->mapState == 0)
            return 1;
        static u32 data = 0;                            // the request, in guest memory
        if (data == 0)
            data = guest::addr(guest::alloc(0x40));
        u32* w = guest::ptr<u32>(data);
        memset(w, 0, 0x40);
        if (how == 0 || how == 1) {
            const u32 links = TheWorld(self)->mapState + (u32)*guest::ptr<s32>(0x2EE518u) * 0x10;
            w[0] = guest::addr(MapDb_GetRoutePoints(MapDb_Get(), (*gPlayerData)[0x103]));
            w[1] = links + 0x20;
            w[2] = links + 0x60;
            memcpy(&w[4], point, 16);
            const float speed = FloatArg(f, 6, 0.02f);
            memcpy(&w[8], &speed, 4);
            w[9] = (u32)how;
            w[10] = IntArg(f, 5) != 0;
            CharaMgr_SetState(charas, id, 2, 2, data);
        } else {
            memcpy(&w[0], point, 16);
            w[4] = how != 2;
            CharaMgr_SetState(charas, id, 2, 8, data);
        }
        return 1;
    });

    // SetObject(id, object): a new object of that kind, straight into the
    // character's hand (0x1F82F0; the id is taken as given, not its pair's).
    Reg(o, "SetObject", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        alignas(16) static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        void* object = ItemDrops_Place(guest::ptr(TheWorld(self)->itemDrops), IntArg(f, 1), kIdentity);
        CharaMgr_fn1438d0(charas, guest::addr(object), IntArg(f, 0));
        return 1;
    });
    // TaskInit(id): hands it back — the player to the pad, anyone else to
    // standing about (0x1F5140).
    Reg(o, "TaskInit", 1, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        const s32 id = IntArg(f, 0);
        if (id == 0)
            CharaMgr_SetState(charas, 0, 0, 0, 0);
        else
            CharaMgr_SetState(charas, id, 2, 0, 0);
        return 1;
    });

    // LookAtChara(id, whom, part): turn the head to a part of someone
    // (0x1F7500; the bone of each part is the table at 0x2D95C0).
    Reg(o, "LookAtChara", 2, [](Player* self, Frame& f) {
        const s32 id = IntArg(f, 0), whom = IntArg(f, 1), part = IntArg(f, 2);
        void* charas = Charas(self);
        if (id == whom || charas == nullptr || part < 0 || part > 8)
            return 1;
        const CharaParams* p = ParamsOf(charas, whom);
        if (p == nullptr || CharaMgr_IsLoaded(charas, whom) == 0 || p->link[1] == 0)
            return 1;
        alignas(16) float m[16];
        memcpy(m, p->matrix, sizeof m);
        Skeleton* skeleton = guest::ptr<Skeleton>(p->link[1]);
        Skeleton_UpdateWorld(skeleton, m);
        const s32 bone = guest::ptr<const s32>(0x2D95C0u)[part];
        alignas(16) float point[4];
        memcpy(point, &guest::ptr<Bone>(skeleton->bones)[bone].world[12], sizeof point);
        CharaMgr_fn146740(charas, id, point);
        return 1;
    });

    // Delete(id) — 0x1F7990.  The player (or its double) goes at once; anyone
    // else is kept aside (see sKeptCharas) unless too many are already.
    Reg(o, "Delete", 1, [](Player* self, Frame& f) {
        const s32 id = IntArg(f, 0);
        World* world = TheWorld(self);
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        if (id == 0 || id == 100) {
            if (void* params = CharaMgr_GetParams(charas, id))
                Accessories_RemoveWearer(params);               // 0x2AB7D0
            World_RemoveActor(world, id);
            ForgetChara(id);
            return 1;
        }
        const s32 armed = ArmedId(id);
        if (sKeptCharas.size() < 9) {
            sKeptCharas.push_back(id);
            CharaMgr_SetState(charas, id, 2, 0, 0);
            CharaMgr_SetState(charas, armed, 2, 0, 0);
            CharaMgr_Renumber(charas, id, id + 10000);
            CharaMgr_Renumber(charas, armed, armed + 10000);
        } else {
            World_RemoveActor(world, id);
            World_RemoveActor(world, armed);
            ForgetChara(id);
        }
        return 1;
    });

    // FormerCleanUp(): the last script's cast really goes (0x1F7E10).
    Reg(o, "FormerCleanUp", 0, [](Player* self, Frame&) {
        World* world = TheWorld(self);
        const std::vector<s32> kept = sKeptCharas;
        for (s32 id : kept) {
            World_RemoveActor(world, id + 10000);
            World_RemoveActor(world, ArmedId(id) + 10000);
            ForgetChara(id);
        }
        sKeptCharas.clear();
        return 1;
    });

    // PlayFrameMotion(id, motion, from, to [, loop, blend, flag]) — 0x1F8570:
    // a stretch of a motion; from = to holds that frame.
    Reg(o, "PlayFrameMotion", 2, [](Player* self, Frame& f) {
        const s32 id = IntArg(f, 0), from = IntArg(f, 2), to = IntArg(f, 3, from);
        UseBody(self, id, false);
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        static u32 request = 0;
        if (request == 0)
            request = guest::addr(guest::alloc(0x18));
        s32* r = guest::ptr<s32>(request);
        r[0] = IntArg(f, 1);
        r[1] = IntArg(f, 4, from == to ? -1 : 1);
        r[2] = IntArg(f, 5);
        r[3] = from;
        r[4] = to;
        guest::ptr<u8>(request)[0x14] = (u8)IntArg(f, 6, 1);
        CharaMgr_fn143bf0(charas, id, request);
        if (from == to)
            CharaMgr_fn143b80(charas, id);
        return 1;
    });

    // WaitMotion(id, frame [, hold]): until its motion reaches that frame —
    // or that many frames have gone by, should it never get there
    // (0x1FCA60 / 0x1FCD00).
    RegWait(o, "WaitMotion", 0, [](Frame& f) {
        f.phase = IntArg(f, 0);
        f.value = IntArg(f, 1);
        f.keep = IntArg(f, 2) != 0 ? sVm->newInt(1) : nullptr;     // hold the motion there
        f.timer = 0;                                               // frames waited
        return 1;
    }, [](Frame& f) {
        if ((float)f.value < f.timer)
            return (int)sol::kEnd;
        void* charas = Charas(sSelf);
        if (charas == nullptr)
            return (int)sol::kError;
        if (f.value != CharaMgr_GetHealthShown(charas, f.phase)) {
            f.timer += 1.0f;
            return (int)sol::kYield;
        }
        if (f.keep)
            CharaMgr_fn143b80(charas, f.phase);
        return (int)sol::kEnd;
    });

    // SetFace(id, expression 0–5) — 0x1F6970 → 0x1433F0: the face's
    // expression layer (anything else is 0, the plain face).
    Reg(o, "SetFace", 2, [](Player* self, Frame& f) {
        s32 face = IntArg(f, 1);
        if (face < 0 || face > 5)
            face = 0;
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        CharaMgr_fn1433f0(charas, IntArg(f, 0), (u32)face);
        return 1;
    });

    // MoveWait(id): until its walk (Move) is over — or it has been stuck for
    // 46 frames (0x1FE2F0 / 0x1FE4D0).  Then it stands again.
    RegWait(o, "MoveWait", 0, [](Frame& f) {
        f.phase = IntArg(f, 0);
        f.value = 0;
        void* charas = Charas(sSelf);
        if (charas == nullptr)
            return 0;
        auto last = std::make_shared<Locator>();
        memcpy(&last->m[12], CharaMgr_GetPosition(charas, f.phase), 16);
        f.keep = last;
        return 1;
    }, [](Frame& f) {
        void* charas = Charas(sSelf);
        if (charas == nullptr)
            return (int)sol::kError;
        const s32 id = f.phase;
        const bool field = sSelf->scene != 0 && guest::ptr<s32>(sSelf->scene)[2] == 1;
        if (CharaMgr_GetAction(charas, id) == 0) {
            if (f.value > 0x2D) {
                Locator* last = AsLocator(f.keep);
                const float* now = CharaMgr_GetPosition(charas, id);
                const float dx = now[0] - last->m[12], dy = now[1] - last->m[13], dz = now[2] - last->m[14];
                if (sqrtf(dx * dx + dy * dy + dz * dz) < 0.2f) {
                    if (id != 0)
                        CharaMgr_SetState(charas, id, 2, 0, 0);
                    else if (!field)
                        CharaMgr_SetState(charas, 0, 0, 0, 0);
                    return (int)sol::kEnd;
                }
                f.value = 0;
                memcpy(&last->m[12], now, 16);
            }
            f.value++;
            return (int)sol::kYield;
        }
        if (id != 0)
            CharaMgr_SetState(charas, id, 2, 0, 0);
        else if (field)
            CharaMgr_SetState(charas, 0, 2, 0, 0);
        else
            CharaMgr_SetState(charas, 0, 0, 0, 0);
        return (int)sol::kEnd;
    });

    // TurnDirection(id, x, y, z [, degrees a frame = 15]): turn to face a
    // place (0x1FDCA0 / 0x1FE100).
    RegWait(o, "TurnDirection", 0, [](Frame& f) {
        void* charas = Charas(sSelf);
        if (charas == nullptr)
            return 0;
        f.phase = IntArg(f, 0);
        auto to = std::make_shared<Locator>();
        for (int i = 0; i < 3; i++)
            to->m[12 + i] = FloatArg(f, (size_t)i + 1) + sLocatorOrigin[i];
        f.keep = to;
        f.timer = FloatArg(f, 4, 15.0f);
        f.value = CharaMgr_GetB5(charas, f.phase);
        return 1;
    }, [](Frame& f) {
        void* charas = Charas(sSelf);
        if (charas == nullptr || CharaMgr_GetParams(charas, f.phase) == nullptr)
            return (int)sol::kError;
        alignas(16) float m[16];
        memcpy(m, CharaMgr_GetTransform(charas, f.phase), sizeof m);
        CharaMgr_fn1466e0(charas, f.phase, 0);
        if (Matrix_TurnTowards(f.timer, m, &AsLocator(f.keep)->m[12]) == 1) {
            if (f.value != 0)
                CharaMgr_fn1466e0(charas, f.phase, f.value);
            return (int)sol::kEnd;
        }
        if (getenv("WOTS2_TRACE_TURN"))
            printf("[turn %d] %d at %.2f %.2f facing %.2f %.2f → %.2f %.2f\n", sSelf->frames, f.phase, m[12], m[14],
                   m[8], m[10], AsLocator(f.keep)->m[12], AsLocator(f.keep)->m[14]);
        memcpy(const_cast<CharaParams*>(ParamsOf(charas, f.phase))->matrix, m, sizeof m);
        return (int)sol::kYield;
    });

    // LookAtPos(id [, locator]): turn the head to a place; with no place,
    // stop looking (0x1F71B0).
    Reg(o, "LookAtPos", 1, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        const s32 id = IntArg(f, 0);
        Locator* l = AsLocator(sol::arg(f, 1));
        if (sol::argCount(f) == 1 || l == nullptr) {
            CharaMgr_fn1466e0(charas, id, 0);
            return 1;
        }
        alignas(16) float point[4] = {l->m[12] + sLocatorOrigin[0], l->m[13] + sLocatorOrigin[1],
                                      l->m[14] + sLocatorOrigin[2], 1.0f};
        CharaMgr_fn146740(charas, id, point);
        return 1;
    });

    // LookIDChara(id, whom): turn its whole body to face someone, at once (0x1F5A30).
    Reg(o, "LookIDChara", 2, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        const s32 id = IntArg(f, 0), whom = IntArg(f, 1);
        if (charas == nullptr || CharaMgr_GetParams(charas, id) == nullptr
            || CharaMgr_GetParams(charas, whom) == nullptr)
            return 1;
        alignas(16) float m[16];
        memcpy(m, CharaMgr_GetTransform(charas, id), sizeof m);
        const float* to = static_cast<const float*>(CharaMgr_GetTransform(charas, whom)) + 12;
        float z[3] = {to[0] - m[12], to[1] - m[13], to[2] - m[14]};
        const float length = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
        if (length == 0.0f)
            return 1;
        for (float& c : z)
            c /= length;
        const float x[3] = {z[2], 0.0f, -z[0]};                         // up × z
        const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        for (int i = 0; i < 3; i++) {
            m[i] = x[i];
            m[4 + i] = y[i];
            m[8 + i] = z[i];
        }
        m[3] = m[7] = m[11] = 0.0f;
        CharaMgr_SetTransform(charas, id, m);
        return 1;
    });

    // PauseTask([id]) / ResumeTask([id]): hold a character still / let it go
    // (0x1F67B0 / 0x1F65F0).  For the player it also changes what kind of
    // world this is (World+8: 0 a story scene, 2).
    Reg(o, "PauseTask", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        const s32 id = IntArg(f, 0);
        CharaMgr_SetIdle(charas, id);
        if (id == 0 && self->scene != 0)
            guest::ptr<s32>(self->scene)[2] = 0;
        return 1;
    });
    Reg(o, "ResumeTask", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        const s32 id = IntArg(f, 0);
        CharaMgr_Release(charas, id);
        if (id == 0 && self->scene != 0)
            guest::ptr<s32>(self->scene)[2] = 2;
        return 1;
    });

    // SetPos(id, locator): place it, relative to the scripts' origin (0x1F8BE0).
    Reg(o, "SetPos", 2, [](Player* self, Frame& f) {
        alignas(16) float m[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
        if (Locator* l = AsLocator(sol::arg(f, 1)))
            memcpy(m, l->m, sizeof m);
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        alignas(16) float origin[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};    // 0x30A9F0
        memcpy(&origin[12], sLocatorOrigin, sizeof sLocatorOrigin);
        alignas(16) float placed[16];
        Matrix_Multiply(placed, origin, m);             // 0x2184E0
        CharaMgr_SetTransform(charas, ActiveId(self, IntArg(f, 0)), placed);
        if (getenv("WOTS2_TRACE_EVENTS"))               // debug hook
            printf("[event] SetPos %d → %.2f %.2f %.2f  (facing %.2f %.2f)\n", ActiveId(self, IntArg(f, 0)),
                   placed[12], placed[13], placed[14], placed[8], placed[10]);
        return 1;
    });

    // GetPos(id): where it is, as a new locator (0x1F5D50).
    Reg(o, "GetPos", 1, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        const CharaParams* p = ParamsOf(charas, ActiveId(self, IntArg(f, 0)));
        if (p == nullptr)
            return 0;
        const Ref made = sol::getAttr(*sVm, sVm->root, "Locator")->clone();
        if (Locator* l = AsLocator(made)) {
            alignas(16) float back[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};     // 0x218490
            for (int i = 0; i < 3; i++)
                back[12 + i] = -sLocatorOrigin[i];
            Matrix_Multiply(l->m, back, p->matrix);
        }
        f.thread->result = made;
        return 1;
    });

    // SetVisible(id, on) — 0x1F62F0.
    // PlayWeapon(id, on): the sword is in its hand, or not, without the
    // motion of drawing it (0x1F4990).
    Reg(o, "PlayWeapon", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        if (IntArg(f, 1) == 1)
            CharaMgr_fn143d40(charas, IntArg(f, 0));
        else
            CharaMgr_fn143cd0(charas, IntArg(f, 0));
        return 1;
    });
    // QuickDrawSword(id, draw = 1, quiet): draws or sheathes (0x1F4C10).
    Reg(o, "QuickDrawSword", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        if (IntArg(f, 1, 1) == 1)
            CharaMgr_Draw_Sword(charas, IntArg(f, 0), IntArg(f, 2));
        else
            CharaMgr_Sheathe(charas, IntArg(f, 0), IntArg(f, 2));
        return 1;
    });
    // NotDead(id, on = 1, floor = its full health): its health does not go
    // below the floor, for both its bodies; off = −1 (0x1F3D40).
    Reg(o, "NotDead", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        const s32 id = IntArg(f, 0);
        CharaParams* p = reinterpret_cast<CharaParams*>(CharaMgr_GetParams(charas, id));
        CharaParams* armed = reinterpret_cast<CharaParams*>(CharaMgr_GetParams(charas, ArmedId(id)));
        if (p == nullptr)
            return 0;
        s16 floor = -1;
        if (IntArg(f, 1, 1) == 1)
            floor = (s16)IntArg(f, 2, armed != nullptr ? armed->maxHp : p->maxHp);
        p->_48 = floor;
        if (armed != nullptr)
            armed->_48 = floor;
        return 1;
    });
    // SetPose(id): back to its standing pose at once (0x1F2950).
    Reg(o, "SetPose", 0, [](Player* self, Frame& f) {
        if (void* charas = Charas(self)) {
            CharaMgr_fn146230(charas, IntArg(f, 0));
            CharaMgr_fn1437d0(charas, IntArg(f, 0));
        }
        return 1;
    });
    // CopyMove(to, from): `to` takes over where `from` is, mid-motion (0x1F4F30).
    Reg(o, "CopyMove", 2, [](Player* self, Frame& f) {
        SwapBody(self, IntArg(f, 0), IntArg(f, 1), true);
        return 1;
    });
    // ChangePerformance(): after a fight, everyone still standing goes back
    // to the unarmed body the acted scenes use (0x1F2E60).
    Reg(o, "ChangePerformance", 0, [](Player* self, Frame&) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        const s32 n = *reinterpret_cast<s32*>(static_cast<u8*>(charas) + 0x30);
        for (s32 i = 0; i < n; i++) {
            const s32 id = CharaMgr_IdAt(charas, i), armed = ArmedId(id);
            if (id >= 1000 || armed == 100 || armed == 0)
                continue;
            const CharaParams* p = ParamsOf(charas, armed);
            if (p == nullptr || p->hp <= 0)
                continue;
            if (CharaMgr_fn144460(charas, armed) == 1)
                CharaMgr_fn1443f0(charas, id);
            UseBody(self, id, false);
            CharaMgr_fn146230(charas, id);
        }
        return 1;
    });
    // LookAtTarget(who, target, part 0–8): `who` keeps its eyes on a part of
    // `target` for as long as the target is on its way somewhere (a Move), or
    // until it has stopped getting anywhere (0x1FD570 / 0x1FD900).
    RegWait(o, "LookAtTarget", 0, [](Frame& f) {
        void* charas = Charas(sSelf);
        f.phase = IntArg(f, 0, -1);
        f.value = IntArg(f, 1, -1);
        auto box = std::make_shared<sol::List>();
        box->items = {sVm->newInt(IntArg(f, 2)), sVm->newInt(0), sVm->newFloat(0), sVm->newFloat(0), sVm->newFloat(0)};
        f.keep = box;
        if (charas == nullptr || CharaMgr_GetParams(charas, f.value) == nullptr ||
            CharaMgr_GetParams(charas, f.phase) == nullptr)
            return 0;
        const float* at = CharaMgr_GetPosition(charas, f.value);
        for (int i = 0; i < 3; i++)
            static_cast<sol::Float*>(box->items[(size_t)i + 2].get())->value = at[i];
        return 1;
    }, [](Frame& f) {
        static const s32 kBones[9] = {20, 8, 1, 13, 17, 15, 19, 3, 6};  // 0x2D9600
        auto* box = static_cast<sol::List*>(f.keep.get());
        const s32 part = static_cast<sol::Int*>(box->items[0].get())->value;
        s32& waited = static_cast<sol::Int*>(box->items[1].get())->value;
        if (f.phase == -1 || f.value == -1 || part < 0 || part > 8)
            return (int)sol::kEnd;
        void* charas = Charas(sSelf);
        if (charas == nullptr)
            return (int)sol::kError;
        const auto stop = [&] {
            if (f.value == 0) CharaMgr_SetState(charas, 0, 0, 0, 0);
            else CharaMgr_SetState(charas, f.value, 2, 0, 0);
            return (int)sol::kEnd;
        };
        const CharaParams* target = ParamsOf(charas, f.value);
        if (target == nullptr || ParamsOf(charas, f.phase) == nullptr)
            return target == nullptr ? (int)sol::kEnd : stop();
        // The target's skeleton: model-space matrices, 0xA0 bytes a bone.
        const u32 skeleton = *guest::ptr<u32>(target->link[1] + 4);
        alignas(16) float point[4];
        memcpy(point, guest::ptr<float>(skeleton + (u32)kBones[part] * 0xA0 + 0x30), sizeof point);
        CharaMgr_fn146740(charas, f.phase, point);
        if (CharaMgr_GetAction(charas, f.value) != 0)
            return stop();
        if (waited > 30) {
            const float* at = CharaMgr_GetPosition(charas, f.value);
            float d2 = 0;
            for (int i = 0; i < 3; i++) {
                float& was = static_cast<sol::Float*>(box->items[(size_t)i + 2].get())->value;
                d2 += (at[i] - was) * (at[i] - was);
                was = at[i];
            }
            if (sqrtf(d2) < 0.2f)
                return stop();
            waited = 0;
        }
        waited++;
        return (int)sol::kYield;
    });
    // Wink(id, how 0–2): its eyes (the face's eye layer); anything else:
    // back to blinking by itself.  0x1F4700
    Reg(o, "Wink", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        const s32 how = IntArg(f, 1, -1);
        if (how < 0 || how > 2)
            CharaMgr_fn143310(charas, IntArg(f, 0));
        else
            CharaMgr_fn143380(charas, IntArg(f, 0), (u32)how);
        return 1;
    });
    // SetMouth(id, open 0 / 1): 0x1F6BD0
    Reg(o, "SetMouth", 0, [](Player* self, Frame& f) {
        const s32 open = IntArg(f, 1);
        void* charas = Charas(self);
        if (charas == nullptr || open < 0 || open > 1)
            return 0;
        CharaMgr_fn1434a0(charas, IntArg(f, 0), open == 0 ? 0u : 2u);
        return 1;
    });
    // Dead(id): it dies where it stands (0x1F41B0).
    Reg(o, "Dead", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        if (CharaMgr_IsLoaded(charas, IntArg(f, 0)) != 0)
            CharaMgr_Kill(charas, IntArg(f, 0));
        return 1;
    });
    // ChangeBattleChara(id, armed): which of its two bodies is in the scene
    // (0x1F3260); GetBattleCharaID(id): the armed one's id (0x1F2FC0).
    Reg(o, "ChangeBattleChara", 0, [](Player* self, Frame& f) {
        UseBody(self, IntArg(f, 0), IntArg(f, 1) != 0);
        return 1;
    });
    Reg(o, "GetBattleCharaID", 0, [](Player*, Frame& f) { Answer(f, ArmedId(IntArg(f, 0))); return 1; });
    // IsEndTask(id): has the move it was sent on (Move) ended.  0x1F43D0
    Reg(o, "IsEndTask", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr || ParamsOf(charas, IntArg(f, 0)) == nullptr)
            return 0;
        Answer(f, CharaMgr_GetAction(charas, IntArg(f, 0)) != 0);
        return 1;
    });
    // StopAllMotion(): everyone (both bodies) holds the frame it is at, stops
    // looking about, and opens its eyes.  0x1F2C70
    Reg(o, "StopAllMotion", 0, [](Player* self, Frame&) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        const s32 n = *reinterpret_cast<s32*>(static_cast<u8*>(charas) + 0x30);
        for (s32 i = 0; i < n; i++) {
            const s32 id = CharaMgr_IdAt(charas, i);
            for (const s32 body : {id, ArmedId(id)}) {
                CharaMgr_fn143b80(charas, body);
                CharaMgr_fn143280(charas, body, 0);
                const CharaParams* p = ParamsOf(charas, body);
                if (p != nullptr && p->link[5] != 0 && (*guest::ptr<u32>(p->link[5]) & 0x10) == 0)
                    CharaMgr_fn143380(charas, body, 0);
                if (body == ArmedId(id) && body == id)
                    break;
            }
        }
        return 1;
    });
    // PathRunningExec2(id, how, avoid, reach, [L, L …] [, state]): walks a
    // round of two to ten places, over and over, until another thread stops
    // it with SetPathRunState2(id, 2) (1 = walk on, 0 = stand and wait);
    // PathAllInit2() forgets every state.  0x1FC0C0 / 0x1FC820, 0x1F3AC0,
    // 0x1F3D10 (states: the map at 0x30A9E0).
    struct PathRun : sol::Obj {
        s32 id = 0, how = 0, avoid = 0, next = 0;
        float reach = 0;
        std::vector<std::array<float, 4>> places;
    };
    static std::map<s32, s32> pathStates;
    static const auto walkTo = [](Player* self, PathRun* run) {
        void* charas = Charas(self);
        static u32 data = 0;
        if (data == 0)
            data = guest::addr(guest::alloc(0x40));
        u32* w = guest::ptr<u32>(data);
        memset(w, 0, 0x40);
        const u32 links = TheWorld(self)->mapState + (u32)*guest::ptr<s32>(0x2EE518u) * 0x10;
        w[0] = guest::addr(MapDb_GetRoutePoints(MapDb_Get(), (*gPlayerData)[0x103]));
        w[1] = links + 0x20;
        w[2] = links + 0x60;
        memcpy(&w[4], run->places[(size_t)run->next].data(), 16);
        memcpy(&w[8], &run->reach, 4);
        w[9] = (u32)run->how;
        w[10] = run->avoid != 0;
        CharaMgr_SetState(charas, run->id, 2, 2, data);
        if (++run->next >= (s32)run->places.size())
            run->next = 0;
    };
    RegWait(o, "PathRunningExec2", 0, [](Frame& f) {
        auto run = std::make_shared<PathRun>();
        run->id = IntArg(f, 0);
        run->how = IntArg(f, 1);
        run->avoid = IntArg(f, 2);
        run->reach = FloatArg(f, 3);
        const Ref list = sol::arg(f, 4);
        if (list && list->type() == sol::Type::List)
            for (const Ref& item : static_cast<sol::List*>(list.get())->items)
                if (const Locator* at = AsLocator(item))
                    run->places.push_back({at->m[12] + sLocatorOrigin[0], at->m[13] + sLocatorOrigin[1],
                                           at->m[14] + sLocatorOrigin[2], 1.0f});
        if (run->places.size() < 2 || run->places.size() > 10 || Charas(sSelf) == nullptr ||
            TheWorld(sSelf)->mapState == 0)
            return 0;
        f.keep = run;
        walkTo(sSelf, run.get());
        pathStates[run->id] = IntArg(f, 5, 1);
        return 1;
    }, [](Frame& f) {
        PathRun* run = static_cast<PathRun*>(f.keep.get());
        void* charas = Charas(sSelf);
        const auto state = pathStates.find(run->id);
        if (charas == nullptr || state == pathStates.end())
            return (int)sol::kEnd;
        if (state->second == 2) {
            state->second = 1;
            CharaMgr_SetState(charas, run->id, 2, 0, 0);
            return (int)sol::kEnd;
        }
        if (state->second == 1) {
            CharaMgr_Release(charas, run->id);
            if (CharaMgr_GetAction(charas, run->id) != 0)
                walkTo(sSelf, run);
        } else if (state->second == 0) {
            CharaMgr_SetIdle(charas, run->id);
        }
        return (int)sol::kYield;
    });
    Reg(o, "SetPathRunState2", 0, [](Player*, Frame& f) {
        const auto state = pathStates.find(IntArg(f, 0));
        const s32 to = IntArg(f, 1);
        if (state != pathStates.end() && to >= 0 && to <= 2)
            state->second = to;
        return 1;
    });
    Reg(o, "PathAllInit2", 0, [](Player*, Frame&) { pathStates.clear(); return 1; });
    // PathAdd(id, [L, L …]): a path of the script's own for a character — up
    // to ten places and the ways between them; PathRunningExec(id, run, avoid)
    // sets the character wandering along it (AI move 4); PathAllInit() forgets
    // them all.  0x1FAEF0, 0x1FAAF0, 0x1FBFE0 (the map at 0x30A9C0).
    struct ScriptPath { PointSet set; GuestIntMap routes; u32 _1c; float points[10][4]; };
    static std::map<s32, u32> scriptPaths;
    Reg(o, "PathAdd", 0, [](Player* self, Frame& f) {
        const Ref list = sol::arg(f, 1);
        std::vector<const Locator*> places;
        if (list && list->type() == sol::Type::List)
            for (const Ref& item : static_cast<sol::List*>(list.get())->items)
                places.push_back(AsLocator(item));
        if (places.empty() || places.size() > 10 || TheWorld(self)->mapState == 0)
            return 0;
        const u32 collision = MapState_GetMap(guest::ptr(TheWorld(self)->mapState));
        const s32 id = IntArg(f, 0);
        if (collision == 0 || scriptPaths.count(id) != 0)
            return 0;
        ScriptPath* path = static_cast<ScriptPath*>(MemAlloc(sizeof(ScriptPath)));
        memset(path, 0, sizeof *path);
        path->set.count = (s32)places.size();
        path->set.points = guest::addr(path->points);
        for (size_t i = 0; i < places.size(); i++)
            if (places[i] != nullptr)
                memcpy(path->points[i], &places[i]->m[12], 16);
        GuestIntMap_Init(&path->routes);
        Routes_BuildFor(&path->routes, &path->set, guest::ptr(collision));
        scriptPaths[id] = guest::addr(path);
        return 1;
    });
    Reg(o, "PathRunningExec", 0, [](Player* self, Frame& f) {
        const auto found = scriptPaths.find(IntArg(f, 0));
        void* charas = Charas(self);
        if (found == scriptPaths.end() || charas == nullptr)
            return 0;
        static u32 data = 0;
        if (data == 0)
            data = guest::addr(guest::alloc(0x20));
        ScriptPath* path = guest::ptr<ScriptPath>(found->second);
        u32* w = guest::ptr<u32>(data);
        w[0] = guest::addr(&path->set);
        w[1] = 0;
        w[2] = guest::addr(&path->routes);
        w[3] = (u32)path->set.count;
        w[4] = path->set.points;
        w[5] = IntArg(f, 1) != 0;
        w[6] = IntArg(f, 2) != 0;
        return CharaMgr_SetState(charas, found->first, 2, 4, data) != 0 ? 1 : 0;
    });
    Reg(o, "PathAllInit", 0, [](Player*, Frame&) {
        for (const auto& entry : scriptPaths) {
            ScriptPath* path = guest::ptr<ScriptPath>(entry.second);
            for (GuestMapNode* n = GuestIntMap_Begin(&path->routes); n != GuestIntMap_End(&path->routes);
                 n = GuestIntMap_Next(n))
                GuestIntMap_Clear(reinterpret_cast<GuestIntMap*>(&n->value));
            GuestIntMap_Clear(&path->routes);
            MemFree(path);
        }
        scriptPaths.clear();
        return 1;
    });
    // CancelMotion(id): drops whatever motion a script gave it (0x1F3460).
    Reg(o, "CancelMotion", 0, [](Player* self, Frame& f) {
        if (void* charas = Charas(self))
            CharaMgr_fn143c60(charas, IntArg(f, 0));
        return 1;
    });
    // Get / Set / Add of Money (the player's: PlayerData+0x106, with running
    // totals of what was earned +0x84C and spent +0x84E), Hp and MaxHp
    // (0x1FA670 / 0x1FA210).  Set and Add take (value [, id]); all are kept
    // within 0 … 9999.
    static const auto numberOf = [](Player* self, s32 id, int what) -> s16* {
        if (what == 0)
            return reinterpret_cast<s16*>(*gPlayerData + 0x106);
        void* charas = Charas(self);
        CharaParams* p = charas ? reinterpret_cast<CharaParams*>(CharaMgr_GetParams(charas, id)) : nullptr;
        if (p == nullptr)
            return nullptr;
        return what == 3 ? &p->maxHp : &p->hp;
    };
    static const auto get = [](Player* self, Frame& f, int what) {
        s32 id = IntArg(f, 0);
        if (what == 1) {
            // (Of a character with two bodies, the one that has been hurt.)
            const s16* own = numberOf(self, id, 1);
            const s16* max = numberOf(self, id, 3);
            if (own != nullptr && *own == *max && numberOf(self, ArmedId(id), 1) != nullptr)
                id = ArmedId(id);
        }
        const s16* value = numberOf(self, id, what);
        if (value == nullptr)
            return 0;
        Answer(f, *value);
        return 1;
    };
    static const auto set = [](Player* self, Frame& f, int what, bool add) {
        s32 value = IntArg(f, 0);
        s16* slot = numberOf(self, IntArg(f, 1), what);
        if (slot == nullptr)
            return 0;
        if (add)
            value += *slot;
        if (value < 0) value = 0;
        if (value > 9999) value = 9999;
        if (what == 1) {
            const s16* max = numberOf(self, IntArg(f, 1), 3);
            *slot = (s16)(value < *max ? value : *max);
        } else if (what == 0) {
            u8* pd = *gPlayerData;
            const s32 change = value - *slot;
            *slot = (s16)value;
            u16* earned = reinterpret_cast<u16*>(pd + 0x84C);
            u16* spent = reinterpret_cast<u16*>(pd + 0x84E);
            if (change < 1) {
                const s32 total = *spent + (-change);
                *spent = (u16)(total > 0xFFFF ? 0xFFFF : total);
            } else {
                const s32 total = *earned + change;
                *earned = (u16)(total > 0xFFFF ? 0xFFFF : total);
            }
        } else {
            *slot = (s16)value;
        }
        return 1;
    };
    Reg(o, "GetMoney", 0, [](Player* self, Frame& f) { return get(self, f, 0); });
    Reg(o, "SetMoney", 0, [](Player* self, Frame& f) { return set(self, f, 0, false); });
    Reg(o, "AddMoney", 0, [](Player* self, Frame& f) { return set(self, f, 0, true); });
    Reg(o, "SetMaxHp", 0, [](Player* self, Frame& f) { return set(self, f, 3, false); });      // 0x1F7140
    // DeadCheck(id) → body flag 0x10 (0x1F6E40).
    Reg(o, "DeadCheck", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        Answer(f, (CharaMgr_GetFlags(charas, IntArg(f, 0)) & 0x10) != 0);
        return 1;
    });
    // GetLastAttackedInfo(id) → who hit it last, −1 nobody (0x1F3630).
    Reg(o, "GetLastAttackedInfo", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        s32 id = IntArg(f, 0);
        if (id < 1000 && id != 100 && id != 0)
            id += 1000;
        const u32 by = (u32)CharaMgr_Get1DC(charas, id);
        s32 who = -1;
        if (by != 0) {
            who = *guest::ptr<s32>(by);
            if (who > 1000)
                who -= 1000;
        }
        Answer(f, who);
        return 1;
    });
    // EraseWeapon(id): its sword model is done with (0x1F27D0 → vtable +0x28).
    Reg(o, "EraseWeapon", 0, [](Player* self, Frame& f) {
        if (void* charas = Charas(self))
            CharaMgr_fn1444d0(charas, IntArg(f, 0));
        return 1;
    });
    // ChildRelease(id): lets go of whoever it holds (0x1F2AE0: params +0x10).
    Reg(o, "ChildRelease", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas != nullptr)
            if (u8* params = reinterpret_cast<u8*>(const_cast<CharaParams*>(ParamsOf(charas, IntArg(f, 0)))))
                *reinterpret_cast<u32*>(params + 0x10) = 0;
        return 1;
    });
    Reg(o, "AddGoodValue", 0, [](Player*, Frame&) { return 1; });  // 0x1F7100: changes nothing in this release
    Reg(o, "GetHp", 0, [](Player* self, Frame& f) { return get(self, f, 1); });
    Reg(o, "SetHp", 0, [](Player* self, Frame& f) { return set(self, f, 1, false); });
    Reg(o, "AddHp", 0, [](Player* self, Frame& f) { return set(self, f, 1, true); });
    Reg(o, "SetVisible", 2, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        CharaMgr_SetActive(charas, ActiveId(self, IntArg(f, 0)), IntArg(f, 1));
        return 1;
    });
}

// ═══ Camera (0x1EDBF0) ════════════════════════════════════════════════════
// Scripts place the camera through the camera list (src/game/camera.cpp):
// setting the eye or the target drops the controller, so the camera stays
// where the script put it until `SetNormal` gives it back to a follow camera.
inline void* CamListOf(Player* self) {                  // 0x1E2AB0
    return guest::ptr<u8>(TheWorld(self)->stage) + 0x60;
}
// A place given to a camera call, in the world (× the scripts' origin, 0x218480).
void CameraPoint(Frame& f, float* out) {
    for (int i = 0; i < 3; i++)
        out[i] = FloatArg(f, (size_t)i) + sLocatorOrigin[i];
    out[3] = 1.0f;
}
u32* EaseSlot() { return guest::ptr<u32>(*guest::ptr<u32>(0x2EE528u) + 0x60); }

void BuildCamera(const Ref& o) {
    // SetViewPos(x, y, z [, ease]): where the camera is; without `ease` it
    // is there at once (0x1F0310).
    Reg(o, "SetViewPos", 3, [](Player* self, Frame& f) {
        alignas(16) float v[4];
        CameraPoint(f, v);
        CamList_SetEye(CamListOf(self), v);
        if (IntArg(f, 3) == 0)
            CamList_Apply(CamListOf(self));
        return 1;
    });
    // SetPos(x, y, z, dx, dy, dz [, ease]): the camera there, looking along
    // (dx, dy, dz) — 0x1EF8E0.
    Reg(o, "SetPos", 6, [](Player* self, Frame& f) {
        alignas(16) float eye[4], target[4];
        CameraPoint(f, eye);
        for (int i = 0; i < 3; i++)
            target[i] = eye[i] + FloatArg(f, (size_t)i + 3);
        target[3] = 2.0f;                               // (1 + 1, as the original's vector add leaves it)
        CamList_SetEye(CamListOf(self), eye);
        CamList_SetTarget(CamListOf(self), target);
        if (IntArg(f, 6) == 0)
            CamList_Apply(CamListOf(self));
        return 1;
    });
    // SetSoloShot(id, L(eye), L(target) [, ease]): a shot of one character:
    // both places are given in the character's own space (in front of it is
    // SetRotateZ(degrees [, ease]): rolls the camera — its up vector turned
    // about z (0x1EF1F0; sceVu0RotMatrixZ 0x1074C8).
    Reg(o, "SetRotateZ", 0, [](Player* self, Frame& f) {
        const float a = (float)IntArg(f, 0) * 3.1415927f / 180.0f;
        alignas(16) const float up[4] = {-std::sin(a), std::cos(a), 0.0f, 0.0f};
        CamList_SetUp(CamListOf(self), up);
        if (IntArg(f, 1) == 0)
            CamList_Apply(CamListOf(self));
        return 1;
    });
    // +z), wherever it stands and whichever way it faces.  0x1EEAD0
    Reg(o, "SetSoloShot", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        const CharaParams* p = charas ? ParamsOf(charas, IntArg(f, 0)) : nullptr;
        const Locator* eye = AsLocator(sol::arg(f, 1));
        const Locator* target = AsLocator(sol::arg(f, 2));
        if (p == nullptr)
            return 1;
        static const float kOrigin[4] = {0, 0, 0, 1};
        const auto place = [p](const float* v, float* out) {       // v × the character's matrix
            const float* m = p->matrix;
            for (int k = 0; k < 4; k++)
                out[k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k] + v[3] * m[12 + k];
        };
        alignas(16) float at[4];
        place(eye ? &eye->m[12] : kOrigin, at);
        CamList_SetEye(CamListOf(self), at);
        place(target ? &target->m[12] : kOrigin, at);
        CamList_SetTarget(CamListOf(self), at);
        if (IntArg(f, 3) == 0)
            CamList_Apply(CamListOf(self));
        return 1;
    });
    // SetWatchPos(x, y, z [, ease]): what it looks at (0x1EFF40).
    Reg(o, "SetWatchPos", 3, [](Player* self, Frame& f) {
        alignas(16) float v[4];
        CameraPoint(f, v);
        CamList_SetTarget(CamListOf(self), v);
        if (IntArg(f, 3) == 0)
            CamList_Apply(CamListOf(self));
        return 1;
    });
    // SetFOV(factor): zoom; 1 = normal (0x1EDF90 → *0x2EDD30, which the map
    // draw multiplies the projection by).
    Reg(o, "SetFOV", 0, [](Player*, Frame& f) {
        *guest::ptr<float>(0x2EDD30u) = FloatArg(f, 0, 1.0f);
        return 1;
    });
    // SetHokan(kind, amount): how the next camera moves are eased — 0 the
    // usual way, 1 a fixed share a frame, 2 in so many seconds unsteadily,
    // 3 in exactly so many seconds (0x1EE680).
    Reg(o, "SetHokan", 0, [](Player*, Frame& f) {
        if (*guest::ptr<u32>(0x2EE528u) == 0)
            return 1;
        void* ease = nullptr;
        switch (IntArg(f, 0)) {
        case 3: if (void* m = MemAlloc(8)) ease = EaseFrames_Ctor(m, (int)(FloatArg(f, 1, 1.0f) * 30.0f)); break;
        case 2: if (void* m = MemAlloc(0xC)) ease = EaseShaky_Ctor(m, (int)(FloatArg(f, 1, 1.0f) * 30.0f)); break;
        case 1: if (void* m = MemAlloc(8)) ease = EaseFixed_Ctor(FloatArg(f, 1, 0.1f), m); break;
        case 0: break;
        default: return 1;
        }
        EaseHolder_SetNext(EaseSlot(), ease);
        EaseHolder_Begin(EaseSlot());
        return 1;
    });
    // SetNormal([id]): back to following a character (0x1EE4C0).
    Reg(o, "SetNormal", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        void* mem = MemAlloc(0xC0);
        void* cam = mem && charas
                        ? FollowCam_Ctor(mem, CharaMgr_GetParams(charas, IntArg(f, 0)), guest::ptr(0x2E0020u))
                        : nullptr;
        CamList_Push(guest::ptr(*guest::ptr<u32>(0x2EE528u)), cam);
        return 1;
    });
}

// ═══ Sound (0x20E280) ═════════════════════════════════════════════════════
// Volumes are per cent (100 → 0x3FFF).  A locator makes a sound come from a
// place: its position goes to the engine as a guest vector.
u32 SoundPlace(Frame& f, size_t i) {
    Locator* l = AsLocator(sol::arg(f, i));
    if (l == nullptr)
        return 0;
    static u32 place = 0;
    if (place == 0)
        place = guest::addr(guest::alloc(0x10));
    float* v = guest::ptr<float>(place);
    for (int k = 0; k < 3; k++)
        v[k] = l->m[12 + k] + sLocatorOrigin[k];
    v[3] = 1.0f;
    return place;
}
void BuildSound(const Ref& o) {
    Reg(o, "PlayBGM", 0, [](Player*, Frame& f) {                // 0x20F150 (id, volume, loop, fade in)
        Bgm_Play(IntArg(f, 0), IntArg(f, 2, 1), IntArg(f, 3), IntArg(f, 1, 100) * 0x3FFF / 100);
        return 1;
    });
    // PlayVoiceSE(id, cry): one of a character's cries (0x20E580 → 0x180F70).
    Reg(o, "PlayVoiceSE", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas != nullptr)
            if (const void* params = ParamsOf(charas, IntArg(f, 0)))
                Sound_Cry(params, IntArg(f, 1));
        return 1;
    });
    // VolumeBGM(id, per cent): of a tune that is playing (0x20E980 → 0x1804F0).
    Reg(o, "VolumeBGM", 0, [](Player*, Frame& f) {
        const s32 id = IntArg(f, 0), volume = IntArg(f, 1) * 0x3FFF / 100;
        const s32 count = *guest::ptr<s32>(0x309F84u);
        const s32* playing = guest::ptr<s32>(*guest::ptr<u32>(0x309F88u));     // {id, voice, layer}
        for (s32 i = 0; i < count; i++) {
            if (playing[i * 3] != id || playing[i * 3 + 2] != 0)
                continue;
            s32 v = volume * *guest::ptr<s32>(0x2EDAD8u) / 10;
            if (v > 0x3FFF)
                v = 0x3FFF;
            Stream_SetVolume(guest::ptr<StreamDriver>(*guest::ptr<u32>(0x2EE5DCu)), playing[i * 3 + 1], (u32)v, (u32)v);
            break;
        }
        return 1;
    });
    Reg(o, "StopBGM", 0, [](Player*, Frame& f) {                // 0x20EBA0
        Bgm_StopFade(IntArg(f, 0), IntArg(f, 1));
        return 1;
    });
    Reg(o, "PlaySE", 0, [](Player*, Frame& f) {                 // 0x20F640 (id, volume [, locator])
        const u32 place = sol::argCount(f) < 3 ? 0 : SoundPlace(f, 2);
        Sound_PlaySE(IntArg(f, 0), (int)place, IntArg(f, 1, 100) * 0x3FFF / 100);
        return 1;
    });
    Reg(o, "PlayStreamSE", 0, [](Player*, Frame& f) {           // 0x20ED90 (id, volume [, locator], flag)
        Voice_Play(IntArg(f, 0), (int)SoundPlace(f, 2), IntArg(f, 1, 100) * 0x3FFF / 100, IntArg(f, 3));
        return 1;
    });
    Reg(o, "StopStreamSE", 0, [](Player*, Frame& f) {           // 0x20E830
        Bgm_StopVoiceLayer(IntArg(f, 0));
        return 1;
    });
    Reg(o, "VolumeAllSE", 0, [](Player*, Frame& f) {            // 0x20F4B0: per cent of the setting
        const u32 config = *guest::ptr<u32>(0x2EE68Cu);
        const s32 setting = config ? *guest::ptr<s32>(config) : 0;
        *guest::ptr<s32>(0x2EDAD0u) = setting * IntArg(f, 0, 100) / 100;
        return 1;
    });
}

// ═══ UI (0x208A50) ════════════════════════════════════════════════════════
// Speech and choices go through the world's conversation manager.
inline void* TalkOf(Player* self) { return guest::ptr(TheWorld(self)->effects); }      // 0x1E2A90
// Text handed to the engine has to be in guest memory; one buffer is enough,
// the conversation manager copies what it is given.
const u16* GuestText(const std::u16string& text) {
    static u32 buffer = 0;
    if (buffer == 0)
        buffer = guest::addr(guest::alloc(0x1000));
    const size_t n = text.size() < 0x7FF ? text.size() : 0x7FF;
    memcpy(guest::ptr(buffer), text.data(), n * 2);
    guest::ptr<u16>(buffer)[n] = 0;
    return guest::ptr<const u16>(buffer);
}
bool sAdvance = false;                                  // (VM+0x30) the player has pressed on

// 0x1E2C80: has the player pressed a button to go on?  (Any face button in
// the free world, Circle or Cross in a story scene.)
bool AdvancePressed(Player* self) {
    if (sAdvance)
        return true;
    const u16 pressed = Pad_GetPressed(PadMgr_Get(), 0);
    const bool field = self->scene != 0 && guest::ptr<s32>(self->scene)[2] == 1;
    if ((pressed & (field ? 0xF0 : 0x60)) == 0)
        return false;
    Voice_Stop();                                       // the line being spoken is cut short
    Sound_PlaySE(7, 0, 0x3FFF);
    sAdvance = true;
    return true;
}

void BuildUI(const Ref& o) {
    // SetSerif(text, speaker, listener, frames, style) — 0x20C360.
    Reg(o, "SetSerif", 3, [](Player* self, Frame& f) {
        const std::u16string* text = sol::argString(f, 0);
        void* charas = Charas(self);
        if (charas == nullptr || text == nullptr)
            return 1;
        u8* speaker = CharaMgr_GetParams(charas, IntArg(f, 1));
        u8* listener = CharaMgr_GetParams(charas, IntArg(f, 2));
        if (speaker == nullptr || listener == nullptr)
            return 1;
        Talk_ScriptSay(TalkOf(self), speaker, listener, GuestText(*text), IntArg(f, 4), IntArg(f, 3, -1));
        return 1;
    });
    // SetSerif2(text, speaker, listener, frames, style, x, y): the same in a
    // balloon at a place on the screen (pixels from the top left).  0x20BB50
    Reg(o, "SetSerif2", 3, [](Player* self, Frame& f) {
        const std::u16string* text = sol::argString(f, 0);
        void* charas = Charas(self);
        if (charas == nullptr || text == nullptr)
            return 1;
        u8* speaker = CharaMgr_GetParams(charas, IntArg(f, 1));
        u8* listener = CharaMgr_GetParams(charas, IntArg(f, 2));
        if (speaker == nullptr || listener == nullptr)
            return 1;
        const u32 desc[8] = {1, guest::addr(speaker), guest::addr(listener), guest::addr(GuestText(*text)),
                             (u32)IntArg(f, 4), (u32)IntArg(f, 3, -1), (u32)IntArg(f, 5), (u32)IntArg(f, 6)};
        Talk_ScriptSayAt(TalkOf(self), desc);
        return 1;
    });
    // AddMessage(text, seconds (< 0: until cleared), type): a line of the HUD.  0x2099A0
    Reg(o, "AddMessage", 1, [](Player*, Frame& f) {
        const std::u16string* text = sol::argString(f, 0);
        if (text == nullptr)
            return 1;
        const float seconds = FloatArg(f, 1);
        Hud_ShowDate(*gHud, GuestText(*text), seconds < 0.0f ? -1 : (int)(seconds * (float)*gFrameRate), IntArg(f, 2));
        return 1;
    });
    // Shop(kind, buyer, seller): the shop menu; lasts until the player is
    // done, and answers 1 if they could not or would not pay for what they
    // took (0x20D160 / 0x20D510; shop_menu.cpp).
    RegWait(o, "Shop", 0, [](Frame& f) {
        void* charas = Charas(sSelf);
        if (charas == nullptr || *gHud == nullptr)
            return 0;
        const u32 params[3] = {(u32)IntArg(f, 0), guest::addr(CharaMgr_GetParams(charas, IntArg(f, 1))),
                               guest::addr(CharaMgr_GetParams(charas, IntArg(f, 2)))};
        Hud_OpenShop(*gHud, params);
        return 1;
    }, [](Frame& f) {
        if (Hud_ShopOpen(*gHud) != 0)
            return (int)sol::kYield;
        const int result = Hud_ShopResult(*gHud);
        Answer(f, result == 10 || result == 9);
        return (int)sol::kEnd;
    });
    Reg(o, "DeleteMessage", 0, [](Player*, Frame&) { Hud_ClearChoice(*gHud); return 1; });     // 0x209980
    // ShowKanban(which) / HideKanban(): the shop's signboard.  0x20A030 / 0x209960
    Reg(o, "ShowKanban", 0, [](Player*, Frame& f) { Hud_ShowKanban(*gHud, IntArg(f, 0)); return 1; });
    // FriendLife(id) / HideFriendLife(): the health plate of someone on the
    // player's side (0x209720 / 0x209610).
    Reg(o, "FriendLife", 0, [](Player* self, Frame& f) {
        Hud_SetFriend(*gHud, nullptr);
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        const s32 id = IntArg(f, 0);
        if (ParamsOf(charas, ArmedId(id)) != nullptr || ParamsOf(charas, id) != nullptr)
            Hud_SetFriend(*gHud, const_cast<CharaParams*>(ParamsOf(charas, ActiveId(self, id))));
        return 1;
    });
    Reg(o, "HideFriendLife", 0, [](Player*, Frame&) { Hud_SetFriend(*gHud, nullptr); return 1; });
    // PadWave(small, big, frames a step, frames in all): shakes the pad — the
    // small motor on or off, the big one at a strength (0x2092A0; it does not
    // look at the Vibrate setting itself: the pad manager's first word does).
    Reg(o, "PadWave", 0, [](Player*, Frame& f) {
        u8 pattern[0x24] = {};
        pattern[0] = (u8)IntArg(f, 0, 1);
        pattern[0x10] = (u8)IntArg(f, 1, 128);
        const s16 step = (s16)IntArg(f, 2, 5), total = (s16)IntArg(f, 3, 5);
        memcpy(pattern + 0x20, &step, 2);
        memcpy(pattern + 0x22, &total, 2);
        Pad_SetVibration(PadMgr_Get(), 0, pattern);
        return 1;
    });
    // SetWeaponMenu() → which carried sword was chosen (1–3), −1 none: the
    // status menu's swords page, opened by the scene to hand a sword over
    // (World+8 = 3; the answer comes back in World+0x58).  0x20CC40 / 0x20CC60
    RegWait(o, "SetWeaponMenu", 0, [](Frame&) {
        World* world = TheWorld(sSelf);
        world->kind = 3;
        world->handOver = 0;
        return 1;
    }, [](Frame& f) {
        const World* world = TheWorld(sSelf);
        if (world->handOver == 0)
            return (int)sol::kYield;
        Answer(f, world->handOver);
        return (int)sol::kEnd;
    });
    // SetMyHomeMenu(): the menu of the player's room — rest, save, the
    // storehouse.  It asks the scene (World+8 = 4: 0x2AF340 opens the pause
    // menu's question list under the date) and waits for the answer in
    // World+0x64.  0x20CA20 / 0x20CA40
    RegWait(o, "SetMyHomeMenu", 0, [](Frame&) {
        World* world = TheWorld(sSelf);
        world->kind = 4;
        world->dialogChoice = 0;
        return 1;
    }, [](Frame& f) {
        const World* world = TheWorld(sSelf);
        if (world->dialogChoice == 0)
            return (int)sol::kYield;
        Answer(f, world->dialogChoice);
        return (int)sol::kEnd;
    });
    Reg(o, "HideKanban", 0, [](Player*, Frame&) { Hud_HideKanban(*gHud); return 1; });
    // Syojikin(show [, x, y]): the player's money.  0x209D80
    Reg(o, "Syojikin", 0, [](Player*, Frame& f) {
        if (IntArg(f, 0) == 0)
            Hud_HideMoney(*gHud);
        else
            Hud_ShowMoney(*gHud, IntArg(f, 1), IntArg(f, 2));
        return 1;
    });
    Reg(o, "DeleteSerif", 0, [](Player* self, Frame&) {         // 0x20AD40
        Effects_EndAll(TalkOf(self), -1);
        return 1;
    });
    // SetTalkSelect(lines, text, speaker, listener, frames, style [, x, y]) — 0x20A440.
    Reg(o, "SetTalkSelect", 4, [](Player* self, Frame& f) {
        const std::u16string* text = sol::argString(f, 1);
        void* charas = Charas(self);
        if (charas == nullptr || text == nullptr)
            return 1;
        u8* speaker = CharaMgr_GetParams(charas, IntArg(f, 2));
        u8* listener = CharaMgr_GetParams(charas, IntArg(f, 3));
        if (speaker == nullptr || listener == nullptr)
            return 1;
        struct { u32 speaker, listener; s32 frames, style, direction; u32 text, flags; s32 x, y; } desc{};
        desc.speaker = guest::addr(speaker);
        desc.listener = guest::addr(listener);
        desc.frames = IntArg(f, 4, -1);
        desc.style = IntArg(f, 5);
        desc.direction = -1;
        desc.text = guest::addr(GuestText(*text));
        const s32 x = IntArg(f, 6, -1), y = IntArg(f, 7, -1);
        if (x != -1 && y != -1) {
            desc.flags = 1;
            desc.x = x;
            desc.y = y;
        }
        Talk_ScriptSelect(TalkOf(self), IntArg(f, 0), &desc);
        return 1;
    });
    Reg(o, "GetTalkSelectID", 0, [](Player* self, Frame& f) {   // 0x20A300: from 0
        Answer(f, guest::ptr<s32>(TheWorld(self)->effects)[5] - 1);
        return 1;
    });
    Reg(o, "IsTalkSelected", 0, [](Player* self, Frame& f) {    // 0x20A1B0
        Answer(f, Talk_Exists(TalkOf(self), guest::ptr<u32>(TheWorld(self)->effects)[9]) == 0);
        return 1;
    });
    Reg(o, "DeleteSelect", 0, [](Player* self, Frame&) {        // 0x20A180
        Talk_CloseChoice(TalkOf(self));
        return 1;
    });
    // Wait(seconds): until the player presses on, or so long (−1: for ever)
    // — 0x20CE60 / 0x20D040.
    RegWait(o, "Wait", 0, [](Frame& f) {
        const float seconds = FloatArg(f, 0, 60.0f * (float)*gFrameRate);
        f.value = seconds != -1.0f;
        f.phase = (int)(seconds * (float)*gFrameRate);
        sSelf->budget = 10;
        sAdvance = false;
        return 1;
    }, [](Frame& f) {
        if (AdvancePressed(sSelf))
            return (int)sol::kEnd;
        if (f.value == 0 || f.phase-- > 0)
            return (int)sol::kYield;
        return (int)sol::kEnd;
    });
}

// ═══ Effect (0x201F30) ════════════════════════════════════════════════════
const GuestPtrVar<void> gRenderMgr{0x2EE684u};

// 0x1E7540: breaks a line of text so that it fits `width` (1/16 px) at `scale`.
std::u16string WrapText(const std::u16string& text, float scale, int width) {
    Font* font = guest::ptr<Font>(*guest::ptr<u32>(0x2EE67Cu));
    std::u16string out, line;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find_first_of(u" \n", at);
        if (end == std::u16string::npos)
            end = text.size();
        const std::u16string word = text.substr(at, end - at);
        const std::u16string tried = line.empty() ? word : line + u" " + word;
        Font_Measure(font, GuestText(tried), scale);
        if (!line.empty() && (int)(u16)Font_GetExtent(font)->w > width) {
            out += line + u"\n";
            line = word;
        } else {
            line = tried;
        }
        if (end < text.size() && text[end] == u'\n') {
            out += line + u"\n";
            line.clear();
        }
        at = end + 1;
    }
    return out + line;
}

// Text over the picture, for this frame (render effect 13).
void ShowOverText(const std::u16string& text, u16 x, u16 y, u32 rgba, bool centred = false) {
    struct Desc { u16 x, y; u32 z, rgba; u16 centred, _e; u32 rep, refs; };
    static u32 block = 0;
    if (block == 0)
        block = guest::addr(guest::alloc(0x20 + 0x10 + 0x1000));
    Desc* d = guest::ptr<Desc>(block);
    u32* rep = guest::ptr<u32>(block + 0x20);
    u16* chars = guest::ptr<u16>(block + 0x30);
    const size_t n = text.size() < 0x7FF ? text.size() : 0x7FF;
    memcpy(chars, text.data(), n * 2);
    chars[n] = 0;
    rep[0] = block + 0x30;
    *d = {x, y, 0xFFFFF0u, rgba, (u16)centred, 0, block + 0x20, 0};
    RenderMgr_TriggerEffect(*gRenderMgr, 0xD, (int)block, 0);
}

void BuildEffect(const Ref& o) {
    // TelopVoice([voice, seconds, text, lip movements], speaker [, volume]) —
    // 0x207470 / 0x207DB0: a spoken line: the subtitle centred near the
    // bottom for seconds + 1, the voice clip and the speaker's mouth.  It ends
    // early when the player has pressed on (UI.Wait sees to that).
    RegWait(o, "TelopVoice", 1, [](Frame& f) {
        const sol::List* line = sol::arg(f, 0) && sol::arg(f, 0)->type() == sol::Type::List
                                    ? static_cast<const sol::List*>(sol::arg(f, 0).get()) : nullptr;
        f.phase = 0;
        f.keep = nullptr;
        if (line != nullptr && line->items.size() >= 4) {
            const Ref seconds = line->items[1], text = line->items[2];
            const float s = seconds && seconds->type() == sol::Type::Float
                                ? static_cast<sol::Float*>(seconds.get())->value : 0.0f;
            f.phase = (int)((s + 1.0f) * (float)*gFrameRate);
            // (The original hands its text to render effect 13 once, here, so
            // a subtitle started after it — the macro Telop, on a thread of
            // its own — takes the effect over.  This port shows text frame by
            // frame: an empty line, as all of this release's are, must not
            // keep wiping that subtitle out.)
            if (text && text->type() == sol::Type::String && !static_cast<sol::String*>(text.get())->value.empty())
                f.keep = text;
            // The clip itself, heard from where the speaker stands.
            const Ref voice = line->items[0];
            const s32 id = voice && voice->type() == sol::Type::Int ? static_cast<sol::Int*>(voice.get())->value : -1;
            const s32 speaker = IntArg(f, 1, -1);
            void* charas = Charas(sSelf);
            const u8* params = charas != nullptr && speaker != -1 ? CharaMgr_GetParams(charas, speaker) : nullptr;
            // The speaker's mouth follows the line's lip data.
            const Ref lips = line->items[3];
            if (speaker != -1 && charas != nullptr && lips && lips->type() == sol::Type::String &&
                CharaMgr_IsActive(charas, speaker)) {
                const std::u16string& text = static_cast<sol::String*>(lips.get())->value;
                u16* copy = static_cast<u16*>(guest::alloc((u32)(text.size() + 1) * 2));
                memcpy(copy, text.c_str(), (text.size() + 1) * 2);
                CharaMgr_fn143550(charas, speaker, guest::addr(copy));  // (it keeps a copy of its own)
                guest::free(copy);
            }
            if (id >= 0)
                Voice_Start(id + 1, params != nullptr ? (int)guest::addr(params + 0x80) : 0,
                            IntArg(f, 2, 100) * 0x3FFF / 100);
        }
        sAdvance = false;
        return 1;
    }, [](Frame& f) {
        if (f.phase == 0)
            return (int)sol::kEnd;
        if (sAdvance) {
            f.phase = 0;
            return (int)sol::kEnd;
        }
        f.phase--;
        const u32 config = *guest::ptr<u32>(0x2EE68Cu);
        if (config != 0 && guest::ptr<s32>(config)[8] != 0 && f.keep) {
            // Centred: the effect's `centred` flag (drawn here through the same call).
            ShowOverText(static_cast<sol::String*>(f.keep.get())->value, (u16)Screen_X(0, 0),
                         (u16)Screen_Y(-0x360, 1), 0xFFF0F0F0u, true);
        }
        return (int)sol::kYield;
    });

    // Telop(text, seconds [, text, seconds …]): subtitles, one after another;
    // pressing on skips the rest (0x206FC0 / 0x2070A0).  Scripts run it on a
    // thread of its own.  Shown only with the Subtitles setting on.
    RegWait(o, "Telop", 2, [](Frame& f) {
        f.phase = 0;            // frames left of the line showing
        f.value = 0;            // next argument
        f.keep = nullptr;
        return 1;
    }, [](Frame& f) {
        if (sAdvance) {
            f.phase = 0;
            return (int)sol::kEnd;
        }
        const u32 config = *guest::ptr<u32>(0x2EE68Cu);
        const bool subtitles = config != 0 && guest::ptr<s32>(config)[8] != 0;
        if (f.phase == 0) {
            if (f.value + 2 > (int)sol::argCount(f))
                return (int)sol::kEnd;
            const std::u16string* text = sol::argString(f, (size_t)f.value);
            f.phase = (int)(FloatArg(f, (size_t)f.value + 1) * (float)*gFrameRate);
            f.keep = sVm->newString(WrapText(text ? *text : std::u16string(), 1.2f, 0x2648));
            f.value += 2;
        } else if (!AdvancePressed(sSelf)) {
            f.phase--;
        } else {
            f.phase = 0;
            f.value = (int)sol::argCount(f);
            return (int)sol::kYield;
        }
        if (subtitles && f.phase > 0 && f.keep)
            ShowOverText(static_cast<sol::String*>(f.keep.get())->value, (u16)Screen_X(0x140, -1),
                         (u16)Screen_Y(-0x520, 1), 0x8080B0A0u);
        return (int)sol::kYield;
    });

    // Wipe(kind [, arg]): the full-screen fade — 0 in, 1 out, 2 black at once
    // (0x205530).  Fading in also takes the loading text away.
    Reg(o, "Wipe", 0, [](Player*, Frame& f) {
        const s32 kind = IntArg(f, 0), second = IntArg(f, 1);
        RenderMgr_TriggerEffect(*gRenderMgr, 0xB, kind, second);
        if (kind == 0) {
            *guest::ptr<s32>(0x2EE6C0u) = 0;        // gLoadingFrames
            *guest::ptr<s32>(0x2EE6BCu) = 0;        // gLoadingDots
            *guest::ptr<s32>(0x2EE6B8u) = 0;        // gLoadingDotTimer
            gLoadingActive = 0;
        }
        return 1;
    });
    // Cinesco(on): the letterbox bars (0x2068E0).
    // Monotone(mode): the picture without its colours (render effect 2: see hle.cpp).  0x205740
    Reg(o, "Monotone", 0, [](Player*, Frame& f) {
        RenderMgr_TriggerEffect(*guest::ptr<u32>(0x2EE684u) ? guest::ptr(*guest::ptr<u32>(0x2EE684u)) : nullptr, 2,
                                IntArg(f, 0), 0);
        return 1;
    });
    // Lights.  The lighting object (*0x2EE678) is still a native stand-in:
    // these reach the light the native renderer lights models with.
    // SetLightVector(x, y, z) (0x2036C0 → 0x165BE0: towards the first light),
    // SetLightColor(r, g, b) (0x2033F0 → 0x165B90), SetLight(x, y, z, r, g, b)
    // (0x203990: both), ReStoreLight() (0x203E70 → 0x165C10: the map's again).
    Reg(o, "SetLightVector", 0, [](Player*, Frame& f) {
        hle_light_set_dir(FloatArg(f, 0), FloatArg(f, 1), FloatArg(f, 2));
        return 1;
    });
    Reg(o, "SetLightColor", 0, [](Player*, Frame& f) {
        hle_light_set_colour(FloatArg(f, 0), FloatArg(f, 1), FloatArg(f, 2));
        return 1;
    });
    Reg(o, "SetLight", 0, [](Player*, Frame& f) {
        hle_light_set_dir(FloatArg(f, 0), FloatArg(f, 1), FloatArg(f, 2));
        hle_light_set_colour(FloatArg(f, 3), FloatArg(f, 4), FloatArg(f, 5));
        return 1;
    });
    Reg(o, "ReStoreLight", 0, [](Player*, Frame&) { hle_light_restore(); return 1; });
    // PointLightClear (0x203EA0 → Camera_Reset), PointLightEnable (0x203ED0),
    // ReleaseMapEffect (0x2033D0 → 0x14FC00).
    Reg(o, "PointLightClear", 0, [](Player*, Frame&) { Camera_Reset(nullptr); return 1; });
    Reg(o, "PointLightEnable", 0, [](Player*, Frame& f) { Lights_EnableMap(IntArg(f, 0, 1) != 0); return 1; });
    Reg(o, "ReleaseMapEffect", 0, [](Player*, Frame&) { RenderMgr_DeleteMarks(*gRenderMgr); return 1; });
    // EndRoll(file, speed [, frames]): starts a text roll (0x205E30);
    // DeleteEndRoll() ends it (0x205BC0).  See text_roll.cpp.
    Reg(o, "EndRoll", 0, [](Player*, Frame& f) {
        const std::u16string* path = sol::argString(f, 0);
        if (path != nullptr)
            TextRoll_Start(sol::narrow(*path).c_str(), FloatArg(f, 1, 1.0f), IntArg(f, 2, -1));
        return 1;
    });
    // Fade(kind [, frames = 15]): render effect 12, black coming down from
    // the top — 0 clears, 1 covers, 2 none, 3 covered (0x205300, modes 0x2D9680).
    Reg(o, "Fade", 0, [](Player*, Frame& f) {
        static const u32 modes[4] = {2, 3, 0, 1};
        RenderMgr_TriggerEffect(*gRenderMgr, 12, modes[IntArg(f, 0) & 3], (u32)IntArg(f, 1, 15));
        return 1;
    });
    // AddCharEffect(effect, id): 0x17 blood pours from the character, 0x16 a
    // pool of it under them (0x204210).  0x1B (Sayo's letter, class 0x2A41D0)
    // is not decompiled.
    Reg(o, "AddCharEffect", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        const s32 effect = IntArg(f, 0);
        const CharaParams* params = charas != nullptr ? ParamsOf(charas, IntArg(f, 1)) : nullptr;
        if (params == nullptr || params->link[1] == 0)
            return 1;
        const float* root = guest::ptr<float>(guest::ptr<u32>(params->link[1])[1]);
        if (effect == 0x16) {
            alignas(16) float at[4];
            memcpy(at, root + 12, 16);
            if (TheWorld(self)->mapState != 0)
                at[1] = Map_CellAt(guest::ptr<MapCollision>(MapState_GetMap(guest::ptr(TheWorld(self)->mapState))), at)
                            ->height + 0.03f;
            Fx_BloodPool(at, guest::ptr<float>(0x2D96B0u));
        } else if (effect == 0x17) {
            const float* v = guest::ptr<float>(0x2D96C0u);
            alignas(16) float dir[4];
            for (int j = 0; j < 4; j++)
                dir[j] = v[0] * root[j] + v[1] * root[4 + j] + v[2] * root[8 + j] + v[3] * root[12 + j];
            Fx_BloodSpray(guest::addr(params), dir);
        } else {
            static bool told = false;
            if (!told)
                printf("[event] AddCharEffect(%d): not decompiled\n", effect);
            told = true;
        }
        return 1;
    });
    // DeleteEffect(): marks and pools go (0x204F80 → 0x14FC00).
    Reg(o, "DeleteEffect", 0, [](Player*, Frame&) { RenderMgr_DeleteMarks(*gRenderMgr); return 1; });
    // AddEnvSound(sound, frames, volume) → handle: a sound of the place,
    // started again every so many frames; DelEnvSound(handle).  0x203010 / 0x202B70
    Reg(o, "AddEnvSound", 0, [](Player*, Frame& f) {
        Answer(f, RenderMgr_AddEnvSound(*gRenderMgr, IntArg(f, 0), (int)(FloatArg(f, 2, 1.0f) * 8191.0f),
                                        IntArg(f, 1)));
        return 1;
    });
    Reg(o, "DelEnvSound", 0, [](Player*, Frame& f) {
        RenderMgr_RemoveEnvSound(*gRenderMgr, IntArg(f, 0));
        return 1;
    });
    // DeleteCharEffect(effect): the first of that kind leaves (0x204050).
    Reg(o, "DeleteCharEffect", 0, [](Player*, Frame& f) {
        if (void* effect = RenderMgr_FindWorldEffect(*gRenderMgr, IntArg(f, 0), 0))
            Shadow_Release(effect);
        return 1;
    });
    Reg(o, "DeleteEndRoll", 0, [](Player*, Frame&) { TextRoll_Stop(); return 1; });
    Reg(o, "Cinesco", 0, [](Player*, Frame& f) {
        RenderMgr_TriggerEffect(*gRenderMgr, 8, IntArg(f, 0), 0);
        return 1;
    });
}


// ═══ Battle (0x24EF40): fights a script sets up ═══════════════════════════
// A script names who fights whom (AddActor), sets *rules* that end the fight
// — up to twelve, numbered from −2 — and calls Start, which hands the player
// back to the pad and waits until one of the rules holds.  GetBreakID then
// tells which.  Rule −2 is "every enemy is down" and −1 "the player is down"
// unless the script replaces them.
struct BattlePair { s32 actor, opponent; u32 flags; };
std::vector<BattlePair> sBattlePairs;                   // 0x30AA50
std::function<bool(s32 frames)> sBattleRules[12];       // 0x30AA60 (objects with a "does it hold" method)
s32 sBreakId = -10;                                     // 0x2EE2C4
s32 sBattleFrames = 0;                                  // the Start object's counter

inline s32 CharaCount(void* charas) { return *reinterpret_cast<s32*>(static_cast<u8*>(charas) + 0x30); }
inline CharaParams* LiveParams(void* charas, s32 id) {
    return reinterpret_cast<CharaParams*>(CharaMgr_GetParams(charas, id));
}
inline bool IsOn(const CharaParams* p) { return p->link[4] != 0 && *guest::ptr<s32>(p->link[4]) != 0; }
inline u32 BodyFlags(const CharaParams* p) { return p->link[5] != 0 ? *guest::ptr<u32>(p->link[5]) : 0; }

void SetBattleRule(s32 id, std::function<bool(s32)> rule, const char* what = "", s32 a = 0, s32 b = 0) {
    if (getenv("WOTS2_TRACE_EVENTS") && *what)
        printf("[event] Battle rule %d: %s %d %d\n", id, what, a, b);
    if (id + 2 >= 0 && id + 2 < 12)
        sBattleRules[id + 2] = std::move(rule);
}
// 0x252E70: `count` of those in the fight are down (−1: all of them).
bool RuleCount(s32 count) {
    void* charas = Charas(sSelf);
    if (charas == nullptr)
        return false;
    s32 down = 0;
    for (s32 i = 0, n = CharaCount(charas); i < n; i++) {
        const s32 id = CharaMgr_IdAt(charas, i);
        const CharaParams* p = LiveParams(charas, id);
        if (p == nullptr)
            return false;
        const u32 flags = CharaMgr_GetFlags(charas, id);
        if (!IsOn(p) || !(flags & 0x1000000) || (flags & 0x2000000))
            continue;
        if (count == -1) {
            if (p->hp > 0)
                return false;
        } else if (p->hp < 1) {
            down++;
        }
    }
    return count == -1 || count <= down;
}
// 0x2521A0 (and the end of 0x252770): nobody is fighting any more — everyone
// but the player (id 0, which keeps its pad routine) is back to standing.
void BattleRelease(Player* self) {
    void* charas = Charas(self);
    if (charas == nullptr)
        return;
    for (s32 i = 0, n = CharaCount(charas); i < n; i++) {
        const s32 id = CharaMgr_IdAt(charas, i);
        if (id == 0)
            continue;
        CharaMgr_SetState(charas, id, 2, 0, 0);
        CharaMgr_fn143140(charas, id);
        CharaMgr_ClearFlags(charas, id, 0xF000000);
    }
}

void BuildBattle(const Ref& o) {
    Reg(o, "Init", 0, [](Player* self, Frame&) {                // 0x2528B0
        for (auto& rule : sBattleRules)
            rule = nullptr;
        SetBattleRule(-1, [](s32) {                             // 0x252CB0
            void* charas = Charas(sSelf);
            const CharaParams* p = charas ? LiveParams(charas, 0) : nullptr;
            return p != nullptr && p->hp < 1;
        });
        SetBattleRule(-2, [](s32) { return RuleCount(-1); });
        sBreakId = -10;
        sBattlePairs.clear();
        if (void* charas = Charas(self))
            for (s32 i = 0, n = CharaCount(charas); i < n; i++) {
                const s32 id = CharaMgr_IdAt(charas, i);
                if (id > 1000)
                    if (CharaParams* p = LiveParams(charas, id))
                        p->_48 = -1;
            }
        return 1;
    });
    // Start([keepFloor]): 0x2530E0, then 0x253580 every frame.
    RegWait(o, "Start", 0, [](Frame& f) {
        Player* self = sSelf;
        const s32 keepFloor = IntArg(f, 0);
        sBattleFrames = 0;
        TheWorld(self)->kind = kWorldKind2;
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        CharaMgr_SetActive(charas, 0, 1);
        CharaMgr_Release(charas, 0);
        void* list = guest::ptr(*guest::ptr<u32>(0x2EE528u));
        CamList_Push(list, FollowCam_Ctor(MemAlloc(0xC0), LiveParams(charas, 0), guest::ptr(0x2E0020u)));
        if (CharaParams* p = LiveParams(charas, 0))
            if (keepFloor != 1)
                p->_48 = -1;
        for (const BattlePair& pair : sBattlePairs) {
            CharaMgr_SetState(charas, pair.actor, 2, 1, 0);
            CharaMgr_SetOpponent(charas, pair.actor, pair.opponent, 1);
            CharaMgr_SetFlags(charas, pair.actor, pair.flags);
        }
        sBattlePairs.clear();
        if (getenv("WOTS2_TRACE_EVENTS"))
            printf("[event] Battle.Start at frame %d\n", self->frames);
        return 1;
    }, [](Frame&) {
        sBattleFrames++;
        for (s32 i = 0; i < 12; i++) {
            if (!sBattleRules[i] || !sBattleRules[i](sBattleFrames))
                continue;
            sBreakId = i - 2;
            if (getenv("WOTS2_TRACE_EVENTS"))
                printf("[event] Battle over after %d frames: rule %d\n", sBattleFrames, sBreakId);
            // (The player cannot be finished off after the fight is over.)
            if (void* charas = Charas(sSelf))
                if (CharaParams* p = LiveParams(charas, 0))
                    p->_48 = p->hp;
            return (int)sol::kEnd;
        }
        return (int)sol::kYield;
    });
    Reg(o, "End", 0, [](Player* self, Frame&) {                 // 0x252770
        TheWorld(self)->kind = kWorldEvent;
        for (auto& rule : sBattleRules)
            rule = nullptr;
        BattleRelease(self);
        return 1;
    });
    // AddActor(who, against [, bystander]): 0x252270
    Reg(o, "AddActor", 2, [](Player* self, Frame& f) {
        const s32 who = IntArg(f, 0), against = IntArg(f, 1), bystander = IntArg(f, 2);
        void* charas = Charas(self);
        if (charas == nullptr || LiveParams(charas, who) == nullptr || LiveParams(charas, against) == nullptr)
            return 0;
        UseBody(self, who, true);
        sBattlePairs.push_back({ArmedId(who), ArmedId(against), (bystander ? 0x2000000u : 0u) | 0x1000000u});
        return 1;
    });
    Reg(o, "DelActorAll", 0, [](Player* self, Frame&) { BattleRelease(self); return 1; });
    for (const char* name : {"Exclude", "Enable", "Disable"})   // 0x252190 … 0x252170: empty in this release
        Reg(o, name, 1, [](Player*, Frame&) { return 1; });
    Reg(o, "SetAreaOut", 2, [](Player*, Frame&) { return 1; }); // 0x251E30: empty too
    // SetTime(rule, seconds): 0x251E40
    Reg(o, "SetTime", 2, [](Player*, Frame& f) {
        const s32 frames = (s32)(FloatArg(f, 1) * (float)*gFrameRate);
        SetBattleRule(IntArg(f, 0), [frames](s32 now) { return frames <= now; }, "time", frames);
        return 1;
    });
    // SetActorHP_Val(rule, who, health) / SetActorHP_Per(rule, who, per cent):
    // the fight ends when `who` is down to that — and it cannot go lower.
    // 0x2519A0 / 0x251470, rule 0x252DF0.
    static const auto healthRule = [](Player* self, s32 rule, s32 who, s32 health, bool always) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 0;
        CharaParams* p = LiveParams(charas, who);
        CharaParams* armed = LiveParams(charas, ArmedId(who));
        if (p == nullptr || (always && armed == nullptr))
            return 0;
        if (p->hp <= health)
            p->hp = (s16)(health + 1) < p->maxHp ? (s16)(health + 1) : p->maxHp;
        if (always || health > 0) {
            p->_48 = (s16)health;
            if (armed != nullptr)
                armed->_48 = (s16)health;
        }
        SetBattleRule(rule, [who, health](s32) {
            const CharaParams* now = CharaResources_GetParams(CharaResources_Get(), ActiveId(sSelf, who));
            return now != nullptr && now->hp <= health;
        }, "health", who, health);
        return 1;
    };
    Reg(o, "SetActorHP_Val", 3, [](Player* self, Frame& f) {
        return healthRule(self, IntArg(f, 0), IntArg(f, 1), IntArg(f, 2), false);
    });
    Reg(o, "SetActorHP_Per", 3, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        const CharaParams* p = charas ? LiveParams(charas, IntArg(f, 1)) : nullptr;
        if (p == nullptr)
            return 0;
        return healthRule(self, IntArg(f, 0), IntArg(f, 1), (s32)((float)(IntArg(f, 2) * p->maxHp) * 0.01f), true);
    });
    // SetActorCount(rule, how many are down): 0x251150
    Reg(o, "SetActorCount", 2, [](Player*, Frame& f) {
        const s32 count = IntArg(f, 1);
        SetBattleRule(IntArg(f, 0), [count](s32) { return RuleCount(count); }, "count", count);
        return 1;
    });
    // SetActorStrike(rule, who, by): `who` has been hit (0x250D70, rule 0x253000;
    // as the original, which compares its last number with the player's id).
    Reg(o, "SetActorStrike", 2, [](Player*, Frame& f) {
        const s32 who = IntArg(f, 1), by = IntArg(f, 2);
        SetBattleRule(IntArg(f, 0), [who, by](s32) {
            CharaResources* res = CharaResources_Get();
            const CharaParams* player = CharaResources_GetParams(res, ActiveId(sSelf, 0));
            const CharaParams* hit = CharaResources_GetParams(res, ActiveId(sSelf, who));
            if (player == nullptr || hit == nullptr)
                return false;
            return (BodyFlags(hit) & 0x10000) != 0 && by != player->id;
        });
        return 1;
    });
    Reg(o, "BreakRuleAllClear", 0, [](Player*, Frame&) {        // 0x250CF0: all but the two standard ones
        for (s32 i = 2; i < 12; i++)
            sBattleRules[i] = nullptr;
        sBreakId = -10;
        return 1;
    });
    Reg(o, "GetBreakID", 0, [](Player* self, Frame& f) {        // 0x250B20
        void* charas = Charas(self);
        const CharaParams* p = charas ? LiveParams(charas, 0) : nullptr;
        if (p == nullptr)
            return 0;
        const u32 flags = BodyFlags(p);
        if (flags == 0)
            return 0;
        if (flags & 0x210)                              // hidden or dead: the player lost
            sBreakId = -1;
        Answer(f, sBreakId);
        return 1;
    });
    // SetSeeCharacter(who, distance, how, run, far, angle, x, y, z [, motion, whom]):
    // an onlooker — AI move 7, which stays `distance` … `far` from the fight
    // (0x2501F0; the move is AiWatch in ai_moves.cpp).
    Reg(o, "SetSeeCharacter", 1, [](Player* self, Frame& f) {
        const s32 who = IntArg(f, 0), how = IntArg(f, 2, 1);
        const float distance = FloatArg(f, 1, 3.0f);
        float far = FloatArg(f, 4, distance + 1.0f);
        if (how != 3 && far < distance)
            far = distance + 1.0f;
        void* charas = Charas(self);
        if (charas == nullptr || TheWorld(self)->mapState == 0)
            return 0;
        static u32 data = 0;
        if (data == 0)
            data = guest::addr(guest::alloc(0x40));
        u32* w = guest::ptr<u32>(data);
        memset(w, 0, 0x40);
        const u32 links = TheWorld(self)->mapState + (u32)*guest::ptr<s32>(0x2EE518u) * 0x10;
        w[0] = guest::addr(MapDb_GetRoutePoints(MapDb_Get(), (*gPlayerData)[0x103]));
        w[1] = links + 0x20;
        w[2] = links + 0x60;
        const float place[4] = {FloatArg(f, 6) + sLocatorOrigin[0], FloatArg(f, 7) + sLocatorOrigin[1],
                                FloatArg(f, 8) + sLocatorOrigin[2], 1.0f};
        memcpy(&w[4], place, 16);
        w[8] = (u32)IntArg(f, 9, -1);
        w[9] = IntArg(f, 3) != 0;                       // run
        w[10] = (u32)how;
        const s32 whom = IntArg(f, 10);
        w[11] = (u32)(whom != 0 && whom != 100 ? whom + 1000 : whom);
        memcpy(&w[12], &distance, 4);
        memcpy(&w[13], &far, 4);
        const float angle = FloatArg(f, 5);
        memcpy(&w[14], &angle, 4);
        w[15] = 1;
        CharaMgr_fn143c60(charas, who);
        CharaMgr_SetState(charas, who, 2, 7, data);
        return 1;
    });
    Reg(o, "GetGroupID", 0, [](Player*, Frame& f) {             // 0x24FF50
        const CharaParams* p = CharaResources_GetParams(CharaResources_Get(), ArmedId(IntArg(f, 0)));
        Answer(f, p != nullptr ? p->faction : 0);
        return 1;
    });
    // SetGroupID(who, side): whose side it is on (the faction of both its bodies).  0x24FC80
    Reg(o, "SetGroupID", 0, [](Player* self, Frame& f) {
        const s32 id = ActiveId(self, IntArg(f, 0)), side = IntArg(f, 1);
        CharaResources* res = CharaResources_Get();
        CharaParams* p = CharaResources_GetParams(res, id);
        if (p == nullptr)
            return 0;
        p->faction = (s16)side;
        p = CharaResources_GetParams(res, ArmedId(id));
        if (p == nullptr)
            return 0;
        p->faction = (s16)side;
        return 1;
    });
}

// ═══ Object (0x281650): things in the world — see world_objects.cpp ═══════
// A script holds an object by the number Object.Add answers (the object
// itself, in the original; here its guest address).
inline void* ObjectsOf(Player* self) { return guest::ptr(TheWorld(self)->itemDrops); }
inline void* ObjectArg(Player* self, Frame& f, size_t i) {
    const s32 handle = IntArg(f, i);
    if (handle == 0 || TheWorld(self)->itemDrops == 0)
        return nullptr;
    void* object = guest::ptr((u32)handle);
    return ItemDrops_Has(ObjectsOf(self), object) != 0 ? object : nullptr;
}
void BuildObject(const Ref& o) {
    // Add(object id, x, y, z) → the object.  0x282B20
    Reg(o, "Add", 0, [](Player* self, Frame& f) {
        if (TheWorld(self)->itemDrops == 0)
            return 0;
        alignas(16) float m[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
        for (int i = 0; i < 3; i++)
            m[12 + i] = FloatArg(f, (size_t)i + 1);
        Answer(f, (s32)guest::addr(ItemDrops_Place(ObjectsOf(self), IntArg(f, 0), m)));
        return 1;
    });
    // Delete(object): 0x282990 (not while it is in someone's hand)
    Reg(o, "Delete", 0, [](Player* self, Frame& f) {
        if (void* object = ObjectArg(self, f, 0))
            if ((*WorldObject_Flags(object) & 0x20) == 0)
                ItemDrops_Remove(ObjectsOf(self), object);
        return 1;
    });
    // SetPos(object, locator): 0x281FB0
    Reg(o, "SetPos", 0, [](Player* self, Frame& f) {
        void* object = ObjectArg(self, f, 0);
        const Locator* at = AsLocator(sol::arg(f, 1));
        if (object == nullptr || at == nullptr)
            return 1;
        float* m = WorldObject_Matrix(object);
        memcpy(m, at->m, 16 * sizeof(float));
        for (int i = 0; i < 3; i++)
            m[12 + i] += sLocatorOrigin[i];
        return 1;
    });
    // Action(object id, which of them (from 1), how): a door or screen slides
    // Acquisition(object id): into the bag — carried if it is a pickup,
    // home if a dropped thing (0x281C40).
    Reg(o, "Acquisition", 0, [](Player*, Frame& f) {
        const s32 id = IntArg(f, 0), type = ItemDrops_ObjectType(id);
        const int home = type == 7 || type == 5 ? 1 : type == 3 ? 0 : -1;
        if (home >= 0 && PlayerData_HasFreeItemSlot(*gPlayerData, home) != 0)
            PlayerData_AddItemSorted(*gPlayerData, home, (u8)ItemDrops_ItemId(nullptr, id));
        return 1;
    });
    // (0x2826E0 → 0x21B310).
    Reg(o, "Action", 0, [](Player* self, Frame& f) {
        if (TheWorld(self)->itemDrops != 0)
            ItemDrops_Action(ObjectsOf(self), IntArg(f, 0), IntArg(f, 1), IntArg(f, 2));
        return 1;
    });
    // SettoChar(character, object): into its hand (0 = empty-handed).  0x282480
    Reg(o, "SettoChar", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        const s32 id = ActiveId(self, IntArg(f, 0));
        if (IntArg(f, 1) == 0) {
            CharaMgr_fn1438d0(charas, 0, id);
        } else if (void* object = ObjectArg(self, f, 1)) {
            if ((*WorldObject_Flags(object) & 0x20) == 0)
                CharaMgr_fn1438d0(charas, guest::addr(object), id);
        }
        return 1;
    });
    // ReleasefromChar(character, object): out of its hand, left where it is.  0x282230
    Reg(o, "ReleasefromChar", 0, [](Player* self, Frame& f) {
        void* charas = Charas(self);
        if (charas == nullptr)
            return 1;
        CharaMgr_fn1438d0(charas, 0, ActiveId(self, IntArg(f, 0)));
        if (void* object = ObjectArg(self, f, 1)) {
            alignas(16) static const float kStill[4] = {0, 0, 0, 0};
            VirtualCall<int, void*, const float*, int>(WorldObject_Vtable(object), 0x24, object, kStill, 0);
        }
        return 1;
    });
}

// ═══ Numbers kept in the player data ══════════════════════════════════════
inline s32& PdInt(u32 offset) { return *reinterpret_cast<s32*>(*gPlayerData + offset); }
inline s16& PdShort(u32 offset) { return *reinterpret_cast<s16*>(*gPlayerData + offset); }
inline s32 Clamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

// Get / Set / Add of one int.
void RegInt(const Ref& o, const char* what, u32 offset) {
    const std::string name = what;
    Reg(o, ("Get" + name).c_str(), 0, [offset](Player*, Frame& f) { Answer(f, PdInt(offset)); return 1; });
    Reg(o, ("Set" + name).c_str(), 1, [offset](Player*, Frame& f) { PdInt(offset) = IntArg(f, 0); return 1; });
    Reg(o, ("Add" + name).c_str(), 1, [offset](Player*, Frame& f) { PdInt(offset) += IntArg(f, 0); return 1; });
}
// The same per faction (0 the magistrate's men, 1 townspeople, 2 the gang), 0–100.
void RegFaction(const Ref& o, const char* what, u32 offset) {
    const std::string name = what;
    Reg(o, ("Get" + name).c_str(), 1, [offset](Player*, Frame& f) {
        const s32 who = IntArg(f, 0);
        if (who >= 0 && who <= 2)
            Answer(f, PdInt(offset + (u32)who * 4));
        return 1;
    });
    Reg(o, ("Set" + name).c_str(), 2, [offset](Player*, Frame& f) {
        const s32 who = IntArg(f, 0);
        if (who >= 0 && who <= 2)
            PdInt(offset + (u32)who * 4) = Clamp(IntArg(f, 1), 0, 100);
        return 1;
    });
    Reg(o, ("Add" + name).c_str(), 2, [offset](Player*, Frame& f) {
        const s32 who = IntArg(f, 0);
        if (who >= 0 && who <= 2)
            PdInt(offset + (u32)who * 4) = Clamp(PdInt(offset + (u32)who * 4) + IntArg(f, 1), 0, 100);
        return 1;
    });
}

void BuildNumbers(const Ref& root, const Ref& flags) {
    RegFaction(root, "PowerPoint", 0x144);              // 0x1E9740 / 0x1E9460 / 0x1E9170
    RegFaction(root, "TrustPoint", 0x12C);              // 0x1E8E60 / 0x1E8B80 / 0x1E8890
    // Vigour, up to its maximum (0x1E8740 / 0x1E8580 / 0x1E83B0).
    Reg(root, "GetStaminaPoint", 0, [](Player*, Frame& f) { Answer(f, PdShort(0x108)); return 1; });
    Reg(root, "SetStaminaPoint", 1, [](Player*, Frame& f) {
        PdShort(0x108) = (s16)Clamp(IntArg(f, 0), 0, PdShort(0x10A));
        return 1;
    });
    Reg(root, "AddStaminaPoint", 1, [](Player*, Frame& f) {
        PdShort(0x108) = (s16)Clamp(PdShort(0x108) + IntArg(f, 0), 0, PdShort(0x10A));
        return 1;
    });
    // 0x1E8070: the swords lying about are cleared away.
    Reg(root, "ReleaseDropWeapon", 0, [](Player*, Frame&) {
        if (const u32 db = *guest::ptr<u32>(0x2EE588u))         // SwordDb_Get
            SwordDb_FreeModels(guest::ptr<void>(db));
        return 1;
    });
    Reg(root, "GetFPS", 0, [](Player*, Frame& f) { Answer(f, *gFrameRate); return 1; });    // 0x1E7F40
    RegInt(flags, "SamuraiPoint", 0x124);               // 0x247450 / 0x2475C0 / 0x247280
    RegInt(flags, "EvaluationPoint", 0x128);            // 0x246F60 / 0x2470D0 / 0x246D90
    // SetExitStatus(n): how the scene ends for the game — see EventPlayer+0x10 (0x24B0F0).
    Reg(flags, "SetExitStatus", 1, [](Player* self, Frame& f) {
        self->result = IntArg(f, 0);
        return 1;
    });
    // SetMessage(text) / ClearMessage(): the note to self shown on the way
    // out (PlayerData+0x5C0, up to 127 characters) — 0x2479F0 / 0x247980.
    // SetEndingID(n): which ending the game is heading for (PlayerData+0x868).  0x246710
    // AddYuukakuUseNum(n): visits to the pleasure quarter (0x2468E0).
    Reg(flags, "AddYuukakuUseNum", 0, [](Player*, Frame& f) {
        u16* count = reinterpret_cast<u16*>(reinterpret_cast<u8*>(*gPlayerData) + 0x844);
        const s32 now = IntArg(f, 0) + *count;
        *count = (u16)(now < 1 ? 0 : now);
        return 1;
    });
    // The blacksmith (src/game/forge.cpp; wrappers 0x248180 … 0x24AE80).
    // The sword is "carried sword n", an int.
    Reg(flags, "CheckForgeHardness", 0, [](Player*, Frame& f) { Answer(f, Forge_CheckHardness(IntArg(f, 0, -1))); return 1; });
    Reg(flags, "CheckForgeQuality", 0, [](Player*, Frame& f) { Answer(f, Forge_CheckQuality(IntArg(f, 0, -1))); return 1; });
    Reg(flags, "CheckForgeSuccess", 0, [](Player*, Frame& f) { Answer(f, Forge_CheckSuccess(IntArg(f, 0, -1))); return 1; });
    Reg(flags, "CheckJudge", 0, [](Player*, Frame& f) { Answer(f, Forge_CheckJudge(IntArg(f, 0, -1))); return 1; });
    Reg(flags, "GetKoudo", 0, [](Player*, Frame& f) { Answer(f, Forge_GetKoudo(IntArg(f, 0, -1))); return 1; });
    Reg(flags, "GetUpParam", 0, [](Player*, Frame& f) { Answer(f, Forge_GetUpParam(IntArg(f, 0))); return 1; });
    Reg(flags, "SetWeaponSecret", 0, [](Player*, Frame& f) { Forge_Secret(IntArg(f, 0, -1)); return 1; });
    Reg(flags, "ForgePenalty", 0, [](Player*, Frame& f) { Answer(f, Forge_Penalty(IntArg(f, 0, -1))); return 1; });
    Reg(flags, "ForgeHardness", 0, [](Player*, Frame& f) { Forge_Hardness(IntArg(f, 0, -1)); return 1; });
    // Appraisal: the title a sword has earned, by what is judged.
    static const char* const kJudges[6] = {"JudgeParam_Attack", "JudgeParam_Defense", "JudgeParam_Quality",
                                           "JudgeParam_Kill", "JudgeParam_Experience", "JudgeParam_Name"};
    for (int judged = 0; judged < 6; judged++)
        Reg(flags, kJudges[judged], 0, [judged](Player*, Frame& f) {
            Answer(f, Forge_Judge(IntArg(f, 0, -1), judged));
            return 1;
        });
    Reg(flags, "GetFirstName", 0, [](Player*, Frame& f) {
        f.thread->result = sVm->newString(Forge_TitleName(IntArg(f, 0, -1)));
        return 1;
    });
    Reg(flags, "SetFirstName", 0, [](Player*, Frame& f) {
        Forge_SetTitle(IntArg(f, 0, -1), IntArg(f, 1, -1));
        return 1;
    });
    Reg(flags, "GetWeaponName", 0, [](Player*, Frame& f) {
        std::u16string name;
        for (const u16* c = Forge_SwordName(IntArg(f, 0, -1)); *c != 0; c++)
            name.push_back((char16_t)*c);
        f.thread->result = sVm->newString(name);
        return 1;
    });
    Reg(flags, "ForgeQuality", 0, [](Player*, Frame& f) {
        Forge_Quality(IntArg(f, 0, -1), IntArg(f, 1), IntArg(f, 2));
        return 1;
    });
    Reg(flags, "SetEndingID", 0, [](Player*, Frame& f) {
        *reinterpret_cast<s16*>(*gPlayerData + 0x868) = (s16)IntArg(f, 0);
        return 1;
    });
    // SetPlayerPose(locator): where the player is to stand, and facing which
    // way, when the scene hands over to the free world (PlayerData+0x6E40).  0x247770
    Reg(flags, "SetPlayerPose", 0, [](Player*, Frame& f) {
        const Locator* at = AsLocator(sol::arg(f, 0));
        static const Locator kFresh;
        alignas(16) float m[16];
        memcpy(m, at ? at->m : kFresh.m, sizeof m);
        for (int i = 0; i < 3; i++)
            m[12 + i] += sLocatorOrigin[i];
        memcpy(*gPlayerData + 0x6E40, m, sizeof m);
        return 1;
    });
    Reg(flags, "SetMessage", 1, [](Player*, Frame& f) {
        const std::u16string* text = sol::argString(f, 0);
        if (text == nullptr)
            return 1;
        const std::u16string wrapped = WrapText(*text, 0.92f, 8000);
        if (wrapped.size() < 0x80) {
            memcpy(*gPlayerData + 0x5C0, wrapped.data(), wrapped.size() * 2);
            memset(*gPlayerData + 0x5C0 + wrapped.size() * 2, 0, 2);
        }
        return 1;
    });
    Reg(flags, "ClearMessage", 0, [](Player*, Frame&) {
        memset(*gPlayerData + 0x5C0, 0, 0x100);
        return 1;
    });
}

// What a script calls that is not there yet.
Ref Missing(const Ref& object, const std::string& name, bool call) {
    static std::set<std::string> reported;
    if (reported.insert(name).second) {
        // One of the game's objects (or the root) lacks it: a built-in that is
        // not written yet.  Anything else is the script's own doing — e.g.
        // Character.GetPos of somebody who is not there answers nothing, and
        // the script then asks that nothing for a method (the original
        // reports "attribute not found" and stops the thread's call).
        bool ours = !object || object == sVm->root;
        if (!ours)
            for (const auto& entry : sVm->root->attrs)
                if (entry.second == object)
                    ours = true;
        if (ours)
            printf("[event] built-in not decompiled yet: %s%s\n", name.c_str(), call ? "()" : "");
        else
            printf("[event] a script asked a value for '%s', which it does not have\n", name.c_str());
    }
    if (!call) {
        Ref made = sVm->newObject();
        if (object)
            object->attrs[name] = made;
        return made;
    }
    auto stub = std::make_shared<sol::Native>();
    stub->onBegin = [](Frame& f) {
        f.thread->result = sVm->newInt(0);
        return 0;
    };
    return stub;
}

}  // namespace

extern "C" {

// ── 0x1E4DF0 ──────────────────────────────────────────────────────────────
EventPlayer* EventPlayer_Ctor(EventPlayer* mem) {
    if (!Enabled())
        return hle_EventPlayer_Ctor(mem);
    Player* self = P(mem);
    self->vm = 0;
    self->frames = 0;
    self->state = 0;
    self->result = 0;
    self->budget = 10;
    sSelf = self;
    return mem;
}

// ── 0x1E3810: a fresh VM with the game's objects, and the `config` script
// (which defines the id tables) run through it ─────────────────────────────
void EventPlayer_Reset(EventPlayer* mem, int scene) {
    if (!Enabled())
        return;
    Player* self = P(mem);
    self->scene = (u32)scene;
    sVm = std::make_unique<sol::VM>();
    sVm->missing = Missing;
    const Ref root = sVm->root;
    for (const char* name : {"Map", "Camera", "Character", "Effect", "UI", "Sound", "EventFlag", "Battle", "Object"})
        root->attrs[name] = sVm->newObject();
    {
        // (A locator is an object too: it has SetValue / at like any other.)
        auto locator = std::make_shared<Locator>();
        locator->cloneAttrs(*sVm->protoObject);
        root->attrs["Locator"] = locator;
    }
    BuildLocator(root->attrs["Locator"]);
    Reg(root, "L", 3, [](Player*, Frame& f) {
        f.thread->result = NewLocator(FloatArg(f, 0), FloatArg(f, 1), FloatArg(f, 2));
        return 1;
    });
    BuildEventFlag(root->attrs["EventFlag"]);
    BuildMap(root->attrs["Map"]);
    BuildEffect(root->attrs["Effect"]);
    BuildCharacter(root->attrs["Character"]);
    BuildCamera(root->attrs["Camera"]);
    BuildSound(root->attrs["Sound"]);
    BuildUI(root->attrs["UI"]);
    BuildBattle(root->attrs["Battle"]);
    BuildObject(root->attrs["Object"]);
    BuildNumbers(root, root->attrs["EventFlag"]);
    BuildRoot(root);
    // WOTS2_DUMP_BUILTINS=1 (debug): what the scripts can call, by object —
    // to set against config/script_builtins.json.
    if (getenv("WOTS2_DUMP_BUILTINS") != nullptr) {
        for (const auto& entry : root->attrs)
            printf("[builtin] . %s\n", entry.first.c_str());
        for (const char* name : {"Locator", "EventFlag", "Map", "Effect", "Character", "Camera", "Sound", "UI", "Battle", "Object"})
            for (const auto& entry : root->attrs[name]->attrs)
                printf("[builtin] %s %s\n", name, entry.first.c_str());
    }
    const Ref config = LoadScript("config");
    if (!config) {
        self->state = 3;
        return;
    }
    sVm->callNow(root, config, sVm->newList(), root);
    sVm->update(0);
    self->state = 1;
    root->attrs["ViewerMode"] = sVm->newInt(0);
}

// ── 0x1E4D50 ──────────────────────────────────────────────────────────────
EventPlayer* EventPlayer_Dtor(EventPlayer* self, s16 flag) {
    if (self != nullptr) {
        if (Enabled()) {
            sVm.reset();
            sSelf = nullptr;
        }
        if (flag > 0)
            guest::free(self);
    }
    return self;
}

void EventPlayer_StartScript(EventPlayer* mem, const char* script, int second);

// ── 0x1E3450 + 0x1E42E0: the day's story: the `StartUp2` script decides
// what, if anything, happens here and now ──────────────────────────────────
void EventPlayer_StartDay(EventPlayer* mem, int a, int b) {
    if (!Enabled())
        return hle_EventPlayer_StartDay(mem, a, b);
    Player* self = P(mem);
    self->result = 0;
    // WOTS2_MONEY=n (debug): start the game with n mon.
    static const char* money = getenv("WOTS2_MONEY");
    if (money != nullptr) {
        *reinterpret_cast<s16*>(*gPlayerData + 0x106) = (s16)atoi(money);
        money = nullptr;
    }
    // WOTS2_AREA=n (debug, with WOTS2_SKIP_INTRO=1): start in area n (0–9)
    // instead of Otemon.
    static const char* area = getenv("WOTS2_AREA");
    if (area != nullptr) {
        (*gPlayerData)[0x103] = (u8)atoi(area);
        if (const u32 scene = *guest::ptr<u32>(0x2EE960u))      // (and where the area map's cursor will be)
            guest::ptr<GameplayScene>(scene)->mapArea = atoi(area);
        area = nullptr;
    }
    // WOTS2_PERIOD=n (debug): start in period n of the day (0 morning … 4
    // late night) — to reach the end of a day quickly.
    static const char* period = getenv("WOTS2_PERIOD");
    if (period != nullptr) {
        (*gPlayerData)[0x101] = (u8)atoi(period);
        (*gPlayerData)[0x102] = (u8)guest::ptr<s32>(0x2DA160u)[(u32)atoi(period) % 5];       // night?
        period = nullptr;
    }
    // WOTS2_DAY=n (debug): start on day n (0 the first … 9).
    static const char* day = getenv("WOTS2_DAY");
    if (day != nullptr) {
        (*gPlayerData)[0x100] = (u8)((u32)atoi(day) % 10);
        day = nullptr;
    }
    // Debug hook: WOTS2_START_SCRIPT=<name> starts that script instead of
    // what StartUp2 would choose.
    static const char* forced = getenv("WOTS2_START_SCRIPT");
    if (forced != nullptr) {
        EventPlayer_StartScript(mem, forced, 0);
        forced = nullptr;
        return;
    }
    // Debug hook: WOTS2_SKIP_INTRO=1 marks the first step of story line 1 as
    // taken (flag S1_STEP), so that StartUp2 does not play the opening: the
    // free world with the real scripts, at once.  The story's state is then
    // not one the game itself would produce.
    static bool skipIntro = getenv("WOTS2_SKIP_INTRO") != nullptr;
    if (skipIntro) {
        skipIntro = false;
        (*gPlayerData)[0x740 + 188] = 1;
    }
    const Ref code = LoadScript("StartUp2");
    if (!code)
        return;
    sVm->callNow(sVm->root, code, sVm->newList(), sVm->newObject());
    if (!sVm->update(0))
        self->state = 2;
}

// Debug aid for WOTS2_TOUR_SEEK (not game code): the story script StartUp2
// would start if the player were on map `map` now, or null.  Nothing is
// started and nothing is changed.
const char* EventPlayer_ProbeStory(int map) {
    if (!Enabled() || !sVm || !sVm->threads.empty())
        return nullptr;
    const Ref code = LoadScript("StartUp2");
    if (!code)
        return nullptr;
    sProbeMap = map;
    sProbeHit.clear();
    sVm->callNow(sVm->root, code, sVm->newList(), sVm->newObject());
    sVm->threads.clear();                               // (a StartThread of the script's)
    sProbeMap = -1;
    return sProbeHit.empty() ? nullptr : sProbeHit.c_str();
}

// ── 0x1E3460: calls the scripts' own `StartScript(name)` ──────────────────
void EventPlayer_StartScript(EventPlayer* mem, const char* script, int second) {
    if (!Enabled())
        return hle_EventPlayer_StartScript(mem, script, second);
    Player* self = P(mem);
    Ref args = sVm->newList();
    static_cast<sol::List*>(args.get())->items.push_back(sVm->newString(sol::widen(script)));
    if (second != 0)
        static_cast<sol::List*>(args.get())->items.push_back(
            sVm->newString(sol::widen(guest::ptr<const char>((u32)second))));
    Ref object, method;
    if (sVm->resolve("StartScript", &object, &method))
        sVm->callNow(object, method, args, sVm->newObject());
    self->state = 2;
}

// ── 0x1E3160 / 0x1E2E20: a scene can be skipped if its script has set
// `OnCancelEvent`; skipping drops every thread and runs that instead ───────
int EventPlayer_CanSkip(EventPlayer*) {
    return Enabled() && sVm && sVm->root->attrs.count("OnCancelEvent") != 0;
}
void EventPlayer_Skip(EventPlayer*) {
    if (!Enabled() || !sVm)
        return;
    sVm->threads.clear();                               // 0x1B4680
    auto it = sVm->root->attrs.find("OnCancelEvent");
    if (it == sVm->root->attrs.end())
        return;
    const Ref handler = it->second;
    sVm->root->attrs.erase(it);
    sVm->start(sVm->root, handler, sVm->newList(), sVm->newObject());
}

// ── 0x1E3790: → non-zero while a script runs ──────────────────────────────
int EventPlayer_Update(EventPlayer* mem, int a) {
    if (!Enabled())
        return hle_EventPlayer_Update(mem, a);
    Player* self = P(mem);
    self->frames++;
    if (self->state != 2)
        return 0;
    const bool done = sVm->update(self->budget);
    if (done)
        self->state = 1;
    return !done;
}

// ── 0x1E3350: the scene is over: tidy up after the script ─────────────────
void EventPlayer_Finish() {
    if (!Enabled() || !sVm || sSelf == nullptr)
        return;
    sAdvance = false;
    sVm->root->attrs.erase("OnCancelEvent");
    if (*guest::ptr<u32>(0x2EE528u) != 0) {             // 0x1EDBB0: the camera eases as usual again
        EaseHolder_SetNext(EaseSlot(), nullptr);
        EaseHolder_Begin(EaseSlot());
    }
    // 0x201EF0: effects — the letterbox goes, the map's lanterns are lit again.
    RenderMgr_TriggerEffect(guest::ptr<void>(*guest::ptr<u32>(0x2EE684u)), 8, 0, 0);
    Lights_EnableMap(1);
    // 0x1F11C0: characters — those kept aside are removed, the lists and the
    // scripts' own paths (0x1FC000) emptied, the player let go and shown.
    if (sSelf->scene != 0) {
        World* world = TheWorld(sSelf);
        for (s32 id : sKeptCharas) {
            World_RemoveActor(world, id + 10000);
            World_RemoveActor(world, ArmedId(id) + 10000);
        }
        sKeptCharas.clear();
        sScriptCharas.clear();
        Ref object, method;
        if (sVm->resolve("Character.PathAllInit", &object, &method))
            sVm->callNow(object, method, sVm->newList(), sVm->newObject());
        if (void* charas = Charas(sSelf)) {
            CharaMgr_Release(charas, 0);
            CharaMgr_SetActive(charas, 0, 1);
        }
    }
    // 0x24EE50: battle — no rules left; the player's CharaParams +0x46 = −1.
    for (auto& rule : sBattleRules)
        rule = nullptr;
    if (void* params = CharaResources_GetParams(CharaResources_Get(), 0))
        *reinterpret_cast<s16*>(static_cast<u8*>(params) + 0x46) = -1;
    // 0x216C10: the scripts' origin is nowhere again.  (Without this a scene
    // inherited the origin of the one before: A01D1x sets (0, 0, 6), and in
    // A01A4x after it the innkeeper stood in a wall and the scene never ended.)
    sLocatorOrigin[0] = sLocatorOrigin[1] = sLocatorOrigin[2] = 0.0f;
    sLocatorOrigin[3] = 1.0f;
    // 0x20E240: sound — the music and the voices stop, the effects are back
    // at the volume of the setting (scripts turn them down: VolumeAllSE).
    Bgm_StopAll(1);
    Bgm_StopVoiceLayer(0);
    if (const u32 config = *guest::ptr<u32>(0x2EE68Cu))
        *guest::ptr<s32>(0x2EDAD0u) = *guest::ptr<s32>(config);
    if (sVm->update(0))
        EventPlayer_Reset(reinterpret_cast<EventPlayer*>(sSelf), (int)sSelf->scene);
}

}  // extern "C"
