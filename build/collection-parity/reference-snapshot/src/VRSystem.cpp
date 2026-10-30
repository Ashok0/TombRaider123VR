#include "VRSystem.h"
#include "Config.h"
#include "Log.h"
#include "GL.h"
#include "GameDll.h"
#include "FirstPerson.h"
#include "Gamepad.h"
#include "LocomotionMath.h"

#include <cstdio>
#include <cmath>
#include <algorithm>

namespace tr {
namespace {

// openvr_api.dll's exported C entry points. openvr.h declares these; we resolve
// them by hand so the DLL has no static import on the runtime.
typedef uint32_t (VR_CALLTYPE* PFN_VR_InitInternal)(vr::EVRInitError*, vr::EVRApplicationType);
typedef void     (VR_CALLTYPE* PFN_VR_ShutdownInternal)();
typedef void*    (VR_CALLTYPE* PFN_VR_GetGenericInterface)(const char*, vr::EVRInitError*);
typedef bool     (VR_CALLTYPE* PFN_VR_IsHmdPresent)();
typedef bool     (VR_CALLTYPE* PFN_VR_IsRuntimeInstalled)();
typedef const char* (VR_CALLTYPE* PFN_VR_GetErrorDesc)(vr::EVRInitError);
typedef const char* (VR_CALLTYPE* PFN_VR_GetErrorSymbol)(vr::EVRInitError);

PFN_VR_InitInternal       g_initInternal     = nullptr;
PFN_VR_ShutdownInternal   g_shutdownInternal = nullptr;
PFN_VR_GetGenericInterface g_getInterface    = nullptr;
PFN_VR_IsHmdPresent       g_isHmdPresent     = nullptr;
PFN_VR_IsRuntimeInstalled g_isRuntimeInstalled = nullptr;
PFN_VR_GetErrorDesc       g_errDesc          = nullptr;
PFN_VR_GetErrorSymbol     g_errSymbol        = nullptr;

// Turn an EVRInitError into something a human can act on. Far more use than the
// bare number: the runtime's own strings name the actual problem.
void LogInitError(const char* what, vr::EVRInitError err) {
    LogF("vr: %s failed -- %s (%d): %s",
         what,
         g_errSymbol ? g_errSymbol(err) : "?",
         static_cast<int>(err),
         g_errDesc ? g_errDesc(err) : "no description available");
}

VRSystem g_vr;

// OpenVR's HmdMatrix34_t is already row-major 3x4, same shape as our Affine.
Affine FromHmd(const vr::HmdMatrix34_t& m) {
    Affine a{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            a.r[i][j] = m.m[i][j];
    return a;
}

vr::HmdMatrix34_t TransformHmd(const Affine& frame,const vr::HmdMatrix34_t& pose) {
    const auto a=Mul(frame,FromHmd(pose));
    vr::HmdMatrix34_t result{};
    for(int i=0;i<3;++i) for(int j=0;j<4;++j) result.m[i][j]=a.r[i][j];
    return result;
}

} // namespace

VRSystem& VR() { return g_vr; }

bool VRSystem::Init() {
    m_dll = LoadLibraryW(L"openvr_api.dll");
    if (!m_dll) {
        Log("vr: openvr_api.dll not found next to the exe or on PATH");
        return false;
    }

    g_initInternal     = reinterpret_cast<PFN_VR_InitInternal>(
                             GetProcAddress(m_dll, "VR_InitInternal"));
    g_shutdownInternal = reinterpret_cast<PFN_VR_ShutdownInternal>(
                             GetProcAddress(m_dll, "VR_ShutdownInternal"));
    g_getInterface     = reinterpret_cast<PFN_VR_GetGenericInterface>(
                             GetProcAddress(m_dll, "VR_GetGenericInterface"));
    g_isHmdPresent     = reinterpret_cast<PFN_VR_IsHmdPresent>(
                             GetProcAddress(m_dll, "VR_IsHmdPresent"));
    g_isRuntimeInstalled = reinterpret_cast<PFN_VR_IsRuntimeInstalled>(
                             GetProcAddress(m_dll, "VR_IsRuntimeInstalled"));
    g_errDesc          = reinterpret_cast<PFN_VR_GetErrorDesc>(
                             GetProcAddress(m_dll, "VR_GetVRInitErrorAsEnglishDescription"));
    g_errSymbol        = reinterpret_cast<PFN_VR_GetErrorSymbol>(
                             GetProcAddress(m_dll, "VR_GetVRInitErrorAsSymbol"));

    if (!g_initInternal || !g_getInterface || !g_shutdownInternal) {
        Log("vr: openvr_api.dll is missing expected exports");
        return false;
    }

    {
        wchar_t loaded[MAX_PATH]{};
        GetModuleFileNameW(m_dll, loaded, MAX_PATH);
        char narrow[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, loaded, -1, narrow, MAX_PATH, nullptr, nullptr);
        LogF("vr: using %s", narrow);
    }

    // These two are diagnostics, NOT gates.
    //
    // VR_IsHmdPresent is a lightweight config probe and returns false in
    // situations where initialisation would in fact succeed -- a headset in
    // standby is the common one. Refusing to continue on it throws away the
    // real EVRInitError, which is the only thing that actually says what is
    // wrong. So: report it and carry on regardless.
    LogF("vr: runtime installed=%s, IsHmdPresent=%s",
         (g_isRuntimeInstalled && g_isRuntimeInstalled()) ? "yes" : "no",
         (g_isHmdPresent && g_isHmdPresent()) ? "yes" : "no");

    // Mono bring-up prefers a Background app: it reads tracking without taking
    // over as the VR scene application, so nothing depends on the compositor
    // and SteamVR will not complain about a missing frame loop.
    const bool mono = Cfg().monoTracking;
    vr::EVRApplicationType appType =
        mono ? vr::VRApplication_Background : vr::VRApplication_Scene;
    const char* appName = mono ? "Background" : "Scene";

    vr::EVRInitError err = vr::VRInitError_None;
    g_initInternal(&err, appType);

    // A Background app deliberately will not start SteamVR; it attaches to a
    // server that is already up. If none is, retry as an Overlay app, which
    // will bring the runtime up and still does not own the scene.
    if (err == vr::VRInitError_Init_NoServerForBackgroundApp && mono) {
        LogInitError("VR_InitInternal(Background)", err);
        Log("vr: no running SteamVR to attach to; retrying as an Overlay app");
        err = vr::VRInitError_None;
        appType = vr::VRApplication_Overlay;
        appName = "Overlay";
        g_initInternal(&err, appType);
    }

    if (err != vr::VRInitError_None) {
        char what[64]{};
        _snprintf_s(what, sizeof(what), _TRUNCATE, "VR_InitInternal(%s)", appName);
        LogInitError(what, err);
        return false;
    }
    LogF("vr: initialised as a %s app", appName);

    m_system = static_cast<vr::IVRSystem*>(
                   g_getInterface(vr::IVRSystem_Version, &err));
    if (!m_system || err != vr::VRInitError_None) {
        LogInitError("VR_GetGenericInterface(IVRSystem)", err);
        LogF("vr: the runtime does not serve %s -- openvr_api.dll and SteamVR "
             "are probably different generations", vr::IVRSystem_Version);
        g_shutdownInternal();
        return false;
    }

    // The compositor is required for stereo and irrelevant for mono.
    m_compositor = static_cast<vr::IVRCompositor*>(
                       g_getInterface(vr::IVRCompositor_Version, &err));
    if (!m_compositor && !mono) {
        LogInitError("VR_GetGenericInterface(IVRCompositor)", err);
        g_shutdownInternal();
        m_system = nullptr;
        return false;
    }

    // Latch the per-eye constants. These do not change while the runtime is up.
    for (int e = 0; e < 2; ++e) {
        const vr::Hmd_Eye eye = (e == 0) ? vr::Eye_Left : vr::Eye_Right;
        m_eyeFromHead[e] = InvertRigid(FromHmd(m_system->GetEyeToHeadTransform(eye)));
        m_system->GetProjectionRaw(eye,
                                   &m_rawProj[e][0], &m_rawProj[e][1],
                                   &m_rawProj[e][2], &m_rawProj[e][3]);
        LogF("vr: eye %d raw tangents l=%.4f r=%.4f t=%.4f b=%.4f",
             e, m_rawProj[e][0], m_rawProj[e][1], m_rawProj[e][2], m_rawProj[e][3]);

        // The eye offset is the entire source of stereo separation. If these
        // are zero the headset is reporting no IPD and no amount of world-scale
        // tuning can produce depth -- so say so loudly rather than let it look
        // like a scale problem.
        const vr::HmdMatrix34_t& e2h = m_system->GetEyeToHeadTransform(eye);
        LogF("vr: eye %d eyeToHead offset = (%+.4f, %+.4f, %+.4f) m",
             e, e2h.m[0][3], e2h.m[1][3], e2h.m[2][3]);
        const float mag = std::fabs(e2h.m[0][3]) + std::fabs(e2h.m[1][3]) + std::fabs(e2h.m[2][3]);
        if (mag < 1e-5f) {
            LogF("vr: WARNING eye %d has a ZERO eye-to-head offset -- there will be "
                 "no stereo separation regardless of WorldUnitsPerMetre", e);
        }
    }

    m_system->GetRecommendedRenderTargetSize(&m_eyeW, &m_eyeH);
    const auto& c = Cfg();
    if (c.eyeWidth  > 0) m_eyeW = static_cast<uint32_t>(c.eyeWidth);
    if (c.eyeHeight > 0) m_eyeH = static_cast<uint32_t>(c.eyeHeight);
    m_eyeW = static_cast<uint32_t>(m_eyeW * c.superSample);
    m_eyeH = static_cast<uint32_t>(m_eyeH * c.superSample);

    LogF("vr: ready, per-eye target %ux%u", m_eyeW, m_eyeH);
    return true;
}

void VRSystem::Shutdown() {
    m_gunHaptics.Reset();
    if (m_system && g_shutdownInternal) g_shutdownInternal();
    m_system     = nullptr;
    m_compositor = nullptr;
    m_loggedTr6Origin = false;
    m_poseValid = false;
    m_headingYawValid=m_headingYawVertical=false;
    m_runtimeRecenterPending=m_runtimeRecenterHadYaw=false;
    m_trackingFromRuntime=Affine::Identity();
    m_viewRecenterPending=false;
    m_controllerPoseValid[0] = m_controllerPoseValid[1] = false;
    m_firstPersonNeutralValid = false;
    m_thirdPersonNeutralValid = m_thirdPersonRecenterPending = false;
    if (m_dll) { FreeLibrary(m_dll); m_dll = nullptr; }
}

void VRSystem::GetEyeSize(uint32_t& w, uint32_t& h) const {
    w = m_eyeW;
    h = m_eyeH;
}

// ---------------------------------------------------------------------------
// WorldLockOffset -- the headset displaces the eye; the stick never does
// ---------------------------------------------------------------------------
//
// THE PROBLEM. Composing finalView = EyeView * gameView puts the eye at
//
//     p_eye = camPos - R_g^T * R_e^T * t_e
//
// with R_g the GAME CAMERA's rotation. The head's offset is therefore carried in
// a frame the game rotates, so rotating the camera SWEEPS the eye on an arc of
// radius |offset| while the player stands perfectly still. Turn with the look
// stick and your viewpoint travels sideways into walls; the game's own camera
// collision keeps camPos clear and knows nothing about that arc. Camera pitch
// does the same thing vertically, turning a forward lean into rise and fall.
//
// PositionalTracking=0 only appeared to fix it by making the offset zero.
//
// WHAT THIS DOES INSTEAD. The offset is kept in WORLD units and updated only by
// what the headset actually did:
//
//     offsetWorld += R_g^T * (v(t) - v(t-1))
//     p_eye        = camPos + offsetWorld
//
// Rotate the camera with the stick and v does not change, so offsetWorld does not
// change, so the eye does not move -- exactly PositionalTracking=0's behaviour for
// anything the camera does. Move your head and the delta is applied in the frame
// you are facing AT THAT MOMENT, so leaning forward always goes into the screen
// and full 6DOF is intact -- exactly PositionalTracking=1's behaviour for the
// headset. Camera TRANSLATION still carries you along, because the offset is
// measured from camPos.
//
// Integrating the delta rather than freezing a reference frame is the whole
// trick. A frozen frame would keep "forward" pointing wherever it pointed when
// the anchor was set, which in a game whose camera re-aims itself constantly
// decays into "leaning does something arbitrary" within a minute.
//
// WHAT IT COSTS. The offset becomes state, so it can drift away from your
// physical centre: lean out, turn 90 degrees, lean back, and the two legs cancel
// in different world directions. It is bounded by how far you actually move,
// RecentreKey re-anchors it on demand, and a camera jump re-anchors it
// automatically -- which covers level loads, cutscene cuts and flyby starts,
// where a carried-over offset would be meaningless.
//
// WHY THE POSE. This is the only place the head is still a plain POSITION. After
// InvertRigid it is a view transform whose translation column is -R^T*p, and
// editing that moves the eye along two axes at once.
//
// AND THE POSE'S OWN ROTATION CANCELS OUT of the conversion, which is what keeps
// this cheap. Writing the pose as (R_p, P) and following the real code path:
//
//     headFromTracking = (R_p^T, -R_p^T P)
//     ToEngineSpace conjugates by F = diag(1,-1,1) and scales the translation,
//     so H = (F R_p^T F, -s F R_p^T P)
//     the head's position in H's own space is -R_h^T t_h = s F P
//
// -- the rotation drops out, as it must: where the head IS does not depend on
// where it is LOOKING. So the head's offset in the engine's view space is just
// its tracked position, scaled, with the Y flip applied, plus one sign on Z to
// go from the mod's -Z-forward eye space to phd view's +Z-forward. Checked
// numerically against that chain rather than trusted.
void VRSystem::WorldLockOffset(vr::HmdMatrix34_t& pose) {
    // The native chase camera already owns collision. An absolute tracking-space
    // position is NOT an offset from that camera: adding it moves the rendered
    // eye outside the position the game collision-tested. Cold TR4/5 startup
    // needs the same relative neutral as an FP handoff, without a mode toggle.
    const int game=CurrentGame();
    if (game != 2) {
        if (game==0 || game==1) {
            // A valid w2v matrix can survive loading while InTitle is already
            // clear. Do not capture a desk/loading pose as the gameplay neutral.
            const auto* dll=GameDllBound();
            const auto base=GameDllBase();
            const bool bound=dll && base && dll->game==game && dll->room && dll->numberRooms && dll->laraItem;
            const uint64_t rooms=bound ? *reinterpret_cast<const uint64_t*>(base+dll->room) : 0;
            const bool loaded=rooms && *reinterpret_cast<const int16_t*>(base+dll->numberRooms)>0 &&
                *reinterpret_cast<const uint64_t*>(base+dll->laraItem);
            const int level=AppState(drva::app_off::level);
            if (!loaded || InTitle() || InFMV()) {
                m_thirdPersonNeutralValid=false;
                m_thirdPersonGame=m_thirdPersonLevel=-1;
                m_thirdPersonRooms=0;
                m_offsetValid=false;
                for(int i=0;i<3;++i) { pose.m[i][3]=0; m_offsetWorld[i]=0; }
                return;
            }
            if (m_thirdPersonGame!=-1 && (m_thirdPersonGame!=game ||
                m_thirdPersonLevel!=level || m_thirdPersonRooms!=rooms)) {
                m_thirdPersonNeutralValid=false;
                m_offsetValid=false;
            }
            m_thirdPersonGame=game; m_thirdPersonLevel=level; m_thirdPersonRooms=rooms;
        }
        if (m_thirdPersonRecenterPending && m_poseValid) RecenterThirdPersonHead();
        if ((game==0 || game==1) && !m_thirdPersonNeutralValid) {
            float initialRot[3][3], initialPos[3];
            if (m_poseValid && !InTitle() && !InFMV() && !InInventory() &&
                CameraViewFrame(initialRot,initialPos)) {
                RecenterThirdPersonHead();
                Log("vr: TR4/5 third-person startup neutral captured at gameplay camera");
            } else {
                // Loading/title may sample a headset on the desk. Keep the view
                // at the native camera until gameplay can establish its neutral.
                // Preserve rotation and stereo IPD; do not latch a stale pose.
                for (int i=0;i<3;++i) { pose.m[i][3]=0; m_offsetWorld[i]=0; }
                m_offsetValid=false;
                return;
            }
        }
        if (m_thirdPersonNeutralValid)
            for (int i = 0; i < 3; ++i) pose.m[i][3] -= m_thirdPersonNeutral[i];
    } else {
        m_thirdPersonNeutralValid = m_thirdPersonRecenterPending = false;
        m_thirdPersonGame=m_thirdPersonLevel=-1;
        m_thirdPersonRooms=0;
    }
    const auto& c = Cfg();
    const float s = LiveWorldUnitsPerMetre();

    // Rotation-only tracking zeroes the offset downstream anyway, and the camera
    // mode is the old behaviour by request.
    if (!c.positionalTracking || !c.headOffsetWorld || !(s > 0.0f)) {
        m_offsetValid = false;
        return;
    }

    float rot[3][3], camPos[3];
    if (!CameraViewFrame(rot, camPos)) {
        // No level, or the camera has not been set up yet. Pass the pose through
        // and re-anchor on the first frame that has a real camera, so nothing is
        // integrated or clamped against a frame that does not exist.
        m_offsetValid = false;
        return;
    }

    const float fy = c.flipViewY ? -1.0f : 1.0f;
    const float v[3] = {      s * pose.m[0][3],
                        fy *  s * pose.m[1][3],
                             -s * pose.m[2][3] };

    // A camera jump -- level load, cutscene cut, flyby, teleport -- makes a
    // carried-over offset meaningless. Two sectors is far enough that no walk or
    // camera swing reaches it in one frame.
    bool anchor = !m_offsetValid || m_recentreRequested;
    if (!anchor) {
        const float dx = camPos[0] - m_lastCamPos[0];
        const float dy = camPos[1] - m_lastCamPos[1];
        const float dz = camPos[2] - m_lastCamPos[2];
        if (dx * dx + dy * dy + dz * dz > 2048.0f * 2048.0f) anchor = true;
    }

    if (anchor) {
        // Start from what the old camera-framed behaviour would have given right
        // now, so re-anchoring never jumps the view.
        for (int i = 0; i < 3; ++i) {
            m_offsetWorld[i] = rot[0][i] * v[0] + rot[1][i] * v[1] + rot[2][i] * v[2];
        }
        if (m_recentreRequested) {
            m_recentreRequested = false;
            Log("vr: head offset re-anchored to the game camera");
        }
    } else {
        const float dv[3] = { v[0] - m_lastView[0],
                              v[1] - m_lastView[1],
                              v[2] - m_lastView[2] };
        for (int i = 0; i < 3; ++i) {
            m_offsetWorld[i] += rot[0][i] * dv[0] + rot[1][i] * dv[1]
                              + rot[2][i] * dv[2];
        }
    }

    for (int i = 0; i < 3; ++i) {
        m_lastView[i]   = v[i];
        m_lastCamPos[i] = camPos[i];
    }
    m_offsetValid = true;

    // Ceiling clearance, now applied to the offset that is actually used rather
    // than to tracking-space height -- which was only the same quantity while the
    // camera had no pitch. Clamped on the way OUT, leaving the integrated state
    // alone, so walking into a taller room gives your real height back.
    //
    // TR is Y-DOWN: the eye is above the camera when out[1] is negative, and the
    // ceiling sits headroom units above, so out[1] >= margin - headroom.
    float out[3] = { m_offsetWorld[0], m_offsetWorld[1], m_offsetWorld[2] };
    float headroom = 0.0f;
    if (c.ceilingClearance && CameraHeadroom(headroom)) {
        const float floorLimit = c.ceilingMarginUnits - headroom;
        if (out[1] < floorLimit) {
            out[1] = floorLimit;
            if (!m_loggedClamp) {
                m_loggedClamp = true;
                LogF("vr: ceiling clamp active -- headroom %.0f units, eye held "
                     "%.0f units above the camera", headroom, -floorLimit);
            }
        }
    }

    // Back to a pose position that reproduces `out` in the camera's current
    // frame. Everything downstream -- both eyes, the culling frustum, the 2D
    // panel -- then sees one consistent head, and 1/fy == fy for +-1.
    float ve[3];
    for (int i = 0; i < 3; ++i) {
        ve[i] = rot[i][0] * out[0] + rot[i][1] * out[1] + rot[i][2] * out[2];
    }
    pose.m[0][3] =      ve[0] / s;
    pose.m[1][3] = fy * ve[1] / s;
    pose.m[2][3] =     -ve[2] / s;
}

void VRSystem::BeginFrame() {
    ++m_poseSerial;
    m_controllerPoseValid[0] = m_controllerPoseValid[1] = false;
    if (!m_system) return;
    // The right System button is normally owned by the runtime. Observe its
    // tracking-origin reset, rather than attempting to remap that button.
    vr::VREvent_t event{};
    while(m_system->PollNextEvent(&event,sizeof(event))) HandleRuntimeEvent(event);

    m_gunHaptics.Update(GetTickCount64(),[&](int hand,unsigned short duration) {
        const auto role=hand==0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
        const auto device=m_system->GetTrackedDeviceIndexForControllerRole(role);
        if (device!=vr::k_unTrackedDeviceIndexInvalid && m_system->IsTrackedDeviceConnected(device))
            m_system->TriggerHapticPulse(device,0,duration);
    });

    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];

