// VideoPanel.h -- offscreen capture and true world-space replay of the engine's
// clip-space-direct passes.
//
// Five of the engine's shaders write gl_Position = vec4(aCoord, 1.0): straight
// to clip space, no matrix at all. Pre-rendered video cutscenes go through that
// path, so nothing put in a uniform can move them.
//
// The viewport-shift approach gets close -- projecting the panel's four corners
// and fitting the viewport to their bounding box recovers position and size --
// but a bounding box is a rectangle and an off-axis quad should be a trapezoid.
// That residual keystone is visible when looking around.
//
// The only way to remove it is to stop faking the transform and make the video
// real geometry:
//
//   1. Capture: render the pass ONCE into an offscreen texture, at the size the
//      engine expected, with no per-eye trickery.
//   2. Replay: draw a genuine quad per eye, positioned in the game camera's
//      frame and transformed by the real per-eye matrices.
//
// The quad is then ordinary geometry, so it keystones, foreshortens and rolls
// exactly like the world does. It also means the video is rasterised once
// rather than twice, which very nearly pays for the extra pass.
#pragma once

#include "GL.h"
#include "StereoMath.h"
#include <cstdint>

namespace tr {

class VideoPanel {
public:
    // Requires a current GL context and gl::LoadedShaderApi(). Idempotent;
    // recreates if the size changed. Returns false if anything failed, in which
    // case the caller should fall back to the viewport-shift path.
    bool Create(uint32_t width, uint32_t height);
    void Destroy();

    bool valid() const { return m_fbo != 0 && m_prog != 0; }
    GLuint fbo() const { return m_fbo; }

    // Bind the capture target and clear it. The engine's draw then lands here
    // instead of in the eye target.
    void BeginCapture();

    // Draw the captured texture as a real quad, using `mvp` for this eye.
    // Restores the GL state it disturbs, and stamps the engine's shadow state so
    // the engine rebinds what it cares about on its next draw.
    void Replay(const mat4& mvp, bool flipV);

private:
    GLuint m_fbo   = 0;
    GLuint m_tex   = 0;
    GLuint m_depth = 0;
    GLuint m_prog  = 0;
    GLuint m_vao   = 0;
    GLuint m_vbo   = 0;
    GLint  m_uMvp  = -1;
    GLint  m_uTex  = -1;
    GLint  m_uFlip = -1;
    uint32_t m_w = 0, m_h = 0;
};

VideoPanel& Video();

} // namespace tr
