#pragma once
#include "engine/containers.h"
#include "engine/motion.h"
#include "game/chara_resources.h"
#include "hal/ps2_types.h"

// One character in the world (0x1C6C50–0x1D0ED0 and 0x269030–0x269C00; 0x200
// bytes for the player's class, 0x210 for everyone else's).
//
// A character is made of
//   - its live parameters (CharaParams: transform, health, links back here),
//   - a *body* (CharaBody): what it is doing — one action at a time, each a
//     motion with a record of what may interrupt it — and where that moves it,
//   - a *control* (CharaControl): who decides what it does next, the pad or
//     an AI routine,
//   - smaller parts (sword holder, voice, effects…) that are still stand-ins.
struct Chara {
    s32   bodyType;         // +0x000  1: one skinned model (the heroes); 0: a body and a head
    u32   textureName;      // +0x004  name hashes of its files…
    u32   bodyName[3];      // +0x008  …by level of detail
    u32   skinBody[3];      // +0x014  models (type 1)
    u32   textures;         // +0x020
    u32   body0[3];         // +0x024  models (type 0)
    u32   _30, _34;
    u32   headName[3];      // +0x038
    u32   head[3];          // +0x044
    u32   _50, _54[3];
    s32   _60;              // +0x060  1
    s32   frameRate;        // +0x064
    s32   _68, _6c, _70;
    s32   partner;          // +0x074  id of the character it is paired with, −1
    s32   active;           // +0x078  takes part in the frame
    s32   indoors;          // +0x07C  1: belongs to an interior…
    s32   interior;         // +0x080  …this one (−1 = any)
    s32   id;               // +0x084  0 = the player
    s32   model;            // +0x088  id in Common/chara/list
    s32   _8c;
    s32   model2;           // +0x090
    s32   fileSize;         // +0x094
    s32   arg;              // +0x098  its motion pack
    s32   _9c;              // +0x09C  its sword's motion pack, −1
    s32   _a0;              // +0x0A0  the sword pack it had before (to unload)
    s32   swordPackReady;   // +0x0A4
    s32   packSize;         // +0x0A8
    s32   kind;             // +0x0AC  CharaParams template
    s32   _b0;
    u8    visible;          // +0x0B4  cleared when culled
    u8    _b5;              // +0x0B5  it has a point to look at (+0xE0)
    u8    _b6[2];
    s32   lookState;        // +0x0B8
    float cameraDistance;   // +0x0BC
    float aboveGround;      // +0x0C0
    float listParam[2];     // +0x0C4  the two floats of its Common/chara/list entry
    u8    _cc[0x14];
    float lookAt[4];        // +0x0E0
    s32   lightSteps;       // +0x0F0
    u8    _f4[0xC];
    float lightCell[4];     // +0x100  the light of the cell underfoot
    float lightTo[4];       // +0x110
    float light[4];         // +0x120  what it is lit with (eased towards lightTo)
    float lightStep[4];     // +0x130
    float tint[3];          // +0x140
    float _14c;
    u32   _150;             // +0x150  a copy of the collision cell it stands on (0x3C bytes):
    u32   cellFlags;        // +0x154
    s32   _158;
    s32   cellIndex;        // +0x15C
    u8    _160[0x2C];
    u32   look;             // +0x18C  head turning (0x40 bytes, 0x200ED0–0x201E90: chara_look.cpp)
    u32   voice;            // +0x190
    u32   resources;        // +0x194  CharaResources*
    u32   params;           // +0x198  CharaParams*: its live parameters (transform at +0x50)
    u32   action;           // +0x19C  CharaControl*
    u32   body;             // +0x1A0  CharaBody*
    u32   flagHandle;       // +0x1A4  its volume in the collision world; results at +0x174…
    u32   flagArg;          // +0x1A8  what that is made from (0x20 bytes of pointers into here)
    u8    _1ac[0x10];
    u32   flagHandles[2];   // +0x1BC  the sword's two volumes
    u32   flagArgs[1];      // +0x1C4
    u32   effects;          // +0x1C8
    u32   sounds;           // +0x1CC
    u32   sheath;           // +0x1D0
    u32   swords;           // +0x1D4  CharaSwords*
    u32   _1d8;
    u32   _1dc;             // +0x1DC  CharaParams* of whoever hit it last
    s32   loadPhase;        // +0x1E0
    s32   loaded;           // +0x1E4
    u32   fileBuffer;       // +0x1E8
    u32   packBuffer;       // +0x1EC  a motion pack being read
    u32   file;             // +0x1F0  ArchiveFile*
    u32   vtable;           // +0x1F4
    u8    _1f8[8];
    s32   deathReported;    // +0x200  (everyone but the player)
    u8    _204[0xC];
};

