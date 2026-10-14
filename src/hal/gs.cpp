#define GL_GLEXT_PROTOTYPES
#include "hal/gs.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include "hal/gl_loader.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <algorithm>

// ── How a frame gets to the screen ─────────────────────────────────────────
// The game draws in immediate mode into a 640 × 448 frame, thirty times a
// second.  Here every draw is first *recorded* (a list of commands with their
// vertices, in the game's own coordinates) and the list is *played* when the
// frame ends.  Played as it is into a 640 × 448 frame that is the picture
// there always was.  But the same list can be played
//   - into a frame of any size (more lines: sharper),
//   - into a wider frame, the 3D view opened out to the sides and the game's
//     640-wide screen in the middle of it,
//   - and more than once: between two frames of the game, with every vertex
//     and view matrix part of the way from the frame before (a draw is paired
//     with the same draw of the frame before; whatever jumped or has no pair
//     is drawn where it now is) — a higher frame rate without the game
//     running any faster.
// hal::gs::Video holds the three choices; mods set them.

static SDL_Window*   s_window   = nullptr;
static SDL_GLContext s_gl_ctx   = nullptr;
static int           s_width    = 640;      // the game's screen
static int           s_height   = 448;
static int           s_fps      = 30;
static int           s_frame    = 0;

static hal::gs::Video s_video;
static int   s_physW = 640, s_physH = 448;  // the frame that is drawn into
static float s_lw = 640.0f, s_xoff = 0.0f;  // its width in the game's pixels; the game's screen starts at s_xoff

static GLuint s_program = 0, s_vao = 0, s_vbo = 0, s_white = 0;
static GLuint s_lastFbo = 0, s_lastTex = 0;   // the frame shown last (the PS2's other buffer): read_frame
static GLuint s_fbo = 0, s_fboTex = 0;   // the frame the game's draws are played into
static GLuint s_grabTex = 0;
static GLuint s_distTex = 0;            // the frame's second colour buffer, distances (R32F): the modern lighting's
static int    s_distW = 0, s_distH = 0;
static int    s_allocW = 0, s_allocH = 0, s_grabW = 0, s_grabH = 0;
static GLint  s_uScreen = -1, s_uAlphaRef = -1, s_uDepth = -1, s_uXoff = -1, s_uUvRect = -1;
static const u32 kNoZ = 0xFFFFFFFFu;
static float  s_alphaRef = 0.0f;

struct Vertex { float x, y, u, v, r, g, b, a; };

static const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform vec2 uScreen;
uniform float uXoff;
out vec2 vUv;
out vec4 vColor;
void main() {
    vUv = aUv;
    vColor = aColor;
    // y grows downwards in the frame; row 0 of the framebuffer is the top line.
    gl_Position = vec4((aPos.x + uXoff) / uScreen.x * 2.0 - 1.0, aPos.y / uScreen.y * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* kFragmentShader = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
uniform float uAlphaRef;
uniform float uDepth;
uniform vec4 uUvRect;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, clamp(vUv, uUvRect.xy, uUvRect.zw)) * vColor;
    if (c.a < uAlphaRef) discard;
    oColor = clamp(c, 0.0, 1.0);
    gl_FragDepth = uDepth;
}
)";

// The 3D fragment stage adds the PS2's depth fog: the colour goes toward the
// fog colour by a coefficient A + B / depth (1 = clear), and what lies beyond
// the far distance is not drawn (the game fades it out over the last 4 units).
static const char* kFragmentShader3d = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
in float vDepth;
in vec3 vWorld;
uniform sampler2D uTex;
uniform float uAlphaRef;
uniform vec4 uFog;          // A, B, far distance, on
uniform vec3 uFogColor;
// Modern lighting (hal::gs::Lighting): 0 none, 2 the map, 3 a model.
uniform int uKind;
// A cut-out texture in the pass that writes depth: 1 = the filtered alpha
// only says where it is cut, and what is left is drawn opaque; 2 = then only
// the soft fringe outside that, blended, writing no depth.
uniform int uCutout;
uniform mat4 uShadowMat;
uniform sampler2DShadow uShadow;
uniform vec4 uShadowParams; // strength, on, texel, share on models
uniform int uLights;
// Bump mapping (a prototype): the map's textures read as heights by their
// brightness; xyz = towards the sun (its strength in w), uBump.x = strength.
uniform vec4 uSunDir;
uniform vec4 uBump;
// Un-baking (a prototype): the map relit instead of its painted-in light.
// x = how much (0 baked … 1 relit), y = exposure, z = how much of the sun a
// shadow takes (the shadow strength: its default 0.55 = all); the map's
// ambient and sun.
uniform vec4 uUnbake;
uniform vec3 uAmbient;
uniform vec3 uSunCol;
uniform vec3 uEye;
uniform vec4 uLightPos[16]; // xyz, radius
uniform vec3 uLightCol[16];
layout(location = 0) out vec4 oColor;
// (While the modern lighting's occlusion is on: the distance from the eye,
// which — unlike the depth buffer — no depth bias has moved.)
layout(location = 1) out vec4 oDist;
// (A model is given more leeway than the map: drivers differ in how far the
// casters' depth offset reaches, and a character darkening its own body in
// stripes is worse than one that casts no shadow on itself.)
// How much brighter (> 1) or darker the bumps make a pixel of the map.  The
// map's lighting is baked into its colours, so the bumps only nudge it: the
// light a bumped surface would get from the sun and from the sky above, over
// what the flat one gets.  No normals on the map: the face's own comes from
// the screen-space derivatives of its world position, its tangents from
// those of the texture coordinates (a cotangent frame).
float bump_light(vec2 uv) {
    vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 n = normalize(cross(dp1, dp2));
    if (dot(n, n) < 0.5) return 1.0;
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    if (isinf(invmax) || isnan(invmax)) return 1.0;
    T *= invmax;
    B *= invmax;
    // Height = brightness; its slope from the texels round this one.
    vec2 texel = 1.5 / vec2(textureSize(uTex, 0));     // (one texel apart: gritty)
    const vec3 lum = vec3(0.3, 0.59, 0.11);
    float hl = dot(texture(uTex, uv - vec2(texel.x, 0.0)).rgb, lum);
    float hr = dot(texture(uTex, uv + vec2(texel.x, 0.0)).rgb, lum);
    float hd = dot(texture(uTex, uv - vec2(0.0, texel.y)).rgb, lum);
    float hu = dot(texture(uTex, uv + vec2(0.0, texel.y)).rgb, lum);
    vec2 slope = vec2(hr - hl, hu - hd) * uBump.y / 1.5;
    vec3 nb = normalize(n - (T * slope.x + B * slope.y));
    vec3 L = normalize(uSunDir.xyz);
    vec3 up = vec3(0.0, 1.0, 0.0);
    float d = (max(dot(nb, L), 0.0) - max(dot(n, L), 0.0)) * uSunDir.w
            + (dot(nb, up) - dot(n, up)) * 0.35;
    return clamp(1.0 + d * uBump.x, 0.55, 1.45);
}
float in_shadow(vec3 p, float bias) {
    vec3 q = (uShadowMat * vec4(p, 1.0)).xyz * 0.5 + 0.5;
    if (q.x <= 0.0 || q.x >= 1.0 || q.y <= 0.0 || q.y >= 1.0 || q.z >= 1.0)
        return 0.0;
    float edge = clamp(min(min(q.x, 1.0 - q.x), min(q.y, 1.0 - q.y)) * 12.0, 0.0, 1.0);
    float lit = 0.0;
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++)
            lit += texture(uShadow, vec3(q.xy + vec2(i, j) * uShadowParams.z, q.z - bias));
    return (1.0 - lit / 9.0) * edge;
}
void main() {
    // (Before any discard: derivatives need every pixel of the quad.)
    float bumped = uKind == 2 && uBump.x > 0.0 ? bump_light(vUv) : 1.0;
    vec3 faceN = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    if (dot(faceN, uEye - vWorld) < 0.0) faceN = -faceN;       // (the side that is seen)
    vec4 t = texture(uTex, vUv);
    vec4 c = t * vColor;
    if (c.a < uAlphaRef) discard;
    if (uCutout == 1) c.a = vColor.a;
    else if (uCutout == 2 && t.a >= 0.5) discard;
    c = clamp(c, 0.0, 1.0);
    if (uKind >= 2) {
        c.rgb *= bumped;
        float sh = uShadowParams.y > 0.5 ? in_shadow(vWorld, uKind == 3 ? 0.004 : 0.0008) : 0.0;
        if (uShadowParams.y > 0.5) {
            float lum = dot(c.rgb, vec3(0.3, 0.59, 0.11));
            float k = uShadowParams.x * smoothstep(0.03, 0.3, lum) * (uKind == 3 ? uShadowParams.w : 1.0);
            c.rgb *= 1.0 - k * sh;
        }
        if (uKind == 2 && uUnbake.x > 0.0) {
            // The vertex colour without its brightness: the surface's own tint.
            float top = max(vColor.r, max(vColor.g, vColor.b));
            vec3 tint = vColor.rgb / max(top, 0.04);
            float ndl = max(dot(faceN, normalize(uSunDir.xyz)), 0.0);
            vec3 light = uAmbient + uSunCol * ndl * (1.0 - sh * uUnbake.z);
            vec3 relit = clamp(t.rgb * tint * light * uUnbake.y, 0.0, 1.0) * bumped;
            c.rgb = mix(c.rgb, relit, uUnbake.x);
        }
        if (uKind == 2)
            for (int i = 0; i < uLights; i++) {
                vec3 d = vWorld - uLightPos[i].xyz;
                float a = clamp(1.0 - dot(d, d) / (uLightPos[i].w * uLightPos[i].w), 0.0, 1.0);
                c.rgb += t.rgb * uLightCol[i] * a * a;
            }
        c.rgb = clamp(c.rgb, 0.0, 1.0);
    }
    if (uFog.w > 0.5) {
        if (vDepth > uFog.z) discard;
        float clear = clamp(uFog.x + uFog.y / max(vDepth, 0.001), 0.0, 1.0);
        c.rgb = mix(uFogColor, c.rgb, clear);
        c.a *= clamp((uFog.z - vDepth) / 4.0, 0.0, 1.0) > 0.0 ? 1.0 : 0.0;
    }
    oColor = c;
    oDist = vec4(vDepth);
}
)";

// 3D program: positions through a matrix.
static GLuint s_program3d = 0, s_vao3d = 0, s_vbo3d = 0, s_depth = 0;
static GLint  s_uMvp = -1, s_uAlphaRef3d = -1, s_uFog = -1, s_uFogColor = -1, s_uXScale = -1;
static float  s_fog[4] = {0, 0, 0, 0}, s_fogColor[3] = {0, 0, 0};
struct Vertex3 { float x, y, z, u, v, r, g, b, a; };

static const char* kVertexShader3d = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform mat4 uMvp;
uniform float uXScale;      // a wider picture: the same view, more of it to the sides
out vec2 vUv;
out vec4 vColor;
out float vDepth;
out vec3 vWorld;
void main() {
    vUv = aUv;
    vColor = aColor;
    vWorld = aPos;
    gl_Position = uMvp * vec4(aPos, 1.0);
    vDepth = gl_Position.w;             // distance in front of the viewer
    gl_Position.x *= uXScale;
    gl_Position.y = -gl_Position.y;     // row 0 of the frame is the top line
}
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "[GS] shader error: %s\n", log);
    }
    return sh;
}

// GS colour (0x80 = 1.0) → float.
// GS colours.  A textured pixel is texel × colour with 0x80 meaning 1.0; an
// untextured one takes its RGB as it is (0xFF = full).  Alpha is 0x80 = 1.0
// either way.
static void unpack(u32 rgba, float out[4], bool textured = true) {
    const float scale = textured ? 128.0f : 255.0f;
    out[0] = (rgba & 0xFF) / scale;
    out[1] = ((rgba >> 8) & 0xFF) / scale;
    out[2] = ((rgba >> 16) & 0xFF) / scale;
    out[3] = ((rgba >> 24) & 0xFF) / 128.0f;
}


// ── The recorded frame ─────────────────────────────────────────────────────
enum class Op : u8 { FrameBegin, Clear, Blend, AlphaRef, Draw2D, DrawDepth, Begin3D, End3D, Mesh,
                     Fog, Bias, Overlay, Grab, TargetBegin, TargetEnd, Post };