    if (Cfg().monoTracking) {
        // Read tracking directly. WaitGetPoses would block on the compositor's
        // frame loop and expects a Submit to follow, neither of which applies
        // when we are only sampling the head pose.
        //
        // Seated by default: standing space is absolute room coordinates, so
        // the head's ~1.6 m height would be scaled into several hundred TR
        // units of camera displacement before the player moves at all.
        const vr::ETrackingUniverseOrigin origin = Cfg().seatedOrigin
            ? vr::TrackingUniverseSeated
            : vr::TrackingUniverseStanding;
        m_system->GetDeviceToAbsoluteTrackingPose(
            origin, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);
    } else {
        if (!m_compositor) return;

        // WaitGetPoses uses the compositor's tracking space, not the origin
        // passed to IVRSystem in the mono path. OpenVR defaults that compositor
        // state independently, so SeatedOrigin could silently become standing
        // space in stereo and add the headset's entire floor height (about 500
        // TR units in the reported session) above TR6's chase camera. Limit the
        // behaviour change to TR6; the established TR4/TR5 path is untouched.
        if (CurrentGame() == 2) {
            const vr::ETrackingUniverseOrigin origin = Cfg().seatedOrigin
                ? vr::TrackingUniverseSeated
                : vr::TrackingUniverseStanding;
            if (m_compositor->GetTrackingSpace() != origin) {
                m_compositor->SetTrackingSpace(origin);
            }
            if (!m_loggedTr6Origin) {
                m_loggedTr6Origin = true;
                LogF("tr6 camera: stereo tracking space set to %s as configured",
                     Cfg().seatedOrigin ? "seated" : "standing");
            }
        }
        m_compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    }