// Chara's virtual methods (offsets into the vtable).
enum : u32 {
    kCharaDestroy = 0x08,       // (self, flag)
    kCharaStepA = 0x0C,         // (self)  start of the frame
    kCharaUpdate = 0x10,        // (self)  control, body, parts
    kCharaStepMap = 0x14,       // (self, map)  keep it on the ground and out of walls
    kCharaDraw = 0x18,          // (self, visible, light)
    kCharaCull = 0x1C,          // (self, pass, float nearest) → non-zero when it is to be drawn
    kCharaSwordChanged = 0x24,  // (self) → non-zero when its sword model must be reloaded
    kCharaSwordDone = 0x28,     // (self)
    kCharaStepFlags = 0x2C,     // (self)  what the collision world found
    kCharaOnDamage = 0x30,      // (self)
    kCharaOnGuard = 0x34,       // (self)
    kCharaOnHit = 0x38,         // (self)  its own blows landing
    kCharaUnloadSword = 0x3C,   // (self) → the sword pack it no longer needs, or −1
    kCharaStepB = 0x40,         // (self)
    kCharaStepD = 0x44,         // (self)
    kCharaWearSword = 0x48,     // (self, amount)
};

// Who drives the character (Chara+0x19C; 0x10 bytes, 0x1C7B60–0x1C7E00).
struct CharaAction {
    u32 current;            // +0x00  what the routine's last step returned (non-zero: it has finished)
    s32 released;           // +0x04  1: the routine runs; 0: held still
    u32 body;               // +0x08
    s32 handle;             // +0x0C  the routine (a CharaRoutine*); tasks know theirs by this
};
typedef CharaAction CharaControl;

// A routine: +0x08 destroy(self, flag), +0x0C attach(self, body), +0x10 step(self) → result.
struct CharaRoutine {
    u32 body;               // +0x00
    s32 kind;               // +0x04  AI: which move; pad: −1
    u32 vtable;             // +0x08  0x2EB840 an AI move, 0x2EB880 the pad
};
struct AiRoutine {
    CharaRoutine base;
    u32 move;               // +0x0C  the move object: +0x08 destroy, +0x0C start, +0x10 step,
                            //        +0x14 result(self, body), +0x18 end(self, body)
    s32 started;            // +0x10
};
struct PadRoutine {
    CharaRoutine base;
    s32 port;               // +0x0C
    s32 guardHeld;          // +0x10
    u32 pad;                // +0x14  PadMgr*
    u32 commands;           // +0x18  the input buffer special moves are read from (0x410 bytes; stand-in)
};

