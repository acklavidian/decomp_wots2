// Decompiled from 0x1001E0 (main), 0x1E09F0 – 0x1E0D58 (App_Run),
// 0x1E0D60 – 0x1E0E58 (App_RenderFrame) and 0x1E0E60 – 0x1E11E8 (App_Init).
//
// The game's real entry point and main loop.  main() initialises the app
// object at 0x2EE960 and hands it to App_Run, which never returns: it runs
// one scene at a time through the scene vtable (Start once, then Update +
// App_RenderFrame every frame) and switches scenes when Update returns 0.
//
// Native port: App_Run becomes the SDL main loop.  The GsDevice_* calls map
// onto the HAL (begin/end frame, clear, swap); sceGsSyncPath and the T0 timer
// writes have no native equivalent.

#include "hal/ps2_types.h"
#include "engine/scene.h"
#include "engine/app.h"
#include "engine/archive_file.h"
#include "engine/font.h"
#include "engine/gs_device.h"
#include "engine/window.h"

struct App {
    u32 scene;        // [0] current scene (Scene*)
    u32 queuedScene;  // [1] successor that has already been started
    u32 bootScreen;   // [2] Mode0 object while App_RunBootScreen runs
    u32 _pad;
    u32 drawTicks;    // [4] T0 count when the frame's packet was sent
    u32 syncTicks;    // [5] T0 count when the GS finished drawing it
};

static Scene* SceneOf(u32 addr) { return guest::ptr<Scene>(addr); }

// ── External functions ─────────────────────────────────────────────────────
struct GsDevice;
extern "C" {
    void  LibcInit();                                   // 0x165640
    void  RunStaticCtors();                             // 0x100260
    void  RunAtExit();                                  // 0x100250

    void* PadMgr_Get();                                 // 0x13A920
    void  PadMgr_Poll(void* mgr);                       // 0x13A7B0
    void  PadMgr_Reset(void* mgr);                      // 0x13A820
    int   Native_EditorPaused();                        // native/editor.cpp

    void  GsDevice_BeginFrame(GsDevice* dev);           // 0x131470
    void  GsDevice_EndFrame(GsDevice* dev);             // 0x131430
    void  GsDevice_ClearScreen(GsDevice* dev);          // 0x12F550  black
    void  GsDevice_ClearScreenColor(GsDevice* dev, u32 rgba);   // 0x12F1F0
    u32   GsDevice_SwapBuffers(GsDevice* dev, u32 mode);        // 0x131240  waits for vsync
    void  GsDevice_SetDisplay(GsDevice* dev, u32 alpha, s16 dx, s32 dy); // 0x12E670
    void  GsDevice_SetVu1Program(GsDevice* dev, u32 id);        // 0x1310A0
    void  GsDevice_SetFogParams(float a, float b, float c, float d,
                                GsDevice* dev, const void* vec);  // 0x12E9D0
    void  GsDevice_SendColorReg(GsDevice* dev, u32 rgba);       // 0x130E70
    void  GsDevice_StoreClearColor(GsDevice* dev, u32 rgba);    // 0x130F30
    void  GsDevice_SetProjection(float scale, float aspectX, float aspectY,
                                 float centerX, float centerY, float zMin, float zMax,
                                 float nearZ, GsDevice* dev, float farZ);  // 0x12F8A0
    void  GsDevice_UpdateMatrices(GsDevice* dev);       // 0x12ECA0
    int   sceGsSyncPath(int mode, u32 timeout);         // 0x104BD0

    void* MemAlloc(u32 size);                           // 0x100820
    void  MemFree(void* p);                             // 0x100650
    void  MemSet(void* dst, int value, u32 size);       // 0x123880
    void  Matrix_Copy(void* dst, const void* src);      // 0x1073C0
    void  Camera_Apply(void* camera, GsDevice* dev);    // 0x1668F0
    void  Bgm_ReapFinished();                    // 0x181110
    void  RenderMgr_DrawEffects(void* mgr);             // 0x14F490
    void  RenderMgr_ClearEffects(void* mgr);            // 0x14FA80
    void  Profiler_Begin(void* p);                      // 0x165730  empty in retail
    void  Profiler_End(void* p);                        // 0x165740  empty in retail

    // App_Init only
    void  App_LoadIopModules();                         // 0x1E21B0  loads the IOP module list
    void  fn_11e8b0();                                  // 0x11E8B0
    void  fn_2658a0();                                  // 0x2658A0  creates the 0x2C0-byte singleton at *0x2EE75C
    int   GetThreadId();                                // syscall
    void  ChangeThreadPriority(int thread, int prio);   // syscall
    void  App_CreateDevices(App* app);                  // 0x1E1BF0  640x448 GsDevice, WindowMgr, …
    void  PadMgr_Init(void* mgr);                       // 0x13A8C0  opens both pad ports
    void  SetDataPath(const char* path);                // 0x142BD0  copies to 0x308A70
    void  fn_1e1370(App* app);                          // 0x1E1370  shows the loading text, then a switch (not recovered)
    void* Mode0_Ctor(void* mem);                        // 0x260EB0  the boot check of the memory card (game/mcard.cpp)
    void  Mode0_Start(void* self);                      // 0x260E20
    int   Mode0_Update(void* self);                     // 0x260100
    void  Mode0_Draw(void* self);                       // 0x262470
    int   Movie_Wanted(const char* name);               // native/movie.cpp
    int   Movie_Play(const char* path);                 // 0x1613D0  blocks until the movie ends; → the pad's buttons then
    void  Rand_Seed(u32 seed);                          // 0x124AF8  libc srand
    void  GsDevice_PresentMode(GsDevice* dev, u32 a, u32 b, u32 on, u32 c);  // 0x131A80

    // Start-up notice screen: a 16-byte object {image, ?, brightness = 0x80, ?}
    void* Notice_Ctor(void* self);                      // 0x2BC7B0
    int   Notice_Load(void* self);                      // 0x2BC6C0  → 0 if there is nothing to show
    int   Notice_Update(void* self);                    // 0x2BC590  → non-zero when finished
    void  Notice_Draw(void* self);                      // 0x2BC4B0
    void  Notice_Dtor(void* self, int flag);            // 0x2BC750
}

