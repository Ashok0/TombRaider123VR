#include "DynamicBones.h"
#include "BoneSkin.h"
#include "GameDll.h"
#include "Engine.h"
#include "Config.h"
#include "InlineHook.h"
#include "Log.h"

#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace tr {
namespace {

// --- Lara's airborne state, read straight from the engine ---------------------
//
// Verified against all three PDBs and retail instruction operands.
constexpr uint32_t kItemFallspeed    = 36;
constexpr uint32_t kItemYRot         = 88 + 14;
constexpr uint32_t kItemFlags        = 484; // uint16 bitfield
constexpr uint32_t kGravityStatusBit = 3;

// Game logic ticks at 30 Hz, and fallspeed is units per tick.
constexpr float kTicksPerSecond = 30.0f;

// --- the hook ---------------------------------------------------------------

hook::InlineHook g_hDrawLaraHD;

// DrawLaraHD takes one pointer in rcx. We only need the scope, not the
// argument, but the signature has to match the call or the stack is wrong on
// the way back through the trampoline.
typedef void (__fastcall* Fn_DrawLaraHD)(void*);

const GameDllLayout* g_boundDll   = nullptr;
uint64_t             g_boundBase  = 0;
uint64_t             g_failedBase = 0;

bool g_inLara      = false;
bool g_loggedFirst = false;

// --- the joint palette ------------------------------------------------------
//
// A joint is 3 vec4s -- glUniform4fv(loc, 96, ...) -- so 12 floats,
// row-major 3x4, translation at [3], [7], [11]. Same packing as view and
// model; NOT the column-major layout the projection uses.

constexpr int kMaxJoints = 32;    // fixed GPU upload: 96 vec4s
constexpr int kLaraJoints = 15;   // canonical skeleton, excluding upload padding

struct Joint { float m[12]; };

// DrawLaraHD also draws hands, face, and attachments. Preserve the TR4/5
// discriminator: a body has many distinct joint transforms, while attachments
// repeat a few. TR1-3 always uploads 32 slots, so score only Lara's 15 skeleton
// slots; stale/padded slots 15..31 must never win the comparison.
struct Capture {
    Joint joints[kMaxJoints];
    int   count    = 0;
    int   shader   = -1;
    int   distinct = 0;
    float extent   = 0.0f;
};

Capture g_best;          // best-scoring draw of the frame
bool    g_haveBest = false;
int     g_bestScore = -1;

// Every draw scored this frame, so the log shows the competition rather than
// only the winner. Three heuristics have now been wrong; seeing the field is
// what caught each one.
struct Candidate { int joints; int distinct; float extent; int shader; };
constexpr int kMaxCandidates = 16;
Candidate g_cand[kMaxCandidates] = {};
int       g_candCount = 0;

// Per-scope and per-frame bookkeeping.
bool     g_scopeLatched   = false;
unsigned g_scopeDraws     = 0;
int      g_scopeJointsMin = 0;
int      g_scopeJointsMax = 0;

constexpr int kMaxScopes = 8;
int      g_frameScopeJoints[kMaxScopes] = {};
int      g_frameScopes = 0;

// The joint the solver is anchored to, latched from the last scope of the
// frame.
bool  g_haveThis  = false;
bool  g_havePrev  = false;
Joint g_torsoThis = {};
Joint g_torsoPrev = {};

int   g_numJoints      = 0;
int   g_reportedJoints = -1;
int   g_laraShader     = -1;

// --- solver state -----------------------------------------------------------
//
// THE FORMULATION CHANGED, AND THE FIRST ONE WAS WRONG.
//
// v1 put a point mass in world space and had a spring chase an anchor the
// torso carried around. That lags under CONSTANT VELOCITY, which is not what
// secondary motion should do -- a bone does not sag because Lara is running
// at a steady speed. Worse, it lags by c*V/k, and at the v1 constants
// (c=12, k=180) with Lara running at roughly 2400 units/s that is 160 units
// against an 18-unit clamp. Which is exactly what the log showed: disp_peak
// pinned at 18.00 in every report of both sessions, never once settling.
//
// v2 solves in the PARENT'S LOCAL FRAME with the parent's acceleration as the
// forcing term:
//
//     x'' = -k*x - c*x' - a_parent_local + g_local
//
// Constant velocity gives a_parent = 0 and the bone sits at its gravity
// equilibrium, which is correct. Only acceleration and rotation move it,
// which is the effect. x is already in the parent's frame, so it needs no
// conversion on the way out.
struct Bone {
    float x[3];        // displacement from rest, in the parent's local frame
    float v[3];        // its rate of change, same frame
    float anchor[3];   // last world anchor, for the finite difference
    float vel[3];      // last world anchor velocity
    float accS[3];     // smoothed acceleration -- see the EMA in Solve
    bool  live;
};

Bone g_bone[2] = {};
bool g_settled = false;

// --- pending restore for the debug displacement ------------------------------
//
// Apply edits the engine's joint array in place; these remember exactly what
// to put back. A single slot is enough because apply and restore bracket one
// validate_draw call.
float* g_patchedAt = nullptr;      // &joints[torso * 12], or null
float  g_patchedSaved[12] = {};
bool   g_appliedThisDraw = false;
int    g_level = -1;
uint64_t g_item = 0;

//// True when the draw currently being observed looked like the body.
bool g_drawIsBody = false;

// The same test WITHOUT the solver lock -- what the shader path deforms.
bool g_drawRenderBody = false;

// THE LATCH HAS TO STAY ON ONE DRAW.
//
// Several draws per frame carry a body-shaped palette -- the measured run
// alternated shader 12 and shader 20 between frames -- and they are not the
// same transform. Differencing across a flip fabricates acceleration: the log
// showed peaks of 285k to 1.9M units per second squared against a gravity of
// 5400, which saturated the clamp and fired the teleport guard 53 times.
//
// So the first body draw seen wins and the latch stays on its shader for as
// long as that shader keeps appearing. Continuity matters more than picking
// the theoretically best candidate, because the solver differentiates twice
// and only a continuous signal survives that.
int  g_lockShader = -1;
bool g_lockSeen   = false;
int  g_lockMiss   = 0;

// Frames the locked shader may be absent before the lock is given up -- a
// level change or an outfit swap, not a frame Lara happened not to draw in.
constexpr int kLockGraceFrames = 120;

/// --- engine-state drive -------------------------------------------------------
//
// WHY THIS REPLACED THE ACCELERATION DRIVE.
//
// The acceleration drive worked out jumps from the torso matrix differentiated
// twice, then had to smooth that and deadzone it to survive the noise. Those
// two filters compound on short events, and a landing is exactly a short
// event. Simulated through the real pipeline, a 16000 landing spike delivered
// NOTHING if it fell inside one frame and 5250 if it spread across three --
// so whether a jump registered depended on how the impact happened to land on
// frame boundaries. That is the "only sometimes" that was reported.
//
// The engine already knows when Lara is airborne. In the torso's own frame the
// bone feels gravity MINUS the torso's acceleration: standing, that is full
// gravity, so it sags; in the air the torso is itself in freefall, so the bone
// goes weightless. The jiggle is simply the step between those two states --
// takeoff releases the sag, landing slams it back -- and an underdamped spring
// rings on both. Nothing is differentiated, so nothing needs filtering, and
// every jump produces the same response. Walking never sets gravity_status,
// so it cannot bounce.
bool     g_airKnown    = false;
bool     g_airPrev     = false;
int      g_fallMax     = 0;      // deepest fallspeed of the current airborne spell
unsigned g_jumps       = 0;
unsigned g_lands       = 0;
int      g_lastLandFall = 0;

// Report accumulators.
unsigned g_frames    = 0;
unsigned g_dupFrames = 0;
unsigned g_snaps     = 0;
float    g_peakDisp  = 0.0f;
float    g_peakDrive = 0.0f;
float    g_peakAccel = 0.0f;
float    g_peakUsed  = 0.0f;   // drive after smoothing, ceiling and deadzone
LARGE_INTEGER g_lastTick = {};

// --- small affine helpers ---------------------------------------------------

// False when there is no Lara to read -- no level, a menu, a DLL mid-swap.
bool ReadLaraAir(bool& airborne, int& fallspeed) {
    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();
    if (!d || !base || d->laraItem == 0) return false;
    const uint64_t item = *reinterpret_cast<uint64_t*>(base + d->laraItem);
    if (!item) return false;
    const uint32_t flags = *reinterpret_cast<uint16_t*>(item + kItemFlags);
    airborne  = ((flags >> kGravityStatusBit) & 1u) != 0;
    fallspeed = *reinterpret_cast<int16_t*>(item + kItemFallspeed);
    return true;
}

void XformPoint(const Joint& M, const float p[3], float out[3]) {
    out[0] = M.m[0] * p[0] + M.m[1] * p[1] + M.m[2]  * p[2] + M.m[3];
    out[1] = M.m[4] * p[0] + M.m[5] * p[1] + M.m[6]  * p[2] + M.m[7];
    out[2] = M.m[8] * p[0] + M.m[9] * p[1] + M.m[10] * p[2] + M.m[11];
}

// Rotate a WORLD vector into the joint's local frame. Transpose, which is the
// inverse only while the rotation stays orthonormal.
void UnrotateVector(const Joint& M, const float w[3], float out[3]) {
    out[0] = M.m[0] * w[0] + M.m[4] * w[1] + M.m[8]  * w[2];
    out[1] = M.m[1] * w[0] + M.m[5] * w[1] + M.m[9]  * w[2];
    out[2] = M.m[2] * w[0] + M.m[6] * w[1] + M.m[10] * w[2];
}

// Rotate a LOCAL vector out into world. The plain rotation, not the
// transpose UnrotateVector uses.
void RotateVector(const Joint& M, const float l[3], float out[3]) {
    out[0] = M.m[0] * l[0] + M.m[1] * l[1] + M.m[2]  * l[2];
    out[1] = M.m[4] * l[0] + M.m[5] * l[1] + M.m[6]  * l[2];
    out[2] = M.m[8] * l[0] + M.m[9] * l[1] + M.m[10] * l[2];
}

// Collapse a vector onto one unit axis, discarding everything perpendicular.
void ProjectOnto(float v[3], const float axis[3]) {
    const float d = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
    v[0] = axis[0] * d;
    v[1] = axis[1] * d;
    v[2] = axis[2] * d;
}

float Len3(const float v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

float RotDet(const Joint& M) {
    return M.m[0] * (M.m[5] * M.m[10] - M.m[6] * M.m[9])
         - M.m[1] * (M.m[4] * M.m[10] - M.m[6] * M.m[8])
         + M.m[2] * (M.m[4] * M.m[9]  - M.m[5] * M.m[8]);
}

// --- the hook ---------------------------------------------------------------

void __fastcall Detour_DrawLaraHD(void* item) {
    g_inLara         = true;
    g_scopeLatched   = false;
    g_scopeDraws     = 0;
    g_scopeJointsMin = 0;
    g_scopeJointsMax = 0;
    g_hDrawLaraHD.Original<Fn_DrawLaraHD>()(item);
    g_inLara = false;

    if (g_frameScopes < kMaxScopes)
        g_frameScopeJoints[g_frameScopes] = g_scopeJointsMax;
    ++g_frameScopes;
}

// All six supported DLL rows share these three complete push instructions.
const uint8_t kDrawLaraHDPrologue[] = { 0x40, 0x53, 0x41, 0x55, 0x41, 0x57 };

bool Install(const GameDllLayout& d, uint64_t base) {
    if (d.drawLaraHD == 0) return false;
    return g_hDrawLaraHD.Install(
        reinterpret_cast<void*>(base + d.drawLaraHD),
        reinterpret_cast<void*>(&Detour_DrawLaraHD),
        6, kDrawLaraHDPrologue, sizeof(kDrawLaraHDPrologue), "DrawLaraHD");
}

void ResetSolver() {
    std::memset(g_bone, 0, sizeof(g_bone));
    g_haveThis = g_havePrev = false;
    g_settled  = false;
    g_numJoints = 0;
    g_reportedJoints = -1;
    g_laraShader = -1;
    g_frames = g_dupFrames = g_snaps = 0;
    g_peakDisp = g_peakDrive = g_peakAccel = g_peakUsed = 0.0f;
    g_lastTick.QuadPart = 0;
    g_scopeLatched = false;
    g_scopeDraws = 0;
    g_scopeJointsMin = g_scopeJointsMax = 0;
    g_frameScopes = 0;
    g_haveBest = false;
    g_bestScore = -1;
    g_candCount = 0;
    g_lockShader = -1;
    g_lockSeen = false;
    g_lockMiss = 0;
    g_airKnown = g_airPrev = false;
    g_fallMax = 0;
    g_jumps = g_lands = 0;
    g_lastLandFall = 0;
}

void Remove() {
    // Never leave the engine's joint array holding our edit. Inlined rather
    // than calling DynamicBonesRestoreDraw, which is defined further down.
    if (g_patchedAt) {
        std::memcpy(g_patchedAt, g_patchedSaved, sizeof(g_patchedSaved));
        g_patchedAt = nullptr;
    }
    g_inLara = false;
    g_hDrawLaraHD.Remove();
    g_boundDll  = nullptr;
    g_boundBase = 0;
    g_level = -1;
    g_item = 0;
    ResetSolver();
    BoneSkinResetRegion();
}

// --- census ------------------------------------------------------------------

// How far apart the translations in a palette sit, and how many of its slots
// are genuinely distinct. A palette mostly filled with one repeated matrix is
// an attachment; a body spans hundreds of units and repeats little.
// Scored straight off the uploaded array, before it is copied anywhere: n is
// at most 15, so this is a few hundred float compares per draw.
//
// `distinct` counts joint origins separated by more than DynamicBonesSeparation
// world units. The threshold matters more than it looks: at the 0.05 units
// this used first, slots differing by under a unit -- padding jitter, not
// anatomy -- each counted as their own joint, which scored a 33-slot palette
// holding 13 identical fillers at "21 distinct" and let it beat a real
// 15-of-15 skeleton. Bones on a body are tens of units apart; filler is not.
void Summarise(const float* joints, int n, float& extent, int& distinct) {
    float lo[3] = {  1e30f,  1e30f,  1e30f };
    float hi[3] = { -1e30f, -1e30f, -1e30f };
    distinct = 0;
    for (int j = 0; j < n; ++j) {
        const float* m = joints + j * 12;
        const float t[3] = { m[3], m[7], m[11] };
        for (int a = 0; a < 3; ++a) {
            if (t[a] < lo[a]) lo[a] = t[a];
            if (t[a] > hi[a]) hi[a] = t[a];
        }
        const float sep = Cfg().dynamicBonesSeparation;
        bool seen = false;
        for (int k = 0; k < j && !seen; ++k) {
            const float* p = joints + k * 12;
            seen = std::fabs(p[3]  - t[0]) < sep
                && std::fabs(p[7]  - t[1]) < sep
                && std::fabs(p[11] - t[2]) < sep;
        }
        if (!seen) ++distinct;
    }
    extent = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const float e = hi[a] - lo[a];
        if (e > extent) extent = e;
    }
}

void LogCensus() {
    if (!g_haveBest) return;

    for (int i = 0; i < g_candCount; ++i) {
        const Candidate& q = g_cand[i];
        LogF("dynbones:   candidate %2d -- %2d joints, %2d separated, spread "
             "%4.0f units, shader %d%s", i, q.joints, q.distinct, q.extent,
             q.shader, (q.distinct == q.joints) ? "   FULL" : "");
    }

    LogF("dynbones: body draw -- %d joints, %d separated, spread %.0f units, "
         "shader %d. Joint origins follow; the torso is the one the head and "
         "both shoulders hang off.",
         g_best.count, g_best.distinct, g_best.extent, g_best.shader);
    for (int j = 0; j < g_best.count; ++j) {
        const float* m = g_best.joints[j].m;
        LogF("dynbones:   [%2d] t=(%9.1f,%9.1f,%9.1f) "
             "r0=(%6.3f,%6.3f,%6.3f) r1=(%6.3f,%6.3f,%6.3f) r2=(%6.3f,%6.3f,%6.3f)",
             j, m[3], m[7], m[11],
             m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    }
}

// --- the solver -------------------------------------------------------------

void Solve(float dt) {
    if (!g_haveThis) return;

    const Config& c = Cfg();
    const Joint&  M = g_torsoThis;

    const float det = RotDet(M);
    if (det < 0.90f || det > 1.10f) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LogF("dynbones: joint %d rotation determinant is %.3f, not ~1. "
                 "Either the index is wrong or the frame carries a scale; "
                 "local-space output from here is not trustworthy.",
                 c.dynamicBonesTorsoJoint, det);
        }
    }