// The acting, moving part (Chara+0x1A0; 0x1F0 bytes, 0x1C9960–0x1CC8A0 with
// the requests at 0x1D0ED0–0x1D2940).
struct CharaBody {
    s32   action;           // +0x000  what it is doing
    u32   mapFlags;         // +0x004  1: the map step may move it; 2: it left the ground over a drop
    u32   flags;            // +0x008  BodyFlag
    u32   actionFlags;      // +0x00C  ActFlag: of the current action
    u32   targetFlags;      // +0x010  BodyEvent
    u32   lying;            // +0x014  low nibble: how it lies dead (1–5)
    u32   state;            // +0x018  BodyState
    u32   clockFrame;       // +0x01C
    s32   clock;            // +0x020  time: actions begun + ticks while walking (the calendar reads it)
    s32   stagger;          // +0x024  frames
    s32   hitStop;          // +0x028  frames frozen after a blow
    s32   _2c;
    s32   swordValue;       // +0x030
    u32   _34, _38;
    s32   _3c, _40;
    u32   inputs[16];       // +0x044  recent stick directions (a ring)
    u32   inputCount;       // +0x084
    u32   inputAt;          // +0x088
    s32   inputRating;      // +0x08C
    float spin[2];          // +0x090  while tumbling through the air
    u8    _98[8];
    float tumble[4];        // +0x0A0  quaternion
    float lastPosition[4];  // +0x0B0  where it was at the start of the frame
    float takeoff[4];       // +0x0C0  where it left the ground
    float position[4];      // +0x0D0  where the motion wants it this frame
    float travel[4];        // +0x0E0  how far the motion carried it this frame (in its own space)
    float velocity[4];      // +0x0F0  on top of that: jumps, falls, knock-backs (world space)
    float _100[4];
    float input[4];         // +0x110  the direction the control asks for (world space)
    float handL[16];        // +0x120  the hand bones' rest matrices
    float handR[16];        // +0x160
    u32   packA;            // +0x1A0  u32* → action table of its motion pack {count, {action, motion}*}
    u32   packB;            // +0x1A4  …and of its sword's
    s32   motionId;         // +0x1A8
    u32   actionDef;        // +0x1AC  u32* → ActionDef of the current action
    u32   params;           // +0x1B0  CharaParams*
    u32   player;           // +0x1B4  MotionPlayer*
    u32   skeleton;         // +0x1B8  Skeleton*
    s32   scriptAction;     // +0x1BC  a motion a task asked for…
    s32   scriptLoops;      // +0x1C0  …times left to play it (−1 for ever; 0 = none in hand)
    s32   scriptBlend;      // +0x1C4
    s32   scriptStart;      // +0x1C8
    s32   scriptEnd;        // +0x1CC
    u8    scriptStill;      // +0x1D0  it does not carry the character along
    u8    _1d1[3];
    u32   techniques;       // +0x1D4  u8[]: the sword's technique states
    u32   techniqueCount;   // +0x1D8  s32*
    u32   comboRoot;        // +0x1DC  the sword style's first blows
    u32   comboNode;        // +0x1E0  the blow in progress
    u32   comboNext;        // +0x1E4  the blow just chosen
    u8    _1e8[8];
};

enum BodyFlag : u32 {
    kBodyStanding = 1, kBodyFaceUp = 2, kBodyFaceDown = 4, kBodyInAir = 8,     // (low nibble: where it is)
    kBodyCorpse = 0x10,     // dead and down: nothing more happens to it
    kBodyIdle = 0x20,
    kBodyArmed = 0x100,     // sword drawn
    kBodyDead = 0x200,
    kBodyCarrying = 0x400,
    kBodyHit = 0x10000,     // took a blow this frame (tasks: level 7)
    kBodyBumped = 0x20000,  // walked into (level 1)
    kBodyParried = 0x40000,
};
enum ActFlag : u32 {        // CharaBody::actionFlags
    kActfDying = 8, kActfF10 = 0x10, kActfF20 = 0x20, kActfF40 = 0x40, kActfF80 = 0x80,
    kActfGuard = 0x100, kActfB200 = 0x200, kActfA400 = 0x400, kActfStandUp = 0x800, kActfLying = 0x1000,
    kActfAttack = 0x2000, kActfCombo = 0x4000, kActfAirborne = 0x8000,
    kActfRun = 0x10000, kActfWalk = 0x20000, kActfDash = 0x40000,
    kActfFrozen = 0x8000000,
};
enum BodyState : u32 {
    kStToggleSword = 1, kStBusy = 4, kStAirCombo = 8, kStHitA = 0x10, kStHitB = 0x20, kStLanded = 0x40,
    kStGetUpA = 0x100, kStGetUpB = 0x200, kStStopFall = 0x1000, kStParrying = 0x2000,
    kStNoLook = 0x10000, kStFloaty = 0x40000,
};

