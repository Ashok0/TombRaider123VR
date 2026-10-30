// Hooks.cpp -- the stereo injection layer.
//
//   vid_setPass     REQUIRED WORK. Classifies each pass as world-space or 2D by
//                   reading which matrix vid_state.proj was pointed at. This is
//                   the only signal in the engine that distinguishes "3D scene"
//                   from "HUD/menu/subtitle", and it is the gate for whether a
//                   pass gets per-eye matrices at all.
//
//   validate_draw   REQUIRED WORK. The single choke point where every uniform
//                   reaches the GPU. Per-eye projection and view are written
//                   into mProj[1] / mView_packed here, the dirty bits are
//                   forced, the original uploads, and the engine's own matrices
//                   are put back so it never observes the substitution.
//
//   ogl_draw        PLUMBING. validate_draw cannot issue a draw -- its two
//   ogl_drawVB      callers do, immediately after it returns. Duplicating the
//                   draw per eye therefore has to happen one level up.
//
//   ogl_present     PLUMBING. The frame boundary: submit to the compositor,
//                   mirror to the window, then WaitGetPoses for the next frame.
//
// TR4/TR5 issue each draw twice into the halves of one double-wide target. TR6
// has a separate PDB-identified render-only hook which replays its complete
// offscreen scene chain once per eye. Neither path needs shader changes, which
// matters: this build has 202 shader pairs and a GL 3.2 Core context, so
// GL_OVR_multiview2 would mean editing 404 GLSL sources.
#include "Hooks.h"
#include "TR4Effects.h"
#include "Engine.h"
#include "StereoMath.h"
#include "Config.h"
#include "GL.h"
#include "Log.h"
#include "InlineHook.h"
#include "StereoRenderer.h"
#include "VideoPanel.h"
#include "Gamepad.h"
#include "InventoryScene.h"
#include "Callsite.h"
#include "GameDll.h"
#include "PortalCull.h"
#include "Sky.h"
#include "DynamicBones.h"
#include "BoneSkin.h"
#include "Overlay.h"
#include "VRSystem.h"
#include "FirstPerson.h"
#include "HKScope.h"
#include "WorldLaser.h"

#include <cstring>
#include <intrin.h>
#include <cmath>
#include <algorithm>

