// VRSystem.h -- OpenVR runtime binding.
//
// openvr_api.dll is loaded at runtime through its exported C entry points
// (VR_InitInternal / VR_GetGenericInterface / VR_ShutdownInternal) rather than
// linked against openvr_api.lib. Only the header is needed to build, which
// keeps the project buildable from a headers-only drop of the SDK and avoids
// an import that would make the DLL fail to load when the runtime is absent.
#pragma once

#include "StereoMath.h"
#include "GL.h"        // GLuint, and windows.h for HMODULE
#include <openvr.h>

namespace tr {

enum class Eye { Left = 0, Right = 1 };

class VRSystem {
public:
    bool Init();
    void Shutdown();

    bool active() const { return m_system != nullptr; }

    // Recommended per-eye target size, already multiplied by SuperSample and
    // overridden by the ini if it set explicit values.
    void GetEyeSize(uint32_t& w, uint32_t& h) const;

    // Blocks until the compositor says it is time to render, then latches the
    // HMD pose for this frame. Call once per frame, at the frame boundary.
    void BeginFrame();

    // Eye transform in ENGINE view space, ready to be composed onto the game's
    // view matrix:   finalView = EyeView(eye) * gameView
    Affine EyeView(Eye eye) const;

    // Fills `out` with this eye's projection in the engine's mProj layout,
    // preserving the near/far currently in use.
    void EyeProjection(Eye eye, float zNear, float zFar, mat4& out) const;

    // Hand the finished double-wide texture to the compositor. `bounds` picks
    // the half: left = [0,0.5], right = [0.5,1].
    void Submit(GLuint doubleWideTex, uint32_t texW, uint32_t texH);

    // Horizontal NDC shift for the flat 2D layer (HUD, menus, subtitles) so it
    // fuses at `depthMetres` instead of tearing apart.
    //
    // The 2D layer is drawn with the engine's ortho projection, identical in
    // both eyes. The headset optics apply a fixed per-eye correction assuming an
    // asymmetric render -- about 15 degrees each way here -- so an unshifted
    // image gets pulled apart and cannot fuse. 3D content carries that shear in
    // its projection; the ortho layer has to be given it explicitly.
    //
    //   shift = -P02  +  s * P00 * halfSeparation / depth
    //           ^align with the optics   ^converge to a comfortable distance
    float HudNdcShiftX(Eye eye, float depthMetres) const;

    // --- controllers ---------------------------------------------------------
    // Read through OpenVR's LEGACY input API. We ship no action manifest, so
    // SteamVR puts us in legacy mode and this is what it fills in. The new Input
    // API would need a manifest plus per-controller binding files, which is a
    // lot of machinery for "act like an Xbox pad".
    struct HandState {
        bool     valid      = false;
        float    stickX     = 0.0f;   // -1..1
        float    stickY     = 0.0f;
        float    trigger    = 0.0f;   //  0..1
        float    grip       = 0.0f;   //  0..1
        bool     btnLower   = false;  // A on the right hand, X on the left
        bool     btnUpper   = false;  // B on the right hand, Y on the left
        bool     stickClick = false;
        uint64_t rawPressed = 0;      // for the ini-mapping log
    };
    // [0] = left hand, [1] = right hand.
    void ReadControllers(HandState out[2]) const;

    bool poseValid() const { return m_poseValid; }

    // One-shot log flag for the ceiling clamp, so it reports the first time it
    // bites rather than every frame it holds.
    bool m_loggedClamp = false;

private:
    vr::IVRSystem*     m_system     = nullptr;
    vr::IVRCompositor* m_compositor = nullptr;
    HMODULE            m_dll        = nullptr;

    Affine m_headFromTracking = Affine::Identity();  // inverse(hmdPose)
    Affine m_eyeFromHead[2]   = { Affine::Identity(), Affine::Identity() };
    float  m_rawProj[2][4]    = {};                  // l, r, t, b per eye
    bool   m_poseValid        = false;
    unsigned m_poseLogTick    = 0;

    uint32_t m_eyeW = 0, m_eyeH = 0;
};

VRSystem& VR();

} // namespace tr