// ── Globals ───────────────────────────────────────────────────────────────
static App* const       gApp           = reinterpret_cast<App*>(0x2EE960u);
static const GuestPtrVar<GsDevice> gGsDevice{0x2EE670u};
static const GuestPtrVar<void> gCamera{0x2EE678u};
static const GuestPtrVar<Font> gFont{0x2EE67Cu};
static const GuestPtrVar<WindowMgr> gWindowMgr{0x2EE680u};
static const GuestPtrVar<void> gRenderMgrPtr{0x2EE684u};
static u32* const       gClearColor    = reinterpret_cast<u32*>(0x2EE6A8u);
static u32* const       gFrameCount    = reinterpret_cast<u32*>(0x2EE6ACu);
static const GuestPtrVar<void> gProfiler{0x2EE6B0u};
static u32* const       gLoadingActive = reinterpret_cast<u32*>(0x2EE6B4u);
static s32* const       gLoadingDotTimer = reinterpret_cast<s32*>(0x2EE6B8u);
static s32* const       gLoadingDots   = reinterpret_cast<s32*>(0x2EE6BCu);
static s32* const       gLoadingFrames = reinterpret_cast<s32*>(0x2EE6C0u);
static const u32* const gFrameRate     = reinterpret_cast<const u32*>(0x2EDA00u);  // 25 = PAL

enum { kAssetFontTexture = 7, kAssetFontGlyphs = 8 };   // Font/Samu2.Pim, Font/Samu2.Fnt
enum { kTextLoading = 0x17B };                          // menutext.txt line

static volatile u32& T0_COUNT = *reinterpret_cast<volatile u32*>(0x10000000u);

// Default render state re-applied at the start of every scene.
static const float* const kFogParams   = reinterpret_cast<const float*>(0x2D9460u); // 64, 255, 20, 64
static const void*  const kFogVec      = reinterpret_cast<const void*>(0x2D9470u);  // (0, 0, 1, 0)
static const float* const kFarZ        = reinterpret_cast<const float*>(0x2EDD28u); // 64.0
static const float* const kNearZ       = reinterpret_cast<const float*>(0x2EDD2Cu); // 0.1
static const float* const kAspect      = reinterpret_cast<const float*>(0x2EDD30u); // 1.0
static const void*  const kIdentityMtx = reinterpret_cast<const void*>(0x2D1A80u);

