#include "Config.h"
#include "DefaultIni.h"
#include "Log.h"

#include <windows.h>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cwchar>

namespace tr {
namespace {

Config g_cfg;
float  g_liveScale = 423.0f;
float  g_liveIpd   = 1.0f;

int GetInt(const wchar_t* key, int def, const wchar_t* ini) {
    return static_cast<int>(GetPrivateProfileIntW(L"VR", key, def, ini));
}

bool GetBool(const wchar_t* key, bool def, const wchar_t* ini) {
    return GetInt(key, def ? 1 : 0, ini) != 0;
}

// GetPrivateProfileIntW parses decimal only and stops at the first non-digit,
// so "0x79" would come back as 0 and silently disable the feature. Read the raw
// string and let wcstol's base-0 handle 0x/decimal alike.
int GetIntAuto(const wchar_t* key, int def, const wchar_t* ini) {
    wchar_t buf[64] = {};
    wchar_t defBuf[64] = {};
    swprintf_s(defBuf, L"%d", def);
    GetPrivateProfileStringW(L"VR", key, defBuf, buf, 64, ini);
    wchar_t* end = nullptr;
    const long v = wcstol(buf, &end, 0);
    if (end == buf) return def;
    return static_cast<int>(v);
}

// Read a comma- or space-separated list of room indices. Returns how many were
// stored. Absent or malformed entries simply yield an empty list -- a bad line
// must not become an exclusion nobody asked for.
// Returns how many parsed, or -1 when the key is ABSENT -- which is not the
// same as present and empty. A caller carrying a built-in list has to tell "the
// ini says nothing" from "the ini says none", and an empty string cannot hold
// that difference on its own, so an out-of-band default carries it instead.
int GetIntList(const wchar_t* key, int* out, int maxOut, const wchar_t* ini) {
    wchar_t buf[512] = {};
    const wchar_t kAbsent[] = L"\x01";
    GetPrivateProfileStringW(L"VR", key, kAbsent, buf, 512, ini);
    if (wcscmp(buf, kAbsent) == 0) return -1;
    int n = 0;
    const wchar_t* p = buf;
    while (*p && n < maxOut) {
        while (*p == L' ' || *p == L',' || *p == L'	') ++p;
        if (!*p) break;
        wchar_t* end = nullptr;
        const long v = wcstol(p, &end, 0);
        if (end == p) break;
        if (v >= 0) out[n++] = static_cast<int>(v);
        p = end;
    }
    return n;
}

float GetFloat(const wchar_t* key, float def, const wchar_t* ini) {
    wchar_t buf[64] = {};
    wchar_t defBuf[64] = {};
    swprintf_s(defBuf, L"%g", def);
    GetPrivateProfileStringW(L"VR", key, defBuf, buf, 64, ini);
    return static_cast<float>(_wtof(buf));
}

} // namespace

const Config& Cfg() { return g_cfg; }

float LiveWorldUnitsPerMetre() { return g_liveScale; }
float LiveIpdScale()           { return g_liveIpd; }

void AdjustWorldScale(float factor) {
    g_liveScale *= factor;
    if (g_liveScale < 16.0f)   g_liveScale = 16.0f;
    if (g_liveScale > 8192.0f) g_liveScale = 8192.0f;
    LogTuning("world scale");
}

void AdjustIpdScale(float factor) {
    g_liveIpd *= factor;
    if (g_liveIpd < 0.1f)  g_liveIpd = 0.1f;
    if (g_liveIpd > 10.0f) g_liveIpd = 10.0f;
    LogTuning("ipd");
}

void ResetTuning() {
    g_liveScale = g_cfg.worldUnitsPerMetre;
    g_liveIpd   = g_cfg.ipdScale;
    LogTuning("reset to ini");
}

void WarnIgnoredOptions() {
    // Mode 3 never writes the view matrix, so anything that acts on it is inert.
    if (g_cfg.eyeOffsetMode == 3) {
        if (!g_cfg.perEyeView) {
            Log("config: PerEyeView=0 is IGNORED with EyeOffsetMode=3 -- that "
                "mode never writes the view matrix. Set EyeOffsetMode=0 to use it.");
        }
        if (g_cfg.debugEyeYawDegrees != 0.0f) {
            Log("config: DebugEyeYawDegrees is IGNORED with EyeOffsetMode=3 -- "
                "it acts on the view matrix, which that mode leaves untouched.");
        }
    }
}

void LogTuning(const char* why) {
    LogF("tuning [%s]: WorldUnitsPerMetre=%.1f  IpdScale=%.3f "
         "(eye separation ~%.1f world units for a 64 mm IPD)",
         why, g_liveScale, g_liveIpd, 0.064f * g_liveScale * g_liveIpd);
}

bool EnsureConfigFile(const wchar_t* path) {
    // CREATE_NEW rather than "test then write": it fails if the file exists,
    // so there is no window between the check and the write in which an
    // existing ini could be clobbered. Overwriting somebody's tuned settings
    // would be the one truly unrecoverable thing this function could do.
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;   // exists, or unwritable

    // The template carries LF; expand to CRLF so the file opens sanely in
    // Notepad. The ini API is happy with either.
    const char* p = DefaultIniText();
    std::string out;
    out.reserve(40000);
    for (; *p; ++p) {
        if (*p == '\n') out += '\r';
        out += *p;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);

    if (!ok || written != out.size()) {
        // A half-written ini is worse than none: the next run would read a
        // truncated file and silently take defaults for everything past the
        // cut. Remove it and let the caller report nothing was created.
        DeleteFileW(path);
        return false;
    }
    return true;
}

void LoadConfig(const wchar_t* ini) {
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        Log("config: no TombRaiderVR.ini found, using defaults");
        return;
    }