    const float rest[2][3] = {
        { -c.dynamicBonesAnchorX, c.dynamicBonesAnchorY, c.dynamicBonesAnchorZ },
        { +c.dynamicBonesAnchorX, c.dynamicBonesAnchorY, c.dynamicBonesAnchorZ },
    };

    // Engine-state drive: read whether Lara is in the air, and turn the
    // grounded/airborne edges into jump and landing events.
    const bool engineMode = (c.dynamicBonesDriveMode == 1);
    float gravityScale = 1.0f;
    float landImpulse  = 0.0f;     // units/s, applied down the vertical axis
    if (engineMode) {
        bool air = false;
        int  fall = 0;
        if (ReadLaraAir(air, fall)) {
            if (g_airKnown && !g_airPrev && air) {
                ++g_jumps;
                g_fallMax = 0;
            }
            if (air && fall > g_fallMax) g_fallMax = fall;
            if (g_airKnown && g_airPrev && !air) {
                ++g_lands;
                // The DEEPEST fallspeed of the spell, not the value on the
                // landing tick: the engine zeroes fallspeed as it lands, so
                // reading it on the edge would hand every landing an impulse
                // of nothing -- the same inconsistency this mode exists to
                // remove.
                g_lastLandFall = g_fallMax;
                landImpulse = static_cast<float>(g_fallMax) * kTicksPerSecond
                            * c.dynamicBonesLandImpulse;
                g_fallMax = 0;
            }
            g_airPrev  = air;
            g_airKnown = true;
            if (air) gravityScale = c.dynamicBonesAirGravity;
        }
    }

