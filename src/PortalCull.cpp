#include "PortalCull.h"
#include "PortalGeom.h"
#include "GameDll.h"
#include "FirstPerson.h"
#include "Engine.h"
#include "Config.h"
#include "InlineHook.h"
#include "StereoMath.h"
#include "VRSystem.h"
#include "Log.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>

namespace tr {
namespace {

// ---------------------------------------------------------------------------
// The engine state this file reads and writes
// ---------------------------------------------------------------------------
//
// All of it came out of the DLLs' own PDBs by name (see GameDll.h). Offsets are
// the ROOM_INFO layout, identical in tomb1/2/3.dll, from
//
//   python tools\typedump.py PDB\tomb1.dll ROOM_INFO
//
namespace off {
constexpr uint32_t room_stride = 168;

constexpr uint32_t room_door   = 8;    // int16*  portal list, null if none
constexpr uint32_t room_x      = 40;   // int32   room origin, world units
constexpr uint32_t room_y      = 44;
constexpr uint32_t room_z      = 48;
constexpr uint32_t room_bound  = 77;   // uint8   bit 0 = "already in draw_rooms"
constexpr uint32_t room_left   = 80;   // int16   screen clip rect
constexpr uint32_t room_right  = 82;
constexpr uint32_t room_top    = 84;
constexpr uint32_t room_bottom = 86;
constexpr uint32_t room_flags  = 102;  // uint16  bit 3 (8) = outdoor room
} // namespace off

// draw_rooms is int16[200] in all three DLLs (400 bytes, from pdbdump). In
// tomb1.dll `number_rooms` sits immediately after it, so overrunning the array
// would corrupt the room count -- this bound is load-bearing, not a nicety.
constexpr int kMaxDrawRooms = 200;

// ROOM_INFO::flags bit for "this room is outdoors". TR2/TR3 use it to decide
// whether the sky is drawn; TR1 has no such state.
constexpr uint16_t kRoomOutside = 0x0008;

// The engine's fixed-point shift. Rotations in w2v_matrix and in the matrix
// stack are stored as value * 16384, and so are the translations that were
// built from them (verified in CalcItemMatrices, which compares such a product
// straight against phd_znear = 0x80000 = 32 << 14).
constexpr float kW2VScale = 1.0f / 16384.0f;

// A portal record is 16 int16s: room number, then the normal, then four
// vertices, all room-relative. Read off GetRoomBounds, which steps its cursor
// by exactly 0x10 shorts per portal and hands SetRoomBounds a pointer one short
// past the room number.
constexpr int kPortalStride = 16;

// Everything below works in the eye space PortalGeom.h defines: viewpoint at
// the origin, X right, Y down, -Z forward.
using portal::Vec3;
using portal::Plane;
using portal::Poly;
using portal::Dot;
using portal::kMaxPlanes;

// ---------------------------------------------------------------------------
// World -> eye
// ---------------------------------------------------------------------------
//
// Three spaces are in play and the difference between two of them is a single
// sign that is easy to get wrong, so it is spelled out here once.
//
//   WORLD          TR world units, Y-DOWN.
//
//   PHD VIEW       what the game's own maths works in: v = R * (p - camPos),
//                  with R the rotation rows of w2v_matrix (scaled 1/16384) and
//                  camPos its columns 3/7/11. X right, Y down, +Z FORWARD --
//                  SetRoomBounds tests `z < 1` for "behind the camera".
//
//   EYE            what the mod's own matrices work in. mView_packed is the
//                  same view transform with its third rotation row NEGATED, so
//                  eye space is X right, Y down, -Z FORWARD. That is the space
//                  VRSystem::EyeView and HeadView are expressed in, because
//                  Hooks.cpp composes them as finalView = EyeView * gameView.
//
// So EYE = HeadView * N * PHD, with N = diag(1, 1, -1), and the whole chain
// collapses to one 3x3 plus two translations:
//
//   e = A * (p - camPos) + t,   A = HeadRot * N * R,   t = HeadTranslation
//
// A is orthogonal (a rotation times a reflection), which is the property the
// back-face test below relies on: it makes dot(A*n, A*(p-camPos) + t) equal to
// dot(n, p - headPos), i.e. the engine's own portal test with the head
// substituted for the camera, without ever computing the head's world position.
struct WorldToEye {
    float a[3][3];
    float c[3];     // camera world position, subtracted first to keep the
                    // magnitudes small before they reach float
    float t[3];
};

inline Vec3 Apply(const WorldToEye& m, float x, float y, float z) {
    const float dx = x - m.c[0], dy = y - m.c[1], dz = z - m.c[2];
    return Vec3{ m.a[0][0] * dx + m.a[0][1] * dy + m.a[0][2] * dz + m.t[0],
                 m.a[1][0] * dx + m.a[1][1] * dy + m.a[1][2] * dz + m.t[1],
                 m.a[2][0] * dx + m.a[2][1] * dy + m.a[2][2] * dz + m.t[2] };
}

// Direction only: no translation, no camera offset.
inline Vec3 Rotate(const WorldToEye& m, float x, float y, float z) {
    return Vec3{ m.a[0][0] * x + m.a[0][1] * y + m.a[0][2] * z,
                 m.a[1][0] * x + m.a[1][1] * y + m.a[1][2] * z,
                 m.a[2][0] * x + m.a[2][1] * y + m.a[2][2] * z };
}

// HeadRot * N, i.e. PHD VIEW -> EYE with no world stage. Used by the object
// test, whose input is already in phd view space via the matrix stack.
struct ViewToEye {
    float a[3][3];
    float t[3];
};

inline Vec3 Apply(const ViewToEye& m, const Vec3& v) {
    return Vec3{ m.a[0][0] * v.x + m.a[0][1] * v.y + m.a[0][2] * v.z + m.t[0],
                 m.a[1][0] * v.x + m.a[1][1] * v.y + m.a[1][2] * v.z + m.t[1],
                 m.a[2][0] * v.x + m.a[2][1] * v.y + m.a[2][2] * v.z + m.t[2] };
}

// HeadView, with its third COLUMN negated -- which is what post-multiplying by
// N = diag(1,1,-1) does. Negating the third column (rather than the third row,
// which is what the engine does when it packs the view matrix for the shader)
// is the difference between "convert my output to their convention" and
// "accept their input in mine", and only the latter is wanted here.
ViewToEye MakeViewToEye(const Affine& head) {
    ViewToEye m{};
    for (int i = 0; i < 3; ++i) {
        m.a[i][0] =  head.r[i][0];
        m.a[i][1] =  head.r[i][1];
        m.a[i][2] = -head.r[i][2];
        m.t[i]    =  head.r[i][3];
    }
    return m;
}

// A head this close to a portal is standing IN the doorway. Clipping the
// opening against planes that pass through the head is degenerate there -- a
// hair either way flips the whole room in or out -- so the parent frustum is
// passed through unchanged instead. 128 units is about a third of Lara's
// height, comfortably more than a frame of head motion.
constexpr float kPortalTouchUnits = 128.0f;

// ---------------------------------------------------------------------------
// The traversal
// ---------------------------------------------------------------------------

struct Ctx {
    WorldToEye m{};
    uint8_t*   rooms        = nullptr;
    int        numRooms     = 0;
    int16_t*   drawRooms    = nullptr;
    int32_t*   numDrawRooms = nullptr;