enum : u8 { kHasZ = 1, kUvRect = 2, kFrameTex = 4, kSpan = 8 };
struct Cmd {
    Op  op;
    u8  prim = 0, flags = 0;
    bool lerp = false, lerpUv = false;  // (played in between) its vertices go part of the way from `match`
    u32 tex = 0, z = 0, first = 0;
    int count = 0, i = 0;
    int owner = -1;                     // a mesh: its Begin3D
    int match = -1;                     // the same draw in the frame before
    u32 who = 0;                        // Begin3D / Mesh: whose view it is (begin_3d's `who`; 0 = not said)
    float f[16] = {};                   // Begin3D: its matrix; Mesh: its box, lo xyz then hi xyz
};
// The world's view, sun and point lights of a frame (set_world), for the
// modern lighting.
struct WorldView {
    bool set = false;
    bool ambience = false, indoors = false, night = false;
    float wind[3] = {}, focus[3] = {};
    float ambient[3] = {1, 1, 1};
    float view[16], proj[16], sun[3], sunColour[3];
    std::vector<float> lights;          // n × {x, y, z, radius, r, g, b, 0}
};
struct Frame {
    std::vector<Cmd> cmds;
    std::vector<Vertex> v2;
    std::vector<Vertex3> v3;
    std::unordered_map<u64, std::vector<u32>> index;    // draws by what they are, in order
    WorldView world;
    int number = 0;                     // which game frame (for the dust's clock)
    void clear() {
        cmds.clear(); v2.clear(); v3.clear(); index.clear();
        world.set = world.ambience = false;
        world.lights.clear();
    }
};
static Frame s_cur, s_prev;
static std::vector<u32> s_dead;         // textures to delete once the frame has been played
// The textures handed out and not yet given back.  A texture given back twice
// would delete whatever has been made under its number since (GL reuses
// them) — a map's wall, say, drawn black from then on; and a draw with one
// given back draws black.  Both are refused and told once.
static std::set<u32> s_live;
// Textures whose every texel is either clear or opaque (grass, leaves,
// fences).  Drawn with smooth filtering their edges come out part clear; blended
// there while writing depth, an edge took the colour of whatever had been drawn
// before it — often the clear colour, the fence behind not yet drawn — and
// kept it, since what lay behind could no longer draw there: a pale outline
// round every blade of grass (user's report, the magistrate's yard).
static std::set<u32> s_cutout;
static bool s_overlayOn = false;
static void check_live(u32 tex, const char* what) {
    static std::set<u32> told;
    if (tex != 0 && !s_live.count(tex) && told.insert(tex).second)
        fprintf(stderr, "[GS] %s texture %u, which was given back (frame %d)\n", what, tex, s_frame);
}

// What the recording side knows of the state that outlives a frame (a list
// starts by putting it back, so that it can be played twice).
static float s_recClear[4] = {0, 0, 0, 1};
static int   s_recBlend = 0, s_recAlphaRef = 0;
static float s_recFog[4] = {0, 0, 0, 0}, s_recFogColor[3] = {0, 0, 0}, s_recFogBeforeTarget = 0;
static u32   s_recZ = kNoZ;
static bool  s_recWide = false;
static int   s_recOwner = -1;
static u32   s_recWho = 0;

static GLuint s_targetFbo = 0, s_targetTex = 0, s_targetDepth = 0;
static int s_targetSize = 0;
static float s_fogBeforeTarget = 0.0f;
static bool s_inTarget = false;

static Cmd& push(Op op) {
    s_cur.cmds.emplace_back();
    s_cur.cmds.back().op = op;
    return s_cur.cmds.back();
}
static GLenum gl_prim(u8 prim) {
    using hal::gs::Prim;
    return (Prim)prim == Prim::Lines ? GL_LINES : (Prim)prim == Prim::Triangles ? GL_TRIANGLES
         : (Prim)prim == Prim::TriangleFan ? GL_TRIANGLE_FAN : GL_TRIANGLE_STRIP;
}
static void apply_blend(int mode) {
    using hal::gs::Blend;
    switch ((Blend)mode) {
    case Blend::Normal:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case Blend::Additive:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    case Blend::Subtractive:
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    }
}
static void set_filter(GLuint tex) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// The shape and size of the frame to draw into, from what was asked for and
// the window; its buffers are made again when they change.
static void size_frame() {
    int winW = 0, winH = 0;
    SDL_GL_GetDrawableSize(s_window, &winW, &winH);
    if (winW < 1 || winH < 1)
        winW = s_width * 2, winH = s_width * 2 * 3 / 4;
    float aspect = 4.0f / 3.0f;
    if (s_video.aspect > 0.0f)
        aspect = s_video.aspect;
    else if (s_video.aspect < 0.0f)
        aspect = (float)winW / (float)winH;
    aspect = aspect < 4.0f / 3.0f ? 4.0f / 3.0f : aspect > 3.6f ? 3.6f : aspect;
    s_lw = (float)s_width * aspect / (4.0f / 3.0f);
    s_xoff = (s_lw - (float)s_width) / 2.0f;
    int h = s_video.height;
    if (h <= 0) {
        h = winH;
        if (s_video.aspect > 0.0f && (float)winW / aspect < (float)h)
            h = (int)((float)winW / aspect);
    }
    h = h < 224 ? 224 : h > 4320 ? 4320 : h;
    // (Without a shape asked for, the frame keeps the PS2's: 640 × 448, shown as 4:3.)
    int w = s_video.aspect == 0.0f ? (int)lroundf((float)h * (float)s_width / (float)s_height)
                                   : (int)lroundf((float)h * aspect);
    if (w == s_allocW && h == s_allocH)
        return;
    s_physW = s_allocW = w;
    s_physH = s_allocH = h;
    glBindTexture(GL_TEXTURE_2D, s_fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, s_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    if (s_distTex != 0) {
        s_distW = w;
        s_distH = h;
        glBindTexture(GL_TEXTURE_2D, s_distTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    }
    if (s_lastTex != 0) {
        glBindTexture(GL_TEXTURE_2D, s_lastTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    if (s_frame > 0)
        printf("[GS] frame %d x %d (%.0f x %d of the game's pixels)\n", w, h, s_lw, s_height);
}

// ── Modern lighting (hal::gs::Lighting) ──────────────────────────────────
// None of it is the PS2's.  It is played on top of the recorded frame, and
// only in a frame that says where the world's camera, sun and lights are
// (set_world, from the map's draw):
//   - before the list is played, everything drawn as a Model (characters,
//     swords, objects) is drawn once more, depth only, as seen from the sun:
//     a shadow map over the middle of the view.  The map and the models then
//     look it up per pixel and darken where it is in shadow — less where the
//     baked lighting has made them dark already;
//   - the map's point lights add a pool of light round them on the map;
//   - when the world is drawn (world_post), ambient occlusion is worked out
//     from the depth buffer (normals from the depths round a pixel, twelve
//     samples in a disc: McGuire's "scalable ambient obscurance"), bright
//     pixels are blurred into a glow, and a grade is laid over the picture.
static hal::gs::Lighting s_light;
static bool  s_litFrame = false;        // this play: the world's lights are known
static GLint s_uUnbake = -1, s_uAmbient = -1, s_uSunCol = -1, s_uEye = -1, s_uSunDir = -1, s_uBump = -1, s_uCutout = -1, s_uKind = -1, s_uShadowMat = -1, s_uShadowParams = -1, s_uLights = -1, s_uLightPos = -1, s_uLightCol = -1;
static GLuint s_programShadow = 0, s_shadowFbo = 0, s_shadowTex = 0, s_noShadowTex = 0;
static GLint s_uShadowMvp = -1;
static int s_shadowAlloc = 0;
static GLuint s_emptyVao = 0, s_pAo = 0, s_pBright = 0, s_pBlur = 0, s_pComp = 0;
static GLuint s_postFbo = 0, s_postTex = 0, s_aoFbo = 0, s_aoTex = 0, s_bloomFbo[2] = {}, s_bloomTex[2] = {};
static int s_postW = 0, s_postH = 0;
static bool s_writeDist = false;        // this play: 3D views of the world write it
static bool s_viewDist = false;         // the 3D view being played writes distances
static void dist_buffers(bool on) {
    static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    if (s_writeDist && !s_inTarget)
        glDrawBuffers(on ? 2 : 1, both);
}

static const char* kShadowVs = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uMvp;
out vec2 vUv;
void main() { vUv = aUv; gl_Position = uMvp * vec4(aPos, 1.0); }
)";
static const char* kShadowFs = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
void main() { if (texture(uTex, vUv).a < 0.5) discard; }
)";
static const char* kScreenVs = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
// (Depth → distance: the depth buffer holds (ndc + 1) / 2 of a projection
// whose clip z = z·A + B and clip w = −z.)
static const char* kAoFs = R"(#version 330 core
in vec2 vUv;
out vec4 o;
uniform sampler2D uDistT;   // distance from the eye (huge: nothing of the world)
uniform vec4 uProj;         // proj[0], proj[5]
uniform float uXScale;
uniform vec3 uAo;           // radius, strength, the frame's height in pixels
uniform vec2 uTexel;        // a pixel of the frame
float dist(vec2 uv) { return texture(uDistT, uv).r; }
vec3 at(vec2 uv, float z) { return vec3((uv.x * 2.0 - 1.0) / uXScale * z / uProj.x, (uv.y * 2.0 - 1.0) * z / uProj.y, -z); }
vec3 view(vec2 uv) { return at(uv, dist(uv)); }
void main() {
    if (dist(vUv) > 1e8) { o = vec4(1.0); return; }
    vec3 P = view(vUv);
    vec3 x1 = view(vUv + vec2(uTexel.x, 0.0)) - P, x2 = P - view(vUv - vec2(uTexel.x, 0.0));
    vec3 y1 = view(vUv + vec2(0.0, uTexel.y)) - P, y2 = P - view(vUv - vec2(0.0, uTexel.y));
    vec3 N = normalize(cross(abs(x1.z) < abs(x2.z) ? x1 : x2, abs(y1.z) < abs(y2.z) ? y1 : y2));
    if (dot(N, P) > 0.0) N = -N;
    float r = uAo.x, z = -P.z;
    float px = min(r * uProj.y / z * 0.5 * uAo.z, uAo.z * 0.1);
    if (px < 1.5) { o = vec4(1.0); return; }
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    const int n = 12;
    float sum = 0.0;
    for (int i = 0; i < n; i++) {
        float a = (float(i) + 0.5) / float(n);
        float angle = a * 6.2831853 * 7.0 + noise * 6.2831853;
        vec2 uv = vUv + vec2(cos(angle), sin(angle)) * (a * px) * uTexel;
        float dq = dist(uv);
        if (dq > 1e8) continue;
        vec3 v = at(uv, dq) - P;
        float vv = dot(v, v), vn = dot(v, N);
        float f = max(r * r - vv, 0.0);
        // (What lies nearly in the surface does not count.)
        sum += f * f * f * max((vn - 0.01 * z - 0.15 * sqrt(vv)) / (vv + 0.01), 0.0);
    }
    // (Never all the way to black: the PS2's pictures are lit softly, and a
    // corner that went black would look like a hole.)
    float ao = max(0.0, 1.0 - sum * 5.0 / (pow(r, 6.0) * float(n)));
    o = vec4(mix(1.0, max(ao, 0.35), clamp(uAo.y, 0.0, 1.0) * 0.75));
}
)";
static const char* kBrightFs = R"(#version 330 core
in vec2 vUv;
out vec4 o;
uniform sampler2D uColor, uDepthT;
uniform vec2 uTexel;
uniform float uThreshold;
void main() {
    // (Only what is in the world glows: the sky, at the far plane, would
    // turn white.)
    if (texture(uDepthT, vUv).r >= 0.9999) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec3 c = (texture(uColor, vUv + vec2(-0.5, -0.5) * uTexel).rgb + texture(uColor, vUv + vec2(0.5, -0.5) * uTexel).rgb +
              texture(uColor, vUv + vec2(-0.5, 0.5) * uTexel).rgb + texture(uColor, vUv + vec2(0.5, 0.5) * uTexel).rgb) * 0.25;
    float l = max(c.r, max(c.g, c.b));
    o = vec4(c * smoothstep(uThreshold, uThreshold + 0.18, l), 1.0);
}
)";
static const char* kBlurFs = R"(#version 330 core
in vec2 vUv;
out vec4 o;
uniform sampler2D uColor;
uniform vec2 uStep;
void main() {
    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 c = texture(uColor, vUv).rgb * w[0];
    for (int i = 1; i < 5; i++)
        c += (texture(uColor, vUv + uStep * float(i)).rgb + texture(uColor, vUv - uStep * float(i)).rgb) * w[i];
    o = vec4(c, 1.0);
}
)";
static const char* kCompFs = R"(#version 330 core
in vec2 vUv;
out vec4 o;
uniform sampler2D uColor, uAoT, uBloom, uDistT;
uniform vec2 uAoTexel;
uniform vec4 uOn;           // ao, bloom strength, grade, the frame's width / height
uniform vec4 uGrade;        // contrast, saturation, warmth, vignette
uniform vec4 uDof;          // strength (0 off), focus distance (0: the middle's), the frame's height
// Depth of field (photo mode): a disc of samples as wide as how far this
// pixel is from the distance in focus; a sample nearer than the focus is
// let spread over what is behind it, one behind is not let bleed forward.
vec3 dof(vec3 c, float z) {
    float focus = uDof.y > 0.0 ? uDof.y : texture(uDistT, vec2(0.5)).r;
    focus = min(focus, 300.0);
    float radius = clamp(abs(z - focus) / max(focus, 0.5), 0.0, 1.0) * uDof.x * uDof.z * 0.012;
    if (radius < 0.5)
        return c;
    vec2 px = 1.0 / vec2(textureSize(uColor, 0));
    vec3 sum = c;
    float weight = 1.0;
    for (int i = 0; i < 24; i++) {
        float a = float(i) * 2.39996, r = sqrt((float(i) + 0.5) / 24.0) * radius;
        vec2 uv = vUv + vec2(cos(a), sin(a)) * r * px;
        float zs = texture(uDistT, uv).r;
        float w = zs < z ? clamp(abs(zs - focus) / max(focus, 0.5) * uDof.x * uDof.z * 0.012 / max(r, 1.0), 0.0, 1.0)
                         : 1.0;
        sum += texture(uColor, uv).rgb * w;
        weight += w;
    }
    return sum / weight;
}
void main() {
    vec4 frame = texture(uColor, vUv);
    vec3 c = frame.rgb;
    float z = texture(uDistT, vUv).r;
    if (uDof.x > 0.0)
        c = dof(c, min(z, 1e4));
    if (uOn.x > 0.5 && z < 1e8) {
        // (The occlusion was made at half size and is noisy: a blur that
        // keeps to surfaces at about the same distance.)
        float sum = 0.0, weight = 0.0;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                vec2 uv = vUv + (vec2(i, j) - 1.5) * uAoTexel;
                float w = max(0.0, 1.0 - abs(texture(uDistT, uv).r - z) / (0.04 * z + 0.05));
                sum += texture(uAoT, uv).r * w;
                weight += w;
            }
        c *= weight > 0.001 ? sum / weight : texture(uAoT, vUv).r;
    }
    c += texture(uBloom, vUv).rgb * uOn.y;
    if (uOn.z > 0.5) {
        c = clamp(c, 0.0, 1.0);
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = mix(vec3(l), c, uGrade.y);
        c = clamp(c, 0.0, 1.0);
        c = mix(c, c * c * (3.0 - 2.0 * c), uGrade.x);
        c *= vec3(1.0 + 0.08 * uGrade.z, 1.0 + 0.02 * uGrade.z, 1.0 - 0.08 * uGrade.z);
        vec2 q = vUv - 0.5;
        q.x *= uOn.w / 1.7778;
        c *= 1.0 - uGrade.w * smoothstep(0.15, 0.75, dot(q, q) * 2.2);
    }
    o = vec4(clamp(c, 0.0, 1.0), frame.a);
}
)";