    // Gravity is along world Y; bring it into the parent's frame once. In
    // engine mode its strength follows the airborne state -- see g_airKnown.
    const float gWorld[3] = { 0.0f, c.dynamicBonesGravity * gravityScale, 0.0f };
    float gLocal[3];
    UnrotateVector(M, gWorld, gLocal);

    // World down in the joint's frame, for the landing impulse. Needed even
    // when the axis constraint is off.
    float downLocal[3];
    {
        const float downWorld[3] = { 0.0f, 1.0f, 0.0f };
        UnrotateVector(M, downWorld, downLocal);
        const float len = Len3(downLocal);
        if (len > 1e-6f) {
            downLocal[0] /= len; downLocal[1] /= len; downLocal[2] /= len;
        }
    }

    // CONSTRAIN TO WORLD VERTICAL.
    //
    // The anchor sits 34 units off centre and 45 forward of the joint origin,
    // so when the torso ROTATES that point swings through an arc -- and an arc
    // is acceleration. Turning therefore drove a large sideways force, which
    // in the debug view threw the whole torso left and right on the stick.
    // Physically that is what an unconstrained bone does; it is not what is
    // wanted here.
    //
    // Projecting onto world vertical removes it at the source rather than
    // hiding it afterwards: the perpendicular components never enter the
    // state, so no lateral energy accumulates and nothing has to bleed off.
    // Gravity is untouched -- it already points along this exact axis.
    //
    // The axis is computed in the joint's frame each step, so it stays world
    // vertical however Lara is leaning, rather than drifting with her torso.
    float axis[3] = { 0.0f, 0.0f, 0.0f };
    bool  constrain = (c.dynamicBonesAxis == 1);
    if (constrain) {
        const float upWorld[3] = { 0.0f, 1.0f, 0.0f };
        UnrotateVector(M, upWorld, axis);
        const float len = Len3(axis);
        if (len > 1e-6f) {
            axis[0] /= len; axis[1] /= len; axis[2] /= len;
        } else {
            constrain = false;
        }
    }

