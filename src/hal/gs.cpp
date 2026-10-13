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
uniform sampler2D uTex;
uniform float uAlphaRef;
uniform vec4 uFog;          // A, B, far distance, on
uniform vec3 uFogColor;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vUv) * vColor;
    if (c.a < uAlphaRef) discard;
    c = clamp(c, 0.0, 1.0);
    if (uFog.w > 0.5) {
        if (vDepth > uFog.z) discard;
        float clear = clamp(uFog.x + uFog.y / max(vDepth, 0.001), 0.0, 1.0);
        c.rgb = mix(uFogColor, c.rgb, clear);
        c.a *= clamp((uFog.z - vDepth) / 4.0, 0.0, 1.0) > 0.0 ? 1.0 : 0.0;
    }
    oColor = c;
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
void main() {
    vUv = aUv;
    vColor = aColor;
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
                     Fog, Bias, Overlay, Grab, TargetBegin, TargetEnd };
enum : u8 { kHasZ = 1, kUvRect = 2, kFrameTex = 4, kSpan = 8 };
struct Cmd {
    Op  op;
    u8  prim = 0, flags = 0;
    bool lerp = false, lerpUv = false;  // (played in between) its vertices go part of the way from `match`
    u32 tex = 0, z = 0, first = 0;
    int count = 0, i = 0;
    int owner = -1;                     // a mesh: its Begin3D
    int match = -1;                     // the same draw in the frame before
    float f[16] = {};
};
struct Frame {
    std::vector<Cmd> cmds;
    std::vector<Vertex> v2;
    std::vector<Vertex3> v3;
    std::unordered_map<u64, std::vector<u32>> index;    // draws by what they are, in order
    void clear() { cmds.clear(); v2.clear(); v3.clear(); index.clear(); }
};
static Frame s_cur, s_prev;
static std::vector<u32> s_dead;         // textures to delete once the frame has been played

// What the recording side knows of the state that outlives a frame (a list
// starts by putting it back, so that it can be played twice).
static float s_recClear[4] = {0, 0, 0, 1};
static int   s_recBlend = 0, s_recAlphaRef = 0;
static float s_recFog[4] = {0, 0, 0, 0}, s_recFogColor[3] = {0, 0, 0}, s_recFogBeforeTarget = 0;
static u32   s_recZ = kNoZ;
static bool  s_recWide = false;
static int   s_recOwner = -1;

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
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    if (s_lastTex != 0) {
        glBindTexture(GL_TEXTURE_2D, s_lastTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    if (s_frame > 0)
        printf("[GS] frame %d x %d (%.0f x %d of the game's pixels)\n", w, h, s_lw, s_height);
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

static void state_3d(const float* mvp, float xscale) {
    glUseProgram(s_program3d);
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
    glDepthMask(on ? GL_FALSE : GL_TRUE);
    glUniform1f(s_uAlphaRef3d, on ? 0.02f : 0.5f);
}
static void state_2d() {
    bias(0);
    overlay(false);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(s_program);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
}

// Plays a frame's list into the frame buffer.  With `before` and t < 1: the
// picture part of the way (t) from the frame before to this one.
static void play(const Frame& f, const Frame* before, float t) {
    static const float kIdentity[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    static const float kNoFog[4] = {0, 0, 0, 0};
    const bool scaled = s_physH != s_height;
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
            state_3d(m, s_inTarget ? 1.0f : (float)s_width / s_lw);
            break;
        }
        case Op::Mesh: {
            const Vertex3* v = verts3(f, c, before, t);
            glBindTexture(GL_TEXTURE_2D, c.tex);
            if (c.tex != s_white) {     // model textures tile
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            }
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)c.count * sizeof(Vertex3)), v, GL_STREAM_DRAW);
            glDrawArrays(gl_prim(c.prim), 0, c.count);
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
static u64 key_of(const Cmd& c) {
    return ((u64)c.op << 56) ^ ((u64)c.prim << 48) ^ ((u64)(u32)c.count << 20) ^ (u64)c.tex * 0x9E3779B97F4A7C15ull;
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
            for (int k = 0; k < 3 && ok; k++) {
                float sa[2], sb[2];
                if (project(ma, a[picks[k]], sa) && project(mb, v[picks[k]], sb))
                    ok = fabsf(sa[0] - sb[0]) <= kJump3D && fabsf(sa[1] - sb[1]) <= kJump3D;
            }
            for (int i = 0; i < c.count && uv; i++)
                uv = fabsf(a[i].u - v[i].u) <= 0.5f && fabsf(a[i].v - v[i].v) <= 0.5f;
            tested++;
            failed += !ok;
            c.lerp = ok;
            c.lerpUv = uv;
        }
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
    glGenRenderbuffers(1, &s_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_depth);
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
void begin_frame() {
    s_cur.clear();
    memcpy(push(Op::FrameBegin).f, s_recClear, sizeof(s_recClear));
    push(Op::Blend).i = s_recBlend;
    push(Op::AlphaRef).i = s_recAlphaRef;
    Cmd& fog = push(Op::Fog);
    memcpy(fog.f, s_recFog, sizeof(s_recFog));
    memcpy(fog.f + 4, s_recFogColor, sizeof(s_recFogColor));
    s_recOwner = -1;
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

void destroy_texture(u32 tex) { s_dead.push_back(tex); }

// A 2D draw of `count` vertices just added to the list's pool.
static Cmd& push_2d(GLuint tex, Prim prim, u32 first, int count) {
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

void begin_3d(const float* mvp) {
    s_recOwner = (int)s_cur.cmds.size();
    memcpy(push(Op::Begin3D).f, mvp, 16 * sizeof(float));
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
    for (int i = 0; i < count; i++) {
        float c[4];
        unpack(verts[i].rgba, c, tex != 0);
        out[i] = {verts[i].x, verts[i].y, verts[i].z, verts[i].u, verts[i].v, c[0], c[1], c[2], c[3]};
    }
    Cmd& c = push(Op::Mesh);
    c.tex = tex ? tex : s_white;
    c.prim = (u8)prim;
    c.first = first;
    c.count = count;
    c.owner = s_recOwner;
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
}

int frame_count()   { return s_frame;  }
int screen_width()  { return s_width;  }
int screen_height() { return s_height; }

}  // namespace hal::gs
