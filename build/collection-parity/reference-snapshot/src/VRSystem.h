// VRSystem.h -- OpenVR runtime binding.
//
// openvr_api.dll is loaded at runtime through its exported C entry points
// (VR_InitInternal / VR_GetGenericInterface / VR_ShutdownInternal) rather than
// linked against openvr_api.lib. Only the header is needed to build, which
// keeps the project buildable from a headers-only drop of the SDK and avoids
// an import that would make the DLL fail to load when the runtime is absent.
#pragma once

#include "StereoMath.h"
#include "GunHaptics.h"
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
    uint64_t PoseSerial() const { return m_poseSerial; }
    float FirstPersonHeightDrop() const;
    float FirstPersonPostureDrop() const; // Standing/crouch inference only; never changes the eye.

    // Eye transform in ENGINE view space, ready to be composed onto the game's
    // view matrix:   finalView = EyeView(eye) * gameView
    Affine EyeView(Eye eye) const;
    // View of a pose captured in raw tracking space. Independent of gameplay
    // camera/FP neutrals, which can change when the inventory pauses the game.
    Affine AnchoredEyeView(Eye eye,const vr::HmdMatrix34_t& anchor) const;

    // The same transform WITHOUT the per-eye offset: head centre rather than
    // either pupil. This is what the room culling wants -- one frustum that
    // covers both eyes, rather than two traversals that would agree about
    // everything except a 64 mm baseline. In mono, EyeView IS this.
    Affine HeadView() const;

    // Half-angle tangents of a SYMMETRIC frustum that contains both eyes'
    // asymmetric ones: the largest |l|,|r| and |t|,|b| across the pair.
    //
    // Deliberately conservative. Culling wants a superset of what will be
    // rendered, and taking the max of the magnitudes removes every dependence
    // on which way round OpenVR's raw tangents are signed.
    //
    // False before the projection is known, in which case the caller should
    // fall back to something wide rather than cull with a guess.
    bool CullTangents(float& tanX, float& tanY) const;

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
    // Queue a strong, bounded burst for a confirmed shot (0=left, 1=right).
    void GunShotHaptic(int hand);
    // Refresh or stop gentle proximity feedback for this hand's holster.
    void HolsterHoverHaptic(int hand,bool active);
    // Both hands catch the ledge: one maximum-strength 80 ms burst.
    void LedgeGrabHaptic();
    // Frame-latched tracking-space poses, matching the HMD sample used by
    // BeginFrame. Hand 0 is left, hand 1 is right.
    bool ControllerPose(int hand, vr::HmdMatrix34_t& out) const;
    bool HeadPose(vr::HmdMatrix34_t& out) const;
    // Eye-height anchor with horizontal facing only (no headset pitch/roll).
    bool LevelHeadPose(vr::HmdMatrix34_t& out) const;
    // Reset the application's tracking heading and position for a view toggle.
    // Head and both controllers share this origin; runtime room setup is untouched.
    void RecenterView();
    // Controller translation relative to the FP camera's head anchor, in
    // metres (right, down, forward). Applies the same neck correction as
    // HeadView so the rendered gun does not drift when turning in place.
    bool FirstPersonControllerOffset(int hand, float& right, float& down,
                                     float& forward) const;

    bool poseValid() const { return m_poseValid; }

    // Physical HMD yaw in tracking space. First person combines it with a
    // stable tracking-to-world heading for rendering and movement.
    float HeadYawRadians() const;
    float HeadPitchRadians() const;
    // Capture a position neutral immediately, without changing tracked rotation.
    void RecenterFirstPersonHead();
    // FP -> chase camera handoff: start third-person translation at the
    // current physical position, including views already sampled this frame.
    void RecenterThirdPersonHead();
    void HeadFloorOffset(float& right, float& forward) const;
    void FirstPersonViewOffset(float& right, float& forward) const;
    // Physical vertical displacement in engine Y convention (metres), without
    // modifying tracking neutral. Caller applies translation/optic gates.
    float FirstPersonVerticalOffset() const;
    void ConsumeHeadFloorOffset(float right, float forward);
    void PivotHeadFloorOffset(float yawDelta);

    // Hold the head CENTRE at the game camera, keeping the per-eye offsets.
    //
    // This is not a stereo switch and it is not mono: the eyes still straddle
    // the centre by half an IPD each, so the world keeps every bit of its depth.
    // What goes away is the 6DOF DISPLACEMENT -- the same thing
    // PositionalTracking=0 drops, and through the same line of code.
    //
    // WHY ANYTHING WOULD WANT THAT: the laser sight. Its dot is not a world
    // object, it is a screen-centre crosshair in the 2D overlay pass, so the mod
    // draws it on the flat panel at HudDepthMetres while the bullet lands on the
    // wall -- two points on ONE LINE out of camera.pos. They project to the same
    // pixel only from an eye that is on that line, and a displaced head is not.
    // Put the head centre back on it and the two agree at every distance, with
    // no need to know where the shot landed. The residual per-eye ±32 mm is
    // equal and opposite, so it cancels in the fused direction and shows up as
    // the dot floating in front of the wall rather than as an aiming error.
    //
    // LATCHED ONCE PER FRAME by the caller, never polled per draw. Every use of
    // the eye transform in a frame -- world draws, the 2D panel, the video panel,
    // the culling frustum -- has to agree about where the head is, and a value
    // read from game memory mid-frame could flip between them and put the panel
    // in a different space from the geometry behind it.
    void SetHeadAtCamera(bool on) { m_headAtCamera = on; }
    bool headAtCamera() const     { return m_headAtCamera; }

    // One-shot log flag for the ceiling clamp, so it reports the first time it
    // bites rather than every frame it holds.
    bool m_loggedClamp = false;

    // Re-anchor the world-locked head offset to the game camera on the next
    // frame. Bound to RecentreKey, for when the integrated offset has drifted
    // away from where you are actually sitting. See WorldLockOffset.
    void RecentreOffset();