    for (int i = 0; i < 2; ++i) {
        float anchor[3];
        XformPoint(M, rest[i], anchor);

        if (!g_bone[i].live) {
            std::memcpy(g_bone[i].anchor, anchor, sizeof(anchor));
            std::memset(g_bone[i].vel,  0, sizeof(g_bone[i].vel));
            std::memset(g_bone[i].accS, 0, sizeof(g_bone[i].accS));
            for (int a = 0; a < 3; ++a) g_bone[i].x[a] = gLocal[a] / c.dynamicBonesStiffness;
            std::memset(g_bone[i].v,    0, sizeof(g_bone[i].v));
            g_bone[i].live = true;
            continue;
        }

        const float step[3] = { anchor[0] - g_bone[i].anchor[0],
                                anchor[1] - g_bone[i].anchor[1],
                                anchor[2] - g_bone[i].anchor[2] };
        const float moved = Len3(step);
        if (moved > g_peakDrive) g_peakDrive = moved;

        // SpringSystem::teleport's job: a level load, a cutscene cut or a
        // camera warp is not an acceleration, and differentiating one twice
        // gives a number that would fling the bone into the clamp and hold it
        // there for a second.
        if (moved > c.dynamicBonesTeleport) {
            std::memcpy(g_bone[i].anchor, anchor, sizeof(anchor));
            std::memset(g_bone[i].vel,  0, sizeof(g_bone[i].vel));
            std::memset(g_bone[i].accS, 0, sizeof(g_bone[i].accS));
            for (int a = 0; a < 3; ++a) g_bone[i].x[a] = gLocal[a] / c.dynamicBonesStiffness;
            std::memset(g_bone[i].v,    0, sizeof(g_bone[i].v));
            if (i == 0) ++g_snaps;
            continue;
        }

        // Two finite differences for the anchor's world acceleration.
        float vel[3], acc[3];
        for (int a = 0; a < 3; ++a) {
            vel[a] = step[a] / dt;
            acc[a] = (vel[a] - g_bone[i].vel[a]) / dt;
        }
        std::memcpy(g_bone[i].anchor, anchor, sizeof(anchor));
        std::memcpy(g_bone[i].vel,    vel,    sizeof(vel));

        float accLocal[3];
        UnrotateVector(M, acc, accLocal);
        const float accMag = Len3(accLocal);
        if (accMag > g_peakAccel) g_peakAccel = accMag;

        // SMOOTH THE ACCELERATION BEFORE ANYTHING USES IT.
        //
        // Position differentiated twice is amplified by 1/dt^2, about 2900 at
        // these frame rates, so 128 units of anchor movement in one frame --
        // an ordinary measured value once the latch stopped flipping --
        // arrives as roughly 370,000 units per second squared. Raw, that is
        // all spike and no signal.
        //
        // A jump lasts tens of frames, so the accelerations worth responding
        // to survive a low-pass easily while single-frame differentiation
        // noise does not. This is the difference between a solver driven by
        // Lara's motion and one driven by numerical debris.
        const float alpha = c.dynamicBonesDriveSmoothing;
        for (int a = 0; a < 3; ++a) {
            g_bone[i].accS[a] += (accLocal[a] - g_bone[i].accS[a]) * alpha;
            accLocal[a] = g_bone[i].accS[a];
        }

        // x'' = -k*x - c*x' - a_parent + g, integrated semi-implicitly.
        // Each of acceleration, velocity and position is projected onto the
        // constraint axis, so nothing perpendicular can survive a step.
        const float k    = c.dynamicBonesStiffness;
        const float damp = c.dynamicBonesDamping;

        // DEADZONE ON THE DRIVE ONLY.
        //
        // Walking bobs the torso a little every step, and an unfiltered solver
        // answers all of it, so she jiggled while strolling. A jump or a
        // landing accelerates the torso by an order of magnitude more than a
        // footfall does, so a threshold separates them cleanly.
        //
        // Applied to the drive term alone, never to gravity or the spring:
        // those two are what return the bone to rest, and thresholding them
        // would leave it stranded wherever the last jump put it.
        //
        // Soft knee rather than a hard gate -- subtracting the deadzone
        // instead of testing against it means the response starts from zero
        // as it crosses, so there is no visible pop at the boundary.
        float drive[3] = { accLocal[0], accLocal[1], accLocal[2] };
        if (constrain) ProjectOnto(drive, axis);
        // A hard ceiling on the drive, in case the signal jumps anyway. The
        // solver differentiates the joint matrix twice, so anything
        // discontinuous that slips past the teleport guard arrives here as a
        // spike of six or seven figures against a gravity of 5400. Clamping
        // bounds the damage to one frame instead of letting it charge the
        // spring and ring for a second afterwards.
        const float ceil = c.dynamicBonesDriveMax;
        if (ceil > 0.0f) {
            const float mag = Len3(drive);
            if (mag > ceil) {
                const float s = ceil / mag;
                drive[0] *= s; drive[1] *= s; drive[2] *= s;
            }
        }

        const float dz = c.dynamicBonesDriveDeadzone;
        if (dz > 0.0f) {
            const float mag = Len3(drive);
            if (mag <= dz) {
                drive[0] = drive[1] = drive[2] = 0.0f;
            } else {
                const float keep = (mag - dz) / mag;
                drive[0] *= keep; drive[1] *= keep; drive[2] *= keep;
            }
        }

        // Engine mode takes no differentiated drive at all -- the airborne
        // state already moved gravity, and landings arrive as an impulse
        // below. The acceleration pipeline above still runs so accel_raw keeps
        // reporting, but none of it reaches the spring.
        if (engineMode) drive[0] = drive[1] = drive[2] = 0.0f;

        // What actually reached the spring, after smoothing, ceiling and
        // deadzone. accel_peak reports the RAW figure, which stays large by
        // nature; this is the one that says whether the drive is sane.
        const float used = Len3(drive);
        if (used > g_peakUsed) g_peakUsed = used;

        float accel[3];
        for (int a = 0; a < 3; ++a) {
            accel[a] = -k * g_bone[i].x[a]
                       - damp * g_bone[i].v[a]
                       - drive[a] * c.dynamicBonesDriveScale
                       + gLocal[a];
        }
        if (constrain) ProjectOnto(accel, axis);

        for (int a = 0; a < 3; ++a) g_bone[i].v[a] += accel[a] * dt;

        // Landing: the torso stops dead and the bone carries on downward.
        // Applied as a velocity kick rather than an acceleration so it lands
        // in full on one frame, whatever that frame's dt happens to be.
        if (landImpulse != 0.0f) {
            for (int a = 0; a < 3; ++a)
                g_bone[i].v[a] += downLocal[a] * landImpulse;
        }
        if (constrain) ProjectOnto(g_bone[i].v, axis);

        for (int a = 0; a < 3; ++a) g_bone[i].x[a] += g_bone[i].v[a] * dt;
        if (constrain) ProjectOnto(g_bone[i].x, axis);

        const float mag = Len3(g_bone[i].x);
        if (mag > c.dynamicBonesMaxDisplace && mag > 1e-6f) {
            const float s = c.dynamicBonesMaxDisplace / mag;
            for (int a = 0; a < 3; ++a) {
                g_bone[i].x[a] *= s;
                g_bone[i].v[a] *= 0.5f;
            }
        }
    }
    g_settled = g_bone[0].live && g_bone[1].live;
}

