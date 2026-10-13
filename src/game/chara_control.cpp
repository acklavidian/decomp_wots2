// Who drives a character — decompiled from 0x1C6C50–0x1C7E20.
//
// A character's control (Chara+0x19C) holds one *routine* and steps it every
// frame while the character is free to act.  There are two kinds: the pad
// (the player) and an AI move (everyone else, and the player during walk-ins
// and scripted moves).  A routine's step returns non-zero when it has
// finished; that and the routine's address are how the task system knows the
// move it asked for is done.

#include "engine/pad.h"
#include "game/chara.h"
#include "native/guest.h"

#include <cmath>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* PadMgr_Get();                                 // 0x13A920
    u16   Pad_GetHeld(void* mgr, int port);             // 0x13A0E0
    u16   Pad_GetPressed(void* mgr, int port);          // 0x13A0C0
    int   Pad_GetStick(void* mgr, int port, u8* x, u8* y);  // 0x139FB0
    void  Matrix_Identity(void* m);                     // 0x107428
}

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
enum : u32 { kVtRoutine = 0x2EB860u, kVtAiRoutine = 0x2EB840u, kVtPadRoutine = 0x2EB880u };
enum : u32 { kRoutineDestroy = 0x08, kRoutineAttach = 0x0C, kRoutineStep = 0x10 };
enum : u32 { kMoveDestroy = 0x08, kMoveStart = 0x0C, kMoveStep = 0x10, kMoveResult = 0x14, kMoveEnd = 0x18 };

static_assert(sizeof(AiRoutine) == 0x14 && sizeof(PadRoutine) == 0x1C && sizeof(CharaControl) == 0x10, "routines");

// ═══ The routines ═════════════════════════════════════════════════════════

// ── 0x1C6C50 ──────────────────────────────────────────────────────────────
static void Routine_Attach(CharaRoutine* self, u32 body) { self->body = body; }
GUEST_BIND(0x1C6C50, Routine_Attach);