private:
    uint64_t m_poseSerial=0;
    GunHaptics m_gunHaptics;
    vr::IVRSystem*     m_system     = nullptr;
    vr::IVRCompositor* m_compositor = nullptr;
    HMODULE            m_dll        = nullptr;

    Affine m_headFromTracking = Affine::Identity();  // inverse(hmdPose)
    vr::HmdMatrix34_t m_rawHeadPose{};
    Affine m_trackingFromRuntime = Affine::Identity();
    bool m_viewRecenterPending = false;
    vr::HmdMatrix34_t m_rawControllerPose[2]{};
    bool m_controllerPoseValid[2] = {};
    float m_firstPersonNeutral[3] = {};
    float m_firstPersonNeutralNeck[2] = {};
    float m_firstPersonNeutralEyeNeckY = .08f;
    bool m_firstPersonNeutralValid = false;
    float m_thirdPersonNeutral[3] = {};
    bool m_thirdPersonNeutralValid = false;
    bool m_thirdPersonRecenterPending = false;
    int m_thirdPersonGame = -1, m_thirdPersonLevel = -1;
    uint64_t m_thirdPersonRooms = 0;
    Affine TrackedHeadView() const;
    Affine m_eyeFromHead[2]   = { Affine::Identity(), Affine::Identity() };
    float  m_rawProj[2][4]    = {};                  // l, r, t, b per eye
    bool   m_poseValid        = false;
    mutable float m_headingYaw = 0;
    mutable bool m_headingYawValid = false, m_headingYawVertical = false;
    bool m_runtimeRecenterPending = false, m_runtimeRecenterHadYaw = false;
    float m_runtimeRecenterYaw = 0;
    void HandleRuntimeEvent(const vr::VREvent_t& event);
    void ApplyRuntimeRecenter();
    bool   m_headAtCamera     = false;
    bool   m_loggedTr6Origin  = false;
    unsigned m_poseLogTick    = 0;

    // The world-locked head offset and the state the integration needs. See
    // WorldLockOffset in the .cpp -- offsetWorld is in TR world units and is the
    // only one of these the rest of the mod ever sees the effect of.
    void  WorldLockOffset(vr::HmdMatrix34_t& pose);
    float m_offsetWorld[3]   = {};
    float m_lastView[3]      = {};
    float m_lastCamPos[3]    = {};
    bool  m_offsetValid      = false;
    bool  m_recentreRequested = false;

    uint32_t m_eyeW = 0, m_eyeH = 0;
};

VRSystem& VR();

} // namespace tr
