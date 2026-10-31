#include "VRSystem.h"
#include "HeadHeightClamp.h"
#include "Config.h"
#include "Log.h"
#include "GL.h"
#include "GameDll.h"
#include "FirstPerson.h"
#include "LocomotionMath.h"
#include "Engine.h"
#include "Gamepad.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <string>

namespace tr {
namespace {

vr::HmdMatrix34_t TransformPose(const Affine& transform,const vr::HmdMatrix34_t& pose) {
    Affine input{};for(int i=0;i<3;++i) for(int j=0;j<4;++j) input.r[i][j]=pose.m[i][j];
    const auto result=Mul(transform,input);vr::HmdMatrix34_t out{};
    for(int i=0;i<3;++i) for(int j=0;j<4;++j) out.m[i][j]=result.r[i][j];
    return out;
}

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

std::string IndexManifestPath() {
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&g_vr),&module)) return {};
    wchar_t path[MAX_PATH]{};
    const DWORD count=GetModuleFileNameW(module,path,MAX_PATH);
    if(!count || count>=MAX_PATH) return {};
    std::wstring full(path);
    const auto slash=full.find_last_of(L"\\/");
    if(slash==std::wstring::npos) return {};
    full.resize(slash+1);full+=L"TombRaiderVRBindings\\actions.json";
    char utf8[MAX_PATH*4]{};
    if(!WideCharToMultiByte(CP_UTF8,0,full.c_str(),-1,utf8,sizeof(utf8),nullptr,nullptr)) return {};
    return utf8;
}

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

    // Register before the first PollNextEvent/UpdateActionState, as OpenVR requires.
    if(Cfg().valveIndexControllers) {
        err=vr::VRInitError_None;
        m_input=static_cast<vr::IVRInput*>(g_getInterface(vr::IVRInput_Version,&err));
        const auto path=IndexManifestPath();
        const auto result=m_input && err==vr::VRInitError_None && !path.empty()
            ? m_indexInput.Init(*m_input,path.c_str()) : vr::VRInputError_InvalidParam;
        if(result==vr::VRInputError_None)
            LogF("vr: Index actions ready; grip press=%.2f release=%.2f; manifest=%s",
                Cfg().valveIndexGripPress,Cfg().valveIndexGripRelease,path.c_str());
        else {
            m_input=nullptr;
            LogF("vr: Index actions unavailable (error %d). Install TombRaiderVRBindings beside TombRaiderVR.dll, or set ValveIndexControllers=0 and restart. Legacy fallback disabled to avoid conflicting controls.",int(result));
        }
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
    // Some SteamVR runtime/driver combinations report zero here while the
    // compositor is still starting. Propagating 0x0 makes StereoRenderer
    // refuse its FBO and leaves the headset on the SteamVR grid with no frame
    // to submit. Use a standard per-eye fallback; explicit INI dimensions still
    // take precedence above.
    if (m_eyeW == 0 || m_eyeH == 0) {
        LogF("vr: runtime returned %ux%u render target; using 1512x1680 fallback",
             m_eyeW, m_eyeH);
        if (m_eyeW == 0) m_eyeW = 1512;
        if (m_eyeH == 0) m_eyeH = 1680;
    }
    m_eyeW = static_cast<uint32_t>(m_eyeW * c.superSample);
    m_eyeH = static_cast<uint32_t>(m_eyeH * c.superSample);

    LogF("vr: ready, per-eye target %ux%u", m_eyeW, m_eyeH);
    return true;
}

void VRSystem::Shutdown() {
    m_gunHaptics.Reset();
    m_input=nullptr;m_indexInput.Reset();m_indexReadError=vr::VRInputError_None;
    if (m_system && g_shutdownInternal) g_shutdownInternal();
    m_system     = nullptr;
    m_compositor = nullptr;
    m_poseValid = false;
    m_headingYawValid=m_headingYawVertical=false;
    m_runtimeRecenterPending=m_runtimeRecenterHadYaw=false;
    m_controllerPoseValid[0] = m_controllerPoseValid[1] = false;
    m_haveNeutral = false;
    m_trackingFromRuntime=Affine::Identity();
    m_viewRecenterPending=false;
    m_levelNeutralValid=false;
    m_levelRooms=nullptr;m_levelGame=m_levelNumber=-1;
    m_poseSerial=0;
    if (m_dll) { FreeLibrary(m_dll); m_dll = nullptr; }
}