static GLuint link(const char* vsSrc, const char* fsSrc) {
    GLuint vs = compile(GL_VERTEX_SHADER, vsSrc), fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        fprintf(stderr, "[GS] link error: %s\n", log);
    }
    return p;
}
static GLuint colour_target(GLuint& fbo, GLuint& tex, int w, int h, GLenum format) {
    if (fbo == 0) {
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &tex);
        set_filter(tex);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return fbo;
}
static bool s_lightingHeld = false;     // F3
static bool lighting_wanted() {
    return !s_lightingHeld && (s_light.shadows || s_light.ao || s_light.lanterns || s_light.bloom > 0.0f || s_light.grade ||
                               s_light.dof > 0.0f);
}
static void lighting_init() {
    if (s_programShadow != 0)
        return;
    s_programShadow = link(kShadowVs, kShadowFs);
    glUseProgram(s_programShadow);
    s_uShadowMvp = glGetUniformLocation(s_programShadow, "uMvp");
    glUniform1i(glGetUniformLocation(s_programShadow, "uTex"), 0);
    s_pAo = link(kScreenVs, kAoFs);
    s_pBright = link(kScreenVs, kBrightFs);
    s_pBlur = link(kScreenVs, kBlurFs);
    s_pComp = link(kScreenVs, kCompFs);
    glUseProgram(s_pComp);
    glUniform1i(glGetUniformLocation(s_pComp, "uColor"), 0);
    glUniform1i(glGetUniformLocation(s_pComp, "uAoT"), 1);
    glUniform1i(glGetUniformLocation(s_pComp, "uBloom"), 2);
    glUniform1i(glGetUniformLocation(s_pComp, "uDistT"), 3);
    glGenVertexArrays(1, &s_emptyVao);
    glUseProgram(s_program);
}

// The sun's view: a box `reach` each way round a point ahead of the camera,
// `deep` along the light; its middle moves in whole texels so that the
// shadows' edges do not crawl as the camera moves.
static void shadow_matrix(const WorldView& w, float* m) {
    const float* v = w.view;
    const float ax[3] = {v[0], v[4], v[8]}, ay[3] = {v[1], v[5], v[9]}, az[3] = {v[2], v[6], v[10]};
    float eye[3], C[3];
    for (int k = 0; k < 3; k++)
        eye[k] = -(v[12] * ax[k] + v[13] * ay[k] + v[14] * az[k]);
    const float e = s_light.shadowReach, deep = 60.0f;
    for (int k = 0; k < 3; k++)
        C[k] = eye[k] - az[k] * e * 0.7f;
    float L[3] = {w.sun[0], w.sun[1], w.sun[2]};
    float len = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    if (len < 0.001f) { L[0] = 0.3f; L[1] = 1.0f; L[2] = 0.2f; len = sqrtf(1.13f); }
    for (float& x : L) x /= len;
    if (L[1] < 0.3f) {      // never so low that the shadows run off to the horizon
        L[1] = 0.3f;
        len = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        for (float& x : L) x /= len;
    }
    const float F[3] = {-L[0], -L[1], -L[2]};
    float R[3] = {F[2], 0.0f, -F[0]};
    len = sqrtf(R[0] * R[0] + R[2] * R[2]);
    if (len < 0.001f) { R[0] = 1.0f; R[2] = 0.0f; len = 1.0f; }
    for (float& x : R) x /= len;
    const float U[3] = {F[1] * R[2] - F[2] * R[1], F[2] * R[0] - F[0] * R[2], F[0] * R[1] - F[1] * R[0]};
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float texel = 2.0f * e / (float)s_light.shadowSize;
    const float cx = floorf(dot(C, R) / texel) * texel, cy = floorf(dot(C, U) / texel) * texel;
    const float out[16] = {R[0] / e, U[0] / e, F[0] / deep, 0,  R[1] / e, U[1] / e, F[1] / deep, 0,
                           R[2] / e, U[2] / e, F[2] / deep, 0,  -cx / e, -cy / e, -dot(C, F) / deep, 1};
    memcpy(m, out, sizeof out);
}

static const Vertex3* verts3(const Frame& f, const Cmd& c, const Frame* before, float t);

static bool box_outside(const float* lo, const float* hi, const float* m, float xscale);
static void mesh_box(const Cmd& c, const Frame* before, float* lo, float* hi);
static void shadow_pass(const Frame& f, const Frame* before, float t, const float* m) {
    const int size = s_light.shadowSize;
    if (s_shadowFbo == 0) {
        glGenFramebuffers(1, &s_shadowFbo);
        glGenTextures(1, &s_shadowTex);
    }
    if (s_shadowAlloc != size) {
        s_shadowAlloc = size;
        glBindTexture(GL_TEXTURE_2D, s_shadowTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glBindFramebuffer(GL_FRAMEBUFFER, s_shadowFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_shadowTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_shadowFbo);
    glViewport(0, 0, size, size);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_BLEND);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 4.0f);
    glUseProgram(s_programShadow);
    glUniformMatrix4fv(s_uShadowMvp, 1, GL_FALSE, m);
    glBindVertexArray(s_vao3d);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo3d);
    bool inTarget = false;
    int kind = 0;
    for (const Cmd& c : f.cmds) {
        if (c.op == Op::TargetBegin) inTarget = true;
        else if (c.op == Op::TargetEnd) inTarget = false;
        else if (c.op == Op::Begin3D) kind = c.i;
        else if (c.op == Op::End3D) kind = 0;
        else if (c.op == Op::Mesh && (kind == (int)hal::gs::View::Model || (s_light.mapShadows && kind == (int)hal::gs::View::Map))
                 && !inTarget && c.count > 0) {
            static const bool off = getenv("WOTS2_NO_CULL") != nullptr;
            float lo[3], hi[3];
            mesh_box(c, before, lo, hi);
            if (!off && lo[0] <= hi[0] && box_outside(lo, hi, m, 1.0f))
                continue;           // (outside the shadow's box)
            const Vertex3* v = verts3(f, c, before, t);
            glBindTexture(GL_TEXTURE_2D, c.tex);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)c.count * sizeof(Vertex3)), v, GL_STREAM_DRAW);
            glDrawArrays(gl_prim(c.prim), 0, c.count);
        }
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
}