    int   maxDepth   = 0;
    int   budget     = 0;      // portals we may look at this frame
    float farUnits2  = 0.0f;   // squared; 0 = no distance limit

    int   visits     = 0;
    int   added      = 0;
    bool  sawOutside = false;
    bool  truncated  = false;
};

inline uint8_t* RoomAt(const Ctx& cx, int idx) {
    return cx.rooms + static_cast<uint32_t>(idx) * off::room_stride;
}

// Append one room to the engine's draw list, if it is not already there.
//
// The "already there" bit is ROOM_INFO::bound_active bit 0, which is what both
// traversal implementations use: TR1's SetRoomBounds writes the whole byte as
// 1, TR2/TR3's GetRoomBounds writes `bound_active | 1` over a counter it keeps
// in the upper bits. Testing and setting bit 0 alone is therefore correct for
// all three, and PrintRoomsList clears the byte for every listed room when it
// is done either way.
void Append(Ctx& cx, int idx) {
    uint8_t* r = RoomAt(cx, idx);
    uint8_t& active = *reinterpret_cast<uint8_t*>(r + off::room_bound);
    if (active & 1) return;

    if (*cx.numDrawRooms >= kMaxDrawRooms) { cx.truncated = true; return; }

    active |= 1;
    cx.drawRooms[(*cx.numDrawRooms)++] = static_cast<int16_t>(idx);
    ++cx.added;

    const uint16_t flags = *reinterpret_cast<uint16_t*>(r + off::room_flags);
    if (flags & kRoomOutside) cx.sawOutside = true;
}

void Visit(Ctx& cx, int idx, const Plane* planes, int nPlanes, int depth) {
    if (idx < 0 || idx >= cx.numRooms) return;

    Append(cx, idx);

    if (depth >= cx.maxDepth) { cx.truncated = true; return; }
    if (cx.visits >= cx.budget) { cx.truncated = true; return; }

    uint8_t* r = RoomAt(cx, idx);
    const int16_t* door = *reinterpret_cast<int16_t**>(r + off::room_door);
    if (!door) return;

    const int count = door[0];
    if (count <= 0) return;

    const float rx = static_cast<float>(*reinterpret_cast<int32_t*>(r + off::room_x));
    const float ry = static_cast<float>(*reinterpret_cast<int32_t*>(r + off::room_y));
    const float rz = static_cast<float>(*reinterpret_cast<int32_t*>(r + off::room_z));

    const int16_t* p = door + 1;
    for (int i = 0; i < count; ++i, p += kPortalStride) {
        if (cx.visits >= cx.budget) { cx.truncated = true; return; }
        ++cx.visits;

        const int nb = p[0];
        if (nb < 0 || nb >= cx.numRooms) continue;

        // The portal opening, in eye space. Vertices are room-relative, which
        // is why the room origin is added -- the same thing SetRoomBounds does
        // with its `param_3->x + param_1[3]`.
        Poly poly;
        poly.n = 4;
        for (int k = 0; k < 4; ++k) {
            poly.v[k] = Apply(cx.m,
                              rx + static_cast<float>(p[4 + k * 3]),
                              ry + static_cast<float>(p[5 + k * 3]),
                              rz + static_cast<float>(p[6 + k * 3]));
        }

        // The engine's own back-face test, with the head where the camera was.
        // A portal is only passable from the side its normal points at:
        // SetRoomBounds returns 0 when dot(normal, vertex - camera) >= 0, and
        // because the transform is orthogonal that comparison survives being
        // done in eye space, where the viewpoint is the origin.
        const Vec3 nrm = Rotate(cx.m, static_cast<float>(p[1]),
                                      static_cast<float>(p[2]),
                                      static_cast<float>(p[3]));
        const float facing = Dot(nrm, poly.v[0]);
        if (facing >= 0.0f) continue;

        // Distance limit, if one was asked for. Rejecting only when ALL four
        // corners are beyond it keeps a portal you are looking through edge-on.
        if (cx.farUnits2 > 0.0f) {
            bool allFar = true;
            for (int k = 0; k < 4 && allFar; ++k) {
                if (Dot(poly.v[k], poly.v[k]) <= cx.farUnits2) allFar = false;
            }
            if (allFar) continue;
        }

        // Standing in the doorway: pass the parent frustum through rather than
        // clip against planes the head is sitting on.
        const float nlen = std::sqrt(Dot(nrm, nrm));
        if (nlen > 0.0f && (-facing / nlen) < kPortalTouchUnits) {
            Visit(cx, nb, planes, nPlanes, depth + 1);
            continue;
        }

        bool overflow = false;
        for (int k = 0; k < nPlanes; ++k) {
            if (!portal::ClipPoly(poly, planes[k])) { overflow = true; break; }
            if (poly.n < 3) break;
        }

        // A polygon too complex to hold is treated as "still visible, frustum
        // unchanged". Over-inclusion costs a room; under-inclusion is the bug.
        if (overflow) {
            Visit(cx, nb, planes, nPlanes, depth + 1);
            continue;
        }
        if (poly.n < 3) continue;

        Plane child[kMaxPlanes];
        const int nChild = portal::BuildPlanes(poly, child);
        if (nChild < 4) {
            // Near plane plus fewer than three usable edges: not a solid
            // frustum. Inherit rather than invent one.
            Visit(cx, nb, planes, nPlanes, depth + 1);
        } else {
            Visit(cx, nb, child, nChild, depth + 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Binding and state
// ---------------------------------------------------------------------------

hook::InlineHook g_hPrintRoomsList;
hook::InlineHook g_hObjectBounds;

typedef void (__cdecl* Fn_PrintRoomsList)(void);
typedef int  (__cdecl* Fn_S_GetObjectBounds)(int16_t*);

// Verified with tools\prologue.py against all three DLLs. Both windows are
// identical in tomb1/2/3.dll, both stop on an instruction boundary, and neither
// contains a RIP-relative operand, so no displacement fixups are needed.
//
//   PrintRoomsList     48 89 5C 24 08     mov [rsp+8], rbx           -> 5
//   S_GetObjectBounds  40 55              push rbp
//                      57                 push rdi
//                      41 54              push r12                   -> 5
const uint8_t kPrintRoomsListPrologue[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
const uint8_t kObjectBoundsPrologue[]     = { 0x40, 0x55, 0x57, 0x41, 0x54 };

const GameDllLayout* g_boundDll  = nullptr;
uint64_t             g_boundBase = 0;

// The base we last failed to hook. InlineHook::Install refuses on a prologue
// mismatch, which is exactly what a patched game looks like -- and this runs
// every frame, so without the latch the failure would be retried and logged
// sixty times a second forever.
uint64_t             g_failedBase = 0;

PortalCullStats g_stats;
bool     g_loggedFirst   = false;
bool     g_loggedNoTan   = false;
bool     g_dumpKeyDown   = false;
bool     g_dumpRequested = false;

template <typename T>
T* Ptr(uint32_t rva) {
    return reinterpret_cast<T*>(g_boundBase + rva);
}

// True when there is a head pose worth culling from AND the mod is actually
// putting it in front of the geometry. With PerEyeView=0 the rendered view is
// the game camera's, so expanding the set would only add draw calls for
// geometry nothing can see -- EXCEPT under EyeOffsetMode=3, which reaches the
// GPU through the projection matrix and never touches the view matrix at all,
// so PerEyeView means nothing there. Same exception Config.cpp warns about.
bool Active() {
    if (!g_boundDll || !g_boundBase) return false;
    if (!Cfg().enabled || !Cfg().portalCulling) return false;
    if (!Cfg().perEyeView && Cfg().eyeOffsetMode != 3) return false;
    return VR().active() && VR().poseValid();
}

// The half-angles the traversal actually uses: the headset's, widened by
// CullFovMarginDegrees.
//
// The margin is applied as an ANGLE at the frustum edge -- tan(a + m) expanded
// -- rather than as a factor on the tangent, which would mean something
// different at every field of view. It buys two things: the couple of degrees
// the two eyes' frusta differ by once canted displays are allowed for (the
// traversal runs once, from the head, not once per pupil), and the head motion
// between this frame's cull and the pose the frame is finally rendered with.
void WidenByMargin(float& tanX, float& tanY) {
    const float margin = Cfg().cullFovMarginDegrees;
    if (!(margin > 0.0f)) return;
    const float k = std::tan(std::atan(1.0f) / 45.0f * margin);
    const float ex = (tanX + k) / (1.0f - tanX * k);
    const float ey = (tanY + k) / (1.0f - tanY * k);
    if (ex > tanX && ex < 60.0f) tanX = ex;   // 60 ~= 89 degrees a side
    if (ey > tanY && ey < 60.0f) tanY = ey;
}

// The headset's tangents, or a wide fallback if OpenVR has not produced any.
void CullTangents(float& tanX, float& tanY) {
    if (!VR().CullTangents(tanX, tanY)) {
        // 1.6 is about 116 degrees of total field of view, wider than any
        // consumer headset, so the traversal over-includes rather than culls
        // something that is really there.
        tanX = tanY = 1.6f;
        if (!g_loggedNoTan) {
            g_loggedNoTan = true;
            Log("cull: no eye projection from OpenVR yet -- culling with a "
                "116-degree fallback frustum until there is one");
        }
    }
    WidenByMargin(tanX, tanY);
}

// Assemble world -> eye for this frame, and the half-angles to cull with.
//
// A = HeadRot * N * R, with R the rotation rows of w2v_matrix scaled by 1/16384
// and N = diag(1, 1, -1). N is not a guess: vid_setViewMatrix (tomb123.exe RVA
// 0x0000A5E0) builds mView_packed from the same int[12] and negates exactly the
// third row -- e02/e12/e22 take -m[8]/-m[9]/-m[10] while every other term takes
// its input unchanged. That negation IS the difference between the space the
// game's own culling works in and the space VRSystem::HeadView is expressed in.
void BuildTransform(WorldToEye& out, float& tanX, float& tanY) {
    const int32_t* w2v = Ptr<int32_t>(g_boundDll->w2vMatrix);

    // The same head translation rule the stereo path uses, so the frustum this
    // culls with starts where the eyes really are. In first person that is
    // Lara's head, not the player's offset from the tracking origin.
    const Affine head = VR().HeadView(
        !FirstPersonActive() || Cfg().firstPersonHeadTranslation);

    // N * R: the world -> phd-view rotation with its third row negated.
    float nr[3][3];
    for (int j = 0; j < 3; ++j) {
        nr[0][j] =  static_cast<float>(w2v[0 + j]) * kW2VScale;
        nr[1][j] =  static_cast<float>(w2v[4 + j]) * kW2VScale;
        nr[2][j] = -static_cast<float>(w2v[8 + j]) * kW2VScale;
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.a[i][j] = head.r[i][0] * nr[0][j]
                        + head.r[i][1] * nr[1][j]
                        + head.r[i][2] * nr[2][j];
        }
        out.t[i] = head.r[i][3];
    }
    out.c[0] = static_cast<float>(w2v[3]);
    out.c[1] = static_cast<float>(w2v[7]);
    out.c[2] = static_cast<float>(w2v[11]);

    CullTangents(tanX, tanY);
}

// Widen every listed room's clip rect to the whole target.
//
// PrintRooms copies that rect into phd_left/right/top/bottom and CheckClipping
// turns it into a scissor box, so a room reached down a narrow corridor is
// pixel-clipped to where the GAME CAMERA saw its doorway. The stereo hook
// already replaces the scissor per draw with the eye's half of the target, so
// on the modern renderer this is belt and braces; on the classic renderer,
// where the rect also drives vertex clipping in calc_roomvert, it is the
// difference between a room being in the list and a room being visible.
void WidenBounds(const Ctx& cx) {
    const int32_t xmax = *Ptr<int32_t>(g_boundDll->phdWinXmax);
    const int32_t ymax = *Ptr<int32_t>(g_boundDll->phdWinYmax);

    for (int i = 0; i < *cx.numDrawRooms; ++i) {
        const int idx = cx.drawRooms[i];
        if (idx < 0 || idx >= cx.numRooms) continue;
        uint8_t* r = RoomAt(cx, idx);
        *reinterpret_cast<int16_t*>(r + off::room_left)   = 0;
        *reinterpret_cast<int16_t*>(r + off::room_top)    = 0;
        *reinterpret_cast<int16_t*>(r + off::room_right)  = static_cast<int16_t>(xmax);
        *reinterpret_cast<int16_t*>(r + off::room_bottom) = static_cast<int16_t>(ymax);
    }

    // TR2 and TR3 keep a second rect for the sky. GetRoomBounds grows it as it
    // meets outdoor rooms and sets `outside` when it meets the first one, so
    // adding an outdoor room the engine did not reach means doing both -- and
    // the rect wants the whole target for the same reason the room rects do.
    if (!g_boundDll->outside) return;

    int32_t* outside = Ptr<int32_t>(g_boundDll->outside);
    if (cx.sawOutside) *outside = static_cast<int32_t>(kRoomOutside);
    if (*outside == 0) return;

    *Ptr<int32_t>(g_boundDll->outsideLeft)   = 0;
    *Ptr<int32_t>(g_boundDll->outsideTop)    = 0;
    *Ptr<int32_t>(g_boundDll->outsideRight)  = xmax;
    *Ptr<int32_t>(g_boundDll->outsideBottom) = ymax;
}

void DumpList(const Ctx& cx, int engineRooms) {
    LogF("cull dump: %d rooms listed -- %d from the engine, %d added by the head "
         "frustum; %d portals looked at%s",
         *cx.numDrawRooms, engineRooms, cx.added, cx.visits,
         cx.truncated ? " (A BUDGET WAS HIT)" : "");

    // Room indices, sixteen to a line, with the ones we added starred. Printed
    // rather than summarised because the useful question in front of a glitch
    // is always "which room is that", and the answer has to be comparable with
    // the level editor's numbering.
    char line[160] = {};
    int  used = 0;
    for (int i = 0; i < *cx.numDrawRooms; ++i) {
        const int n = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
                                  " %d%s", cx.drawRooms[i],
                                  (i < engineRooms) ? "" : "*");
        used = (n < 0) ? static_cast<int>(sizeof(line)) - 1 : used + n;
        if ((i % 16) == 15 || i + 1 == *cx.numDrawRooms) {
            LogF("cull dump: %s", line);
            used = 0;
            line[0] = 0;
        }
    }
    Log("cull dump: * marks a room the engine's own traversal did not reach");
}

// ---------------------------------------------------------------------------
// PrintRoomsList -- expand the list, then let the engine draw it
// ---------------------------------------------------------------------------
void __cdecl Detour_PrintRoomsList() {
    if (!Active()) {
        g_hPrintRoomsList.Original<Fn_PrintRoomsList>()();
        return;
    }

    Ctx cx;
    cx.rooms        = *Ptr<uint8_t*>(g_boundDll->room);
    cx.numRooms     = *Ptr<int16_t>(g_boundDll->numberRooms);
    cx.drawRooms    =  Ptr<int16_t>(g_boundDll->drawRooms);
    cx.numDrawRooms =  Ptr<int32_t>(g_boundDll->numberDrawRooms);

    const int engineRooms = cx.numDrawRooms ? *cx.numDrawRooms : 0;

    // No level, or a draw list the engine has not seeded. Nothing to expand
    // from: the traversal needs a room it is known to be standing in.
    if (!cx.rooms || cx.numRooms <= 0 || engineRooms <= 0) {
        g_hPrintRoomsList.Original<Fn_PrintRoomsList>()();
        return;
    }

    float tanX = 0.0f, tanY = 0.0f;
    BuildTransform(cx.m, tanX, tanY);

    cx.maxDepth = Cfg().cullMaxDepth;
    cx.budget   = Cfg().cullMaxPortals;
    if (Cfg().cullFarUnits > 0.0f) {
        cx.farUnits2 = Cfg().cullFarUnits * Cfg().cullFarUnits;
    }

    // draw_rooms[0] is the room the engine seeded the traversal with -- the one
    // the game camera is in -- for both of GetVisibleRooms' callers and for
    // both of its implementations (TR1 writes it directly, TR2/TR3 reach it
    // first out of bound_list). Taking it from the list rather than from
    // camera.pos.room_number means the two can never disagree.
    const int seed = cx.drawRooms[0];

    Plane root[kMaxPlanes];
    const int nRoot = portal::RootPlanes(tanX, tanY, root);
    Visit(cx, seed, root, nRoot, 0);

    if (Cfg().cullWidenBounds) WidenBounds(cx);

    g_stats.rooms  += static_cast<unsigned>(engineRooms);
    g_stats.added  += static_cast<unsigned>(cx.added);
    g_stats.frames += 1;
    if (cx.truncated) g_stats.truncated += 1;

    if (!g_loggedFirst) {
        g_loggedFirst = true;
        LogF("cull: head-frustum portal traversal live on %s -- frustum "
             "%.1f x %.1f degrees, depth<=%d, %d portals/frame budget",
             g_boundDll->name,
             2.0f * std::atan(tanX) * 45.0f / std::atan(1.0f),
             2.0f * std::atan(tanY) * 45.0f / std::atan(1.0f),
             cx.maxDepth, cx.budget);
    }
    if (g_dumpRequested) {
        g_dumpRequested = false;
        DumpList(cx, engineRooms);
    }

    g_hPrintRoomsList.Original<Fn_PrintRoomsList>()();
}

// ---------------------------------------------------------------------------
// S_GetObjectBounds -- the same fix, one level down
// ---------------------------------------------------------------------------
//
// The engine's version projects the item's bounding box with the top of the
// matrix stack and answers 1 (wholly on screen), -1 (partly) or 0 (not at all).
// 0 is reached two ways, and BOTH of them are the game camera's opinion: every
// corner behind phd_znear, or the projected rectangle missing phd_left/right/
// top/bottom. An enemy behind the game camera fails the first; one beside it
// fails the second. So a room this file added would draw with nothing in it.
//
// Only the 0 answer is second-guessed, and only ever upward, to -1: "visible,
// clip it". Answering 1 would promote the item onto the unclipped fast path on
// the strength of a test the engine did not run.
int __cdecl Detour_S_GetObjectBounds(int16_t* bounds) {
    const int engine = g_hObjectBounds.Original<Fn_S_GetObjectBounds>()(bounds);
    if (engine != 0) return engine;

    if (!Active() || !Cfg().cullObjects || !bounds) return engine;

    // The inventory renders real meshes through an orthographic projection and
    // the title screen runs its own camera; neither is a world the head pose
    // means anything in, and overriding there would draw items the engine had
    // deliberately dropped.
    if (InInventory() || InTitle()) return engine;

    const int32_t* const* mxptr = Ptr<const int32_t*>(g_boundDll->phdMxptr);
    const int32_t* m = mxptr ? *mxptr : nullptr;
    if (!m) return engine;

    const ViewToEye v2e = MakeViewToEye(VR().HeadView());

    float tanX = 0.0f, tanY = 0.0f;
    CullTangents(tanX, tanY);

    Plane planes[kMaxPlanes];
    const int nPlanes = portal::RootPlanes(tanX, tanY, planes);

    // bounds is {MinX, MaxX, MinY, MaxY, MinZ, MaxZ} -- the order the engine's
    // own corner walk uses. The matrix is row-major 3x4 with rotation AND
    // translation pre-scaled by 16384, so one divide at the end recovers world
    // units; the products are formed in float because in int32 they overflow
    // (a 16384-scaled translation is already of the order of 1e9).
    Vec3 corner[8];
    int  ci = 0;
    for (int xi = 0; xi < 2; ++xi) {
        for (int yi = 0; yi < 2; ++yi) {
            for (int zi = 0; zi < 2; ++zi) {
                const float bx = static_cast<float>(bounds[0 + xi]);
                const float by = static_cast<float>(bounds[2 + yi]);
                const float bz = static_cast<float>(bounds[4 + zi]);
                const Vec3 view{
                    (m[0] * bx + m[1] * by + m[2]  * bz + static_cast<float>(m[3]))  * kW2VScale,
                    (m[4] * bx + m[5] * by + m[6]  * bz + static_cast<float>(m[7]))  * kW2VScale,
                    (m[8] * bx + m[9] * by + m[10] * bz + static_cast<float>(m[11])) * kW2VScale
                };
                corner[ci++] = Apply(v2e, view);
            }
        }
    }

    if (!portal::BoxVisible(corner, planes, nPlanes)) return 0;

    ++g_stats.items;
    return -1;
}

// ---------------------------------------------------------------------------
// Install / remove
// ---------------------------------------------------------------------------

bool Install(const GameDllLayout& d, uint64_t base) {
    auto fn = [base](uint32_t rva) { return reinterpret_cast<void*>(base + rva); };

    if (!g_hPrintRoomsList.Install(fn(d.printRoomsList),
                                   reinterpret_cast<void*>(&Detour_PrintRoomsList),
                                   5, kPrintRoomsListPrologue,
                                   sizeof(kPrintRoomsListPrologue),
                                   "PrintRoomsList")) {
        return false;
    }

    // The object hook is optional: without it the added rooms still draw, just
    // empty. So a failure here is reported and survived rather than rolling the
    // room fix back with it.
    if (Cfg().cullObjects) {
        if (!g_hObjectBounds.Install(fn(d.sGetObjectBounds),
                                     reinterpret_cast<void*>(&Detour_S_GetObjectBounds),
                                     5, kObjectBoundsPrologue,
                                     sizeof(kObjectBoundsPrologue),
                                     "S_GetObjectBounds")) {
            Log("cull: S_GetObjectBounds could not be hooked -- rooms behind the "
                "game camera will draw, but the items in them will not");
        }
    }
    return true;
}

void Remove() {
    g_hObjectBounds.Remove();
    g_hPrintRoomsList.Remove();
    g_boundDll  = nullptr;
    g_boundBase = 0;
}

} // namespace

void PortalCullUpdate() {
    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();

    const bool want = Cfg().enabled && Cfg().portalCulling && d && base;

    // Rebind whenever the DLL or its base changes. The player can switch games
    // from the title screen, and a hook left in a DLL that is no longer the one
    // running would patch code nothing calls at best.
    if (g_boundDll && (!want || d != g_boundDll || base != g_boundBase)) {
        LogF("cull: unhooking %S", g_boundDll->module);
        Remove();
    }
    if (!want || g_boundDll) return;
    if (base == g_failedBase) return;

    g_boundDll  = d;
    g_boundBase = base;
    if (!Install(*d, base)) {
        LogF("cull: PrintRoomsList could not be hooked in %S -- the engine's own "
             "culling stands, and geometry will disappear when you look away "
             "from the game camera. A prologue mismatch here means the game was "
             "patched; re-run tools\verify_addresses.py.", d->module);
        Remove();
        g_failedBase = base;
        return;
    }
    LogF("cull: hooked %S (%s)", d->module, d->name);
}

void PortalCullShutdown() {
    if (g_boundDll) Remove();
    g_loggedFirst = false;
    g_failedBase  = 0;
}

void PortalCullPollKey() {
    const int key = Cfg().cullDumpKey;
    if (key == 0) { g_dumpKeyDown = false; return; }
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (down && !g_dumpKeyDown) g_dumpRequested = true;
    g_dumpKeyDown = down;
}

PortalCullStats PortalCullTakeStats() {
    const PortalCullStats s = g_stats;
    g_stats = PortalCullStats();
    return s;
}

} // namespace tr