    g_cfg.enabled             = GetBool (L"Enabled",            g_cfg.enabled,            ini);

    // Mode=mono | stereo
    {
        wchar_t mode[32] = {};
        GetPrivateProfileStringW(L"VR", L"Mode", L"stereo", mode, 32, ini);
        g_cfg.monoTracking = (_wcsicmp(mode, L"mono") == 0);
    }

    g_cfg.positionalTracking  = GetBool (L"PositionalTracking", g_cfg.positionalTracking, ini);
    g_cfg.seatedOrigin        = GetBool (L"SeatedOrigin",       g_cfg.seatedOrigin,       ini);
    g_cfg.ceilingClearance    = GetBool (L"CeilingClearance",   g_cfg.ceilingClearance,   ini);
    g_cfg.ceilingMarginUnits  = GetFloat(L"CeilingMarginUnits", g_cfg.ceilingMarginUnits, ini);
    g_cfg.portalCulling       = GetBool (L"PortalCulling",      g_cfg.portalCulling,      ini);
    g_cfg.cullFovMarginDegrees = GetFloat(L"CullFovMarginDegrees", g_cfg.cullFovMarginDegrees, ini);
    g_cfg.cullMaxDepth        = GetInt  (L"CullMaxDepth",       g_cfg.cullMaxDepth,       ini);
    g_cfg.cullMaxPortals      = GetInt  (L"CullMaxPortals",     g_cfg.cullMaxPortals,     ini);
    g_cfg.cullFarUnits        = GetFloat(L"CullFarUnits",       g_cfg.cullFarUnits,       ini);
    g_cfg.cullWidenBounds     = GetBool (L"CullWidenBounds",    g_cfg.cullWidenBounds,    ini);
    g_cfg.cullObjects         = GetBool (L"CullObjects",        g_cfg.cullObjects,        ini);
    g_cfg.cullDumpKey         = GetIntAuto(L"CullDumpKey",      g_cfg.cullDumpKey,        ini);
    g_cfg.skyAtInfinity       = GetBool (L"SkyAtInfinity",      g_cfg.skyAtInfinity,      ini);
    g_cfg.firstPerson         = GetBool (L"FirstPerson",        g_cfg.firstPerson,        ini);
    g_cfg.firstPersonJoint    = GetInt  (L"FirstPersonJoint",   g_cfg.firstPersonJoint,   ini);
    g_cfg.firstPersonAnchorX  = GetInt  (L"FirstPersonAnchorX", g_cfg.firstPersonAnchorX, ini);
    g_cfg.firstPersonAnchorY  = GetInt  (L"FirstPersonAnchorY", g_cfg.firstPersonAnchorY, ini);
    g_cfg.firstPersonAnchorZ  = GetInt  (L"FirstPersonAnchorZ", g_cfg.firstPersonAnchorZ, ini);
    g_cfg.firstPersonYawFromLara     = GetBool(L"FirstPersonYawFromLara",
                                                g_cfg.firstPersonYawFromLara, ini);
    g_cfg.firstPersonHeadTranslation = GetBool(L"FirstPersonHeadTranslation",
                                                g_cfg.firstPersonHeadTranslation, ini);
    g_cfg.firstPersonHideHead        = GetBool(L"FirstPersonHideHead",
                                                g_cfg.firstPersonHideHead, ini);
    g_cfg.firstPersonRecenterKey     = GetIntAuto(L"FirstPersonRecenterKey",
                                                g_cfg.firstPersonRecenterKey, ini);
    g_cfg.firstPersonBodyFollowsHead = GetBool (L"FirstPersonBodyFollowsHead",
                                                g_cfg.firstPersonBodyFollowsHead, ini);
    g_cfg.firstPersonBodyDeadzoneDegrees = GetFloat(L"FirstPersonBodyDeadzoneDegrees",
                                                g_cfg.firstPersonBodyDeadzoneDegrees, ini);
    g_cfg.firstPersonBodyTurnDegreesPerFrame = GetFloat(L"FirstPersonBodyTurnDegreesPerFrame",
                                                g_cfg.firstPersonBodyTurnDegreesPerFrame, ini);
    g_cfg.firstPersonTurnDegreesPerSecond = GetFloat(L"FirstPersonTurnDegreesPerSecond",
                                                g_cfg.firstPersonTurnDegreesPerSecond, ini);
    g_cfg.firstPersonTurnDeadzone = GetFloat(L"FirstPersonTurnDeadzone",
                                                g_cfg.firstPersonTurnDeadzone, ini);
    g_cfg.firstPersonDriftLog = GetBool(L"FirstPersonDriftLog", g_cfg.firstPersonDriftLog, ini);
    g_cfg.firstPersonRoomscaleMove   = GetBool (L"FirstPersonRoomscaleMove",
                                                g_cfg.firstPersonRoomscaleMove, ini);
    g_cfg.firstPersonRoomscaleDeadzoneMetres = GetFloat(L"FirstPersonRoomscaleDeadzoneMetres",
                                                g_cfg.firstPersonRoomscaleDeadzoneMetres, ini);
    g_cfg.firstPersonRoomscaleFullMetres = GetFloat(L"FirstPersonRoomscaleFullMetres",
                                                g_cfg.firstPersonRoomscaleNeckMetres, ini);
    g_cfg.firstPersonRoomscaleNeckMetres = GetFloat(L"FirstPersonRoomscaleNeckMetres",
                                                g_cfg.firstPersonRoomscaleNeckMetres, ini);
    g_cfg.firstPersonRoomscaleDriftMetres = GetFloat(L"FirstPersonRoomscaleDriftMetres",
                                                g_cfg.firstPersonRoomscaleDriftMetres, ini);
    g_cfg.firstPersonMoveWithHead    = GetBool (L"FirstPersonMoveWithHead",
                                                g_cfg.firstPersonMoveWithHead, ini);
    g_cfg.worldUnitsPerMetre  = GetFloat(L"WorldUnitsPerMetre", g_cfg.worldUnitsPerMetre, ini);
    g_cfg.ipdScale            = GetFloat(L"IpdScale",           g_cfg.ipdScale,           ini);
    g_cfg.scaleUpKey          = GetIntAuto(L"ScaleUpKey",       g_cfg.scaleUpKey,         ini);
    g_cfg.scaleDownKey        = GetIntAuto(L"ScaleDownKey",     g_cfg.scaleDownKey,       ini);
    g_cfg.ipdUpKey            = GetIntAuto(L"IpdUpKey",         g_cfg.ipdUpKey,           ini);
    g_cfg.ipdDownKey          = GetIntAuto(L"IpdDownKey",       g_cfg.ipdDownKey,         ini);
    g_cfg.resetTuningKey      = GetIntAuto(L"ResetTuningKey",   g_cfg.resetTuningKey,     ini);
    g_cfg.scaleStep           = GetFloat(L"ScaleStep",          g_cfg.scaleStep,          ini);
    if (g_cfg.scaleStep < 1.01f) g_cfg.scaleStep = 1.01f;
    if (g_cfg.scaleStep > 4.0f)  g_cfg.scaleStep = 4.0f;
    g_cfg.eyeWidth            = GetInt  (L"EyeWidth",           g_cfg.eyeWidth,           ini);
    g_cfg.eyeHeight           = GetInt  (L"EyeHeight",          g_cfg.eyeHeight,          ini);
    g_cfg.superSample         = GetFloat(L"SuperSample",        g_cfg.superSample,        ini);
    g_cfg.flipProjectionY     = GetBool (L"FlipProjectionY",    g_cfg.flipProjectionY,    ini);
    g_cfg.flipViewY           = GetBool (L"FlipViewY",          g_cfg.flipViewY,          ini);
    g_cfg.swapEyes            = GetBool (L"SwapEyes",           g_cfg.swapEyes,           ini);
    g_cfg.flipSubmitV         = GetBool (L"FlipSubmitV",        g_cfg.flipSubmitV,        ini);
    g_cfg.eyeMarkers          = GetBool (L"EyeMarkers",         g_cfg.eyeMarkers,         ini);
    g_cfg.perEyeView          = GetBool (L"PerEyeView",         g_cfg.perEyeView,         ini);
    g_cfg.debugEyeYawDegrees  = GetFloat(L"DebugEyeYawDegrees", g_cfg.debugEyeYawDegrees, ini);
    g_cfg.eyeOffsetMode       = GetIntAuto(L"EyeOffsetMode",    g_cfg.eyeOffsetMode,      ini);
    g_cfg.hudDepthMetres      = GetFloat(L"HudDepthMetres",     g_cfg.hudDepthMetres,     ini);
    g_cfg.hudSizeDegrees      = GetFloat(L"HudSizeDegrees",     g_cfg.hudSizeDegrees,     ini);
    g_cfg.hudLockToHead       = GetBool (L"HudLockToHead",      g_cfg.hudLockToHead,      ini);
    g_cfg.hudFlipY            = GetBool (L"HudFlipY",           g_cfg.hudFlipY,           ini);
    g_cfg.preserveProjOffset  = GetBool (L"PreserveProjOffset", g_cfg.preserveProjOffset, ini);
    g_cfg.projOffsetScale     = GetFloat(L"ProjOffsetScale",    g_cfg.projOffsetScale,    ini);
    g_cfg.ortho3D             = GetBool (L"Ortho3D",            g_cfg.ortho3D,            ini);
    g_cfg.ortho3DDepthMetres  = GetFloat(L"Ortho3DDepthMetres", g_cfg.ortho3DDepthMetres, ini);
    g_cfg.ortho3DLockToHead   = GetBool (L"Ortho3DLockToHead",  g_cfg.ortho3DLockToHead,  ini);
    g_cfg.ortho3DSizeDegrees  = GetFloat(L"Ortho3DSizeDegrees", g_cfg.ortho3DSizeDegrees, ini);
    g_cfg.ortho3DSlabMetres   = GetFloat(L"Ortho3DSlabMetres",  g_cfg.ortho3DSlabMetres,  ini);
    g_cfg.dumpDraws           = GetIntAuto(L"DumpDraws",        g_cfg.dumpDraws,          ini);
    g_cfg.dumpKey             = GetIntAuto(L"DumpKey",          g_cfg.dumpKey,            ini);
    g_cfg.videoDepthMetres    = GetFloat(L"VideoDepthMetres",   g_cfg.videoDepthMetres,   ini);
    g_cfg.videoLockToHead     = GetBool (L"VideoLockToHead",    g_cfg.videoLockToHead,    ini);
    g_cfg.videoSizeDegrees    = GetFloat(L"VideoSizeDegrees",   g_cfg.videoSizeDegrees,   ini);
    g_cfg.videoOffscreen      = GetBool (L"VideoOffscreen",     g_cfg.videoOffscreen,     ini);
    g_cfg.videoFlipV          = GetBool (L"VideoFlipV",         g_cfg.videoFlipV,         ini);
    g_cfg.gamepadEnabled      = GetBool (L"GamepadEnabled",     g_cfg.gamepadEnabled,     ini);
    g_cfg.menuChordSeconds    = GetFloat(L"MenuChordSeconds",    g_cfg.menuChordSeconds,   ini);
    g_cfg.menuChordPressSeconds = GetFloat(L"MenuChordPressSeconds", g_cfg.menuChordPressSeconds, ini);
    g_cfg.dpadShift           = GetBool (L"DpadShift",           g_cfg.dpadShift,          ini);
    g_cfg.dpadShiftDeadzone   = GetFloat(L"DpadShiftDeadzone",   g_cfg.dpadShiftDeadzone,  ini);
    g_cfg.gamepadLogButtons   = GetBool (L"GamepadLogButtons",  g_cfg.gamepadLogButtons,  ini);
    g_cfg.decoupledPitch      = GetBool (L"DecoupledPitch",     g_cfg.decoupledPitch,     ini);
    g_cfg.decoupledPitchChord = GetBool (L"DecoupledPitchChord", g_cfg.decoupledPitchChord, ini);
    g_cfg.decoupledPitchWaterOff = GetBool (L"DecoupledPitchWaterOff", g_cfg.decoupledPitchWaterOff, ini);
    g_cfg.gamepadMenuUsesBack = GetBool (L"GamepadMenuUsesBack", g_cfg.gamepadMenuUsesBack, ini);
    g_cfg.logCallsites        = GetBool (L"LogCallsites",       g_cfg.logCallsites,       ini);
    g_cfg.perEyeProjection    = GetIntAuto(L"PerEyeProjection", g_cfg.perEyeProjection,   ini);
    g_cfg.flatHud             = GetBool (L"FlatHud",            g_cfg.flatHud,            ini);
    g_cfg.duplicateDraws      = GetBool (L"DuplicateDraws",     g_cfg.duplicateDraws,     ini);
    g_cfg.mirrorToWindow      = GetBool (L"MirrorToWindow",     g_cfg.mirrorToWindow,     ini);
    g_cfg.nearClip            = GetFloat(L"NearClip",           g_cfg.nearClip,           ini);
    g_cfg.farClip             = GetFloat(L"FarClip",            g_cfg.farClip,            ini);
    g_cfg.verboseFirstFrame   = GetBool (L"VerboseFirstFrame",  g_cfg.verboseFirstFrame,  ini);
    g_cfg.traceFrames         = GetInt  (L"TraceFrames",        g_cfg.traceFrames,        ini);
    g_cfg.traceStartFrame     = GetInt  (L"TraceStartFrame",    g_cfg.traceStartFrame,    ini);
    g_cfg.traceKey            = GetIntAuto(L"TraceKey",         g_cfg.traceKey,           ini);

