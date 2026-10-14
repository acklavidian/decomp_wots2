#pragma once
// OpenGL beyond 1.1 on Windows, whose opengl32 exports nothing newer: the
// functions are fetched from the driver once a context exists.  Include after
// <SDL2/SDL_opengl.h> (with GL_GLEXT_PROTOTYPES); elsewhere this is nothing.
#ifdef _WIN32
#include <SDL2/SDL.h>

#define WOTS2_GL_FUNCTIONS(X) \
    X(glBindFramebuffer) X(glGetUniformLocation) X(glUniform1f) X(glVertexAttribPointer) \
    X(glEnableVertexAttribArray) X(glBindBuffer) X(glUseProgram) X(glBindVertexArray) X(glGenFramebuffers) \
    X(glFramebufferTexture2D) X(glDeleteShader) X(glAttachShader) X(glUniform4f) X(glRenderbufferStorage) \
    X(glBufferData) X(glBlitFramebuffer) X(glBlendEquation) X(glBindRenderbuffer) X(glUniform4fv) \
    X(glUniform2f) X(glUniform1i) X(glLinkProgram) X(glGenVertexArrays) X(glGenRenderbuffers) X(glGenBuffers) \
    X(glFramebufferRenderbuffer) X(glCreateProgram) X(glUniformMatrix4fv) X(glUniform3fv) X(glShaderSource) \
    X(glGetShaderiv) X(glGetShaderInfoLog) X(glCreateShader) X(glCompileShader) X(glCheckFramebufferStatus) \
    X(glGetProgramiv) X(glGetProgramInfoLog) X(glActiveTexture) X(glUniform3f) X(glDrawBuffers) X(glClearBufferfv) X(glCompressedTexImage2D) \
    X(glTexImage3D) X(glFramebufferTextureLayer)

#define X(name) inline decltype(&::name) wots2_##name = nullptr;
WOTS2_GL_FUNCTIONS(X)
#undef X

// → false (naming the first one missing) if the driver lacks any.
inline bool wots2_gl_load() {
#define X(name) \
    wots2_##name = reinterpret_cast<decltype(&::name)>(SDL_GL_GetProcAddress(#name)); \
    if (wots2_##name == nullptr) { SDL_Log("OpenGL: no %s", #name); return false; }
    WOTS2_GL_FUNCTIONS(X)
#undef X
    return true;
}

#define glBindFramebuffer wots2_glBindFramebuffer
#define glGetUniformLocation wots2_glGetUniformLocation
#define glUniform1f wots2_glUniform1f
#define glVertexAttribPointer wots2_glVertexAttribPointer
#define glEnableVertexAttribArray wots2_glEnableVertexAttribArray
#define glBindBuffer wots2_glBindBuffer
#define glUseProgram wots2_glUseProgram
#define glBindVertexArray wots2_glBindVertexArray
#define glGenFramebuffers wots2_glGenFramebuffers
#define glFramebufferTexture2D wots2_glFramebufferTexture2D
#define glDeleteShader wots2_glDeleteShader
#define glAttachShader wots2_glAttachShader
#define glUniform4f wots2_glUniform4f
#define glRenderbufferStorage wots2_glRenderbufferStorage
#define glBufferData wots2_glBufferData
#define glBlitFramebuffer wots2_glBlitFramebuffer
#define glBlendEquation wots2_glBlendEquation
#define glBindRenderbuffer wots2_glBindRenderbuffer
#define glUniform4fv wots2_glUniform4fv
#define glUniform2f wots2_glUniform2f
#define glUniform1i wots2_glUniform1i
#define glLinkProgram wots2_glLinkProgram
#define glGenVertexArrays wots2_glGenVertexArrays
#define glGenRenderbuffers wots2_glGenRenderbuffers
#define glGenBuffers wots2_glGenBuffers
#define glFramebufferRenderbuffer wots2_glFramebufferRenderbuffer
#define glCreateProgram wots2_glCreateProgram
#define glUniformMatrix4fv wots2_glUniformMatrix4fv
#define glUniform3fv wots2_glUniform3fv
#define glShaderSource wots2_glShaderSource
#define glGetShaderiv wots2_glGetShaderiv
#define glGetShaderInfoLog wots2_glGetShaderInfoLog
#define glCreateShader wots2_glCreateShader
#define glCompileShader wots2_glCompileShader
#define glCheckFramebufferStatus wots2_glCheckFramebufferStatus
#define glGetProgramiv wots2_glGetProgramiv
#define glGetProgramInfoLog wots2_glGetProgramInfoLog
#define glActiveTexture wots2_glActiveTexture
#define glUniform3f wots2_glUniform3f
#define glDrawBuffers wots2_glDrawBuffers
#define glClearBufferfv wots2_glClearBufferfv
#define glCompressedTexImage2D wots2_glCompressedTexImage2D
#define glTexImage3D wots2_glTexImage3D
#define glFramebufferTextureLayer wots2_glFramebufferTextureLayer
#else
inline bool wots2_gl_load() { return true; }
#endif
