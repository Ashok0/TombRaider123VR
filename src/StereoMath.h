// StereoMath.h -- matrix helpers written against this engine's actual
// conventions.
//
// NOTE ON THE FILENAME: this must not be called Math.h. `src` is on the include
// path, Windows filesystems are case-insensitive, and the CRT's <cmath> does
// `#include <math.h>` -- which would resolve to this file, get swallowed by
// #pragma once, and leave every std math declaration missing.
//
// Two conventions matter and neither is the textbook one.
//
// 1. PROJECTION (mProj[1], a mat4 uploaded with glUniformMatrix4fv, transpose
//    GL_FALSE, so plain column-major float[16]).
//
//    ogl_setPerspAngles builds:
//        m[0]  =  1/tanX          m[5]  = -1/tanY      <-- note the minus
//        m[8]  =  shearX          m[9]  =  shearY      (vid_setPerspOffset)
//        m[10] = (n+f)/(n-f)      m[11] = -1
//        m[14] = 2fn/(n-f)        m[15] =  0
//
//    That is glFrustum's layout with Y negated. The engine renders Y-flipped
//    throughout -- ogl_setScissor also flips Y against gTargetHeight.
//
// 2. VIEW (mView_packed, uploaded with glUniform4fv(loc, 4, ...), i.e. as
//    vec4[4], NOT as a matrix). vid_setViewMatrix packs it as:
//        float[0..3]   = (r00, r01, r02, tx)
//        float[4..7]   = (r10, r11, r12, ty)
//        float[8..11]  = (-r20, -r21, -r22, tz)
//        float[12..15] = (1, 0, ...)            unused by the shader
//
//    So it is a row-major 3x4 affine matrix, one row per vec4, with the
//    rotation in fixed point scaled by 1/16384 and the translation passed
//    through raw in TR world units. The third row's rotation is negated: that
//    is the handedness flip that puts the camera on -Z, GL style.
//
//    TR world space is Y-down. OpenVR is Y-up, in metres. Bridging the two is
//    a Y flip (S = diag(1,-1,1), applied on both sides of the eye transform so
//    it stays a rigid transform) plus a world-units-per-metre scale.
#pragma once

#include "Engine.h"
#include <cmath>
#include <cstring>

namespace tr {

// A 4x4 affine transform stored row-major: r[row][col]. Row 3 is implicitly
// (0,0,0,1) and is never stored.
struct Affine {
    float r[3][4];

    static Affine Identity() {
        Affine a{};
        a.r[0][0] = a.r[1][1] = a.r[2][2] = 1.0f;
        return a;
    }
};

// out = a * b   (apply b first, then a)
inline Affine Mul(const Affine& a, const Affine& b) {
    Affine o{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            o.r[i][j] = a.r[i][0] * b.r[0][j]
                      + a.r[i][1] * b.r[1][j]
                      + a.r[i][2] * b.r[2][j];
        }
        o.r[i][3] = a.r[i][0] * b.r[0][3]
                  + a.r[i][1] * b.r[1][3]
                  + a.r[i][2] * b.r[2][3]
                  + a.r[i][3];
    }
    return o;
}

// Inverse of a rigid transform: R^T, and -R^T * t.
inline Affine InvertRigid(const Affine& m) {
    Affine o{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            o.r[i][j] = m.r[j][i];
    for (int i = 0; i < 3; ++i) {
        o.r[i][3] = -(o.r[i][0] * m.r[0][3]
                    + o.r[i][1] * m.r[1][3]
                    + o.r[i][2] * m.r[2][3]);
    }
    return o;
}

// Conjugate by S = diag(1,-1,1) and rescale the translation.
//
// S*R*S negates exactly the rotation entries with one index equal to 1, and
// S*t negates t.y. det(S)^2 == 1, so the result is still a rigid transform.
// This is what converts an OpenVR-convention (Y-up, metres) transform into the
// engine's view space (Y-down, TR units).
inline Affine ToEngineSpace(const Affine& ovr, float unitsPerMetre, bool flipY) {
    Affine o = ovr;
    if (flipY) {
        o.r[0][1] = -o.r[0][1];
        o.r[1][0] = -o.r[1][0];
        o.r[1][2] = -o.r[1][2];
        o.r[2][1] = -o.r[2][1];
        o.r[1][3] = -o.r[1][3];
    }
    o.r[0][3] *= unitsPerMetre;
    o.r[1][3] *= unitsPerMetre;
    o.r[2][3] *= unitsPerMetre;
    return o;
}

// Rotation about the engine's Y axis, as a view-space pre-multiply. Diagnostic
// helper for DebugEyeYawDegrees.
inline Affine RotY(float degrees) {
    const float a = degrees * 3.14159265358979f / 180.0f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    Affine o{};
    o.r[0][0] =  c; o.r[0][1] = 0.0f; o.r[0][2] = s;   o.r[0][3] = 0.0f;
    o.r[1][0] = 0.0f; o.r[1][1] = 1.0f; o.r[1][2] = 0.0f; o.r[1][3] = 0.0f;
    o.r[2][0] = -s; o.r[2][1] = 0.0f; o.r[2][2] = c;   o.r[2][3] = 0.0f;
    return o;
}

// --- mView_packed <-> Affine -----------------------------------------------
//
// The packed layout IS the row-major affine matrix the shader consumes, so
// these are straight copies. Row 3 (floats 12..15) is left alone.

inline Affine ReadPackedView(const mat4& packed) {
    Affine a{};
    std::memcpy(a.r[0], &packed.m[0], 4 * sizeof(float));
    std::memcpy(a.r[1], &packed.m[4], 4 * sizeof(float));
    std::memcpy(a.r[2], &packed.m[8], 4 * sizeof(float));
    return a;
}

inline void WritePackedView(mat4& packed, const Affine& a) {
    std::memcpy(&packed.m[0], a.r[0], 4 * sizeof(float));
    std::memcpy(&packed.m[4], a.r[1], 4 * sizeof(float));
    std::memcpy(&packed.m[8], a.r[2], 4 * sizeof(float));
    // floats 12..15 are (1,0,...) and unused by the shader -- leave them.
}

// Full 4x4 product, column-major: (A*B)[c][r] = sum_k A[k][r] * B[c][k].
inline mat4 Mul4(const mat4& A, const mat4& B) {
    mat4 o{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += A.m[k * 4 + r] * B.m[c * 4 + k];
            o.m[c * 4 + r] = sum;
        }
    }
    return o;
}