static const u32 kBlack = 0x80000000u;

// Two black frames, one per display buffer.
void App_PresentBlackFrames() {
    GsDevice* dev = *gGsDevice;
    for (int i = 0; i < 2; i++) {
        GsDevice_BeginFrame(dev);
        GsDevice_ClearScreen(dev);
        GsDevice_EndFrame(dev);
        sceGsSyncPath(0, 0);
        GsDevice_SwapBuffers(dev, 0);
    }
}

// Inlined three times in App_Run: destroy the current scene and install `next`,
// or the scene it names by id when `next` is null.
static void ChangeScene(App* app, u32 next) {
    PadMgr_Poll(PadMgr_Get());
    PadMgr_Reset(PadMgr_Get());

    s32 nextId = -1;
    if (app->scene) {
        nextId = SceneCall<s32>(SceneOf(app->scene), kSceneGetNextSceneId);
        if (app->scene)
            SceneCall<void>(SceneOf(app->scene), kSceneDestroy, 1);
    }
    if (next)
        app->scene = next;
    else
        app->scene = (nextId < 0) ? 0 : guest::addr(Scene_Create(nextId));
}

static void App_DrawLoadingText(App* app);
static int  App_LoadFont(App* app);

// ── 0x1E0D60 App_RenderFrame ──────────────────────────────────────────────
int App_RenderFrame(App* app);
int App_RenderFrame(App* app) {
    GsDevice* dev = *gGsDevice;

    GsDevice_ClearScreenColor(dev, *gClearColor);
    GsDevice_SetVu1Program(dev, 1);
    Camera_Apply(*gCamera, dev);

    int result = SceneCall<int>(SceneOf(app->scene), kSceneDraw);

    WindowMgr_Draw(*gWindowMgr, dev, *gFont);
    RenderMgr_DrawEffects(*gRenderMgrPtr);
    if (*gLoadingActive)
        App_DrawLoadingText(app);

    Profiler_End(*gProfiler);
    GsDevice_EndFrame(dev);
    app->drawTicks = T0_COUNT;
    Profiler_Begin(*gProfiler);
    sceGsSyncPath(0, 0);
    app->syncTicks = T0_COUNT;

    GsDevice_SwapBuffers(dev, 0);
    GsDevice_SetDisplay(dev, 0x80, 1, 1);
    ++*gFrameCount;
    return result;
}

// ── 0x1E09F0 App_Run ──────────────────────────────────────────────────────
[[noreturn]] void App_Run(App* app) {
    for (;;) {
        for (;;) {
            GsDevice* dev = *gGsDevice;
            int startFailed;

            if (app->queuedScene) {
                // This scene was already started while its predecessor ran.
                app->queuedScene = 0;
                startFailed = 0;
            } else {
                startFailed = SceneCall<int>(SceneOf(app->scene), kSceneStart);
                GsDevice_SetFogParams(kFogParams[0], kFogParams[1], kFogParams[2], kFogParams[3],
                                      dev, kFogVec);
                GsDevice_SendColorReg(dev, *gClearColor);
                GsDevice_StoreClearColor(dev, *gClearColor);
                GsDevice_SetProjection(-1.0f, *kAspect, *kAspect, 0.0f, 0.0f,
                                       256.0f, 16773120.0f, *kNearZ, dev, *kFarZ);
                GsDevice_UpdateMatrices(dev);
            }

            if (!startFailed) {
                // ── Frame loop ────────────────────────────────────────────
                for (;;) {
                    PadMgr_Poll(PadMgr_Get());
                    GsDevice_BeginFrame(dev);
                    T0_COUNT = 0;
                    Bgm_ReapFinished();
                    WindowMgr_Update(*gWindowMgr);

                    if (SceneOf(app->scene)->hasQueuedScene && !app->queuedScene) {
                        app->queuedScene = guest::addr(SceneCall<Scene*>(SceneOf(app->scene), kSceneGetNextScene));
                        SceneCall<int>(SceneOf(app->queuedScene), kSceneStart);
                    }

                    // (Native: the level editor holds the game still.)
                    if (!Native_EditorPaused() && !SceneCall<int>(SceneOf(app->scene), kSceneUpdate))
                        break;
                    App_RenderFrame(app);
                }
                App_PresentBlackFrames();
            }

            // ── Scene teardown ────────────────────────────────────────────
            PadMgr_Poll(PadMgr_Get());
            PadMgr_Reset(PadMgr_Get());
            RenderMgr_ClearEffects(*gRenderMgrPtr);
            WindowMgr_Clear(*gWindowMgr);
            Matrix_Copy(reinterpret_cast<u8*>(dev) + 0x360, kIdentityMtx);  // world-view matrix
            *gClearColor = kBlack;

            if (SceneOf(app->scene)->hasQueuedScene)
                break;
            ChangeScene(app, guest::addr(SceneCall<Scene*>(SceneOf(app->scene), kSceneGetNextScene)));
        }
        // queuedScene stays set so the next pass skips Start.
        ChangeScene(app, app->queuedScene);
    }
}