// ── 0x1C6E40 ──────────────────────────────────────────────────────────────
static CharaRoutine* Routine_Dtor(CharaRoutine* self, s16 flag) {
    if (self) {
        self->vtable = kVtRoutine;
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
GUEST_BIND(0x1C6E40, Routine_Dtor);

// ── An AI move (0x1C6C60–0x1C6DE0) ────────────────────────────────────────
static AiRoutine* AiRoutine_Ctor(AiRoutine* self, s32 mode, const void* data) {     // 0x1C6DE0
    self->base.body = 0;
    self->base.vtable = kVtAiRoutine;
    self->started = 0;
    self->base.kind = mode;
    self->move = AiMove_Create((u32)mode, data);
    return self;
}
static u32 AiRoutine_Step(AiRoutine* self) {                                        // 0x1C6C60
    if (self->move == 0 || self->base.body == 0)
        return 0;
    const u32 vt = *P<u32>(self->move);
    if (!self->started) {
        VirtualCall<void, void*, u32>(vt, kMoveStart, P(self->move), self->base.body);
        self->started = 1;
        return 0;
    }
    VirtualCall<void, void*, u32>(vt, kMoveStep, P(self->move), self->base.body);
    if (self->base.body == 0 || CharaBody_Busy(P<CharaBody>(self->base.body)))
        return 0;
    return VirtualCall<u32, void*, u32>(*P<u32>(self->move), kMoveResult, P(self->move), self->base.body);
}
GUEST_BIND(0x1C6C60, AiRoutine_Step);
static AiRoutine* AiRoutine_Dtor(AiRoutine* self, s16 flag) {                       // 0x1C6D30
    if (!self)
        return self;
    self->base.vtable = kVtAiRoutine;
    if (self->move != 0) {
        VirtualCall<void, void*, u32>(*P<u32>(self->move), kMoveEnd, P(self->move), self->base.body);
        VirtualCall<void*, void*, int>(*P<u32>(self->move), kMoveDestroy, P(self->move), 1);
        self->move = 0;
    }
    self->base.vtable = kVtRoutine;
    if (flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x1C6D30, AiRoutine_Dtor);

// ── The pad (0x1C6E90–0x1C7B60) ───────────────────────────────────────────
static inline CharaBody* Body(PadRoutine* self) { return P<CharaBody>(self->base.body); }
static inline const float* Facing(CharaBody* b) { return &P<CharaParams>(b->params)->matrix[8]; }

// Pad directions are turned into the world by the camera's heading.
static void ToWorld(float* dir) {
    alignas(16) float m[16];
    if (*P<u32>(0x2EE528u) == 0)
        Matrix_Identity(m);
    else
        Camera_GetViewRotation(m);
    const float length = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const float x = dir[0] / length, y = dir[1] / length, z = dir[2] / length, w = dir[3];
    for (int j = 0; j < 4; j++)
        dir[j] = x * m[j] + y * m[4 + j] + z * m[8 + j] + w * m[12 + j];
}

// 0x1C77F0: the d-pad, one of eight ways.  → 2 (a full push), or 0.
static int DpadDirection(PadRoutine*, float* out, u32 held) {
    static const float (*const kWay)[4] = guest::ptr<const float[4]>(0x2D8500u);   // from up, by eighths, through left
    if ((held & 0xF000) == 0)
        return 0;
    int way = -1;
    if (held & kPadUp)          way = (held & kPadLeft) ? 1 : (held & kPadRight) ? 7 : 0;
    else if (held & kPadDown)   way = (held & kPadLeft) ? 3 : (held & kPadRight) ? 5 : 4;
    else if (held & kPadLeft)   way = 2;
    else if (held & kPadRight)  way = 6;
    if (way >= 0)
        memcpy(out, kWay[way], 16);
    ToWorld(out);
    return 2;
}
// 0x1C76B0: the left stick.  → 1 pushed, 2 pushed right over, 0 centred.
static int StickDirection(PadRoutine* self, float* out) {
    u8 x, y;
    Pad_GetStick(P(self->pad), self->port, &x, &y);
    if (x >= 0x50 && x <= 0xB0 && y >= 0x50 && y <= 0xB0)
        return 0;
    out[0] = (float)-((int)x - 0x80);
    out[2] = (float)-((int)y - 0x80);
    out[1] = out[3] = 0.0f;
    ToWorld(out);
    return (x < 0x20 || x > 0xE0 || y < 0x20 || y > 0xE0) ? 2 : 1;
}

// 0x1C6FB0: a blow with the sword.
static int PadAttack(PadRoutine* self, u32 pressed) {
    CharaBody* body = Body(self);
    const int airborne = (body->actionFlags & kActfAirborne) != 0;
    float facing[4];
    memcpy(facing, Facing(body), 16);
    if ((body->actionFlags & kActfCombo) == 0) {
        if ((pressed & (kPadSquare | kPadTriangle)) == 0)
            return 0;
        if (CharaBody_Attack(body, PadCommands_Attack(P(self->commands), facing, airborne)) == -1)
            return 0;
    } else if (CharaBody_Attack(body, PadCommands_ComboAttack(P(self->commands), facing, airborne)) == -1) {
        return 0;
    }
    PadCommands_Clear(P(self->commands));
    return 1;
}

// 0x1C70C0: the attack buttons.  → 1 when something was begun.
static int PadActions(PadRoutine* self, u32 held, u32 pressed, int push) {
    CharaBody* body = Body(self);
    if ((body->flags & kBodyArmed) == 0) {
        // Unarmed: R1 + Square kicks.
        if ((held & kPadR1) == 0 || (pressed & kPadSquare) == 0)
            return 0;
        if (body->flags & kBodyCarrying) {
            CharaBody_fn1d21f0(body, push != 0);
            return 0;
        }
        CharaBody_Kick(body, (push != 0) + ((body->actionFlags & kActfAirborne) ? 2 : 0));
        return 1;
    }
    if (CharaBody_IsLyingAction(body)) {
        const u32 now = Pad_GetPressed(P(self->pad), self->port);
        return (now & (kPadSquare | kPadTriangle)) && CharaBody_fn1d1960(body);
    }
    if (body->action == 0x93) {
        // After a parry: Square or Triangle answers.
        const u32 now = Pad_GetPressed(P(self->pad), self->port);
        int which = -1;
        if (now & kPadSquare)   which = 0;
        if (now & kPadTriangle) which = 1;
        if (which != -1)
            CharaBody_fn1d2160(body, which);
        return 0;
    }
    return PadAttack(self, pressed);
}

// 0x1C6E90: walking, or stepping round an opponent while locked on.
static int PadMove(PadRoutine* self, const float* dir, int push) {
    CharaBody* body = Body(self);
    if ((body->targetFlags & 0x1000000) == 0) {
        if (push != 0) {
            CharaBody_Walk(body, push == 2);
            memcpy(body->input, dir, 16);
            return 1;
        }
    } else {
        float facing[4];
        memcpy(facing, Facing(body), 16);
        int which = PadCommands_Special(P(self->commands), facing);
        if (which != -1) {
            CharaBody_Special(body, which);
            return 1;
        }
        if (push != 0 && (which = PadCommands_Dodge(P(self->commands), facing, dir)) != -1) {
            CharaBody_DodgeTo(body, which);
            return 1;
        }
    }
    CharaBody_Idle(body);
    return 0;
}

// 0x1C7260
static u32 PadRoutine_Step(PadRoutine* self) {
    const u32 held = Pad_GetHeld(P(self->pad), self->port);
    const u32 pressed = Pad_GetPressed(P(self->pad), self->port);
    CharaBody* body = Body(self);

    // R2, sword drawn: keep facing the opponent.
    if ((held & kPadR2) == 0) {
        body->targetFlags &= ~0x20u;
        if (self->guardHeld == 1) {
            self->guardHeld = 0;
            body->targetFlags |= 0x10;
        }
    } else if (body->flags & kBodyArmed) {
        body->targetFlags |= 0x20;
        if (self->guardHeld == 0)
            self->guardHeld = 1;
    }

    float dir[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int push = DpadDirection(self, dir, held);
    if (push == 0)
        push = StickDirection(self, dir);
    PadCommands_Push(P(self->commands), dir);

    // Forward or back against a blow in progress is remembered (parries).
    u32 lean = 0;
    if ((body->actionFlags & kActfAttack) && (dir[0] != 0.0f || dir[2] != 0.0f)) {
        const float* f = Facing(body);
        lean = dir[0] * f[0] + dir[1] * f[1] + dir[2] * f[2] > 0.0f ? 0x1000000u : 0x2000000u;
    }
    CharaBody_PushInput(body, lean, 1);

    if (PadActions(self, held, pressed, push))
        return 0;
    memcpy(body->input, dir, 16);
    if (body->flags & kBodyInAir)
        return 0;
    // L1: draw the sword / put it away.
    if ((pressed & kPadL1) && (body->flags & (kBodyFaceUp | kBodyFaceDown)) == 0) {
        if (body->flags & kBodyArmed)
            CharaBody_fn1d1c90(body);
        else
            CharaBody_fn1d1ce0(body);
        return 0;
    }
    // Down: a quick button gets it up again.
    if (CharaBody_CanRoll(body) && PadCommands_Pressed(P(self->commands), kPadCross | kPadR1, 3) && CharaBody_Rise(body))
        return 0;
    if ((pressed & (kPadCross | kPadR1)) && CharaBody_IsLyingAction(body)) {
        CharaBody_RollUp(body);
        return 0;
    }
    if ((pressed & kPadCross) && (body->flags & kBodyCarrying) == 0) {
        CharaBody_Jump(body);
        return 0;
    }
    // R1, sword drawn: guard.
    if (body->flags & kBodyArmed) {
        if (held & kPadR1) {
            if (CharaBody_Guard(body, 0))
                return 0;
        } else if (CharaBody_GuardEnd(body)) {
            return 0;
        }
    }
    if (pressed & kPadSquare) {
        if (body->flags & kBodyCarrying)
            CharaBody_fn1d1be0(body);
        else
            CharaBody_StartRoll(body);
    }
    PadMove(self, dir, push);
    return 0;
}
GUEST_BIND(0x1C7260, PadRoutine_Step);

static PadRoutine* PadRoutine_Ctor(PadRoutine* self, int port) {                    // 0x1C7AB0
    self->base.body = 0;
    self->base.vtable = kVtPadRoutine;
    self->port = port;
    self->pad = guest::addr(PadMgr_Get());
    void* commands = MemAlloc(0x410);
    self->commands = commands ? guest::addr(PadCommands_Ctor(commands, PadMgr_Get(), port)) : 0;
    self->guardHeld = 0;
    self->base.kind = -1;
    return self;
}
static PadRoutine* PadRoutine_Dtor(PadRoutine* self, s16 flag) {                    // 0x1C7A20
    if (!self)
        return self;
    self->base.vtable = kVtPadRoutine;
    if (self->commands != 0)
        PadCommands_Dtor(P(self->commands), 1);
    self->commands = 0;
    self->pad = 0;
    self->base.vtable = kVtRoutine;
    if (flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x1C7A20, PadRoutine_Dtor);

// ── 0x1C7E20 ──────────────────────────────────────────────────────────────
u32 CharaAction_Make(int kind, u32 mode, const void* data) {
    if (kind == 2) {
        void* mem = MemAlloc(sizeof(AiRoutine));
        return mem ? guest::addr(AiRoutine_Ctor(static_cast<AiRoutine*>(mem), (s32)mode, data)) : 0;
    }
    if (kind == 0 || kind == 1) {
        void* mem = MemAlloc(sizeof(PadRoutine));
        return mem ? guest::addr(PadRoutine_Ctor(static_cast<PadRoutine*>(mem), kind)) : 0;
    }
    return 0;
}

// ═══ The control ══════════════════════════════════════════════════════════

static void DropRoutine(CharaControl* self) {
    self->released = 1;
    self->current = 0;
    if (self->handle != 0) {
        CharaRoutine* r = P<CharaRoutine>((u32)self->handle);
        VirtualCall<void*, CharaRoutine*, int>(r->vtable, kRoutineDestroy, r, 1);
    }
    self->handle = 0;
}

// ── 0x1C7E00 ──────────────────────────────────────────────────────────────
CharaControl* CharaControl_Ctor(CharaControl* self) {
    self->body = 0;
    self->handle = 0;
    self->released = 1;
    self->current = 0;
    return self;
}
// ── 0x1C7D80 ──────────────────────────────────────────────────────────────
CharaControl* CharaControl_Dtor(CharaControl* self, s16 flag) {
    if (self) {
        self->body = 0;
        DropRoutine(self);
        if (flag > 0)
            MemFree(self);
    }
    return self;
}
// ── 0x1C7D10 ──────────────────────────────────────────────────────────────
void CharaControl_Attach(CharaControl* self, CharaBody* body) { self->body = guest::addr(body); }
// ── 0x1C7D20 ──────────────────────────────────────────────────────────────
void CharaAction_fn1c7d20(CharaAction* action) { DropRoutine(action); }
// ── 0x1C7B60 ──────────────────────────────────────────────────────────────
s32 CharaControl_Kind(CharaControl* self) {
    return self->handle != 0 ? P<CharaRoutine>((u32)self->handle)->kind : -1;
}

// ── 0x1C7B80 ──────────────────────────────────────────────────────────────
u32 CharaControl_Set(CharaControl* self, u32 routine) {
    DropRoutine(self);
    self->handle = (s32)routine;
    CharaRoutine* r = P<CharaRoutine>(routine);
    VirtualCall<void, CharaRoutine*, u32>(r->vtable, kRoutineAttach, r, self->body);
    return routine;
}

// ── 0x1C7C00 ──────────────────────────────────────────────────────────────
void CharaControl_Step(CharaControl* self) {
    CharaBody* body = P<CharaBody>(self->body);
    // Anyone but the player (and character 1) who lies idle on the ground
    // gets up again — unless its routine is a fight, which sees to that.
    if (*P<s32>(body->params) > 1) {
        if (CharaControl_Kind(self) != 1) {
            const u32 link = P<CharaParams>(body->params)->link[5];
            const u32 flags = link ? *P<u32>(link) : 0;
            if ((flags & (kBodyFaceUp | kBodyFaceDown)) && (flags & kBodyIdle))
                CharaBody_RollUp(body);
        }
    }
    if (body->flags & kBodyCorpse) {
        DropRoutine(self);
        return;
    }
    if (self->released != 0 && self->handle != 0) {
        CharaRoutine* r = P<CharaRoutine>((u32)self->handle);
        self->current = VirtualCall<u32, CharaRoutine*>(r->vtable, kRoutineStep, r);
        return;
    }
    if (self->released == 0)
        CharaBody_Idle(body);
}