    g_liveScale = g_cfg.worldUnitsPerMetre;
    g_liveIpd   = g_cfg.ipdScale;

    if (g_cfg.superSample < 0.25f) g_cfg.superSample = 0.25f;
    if (g_cfg.superSample > 4.0f)  g_cfg.superSample = 4.0f;
    if (g_cfg.worldUnitsPerMetre < 1.0f) g_cfg.worldUnitsPerMetre = 1.0f;

    // Culling budgets. The lower bounds are not taste: a depth or portal budget
    // of zero would silently turn the feature into a no-op that still logs as
    // active, and the upper bounds keep a typo out of the render thread.
    if (g_cfg.cullMaxDepth   < 1)     g_cfg.cullMaxDepth   = 1;
    if (g_cfg.cullMaxDepth   > 64)    g_cfg.cullMaxDepth   = 64;
    if (g_cfg.cullMaxPortals < 64)    g_cfg.cullMaxPortals = 64;
    if (g_cfg.cullMaxPortals > 65536) g_cfg.cullMaxPortals = 65536;
    if (g_cfg.cullFovMarginDegrees < 0.0f)  g_cfg.cullFovMarginDegrees = 0.0f;
    if (g_cfg.cullFovMarginDegrees > 45.0f) g_cfg.cullFovMarginDegrees = 45.0f;
    if (g_cfg.cullFarUnits < 0.0f)          g_cfg.cullFarUnits = 0.0f;

