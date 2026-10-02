// selftest.cpp -- checks the two things in this project that are easy to get
// silently wrong: the inline hook mechanism, and the projection/view maths
// against the engine's own formulas.
//
// Build:  tests\build_selftest.cmd
//
// The projection test is the important one. It asserts that feeding a SYMMETRIC
// frustum through the OpenVR path reproduces the engine's own ogl_setPersp
// (RVA 0x0000FA90) byte for byte:
//
//     mProj[1].e00 =  1/tanX
//     mProj[1].e11 = -1/tanY
//     mProj[1].e22 = (zNear + zFar) / (zNear - zFar)
//     mProj[1].e23 = (2 * zFar * zNear) / (zNear - zFar)
//     mProj[1].e32 = -1
//
// If that holds, the only difference between the engine's own projection and
// ours is the asymmetry OpenVR asks for -- which is the entire point.

#include "Engine.h"
#include "StereoMath.h"
#include "PortalGeom.h"
#include "InlineHook.h"
#include "Log.h"
#include "LocomotionMath.h"
#include "FirstPersonStabilization.h"
#include "FirstPersonClearance.h"
#include "FirstPersonVisibility.h"
#include "FirstPersonActionIcon.h"
#include "MotionGunMath.h"
#include "MotionGunInput.h"
#include "WristCap.h"
#include "Config.h"
#include "HeadHeightClamp.h"

#include <windows.h>
#include <cstdio>
#include <cmath>
#include <string>

static int g_fail = 0;

static void Check(bool cond, const char* what) {
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) ++g_fail;
}

