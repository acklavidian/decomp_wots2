// The character body — decompiled from 0x1C9760–0x1CC8A0 (state, actions,
// movement) and 0x1D0ED0–0x1D2940 (the requests the controls make).
//
// The body does one *action* at a time.  An action is a number; the motion
// pack's table says which motion plays for it, and the motion's ActionDef
// (chara_motions.h) what kinds of action may cut in and from which frame.
// Every frame the motion player poses the skeleton and reports how far the
// motion carried the character; the body turns that, plus a velocity for
// jumps, falls and knock-backs, into the position it wants (`position`),
// which the character's map step then checks against the ground and walls.

#include "game/chara.h"
#include "game/chara_motions.h"
#include "native/guest.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    int   Rand_Next();                                  // 0x1200F0
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Matrix_Rotate(void* dst, const void* src, float angle);   // 0x107618  about y
}
int Matrix_TurnTowards(float degrees, float* matrix, const float* point);         // 0x1C8C40 (task_actions.cpp)
extern "C" void Fx_BloodPool(const float* pos, const float* dir);                       // world_effects.cpp

static_assert(sizeof(CharaBody) == 0x1F0 && offsetof(CharaBody, packA) == 0x1A0, "CharaBody");
static_assert(offsetof(CharaBody, position) == 0xD0 && offsetof(CharaBody, input) == 0x110, "CharaBody");
static_assert(offsetof(CharaBody, inputCount) == 0x84 && offsetof(CharaBody, techniques) == 0x1D4, "CharaBody");

static s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);
static u32* const gFrameCount = guest::ptr<u32>(0x2EE6ACu);

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }
static inline CharaParams*  Params(CharaBody* b) { return P<CharaParams>(b->params); }
static inline MotionPlayer* Player(CharaBody* b) { return P<MotionPlayer>(b->player); }
static inline Skeleton*     Bones(CharaBody* b)  { return P<Skeleton>(b->skeleton); }
static inline float* Position(CharaBody* b) { return &Params(b)->matrix[12]; }
static inline const ActionDef* Def(const CharaBody* b) {
    if (b->actionDef == 0)
        return nullptr;
    const u32 def = *P<u32>(b->actionDef);
    return def ? P<const ActionDef>(def) : nullptr;
}
static inline const ActionAttack* Attack(const ActionDef* d) { return d && d->attack ? P<const ActionAttack>(d->attack) : nullptr; }
static inline int Frame(CharaBody* b) { return (int)Player(b)->lastFrame; }

// Small vector helpers (libvu0 in the original).
static inline void Copy(float* d, const float* s) { memcpy(d, s, 16); }
static inline void Zero(float* d) { d[0] = d[1] = d[2] = d[3] = 0.0f; }                 // the vector at 0x306900
static inline void UnitW(float* d) { d[0] = d[1] = d[2] = 0.0f; d[3] = 1.0f; }          // the vector at 0x2D1AB0
static inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float Normalize(float* d, const float* s) {      // → the length (0x136BA0); a zero vector stays zero
    const float length = sqrtf(Dot(s, s));
    const float scale = length != 0.0f ? 1.0f / length : 0.0f;
    d[0] = s[0] * scale; d[1] = s[1] * scale; d[2] = s[2] * scale; d[3] = s[3];
    return length;
}
// v × m for a row-major matrix (sceVu0ApplyMatrix).
static void Apply(float* out, const float* m, const float* v) {
    float r[4];
    for (int j = 0; j < 4; j++)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    Copy(out, r);
}
// Which way the root bone's z axis points, up or down: how a body lies.
static bool FaceDown(CharaBody* self) {
    const float* root = P<Bone>(Bones(self)->bones)->world;
    return root[9] < 0.0f;          // (0, 0, 1, 0) × world, y component
}

// ── 0x1CC8A0 ──────────────────────────────────────────────────────────────
CharaBody* CharaBody_Ctor(CharaBody* self, CharaParams* params) {
    self->action = 0;
    self->skeleton = 0;
    MotionPlayer* player = static_cast<MotionPlayer*>(MemAlloc(sizeof(MotionPlayer)));
    self->player = player ? guest::addr(MotionPlayer_Ctor(player)) : 0;
    self->packA = self->packB = 0;
    self->motionId = 0;
    self->actionDef = 0;
    self->params = guest::addr(params);
    self->flags = kBodyStanding;
    self->actionFlags = 0;
    self->lying = 0;
    self->state = 0;
    self->targetFlags = 0;
    self->techniques = self->techniqueCount = 0;
    self->comboRoot = self->comboNode = self->comboNext = 0;
    self->clock = -1;
    self->spin[0] = self->spin[1] = 0.0f;
    Zero(self->tumble);
    memset(self->inputs, 0, sizeof self->inputs);
    self->inputCount = self->inputAt = 0;
    self->inputRating = 0;
    self->swordValue = -1;
    self->stagger = 0;
    self->hitStop = -1;
    self->_2c = 0;
    self->_34 = self->_38 = 0;
    self->_3c = self->_40 = 0;
    UnitW(self->takeoff);
    Zero(self->travel);
    params->link[0] = guest::addr(self);
    params->link[7] = guest::addr(&self->clock);
    params->link[5] = guest::addr(&self->flags);
    params->link[6] = guest::addr(&self->actionFlags);
    params->link[8] = guest::addr(&self->targetFlags);
    Zero(self->velocity);
    UnitW(self->position);
    Zero(self->input);
    self->scriptAction = self->scriptLoops = 0;
    self->scriptStart = self->scriptEnd = 0;
    self->scriptStill = 0;
    self->scriptBlend = 0;
    return self;
}

// ── 0x1CC830 ──────────────────────────────────────────────────────────────
CharaBody* CharaBody_Dtor(CharaBody* self, s16 flag) {
    if (self) {
        if (self->player != 0)
            MotionPlayer_Dtor(Player(self), 1);
        if (flag > 0)
            MemFree(self);
    }
    return self;
}

// ── 0x1CC7E0 ──────────────────────────────────────────────────────────────
// Bones 28 and 29 are the hands' sword mounts; their rest matrices are put
// back at the start of every action.
void CharaBody_SetSkeleton(CharaBody* self, Skeleton* skeleton) {
    self->skeleton = guest::addr(skeleton);
    Bone* bones = P<Bone>(skeleton->bones);
    Matrix_Copy(self->handL, bones[28].local);
    Matrix_Copy(self->handR, bones[29].local);
}

