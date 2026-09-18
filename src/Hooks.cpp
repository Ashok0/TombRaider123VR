// Hooks.cpp -- the stereo injection layer for Tomb Raider I-III Remastered.
//
// Six hooks. Two do the VR work; four are plumbing that stereo cannot function
// without.
//
//   vid_setPass          REQUIRED WORK. Classifies each pass as world-space or
//                        2D by reading which matrix vid_state.proj was pointed
//                        at. This is the only signal in the engine that
//                        distinguishes "3D scene" from "HUD/menu/subtitle".
//
//   validate_draw        REQUIRED WORK. The single choke point where every
//                        uniform reaches the GPU. Per-eye projection and view
//                        are written into mProj[1] / mView_packed here, the
//                        dirty bits are forced, the original uploads, and the
//                        engine's own matrices are put back so it never
//                        observes the substitution.
//
//   ogl_draw             PLUMBING. validate_draw cannot issue a draw -- its
//                        caller does, immediately after it returns. Duplicating
//                        the draw per eye therefore happens one level up.
//
//   ogl_present          PLUMBING. The frame boundary: submit to the
//                        compositor, mirror to the window, then WaitGetPoses.
//
//   ogl_setRenderTarget  PLUMBING. TR1-3 has no ogl_rt latch to read, so the
//                        only way to know whether the engine is drawing to the
//                        backbuffer is to watch it change targets.
//
//   fmvShow              PLUMBING. An exact "a video is on screen" signal.
//
// The engine is never asked to render the scene twice. Each draw call is issued
// twice into the two halves of one double-wide target, with different matrices
// and a different viewport. That needs no shader changes.
//
// ---------------------------------------------------------------------------
// WHY THE DUPLICATION IS AIRTIGHT  (verified, not assumed)
// ---------------------------------------------------------------------------
//
// tools\xrefs.py disassembles all 1017 functions the PDB names and reports who
// calls whom. Against tomb123.exe:
//
//   validate_draw        <- ogl_draw (x1)
//   ogl_draw             <- (no direct call in .text -- APP vtable)
//   vid_setPass          <- (no direct call in .text -- APP vtable)
//   ogl_present          <- (no direct call in .text -- APP vtable)
//   fmvShow              <- (no direct call in .text -- APP vtable)
//   ogl_setRenderTarget  <- (no direct call in .text -- APP vtable)
//
// Two things follow, and both are load-bearing:
//
//   1. validate_draw has EXACTLY ONE caller, ogl_draw, which we hook. So matrix
//      injection can never run outside the per-eye duplication loop -- there is
//      no second path into it that would inject with a stale g_currentEye. In
//      TR4-6 there were two callers (ogl_draw and ogl_drawVB) and both had to be
//      hooked to get this property; here it is free.
//
//   2. The other five are never called from inside the exe at all. vidInit and
//      init_ogl install them into the APP struct and the game DLLs call through
//      those function pointers, so patching the function bodies catches every
//      call from tomb1.dll, tomb2.dll and tomb3.dll without hooking anything in
//      the DLLs themselves -- which matters, because there are three of them and
//      they are swapped as the player changes game.
//
// (An earlier version of that scan swept .text linearly and reported zero
// callers for everything, including validate_draw. It had desynchronised on
// alignment padding. Do not re-derive this with a linear sweep.)
//
// ---------------------------------------------------------------------------
// DIFFERENCES FROM THE TR4-6 VERSION OF THIS FILE
// ---------------------------------------------------------------------------
//
//   * ogl_drawVB does not exist here, so there is ONE duplication path.
//   * vid_setPass takes three arguments, not four.
//   * The whole alternate-eye (AER) machinery is gone. It existed for TR6,
//     which drew its scene offscreen and composited; TR1, TR2 and TR3 all draw
//     straight to the backbuffer, so per-draw duplication reaches everything
//     and every eye updates at full rate.
//   * TargetIsBackbuffer() is maintained by a hook rather than read from a
//     global.
//   * preserveProjOffset is kept but is expected to be permanently inert: this
//     build has no vid_setPerspOffset, and ogl_setPersp explicitly zeroes
//     mProj[1].e02/.e12 (verified at RVA 0x0000FAD0). It is left in because it
//     is guarded on the shear being non-zero, so it costs nothing and would do
//     the right thing if some pass ever did carry one.
#include "Hooks.h"
#include "Engine.h"
#include "StereoMath.h"
#include "Config.h"
#include "GL.h"
#include "Log.h"
#include "InlineHook.h"
#include "StereoRenderer.h"
#include "VideoPanel.h"
#include "Gamepad.h"
#include "Callsite.h"
#include "GameDll.h"
#include "PortalCull.h"
#include "Sky.h"
#include "FirstPerson.h"
#include "VRSystem.h"

