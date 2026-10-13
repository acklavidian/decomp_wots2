// What task commands leave running — decompiled from 0x20FA00–0x210420
// (DrawSerif), 0x210600–0x210A90 (DrawTalkSelect), 0x210A90–0x210E20
// (PlayMotion), 0x222DA0–0x2231F0 (SetAI) and 0x275900–0x275C40
// (SetDirection).  See task_mgr.h.

#include "engine/font.h"
#include "game/map_db.h"
#include "game/talk_mgr.h"
#include "game/task_mgr.h"
#include "native/guest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
    int   Rand_Next();                                  // 0x1200F0
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Matrix_Rotate(void* dst, const void* src, float angle);   // 0x107618
    void  CharaMgr_fn143bf0(CharaMgr* mgr, int id, u32 request);    // 0x143BF0  play a motion
    u8    CharaMgr_fn143a10(CharaMgr* mgr, int id);     // 0x143A10  has its motion finished
    void* MemAllocAligned(u32 align, u32 size);         // 0x1228C8
    void  MemFreeAligned(void* p);                      // 0x122B50
    void  MemFree(void* p);                             // 0x100650
    u32   CharaMgr_GetFlags(CharaMgr* mgr, int id);     // 0x1467B0
    int   CharaMgr_GetPartner(CharaMgr* mgr, int id);   // 0x146B00
}

static const GuestPtrVar<u8> gPlayerData{0x2EE688u};
static s32& gFrameRate = *guest::ptr<s32>(0x2EDA00u);
static const u16* const kEllipsis = guest::ptr<const u16>(0x2EE188u);   // "..." for a line without text

template <typename T = void> static inline T* P(u32 a) { return guest::ptr<T>(a); }

struct TaskSerif {
    TaskAction base;        // handle = the talk entry
    u32 talk;               // +0x14
    u32 charas;             // +0x18
    u32 text;               // +0x1C  the line with the player's name filled in
    s32 listener;           // +0x20
};
struct TaskTalkSelect {
    TaskAction base;
    u32 talk;               // +0x14
    u32 charas;             // +0x18
    u32 text;               // +0x1C
    s32 choices;            // +0x20
};

static int Resolve(s32 who, int self, const TaskCommand* cmd, CharaMgr* charas) {
    switch (who) {
    case kWhoOther:   return cmd->other;
    case kWhoPartner: {
        const int partner = CharaMgr_GetPartner(charas, self);
        return partner != -1 ? partner : self;
    }
    case kWhoTarget:  return cmd->target;
    case kWhoPlayer:  return 0;
    case kWhoSelf:    return self;
    default:          return who;
    }
}

static u16* CopyText(const u16* text, int length) {
    u16* copy = static_cast<u16*>(MemAllocAligned(0x10, (u32)(length + 1) * 2));
    memcpy(copy, text, (size_t)length * 2);
    copy[length] = 0;
    return copy;
}

// ══ DrawSerif ═════════════════════════════════════════════════════════════

// ── 0x20FA00 ──────────────────────────────────────────────────────────────
// Done when the line has gone from the screen; −1 (and on to the line named
// in arg 3) if the listener was hit meanwhile.
static int Serif_Update(TaskSerif* self) {
    const TaskCommand* cmd = P<TaskCommand>(self->base.command);
    if (cmd->arg[3] != -1 && (CharaMgr_GetFlags(P<CharaMgr>(self->charas), self->listener) & 0x10000) != 0) {
        self->base.nextLine = cmd->arg[3];
        return -1;
    }
    return Talk_Exists(P(self->talk), (u32)self->base.handle) == 0;
}