// ── 0x1CAC30 ──────────────────────────────────────────────────────────────
s32 CharaBody_MotionFor(CharaBody* self, s32 action) {
    for (u32 pack : {self->packA, self->packB}) {
        if (pack == 0 || *P<u32>(pack) == 0)
            continue;
        const s32* table = P<s32>(*P<u32>(pack));
        const s32* pair = P<s32>((u32)table[1]);
        for (s32 i = 0; i < table[0]; i++)
            if (pair[i * 2] == action)
                return pair[i * 2 + 1];
    }
    return -1;
}
static const ActionDef* DefFor(CharaBody* self, s32 action) {
    const u32 holder = CharaResources_ActionOf(CharaResources_Get(), CharaBody_MotionFor(self, action));
    if (holder == 0 || *P<u32>(holder) == 0)
        return nullptr;
    return P<const ActionDef>(*P<u32>(holder));
}

// ── 0x1CB2B0 ──────────────────────────────────────────────────────────────
// What an action's record says about it, as body flags.
static void ApplyActionFlags(CharaBody* self, u32 f) {
    if (f & 0x202004) {
        if ((self->flags & kBodyInAir) == 0)
            Copy(self->takeoff, Position(self));
        self->flags |= kBodyInAir;
        self->flags &= ~(u32)kBodyStanding;
        self->velocity[1] = (f & 4) ? 0.36f : (f & 0x200000) ? 0.4f : 0.28399998f;
    }
    if (f == 4 || f == 0x200000 || f == 0x2000)
        self->actionFlags |= kActfAirborne;
    if (f & 2)
        self->actionFlags |= kActfCombo;
    if (f & 8)
        self->actionFlags |= kActfAttack;
    else
        self->actionFlags &= ~(u32)kActfAttack;
    if (f & 0x20)
        self->actionFlags |= kActfWalk;
    else if (f & 0x40)
        self->actionFlags |= kActfRun;
    else if (f & 0x40000)
        self->actionFlags |= kActfDash;
    if (f & 0x80)     self->actionFlags |= kActfGuard;
    if (f & 0x1000)   self->actionFlags |= kActfB200;
    if (f & 0x100)    self->actionFlags |= kActfA400;
    if (f & 0x10)     self->actionFlags |= kActfLying;
    if (f & 0x200)    self->actionFlags |= 0x1000000;
    if (f & 0x400)    self->actionFlags |= 0x2000000;
    if (f & 0x800)    self->actionFlags |= 0x4000000;
    if (f & 0x8000)   self->state |= kStGetUpA;
    if (f & 0x10000)  self->state |= kStGetUpB;
    if ((self->flags & kBodyInAir) && (self->actionFlags & kActfCombo))
        self->state |= kStAirCombo;
    if (f & 0x100000) self->state |= 0x2000000;
    if (f & 0x400000) {
        self->flags &= ~0xFu;
        self->flags |= kBodyStanding;
    }
    if (f & 0x800000) self->actionFlags |= kActfStandUp;
}

// ── 0x1CC130 ──────────────────────────────────────────────────────────────
int CharaBody_Start(CharaBody* self, s32 action, s32 start, s32 blend) {
    self->flags &= 0xFFFFFF1Fu;
    self->actionFlags &= 0xF0000000u;
    self->state &= 0xF0000000u;
    if (self->scriptAction != action) {
        self->scriptAction = self->scriptLoops = 0;
        self->scriptStart = self->scriptEnd = 0;
        self->scriptStill = 0;
        self->scriptBlend = 0;
    }
    self->_2c = 0;
    Bone* bones = P<Bone>(Bones(self)->bones);
    Matrix_Copy(bones[28].local, self->handL);
    Matrix_Copy(bones[29].local, self->handR);
    self->action = action;

    CharaResources* res = CharaResources_Get();
    const u32 motion = CharaResources_MotionOf(res, CharaBody_MotionFor(self, action));
    const u32 def = CharaResources_ActionOf(res, CharaBody_MotionFor(self, action));
    if (motion == 0 || def == 0)
        return 0;                   // (the original prints a complaint)
    self->motionId = CharaBody_MotionFor(self, action);
    self->actionDef = def;
    ApplyActionFlags(self, P<ActionDef>(*P<u32>(def))->flags);

    // A blow chosen for this action becomes the blow in progress.
    u32 node = self->comboNext;
    if (node != 0 && *P<s32>(node) != self->action)
        node = 0;
    self->comboNode = node;
    self->comboNext = 0;

    alignas(16) float matrix[16];
    Matrix_Copy(matrix, Params(self)->matrix);
    MotionPlayer_Play((float)start, (float)blend, Player(self), Bones(self), matrix, motion);
    Matrix_Copy(Params(self)->matrix, matrix);
    self->clock++;
    self->clockFrame = *gFrameCount;
    MotionPlayer_AddFlags(Player(self), kMotionAdvance | kMotionRoot);
    return 1;
}

// ── 0x1CC460 ──────────────────────────────────────────────────────────────
int CharaBody_Play(CharaBody* self, s32 action) {
    const ActionDef* def = DefFor(self, action);
    if (!def)
        return 0;
    const ActionFrames* frames = P<const ActionFrames>(def->frames);
    CharaBody_Start(self, action, frames->start, frames->blend);
    return 1;
}

// ── 0x1CC360 ──────────────────────────────────────────────────────────────
int CharaBody_Resume(CharaBody* self, s32 action) {
    const ActionDef* def = DefFor(self, action);
    const u32 holder = CharaResources_MotionOf(CharaResources_Get(), CharaBody_MotionFor(self, action));
    const u32 motion = holder ? *P<u32>(holder) : 0;
    if (!def || motion == 0)
        return 0;
    CharaBody_Start(self, action, *P<s16>(motion + 4), P<const ActionFrames>(def->frames)->blend);
    return 1;
}

// ── 0x1CA750 ──────────────────────────────────────────────────────────────
// An action that follows on from the current one at this frame.
static int TryLink(CharaBody* self, s32 action) {
    if (self->actionFlags & kActfFrozen)
        return 0;
    const ActionDef* def = DefFor(self, action);
    if (!def || def->links == 0)
        return 0;
    const int frame = Frame(self);
    const int count = ActionDef_LinkCount(def);
    for (int i = 0; i < count; i++) {
        const ActionLink* link = ActionDef_Link(def, i);
        if (frame < link->first || frame > link->first + link->length || self->action != link->from)
            continue;
        if (def->flags & 0x1000) {
            // Not on someone who is already down.
            const u32 victim = Params(self)->_d8[0];
            if (victim != 0) {
                const CharaParams* v = P<CharaParams>(victim);
                if (v->child != 0)
                    continue;
                const u32 flags = v->link[5] ? *P<u32>(v->link[5]) : 0;
                if (flags & kBodyCorpse)
                    continue;
            }
        }
        CharaBody_Start(self, action, link->start, link->blend);
        return 1;
    }
    return 0;
}