// Before a list is played: the shadow map, and what the 3D program needs to
// know of the world's lights.
static void lighting_begin(const Frame& f, const Frame* before, float t) {
    s_litFrame = f.world.set && lighting_wanted();
    s_writeDist = false;
    if (!s_litFrame)
        return;
    lighting_init();
    if (s_light.ao || s_light.dof > 0.0f) {
        // The distance buffer: made on first use, the frame's size, beside
        // the colour (it is cleared with the frame and written by the
        // world's 3D views only — see dist_buffers).
        if (s_distTex == 0) {
            glGenTextures(1, &s_distTex);
            glBindTexture(GL_TEXTURE_2D, s_distTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        if (s_distW != s_physW || s_distH != s_physH) {
            s_distW = s_physW;
            s_distH = s_physH;
            glBindTexture(GL_TEXTURE_2D, s_distTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, s_distW, s_distH, 0, GL_RED, GL_FLOAT, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, s_distTex, 0);
        }
        s_writeDist = true;
    }
    float m[16] = {};
    const bool shadows = s_light.shadows && s_light.shadowStrength > 0.0f;
    if (shadows) {
        shadow_matrix(f.world, m);
        shadow_pass(f, before, t, m);
    }
    glUseProgram(s_program3d);
    const float sun = f.world.sunColour[0] * 0.3f + f.world.sunColour[1] * 0.59f + f.world.sunColour[2] * 0.11f;
    glUniform4f(s_uShadowParams, s_light.shadowStrength * (sun > 1.0f ? 1.0f : sun < 0.0f ? 0.0f : sun),
                shadows ? 1.0f : 0.0f, 1.0f / (float)s_light.shadowSize,
                s_light.modelShadow);
    glUniformMatrix4fv(s_uShadowMat, 1, GL_FALSE, m);
    glUniform4f(s_uSunDir, f.world.sun[0], f.world.sun[1], f.world.sun[2], sun > 1.0f ? 1.0f : sun < 0.0f ? 0.0f : sun);
    glUniform4f(s_uBump, s_light.bump, s_light.bumpDepth, 0.0f, 0.0f);
    {
        // Only by day and out of doors: a night map's painted light holds its
        // lanterns' pools (relit, the night went flat and bright — its light
        // numbers are the day's), and interiors have no sun to speak of.
        const float share = f.world.ambience && !f.world.indoors && !f.world.night ? 1.0f : 0.0f;
        const float block = s_light.shadowStrength / 0.55f;
        glUniform4f(s_uUnbake, s_light.unbake * share, s_light.unbakeExposure, block > 1.0f ? 1.0f : block, 0.0f);
    }
    glUniform3fv(s_uAmbient, 1, f.world.ambient);
    glUniform3fv(s_uSunCol, 1, f.world.sunColour);
    {
        const float* v = f.world.view;
        float eye[3];
        for (int k = 0; k < 3; k++)
            eye[k] = -(v[12] * v[k * 4] + v[13] * v[k * 4 + 1] + v[14] * v[k * 4 + 2]);
        glUniform3fv(s_uEye, 1, eye);
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, shadows ? s_shadowTex : s_noShadowTex);
    glActiveTexture(GL_TEXTURE0);
    // The sixteen point lights nearest the middle of the view.
    int n = 0;
    if (s_light.lanterns) {
        const float* v = f.world.view;
        float centre[3];
        for (int k = 0; k < 3; k++)
            centre[k] = -(v[12] * v[k * 4] + v[13] * v[k * 4 + 1] + v[14] * v[k * 4 + 2]) - v[k * 4 + 2] * 8.0f;
        std::vector<std::pair<float, int>> nearest;
        const int count = (int)(f.world.lights.size() / 8);
        for (int i = 0; i < count; i++) {
            const float* l = &f.world.lights[(size_t)i * 8];
            const float d[3] = {l[0] - centre[0], l[1] - centre[1], l[2] - centre[2]};
            nearest.push_back({d[0] * d[0] + d[1] * d[1] + d[2] * d[2], i});
        }
        std::sort(nearest.begin(), nearest.end());
        float pos[16][4], col[16][3];
        for (; n < 16 && n < (int)nearest.size(); n++) {
            const float* l = &f.world.lights[(size_t)nearest[(size_t)n].second * 8];
            pos[n][0] = l[0]; pos[n][1] = l[1]; pos[n][2] = l[2];
            pos[n][3] = l[3] * s_light.lanternRadius / 5.0f;
            for (int k = 0; k < 3; k++)
                col[n][k] = l[4 + k] * s_light.lanternStrength;
        }
        if (n > 0) {
            glUniform4fv(s_uLightPos, n, &pos[0][0]);
            glUniform3fv(s_uLightCol, n, &col[0][0]);
        }
    }
    glUniform1i(s_uLights, n);
    glUseProgram(s_program);
}

// The world is drawn: occlusion, glow and grade, through a copy.
// ── Wind and dust (Lighting::dust) ─────────────────────────────────────────
// The game has wind — a gust that builds and dies away every second or two
// (World+0x80), which swings hair and cloth and drifts chimney smoke — but
// nothing in the air shows it.  Here: motes drifting round the player, and
// when a gust blows, dust swept along the ground with it.  Simulated as the
// pictures are shown (so smooth at any rate), drawn after the world's 3D with
// the map's own view (in-betweens included), depth-tested, writing nothing.
static inline float mix(float a, float b, float t);
static void state_3d(const float* mvp, float xscale, int kind);
static void state_2d();
static void overlay(bool on);
struct Mote { float p[3], v[3], age, life, size, seed; int kind; };
static std::vector<Mote> s_motes;
static GLuint s_dotTex = 0;
static double s_dustClock = -1.0;
static bool s_frozen = false;       // photo mode: the game is held still
static std::string s_picturePath;   // photo mode: a picture asked for
static u32 s_dustRand = 12345;
static float frand(float a, float b) {
    s_dustRand = s_dustRand * 1664525u + 1013904223u;
    return a + (b - a) * (float)(s_dustRand >> 8) / 16777216.0f;
}
static void dust_pass(const Frame& f, const Frame* before, float t) {
    const WorldView& w = f.world;
    if (s_light.dust <= 0.0f || !w.ambience)
        return;
    // The map's view, as the map was drawn with it.
    int at = -1;
    for (size_t i = 0; i < f.cmds.size(); i++)
        if (f.cmds[i].op == Op::Begin3D && f.cmds[i].i == (int)hal::gs::View::Map) { at = (int)i; break; }
    if (at < 0)
        return;
    const Cmd& view = f.cmds[(size_t)at];
    float m[16];
    memcpy(m, view.f, sizeof m);
    if (before != nullptr && view.match >= 0)
        for (int k = 0; k < 16; k++)
            m[k] = mix(before->cmds[(size_t)view.match].f[k], m[k], t);
    // The clock: game frames (30 a second), with the in-between's share.
    const double clock = (double)f.number - 1.0 + (double)t;
    float dt = s_dustClock < 0.0 || s_frozen ? 0.0f : (float)((clock - s_dustClock) / 30.0);
    s_dustClock = clock;
    dt = dt < 0.0f ? 0.0f : dt > 0.1f ? 0.1f : dt;

    // The wind: the game's gust along x (up to 0.1 × the map's wind gain).
    const float gust = fabsf(w.wind[0]) / 0.05f;
    const float s = gust > 2.0f ? 2.0f : gust;
    const float dir = w.wind[0] < 0.0f ? -1.0f : 1.0f;
    const float R = 9.0f;
    const float* c = w.focus;
    int motes = 0, sweeps = 0;
    for (const Mote& p : s_motes) (p.kind == 0 ? motes : sweeps)++;
    const int wantMotes = (int)((w.indoors ? 40.0f : 90.0f) * s_light.dust);
    const float sweepShare = w.indoors ? 0.0f : (s - 0.25f) / 0.75f;
    const int wantSweeps = (int)(110.0f * s_light.dust * (sweepShare < 0 ? 0 : sweepShare > 1 ? 1 : sweepShare));
    for (; motes < wantMotes; motes++) {
        Mote p{};
        p.kind = 0;
        p.p[0] = c[0] + frand(-R, R); p.p[1] = c[1] + frand(0.2f, 3.2f); p.p[2] = c[2] + frand(-R, R);
        p.life = frand(5.0f, 9.0f);
        p.size = frand(0.035f, 0.06f);
        p.seed = frand(0.0f, 100.0f);
        s_motes.push_back(p);
    }
    // Gust dust comes in from upwind, a few a frame.
    int budget = (int)(dt * 60.0f * s_light.dust * (1.0f + s)) + 1;
    for (; sweeps < wantSweeps && budget > 0; sweeps++, budget--) {
        Mote p{};
        p.kind = 1;
        p.p[0] = c[0] - dir * frand(0.3f, 1.0f) * R; p.p[1] = c[1] + frand(0.0f, 0.35f); p.p[2] = c[2] + frand(-R, R);
        const float speed = (2.5f + 3.0f * s) * frand(0.7f, 1.3f);
        p.v[0] = dir * speed; p.v[1] = frand(0.05f, 0.25f); p.v[2] = frand(-0.4f, 0.4f);
        p.life = frand(1.2f, 2.5f);
        p.size = frand(0.3f, 0.8f);
        p.seed = frand(0.0f, 100.0f);
        s_motes.push_back(p);
    }
    for (Mote& p : s_motes) {
        p.age += dt;
        if (p.kind == 0) {
            const float a = p.age, k = p.seed;
            p.v[0] = dir * (0.15f + 0.8f * s) + sinf(a * 1.3f + k) * 0.08f;
            p.v[1] = sinf(a * 0.9f + k * 2.0f) * 0.05f;
            p.v[2] = cosf(a * 1.1f + k) * 0.08f;
        } else {
            const float drag = 1.0f - 0.3f * dt;
            for (float& x : p.v) x *= drag;
            p.size += dt * 0.25f;
        }
        for (int i = 0; i < 3; i++) p.p[i] += p.v[i] * dt;
    }
    s_motes.erase(std::remove_if(s_motes.begin(), s_motes.end(), [&](const Mote& p) {
        const float dx = p.p[0] - c[0], dz = p.p[2] - c[2];
        return p.age > p.life || dx * dx + dz * dz > R * R * 2.0f || fabsf(p.p[1] - c[1]) > 6.0f;
    }), s_motes.end());
    if (s_motes.empty())
        return;

    if (s_dotTex == 0) {        // a soft round dot
        std::vector<u8> px(32 * 32 * 4);
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                const float dx = (x + 0.5f) / 16.0f - 1.0f, dy = (y + 0.5f) / 16.0f - 1.0f;
                const float r = sqrtf(dx * dx + dy * dy);
                const float a = r >= 1.0f ? 0.0f : (1.0f - r) * (1.0f - r);
                u8* q = &px[(size_t)(y * 32 + x) * 4];
                q[0] = q[1] = q[2] = 255;
                q[3] = (u8)(a * 255.0f);
            }
        glGenTextures(1, &s_dotTex);
        set_filter(s_dotTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    const float* v = w.view;
    const float right[3] = {v[0], v[4], v[8]}, up[3] = {v[1], v[5], v[9]};
    float eye[3];           // (translation in the last row: row-vector convention)
    for (int k = 0; k < 3; k++)
        eye[k] = -(v[12] * v[k * 4] + v[13] * v[k * 4 + 1] + v[14] * v[k * 4 + 2]);
    const float lum = w.sunColour[0] * 0.3f + w.sunColour[1] * 0.59f + w.sunColour[2] * 0.11f;
    const float light = 0.35f + 0.65f * (lum > 1.0f ? 1.0f : lum < 0.0f ? 0.0f : lum);
    std::vector<Vertex3> q;
    q.reserve(s_motes.size() * 6);
    for (const Mote& p : s_motes) {
        const float in = p.age / (p.life * 0.15f), out = (p.life - p.age) / (p.life * 0.3f);
        float env = in < out ? in : out;
        env = env > 1.0f ? 1.0f : env;
        float r, g, b, a;
        if (p.kind == 0) {
            // Close to the eye a mote would be a snowflake: gone within 3 units.
            const float dx = p.p[0] - eye[0], dy = p.p[1] - eye[1], dz = p.p[2] - eye[2];
            const float closeness = (sqrtf(dx * dx + dy * dy + dz * dz) - 1.5f) / 1.5f;
            env *= closeness < 0.0f ? 0.0f : closeness > 1.0f ? 1.0f : closeness;
            r = 1.1f; g = 0.98f; b = 0.8f; a = 0.6f;
        }
        else             { r = 0.82f; g = 0.76f; b = 0.66f; a = 0.18f; }
        r *= light; g *= light; b *= light; a *= env;
        const float h = p.size;
        float corner[4][3];
        for (int k = 0; k < 3; k++) {
            corner[0][k] = p.p[k] + (-right[k] - up[k]) * h;
            corner[1][k] = p.p[k] + ( right[k] - up[k]) * h;
            corner[2][k] = p.p[k] + ( right[k] + up[k]) * h;
            corner[3][k] = p.p[k] + (-right[k] + up[k]) * h;
        }
        const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int idx : {0, 1, 2, 0, 2, 3})
            q.push_back({corner[idx][0], corner[idx][1], corner[idx][2], uv[idx][0], uv[idx][1], r, g, b, a});
    }
    state_3d(m, (float)s_width / s_lw, 0);
    overlay(true);
    glBindTexture(GL_TEXTURE_2D, s_dotTex);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(q.size() * sizeof(Vertex3)), q.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)q.size());
    overlay(false);
    state_2d();
}

static void post_pass(const Frame& f) {
    if (!(s_light.ao || s_light.bloom > 0.0f || s_light.grade || s_light.dof > 0.0f))
        return;
    const int W = s_physW, H = s_physH, hw = (W + 1) / 2, hh = (H + 1) / 2;
    if (s_postW != W || s_postH != H) {
        s_postW = W;
        s_postH = H;
        colour_target(s_postFbo, s_postTex, W, H, GL_RGBA8);
        colour_target(s_aoFbo, s_aoTex, hw, hh, GL_RGBA8);
        colour_target(s_bloomFbo[0], s_bloomTex[0], hw, hh, GL_RGBA8);
        colour_target(s_bloomFbo[1], s_bloomTex[1], hw, hh, GL_RGBA8);
    }
    const float* p = f.world.proj;
    const float proj[4] = {p[0], p[5], p[10], p[14]};
    const float xscale = (float)s_width / s_lw;
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindVertexArray(s_emptyVao);
    if (s_light.ao) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_aoFbo);
        glViewport(0, 0, hw, hh);
        glUseProgram(s_pAo);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_distTex);
        glUniform1i(glGetUniformLocation(s_pAo, "uDistT"), 0);
        glUniform4fv(glGetUniformLocation(s_pAo, "uProj"), 1, proj);
        glUniform1f(glGetUniformLocation(s_pAo, "uXScale"), xscale);
        glUniform3f(glGetUniformLocation(s_pAo, "uAo"), s_light.aoRadius, s_light.aoStrength, (float)H);
        glUniform2f(glGetUniformLocation(s_pAo, "uTexel"), 1.0f / (float)W, 1.0f / (float)H);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (s_light.bloom > 0.0f) {
        glViewport(0, 0, hw, hh);
        glBindFramebuffer(GL_FRAMEBUFFER, s_bloomFbo[0]);
        glUseProgram(s_pBright);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_fboTex);
        glUniform1i(glGetUniformLocation(s_pBright, "uColor"), 0);
        glUniform1i(glGetUniformLocation(s_pBright, "uDepthT"), 1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_depth);
        glActiveTexture(GL_TEXTURE0);
        glUniform2f(glGetUniformLocation(s_pBright, "uTexel"), 1.0f / (float)W, 1.0f / (float)H);
        glUniform1f(glGetUniformLocation(s_pBright, "uThreshold"), s_light.bloomThreshold);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(s_pBlur);
        glUniform1i(glGetUniformLocation(s_pBlur, "uColor"), 0);
        const GLint step = glGetUniformLocation(s_pBlur, "uStep");
        const float scale = (float)H / 448.0f;          // the same glow at any size
        for (int pass = 0; pass < 2; pass++) {
            const float k = (pass + 1) * scale;
            glBindFramebuffer(GL_FRAMEBUFFER, s_bloomFbo[1]);
            glBindTexture(GL_TEXTURE_2D, s_bloomTex[0]);
            glUniform2f(step, k / (float)hw, 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindFramebuffer(GL_FRAMEBUFFER, s_bloomFbo[0]);
            glBindTexture(GL_TEXTURE_2D, s_bloomTex[1]);
            glUniform2f(step, 0.0f, k / (float)hh);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_postFbo);
    glViewport(0, 0, W, H);
    glUseProgram(s_pComp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_fboTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_aoTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s_bloomTex[0]);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, s_writeDist ? s_distTex : s_depth);
    glUniform2f(glGetUniformLocation(s_pComp, "uAoTexel"), 1.0f / (float)hw, 1.0f / (float)hh);
    glUniform4f(glGetUniformLocation(s_pComp, "uOn"), s_light.ao ? 1.0f : 0.0f, s_light.bloom,
                s_light.grade ? 1.0f : 0.0f, (float)W / (float)H);
    glUniform4f(glGetUniformLocation(s_pComp, "uGrade"), s_light.contrast, s_light.saturation, s_light.warmth,
                s_light.vignette);
    glUniform4f(glGetUniformLocation(s_pComp, "uDof"), s_writeDist ? s_light.dof : 0.0f, s_light.dofFocus, (float)H, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_shadowTex != 0 ? s_shadowTex : s_noShadowTex);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_postFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glEnable(GL_BLEND);
}

// ── Playing a list ─────────────────────────────────────────────────────────
static std::vector<Vertex> s_tmp2;
static std::vector<Vertex3> s_tmp3;

static inline float mix(float a, float b, float t) { return a + (b - a) * t; }