// ── 0x1E0E60 App_Init ─────────────────────────────────────────────────────
// Movie path tables: [ACQUIRE, SPIKE, OPENING, STAFF0..3, PLAYDEMO] per region.
// (32-bit string pointers.)
static const u32* const kMoviesNtsc = reinterpret_cast<const u32*>(0x2D1B20u);
static const u32* const kMoviesPal  = reinterpret_cast<const u32*>(0x2D1B40u);
static const u32* const gDisplayArg  = reinterpret_cast<const u32*>(0x2EE4C8u);
static u32* const       gInitFlag    = reinterpret_cast<u32*>(0x2EE6C4u);

// ── 0x1E11F0 ──────────────────────────────────────────────────────────────
// The memory card is looked at before anything is shown: is there a card, is
// there a save file on it — and if not, shall one be made?
static void App_RunBootScreen(App* app) {
    // (Native: a run steered by debug hooks goes without it, like the movies.)
    if (!Movie_Wanted("CARDCHECK"))
        return;
    GsDevice* dev = *gGsDevice;
    void* mem = MemAlloc(0x150E8);
    app->bootScreen = mem ? guest::addr(Mode0_Ctor(mem)) : 0;
    void* screen = guest::ptr(app->bootScreen);
    Mode0_Start(screen);
    int more;
    do {
        GsDevice_BeginFrame(dev);
        PadMgr_Poll(PadMgr_Get());
        WindowMgr_Update(*gWindowMgr);
        more = Mode0_Update(screen);
        GsDevice_ClearScreenColor(dev, *gClearColor);
        GsDevice_SetVu1Program(dev, 1);
        Mode0_Draw(screen);
        WindowMgr_Draw(*gWindowMgr, dev, *gFont);
        Profiler_End(*gProfiler);
        GsDevice_EndFrame(dev);
        Profiler_Begin(*gProfiler);
        sceGsSyncPath(0, 0);
        GsDevice_SwapBuffers(dev, 0);
        GsDevice_SetDisplay(dev, 0x80, 1, 1);
    } while (more != 0);
    if (app->bootScreen != 0)
        VirtualCall<void, void*, int>(*guest::ptr<u32>(app->bootScreen), 8, screen, 1);
    app->bootScreen = 0;
    for (int i = 0; i < 4; i++) {
        GsDevice_BeginFrame(dev);
        GsDevice_ClearScreen(dev);
        GsDevice_EndFrame(dev);
        sceGsSyncPath(0, 0);
        GsDevice_SwapBuffers(dev, 0);
        GsDevice_SetDisplay(dev, 0x80, 1, 1);
    }
}