    LogF("config: mode=%s tracking=%s origin=%s units/m=%.1f ss=%.2f "
         "flipProjY=%d flipViewY=%d dup=%d flatHud=%d",
         g_cfg.monoTracking ? "MONO head-tracking" : "stereo",
         g_cfg.positionalTracking ? "rotation+position" : "rotation only",
         g_cfg.seatedOrigin ? "seated" : "standing",
         g_cfg.worldUnitsPerMetre, g_cfg.superSample,
         g_cfg.flipProjectionY, g_cfg.flipViewY,
         g_cfg.duplicateDraws, g_cfg.flatHud);
    LogF("config: culling=%s margin=%.1fdeg depth<=%d portals<=%d far=%.0f "
         "widen=%d objects=%d  sky=%s",
         g_cfg.portalCulling ? "head frustum" : "ENGINE (rooms will vanish)",
         g_cfg.cullFovMarginDegrees, g_cfg.cullMaxDepth, g_cfg.cullMaxPortals,
         g_cfg.cullFarUnits, g_cfg.cullWidenBounds, g_cfg.cullObjects,
         g_cfg.skyAtInfinity ? "infinity" : "ENGINE (finite dome)");
    if (g_cfg.firstPerson) {
        LogF("config: camera=FIRST PERSON, joint %d offset (%d,%d,%d), yaw from %s, "
             "head translation %s, head %s",
             g_cfg.firstPersonJoint, g_cfg.firstPersonAnchorX,
             g_cfg.firstPersonAnchorY, g_cfg.firstPersonAnchorZ,
             "stable VR heading",
             g_cfg.firstPersonHeadTranslation ? "ON" : "off (rotation only)",
             g_cfg.firstPersonHideHead ? "hidden (mesh_bits bit 14)" : "DRAWN");
        LogF("config: first person body-follows-head=%s (dead zone %.0f deg, "
             "%.1f deg/frame), move-with-head=%s",
             g_cfg.firstPersonBodyFollowsHead ? "on" : "OFF",
             g_cfg.firstPersonBodyDeadzoneDegrees,
             g_cfg.firstPersonBodyTurnDegreesPerFrame,
             g_cfg.firstPersonMoveWithHead ? "on (camera-relative controls only)" : "OFF");
        LogF("config: first person roomscale=%s (dead zone %.2f m, neck pivot %.2f m, "
             "direct collision drag; turn %.0f deg/sec)",
             g_cfg.firstPersonRoomscaleMove ? "on" : "OFF",
             g_cfg.firstPersonRoomscaleDeadzoneMetres,
             g_cfg.firstPersonRoomscaleNeckMetres,
             g_cfg.firstPersonTurnDegreesPerSecond);
    } else
        Log("config: camera=third person (the engine's own)");
    if (g_cfg.traceFrames > 0) {
        LogF("config: frame-graph trace armed -- %d frame(s), hotkey vk=0x%02X",
             g_cfg.traceFrames, g_cfg.traceKey);
    }
}

} // namespace tr
