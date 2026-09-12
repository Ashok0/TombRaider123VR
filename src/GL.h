// GL.h -- the small slice of OpenGL this project needs.
//
// The game creates a 3.2 Core context (init_ogl asks for CTX_MAJOR 3 /
// CTX_MINOR 2 / PROFILE_CORE via wglCreateContextAttribsARB). opengl32.lib only
// exports GL 1.1, so everything framebuffer-related has to come through
// wglGetProcAddress once a context is current.
#pragma once

#include <windows.h>
#include <gl/GL.h>
#include <cstdint>

// --- enums the Windows GL header predates ----------------------------------
#define GL_FRAMEBUFFER            0x8D40
#define GL_READ_FRAMEBUFFER       0x8CA8
#define GL_DRAW_FRAMEBUFFER       0x8CA9
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_DEPTH_COMPONENT24      0x81A6
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_FRAMEBUFFER_BINDING    0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_TEXTURE_2D_ARRAY       0x8C1A
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_SRGB8_ALPHA8           0x8C43
#define GL_RGBA8                  0x8058
#define GL_FRAMEBUFFER_SRGB       0x8DB9
#define GL_CURRENT_PROGRAM        0x8B8D
#define GL_VERTEX_SHADER          0x8B31
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_ARRAY_BUFFER           0x8892
#define GL_STATIC_DRAW            0x88E4
#define GL_TEXTURE0               0x84C0
#define GL_VERTEX_ARRAY_BINDING   0x85B5
#define GL_ARRAY_BUFFER_BINDING   0x8894
#define GL_ACTIVE_TEXTURE         0x84E0

typedef ptrdiff_t GLsizeiptr_t;

namespace gl {

// GL 3.0+ framebuffer / renderbuffer entry points.
extern void (APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
extern void (APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
extern void (APIENTRY* BindFramebuffer)(GLenum, GLuint);
extern void (APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
extern void (APIENTRY* GenRenderbuffers)(GLsizei, GLuint*);
extern void (APIENTRY* DeleteRenderbuffers)(GLsizei, const GLuint*);
extern void (APIENTRY* BindRenderbuffer)(GLenum, GLuint);
extern void (APIENTRY* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
extern void (APIENTRY* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
extern GLenum (APIENTRY* CheckFramebufferStatus)(GLenum);
extern void (APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint,
                                        GLint, GLint, GLint, GLint,
                                        GLbitfield, GLenum);

// GL 2.0. Used only by the per-eye verification in Hooks.cpp, so its absence is
// reported but not fatal -- a driver without it loses the diagnostic, not stereo.
extern void (APIENTRY* GetUniformfv)(GLuint, GLint, GLfloat*);

// GL 2.0/3.0 shader + buffer entry points, used only by VideoPanel.
extern GLuint (APIENTRY* CreateShader)(GLenum);
extern void   (APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
extern void   (APIENTRY* CompileShader)(GLuint);
extern void   (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
extern void   (APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
extern void   (APIENTRY* DeleteShader)(GLuint);
extern GLuint (APIENTRY* CreateProgram)(void);
extern void   (APIENTRY* AttachShader)(GLuint, GLuint);
extern void   (APIENTRY* LinkProgram)(GLuint);
extern void   (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
extern void   (APIENTRY* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*);
extern void   (APIENTRY* DeleteProgram)(GLuint);
extern void   (APIENTRY* UseProgram)(GLuint);
extern GLint  (APIENTRY* GetUniformLocation)(GLuint, const char*);
extern void   (APIENTRY* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
extern void   (APIENTRY* Uniform1i)(GLint, GLint);
extern void   (APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
extern void   (APIENTRY* BindVertexArray)(GLuint);
extern void   (APIENTRY* DeleteVertexArrays)(GLsizei, const GLuint*);
extern void   (APIENTRY* GenBuffers)(GLsizei, GLuint*);
extern void   (APIENTRY* BindBuffer)(GLenum, GLuint);
extern void   (APIENTRY* BufferData)(GLenum, GLsizeiptr_t, const void*, GLenum);
extern void   (APIENTRY* DeleteBuffers)(GLsizei, const GLuint*);
extern void   (APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
extern void   (APIENTRY* EnableVertexAttribArray)(GLuint);
extern GLint  (APIENTRY* GetAttribLocation)(GLuint, const char*);
extern void   (APIENTRY* ActiveTexture)(GLenum);

// True once the shader/buffer set above resolved. VideoPanel needs all of it.
bool LoadedShaderApi();

// Resolve everything. Requires a current context. Safe to call repeatedly.
bool Load();
bool Loaded();

// Drains and logs any pending GL errors, tagged with `where`.
void CheckErrors(const char* where);

} // namespace gl