// ── 0x1CA940 ──────────────────────────────────────────────────────────────
// An action that cuts the current one short, if its record allows that kind.
static int TryCancel(CharaBody* self, s32 action) {
    if (self->actionFlags & kActfFrozen)
        return 0;
    if (action == self->action)
        return 0;
    const ActionDef* def = DefFor(self, action);
    if (!def)
        return 0;
    const ActionDef* current = Def(self);
    if (!current)
        return 0;
    if ((current->flags & kActAnyTime) == 0) {
        const u32 kinds = def->flags;
        if (current->cancel == 0) {
            bool ok = false;
            if ((self->actionFlags & kActfAirborne) && (kinds & 2))
                ok = true;
            else if ((self->actionFlags & kActfAttack) && self->_2c == 0 &&
                     (kinds == 4 || (kinds & 2) || (kinds & 0x40000)))
                ok = true;
            if (!ok)
                return 0;
        } else {
            const s32* cancel = P<s32>(current->cancel);
            if (((u32)cancel[0] & kinds) == 0)
                return 0;
            if (Frame(self) < cancel[1])
                return 0;
        }
    }
    const ActionFrames* frames = P<const ActionFrames>(def->frames);
    CharaBody_Start(self, action, frames->start, frames->blend);
    return 1;
}

// 0x1CA750 / 0x1CA590 for the fight AI, which follows a combo by itself.
int CharaBody_TryLink(CharaBody* self, s32 action) { return TryLink(self, action); }

// ── 0x1CC500 ──────────────────────────────────────────────────────────────
int CharaBody_Try(CharaBody* self, s32 action) {
    return TryLink(self, action) || TryCancel(self, action);
}

// ── Sword-style combos ────────────────────────────────────────────────────
// A node: {s32 action, s32 direction, u8 needs[4], s32 count, node*[] next}.
struct ComboNode { s32 action, direction; u8 needs[4]; s32 count; u32 next; };

// 0x1CA2F0: what a blow needs — techniques learnt, or a state.
static bool ComboAllowed(CharaBody* self, const ComboNode* node) {
    bool ok = true;
    for (int i = 0; i < 4; i++) {
        const u32 need = node->needs[i];
        if (need == 0xFF)
            return ok;
        if (need == 0xFC) {
            if ((self->state & kStLanded) == 0) ok = false;
        } else if (need == 0xFE) {
            if ((self->state & kStHitA) == 0) ok = false;
        } else if (need == 0xFD) {
            if ((self->state & (kStHitA | kStHitB)) == 0) ok = false;
        } else {
            const s8 known = (s32)need < *P<s32>(self->techniqueCount) ? (s8)P<u8>(self->techniques)[need] : -1;
            if (known != 1) ok = false;
        }
    }
    return ok;
}
// 0x1CA210 / 0x1CA590 / 0x1CA670: the first blow of a table for a direction.
static s32 ComboPick(CharaBody* self, u32 table, s8 direction) {
    if (table == 0)
        return -1;
    const ComboNode* parent = P<ComboNode>(table);
    for (s32 i = 0; i < parent->count; i++) {
        const u32 at = P<u32>(parent->next)[i];
        const ComboNode* node = P<ComboNode>(at);
        if (ComboAllowed(self, node) && (node->direction == 0 || node->direction == direction)) {
            self->comboNext = at;
            return node->action;
        }
    }
    return -1;
}
s32 CharaBody_ComboNext(CharaBody* self, u8 code) { return ComboPick(self, self->comboNode, (s8)code); }   // 0x1CA590

// 0x1CA510: does the blow in progress have a follow-up for landing?
static int ComboHasLanding(CharaBody* self) {
    if (self->comboNode == 0)
        return 0;
    const ComboNode* parent = P<ComboNode>(self->comboNode);
    for (s32 i = 0; i < parent->count; i++) {
        const ComboNode* node = P<ComboNode>(P<u32>(parent->next)[i]);
        for (int k = 0; k < 4; k++) {
            if (node->needs[k] == 0xFF)
                break;
            if (node->needs[k] == 0xFC)
                return 1;
        }
    }
    return 0;
}
// 0x1CA3F0: follow-ups that need no button.
static int ComboAuto(CharaBody* self) {
    if ((self->actionFlags & kActfCombo) == 0 || self->comboNode == 0)
        return 0;
    const ComboNode* parent = P<ComboNode>(self->comboNode);
    for (s32 i = 0; i < parent->count; i++) {
        const u32 at = P<u32>(parent->next)[i];
        const ComboNode* node = P<ComboNode>(at);
        if (node->direction != 0 || !ComboAllowed(self, node))
            continue;
        self->comboNext = at;
        if (TryLink(self, node->action) || TryCancel(self, node->action))
            return 1;
    }
    return 0;
}

// ── The stick's history (0x1C9E60–0x1CA130) ───────────────────────────────
void CharaBody_PushInput(CharaBody* self, u32 direction, int count) {
    for (int i = 0; i < count; i++) {
        self->inputs[self->inputAt] = direction;
        self->inputAt = (self->inputAt + 1) & 0xF;
    }
    self->inputCount += (u32)count;
    if (self->inputCount > 0x10)
        self->inputCount = 0x10;
}
static u32 LastInput(const CharaBody* self) {
    return self->inputCount != 0 ? self->inputs[(self->inputAt - 1) & 0xF] : 0;
}
// 0x1C9E60: how fresh the push was — 2 within four frames, 1 within eight.
static void RateInput(CharaBody* self) {
    const u32 last = LastInput(self);
    s32 run = -1;
    for (u32 i = 0; i < self->inputCount; i++) {
        const u32 v = self->inputs[(self->inputAt - 1 - i) & 0xF];
        if (v == last) {
            run = (s32)i;
        } else if (v != 0) {
            run = -1;
            break;
        }
    }
    self->inputRating = (run == -1 || run > 7) ? 0 : run < 4 ? 2 : 1;
}
u8 CharaBody_Parries(CharaBody* self, u32 attackFlags) {
    if ((self->actionFlags & kActfFrozen) == 0 || (self->state & kStParrying) || (self->actionFlags & kActfAttack) == 0 ||
        (attackFlags & kActfFrozen) == 0)
        return 0;
    if (LastInput(self) == 0)
        return 0;
    self->state |= kStParrying;
    if ((attackFlags & LastInput(self)) == 0)
        return 0;
    RateInput(self);
    self->inputCount = 0;
    return 1;
}

// ── 0x1C9E40 ──────────────────────────────────────────────────────────────
void CharaBody_Freeze(CharaBody* self) {
    self->hitStop = 2;
    self->actionFlags |= kActfFrozen;
}