void VRSystem::GetEyeSize(uint32_t& w, uint32_t& h) const {
    w = m_eyeW;
    h = m_eyeH;
}

void VRSystem::BeginFrame() {
    m_controllerPoseValid[0]=m_controllerPoseValid[1]=false;
    if (!m_system) return;
    vr::VREvent_t event{};
    while(m_system->PollNextEvent(&event,sizeof(event))) HandleRuntimeEvent(event);

    m_gunHaptics.Update(GetTickCount64(),[&](int hand,unsigned short duration) {
        if(Cfg().valveIndexControllers) {
            if(m_input) m_indexInput.Haptic(*m_input,hand,duration);
            return;
        }
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
        m_compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    }

    ++m_poseSerial;
    const auto& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    const bool wasValid = m_poseValid;
    m_poseValid = hmd.bPoseIsValid && hmd.bDeviceIsConnected;
    if (m_poseValid) {
        m_rawHeadPose=TransformPose(m_trackingFromRuntime,hmd.mDeviceToAbsoluteTracking);
        vr::HmdMatrix34_t pose = m_rawHeadPose;
        const auto* dll=GameDllBound();const auto base=GameDllBase();
        const int game=CurrentGame(),level=AppFlag(drva::app_off::level);
        const void* rooms=dll && base ? *reinterpret_cast<void**>(base+dll->room) : nullptr;
        const bool loaded=rooms && *reinterpret_cast<int16_t*>(base+dll->numberRooms)>0 &&
            *reinterpret_cast<void**>(base+dll->laraItem) && !InTitle();
        if(!FirstPersonActive()) {
            if(!loaded || rooms!=m_levelRooms || game!=m_levelGame || level!=m_levelNumber)
                m_levelNeutralValid=false;
            if(loaded && !InInventory() && !InCutscene() && !m_levelNeutralValid) {
                m_haveNeutral=false;m_levelNeutralValid=true;
                m_levelRooms=rooms;m_levelGame=game;m_levelNumber=level;
            }
        }

        // Ceiling clearance. Cap the head's HEIGHT before the pose is inverted,
        // which is the only place it is still a plain position -- afterwards it
        // is a view transform whose translation column is -R^T*p, and clamping
        // that would move the eye sideways as well as down.
        //
        // The eye sits at the game camera at the recentered headset height,
        // regardless of the runtime's seated or standing origin. The cap must
        // therefore be relative to that neutral height. Applying it to raw Y
        // made a grounded frame jump by the entire headset-origin height when
        // the clamp resumed after landing or Lara mounted a crate.
        if (!m_haveNeutral || m_runtimeRecenterPending) {
            m_headNeutral[0] = pose.m[0][3];
            m_headNeutral[1] = pose.m[1][3];
            m_headNeutral[2] = pose.m[2][3];
            m_neutralEyeNeckY=.08f*m_rawHeadPose.m[1][1]-.12f*m_rawHeadPose.m[1][2];
            m_haveNeutral = true;
            LogF("vr: head neutral set at (%+.2f, %+.2f, %+.2f) m -- tracked "
                 "translation is measured from here", m_headNeutral[0],
                 m_headNeutral[1], m_headNeutral[2]);
        }
        float headroom = 0.0f;
        if (Cfg().ceilingClearance && CameraHeadroom(headroom)) {
            const float scale = LiveWorldUnitsPerMetre();
            if (scale > 0.0f) {
                const float capped = headheight::Clamp(pose.m[1][3],
                    m_headNeutral[1], headroom, Cfg().ceilingMarginUnits, scale);
                if (capped < pose.m[1][3]) {
                    pose.m[1][3] = capped;
                    if (!m_loggedClamp) {
                        m_loggedClamp = true;
                        LogF("vr: ceiling clamp active -- headroom %.0f units, "
                             "head capped at %.2f m above neutral",
                             headroom, capped - m_headNeutral[1]);
                    }
                }
            }
        }

        // WHERE "STILL" IS.
        //
        // A head position only means anything as a displacement from somewhere,
        // and the runtime's own origin is not that somewhere: in a standing
        // universe it sits on the floor, so the head arrives about 1.7 m up and,
        // scaled into world units, lifts the camera several hundred units above
        // the game's. That is what made positional tracking look broken in first
        // person -- the player floating over Lara's head -- and why it had been
        // switched off there.
        //
        // So a neutral is captured and everything is measured from it: leaning
        // and ducking move the camera, while merely having a head does not.
        // FirstPerson takes it when first person engages, rather than at the
        // first pose the runtime produces, which is typically while the headset
        // is still sitting on a desk.
        m_headPosRaw[0] = pose.m[0][3];
        m_headPosRaw[1] = pose.m[1][3];
        m_headPosRaw[2] = pose.m[2][3];

        pose.m[0][3] -= m_headNeutral[0];
        pose.m[1][3] -= m_headNeutral[1];
        pose.m[2][3] -= m_headNeutral[2];

        // mDeviceToAbsoluteTracking is head->tracking; we want tracking->head.
        m_headFromTracking = InvertRigid(FromHmd(pose));
        ApplyRuntimeRecenter();
        HeadYawRadians();
        if (!wasValid) RecenterHead();
        else RefreshHeadTranslation();
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
            m_controllerPose[hand] = TransformPose(m_trackingFromRuntime,poses[idx].mDeviceToAbsoluteTracking);
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
    if(m_viewRecenterPending && m_poseValid) RecenterView();
    if(!FirstPersonActive() && !m_levelNeutralValid)
        m_headFromTracking.r[0][3]=m_headFromTracking.r[1][3]=m_headFromTracking.r[2][3]=0;

}

bool VRSystem::ControllerPose(int hand, vr::HmdMatrix34_t& out) const {
    if (hand < 0 || hand > 1 || !m_controllerPoseValid[hand]) return false;
    out = m_controllerPose[hand];
    return true;
}

bool VRSystem::ControllerHeightBelowHead(int hand, float& down) const {
    if (hand<0 || hand>1 || !m_poseValid || !m_controllerPoseValid[hand]) return false;
    down=m_headPosRaw[1]-m_controllerPose[hand].m[1][3];
    return std::isfinite(down);
}

bool VRSystem::FirstPersonControllerOffset(int hand, float& right, float& down,
                                           float& forward) const {
    if (hand < 0 || hand > 1 || !m_poseValid || !m_haveNeutral ||
        !m_controllerPoseValid[hand]) return false;
    FirstPersonViewOffset(right, forward);
    right += m_controllerPose[hand].m[0][3] - m_headPosRaw[0];
    forward -= m_controllerPose[hand].m[2][3] - m_headPosRaw[2];
    down = m_headNeutral[1] - m_controllerPose[hand].m[1][3];
    return std::isfinite(right) && std::isfinite(down) && std::isfinite(forward);
}

void VRSystem::HeadFloorOffset(float& right, float& forward) const {
    right = forward = 0;
    if (!m_poseValid || !m_haveNeutral) return;
    const locomotion::Vec raw{m_headPosRaw[0] - m_headNeutral[0],
                             -(m_headPosRaw[2] - m_headNeutral[2])};
    const auto pivot = locomotion::NeckToHead(HeadYawRadians(),
        std::clamp(Cfg().firstPersonRoomscaleNeckMetres, 0.0f, 0.4f));
    const auto body=locomotion::ViewFloorOffset(raw,
        pivot-locomotion::Vec{m_neutralNeckToHead[0],m_neutralNeckToHead[1]},false);
    right=body.x;
    forward=body.z;
}

void VRSystem::FirstPersonViewOffset(float& right, float& forward) const {
    right=forward=0;
    if (!m_poseValid || !m_haveNeutral) return;
    // A stable grounded eye anchor already excludes Lara's animated head arc.
    // The eye and tracked hands must receive physical HMD movement alone;
    // neck compensation is reserved for moving Lara's collision body.
    const locomotion::Vec raw{m_headPosRaw[0]-m_headNeutral[0],
                             -(m_headPosRaw[2]-m_headNeutral[2])};
    const auto pivot=locomotion::NeckToHead(HeadYawRadians(),
        std::clamp(Cfg().firstPersonRoomscaleNeckMetres,0.0f,0.4f));
    const auto view=locomotion::ViewFloorOffset(raw,
        pivot-locomotion::Vec{m_neutralNeckToHead[0],m_neutralNeckToHead[1]},
        Cfg().firstPersonMovementStabilization);
    right=view.x;
    forward=view.z;
}

void VRSystem::RefreshHeadTranslation() {
    float displacement[3] = {
        m_headPosRaw[0] - m_headNeutral[0],
        m_headPosRaw[1] - m_headNeutral[1],
        m_headPosRaw[2] - m_headNeutral[2]
    };
    if (FirstPersonActive()) {
        // The grounded scene anchor is stable. Use real HMD displacement for
        // the eye; the estimated neck arc only belongs to body room-scale drag.
        float right, forward;
        FirstPersonViewOffset(right, forward);
        displacement[0] = right;
        displacement[2] = -forward;
    }
    for (int i = 0; i < 3; ++i) {
        m_headFromTracking.r[i][3] = 0;
        for (int j = 0; j < 3; ++j)
            m_headFromTracking.r[i][3] -= m_headFromTracking.r[i][j]
                * displacement[j];
    }
}

void VRSystem::ConsumeHeadFloorOffset(float right, float forward) {
    if (!m_poseValid || !m_haveNeutral) return;
    m_headNeutral[0] += right;
    m_headNeutral[2] -= forward;
    // Update THIS frame too, so culling and both eyes use the same origin.
    RefreshHeadTranslation();
}

void VRSystem::PivotHeadFloorOffset(float yawDelta) {
    if (!m_poseValid || !m_haveNeutral) return;
    const locomotion::Vec before{m_headPosRaw[0] - m_headNeutral[0],
                                -(m_headPosRaw[2] - m_headNeutral[2])};
    const auto pivot = locomotion::NeckToHead(HeadYawRadians(),
        std::clamp(Cfg().firstPersonRoomscaleNeckMetres, 0.0f, 0.4f));
    const locomotion::Vec neckArc =
        pivot - locomotion::Vec{m_neutralNeckToHead[0], m_neutralNeckToHead[1]};
    // Keep actual neck translation fixed in world while the artificial yaw
    // changes. The neck-to-eye arc stays in tracking space so it follows the
    // newly rotated Lara instead of orbiting her body until 360 degrees.
    const auto after = Cfg().firstPersonMovementStabilization
        ? locomotion::Rotate(before,-yawDelta)
        : locomotion::PivotFloorOffset(before, neckArc, yawDelta);
    ConsumeHeadFloorOffset(before.x - after.x, before.z - after.z);
}

bool VRSystem::LevelHeadPose(vr::HmdMatrix34_t& out) const {
    if(!m_poseValid) return false;
    const float yaw=HeadYawRadians(),c=std::cos(yaw),s=std::sin(yaw);
    out={{{c,0,-s,m_rawHeadPose.m[0][3]},
          {0,1,0,m_rawHeadPose.m[1][3]},
          {s,0,c,m_rawHeadPose.m[2][3]}}};
    return true;
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

void VRSystem::RecenterView() {
    m_viewRecenterPending=!m_poseValid;
    if(!m_poseValid) return;
    const float oldYaw=HeadYawRadians();
    vr::HmdMatrix34_t level{};LevelHeadPose(level);
    for(int i=0;i<3;++i) level.m[i][3]=0;
    const auto correction=InvertRigid(FromHmd(level));
    m_trackingFromRuntime=Mul(correction,m_trackingFromRuntime);
    m_rawHeadPose=TransformPose(correction,m_rawHeadPose);
    for(int hand=0;hand<2;++hand) if(m_controllerPoseValid[hand])
        m_controllerPose[hand]=TransformPose(correction,m_controllerPose[hand]);
    for(int i=0;i<3;++i) m_headPosRaw[i]=m_rawHeadPose.m[i][3];
    m_headFromTracking=InvertRigid(FromHmd(m_rawHeadPose));
    m_headingYaw=0;m_headingYawValid=true;m_headingYawVertical=false;
    if(FirstPersonActive()) FirstPersonTrackingOriginChanged(oldYaw);
    else RecenterHead();
    WorldInventoryRecenter();
}

void VRSystem::RecenterHead() {
    m_haveNeutral = m_poseValid;
    if (m_poseValid) {
        for (int i = 0; i < 3; ++i) m_headNeutral[i] = m_headPosRaw[i];
        m_neutralEyeNeckY=.08f*m_rawHeadPose.m[1][1]-.12f*m_rawHeadPose.m[1][2];
        const auto pivot = locomotion::NeckToHead(HeadYawRadians(),
            std::clamp(Cfg().firstPersonRoomscaleNeckMetres, 0.0f, 0.4f));
        m_neutralNeckToHead[0] = pivot.x;
        m_neutralNeckToHead[1] = pivot.z;
        RefreshHeadTranslation();
    }
}

float VRSystem::FirstPersonPostureDrop() const {
    if(!m_poseValid || !m_haveNeutral) return 0;
    // Match the TR4/5 eye-to-neck model: looking down lowers the eyes along
    // an arc without lowering the pelvis. Keep actual eye/view tracking raw.
    const float eyeNeckY=.08f*m_rawHeadPose.m[1][1]-.12f*m_rawHeadPose.m[1][2];
    return FirstPersonHeightDrop()-std::max(0.f,m_neutralEyeNeckY-eyeNeckY);
}

float VRSystem::HeadYawRadians() const {
    if (!m_system) return 0.0f;
    if (!m_poseValid) return 0.0f;
    const float x=-m_headFromTracking.r[2][0],z=m_headFromTracking.r[2][2];
    const float horizontal=std::hypot(x,z);
    if(!std::isfinite(horizontal)) return m_headingYawValid ? m_headingYaw : 0;
    // Yaw from a nearly vertical forward vector is undefined/noisy. Freeze
    // the locomotion/body heading, not the full tracked camera orientation.
    // Hysteresis prevents repeated lock/unlock while nodding at the boundary.
    if(!m_headingYawValid) {
        m_headingYaw=horizontal>=.35f ? std::atan2(x,z)
            : std::atan2(m_headFromTracking.r[0][2],m_headFromTracking.r[0][0]);
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
}

void VRSystem::ApplyRuntimeRecenter() {
    if(!m_runtimeRecenterPending || !m_poseValid) return;
    m_runtimeRecenterPending=false;
    WorldInventoryRecenter();
    m_headingYawValid=m_headingYawVertical=false;
    const float yaw=HeadYawRadians();
    if(FirstPersonActive()) {
        FirstPersonTrackingOriginChanged(m_runtimeRecenterHadYaw ? m_runtimeRecenterYaw-yaw : 0);
        Log("vr: runtime System recenter applied; first-person height recalibrated");
    } else {
        RecenterHead();
    }
    m_runtimeRecenterHadYaw=false;
}

float VRSystem::HeadVerticalOffset() const {
    return m_poseValid && m_haveNeutral
        ? m_headPosRaw[1] - m_headNeutral[1] : 0.0f;
}

float VRSystem::HeadPitchRadians() const {
    if (!m_system || !m_poseValid) return 0.0f;
    // Native weapon angles use positive pitch for looking up (world Y-down).
    return -std::asin(std::clamp(m_headFromTracking.r[2][1], -1.0f, 1.0f));
}

Affine VRSystem::HeadView(bool headTranslation) const {
    const auto& c = Cfg();

    // Head pose, with positional dropped when only rotation is wanted. This is
    // honoured in stereo as well as mono -- it used to apply only to mono, which
    // made PositionalTracking=0 silently do nothing once stereo was on.
    Affine head = m_headFromTracking;
    if (!c.positionalTracking || !headTranslation) {
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

Affine VRSystem::EyeView(Eye eye, bool headTranslation) const {
    const auto& c = Cfg();

    const float scale = LiveWorldUnitsPerMetre();

    // Mono: the centred head view, with no per-eye offset. There is only one
    // image, so applying half an IPD to it would just shift the whole picture.
    if (c.monoTracking) {
        return HeadView(headTranslation);
    }

    Affine head = m_headFromTracking;
    if (!c.positionalTracking || !headTranslation) {
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

void VRSystem::LedgeCatchHaptic() {
    if (m_system) m_gunHaptics.LedgeCatch(GetTickCount64());
}

void VRSystem::GunShotHaptic(int hand) {
    if (m_system) m_gunHaptics.Shot(hand,GetTickCount64());
}

void VRSystem::ReadControllers(HandState out[2]) const {
    out[0] = HandState();
    out[1] = HandState();
    if (!m_system) return;
    if(Cfg().valveIndexControllers) {
        if(m_input) {
            HandState hands[2]{};
            const auto error=m_indexInput.Read(*m_input,hands,Cfg().valveIndexGripPress,Cfg().valveIndexGripRelease);
            out[0]=hands[0];out[1]=hands[1];
            if(error!=m_indexReadError) {
                LogF("vr: Index action polling error=%d (0=recovered)",int(error));
                m_indexReadError=error;
            }
        }
        return;
    }

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

void VRSystem::HolsterHoverHaptic(int hand,bool inside) {
    if(hand<0 || hand>1) return;
    m_gunHaptics.Hover(hand,inside && m_system,GetTickCount64());
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