// A draw's vertices: its own, or (in between) part of the way from the
// matching draw of the frame before; then moved for a wider picture.
static const Vertex* verts2(const Frame& f, const Cmd& c, const Frame* before, float t) {
    const Vertex* v = f.v2.data() + c.first;
    const bool between = before != nullptr && c.lerp;
    const bool wide = s_xoff > 0.0f && (c.flags & (kSpan | kFrameTex)) != 0;
    if (!between && !wide)
        return v;
    s_tmp2.assign(v, v + c.count);
    if (between) {
        const Vertex* a = before->v2.data() + before->cmds[(size_t)c.match].first;
        for (int i = 0; i < c.count; i++) {
            Vertex& o = s_tmp2[(size_t)i];
            o.x = mix(a[i].x, o.x, t); o.y = mix(a[i].y, o.y, t);
            o.r = mix(a[i].r, o.r, t); o.g = mix(a[i].g, o.g, t);
            o.b = mix(a[i].b, o.b, t); o.a = mix(a[i].a, o.a, t);
        }
    }
    if (wide)
        for (Vertex& o : s_tmp2) {
            // (The frame as a texture: its texels are the whole, wider frame.)
            if (c.flags & kFrameTex)
                o.u = (o.u * (float)s_width + s_xoff) / s_lw;
            if (c.flags & kSpan) {
                const float push = o.x <= 0.5f ? -s_xoff : o.x >= (float)s_width - 0.5f ? s_xoff : 0.0f;
                o.x += push;
                if (c.flags & kFrameTex)
                    o.u += push / s_lw;
            }
        }
    return s_tmp2.data();
}
static const Vertex3* verts3(const Frame& f, const Cmd& c, const Frame* before, float t) {
    const Vertex3* v = f.v3.data() + c.first;
    const bool between = before != nullptr && c.lerp;
    if (!between && c.op != Op::DrawDepth)
        return v;
    s_tmp3.assign(v, v + c.count);
    if (between) {
        const Vertex3* a = before->v3.data() + before->cmds[(size_t)c.match].first;
        for (int i = 0; i < c.count; i++) {
            Vertex3& o = s_tmp3[(size_t)i];
            o.x = mix(a[i].x, o.x, t); o.y = mix(a[i].y, o.y, t); o.z = mix(a[i].z, o.z, t);
            if (c.lerpUv) { o.u = mix(a[i].u, o.u, t); o.v = mix(a[i].v, o.v, t); }
            o.r = mix(a[i].r, o.r, t); o.g = mix(a[i].g, o.g, t);
            o.b = mix(a[i].b, o.b, t); o.a = mix(a[i].a, o.a, t);
        }
    }
    if (c.op == Op::DrawDepth)
        for (Vertex3& o : s_tmp3) {     // screen positions → clip space (the 3D vertex shader turns y over)
            float x = o.x;
            if (s_xoff > 0.0f) {
                if (c.flags & kFrameTex)
                    o.u = (o.u * (float)s_width + s_xoff) / s_lw;
                if (c.flags & kSpan) {
                    const float push = x <= 0.5f ? -s_xoff : x >= (float)s_width - 0.5f ? s_xoff : 0.0f;
                    x += push;
                    if (c.flags & kFrameTex)
                        o.u += push / s_lw;
                }
            }
            o.x = (x + s_xoff) / s_lw * 2.0f - 1.0f;
            o.y = 1.0f - o.y / (float)s_height * 2.0f;
        }
    return s_tmp3.data();
}

static void state_3d(const float* mvp, float xscale, int kind = 0) {
    s_viewDist = kind == (int)hal::gs::View::Map || kind == (int)hal::gs::View::Model;
    dist_buffers(s_viewDist);
    glUseProgram(s_program3d);
    glUniform1i(s_uKind, s_litFrame && !s_inTarget ? kind : 0);
    glUniformMatrix4fv(s_uMvp, 1, GL_FALSE, mvp);  // row-major, row-vector = column-major, column-vector
    glUniform1f(s_uXScale, xscale);
    glUniform1f(s_uAlphaRef3d, 0.5f);
    glUniform4fv(s_uFog, 1, s_fog);
    glUniform3fv(s_uFogColor, 1, s_fogColor);
    glBindVertexArray(s_vao3d);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo3d);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    // (One depth buffer for the whole frame, cleared when it begins:
    // everything hides and is hidden by everything else, whoever draws it.)
}
static void bias(int steps) {
    if (steps == 0) {
        glDisable(GL_POLYGON_OFFSET_FILL);
    } else {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset((float)steps * 0.15f, (float)steps * 2.0f);
    }
}
static void overlay(bool on) {
    s_overlayOn = on;
    dist_buffers(s_viewDist && !on);    // (a blended overlay hides nothing: no distance)
    glDepthMask(on ? GL_FALSE : GL_TRUE);
    glUniform1f(s_uAlphaRef3d, on ? 0.02f : 0.5f);
}
static void state_2d() {
    s_viewDist = false;
    bias(0);
    overlay(false);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(s_program);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
}

// Plays a frame's list into the frame buffer.  With `before` and t < 1: the
// picture part of the way (t) from the frame before to this one.
// ── Culling ─────────────────────────────────────────────────────────────────
// Everything 3D is recorded whole (so the in-between pictures pair the same
// draws whatever is in view) and culled as it is played: a mesh whose box —
// its own, and the frame before's when it is drawn part of the way from that
// — lies wholly outside one side of the view, or (map and models, with fog)
// further than the fog's end, is not drawn.  The view is the one the vertex
// shader uses, the widescreen narrowing included.  WOTS2_NO_CULL=1 draws all.
static float s_playMvp[16], s_playXScale = 1.0f, s_playEye[3];
static int s_playKind = 0;
static bool s_playEyeSet = false;
static int s_culled = 0, s_drawn = 0;
static bool box_outside(const float* lo, const float* hi, const float* m, float xscale) {
    int out[6] = {};
    for (int i = 0; i < 8; i++) {
        const float p[3] = {i & 1 ? hi[0] : lo[0], i & 2 ? hi[1] : lo[1], i & 4 ? hi[2] : lo[2]};
        float c[4];
        for (int j = 0; j < 4; j++)
            c[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
        c[0] *= xscale;
        out[0] += c[0] < -c[3];
        out[1] += c[0] > c[3];
        out[2] += c[1] < -c[3];
        out[3] += c[1] > c[3];
        out[4] += c[2] < -c[3];
        out[5] += c[2] > c[3];
    }
    for (int k = 0; k < 6; k++)
        if (out[k] == 8)
            return true;
    return false;
}
static void mesh_box(const Cmd& c, const Frame* before, float* lo, float* hi) {
    memcpy(lo, c.f, 3 * sizeof(float));
    memcpy(hi, c.f + 3, 3 * sizeof(float));
    if (before != nullptr && c.lerp && c.match >= 0) {
        const float* o = before->cmds[(size_t)c.match].f;
        for (int k = 0; k < 3; k++) {
            lo[k] = o[k] < lo[k] ? o[k] : lo[k];
            hi[k] = o[3 + k] > hi[k] ? o[3 + k] : hi[k];
        }
    }
}
static bool culled(const Cmd& c, const Frame* before) {
    static const bool off = getenv("WOTS2_NO_CULL") != nullptr;
    if (off || c.count == 0)
        return false;
    float lo[3], hi[3];
    mesh_box(c, before, lo, hi);
    if (lo[0] > hi[0])
        return false;
    if (box_outside(lo, hi, s_playMvp, s_playXScale))
        return true;
    if (s_fog[3] > 0.5f && s_playEyeSet &&
        (s_playKind == (int)hal::gs::View::Map || s_playKind == (int)hal::gs::View::Model)) {
        float d2 = 0.0f;                // the box's nearest point to the eye
        for (int k = 0; k < 3; k++) {
            const float e = s_playEye[k], v = e < lo[k] ? lo[k] - e : e > hi[k] ? e - hi[k] : 0.0f;
            d2 += v * v;
        }
        const float fogEnd = s_fog[2] + 2.0f;
        if (d2 > fogEnd * fogEnd)
            return true;
    }
    return false;
}

static void play(const Frame& f, const Frame* before, float t) {
    s_playEyeSet = f.world.set;
    if (f.world.set) {
        const float* v = f.world.view;
        for (int k = 0; k < 3; k++)
            s_playEye[k] = -(v[12] * v[k * 4] + v[13] * v[k * 4 + 1] + v[14] * v[k * 4 + 2]);
    }
    static const float kIdentity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    static const float kNoFog[4] = {0, 0, 0, 0};
    const bool scaled = s_physH != s_height;
    lighting_begin(f, before, t);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_physW, s_physH);
    state_2d();
    glUniform2f(s_uScreen, s_lw, (float)s_height);
    glUniform1f(s_uXoff, s_xoff);
    glUniform4f(s_uUvRect, -1e9f, -1e9f, 1e9f, 1e9f);
    glLineWidth(scaled ? (float)s_physH / (float)s_height : 1.0f);
    s_inTarget = false;
    for (const Cmd& c : f.cmds) {
        switch (c.op) {
        case Op::FrameBegin:
            glClearColor(c.f[0], c.f[1], c.f[2], c.f[3]);
            glDepthMask(GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (s_writeDist && !s_inTarget) {
                static const float kNothing[4] = {1e9f, 1e9f, 1e9f, 1e9f};
                dist_buffers(true);
                glClearBufferfv(GL_COLOR, 1, kNothing);
                dist_buffers(false);
            }
            break;
        case Op::Clear:
            glClearColor(c.f[0], c.f[1], c.f[2], c.f[3]);
            glClear(GL_COLOR_BUFFER_BIT);
            break;
        case Op::Blend:
            apply_blend(c.i);
            break;
        case Op::AlphaRef:
            s_alphaRef = (float)c.i / 128.0f;
            glUniform1f(s_uAlphaRef, s_alphaRef);
            break;
        case Op::Fog:
            memcpy(s_fog, c.f, sizeof(s_fog));
            memcpy(s_fogColor, c.f + 4, sizeof(s_fogColor));
            break;
        case Op::Draw2D: {
            const Vertex* v = verts2(f, c, before, t);
            glBindTexture(GL_TEXTURE_2D, c.tex);
            glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)c.count * sizeof(Vertex)), v, GL_STREAM_DRAW);
            // A sprite drawn larger than life must not take in the texels
            // next to it on its sheet.
            const bool rect = scaled && (c.flags & kUvRect) != 0;
            if (rect) {
                float lo[2] = {c.f[0] + c.f[4], c.f[1] + c.f[5]}, hi[2] = {c.f[2] - c.f[4], c.f[3] - c.f[5]};
                for (int k = 0; k < 2; k++)
                    if (lo[k] > hi[k])
                        lo[k] = hi[k] = (c.f[k] + c.f[k + 2]) / 2.0f;
                glUniform4f(s_uUvRect, lo[0], lo[1], hi[0], hi[1]);
            }
            if (c.flags & kHasZ) {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_TRUE);
                glUniform1f(s_uDepth, (float)((double)(0xFFFFFFu - (c.z & 0xFFFFFFu)) / 16777215.0));
            }
            glDrawArrays(gl_prim(c.prim), 0, c.count);
            if (c.flags & kHasZ)
                glDisable(GL_DEPTH_TEST);
            if (rect)
                glUniform4f(s_uUvRect, -1e9f, -1e9f, 1e9f, 1e9f);
            break;
        }
        case Op::DrawDepth: {
            const Vertex3* v = verts3(f, c, before, t);
            state_3d(kIdentity, 1.0f);
            glUniform4fv(s_uFog, 1, kNoFog);
            overlay(true);
            glBindTexture(GL_TEXTURE_2D, c.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)c.count * sizeof(Vertex3)), v, GL_STREAM_DRAW);
            glDrawArrays(gl_prim(c.prim), 0, c.count);
            state_2d();
            break;
        }
        case Op::Begin3D: {
            float m[16];
            memcpy(m, c.f, sizeof(m));
            if (before != nullptr && c.match >= 0) {
                const float* a = before->cmds[(size_t)c.match].f;
                for (int k = 0; k < 16; k++)
                    m[k] = mix(a[k], m[k], t);
            }
            memcpy(s_playMvp, m, sizeof m);
            s_playXScale = s_inTarget ? 1.0f : (float)s_width / s_lw;
            s_playKind = c.i;
            state_3d(m, s_playXScale, c.i);
            break;
        }
        case Op::Mesh: {
            if (culled(c, before)) {
                s_culled++;
                break;
            }
            s_drawn++;
            const Vertex3* v = verts3(f, c, before, t);
            glBindTexture(GL_TEXTURE_2D, c.tex);
            const bool cutout = !s_overlayOn && s_cutout.count(c.tex);
            glUniform1i(s_uCutout, cutout ? 1 : 0);
            if (c.tex != s_white) {     // model textures tile
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            }
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)c.count * sizeof(Vertex3)), v, GL_STREAM_DRAW);
            glDrawArrays(gl_prim(c.prim), 0, c.count);
            if (cutout) {
                // The fringe: blended over what is there now, hiding nothing —
                // whatever lies behind and comes later simply draws over it.
                glUniform1i(s_uCutout, 2);
                overlay(true);
                glDrawArrays(gl_prim(c.prim), 0, c.count);
                overlay(false);
                glUniform1i(s_uCutout, 0);
            }
            break;
        }
        case Op::End3D:
            state_2d();
            break;
        case Op::Bias:
            bias(c.i);
            break;
        case Op::Overlay:
            overlay(c.i != 0);
            break;
        case Op::Grab:
            glBindTexture(GL_TEXTURE_2D, s_grabTex);
            if (s_grabW != s_physW || s_grabH != s_physH) {
                s_grabW = s_physW;
                s_grabH = s_physH;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, s_grabW, s_grabH, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            }
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, s_physW, s_physH);
            break;
        case Op::TargetBegin:
            if (s_targetSize != c.i) {
                s_targetSize = c.i;
                set_filter(s_targetTex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c.i, c.i, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindFramebuffer(GL_FRAMEBUFFER, s_targetFbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_targetTex, 0);
                glBindRenderbuffer(GL_RENDERBUFFER, s_targetDepth);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, c.i, c.i);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_targetDepth);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, s_targetFbo);
            glViewport(0, 0, c.i, c.i);
            glDepthMask(GL_TRUE);
            glClearColor(0.f, 0.f, 0.f, 0.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable(GL_BLEND);
            s_fogBeforeTarget = s_fog[3];
            s_fog[3] = 0.0f;
            s_inTarget = true;
            break;
        case Op::Post:
            if (s_litFrame && !s_inTarget) {
                dust_pass(f, before, t);
                post_pass(f);
                state_2d();
            }
            break;
        case Op::TargetEnd:
            s_fog[3] = s_fogBeforeTarget;
            glEnable(GL_BLEND);
            glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
            glViewport(0, 0, s_physW, s_physH);
            s_inTarget = false;
            break;
        }
    }
    state_2d();
}