namespace tr {
namespace {

// --- hook objects -----------------------------------------------------------
hook::InlineHook g_hSetPass;
hook::InlineHook g_hValidateDraw;
hook::InlineHook g_hDraw;
hook::InlineHook g_hDrawVB;
hook::InlineHook g_hPresent;
hook::InlineHook g_hFmvShow;
hook::InlineHook g_hTr6RenderScene;
hook::InlineHook g_hTr6Calculate;
hook::InlineHook g_hTr6ClippedObb;
hook::InlineHook g_hTr6ClippedAabb;
hook::InlineHook g_hTr6DrawProjectedShadows;
hook::InlineHook g_hTr6EffectsUpdate;
hook::InlineHook g_hTr6FxCamDist;
hook::InlineHook g_hTr6FxBoundsClip;
hook::InlineHook g_hTr6FxNodeBoundsClip;

typedef void(__cdecl* Fn_vid_setPass)(int shader, float* params, int cull, int blend);
typedef void(__cdecl* Fn_validate_draw)();
typedef void(__cdecl* Fn_ogl_draw)(void* vb, unsigned firstIndex, unsigned count, int strip);
typedef void(__cdecl* Fn_ogl_drawVB)(int fvf, void* vb, void* ib, int stride,
                                     unsigned first, unsigned count, int strip);
typedef void(__cdecl* Fn_ogl_present)();
typedef void(__cdecl* Fn_tr6_render_scene)();
typedef void(__fastcall* Fn_tr6_draw_projected_shadows)(void* drawBuffer);
typedef bool(__fastcall* Fn_tr6_clipped_obb)(
    const mat4* cameraProject, const void* viewport,
    const void* boundsMin, const void* boundsMax);
typedef bool(__fastcall* Fn_tr6_clipped_aabb)(
    const void* clipBounds, const void* boundsMin, const void* boundsMax);

// PDB: every SYS_DRAW_ITEM_POOL is the same 24-byte header. These two pools
// are the input room-pointer list passed to Calculate and the rooms/portal
// rectangles at the front of SYS_DRAW_CRP.
struct Tr6ItemPool {
    uint32_t maxItems;
    uint32_t items;
    uint32_t itemsNeeded;
    uint32_t padding;
    void*    data;
};
static_assert(sizeof(Tr6ItemPool) == 24, "TR6 item-pool layout changed");

struct Tr6RoomPortalsDesc {
    uint32_t roomIndex;
    uint16_t firstPortal;
    uint16_t portalCount;
};
static_assert(sizeof(Tr6RoomPortalsDesc) == 8,
              "TR6 room descriptor layout changed");

// PDB: SYS_DRAW_CAMERA_VIEW is 400 bytes. Calculate receives it as const and
// only the first three matrices need changing; the opaque tail is copied so
// every other field reaches the original function byte-for-byte.
struct alignas(16) Tr6CameraView {
    mat4    project;
    mat4    camera;
    mat4    cameraProject;
    uint8_t tail[208];
};
static_assert(sizeof(Tr6CameraView) == 400, "TR6 camera-view layout changed");

typedef void(__fastcall* Fn_tr6_effects_update)(
    const Tr6CameraView* cameraView);
typedef float(__fastcall* Fn_tr6_fx_cam_dist)(const float* position);
typedef int32_t(__fastcall* Fn_tr6_fx_bounds_clip)(const float* bounds);

typedef void(__fastcall* Fn_tr6_calculate)(
    void* crp, void* drawBuffer, void* matrixStack, void* roomPool,
    const Tr6CameraView* cameraView, uint32_t flags, float lodScale,
    const void* clipMin, const void* clipMax);

constexpr int kTr6MaxRooms = 192;

// --- verified prologue bytes ------------------------------------------------
// Read out of Ghidra's disassembly of this exact build. Install() refuses to
// patch if these do not match, so a game update degrades to "VR did not start"
// rather than a corrupted instruction stream.

// 48 8B C4              mov  rax, rsp
// 48 81 EC 88 00 00 00  sub  rsp, 0x88          -> 10 bytes, both PIC
const uint8_t kSetPassPrologue[]  = { 0x48, 0x8B, 0xC4, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 };

// 40 53                 push rbx
// 48 83 EC 30           sub  rsp, 0x30          -> 6 bytes, both PIC
const uint8_t kValidatePrologue[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30 };

// 48 89 5C 24 08        mov  [rsp+8], rbx       -> 5 bytes, PIC
const uint8_t kDrawPrologue[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

// 48 83 EC 28           sub  rsp, 0x28
// 33 C9                 xor  ecx, ecx           -> 6 bytes, both PIC
// (the next instruction is a RIP-relative CMP, so we stop here)
const uint8_t kPresentPrologue[]  = { 0x48, 0x83, 0xEC, 0x28, 0x33, 0xC9 };

// MOV RAX,RSP ; MOV [RAX+8],RBX  -- 3 + 4 = 7 bytes, no RIP-relative operand.
const uint8_t kFmvShowPrologue[]  = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08 };

// tomb6.dll prologues. Addresses, call-site allowlists and the three prologues
// that embed a RIP-relative displacement are per build and live in Tr6Layout
// (GameDll.h / GameDll.cpp). The bytes below are identical in the debug and the
// retail tomb6.dll.
//
// App_Render_Scene:
//   48 8B C4              mov rax, rsp
//   48 89 58 08           mov [rax+8], rbx
// Seven bytes, both complete and position-independent.
const uint8_t kTr6RenderScenePrologue[] =
    { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08 };

// SYS_DRAW_CRP::Calculate:
//   48 8B C4              mov rax, rsp
//   55                    push rbp
//   53                    push rbx
// Five complete, position-independent bytes.
const uint8_t kTr6CalculatePrologue[] = { 0x48, 0x8B, 0xC4, 0x55, 0x53 };

// ClippedOBB_CPP: MOV RAX,RSP ; MOV [RAX+8],RBX. The function is shared by
// gameplay and shadows, so its detour changes only exact render return sites.
const uint8_t kTr6ClippedObbPrologue[] =
    { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08 };

// The final object renderer calls this cheaper axis-aligned test before
// ClippedOBB_CPP. MOV RAX,RSP ; PUSH RBP ; LEA RBP,[RAX-0x5F].
const uint8_t kTr6ClippedAabbPrologue[] =
    { 0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0x68, 0xA1 };

// App_DrawChar_DrawProjectedShadows: MOV R11,RSP ; MOV [R11+0x10],RDX.
// This is the light-camera depth pass used to construct projected character
// shadows. It must not receive a headset-eye view transform.
const uint8_t kTr6DrawProjectedShadowsPrologue[] =
    { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x53, 0x10 };

// App_DrawEffects_UpdateRenderData: MOV RAX,RSP ; PUSH R12.
const uint8_t kTr6EffectsUpdatePrologue[] =
    { 0x48, 0x8B, 0xC4, 0x41, 0x54 };

// fxCamDist: SUB RSP,0x28 followed by a RIP-relative read of
// gcamCamera.Position.x. The displacement starts at byte 8 and is relocated in
// the trampoline.
const int kTr6FxCamDistRipOffsets[] = { 8 };

// mathIsBoundsClipped: SUB RSP,0x48 followed by a RIP-relative security-cookie
// read. Only the call made by fxProcessBox's local-light case is overridden.
const int kTr6FxBoundsClipRipOffsets[] = { 7 };

// mathIsBoundsClippedAlt has the same stack/cookie prologue. mapDrawRoomList
// uses its sign to set every FX node's primary 0x100 off-camera flag.
const int kTr6FxNodeBoundsClipRipOffsets[] = { 7 };

// TR6's stock 65536-unit far plane is visible in a headset because the wider
// tracked view exposes long sightlines the third-person monitor camera rarely
// frames. Four times the stock distance keeps the level geometry inside the
// render frustum without materially compromising a 24-bit depth buffer.
constexpr float kTr6VrFarPlane = 262144.0f;

// Call-site policy, whose addresses are per build (see Tr6Layout):
//
//   * mainCalculateReturn / reflectionCalculateReturn -- the two render-side
//     Calculate calls. mapCalcVisibleRooms is intentionally not hooked: its
//     result also owns object state, not just a room list, and a second private
//     pass hid stock pickups.
//   * mainRoomGroupObbReturn / clipRoomObbReturn -- App_Render_Scene_Main first
//     rejects a transformed static mesh directly, then ClipRoom_SYS_D3D_ROOM
//     rejects individual 0x40-byte render runs. Both tests are downstream of the
//     room-list builder and both use the third-person game camera. Returning
//     "not clipped" at only these sites preserves all other OBB tests while
//     making every run of an accepted room available to the VR view.
//   * character/animated/water OBB returns -- object-list builders perform
//     their own OBB reject after a room has survived portal traversal.
//   * sceneObject AABB/OBB returns -- completed object pools are clipped again
//     while App_Render_Scene_Main consumes them. Each path first calls the AABB
//     helper and only then ClippedOBB_CPP, so small pickups require an exact
//     paired bypass at both stages. Nearby calls belong to other scene views or
//     map/gameplay work; the allowlists stay exact rather than disabling either
//     shared bounds helper globally.
bool Tr6ListHas(const uint32_t* list, uintptr_t returnRva) {
    for (int i = 0; i < Tr6Layout::kMaxSceneObjectSites; ++i)
        if (list[i] != 0 && list[i] == returnRva) return true;
    return false;
}

// --- per-frame state --------------------------------------------------------

bool  g_worldPass    = false;   // set by the vid_setPass hook
int   g_currentEye   = 0;
bool  g_inDuplicate  = false;   // guards against recursive duplication
bool  g_ready        = false;   // GL objects created, VR live
bool  g_loggedFrame  = false;
// One-shot per-eye diagnostics. The single-shot log only ever showed eye 0,
// which hid the only question that matters: do the two eyes actually differ?
bool  g_loggedEye[2] = { false, false };
float g_eyeViewT[2][3] = {};

bool  g_reportedSeparation = false;
// Per-eye injection counts. If eye 1 never gets injected, the duplication loop
// is not reaching validate_draw with the right eye selected.
unsigned g_injectCount[2] = { 0, 0 };
unsigned g_dupCount = 0;
unsigned g_classifyMismatch = 0;   // live vs cached world-pass disagreements
unsigned g_lastReportFrame = 0;
// Wall-clock at the last health report, for a real frame-rate figure. Under
// alternate-eye each eye updates at HALF this, so it is the number that decides
// whether the judder is worth chasing.
LARGE_INTEGER g_lastReportTime = {};
unsigned g_prevDup = 0, g_prevInj0 = 0, g_prevInj1 = 0;

// World-space draws seen at validate_draw, counted per eye-pass, so a
// duplicated draw contributes two. This is the honest denominator for the
// health report. The previous one divided injections by duplications, but
// injection and duplication are gated on the SAME condition, so it read 100%
// by construction and could never detect the failure it warned about.
unsigned g_worldDraws     = 0;
unsigned g_worldOffscreen = 0;   // world draws skipped: target was not the backbuffer

// Draws on the ortho-3D layer -- the inventory. Counted separately from
// g_worldDraws precisely because they are NOT world-space: folding them in
// would inflate the health report's denominator with draws that are never
// meant to be injected, and the injection percentage would read as a fault.
unsigned g_ortho3DDraws  = 0;
bool     g_loggedOrtho3D = false;

// Draws whose projection carried a non-zero vid_setPerspOffset shear -- the
// engine placing an element on screen through the projection rather than
// through its geometry. Zero during gameplay; the inventory is nothing but.
unsigned g_offsetDraws   = 0;
bool     g_loggedOffset  = false;

// Draws issued from inside DrawSkyHD -- the HD sky dome. Per injected eye, so
// the sky= count in the health report should be non-zero outdoors.
unsigned g_skyDraws      = 0;
bool     g_loggedSky     = false;

unsigned g_prevOrtho3D   = 0;
unsigned g_prevOffset    = 0;
unsigned g_prevSky       = 0;
unsigned g_prevWorld = 0, g_prevOffscreen = 0;

// GPU-side verification of the per-eye view matrix. Re-armed at every health
// report rather than latched once, so it tracks live IpdScale and
// WorldUnitsPerMetre changes instead of only ever sampling frame 1.
// Shader ids seen with no uProjMatrix at all -- the clip-space-direct passes.
// Logged once each so that if something still refuses to fuse, the log names
// the shader instead of costing another play session to find.
int   g_bypassSeen[8]  = { -1, -1, -1, -1, -1, -1, -1, -1 };

// Alternate-eye rendering: which eye this whole frame belongs to.
int  g_frameEye = 0;
bool g_loggedAltEye = false;

// Offscreen world draws, this frame and last. This is how we tell TR6 gameplay
// (an offscreen 3D scene, ~800 a frame) from its menus and FMVs (2D, near zero).
unsigned g_offscreenWorld     = 0;
unsigned g_offscreenWorldPrev = 0;

// Latched once per frame. AlternateEyeActive() is consulted from the draw path,
// the injection gate and present, so it MUST NOT change mid-frame -- flipping
// halfway would leave the frame split across two targets.
bool g_aerLatched = false;

// The hook runs App_Render_Scene twice, but everything outside that render-only
// boundary (input, physics, audio and game state) still runs once.
bool g_inNativeTr6Scene    = false;
bool g_loggedNativeTr6     = false;
bool g_loggedTr6Cull       = false;
bool g_warnedTr6Build      = false;
bool g_tr6SceneAttempted   = false;
bool g_tr6CullAttempted    = false;
uintptr_t g_tr6ModuleBase  = 0;
// The tomb6.dll row g_tr6ModuleBase belongs to. Set together with it, before
// any TR6 hook is installed, so every detour can rely on it when the base is.
const Tr6Layout* g_tr6     = nullptr;
bool g_loggedTr6RoomRuns         = false;
bool g_loggedTr6AllRooms         = false;
bool g_loggedTr6ObjectCull       = false;
bool g_loggedTr6SceneObjectCull  = false;
bool g_loggedTr6SceneObjectAabb  = false;
bool g_loggedTr6FarPlane         = false;
bool g_loggedTr6ShadowIsolation  = false;
bool g_loggedTr6EffectsCamera    = false;
bool g_loggedTr6FxDistance        = false;
bool g_loggedTr6FxBounds          = false;
bool g_loggedTr6FxNodeBounds      = false;
bool g_tr6VrCalculateActive      = false;
bool g_inTr6ShadowDepthPass      = false;

uint32_t ModuleTimestamp(HMODULE module) {
    if (!module) return 0;
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

bool NativeTr6HookReady() {
    return g_hTr6RenderScene.installed()
        && g_hTr6DrawProjectedShadows.installed()
        && g_hTr6EffectsUpdate.installed()
        && g_hTr6FxCamDist.installed()
        && g_hTr6FxBoundsClip.installed()
        && g_hTr6FxNodeBoundsClip.installed();
}

bool NativeTr6Capable() {
    return Cfg().nativeStereoGame6
        && !Cfg().monoTracking
        && CurrentGame() == 2
        && NativeTr6HookReady()
        && Stereo().monoValid();
}

bool NativeTr6Active() {
    return NativeTr6Capable();
}

bool AlternateEyeCapable() {
    return Cfg().alternateEyeGame6
        && !(Cfg().nativeStereoGame6 && NativeTr6HookReady())
        && !Cfg().monoTracking
        && CurrentGame() == 2
        && Stereo().monoValid();
}

void TryInstallTr6Hooks();

// TR6 renders its scene offscreen, so per-draw duplication cannot reach it.
// Each frame is one eye instead -- but only while there IS an offscreen scene.
// See Config::alternateEyeMinOffscreen.
bool AlternateEyeActive() {
    return g_aerLatched;
}

bool  g_verifyArmed    = true;
bool  g_verifyDone[2]  = { false, false };
float g_gpuViewT[2][3] = {};
int   g_gpuProg[2]     = { 0, 0 };
int   g_gpuLoc[2]      = { -1, -1 };

// Histogram of the actual vid_state.proj pointers seen at draw time.
// The world/2D test assumes proj is only ever &mProj[0] or &mProj[1]; if the
// engine points it somewhere else during gameplay the test silently says "2D"
// for everything and no per-eye matrices are ever applied.
struct ProjSeen { const void* ptr; unsigned count; };
ProjSeen g_projSeen[6] = {};

void NoteProjPointer(const void* p) {
    for (auto& e : g_projSeen) {
        if (e.ptr == p) { ++e.count; return; }
        if (e.ptr == nullptr) { e.ptr = p; e.count = 1; return; }
    }
}

void LogProjHistogram() {
    const void* ortho = &Proj()[0];
    const void* persp = &Proj()[1];
    LogF("proj pointers: mProj[0](ortho)=%p  mProj[1](persp)=%p", ortho, persp);
    for (const auto& e : g_projSeen) {
        if (!e.ptr) break;
        const char* what = (e.ptr == ortho) ? "ortho/2D"
                         : (e.ptr == persp) ? "perspective/world"
                         : "UNRECOGNISED";
        LogF("  proj=%p  draws=%-7u  %s", e.ptr, e.count, what);
    }
}
GLuint g_realDefaultFbo = 0;
unsigned g_frameIndex = 0;

// Deferred OpenVR bring-up (see BringUpVR).
unsigned g_vrAttempts        = 0;
unsigned g_vrLastAttemptFrame = 0;
bool     g_vrGaveUp          = false;
bool     g_loggedStereoFrame = false;

// --- frame-graph tracer -----------------------------------------------------
// Answers the one question stereo depends on: where is the 3D scene actually
// drawn? Reads the ogl_rt latch before each draw and logs every transition,
// with how many world-space vs 2D draws happened against each target. No extra
// hook needed -- validate_draw already runs before every single draw.
struct TraceRun {
    int32_t colorId = -99, depthId = -99, colorIndex = -99;
    GLint   fbo     = -1;
    int     world   = 0;
    int     flat    = 0;
};
TraceRun g_run;
bool     g_tracing        = false;
int      g_traceEmitted   = 0;
int      g_frameWorld     = 0;   // per-frame totals, to tell gameplay from a menu
int      g_frameFlat      = 0;
unsigned g_traceUntilFrame = 0;  // set by the hotkey
bool     g_traceKeyWasDown = false;

// Poll the capture hotkey. Guessing a frame number is unreliable -- frame 600
// can easily still be a title screen -- so the trace is armed by a keypress
// while the game is actually being played.
void PollTraceKey() {
    const auto& c = Cfg();
    if (c.traceKey == 0 || c.traceFrames <= 0) return;

    const bool down = (GetAsyncKeyState(c.traceKey) & 0x8000) != 0;
    if (down && !g_traceKeyWasDown && !FirstPersonCalibrationKeyReserved(c.traceKey)) {
        g_traceUntilFrame = g_frameIndex + static_cast<unsigned>(c.traceFrames);
        LogF("trace: hotkey pressed, capturing %d frame(s)", c.traceFrames);
    }
    g_traceKeyWasDown = down;
}

// --- per-draw state dump ----------------------------------------------------
// Where does the engine put the placement of one element relative to the next?
// It has three choices -- projection, view matrix, model matrix -- and which
// one it used decides which of our substitutions can break a layout. Guessing
// is how the ortho3D theory got written and shipped inert; this measures it.
//
// One line per draw (eye 0 only, so duplication does not double the log),
// logged BEFORE any substitution, so what appears is what the engine set.
unsigned g_dumpRemaining  = 0;
unsigned g_dumpIndex      = 0;
bool     g_dumpKeyWasDown = false;

void PollDumpKey() {
    const auto& c = Cfg();
    if (c.dumpKey == 0 || c.dumpDraws <= 0) return;
    const bool down = (GetAsyncKeyState(c.dumpKey) & 0x8000) != 0;
    if (down && !g_dumpKeyWasDown && !FirstPersonCalibrationKeyReserved(c.dumpKey)) {
        g_dumpRemaining = static_cast<unsigned>(c.dumpDraws);
        g_dumpIndex     = 0;
        LogF("dump: hotkey pressed, capturing the next %d draw(s)", c.dumpDraws);
    }
    g_dumpKeyWasDown = down;
}

void DumpDrawState() {
    if (g_dumpRemaining == 0 || g_currentEye != 0) return;
    --g_dumpRemaining;

    const RenderState& vs = VidState();
    const mat4* p = vs.proj;

    // Which slot, and -- separately -- what the matrix in it actually is.
    // vid_setOrtho3D can put ortho content in the world slot, so the two
    // questions are not the same one.
    const char* slot = (p == &Proj()[0]) ? "ortho-slot"
                     : (p == &Proj()[1]) ? "world-slot"
                     : "OTHER-slot";
    const char* kind = !p ? "null"
                     : (IsOrthoProjection(*p) ? "ORTHO" : "persp");

    // view and model are row-major packed 3x4 affines uploaded as vec4[4], so
    // the translation is floats 3, 7, 11 -- not the column-major 12, 13, 14
    // that the projection uses.
    float vt[3] = { 0, 0, 0 };
    float mt[3] = { 0, 0, 0 };
    if (vs.view)  { vt[0] = vs.view->m[3];  vt[1] = vs.view->m[7];  vt[2] = vs.view->m[11]; }
    if (vs.model) { mt[0] = vs.model->m[3]; mt[1] = vs.model->m[7]; mt[2] = vs.model->m[11]; }

    if (p) {
        LogF("dump %-3u f=%u sh=%-3d %s/%s  P[x=%.4f y=%.4f z=%.4f w=%.4f "
             "shear=%.4f,%.4f ofs=%.4f,%.4f]  V=(%.1f,%.1f,%.1f)  "
             "M=(%.1f,%.1f,%.1f)  joints=%d rt=%d",
             g_dumpIndex++, g_frameIndex, vs.shader, slot, kind,
             p->m[0], p->m[5], p->m[10], p->m[11], p->m[8], p->m[9],
             p->m[12], p->m[13],
             vt[0], vt[1], vt[2], mt[0], mt[1], mt[2],
             vs.num_joints, Rt().color_id);
    } else {
        LogF("dump %-3u f=%u sh=%-3d %s/%s  V=(%.1f,%.1f,%.1f)  "
             "M=(%.1f,%.1f,%.1f)  joints=%d rt=%d",
             g_dumpIndex++, g_frameIndex, vs.shader, slot, kind,
             vt[0], vt[1], vt[2], mt[0], mt[1], mt[2],
             vs.num_joints, Rt().color_id);
    }

    if (g_dumpRemaining == 0) Log("dump: capture complete");
}

// --- live tuning hotkeys ----------------------------------------------------
bool g_tuneWasDown[6] = { false, false, false, false, false, false };

void PollTuningKeys() {
    const auto& c = Cfg();
    const int keys[6] = { c.scaleUpKey, c.scaleDownKey,
                          c.ipdUpKey,   c.ipdDownKey, c.resetTuningKey,
                          c.recentreKey };
    const float step = c.scaleStep;
    for (int i = 0; i < 6; ++i) {
        if (keys[i] == 0) continue;
        const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !g_tuneWasDown[i] && !FirstPersonCalibrationKeyReserved(keys[i])) {
            switch (i) {
            case 0: AdjustWorldScale(step);        break;  // smaller world, stronger depth
            case 1: AdjustWorldScale(1.0f / step); break;  // bigger world, weaker depth
            case 2: AdjustIpdScale(step);          break;
            case 3: AdjustIpdScale(1.0f / step);   break;
            case 4: ResetTuning();                 break;
            case 5: VR().RecentreOffset();         break;
            }
        }
        g_tuneWasDown[i] = down;
    }
}

bool TraceActive() {
    const auto& c = Cfg();
    if (c.traceFrames <= 0) return false;
    if (g_frameIndex < g_traceUntilFrame) return true;
    if (c.traceStartFrame > 0) {
        const unsigned start = static_cast<unsigned>(c.traceStartFrame);
        return g_frameIndex >= start &&
               g_frameIndex < start + static_cast<unsigned>(c.traceFrames);
    }
    return false;
}

void FlushTraceRun() {
    if (g_run.world == 0 && g_run.flat == 0) return;
    LogF("  target color=%-3d depth=%-3d layer=%-2d glFBO=%-3d size=%dx%d "
         "| draws world=%-4d flat=%-4d",
         g_run.colorId, g_run.depthId, g_run.colorIndex, g_run.fbo,
         TargetWidth(), TargetHeight(), g_run.world, g_run.flat);
    ++g_traceEmitted;
    g_run.world = g_run.flat = 0;
}

// Called from validate_draw, before every draw.
void TraceDraw(bool worldPass) {
    if (!g_tracing) return;

    const OglRenderTarget& rt = Rt();
    if (rt.color_id != g_run.colorId || rt.depth_id != g_run.depthId ||
        rt.color_index != g_run.colorIndex) {
        FlushTraceRun();
        GLint fbo = -1;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        g_run.colorId    = rt.color_id;
        g_run.depthId    = rt.depth_id;
        g_run.colorIndex = rt.color_index;
        g_run.fbo        = fbo;
    }
    if (worldPass) { ++g_run.world; ++g_frameWorld; }
    else           { ++g_run.flat;  ++g_frameFlat;  }
}

// Saved copies of the engine's own matrices, restored after each upload so the
// engine never sees our per-eye substitution.
mat4 g_savedProj{};
mat4 g_savedView{};
mat4 g_savedModel{};
mat4 g_savedHud{};
mat4 g_savedOrtho3D{};
bool g_modelPatched = false;

bool VrLive() {
    if (!Cfg().enabled || !VR().active() || !g_ready) return false;
    // Mono owns no render target, so it has no stereo objects to require.
    return Cfg().monoTracking || Stereo().valid();
}

// DrawSkyHD is on the stack and the sky-at-infinity path is enabled. Those
// draws keep the rotation of the eye transform (looking around still turns
// the sky) and drop its translation (IPD + 6DOF), so the dome fuses at
// optical infinity instead of at its mesh radius.
bool SkyInfinity() {
    return Cfg().skyAtInfinity && SkyPassActive();
}

// The engine draws its post-processing chain into its own offscreen array
// textures before compositing to the backbuffer. Only the backbuffer-targeted
// work should be split per eye; an offscreen pass wants its own full viewport.
//
// ogl_rt is the render-target latch ogl_setRenderTarget writes; colour and
// depth both zero is the "back to the default framebuffer" case.
// vid_setPass assigns vid_state.shader before its own range check, so anything
// indexing shaders[] has to clamp first -- the array is only 0xCA entries.
bool vs_shader_in_range(int shader) {
    return shader >= 0 && shader <= 0xC9;
}

// True while the video pass is being captured offscreen, so the per-eye
// machinery stands aside and lets it render plainly.
bool g_inVideoCapture = false;

// A pass with no uProjMatrix at all writes clip space directly and cannot be
// moved by any uniform. That is the video path.
// Is the offscreen video panel actually going to handle this frame's bypass
// passes? Both the capture path and the viewport-shift fallback must agree: if
// they disagree, a bypass pass gets NEITHER, which is head-locked and unfused.
// That is exactly what VideoSkipGame6 caused in TR6 -- capture correctly
// skipped, fallback still suppressed by "the panel exists".
// Frame on which fmvShow was last called. The engine calls it once per frame
// for as long as a video is on screen.
unsigned g_fmvFrame = 0;

bool FmvActive() {
    return g_fmvFrame != 0 && (g_frameIndex - g_fmvFrame) <= 2;
}

bool VideoOffscreenActive() {
    if (!Cfg().videoOffscreen || !Video().valid()) return false;

    // TR6's scene composite goes through a clip-space-direct shader, the same
    // signature as an FMV quad, so the shader test alone would capture the whole
    // game. Require an actual video there. TR4/5 keep the original behaviour,
    // which is working.
    if (CurrentGame() == 2 && Cfg().videoSkipGame6 && !FmvActive()) return false;

    return true;
}

bool IsBypassPass() {
    const int sid = VidState().shader;
    return vs_shader_in_range(sid) && Shaders()[sid].uid[0] < 0;
}

// Model-view-projection for the video quad in one eye: the unit quad is scaled
// to the panel, placed at videoDepthMetres in the game camera's frame, and then
// put through the real per-eye transform and projection.
void BuildVideoMvp(int eye, mat4& out) {
    const Eye e = (eye == 0) ? Eye::Left : Eye::Right;
    const float Z = Cfg().videoDepthMetres * LiveWorldUnitsPerMetre();

    float zn = 16.0f, zf = 32768.0f;
    ExtractNearFar(Proj()[1], zn, zf);
    if (Z <= zn) zn = Z * 0.5f;
    if (Z >= zf) zf = Z * 2.0f;

    mat4 P{};
    VR().EyeProjection(e, zn, zf, P);

    const float halfFov = Cfg().videoSizeDegrees * 0.5f * 3.14159265358979f / 180.0f;
    const float W = Z * std::tan(halfFov);
    const int   sw = ScreenWidth();
    const int   sh = ScreenHeight();
    const float aspect = (sh > 0) ? (float)sw / (float)sh : 1.7778f;
    const float H = (aspect > 0.0f) ? W / aspect : W;

    // Unit quad -> panel. Y negated for the same reason as the HUD: the per-eye
    // projection is built for Y-down input (see Config::hudFlipY).
    mat4 L{};
    L.m[0]  = W;
    L.m[5]  = Cfg().hudFlipY ? -H : H;
    L.m[10] = 1.0f;
    L.m[14] = -Z;
    L.m[15] = 1.0f;

    out = Mul4(P, Mul4(AffineToMat4(VR().EyeView(e)), L));
}

bool TargetIsBackbuffer() {
    const OglRenderTarget& rt = Rt();
    return rt.color_id == 0 && rt.depth_id == 0;
}

// Read uViewMatrix back off the live GL program.
//
// The in-memory readback in the injection proves our write landed in
// mView_packed. It does NOT prove the engine uploaded it: validate_draw only
// uploads when consts has kView set and shaders[].uid[1] >= 0, and everything
// downstream of that is invisible from the CPU side. Asking the GPU what it
// actually holds is the only way to tell a failed upload from a failed
// viewport, target or submit -- which is the whole remaining question when the
// eyes are provably different in memory and still identical on screen.
//
// uViewMatrix is declared `uniform vec4 uViewMatrix[4]` and uploaded with
// glUniform4fv(loc, 4, ...), so each array element carries its own location.
// The translation lives in the .w of the first three rows; that is the part
// that differs between the eyes and the only part worth reading.
void ReadBackViewTranslation(uint32_t program, int loc, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!gl::GetUniformfv || program == 0 || loc < 0) return;
    for (int row = 0; row < 3; ++row) {
        float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        gl::GetUniformfv(program, loc + row, v);
        out[row] = v[3];
    }
}

// uProjMatrix is a real mat4, so one location returns all 16 floats. In
// EyeOffsetMode 3 the per-eye transform lives here rather than in the view
// matrix, so this is what has to be verified.
void ReadBackProjTranslation(uint32_t program, int loc, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!gl::GetUniformfv || program == 0 || loc < 0) return;
    float m[16] = {};
    gl::GetUniformfv(program, loc, m);
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
}

