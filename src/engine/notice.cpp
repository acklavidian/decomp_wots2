// The publisher's logo at start-up — decompiled from 0x2BC4B0–0x2BC7D0.
//
// `Common/CAPCOM.bpm` (asset 0x1C) in the middle of a white screen: it comes
// up out of black (a veil whose alpha falls 120 / frame rate a frame), stays
// for three seconds or until Start, and goes back to black (60 / frame rate
// a frame).  App_Init runs it before the logo movies.

#include "engine/pad.h"
#include "native/guest.h"

#include <cstring>

extern "C" {
    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void* SpriteSet_Ctor(void* mem);                    // 0x17C630
    char  SpriteSet_Load(void* set, int asset, int asset2);     // 0x17C430
    int   SpriteSet_IsLoaded(void* set);                // 0x17C090
    int   SpriteSet_Bind(void* set);                    // 0x17BFD0
    void* SpriteSet_GetSprite(void* set, int index);    // 0x17BDF0
    void  SpriteSet_Draw(void* set, int index, int x, int y, int z, u32 rgba);  // 0x17B780
    void  SpriteSet_Dtor(void* set, int flag);          // 0x17C570
    u32   Screen_X(int v, int align);                   // 0x12E5E0
    u32   Screen_Y(int v, int align);                   // 0x12E550
    void  GsDevice_ClearScreenColor(void* dev, u32 rgba);       // 0x12F1F0
    void* PadMgr_Get();                                 // 0x13A920
    u16   Pad_GetPressed(void* mgr, int port);          // 0x13A0C0
    int   Movie_Wanted(const char* name);               // native/movie.cpp
}

namespace {

struct Notice {
    u32 sprites;        // SpriteSet*
    s32 state;          // 0 coming up, 1 shown, 2 going
    s32 veil;           // the black over it, 0x80 = all black
    s32 frames;
};
const s32* const gFrameRate = guest::ptr<s32>(0x2EDA00u);
const u32* const gGsDevice = guest::ptr<u32>(0x2EE670u);

}  // namespace

extern "C" {

void* Notice_Ctor(void* mem) {                          // 0x2BC7B0
    Notice* self = static_cast<Notice*>(mem);
    self->sprites = 0;
    self->state = 0;
    self->veil = 0x80;
    self->frames = 0;
    return self;
}

// → 0 if there is nothing to show.
int Notice_Load(void* mem) {                            // 0x2BC6C0
    Notice* self = static_cast<Notice*>(mem);
    // (Native: a run steered by debug hooks goes without it, like the movies,
    // so that scripted input keeps its frame numbers.)
    if (!Movie_Wanted("CAPCOM"))
        return 0;
    void* set = MemAlloc(0x14);
    if (set != nullptr)
        set = SpriteSet_Ctor(set);
    self->sprites = guest::addr(set);
    if (set == nullptr || SpriteSet_Load(set, 0x1C, 0) == 0)
        return 0;
    while (SpriteSet_IsLoaded(set) == 0) {
    }
    return 1;
}

// → non-zero when it is over.
int Notice_Update(void* mem) {                          // 0x2BC590
    Notice* self = static_cast<Notice*>(mem);
    void* pads = PadMgr_Get();
    if (self->state == 2) {
        self->veil += 0x3C / *gFrameRate;
        if (self->veil > 0x7F) {
            self->veil = 0x80;
            return 1;
        }
    } else if (self->state == 1) {
        self->frames++;
        if (self->frames >= *gFrameRate * 3 || (Pad_GetPressed(pads, 0) & kPadStart) != 0)
            self->state = 2;
    } else if (self->state == 0) {
        self->veil -= 0x78 / *gFrameRate;
        if (self->veil < 1) {
            self->veil = 0;
            self->state = 1;
        }
    }
    return 0;
}

void Notice_Draw(void* mem) {                           // 0x2BC4B0
    Notice* self = static_cast<Notice*>(mem);
    const u32 x = Screen_X(0, 0), y = Screen_Y(0, 0);
    void* dev = guest::ptr(*gGsDevice);
    void* set = guest::ptr(self->sprites);
    if (SpriteSet_Bind(set) == 0)
        return;
    GsDevice_ClearScreenColor(dev, 0x80FFFFFFu);
    if (const u8* sprite = static_cast<const u8*>(SpriteSet_GetSprite(set, 0))) {
        u16 w, h;
        memcpy(&w, sprite + 0x0C, 2);
        memcpy(&h, sprite + 0x0E, 2);
        SpriteSet_Draw(set, 0, (int)((x & 0xFFFF) - w * 8u), (int)((y & 0xFFFF) - h * 8u), 0, 0x80808080u);
    }
    GsDevice_ClearScreenColor(dev, (u32)(u8)self->veil << 24);
}

void Notice_Dtor(void* mem, int flag) {                 // 0x2BC750
    Notice* self = static_cast<Notice*>(mem);
    if (self == nullptr)
        return;
    if (self->sprites != 0)
        SpriteSet_Dtor(guest::ptr(self->sprites), 1);
    self->sprites = 0;
    if ((s16)flag > 0)
        MemFree(self);
}

}  // extern "C"