// ── Pairing a frame's draws with those of the frame before ─────────────────
// (A mesh is paired only with one of the same thing: two townspeople of one
// model have meshes alike in every other way, and when they swapped places
// in the order of drawing, the picture in between showed a ghost of one
// halfway to the other.)
static u64 key_of(const Cmd& c) {
    return ((u64)c.op << 56) ^ ((u64)c.prim << 48) ^ ((u64)(u32)c.count << 20) ^ (u64)c.tex * 0x9E3779B97F4A7C15ull ^
           (u64)c.who * 0xC2B2AE3D27D4EB4Full;
}
static bool has_verts(Op op) { return op == Op::Draw2D || op == Op::DrawDepth || op == Op::Mesh; }
static void index_frame(Frame& f) {
    f.index.clear();
    for (size_t i = 0; i < f.cmds.size(); i++)
        if (has_verts(f.cmds[i].op))
            f.index[key_of(f.cmds[i])].push_back((u32)i);
}
// Where a vertex comes out on the screen (−1 … 1), row-vector convention.
static bool project(const float* m, const Vertex3& v, float* out) {
    const float w = v.x * m[3] + v.y * m[7] + v.z * m[11] + m[15];
    if (w < 0.05f)
        return false;
    out[0] = (v.x * m[0] + v.y * m[4] + v.z * m[8] + m[12]) / w;
    out[1] = (v.x * m[1] + v.y * m[5] + v.z * m[9] + m[13]) / w;
    return true;
}
// Where the eye of a world → view matrix is (translation in the last row or
// the last column, whichever it has), and the angle between two of them.
static void eye_of(const float* m, float* eye) {
    const bool rows = fabsf(m[3]) + fabsf(m[7]) + fabsf(m[11]) < 1e-6f;
    const float t[3] = {rows ? m[12] : m[3], rows ? m[13] : m[7], rows ? m[14] : m[11]};
    for (int j = 0; j < 3; j++)
        eye[j] = rows ? -(t[0] * m[j * 4] + t[1] * m[j * 4 + 1] + t[2] * m[j * 4 + 2])
                      : -(t[0] * m[j] + t[1] * m[4 + j] + t[2] * m[8 + j]);
}
// Did the camera cut between two world frames?  An ease or a swing of the
// right stick moves the eye a few tenths and turns the view a few degrees in
// a thirtieth of a second; a cut moves it metres or turns it round.
static bool camera_cut(const WorldView& a, const WorldView& b) {
    float ea[3], eb[3];
    eye_of(a.view, ea);
    eye_of(b.view, eb);
    const float dx = ea[0] - eb[0], dy = ea[1] - eb[1], dz = ea[2] - eb[2];
    float tr = 0.0f;                    // trace(Ra Rbᵀ) = 1 + 2 cos(angle)
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            tr += a.view[i * 4 + j] * b.view[i * 4 + j];
    const bool cut = dx * dx + dy * dy + dz * dz > 1.5f * 1.5f || (tr - 1.0f) / 2.0f < 0.866f;     // cos 30°
    static const bool trace = getenv("WOTS2_TRACE_PAIR") != nullptr;
    if (trace)
        printf("[pair] camera: eye moved %.2f, turned %.1f degrees%s\n", sqrtf(dx * dx + dy * dy + dz * dz),
               acosf(fminf(1.0f, fmaxf(-1.0f, (tr - 1.0f) / 2.0f))) * 57.29578f, cut ? " -> CUT" : "");
    return cut;
}

static void pair_frames(Frame& f, const Frame& p) {
    const float kJump2D = 100.0f;       // game pixels in a thirtieth of a second: further is a jump, not a move
    const float kJump3D = 0.35f;        // of half the screen
    std::unordered_map<u64, u32> seen;
    for (Cmd& c : f.cmds) {
        c.match = -1;
        c.lerp = c.lerpUv = false;
        if (!has_verts(c.op))
            continue;
        const u64 key = key_of(c);
        const auto it = p.index.find(key);
        const u32 nth = seen[key]++;
        if (it == p.index.end() || nth >= it->second.size())
            continue;
        c.match = (int)it->second[nth];
        const Cmd& o = p.cmds[(size_t)c.match];
        if (c.op == Op::Draw2D) {
            if ((c.flags & kHasZ) != (o.flags & kHasZ))
                continue;
            const Vertex* a = p.v2.data() + o.first;
            const Vertex* b = f.v2.data() + c.first;
            bool ok = true;
            for (int i = 0; i < c.count && ok; i++)
                ok = a[i].u == b[i].u && a[i].v == b[i].v && fabsf(a[i].x - b[i].x) <= kJump2D &&
                     fabsf(a[i].y - b[i].y) <= kJump2D;
            c.lerp = ok;
        } else if (c.op == Op::DrawDepth) {
            const Vertex3* a = p.v3.data() + o.first;
            const Vertex3* b = f.v3.data() + c.first;
            bool ok = true;
            for (int i = 0; i < c.count && ok; i++)
                ok = a[i].u == b[i].u && a[i].v == b[i].v && fabsf(a[i].x - b[i].x) <= kJump2D &&
                     fabsf(a[i].y - b[i].y) <= kJump2D;
            c.lerp = ok;
        }
    }
    // 3D: a view (Begin3D … End3D) with the meshes drawn under it is one
    // thing.  It moves smoothly if its meshes come out near where the frame
    // before had them; a cut of the camera or another character in its place
    // does not, and is drawn where it now is.
    // In the world that is asked of the camera and of the world (the user
    // found turning the camera jumpy: the map's meshes near the eye sweep
    // across the screen as it swings round the player, and a quarter of them
    // "jumping" had the whole map drawn without in-betweens, by fits and
    // starts, while the characters and the sky moved smoothly): a camera cut
    // stops every view; else the sky and the map always move, and a
    // character or a thing only if its meshes moved less than a unit.
    const bool world = f.world.set && p.world.set;
    const bool cut = world && camera_cut(f.world, p.world);
    const float kJumpWorld = 1.0f;
    for (size_t b = 0; b < f.cmds.size(); b++) {
        if (f.cmds[b].op != Op::Begin3D)
            continue;
        int owner = -1;
        size_t end = b + 1;
        for (; end < f.cmds.size() && f.cmds[end].op != Op::End3D && f.cmds[end].op != Op::Begin3D; end++)
            if (owner < 0 && f.cmds[end].op == Op::Mesh && f.cmds[end].match >= 0)
                owner = p.cmds[(size_t)f.cmds[end].match].owner;
        if (owner < 0)
            continue;
        const float* ma = p.cmds[(size_t)owner].f;
        const float* mb = f.cmds[b].f;
        int tested = 0, failed = 0;
        for (size_t j = b + 1; j < end; j++) {
            Cmd& c = f.cmds[j];
            if (c.op != Op::Mesh || c.match < 0 || p.cmds[(size_t)c.match].owner != owner || c.count == 0)
                continue;
            const Vertex3* a = p.v3.data() + p.cmds[(size_t)c.match].first;
            const Vertex3* v = f.v3.data() + c.first;
            bool ok = true, uv = true;
            const int picks[3] = {0, c.count / 2, c.count - 1};
            const int kind = f.cmds[b].i;
            if (world) {
                ok = !cut;
                if (ok && kind != (int)hal::gs::View::Sky && kind != (int)hal::gs::View::Map)
                    for (int k = 0; k < 3 && ok; k++) {
                        const Vertex3& pa = a[picks[k]];
                        const Vertex3& pb = v[picks[k]];
                        ok = fabsf(pa.x - pb.x) <= kJumpWorld && fabsf(pa.y - pb.y) <= kJumpWorld &&
                             fabsf(pa.z - pb.z) <= kJumpWorld;
                    }
            } else {
                for (int k = 0; k < 3 && ok; k++) {
                    float sa[2], sb[2];
                    if (project(ma, a[picks[k]], sa) && project(mb, v[picks[k]], sb))
                        ok = fabsf(sa[0] - sb[0]) <= kJump3D && fabsf(sa[1] - sb[1]) <= kJump3D;
                }
            }
            for (int i = 0; i < c.count && uv; i++)
                uv = fabsf(a[i].u - v[i].u) <= 0.5f && fabsf(a[i].v - v[i].v) <= 0.5f;
            tested++;
            failed += !ok;
            c.lerp = ok;
            c.lerpUv = uv;
        }
        static const bool tracePair = getenv("WOTS2_TRACE_PAIR") != nullptr;
        if (tracePair)
            printf("[pair] view kind %d: %d meshes, %d moved too far%s\n", f.cmds[b].i, tested, failed,
                   tested == 0 || failed * 4 > tested ? " -> CUT" : "");
        if (tested == 0 || failed * 4 > tested) {
            for (size_t j = b + 1; j < end; j++)
                f.cmds[j].lerp = false;
        } else {
            f.cmds[b].match = owner;
        }
    }
    // (A mesh left with a pair under another view, or under none, stays put.)
    for (size_t b = 0, view = (size_t)-1; b < f.cmds.size(); b++) {
        Cmd& c = f.cmds[b];
        if (c.op == Op::Begin3D)
            view = b;
        else if (c.op == Op::Mesh && c.lerp &&
                 (view == (size_t)-1 || f.cmds[view].match < 0 || p.cmds[(size_t)c.match].owner != f.cmds[view].match))
            c.lerp = false;
    }
}

namespace hal::gs {

static void apply_window() {
    if (s_window == nullptr)
        return;
    const bool full = (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (full != s_video.fullscreen)
        SDL_SetWindowFullscreen(s_window, s_video.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!s_video.fullscreen && s_video.windowWidth > 0 && s_video.windowHeight > 0) {
        SDL_SetWindowSize(s_window, s_video.windowWidth, s_video.windowHeight);
        SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    // Debug: WOTS2_FAST=1 runs as fast as it can (scripted test runs).
    SDL_GL_SetSwapInterval(getenv("WOTS2_FAST") || !s_video.vsync ? 0 : 1);
}
void set_video(const Video& video) {
    s_video = video;
    apply_window();
}
void save_picture(const char* path) { s_picturePath = path; }
void set_frozen(bool frozen) { s_frozen = frozen; }
void set_rate(int fps, bool vsync) {
    s_video.fps = fps;
    s_video.vsync = vsync;
    if (s_window != nullptr)
        SDL_GL_SetSwapInterval(getenv("WOTS2_FAST") || !vsync ? 0 : 1);
}
Video video() { return s_video; }
void toggle_fullscreen() {
    s_video.fullscreen = !s_video.fullscreen;
    apply_window();
}
float wide_margin() { return s_xoff; }
void set_full_width() { s_recWide = true; }

void init(int width, int height, const char* title) {
    s_width  = width;
    s_height = height;

    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // The PS2's 640×448 frame is shown at 4:3, so the window is 2× wide and
    // slightly more than 2× tall.
    s_window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width * 2, width * 2 * 3 / 4,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    s_gl_ctx = SDL_GL_CreateContext(s_window);
    if (s_gl_ctx == nullptr || !wots2_gl_load()) {
        fprintf(stderr, "[gs] no OpenGL 3.3: %s\n", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Way of the Samurai 2",
                                 "This needs a graphics driver with OpenGL 3.3.", nullptr);
        exit(1);
    }
    apply_window();

    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glLinkProgram(s_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(s_program);
    s_uScreen   = glGetUniformLocation(s_program, "uScreen");
    s_uXoff     = glGetUniformLocation(s_program, "uXoff");
    s_uUvRect   = glGetUniformLocation(s_program, "uUvRect");
    s_uAlphaRef = glGetUniformLocation(s_program, "uAlphaRef");
    s_uDepth = glGetUniformLocation(s_program, "uDepth");
    glUniform1i(glGetUniformLocation(s_program, "uTex"), 0);
    glUniform2f(s_uScreen, (float)width, (float)height);

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(4 * sizeof(float)));

    const u8 white[4] = {255, 255, 255, 255};
    s_white = create_texture(white, 1, 1);

    // By default the frame is the PS2's and the finished frame is scaled to
    // the window, as a TV would (size_frame gives it its storage).
    s_fboTex = create_texture(nullptr, width, height);
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fboTex, 0);
    // (The depth buffer is a texture: the modern lighting reads it.)
    glGenTextures(1, &s_depth);
    glBindTexture(GL_TEXTURE_2D, s_depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[GS] framebuffer incomplete\n");
    s_allocW = width;
    s_allocH = height;

    GLuint vs3 = compile(GL_VERTEX_SHADER, kVertexShader3d);
    GLuint fs3 = compile(GL_FRAGMENT_SHADER, kFragmentShader3d);
    s_program3d = glCreateProgram();
    glAttachShader(s_program3d, vs3);
    glAttachShader(s_program3d, fs3);
    glLinkProgram(s_program3d);
    glDeleteShader(vs3);
    glDeleteShader(fs3);
    glUseProgram(s_program3d);
    s_uMvp = glGetUniformLocation(s_program3d, "uMvp");
    s_uXScale = glGetUniformLocation(s_program3d, "uXScale");
    s_uAlphaRef3d = glGetUniformLocation(s_program3d, "uAlphaRef");
    s_uFog = glGetUniformLocation(s_program3d, "uFog");
    s_uFogColor = glGetUniformLocation(s_program3d, "uFogColor");
    glUniform1i(glGetUniformLocation(s_program3d, "uTex"), 0);
    glUniform1f(s_uXScale, 1.0f);
    s_uKind = glGetUniformLocation(s_program3d, "uKind");
    s_uCutout = glGetUniformLocation(s_program3d, "uCutout");
    s_uShadowMat = glGetUniformLocation(s_program3d, "uShadowMat");
    s_uShadowParams = glGetUniformLocation(s_program3d, "uShadowParams");
    s_uSunDir = glGetUniformLocation(s_program3d, "uSunDir");
    s_uBump = glGetUniformLocation(s_program3d, "uBump");
    s_uUnbake = glGetUniformLocation(s_program3d, "uUnbake");
    s_uAmbient = glGetUniformLocation(s_program3d, "uAmbient");
    s_uSunCol = glGetUniformLocation(s_program3d, "uSunCol");
    s_uEye = glGetUniformLocation(s_program3d, "uEye");
    s_uLights = glGetUniformLocation(s_program3d, "uLights");
    s_uLightPos = glGetUniformLocation(s_program3d, "uLightPos");
    s_uLightCol = glGetUniformLocation(s_program3d, "uLightCol");
    glUniform1i(glGetUniformLocation(s_program3d, "uShadow"), 1);
    glUniform1i(s_uKind, 0);
    glUniform1i(s_uLights, 0);
    // (Unit 1 always holds a depth texture that compares, as the shadow
    // sampler wants, even when nothing is in shadow.)
    {
        const u32 one = 0xFFFFFFFFu;
        glGenTextures(1, &s_noShadowTex);
        glBindTexture(GL_TEXTURE_2D, s_noShadowTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, &one);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_noShadowTex);
        glActiveTexture(GL_TEXTURE0);
    }
    glGenVertexArrays(1, &s_vao3d);
    glBindVertexArray(s_vao3d);
    glGenBuffers(1, &s_vbo3d);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo3d);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex3), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex3), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex3), (void*)(5 * sizeof(float)));
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glUseProgram(s_program);

    glEnable(GL_BLEND);
    apply_blend(0);
    glClearColor(0.f, 0.f, 0.f, 1.f);

    printf("[GS] init %dx%d\n", width, height);
}

