// PortalGeom.h -- the frustum arithmetic behind PortalCull.cpp.
//
// Split out of PortalCull.cpp for one reason: it is the part that can be wrong
// without crashing, and it is the part that can be tested without a game, a
// headset or a hook. tests\selftest.cpp includes this header directly and
// checks it against cases worked out by hand.
//
// Everything here lives in EYE SPACE with the viewpoint at the ORIGIN:
// X right, Y down, -Z forward. That is why every plane a portal produces has
// zero distance -- it passes through the apex by construction -- and why the
// only plane in the set that does not is the near plane.
#pragma once

#include <cmath>

namespace tr {
namespace portal {

struct Vec3 { float x, y, z; };

inline float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return Vec3{ a.y * b.z - a.z * b.y,
                 a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x };
}

// A half-space. Inside is dot(n, p) + d >= 0.
struct Plane { Vec3 n; float d; };

// A clipped portal opening. Four vertices go in and one more comes out per
// plane that actually cuts it; 16 is far past anything a convex quad against a
// handful of planes produces. Overflow is reported rather than truncated,
// because a truncated polygon would cull geometry that is visible -- the exact
// failure this whole mechanism exists to remove.
constexpr int kMaxPolyVerts = 16;
constexpr int kMaxPlanes    = kMaxPolyVerts + 1;   // one per edge, plus near

struct Poly { int n; Vec3 v[kMaxPolyVerts]; };

// How close to the viewpoint the traversal starts, in TR world units. This is
// not a render near plane; it only keeps the portal maths on the forward side
// of the apex. 16 units is about 4 cm at the default world scale.
constexpr float kNearUnits = 16.0f;

// The starting frustum: the near plane, then four symmetric sides.
//
// Symmetric on purpose. The headset's own frustum is asymmetric and differs
// between the eyes, and culling wants ONE shape that contains both -- so the
// caller passes the largest half-angle tangent seen on either eye and the
// result is a superset of both. It also means nothing here depends on which
// way round OpenVR signs its raw tangents.
//
// `out` must have room for at least 5 planes. Returns how many were written.
inline int RootPlanes(float tanX, float tanY, Plane* out) {
    int n = 0;
    out[n++] = Plane{ Vec3{  0.0f,  0.0f, -1.0f }, -kNearUnits };
    out[n++] = Plane{ Vec3{  1.0f,  0.0f, -tanX },  0.0f };   // left
    out[n++] = Plane{ Vec3{ -1.0f,  0.0f, -tanX },  0.0f };   // right
    out[n++] = Plane{ Vec3{  0.0f,  1.0f, -tanY },  0.0f };   // up
    out[n++] = Plane{ Vec3{  0.0f, -1.0f, -tanY },  0.0f };   // down
    return n;
}

// Sutherland-Hodgman against one half-space.
//
// Returns false if the result did not fit, in which case `p` is left untouched
// and must not be trusted. On success `p` is the clipped polygon, which may be
// empty (n < 3) meaning the plane removed it entirely.
inline bool ClipPoly(Poly& p, const Plane& pl) {
    Poly out;
    out.n = 0;
    for (int i = 0; i < p.n; ++i) {
        const Vec3& a = p.v[i];
        const Vec3& b = p.v[(i + 1 == p.n) ? 0 : i + 1];
        const float da = Dot(pl.n, a) + pl.d;
        const float db = Dot(pl.n, b) + pl.d;
        const bool  ia = da >= 0.0f;
        const bool  ib = db >= 0.0f;

        if (ia) {
            if (out.n >= kMaxPolyVerts) return false;
            out.v[out.n++] = a;
        }
        if (ia != ib) {
            const float denom = da - db;
            if (std::fabs(denom) > 1e-20f) {
                const float s = da / denom;
                if (out.n >= kMaxPolyVerts) return false;
                out.v[out.n++] = Vec3{ a.x + (b.x - a.x) * s,
                                       a.y + (b.y - a.y) * s,
                                       a.z + (b.z - a.z) * s };
            }
        }
    }
    p = out;
    return true;
}

// Build the frustum that looks through an already-clipped opening: the near
// plane, plus one plane per edge running from the apex through that edge.
//
// The inward direction comes from the polygon's own centroid rather than from
// its winding. The engine stores portal vertices in a fixed order, but that
// order is relative to the room that owns the portal, and after clipping the
// surviving loop can be either way round -- testing against a point known to be
// inside is winding-independent and costs one dot product per edge.
//
// Edges the apex is collinear with are dropped rather than approximated:
// dropping a plane can only widen the frustum, and widening is the safe
// direction here. `out` must have room for kMaxPlanes. Returns how many were
// written; fewer than 4 (near plus three edges) means the opening did not
// produce a solid frustum and the caller should keep the one it had.
inline int BuildPlanes(const Poly& p, Plane* out) {
    int n = 0;
    out[n++] = Plane{ Vec3{ 0.0f, 0.0f, -1.0f }, -kNearUnits };
    if (p.n < 3) return n;

    Vec3 ctr{ 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < p.n; ++i) { ctr.x += p.v[i].x; ctr.y += p.v[i].y; ctr.z += p.v[i].z; }
    const float inv = 1.0f / static_cast<float>(p.n);
    ctr.x *= inv; ctr.y *= inv; ctr.z *= inv;

    for (int i = 0; i < p.n && n < kMaxPlanes; ++i) {
        const Vec3& a = p.v[i];
        const Vec3& b = p.v[(i + 1 == p.n) ? 0 : i + 1];
        Vec3 e = Cross(a, b);

        // Degenerate when the apex lies on the line through the edge. The
        // tolerances are relative to the operands so they mean the same thing
        // at any distance from the viewpoint.
        const float len2  = Dot(e, e);
        const float scale = Dot(a, a) * Dot(b, b);
        if (len2 <= 1e-10f * (scale + 1.0f)) continue;

        const float side = Dot(e, ctr);
        if (std::fabs(side) <= 1e-6f * std::sqrt(len2 * (Dot(ctr, ctr) + 1.0f))) continue;
        if (side < 0.0f) { e.x = -e.x; e.y = -e.y; e.z = -e.z; }

        const float k = 1.0f / std::sqrt(len2);
        out[n].n = Vec3{ e.x * k, e.y * k, e.z * k };
        out[n].d = 0.0f;
        ++n;
    }
    return n;
}

// Conservative box-versus-frustum: outside only when one plane has all eight
// corners behind it.
//
// The reverse test -- "is any corner inside" -- is wrong for a box larger than
// the frustum's cross-section, which is precisely the case of standing next to
// a big static mesh with the doorway framing its middle.
inline bool BoxVisible(const Vec3* corners8, const Plane* planes, int nPlanes) {
    for (int p = 0; p < nPlanes; ++p) {
        int out = 0;
        for (int i = 0; i < 8; ++i) {
            if (Dot(planes[p].n, corners8[i]) + planes[p].d < 0.0f) ++out;
        }
        if (out == 8) return false;
    }
    return true;
}

} // namespace portal
} // namespace tr