// ---------------------------------------------------------------------------
// vid_setPass -- pass classification
// ---------------------------------------------------------------------------
void __cdecl Detour_vid_setPass(int shader, float* params, int cull, int blend) {
    if (Cfg().logCallsites) NoteCallsite("vid_setPass", _ReturnAddress());

    // The engine has a real robustness gap here: vid_setPass assigns
    // vid_state.shader = shader BEFORE its range check, and a shader id above
    // 201 then skips all configuration and falls through, leaving validate_draw
    // to index shaders[] out of bounds. We do not synthesise pass ids, but
    // since we are already in the path it costs nothing to notice.
    if (shader < 0 || shader > 0xC9) {
        LogF("setPass: out-of-range shader id %d (engine would index shaders[] OOB)", shader);
    }

    g_hSetPass.Original<Fn_vid_setPass>()(shader, params, cull, blend);

    // After the original runs, vid_state.proj points at mProj[0] for the 2D
    // ortho layer (logos, menus, subtitles, fades, the title art in cases
    // 0x3B-0x3D) and at mProj[1] for world-space 3D.
    //
    // Some cases never assign proj at all (0x3F, 0x40, 0x48, 0xC0, 0xC1,
    // 0xC4-0xC7) and inherit whatever the previous pass left, so reading the
    // pointer after the call is the only correct way to classify -- a static
    // table of shader ids would get the sticky ones wrong.
    g_worldPass = IsWorldPass();
}

// ---------------------------------------------------------------------------
// validate_draw -- per-eye matrix injection
// ---------------------------------------------------------------------------
// Sample one ordinary world draw per tracked pose. This is CPU-only: no GPU
// readback/stall, no camera correction and no filtering of headset movement.
void TraceFirstPersonView(const Affine& view,int shader,int eye) {
    struct Trace {
        bool valid=false;
        uint64_t serial=0,time=0,report=0;
        Affine view{},head{};
        float native[3][3]{};
        float headStep=0,viewStep=0,nativeStep=0;
        uint64_t frameMs=0;
    };
    static Trace trace;
    if(!Cfg().firstPersonDriftLog || !FirstPersonActive()) { trace={};return; }
    if(eye || SkyInfinity() || FirstPersonDrawingTrackedHands() ||
       (trace.valid && trace.serial==VR().PoseSerial())) return;
    vr::HmdMatrix34_t raw{};float native[3][3]{},position[3]{};
    if(!VR().HeadPose(raw) || !CameraViewFrame(native,position)) return;
    Affine head{};std::memcpy(head.r,raw.m,sizeof(head.r));
    const auto step=[](const auto& a,const auto& b) {
        float dot=0;
        for(int row=0;row<3;++row) for(int col=0;col<3;++col) dot+=a[row][col]*b[row][col];
        return std::acos(std::clamp((dot-1)*.5f,-1.f,1.f))*57.2957795f;
    };
    const auto now=GetTickCount64();
    if(trace.valid) {
        trace.headStep=std::max(trace.headStep,step(head.r,trace.head.r));
        trace.viewStep=std::max(trace.viewStep,step(view.r,trace.view.r));
        trace.nativeStep=std::max(trace.nativeStep,step(native,trace.native));
        trace.frameMs=std::max(trace.frameMs,now-trace.time);
        if(now-trace.report>=250) {
            LogF("fp-view: t=%llu pose=%llu shader=%d max-frame-ms=%llu max-rotation-deg hmd=%.2f native=%.2f draw=%.2f native-yaw=%.2f draw-forward=(%.3f,%.3f,%.3f) hmd-forward=(%.3f,%.3f,%.3f)",
                 now,VR().PoseSerial(),shader,trace.frameMs,trace.headStep,trace.nativeStep,trace.viewStep,
                 std::atan2(native[2][0],native[2][2])*57.2957795f,
                 view.r[2][0],view.r[2][1],view.r[2][2],raw.m[0][2],raw.m[1][2],raw.m[2][2]);
            trace.report=now;trace.headStep=trace.viewStep=trace.nativeStep=0;trace.frameMs=0;
        }
    } else trace.report=now;
    trace.valid=true;trace.serial=VR().PoseSerial();trace.time=now;trace.view=view;trace.head=head;
    std::memcpy(trace.native,native,sizeof(native));
}