// ── 0x1C9960 ──────────────────────────────────────────────────────────────
// Thrown by a blow: up and back (0x38), up and forward (0x39), or straight up.
void CharaBody_Knock(CharaBody* self, s32 hurt, s32 strength) {
    const u32 flags = Params(self)->link[5] ? *P<u32>(Params(self)->link[5]) : 0;
    const float lift = (float)(strength % 10 + 1);
    if ((flags & kBodyInAir) == 0) {
        if (hurt == 0x38 || hurt == 0x39) {
            const float push[4] = {0.0f, 0.0f, hurt == 0x38 ? 0.015625f : -0.015625f, 0.0f};
            float v[4];
            Apply(v, Params(self)->matrix, push);
            v[1] = lift * 0.03125f;
            self->flags = (self->flags & ~0xFu) | kBodyInAir;
            Copy(self->velocity, v);
            self->state |= kStFloaty;
        }
        if (hurt == 0x3A) {
            const float push[4] = {0.0f, 0.0f, lift * 0.015625f, 0.0f};
            float v[4];
            Apply(v, Params(self)->matrix, push);
            v[1] = 0.125f;
            self->flags = (self->flags & ~0xFu) | kBodyInAir;
            Copy(self->velocity, v);
        }
        return;
    }
    // Already in the air: juggled.
    self->state |= kStFloaty;
    if (hurt == 0x3A || hurt == 0x3B)
        return;
    self->velocity[0] *= 0.9375f;
    self->velocity[1] = lift * 0.03125f * 0.53125f;
    self->velocity[2] *= 0.9375f;
    u32 r = (u32)Rand_Next();
    self->spin[0] = (float)(r & 0x3F) * 0.0025f + 0.03175f;
    if (r & 1) self->spin[0] = -self->spin[0];
    r = (u32)Rand_Next();
    self->spin[1] = (float)(r & 0x3F) * 0.005f + 0.125f;
    if (r & 1) self->spin[1] = -self->spin[1];
}

// ── 0x1C9D60 / 0x1C9DC0 / 0x1C9DF0 ────────────────────────────────────────
int CharaBody_StartRoll(CharaBody* self) {
    const ActionDef* def = Def(self);
    if (!def || (def->flags & kActAnyTime) == 0)
        return 0;
    self->state |= 0x1100000;
    return 1;
}
int CharaBody_IsLyingAction(const CharaBody* self) { return self->action == 0x43 || self->action == 0x44; }
int CharaBody_CanRoll(const CharaBody* self) {
    return (self->action == 0x3F || self->action == 0x40) && P<MotionPlayer>(self->player)->lastFrame < 3.0f;
}

// ── What the record says about this frame (0x1CAB30–0x1CAE30) ─────────────
int CharaBody_Busy(CharaBody* self) { return (self->state & kStBusy) != 0; }
// (Where the record has no attack part the original reads through a null
// pointer, which on the PS2 is harmless; here those cases answer "no".)
int CharaBody_CanGuard(CharaBody* self) {
    if (!Def(self)) return 1;
    const ActionAttack* a = Attack(Def(self));
    return a && Frame(self) >= a->guardFrom && Frame(self) <= a->guardTo;
}
int CharaBody_AttackStrength(CharaBody* self) { const ActionAttack* a = Attack(Def(self)); return a ? a->strength : -1; }
int CharaBody_AttackKind(CharaBody* self)     { const ActionAttack* a = Attack(Def(self)); return a ? a->kind : -1; }
int CharaBody_CanHit(CharaBody* self) {
    const ActionAttack* a = Attack(Def(self));
    return a && Frame(self) >= a->hitFrom && Frame(self) <= a->hitTo;
}
int CharaBody_AtHitFrame(CharaBody* self) {
    const ActionAttack* a = Attack(Def(self));
    return a && a->hitFrom == Frame(self);
}
int CharaBody_IsAttack(CharaBody* self) { const ActionDef* d = Def(self); return d ? d->attack != 0 : 0; }
int CharaBody_VoiceNow(CharaBody* self) { const ActionDef* d = Def(self); return d ? ActionDef_VoiceAt(d, Frame(self)) : -1; }
int CharaBody_SoundNow(CharaBody* self) { const ActionDef* d = Def(self); return d ? ActionDef_SoundAt(d, Frame(self)) : -1; }

// ── A motion for a task (0x1CAE80–0x1CB0E0) ───────────────────────────────
u8 CharaBody_fn1cae80(CharaBody* body) { return body->scriptLoops == 0; }
void CharaBody_StopScript(CharaBody* self) {
    self->scriptLoops = 0;
    CharaBody_Settle(self);
}
static void ClearScript(CharaBody* self) {
    self->scriptAction = self->scriptLoops = 0;
    self->scriptStart = self->scriptEnd = 0;
    self->scriptStill = 0;
    self->scriptBlend = 0;
}
// 0x1CAEA0
static void StepScript(CharaBody* self) {
    if (self->scriptStill)
        Zero(self->travel);
    if (self->scriptEnd > (int)Player(self)->frame)
        return;
    if (self->scriptLoops != -1)
        self->scriptLoops--;
    if (self->scriptLoops == 0) {
        CharaBody_Settle(self);
        return;
    }
    const s16 loop = MotionPlayer_LoopStart(Player(self));
    MotionPlayer_SetFrame(loop == -1 ? (float)self->scriptStart : (float)loop, Player(self), 1);
    MotionPlayer_SetBlend(0.0f, Player(self));
}
void CharaBody_PlayScripted(CharaBody* self, s32 action, u8 still, s32 loops, s32 start, s32 end, s32 blend) {
    const ActionDef* def = DefFor(self, action);
    if (!def) {
        ClearScript(self);          // (the original prints "no such motion")
        return;
    }
    const ActionFrames* frames = P<const ActionFrames>(def->frames);
    self->scriptAction = action;
    self->scriptStill = still;
    self->scriptLoops = loops;
    self->scriptStart = start == -1 ? frames->start : start;
    self->scriptEnd = end == -1 ? frames->end : end;
    self->scriptBlend = blend == -1 ? frames->blend : blend;
    CharaBody_Start(self, self->scriptAction, self->scriptStart, self->scriptBlend);
    MotionPlayer_SetFlags(Player(self), kMotionAdvance | kMotionRoot);
}
void CharaBody_PlayScript(CharaBody* self, const s32* r) {
    // (0x1CAF90 stores the request first and then does the same.)
    CharaBody_PlayScripted(self, r[0], (u8)r[5], r[1], r[3], r[4], r[2]);
}

// ── 0x1CB260 ──────────────────────────────────────────────────────────────
void CharaBody_BeginFrame(CharaBody* self) {
    self->flags &= 0xFF00FFFFu;
    self->state &= 0xFE0FFFFFu;
    self->targetFlags &= 0xFEFFFFFFu;
    Copy(self->lastPosition, Position(self));
}