#include <cstring>
#include <intrin.h>
#include <cmath>

namespace tr {
namespace {

// --- hook objects -----------------------------------------------------------
hook::InlineHook g_hSetPass;
hook::InlineHook g_hValidateDraw;
hook::InlineHook g_hDraw;
hook::InlineHook g_hPresent;
hook::InlineHook g_hFmvShow;
hook::InlineHook g_hSetRenderTarget;

typedef void(__cdecl* Fn_vid_setPass)(int shader, float* params, int cull);
typedef void(__cdecl* Fn_validate_draw)();
typedef void(__cdecl* Fn_ogl_draw)(void* mesh, unsigned firstIndex, unsigned count);
typedef void(__cdecl* Fn_ogl_present)();
typedef void(__cdecl* Fn_fmvShow)(void);
typedef void(__cdecl* Fn_ogl_setRenderTarget)(int a0, int a1, int a2, int a3);

// --- verified prologue bytes ------------------------------------------------
//
// Produced by tools\prologue.py against tomb123.exe, which prints the raw bytes
// and flags any RIP-relative operand. Install() refuses to patch if these do not
// match, so a game update degrades to "VR did not start" rather than a corrupted
// instruction stream.
//
// Every window below stops on an instruction boundary and contains NO
// RIP-relative operand, so no displacement fixups are needed and the
// ripDisp32Offsets argument stays null throughout. That is checked, not assumed:
// prologue.py marks such operands explicitly, and vid_setPass is exactly why the
// window there is 6 bytes and not more -- its third instruction is a
// RIP-relative MOVSS.

// 40 53           push rbx
// 48 83 EC 30     sub  rsp, 0x30      -> 6 bytes, both PIC
// (the next instruction is `movss xmm1, [rip+...]`, so we stop here)
const uint8_t kSetPassPrologue[]  = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30 };

// 48 89 5C 24 08  mov [rsp+8], rbx    -> 5 bytes, PIC
const uint8_t kValidatePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

// 48 89 5C 24 08  mov [rsp+8], rbx    -> 5 bytes, PIC
const uint8_t kDrawPrologue[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

// 48 89 5C 24 08  mov [rsp+8], rbx    -> 5 bytes, PIC
const uint8_t kPresentPrologue[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

// 48 89 5C 24 08  mov [rsp+8], rbx    -> 5 bytes, PIC
const uint8_t kFmvShowPrologue[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

// 40 56        push rsi     <- TWO bytes: a REX prefix with no bits set, which
//                               the compiler emits here and which is easy to
//                               miscount as the one-byte 0x56 form. Getting this
//                               wrong makes the window below end one byte short,
//                               inside `sub rsp, 0x30`, and the trampoline then
//                               returns into the middle of an instruction.
//                               toolserify_addresses.py checks for exactly this.
// 57           push rdi
// 41 54        push r12
// 41 56        push r14
// 41 57        push r15
// 48 83 EC 30  sub  rsp, 0x30   -> 13 bytes, all PIC
const uint8_t kSetRtPrologue[]    = { 0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
                                      0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 };

// --- per-frame state --------------------------------------------------------

bool  g_worldPass    = false;   // set by the vid_setPass hook
int   g_currentEye   = 0;
bool  g_inDuplicate  = false;   // guards against recursive duplication
bool  g_ready        = false;   // GL objects created, VR live
bool  g_loggedEye[2] = { false, false };
float g_eyeViewT[2][3] = {};
bool  g_reportedSeparation = false;

unsigned g_injectCount[2] = { 0, 0 };
unsigned g_dupCount = 0;
unsigned g_classifyMismatch = 0;
unsigned g_lastReportFrame = 0;
LARGE_INTEGER g_lastReportTime = {};
unsigned g_prevDup = 0, g_prevInj0 = 0, g_prevInj1 = 0;

unsigned g_worldDraws     = 0;
unsigned g_worldOffscreen = 0;
unsigned g_ortho3DDraws  = 0;
bool     g_loggedOrtho3D = false;
unsigned g_skyDraws      = 0;
bool     g_loggedSky     = false;
unsigned g_offsetDraws   = 0;
bool     g_loggedOffset  = false;
unsigned g_prevOrtho3D   = 0;
unsigned g_prevSky       = 0;
unsigned g_prevOffset    = 0;
unsigned g_prevWorld = 0, g_prevOffscreen = 0;

int   g_bypassSeen[8]  = { -1, -1, -1, -1, -1, -1, -1, -1 };

bool  g_verifyArmed    = true;
bool  g_verifyDone[2]  = { false, false };
float g_gpuViewT[2][3] = {};
int   g_gpuProg[2]     = { 0, 0 };
int   g_gpuLoc[2]      = { -1, -1 };

// Histogram of the actual vid_state.proj pointers seen at draw time. The
// world/2D test assumes proj is only ever &mProj[0] or &mProj[1]; if the engine
// points it somewhere else the test silently says "2D" for everything and no
// per-eye matrices are ever applied.
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

GLuint   g_realDefaultFbo = 0;
unsigned g_frameIndex     = 0;

// Deferred OpenVR bring-up (see BringUpVR).
unsigned g_vrAttempts         = 0;
unsigned g_vrLastAttemptFrame = 0;
bool     g_vrGaveUp           = false;
bool     g_loggedStereoFrame  = false;
int      g_loggedGame         = -1;

// --- frame-graph tracer -----------------------------------------------------
// Answers the one question stereo depends on: where is the 3D scene actually
// drawn? No extra hook needed -- validate_draw already runs before every draw.
struct TraceRun {
    bool  backbuffer = true;
    GLint fbo        = -1;
    int   world      = 0;
    int   flat       = 0;
};
TraceRun g_run;
bool     g_tracing         = false;
int      g_traceEmitted    = 0;
int      g_frameWorld      = 0;
int      g_frameFlat       = 0;
unsigned g_traceUntilFrame = 0;
bool     g_traceKeyWasDown = false;

void PollTraceKey() {
    const auto& c = Cfg();
    if (c.traceKey == 0 || c.traceFrames <= 0) return;
    const bool down = (GetAsyncKeyState(c.traceKey) & 0x8000) != 0;
    if (down && !g_traceKeyWasDown) {
        g_traceUntilFrame = g_frameIndex + static_cast<unsigned>(c.traceFrames);
        LogF("trace: hotkey pressed, capturing %d frame(s)", c.traceFrames);
    }
    g_traceKeyWasDown = down;
}

// --- per-draw state dump ----------------------------------------------------
unsigned g_dumpRemaining  = 0;
unsigned g_dumpIndex      = 0;
bool     g_dumpKeyWasDown = false;

void PollDumpKey() {
    const auto& c = Cfg();
    if (c.dumpKey == 0 || c.dumpDraws <= 0) return;
    const bool down = (GetAsyncKeyState(c.dumpKey) & 0x8000) != 0;
    if (down && !g_dumpKeyWasDown) {
        g_dumpRemaining = static_cast<unsigned>(c.dumpDraws);
        g_dumpIndex     = 0;
        LogF("dump: hotkey pressed, capturing the next %d draw(s)", c.dumpDraws);
    }
    g_dumpKeyWasDown = down;
}

// vid_setPass assigns vid_state.shader before its own range check, so anything
// indexing shaders[] has to clamp first -- the array is only 74 entries.
bool vs_shader_in_range(int shader) {
    return shader >= 0 && shader < kShaderCount;
}

void DumpDrawState() {
    if (g_dumpRemaining == 0 || g_currentEye != 0) return;
    --g_dumpRemaining;

    const RenderState& vs = VidState();
    const mat4* p = vs.proj;

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
             "M=(%.1f,%.1f,%.1f)  bb=%d",
             g_dumpIndex++, g_frameIndex, vs.shader, slot, kind,
             p->m[0], p->m[5], p->m[10], p->m[11], p->m[8], p->m[9],
             p->m[12], p->m[13],
             vt[0], vt[1], vt[2], mt[0], mt[1], mt[2],
             TargetIsBackbuffer() ? 1 : 0);
    } else {
        LogF("dump %-3u f=%u sh=%-3d %s/%s  V=(%.1f,%.1f,%.1f)  "
             "M=(%.1f,%.1f,%.1f)  bb=%d",
             g_dumpIndex++, g_frameIndex, vs.shader, slot, kind,
             vt[0], vt[1], vt[2], mt[0], mt[1], mt[2],
             TargetIsBackbuffer() ? 1 : 0);
    }

    if (g_dumpRemaining == 0) Log("dump: capture complete");
}

// --- live tuning hotkeys ----------------------------------------------------
bool g_tuneWasDown[5] = { false, false, false, false, false };

void PollTuningKeys() {
    const auto& c = Cfg();
    const int keys[5] = { c.scaleUpKey, c.scaleDownKey,
                          c.ipdUpKey,   c.ipdDownKey, c.resetTuningKey };
    const float step = c.scaleStep;
    for (int i = 0; i < 5; ++i) {
        if (keys[i] == 0) continue;
        const bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !g_tuneWasDown[i]) {
            switch (i) {
            case 0: AdjustWorldScale(step);        break;
            case 1: AdjustWorldScale(1.0f / step); break;
            case 2: AdjustIpdScale(step);          break;
            case 3: AdjustIpdScale(1.0f / step);   break;
            case 4: ResetTuning();                 break;
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
    LogF("  target %-11s glFBO=%-3d size=%dx%d | draws world=%-4d flat=%-4d",
         g_run.backbuffer ? "BACKBUFFER" : "offscreen",
         g_run.fbo, TargetWidth(), TargetHeight(), g_run.world, g_run.flat);
    ++g_traceEmitted;
    g_run.world = g_run.flat = 0;
}

void TraceDraw(bool worldPass) {
    if (!g_tracing) return;

    const bool bb = TargetIsBackbuffer();
    if (bb != g_run.backbuffer) {
        FlushTraceRun();
        GLint fbo = -1;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        g_run.backbuffer = bb;
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

// True while the video pass is being captured offscreen, so the per-eye
// machinery stands aside and lets it render plainly.
bool g_inVideoCapture = false;

// Frame on which fmvShow was last called. The engine calls it once per frame
// for as long as a video is on screen.
unsigned g_fmvFrame = 0;

bool FmvActive() {
    return g_fmvFrame != 0 && (g_frameIndex - g_fmvFrame) <= 2;
}

bool VideoOffscreenActive() {
    return Cfg().videoOffscreen && Video().valid();
}

// A pass with no uProjMatrix at all writes clip space directly and cannot be
// moved by any uniform. That is the video path.
bool IsBypassPass() {
    const int sid = VidState().shader;
    return vs_shader_in_range(sid) && Shaders()[sid].uid[0] < 0;
}

// Model-view-projection for the video quad in one eye.
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

// Read uViewMatrix back off the live GL program.
//
// The in-memory readback in the injection proves our write landed in
// mView_packed. It does NOT prove the engine uploaded it. Asking the GPU what it
// actually holds is the only way to tell a failed upload from a failed viewport,
// target or submit.
void ReadBackViewTranslation(uint32_t program, int loc, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!gl::GetUniformfv || program == 0 || loc < 0) return;
    for (int row = 0; row < 3; ++row) {
        float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        gl::GetUniformfv(program, loc + row, v);
        out[row] = v[3];
    }
}

// uProjMatrix is a real mat4, so one location returns all 16 floats.
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
// ogl_setRenderTarget -- backbuffer latch
// ---------------------------------------------------------------------------
//
// TR1-3 has no ogl_rt global, so this hook IS the render-target state. It
// reproduces the engine's own branch: arg1 == 0 means "back to FBO_default",
// anything else means an offscreen target. See Engine.cpp::NoteRenderTarget.
void __cdecl Detour_ogl_setRenderTarget(int a0, int a1, int a2, int a3) {
    g_hSetRenderTarget.Original<Fn_ogl_setRenderTarget>()(a0, a1, a2, a3);
    NoteRenderTarget(a1);
}

// ---------------------------------------------------------------------------
// vid_setPass -- pass classification
// ---------------------------------------------------------------------------
void __cdecl Detour_vid_setPass(int shader, float* params, int cull) {
    if (Cfg().logCallsites) NoteCallsite("vid_setPass", _ReturnAddress());

    // The engine has a real robustness gap here: vid_setPass assigns
    // vid_state.shader = shader BEFORE its range check, and an out-of-range id
    // then skips all configuration and falls through, leaving validate_draw to
    // index shaders[] out of bounds. We do not synthesise pass ids, but since we
    // are already in the path it costs nothing to notice.
    if (!vs_shader_in_range(shader)) {
        LogF("setPass: out-of-range shader id %d (engine would index shaders[] "
             "OOB; the array is %d entries)", shader, kShaderCount);
    }

    g_hSetPass.Original<Fn_vid_setPass>()(shader, params, cull);

    // After the original runs, vid_state.proj points at mProj[0] for the 2D
    // ortho layer and at mProj[1] for world-space 3D. Some passes never assign
    // proj at all and inherit whatever the previous pass left, so reading the
    // pointer after the call is the only correct way to classify -- a static
    // table of shader ids would get the sticky ones wrong.
    g_worldPass = IsWorldPass();
}

// ---------------------------------------------------------------------------
// validate_draw -- per-eye matrix injection
// ---------------------------------------------------------------------------
void __cdecl Detour_validate_draw() {
    DumpDrawState();

    // Read the world/2D classification LIVE, from vid_state.proj, rather than
    // trusting the flag cached by the vid_setPass hook. vid_setPass is called
    // once to configure a pass and then many draws follow, and anything that
    // repoints vid_state.proj in between (vid_setOrtho3D does exactly that)
    // leaves the cached flag stale.
    const bool worldPass = IsWorldPass();
    if (worldPass != g_worldPass) ++g_classifyMismatch;
    NoteProjPointer(VidState().proj);

    TraceDraw(worldPass);

    // ...and the pointer is still not the whole truth. vid_setOrtho3D copies
    // mProj[0] -- the ORTHO matrix -- into mProj[1] and repoints vid_state.proj
    // at it, so a pass that is orthographic arrives here indistinguishable from
    // world space by pointer alone, and handing it a per-eye perspective frustum
    // would divide an ortho layout by a depth it was never built for. So
    // classify by CONTENT as well: e32/e33 tell the two projections apart.
    //
    // Unlike TR4-6, vid_setOrtho3D here is a real 78-byte function that the
    // game does call, so this path is expected to fire rather than being
    // dead weight. The ortho3D= counter in the health report says whether it
    // actually does.
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
                     && (Cfg().monoTracking || TargetIsBackbuffer());

    // Count every world-space draw, injected or not, and separately the ones
    // rejected purely because the engine was drawing offscreen. Those two
    // numbers are what the health report needs to say something falsifiable.
    if (worldPass && !ortho3D) {
        ++g_worldDraws;
        if (!TargetIsBackbuffer()) {
            ++g_worldOffscreen;
            if (!inject && VrLive() && VR().poseValid() && !Cfg().monoTracking) {
                ++g_worldOffscreen;
            }
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
            // translation alone, which makes it stretch off-axis.
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

            // Engine camera looks down -Z (ogl_setPersp writes e32 = -1).
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

    // The ortho-3D layer (the inventory ring), left with the engine's own ortho
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

            if (Cfg().ortho3DLockToHead) {
                // Ortho output has w == 1 and no perspective divide, so moving
                // m[12] is a pure convergence shift: it changes where the layer
                // fuses and nothing else. Every relative x, y and z survives it
                // untouched.
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
                // near/far. mProj[1] holds the ortho matrix right now, so there
                // is no scene near/far left to read, and a tight bracket gives
                // this thin layer the depth resolution not to z-fight.
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

                const mat4 E = AffineToMat4(VR().EyeView(oe));
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

            if (Cfg().hudLockToHead) {
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

                const mat4 E = AffineToMat4(VR().EyeView(hudEye));
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

    // vid_state.proj should be &mProj[1] here and vid_state.view should be
    // &mView_packed, but both are pointers the engine is free to repoint, so
    // work through them rather than assuming.
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

    // First person puts the camera in Lara's head, so the tracked head
    // translation is hers, not the player's -- see Config.h.
    Affine eyeXform = VR().EyeView(
        eye, !FirstPersonActive() || Cfg().firstPersonHeadTranslation);
    if (skyInfinity) {
        // Rotation only. IPD and head translation are what give the dome a
        // finite stereo depth; looking around is the rotation, and that stays.
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
        // and clobbering them would break fog and depth precision.
        float zn = 0.0f, zf = 0.0f;
        if (!ExtractNearFar(*livePr, zn, zf)) {
            zn = 16.0f;
            zf = 32768.0f;
        }
        if (Cfg().nearClip > 0.0f) zn = Cfg().nearClip;
        if (Cfg().farClip  > 0.0f) zf = Cfg().farClip;

        VR().EyeProjection(eye, zn, zf, eyeProj);
        std::memcpy(livePr, &eyeProj, sizeof(mat4));
    }

    // finalView = eyeView * gameView, both as row-major 3x4 affines.
    const Affine gameView  = ReadPackedView(g_savedView);
    Affine finalView = Cfg().perEyeView
                     ? Mul(eyeXform, gameView)
                     : gameView;

    // Diagnostic: swing the right eye's view by a large, unmistakable angle.
    if (Cfg().debugEyeYawDegrees != 0.0f && eye == Eye::Right) {
        finalView = Mul(RotY(Cfg().debugEyeYawDegrees), finalView);
    }

    // Move the per-eye TRANSLATION out of the view matrix and into the model
    // matrix, where both shader families read it. The rotation stays in the
    // view matrix -- rotation-only shaders honour that.
    //
    // A shift d in view space is a shift R^T * d in the model matrix's space,
    // because those shaders compute R * p and we need R * (p + R^T d) = R p + d.
    g_modelPatched = false;
    float dView[3] = { 0.0f, 0.0f, 0.0f };
    if (Cfg().eyeOffsetMode != 0 && Cfg().eyeOffsetMode != 3) {
        dView[0] = finalView.r[0][3] - gameView.r[0][3];
        dView[1] = finalView.r[1][3] - gameView.r[1][3];
        dView[2] = finalView.r[2][3] - gameView.r[2][3];

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
    // Every shader ends with uProjMatrix * vec4(viewpos, 1.0), so this reaches
    // paths the view and model matrices cannot -- skinned geometry included.
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

    // Re-apply the engine's own projection OFFSET, last and innermost.
    //
    // EXPECTED TO BE INERT ON THIS GAME. TR4-6 had vid_setPerspOffset, which
    // wrote e02/e12 to place elements through the projection; TR1-3 has no such
    // function, and ogl_setPersp explicitly zeroes mProj[1].e02/.e12 (verified
    // at RVA 0x0000FAD0). The guard below therefore never fires in practice.
    //
    // It is kept anyway because it is guarded on the shear being non-zero, so it
    // costs one compare per injected draw, and because if some pass ever does
    // carry a shear the correct handling is subtle: the shear must multiply the
    // ENGINE's v.z, not (E*v).z, so it has to be applied as a camera-space skew
    // AFTER the per-eye transform, and converted angle-to-angle by dividing
    // through the engine's own m00/m11 rather than copied as raw NDC.
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
            LogF("projoffset: shader %d carries a projection shear (%.4f, %.4f) -- "
                 "unexpected on TR1-3, which has no vid_setPerspOffset. "
                 "Re-applied as a camera-space skew (%.4f, %.4f).",
                 vs.shader, g_savedProj.m[8], g_savedProj.m[9], a, b);
        }
    }

    // Force the uploads. The engine's own dirty tracking would skip them: the
    // proj bit is set on a POINTER comparison in vid_setPass, so writing new
    // contents into the same mProj[1] would not trip it.
    if (Cfg().eyeOffsetMode != 3) vs.consts |= kView;
    if (doProj) vs.consts |= kProj;

    const int eyeIdx = (g_currentEye == 0) ? 0 : 1;
    ++g_injectCount[eyeIdx];
    if (Cfg().verboseFirstFrame && !g_loggedEye[eyeIdx]) {
        g_loggedEye[eyeIdx] = true;
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

        LogF("inject: eye=%d packed view translation IN MEMORY = (%.1f, %.1f, %.1f) "
             "[floats 3,7,11 at %p]",
             eyeIdx, liveVw->m[3], liveVw->m[7], liveVw->m[11], (void*)liveVw);
        if (vs_shader_in_range(vs.shader)) {
            LogF("inject: eye=%d uid[1] (uViewMatrix location) = %d, shader=%d",
                 eyeIdx, Shaders()[vs.shader].uid[1], vs.shader);
        }
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
    // Sky draws deliberately share a translation-free E, so sampling one would
    // report zero separation and look like an injection failure.
    if (g_verifyArmed && !g_verifyDone[eyeIdx] && vs_shader_in_range(vs.shader)
        && !skyInfinity) {
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

    // Put the engine's matrices back. It never observes the substitution, so the
    // next pass composes from pristine state rather than compounding.
    if (Cfg().eyeOffsetMode != 3) {
        std::memcpy(liveVw, &g_savedView, sizeof(mat4));
    }
    if (doProj || Cfg().eyeOffsetMode >= 2) {
        std::memcpy(livePr, &g_savedProj, sizeof(mat4));
    }
}

// ---------------------------------------------------------------------------
// ogl_draw -- per-eye duplication
// ---------------------------------------------------------------------------
//
// Every draw is issued twice, into the two halves of the double-wide target.
// Matrix injection is gated on the pass being world-space, so 2D/HUD geometry is
// drawn twice at the same screen position unless the HUD panel path moved it.
//
// TR1-3 has a single draw entry point, so unlike the TR4-6 mod this is not a
// template over two hooks.
void __cdecl Detour_ogl_draw(void* mesh, unsigned firstIndex, unsigned count) {
    if (Cfg().logCallsites) NoteCallsite("ogl_draw", _ReturnAddress());

    // Push sky fragments to the far plane so a dome of finite mesh radius
    // cannot occlude distant world geometry. Restored after the draw; the
    // engine's own depth range is left untouched for every other pass.
    GLfloat oldDepthRange[2] = { 0.0f, 1.0f };
    const bool skyDepth = SkyInfinity() && VrLive();
    if (skyDepth) {
        glGetFloatv(GL_DEPTH_RANGE, oldDepthRange);
        glDepthRange(1.0, 1.0);
    }

    if (!VrLive() || g_inDuplicate || Cfg().monoTracking
        || !Cfg().duplicateDraws || !TargetIsBackbuffer()) {
        g_hDraw.Original<Fn_ogl_draw>()(mesh, firstIndex, count);
        if (skyDepth) glDepthRange(oldDepthRange[0], oldDepthRange[1]);
        return;
    }

    ++g_dupCount;

    // Clip-space-direct passes (video) cannot be moved by any matrix, so capture
    // the draw once offscreen and replay it as real geometry per eye. That is
    // the only way to get correct keystone and roll.
    if (VideoOffscreenActive() && IsBypassPass()) {
        GLint prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

        g_inVideoCapture = true;
        Video().BeginCapture();
        g_hDraw.Original<Fn_ogl_draw>()(mesh, firstIndex, count);
        g_inVideoCapture = false;
        if (skyDepth) glDepthRange(oldDepthRange[0], oldDepthRange[1]);

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
        g_hDraw.Original<Fn_ogl_draw>()(mesh, firstIndex, count);
    }
    g_currentEye = 0;
    g_inDuplicate = false;
    if (skyDepth) glDepthRange(oldDepthRange[0], oldDepthRange[1]);
}

// ---------------------------------------------------------------------------
// fmvShow -- "a video is on screen this frame"
// ---------------------------------------------------------------------------
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
    // GL_FRAMEBUFFER_BINDING, and ogl_setRenderTarget binds it whenever the game
    // asks to go back to the screen (verified at RVA 0x000105AA). Overwriting it
    // redirects every "back to the screen" in the engine into the stereo target.
    g_realDefaultFbo = FboDefault();
    FboDefault() = Stereo().fbo();
    LogF("present: redirected FBO_default %u -> %u", g_realDefaultFbo, Stereo().fbo());

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
    ++g_frameIndex;

    // Which of the three games is running. gGame only becomes meaningful once a
    // title has been selected, so this is reported when it changes rather than
    // once at startup.
    {
        const int g = CurrentGame();
        if (g != g_loggedGame && g >= 0 && g <= 2) {
            g_loggedGame = g;
            LogF("engine: now running %s (gGame=%d)", CurrentGameName(), g);
        }
    }

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

    // Re-resolve tomb1/2/3.dll every frame. The three are loaded and unloaded as
    // the player moves between games from the title screen, so this is not a
    // one-time bind.
    GameDllUpdate();

    // Install or drop the culling hooks to match. Must follow GameDllUpdate:
    // it hooks INSIDE the game DLL, so it needs to know which one is live.
    PortalCullUpdate();
    SkyUpdate();
    FirstPersonUpdate();

    // Periodic health report, in deltas. A one-shot report at a fixed frame only
    // ever samples the menus, where almost everything legitimately is 2D.
    if (VrLive() && g_frameIndex >= g_lastReportFrame + 1800) {
        const unsigned dDup   = g_dupCount        - g_prevDup;
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

        // Against total world draws, not against duplications: injection and
        // duplication are gated on the SAME condition, so dividing one by the
        // other would read 100% by construction and could never detect the
        // failure this warns about.
        const unsigned pct = dWorld ? (100u * (dInj0 + dInj1) / dWorld) : 0u;

        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (g_lastReportTime.QuadPart != 0 && freq.QuadPart != 0) {
            const double secs = double(now.QuadPart - g_lastReportTime.QuadPart)
                              / double(freq.QuadPart);
            if (secs > 0.0) LogF("perf: %.1f fps rendered", 1800.0 / secs);
        }
        g_lastReportTime = now;

        LogF("stereo health @frame %u [%s]: world draws=%u  duplicated=%u  "
             "injected eye0=%u eye1=%u  (%u%% of world draws got per-eye matrices)  "
             "offscreen-skipped=%u  classify-mismatch=%u  ortho3D=%u  sky=%u  "
             "projoffset=%u",
             g_frameIndex, CurrentGameName(), dWorld, dDup, dInj0, dInj1, pct,
             dOff, g_classifyMismatch, dO3D, dSky, dOfs);

        // What the GPU actually held, per eye. This is the load-bearing line: if
        // the two translations match, the per-eye view never reached the shader
        // and nothing downstream can produce depth. If they differ, the matrices
        // are on the GPU and the fault is viewport, target or submit.
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
        // and the clear both happen later in this function.
        if (VrLive() && !Cfg().monoTracking) {
            int samples = 0, differing = 0;
            Stereo().CompareHalves(samples, differing);
            // NOT a depth measure. It compares the SAME pixel coordinate in both
            // halves, so it reports image SHIFT: a large constant offset (the
            // frustum shear) lights it up, while correct parallax of a few pixels
            // across smooth texture falls under the threshold and reads as zero.
            // A low number here means nothing is wrong.
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

    if (Cfg().eyeMarkers) Stereo().MarkEyes();

    // Hand both halves to the compositor before the swap so the runtime gets the
    // frame as early as possible.
    uint32_t w = 0, h = 0;
    VR().GetEyeSize(w, h);
    VR().Submit(Stereo().texture(), w, h);

    if (Cfg().mirrorToWindow) {
        Stereo().MirrorToWindow(ScreenWidth(), ScreenHeight(), g_realDefaultFbo);
    }

    // The original swaps the window. Point FBO_default back at the real window
    // for the duration so anything inside it that touches the default
    // framebuffer behaves.
    FboDefault() = g_realDefaultFbo;
    g_hPresent.Original<Fn_ogl_present>()();
    FboDefault() = Stereo().fbo();

    // Latch poses for the frame we are about to render, then re-arm the target.
    VR().BeginFrame();
    Stereo().BeginFrame();

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
    };

    const Target targets[] = {
        { &g_hSetPass,         L().vid_setPass,
          reinterpret_cast<void*>(&Detour_vid_setPass),
          6,  kSetPassPrologue,  sizeof(kSetPassPrologue),  "vid_setPass"    },
        { &g_hValidateDraw,    L().validate_draw,
          reinterpret_cast<void*>(&Detour_validate_draw),
          5,  kValidatePrologue, sizeof(kValidatePrologue), "validate_draw"  },
        { &g_hDraw,            L().ogl_draw,
          reinterpret_cast<void*>(&Detour_ogl_draw),
          5,  kDrawPrologue,     sizeof(kDrawPrologue),     "ogl_draw"       },
        { &g_hPresent,         L().ogl_present,
          reinterpret_cast<void*>(&Detour_ogl_present),
          5,  kPresentPrologue,  sizeof(kPresentPrologue),  "ogl_present"    },
        { &g_hFmvShow,         L().fmvShow,
          reinterpret_cast<void*>(&Detour_fmvShow),
          5,  kFmvShowPrologue,  sizeof(kFmvShowPrologue),  "fmvShow"        },
        { &g_hSetRenderTarget, L().ogl_setRenderTarget,
          reinterpret_cast<void*>(&Detour_ogl_setRenderTarget),
          13, kSetRtPrologue,    sizeof(kSetRtPrologue),    "ogl_setRenderTarget" },
    };

    bool allOk = true;
    for (const Target& t : targets) {
        // No RIP-relative displacements occur inside any of these stolen-byte
        // windows, so no fixup list is passed. See the prologue table above.
        if (!t.hook->Install(Fn(t.rva), t.detour, t.stolen, t.expect, t.expectLen,
                             t.name, nullptr, 0))
            allOk = false;
    }

    if (!allOk) {
        Log("hooks: at least one hook failed -- rolling all of them back");
        RemoveHooks();
        return false;
    }

    Log("hooks: all six installed");
    return true;
}

void RemoveHooks() {
    GamepadShutdown();
    FirstPersonShutdown();
    SkyShutdown();
    PortalCullShutdown();
    GameDllShutdown();

    // Reverse order of installation.
    g_hSetRenderTarget.Remove();
    g_hFmvShow.Remove();
    g_hPresent.Remove();
    g_hDraw.Remove();
    g_hValidateDraw.Remove();
    g_hSetPass.Remove();

    if (g_ready && g_realDefaultFbo != 0) {
        FboDefault() = g_realDefaultFbo;
    }
    g_ready = false;
}

} // namespace tr