void Report() {
    const Config& c = Cfg();
    const unsigned every = static_cast<unsigned>(c.dynamicBonesReportFrames);
    if (!g_frames || (g_frames % every) != 0) return;

    float d[2][3] = {};
    DynamicBonesDisplacement(d);

    const unsigned dupPct = g_frames ? (100u * g_dupFrames / g_frames) : 0u;
    LogF("dynbones: joints=%d shader=%d lock=%d  frames=%u dup=%u%% snaps=%u  "
         "drive_peak=%.1f accel_raw=%.0f drive_used=%.0f  disp_peak=%.2f  "
         "L=(%.2f,%.2f,%.2f) R=(%.2f,%.2f,%.2f)",
         g_numJoints, g_laraShader, g_lockShader, g_frames, dupPct, g_snaps,
         g_peakDrive, g_peakAccel, g_peakUsed, g_peakDisp,
         d[0][0], d[0][1], d[0][2], d[1][0], d[1][1], d[1][2]);

    // The number that settles whether jumps register every time: each jump
    // should pair with a landing, and each landing should carry a fallspeed.
    if (c.dynamicBonesDriveMode == 1) {
        LogF("dynbones: engine drive -- jumps=%u lands=%u  last landing "
             "fallspeed=%d (%s)", g_jumps, g_lands, g_lastLandFall,
             g_airKnown ? (g_airPrev ? "airborne now" : "grounded now")
                        : "no Lara item read yet");
    }

    {
        char buf[128];
        int n = 0;
        const int shown = (g_frameScopes < kMaxScopes) ? g_frameScopes : kMaxScopes;
        for (int i = 0; i < shown && n < static_cast<int>(sizeof(buf)) - 8; ++i)
            n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                             i ? ",%d" : "%d", g_frameScopeJoints[i]);
        if (shown == 0) buf[0] = 0;
        LogF("dynbones: this frame ran %d DrawLaraHD scope(s), joints [%s]",
             g_frameScopes, buf);
    }

    // Still pinned at the clamp means the drive is wrong, not that the spring
    // is merely stiff. Reported per WINDOW, not once per session: the peaks
    // used to accumulate forever, so a single level-load transient early on
    // left disp_peak reading 18.00 for the rest of the run and made a solver
    // that had settled correctly look permanently pinned.
    if (g_peakDisp >= c.dynamicBonesMaxDisplace - 0.01f) {
        LogF("dynbones: displacement hit the %.1f-unit clamp during this "
             "window. Either the anchor joint is wrong or "
             "DynamicBonesDriveScale is too high for these units.",
             c.dynamicBonesMaxDisplace);
    }

    if (c.dynamicBonesLogJoints) LogCensus();

    // Window the peaks so each line describes its own interval.
    g_peakDisp = g_peakDrive = g_peakAccel = g_peakUsed = 0.0f;

    static bool saidDup = false;
    if (!saidDup && g_frames >= every * 2 && dupPct >= 40) {
        saidDup = true;
        LogF("dynbones: %u%% of rendered frames repeat the previous joint "
             "matrix. The animation is stepping slower than the headset, so a "
             "spring driven straight off it will buzz.", dupPct);
    }
}

} // namespace