void __cdecl Detour_validate_draw() {
    if (HKScopeCapturing()) {
        auto& state=VidState();
        if (state.proj) {
            const mat4 saved=*state.proj;
            HKScopeProjection(*state.proj,SkyInfinity());
            state.consts|=kProj|kView;
            g_hValidateDraw.Original<Fn_validate_draw>()();
            *state.proj=saved;
            state.consts|=kProj|kView;
        }
        HKScopeCaptureViewport();
        return;
    }
    // Mono redirects no targets and resizes no viewport, so it can and should
    // inject on every world-space pass -- including ones the engine renders
    // offscreen before compositing. Restricting mono to backbuffer-targeted
    // passes would silently produce no head-tracking at all if the scene is
    // drawn offscreen first, which is exactly the ambiguity this mode exists
    // to remove.
    // Read the world/2D classification LIVE, from vid_state.proj, rather than
    // trusting the flag cached by the vid_setPass hook.
    //
    // vid_setPass is called once to configure a pass and then many draws follow,
    // and anything that repoints vid_state.proj in between (vid_setOrtho3D does
    // exactly that) leaves the cached flag stale. The pointer in vid_state at
    // the moment of the draw is the actual truth about which matrix is about to
    // be uploaded, so use that.
    DumpDrawState();

    // vid_state.joints is live here and nowhere else -- this is the only point
    // in the frame where the joint array the draw is about to upload can be
    // read or changed. Self-gated: both do nothing unless DrawLaraHD is on the
    // stack, and Apply additionally needs DynamicBonesApply=1.
    DynamicBonesObserveDraw();
    DynamicBonesApplyToDraw();

    // Apply edits the engine's own joint array, so the edit has to come back
    // out once validate_draw has uploaded it -- leaving it in place would hand
    // the next object a corrupted TORSO matrix. validate_draw has five
    // returns; a scope guard covers all of them, including the early ones,
    // without each having to remember.
    //
    // The same guard uploads the per-vertex chest offset. By the time it runs
    // the original has bound this draw's program and ogl_draw has bound its VAO,
    // and glDrawElements has not happened yet -- the one window where both the
    // uniform and the vertex layout are live.
    struct JointPatchGuard {
        ~JointPatchGuard() {
            DynamicBonesRestoreDraw();
            BoneSkinAfterValidate();
        }
    } jointPatchGuard;

    const bool worldPass = IsWorldPass();
    if (worldPass != g_worldPass) ++g_classifyMismatch;
    NoteProjPointer(VidState().proj);

    TraceDraw(worldPass);

    // ...and the pointer is still not the whole truth. vid_setOrtho3D copies
    // mProj[0] -- the ORTHO matrix -- into mProj[1] and repoints vid_state.proj
    // at it, so a pass that is orthographic arrives here indistinguishable from
    // world space by pointer alone, and handing it a per-eye perspective
    // frustum would divide an ortho layout by a depth it was never built for.
    // So classify by CONTENT as well: e32/e33 tell the two projections apart.
    //
    // Unverified. This was written for the stacked inventory and was WRONG
    // about it -- the inventory is a perspective pass placed by
    // vid_setPerspOffset (see preserveProjOffset). Measured over a TR4 and a
    // TR5 session, ortho3D never fired once. It is kept because an ortho matrix
    // must not be replaced by a perspective frustum whatever draws it, and the
    // ortho3D= counter in the health report reports honestly if it ever does.
    const mat4* liveProj = VidState().proj;
    const bool ortho3D = Cfg().ortho3D && worldPass && liveProj
                      && IsOrthoProjection(*liveProj);

    if (ortho3D) {
        ++g_ortho3DDraws;
        if (!g_loggedOrtho3D) {
            g_loggedOrtho3D = true;
            LogF("ortho3D: shader %d has an ORTHO matrix in mProj[1] "
                 "(vid_setOrtho3D). Keeping the engine's projection; %s.",
                 VidState().shader,
                 Cfg().ortho3DDepthMetres <= 0.0f ? "no convergence shift"
                 : (Cfg().ortho3DLockToHead ? "head-locked convergence"
                                            : "world-locked panel"));
        }
    }

    const bool inject = VrLive()
                     && VR().poseValid()
                     && worldPass
                     && !ortho3D
                     && !g_inTr6ShadowDepthPass
                     && !FirstPersonShadowPass()
                     && (Cfg().monoTracking || TargetIsBackbuffer()
                         || AlternateEyeActive()
                         || (NativeTr6Active() && g_inNativeTr6Scene));

    // Count every world-space draw, injected or not, and separately the ones
    // rejected purely because the engine was drawing offscreen. Those two
    // numbers are what the health report needs to say something falsifiable.
    if (worldPass && !ortho3D) {
        ++g_worldDraws;
        if (!TargetIsBackbuffer()) ++g_offscreenWorld;
        if (!inject && VrLive() && VR().poseValid()
            && !Cfg().monoTracking && !TargetIsBackbuffer()) {
            ++g_worldOffscreen;
        }
    }

    // Full-screen passes that bypass uProjMatrix entirely: uid[0] < 0 means the
    // program has no such uniform, so the shader writes clip space directly.
    // Pre-rendered video runs through here. Nothing we can put in a matrix will
    // move it, so shift the VIEWPORT for this draw instead -- glDrawElements
    // runs right after validate_draw returns, and the next draw's
    // SetEyeViewport puts it back.
    if (VrLive() && !Cfg().monoTracking && VR().poseValid()
        && Cfg().videoDepthMetres > 0.0f && TargetIsBackbuffer()
        && Stereo().valid() && g_inDuplicate && !g_inVideoCapture
        && !VideoOffscreenActive()
        && IsBypassPass()) {

        const int sid = VidState().shader;
        for (auto& e : g_bypassSeen) {
            if (e == sid) break;
            if (e == -1) {
                e = sid;
                LogF("bypass: shader %d (0x%02X) has no uProjMatrix -- writes clip "
                     "space directly. Shifting its viewport per eye.", sid, sid);
                break;
            }
        }

        const Eye be = (g_currentEye == 0) ? Eye::Left : Eye::Right;
        const GLsizei ew = static_cast<GLsizei>(Stereo().eyeWidth());
        const GLsizei eh = static_cast<GLsizei>(Stereo().eyeHeight());
        const GLint   x0 = (g_currentEye == 0) ? 0 : static_cast<GLint>(ew);

        bool  fitted = false;
        GLint vx = x0, vy = 0;
        GLsizei vw = ew, vh = eh;

        if (!Cfg().videoLockToHead) {
            // World-locked: project the panel's four CORNERS and fit the
            // viewport to their bounding box. The quad fills whatever viewport
            // it is given, so this lands it where real geometry would --
            // position AND perspective size. Projecting only the centre gives
            // translation alone, which is what made it stretch off-axis.
            const float Z = Cfg().videoDepthMetres * LiveWorldUnitsPerMetre();
            float zn = 16.0f, zf = 32768.0f;
            ExtractNearFar(Proj()[1], zn, zf);
            if (Z <= zn) zn = Z * 0.5f;
            if (Z >= zf) zf = Z * 2.0f;

            mat4 P{};
            VR().EyeProjection(be, zn, zf, P);
            const Affine E = VR().EyeView(be);

            const float halfFov = Cfg().videoSizeDegrees * 0.5f
                                * 3.14159265358979f / 180.0f;
            const float W = Z * std::tan(halfFov);
            const int   sw = ScreenWidth();
            const int   sh = ScreenHeight();
            const float aspect = (sh > 0) ? (float)sw / (float)sh : 1.7778f;
            const float H = (aspect > 0.0f) ? W / aspect : W;

            // Engine camera looks down -Z (ogl_setPerspAngles writes e32 = -1).
            const float corner[4][3] = {
                { -W, -H, -Z }, { +W, -H, -Z }, { -W, +H, -Z }, { +W, +H, -Z }
            };
            float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
            bool ok = true;
            for (int k = 0; k < 4 && ok; ++k) {
                float ce[3];
                for (int i = 0; i < 3; ++i) {
                    ce[i] = E.r[i][0] * corner[k][0] + E.r[i][1] * corner[k][1]
                          + E.r[i][2] * corner[k][2] + E.r[i][3];
                }
                float clip[4];
                for (int r = 0; r < 4; ++r) {
                    clip[r] = P.m[0 * 4 + r] * ce[0] + P.m[1 * 4 + r] * ce[1]
                            + P.m[2 * 4 + r] * ce[2] + P.m[3 * 4 + r];
                }
                if (clip[3] <= 1e-6f) { ok = false; break; }   // behind the eye
                const float nx = clip[0] / clip[3];
                const float ny = clip[1] / clip[3];
                if (nx < xmin) xmin = nx;
                if (nx > xmax) xmax = nx;
                if (ny < ymin) ymin = ny;
                if (ny > ymax) ymax = ny;
            }

            if (ok && xmax > xmin && ymax > ymin) {
                const float pxMin = (xmin + 1.0f) * 0.5f * (float)ew;
                const float pxMax = (xmax + 1.0f) * 0.5f * (float)ew;
                const float pyMin = (ymin + 1.0f) * 0.5f * (float)eh;
                const float pyMax = (ymax + 1.0f) * 0.5f * (float)eh;
                vx = x0 + (GLint)pxMin;
                vy = (GLint)pyMin;
                vw = (GLsizei)(pxMax - pxMin);
                vh = (GLsizei)(pyMax - pyMin);
                if (vw > 0 && vh > 0) fitted = true;
            }
        }

        if (!fitted) {
            // Head-locked, or the panel is behind the eye: constant alignment.
            const float ndcX = VR().HudNdcShiftX(be, Cfg().videoDepthMetres);
            vx = x0 + (GLint)(ndcX * (float)ew * 0.5f);
            vy = 0; vw = ew; vh = eh;
        }

        glViewport(vx, vy, vw, vh);
        // Keep the scissor on this eye's half so the shifted quad cannot bleed
        // into the other eye. BeginFrame disables scissor for its full clear, so
        // it has to be enabled explicitly here.
        glScissor(x0, 0, ew, eh);
        glEnable(GL_SCISSOR_TEST);
    }

    // The ortho-3D layer (the inventory), left with the engine's own ortho
    // projection by the gate above. It still needs stereo -- an ortho matrix is
    // identical in both eyes, so without help it double-visions exactly like an
    // untouched HUD -- but it must NOT go through the HUD's panel, which
    // discards the z input. These are 3D meshes with depth of their own, and
    // flattening them onto one plane leaves every item z-fighting itself.
    if (ortho3D && VrLive() && !Cfg().monoTracking && VR().poseValid()
        && Cfg().ortho3DDepthMetres > 0.0f && TargetIsBackbuffer()) {
        mat4* op = VidState().proj;
        if (op) {
            const Eye oe = (g_currentEye == 0) ? Eye::Left : Eye::Right;
            std::memcpy(&g_savedOrtho3D, op, sizeof(mat4));

            const bool spatialInventory=WorldInventoryActive() && !InventorySceneDrawing();
            if (Cfg().ortho3DLockToHead && !spatialInventory) {
                // Ortho output has w == 1 and no perspective divide, so moving
                // m[12] is a pure convergence shift: it changes where the layer
                // fuses and nothing else. Every relative x, y and z survives it
                // untouched, which is the whole reason this is the default here
                // when it is not for the HUD.
                op->m[12] += VR().HudNdcShiftX(oe, Cfg().ortho3DDepthMetres);
            } else {
                // World-locked, built like the HUD's panel:
                //
                //   Q = P_persp * E * L * P_o
                //
                // with one difference that matters. The HUD's L zeroes its z
                // column; this one sets it, so ortho NDC z in [-1, 1] maps to
                // camera z of -(Zc -/+ S) instead of collapsing onto -Zc. That
                // is what keeps each mesh's own depth ordering.
                const float scale = LiveWorldUnitsPerMetre();
                const float Zc = Cfg().ortho3DDepthMetres * scale;
                const float S  = (Cfg().ortho3DSlabMetres > 0.0f
                                  ? Cfg().ortho3DSlabMetres : 0.5f)
                               * 0.5f * scale;
                const float halfFov = Cfg().ortho3DSizeDegrees * 0.5f
                                    * 3.14159265358979f / 180.0f;
                const float W = Zc * std::tan(halfFov);
                const int   sw = ScreenWidth();
                const int   sh = ScreenHeight();
                const float aspect = (sh > 0) ? (float)sw / (float)sh : 1.7778f;
                const float H = (aspect > 0.0f) ? W / aspect : W;

                // Bracket the slab tightly rather than reusing the scene's
                // near/far. mProj[1] holds the ortho matrix right now -- that
                // is what got us here -- so there is no scene near/far left to
                // read, and a tight bracket is what gives this thin layer the
                // depth resolution not to z-fight.
                float zn = Zc - S * 1.5f;
                float zf = Zc + S * 1.5f;
                if (zn < 0.01f * scale) zn = 0.01f * scale;
                if (zf <= zn) zf = zn * 2.0f;

                mat4 Ppersp{};
                VR().EyeProjection(oe, zn, zf, Ppersp);

                mat4 L{};
                L.m[0]  = W;
                L.m[5]  = Cfg().hudFlipY ? -H : H;   // same double flip as the HUD
                L.m[10] = -S;                        // ortho z -> slab depth
                L.m[14] = -Zc;
                L.m[15] = 1.0f;

                const mat4 E = AffineToMat4(spatialInventory
                    ? WorldInventoryEyeView(oe) : VR().EyeView(oe));
                *op = Mul4(Ppersp, Mul4(E, Mul4(L, g_savedOrtho3D)));
            }

            VidState().consts |= kProj;
            g_hValidateDraw.Original<Fn_validate_draw>()();
            std::memcpy(op, &g_savedOrtho3D, sizeof(mat4));
            return;
        }
    }

    // The flat 2D layer (HUD, menus, subtitles, fades) is drawn with the
    // engine's ortho projection, identical in both eyes. The headset optics
    // apply a fixed per-eye correction assuming an asymmetric render, so an
    // unshifted image is pulled apart and cannot fuse -- which is the double
    // vision on menus. Give it the same shear the 3D layer gets, plus enough
    // convergence to sit at HudDepthMetres.
    if (!inject && !worldPass && VrLive() && !Cfg().monoTracking
        && VR().poseValid() && Cfg().hudDepthMetres > 0.0f
        && TargetIsBackbuffer()) {
        mat4* hudProj = VidState().proj;
        if (hudProj) {
            const Eye hudEye = (g_currentEye == 0) ? Eye::Left : Eye::Right;
            std::memcpy(&g_savedHud, hudProj, sizeof(mat4));

            const bool spatialInventory=WorldInventoryActive() && !InventorySceneDrawing();
            if (Cfg().hudLockToHead && !spatialInventory) {
                // Screen-locked: a flat per-eye NDC shift. Ortho output has
                // w == 1, so ndc.x moves with m[12] directly.
                hudProj->m[12] += VR().HudNdcShiftX(hudEye, Cfg().hudDepthMetres);
            } else {
                // World-locked: turn the 2D layer into a quad sitting in the
                // GAME camera's frame, then look at it through the per-eye
                // transform. Head rotation lands in E, so the panel stays put
                // while you look around it.
                //
                //   Q = P_persp * E * L * P_o
                //
                // L maps ortho NDC (x, y, *, 1) to camera space
                // (W*x, H*y, -Z, 1): the z input is deliberately discarded, so
                // every 2D element lands on the one plane.
                const float scale = LiveWorldUnitsPerMetre();
                const float Z = Cfg().hudDepthMetres * scale;
                const float halfFov = Cfg().hudSizeDegrees * 0.5f
                                    * 3.14159265358979f / 180.0f;
                const float W = Z * std::tan(halfFov);
                const int   sw = ScreenWidth();
                const int   sh = ScreenHeight();
                const float aspect = (sh > 0) ? (float)sw / (float)sh : 1.7778f;
                const float H = (aspect > 0.0f) ? W / aspect : W;

                float zn = 16.0f, zf = 32768.0f;
                ExtractNearFar(Proj()[1], zn, zf);
                if (Z <= zn) zn = Z * 0.5f;
                if (Z >= zf) zf = Z * 2.0f;

                mat4 Ppersp{};
                VR().EyeProjection(hudEye, zn, zf, Ppersp);

                mat4 L{};
                L.m[0]  = W;
                // Y-down, to match what the per-eye projection expects. See
                // Config::hudFlipY -- getting this wrong both inverts the image
                // and reverses winding, so culling removes the whole layer.
                L.m[5]  = Cfg().hudFlipY ? -H : H;
                L.m[14] = -Z;
                L.m[15] = 1.0f;

                const mat4 E = AffineToMat4(spatialInventory
                    ? WorldInventoryEyeView(hudEye) : VR().EyeView(hudEye));
                *hudProj = Mul4(Ppersp, Mul4(E, Mul4(L, g_savedHud)));
            }

            VidState().consts |= kProj;
            g_hValidateDraw.Original<Fn_validate_draw>()();
            std::memcpy(hudProj, &g_savedHud, sizeof(mat4));
            return;
        }
    }

    if (!inject) {
        g_hValidateDraw.Original<Fn_validate_draw>()();
        return;
    }

    RenderState& vs = VidState();

    // vid_state.proj should be &mProj[1] here (that is what g_worldPass means)
    // and vid_state.view should be &mView_packed, but both are pointers the
    // engine is free to repoint, so work through them rather than assuming.
    mat4* livePr = vs.proj;
    mat4* liveVw = vs.view;
    if (!livePr || !liveVw) {
        g_hValidateDraw.Original<Fn_validate_draw>()();
        return;
    }

    // Mono keeps the engine's own projection untouched: field of view stays
    // exactly as the game intended, so anything that looks wrong on screen is
    // the view maths and nothing else.
    const bool doProj = !Cfg().monoTracking && Cfg().perEyeProjection != 0;

    // Snapshot, substitute, upload, restore.
    std::memcpy(&g_savedView, liveVw, sizeof(mat4));
    if (doProj || Cfg().eyeOffsetMode >= 2 || Cfg().preserveProjOffset) {
        std::memcpy(&g_savedProj, livePr, sizeof(mat4));
    }

    const Eye eye = (g_currentEye == 0) ? Eye::Left : Eye::Right;
    const bool skyInfinity = SkyInfinity();

    // Every use of the eye transform below reads this instead of calling
    // VR().EyeView(eye) again. For a sky draw its translation -- IPD plus any
    // 6DOF head offset -- is zeroed: that translation is exactly what gives the
    // dome a finite stereo depth, and dropping it here reaches every mode below
    // (view matrix, model-matrix redistribution, or projection) uniformly. The
    // rotation stays, so head-look still turns the sky.
    Affine eyeXform = WorldInventoryActive() && !InventorySceneDrawing()
        ? WorldInventoryEyeView(eye) : VR().EyeView(eye);
    if (skyInfinity) {
        eyeXform.r[0][3] = eyeXform.r[1][3] = eyeXform.r[2][3] = 0.0f;
        ++g_skyDraws;
        if (!g_loggedSky) {
            g_loggedSky = true;
            LogF("sky: first DrawSkyHD draw (shader %d) -- rotation-only eye "
                 "transform, far-plane depth", vs.shader);
        }
    }

    mat4 eyeProj{};
    if (doProj) {
        // Preserve the engine's own near/far -- vid_setPass picks them per pass
        // and clobbering them would break fog, depth precision and the shadow
        // chain.
        float zn = 0.0f, zf = 0.0f;
        if (!ExtractNearFar(*livePr, zn, zf)) {
            zn = 16.0f;
            zf = 32768.0f;
        }
        if (Cfg().nearClip > 0.0f) zn = Cfg().nearClip;
        if (Cfg().farClip > 0.0f) {
            zf = Cfg().farClip;
        } else if (CurrentGame() == 2 && g_inNativeTr6Scene
                   && Cfg().portalCulling) {
            // Match the conservative TR6 CPU culling projection below. This
            // is deliberately automatic only when FarClip is unset; an
            // explicit user value remains authoritative.
            if (zf < kTr6VrFarPlane) zf = kTr6VrFarPlane;
            if (!g_loggedTr6FarPlane) {
                g_loggedTr6FarPlane = true;
                LogF("tr6 cull: native-stereo far plane extended to %.0f "
                     "world units", zf);
            }
        }

        VR().EyeProjection(eye, zn, zf, eyeProj);
        std::memcpy(livePr, &eyeProj, sizeof(mat4));
    }

    // finalView = eyeView * gameView, both as row-major 3x4 affines.
    //
    // With PerEyeView off the game's own view is used unchanged, so the only
    // remaining per-eye difference is the frustum shear. If the two halves
    // still differ in that configuration, the difference was never parallax.
    const Affine gameView  = ReadPackedView(g_savedView);
    TraceFirstPersonView(gameView,vs.shader,g_currentEye);
    Affine finalView = Cfg().perEyeView
                     ? Mul(eyeXform, gameView)
                     : gameView;

    // Diagnostic: swing the right eye's view by a large, unmistakable angle.
    if (Cfg().debugEyeYawDegrees != 0.0f && eye == Eye::Right) {
        finalView = Mul(RotY(Cfg().debugEyeYawDegrees), finalView);
    }

    // Move the per-eye TRANSLATION out of the view matrix and into the model
    // matrix, where both shader families read it. The rotation stays in the
    // view matrix -- rotation-only shaders honour that, as the yaw test showed.
    //
    // A shift d in view space is a shift R^T * d in the model matrix's space,
    // because those shaders compute R * p and we need R * (p + R^T d) = R p + d.
    g_modelPatched = false;
    float dView[3] = { 0.0f, 0.0f, 0.0f };
    if (Cfg().eyeOffsetMode != 0 && Cfg().eyeOffsetMode != 3) {
        // The view-space translation the eye/head offset introduces.
        dView[0] = finalView.r[0][3] - gameView.r[0][3];
        dView[1] = finalView.r[1][3] - gameView.r[1][3];
        dView[2] = finalView.r[2][3] - gameView.r[2][3];

        // Leave the view matrix's translation exactly as the game set it; the
        // rotation stays, because rotation-only shaders do honour that.
        finalView.r[0][3] = gameView.r[0][3];
        finalView.r[1][3] = gameView.r[1][3];
        finalView.r[2][3] = gameView.r[2][3];
    }

    if (Cfg().eyeOffsetMode == 1) {
        mat4* liveMd = vs.model;
        if (liveMd) {
            float w[3];
            for (int i = 0; i < 3; ++i) {
                w[i] = gameView.r[0][i] * dView[0]
                     + gameView.r[1][i] * dView[1]
                     + gameView.r[2][i] * dView[2];
            }
            std::memcpy(&g_savedModel, liveMd, sizeof(mat4));
            liveMd->m[3]  += w[0];
            liveMd->m[7]  += w[1];
            liveMd->m[11] += w[2];
            vs.consts |= kModel;
            g_modelPatched = true;
        }
    }

    // Mode 3 leaves the view matrix exactly as the engine set it. That is the
    // whole point: untouched, both shader families agree on the view-space
    // position, and the per-eye transform goes into the projection below.
    if (Cfg().eyeOffsetMode != 3) {
        WritePackedView(*liveVw, finalView);
    }

    // Mode 2: fold the view-space offset into the projection as P * translate(d).
    //
    // Every shader ends with uProjMatrix * vec4(viewpos, 1.0), so this reaches
    // paths the view and model matrices cannot -- skinned geometry through
    // uJoints included. Applied to whatever is already in livePr, which is the
    // per-eye projection when doProj is on and the engine's own when it is not.
    if (Cfg().eyeOffsetMode == 2 &&
        (dView[0] != 0.0f || dView[1] != 0.0f || dView[2] != 0.0f)) {
        mat4& P = *livePr;
        for (int row = 0; row < 4; ++row) {
            P.m[12 + row] += P.m[0 + row] * dView[0]
                           + P.m[4 + row] * dView[1]
                           + P.m[8 + row] * dView[2];
        }
        vs.consts |= kProj;
    }

    // Mode 3: livePr <- livePr * E, with E the eye-from-game-camera transform.
    // Column-major, so (P*E)[c][r] = sum_k P[k][r] * E[c][k]; E's bottom row is
    // (0,0,0,1), which is why column 3 picks up P's own column 3.
    //
    // For the sky, E's translation has already been zeroed above, so this is
    // P * R -- head rotation with no IPD. The per-eye frustum shear still
    // lives in P, which is the HMD optical correction, not stereo depth.
    if (Cfg().eyeOffsetMode == 3) {
        const Affine& E = eyeXform;
        const mat4 P = *livePr;
        mat4 out{};
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                float sum = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    sum += P.m[k * 4 + r] * ((c < 3) ? E.r[k][c] : E.r[k][3]);
                }
                if (c == 3) sum += P.m[3 * 4 + r];
                out.m[c * 4 + r] = sum;
            }
        }
        *livePr = out;
        vs.consts |= kProj;
    }

    // Re-apply the engine's own projection OFFSET -- last, and innermost.
    //
    // vid_setPerspOffset writes e02/e12 (m[8]/m[9]) and nothing else, and a
    // projection carrying shear (sx, sy) is EXACTLY the same projection without
    // it, applied to space pre-skewed by
    //
    //     (x, y, z) -> (x + (sx/m00)*z,  y + (sy/m11)*z,  z)
    //
    // -- multiply it out and the two agree term for term. So the engine's
    // placement is not really a property of its projection at all: it is a skew
    // of the engine's own camera space, and reproducing it means post-
    // multiplying whatever projection we have ended up with by that skew.
    // Post-multiplying touches column 2 alone, which is why this is four
    // multiply-adds rather than a matrix product.
    //
    // The first attempt added the shear straight onto the eye frustum instead,
    // and got two things wrong at once:
    //
    //   * Wrong side of the per-eye transform. Every mode ends up evaluating
    //     P * E * v, and the engine's shear must multiply the ENGINE's v.z --
    //     but sitting in P it multiplied (E*v).z instead, so the offset swung
    //     with head orientation and the row sheared as you looked around. Mode
    //     3 makes that worst, since there the whole head transform lives in the
    //     projection. Applied here, after E, the skew reaches v itself.
    //
    //   * Same NDC, not the same ANGLE. A shear of s offsets by s*tan(fov), and
    //     the engine's frustum is not the headset's: measured here, engine
    //     tanX 1.119 vs eye 1.108, but engine tanY 0.629 vs eye 1.197. So the
    //     row landed right (1% apart) while the vertical placement was thrown
    //     nearly twice as far as intended, out into the lens distortion. That
    //     was the fishbowl. Dividing by the engine's own m00/m11 here converts
    //     angle to angle, and the ratio falls out on its own.
    //
    // ogl_setPersp and ogl_setPerspAngles both zero e02/e12 explicitly, so this
    // is inert during gameplay -- only the engine's screen-placed elements, the
    // inventory among them, ever carry a shear.
    if (Cfg().preserveProjOffset
        && (g_savedProj.m[8] != 0.0f || g_savedProj.m[9] != 0.0f)
        && g_savedProj.m[0] != 0.0f && g_savedProj.m[5] != 0.0f) {
        const float k = Cfg().projOffsetScale;
        const float a = (g_savedProj.m[8] / g_savedProj.m[0]) * k;
        const float b = (g_savedProj.m[9] / g_savedProj.m[5]) * k;
        mat4& P = *livePr;
        for (int r = 0; r < 4; ++r) {
            P.m[8 + r] += a * P.m[0 + r] + b * P.m[4 + r];
        }
        vs.consts |= kProj;
        ++g_offsetDraws;
        if (!g_loggedOffset) {
            g_loggedOffset = true;
            LogF("projoffset: shader %d carries vid_setPerspOffset shear "
                 "(%.4f, %.4f) -- the engine is placing this draw through the "
                 "projection. Re-applied as a camera-space skew (%.4f, %.4f).",
                 vs.shader, g_savedProj.m[8], g_savedProj.m[9], a, b);
        }
    }

    // Force the uploads. The engine's own dirty tracking would skip them: the
    // proj bit is set on a POINTER comparison in vid_setPass, so writing new
    // contents into the same mProj[1] would not trip it.
    if (Cfg().eyeOffsetMode != 3) vs.consts |= kView;
    if (doProj) vs.consts |= kProj;

    // Log each eye exactly once, then report the separation between them. If
    // the two translations are identical the eyes are rendering the same image
    // and there is no stereo, no matter what the scale is set to.
    const int eyeIdx = (g_currentEye == 0) ? 0 : 1;
    ++g_injectCount[eyeIdx];
    if (Cfg().verboseFirstFrame && !g_loggedEye[eyeIdx]) {
        g_loggedEye[eyeIdx] = true;
        g_loggedFrame = true;
        if (doProj) {
            LogF("inject: eye=%d proj[0]=%.4f proj[5]=%.4f proj[8]=%.4f proj[9]=%.4f",
                 eyeIdx, eyeProj.m[0], eyeProj.m[5], eyeProj.m[8], eyeProj.m[9]);
        } else {
            Log("inject: MONO -- view only, engine projection untouched");
        }
        if (!Cfg().perEyeView || Cfg().perEyeProjection != 1) {
            static const char* kProjMode[3] = { "OFF (engine projection)",
                                                "HMD asymmetric",
                                                "HMD FOV, SYMMETRIC (no shear)" };
            const int pm = (Cfg().perEyeProjection >= 0 && Cfg().perEyeProjection <= 2)
                         ? Cfg().perEyeProjection : 1;
            LogF("inject: DIAGNOSTIC MODE -- per-eye view=%s projection=%s",
                 Cfg().perEyeView ? "on" : "OFF", kProjMode[pm]);
        }
        if (Cfg().debugEyeYawDegrees != 0.0f) {
            LogF("inject: DEBUG YAW -- right eye view rotated %.1f degrees",
                 Cfg().debugEyeYawDegrees);
        }
        const Affine ev = VR().EyeView(eye);
        LogF("inject: eye=%d eyeView translation = (%+.1f, %+.1f, %+.1f) world units",
             eyeIdx, ev.r[0][3], ev.r[1][3], ev.r[2][3]);
        LogF("inject: eye=%d final view translation = (%.1f, %.1f, %.1f)",
             eyeIdx, finalView.r[0][3], finalView.r[1][3], finalView.r[2][3]);
        for (int k = 0; k < 3; ++k) g_eyeViewT[eyeIdx][k] = finalView.r[k][3];

        // Read back what actually landed in the engine's memory, rather than
        // trusting the Affine we computed. The translation lives at float
        // indices 3, 7 and 11 of the packed view; if those differ between the
        // eyes then our write is correct and any missing parallax is downstream
        // (upload or shader). If they match, the write itself is wrong.
        LogF("inject: eye=%d packed view translation IN MEMORY = (%.1f, %.1f, %.1f) "
             "[floats 3,7,11 at %p]",
             eyeIdx, liveVw->m[3], liveVw->m[7], liveVw->m[11], (void*)liveVw);
        LogF("inject: eye=%d uid[1] (uViewMatrix location) = %d, shader=%d",
             eyeIdx, Shaders()[vs.shader].uid[1], vs.shader);
    }
    if (!g_reportedSeparation && g_loggedEye[0] && g_loggedEye[1]) {
        g_reportedSeparation = true;
        const float dx = g_eyeViewT[0][0] - g_eyeViewT[1][0];
        const float dy = g_eyeViewT[0][1] - g_eyeViewT[1][1];
        const float dz = g_eyeViewT[0][2] - g_eyeViewT[1][2];
        const float sep = std::sqrt(dx*dx + dy*dy + dz*dz);
        LogF("inject: SEPARATION between eyes = %.2f world units (dx=%.2f dy=%.2f dz=%.2f)",
             sep, dx, dy, dz);
        if (sep < 0.01f) {
            Log("inject: *** ZERO SEPARATION -- both eyes are rendering the SAME view. "
                "This is why depth is flat and why world scale changes nothing. ***");
        }
    }

    g_hValidateDraw.Original<Fn_validate_draw>()();

    // The upload has now happened (or been skipped). Ask the GPU what it holds,
    // once per eye per report window. glGetUniformfv forces a pipeline sync, so
    // this must stay rare -- twice per 1800 frames is free.
    // Sky draws deliberately share a translation-free eye transform, so
    // sampling one would report zero separation and look like an injection
    // failure -- wait for a non-sky world draw instead.
    if (g_verifyArmed && !g_verifyDone[eyeIdx] && !skyInfinity) {
        g_verifyDone[eyeIdx] = true;
        const Shader& sh = Shaders()[vs.shader];
        g_gpuProg[eyeIdx] = static_cast<int>(sh.id);
        if (Cfg().eyeOffsetMode == 3) {
            // Mode 3 leaves uViewMatrix identical in both eyes ON PURPOSE, so
            // reading it would report zero separation and look like a failure.
            g_gpuLoc[eyeIdx] = sh.uid[0];
            ReadBackProjTranslation(sh.id, sh.uid[0], g_gpuViewT[eyeIdx]);
        } else {
            g_gpuLoc[eyeIdx] = sh.uid[1];
            ReadBackViewTranslation(sh.id, sh.uid[1], g_gpuViewT[eyeIdx]);
        }
    }

    if (g_modelPatched && vs.model) {
        std::memcpy(vs.model, &g_savedModel, sizeof(mat4));
        g_modelPatched = false;
    }

    // Put the engine's matrices back. It never observes the substitution, so
    // the next pass composes from pristine state rather than compounding.
    if (Cfg().eyeOffsetMode != 3) {
        std::memcpy(liveVw, &g_savedView, sizeof(mat4));
    }
    if (doProj || Cfg().eyeOffsetMode >= 2) {
        std::memcpy(livePr, &g_savedProj, sizeof(mat4));
    }
}