void shutdown() {
    SDL_GL_DeleteContext(s_gl_ctx);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
}

// A new list, opening with the state the last one left.
static int s_recNumber = 0;
void begin_frame() {
    s_cur.clear();
    s_cur.number = ++s_recNumber;
    memcpy(push(Op::FrameBegin).f, s_recClear, sizeof(s_recClear));
    push(Op::Blend).i = s_recBlend;
    push(Op::AlphaRef).i = s_recAlphaRef;
    Cmd& fog = push(Op::Fog);
    memcpy(fog.f, s_recFog, sizeof(s_recFog));
    memcpy(fog.f + 4, s_recFogColor, sizeof(s_recFogColor));
    s_recOwner = -1;
}

// A PNG without compression (stored deflate blocks): no image library needed.
static u32 crc32_of(const u8* p, size_t n, u32 crc = 0) {
    static u32 table[256];
    if (table[1] == 0)
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static bool write_png(const char* path, const u8* rgb, int w, int h) {
    std::vector<u8> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw.size() || at == 0; ) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n >= raw.size() ? 1 : 0);
        z.push_back((u8)n); z.push_back((u8)(n >> 8));
        z.push_back((u8)~n); z.push_back((u8)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)at, raw.begin() + (long)(at + n));
        at += n;
        if (n == 0) break;
    }
    const u32 adler = b << 16 | a;
    for (int k = 3; k >= 0; k--) z.push_back((u8)(adler >> (k * 8)));
    FILE* f = fopen(path, "wb");
    if (f == nullptr)
        return false;
    const auto be = [](u8* o, u32 v) { o[0] = (u8)(v >> 24); o[1] = (u8)(v >> 16); o[2] = (u8)(v >> 8); o[3] = (u8)v; };
    const auto chunk = [&](const char* type, const u8* data, size_t n) {
        u8 head[8];
        be(head, (u32)n);
        memcpy(head + 4, type, 4);
        fwrite(head, 1, 8, f);
        if (n) fwrite(data, 1, n, f);
        u8 crc[4];
        be(crc, crc32_of(data, n, crc32_of(head + 4, 4)));
        fwrite(crc, 1, 4, f);
    };
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    u8 ihdr[13];
    be(ihdr, (u32)w);
    be(ihdr + 4, (u32)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), z.size());
    chunk("IEND", nullptr, 0);
    return fclose(f) == 0;
}
static void write_shot(const char* name) {
    const int w = s_physW, h = s_physH;
    std::vector<u8> px((size_t)w * h * 3);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    const char* dir = getenv("WOTS2_SHOT_DIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir ? dir : ".", name);
    if (FILE* f = fopen(path, "wb")) {
        // The frame is drawn with y down, so its rows are already top-first.
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        fwrite(px.data(), 1, px.size(), f);
        fclose(f);
        printf("[GS] wrote %s\n", path);
    }
}

// Debug aid: WOTS2_SHOTS="120,300" writes those frames to WOTS2_SHOT_DIR (default
// ".") as shot_NNNNN.ppm, and WOTS2_EXIT_FRAME=N quits after frame N.
// (WOTS2_SHOT_BETWEEN=1: also the pictures shown in between, shot_NNNNN_k.)
static std::set<int> s_shots;
static int s_exitFrame = -1;
static void parse_frame_hooks() {
    static bool parsed = false;
    if (parsed)
        return;
    parsed = true;
    if (const char* e = getenv("WOTS2_SHOTS"))
        for (const char* p = e; *p; ) {
            s_shots.insert(atoi(p));
            while (*p && *p != ',') p++;
            if (*p) p++;
        }
    if (const char* e = getenv("WOTS2_EXIT_FRAME"))
        s_exitFrame = atoi(e);
}
static void debug_frame_hooks() {
    static const bool traceCull = getenv("WOTS2_TRACE_CULL") != nullptr;
    if (traceCull && s_drawn + s_culled > 0) {
        printf("[cull] frame %d: %d meshes drawn, %d culled\n", s_frame, s_drawn, s_culled);
        fflush(stdout);
    }
    s_drawn = s_culled = 0;
    if (!s_picturePath.empty()) {       // photo mode: the frame without the overlay
        const int w = s_physW, h = s_physH;
        std::vector<u8> px((size_t)w * h * 3);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        printf("[photo] %s %s\n", write_png(s_picturePath.c_str(), px.data(), w, h) ? "saved" : "could not write",
               s_picturePath.c_str());
        fflush(stdout);
        s_picturePath.clear();
    }
    parse_frame_hooks();
    if (s_shots.count(s_frame)) {
        char name[64];
        snprintf(name, sizeof(name), "shot_%05d", s_frame);
        write_shot(name);
    }
    if (s_exitFrame >= 0 && s_frame >= s_exitFrame) {
        printf("[GS] exit at frame %d\n", s_frame);
        exit(0);
    }
}

static void (*s_overlay)() = nullptr;
static int s_presentRect[4] = {0, 0, 640, 448};

// The finished frame to the window: stretched over it as a TV would, or —
// when a shape was asked for — at that shape with black round it.
static void present() {
    int w, h;
    SDL_GL_GetDrawableSize(s_window, &w, &h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (s_video.aspect != 0.0f) {
        const float aspect = s_lw / (float)s_width * 4.0f / 3.0f;
        int fw = w, fh = (int)lroundf((float)w / aspect);
        if (fh > h) {
            fh = h;
            fw = (int)lroundf((float)h * aspect);
        }
        x0 = (w - fw) / 2; y0 = (h - fh) / 2; x1 = x0 + fw; y1 = y0 + fh;
        if (fw != w || fh != h) {
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glBlitFramebuffer(0, s_physH, s_physW, 0, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    s_presentRect[0] = x0; s_presentRect[1] = y0; s_presentRect[2] = x1; s_presentRect[3] = y1;
    if (s_overlay != nullptr) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        s_overlay();
        glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    }
    SDL_GL_SwapWindow(s_window);
}

static int s_refresh = 60;
// How many pictures to show for one frame of the game.
static int pictures_per_frame() {
    if (s_video.fps == 0 || (getenv("WOTS2_FAST") && !getenv("WOTS2_SHOT_BETWEEN")))
        return 1;
    const int rate = s_fps > 0 ? s_fps : 30;
    int refresh = 60;
    SDL_DisplayMode mode;
    if (SDL_GetWindowDisplayMode(s_window, &mode) == 0 && mode.refresh_rate > 0)
        refresh = mode.refresh_rate;
    // (At the display's rate the pictures in between are timed by the clock,
    // see end_frame: this is then the most there can be.)
    int n = s_video.fps < 0 ? (refresh + rate - 1) / rate : (s_video.fps + rate / 2) / rate;
    // (Waiting for the display more often than it refreshes would slow the game down.)
    if (s_video.fps > 0 && s_video.vsync && n * rate > refresh + 1)
        n = refresh / rate;
    s_refresh = refresh;
    return n < 1 ? 1 : n > 16 ? 16 : n;
}

static int rate_of_game() { return s_fps > 0 ? s_fps : 30; }

void end_frame() {
    static double t0 = 0.0;             // when the frame before was shown (ms)
    const double period = 1000.0 / (double)(s_fps > 0 ? s_fps : 30);
    const auto now = []() { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); };
    const auto wait_until = [&](double when) {
        if (getenv("WOTS2_FAST"))
            return;
        const double left = when - now();
        if (left >= 1.0)
            SDL_Delay((Uint32)left);
    };
    size_frame();
    const int n = pictures_per_frame();
    if (n > 1 && !s_prev.cmds.empty() && !s_cur.cmds.empty()) {
        pair_frames(s_cur, s_prev);
        // Following the display, a picture in between is made for the moment
        // it is drawn at (displays do not all refresh a whole number of times
        // in a thirtieth of a second); at a rate asked for, at even steps.
        const bool timed = s_video.fps < 0 && s_video.vsync && !getenv("WOTS2_FAST");
        for (int k = 1; k < n; k++) {
            float t = (float)k / (float)n;
            if (timed) {
                t = (float)((now() - t0) / period);
                if (t >= 1.0f - 0.5f * (float)rate_of_game() / (float)s_refresh)
                    break;
                t = t < 0.0f ? 0.0f : t;
            }
            play(s_cur, &s_prev, t);
            if (getenv("WOTS2_SHOT_BETWEEN")) {
                parse_frame_hooks();
                if (s_shots.count(s_frame)) {
                    char name[64];
                    snprintf(name, sizeof(name), "shot_%05d_%d", s_frame, k);
                    write_shot(name);
                }
            }
            present();
            wait_until(t0 + period * (double)k / (double)n);
        }
    }
    play(s_cur, nullptr, 1.0f);
    debug_frame_hooks();
    s_frame++;

    // Keep the finished frame: the next one is begun before the game's
    // update runs, and an effect may ask for "the picture on the screen".
    if (s_lastFbo == 0) {
        s_lastTex = create_texture(nullptr, s_physW, s_physH);
        glGenFramebuffers(1, &s_lastFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s_lastFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_lastTex, 0);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_lastFbo);
    glBlitFramebuffer(0, 0, s_physW, s_physH, 0, 0, s_physW, s_physH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    present();
    // The game logic is tied to its frame rate (30 fps NTSC, 25 PAL).
    wait_until(t0 + period);
    t0 += period;
    if (now() - t0 > period)            // fell behind (loading): start counting again
        t0 = now();

    for (u32 tex : s_dead) {
        GLuint t = tex;
        glDeleteTextures(1, &t);
    }
    s_dead.clear();
    if (n > 1) {
        index_frame(s_cur);
        std::swap(s_prev, s_cur);
    } else {
        s_prev.clear();
    }
    // (A frame ended without another begun is shown again as it is.)
    s_cur.clear();
}

void set_frame_rate(int fps) { s_fps = fps; }

void clear(u32 rgba) {
    unpack(rgba, s_recClear, false);
    s_recClear[3] = 1.0f;
    memcpy(push(Op::Clear).f, s_recClear, sizeof(s_recClear));
}

void set_blend(Blend mode) {
    s_recBlend = (int)mode;
    push(Op::Blend).i = (int)mode;
}

void set_z(u32 z) { s_recZ = z; }

void set_alpha_ref(int ref) {
    s_recAlphaRef = ref;
    push(Op::AlphaRef).i = ref;
}

u32 create_texture(const u8* rgba, int width, int height) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    set_filter(tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    s_live.insert(tex);
    bool clear = false, opaque = false, between = false;
    for (size_t i = 3; rgba && i < (size_t)width * height * 4 && !between; i += 4) {
        clear |= rgba[i] == 0;
        opaque |= rgba[i] == 255;
        between |= rgba[i] != 0 && rgba[i] != 255;
    }
    if (clear && opaque && !between)
        s_cutout.insert(tex);
    return tex;
}

void set_cutout(u32 tex, bool on) {
    if (on) s_cutout.insert(tex);
    else s_cutout.erase(tex);
}

u32 create_texture_compressed(Compressed format, const Level* levels, int count) {
    const GLenum gl = format == Compressed::DXT1 ? 0x83F1      // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                    : format == Compressed::DXT3 ? 0x83F2      // …DXT3
                    : 0x83F3;                                  // …DXT5
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    for (int i = 0; i < count; i++)
        glCompressedTexImage2D(GL_TEXTURE_2D, i, gl, levels[i].width, levels[i].height, 0, (GLsizei)levels[i].size,
                               levels[i].data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, count - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, count > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s_live.insert(tex);
    return tex;
}

u32 grab_frame() {
    if (s_grabTex == 0) {
        glGenTextures(1, &s_grabTex);
        set_filter(s_grabTex);
    }
    push(Op::Grab);
    return s_grabTex;
}

// (At the game's own size whatever the frame's: what asks works on 640 × 448.)
void read_frame(std::vector<u8>& rgba) {
    rgba.assign((size_t)s_width * s_height * 4, 0);
    if (s_lastFbo == 0)
        return;
    static GLuint small = 0, smallTex = 0;
    GLint was = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &was);
    GLuint from = s_lastFbo;
    if (s_physW != s_width || s_physH != s_height) {
        if (small == 0) {
            smallTex = create_texture(nullptr, s_width, s_height);
            glGenFramebuffers(1, &small);
            glBindFramebuffer(GL_FRAMEBUFFER, small);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, smallTex, 0);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_lastFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, small);
        glBlitFramebuffer(0, 0, s_physW, s_physH, 0, 0, s_width, s_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        from = small;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s_width, s_height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());     // drawn y down: top row first already
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)was);
}

void begin_target(int size) {
    if (s_targetFbo == 0) {
        glGenFramebuffers(1, &s_targetFbo);
        glGenTextures(1, &s_targetTex);
        glGenRenderbuffers(1, &s_targetDepth);
    }
    push(Op::TargetBegin).i = size;
    s_recClear[0] = s_recClear[1] = s_recClear[2] = s_recClear[3] = 0.0f;
    s_recFogBeforeTarget = s_recFog[3];
    s_recFog[3] = 0.0f;
}

u32 end_target() {
    push(Op::TargetEnd);
    s_recFog[3] = s_recFogBeforeTarget;
    return s_targetTex;
}

void destroy_texture(u32 tex) {
    if (tex == 0)
        return;
    if (s_live.erase(tex) == 0) {
        check_live(tex, "asked to give back again");
        return;
    }
    s_cutout.erase(tex);
    s_dead.push_back(tex);
}

// A 2D draw of `count` vertices just added to the list's pool.
static bool own_texture(u32 tex) { return tex == s_white || tex == s_grabTex || tex == s_targetTex; }
static Cmd& push_2d(GLuint tex, Prim prim, u32 first, int count) {
    if (!own_texture(tex))
        check_live(tex, "a draw with");
    Cmd& c = push(Op::Draw2D);
    c.tex = tex;
    c.prim = (u8)prim;
    c.first = first;
    c.count = count;
    if (s_recZ != kNoZ) {
        c.flags |= kHasZ;
        c.z = s_recZ;
        s_recZ = kNoZ;
    }
    if (tex == s_grabTex && s_grabTex != 0)
        c.flags |= kFrameTex;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < count; i++) {
        const float x = s_cur.v2[(size_t)first + (size_t)i].x;
        lo = x < lo ? x : lo;
        hi = x > hi ? x : hi;
    }
    if (lo <= 0.5f && hi >= (float)s_width - 0.5f && (tex == s_white || (c.flags & kFrameTex) || s_recWide))
        c.flags |= kSpan;
    s_recWide = false;
    return c;
}

static void draw_quad(GLuint tex, float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1, u32 rgba, float texelU, float texelV) {
    float c[4];
    unpack(rgba, c, tex != s_white);
    const Vertex verts[6] = {
        {x,     y,     u0, v0, c[0], c[1], c[2], c[3]},
        {x + w, y,     u1, v0, c[0], c[1], c[2], c[3]},
        {x + w, y + h, u1, v1, c[0], c[1], c[2], c[3]},
        {x,     y,     u0, v0, c[0], c[1], c[2], c[3]},
        {x + w, y + h, u1, v1, c[0], c[1], c[2], c[3]},
        {x,     y + h, u0, v1, c[0], c[1], c[2], c[3]},
    };
    const u32 first = (u32)s_cur.v2.size();
    s_cur.v2.insert(s_cur.v2.end(), verts, verts + 6);
    Cmd& cmd = push_2d(tex, Prim::Triangles, first, 6);
    if (tex != s_white && !(cmd.flags & kFrameTex)) {
        cmd.flags |= kUvRect;
        cmd.f[0] = u0 < u1 ? u0 : u1; cmd.f[1] = v0 < v1 ? v0 : v1;
        cmd.f[2] = u0 < u1 ? u1 : u0; cmd.f[3] = v0 < v1 ? v1 : v0;
        cmd.f[4] = texelU / 2.0f;
        cmd.f[5] = texelV / 2.0f;
    }
}

void draw_sprite(u32 tex, int tex_w, int tex_h,
                 float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, u32 rgba) {
    draw_quad(tex, x, y, w, h, u0 / tex_w, v0 / tex_h, u1 / tex_w, v1 / tex_h, rgba, 1.0f / tex_w, 1.0f / tex_h);
}

void fill_rect(float x, float y, float w, float h, u32 rgba) {
    draw_quad(s_white, x, y, w, h, 0, 0, 1, 1, rgba, 0, 0);
}

void draw_prim(u32 tex, int tex_w, int tex_h, Prim prim, const Vtx* verts, int count) {
    const u32 first = (u32)s_cur.v2.size();
    for (int i = 0; i < count; i++) {
        float c[4];
        unpack(verts[i].rgba, c, tex != 0);
        s_cur.v2.push_back({verts[i].x, verts[i].y,
                            tex ? verts[i].u / tex_w : 0.0f, tex ? verts[i].v / tex_h : 0.0f,
                            c[0], c[1], c[2], c[3]});
    }
    push_2d(tex ? tex : s_white, prim, first, count);
}

void begin_3d(const float* mvp, View kind, u32 who) {
    s_recOwner = (int)s_cur.cmds.size();
    s_recWho = who;
    Cmd& c = push(Op::Begin3D);
    memcpy(c.f, mvp, 16 * sizeof(float));
    c.i = (int)kind;
    c.who = who;
}

void set_lighting(const Lighting& lighting) {
    s_light = lighting;
    s_light.shadowSize = s_light.shadowSize < 256 ? 256 : s_light.shadowSize > 8192 ? 8192 : s_light.shadowSize;
    s_light.shadowReach = s_light.shadowReach < 2.0f ? 2.0f : s_light.shadowReach;
    s_light.aoRadius = s_light.aoRadius < 0.05f ? 0.05f : s_light.aoRadius;
}
Lighting lighting() { return s_light; }
// (Only in a frame of the world: elsewhere — the character setup's preview —
// the game's own shadow stays.)
bool real_shadows() { return !s_lightingHeld && s_light.shadows && s_light.shadowStrength > 0.0f && s_cur.world.set; }
bool lighting_held() { return s_lightingHeld; }
bool toggle_lighting() {
    s_lightingHeld = !s_lightingHeld;
    return !s_lightingHeld;
}
void set_world(const float* view, const float* proj, const float* sunDir, const float* sunColour,
               const float* lights, int n, const float* ambient) {
    WorldView& w = s_cur.world;
    if (ambient != nullptr)
        memcpy(w.ambient, ambient, sizeof w.ambient);
    w.set = true;
    memcpy(w.view, view, sizeof w.view);
    memcpy(w.proj, proj, sizeof w.proj);
    memcpy(w.sun, sunDir, sizeof w.sun);
    memcpy(w.sunColour, sunColour, sizeof w.sunColour);
    w.lights.assign(lights, lights + (size_t)(n > 0 ? n : 0) * 8);
}
void world_post() { push(Op::Post); }
void set_ambience(const float* wind, const float* focus, bool indoors, bool night) {
    WorldView& w = s_cur.world;
    w.ambience = true;
    w.indoors = indoors;
    w.night = night;
    memcpy(w.wind, wind, sizeof w.wind);
    memcpy(w.focus, focus, sizeof w.focus);
}

// The game's fog numbers (GsDevice_SetFogParams 0x12E9D0): the coefficient
// runs from `clearAt` / 256 at depth `nearDepth` to `fogAt` / 256 at
// `farDepth`, where drawing stops.  on = false: no fog (the viewers).
void set_fog(bool on, float fogAt, float clearAt, float nearDepth, float farDepth, u32 rgb) {
    s_recFog[3] = on ? 1.0f : 0.0f;
    if (on && farDepth > nearDepth) {
        const float a = (fogAt + clearAt + ((fogAt - clearAt) * (farDepth + nearDepth)) / (farDepth - nearDepth)) / 2.0f;
        const float b = (farDepth * nearDepth * (clearAt - fogAt)) / (farDepth - nearDepth);
        s_recFog[0] = a / 256.0f;
        s_recFog[1] = b / 256.0f;
        s_recFog[2] = farDepth;
    }
    s_recFogColor[0] = (rgb & 0xFF) / 255.0f;
    s_recFogColor[1] = ((rgb >> 8) & 0xFF) / 255.0f;
    s_recFogColor[2] = ((rgb >> 16) & 0xFF) / 255.0f;
    Cmd& fog = push(Op::Fog);
    memcpy(fog.f, s_recFog, sizeof(s_recFog));
    memcpy(fog.f + 4, s_recFogColor, sizeof(s_recFogColor));
}

void set_depth_bias(int steps) { push(Op::Bias).i = steps; }
void set_overlay(bool on)      { push(Op::Overlay).i = on; }

void draw_mesh(u32 tex, const Vtx3* verts, int count, Prim prim) {
    const u32 first = (u32)s_cur.v3.size();
    s_cur.v3.resize((size_t)first + (size_t)count);
    Vertex3* out = s_cur.v3.data() + first;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < count; i++) {
        float c[4];
        unpack(verts[i].rgba, c, tex != 0);
        out[i] = {verts[i].x, verts[i].y, verts[i].z, verts[i].u, verts[i].v, c[0], c[1], c[2], c[3]};
        const float p[3] = {verts[i].x, verts[i].y, verts[i].z};
        for (int k = 0; k < 3; k++) {
            lo[k] = p[k] < lo[k] ? p[k] : lo[k];
            hi[k] = p[k] > hi[k] ? p[k] : hi[k];
        }
    }
    if (!own_texture(tex))
        check_live(tex, "a 3D draw with");
    Cmd& c = push(Op::Mesh);
    memcpy(c.f, lo, sizeof lo);
    memcpy(c.f + 3, hi, sizeof hi);
    c.tex = tex ? tex : s_white;
    c.prim = (u8)prim;
    c.first = first;
    c.count = count;
    c.owner = s_recOwner;
    c.who = s_recWho;
}

