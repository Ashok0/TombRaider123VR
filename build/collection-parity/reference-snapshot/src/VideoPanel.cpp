#include "VideoPanel.h"
#include "Engine.h"
#include "Log.h"

namespace tr {
namespace {

VideoPanel g_video;

const char* kVert =
    "#version 150\n"
    "in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "uniform mat4 uMVP;\n"
    "uniform int  uFlip;\n"
    "void main() {\n"
    "    vec2 uv = aPos * 0.5 + 0.5;\n"
    "    if (uFlip != 0) uv.y = 1.0 - uv.y;\n"
    "    vUV = uv;\n"
    "    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);\n"
    "}\n";

const char* kFrag =
    "#version 150\n"
    "in vec2 vUV;\n"
    "out vec4 oColor;\n"
    "uniform sampler2D uTex;\n"
    "void main() { oColor = texture(uTex, vUV); }\n";

GLuint Compile(GLenum type, const char* src, const char* what) {
    GLuint sh = gl::CreateShader(type);
    if (!sh) return 0;
    gl::ShaderSource(sh, 1, &src, nullptr);
    gl::CompileShader(sh);
    GLint ok = 0;
    gl::GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        gl::GetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        LogF("video: %s shader failed to compile: %s", what, log);
        gl::DeleteShader(sh);
        return 0;
    }
    return sh;
}

} // namespace

VideoPanel& Video() { return g_video; }

bool VideoPanel::Create(uint32_t width, uint32_t height) {
    if (valid() && m_w == width && m_h == height) return true;
    Destroy();

    if (!gl::Loaded() || !gl::LoadedShaderApi()) {
        Log("video: shader API unavailable, offscreen panel disabled");
        return false;
    }
    if (width == 0 || height == 0) return false;

    m_w = width;
    m_h = height;

    GLint prevFbo = 0, prevTex = 0, prevVao = 0, prevBuf = 0, prevProg = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);

    // --- capture target ---
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)m_w, (GLsizei)m_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl::GenRenderbuffers(1, &m_depth);
    gl::BindRenderbuffer(GL_RENDERBUFFER, m_depth);
    gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                            (GLsizei)m_w, (GLsizei)m_h);

    gl::GenFramebuffers(1, &m_fbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, m_tex, 0);
    gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, m_depth);
    const GLenum status = gl::CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl::BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LogF("video: capture framebuffer incomplete (0x%04X)", status);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        Destroy();
        return false;
    }

    // --- replay program ---
    GLuint vs = Compile(GL_VERTEX_SHADER, kVert, "vertex");
    GLuint fs = vs ? Compile(GL_FRAGMENT_SHADER, kFrag, "fragment") : 0;
    if (vs && fs) {
        m_prog = gl::CreateProgram();
        gl::AttachShader(m_prog, vs);
        gl::AttachShader(m_prog, fs);
        gl::LinkProgram(m_prog);
        GLint ok = 0;
        gl::GetProgramiv(m_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            gl::GetProgramInfoLog(m_prog, sizeof(log) - 1, nullptr, log);
            LogF("video: program failed to link: %s", log);
            gl::DeleteProgram(m_prog);
            m_prog = 0;
        }
    }
    if (vs) gl::DeleteShader(vs);
    if (fs) gl::DeleteShader(fs);

    if (!m_prog) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        Destroy();
        return false;
    }

    m_uMvp  = gl::GetUniformLocation(m_prog, "uMVP");
    m_uTex  = gl::GetUniformLocation(m_prog, "uTex");
    m_uFlip = gl::GetUniformLocation(m_prog, "uFlip");

    // --- unit quad ---
    const float verts[] = {
        -1.0f, -1.0f,   1.0f, -1.0f,   -1.0f,  1.0f,
         1.0f, -1.0f,   1.0f,  1.0f,   -1.0f,  1.0f,
    };
    gl::GenVertexArrays(1, &m_vao);
    gl::BindVertexArray(m_vao);
    gl::GenBuffers(1, &m_vbo);
    gl::BindBuffer(GL_ARRAY_BUFFER, m_vbo);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)sizeof(verts), verts, GL_STATIC_DRAW);
    const GLint aPos = gl::GetAttribLocation(m_prog, "aPos");
    if (aPos >= 0) {
        gl::EnableVertexAttribArray((GLuint)aPos);
        gl::VertexAttribPointer((GLuint)aPos, 2, GL_FLOAT, GL_FALSE,
                                2 * sizeof(float), (const void*)0);
    }

    gl::BindVertexArray((GLuint)prevVao);
    gl::BindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    gl::UseProgram((GLuint)prevProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    LogF("video: offscreen panel ready (%ux%u, fbo %u, tex %u, prog %u)",
         m_w, m_h, m_fbo, m_tex, m_prog);
    gl::CheckErrors("VideoPanel::Create");
    return true;
}

