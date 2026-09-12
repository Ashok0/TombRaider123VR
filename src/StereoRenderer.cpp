#include "StereoRenderer.h"
#include "Log.h"

namespace tr {
namespace {
StereoRenderer g_stereo;
}

StereoRenderer& Stereo() { return g_stereo; }

bool StereoRenderer::Create(uint32_t eyeW, uint32_t eyeH) {
    if (m_fbo && m_eyeW == eyeW && m_eyeH == eyeH) return true;
    Destroy();

    if (!gl::Loaded() && !gl::Load()) return false;
    if (eyeW == 0 || eyeH == 0) return false;

    m_eyeW = eyeW;
    m_eyeH = eyeH;

    const GLsizei w = static_cast<GLsizei>(eyeW * 2);
    const GLsizei h = static_cast<GLsizei>(eyeH);

    GLint prevFbo = 0, prevTex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl::GenRenderbuffers(1, &m_depth);
    gl::BindRenderbuffer(GL_RENDERBUFFER, m_depth);
    // The engine's own targets are depth-only, never depth+stencil
    // (ogl_setRenderTarget uses GL_DEPTH_ATTACHMENT and nothing else), so
    // matching that is enough.
    gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    gl::GenFramebuffers(1, &m_fbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);
    gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth);

    const GLenum status = gl::CheckFramebufferStatus(GL_FRAMEBUFFER);

    gl::BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LogF("stereo: framebuffer incomplete (0x%04X)", status);
        Destroy();
        return false;
    }

    LogF("stereo: created %dx%d double-wide target (fbo %u, tex %u)", w, h, m_fbo, m_tex);
    gl::CheckErrors("StereoRenderer::Create");
    return true;
}

void StereoRenderer::Destroy() {
    if (m_monoFbo)   { gl::DeleteFramebuffers(1, &m_monoFbo);      m_monoFbo = 0; }
    if (m_monoDepth) { gl::DeleteRenderbuffers(1, &m_monoDepth);   m_monoDepth = 0; }
    if (m_monoTex)   { glDeleteTextures(1, &m_monoTex);            m_monoTex = 0; }
    m_monoW = m_monoH = 0;

    if (m_fbo)   { gl::DeleteFramebuffers(1, &m_fbo);   m_fbo = 0; }
    if (m_depth) { gl::DeleteRenderbuffers(1, &m_depth); m_depth = 0; }
    if (m_tex)   { glDeleteTextures(1, &m_tex);          m_tex = 0; }
    m_eyeW = m_eyeH = 0;
}

void StereoRenderer::BeginFrame() {
    if (!m_fbo) return;
    gl::BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, static_cast<GLsizei>(m_eyeW * 2), static_cast<GLsizei>(m_eyeH));
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

bool StereoRenderer::CreateMono(uint32_t w, uint32_t h) {
    if (m_monoFbo && m_monoW == w && m_monoH == h) return true;
    if (!gl::Loaded() && !gl::Load()) return false;
    if (w == 0 || h == 0) return false;

    if (m_monoFbo)   { gl::DeleteFramebuffers(1, &m_monoFbo);      m_monoFbo = 0; }
    if (m_monoDepth) { gl::DeleteRenderbuffers(1, &m_monoDepth);   m_monoDepth = 0; }
    if (m_monoTex)   { glDeleteTextures(1, &m_monoTex);            m_monoTex = 0; }

    m_monoW = w;
    m_monoH = h;

    GLint prevFbo = 0, prevTex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    glGenTextures(1, &m_monoTex);
    glBindTexture(GL_TEXTURE_2D, m_monoTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl::GenRenderbuffers(1, &m_monoDepth);
    gl::BindRenderbuffer(GL_RENDERBUFFER, m_monoDepth);
    gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                            (GLsizei)w, (GLsizei)h);

    gl::GenFramebuffers(1, &m_monoFbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, m_monoFbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, m_monoTex, 0);
    gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, m_monoDepth);
    const GLenum status = gl::CheckFramebufferStatus(GL_FRAMEBUFFER);

    gl::BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LogF("stereo: mono scratch incomplete (0x%04X)", status);
        if (m_monoFbo)   { gl::DeleteFramebuffers(1, &m_monoFbo);    m_monoFbo = 0; }
        if (m_monoDepth) { gl::DeleteRenderbuffers(1, &m_monoDepth); m_monoDepth = 0; }
        if (m_monoTex)   { glDeleteTextures(1, &m_monoTex);          m_monoTex = 0; }
        return false;
    }

    LogF("stereo: mono scratch ready (%ux%u, fbo %u, tex %u)",
         w, h, m_monoFbo, m_monoTex);
    return true;
}