// ── 0x1CB600 ──────────────────────────────────────────────────────────────
// The frames of the draw / sheathe motions at which the sword changes hands.
void CharaBody_fn1cb600(CharaBody* body) {
    const int frame = Frame(body);
    switch (body->action) {
    case 0xC:
        if (frame == 6) body->state |= 0x1200000;
        if (frame == 0x10 && (body->flags & kBodyCarrying)) body->state |= 0x700000;
        break;
    case 0x8F:
        if (frame == 6) body->state |= 0x1200000;
        break;
    case 0xD:
        if (frame == 10) body->state |= (body->flags & kBodyCarrying) ? 0x400000 : 0x1200000;
        break;
    case 0xE:
        if (frame == 0x12) body->state |= 0x500000;
        break;
    case 0x10:
        if (frame > 0xD && frame < 0x13) body->state |= 0x800000;
        break;
    }
}

// ── Movement (0x1CB740–0x1CBF90) ──────────────────────────────────────────
// 0x1CB8A0: face the opponent while locked on.
static void TurnToOpponent(CharaBody* self) {
    if ((self->targetFlags & 0x1000000) == 0)
        return;
    const ActionDef* def = Def(self);
    if (!def)
        return;
    const u32 opponent = Params(self)->link[3];
    if (def->attack == 0) {
        if ((self->actionFlags & 0xFA000) == 0 || opponent == 0)
            return;
    } else {
        const ActionAttack* a = P<ActionAttack>(def->attack);
        if (Frame(self) < a->turnFrom || Frame(self) > a->turnTo || opponent == 0)
            return;
    }
    alignas(16) float m[16];
    Matrix_Copy(m, Params(self)->matrix);
    Matrix_TurnTowards(10.0f, m, &P<CharaParams>(opponent)->matrix[12]);
    Matrix_Copy(Params(self)->matrix, m);
}
// 0x1CB740: step in on the opponent during the frames a blow allows it.
static void CloseIn(CharaBody* self) {
    if ((self->targetFlags & 0x1000000) == 0)
        return;
    const ActionAttack* a = Attack(Def(self));
    if (!a || Frame(self) < a->stepFrom || Frame(self) > a->stepTo)
        return;
    const u32 opponent = Params(self)->link[3];
    if (opponent == 0)
        return;
    float* pos = Position(self);
    const float* there = &P<CharaParams>(opponent)->matrix[12];
    float to[4] = {there[0] - pos[0], there[1] - pos[1], there[2] - pos[2], 0.0f};
    Normalize(to, to);
    float facing[4];
    Normalize(facing, &Params(self)->matrix[8]);
    if (Dot(to, facing) > 0.0f)
        for (int i = 0; i < 3; i++)
            pos[i] += to[i] * 0.0625f;
}
// 0x1CBC00: walking and running go where the control points, turning the
// character that way a little each frame (π/15 walking, π/10 running).
static int Steer(CharaBody* self) {
    if ((self->actionFlags & (kActfRun | kActfWalk)) == 0)
        return 0;
    const float* dir = self->input;
    if (dir[0] == 0.0f && dir[2] == 0.0f)
        return 0;
    if ((self->targetFlags & 0x1000000) == 0) {
        static const float kRate[2] = {15.0f, 10.0f};      // 0x2EDCD0
        alignas(16) float m[16];
        Matrix_Copy(m, Params(self)->matrix);
        if (Dot(&m[8], dir) >= 0.9781476f) {
            m[8] = dir[0];  m[10] = dir[2];
            m[0] = dir[2];  m[2] = -dir[0];
            Matrix_Copy(Params(self)->matrix, m);
            return 0;
        }
        float pos[4];
        Copy(pos, &m[12]);
        UnitW(&m[12]);
        const float step = 3.1415927f / kRate[(self->actionFlags & kActfWalk) == 0];
        Matrix_Rotate(m, m, m[8] * dir[2] - m[10] * dir[0] < 0.0f ? step : -step);
        Copy(&m[12], pos);
        Matrix_Copy(Params(self)->matrix, m);
    }
    float t[4];
    Copy(t, self->travel);
    t[1] = 0.0f;
    const float length = Normalize(t, t);
    t[0] = dir[0] * length;
    t[2] = dir[2] * length;
    Copy(self->travel, t);
    float* pos = Position(self);
    pos[0] += t[0]; pos[1] += t[1]; pos[2] += t[2];
    pos[3] = 1.0f;
    return 1;
}
// 0x1CBA70: a little air control, and the tumble of a thrown body.
static void Drift(CharaBody* self) {
    if (self->actionFlags & kActfAirborne) {
        float n[4];
        Normalize(n, self->input);
        self->velocity[0] += n[0] * 0.004f;
        self->velocity[2] += n[2] * 0.004f;
    }
    if (self->action != 0x50)
        return;
    Rand_Next();
    float q[4];
    Copy(q, self->tumble);
    const float axis[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    Quat_Turn(self->spin[0], q, axis);
    Quat_ToMatrix(q, P<Bone>(Bones(self)->bones)->local);
    Copy(self->tumble, q);
    alignas(16) float m[16];
    Matrix_Copy(m, Params(self)->matrix);
    float pos[4];
    Copy(pos, &m[12]);
    Matrix_Rotate(m, m, self->spin[1]);
    Copy(&m[12], pos);
    Matrix_Copy(Params(self)->matrix, m);
    self->spin[0] *= 0.96825f;
    self->spin[1] *= 0.96825f;
}
// 0x1CBE90: where this frame's motion and velocity would put it.
static void Move(CharaBody* self) {
    TurnToOpponent(self);
    CloseIn(self);
    float t[4];
    Copy(t, self->travel);
    if (Steer(self))
        UnitW(t);
    Drift(self);
    float v[4];
    Copy(v, self->velocity);
    if (self->state & kStStopFall) {
        v[1] = 0.0f;
        self->state &= ~(u32)kStStopFall;
    }
    alignas(16) float m[16];
    Matrix_Copy(m, Params(self)->matrix);
    UnitW(&m[12]);
    float w[4];
    Apply(w, m, t);
    w[1] = 0.0f;
    const float* pos = Position(self);
    for (int i = 0; i < 3; i++)
        w[i] += v[i] + pos[i];
    w[3] = 1.0f;
    Copy(self->position, w);
}
// 0x1CBF90: gravity on the velocity (a blow in the air hangs until it lands).
static void Gravity(CharaBody* self) {
    const ActionDef* def = Def(self);
    if (!def)
        return;
    if ((self->flags & kBodyInAir) == 0) {
        Zero(self->velocity);
        return;
    }
    const ActionAttack* a = Attack(def);
    float y = self->velocity[1];
    if (a && y < 0.0f && Player(self)->lastFrame < (float)a->hitFrom) {
        self->state |= kStStopFall;
        return;
    }
    if (self->state & kStFloaty)
        y += -fabsf(y * 0.15625f) - 0.004f;
    else
        y += -fabsf(y * 0.2f) - 0.01f;
    self->velocity[1] = y;
}

// ── 0x1CC560 ──────────────────────────────────────────────────────────────
void CharaBody_Update(CharaBody* self, Skeleton* skeleton) {
    if (self->stagger != 0)
        self->stagger--;
    if (self->hitStop >= 0) {
        if (self->hitStop == 0)
            self->actionFlags &= ~(u32)kActfFrozen;
        self->hitStop--;
    }
    if (self->_2c != 0)
        self->_2c--;
    if (self->actionFlags & kActfFrozen)
        return;

    ComboAuto(self);
    MotionPlayer* player = Player(self);
    const float frame = VirtualCall<float, MotionPlayer*, Skeleton*, float*, int>(player->vtable, 0x0C, player, skeleton,
                                                                                  self->travel, 1);
    if (self->scriptLoops == 0) {
        if ((int)frame == -1 && player->lastFrame != (float)MotionPlayer_LoopStart(player)) {
            CharaBody_Settle(self);
            UnitW(self->travel);
        }
    } else {
        StepScript(self);
    }
    Gravity(self);
    Move(self);

    // Time passes while walking (three ticks a second) or running (five).
    const u32 now = *gFrameCount;
    if (self->actionFlags & kActfRun) {
        if ((u32)(*gFrameRate / 5) < now - self->clockFrame) {
            self->clock++;
            self->clockFrame = now;
        }
    } else if (self->actionFlags & kActfWalk) {
        if ((u32)(*gFrameRate / 3) < now - self->clockFrame) {
            self->clock++;
            self->clockFrame = now;
        }
    }
}

// ── 0x1CC760 ──────────────────────────────────────────────────────────────
const float* CharaBody_Moved(CharaBody* self) {
    float* out = P<float>(0x30A5D0u);
    for (int i = 0; i < 4; i++)
        out[i] = self->position[i] - self->lastPosition[i];
    return out;
}

// ═══ Requests (0x1D0ED0–0x1D2940) ═════════════════════════════════════════

// ── 0x1D28E0 ──────────────────────────────────────────────────────────────
// The action for doing nothing, as things stand.
s32 CharaBody_Rest(CharaBody* self, int now) {
    const u32 flags = self->flags;
    if (flags & kBodyDead) {
        if ((flags & (kBodyFaceUp | kBodyFaceDown)) == 0) {
            if (CharaBody_Collapse(self, now) != 0)
                return 1;
        } else if (CharaBody_Die(self, 1, now) != 0) {
            return 1;
        }
    }
    s32 action;
    if (flags & kBodyFaceUp)            action = 0x44;
    else if (flags & kBodyFaceDown)     action = 0x43;
    else if (self->flags & kBodyArmed)  action = 0x83;
    else                                action = (flags & kBodyCarrying) ? 8 : 0;
    if (!(now ? CharaBody_Play(self, action) : CharaBody_Try(self, action)))
        return -1;
    self->flags |= kBodyIdle;
    return action;
}
void CharaBody_Refresh(CharaBody* body) { CharaBody_Rest(body, 1); }       // 0x1C9770
void CharaBody_Idle(CharaBody* self)    { CharaBody_Rest(self, 0); }       // 0x1C9950

// ── 0x1D12F0 ──────────────────────────────────────────────────────────────
// Lying dead.  `fresh`: it has just gone down, and how it lies is decided now.
s32 CharaBody_Die(CharaBody* self, int fresh, int now) {
    s32 action = 0;
    if (!fresh) {
        switch (self->lying & 0xF) {
        case 1: action = 0x5D; break;
        case 2: action = 0x5E; break;
        case 3: action = 0x5F; break;
        case 4: action = 0x61; break;
        case 5: action = 0x62; break;
        }
    } else {
        self->lying &= ~0xFu;
        if (FaceDown(self)) {
            action = 0x61;
            self->lying |= 4;
        } else {
            action = 0x62;
            self->lying |= 5;
        }
    }
    if (!(now ? CharaBody_Play(self, action) : CharaBody_Try(self, action)))
        return -1;
    self->state |= kStBusy;
    if ((self->flags & kBodyCorpse) == 0) {
        self->flags |= kBodyCorpse;
        const float up[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        float at[4];
        Copy(at, Position(self));
        at[1] += 0.01f;
        Fx_BloodPool(at, up);           // (made in place in the original: class 0x187A00)
    }
    return 1;
}

// ── 0x1D1540 ──────────────────────────────────────────────────────────────
// Dying on its feet: one of three ways down.
s32 CharaBody_Collapse(CharaBody* self, int now) {
    static const s32 kAction[3] = {0x59, 0x5A, 0x5B};      // 0x2D88D0
    static const u32 kLying[3] = {1, 2, 3};                // 0x2D88E0
    const int pick = Rand_Next() % 3;
    if (!(now ? CharaBody_Play(self, kAction[pick]) : CharaBody_Try(self, kAction[pick])))
        return -1;
    self->lying = (self->lying & ~0xFu) | kLying[pick];
    self->stagger = 5;
    self->actionFlags |= kActfDying;
    self->state |= kStBusy;
    return 1;
}

// ── 0x1D1110 ──────────────────────────────────────────────────────────────
static int SettleDown(CharaBody* self) {
    if (self->flags & kBodyDead)
        return CharaBody_Die(self, 1, 1) != 0;
    self->flags = (self->flags & ~0xFu) | (FaceDown(self) ? kBodyFaceDown : kBodyFaceUp);
    return CharaBody_Rest(self, 1) != 0;
}

// ── 0x1D1F60 ──────────────────────────────────────────────────────────────
// Into the falling action.  Stepping off an edge keeps the pace it had.
s32 CharaBody_Fall(CharaBody* self, int keepVelocity) {
    if (!keepVelocity) {
        float v[4];
        UnitW(v);
        const float* in = self->input;
        if (in[0] != 0.0f || in[2] != 0.0f) {
            float t[4];
            Copy(t, self->travel);
            t[1] = 0.0f;
            const float pace = Normalize(t, t);
            if (in[0] == 0.0f && in[1] == 0.0f && in[2] == 0.0f) {
                v[0] = Params(self)->matrix[8] * pace;
                v[2] = Params(self)->matrix[10] * pace;
            } else {
                v[0] = in[0] * pace;
                v[2] = in[2] * pace;
            }
        }
        Copy(self->velocity, v);
    }
    if (self->flags & kBodyInAir) {
        if ((self->flags & kBodyArmed) && ComboHasLanding(self))
            return -1;
        const u32 state = self->state;
        if (!CharaBody_Play(self, 6))
            return -1;
        if (state & kStAirCombo)
            self->state |= kStAirCombo;
    }
    return 6;
}

// ── 0x1D0ED0 ──────────────────────────────────────────────────────────────
// The action has run out: what follows from the state it leaves.
s32 CharaBody_Settle(CharaBody* self) {
    s32 result = 0;
    if (self->_40 != 0) {
        self->_40 = 0;
        self->_3c = 1;
    }
    if (self->flags & kBodyInAir) {
        if ((self->actionFlags & kActfLying) == 0)
            result = CharaBody_Fall(self, 1);
        return result;
    }
    const u32 state = self->state;
    if (state & kStToggleSword) {
        // The draw (0x2E) and sheathe (0x30) motions end by changing stance.
        s32 action;
        if (self->flags & kBodyArmed) {
            action = 0x31;
            self->flags &= ~(u32)kBodyArmed;
        } else {
            action = 0x2F;
            self->flags |= kBodyArmed;
        }
        if (!CharaBody_Play(self, action))
            return -1;
        self->state |= kStBusy;
    } else if (self->actionFlags & kActfAttack) {
        const u32 owner = Params(self)->link[0];
        if ((owner ? *P<s32>(owner) : -1) != 0x90)
            CharaBody_Resume(self, 0x90);
    } else if (state & kStGetUpA) {
        SettleDown(self);
    } else if (state & kStGetUpB) {
        const bool down = FaceDown(self);
        self->flags = (self->flags & ~0xFu) | (down ? kBodyFaceDown : kBodyFaceUp);
        CharaBody_Play(self, down ? 0x3F : 0x40);
    } else if ((self->actionFlags & kActfDying) || (self->flags & kBodyCorpse)) {
        CharaBody_Die(self, 0, 1);
    } else {
        CharaBody_Rest(self, 1);
    }
    return result;
}

// ── 0x1D1D30 ──────────────────────────────────────────────────────────────
// Back on the ground.
s32 CharaBody_Land(CharaBody* self) {
    self->flags &= ~0xFu;
    if (self->actionFlags & kActfLying) {
        const bool down = FaceDown(self);
        self->flags = (self->flags & ~0xFu) | (down ? kBodyFaceDown : kBodyFaceUp);
        return CharaBody_Play(self, down ? 0x3F : 0x40) ? 0 : 1;
    }
    const s32 action = (self->flags & kBodyArmed) ? 0x8E : 5;
    self->flags |= kBodyStanding;
    bool moving = false;
    if ((self->state & kStAirCombo) == 0) {
        const float* in = self->input;
        moving = in[0] != 0.0f || in[1] != 0.0f || in[2] != 0.0f;
    }
    if (self->actionFlags & kActfCombo) {
        self->state |= kStLanded;
        if (self->comboNode != 0) {
            const s32 next = ComboPick(self, self->comboNode, 0);
            if (next != -1)
                return CharaBody_Play(self, next) ? next : -1;
        }
    }
    if (moving) {
        CharaBody_Settle(self);
        return action;
    }
    if (!CharaBody_Play(self, action))
        return -1;
    self->state |= kStBusy;
    return action;
}

// ── 0x1D16A0 ──────────────────────────────────────────────────────────────
// Hit: the reaction for a kind of blow (bit 28: from behind).
s32 CharaBody_Hurt(CharaBody* self, u32 kind) {
    static const s32* const kReact = guest::ptr<const s32>(0x2D8680u);    // [kind][armed × 2 + behind]
    static const s32* const kThrown = guest::ptr<const s32>(0x2D87A0u);   // kinds 0x18 on
    if (kind == 0xFFFFFFFFu)
        return 0;
    s32 action = -1;
    const u32 flags = self->flags;
    if (flags & kBodyCorpse) {
        switch (self->lying & 0xF) {
        case 1: action = 0x63; break;
        case 2: action = 0x64; break;
        case 3: action = 0x65; break;
        case 4: action = 0x67; break;
        case 5: action = 0x68; break;
        }
    } else {
        const int behind = (kind & 0x10000000u) != 0;
        const u32 which = kind & 0x0FFFFFFFu;
        if (which >= 0x18) {
            action = kThrown[which];
        } else if (flags & (kBodyFaceUp | kBodyFaceDown)) {
            action = (flags & kBodyFaceDown) ? 0x47 : 0x48;
        } else if (flags & kBodyInAir) {
            const u32 owner = Params(self)->link[0];
            if ((owner ? *P<s32>(owner) : -1) != 0x50)
                Copy(self->tumble, P<Bone>(P<Skeleton>(Params(self)->link[1])->bones)->rotation);
            action = 0x50;
        } else if (flags & kBodyDead) {
            CharaBody_Collapse(self, 1);
            return 1;
        } else {
            action = kReact[which * 4 + ((self->flags & kBodyArmed) ? 2 : 0) + behind];
        }
    }
    if (!CharaBody_Play(self, action))
        return -1;
    self->stagger = 5;
    self->state |= 0x600004;
    return action;
}

// ── Reactions and moves (0x1D1200–0x1D2830) ───────────────────────────────
int CharaBody_fn1d1200(CharaBody* self) {
    if (!CharaBody_Play(self, 0x51)) return 0;
    self->actionFlags |= kActfF40;
    return 1;
}
int CharaBody_Parry(CharaBody* self) {
    if (!CharaBody_Play(self, 0x93)) return 0;
    self->actionFlags |= kActfF20;
    return 1;
}
int CharaBody_GuardBreak(CharaBody* self) {
    if (!CharaBody_Play(self, 0x53)) return 0;
    self->state |= kStBusy;
    self->actionFlags |= kActfF80;
    return 1;
}
int CharaBody_Clash(CharaBody* self, int heavy) {
    static const s32 kAction[2] = {0x91, 0x92};            // 0x2EDCF0
    return CharaBody_Play(self, kAction[heavy]);
}
int CharaBody_Bumped(CharaBody* self) {
    if (!CharaBody_Play(self, 0x54)) return -1;
    self->state |= kStBusy;
    return 1;
}
int CharaBody_fn1d1b20(CharaBody* self, int which) {
    static const s32 kAction[2] = {0x3AA, 0x3AB};          // 0x2EDCE8
    if (!CharaBody_Try(self, kAction[which])) return 0;
    self->state |= 0x20000;
    return 1;
}
s32 CharaBody_fn1d2110(CharaBody* self) {
    if (!CharaBody_Try(self, 0x9F)) return -1;
    self->actionFlags |= kActfF10;
    return 0x9F;
}
int CharaBody_fn1d1be0(CharaBody* self) {
    if (!CharaBody_Try(self, 0xD)) return 0;
    self->state |= kStBusy;
    return 1;
}
int CharaBody_fn1d1c30(CharaBody* self) {
    if (!CharaBody_Try(self, (self->flags & kBodyArmed) ? 0x8F : 0xC)) return 0;
    self->state |= kStBusy;
    return 1;
}
// Getting up.
int CharaBody_Rise(CharaBody* self) {
    if (self->flags & kBodyDead)
        return 0;
    if (!CharaBody_Play(self, 0x4F))
        return 0;
    self->flags = (self->flags & ~0xFu) | kBodyStanding;
    self->state |= kStBusy;
    self->stagger = 10;
    return 1;
}
int CharaBody_fn1d1960(CharaBody* self) {
    if (!CharaBody_Play(self, 0x9E)) return 0;
    self->flags = (self->flags & ~0xFu) | kBodyStanding;
    return 1;
}
int CharaBody_RollUp(CharaBody* self) {
    if (!CharaBody_Play(self, (self->flags & 0xF) == kBodyFaceDown ? 0x4B : 0x4C)) return 0;
    self->flags = (self->flags & ~0xFu) | kBodyStanding;
    self->stagger = 0x14;
    return 1;
}
int CharaBody_Guard(CharaBody* self, int resume) {
    const u32 owner = Params(self)->link[0];
    if ((owner ? *P<s32>(owner) : -1) != 0x90)
        return resume ? CharaBody_Resume(self, 0x90) : CharaBody_Try(self, 0x90);
    return 1;
}
int CharaBody_GuardEnd(CharaBody* self) {
    const u32 owner = Params(self)->link[0];
    if ((owner ? *P<s32>(owner) : -1) != 0x90)
        return 0;
    return CharaBody_Rest(self, 1) == 0;
}
void CharaBody_fn1d1c90(CharaBody* body) {
    if (CharaBody_Try(body, 0x30))
        body->state |= kStBusy | kStToggleSword;
}
void CharaBody_fn1d1ce0(CharaBody* body) {
    if (CharaBody_Try(body, 0x2E))
        body->state |= 0x600000 | kStBusy | kStToggleSword;
}
int CharaBody_fn1d2160(CharaBody* self, int which) {
    if ((int)Player(self)->frame < 5)
        return -1;
    if (!CharaBody_Play(self, 0x380))
        return -1;
    if (which == 1)
        self->_3c = 1;
    return 0x380;
}
int CharaBody_fn1d21f0(CharaBody* self, int which) {
    static const s32 kAction[2] = {0x10, 0xE};             // 0x2EDCE0
    return CharaBody_Try(self, kAction[which]) ? kAction[which] : -1;
}
int CharaBody_Kick(CharaBody* self, int which) {
    static const s32 kAction[4] = {0x27, 0x28, 0x29, 0x29};        // 0x2D88C0
    return CharaBody_Try(self, kAction[which]) ? kAction[which] : -1;
}

// ── 0x1D22B0 ──────────────────────────────────────────────────────────────
// A blow: the next of the combo in progress, or the first of the style's for
// this direction (high nibble the button, low the stick); failing that the
// same without the stick.
s32 CharaBody_Attack(CharaBody* self, u8 direction) {
    for (int attempt = 0; attempt < 2; attempt++) {
        s32 action = ComboPick(self, self->comboNode, (s8)direction);
        if (action != -1 && TryLink(self, action))
            return action;
        action = ComboPick(self, self->comboRoot, (s8)direction);
        if (action != -1 && TryCancel(self, action))
            return action;
        if (((s8)direction | 0xF0) == 0xF0 || (direction & 8))
            return -1;
        direction &= 0xF0;
    }
    return -1;
}

// ── 0x1D23B0 ──────────────────────────────────────────────────────────────
// A jump, in the direction asked: three quarters of the pace straight ahead,
// half to the side or back.
int CharaBody_Jump(CharaBody* self) {
    const float dirX = self->input[0], dirZ = self->input[2];
    if (!CharaBody_Try(self, (self->flags & kBodyArmed) ? 0x8D : 4))
        return -1;
    float v[4], t[4];
    Copy(v, self->velocity);
    Copy(t, self->travel);
    t[1] = 0.0f;
    const float pace = Normalize(t, t);
    t[0] = dirX * pace;
    t[2] = dirZ * pace;
    Copy(self->travel, t);
    float flat[4] = {t[0], 0.0f, t[2], t[3]}, n[4];
    Normalize(n, flat);
    const float share = Dot(&Params(self)->matrix[8], n) > 0.9945219f ? 0.75f : 0.5f;
    v[0] = flat[0] * share;
    v[2] = flat[2] * share;
    Copy(self->velocity, v);
    return 1;
}

// ── 0x1D2540 / 0x1D26B0 ───────────────────────────────────────────────────
// A dodge step: forward, back, left or right of where it faces.
static const s32 kDodgeAction[4] = {0x85, 0x86, 0x88, 0x87};       // 0x2D8880 / 0x2D88A0
static const u32 kDodgeFlag[4] = {0x100000, 0x200000, 0x300000, 0x400000};
s32 CharaBody_DodgeTo(CharaBody* self, int side) {
    if (!self || self->action == kDodgeAction[side])
        return -1;
    if (!CharaBody_Try(self, kDodgeAction[side]))
        return -1;
    self->actionFlags |= kDodgeFlag[side];
    return 0;
}
s32 CharaBody_Dodge(CharaBody* self, const float* direction) {
    float d[4];
    Normalize(d, direction);
    Copy(self->input, d);
    const float* facing = &Params(self)->matrix[8];
    const float along = Dot(facing, d);
    int side = 0;
    if (along <= 0.75f) {
        side = 1;
        if (along >= -0.75f)
            side = d[0] * facing[2] - d[2] * facing[0] <= 0.0f ? 3 : 2;
    }
    if (!self || self->action == kDodgeAction[side])
        return -1;
    if (!CharaBody_Try(self, kDodgeAction[side]))
        return -1;
    self->actionFlags |= kDodgeFlag[side];
    return kDodgeAction[side];
}

// ── 0x1D2770 ──────────────────────────────────────────────────────────────
s32 CharaBody_Special(CharaBody* self, int which) {
    static const s32 kAction[4] = {0x89, 0x8A, 0x8C, 0x8B};        // 0x2D8870
    if (CharaBody_Try(self, kAction[which]))
        self->state |= kStBusy;
    return -1;
}

// ── 0x1D2830 / 0x1D27E0 ───────────────────────────────────────────────────
// Walk (1) or run (3); armed and carrying have one pace each (0x84, 9).
int CharaBody_Walk(CharaBody* self, int run) {
    s32 action = -1;
    if ((self->flags & 0xF) == kBodyStanding) {
        if (self->flags & kBodyArmed)           action = 0x84;
        else if (self->flags & kBodyCarrying)   action = 9;
        else                                    action = run ? 3 : 1;
    }
    return CharaBody_Try(self, action) ? action : -1;
}
s32 CharaBody_WalkTo(CharaBody* self, s32 run, const float* direction) {
    CharaBody_Walk(self, run);
    Copy(self->input, direction);
    return run;
}