    const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    const bool wasValid = m_poseValid;
    m_poseValid = hmd.bPoseIsValid && hmd.bDeviceIsConnected;
    if (m_poseValid) {
        vr::HmdMatrix34_t pose = TransformHmd(m_trackingFromRuntime,hmd.mDeviceToAbsoluteTracking);
        m_rawHeadPose = pose;

        // The head's DISPLACEMENT, integrated in world space rather than carried
        // in the game camera's frame. This is what makes the stick behave like
        // PositionalTracking=0 while the headset behaves like 1.
        // FP owns a separate neck-corrected neutral. Do not accumulate its
        // room-scale movement against the chase camera in the background.
        if (!FirstPersonActive()) WorldLockOffset(pose);

        // mDeviceToAbsoluteTracking is head->tracking; we want tracking->head.
        m_headFromTracking = InvertRigid(FromHmd(pose));
        ApplyRuntimeRecenter();
        HeadYawRadians(); // Keep the last reliable heading even between FP draws.
        // Recenter must capture this pose's yaw, not the last valid frame's.
        if (FirstPersonActive() && (!wasValid || !m_firstPersonNeutralValid))
            FirstPersonRecenter();
    }

    const vr::ETrackedControllerRole roles[2] = {
        vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand
    };
    for (int hand = 0; hand < 2; ++hand) {
        const auto idx = m_system->GetTrackedDeviceIndexForControllerRole(roles[hand]);
        m_controllerPoseValid[hand] = idx != vr::k_unTrackedDeviceIndexInvalid &&
            idx < vr::k_unMaxTrackedDeviceCount && poses[idx].bPoseIsValid &&
            poses[idx].bDeviceIsConnected;
        if (m_controllerPoseValid[hand])
            m_rawControllerPose[hand] = TransformHmd(m_trackingFromRuntime,poses[idx].mDeviceToAbsoluteTracking);
    }
    if(m_viewRecenterPending && m_poseValid) RecenterView();

