#include "GL.h"
#include "Log.h"

namespace gl {

void (APIENTRY* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
void (APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
void (APIENTRY* BindFramebuffer)(GLenum, GLuint) = nullptr;
void (APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
void (APIENTRY* GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
void (APIENTRY* DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
void (APIENTRY* BindRenderbuffer)(GLenum, GLuint) = nullptr;
void (APIENTRY* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
void (APIENTRY* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
GLenum (APIENTRY* CheckFramebufferStatus)(GLenum) = nullptr;
void (APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint,
                                 GLint, GLint, GLint, GLint,
                                 GLbitfield, GLenum) = nullptr;
void (APIENTRY* GetUniformfv)(GLuint, GLint, GLfloat*) = nullptr;

GLuint (APIENTRY* CreateShader)(GLenum) = nullptr;
void   (APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*) = nullptr;
void   (APIENTRY* CompileShader)(GLuint) = nullptr;
void   (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
void   (APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*) = nullptr;
void   (APIENTRY* DeleteShader)(GLuint) = nullptr;
GLuint (APIENTRY* CreateProgram)(void) = nullptr;
void   (APIENTRY* AttachShader)(GLuint, GLuint) = nullptr;
void   (APIENTRY* LinkProgram)(GLuint) = nullptr;
void   (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void   (APIENTRY* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*) = nullptr;
void   (APIENTRY* DeleteProgram)(GLuint) = nullptr;
void   (APIENTRY* UseProgram)(GLuint) = nullptr;
GLint  (APIENTRY* GetUniformLocation)(GLuint, const char*) = nullptr;
void   (APIENTRY* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
void   (APIENTRY* Uniform1i)(GLint, GLint) = nullptr;
void   (APIENTRY* GenVertexArrays)(GLsizei, GLuint*) = nullptr;
void   (APIENTRY* BindVertexArray)(GLuint) = nullptr;
void   (APIENTRY* DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
void   (APIENTRY* GenBuffers)(GLsizei, GLuint*) = nullptr;
void   (APIENTRY* BindBuffer)(GLenum, GLuint) = nullptr;
void   (APIENTRY* BufferData)(GLenum, GLsizeiptr_t, const void*, GLenum) = nullptr;
void   (APIENTRY* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
void   (APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
void   (APIENTRY* EnableVertexAttribArray)(GLuint) = nullptr;
GLint  (APIENTRY* GetAttribLocation)(GLuint, const char*) = nullptr;
void   (APIENTRY* ActiveTexture)(GLenum) = nullptr;

namespace { bool g_shaderApi = false; }
bool LoadedShaderApi() { return g_shaderApi; }

namespace {

bool g_loaded = false;

template <typename T>
bool Grab(T& fn, const char* name) {
    fn = reinterpret_cast<T>(wglGetProcAddress(name));
    if (!fn) {
        // A few of these live in opengl32.dll itself on some drivers.
        static HMODULE ogl = GetModuleHandleW(L"opengl32.dll");
        if (ogl) fn = reinterpret_cast<T>(GetProcAddress(ogl, name));
    }
    if (!fn) LogF("gl: failed to resolve %s", name);
    return fn != nullptr;
}

} // namespace

bool Loaded() { return g_loaded; }

bool Load() {
    if (g_loaded) return true;
    if (!wglGetCurrentContext()) {
        Log("gl: no current context, deferring loader");
        return false;
    }

    bool ok = true;
    ok &= Grab(GenFramebuffers,        "glGenFramebuffers");
    ok &= Grab(DeleteFramebuffers,     "glDeleteFramebuffers");
    ok &= Grab(BindFramebuffer,        "glBindFramebuffer");
    ok &= Grab(FramebufferTexture2D,   "glFramebufferTexture2D");
    ok &= Grab(GenRenderbuffers,       "glGenRenderbuffers");
    ok &= Grab(DeleteRenderbuffers,    "glDeleteRenderbuffers");
    ok &= Grab(BindRenderbuffer,       "glBindRenderbuffer");
    ok &= Grab(RenderbufferStorage,    "glRenderbufferStorage");
    ok &= Grab(FramebufferRenderbuffer,"glFramebufferRenderbuffer");
    ok &= Grab(CheckFramebufferStatus, "glCheckFramebufferStatus");
    ok &= Grab(BlitFramebuffer,        "glBlitFramebuffer");

    // Diagnostics only. Deliberately not folded into `ok`: a driver that does
    // not hand this out must not disable stereo, it just loses the GPU-side
    // verification line in the log.
    Grab(GetUniformfv, "glGetUniformfv");

    // Shader/buffer set for VideoPanel. Kept out of `ok` for the same reason as
    // GetUniformfv: if a driver will not hand these over we lose the offscreen
    // video panel, not stereo.
    bool sh = true;
    sh &= Grab(CreateShader,            "glCreateShader");
    sh &= Grab(ShaderSource,            "glShaderSource");
    sh &= Grab(CompileShader,           "glCompileShader");
    sh &= Grab(GetShaderiv,             "glGetShaderiv");
    sh &= Grab(GetShaderInfoLog,        "glGetShaderInfoLog");
    sh &= Grab(DeleteShader,            "glDeleteShader");
    sh &= Grab(CreateProgram,           "glCreateProgram");
    sh &= Grab(AttachShader,            "glAttachShader");
    sh &= Grab(LinkProgram,             "glLinkProgram");
    sh &= Grab(GetProgramiv,            "glGetProgramiv");
    sh &= Grab(GetProgramInfoLog,       "glGetProgramInfoLog");
    sh &= Grab(DeleteProgram,           "glDeleteProgram");
    sh &= Grab(UseProgram,              "glUseProgram");
    sh &= Grab(GetUniformLocation,      "glGetUniformLocation");
    sh &= Grab(UniformMatrix4fv,        "glUniformMatrix4fv");
    sh &= Grab(Uniform1i,               "glUniform1i");
    sh &= Grab(GenVertexArrays,         "glGenVertexArrays");
    sh &= Grab(BindVertexArray,         "glBindVertexArray");
    sh &= Grab(DeleteVertexArrays,      "glDeleteVertexArrays");
    sh &= Grab(GenBuffers,              "glGenBuffers");
    sh &= Grab(BindBuffer,              "glBindBuffer");
    sh &= Grab(BufferData,              "glBufferData");
    sh &= Grab(DeleteBuffers,           "glDeleteBuffers");
    sh &= Grab(VertexAttribPointer,     "glVertexAttribPointer");
    sh &= Grab(EnableVertexAttribArray, "glEnableVertexAttribArray");
    sh &= Grab(GetAttribLocation,       "glGetAttribLocation");
    sh &= Grab(ActiveTexture,           "glActiveTexture");
    g_shaderApi = sh;

    g_loaded = ok;
    LogF("gl: loader %s (shader api %s)", ok ? "ready" : "INCOMPLETE",
         g_shaderApi ? "ready" : "unavailable");
    return ok;
}

void CheckErrors(const char* where) {
    for (int i = 0; i < 8; ++i) {
        GLenum e = glGetError();
        if (e == GL_NO_ERROR) return;
        LogF("gl: error 0x%04X at %s", e, where);
    }
}

} // namespace gl
