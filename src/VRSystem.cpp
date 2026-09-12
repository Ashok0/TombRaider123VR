#include "VRSystem.h"
#include "Config.h"
#include "Log.h"
#include "GL.h"
#include "GameDll.h"

#include <cstdio>
#include <cmath>

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
    if (m_system && g_shutdownInternal) g_shutdownInternal();
    m_system     = nullptr;
    m_compositor = nullptr;
    if (m_dll) { FreeLibrary(m_dll); m_dll = nullptr; }
}

void VRSystem::GetEyeSize(uint32_t& w, uint32_t& h) const {
    w = m_eyeW;
    h = m_eyeH;
}

void VRSystem::BeginFrame() {
    if (!m_system) return;

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
        m_compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    }

    const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    const bool wasValid = m_poseValid;
    m_poseValid = hmd.bPoseIsValid && hmd.bDeviceIsConnected;
    if (m_poseValid) {
        vr::HmdMatrix34_t pose = hmd.mDeviceToAbsoluteTracking;

        // Ceiling clearance. Cap the head's HEIGHT before the pose is inverted,
        // which is the only place it is still a plain position -- afterwards it
        // is a view transform whose translation column is -R^T*p, and clamping
        // that would move the eye sideways as well as down.
        //
        // Seated origin means pose Y is height above the seated zero, and the
        // eye sits at the game camera when that is 0, so the camera rises by
        // exactly poseY * unitsPerMetre. Cap that at the room's headroom less a
        // margin. Nothing else is touched: ducking, leaning and every rotation
        // pass through untouched, and below the cap this is a no-op.
        float headroom = 0.0f;
        if (Cfg().ceilingClearance && CameraHeadroom(headroom)) {
            const float scale = LiveWorldUnitsPerMetre();
            if (scale > 0.0f) {
                float maxRise = (headroom - Cfg().ceilingMarginUnits) / scale;
                if (maxRise < 0.0f) maxRise = 0.0f;   // already at the ceiling
                if (pose.m[1][3] > maxRise) {
                    pose.m[1][3] = maxRise;
                    if (!m_loggedClamp) {
                        m_loggedClamp = true;
                        LogF("vr: ceiling clamp active -- headroom %.0f units, "
                             "head capped at %.2f m above the seated zero",
                             headroom, maxRise);
                    }
                }
            }
        }

        // mDeviceToAbsoluteTracking is head->tracking; we want tracking->head.
        m_headFromTracking = InvertRigid(FromHmd(pose));
    }

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

Affine VRSystem::HeadView() const {
    const auto& c = Cfg();

    // Head pose, with positional dropped when only rotation is wanted. This is
    // honoured in stereo as well as mono -- it used to apply only to mono, which
    // made PositionalTracking=0 silently do nothing once stereo was on.
    Affine head = m_headFromTracking;
    if (!c.positionalTracking) {
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

    Affine head = m_headFromTracking;
    if (!c.positionalTracking) {
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
}

} // namespace tr