struct CharaSwords {
    u32 _0;
    u32 sword;              // +0x04  Sword*: the one in hand
    u32 _8;
    u8  _c[0x10];
};

extern "C" {
    // ── The body (src/game/chara_body.cpp) ────────────────────────────────
    CharaBody* CharaBody_Ctor(CharaBody* self, CharaParams* params);        // 0x1CC8A0
    CharaBody* CharaBody_Dtor(CharaBody* self, s16 flag);                   // 0x1CC830
    void  CharaBody_SetSkeleton(CharaBody* self, Skeleton* skeleton);       // 0x1CC7E0
    s32   CharaBody_MotionFor(CharaBody* self, s32 action);                 // 0x1CAC30  → motion id, −1
    int   CharaBody_Start(CharaBody* self, s32 action, s32 start, s32 blend);   // 0x1CC130
    int   CharaBody_Play(CharaBody* self, s32 action);                      // 0x1CC460  whatever it is doing
    int   CharaBody_Try(CharaBody* self, s32 action);                       // 0x1CC500  if the current action lets it
    int   CharaBody_Resume(CharaBody* self, s32 action);                    // 0x1CC360  from the motion's loop point
    int   CharaBody_TryLink(CharaBody* self, s32 action);                   // 0x1CA750  only as the next blow of a combo
    s32   CharaBody_ComboNext(CharaBody* self, u8 code);                    // 0x1CA590  the combo's next blow for a code, −1
    void  CharaBody_Update(CharaBody* self, Skeleton* skeleton);            // 0x1CC560  one frame
    void  CharaBody_BeginFrame(CharaBody* self);                            // 0x1CB260
    void  CharaBody_fn1cb600(CharaBody* body);                              // 0x1CB600  sword-drawing moments
    const float* CharaBody_Moved(CharaBody* self);                          // 0x1CC760  next − last position
    // A motion for a task (PlayMotion).
    void  CharaBody_PlayScript(CharaBody* self, const s32* request);        // 0x1CAF90  {action, loops, blend, start, end, still}
    void  CharaBody_PlayScripted(CharaBody* self, s32 action, u8 still, s32 loops, s32 start, s32 end, s32 blend);  // 0x1CB0E0
    u8    CharaBody_fn1cae80(CharaBody* body);                              // 0x1CAE80  no task motion in hand
    void  CharaBody_StopScript(CharaBody* self);                            // 0x1CAE90
    // What its action record says about the frame it is on.
    int   CharaBody_Busy(CharaBody* self);                                  // 0x1CAB30
    int   CharaBody_CanGuard(CharaBody* self);                              // 0x1CAB40
    int   CharaBody_AttackStrength(CharaBody* self);                        // 0x1CAB90
    int   CharaBody_AttackKind(CharaBody* self);                            // 0x1CABE0
    int   CharaBody_CanHit(CharaBody* self);                                // 0x1CACF0
    int   CharaBody_AtHitFrame(CharaBody* self);                            // 0x1CAD40
    int   CharaBody_IsAttack(CharaBody* self);                              // 0x1CAD90
    int   CharaBody_VoiceNow(CharaBody* self);                              // 0x1CADE0
    int   CharaBody_SoundNow(CharaBody* self);                              // 0x1CAE30
    // The stick's recent history (for parries).
    void  CharaBody_PushInput(CharaBody* self, u32 direction, int count);   // 0x1C9F80
    u8    CharaBody_Parries(CharaBody* self, u32 attackFlags);              // 0x1CA130
    void  CharaBody_Freeze(CharaBody* self);                                // 0x1C9E40  two frames of hit stop
    void  CharaBody_Knock(CharaBody* self, s32 hurt, s32 strength);         // 0x1C9960
    int   CharaBody_IsLyingAction(const CharaBody* self);                   // 0x1C9DC0
    int   CharaBody_CanRoll(const CharaBody* self);                         // 0x1C9DF0
    int   CharaBody_StartRoll(CharaBody* self);                             // 0x1C9D60
    // Requests (each → the action started, or −1 / 0 when it could not).
    s32   CharaBody_Settle(CharaBody* self);                                // 0x1D0ED0  an action has ended
    void  CharaBody_Refresh(CharaBody* body);                               // 0x1C9770  back to rest, now
    void  CharaBody_Idle(CharaBody* self);                                  // 0x1C9950  …if the action lets it
    s32   CharaBody_Rest(CharaBody* self, int now);                         // 0x1D28E0
    s32   CharaBody_Die(CharaBody* self, int fresh, int now);               // 0x1D12F0
    s32   CharaBody_Collapse(CharaBody* self, int now);                     // 0x1D1540
    s32   CharaBody_Fall(CharaBody* self, int keepVelocity);                // 0x1D1F60
    s32   CharaBody_Land(CharaBody* self);                                  // 0x1D1D30
    s32   CharaBody_Hurt(CharaBody* self, u32 kind);                        // 0x1D16A0
    int   CharaBody_Bumped(CharaBody* self);                                // 0x1D1650
    int   CharaBody_Clash(CharaBody* self, int heavy);                      // 0x1D12B0
    int   CharaBody_GuardBreak(CharaBody* self);                            // 0x1D1250
    int   CharaBody_fn1d1200(CharaBody* self);                              // 0x1D1200
    int   CharaBody_Parry(CharaBody* self);                                 // 0x1D1B90
    int   CharaBody_Walk(CharaBody* self, int run);                         // 0x1D2830
    int   CharaBody_Jump(CharaBody* self);                                  // 0x1D23B0
    s32   CharaBody_Attack(CharaBody* self, u8 direction);                  // 0x1D22B0
    int   CharaBody_Guard(CharaBody* self, int resume);                     // 0x1D1AA0
    int   CharaBody_GuardEnd(CharaBody* self);                              // 0x1D1A40
    void  CharaBody_fn1d1c90(CharaBody* body);                              // 0x1D1C90  put the sword away
    void  CharaBody_fn1d1ce0(CharaBody* body);                              // 0x1D1CE0  draw it
    int   CharaBody_Kick(CharaBody* self, int which);                       // 0x1D2250
    int   CharaBody_fn1d21f0(CharaBody* self, int which);                   // 0x1D21F0
    int   CharaBody_fn1d2160(CharaBody* self, int which);                   // 0x1D2160
    int   CharaBody_fn1d1be0(CharaBody* self);                              // 0x1D1BE0
    int   CharaBody_fn1d1c30(CharaBody* self);                              // 0x1D1C30
    int   CharaBody_Rise(CharaBody* self);                                  // 0x1D18D0
    int   CharaBody_fn1d1960(CharaBody* self);                              // 0x1D1960
    int   CharaBody_RollUp(CharaBody* self);                                // 0x1D19C0
    int   CharaBody_fn1d1b20(CharaBody* self, int which);                   // 0x1D1B20
    s32   CharaBody_fn1d2110(CharaBody* self);                              // 0x1D2110
    s32   CharaBody_Dodge(CharaBody* self, const float* direction);         // 0x1D2540
    s32   CharaBody_DodgeTo(CharaBody* self, int side);                     // 0x1D26B0
    s32   CharaBody_Special(CharaBody* self, int which);                    // 0x1D2770
    s32   CharaBody_WalkTo(CharaBody* self, s32 run, const float* direction);   // 0x1D27E0

    // ── The character (src/game/chara.cpp) ────────────────────────────────
    Chara* Chara_BaseCtor(Chara* self, int id, int kind, int model, int arg);   // 0x1D0970
    Chara* Chara_BaseDtor(Chara* self, s16 flag);                           // 0x1D06B0
    Chara* Chara_CtorA(void* mem, int id, int kind, int model, int arg);    // 0x269C00  the player's class
    Chara* Chara_CtorB(void* mem, int id, int kind, int model, int arg);    // 0x269330  everyone else's
    int   Chara_StartLoadPhase(Chara* self, int phase);     // 0x1CFA50  0 = nothing to do
    int   Chara_PollLoadPhase(Chara* self, int phase);      // 0x1CF580  1 done, −1 failed
    u32   Chara_InstallMotions(Chara* self, int fromFile, s32 pack, int player);   // 0x1CF030
    void  Chara_Reset(Chara* self);                         // 0x1D05B0
    void  Chara_fn1cccf0(Chara* self);                      // 0x1CCCF0
    void  Chara_fn1cccb0(Chara* self);                      // 0x1CCCB0
    void  Chara_fn1ccc30(Chara* self, int drop);            // 0x1CCC30
    void  Chara_fn1ccb90(Chara* self);                      // 0x1CCB90
    void  Chara_fn1ccb30(Chara* self);                      // 0x1CCB30
    void  Chara_fn1cca90(Chara* self);                      // 0x1CCA90
    void  Chara_fn1ccad0(Chara* self, u32 arg);             // 0x1CCAD0
    void  Chara_fn1ccb20(Chara* self, u32 arg);             // 0x1CCB20
    void  Chara_fn1cccc0(Chara* self, u32 arg);             // 0x1CCCC0  play a task's motion (arg: the request)
    void  Chara_ClearTransient(Chara* self);                // 0x1CDB70
    int   Chara_fn1cce20(Chara* self, u32 a, u32 b);        // 0x1CCE20  copies out two joint positions
    void  Chara_fn1ccfa0(Chara* self);                      // 0x1CCFA0
    int   Chara_TakeSword(Chara* self, void* sword);        // 0x1CD770  non-zero: its model must load
    void  Chara_fn1cd850(Chara* self, u32 v);               // 0x1CD850
    void  Chara_fn1cd690(Chara* self, u32 v);               // 0x1CD690
    void  Chara_fn1cd6b0(Chara* self, u32 v);               // 0x1CD6B0
    void  Chara_fn1cd6c0(Chara* self, void* sword, u32 a);  // 0x1CD6C0
    void  Chara_fn1cd860(Chara* self);                      // 0x1CD860
    int   Chara_SetAction(Chara* self, u32 routine);        // 0x1CDB90  → handle
    void  Chara_fn1cdb00(Chara* self);                      // 0x1CDB00
    void  Chara_fn1cec20(Chara* self);                      // 0x1CEC20
    void  Chara_LookAt(Chara* self, const float* point);    // 0x1CEDB0
    void  Chara_StepMap(Chara* self, struct MapCollision* map);     // 0x1CE580  (vtable +0x14)
    // Registers the two classes' virtual methods with the guest registry.
    void  Chara_BindMethods();

    // ── Control (src/game/chara_control.cpp) ──────────────────────────────
    CharaControl* CharaControl_Ctor(CharaControl* self);                    // 0x1C7E00
    CharaControl* CharaControl_Dtor(CharaControl* self, s16 flag);          // 0x1C7D80
    void  CharaControl_Attach(CharaControl* self, CharaBody* body);         // 0x1C7D10
    u32   CharaControl_Set(CharaControl* self, u32 routine);                // 0x1C7B80
    void  CharaControl_Step(CharaControl* self);                            // 0x1C7C00
    void  CharaAction_fn1c7d20(CharaAction* action);                        // 0x1C7D20  drop the routine
    s32   CharaControl_Kind(CharaControl* self);                            // 0x1C7B60
    // A routine: kind 2 = an AI move (mode, the data TaskAI_Ctor builds), 0 / 1 = the pad on port 0 / 1.
    u32   CharaAction_Make(int kind, u32 mode, const void* data);           // 0x1C7E20

    u32   AiMove_Create(u32 mode, const void* data);        // 0x18BFD0  → the move object of an AI routine (ai_moves.cpp)

    // ── Not decompiled; see src/native/hle_chara.cpp ──────────────────────
    void* PadCommands_Ctor(void* mem, void* padMgr, int port);              // 0x1910F0
    void* PadCommands_Dtor(void* self, int flag);                           // 0x1910A0
    void  PadCommands_Push(void* self, const float* direction);             // 0x190FF0
    void  PadCommands_Clear(void* self);                                    // 0x190FE0
    int   PadCommands_Special(void* self, const float* facing);             // 0x190DF0  → −1
    int   PadCommands_Dodge(void* self, const float* facing, const float* direction);   // 0x190630
    u8    PadCommands_Attack(void* self, const float* facing, int airborne);    // 0x190AC0
    u8    PadCommands_ComboAttack(void* self, const float* facing, int airborne);   // 0x190750
    int   PadCommands_Pressed(void* self, u32 buttons, int frames);         // 0x1906F0
    void  Camera_GetViewRotation(float* matrix);            // 0x150440  (when a camera exists: *0x2EE528)
    void* SwordHolder_Ctor(void* mem);                      // 0x245A00  (sword_holder.cpp)
    void* SwordHolder_Dtor(void* self, int flag);           // 0x245940
    int   CharaSwords_Wear(void* swords, int amount, CharaParams* owner);   // 0x2452F0
    int   CharaSwords_IsReady(CharaSwords* swords);         // 0x245710
    void  CharaSwords_fn2454b0(CharaSwords* swords);        // 0x2454B0
    void  CharaSwords_fn245150(CharaSwords* swords);        // 0x245150
    void  CharaVoice_fn26a7c0(void* voice, u32 a, int b, int c);    // 0x26A7C0
    int   ItemDrops_fn242550(void* drops, const void* matrix, void* swordDb);   // 0x242550
    void  SwordDb_fn15ccd0(void* db, void* sword);          // 0x15CCD0
    void  SwordDb_fn15c330(void* db, u32 a, u32 b, const void* matrix);     // 0x15C330
    void* SwordDb_fn15d0d0(void* db, u32 id);               // 0x15D0D0  → Sword*
    u32   SwordDb_fn15d890(void* db, u32 a, int b);         // 0x15D890
    u32   SwordDb_fn15e4b0(void* db, u32 a);                // 0x15E4B0
    u32   SwordDb_fn15e470(void* db, u32 a);                // 0x15E470
    int   SwordDb_fn15ce60(void* db, u32 kind, void* sword);    // 0x15CE60
    int   SkillSheets_Request(void* self, int sheet, int slot);     // 0x23D870 (src/game/skill_sheets.cpp)
    void  FlagsObj_Remove(void* flags, u32 handle);         // 0x156E80
    u32   FlagsObj_AddChara(void* flags, u32 a, int id);    // 0x157020
    u32   FlagsObj_Add(void* flags, u32 a);                 // 0x156FB0
    int   Frustum_Outside(const void* matrix, const void* point, const void* planes, int count);    // 0x165A20
    void  Frustum_LoadPlanes(const void* a, const void* b, void* planes, int count);                // 0x165890
    void  Frustum_MakeMatrix(float w, float h, float d, float zn, float zf, int a, void* out);     // 0x165AD0
    float Vec_Distance(const void* a, const void* b);       // 0x136DA0
    void  CharaResources_fn1d2b20(CharaResources* res, u32 arg);    // 0x1D2B20
    void  CharaResources_fn1d2bb0(CharaResources* res);     // 0x1D2BB0
    void  CharaResources_fn1d6300(CharaResources* res);     // 0x1D6300
    void  Debug_Print(const char* text);                    // 0x165800
}