// ---------------------------------------------------------------------------

void DynamicBonesObserveDraw() {
    g_drawIsBody = false;
    g_drawRenderBody = false;
    if (!g_inLara) return;

    const RenderState& vs = VidState();
    if (!vs.joints || vs.shader < 0 || vs.shader >= kShaderCount
        || Shaders()[vs.shader].uid[7] < 0) return;

    ++g_scopeDraws;
    if (g_scopeDraws == 1) {
        g_scopeJointsMin = g_scopeJointsMax = kLaraJoints;
    } else {
        if (kLaraJoints < g_scopeJointsMin) g_scopeJointsMin = kLaraJoints;
        if (kLaraJoints > g_scopeJointsMax) g_scopeJointsMax = kLaraJoints;
    }

    // Rank each draw within Lara's scope, ignoring the GPU palette padding.
    const int n = kLaraJoints;
    float extent = 0.0f;
    int   distinct = 0;
    Summarise(vs.joints, n, extent, distinct);

    if (g_candCount < kMaxCandidates) {
        g_cand[g_candCount++] = { n, distinct, extent, vs.shader };
    }

    // Same majority-of-distinct-joints discriminator as the TR4/5 source.
    g_drawIsBody = (distinct * 2 >= n);

    // A shadow or 2D pass can present a body-shaped palette in a completely
    // different space. Only the world pass is the real Lara.
    if (!IsWorldPass() || !vs.proj || std::fabs(vs.proj->m[15]) > 0.5f
        || InInventory() || InTitle()) g_drawIsBody = false;

    // Captured before the lock narrows it: rendering wants every body draw.
    g_drawRenderBody = g_drawIsBody;

    // Once locked, only that shader may drive the solver. Others still score
    // into the candidate list so the log keeps showing the whole field.
    if (g_drawIsBody && g_lockShader >= 0) {
        if (vs.shader == g_lockShader) g_lockSeen = true;
        else g_drawIsBody = false;
    }
    if (!g_drawIsBody) return;

    // Prefer a fully populated skeleton over repeated attachment transforms.
    const bool full  = (distinct == n);
    const int  score = distinct + (full ? 1000 : 0);


    if (!g_haveBest || score > g_bestScore) {
        g_bestScore = score;
        std::memcpy(g_best.joints, vs.joints,
                    sizeof(float) * 12 * static_cast<size_t>(n));
        g_best.count    = n;
        g_best.shader   = vs.shader;
        g_best.distinct = distinct;
        g_best.extent   = extent;
        g_haveBest      = true;

        g_numJoints  = kLaraJoints;
        g_laraShader = vs.shader;

        const int idx = Cfg().dynamicBonesTorsoJoint;
        if (idx < 0 || idx >= n) {
            static int complainedAbout = -2;
            if (complainedAbout != idx) {
                complainedAbout = idx;
                LogF("dynbones: DynamicBonesTorsoJoint=%d is outside the %d "
                     "joints the body draw uploads; nothing to latch.", idx, n);
            }
            return;
        }
        std::memcpy(g_torsoThis.m, vs.joints + idx * 12, sizeof(g_torsoThis.m));
        g_haveThis = true;
    }
}