    if (wasValid != m_poseValid) {
        LogF("vr: head pose %s", m_poseValid ? "ACQUIRED" : "LOST");
    }

    // In mono bring-up, print the head position periodically. If these numbers
    // do not move when you move, the problem is tracking, not the injection.
    if (Cfg().monoTracking && m_poseValid) {
        if (++m_poseLogTick % 120 == 0) {
            const auto& t = hmd.mDeviceToAbsoluteTracking.m;
            LogF("vr: head @ x=%+.3f y=%+.3f z=%+.3f m  (fwd %+.2f %+.2f %+.2f)",
                 t[0][3], t[1][3], t[2][3], -t[0][2], -t[1][2], -t[2][2]);
        }
    }
}

float VRSystem::HeadYawRadians() const {
    if (!m_system) return 0.0f;
    if (!m_poseValid) return 0.0f;
    const float x=-m_rawHeadPose.m[0][2],z=m_rawHeadPose.m[2][2];
    const float horizontal=std::hypot(x,z);
    if(!std::isfinite(horizontal)) return m_headingYawValid ? m_headingYaw : 0;
    // Yaw from a nearly vertical forward vector is undefined/noisy. Freeze
    // the locomotion/body heading, not the full tracked camera orientation.
    // Hysteresis prevents repeated lock/unlock while nodding at the boundary.
    if(!m_headingYawValid) {
        m_headingYaw=horizontal>=.35f ? std::atan2(x,z)
            : std::atan2(m_rawHeadPose.m[2][0],m_rawHeadPose.m[0][0]);
        m_headingYawValid=true;
    }
    if(horizontal<.35f) m_headingYawVertical=true;
    else if(horizontal>.45f) m_headingYawVertical=false;
    if(!m_headingYawVertical) m_headingYaw=std::atan2(x,z);
    return m_headingYaw;
}

void VRSystem::HandleRuntimeEvent(const vr::VREvent_t& event) {
    if(event.eventType!=vr::VREvent_SeatedZeroPoseReset &&
       event.eventType!=vr::VREvent_StandingZeroPoseReset) return;
    if(!m_runtimeRecenterPending) {
        m_runtimeRecenterHadYaw=m_poseValid;
        m_runtimeRecenterYaw=HeadYawRadians();
    }
    m_runtimeRecenterPending=true;
    // The runtime has supplied a new origin. Do not apply the previous local
    // calibration to it; ApplyRuntimeRecenter reconciles the new pose next frame.
    m_trackingFromRuntime=Affine::Identity();
}

void VRSystem::ApplyRuntimeRecenter() {
    if(!m_runtimeRecenterPending || !m_poseValid) return;
    m_runtimeRecenterPending=false;
    m_headingYawValid=m_headingYawVertical=false;
    const float yaw=HeadYawRadians();
    WorldInventoryRecenter();
    if(FirstPersonActive()) {
        FirstPersonTrackingOriginChanged(m_runtimeRecenterHadYaw ? m_runtimeRecenterYaw-yaw : 0);
        Log("vr: runtime System recenter applied; first-person height recalibrated");
    } else {
        m_recentreRequested=true;
        if(CurrentGame()==0 || CurrentGame()==1) RecenterThirdPersonHead();
    }
    m_runtimeRecenterHadYaw=false;
}

float VRSystem::HeadPitchRadians() const {
    if (!m_system || !m_poseValid) return 0.0f;
    // phd_GetVectorAngles uses positive pitch for looking up (world Y-down).
    return -std::asin(std::clamp(m_headFromTracking.r[2][1], -1.0f, 1.0f));
}

void VRSystem::RecenterFirstPersonHead() {
    m_firstPersonNeutralValid = m_poseValid;
    if (!m_poseValid) return;
    for (int i = 0; i < 3; ++i)
        m_firstPersonNeutral[i] = m_rawHeadPose.m[i][3];
    const auto pivot = locomotion::NeckToHead(HeadYawRadians(),
        std::clamp(Cfg().firstPersonRoomscaleNeckMetres, 0.0f, 0.4f));
    m_firstPersonNeutralNeck[0] = pivot.x;
    m_firstPersonNeutralNeck[1] = pivot.z;
    m_firstPersonNeutralEyeNeckY=.08f*m_rawHeadPose.m[1][1]-.12f*m_rawHeadPose.m[1][2];
    LogF("firstperson: headset neutral=(%.3f,%.3f,%.3f)m",
         m_firstPersonNeutral[0], m_firstPersonNeutral[1], m_firstPersonNeutral[2]);
}

void VRSystem::RecenterThirdPersonHead() {
    m_thirdPersonNeutralValid = m_poseValid;
    m_thirdPersonRecenterPending = !m_poseValid;
    for (int i = 0; i < 3; ++i) {
        if (m_poseValid) m_thirdPersonNeutral[i] = m_rawHeadPose.m[i][3];
        m_offsetWorld[i] = 0;
        m_headFromTracking.r[i][3] = 0;
    }

    m_offsetValid = false;
    m_recentreRequested = false;
    Log("vr: third-person positional neutral captured; tracked rotation preserved");
}

bool VRSystem::ControllerPose(int hand, vr::HmdMatrix34_t& out) const {
    if (hand < 0 || hand > 1 || !m_controllerPoseValid[hand]) return false;
    out = m_rawControllerPose[hand];
    return true;
}

bool VRSystem::HeadPose(vr::HmdMatrix34_t& out) const {
    if (!m_poseValid) return false;
    out = m_rawHeadPose;
    return true;
}

bool VRSystem::LevelHeadPose(vr::HmdMatrix34_t& out) const {
    if(!m_poseValid) return false;
    const float yaw=HeadYawRadians(),c=std::cos(yaw),s=std::sin(yaw);
    out={{{c,0,-s,m_rawHeadPose.m[0][3]},
          {0,1,0,m_rawHeadPose.m[1][3]},
          {s,0,c,m_rawHeadPose.m[2][3]}}};
    return true;
}

void VRSystem::RecenterView() {
    m_viewRecenterPending=!m_poseValid;
    const float oldYaw=HeadYawRadians();
    if(m_poseValid) {
        vr::HmdMatrix34_t level{};
        LevelHeadPose(level);
        // Rotate the entire tracking frame, including controller positions.
        // Position neutrals below remove the physical displacement. Keeping
        // this yaw-only preserves gravity and real headset pitch/roll.
        for(int i=0;i<3;++i) level.m[i][3]=0;
        const auto correction=InvertRigid(FromHmd(level));
        m_trackingFromRuntime=Mul(correction,m_trackingFromRuntime);
        m_rawHeadPose=TransformHmd(correction,m_rawHeadPose);
        for(int hand=0;hand<2;++hand) if(m_controllerPoseValid[hand])
            m_rawControllerPose[hand]=TransformHmd(correction,m_rawControllerPose[hand]);
        m_headFromTracking=InvertRigid(FromHmd(m_rawHeadPose));
        m_headingYaw=0;
        m_headingYawValid=true;
    }
    // Tracking may return after the new first-person camera is already live.
    // Rebase that camera too, so the deferred reset cannot turn Lara's facing.
    if(FirstPersonActive() && m_poseValid) FirstPersonTrackingOriginChanged(oldYaw);
    else FirstPersonRecenter();
    RecenterThirdPersonHead();
    WorldInventoryRecenter();
    Log("vr: view toggle recalibrated shared head/controller heading and position");
}

bool VRSystem::FirstPersonControllerOffset(int hand, float& right, float& down,
                                           float& forward) const {
    if (hand < 0 || hand > 1 || !m_poseValid || !m_firstPersonNeutralValid ||
        !m_controllerPoseValid[hand]) return false;
    FirstPersonViewOffset(right, forward);
    const auto& controller = m_rawControllerPose[hand];
    right += controller.m[0][3] - m_rawHeadPose.m[0][3];
    forward -= controller.m[2][3] - m_rawHeadPose.m[2][3];
    down = m_firstPersonNeutral[1] - controller.m[1][3];
    return std::isfinite(right) && std::isfinite(down) && std::isfinite(forward);
}

void VRSystem::HeadFloorOffset(float& right, float& forward) const {
    right = forward = 0;
    if (!m_poseValid || !m_firstPersonNeutralValid) return;
    const auto pivot = locomotion::NeckToHead(HeadYawRadians(),
        std::clamp(Cfg().firstPersonRoomscaleNeckMetres, 0.0f, 0.4f));
    right = m_rawHeadPose.m[0][3] - m_firstPersonNeutral[0]
        - (pivot.x - m_firstPersonNeutralNeck[0]);
    forward = -(m_rawHeadPose.m[2][3] - m_firstPersonNeutral[2])
        - (pivot.z - m_firstPersonNeutralNeck[1]);
}

void VRSystem::PivotHeadFloorOffset(float yawDelta) {
    if (!m_poseValid || !m_firstPersonNeutralValid) return;
    const locomotion::Vec before{m_rawHeadPose.m[0][3] - m_firstPersonNeutral[0],
                                -(m_rawHeadPose.m[2][3] - m_firstPersonNeutral[2])};
    const auto pivot = locomotion::NeckToHead(HeadYawRadians(),
        std::clamp(Cfg().firstPersonRoomscaleNeckMetres, 0.0f, 0.4f));
    const auto neckArc = pivot - locomotion::Vec{m_firstPersonNeutralNeck[0],
                                               m_firstPersonNeutralNeck[1]};
    const auto after = Cfg().firstPersonMovementStabilization
        ? locomotion::Rotate(before,-yawDelta)
        : locomotion::PivotFloorOffset(before, neckArc, yawDelta);
    ConsumeHeadFloorOffset(before.x - after.x, before.z - after.z);
}

void VRSystem::ConsumeHeadFloorOffset(float right, float forward) {
    if (!m_poseValid || !m_firstPersonNeutralValid) return;
    m_firstPersonNeutral[0] += right;
    m_firstPersonNeutral[2] -= forward;
    // TrackedHeadView derives translation on demand: culling and both eyes
    // see this change in the same frame, without a stale cached eye origin.
}

void VRSystem::FirstPersonViewOffset(float& right,float& forward) const {
    if (!Cfg().firstPersonMovementStabilization) {
        HeadFloorOffset(right,forward); return;
    }
    right=forward=0;
    if (!m_poseValid || !m_firstPersonNeutralValid) return;
    // Stabilized camera no longer follows the animated body's physical-yaw
    // orbit. Render actual HMD translation, not an invented neck arc. The
    // compensated neck position still drives room-scale BODY movement only.
    right=m_rawHeadPose.m[0][3]-m_firstPersonNeutral[0];
    forward=-(m_rawHeadPose.m[2][3]-m_firstPersonNeutral[2]);
}

float VRSystem::FirstPersonVerticalOffset() const {
    if (!m_poseValid || !m_firstPersonNeutralValid) return 0;
    return (m_rawHeadPose.m[1][3]-m_firstPersonNeutral[1])*(Cfg().flipViewY ? -1.0f : 1.0f);
}

float VRSystem::FirstPersonHeightDrop() const {
    return m_poseValid && m_firstPersonNeutralValid ? m_firstPersonNeutral[1]-m_rawHeadPose.m[1][3] : 0;
}
float VRSystem::FirstPersonPostureDrop() const {
    if(!m_poseValid || !m_firstPersonNeutralValid) return 0;
    // Looking down moves the HMD around the neck even when standing upright.
    // Remove only that estimated downward arc from the standing-leg gate;
    // rendering continues to use the unmodified, fully tracked eye position.
    const float eyeNeckY=.08f*m_rawHeadPose.m[1][1]-.12f*m_rawHeadPose.m[1][2];
    return FirstPersonHeightDrop()-std::max(0.f,m_firstPersonNeutralEyeNeckY-eyeNeckY);
}

void VRSystem::RecentreOffset() {
    WorldInventoryRecenter();
    m_recentreRequested = true;
    if (FirstPersonActive()) FirstPersonRecenter();
    else if (CurrentGame()==0 || CurrentGame()==1) RecenterThirdPersonHead();
}

Affine VRSystem::TrackedHeadView() const {
    if (!FirstPersonActive()) return m_headFromTracking;
    // The scene anchor supplies eye height. Only physical displacement since
    // entering first person belongs on top of it; never the standing origin's
    // full floor-to-head height or the third-person world-offset accumulator.
    auto pose = m_rawHeadPose;
    for (int i = 0; i < 3; ++i)
        pose.m[i][3] = m_firstPersonNeutralValid
            ? pose.m[i][3] - m_firstPersonNeutral[i] : 0.0f;
    // Stabilization uses raw tracking; the opt-out retains legacy neck-arc
    // compensation. Vertical displacement is unmodified in either mode.
    float right, forward;
    FirstPersonViewOffset(right, forward);
    pose.m[0][3] = right;
    pose.m[2][3] = -forward;
    return InvertRigid(FromHmd(pose));
}

Affine VRSystem::HeadView() const {
    const auto& c = Cfg();

    // Head pose, with positional dropped when only rotation is wanted. This is
    // honoured in stereo as well as mono -- it used to apply only to mono, which
    // made PositionalTracking=0 silently do nothing once stereo was on.
    //
    // m_headAtCamera drops it for the duration of an optic, for the laser
    // sight's sake. See SetHeadAtCamera. Culling follows it on purpose: with the
    // head back at the camera the engine's own visible set is the right one
    // again, so the head frustum simply stops adding rooms.
    Affine head = TrackedHeadView();
    bool dropTranslation = !c.positionalTracking || m_headAtCamera;
    if (FirstPersonActive() && !c.firstPersonHeadTranslation)
        dropTranslation = true;
    if (dropTranslation) {
        head.r[0][3] = head.r[1][3] = head.r[2][3] = 0.0f;
    }
    return ToEngineSpace(head, LiveWorldUnitsPerMetre(), c.flipViewY);
}

bool VRSystem::CullTangents(float& tanX, float& tanY) const {
    float x = 0.0f, y = 0.0f;
    for (int e = 0; e < 2; ++e) {
        const float l = std::fabs(m_rawProj[e][0]);
        const float r = std::fabs(m_rawProj[e][1]);
        const float t = std::fabs(m_rawProj[e][2]);
        const float b = std::fabs(m_rawProj[e][3]);
        if (l > x) x = l;
        if (r > x) x = r;
        if (t > y) y = t;
        if (b > y) y = b;
    }
    // Zero means GetProjectionRaw has not run (no runtime, or Init failed part
    // way). Report that rather than hand back a degenerate frustum that would
    // cull everything.
    if (!(x > 0.0f) || !(y > 0.0f)) return false;
    tanX = x;
    tanY = y;
    return true;
}

Affine VRSystem::EyeView(Eye eye) const {
    const auto& c = Cfg();

    const float scale = LiveWorldUnitsPerMetre();

    // Mono: the centred head view, with no per-eye offset. There is only one
    // image, so applying half an IPD to it would just shift the whole picture.
    if (c.monoTracking) {
        return HeadView();
    }

    // The head CENTRE only. m_eyeFromHead below is deliberately left alone --
    // that is what keeps both eyes, and so the world's depth, while the 6DOF
    // displacement goes away. See SetHeadAtCamera.
    Affine head = TrackedHeadView();
    bool dropTranslation = !c.positionalTracking || m_headAtCamera;
    if (FirstPersonActive() && !c.firstPersonHeadTranslation)
        dropTranslation = true;
    if (dropTranslation) {
        head.r[0][3] = head.r[1][3] = head.r[2][3] = 0.0f;
    }

    int idx = static_cast<int>(eye);
    if (c.swapEyes) idx = 1 - idx;

    // IpdScale stretches the eye offset only, leaving world size alone. Applied
    // here rather than folded into `scale` so that world size and stereo
    // strength stay independently adjustable.
    Affine eyeFromHead = m_eyeFromHead[idx];
    const float ipd = LiveIpdScale();
    eyeFromHead.r[0][3] *= ipd;
    eyeFromHead.r[1][3] *= ipd;
    eyeFromHead.r[2][3] *= ipd;

    // In OpenVR's own convention first: eye <- head <- tracking origin.
    const Affine ovr = Mul(eyeFromHead, head);

    // Then into the engine's Y-down, TR-unit view space.
    return ToEngineSpace(ovr, scale, c.flipViewY);
}

Affine VRSystem::AnchoredEyeView(Eye eye,const vr::HmdMatrix34_t& anchor) const {
    Affine relative=Mul(InvertRigid(FromHmd(m_rawHeadPose)),FromHmd(anchor));
    if(!Cfg().monoTracking) {
        int idx=static_cast<int>(eye);
        if(Cfg().swapEyes) idx=1-idx;
        Affine offset=m_eyeFromHead[idx];
        for(int i=0;i<3;++i) offset.r[i][3]*=LiveIpdScale();
        relative=Mul(offset,relative);
    }
    return ToEngineSpace(relative,LiveWorldUnitsPerMetre(),Cfg().flipViewY);
}

void VRSystem::EyeProjection(Eye eye, float zNear, float zFar, mat4& out) const {
    const auto& c = Cfg();
    int idx = static_cast<int>(eye);
    if (c.swapEyes) idx = 1 - idx;

    float l = m_rawProj[idx][0], r = m_rawProj[idx][1];
    float t = m_rawProj[idx][2], b = m_rawProj[idx][3];

    // Mode 2: keep the total field of view per axis, drop the off-centring.
    // Averaging the magnitudes preserves (r-l) and (b-t) exactly, so scale and
    // aspect are untouched and the only thing that changes is the shear.
    if (c.perEyeProjection == 2) {
        const float hw = (std::fabs(l) + std::fabs(r)) * 0.5f;
        const float hh = (std::fabs(t) + std::fabs(b)) * 0.5f;
        l = -hw; r = hw;
        t = -hh; b = hh;
    }

    BuildEyeProjection(out, l, r, t, b, zNear, zFar, c.flipProjectionY);
}

float VRSystem::HudNdcShiftX(Eye eye, float depthMetres) const {
    if (depthMetres <= 0.0f) return 0.0f;

    const auto& c = Cfg();
    int idx = static_cast<int>(eye);
    if (c.swapEyes) idx = 1 - idx;

    const float l = m_rawProj[idx][0];
    const float r = m_rawProj[idx][1];
    const float rl = r - l;
    if (std::fabs(rl) < 1e-9f) return 0.0f;

    const float p00 = 2.0f / rl;
    const float p02 = (r + l) / rl;

    // Half the physical eye separation, in world units, honouring live tuning.
    const float scale = LiveWorldUnitsPerMetre();
    const float halfSepM =
        0.5f * std::fabs(m_eyeFromHead[0].r[0][3] - m_eyeFromHead[1].r[0][3]);
    const float halfSep = halfSepM * scale * LiveIpdScale();
    const float depthW  = depthMetres * scale;
    if (depthW <= 0.0f) return -p02;

    const float s = (idx == 0) ? 1.0f : -1.0f;
    return -p02 + s * p00 * halfSep / depthW;
}

void VRSystem::GunShotHaptic(int hand) {
    if (m_system) m_gunHaptics.Shot(hand,GetTickCount64());
}

void VRSystem::HolsterHoverHaptic(int hand,bool active) {
    m_gunHaptics.Hover(hand,active && m_system,GetTickCount64());
}

void VRSystem::LedgeGrabHaptic() {
    if (!m_system) return;
    const auto now=GetTickCount64();
    // Share the pulse scheduler so overlapping effects retain its 5 ms limit.
    m_gunHaptics.Shot(0,now);
    m_gunHaptics.Shot(1,now);
}

void VRSystem::ReadControllers(HandState out[2]) const {
    out[0] = HandState();
    out[1] = HandState();
    if (!m_system) return;

    const vr::ETrackedControllerRole roles[2] = {
        vr::TrackedControllerRole_LeftHand,
        vr::TrackedControllerRole_RightHand
    };

    for (int h = 0; h < 2; ++h) {
        const vr::TrackedDeviceIndex_t idx =
            m_system->GetTrackedDeviceIndexForControllerRole(roles[h]);
        if (idx == vr::k_unTrackedDeviceIndexInvalid) continue;

        vr::VRControllerState_t st{};
        if (!m_system->GetControllerState(idx, &st, sizeof(st))) continue;

        HandState& o = out[h];
        o.valid      = true;
        o.rawPressed = st.ulButtonPressed;

        // Legacy axis layout for Touch-style controllers.
        o.stickX  = st.rAxis[0].x;
        o.stickY  = st.rAxis[0].y;
        o.trigger = st.rAxis[1].x;
        o.grip    = st.rAxis[2].x;

        const uint64_t P = st.ulButtonPressed;
        o.btnLower   = (P & vr::ButtonMaskFromId(vr::k_EButton_A)) != 0;
        o.btnUpper   = (P & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0;
        o.stickClick = (P & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;

        // Some runtimes report the trigger and grip only as buttons.
        if ((P & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) && o.trigger < 0.5f) {
            o.trigger = 1.0f;
        }
        if ((P & vr::ButtonMaskFromId(vr::k_EButton_Grip)) && o.grip < 0.5f) {
            o.grip = 1.0f;
        }
    }
}

void VRSystem::Submit(GLuint tex, uint32_t, uint32_t) {
    if (!m_compositor || !tex) return;

    vr::Texture_t t{};
    t.handle      = reinterpret_cast<void*>(static_cast<uintptr_t>(tex));
    t.eType       = vr::TextureType_OpenGL;
    t.eColorSpace = vr::ColorSpace_Gamma;

    // One double-wide texture, split by UV bounds -- avoids a second FBO and a
    // second attachment switch per frame.
    //
    // V runs 0..1 by default, i.e. ordinary GL orientation. This used to be
    // tied to FlipProjectionY, which was wrong and made the headset image
    // upside down: the engine's negative e11 is there to cancel TR's Y-down
    // world, so what lands in the eye texture is already a correctly oriented
    // GL render with its origin at lower-left. Inverting V as well flipped a
    // correct image. FlipSubmitV stays available as an escape hatch.
    const bool flip = Cfg().flipSubmitV;
    const float v0 = flip ? 1.0f : 0.0f;
    const float v1 = flip ? 0.0f : 1.0f;

    vr::VRTextureBounds_t left { 0.0f, v0, 0.5f, v1 };
    vr::VRTextureBounds_t right{ 0.5f, v0, 1.0f, v1 };

    m_compositor->Submit(vr::Eye_Left,  &t, &left);
    m_compositor->Submit(vr::Eye_Right, &t, &right);
    // Read compositor history at a low cadence; never wait for the GPU or
    // modify timing/reprojection settings. Compare with arm-glance timestamps.
    if(Cfg().firstPersonDriftLog && FirstPersonActive()) {
        static uint64_t nextTrace=0;
        const auto now=GetTickCount64();
        if(now>=nextTrace) {
            nextTrace=now+500;
            vr::Compositor_FrameTiming timings[32]{};
            timings[0].m_nSize=sizeof(timings[0]);
            const auto count=std::min(m_compositor->GetFrameTimings(timings,32),uint32_t(32));
            unsigned dropped=0,misPresented=0,motion=0;uint32_t flags=0;
            float gpu=0,interval=0,submit=0,render=0;
            for(uint32_t i=0;i<count;++i) {
                const auto& f=timings[i];dropped+=f.m_nNumDroppedFrames;misPresented+=f.m_nNumMisPresented;
                motion+=(f.m_nReprojectionFlags&vr::VRCompositor_ReprojectionMotion)!=0;flags|=f.m_nReprojectionFlags;
                gpu=std::max(gpu,f.m_flPreSubmitGpuMs);interval=std::max(interval,f.m_flClientFrameIntervalMs);
                submit=std::max(submit,f.m_flSubmitFrameMs);
                render=std::max(render,f.m_flNewFrameReadyMs-f.m_flNewPosesReadyMs);
            }
            if(count) LogF("vr-glance: t=%llu pose=%llu history=%u frames=%u..%u max-ms gpu=%.2f app=%.2f interval=%.2f submit=%.2f dropped=%u mispresented=%u motion-smoothed=%u flags=%x",
                now,m_poseSerial,count,timings[0].m_nFrameIndex,timings[count-1].m_nFrameIndex,
                gpu,render,interval,submit,dropped,misPresented,motion,flags);
        }
    }
}

} // namespace tr