// ---------------------------------------------------------------------------
// ogl_draw / ogl_drawVB -- per-eye duplication
// ---------------------------------------------------------------------------
//
// Every draw is issued twice, into the two halves of the double-wide target.
// Matrix injection is gated on the pass being world-space, so 2D/HUD geometry
// is drawn twice at the same screen position -- a flat overlay locked to both
// eyes, which is what you want before there is a proper world-space HUD.

// Pushes every fragment drawn while it is alive to the far plane, so a sky
// dome of finite mesh radius cannot win a depth test against the world behind
// it. Scoped rather than paired manually because DuplicatePerEye below returns
// from several places, and a depth range left at (1,1) after an early return
// would flatten everything drawn afterwards.
struct ScopedSkyDepth {
    bool active;
    GLfloat saved[2] = { 0.0f, 1.0f };
    explicit ScopedSkyDepth(bool enable) : active(enable) {
        if (active) {
            glGetFloatv(GL_DEPTH_RANGE, saved);
            glDepthRange(1.0, 1.0);
        }
    }
    ~ScopedSkyDepth() {
        if (active) glDepthRange(saved[0], saved[1]);
    }
};

template <typename Fn, typename... Args>
void DuplicatePerEye(const hook::InlineHook& h, Args... args) {
    const ScopedSkyDepth skyDepth(SkyInfinity() && VrLive());

    // App_Render_Scene itself is already being replayed once per eye. Every
    // draw inside it must therefore execute exactly once; duplicating here as
    // well would produce four scene passes and overwrite both eye results.
    if (g_inNativeTr6Scene) {
        h.Original<Fn>()(args...);
        return;
    }

    if (!VrLive() || g_inDuplicate || Cfg().monoTracking
        || !Cfg().duplicateDraws || !TargetIsBackbuffer()) {
        h.Original<Fn>()(args...);
        return;
    }

    // Alternate-eye: draw ONCE. The whole frame is one eye, so there is nothing
    // to duplicate -- only the backbuffer-targeted draws need the eye viewport,
    // and the offscreen passes keep the engine's own full-size viewport.
    if (AlternateEyeActive()) {
        // One draw, and no eye viewport: the whole frame goes into the scratch
        // target at the engine's own size, and present blits it into one half.
        g_currentEye = g_frameEye;
        h.Original<Fn>()(args...);
        return;
    }

    ++g_dupCount;

    // Clip-space-direct passes (video) cannot be moved by any matrix, so
    // capture the draw once offscreen and replay it as real geometry per eye.
    // That is the only way to get correct keystone and roll.
    // TR6 composites its offscreen scene to the backbuffer through a
    // clip-space-direct shader -- the same signature as an FMV quad. Capturing
    // that and replaying it as a 60-degree panel is what turned TR6 into a small
    // floating rectangle. Its FMVs use the viewport-shift path instead, which
    // VideoOffscreenActive() keeps consistent with this decision.
    if (VideoOffscreenActive() && IsBypassPass()) {
        GLint prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

        g_inVideoCapture = true;
        Video().BeginCapture();
        h.Original<Fn>()(args...);
        g_inVideoCapture = false;

        gl::BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);

        g_inDuplicate = true;
        for (int eye = 0; eye < 2; ++eye) {
            g_currentEye = eye;
            Stereo().SetEyeViewport(eye);
            mat4 mvp{};
            BuildVideoMvp(eye, mvp);
            Video().Replay(mvp, Cfg().videoFlipV);
        }
        g_currentEye = 0;
        g_inDuplicate = false;
        return;
    }

    g_inDuplicate = true;
    for (int eye = 0; eye < 2; ++eye) {
        g_currentEye = eye;
        Stereo().SetEyeViewport(eye);
        h.Original<Fn>()(args...);
    }
    g_currentEye = 0;
    g_inDuplicate = false;
    // Render-only third view. Never replay simulation, native optics, HUD,
    // shadow/offscreen targets, or Lara's controller-driven hands.
    if (IsWorldPass() && VidState().proj && !IsOrthoProjection(*VidState().proj) &&
        !IsBypassPass() && HKScopeBeginCapture()) {
        g_inDuplicate=true;
        h.Original<Fn>()(args...);
        g_inDuplicate=false;
        HKScopeEndCapture();
    }
}

void __cdecl Detour_ogl_draw(void* vb, unsigned firstIndex, unsigned count, int strip) {
    DuplicatePerEye<Fn_ogl_draw>(g_hDraw, vb, firstIndex, count, strip);
}

void __cdecl Detour_ogl_drawVB(int fvf, void* vb, void* ib, int stride,
                               unsigned first, unsigned count, int strip) {
    if (Cfg().logCallsites) NoteCallsite("ogl_drawVB", _ReturnAddress());

    DuplicatePerEye<Fn_ogl_drawVB>(g_hDrawVB, fvf, vb, ib, stride, first, count, strip);
}