static void CheckNear(float got, float want, const char* what, float eps = 1e-5f) {
    const bool ok = std::fabs(got - want) <= eps * (1.0f + std::fabs(want));
    printf("  [%s] %-46s got %12.6f want %12.6f\n", ok ? "ok" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}

// ---------------------------------------------------------------------------
// 1. Projection: symmetric OpenVR input must reproduce ogl_setPersp.
// ---------------------------------------------------------------------------
static void TestProjectionMatchesEngine() {
    printf("\nprojection vs ogl_setPersp (symmetric case)\n");

    const float tanX = 0.8f, tanY = 0.6f;
    const float zn = 16.0f, zf = 32768.0f;

    tr::mat4 p{};
    // OpenVR raw tangents for a symmetric frustum.
    tr::BuildEyeProjection(p, -tanX, tanX, -tanY, tanY, zn, zf, /*flipY=*/true);

    CheckNear(p.m[0],  1.0f / tanX,                       "e00 =  1/tanX");
    CheckNear(p.m[5], -1.0f / tanY,                       "e11 = -1/tanY  (engine Y flip)");
    CheckNear(p.m[8],  0.0f,                              "e02 =  0  (no shear)");
    CheckNear(p.m[9],  0.0f,                              "e12 =  0  (no shear)");
    CheckNear(p.m[10], (zn + zf) / (zn - zf),             "e22 = (n+f)/(n-f)");
    CheckNear(p.m[14], (2.0f * zf * zn) / (zn - zf),      "e23 = 2fn/(n-f)", 1e-4f);
    CheckNear(p.m[11], -1.0f,                             "e32 = -1");
    CheckNear(p.m[15], 0.0f,                              "e33 =  0");
}

// ---------------------------------------------------------------------------
// 2. Asymmetric frustum: the shear must land in e02/e12.
//
//    In TR4-6 those were the two terms vid_setPerspOffset wrote. TR1-3 has no
//    such function -- ogl_setPersp zeroes both (verified at RVA 0x0000FAD0) --
//    so on this game they are ours alone, and nothing in the engine competes
//    for them. The test matters more here, not less: there is no second writer
//    whose behaviour would reveal a sign error.
// ---------------------------------------------------------------------------
static void TestAsymmetricShear() {
    printf("\nasymmetric frustum shear lands in e02/e12\n");

    // A left eye: more extent to the left than the right.
    const float l = -1.0f, r = 0.6f, t = -0.8f, b = 0.9f;
    const float zn = 16.0f, zf = 4096.0f;

    tr::mat4 p{};
    tr::BuildEyeProjection(p, l, r, t, b, zn, zf, true);

    CheckNear(p.m[0], 2.0f / (r - l),            "e00 = 2/(r-l)");
    CheckNear(p.m[8], (r + l) / (r - l),         "e02 = (r+l)/(r-l)");
    CheckNear(p.m[5], -(2.0f / (b - t)),         "e11 = -2/(b-t)   (scale negated)");
    // The Y flip negates the SCALE only. The shear multiplies vz, which is not
    // flipped, so it keeps its sign. Negating it too was a real bug: invisible
    // on a symmetric frustum, a fishbowl warp on a real headset.
    CheckNear(p.m[9], (b + t) / (b - t),         "e12 = +(b+t)/(b-t) (shear NOT negated)");
    Check(p.m[8] != 0.0f, "shear is non-zero for an asymmetric eye");
}

// ---------------------------------------------------------------------------
// 2b. The real test: project the frustum's own edges and check they land on the
//     edges of NDC. Checking matrix entries against the formula that produced
//     them is circular; this is not.
//
//     Uses the measured tangents from a real headset (asymmetric vertically),
//     which is the case that exposed the shear-sign bug.
// ---------------------------------------------------------------------------
static void TestFrustumEdgesMapToNDC() {
    printf("\nfrustum edges land on NDC edges (real headset tangents)\n");

    // Left eye of a Quest-class headset, as reported by GetProjectionRaw.
    const float l = -1.3764f, r = 0.8391f, t = -1.4281f, b = 0.9657f;
    const float zn = 16.0f, zf = 32768.0f;

    tr::mat4 p{};
    tr::BuildEyeProjection(p, l, r, t, b, zn, zf, /*flipY=*/true);

    // A point at depth d in ENGINE view space: x right, y DOWN, -z forward.
    // clip = P * (vx, vy, -d, 1);  w = d;  ndc = clip.xy / w.
    auto ndc = [&](float xOverD, float yOverD, float& nx, float& ny) {
        const float d  = 100.0f;
        const float vx = xOverD * d, vy = yOverD * d, vz = -d;
        const float cx = p.m[0] * vx + p.m[4] * vy + p.m[8]  * vz + p.m[12];
        const float cy = p.m[1] * vx + p.m[5] * vy + p.m[9]  * vz + p.m[13];
        const float cw = p.m[3] * vx + p.m[7] * vy + p.m[11] * vz + p.m[15];
        nx = cx / cw;
        ny = cy / cw;
    };

    float nx = 0.0f, ny = 0.0f;

    ndc(r, 0.0f, nx, ny);
    CheckNear(nx, 1.0f, "right edge  -> ndc.x = +1", 1e-4f);

    ndc(l, 0.0f, nx, ny);
    CheckNear(nx, -1.0f, "left edge   -> ndc.x = -1", 1e-4f);

    // OpenVR's raw t/b are given in a Y-up sense: the TOP edge of the image sits
    // at vy/d = b in Y-up space, which is -b once Y points down.
    ndc(0.0f, -b, nx, ny);
    CheckNear(ny, 1.0f, "top edge    -> ndc.y = +1", 1e-4f);

    ndc(0.0f, -t, nx, ny);
    CheckNear(ny, -1.0f, "bottom edge -> ndc.y = -1", 1e-4f);

    // With the shear wrongly negated the top edge came out at ~0.61, so this
    // guards the exact regression.
    ndc(0.0f, -b, nx, ny);
    Check(ny > 0.99f, "top edge is not short of the viewport (the fishbowl bug)");
}

// ---------------------------------------------------------------------------
// 3. ExtractNearFar must invert the engine's own depth terms.
// ---------------------------------------------------------------------------
static void TestNearFarRoundTrip() {
    printf("\nnear/far recovery from mProj[1]\n");

    const float zn = 16.0f, zf = 32768.0f;
    tr::mat4 p{};
    tr::BuildEyeProjection(p, -0.8f, 0.8f, -0.6f, 0.6f, zn, zf, true);

    float n = 0.0f, f = 0.0f;
    Check(tr::ExtractNearFar(p, n, f), "ExtractNearFar succeeds");
    CheckNear(n, zn, "recovered zNear", 1e-3f);
    CheckNear(f, zf, "recovered zFar",  1e-3f);

    // A zeroed matrix must be rejected rather than producing garbage.
    tr::mat4 zero{};
    Check(!tr::ExtractNearFar(zero, n, f), "rejects a degenerate matrix");
}

// ---------------------------------------------------------------------------
// 4. Rigid-transform algebra.
// ---------------------------------------------------------------------------
static void TestAffine() {
    printf("\naffine algebra\n");

    // A 90-degree yaw with a translation.
    tr::Affine m{};
    m.r[0][0] =  0.0f; m.r[0][1] = 0.0f; m.r[0][2] = 1.0f; m.r[0][3] = 10.0f;
    m.r[1][0] =  0.0f; m.r[1][1] = 1.0f; m.r[1][2] = 0.0f; m.r[1][3] = 20.0f;
    m.r[2][0] = -1.0f; m.r[2][1] = 0.0f; m.r[2][2] = 0.0f; m.r[2][3] = 30.0f;

    const tr::Affine inv = tr::InvertRigid(m);
    const tr::Affine id  = tr::Mul(m, inv);

    bool ok = true;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            ok &= std::fabs(id.r[i][j] - (i == j ? 1.0f : 0.0f)) < 1e-5f;
    for (int i = 0; i < 3; ++i)
        ok &= std::fabs(id.r[i][3]) < 1e-4f;
    Check(ok, "M * M^-1 == identity");

    const tr::Affine I = tr::Affine::Identity();
    const tr::Affine same = tr::Mul(I, m);
    bool ok2 = true;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            ok2 &= std::fabs(same.r[i][j] - m.r[i][j]) < 1e-6f;
    Check(ok2, "I * M == M");
}

// ---------------------------------------------------------------------------
// 5. Y-flip conjugation must stay a rigid transform and scale translation.
// ---------------------------------------------------------------------------
static void TestEngineSpaceConversion() {
    printf("\nOpenVR (Y-up, metres) -> engine view space (Y-down, TR units)\n");

    tr::Affine ovr = tr::Affine::Identity();
    ovr.r[0][3] = 0.032f;   // +32 mm to the right: half of a 64 mm IPD
    ovr.r[1][3] = 0.100f;   // 100 mm up
    ovr.r[2][3] = -0.050f;  // 50 mm forward

    const float scale = 423.0f;
    const tr::Affine e = tr::ToEngineSpace(ovr, scale, /*flipY=*/true);

    CheckNear(e.r[0][3],  0.032f * scale, "x translation scaled, sign kept");
    CheckNear(e.r[1][3], -0.100f * scale, "y translation scaled AND negated");
    CheckNear(e.r[2][3], -0.050f * scale, "z translation scaled, sign kept");

    // Rotation conjugation: a yaw must survive, and S*R*S must stay orthonormal.
    tr::Affine rot = tr::Affine::Identity();
    const float c = 0.8f, s = 0.6f;
    rot.r[0][0] = c; rot.r[0][2] = s;
    rot.r[2][0] = -s; rot.r[2][2] = c;
    const tr::Affine re = tr::ToEngineSpace(rot, 1.0f, true);

    bool orth = true;
    for (int i = 0; i < 3; ++i) {
        float len = 0.0f;
        for (int j = 0; j < 3; ++j) len += re.r[i][j] * re.r[i][j];
        orth &= std::fabs(len - 1.0f) < 1e-5f;
    }
    Check(orth, "conjugated rotation stays orthonormal");
}

// ---------------------------------------------------------------------------
// 6. Packed view round-trip -- the mView_packed layout.
// ---------------------------------------------------------------------------
static void TestPackedView() {
    printf("\nmView_packed round-trip\n");

    tr::mat4 packed{};
    for (int i = 0; i < 16; ++i) packed.m[i] = static_cast<float>(i + 1);

    const tr::Affine a = tr::ReadPackedView(packed);
    Check(a.r[0][0] == 1.0f && a.r[0][3] == 4.0f,  "row 0 is floats 0..3");
    Check(a.r[1][0] == 5.0f && a.r[1][3] == 8.0f,  "row 1 is floats 4..7");
    Check(a.r[2][0] == 9.0f && a.r[2][3] == 12.0f, "row 2 is floats 8..11");

    tr::mat4 out{};
    out.m[12] = 1.0f;              // the (1,0,..) tail the engine writes
    tr::WritePackedView(out, a);
    bool same = true;
    for (int i = 0; i < 12; ++i) same &= (out.m[i] == packed.m[i]);
    Check(same, "write(read(x)) == x for floats 0..11");
    Check(out.m[12] == 1.0f, "floats 12..15 left untouched");
}

// ---------------------------------------------------------------------------
// 7. The inline hook itself, against a synthetic function whose prologue is
//    byte-identical to ogl_draw's.
// ---------------------------------------------------------------------------
typedef int (*TargetFn)();

static const uint8_t kTargetCode[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,   // mov [rsp+8], rbx   <- same 5 bytes as ogl_draw
    0xB8, 0x2A, 0x00, 0x00, 0x00,   // mov eax, 42
    0xC3                            // ret
};

static hook::InlineHook g_testHook;

static int DetourTarget() {
    return g_testHook.Original<TargetFn>()() + 100;
}

static void TestInlineHook() {
    printf("\ninline hook mechanism\n");

    void* mem = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    Check(mem != nullptr, "allocated a synthetic target");
    if (!mem) return;
    memcpy(mem, kTargetCode, sizeof(kTargetCode));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(kTargetCode));

    TargetFn fn = reinterpret_cast<TargetFn>(mem);
    Check(fn() == 42, "target returns 42 before hooking");

    // Wrong expected bytes must be refused.
    const uint8_t wrong[5] = { 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    hook::InlineHook bad;
    Check(!bad.Install(mem, reinterpret_cast<void*>(&DetourTarget), 5,
                       wrong, sizeof(wrong), "mismatch"),
          "refuses to patch on prologue mismatch");
    Check(fn() == 42, "target untouched after a refused install");

    Check(g_testHook.Install(mem, reinterpret_cast<void*>(&DetourTarget), 5,
                             kTargetCode, 5, "selftest"),
          "installs with matching prologue");
    Check(fn() == 142, "detour runs and can call the original via the trampoline");

    g_testHook.Remove();
    Check(fn() == 42, "original bytes restored on Remove()");

    VirtualFree(mem, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------------------
// 6. TR1-3 engine layout: the struct sizes and the consts bit table.
//
// The static_asserts in Engine.h already fix sizeof(RenderState) == 152 and
// sizeof(Shader) == 60 at compile time, so reaching this function at all proves
// those. What is checked here is the part a static_assert cannot reach: that the
// ConstBits enum agrees with the mask validate_draw actually writes.
//
// validate_draw forces consts = 0x3F001F on a shader switch (the immediate at
// RVA 0x0000F053). Every bit in that mask must be one this enum names, and every
// bit this enum names must be in that mask -- otherwise an injected draw either
// fails to upload a matrix we changed, or sets a bit the engine does not expect.
// ---------------------------------------------------------------------------
static void TestEngineConstBits() {
    printf("\nTR1-3 consts bits vs the 0x3F001F mask validate_draw writes\n");

    const uint32_t named = tr::kProj | tr::kView | tr::kShadow | tr::kFogColor
                         | tr::kContacts | tr::kModel | tr::kParams | tr::kJoints
                         | tr::kLPos | tr::kLCol | tr::kAmbient;

    CheckNear((float)named, (float)0x003F001Fu,
              "named bits == shader-switch mask", 0.0f);
    Check(tr::kAllOnShaderSwitch == 0x003F001Fu, "kAllOnShaderSwitch == 0x3F001F");

    // The two the injection actually forces. Getting either wrong is silent:
    // the write lands in memory and is simply never uploaded.
    Check(tr::kProj == 1u, "kProj is bit 0  (uid[0], glUniformMatrix4fv)");
    Check(tr::kView == 2u, "kView is bit 1  (uid[1], glUniform4fv n=4)");

    // TR4-6 had kFogColor at bit 5 and no contacts bit. Confusing the two
    // engines here would force the wrong upload.
    Check(tr::kFogColor == (1u << 3), "kFogColor is bit 3 (bit 5 in TR4-6)");
    Check(tr::kContacts == (1u << 4), "kContacts is bit 4 (absent in TR4-6)");

    Check(sizeof(tr::RenderState) == 152, "sizeof(RenderState) == 152");
    Check(sizeof(tr::Shader) == 60,       "sizeof(Shader) == 60");
    Check(sizeof(tr::mat4) == 64,         "sizeof(mat4) == 64");
    Check(tr::kShaderCount == 74,         "kShaderCount == 74");
}

// ---------------------------------------------------------------------------
// 7. Portal frustum maths (PortalGeom.h).
//
// This is the part of the room culling that can be wrong without crashing: a
// sign slip in the edge planes turns "visible through that doorway" into "not
// visible", and the symptom on screen -- geometry missing when you look away
// from the game camera -- is indistinguishable from the bug the whole feature
// exists to fix. So the cases below are worked out by hand rather than
// captured from a run.
//
// Everything is in eye space with the viewpoint at the origin and -Z forward.
// ---------------------------------------------------------------------------
using tr::portal::Vec3;
using tr::portal::Plane;
using tr::portal::Poly;

static bool InsideAll(const Plane* pl, int n, const Vec3& p) {
    for (int i = 0; i < n; ++i) {
        if (tr::portal::Dot(pl[i].n, p) + pl[i].d < 0.0f) return false;
    }
    return true;
}

static void TestPortalGeometry() {
    printf("\nportal frustum maths\n");

    // --- the root frustum ---------------------------------------------------
    //
    // tanX = 1 is 45 degrees a side, so at 1000 units ahead the edge is at
    // x = +/-1000 exactly. The boundary case matters: `inside` is >= 0, so a
    // point exactly on the edge counts as visible, which is the conservative
    // direction.
    Plane root[tr::portal::kMaxPlanes];
    const int nRoot = tr::portal::RootPlanes(1.0f, 0.5f, root);
    Check(nRoot == 5, "root frustum is 5 planes (near + four sides)");

    Check( InsideAll(root, nRoot, Vec3{    0.0f,   0.0f, -1000.0f }), "straight ahead is inside");
    Check( InsideAll(root, nRoot, Vec3{  999.0f,   0.0f, -1000.0f }), "just inside the right edge");
    Check(!InsideAll(root, nRoot, Vec3{ 1001.0f,   0.0f, -1000.0f }), "just outside the right edge");
    Check( InsideAll(root, nRoot, Vec3{    0.0f, 499.0f, -1000.0f }), "just inside the bottom edge");
    Check(!InsideAll(root, nRoot, Vec3{    0.0f, 501.0f, -1000.0f }), "just outside the bottom edge");
    Check(!InsideAll(root, nRoot, Vec3{    0.0f,   0.0f,  1000.0f }), "behind the head is outside");
    Check(!InsideAll(root, nRoot, Vec3{    0.0f,   0.0f,    -1.0f }), "nearer than the near plane is outside");

    // --- looking through a doorway -----------------------------------------
    //
    // A 200x200 opening 1000 units ahead. The cone through it widens linearly,
    // so at 2000 units it is 400 wide: x = 150 is inside, x = 250 is not. That
    // is the whole point of the mechanism -- the frustum SHRINKS at the doorway
    // rather than the room beyond simply being let in.
    Poly door;
    door.n = 4;
    door.v[0] = Vec3{ -100.0f, -100.0f, -1000.0f };
    door.v[1] = Vec3{  100.0f, -100.0f, -1000.0f };
    door.v[2] = Vec3{  100.0f,  100.0f, -1000.0f };
    door.v[3] = Vec3{ -100.0f,  100.0f, -1000.0f };

    Plane thru[tr::portal::kMaxPlanes];
    const int nThru = tr::portal::BuildPlanes(door, thru);
    Check(nThru == 5, "a quad opening yields near + four edge planes");

    Check( InsideAll(thru, nThru, Vec3{   0.0f,   0.0f, -2000.0f }), "through the doorway, on axis");
    Check( InsideAll(thru, nThru, Vec3{ 150.0f,   0.0f, -2000.0f }), "through the doorway, inside the cone");
    Check(!InsideAll(thru, nThru, Vec3{ 250.0f,   0.0f, -2000.0f }), "beside the doorway is culled");
    Check(!InsideAll(thru, nThru, Vec3{   0.0f, 250.0f, -2000.0f }), "above the doorway is culled");
    Check( InsideAll(thru, nThru, Vec3{  90.0f,   0.0f, -1000.0f }), "in the doorway itself");

    // The engine stores portal vertices in an order relative to the room that
    // owns them, so the same opening arrives wound either way depending on
    // which side it is crossed from. The centroid test in BuildPlanes is what
    // makes that not matter, and this is the case that would catch it.
    Poly reversed;
    reversed.n = 4;
    for (int i = 0; i < 4; ++i) reversed.v[i] = door.v[3 - i];
    Plane rthru[tr::portal::kMaxPlanes];
    const int nR = tr::portal::BuildPlanes(reversed, rthru);
    Check(nR == nThru, "reversed winding yields the same plane count");
    Check( InsideAll(rthru, nR, Vec3{ 150.0f, 0.0f, -2000.0f }), "reversed winding: inside is still inside");
    Check(!InsideAll(rthru, nR, Vec3{ 250.0f, 0.0f, -2000.0f }), "reversed winding: outside is still outside");

    // --- clipping one opening against another ------------------------------
    Poly cut = door;
    const Plane half{ Vec3{ 1.0f, 0.0f, 0.0f }, 0.0f };      // keep x >= 0
    Check(tr::portal::ClipPoly(cut, half), "clip against a half-space succeeds");
    Check(cut.n == 4, "a square cut down the middle is still four-sided");
    bool allRight = true;
    for (int i = 0; i < cut.n; ++i) if (cut.v[i].x < -1e-3f) allRight = false;
    Check(allRight, "every surviving vertex is on the kept side");

    Poly gone = door;
    const Plane away{ Vec3{ 1.0f, 0.0f, 0.0f }, -1000.0f };  // keep x >= 1000
    Check(tr::portal::ClipPoly(gone, away), "clip that removes everything still succeeds");
    Check(gone.n < 3, "a fully-clipped opening is empty");

    // --- degenerate openings ------------------------------------------------
    //
    // A portal seen exactly edge-on has every edge in a plane through the apex.
    // BuildPlanes must drop those rather than emit garbage normals; reporting
    // fewer than four planes is how the traversal is told to keep the frustum
    // it already had.
    Poly edgeOn;
    edgeOn.n = 4;
    edgeOn.v[0] = Vec3{ 0.0f, -100.0f, -1000.0f };
    edgeOn.v[1] = Vec3{ 0.0f, -100.0f, -2000.0f };
    edgeOn.v[2] = Vec3{ 0.0f,  100.0f, -2000.0f };
    edgeOn.v[3] = Vec3{ 0.0f,  100.0f, -1000.0f };
    Plane eplanes[tr::portal::kMaxPlanes];
    const int nE = tr::portal::BuildPlanes(edgeOn, eplanes);
    Check(nE >= 1 && nE <= 5, "an edge-on opening produces no bogus planes");

    // --- box culling --------------------------------------------------------
    //
    // A box far larger than the frustum's cross-section, straddling the axis.
    // No corner of it is inside, and the naive "is any corner visible" test
    // would drop it -- which on screen is a wall or a large static mesh
    // vanishing when you stand close to it.
    Vec3 big[8];
    int  bi = 0;
    for (int xs = 0; xs < 2; ++xs)
        for (int ys = 0; ys < 2; ++ys)
            for (int zs = 0; zs < 2; ++zs)
                big[bi++] = Vec3{ xs ? 5000.0f : -5000.0f,
                                  ys ? 5000.0f : -5000.0f,
                                  zs ? -900.0f : -1100.0f };
    bool anyCornerInside = false;
    for (int i = 0; i < 8; ++i) if (InsideAll(root, nRoot, big[i])) anyCornerInside = true;
    Check(!anyCornerInside, "the oversized box has no corner inside the frustum");
    Check(tr::portal::BoxVisible(big, root, nRoot), "...and BoxVisible reports it visible anyway");

    Vec3 behind[8];
    bi = 0;
    for (int xs = 0; xs < 2; ++xs)
        for (int ys = 0; ys < 2; ++ys)
            for (int zs = 0; zs < 2; ++zs)
                behind[bi++] = Vec3{ xs ? 100.0f : -100.0f,
                                     ys ? 100.0f : -100.0f,
                                     zs ? 2000.0f : 1000.0f };
    Check(!tr::portal::BoxVisible(behind, root, nRoot), "a box wholly behind the head is culled");
}

static void TestLocomotion() {
    using namespace tr;
    using namespace tr::locomotion;
    printf("\nFirst-person locomotion\n");
    Heading heading;
    heading.Align(0.4f, -0.7f);
    CheckNear(heading.World(-0.7f), 0.4f, "entry aligns current HMD to Lara");
    heading.Turn(Pi / 2);
    CheckNear(Wrap(heading.World(-0.7f) - 0.4f), Pi / 2, "right stick turns world heading right");
    CheckNear(Wrap(heading.World(-0.7f + Pi / 2) - 0.4f - Pi), 0, "physical and artificial turns compose");

    // Regression for the circular walk: vary the engine movement camera while
    // holding the HMD fixed. Undoing the engine frame must give a constant
    // world direction, including wraparound and rear-facing movement.
    bool stable = true, renderMatches = true;
    for (float physical : {-Pi, -Pi / 2, 0.0f, Pi / 2, Pi - 0.001f}) {
        for (float artificial : {-Pi, -Pi / 2, 0.0f, Pi / 2}) {
            heading.Align(0, 0);
            heading.Turn(artificial);
            const float world = heading.World(physical);
            const Vec expected{std::sin(world), std::cos(world)};
            for (int frame = 0; frame < 360; ++frame) {
                const float engineYaw = frame * Pi / 180;
                const Vec pad = Rotate(Rotate({0, 1}, world), -engineYaw);
                const Vec moved = Rotate(pad, engineYaw);
                stable &= Length(moved - expected) < 1e-5f;
            }
            // Actual render composition is HeadView * N * engineW2V. N flips
            // TR's +Z view to GL's -Z. Test independently against that matrix.
            const float c = std::cos(heading.base), s = std::sin(heading.base);
            Affine game = Affine::Identity();
            game.r[0][0] = c; game.r[0][2] = -s;
            game.r[2][0] = -s; game.r[2][2] = -c;
            const Affine view = Mul(RotY(physical * 180 / Pi), game);
            const Vec visibleForward{-view.r[2][0], -view.r[2][2]};
            renderMatches &= Length(visibleForward - expected) < 1e-5f;
        }
    }
    Check(stable, "physical/stick turns + orbiting chase camera never curve forward");
    Check(renderMatches, "rendered HMD forward equals world locomotion forward");

    Affine pitch = Affine::Identity();
    const float p = 0.8f;
    pitch.r[1][1] = pitch.r[2][2] = std::cos(p);
    pitch.r[1][2] = -std::sin(p); pitch.r[2][1] = std::sin(p);
    Affine roll = Affine::Identity();
    roll.r[0][0] = roll.r[1][1] = std::cos(0.6f);
    roll.r[0][1] = -std::sin(0.6f); roll.r[1][0] = std::sin(0.6f);
    const Affine pose = Mul(RotY(-45), Mul(pitch, roll));
    CheckNear(TrackingYaw(InvertRigid(pose)), Pi / 4, "pitch and roll cannot change the HMD heading");

    const Vec offset{0.3f, 0.2f};
    const Vec turned = PivotFloorOffset(offset, {}, Pi / 2);
    CheckNear(Length(Rotate(turned, Pi / 2) - offset), 0, "stick turn pivots around the current head");
    const Vec neckArc{0.12f, -0.07f};
    const Vec translatedNeck{0.22f, 0.18f};
    const Vec rawEye = translatedNeck + neckArc;
    const Vec pivotedEye = PivotFloorOffset(rawEye, neckArc, Pi / 2);
    CheckNear(Length(pivotedEye - neckArc - Rotate(translatedNeck, -Pi / 2)), 0,
              "stick turn pivots room translation but keeps neck arc attached to Lara");
    CheckNear(Length(PivotFloorOffset(neckArc, neckArc, Pi / 2) - neckArc), 0,
              "combined physical and stick turn cannot orbit the eye around Lara");
    CheckNear(Length(PivotFloorOffset(rawEye, neckArc, 2 * Pi) - rawEye), 0,
              "full artificial rotation returns the same floor offset");
    const Vec stillRaw{0.12f,0.18f};
    const Vec neutralNeck=NeckToHead(0,0.15f);
    bool stableEye=true, compensatedBody=false, stableController=true;
    const Vec fixedController{0.35f,0.27f};
    for (float yaw : {-Pi,-Pi/2,0.f,Pi/2,Pi}) {
        const Vec arc=NeckToHead(yaw,0.15f)-neutralNeck;
        const Vec view=ViewFloorOffset(stillRaw,arc,true);
        const Vec body=ViewFloorOffset(stillRaw,arc,false);
        stableEye &= Length(view-stillRaw)<1e-5f;
        compensatedBody |= Length(body-stillRaw)>0.01f;
        stableController &= Length(view+(fixedController-stillRaw)-fixedController)<1e-5f;
    }
    Check(stableEye && compensatedBody && stableController,
          "physical head turns leave world and controller fixed while body keeps neck compensation");
    CheckNear(Length(ViewFloorOffset(stillRaw,neckArc,false)-
                     NeckFloorOffset(stillRaw,neckArc)),0,
              "stabilization opt-out retains the legacy rendered-eye offset");
    CheckNear(Length(Rotate(Rotate(stillRaw,-Pi/2),Pi/2)-stillRaw),0,
              "artificial turn pivots the raw stabilized eye offset");
    Check(MovementAction({0, 1}) == Forward, "forward stick selects native forward movement");
    Check(MovementAction({0, -1}) == (Back | Walk),
          "back stick selects native backpedal instead of fast-back hop");
    Check(MovementAction({-1, 0}) == StepLeft, "left stick selects native left sidestep");
    Check(MovementAction({1, 0}) == StepRight, "right stick selects native right sidestep");
    Check(MovementAction({-1, 0}, true) == Left,
          "left stick becomes native left direction while preparing a jump");
    Check(MovementAction({1, 0}, true) == Right,
          "right stick becomes native right direction while preparing a jump");
    Check(MovementAction({.7f, .8f}) == Forward,
          "a forward-dominant diagonal keeps the forward animation");
    Check(MovementAction({-.8f, .7f}) == StepLeft,
          "a lateral-dominant diagonal keeps the sidestep animation");
    Check(MovementAction({}) == 0, "centred stick adds no movement action");
    Check(DirectionalRootScale(StepLeft, false) == 3,
          "native sidestep root motion scales to running pace");
    Check(DirectionalRootScale(Back | Walk, false) == 3,
          "native backpedal root motion scales to running pace");
    Check(DirectionalRootScale(Forward, false) == 1,
          "forward run keeps the engine's normal root motion");
    Check(DirectionalRootScale(StepRight, true) == 1,
          "jump preparation never receives scaled root motion");
    CheckNear(Length(CardinalMovement({.6f, .8f}) - Vec{0, 1}), 0,
              "forward-dominant analog is cardinalised without losing magnitude");
    CheckNear(Length(CardinalMovement({-.8f, .6f}) - Vec{-1, 0}), 0,
              "lateral-dominant analog is cardinalised without losing magnitude");
    for (float head : {-Pi, -Pi / 2, 0.0f, Pi / 2, Pi - .001f}) {
        const Vec backward = MovementWorld({0, -1}, head);
        const float yaw = MovementYaw({0, -1}, head);
        CheckNear(Length(Vec{std::sin(yaw), std::cos(yaw)} - backward), 0,
                  "backpedal root-motion angle remains backward after every turn", 1e-5f);
        const Vec left = MovementWorld({-1, 0}, head);
        const float leftYaw = MovementYaw({-1, 0}, head);
        CheckNear(Length(Vec{std::sin(leftYaw), std::cos(leftYaw)} - left), 0,
                  "sidestep root-motion angle remains lateral after every turn", 1e-5f);
    }
    CheckNear(Length(Limit({1, 1})), 1, "combined diagonal input uses radial limiting");

    // Physically turn about a neck pivot through a complete revolution. The
    // HMD moves on an arc, but the corrected body request must remain zero.
    bool stationaryTurn = true, lateralDrag = true, jumpHeading = true;
    const float neutralYaw = 0.37f, neck = 0.15f;
    for (int degrees = -180; degrees <= 180; ++degrees) {
        const float yaw = degrees * Pi / 180;
        const Vec initialHead = NeckToHead(neutralYaw, neck);
        const Vec currentHead = NeckToHead(yaw, neck);
        const Vec rawArc = currentHead - initialHead;
        const Vec corrected = NeckFloorOffset(rawArc,
            NeckToHead(yaw, neck) - NeckToHead(neutralYaw, neck));
        stationaryTurn &= Length(DragRequest(corrected, .02f)) < 1e-5f;
        stationaryTurn &= Length(corrected) < 1e-5f; // rendered horizontal translation too
        for (float base : {0.0f, Pi/2, -Pi/2, Pi}) {
            // Translation plus rotation retains exactly the physical step,
            // including a sideways step after an arbitrary artificial turn.
            const Vec step = Rotate({.3f, 0}, yaw);
            const Vec request = Rotate(DragRequest(corrected + step, .02f), base);
            const Vec relative = Rotate(request, -(base + yaw));
            lateralDrag &= std::fabs(relative.x - .28f) < 1e-5f && std::fabs(relative.z) < 1e-5f;
            const Vec forward = Rotate({0, 1}, base + yaw);
            // An old chase camera may point anywhere; simulation publishes
            // the same HMD frame in compression AND airborne forward jump.
            for (int state : {15, 3}) {
                const Vec stick = SimulationStick(forward, base + yaw, 95);
                const Vec trajectory = Rotate(stick * (1/95.0f), base + yaw);
                jumpHeading &= IsJumpSteeringState(state) && Length(trajectory-forward) < 1e-5f;
            }
        }
    }
    Check(stationaryTurn, "neck-pivot correction produces no body-drag request through 360 degrees");
    Check(lateralDrag, "direct sideways drag remains lateral after physical and artificial turns");
    Check(jumpHeading, "compression and forward flight keep the HMD direction after every turn");
    Check(!IsJumpSteeringState(10), "jump steering excludes hanging interactions");
    CheckNear(DragRequest({.32f,0}, .02f).x, .30f, "body drag requests metres, not an analog strength");

    for (int state : {10, 30, 31, 36, 37, 38, 56, 57, 58, 59, 60, 61,
                      75, 82, 83}) {
        Check(IsConstrainedInteractionState(state),
              "hang and push/pull states use the collision-safe camera anchor");
        Check(FirstPersonAnchorZ(state, 144, 16) == 16,
              "constrained interaction retracts the avatar-fit forward offset");
    }
    Check(!IsConstrainedInteractionState(2) && FirstPersonAnchorZ(2, 144, 16) == 144,
          "ordinary locomotion keeps the configured first-person anchor");
    Check(FirstPersonAnchorZ(36, -12, 16) == -12,
          "interaction safety never pushes a custom anchor farther forward");
    Check(!IsClimbingCameraState(19) && IsClimbingCameraState(56) &&
          IsClimbingCameraState(61) && !IsClimbingCameraState(10) &&
          !IsClimbingCameraState(36),
          "wall-climb camera sweep excludes pull-up, hanging and blocks");
    Check(CanHardStopState(1,2,false) &&
          CanHardStopState(2,1,false) &&
          !CanHardStopState(1,2,true) &&
          !CanHardStopState(1,3,false) &&
          !CanHardStopState(19,2,false),
          "stick-release stop excludes gravity, jumps and pull-ups");
    for (int state : {10, 30, 31, 75, 82, 83})
        Check(IsLedgeHangState(state),
              "hanging retains Lara's arms in first person");
    for (int state : {2, 3, 9, 15, 19, 28, 36, 56, 61})
        Check(!IsLedgeHangState(state),
              "TR4/5-style arm mask ends before pull-up, crate and jump states");
    Check(IsJumpOrFallState(3) && IsJumpOrFallState(9) &&
          IsJumpOrFallState(15) && IsJumpOrFallState(25) &&
          IsJumpOrFallState(29) && !IsJumpOrFallState(2) &&
          !IsJumpOrFallState(19),
          "ceiling clearance pauses for jumps and falls, not standing or pull-up");
    using tr::headheight::Clamp;
    constexpr float neutralY=1.25f;
    constexpr float scale=423.0f;
    CheckNear(Clamp(neutralY, neutralY, 128.0f, 128.0f, scale), neutralY,
              "landing clamp preserves a neutral headset height");
    CheckNear(Clamp(neutralY+0.30f, neutralY, 128.0f, 128.0f, scale), neutralY,
              "low ceiling caps rise relative to the headset neutral");
    CheckNear(Clamp(neutralY+0.30f, neutralY, 658.0f, 128.0f, scale),
              neutralY+0.30f,
              "crate mount keeps an ordinary head rise below the cap");
    CheckNear(Clamp(neutralY+1.50f, neutralY, 658.0f, 128.0f, scale),
              neutralY+(658.0f-128.0f)/scale,
              "crate ceiling caps only the excess head rise");
    using tr::firstperson::HdDrawMeshBits;
    Check(!tr::firstperson::UseHeadCamera(0) && !tr::firstperson::UseHeadCamera(-1) &&
          tr::firstperson::UseHeadCamera(1),
          "death yields the head camera and respawn restores eligibility");
    const int32_t outfitMapping[]={14,8,9,10,7,13,-1,32};
    Check(tr::firstperson::SkinJointMask(tr::firstperson::ArmMeshBits,outfitMapping,8)==0x2eu &&
          tr::firstperson::SkinJointMask(tr::firstperson::ArmMeshBits,nullptr,15)==0x3f00u &&
          tr::firstperson::SkinJointMask(0xffffffffu,nullptr,32)==0xffffffffu,
          "body visibility maps outfit bones and handles legacy and high palette slots");
    Check(HdDrawMeshBits(0, false, true) == tr::firstperson::ArmMeshBits &&
          HdDrawMeshBits(0x400, false, true) == tr::firstperson::ArmMeshBits,
          "unmasked HD hang body draws both complete arms despite stale item mask");
    Check(HdDrawMeshBits(0x600, true, true) == 0x600 &&
          HdDrawMeshBits(0x3000, true, true) == 0x3000,
          "native masked right and left hand passes keep their own joints");
    Check(HdDrawMeshBits(0, false, false) == ~tr::firstperson::HeadMeshBit &&
          HdDrawMeshBits(0x600, true, false) == 0x600,
          "ordinary first-person HD body and hand passes hide only the head");

    for (int rate : {30, 60, 90, 120, 360}) {
        float yaw = 0;
        for (int i = 0; i < rate; ++i) yaw += StickTurn(1, 0.25f, 120, 1.0f / rate);
        CheckNear(yaw * 180 / Pi, 120, "one-second turn independent of input polling rate", 1e-4f);
    }
    CheckNear(StickTurn(0.1f, 0.25f, 120, 0.01f), 0, "stick drift cannot turn view");
    CheckNear(StickTurn(1, 0.25f, 120, 20) * 180 / Pi, 6, "pause cannot queue a large turn");

    for (int polls : {30, 180}) {
        tr::stabilization::RenderTurn turn;
        float yaw = 0;
        turn.Sample(120 * Pi / 180, 0);
        for (int frame = 1; frame <= 90; ++frame) {
            const double time = double(frame) / 90;
            for (int poll = 1; poll <= polls; ++poll) {
                const double pollTime = double(poll) / polls;
                if (pollTime > double(frame - 1) / 90 && pollTime <= time)
                    turn.Sample(120 * Pi / 180, pollTime);
            }
            yaw += turn.Step(time);
        }
        CheckNear(yaw * 180 / Pi, 120,
                  "render turn independent of controller poll rate", 1e-4f);
    }
    tr::stabilization::RenderTurn released;
    released.Sample(Pi, 0);
    CheckNear(released.Step(0.01), Pi * 0.01f, "render turn advances on scene view");
    released.Sample(0, 0.02);
    CheckNear(released.Step(0.03), 0, "released stick has no render-turn tail");
    released.Sample(Pi, 0.04);
    CheckNear(released.Step(0.20), 0, "stale input poll cannot keep turning");

    tr::stabilization::RootMotion root;
    tr::locomotion::Vec corrected{};
    Check(root.Step({0, 20}, {1, 0}, 1, 1, corrected) &&
          corrected.x == 20 && corrected.z == 0,
          "walking root follows requested horizontal direction");
    Check(root.Step({0, 28}, {1, 0}, 1, 1, corrected) &&
          corrected.x == 22,
          "same-gait root speed changes smoothly");
    Check(root.Step({0, 20}, {-1, 0}, 1, 1, corrected) &&
          corrected.x == -20,
          "direction reversal starts at native speed");
    Check(root.Step({0, 0}, {1, 0}, 1, 1, corrected) &&
          corrected.x == 0 && corrected.z == 0 && !root.valid,
          "zero native root motion stops without a tail");

    using namespace tr::motiongun;
    const float controller[3][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    const auto gun=GunBasis(ControllerBasis(controller,0));
    const auto barrel=Transform(gun,{0,1,0});
    CheckNear(barrel.z,1,"tracked barrel points along controller forward");
    int32_t flash[12]={16384,0,0,1600, 0,16384,0,3200,
                       0,0,16384,4800};
    Check(RetargetFlashMatrix(flash,gun,{10,20,30}) &&
          flash[0]==16384 && flash[1]==0 && flash[2]==0 &&
          flash[3]==1600+10*16384 && flash[7]==3200+20*16384 &&
          flash[11]==4800+30*16384,
          "muzzle flash keeps world axes used by the HD skin palette");
    for (float heading : {0.f,Pi/2,Pi,-Pi/2,.73f}) {
        const auto rotatedGun=GunBasis(ControllerBasis(controller,heading));
        const tr::motiongun::Vec nativeHand{30,-400,120};
        const tr::motiongun::Vec renderOrigin{200,0,300};
        const auto trackedHand=HandInWorld({100,-500,200},.2f,-.1f,.4f,heading,1000);
        const auto nativeRelative=Sub(nativeHand,renderOrigin);
        int32_t matrix[12]={16384,0,0,int32_t(nativeRelative.x*16384),
                           0,16384,0,int32_t(nativeRelative.y*16384),
                           0,0,16384,int32_t(nativeRelative.z*16384)};
        Check(RetargetFlashMatrix(matrix,rotatedGun,Sub(trackedHand,nativeHand)),
              "flash correction accepts artificial turn");
        const auto local=MuzzleLocal(1,1,0);
        const auto muzzle=Add(trackedHand,Transform(rotatedGun,local));
        // Native DrawGunFlash adds its local barrel offset before the shared
        // renderer applies the scene view. Reconstruct that world position.
        const tr::motiongun::Vec flashMuzzle{
            renderOrigin.x+(matrix[3]+matrix[0]*local.x+matrix[1]*local.y+matrix[2]*local.z)/16384.f,
            renderOrigin.y+(matrix[7]+matrix[4]*local.x+matrix[5]*local.y+matrix[6]*local.z)/16384.f,
            renderOrigin.z+(matrix[11]+matrix[8]*local.x+matrix[9]*local.y+matrix[10]*local.z)/16384.f};
        Check(std::fabs(flashMuzzle.x-muzzle.x)<.03f &&
              std::fabs(flashMuzzle.y-muzzle.y)<.03f &&
              std::fabs(flashMuzzle.z-muzzle.z)<.03f,
              "flash and bullet muzzle agree after stick rotation");
        const auto ray=Transform(rotatedGun,{0,1,0});
        float hitDistance=0;
        Check(ShotSphere(muzzle,ray,Add(muzzle,Scale(ray,1000)),100,hitDistance) &&
              std::fabs(hitDistance-900)<.01f,
              "barrel finds enemy sphere without head/body target at every heading");
    }
    // FireShotgun uses four world-space GetJointAbsPosition queries instead
    // of DrawGunFlash. Exercise physical yaw separately from artificial yaw,
    // including their combinations and a full physical revolution.
    const uint32_t effectCallers[4]={0x100,0x200,0x300,0x400};
    for (auto caller : effectCallers)
        Check(IsShotgunEffectQuery(caller,effectCallers,true,4,10),
              "shotgun smoke/spark origin and direction calls are admitted");
    Check(!IsShotgunEffectQuery(0x101,effectCallers,true,4,10) &&
          !IsShotgunEffectQuery(0x100,effectCallers,false,4,10) &&
          !IsShotgunEffectQuery(0x100,effectCallers,true,1,10) &&
          !IsShotgunEffectQuery(0x100,effectCallers,true,4,13) &&
          !IsShotgunEffectQuery(0,effectCallers,true,4,10),
          "other joint queries, enemies, pistols and left hands remain native");
    for (float stick : {0.f,Pi/2,-Pi/2,.73f}) {
        for (int step=0;step<=12;++step) {
            const float physical=step*Pi/6;
            const float c=std::cos(physical),s=std::sin(physical);
            // OpenVR right-handed yaw corresponding to a TR +Y-down turn.
            const float pose[3][4]={{c,0,-s,0},{0,1,0,0},{s,0,c,0}};
            const auto basis=GunBasis(ControllerBasis(pose,stick));
            const auto hand=HandInWorld({100,-500,200},
                .2f*c+.4f*s,-.1f,-.2f*s+.4f*c,stick,1000);
            int32_t smoke[3]={0,228,32}, spark[3]={0,356,82}, end[3]={0,1508,32};
            Check(RetargetEffectPoint(smoke,basis,hand) &&
                  RetargetEffectPoint(spark,basis,hand) &&
                  RetargetEffectPoint(end,basis,hand),
                  "shotgun effect points accept physical and mixed turns");
            const float yaw=stick+physical;
            const auto atBarrel=[&](const int32_t* point,float forward,float up) {
                return std::fabs(point[0]-(hand.x+std::sin(yaw)*forward))<=.51f &&
                       std::fabs(point[1]-(hand.y-up))<=.51f &&
                       std::fabs(point[2]-(hand.z+std::cos(yaw)*forward))<=.51f;
            };
            Check(atBarrel(smoke,228,32) && atBarrel(spark,356,82) && atBarrel(end,1508,32),
                  "shotgun particles follow physical yaw through 360 degrees");
            Check(std::fabs((end[0]-smoke[0])-1280*std::sin(yaw))<=1.01f &&
                  end[1]==smoke[1] &&
                  std::fabs((end[2]-smoke[2])-1280*std::cos(yaw))<=1.01f,
                  "shotgun smoke velocity turns with its origin");
            // Emission belongs to the controller, independent of Lara's old
            // or current body yaw. Pitched/rolled calibrated guns use the same
            // frame as the visible mesh, not headset yaw added a second time.
            Calibration fit{}; fit.pitchDegrees=-30; fit.rollDegrees=19;
            const auto calibrated=GunBasis(CalibratedController(ControllerBasis(pose,stick),fit));
            const auto grip=GripFrame(calibrated,hand,75,-29.55f,4);
            int32_t local[3]={0,356,82};
            const auto meshPoint=Transform(grip,{0,356,82});
            Check(RetargetEffectPoint(local,grip.basis,grip.origin) &&
                  std::fabs(local[0]-meshPoint.x)<=.51f &&
                  std::fabs(local[1]-meshPoint.y)<=.51f &&
                  std::fabs(local[2]-meshPoint.z)<=.51f,
                  "shotgun flare stays on calibrated gun under yaw pitch and roll");
        }
    }
    int32_t invalidEffect[3]={0,228,32};
    Check(!RetargetEffectPoint(invalidEffect,gun,{NAN,0,0}) &&
          invalidEffect[0]==0 && invalidEffect[1]==228 && invalidEffect[2]==32,
          "invalid tracked effect point preserves the original query for fallback");
    float sphereDistance=0;
    Check(!ShotSphere({0,0,0},{0,0,1},{200,0,600},100,sphereDistance) &&
          !ShotSphere({0,0,0},{0,0,1},{0,0,-600},100,sphereDistance) &&
          !ShotSphere({0,0,0},{0,0,1},{0,0,600},0,sphereDistance),
          "sphere selection rejects sideways, rear and disabled hit spheres");
    const Basis identity{{{1,0,0},{0,1,0},{0,0,1}}};
    const Frame wrist{identity,{100,-500,200}};
    Frame inverseBind{},correction{};
    Check(Inverse(wrist,inverseBind) &&
          PaletteCorrection(Multiply(wrist,inverseBind),inverseBind,
                            {gun,{130,-520,240}},correction),
          "tracked wrist correction accepts nonzero bind pivot");
    CheckNear(Transform(Multiply(correction,Multiply(wrist,inverseBind)),
                        Transform(wrist,{0,0,0})).x,130,
              "corrected skin palette places wrist at controller");
    Check(HandOnlyMask(0x600)==0x400 && HandOnlyMask(0x3000)==0x2000 &&
          HandOnlyMask(0x3600)==0x2400,
          "tracked render keeps equipped hands and removes forearms");
    tr::motiongun::Vec assisted{};
    Check(AssistedDirection({0,0,0},{0,0,1},{30,0,600},assisted) &&
          !AssistedDirection({0,0,0},{0,0,1},{300,0,600},assisted),
          "native selected target assists only inside narrow barrel cone");
    Check(AssistedDirection({0,0,0},{0,0,1},{300,0,600},assisted,45) &&
          !AssistedDirection({0,0,0},{0,0,1},{900,0,600},assisted,45) &&
          !AssistedDirection({0,0,0},{0,0,1},{30,0,600},assisted,0),
          "configurable first-person aim widens native target assist and can be disabled");
    CalibrationKeys calibrationKeys;
    int command=-1;
    Check(calibrationKeys.Event(0,true,true,false,false,true,false,command) &&
          command==0 &&
          calibrationKeys.Event(0,true,true,false,false,true,true,command) &&
          command==-1 &&
          calibrationKeys.Event(0,false,false,false,false,false,false,command),
          "live gun-fit hotkeys consume one press and its release without repeats");
    Check(!calibrationKeys.Event(1,true,false,false,false,true,false,command) &&
          !calibrationKeys.Event(1,true,true,false,true,true,false,command),
          "plain and Alt function keys retain their normal input");
    Calibration fit{};
    const float initialRight=fit.rightMetres;
    AdjustCalibration(fit,1);
    CheckNear(fit.rightMetres-initialRight,.00635f,
              "live gun-fit position key moves one quarter inch");
    AdjustCalibration(fit,10);
    CheckNear(fit.pitchDegrees,-29.0f,
              "live gun-fit angle key changes pitch by one degree");
    TriggerInput trigger;
    trigger.Update(true,true,false,false,0);
    trigger.Update(true,true,true,false,100);
    trigger.Update(true,true,false,false,200);
    Check(trigger.Consume(0) && !trigger.Consume(1),
          "left trigger tap queues only the left gun");
    trigger.Update(true,true,true,false,1000);
    trigger.Update(true,true,true,false,1600);
    Check(trigger.Equip(1600) && !trigger.WantsShot(),
          "long left hold requests equip without firing");
    TriggerInput sustainedRight;
    sustainedRight.Update(true,true,false,false,0);
    sustainedRight.Update(true,true,false,true,20);
    Check(sustainedRight.Consume(1),"RT requests an initial native shot");
    sustainedRight.Update(true,true,false,true,100);
    Check(sustainedRight.Consume(1),"held RT remains able to fire after a shot");
    sustainedRight.Update(true,false,false,true,120);
    sustainedRight.Update(true,true,false,true,180);
    Check(sustainedRight.Consume(1),"held RT resumes after a hit interruption");
    TriggerInput both;
    both.Update(true,true,false,false,0);
    for (uint64_t t=10;t<3000;t+=100) {
        both.Update(true,true,true,true,t);
        Check(both.Consume(0) && both.Consume(1) && !both.Equip(t),
              "both triggers keep firing beyond holster threshold while guns stay armed");
    }
    both.Update(true,true,true,false,3200);
    Check(both.Consume(0) && !both.Equip(3200),
          "releasing RT first cannot turn a dual fire hold into holstering");
    both.Update(true,true,false,false,3300);
    both.Update(true,true,true,false,3400);
    both.Update(true,true,true,false,4000);
    Check(both.Equip(4000),"fresh solo LT hold still holsters after dual firing");
    both.Update(true,true,true,true,4300);
    Check(both.Consume(0) && both.Consume(1) && !both.Equip(4300),
          "RT clears an already latched LT hold when guns are still ready");
    int bat=1;
    void* nativeTarget=&bat;
    TriggerInput attackingBat;
    attackingBat.Update(true,true,false,false,0);
    unsigned shots=0;
    // Native AnimatePistols raises/fires with an arm lock OR with fire input
    // and a null native target. A bat can remain targeted after both arm
    // locks are lost. Exercise sustained triggers through that transition.
    for (uint64_t t=20;t<=120000;t+=20) {
        attackingBat.Update(true,true,true,true,t);
        const bool locked=t<2000;
        const bool fire=attackingBat.WantsShot();
        if (t==2000) Check(fire && !(locked || (fire && !nativeTarget)),
            "reproduce drawn guns blocked by an unlocked native bat target");
        {
            ScopedControllerAim scope(nativeTarget,fire);
            if ((locked || (fire && !nativeTarget)) && t%200==0) {
                shots+=attackingBat.Consume(0);
                shots+=attackingBat.Consume(1);
            }
        }
        if (nativeTarget!=&bat || attackingBat.Equip(t)) break;
    }
    Check(shots==1200 && nativeTarget==&bat,
          "two-minute dual-trigger hold keeps native cadence after arm-lock loss and restores targeting");
    { ScopedControllerAim scope(nativeTarget,false);
      Check(nativeTarget==&bat,"third-person/non-firing target remains untouched"); }
    TriggerInput hitRecovery;
    hitRecovery.Update(true,true,false,false,10);
    hitRecovery.Update(false,false,true,false,20); // interrupted by hit camera
    hitRecovery.Update(true,true,true,false,30);   // LT was already held
    hitRecovery.Update(true,true,true,true,40);    // fresh RT press
    Check(hitRecovery.Consume(1) && hitRecovery.Consume(0),
          "both held triggers resume dual firing after a hit interruption");
    hitRecovery.Update(true,true,false,false,50);
    hitRecovery.Update(true,true,true,false,60);
    hitRecovery.Update(true,true,false,false,80);
    Check(hitRecovery.Consume(0) && !hitRecovery.waitRelease,
          "LT release and new tap recover after interrupted armed state");
    TriggerInput leftRecovery;
    leftRecovery.Update(false,false,false,true,100);
    leftRecovery.Update(true,true,false,true,110);
    Check(leftRecovery.Consume(1),"held RT resumes after adapter reset without a release");
    leftRecovery.Update(true,true,true,true,120);
    leftRecovery.Update(true,true,false,true,180);
    Check(leftRecovery.Consume(0) && leftRecovery.Consume(1) &&
          !leftRecovery.leftWaitRelease,
          "LT works alongside resumed RT after damage");
    float verticalYaw=0,verticalPitch=0;
    Check(DirectionAngles({0,-1,0},verticalYaw,verticalPitch) &&
          std::fabs(verticalPitch-Pi/2)<.0001f &&
          DirectionAngles({0,1,0},verticalYaw,verticalPitch) &&
          std::fabs(verticalPitch+Pi/2)<.0001f &&
          !DirectionAngles({0,0,0},verticalYaw,verticalPitch),
          "aiming vertically at flying enemies remains a valid shot direction");
    tr::wristcap::Vertex cut[3]{};
    cut[0].position={-10,0,0}; cut[1].position={10,0,0}; cut[2].position={0,10,0};
    for (auto& v:cut) { v.joint[0]=10; v.joint[1]=9; }
    cut[0].weight[1]=cut[1].weight[1]=1;
    cut[2].weight[0]=1;
    tr::wristcap::Edge rim[2];
    Check(tr::wristcap::CutTriangle(cut,10,rim),"mixed wrist triangle contributes a sealing rim segment");
    float capPalette[384]{};
    for (int j=0;j<32;++j) { capPalette[j*12]=capPalette[j*12+5]=capPalette[j*12+10]=1; capPalette[j*12+3]=100; }
    capPalette[9*12+3]=-1000; // Forearm animation must not pull the cap away.
    const auto edgeA=tr::wristcap::Rim(rim[0],capPalette,10);
    const auto edgeB=tr::wristcap::Rim(rim[1],capPalette,10);
    Check(std::fabs(edgeA.y-5.f)<.001f && std::fabs(edgeB.y-5.f)<.001f &&
          std::fabs(edgeA.x-105.f)<.001f && std::fabs(edgeB.x-95.f)<.001f,
          "wrist cap matches the rigid wrist and 50-percent seam independently of forearm pose");
    Check(!tr::wristcap::CutTriangle(cut,13,rim),"opposite hand contributes no cap geometry");
    tr::wristcap::Boundary boundary;
    // Duplicated texture vertices and repeated render-pass faces form one
    // square surface, not six independent edges or a sealed manifold.
    tr::wristcap::Vertex square[4]{};
    square[0].position={-1,-1,0}; square[1].position={1,-1,0};
    square[2].position={1,1,0}; square[3].position={-1,1,0};
    for (auto& v:square) { v.joint[0]=10; v.weight[0]=1; }
    const int faces[2][3]={{0,1,2},{0,2,3}};
    for (int repeat=0;repeat<2;++repeat) for (const auto& face:faces) {
        tr::wristcap::Vertex t[3]={square[face[0]],square[face[1]],square[face[2]]};
        boundary.Add(t,10);
    }
    const auto loops=boundary.Loops();
    Check(loops.size()==1 && loops[0].size()==4,
          "native open boundaries survive full hand weights, split texture vertices and duplicate passes");
    EquipInput nativeEquip;
    Check(nativeEquip.Update(true,0,true) &&
          nativeEquip.Update(true,2,false) &&
          nativeEquip.Update(true,4,false) &&
          nativeEquip.Update(true,4,false),
          "modern native draw stays held through ready after LT release");
    Check(!nativeEquip.Update(true,4,true) &&
          !nativeEquip.Update(true,3,false) &&
          !nativeEquip.Update(true,0,false),
          "next equip gesture releases native hold to holster");
    EquipInput unfocusedEquip;
    const bool vrPoll=VrGunInputEnabled(false,true,false);
    Check(vrPoll && unfocusedEquip.Update(true,0,true) &&
          VrGunInputEnabled(false,true,false) &&
          unfocusedEquip.Update(true,2,false) &&
          unfocusedEquip.Update(true,4,false) &&
          !VrGunInputEnabled(true,true,false) &&
          !VrGunInputEnabled(false,false,false),
          "SteamVR LT stays armed after desktop mirror loses foreground focus");

    int32_t samples[18];
    for (int i = 0; i < 18; i += 3) {
        samples[i] = 900; samples[i + 1] = -900; samples[i + 2] = 0;
    }
    Check(!tr::firstperson::EyeBlocked(samples, 0, -500, false),
          "airborne eye may see over a floor drop");
    samples[3] = -480;
    Check(tr::firstperson::EyeBlocked(samples, 0, -500, false),
          "raised crate floor blocks airborne eye at its height");
    samples[3] = 900;
    Check(!tr::firstperson::EyeBlocked(samples, 0, -1000, false),
          "airborne eye above crate clears its top");
    Check(tr::firstperson::EyeBlocked(samples, 0, -500, true),
          "static obstacle blocks airborne eye");

    tr::stabilization::GroundEye standing;
    const auto firstEye = standing.Apply({100, 0, 200}, 0, {110, -700, 220});
    const auto heldEye = standing.Apply({100, 0, 200}, 0, {140, -675, 245});
    CheckNear(firstEye.x, heldEye.x, "animated ground-head sway is stabilized");
    CheckNear(firstEye.y, heldEye.y, "animated ground-head bounce is stabilized");
    const auto steppedEye = standing.Apply({125, 0, 200}, 0, {165, -675, 245});
    CheckNear(steppedEye.x - heldEye.x, 25, "body root motion remains unfiltered");
    const auto turnedEye = standing.Apply({125, 0, 200}, Pi / 2, {165, -675, 245});
    CheckNear(turnedEye.x, 145, "artificial turn pivots standing eye reference");
    CheckNear(turnedEye.z, 190, "artificial turn rotates the forward eye offset");
    tr::stabilization::GroundEye vaultEye;
    const auto staleVault=vaultEye.Apply({0,-768,0},0,{0,-890,0});
    Check(!vaultEye.valid && staleVault.y==-890,
          "first grounded vault frame uses the native joint, not a guessed height");
    const auto recoveredVault=vaultEye.Apply({0,-768,0},0,{0,-1480,0});
    Check(vaultEye.valid && recoveredVault.y==-1480,
          "settled standing skeleton restores eye height after tall crate mount");
    const auto lateClimb=vaultEye.Apply({0,-768,0},0,{0,-1968,0});
    Check(lateClimb.y==-1480,
          "late pull-up skeleton cannot replace valid standing eye reference");

    tr::stabilization::GroundEye handoffEye;
    const auto calibrated=handoffEye.Apply({100,0,200},.35f,{120,-710,340});
    // The user looks around during a fixed camera. On return, the HMD gets a
    // new neutral while Lara's animation is looking sideways and leaning.
    handoffEye.Resume(true,.35f,-1.1f,0);
    auto resumed=handoffEye.Apply({400,0,500},-1.1f,{610,-650,510});
    CheckNear(resumed.x,420,"camera handoff preserves horizontal body centering despite return-pose sway");
    CheckNear(resumed.z,640,"headset yaw during a fixed camera cannot rotate the saved eye offset");
    CheckNear(resumed.y,calibrated.y,"camera handoff preserves calibrated standing height");
    // A scripted 90-degree body turn and teleport rotate/translate the same
    // calibration; a new head pose is still not a new body calibration.
    handoffEye.Resume(true,-1.1f,.8f,Pi/2);
    resumed=handoffEye.Apply({5000,-768,6000},.8f,{5300,-1400,6040});
    CheckNear(resumed.x,5140,"scripted body turn rotates the calibrated eye with Lara");
    CheckNear(resumed.z,5980,"scripted relocation keeps the eye centered on the moved body");
    CheckNear(resumed.y,-1478,"scripted relocation retains eye height above the new floor");
    float basis=.8f;
    for (int i=0;i<50;++i) {
        const float next=(i%2) ? -.7f : 1.4f;
        handoffEye.Resume(true,basis,next,0); basis=next;
        resumed=handoffEye.Apply({5000,-768,6000},basis,{5600,-1380,5900});
    }
    Check(std::fabs(resumed.x-5140)<.01f && std::fabs(resumed.z-5980)<.01f,
          "repeated camera takeovers do not accumulate a sideways mesh offset");
    handoffEye.Resume(false,basis,0,0);
    Check(!handoffEye.valid,"new level or Lara invalidates the old body calibration");
    resumed=handoffEye.Apply({0,0,0},0,{0,-720,100});
    CheckNear(resumed.z,100,"new body captures its own eye offset");

    const int32_t viewMatrix[12] = {
        16384,0,0,0, 0,16384,0,0, 0,0,16384,0
    };
    tr::actionicon::Point prompt{80, -80, 50};
    Check(tr::actionicon::Place(prompt, viewMatrix, 320, 640, 360, 32, 30000)
              && prompt.z >= 256,
          "nearby native Action prompt moves in front of first-person eye");
    tr::actionicon::Point normalPrompt{10, 0, 1000};
    Check(!tr::actionicon::Place(normalPrompt, viewMatrix, 320, 640, 360, 32, 30000)
              && normalPrompt.z == 1000,
          "visible Action prompt retains native world position");
    tr::actionicon::Point distantPrompt{10, 0, 1500};
    Check(!tr::actionicon::Place(distantPrompt, viewMatrix, 320, 640, 360, 32, 30000),
          "distant Action prompt is not pulled into view");
}

static void TestPhysicalBodyCentering() {
    using namespace tr::locomotion;
    using tr::stabilization::GroundEye;
    printf("\nphysical body centering with the stabilized camera and skin palette\n");
    const float units=423,neutralYaw=.37f,neck=.15f;
    const Vec nativeEye{17,155};
    bool centered=true,stableWorld=true,realLean=true,paddingClear=true;
    float previousBugPeak=0;
    for (float base : {0.f,.7f,-Pi/2,Pi}) for (bool stabilized : {false,true}) {
        GroundEye fit;
        const Vec initial=Rotate(nativeEye,base+neutralYaw);
        fit.Apply({0,0,0},base,{initial.x,-710,initial.z},base+neutralYaw);
        for (int degrees=-720;degrees<=720;degrees+=5) {
            const float headYaw=neutralYaw+degrees*Pi/180;
            const Vec arc=NeckToHead(headYaw,neck)-NeckToHead(neutralYaw,neck);
            for (Vec step : {Vec{},Vec{.3f,-.2f}}) {
                // Same tracking functions as VRSystem: neck displacement for
                // collision drag, raw eye displacement for the stable view.
                const Vec raw=arc+step;
                const Vec view=ViewFloorOffset(raw,arc,stabilized);
                const Vec floor=NeckFloorOffset(raw,arc);
                const float bodyYaw=base+headYaw;
                const Vec animated=Rotate(nativeEye,bodyYaw);
                const auto eye=fit.Apply({0,0,0},base,
                    {animated.x,-710,animated.z},bodyYaw);
                const Vec camera=Vec{eye.x,eye.z}+Rotate(view,base)*units;
                previousBugPeak=std::max(previousBugPeak,Length(camera-animated));
                const Vec shift=fit.BodyOffset(base,bodyYaw,view,floor,units);
                float palette[24]{};
                palette[0]=palette[5]=palette[10]=1;
                palette[3]=animated.x; palette[7]=-710; palette[11]=animated.z;
                tr::stabilization::OffsetBodyPalette(palette,2,shift);
                const Vec renderedEye{palette[3],palette[11]};
                centered &= Length(camera-renderedEye-Rotate(floor,base)*units)<.001f;
                stableWorld &= Length(camera-initial-Rotate(view,base)*units)<.001f;
                realLean &= Length(camera-renderedEye-Rotate(step,base)*units)<.001f;
                const float stick=Pi/3;
                const Vec pivoted=stabilized ? Rotate(raw,-stick) : PivotFloorOffset(raw,arc,stick);
                const Vec turnedView=ViewFloorOffset(pivoted,arc,stabilized);
                const Vec turnedFloor=NeckFloorOffset(pivoted,arc);
                const auto turned=fit.Apply({0,0,0},base+stick,{0,-710,0},bodyYaw+stick);
                const Vec turnedShift=fit.BodyOffset(base+stick,bodyYaw+stick,turnedView,turnedFloor,units);
                centered &= Length(Vec{turned.x,turned.z}+Rotate(turnedView,base+stick)*units-
                    Rotate(nativeEye,bodyYaw+stick)-turnedShift-Rotate(turnedFloor,base+stick)*units)<.001f;
                if (stabilized) stableWorld &= Length(Rotate(turnedView,base+stick)-Rotate(view,base))<.0001f;
                for (int i=12;i<24;++i) paddingClear &= palette[i]==0;
                paddingClear &= palette[0]==1 && palette[5]==1 && palette[10]==1 && palette[7]==-710;
            }
        }
        // A fixed camera changes the tracking basis. Subsequent physical
        // rotation must still use the original body-space calibration.
        const float nextBase=base+.9f;
        fit.Resume(true,base,nextBase,Pi/2);
        const float yaw=nextBase+1.1f;
        const Vec view{.08f,-.12f},floor{.03f,.04f};
        const auto eye=fit.Apply({0,0,0},nextBase,{300,-650,200},yaw);
        const Vec shift=fit.BodyOffset(nextBase,yaw,view,floor,units);
        centered &= Length(Vec{eye.x,eye.z}+Rotate(view,nextBase)*units-
            (Rotate(nativeEye,yaw)+shift)-Rotate(floor,nextBase)*units)<.001f;
    }
    Check(previousBugPeak>200,"reproduce the old intermediate-angle camera/mesh mismatch");
    Check(centered,"rendered mesh stays centered throughout two full physical turns in both directions and after camera handoff");
    Check(stableWorld,"physical body correction does not move the stable camera or world");
    Check(realLean,"body centering preserves genuine roomscale translation and leaning");
    Check(paddingClear,"body palette fit preserves rotations, height and hidden/padding matrices");

    // The grounded camera may look beyond a crate edge without requiring a
    // walkable floor there. Walls, ceilings and invalid rooms still block it.
    int32_t samples[18]{};
    for (int i=0;i<18;i+=3) { samples[i]=8192; samples[i+1]=-1000; }
    Check(!tr::firstperson::EyeBlocked(samples,0,-710,false),
          "grounded eye looks over deep ledge without camera retraction");
    samples[3]=-700;
    Check(tr::firstperson::EyeBlocked(samples,0,-710,false),"eye-height crate wall still blocks camera");
    samples[3]=8192; samples[4]=0;
    Check(tr::firstperson::EyeBlocked(samples,0,-710,false),"near ceiling still blocks camera at an edge");
    samples[4]=-32512;
    Check(tr::firstperson::EyeBlocked(samples,0,-710,false),"missing room is not treated as an open ledge");

    Check(AcceptCollisionDragStep({20,20},{0,20}) &&
          AcceptCollisionDragStep({0,32},{0,5}) &&
          !AcceptCollisionDragStep({0,5},{0,-30}) &&
          !AcceptCollisionDragStep({0,5},{0,30}) &&
          !AcceptCollisionDragStep({0,5},{30,0}) &&
          !AcceptCollisionDragStep({0,5},{0,0}),
          "roomscale accepts shortened/sliding travel but rejects collision pushback and overshoot");
    // Same feedback as DragBody -> interpolated ConsumeHeadFloorOffset. A
    // near-edge native pushback used to move the neutral the wrong way each
    // tick, eventually hitting the two-metre guard and persisting after jumps.
    bool noDrift=true,releases=true;
    float oldPending=.03f;
    for (int tick=0;tick<40;++tick) {
        if (Length(DragRequest({0,oldPending},.02f))<=2.f) oldPending+=30.f/units;
    }
    Check(oldPending>2.f,"reproduce accumulated ledge pushback reaching the persistent roomscale guard");
    for (int degrees=0;degrees<360;degrees+=15) {
        const float yaw=degrees*Pi/180;
        Vec pending{0,.03f};
        for (int tick=0;tick<300;++tick) {
            const Vec request=Rotate(DragRequest(pending,.02f),yaw)*units;
            const Vec pushback=Rotate({0,-30},yaw);
            if (AcceptCollisionDragStep(request,pushback)) pending=pending-Rotate(pushback,-yaw)*(1/units);
        }
        noDrift &= Length(pending-Vec{0,.03f})<.0001f;
        // Airborne frames consume nothing; landing on clear ground can then
        // accept the small remaining physical step without a view toggle.
        const Vec clear=Rotate(DragRequest(pending,.02f),yaw)*units;
        if (AcceptCollisionDragStep(clear,clear)) pending=pending-Rotate(clear,-yaw)*(1/units);
        releases &= Length(pending)<=.02001f;
    }
    Check(noDrift,"five seconds of ledge pushback at every heading cannot corrupt the tracking neutral");
    Check(releases,"jump and landing recover the remaining roomscale step without toggling views");

    GroundEye edgeFit;
    edgeFit.Apply({0,0,0},0,{17,-710,155},0);
    bool finalEyeCentered=true,calibrationStable=true;
    for (int frame=0;frame<120;++frame) {
        const float yaw=frame*Pi/30;
        const Vec native=Rotate(nativeEye,yaw);
        const Vec root{float(frame*20),float(frame*-10)};
        const auto eye=edgeFit.Apply({root.x,-768,root.z},0,
            {root.x+native.x,-1478,root.z+native.z},yaw);
        // Alternate free edge views and real wall retractions, including
        // leaving the obstacle. Only the current resolved eye is fitted.
        const Vec collision=(frame%3)==0 ? Vec{-40,-90} : Vec{};
        const Vec finalAnchor=Vec{eye.x,eye.z}+collision;
        const Vec view{.04f,.03f},floor{.01f,.02f};
        const Vec shift=edgeFit.BodyOffsetAtEye(0,yaw,view,floor,units,finalAnchor-root);
        float palette[12]={1,0,0,root.x+native.x,0,1,0,-1478,0,0,1,root.z+native.z};
        tr::stabilization::OffsetBodyPalette(palette,1,shift);
        finalEyeCentered &= Length(finalAnchor+view*units-
            Vec{palette[3],palette[11]}-floor*units)<.002f;
        calibrationStable &= edgeFit.local.x==17 && edgeFit.local.z==155;
    }
    Check(finalEyeCentered,"rendered torso follows final collision-resolved eye through repeated edges and turns");
    Check(calibrationStable,"edge and wall camera retractions never become permanent standing offsets");
}

static void TestGroundRollHeading() {
    using namespace tr::locomotion;
    printf("\nfirst-person native ground-roll heading\n");
    bool once=true,forward=true,tracking=true,centered=true;
    for (int degrees=-180;degrees<180;degrees+=15) {
        const float start=degrees*Pi/180,head=.37f;
        Heading heading; heading.Align(start,head);
        const float oldBase=heading.base;
        const int16_t initial=Angle(start);
        const int16_t reversed=int16_t(uint16_t(initial)^0x8000u);
        int flips=0;
        Vec manual=Rotate({0,1},heading.World(head));
        tr::stabilization::GroundEye eye;
        const Vec nativeEye{17,155},initialEye=Rotate(nativeEye,start);
        eye.Apply({0,0,0},oldBase,{initialEye.x,-710,initialEye.z},start);
        // The engine's orientation change can occur within either roll
        // state, at the roll-state boundary, or on its final exit tick.
        for (int tick=0;tick<90;++tick) {
            const int before=tick<30 ? 45 : tick<60 ? 23 : 2;
            const int after=tick<29 ? 45 : tick<59 ? 23 : 2;
            const float delta=GroundRollTurn(before,after,
                tick<=29 ? initial : reversed,tick<29 ? initial : reversed);
            if (delta!=0) { ++flips; heading.Turn(delta); manual=Rotate(manual,delta); }
        }
        once &= flips==1;
        forward &= Length(MovementWorld({0,1},heading.World(head))-
            Rotate({0,1},Radians(reversed)))<.001f;
        forward &= Length(manual-Rotate({0,1},heading.World(head)))<.001f;
        for (bool stabilized : {false,true}) {
            const Vec raw{.11f,-.08f},arc{.03f,.02f};
            const Vec after=stabilized ? Rotate(raw,-Pi) : PivotFloorOffset(raw,arc,Pi);
            // Same VR pivot as a stick turn: physical lean stays in world
            // space; no new neutral or extra roomscale displacement.
            const Vec beforeOffset=stabilized ? raw : NeckFloorOffset(raw,arc);
            const Vec afterOffset=stabilized ? after : NeckFloorOffset(after,arc);
            tracking &= Length(Rotate(beforeOffset,oldBase)-Rotate(afterOffset,heading.base))<.0001f;
        }
        const Vec finalNativeEye=Rotate(nativeEye,Radians(reversed));
        const auto finalEye=eye.Apply({100,0,200},heading.base,
            {100+finalNativeEye.x,-710,200+finalNativeEye.z},Radians(reversed));
        const Vec fit=eye.BodyOffsetAtEye(heading.base,Radians(reversed),{},{},423,
            {finalEye.x-100,finalEye.z-200});
        centered &= Length(Vec{finalEye.x-100,finalEye.z-200}-finalNativeEye-fit)<.001f;
        centered &= Length(Vec{finalEye.x-100,finalEye.z-200}+initialEye)<.001f;
    }
    Check(once,"one native half-turn changes VR heading once across repeated roll frames and yaw wrap");
    Check(forward,"held forward and subsequent movement use Lara's reversed heading");
    Check(tracking,"roll turn preserves roomscale lean through the artificial-turn pivot");
    Check(centered,"standing eye offset rotates to the new forward side and body remains centered after roll");
    Check(GroundRollTurn(45,45,0,-32768)!=0 && GroundRollTurn(23,2,0,-32768)!=0 &&
          GroundRollTurn(2,45,0,-32768)!=0,
          "native turn is detected inside the roll or on entry/exit animation ticks");
    Check(GroundRollTurn(45,2,123,123)==0 && GroundRollTurn(45,45,0,100)==0 &&
          GroundRollTurn(2,2,0,-32768)==0 && GroundRollTurn(66,66,0,-32768)==0 &&
          GroundRollTurn(72,72,0,-32768)==0,
          "cancelled rolls, ordinary turns and water/air rolls do not request a ground-roll flip");
    Heading twice; twice.Turn(GroundRollTurn(45,23,0,-32768));
    twice.Turn(GroundRollTurn(45,23,-32768,0));
    CheckNear(twice.World(0),0,"two completed ground rolls return to original heading");

    using tr::firstperson::RollEyeAboveFloor;
    bool aboveFloor=true,animationPreserved=true;
    for (int rootY : {0,-768,2048}) for (int floorDelta : {-120,0,200})
        for (double rise : {-180.,0.,120.}) for (int frame=0;frame<=90;++frame) {
            // A complete down/up roll, including frames where its skull
            // crosses the floor, physical ducking and different floor heights.
            const double animated=rootY-710+790*std::sin(frame*Pi/90);
            const double floor=rootY+floorDelta;
            const double resolved=RollEyeAboveFloor(animated,rise,rootY,floorDelta);
            aboveFloor &= resolved-rise<=floor-64+.0001;
            if (animated-rise<=floor-64)
                animationPreserved &= std::fabs(resolved-animated)<.0001;
        }
    Check(aboveFloor,"roll floor clearance protects the final tracked eye on flat, raised and sloping floors");
    Check(animationPreserved,"roll descent and recovery remain native whenever the eye clears the floor");
    CheckNear(float(RollEyeAboveFloor(-200,0,0,0)),-200,"low roll view is preserved above the clearance limit");
    CheckNear(float(RollEyeAboveFloor(80,0,0,0)),-64,"below-floor roll head stops close to the floor instead of standing height");
    CheckNear(float(RollEyeAboveFloor(-100,-180,0,0)),-244,"physical ducking is included before clamping and compensated in the anchor");
    CheckNear(float(RollEyeAboveFloor(20,120,0,0)),20,"physical head rise can already put the animated eye safely above the floor");
    CheckNear(float(RollEyeAboveFloor(-700,0,-768,-32512)),-832,"missing floor sample falls back to Lara's root plane");
    Check(IsGroundRollState(23) && IsGroundRollState(45) &&
          !IsGroundRollState(66) && !IsGroundRollState(72) && !IsGroundRollState(19),
          "roll floor clearance is limited to ground rolls, excluding water, air and pull-ups");
}

static void TestGunCalibrationPersistence() {
    printf("\nlive motion-gun fit persistence\n");
    wchar_t folder[MAX_PATH]{}, path[MAX_PATH]{};
    const DWORD length=GetTempPathW(MAX_PATH,folder);
    Check(length>0 && length<MAX_PATH &&
          GetTempFileNameW(folder,L"trv",0,path)!=0,
          "temporary calibration INI created");
    if (!path[0]) return;
    const std::wstring backup=std::wstring(path)+L".motion-gun-calibration.bak";
    WritePrivateProfileStringW(L"VR",L"FirstPersonMotionGunPitchDegrees",L"-30",path);
    WritePrivateProfileStringW(L"VR",L"FirstPersonAutoAimDegrees",L"40",path);
    tr::LoadConfig(path);
    CheckNear(tr::Cfg().firstPersonAutoAimDegrees,40,
              "first-person native-target assist cone loads from INI");
    tr::AdjustMotionGunCalibration(10);
    CheckNear(tr::LiveMotionGunCalibration().pitchDegrees,-29,
              "live calibration updates the pose used by tracked guns");
    Check(tr::SaveMotionGunCalibration() &&
          GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES,
          "saving live fit first backs up the INI");
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"VR",L"FirstPersonMotionGunPitchDegrees",L"",value,32,path);
    CheckNear(float(_wtof(value)),-29,"saved INI contains the live pitch");
    tr::AdjustMotionGunCalibration(10);
    tr::RestoreMotionGunCalibration();
    CheckNear(tr::LiveMotionGunCalibration().pitchDegrees,-29,
              "restore returns to the last saved gun fit");
    DeleteFileW(backup.c_str());
    DeleteFileW(path);
}

int main() {
    printf("TombRaiderVR self-test\n======================\n");

    TestProjectionMatchesEngine();
    TestAsymmetricShear();
    TestFrustumEdgesMapToNDC();
    TestNearFarRoundTrip();
    TestAffine();
    TestEngineSpaceConversion();
    TestPackedView();
    TestEngineConstBits();
    TestPortalGeometry();
    TestLocomotion();
    TestPhysicalBodyCentering();
    TestGroundRollHeading();
    TestGunCalibrationPersistence();
    TestInlineHook();

    printf("\n%s (%d failure%s)\n",
           g_fail == 0 ? "ALL PASSED" : "FAILURES",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