void StereoRenderer::BeginMono() {
    if (!m_monoFbo) return;
    gl::BindFramebuffer(GL_FRAMEBUFFER, m_monoFbo);
    glViewport(0, 0, (GLsizei)m_monoW, (GLsizei)m_monoH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void StereoRenderer::BlitMonoToHalf(int eye) {
    if (!m_monoFbo || !m_fbo) return;
    const GLint x = (eye == 0) ? 0 : (GLint)m_eyeW;
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, m_monoFbo);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
    glDisable(GL_SCISSOR_TEST);
    gl::BlitFramebuffer(0, 0, (GLint)m_monoW, (GLint)m_monoH,
                        x, 0, x + (GLint)m_eyeW, (GLint)m_eyeH,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl::BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
}


void StereoRenderer::SetEyeViewport(int eye) {
    if (!m_fbo) return;
    const GLint x = (eye == 0) ? 0 : static_cast<GLint>(m_eyeW);
    glViewport(x, 0, static_cast<GLsizei>(m_eyeW), static_cast<GLsizei>(m_eyeH));

    // init_ogl leaves GL_SCISSOR_TEST enabled globally, so the scissor box has
    // to track the viewport or the second eye gets clipped away by whatever
    // rectangle the engine last set.
    glScissor(x, 0, static_cast<GLsizei>(m_eyeW), static_cast<GLsizei>(m_eyeH));
}

void StereoRenderer::CompareHalves(int& samples, int& differing) {
    samples = differing = 0;
    if (!m_fbo) return;

    GLint prevRead = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);

    // A 5x5 interior grid. Correct stereo at any sane IPD moves most of these
    // by far more than the threshold; identical halves move none of them.
    const int N = 5;
    for (int gy = 1; gy <= N; ++gy) {
        for (int gx = 1; gx <= N; ++gx) {
            const GLint x = static_cast<GLint>(m_eyeW * gx / (N + 1));
            const GLint y = static_cast<GLint>(m_eyeH * gy / (N + 1));
            unsigned char a[4] = { 0, 0, 0, 0 };
            unsigned char b[4] = { 0, 0, 0, 0 };
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
            glReadPixels(x + static_cast<GLint>(m_eyeW), y, 1, 1,
                         GL_RGBA, GL_UNSIGNED_BYTE, b);
            ++samples;
            int d = 0;
            for (int c = 0; c < 3; ++c) {
                d += (a[c] > b[c]) ? (a[c] - b[c]) : (b[c] - a[c]);
            }
            if (d > 8) ++differing;
        }
    }

    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
}

void StereoRenderer::MarkEyes() {
    if (!m_fbo) return;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    const GLsizei bw = static_cast<GLsizei>(m_eyeW / 4);
    const GLsizei bh = static_cast<GLsizei>(m_eyeH / 12);
    const GLint   bx = static_cast<GLint>(m_eyeW / 2) - bw / 2;

    glEnable(GL_SCISSOR_TEST);
    glScissor(bx, 0, bw, bh);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(bx + static_cast<GLint>(m_eyeW), 0, bw, bh);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    gl::BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
}

void StereoRenderer::MirrorToWindow(int winW, int winH, GLuint windowFbo) {
    if (!m_fbo || winW <= 0 || winH <= 0) return;

    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, windowFbo);
    glDisable(GL_SCISSOR_TEST);
    gl::BlitFramebuffer(0, 0, static_cast<GLint>(m_eyeW), static_cast<GLint>(m_eyeH),
                        0, 0, winW, winH,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl::BindFramebuffer(GL_FRAMEBUFFER, windowFbo);
}

} // namespace tr