// ---------------------------------------------------------------------------
// TR6 App_Render_Scene -- full offscreen scene replay per eye
// ---------------------------------------------------------------------------
float __fastcall Detour_Tr6FxCamDist(const float* position) {
    const auto original = g_hTr6FxCamDist.Original<Fn_tr6_fx_cam_dist>();
    const uintptr_t returnAddress =
        reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool fromFxProcessBox = g_tr6ModuleBase
        && returnAddress >= g_tr6ModuleBase + g_tr6->fxProcessBox
        && returnAddress < g_tr6ModuleBase + g_tr6->fxProcessBox
                                            + g_tr6->fxProcessBoxSize;
    if (!NativeTr6Active() || !fromFxProcessBox || !position) {
        return original(position);
    }

    // fxProcessBox normally measures emitter range from gcamCamera.Position.
    // Rotating the third-person camera moves that position around the player,
    // so dust emitters cross their range thresholds even though neither Lara
    // nor the emitter moved. LookAt is the stable gameplay focal point during
    // an orbit and keeps those decisions independent of camera-stick yaw.
    const float* gcam = reinterpret_cast<const float*>(
        g_tr6ModuleBase + g_tr6->gcamCamera);
    const float dx = gcam[4] - position[0];
    const float dy = gcam[5] - position[1];
    const float dz = gcam[6] - position[2];

    if (!g_loggedTr6FxDistance) {
        g_loggedTr6FxDistance = true;
        Log("tr6 effects: fxProcessBox emitter range now uses the stable "
            "camera target instead of the orbiting chase-camera position");
    }
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int32_t __fastcall Detour_Tr6FxBoundsClip(const float* bounds) {
    const auto original =
        g_hTr6FxBoundsClip.Original<Fn_tr6_fx_bounds_clip>();
    const uintptr_t returnAddress =
        reinterpret_cast<uintptr_t>(_ReturnAddress());

    // This exact fxProcessBox call guards fxInsertFXLight. Its frustum is still
    // the third-person camera's, so a lamp pool can disappear while it remains
    // inside the tracked VR view. Other users of mathIsBoundsClipped (including
    // actor simulation) retain the engine's original result.
    if (NativeTr6Active() && g_tr6ModuleBase
        && returnAddress == g_tr6ModuleBase + g_tr6->fxLightBoundsReturn) {
        if (!g_loggedTr6FxBounds) {
            g_loggedTr6FxBounds = true;
            Log("tr6 effects: fxProcessBox local-light frustum reject disabled "
                "for native stereo");
        }
        return 0;
    }
    return original(bounds);
}

int32_t __fastcall Detour_Tr6FxNodeBoundsClip(const float* bounds) {
    const auto original =
        g_hTr6FxNodeBoundsClip.Original<Fn_tr6_fx_bounds_clip>();
    const uintptr_t returnAddress =
        reinterpret_cast<uintptr_t>(_ReturnAddress());

    // mapDrawRoomList converts the sign of this result into bit 0x100 on the
    // FX node. Most fxProcessBox particle/light cases test that bit before any
    // of their type-specific logic and return immediately when it is set.
    if (NativeTr6Active() && g_tr6ModuleBase
        && returnAddress == g_tr6ModuleBase + g_tr6->fxNodeBoundsReturn) {
        if (!g_loggedTr6FxNodeBounds) {
            g_loggedTr6FxNodeBounds = true;
            Log("tr6 effects: primary mapDrawRoomList FX-node off-camera flag "
                "disabled for native stereo");
        }
        return 0;
    }
    return original(bounds);
}

void __fastcall Detour_Tr6EffectsUpdate(const Tr6CameraView* cameraView) {
    const auto original = g_hTr6EffectsUpdate.Original<Fn_tr6_effects_update>();
    if (!NativeTr6Active() || !g_inNativeTr6Scene || !cameraView
        || !g_tr6ModuleBase) {
        original(cameraView);
        return;
    }

    // Particle and light-beam vertices are generated before the main draw.
    // Most builders use the camera matrix in SYS_DRAW_CAMERA_VIEW, while
    // fxParticleAddSquareCamFacing reads gcamCamera.Position/LookAt directly.
    // Feed both paths the same tracked HEAD-CENTRE view. Head centre (rather
    // than either eye) keeps billboard geometry identical in both scene passes;
    // the ordinary draw injection supplies the actual per-eye parallax later.
    alignas(16) Tr6CameraView adjusted;
    std::memcpy(&adjusted, cameraView, sizeof(adjusted));
    adjusted.camera = Mul4(AffineToMat4(VR().HeadView()), adjusted.camera);
    adjusted.cameraProject = Mul4(adjusted.project, adjusted.camera);

    auto* gcam = reinterpret_cast<float*>(
        g_tr6ModuleBase + g_tr6->gcamCamera);
    float savedPositionLookAt[8];
    std::memcpy(savedPositionLookAt, gcam, sizeof(savedPositionLookAt));

    // Invert the rigid view matrix at the origin: position = -R^T*t. The
    // engine camera looks down -Z, so world forward is -R^T*(0,0,1).
    const float position[3] = {
        -(adjusted.camera.m[0] * adjusted.camera.m[12]
          + adjusted.camera.m[1] * adjusted.camera.m[13]
          + adjusted.camera.m[2] * adjusted.camera.m[14]),
        -(adjusted.camera.m[4] * adjusted.camera.m[12]
          + adjusted.camera.m[5] * adjusted.camera.m[13]
          + adjusted.camera.m[6] * adjusted.camera.m[14]),
        -(adjusted.camera.m[8] * adjusted.camera.m[12]
          + adjusted.camera.m[9] * adjusted.camera.m[13]
          + adjusted.camera.m[10] * adjusted.camera.m[14])
    };
    const float forward[3] = {
        -adjusted.camera.m[2],
        -adjusted.camera.m[6],
        -adjusted.camera.m[10]
    };
    for (int axis = 0; axis < 3; ++axis) {
        gcam[axis] = position[axis];
        gcam[4 + axis] = position[axis] + forward[axis] * 1024.0f;
    }

    original(&adjusted);
    std::memcpy(gcam, savedPositionLookAt, sizeof(savedPositionLookAt));

    if (!g_loggedTr6EffectsCamera) {
        g_loggedTr6EffectsCamera = true;
        Log("tr6 effects: particle billboards and light-beam render data now "
            "face the tracked head camera; game camera restored");
    }
}

void __fastcall Detour_Tr6DrawProjectedShadows(void* drawBuffer) {
    const auto original = g_hTr6DrawProjectedShadows
        .Original<Fn_tr6_draw_projected_shadows>();
    if (!NativeTr6Active() || !g_inNativeTr6Scene) {
        original(drawBuffer);
        return;
    }

    // The function switches the renderer to each character shadow's LIGHT
    // camera and draws character depth into the shadow atlas. These matrices
    // must be identical for both scene eyes. The later room projection pass is
    // outside this scope and still receives the correct per-eye scene camera.
    g_inTr6ShadowDepthPass = true;
    original(drawBuffer);
    g_inTr6ShadowDepthPass = false;

    if (!g_loggedTr6ShadowIsolation) {
        g_loggedTr6ShadowIsolation = true;
        Log("tr6 shadow: projected-character light-camera depth pass isolated "
            "from headset view injection");
    }
}

void __cdecl Detour_Tr6RenderScene() {
    const auto original = g_hTr6RenderScene.Original<Fn_tr6_render_scene>();

    // The latch deliberately stays off for menus and FMVs. Re-entry is not
    // expected, but the guard makes it safe to call through if the scene
    // renderer ever invokes itself through a path hidden by the symbols.
    if (!NativeTr6Active() || g_inNativeTr6Scene || !VrLive()
        || !VR().poseValid() || !Stereo().monoValid()) {
        original();
        return;
    }

    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
    g_inNativeTr6Scene = true;

    // TR6 owns several full-size intermediate colour/depth targets. Let it use
    // those normally and replay the entire chain sequentially. The final
    // composite for each pass lands in the single-eye scratch target and is
    // copied out before the next eye overwrites any intermediates.
    for (int eye = 0; eye < 2; ++eye) {
        g_currentEye = eye;
        FboDefault() = Stereo().monoFbo();
        Stereo().BeginMono();
        original();
        Stereo().BlitMonoToHalf(eye);
    }

    // App_Render_Scene has completed, but TR6 may still draw UI before
    // ogl_present. Put the ordinary stereo target back so those draws retain
    // the established per-draw stereo/HUD path.
    FboDefault() = Stereo().fbo();
    gl::BindFramebuffer(GL_FRAMEBUFFER, Stereo().fbo());
    if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST);
    else                   glDisable(GL_SCISSOR_TEST);
    g_currentEye = 0;
    g_inNativeTr6Scene = false;

    if (!g_loggedNativeTr6) {
        g_loggedNativeTr6 = true;
        Log("tr6: native stereo active -- App_Render_Scene and its complete "
            "offscreen/postprocess chain now render once per eye");
    }
}

// Build the conservative frustum used by TR6's render-list builder. Replace
// the monitor-sized X/Y field of view with the union of the original projection
// and both headset eyes, and extend the stock far plane to cover VR sightlines.
// Keeping the original angular extent prevents a narrow scripted projection or
// unusual aspect ratio from clipping the tracked view.
void WidenTr6CullProjection(mat4& projection, float& outTanX, float& outTanY) {
    float hmdTanX = 1.6f;
    float hmdTanY = 1.6f;
    VR().CullTangents(hmdTanX, hmdTanY);
    const float margin = Cfg().cullFovMarginDegrees;
    if (margin > 0.0f) {
        constexpr float kPi = 3.14159265358979323846f;
        const float k = std::tan(margin * kPi / 180.0f);
        const float ex = (hmdTanX + k) / (1.0f - hmdTanX * k);
        const float ey = (hmdTanY + k) / (1.0f - hmdTanY * k);
        if (ex > hmdTanX && ex < 60.0f) hmdTanX = ex;
        if (ey > hmdTanY && ey < 60.0f) hmdTanY = ey;
    }

    // For an asymmetric perspective matrix the farthest horizontal edge is
    // (1 + abs(shearX)) / abs(scaleX), and likewise for Y. Keep that edge if
    // it is wider than the headset so scripted/cutscene views cannot regress.
    const float ax = std::fabs(projection.m[0]);
    const float ay = std::fabs(projection.m[5]);
    const float gameTanX = ax > 1.0e-6f
        ? (1.0f + std::fabs(projection.m[8])) / ax : hmdTanX;
    const float gameTanY = ay > 1.0e-6f
        ? (1.0f + std::fabs(projection.m[9])) / ay : hmdTanY;
    outTanX = gameTanX > hmdTanX ? gameTanX : hmdTanX;
    outTanY = gameTanY > hmdTanY ? gameTanY : hmdTanY;

    const float signX = projection.m[0] < 0.0f ? -1.0f : 1.0f;
    const float signY = projection.m[5] < 0.0f ? -1.0f : 1.0f;
    projection.m[0] = signX / outTanX;
    projection.m[5] = signY / outTanY;
    projection.m[8] = 0.0f;
    projection.m[9] = 0.0f;

    float zn = 0.0f, zf = 0.0f;
    if (ExtractNearFar(projection, zn, zf)) {
        const float requestedFar = Cfg().farClip > 0.0f
            ? Cfg().farClip : (zf < kTr6VrFarPlane ? kTr6VrFarPlane : zf);
        if (requestedFar > zn) {
            projection.m[10] = (zn + requestedFar) / (zn - requestedFar);
            projection.m[14] = (2.0f * zn * requestedFar)
                             / (zn - requestedFar);
        }
    }
}

bool IsTr6RenderCalculateCall(const void* returnAddress) {
    if (!g_tr6ModuleBase) return false;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(returnAddress);
    return caller == g_tr6ModuleBase + g_tr6->mainCalculateReturn
        || caller == g_tr6ModuleBase + g_tr6->reflectionCalculateReturn;
}

bool Tr6CullActive() {
    if (!g_tr6ModuleBase || !Cfg().enabled || !Cfg().portalCulling) return false;
    if (!Cfg().perEyeView && Cfg().eyeOffsetMode != 3) return false;
    if (!VR().active() || !VR().poseValid()) return false;
    return Cfg().monoTracking || NativeTr6Capable() || AlternateEyeActive();
}

// App_Render_Scene_Main performs this inexpensive AABB reject before the five
// matching OBB calls. A candy bar can fail here and never reach the existing
// ClippedOBB_CPP detour, so both stages use the same render-only policy.
__declspec(noinline) bool __fastcall Detour_Tr6ClippedAabb(
    const void* clipBounds, const void* boundsMin, const void* boundsMax) {
    const auto original =
        g_hTr6ClippedAabb.Original<Fn_tr6_clipped_aabb>();
    const uintptr_t returnRva = g_tr6ModuleBase
        ? reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_tr6ModuleBase
        : 0;
    if (!g_tr6 || !Tr6ListHas(g_tr6->sceneObjectAabbReturn, returnRva)
        || !Tr6CullActive()) {
        return original(clipBounds, boundsMin, boundsMax);
    }

    if (!g_loggedTr6SceneObjectAabb) {
        g_loggedTr6SceneObjectAabb = true;
        Log("tr6 cull: final scene-object AABB rejection disabled; small "
            "ground pickups now reach the oriented-bounds and draw stages");
    }
    return false;
}

// TR6 has two visibility tiers after room preparation: the CRP builders and
// the final scene-object renderer both independently OBB-test their entries.
// Bypass only the verified render return sites; gameplay, effects, shadows and
// other scene views continue through the original shared function.
__declspec(noinline) bool __fastcall Detour_Tr6ClippedObb(
    const mat4* cameraProject, const void* viewport,
    const void* boundsMin, const void* boundsMax) {
    const auto original = g_hTr6ClippedObb.Original<Fn_tr6_clipped_obb>();
    const uintptr_t returnRva = g_tr6ModuleBase
        ? reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_tr6ModuleBase
        : 0;
    if (!g_tr6) {
        return original(cameraProject, viewport, boundsMin, boundsMax);
    }
    const Tr6Layout& t = *g_tr6;
    const bool roomRenderTest =
        returnRva == t.mainRoomGroupObbReturn
        || returnRva == t.clipRoomObbReturn;
    const bool objectListTest =
        returnRva == t.characterObbReturn
        || returnRva == t.animatedDynamicObbReturn
        || returnRva == t.animatedStaticObbReturn
        || returnRva == t.waterObbReturn[0]
        || returnRva == t.waterObbReturn[1]
        || returnRva == t.waterObbReturn[2];
    const bool sceneObjectRenderTest =
        Tr6ListHas(t.sceneObjectObbReturn, returnRva);
    const bool bypass = roomRenderTest || sceneObjectRenderTest
        || (objectListTest && g_tr6VrCalculateActive);
    if (!bypass || !Tr6CullActive()) {
        return original(cameraProject, viewport, boundsMin, boundsMax);
    }

    if (!g_loggedTr6RoomRuns) {
        g_loggedTr6RoomRuns = true;
        Log("tr6 cull: render-only static-mesh and room-run OBB rejection "
            "disabled; accepted rooms now retain all geometry for head look");
    }
    if (objectListTest && !g_loggedTr6ObjectCull) {
        g_loggedTr6ObjectCull = true;
        Log("tr6 cull: render-list character, animated-object and water OBB "
            "rejection disabled; props no longer follow the right-stick camera");
    }
    if (sceneObjectRenderTest && !g_loggedTr6SceneObjectCull) {
        g_loggedTr6SceneObjectCull = true;
        Log("tr6 cull: final scene-object OBB rejection disabled; ground "
            "pickups and props remain visible to the tracked view");
    }
    return false; // ClippedOBB_CPP: false means the bounds remain visible.
}

// SYS_DRAW_CRP begins with a room-descriptor pool followed by a RECTF portal
// pool. The engine's original input seeds are already the active rooms; do not
// replace or reorder them, because their identity participates in object
// ownership. After stock object-pool construction, give the corresponding
// output descriptors one full-screen portal so the headset retains room shells.
bool ForceAllTr6RoomsVisible(void* crp, int32_t activeRooms,
                             uint32_t calculatedRooms,
                             uint32_t originalSeeds) {
    if (!crp || activeRooms <= 0) return false;
    auto* rooms = reinterpret_cast<Tr6ItemPool*>(crp);
    auto* portals = reinterpret_cast<Tr6ItemPool*>(
        reinterpret_cast<uint8_t*>(crp) + sizeof(Tr6ItemPool));
    if (!rooms->data || !portals->data
        || rooms->maxItems < static_cast<uint32_t>(activeRooms)
        || portals->maxItems == 0) {
        return false;
    }

    auto* fullRect = reinterpret_cast<float*>(portals->data);
    fullRect[0] = -1.0f;
    fullRect[1] = -1.0f;
    fullRect[2] =  1.0f;
    fullRect[3] =  1.0f;
    portals->items = 1;
    portals->itemsNeeded = 0;

    auto* desc = reinterpret_cast<Tr6RoomPortalsDesc*>(rooms->data);
    for (int32_t room = 0; room < activeRooms; ++room) {
        desc[room].roomIndex = static_cast<uint32_t>(room);
        desc[room].firstPortal = 0;
        desc[room].portalCount = 1;
    }
    rooms->items = static_cast<uint32_t>(activeRooms);
    rooms->itemsNeeded = 0;

    if (!g_loggedTr6AllRooms) {
        g_loggedTr6AllRooms = true;
        LogF("tr6 cull: correctness-first room submission active -- %u map "
             "seeds, %u portal-visible rooms expanded to %d active rooms",
             originalSeeds, calculatedRooms, activeRooms);
    }
    return true;
}

