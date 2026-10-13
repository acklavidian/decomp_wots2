// The text roll: lines of text that scroll up the screen (or fade in and out
// in place), each for a while — the opening's "It is the late Edo Period…".
//
// Decompiled from 0x2B5230–0x2B5C60 (the ".er2" roll, vtable 0x2ED7B0) under
// the render effect that owns it (type 0x1E, 0x2B7270–0x2B7560; started by
// Effect.EndRoll through 0x14E850, ended by 0x14E7E0).  The picture rolls
// (".erp", class 0x2B68A0) are not done — and no .erp file is on this disc.
//
// An .er2 file:
//   +0x00 s16 x, y (pixels: from the left; from the bottom when scrolling)
//   +0x04 s16 mode (0 scrolls up, 1 stays in place)   +0x06 s16 frames it lasts
//   +0x08 s16 frames a line stays lit   +0x0C float pixels a frame (0: the caller's)
//   +0x10 s16 voiceCount, {s16 frame, s16 voice}[]
//   then lines: {s16 pixels below the line before, 16-bit text, 0}
//
// The object's lists are host containers here; nothing else reads them.

#include "engine/font.h"
#include "native/archive.h"
#include "native/guest.h"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

extern "C" {
    u32  Screen_X(int v, int align);                            // 0x12E5E0
    u32  Screen_Y(int v, int align);                            // 0x12E550
    void Voice_Start(int id, int positional, int volume);       // 0x1801F0
    void Voice_Stop();                                          // 0x1801E0
}

namespace {

const GuestPtrVar<Font> gFont{0x2EE67Cu};
s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);

struct Line {
    s32 gap;            // 1/16 pixel below the line before
    float stay;         // frames it still stays lit
    u32 text;           // const u16* (guest)
    float alpha;        // 0 … 127
};
struct Roll {
    bool active = false;
    s16 x = 0, y = 0, mode = 0;
    s32 total = 0;      // +0x08
    float speed = 0;    // +0x0C
    s32 state = 0;      // +0x10  0 running, 1 held, 2 fading away
    std::deque<Line> pending;                   // +0x38
    std::vector<Line> shown;                    // +0x44
    std::vector<std::pair<s16, s16>> voices;    // +0x2C  {frame, voice}
    float untilNext = 0;                        // +0x50
    float elapsed = 0;                          // +0x54
    u32 file = 0;
};
Roll sRoll;

}  // namespace

// 0x14E850 (with the parse, 0x2B5870).  → false if there is no such roll.
bool TextRoll_Start(const char* path, float speed, int total) {
    std::vector<u8> data;
    if (!archive::read(path, data) || data.size() < 0x12)
        return false;
    sRoll = Roll{};
    // The lines are drawn from where they lie, so the file goes into guest memory.
    u8* d = static_cast<u8*>(guest::alloc((u32)data.size() + 2));
    memcpy(d, data.data(), data.size());
    d[data.size()] = d[data.size() + 1] = 0;
    sRoll.file = guest::addr(d);
    s16 header[6];
    float ownSpeed;
    memcpy(header, d, sizeof header);
    memcpy(&ownSpeed, d + 0x0C, 4);
    sRoll.x = header[0];
    sRoll.y = header[1];
    sRoll.mode = header[2];
    sRoll.total = total == -1 ? header[3] : total;
    sRoll.speed = ownSpeed != 0.0f ? ownSpeed : speed;
    const s16* p = reinterpret_cast<const s16*>(d + 0x10);
    const s16* end = reinterpret_cast<const s16*>(d + (data.size() / 2) * 2);
    for (s16 n = *p++; n > 0 && p + 1 < end; n--, p += 2)
        sRoll.voices.push_back({p[0], p[1]});
    while (p < end) {
        Line line{(s32)*p << 4, (float)header[4], guest::addr(p + 1), 0.0f};
        p++;
        while (p < end && *p != 0)
            p++;
        p++;
        sRoll.pending.push_back(line);
    }
    sRoll.active = true;
    return true;
}
// 0x14E7E0
// (Told to leave, the effect above the roll — 0x2B72C0 — first cuts the
// narration its cues started.)
void TextRoll_Stop() {
    if (sRoll.active)
        Voice_Stop();
    sRoll.active = false;
}
bool TextRoll_Active() { return sRoll.active; }