void VideoPanel::Destroy() {
    if (m_vbo)   { gl::DeleteBuffers(1, &m_vbo);          m_vbo = 0; }
    if (m_vao)   { gl::DeleteVertexArrays(1, &m_vao);     m_vao = 0; }
    if (m_prog)  { gl::DeleteProgram(m_prog);             m_prog = 0; }
    if (m_fbo)   { gl::DeleteFramebuffers(1, &m_fbo);     m_fbo = 0; }
    if (m_depth) { gl::DeleteRenderbuffers(1, &m_depth);  m_depth = 0; }
    if (m_tex)   { glDeleteTextures(1, &m_tex);           m_tex = 0; }
    m_w = m_h = 0;
}

void VideoPanel::BeginCapture() {
    if (!valid()) return;
    gl::BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, (GLsizei)m_w, (GLsizei)m_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void VideoPanel::Replay(const mat4& mvp, bool flipV) {
    if (!valid()) return;

    GLint prevVao = 0, prevBuf = 0, prevProg = 0, prevTex = 0, prevActive = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);

    gl::ActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    const GLboolean hadDepth   = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean hadBlend   = glIsEnabled(GL_BLEND);
    const GLboolean hadCull    = glIsEnabled(GL_CULL_FACE);
    const GLboolean hadScissor = glIsEnabled(GL_SCISSOR_TEST);

    // The panel is opaque and owns its pixels: no depth interaction, no
    // blending, and no culling, so quad winding cannot matter.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    gl::UseProgram(m_prog);
    if (m_uMvp  >= 0) gl::UniformMatrix4fv(m_uMvp, 1, GL_FALSE, mvp.m);
    if (m_uTex  >= 0) gl::Uniform1i(m_uTex, 0);
    if (m_uFlip >= 0) gl::Uniform1i(m_uFlip, flipV ? 1 : 0);
    glBindTexture(GL_TEXTURE_2D, m_tex);

    gl::BindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Restore what we touched.
    gl::BindVertexArray((GLuint)prevVao);
    gl::BindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    gl::UseProgram((GLuint)prevProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    gl::ActiveTexture((GLenum)prevActive);

    if (hadDepth)   glEnable(GL_DEPTH_TEST);
    if (hadBlend)   glEnable(GL_BLEND);
    if (hadCull)    glEnable(GL_CULL_FACE);
    if (hadScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

    // The engine caches what it last bound and skips redundant calls. We have
    // been behind its back, so invalidate the parts we disturbed: 0xCA is the
    // out-of-range shader sentinel init_ogl itself uses, and null vb/ib forces
    // the vertex array and buffers to be rebound on the next draw.
    VidStatePrev().shader = 0xCA;
    VidStatePrev().vb = nullptr;
    VidStatePrev().ib = nullptr;
    VidStatePrev().tex[0] = 0xFFFFFFFFu;

    gl::CheckErrors("VideoPanel::Replay");
}

} // namespace tr