__declspec(noinline) void __fastcall Detour_Tr6Calculate(
    void* crp, void* drawBuffer, void* matrixStack, void* roomPool,
    const Tr6CameraView* cameraView, uint32_t flags, float lodScale,
    const void* clipMin, const void* clipMax) {
    const auto original = g_hTr6Calculate.Original<Fn_tr6_calculate>();

    // Calculate also runs from map processing. Only the two render callers are
    // eligible; map/object preparation must remain entirely stock.
    const void* returnAddress = _ReturnAddress();
    const bool renderCall = IsTr6RenderCalculateCall(returnAddress);
    const bool mainRenderCall = reinterpret_cast<uintptr_t>(returnAddress)
        == g_tr6ModuleBase + (g_tr6 ? g_tr6->mainCalculateReturn : 0);
    const bool renderedViewTracksHead = Cfg().monoTracking
        || g_inNativeTr6Scene || AlternateEyeActive();
    const bool active = cameraView
        && renderCall && renderedViewTracksHead
        && Cfg().enabled && Cfg().portalCulling
        && (Cfg().perEyeView || Cfg().eyeOffsetMode == 3)
        && VR().active() && VR().poseValid();
    if (!active) {
        original(crp, drawBuffer, matrixStack, roomPool, cameraView, flags,
                 lodScale, clipMin, clipMax);
        return;
    }

    // The stock main input pool already contains the active room pointers in
    // the engine's chosen order. Preserve it byte-for-byte: replacing or
    // reordering those pointers changed overlapping-room object ownership and
    // hid small pickups. We need only its validated count for the final room
    // descriptors written after Calculate has built object pools normally.
    uint32_t originalSeeds = 0;
    int32_t activeRooms = -1;
    if (mainRenderCall && roomPool) {
        const auto* seeds = reinterpret_cast<const Tr6ItemPool*>(roomPool);
        if (seeds->data && seeds->items > 0
            && seeds->items <= static_cast<uint32_t>(kTr6MaxRooms)
            && seeds->items <= seeds->maxItems) {
            originalSeeds = seeds->items;
            activeRooms = static_cast<int32_t>(seeds->items);
        }
    }

    // SYS_DRAW_CAMERA_VIEW owns the CRP at +0xE0. A stack copy paired with the
    // original CRP argument splits one logical object across two addresses and
    // corrupts pickup preparation. The main call stays completely stock. The
    // reflection call still needs the tracked view, so patch only its three
    // matrices in the real owner and restore them immediately afterward.
    auto* liveView = const_cast<Tr6CameraView*>(cameraView);
    mat4 savedProject = {};
    mat4 savedCamera = {};
    mat4 savedCameraProject = {};
    const bool adjustCamera = !mainRenderCall;
    if (adjustCamera) {
        savedProject = liveView->project;
        savedCamera = liveView->camera;
        savedCameraProject = liveView->cameraProject;
        float tanX = 0.0f;
        float tanY = 0.0f;
        WidenTr6CullProjection(liveView->project, tanX, tanY);
        liveView->camera = Mul4(AffineToMat4(VR().HeadView()),
                                liveView->camera);
        liveView->cameraProject = Mul4(liveView->project, liveView->camera);
    }

    if (mainRenderCall && !g_loggedTr6Cull) {
        g_loggedTr6Cull = true;
        Log("tr6 cull: stock map camera, room seeds and object preparation "
            "preserved; final room visibility expands after Calculate");
    }

    g_tr6VrCalculateActive = renderCall;
    original(crp, drawBuffer, matrixStack, roomPool, cameraView, flags,
             lodScale, clipMin, clipMax);
    g_tr6VrCalculateActive = false;

    if (adjustCamera) {
        liveView->project = savedProject;
        liveView->camera = savedCamera;
        liveView->cameraProject = savedCameraProject;
    }

    if (mainRenderCall && crp && activeRooms > 0) {
        const uint32_t calculatedRooms =
            reinterpret_cast<Tr6ItemPool*>(crp)->items;
        ForceAllTr6RoomsVisible(crp, activeRooms, calculatedRooms,
                                originalSeeds);
    }
}

void TryInstallTr6Hooks() {
    if (CurrentGame() != 2) {
        return;
    }

    const bool wantScene = Cfg().nativeStereoGame6
        && !NativeTr6HookReady() && !g_tr6SceneAttempted;
    const bool wantCull = Cfg().portalCulling
        && !g_hTr6Calculate.installed() && !g_tr6CullAttempted;
    if (!wantScene && !wantCull) return;

    HMODULE module = GetModuleHandleW(L"tomb6.dll");
    if (!module) return;

    const uint32_t stamp = ModuleTimestamp(module);
    const Tr6Layout* layout = Tr6LayoutFor(stamp);
    if (!layout) {
        if (wantScene) g_tr6SceneAttempted = true;
        if (wantCull)  g_tr6CullAttempted = true;
        if (!g_warnedTr6Build) {
            g_warnedTr6Build = true;
            LogF("tr6: build-specific scene/culling hooks unavailable -- "
                 "tomb6.dll timestamp 0x%08X is not supported; native stereo "
                 "uses AER and culling stays stock. Known builds:", stamp);
            Tr6LogKnownBuilds("tr6:");
        }
        return;
    }

    if (g_tr6 != layout) {
        LogF("tr6: bound to %s (build 0x%08X)", layout->name, stamp);
    }
    g_tr6 = layout;
    g_tr6ModuleBase = reinterpret_cast<uintptr_t>(module);

    if (wantCull) {
        g_tr6CullAttempted = true;
        auto* calculate = reinterpret_cast<uint8_t*>(module) + g_tr6->calculate;
        auto* clippedObb = reinterpret_cast<uint8_t*>(module) + g_tr6->clippedObb;
        auto* clippedAabb = reinterpret_cast<uint8_t*>(module) + g_tr6->clippedAabb;

        bool ok = g_hTr6Calculate.Install(calculate,
                reinterpret_cast<void*>(&Detour_Tr6Calculate),
                sizeof(kTr6CalculatePrologue),
                 kTr6CalculatePrologue, sizeof(kTr6CalculatePrologue),
                 "TR6 SYS_DRAW_CRP::Calculate");
        if (ok) {
            ok = g_hTr6ClippedObb.Install(clippedObb,
                reinterpret_cast<void*>(&Detour_Tr6ClippedObb),
                sizeof(kTr6ClippedObbPrologue),
                kTr6ClippedObbPrologue, sizeof(kTr6ClippedObbPrologue),
                "TR6 ClippedOBB_CPP");
        }
        if (ok) {
            ok = g_hTr6ClippedAabb.Install(clippedAabb,
                reinterpret_cast<void*>(&Detour_Tr6ClippedAabb),
                sizeof(kTr6ClippedAabbPrologue),
                kTr6ClippedAabbPrologue, sizeof(kTr6ClippedAabbPrologue),
                "TR6 final-object AABB clip");
        }

        if (!ok) {
            // Calculate and both bounds stages are one render-only mechanism.
            // Roll back atomically rather than leave half the object path live.
            g_hTr6ClippedAabb.Remove();
            g_hTr6ClippedObb.Remove();
            g_hTr6Calculate.Remove();
            Log("tr6 cull: render-only hook set failed its byte/safety checks; "
                "leaving stock visibility active");
        } else {
            LogF("tr6 cull: render-only visibility hooks installed (Calculate "
                 "+0x%X, OBB +0x%X, object AABB +0x%X)",
                 g_tr6->calculate, g_tr6->clippedObb,
                 g_tr6->clippedAabb);
        }
    }

    if (wantScene) {
        g_tr6SceneAttempted = true;
        auto* fxCamDistTarget = reinterpret_cast<uint8_t*>(module)
                              + g_tr6->fxCamDist;
        auto* fxBoundsTarget = reinterpret_cast<uint8_t*>(module)
                             + g_tr6->fxBoundsClip;
        auto* fxNodeBoundsTarget = reinterpret_cast<uint8_t*>(module)
                                 + g_tr6->fxNodeBoundsClip;
        auto* shadowTarget = reinterpret_cast<uint8_t*>(module)
                           + g_tr6->drawProjectedShadows;
        auto* effectsTarget = reinterpret_cast<uint8_t*>(module)
                            + g_tr6->effectsUpdate;
        auto* sceneTarget = reinterpret_cast<uint8_t*>(module)
                          + g_tr6->renderScene;
        bool ok = g_hTr6FxCamDist.Install(fxCamDistTarget,
                reinterpret_cast<void*>(&Detour_Tr6FxCamDist),
                sizeof(g_tr6->fxCamDistPrologue),
                g_tr6->fxCamDistPrologue, sizeof(g_tr6->fxCamDistPrologue),
                "TR6 fxCamDist", kTr6FxCamDistRipOffsets,
                sizeof(kTr6FxCamDistRipOffsets)
                    / sizeof(kTr6FxCamDistRipOffsets[0]));
        if (ok) {
            ok = g_hTr6FxBoundsClip.Install(fxBoundsTarget,
                reinterpret_cast<void*>(&Detour_Tr6FxBoundsClip),
                sizeof(g_tr6->fxBoundsClipPrologue),
                g_tr6->fxBoundsClipPrologue,
                sizeof(g_tr6->fxBoundsClipPrologue),
                "TR6 mathIsBoundsClipped (FX light)",
                kTr6FxBoundsClipRipOffsets,
                sizeof(kTr6FxBoundsClipRipOffsets)
                    / sizeof(kTr6FxBoundsClipRipOffsets[0]));
        }
        if (ok) {
            ok = g_hTr6FxNodeBoundsClip.Install(fxNodeBoundsTarget,
                reinterpret_cast<void*>(&Detour_Tr6FxNodeBoundsClip),
                sizeof(g_tr6->fxNodeBoundsClipPrologue),
                g_tr6->fxNodeBoundsClipPrologue,
                sizeof(g_tr6->fxNodeBoundsClipPrologue),
                "TR6 mathIsBoundsClippedAlt (FX nodes)",
                kTr6FxNodeBoundsClipRipOffsets,
                sizeof(kTr6FxNodeBoundsClipRipOffsets)
                    / sizeof(kTr6FxNodeBoundsClipRipOffsets[0]));
        }
        if (ok) {
            ok = g_hTr6DrawProjectedShadows.Install(shadowTarget,
                reinterpret_cast<void*>(&Detour_Tr6DrawProjectedShadows),
                sizeof(kTr6DrawProjectedShadowsPrologue),
                kTr6DrawProjectedShadowsPrologue,
                sizeof(kTr6DrawProjectedShadowsPrologue),
                "TR6 App_DrawChar_DrawProjectedShadows");
        }
        if (ok) {
            ok = g_hTr6EffectsUpdate.Install(effectsTarget,
                reinterpret_cast<void*>(&Detour_Tr6EffectsUpdate),
                sizeof(kTr6EffectsUpdatePrologue),
                kTr6EffectsUpdatePrologue,
                sizeof(kTr6EffectsUpdatePrologue),
                "TR6 App_DrawEffects_UpdateRenderData");
        }
        if (ok) {
            ok = g_hTr6RenderScene.Install(sceneTarget,
                reinterpret_cast<void*>(&Detour_Tr6RenderScene),
                sizeof(kTr6RenderScenePrologue),
                kTr6RenderScenePrologue, sizeof(kTr6RenderScenePrologue),
                "TR6 App_Render_Scene");
        }
        if (!ok) {
            // Native stereo must never run without identifying the light-view
            // shadow pass or head-facing effect builder: the generic offscreen
            // path would distort shadows and leave billboards camera-bound.
            g_hTr6RenderScene.Remove();
            g_hTr6EffectsUpdate.Remove();
            g_hTr6DrawProjectedShadows.Remove();
            g_hTr6FxNodeBoundsClip.Remove();
            g_hTr6FxBoundsClip.Remove();
            g_hTr6FxCamDist.Remove();
            Log("tr6: native scene/shadow/effects hook set failed its "
                "byte/safety "
                "checks; "
                "using AER fallback");
        } else {
            LogF("tr6: native scene/shadow/effects hooks installed (scene +0x%X, "
                 "projected shadows +0x%X, effects +0x%X, FX range +0x%X, "
                 "FX bounds +0x%X, FX-node bounds +0x%X)",
                 g_tr6->renderScene, g_tr6->drawProjectedShadows,
                 g_tr6->effectsUpdate, g_tr6->fxCamDist,
                 g_tr6->fxBoundsClip, g_tr6->fxNodeBoundsClip);
        }
    }
}

// ---------------------------------------------------------------------------
// fmvShow -- "a video is on screen this frame"
// ---------------------------------------------------------------------------
typedef void (__cdecl* Fn_fmvShow)(void);

void __cdecl Detour_fmvShow() {
    g_fmvFrame = g_frameIndex;
    g_hFmvShow.Original<Fn_fmvShow>()();
}

// ---------------------------------------------------------------------------
// ogl_present -- frame boundary
// ---------------------------------------------------------------------------
void LazyInit() {
    if (g_ready) return;
    if (!VR().active()) return;          // nothing to set up until OpenVR is live
    if (!wglGetCurrentContext()) return;

    // Mono touches no GL objects at all: no eye target, no FBO_default
    // redirection, nothing to fail. It just needs poses.
    if (Cfg().monoTracking) {
        g_ready = true;
        VR().BeginFrame();
        Log("present: MONO head-tracking active (no stereo target, no submit)");
        return;
    }

    if (!gl::Load()) return;

    uint32_t w = 0, h = 0;
    VR().GetEyeSize(w, h);
    if (!Stereo().Create(w, h)) return;

    // Make the engine's notion of "the backbuffer" be our eye target.
    //
    // FBO_default is a single global that init_ogl latches from
    // GL_FRAMEBUFFER_BINDING, and ogl_setRenderTarget binds it whenever the
    // game asks for colour_id == 0 && depth_id == 0. Overwriting it redirects
    // every "back to the screen" in the engine into the stereo target without
    // hooking ogl_setRenderTarget at all.
    g_realDefaultFbo = FboDefault();
    FboDefault() = Stereo().fbo();
    LogF("present: redirected FBO_default %u -> %u", g_realDefaultFbo, Stereo().fbo());

    // Single-eye scratch, at the engine's own render size. Native TR6 stereo
    // reuses it for each full scene pass; AER uses it for one whole frame.
    if (Cfg().nativeStereoGame6 || Cfg().alternateEyeGame6) {
        const int mw = ScreenWidth()  > 0 ? ScreenWidth()  : (int)(w * 2);
        const int mh = ScreenHeight() > 0 ? ScreenHeight() : (int)h;
        if (!Stereo().CreateMono((uint32_t)mw, (uint32_t)mh)) {
            Log("stereo: mono scratch unavailable -- TR6 native stereo and "
                "alternate-eye fallback are disabled");
        }
    }

    // Offscreen video panel. Optional: if it fails to build, the viewport-fit
    // path still handles the video, just with residual keystone.
    if (Cfg().videoOffscreen) {
        const int vw = ScreenWidth()  > 0 ? ScreenWidth()  : (int)w;
        const int vh = ScreenHeight() > 0 ? ScreenHeight() : (int)h;
        if (!Video().Create((uint32_t)vw, (uint32_t)vh)) {
            Log("video: offscreen panel unavailable, falling back to the "
                "viewport fit (residual keystone off-axis)");
        }
    }

    g_ready = true;
    VR().BeginFrame();
    Stereo().BeginFrame();
}

// Bring OpenVR up from inside the render loop rather than at process start, and
// retry on a slow cadence.
//
// This buys resilience: SteamVR can be started after the game, and a runtime
// that is mid-startup gets another chance instead of losing the session. If it
// never comes up, every detour early-outs through VrLive() and the game runs
// exactly as it would without the mod.
bool BringUpVR() {
    if (VR().active()) return true;
    if (g_vrGaveUp || !Cfg().enabled) return false;

    // Retry on a slow cadence: SteamVR may still be starting up.
    if (g_vrLastAttemptFrame != 0 && (g_frameIndex - g_vrLastAttemptFrame) < 120) return false;
    g_vrLastAttemptFrame = g_frameIndex;
    ++g_vrAttempts;

    LogF("vr: initialisation attempt %u at frame %u", g_vrAttempts, g_frameIndex);
    if (VR().Init()) {
        Log("vr: up and running");
        return true;
    }

    if (g_vrAttempts >= 10) {
        g_vrGaveUp = true;
        Log("vr: giving up after 10 attempts -- the game continues unmodified");
    }
    return false;
}