// 0x2B5560: once a frame.
void TextRoll_Update() {
    Roll& r = sRoll;
    if (!r.active)
        return;
    const float step = 30.0f / (float)*gFrameRate;
    if (r.state != 1) {
        r.untilNext -= r.speed * 16.0f * step;
        if (r.untilNext <= 0.0f && !r.pending.empty()) {
            r.shown.push_back(r.pending.front());
            r.pending.pop_front();
            if (!r.pending.empty())
                r.untilNext += (float)r.pending.front().gap;
        }
    }
    bool anyLit = false;
    for (Line& line : r.shown) {
        if (r.state == 2) {
            if (line.alpha != 0.0f && (line.alpha -= step * 8.466666f) <= 0.0f)
                line.alpha = 0.0f;
        } else {
            line.stay -= step;
            if (line.stay <= 0.0f) {
                line.stay = 0.0f;
                if ((line.alpha -= step * 2.0f) <= 0.0f)
                    line.alpha = 0.0f;
            } else if ((line.alpha += step * 2.0f) > 127.0f) {
                line.alpha = 127.0f;
            }
        }
        anyLit = anyLit || line.alpha != 0.0f;
    }
    if ((float)r.total <= r.elapsed) {
        r.elapsed = (float)r.total;
        r.state = 2;
        if (!anyLit)
            r.active = false;                   // (the effect above it, 0x2B72C0, lets go)
        return;
    }
    r.elapsed += step;
    for (auto it = r.voices.begin(); it != r.voices.end();) {
        if ((float)it->first <= r.elapsed) {
            Voice_Start(it->second + 1, 0, 0x3FFF);
            it = r.voices.erase(it);
        } else {
            ++it;
        }
    }
}

// 0x2B5240
void TextRoll_Draw() {
    Roll& r = sRoll;
    if (!r.active)
        return;
    Font* font = *gFont;
    const s32 x = (s32)((r.x * 16 + (Screen_X(0, -1) & 0xFFFF)) & 0xFFFF);
    s32 y;
    if (r.mode == 1)
        y = (s32)((r.y * 16 + (Screen_Y(0, -1) & 0xFFFF)) & 0xFFFF);
    else
        y = (s32)((float)(s32)(r.y * 16 + (Screen_Y(0, 1) & 0xFFFF)) - r.elapsed * r.speed * 16.0f) & 0xFFFF;
    for (size_t i = 0; i < r.shown.size(); i++) {
        const Line& line = r.shown[i];
        if (i != 0)
            y = (y + line.gap) & 0xFFFF;
        if (line.alpha == 0.0f)
            continue;
        const u32 a = (u32)(s32)(line.alpha + 0.5f) << 24;
        const u16* text = guest::ptr<const u16>(line.text);
        Font_SetColor(font, a);
        Font_DrawString(font, text, x - 0x10, y - 0x10, 0xFFFFF0u, 1.2f, 0.0f);
        Font_DrawString(font, text, x - 0x10, y + 0x10, 0xFFFFF0u, 1.2f, 0.0f);
        Font_DrawString(font, text, x + 0x10, y + 0x10, 0xFFFFF0u, 1.2f, 0.0f);
        Font_DrawString(font, text, x + 0x10, y - 0x10, 0xFFFFF0u, 1.2f, 0.0f);
        Font_SetColor(font, a | 0xCCCCCCu);
        Font_DrawString(font, text, x, y, 0xFFFFF0u, 1.2f, 0.0f);
    }
}
