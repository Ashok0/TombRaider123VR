// StereoRenderer.h -- one double-wide eye target and the viewport bookkeeping
// around it.
//
// Layout is a single 2W x H RGBA8 texture: left eye occupies [0,W), right eye
// [W,2W). Submitting is then two Submit() calls differing only in UV bounds,
// with no second FBO and no attachment switch between eyes.
//
// This deliberately does NOT reuse the engine's FBO_custom. That is a single
// global (created in init_ogl, destroyed in ogl_release) shared by every
// offscreen pass in the game, and ogl_setRenderTarget re-attaches colour and
// depth and calls glCheckFramebufferStatus on every change. Borrowing it would
// mean fighting the engine for attachments twice per frame.
#pragma once

#include "GL.h"
#include <cstdint>

namespace tr {

class StereoRenderer {
public:
    // Requires a current GL context. Idempotent; recreates if the size changed.
    bool Create(uint32_t eyeWidth, uint32_t eyeHeight);
    void Destroy();

    bool valid() const { return m_fbo != 0; }

    GLuint texture() const { return m_tex; }
    GLuint fbo()     const { return m_fbo; }
    uint32_t eyeWidth()  const { return m_eyeW; }
    uint32_t eyeHeight() const { return m_eyeH; }

    // Bind the eye target and clear it. Called once per frame.
    void BeginFrame();

    // --- alternate-eye scratch target ---------------------------------------
    //
    // Under AER only one half is redrawn per frame, so the other must survive
    // untouched. It cannot: FBO_default is the double-wide target, and any
    // full-target glClear the engine issues wipes BOTH halves -- which reads as
    // a hard flicker at half the frame rate, with no head movement needed.
    //
    // So AER points FBO_default at this single-eye scratch instead. The engine
    // clears and renders into it exactly as it would a normal backbuffer, and
    // present blits the result into one half. The other half is never bound, so
    // nothing can disturb it.
    bool CreateMono(uint32_t w, uint32_t h);
    bool monoValid() const { return m_monoFbo != 0; }
    GLuint monoFbo() const { return m_monoFbo; }

    // Bind the scratch target and clear it. Once per frame, before the game
    // renders.
    void BeginMono();

    // Blit the scratch into one half of the eye target. The engine renders at
    // its own size and the halves are narrower, so this rescales -- which is
    // correct, because the per-eye projection already carries the HMD's aspect.
    void BlitMonoToHalf(int eye);

    // Set viewport + scissor to one half of the double-wide target.
    void SetEyeViewport(int eye);

    // Blit the left half back to the game's window framebuffer so the flat
    // screen still shows something sensible.
    void MirrorToWindow(int windowWidth, int windowHeight, GLuint windowFbo);

    // Sample a grid of pixels from each half of the rendered frame and count
    // how many differ. Everything upstream of this -- matrices, viewport,
    // target, submit bounds -- has been verified correct while the headset
    // still shows mono, so the only remaining question is whether the two
    // halves actually contain different pixels. This answers it directly.
    // Costs a pipeline stall, so call it rarely.
    void CompareHalves(int& samples, int& differing);

    // Burn a red bar into the left half and a blue bar into the right half,
    // in the same place in each. Purely a human-readable eye-mapping check:
    //   red in left eye, blue in right ... halves map to eyes correctly
    //   both bars in both eyes .......... submit bounds are being ignored
    //   same colour in both eyes ........ both eyes get the same half
    //   no bars at all .................. what you see is not this texture
    void MarkEyes();

private:
    GLuint   m_monoFbo   = 0;
    GLuint   m_monoTex   = 0;
    GLuint   m_monoDepth = 0;
    uint32_t m_monoW = 0, m_monoH = 0;

    GLuint   m_fbo   = 0;
    GLuint   m_tex   = 0;
    GLuint   m_depth = 0;
    uint32_t m_eyeW  = 0;
    uint32_t m_eyeH  = 0;
};

StereoRenderer& Stereo();

} // namespace tr