void __cdecl Detour_ogl_present() {
    // Composite using this frame's poses before polling/advancing any state.
    HKScopePresent();
    WorldLaserPresent();
    ++g_frameIndex;

    // Frame-graph trace bookkeeping, around the frame boundary.
    if (g_tracing) {
        FlushTraceRun();
        LogF("trace: --- frame %u totals: world=%d flat=%d across %d target run(s) ---",
             g_frameIndex - 1, g_frameWorld, g_frameFlat, g_traceEmitted);
        if (g_frameWorld + g_frameFlat < 30) {
            Log("trace: NOTE very few draws -- this is a menu or loading screen, "
                "not gameplay. Re-capture with a level running.");
        }
    }

    PollTraceKey();
    PollDumpKey();
    PollTuningKeys();
    PortalCullPollKey();

    // Re-assert the XInput pointer every frame: cheap, and it self-heals if the
    // game re-resolves XInput or if we got here before WinMain had.
    GamepadUpdate();

    // The game DLLs load after we do, and the player can switch between TR4 and
    // TR5 without restarting, so both of these run every frame.
    GameDllUpdate();
    InventorySceneUpdate();
    FirstPersonUpdate();

    // TR6's render boundary and visibility builder live in tomb6.dll rather
    // than the shared engine. Install them only after that DLL is live and has
    // been identified exactly.
    TryInstallTr6Hooks();

    // Must follow GameDllUpdate: it hooks INSIDE the game DLL, so it needs to
    // know which one is live and which build it is.
    PortalCullUpdate();
    SkyUpdate();
    OverlayUpdate();

    // Measurement only -- see DynamicBones.h. Must follow GameDllUpdate for
    // the same reason PortalCullUpdate does: it hooks inside the game DLL.
    DynamicBonesUpdate();

    // Latch "an optic is up" ONCE, here, rather than letting anything poll it
    // per draw. Everything that reads the eye transform during a frame -- world
    // draws, the 2D panel, the video panel, the culling frustum -- has to agree
    // about where the head is, and a value read out of game memory mid-frame
    // could flip between them and leave the panel in a different space from the
    // geometry behind it.
    //
    // Reading it at the frame boundary means it trails the game's own control
    // phase by one frame. That is 1/60 s of staleness on the way into and out of
    // a zoom whose ramp is tens of frames long, and it buys the consistency
    // above; polling it later would cost that and buy nothing.
    {
        const bool hold = Cfg().opticsHeadAtCamera && IsOpticsZoomed();
        if (hold != VR().headAtCamera()) {
            VR().SetHeadAtCamera(hold);
            LogF("optics: head centre %s -- laser sight and bullet %s (eyes keep "
                 "their offsets either way, so world depth is unchanged)",
                 hold ? "HELD AT THE GAME CAMERA" : "released to the tracked pose",
                 hold ? "agree at every distance" : "diverge by head offset");
        }
    }

    // Periodic health report, in deltas. A one-shot report at frame 300 only
    // ever sampled the menus, where almost everything legitimately is 2D -- it
    // said "2 injections out of 725 draws" and looked like a bug when it was
    // just a title screen.
    if (VrLive() && g_frameIndex >= g_lastReportFrame + 1800) {
        const unsigned dDup   = g_dupCount       - g_prevDup;
        const unsigned dInj0  = g_injectCount[0]  - g_prevInj0;
        const unsigned dInj1  = g_injectCount[1]  - g_prevInj1;
        const unsigned dWorld = g_worldDraws      - g_prevWorld;
        const unsigned dOff   = g_worldOffscreen  - g_prevOffscreen;
        const unsigned dO3D   = g_ortho3DDraws    - g_prevOrtho3D;
        const unsigned dSky   = g_skyDraws        - g_prevSky;
        const unsigned dOfs   = g_offsetDraws     - g_prevOffset;
        g_lastReportFrame = g_frameIndex;
        g_prevDup = g_dupCount; g_prevInj0 = g_injectCount[0]; g_prevInj1 = g_injectCount[1];
        g_prevWorld = g_worldDraws; g_prevOffscreen = g_worldOffscreen;
        g_prevOrtho3D = g_ortho3DDraws;
        g_prevSky = g_skyDraws;
        g_prevOffset = g_offsetDraws;

        // Against total world draws, not against duplications. See g_worldDraws.
        const unsigned pct = dWorld ? (100u * (dInj0 + dInj1) / dWorld) : 0u;
        // Measured frame rate, and what each eye actually gets.
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (g_lastReportTime.QuadPart != 0 && freq.QuadPart != 0) {
            const double secs = double(now.QuadPart - g_lastReportTime.QuadPart)
                              / double(freq.QuadPart);
            if (secs > 0.0) {
                const double fps = 1800.0 / secs;
                const char* mode = NativeTr6Active()
                    ? " -- TR6 native stereo, two scene passes per frame"
                    : (AlternateEyeActive()
                       ? " -- alternate-eye, so each EYE updates at half that"
                       : "");
                LogF("perf: %.1f fps rendered%s", fps, mode);
            }
        }
        g_lastReportTime = now;

        LogF("stereo health @frame %u: world draws=%u  duplicated=%u  "
             "injected eye0=%u eye1=%u  (%u%% of world draws got per-eye matrices)  "
             "offscreen-skipped=%u  classify-mismatch=%u  ortho3D=%u  sky=%u  "
             "projoffset=%u",
             g_frameIndex, dWorld, dDup, dInj0, dInj1, pct, dOff, g_classifyMismatch,
             dO3D, dSky, dOfs);

        // What the GPU actually held, per eye. This is the load-bearing line:
        // if the two translations match, the per-eye view never reached the
        // shader and nothing downstream can produce depth. If they differ, the
        // matrices are on the GPU and the fault is viewport, target or submit.
        if (g_verifyDone[0] && g_verifyDone[1]) {
            const float dx = g_gpuViewT[0][0] - g_gpuViewT[1][0];
            const float dy = g_gpuViewT[0][1] - g_gpuViewT[1][1];
            const float dz = g_gpuViewT[0][2] - g_gpuViewT[1][2];
            const float sep = std::sqrt(dx*dx + dy*dy + dz*dz);
            LogF("verify: %s ON GPU  eye0=(%.1f, %.1f, %.1f) prog=%d loc=%d  "
                 "eye1=(%.1f, %.1f, %.1f) prog=%d loc=%d  separation=%.2f",
                 (Cfg().eyeOffsetMode == 3) ? "uProjMatrix translation column"
                                            : "uViewMatrix translation",
                 g_gpuViewT[0][0], g_gpuViewT[0][1], g_gpuViewT[0][2],
                 g_gpuProg[0], g_gpuLoc[0],
                 g_gpuViewT[1][0], g_gpuViewT[1][1], g_gpuViewT[1][2],
                 g_gpuProg[1], g_gpuLoc[1], sep);
            if (!gl::GetUniformfv) {
                Log("verify: glGetUniformfv unavailable -- GPU-side check skipped, "
                    "the numbers above are meaningless.");
            } else if (sep < 0.01f) {
                Log("verify: *** the two eyes hold the SAME matrix. The per-eye "
                    "transform is not reaching the shader. ***");
            }
        } else if (VrLive() && !Cfg().monoTracking) {
            Log("verify: no per-eye GPU sample taken this window -- injection is "
                "not running on both eyes.");
        }
        g_verifyArmed = true;
        g_verifyDone[0] = g_verifyDone[1] = false;

        // The frame just rendered is still sitting in the eye target -- Submit
        // and the clear both happen later in this function. Compare the two
        // halves pixel for pixel. Every upstream stage has now been verified
        // correct, so this is the measurement that says whether the halves are
        // genuinely two different images or one image twice.
        if (VrLive() && !Cfg().monoTracking) {
            int samples = 0, differing = 0;
            Stereo().CompareHalves(samples, differing);
            // NOT a depth measure. It compares the SAME pixel coordinate in
            // both halves, so it reports image SHIFT: a large constant offset
            // (the frustum shear) lights it up, while correct parallax of a few
            // pixels across smooth texture falls under the threshold and reads
            // as zero. A low number here means nothing is wrong.
            LogF("halves: %d of %d sampled pixels differ (image shift indicator "
                 "only -- not a depth measure)", differing, samples);
        }

        LogProjHistogram();

        // Room culling, averaged over the window. `added` is the whole point:
        // zero of it means either the head never left the game camera's cone or
        // the traversal is not running, and the two are told apart by whether
        // the "cull: head-frustum portal traversal live" line ever appeared.
        {
            const PortalCullStats cs = PortalCullTakeStats();
            if (cs.frames) {
                LogF("cull: %.1f rooms/frame from the engine + %.1f added by the "
                     "head frustum, %.1f items rescued/frame%s",
                     double(cs.rooms) / cs.frames,
                     double(cs.added) / cs.frames,
                     double(cs.items) / cs.frames,
                     cs.truncated ? "  *** A BUDGET WAS HIT -- raise "
                                    "CullMaxPortals/CullMaxDepth ***" : "");
            }
        }

        if (dWorld > 200 && pct < 25) {
            LogF("stereo health: *** only %u%% of world draws got per-eye matrices "
                 "(%u of %u were skipped as offscreen), so most of the scene renders "
                 "the same view into both halves -- that is why there is no depth. ***",
                 pct, dOff, dWorld);
        }
    }

    const bool wantTrace = TraceActive();
    if (wantTrace && !g_tracing) {
        LogF("trace: BEGIN frame %u  screen=%dx%d  FBO_default=%u  FBO_custom=%u",
             g_frameIndex, ScreenWidth(), ScreenHeight(), FboDefault(), FboCustom());
    }
    if (!wantTrace && g_tracing) {
        Log("trace: done");
    }
    if (wantTrace != g_tracing) {
        g_tracing = wantTrace;
        g_run = TraceRun{};
    }
    g_traceEmitted = 0;
    g_frameWorld = g_frameFlat = 0;

    BringUpVR();
    LazyInit();

    if (!VrLive()) {
        g_hPresent.Original<Fn_ogl_present>()();
        return;
    }

    // Mono: swap normally, then sample the head pose for the next frame. No
    // compositor involvement whatsoever.
    if (Cfg().monoTracking) {
        g_hPresent.Original<Fn_ogl_present>()();
        VR().BeginFrame();
        return;
    }

    // Hand both halves to the compositor before the swap so the runtime gets
    // the frame as early as possible.
    // AER: fold this frame's eye into its half. The other half is untouched and
    // keeps the frame it was given.
    if (AlternateEyeActive() && Stereo().monoValid()) {
        Stereo().BlitMonoToHalf(g_frameEye);
    }

    if (Cfg().eyeMarkers) Stereo().MarkEyes();

    uint32_t w = 0, h = 0;
    VR().GetEyeSize(w, h);
    VR().Submit(Stereo().texture(), w, h);

    if (Cfg().mirrorToWindow) {
        Stereo().MirrorToWindow(ScreenWidth(), ScreenHeight(), g_realDefaultFbo);
    }

    // The original swaps the window and resets the vb/ib cache. Point
    // FBO_default back at the real window for the duration so anything inside
    // it that touches the default framebuffer behaves.
    FboDefault() = g_realDefaultFbo;
    g_hPresent.Original<Fn_ogl_present>()();
    FboDefault() = (AlternateEyeActive() && Stereo().monoValid())
                 ? Stereo().monoFbo()
                 : Stereo().fbo();

    // Latch poses for the frame we are about to render, then re-arm the target.
    VR().BeginFrame();

    // Native stereo is scoped by the real App_Render_Scene call and needs no
    // heuristic. Only AER must decide whether the coming whole frame has TR6's
    // offscreen 3D scene. Menus and FMVs use ordinary per-draw duplication.
    {
        g_offscreenWorldPrev = g_offscreenWorld;
        g_offscreenWorld = 0;
        const unsigned threshold =
            static_cast<unsigned>(Cfg().alternateEyeMinOffscreen < 0
                                  ? 0 : Cfg().alternateEyeMinOffscreen);
        const bool hasScene = g_offscreenWorldPrev >= threshold;
        const bool wantAer = AlternateEyeCapable() && hasScene;

        if (wantAer != g_aerLatched) {
            LogF("tr6: alternate-eye %s (%u offscreen world draws last frame)",
                 wantAer ? "ON -- native hook unavailable"
                         : "off -- native stereo or 2D",
                 g_offscreenWorldPrev);
        }
        g_aerLatched = wantAer;
    }

    if (AlternateEyeActive()) {
        // Flip eyes, then render the whole frame into the single-eye scratch.
        // FBO_default points there, so the engine's own clears cannot reach the
        // half holding the other eye.
        g_frameEye = 1 - g_frameEye;
        g_currentEye = g_frameEye;
        Stereo().BeginMono();
        FboDefault() = Stereo().monoFbo();
        if (!g_loggedAltEye) {
            g_loggedAltEye = true;
            Log("tr6: alternate-eye rendering active -- the scene is drawn "
                "offscreen and composited, so each FRAME is one eye. Both eyes "
                "are stereo-correct; each updates at half the frame rate.");
        }
    } else {
        // Native stereo also starts from a cleared double-wide destination.
        // App_Render_Scene will clear and reuse the single-eye scratch itself
        // before each eye pass, then copy both completed composites here.
        FboDefault() = Stereo().fbo();
        Stereo().BeginFrame();
    }

    if (!g_loggedStereoFrame) {
        g_loggedStereoFrame = true;
        Log("present: first stereo frame submitted");
    }
}

} // namespace

bool InstallHooks() {
    if (!Bind()) return false;

    struct Target {
        hook::InlineHook* hook;
        uint32_t          rva;
        void*             detour;
        size_t            stolen;
        const uint8_t*    expect;
        size_t            expectLen;
        const char*       name;
        const int*        rip;       // RIP-relative disp32 offsets, or nullptr
        size_t            ripCount;
    };

    const Target targets[] = {
        { &g_hSetPass,      L().vid_setPass,   reinterpret_cast<void*>(&Detour_vid_setPass),
          10, kSetPassPrologue,  sizeof(kSetPassPrologue),  "vid_setPass"   },
        { &g_hValidateDraw, L().validate_draw, reinterpret_cast<void*>(&Detour_validate_draw),
           6, kValidatePrologue, sizeof(kValidatePrologue), "validate_draw" },
        { &g_hDraw,         L().ogl_draw,      reinterpret_cast<void*>(&Detour_ogl_draw),
           5, kDrawPrologue,     sizeof(kDrawPrologue),     "ogl_draw"      },
        { &g_hDrawVB,       L().ogl_drawVB,    reinterpret_cast<void*>(&Detour_ogl_drawVB),
           5, kDrawPrologue,     sizeof(kDrawPrologue),     "ogl_drawVB"    },
        { &g_hPresent,      L().ogl_present,   reinterpret_cast<void*>(&Detour_ogl_present),
           6, kPresentPrologue,  sizeof(kPresentPrologue),  "ogl_present"   },
        { &g_hFmvShow,      L().fmvShow,       reinterpret_cast<void*>(&Detour_fmvShow),
           7, kFmvShowPrologue,  sizeof(kFmvShowPrologue),  "fmvShow"       },
    };

    bool allOk = true;
    for (const Target& t : targets) {
        if (!t.hook->Install(Fn(t.rva), t.detour, t.stolen, t.expect, t.expectLen,
                             t.name, t.rip, t.ripCount))
            allOk = false;
    }

    if (!allOk) {
        Log("hooks: at least one hook failed -- rolling all of them back");
        RemoveHooks();
        return false;
    }

    Log("hooks: all six installed");

    // Optional, and deliberately outside the all-or-nothing table above: a
    // failure here costs the per-vertex chest path, never stereo. It has to go
    // in now, before the game creates its context and runs shader_init.
    BoneSkinInstall();
    TR4EffectsInstall();
    return true;
}

void RemoveHooks() {
    InventorySceneShutdown();
    TR4EffectsShutdown();
    HKScopeShutdown();
    WorldLaserShutdown();
    GamepadShutdown();
    FirstPersonShutdown();
    OverlayShutdown();
    SkyShutdown();
    DynamicBonesShutdown();
    BoneSkinShutdown();
    PortalCullShutdown();
    GameDllShutdown();

    // The game-DLL hooks were installed dynamically after the shared hooks.
    g_hTr6RenderScene.Remove();
    g_hTr6EffectsUpdate.Remove();
    g_hTr6DrawProjectedShadows.Remove();
    g_hTr6FxNodeBoundsClip.Remove();
    g_hTr6FxBoundsClip.Remove();
    g_hTr6FxCamDist.Remove();
    g_hTr6ClippedAabb.Remove();
    g_hTr6ClippedObb.Remove();
    g_hTr6Calculate.Remove();

    // Reverse order of shared-hook installation.
    g_hFmvShow.Remove();
    g_hPresent.Remove();
    g_hDrawVB.Remove();
    g_hDraw.Remove();
    g_hValidateDraw.Remove();
    g_hSetPass.Remove();

    if (g_ready && g_realDefaultFbo != 0) {
        FboDefault() = g_realDefaultFbo;
    }
    g_ready = false;
    g_inNativeTr6Scene = false;
    g_inTr6ShadowDepthPass = false;
    g_tr6SceneAttempted = false;
    g_tr6CullAttempted = false;
    g_tr6ModuleBase = 0;
    g_tr6 = nullptr;
    g_aerLatched = false;
}

} // namespace tr