void DynamicBonesUpdate() {
    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();

    const bool want = Cfg().enabled && Cfg().dynamicBones && d && base
                   && d->drawLaraHD != 0;

    if (g_boundDll && (!want || d != g_boundDll || base != g_boundBase)) {
        LogF("dynbones: unhooking %S", g_boundDll->module);
        Remove();
    }
    if (!want) return;

    if (!g_boundDll) {
        if (base == g_failedBase) return;
        g_boundDll  = d;
        g_boundBase = base;
        if (!Install(*d, base)) {
            LogF("dynbones: DrawLaraHD could not be hooked in %S -- nothing is "
                 "measured. A prologue mismatch here means the game was "
                 "patched; re-run tools\\verify_addresses.py.", d->module);
            Remove();
            g_failedBase = base;
            return;
        }
        if (!g_loggedFirst) {
            g_loggedFirst = true;
            LogF("dynbones: hooked %S (%s) -- chest physics enabled; draws scoped "
                 "to Lara", d->module, d->name);
        }
    }

    const int level = AppFlag(drva::app_off::level);
    const uint64_t item = *reinterpret_cast<uint64_t*>(base + d->laraItem);
    if (level != g_level || item != g_item) {
        ResetSolver();
        BoneSkinResetRegion();
        g_level = level;
        g_item = item;
    }

    // Lara was not drawn this frame: menu, loading screen, a cutscene camera
    // that excludes her. Hold the solver where it is rather than integrating
    // against a stale anchor, and drop the previous-frame matrix so the gap
    // does not register as one enormous drive step when she comes back.
    if (!g_haveThis) {
        // Do not replay a pre-menu landing or use a stale torso after loading.
        g_havePrev = false;
        g_settled = false;
        g_airKnown = false;
        g_fallMax = 0;
        g_lastTick.QuadPart = 0;
        for (auto& bone : g_bone) bone.live = false;
        if (g_lockShader >= 0 && ++g_lockMiss > kLockGraceFrames) {
            g_lockShader = -1;
            g_lockMiss = 0;
        }
        g_frameScopes = 0;
        g_haveBest    = false;
        g_bestScore   = -1;
        g_candCount   = 0;
        return;
    }

    if (g_numJoints != g_reportedJoints) {
        g_reportedJoints = g_numJoints;
        LogF("dynbones: TR1-3 Lara: %d skeleton joints, 32-slot GPU upload",
             g_numJoints);
    }

    // Real elapsed time, not a fixed step: how the solver behaves at headset
    // frame rates against the source animation is the whole question, and a
    // fixed step would hide exactly that.
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = 1.0f / 60.0f;
    if (g_lastTick.QuadPart != 0 && freq.QuadPart != 0) {
        dt = static_cast<float>(double(now.QuadPart - g_lastTick.QuadPart)
                                / double(freq.QuadPart));
    }
    g_lastTick = now;
    // A breakpoint, an alt-tab or a level load can hand us an arbitrarily
    // large dt, and a spring integrated across that explodes.
    if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;

    ++g_frames;

    if (g_havePrev
        && std::memcmp(g_torsoThis.m, g_torsoPrev.m, sizeof(g_torsoThis.m)) == 0) {
        ++g_dupFrames;
    }

    // Lock onto the first body draw, and keep the lock while its shader keeps
    // turning up.
    if (g_lockShader < 0 && g_haveBest) {
        g_lockShader = g_best.shader;
        g_lockMiss   = 0;
        LogF("dynbones: locked to shader %d (%d joints) -- the latch stays "
             "here so the drive stays continuous", g_lockShader, g_best.count);
    } else if (g_lockShader >= 0) {
        if (g_lockSeen) {
            g_lockMiss = 0;
        } else if (++g_lockMiss > kLockGraceFrames) {
            LogF("dynbones: shader %d absent for %d frames -- unlocking and "
                 "re-choosing", g_lockShader, g_lockMiss);
            g_lockShader = -1;
            g_lockMiss   = 0;
        }
    }
    g_lockSeen = false;

    Solve(dt);

    float disp[2][3] = {};
    if (DynamicBonesDisplacement(disp)) {
        for (int i = 0; i < 2; ++i) {
            const float mag = Len3(disp[i]);
            if (mag > g_peakDisp) g_peakDisp = mag;
        }
    }

    Report();

    g_torsoPrev   = g_torsoThis;
    g_havePrev    = true;
    g_haveThis    = false;   // re-latched by the next frame's draws
    g_frameScopes = 0;
    g_haveBest    = false;   // the body is re-chosen from scratch each frame
    g_bestScore   = -1;
    g_candCount   = 0;
}

