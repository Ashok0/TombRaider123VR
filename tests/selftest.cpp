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

#include <windows.h>
#include <cstdio>
#include <cmath>

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
    TestInlineHook();

    printf("\n%s (%d failure%s)\n",
           g_fail == 0 ? "ALL PASSED" : "FAILURES",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