// ── 0x20FA90 ──────────────────────────────────────────────────────────────
static TaskSerif* Serif_Dtor(TaskSerif* self, s16 flag) {
    if (self == nullptr)
        return self;
    if (self->text != 0)
        MemFreeAligned(P(self->text));
    if (flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x20FA90, Serif_Dtor);
GUEST_BIND(0x20FA00, Serif_Update);

// ── 0x20FB10 ──────────────────────────────────────────────────────────────
// arg 0: balloon style; arg 1: milliseconds it stays (−1: until a button).
TaskAction* TaskSerif_Ctor(void* mem, int actor, TaskCommand* cmd, CharaMgr* charas, void* talk) {
    TaskSerif* self = static_cast<TaskSerif*>(mem);
    self->base.vtable = 0x2EC220u;
    self->base.actor = actor;
    self->base.command = guest::addr(cmd);
    self->talk = guest::addr(talk);
    self->charas = guest::addr(charas);
    self->base.nextLine = cmd->wait == 0 ? -1 : cmd->index + 1;
    CharaResources* res = CharaResources_Get();
    const int speaker = Resolve(cmd->who1, actor, cmd, charas);
    const int listener = Resolve(cmd->who2, actor, cmd, charas);
    self->listener = listener;

    // "%s" (or "%S") in a line is the player's name.
    const u16* name = reinterpret_cast<const u16*>(*gPlayerData + 0x480);
    const int nameLength = WStr_Length(name);
    const u16* text = P<const u16>(cmd->text);
    if (text == nullptr) {
        self->text = guest::addr(CopyText(kEllipsis, 3));
    } else {
        const int length = WStr_Length(text);
        int names = 0;
        for (int i = 0; text[i] != 0; i++)
            if (text[i] == '%' && (text[i + 1] == 's' || text[i + 1] == 'S'))
                names++;
        if (names == 0) {
            self->text = guest::addr(CopyText(text, length));
        } else {
            const int total = length - 2 + nameLength * names;
            u16* out = static_cast<u16*>(MemAllocAligned(0x10, (u32)(total + 1) * 2));
            self->text = guest::addr(out);
            int o = 0;
            for (int i = 0; i < length; i++) {
                if (text[i] == '%' && (text[i + 1] == 's' || text[i + 1] == 'S')) {
                    i++;
                    for (int k = 0; k < nameLength; k++)
                        out[o++] = name[k];
                } else {
                    out[o++] = text[i];
                }
            }
            out[total] = 0;     // (so the original: right for one name per line)
        }
    }
    if (getenv("WOTS2_DUMP_ACTORS")) {          // debug hook, not in the original
        printf("[say] %d -> %d: ", speaker, listener);
        for (const u16* t = P<const u16>(self->text); *t; t++)
            putchar(*t == '\n' ? ' ' : *t < 0x7F ? (int)*t : '?');
        putchar('\n');
    }
    const int style = cmd->arg[0] != -1 ? cmd->arg[0] : 0;
    const int frames = cmd->arg[1] != -1 ? (int)((float)cmd->arg[1] / 1000.0f * (float)gFrameRate) : -1;
    self->base.handle = (s32)Talk_Say(talk, CharaResources_GetParams(res, speaker),
                                      CharaResources_GetParams(res, listener), P<const u16>(self->text), style,
                                      frames, actor);
    return &self->base;
}

// ══ DrawTalkSelect ════════════════════════════════════════════════════════

// ── 0x210600 ──────────────────────────────────────────────────────────────
// Done when the choice has been made: on to the line of the chosen entry
// (args 0–3), arg 4 when the player walked away, the next line on a timeout.
static int TalkSelect_Update(TaskTalkSelect* self) {
    const TaskCommand* cmd = P<TaskCommand>(self->base.command);
    if (cmd->arg[3] != -1 && (CharaMgr_GetFlags(P<CharaMgr>(self->charas), 0) & 0x10000) != 0) {
        self->base.nextLine = cmd->arg[3];
        return 1;
    }
    const TalkMgr* talk = P<TalkMgr>(self->talk);
    if (Talk_Exists(P(self->talk), talk->choiceHandle) != 0)
        return 0;
    const int choice = talk->selection - 1;
    if (choice < 0 || self->base.actor != talk->chooser)
        return 0;
    if (choice == 0xFF) {
        self->base.nextLine = cmd->index + 1;
    } else if (choice == 0x100) {
        if (cmd->arg[4] == -1)
            self->base.nextLine--;
        else
            self->base.nextLine = cmd->arg[4];
    } else {
        self->base.nextLine = cmd->arg[choice];
    }
    return 1;
}

// ── 0x210730 ──────────────────────────────────────────────────────────────
static TaskTalkSelect* TalkSelect_Dtor(TaskTalkSelect* self, s16 flag) {
    if (self == nullptr)
        return self;
    if (self->text != 0)
        MemFreeAligned(P(self->text));
    if (flag > 0)
        MemFree(self);
    return self;
}
GUEST_BIND(0x210730, TalkSelect_Dtor);
GUEST_BIND(0x210600, TalkSelect_Update);

// ── 0x2107B0 ──────────────────────────────────────────────────────────────
// The text's lines are the entries.  The choice is put to the player.
TaskAction* TaskTalkSelect_Ctor(void* mem, int actor, TaskCommand* cmd, CharaMgr* charas, void* talk) {
    TaskTalkSelect* self = static_cast<TaskTalkSelect*>(mem);
    self->base.vtable = 0x2EC250u;
    self->choices = 1;
    self->base.actor = actor;
    self->base.command = guest::addr(cmd);
    self->talk = guest::addr(talk);
    self->charas = guest::addr(charas);
    self->base.nextLine = cmd->index + 1;
    CharaResources* res = CharaResources_Get();
    const u16* text = P<const u16>(cmd->text);
    u16* copy = text == nullptr ? CopyText(guest::ptr<const u16>(0x2EE190u), 3) : CopyText(text, WStr_Length(text));
    self->text = guest::addr(copy);
    for (int i = 0; copy[i] != 0; i++)
        if (copy[i] == '\r' || copy[i] == '\n')
            self->choices++;
    self->base.handle = (s32)Talk_Select(talk, CharaResources_GetParams(res, actor), CharaResources_GetParams(res, 0),
                                         copy, 0, self->choices, gFrameRate * 10);
    return &self->base;
}

// ══ Turning ═══════════════════════════════════════════════════════════════

// ── 0x1C8C40 ──────────────────────────────────────────────────────────────
// Turns a character's matrix towards a point by at most `degrees`; 1 once it
// faces it (within about 11 degrees, then it snaps).
int Matrix_TurnTowards(float degrees, float* matrix, const float* point) {
    float fx = matrix[8], fz = matrix[10];
    const float fl = __builtin_sqrtf(fx * fx + matrix[9] * matrix[9] + fz * fz);
    fx /= fl;
    fz /= fl;
    float dx = point[0] - matrix[12], dz = point[2] - matrix[14];
    const float dl = __builtin_sqrtf(dx * dx + dz * dz);
    dx /= dl;
    dz /= dl;
    if (fx * dx + fz * dz < 0.98f) {
        alignas(16) float position[4];
        memcpy(position, &matrix[12], sizeof position);
        memcpy(&matrix[12], guest::ptr<float>(0x2D1AB0u), sizeof position);
        const float step = degrees * 0.017453292f;
        Matrix_Rotate(matrix, matrix, fx * dz - fz * dx < 0.0f ? step : -step);
        memcpy(&matrix[12], position, sizeof position);
        return 0;
    }
    matrix[8] = dx;
    matrix[10] = dz;
    matrix[0] = dz;
    matrix[2] = -dx;
    return 1;
}

static bool SamePlace(const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

static TaskAction* BaseDtor(TaskAction* self, s16 flag) {       // 0x222E10, 0x210B90, 0x275A70
    if (self != nullptr && flag > 0)
        MemFree(self);
    return self;
}

// ══ SetDirection ══════════════════════════════════════════════════════════

struct TaskDirection {
    TaskAction base;
    s32 who, at;            // +0x14
    u32 _1c;
    float point[4];         // +0x20  the place to face, when the command names one
    u32 charas;             // +0x30
    u32 _34[3];
};

// ── 0x275900 ──────────────────────────────────────────────────────────────
// Turns a little each frame; done when it faces the place (or the other).
static int Direction_Update(TaskDirection* self) {
    CharaMgr* charas = P<CharaMgr>(self->charas);
    const TaskCommand* cmd = P<TaskCommand>(self->base.command);
    alignas(16) float matrix[16], target[4];
    Matrix_Copy(matrix, CharaMgr_GetTransform(charas, self->who));
    memcpy(target, cmd->arg[0] == -1 ? CharaMgr_GetPosition(charas, self->at) : self->point, sizeof target);
    if (SamePlace(target, &matrix[12]))
        return 1;
    const CharaParams* p = CharaResources_GetParams(CharaResources_Get(), self->who);
    if (p == nullptr)
        return 0;
    const u32* state = P<u32>(p->link[6]);
    const u32* flags = P<u32>(p->link[5]);
    // Not while it is knocked about or lying down.
    if (((state != nullptr ? *state : 0) & 0x1000) != 0 || ((flags != nullptr ? *flags : 0) & 6) != 0)
        return 0;
    const int done = Matrix_TurnTowards(10.0f, matrix, target);
    CharaMgr_SetTransform(charas, self->who, matrix);
    return done;
}
static TaskAction* Direction_Dtor(TaskAction* self, s16 flag) { return BaseDtor(self, flag); }
GUEST_BIND(0x275A70, Direction_Dtor);
GUEST_BIND(0x275900, Direction_Update);

// ── 0x275AD0 ──────────────────────────────────────────────────────────────
TaskAction* TaskDirection_Ctor(void* mem, int actor, TaskCommand* cmd, CharaMgr* charas, const float* point) {
    TaskDirection* self = static_cast<TaskDirection*>(mem);
    self->base.vtable = 0x2ECDF0u;
    self->base.actor = actor;
    self->base.command = guest::addr(cmd);
    self->charas = guest::addr(charas);
    self->base.nextLine = cmd->wait == 0 ? -1 : cmd->index + 1;
    if (cmd->arg[0] != -1)
        memcpy(self->point, point, sizeof self->point);
    self->who = Resolve(cmd->who1, actor, cmd, charas);
    self->at = Resolve(cmd->who2, actor, cmd, charas);
    return &self->base;
}

// ══ PlayMotion ════════════════════════════════════════════════════════════

struct TaskMotion {
    TaskAction base;
    s32 who, at;            // +0x14
    struct { s32 motion, arg; s32 _8[3]; u8 flag; } request;    // +0x1C  what the character is asked to play
    u32 charas;             // +0x34
};

// ── 0x210A90 ──────────────────────────────────────────────────────────────
// With arg 2 set the character keeps turning to the other while it plays.
// A command that waits is done when the motion is.
static int Motion_Update(TaskMotion* self) {
    CharaMgr* charas = P<CharaMgr>(self->charas);
    const TaskCommand* cmd = P<TaskCommand>(self->base.command);
    if (cmd->arg[2] != -1) {
        alignas(16) float matrix[16], target[4];
        Matrix_Copy(matrix, CharaMgr_GetTransform(charas, self->who));
        memcpy(target, CharaMgr_GetPosition(charas, self->at), sizeof target);
        if (!SamePlace(target, &matrix[12])) {
            Matrix_TurnTowards(10.0f, matrix, target);
            CharaMgr_SetTransform(charas, self->who, matrix);
        }
    }
    if (cmd->wait == -1)
        return CharaMgr_fn143a10(charas, self->who) != 0;
    return cmd->arg[2] == -1;
}
static TaskAction* Motion_Dtor(TaskAction* self, s16 flag) { return BaseDtor(self, flag); }
GUEST_BIND(0x210B90, Motion_Dtor);
GUEST_BIND(0x210A90, Motion_Update);

// ── 0x210BF0 ──────────────────────────────────────────────────────────────
TaskAction* TaskMotion_Ctor(void* mem, int actor, TaskCommand* cmd, CharaMgr* charas) {
    TaskMotion* self = static_cast<TaskMotion*>(mem);
    self->base.vtable = 0x2EC260u;
    self->base.actor = actor;
    self->base.command = guest::addr(cmd);
    self->charas = guest::addr(charas);
    self->base.nextLine = cmd->wait == 0 ? -1 : cmd->index + 1;
    self->request.motion = cmd->arg[0];
    self->request.arg = cmd->arg[1];
    self->request._8[0] = self->request._8[1] = self->request._8[2] = -1;
    self->request.flag = 0;
    self->who = Resolve(cmd->who1, actor, cmd, charas);
    self->at = Resolve(cmd->who2, actor, cmd, charas);
    CharaMgr_fn143bf0(charas, self->who, guest::addr(&self->request));
    return &self->base;
}

// ══ SetAI ═════════════════════════════════════════════════════════════════

struct TaskAI {
    TaskAction base;        // handle = the action it gave the character
    u32 chara;              // +0x14  Chara*
};

// ── 0x222DA0 ──────────────────────────────────────────────────────────────
// Done when the character reports that action finished; −1 for the modes
// that end with it walking out of the area.
static int AI_Update(TaskAI* self) {
    const CharaAction* action = P<CharaAction>(P<Chara>(self->chara)->action);
    const int finished = action->current != 0 ? action->handle : 0;
    if (self->base.handle != finished)
        return 0;
    const int mode = P<TaskCommand>(self->base.command)->arg[0];
    return mode == 5 || mode == 6 ? -1 : 1;
}
static TaskAction* AI_Dtor(TaskAction* self, s16 flag) { return BaseDtor(self, flag); }
GUEST_BIND(0x222E10, AI_Dtor);
GUEST_BIND(0x222DA0, AI_Update);

// ── 0x222E70 ──────────────────────────────────────────────────────────────
// arg 0 is the mode; the character's action system gets it with what it
// needs to find its way: the area's route points and the map state's
// connectivity tables (see "Map state"), then per mode
//   2  a target point (arg 1) at speed 0.8      3  someone (who2), to within arg 2 / 1000
//   4  a path of the area (arg 1, −1 = any)      5  one of the area's ten entrances (it leaves)
//   6  someone, and the area's target points     others: nothing (0 stand, 1 fight)
TaskAction* TaskAI_Ctor(void* mem, int actor, TaskCommand* cmd, Chara* chara, void* mapState) {
    static const GuestPtrVar<u8> gPlayer{0x2EE688u};
    TaskAI* self = static_cast<TaskAI*>(mem);
    self->base.vtable = 0x2EC5A0u;
    self->base.actor = actor;
    self->base.command = guest::addr(cmd);
    self->chara = guest::addr(chara);
    self->base.nextLine = cmd->wait == 0 ? -1 : cmd->index + 1;
    int target = actor;
    switch (cmd->who2) {
    case kWhoOther:   target = cmd->other; break;
    case kWhoPartner: if (chara->partner != -1) target = chara->partner; break;
    case kWhoTarget:  target = cmd->target; break;
    case kWhoPlayer:  target = 0; break;
    case kWhoSelf:    break;
    default:          target = cmd->who2; break;
    }
    MapDb* db = MapDb_Get();
    const s32 area = (s8)(*gPlayer)[0x103];
    const s32* arg = cmd->arg;
    const s32 indoors = *guest::ptr<s32>(0x2EE518u);
    const u32 links = guest::addr(mapState) + (u32)indoors * 0x10;
    const PointSet* targets = P<PointSet>(db->targetPoints) + area;
    struct Common { u32 route, linksA, linksB; };
    const Common common{guest::addr(MapDb_GetRoutePoints(db, area)), links + 0x20, links + 0x60};
    alignas(16) struct { Common c; u32 w[9]; } data{};
    data.c = common;
    const void* request = &data;
    switch (arg[0]) {
    case 6:
        data.w[0] = guest::addr(CharaResources_GetParams(CharaResources_Get(), target));
        data.w[1] = guest::addr(targets);
        data.w[2] = (u32)arg[1];
        data.w[3] = 1;
        break;
    case 5: {
        const int entrance = Rand_Next() % 10;
        if (entrance < targets->count)
            memcpy(&data.w[1], P<float>(targets->points) + entrance * 4, 16);
        data.w[5] = (u32)arg[1];
        data.w[6] = 1;
        break;
    }
    case 3: {
        data.w[0] = guest::addr(CharaResources_GetParams(CharaResources_Get(), target));
        float distance = 1.0f;
        data.w[4] = 1;
        if (arg[2] != -1) {
            distance = (float)arg[2] / 1000.0f;
            if (distance == 0.0f)
                data.w[4] = 0;
        }
        memcpy(&data.w[1], &distance, 4);
        data.w[3] = (u32)arg[1];
        break;
    }
    case 4: {
        const GuestVector* paths = db->pathGroups != 0 ? P<GuestVector>(db->pathGroups) + area : nullptr;
        const u32 pick = arg[1] == -1 ? (u32)(Rand_Next() % (int)paths->count) : (u32)arg[1];
        const PointSet* path = P<PointSet>(P<u32>(paths->data)[pick]);
        data.w[0] = (u32)path->count;
        data.w[1] = path->points;
        data.w[2] = (u32)arg[2];
        data.w[3] = 1;
        break;
    }
    case 2: {
        if (arg[1] < targets->count)
            memcpy(&data.w[1], P<float>(targets->points) + arg[1] * 4, 16);
        const float speed = 0.8f;
        memcpy(&data.w[5], &speed, 4);
        data.w[6] = (u32)arg[2];
        data.w[7] = 1;
        break;
    }
    default:
        request = nullptr;
        break;
    }
    self->base.handle = Chara_SetAction(chara, CharaAction_Make(2, (u32)arg[0], request));
    return &self->base;
}
