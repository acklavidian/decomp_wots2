// What a motion makes heard — decompiled from 0x2A2990–0x2A2F90 and 0x222CB0.
//
// A motion's action record lists "voice" events by frame.  A plain number is
// a sound effect played where the character stands; with bit 0x1000 it is
// one of nine things that depend on who makes it: a footstep by the ground
// underfoot, a cry of the character's voice set, the swish of a sword.

#include "engine/bgm.h"
#include "game/chara.h"
#include "game/chara_resources.h"
#include "native/guest.h"

#include <cstring>

extern "C" {
    int   CharaFace_Set(void* cries, const void* params, int kind);    // 0x2A2880  a cry of a kind (bgm.cpp)
}

namespace {

inline int Place(const CharaParams* p) { return (int)guest::addr(&p->matrix[12]); }

// ── 0x2A2990 ──────────────────────────────────────────────────────────────
// A footstep: which sound depends on the ground of the cell underfoot.
bool Footstep(const CharaParams* params, const u8* cell) {
    static const int kStep[3] = {11, 12, 13};       // 0x2DB3B0
    u32 ground;
    memcpy(&ground, cell + 8, 4);
    int which = -1;
    if (ground & 1) which = 0;
    else if (ground & 2) which = 1;
    else if (ground & 4) which = 0;
    else if (ground & 8) which = 2;
    if (which != -1)
        Sound_PlaySE(kStep[which], Place(params), 0x3FFF);
    return which != -1;
}

// ── 0x2A2A50 ──────────────────────────────────────────────────────────────
void Special(void* cries, u32 what, Chara* chara, CharaBody* body) {
    const CharaParams* params = guest::ptr<CharaParams>(body->params);
    static const int kCry[7] = {-1, 0, 1, 4, 5, 7, 9};      // the kind of cry for 1–6
    const u32 n = what & ~0x1000u;
    if (n == 0) {
        Footstep(params, reinterpret_cast<const u8*>(chara) + 0x150);
    } else if (n <= 6) {
        CharaFace_Set(cries, params, kCry[n]);
    } else if (n == 7 || n == 8) {
        // The swish of what is in hand (not for a sword whose model says no).
        const CharaSwords* swords = guest::ptr<CharaSwords>(chara->swords);
        if (swords->_0 != 0 && *guest::ptr<s16>(swords->_0 + 8) == 0)
            Sound_PlaySE(n == 7 ? 0x13 : 0x14, Place(params), 0x3FFF);
    }
}

}  // namespace

extern "C" {

// ── 0x2A2F90 ──────────────────────────────────────────────────────────────
// Every frame: the events the motion has at this frame.
void CharaFace_Update(void* cries, Chara* chara, CharaBody* body) {
    if (body->state & 0x8000000)
        return;
    for (int what; (what = CharaBody_VoiceNow(body)) != -1;) {
        if ((what & 0x1000) == 0)
            Sound_PlaySE(what, Place(guest::ptr<CharaParams>(body->params)), 0x3FFF);
        else
            Special(cries, (u32)what, chara, body);
    }
}

// ── 0x222CB0 ──────────────────────────────────────────────────────────────
// A character with a sound of its own (CharaParams+8) repeats it every 17
// frames while its state has 0x10000 or 0x20000 set.
void CharaEffects_Update(void* effects, CharaBody* body) {
    if (body == nullptr)
        return;
    s32* count = static_cast<s32*>(effects);
    const CharaParams* params = guest::ptr<CharaParams>(body->params);
    if (params->_08 == -1)
        return;
    if ((body->state & 0x30000) == 0) {
        *count = 0;
    } else if ((*count)++ > 0xF) {
        Sound_PlaySE(params->_08, Place(params), 0x3FFF);
        *count = 0;
    }
}

}  // extern "C"