int App_Init(App* app) {
    App_LoadIopModules();
    fn_11e8b0();
    fn_2658a0();
    ChangeThreadPriority(GetThreadId(), 32);
    App_CreateDevices(app);
    PadMgr_Init(PadMgr_Get());
    *gInitFlag = 1;
    SetDataPath(reinterpret_cast<const char*>(0x2E44C0u));  // "Us/"
    App_LoadFont(app);
    TextTable_Load(TextTable_Get());
    fn_1e1370(app);
    App_RunBootScreen(app);

    GsDevice* dev = *gGsDevice;

    // Start-up notice screen, shown until it reports it is finished.
    u32 notice[4] = {};
    Notice_Ctor(notice);
    if (Notice_Load(notice)) {
        while (!Notice_Update(notice)) {
            PadMgr_Poll(PadMgr_Get());
            GsDevice_BeginFrame(dev);
            Notice_Draw(notice);
            GsDevice_EndFrame(dev);
            sceGsSyncPath(0, 0);
            GsDevice_SwapBuffers(dev, 0);
            GsDevice_SetDisplay(dev, 0x80, 0, 1);
        }
    }

    // Publisher and developer logo movies.
    App_PresentBlackFrames();
    GsDevice_PresentMode(dev, 1, *gDisplayArg, 1, 0);
    App_PresentBlackFrames();
    const u32* movies = (*gFrameRate == 25) ? kMoviesPal : kMoviesNtsc;
    Movie_Play(guest::ptr<const char>(movies[1]));   // SPIKE.PSS
    Movie_Play(guest::ptr<const char>(movies[0]));   // ACQUIRE.PSS
    App_PresentBlackFrames();
    GsDevice_PresentMode(dev, 1, *gDisplayArg, 0, 0);
    App_PresentBlackFrames();

    Rand_Seed(T0_COUNT * 2);

    ChangeScene(app, guest::addr(Scene_Create(kSceneBoot)));
    Notice_Dtor(notice, -1);
    return 0;
}

// ── 0x1001E0 main ─────────────────────────────────────────────────────────
// Named GameMain here because the native executable has its own main().
int GameMain() {
    LibcInit();
    RunStaticCtors();
    App_Init(gApp);
    App_Run(gApp);
    RunAtExit();   // unreachable in practice
    return 0;
}

// ── 0x1E1FD0 App_LoadFont ─────────────────────────────────────────────────
// Creates the font from Font/Samu2.Pim (textures) and Font/Samu2.Fnt (glyphs).
static int App_LoadFont(App*) {
    if (!*gGsDevice)
        return 0;

    Font* font = static_cast<Font*>(MemAlloc(sizeof(Font)));
    if (font)
        font = Font_Ctor(font, *gGsDevice);
    *guest::ptr<u32>(gFont.address) = guest::addr(font);

    ArchiveFile* file = ArchiveFile_Create();
    if (!file)
        return 0;
    ArchiveFile_Open(file, kAssetFontTexture);
    u32 size = ArchiveFile_GetSize(file);
    void* pim = MemAlloc(size);
    ArchiveFile_Read(file, pim, size);
    ArchiveFile_Destroy(file);
    Font_SetTexture(*gFont, pim);
    MemFree(pim);

    file = ArchiveFile_Create();
    if (!file)
        return 0;
    ArchiveFile_Open(file, kAssetFontGlyphs);
    Font_SetGlyphs(*gFont, static_cast<const u16*>(ArchiveFile_ReadAll(file, 0)));
    ArchiveFile_Destroy(file);
    return 1;
}

// ── 0x1E0770 App_DrawLoadingText ──────────────────────────────────────────
// Once loading has lasted a second, draws the "loading" string in the bottom
// left corner with up to six dots that grow three times a second.
static void App_DrawLoadingText(App*) {
    bool visible = (s32)*gFrameRate <= *gLoadingFrames;
    (*gLoadingFrames)++;
    if (!visible)
        return;

    u16 text[128];
    MemSet(text, 0, sizeof(text));
    TextTable* table = TextTable_Get();
    if (!table)
        return;

    int length = WStr_Length(TextTable_Find(table, kTextLoading));
    WStr_Copy(text, TextTable_Find(table, kTextLoading));
    for (int i = 0; i < *gLoadingDots; i++)
        text[length + i] = '.';

    Font_SetColor(*gFont, 0xFF808080u);
    s16 x = (s16)Screen_X(36 * 16, -1);
    s16 y = (s16)Screen_Y(-48 * 16, 1);
    Font_DrawString(*gFont, text, x, y, 0xFFFFF0, 1.0f, 0.0f);

    bool addDot = (s32)*gFrameRate / 3 <= *gLoadingDotTimer;
    (*gLoadingDotTimer)++;
    if (addDot) {
        *gLoadingDotTimer = 0;
        bool wrap = *gLoadingDots > 5;
        (*gLoadingDots)++;
        if (wrap)
            *gLoadingDots = 0;
    }
}