bool DynamicBonesDisplacement(float out[2][3]) {
    if (!g_settled || !g_boundDll) return false;
    // x already lives in the parent's frame -- that is the point of the v2
    // formulation -- so there is nothing to convert here.
    for (int i = 0; i < 2; ++i)
        std::memcpy(out[i], g_bone[i].x, sizeof(float) * 3);
    return true;
}

bool DynamicBonesRenderBody() {
    return g_drawRenderBody && g_boundDll != nullptr;
}

bool DynamicBonesWorldOffset(float out[3]) {
    if (!g_settled || !g_boundDll) return false;
    const Config& c = Cfg();

    float avg[3];
    for (int a = 0; a < 3; ++a)
        avg[a] = 0.5f * (g_bone[0].x[a] + g_bone[1].x[a]);
    RotateVector(g_torsoThis, avg, out);

    // Grounded equilibrium in world space is simply gravity / stiffness straight
    // down: gravity is world Y and the spring is isotropic, so no rotation is
    // involved. Removing it makes rest mean "as authored".
    if (c.dynamicBonesStiffness > 1e-3f)
        out[1] -= c.dynamicBonesGravity / c.dynamicBonesStiffness;

    const float s = c.dynamicBonesDebugScale;
    out[0] *= s; out[1] *= s; out[2] *= s;
    return true;
}

bool DynamicBonesTorsoFrame(float out[12]) {
    if (!g_settled || !g_boundDll) return false;
    std::memcpy(out, g_torsoThis.m, sizeof(float) * 12);
    return true;
}

bool DynamicBonesLaraForward(float out[3]) {
    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();
    if (!d || !base || d->laraItem == 0) return false;
    const uint64_t item = *reinterpret_cast<uint64_t*>(base + d->laraItem);
    if (!item) return false;
    const int16_t yrot = *reinterpret_cast<int16_t*>(item + kItemYRot);
    // TR moves forward by x += sin(y_rot), z += cos(y_rot).
    const float t = static_cast<float>(yrot) * (6.28318530718f / 65536.0f);
    out[0] = std::sin(t);
    out[1] = 0.0f;
    out[2] = std::cos(t);
    return true;
}

bool DynamicBonesAppliedToDraw() { return g_appliedThisDraw; }

void DynamicBonesApplyToDraw() {
    g_appliedThisDraw = false;
    g_patchedAt = nullptr;

    // The per-vertex shader path supersedes the rigid joint edit. Doing both
    // would move the chest twice and drag the back along with it again.
    if (BoneSkinActive()) return;

    const Config& c = Cfg();
    if (!c.dynamicBonesApply || !g_inLara || !g_drawIsBody || !g_settled) return;
    if (!g_boundDll) return;

    RenderState& vs = VidState();
    if (!vs.joints || vs.shader < 0 || vs.shader >= kShaderCount
        || Shaders()[vs.shader].uid[7] < 0) return;

    const int idx = c.dynamicBonesTorsoJoint;
    if (idx < 0 || idx >= kLaraJoints) return;

    // One joint, so the two bones become one motion. They track each other
    // closely anyway -- the measured L and R agreed to about 0.1 units -- and
    // averaging keeps the debug view symmetric instead of favouring a side.
    float avg[3];
    for (int a = 0; a < 3; ++a)
        avg[a] = 0.5f * (g_bone[0].x[a] + g_bone[1].x[a]);

    float* m = vs.joints + idx * 12;
    std::memcpy(g_patchedSaved, m, sizeof(g_patchedSaved));
    g_patchedAt = m;
    g_appliedThisDraw = true;
    vs.consts |= kJoints;

    // The solver works in the joint's own frame, so rotate back out to world
    // before touching the translation column.
    Joint M;
    std::memcpy(M.m, m, sizeof(M.m));
    float world[3];
    RotateVector(M, avg, world);

    const float s = c.dynamicBonesDebugScale;
    m[3]  += world[0] * s;
    m[7]  += world[1] * s;
    m[11] += world[2] * s;
}

void DynamicBonesRestoreDraw() {
    if (!g_patchedAt) return;
    std::memcpy(g_patchedAt, g_patchedSaved, sizeof(g_patchedSaved));
    g_patchedAt = nullptr;
    VidState().consts |= kJoints;
}

void DynamicBonesShutdown() {
    if (g_boundDll) Remove();
    ResetSolver();
    g_loggedFirst = false;
    g_failedBase  = 0;
}

} // namespace tr