// Widen a rigid 3x4 affine to a full 4x4 with bottom row (0,0,0,1).
inline mat4 AffineToMat4(const Affine& a) {
    mat4 o{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 3; ++r) o.m[c * 4 + r] = a.r[r][c];
    }
    o.m[3] = o.m[7] = o.m[11] = 0.0f;
    o.m[15] = 1.0f;
    return o;
}

// --- projection -------------------------------------------------------------

// Recover the near/far the engine currently has in mProj[1], so per-eye
// projection can preserve whatever the active pass chose.
//
//   A = m[10] = (n+f)/(n-f)      B = m[14] = 2fn/(n-f)
//   =>  n = B/(A-1),  f = B/(A+1)
inline bool ExtractNearFar(const mat4& proj, float& outNear, float& outFar) {
    const float A = proj.m[10];
    const float B = proj.m[14];
    const float dn = A - 1.0f;
    const float df = A + 1.0f;
    if (std::fabs(dn) < 1e-6f || std::fabs(df) < 1e-6f) return false;
    const float n = B / dn;
    const float f = B / df;
    if (!(n > 0.0f) || !(f > n)) return false;
    outNear = n;
    outFar  = f;
    return true;
}

// True when a projection matrix is ORTHOGRAPHIC rather than a perspective
// frustum.
//
// This exists because pointer identity is not enough to classify a pass.
// vid_setOrtho3D (0x0000B880) copies mProj[0] -- the ortho matrix -- into
// mProj[1] and repoints vid_state.proj at it, so IsWorldPass() reports
// "world space" for a layer that is orthographic. The inventory is drawn that
// way: real 3D item meshes, ortho projection.
//
// The bottom row separates them cleanly. ogl_setPerspAngles writes
// e32 = -1, e33 = 0; ogl_setOrtho writes e32 = 0, e33 = 1. In the engine's
// column-major layout (verified against the PDB's mat4: offset 0 = e00,
// 4 = e10, so index = col*4 + row) those are m[11] and m[15].
inline bool IsOrthoProjection(const mat4& proj) {
    return std::fabs(proj.m[11]) < 1e-6f && std::fabs(proj.m[15]) > 1e-6f;
}

// Build an asymmetric frustum from OpenVR's raw tangents into the engine's
// layout.
//
// l/r/t/b are exactly what IVRSystem::GetProjectionRaw returns. OpenVR's own
// GetProjectionMatrix uses idy = 1/(b-t) and sy = b+t, i.e. its top/bottom
// tangents run downward-positive; we mirror that and then apply the engine's
// Y negation, which is what `flipY` selects.
//
// Sanity check on the defaults: for a symmetric frustum (l=-k, r=k, t=-j, b=j)
// this yields m[0] = 1/k and m[5] = -1/j -- byte-for-byte what
// ogl_setPerspAngles(k, j, n, f) writes. The default path degenerates to the
// engine's own formula, which is the behaviour we want.
inline void BuildEyeProjection(mat4& out,
                               float l, float r, float t, float b,
                               float zNear, float zFar,
                               bool flipY) {
    std::memset(out.m, 0, sizeof(out.m));

    const float rl = r - l;
    const float bt = b - t;
    if (std::fabs(rl) < 1e-9f || std::fabs(bt) < 1e-9f) return;

    const float xScale = 2.0f / rl;
    const float xShear = (r + l) / rl;
    float       yScale = 2.0f / bt;
    const float yShear = (b + t) / bt;

    // Negate the Y SCALE only -- never the Y shear.
    //
    // This is the subtle one. There are two different operations that both look
    // like "flip Y", and only one is correct here:
    //
    //   (a) mirror the output image  -> negate the whole clip-space Y row,
    //                                   i.e. both m[5] and m[9]
    //   (b) compensate a Y-flipped INPUT -> negate only the terms that multiply
    //                                   vy, i.e. m[5] alone
    //
    // The engine's view space is Y-down (TR world space is Y-down), so what we
    // need is (b). m[9] is the vertical frustum shear and multiplies vz, which
    // is not flipped, so it must keep its sign.
    //
    // Getting this wrong is invisible on a symmetric frustum -- b + t == 0 makes
    // the shear zero either way, which is why a monitor and the symmetric
    // self-test both look fine. On a headset with an asymmetric vertical FOV it
    // puts the frustum edges in the wrong place: for t=-1.4281, b=0.9657 the top
    // edge lands at NDC 0.61 instead of 1.0, and the result reads as a fishbowl
    // warp that shifts as you turn your head.
    if (flipY) {
        yScale = -yScale;
    }

    const float nf = zNear - zFar;

    out.m[0]  = xScale;
    out.m[5]  = yScale;
    out.m[8]  = xShear;
    out.m[9]  = yShear;
    out.m[10] = (zNear + zFar) / nf;
    out.m[11] = -1.0f;
    out.m[14] = (2.0f * zFar * zNear) / nf;
    out.m[15] = 0.0f;
}

} // namespace tr