// (Kept as the game gave them — screen positions with a depth each — and
// turned into clip space when played.)
void draw_prim_depth(u32 tex, int tex_w, int tex_h, Prim prim, const Vtx* verts, const float* depth, int count) {
    const u32 first = (u32)s_cur.v3.size();
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < count; i++) {
        float c[4];
        unpack(verts[i].rgba, c, tex != 0);
        s_cur.v3.push_back({verts[i].x, verts[i].y, depth[i],
                            tex ? verts[i].u / tex_w : 0.0f, tex ? verts[i].v / tex_h : 0.0f, c[0], c[1], c[2], c[3]});
        lo = verts[i].x < lo ? verts[i].x : lo;
        hi = verts[i].x > hi ? verts[i].x : hi;
    }
    if (!own_texture(tex))
        check_live(tex, "a world draw with");
    Cmd& c = push(Op::DrawDepth);
    c.tex = tex ? tex : s_white;
    c.prim = (u8)prim;
    c.first = first;
    c.count = count;
    if (tex == s_grabTex && s_grabTex != 0)
        c.flags |= kFrameTex;
    if (lo <= 0.5f && hi >= (float)s_width - 0.5f && (tex == 0 || (c.flags & kFrameTex) || s_recWide))
        c.flags |= kSpan;
    s_recWide = false;
}

void end_3d() {
    push(Op::End3D);
    s_recOwner = -1;
    s_recWho = 0;
}

int frame_count()   { return s_frame;  }

SDL_Window* window() { return s_window; }
void* gl_context() { return s_gl_ctx; }
void set_overlay(void (*draw)()) { s_overlay = draw; }
void present_rect(int out[4]) { memcpy(out, s_presentRect, sizeof s_presentRect); }
float clip_x_scale() { return (float)s_width / s_lw; }
int screen_width()  { return s_width;  }
int screen_height() { return s_height; }

}  // namespace hal::gs
