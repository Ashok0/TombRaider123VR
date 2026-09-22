#include "FirstPerson.h"
#include "GameDll.h"
#include "Engine.h"
#include "Config.h"
#include "InlineHook.h"
#include "VRSystem.h"
#include "Log.h"
#include "LocomotionMath.h"

#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdlib>
#include <cstddef>

namespace tr {
namespace {

// --- engine types, from the PDBs (tools\typedump.py) ------------------------
//
// Identical in tomb1/2/3.dll, like every other struct this mod reads.

// 20 bytes. What phd_GenerateW2V consumes and what ITEM_INFO::pos is.
struct PHD_3DPOS {
    int32_t x_pos, y_pos, z_pos;   //  0,  4,  8
    int16_t x_rot, y_rot, z_rot;   // 12, 14, 16
    int16_t _padding;              // 18
};

namespace off {
// ITEM_INFO, 3664 bytes
constexpr uint32_t item_mesh_bits   = 12;    // uint32, one bit per mesh
constexpr uint32_t item_object_number = 16;  // int16
constexpr uint32_t item_anim_state    = 18;  // int16
constexpr uint32_t item_room_number = 28;    // int16
constexpr uint32_t item_speed       = 34;    // int16
constexpr uint32_t item_hit_points    = 38;  // int16
constexpr uint32_t item_pos         = 88;    // PHD_3DPOS, this tick
constexpr uint32_t item_pos_prev    = 108;   // PHD_3DPOS, the previous tick
// The animated skeleton, as GetJointAbsPosition reads it: one 3x4 matrix of
// int32 per joint, 48 bytes apart, rotation in 1/16384 fixed point and the
// translation column scaled the same way. Two copies, one per simulation tick,
// which is what makes a smooth frame possible between them.
constexpr uint32_t item_joints_prev = 0x1F0;
constexpr uint32_t item_joints_cur  = 0x820;
constexpr uint32_t joint_stride     = 48;
// camera_info, 128 bytes
constexpr uint32_t camera_type      = 32;    // int32
// object_info, 2304 bytes: the geometry a draw is about to use. DrawLaraHD
// copies a GEOM_INFO into here before each DrawCreatureHD call, which is what
// makes the face and the sunglasses identifiable at the hook.
constexpr uint32_t object_stride    = 2304;
constexpr uint32_t object_geom      = 88;    // GEOM_INFO
// GEOM_INFO, 104 bytes
constexpr uint32_t geom_stride      = 104;
constexpr uint32_t geom_mesh        = 16;    // void*
} // namespace off

// gLaraHead is GEOM_INFO[2] -- the animated face and the sunglasses. gActorHead
// is GEOM_INFO[3], the cutscene actor's. Both are counted out of the symbol
// sizes in the PDBs (208 and 312 bytes).
constexpr int kLaraHeadGeoms  = 2;
constexpr int kActorHeadGeoms = 3;

constexpr int kFixedShift = 14;   // 16384 == 1.0

// camera_info::type. The classic set, unchanged in the remaster: the engine
// uses 1 for a level-placed fixed camera and 4/5 for cinematic and "heavy"
// (trigger-driven) cameras. Those framings are chosen by the level designer and
// are left alone, exactly as the other TR1-3 first-person attempt does.
constexpr int32_t kCamFixed     = 1;
constexpr int32_t kCamCinematic = 4;

typedef void (__cdecl* Fn_GenerateW2V)(PHD_3DPOS*);
typedef void (__cdecl* Fn_DrawCreatureHD)(void*, int32_t);
typedef void (__cdecl* Fn_DrawHair)(int32_t);
typedef void (__cdecl* Fn_LaraAboveWater)(uint8_t*, void*);
typedef void (__cdecl* Fn_AnimateLara)(uint8_t*);

hook::InlineHook g_hGenerateW2V;
hook::InlineHook g_hDrawCreatureHD;
hook::InlineHook g_hDrawHair;
hook::InlineHook g_hLaraAboveWater;
hook::InlineHook g_hAnimateLara;

// Lara's head is mesh 14 of 15 in all three games -- the same index the camera
// anchors to, because the HD skeleton's first meshes line up with the classic
// ones. `mesh_bits` is indexed the same way: DrawLaraHD itself writes 0x600 for
// the right hand and 0x3000 for the left.
constexpr uint32_t kHeadMeshBit = 1u << 14;

// 48 89 5C 24 08   mov [rsp+8], rbx   -> 5 bytes, PIC, instruction-aligned.
// The same window as PrintRoomsList and DrawSkyHD, and identical in all three
// DLLs and both builds (tools\verify_addresses.py checks it).
const uint8_t kGenerateW2VPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

// 48 89 5C 24 10   mov [rsp+0x10], rbx   -> 5 bytes, PIC, instruction-aligned.
// Identical in all three DLLs and both builds. (DrawLara's own prologue is NOT
// -- it differs in every DLL -- which is one reason the head is hidden through
// mesh_bits and this function rather than by hooking DrawLara.)
const uint8_t kDrawCreatureHDPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };

// DrawHair is the one hook in this mod whose window is not position
// independent:
//   48 83 EC 28              sub rsp, 0x28
//   48 8B 05 <disp32>        mov rax, [rip + disp32]     <- RIP-relative
// Eleven bytes, because five would end inside that second instruction. The
// displacement at offset 7 is fixed up when the bytes are copied to the
// trampoline, which is what kDrawHairRipFixups declares. Only the first seven
// bytes are compared, since the displacement itself differs per DLL.
const uint8_t kDrawHairPrologue[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05 };
const int kDrawHairRipFixups[] = { 7 };

const GameDllLayout* g_boundDll   = nullptr;
uint64_t             g_boundBase  = 0;
uint64_t             g_failedBase = 0;

bool     g_active      = false;   // anchored on the last scene camera
bool     g_runtimeEnabled = false; // whole first-person package, toggled in play
bool     g_runtimeInitialized = false;
bool     g_headHidden  = false;   // mesh_bits bit 14 is currently cleared
bool     g_rollHidden  = false;   // all Lara geometry suppressed during a roll
bool     g_meshOverride = false;
uint8_t* g_meshItem = nullptr;
uint32_t g_meshBaseBits = 0;
unsigned g_headDraws   = 0;       // Lara draws routed through the mesh_bits path
unsigned g_headSkips   = 0;       // face / sunglasses draws dropped
unsigned g_hairSkips   = 0;       // braid draws dropped
bool     g_loggedFirst = false;
bool     g_neutralTaken = false;   // the neutral is taken once first person is live
bool     g_loggedLost  = false;
unsigned g_anchored    = 0;
unsigned g_skipped     = 0;

locomotion::Heading g_heading;
bool g_haveHeading = false;
uint8_t* g_headingItem = nullptr;
locomotion::Vec g_previousBody;
float g_lastHeadWorld = 0;
locomotion::Vec g_manualLocal;
locomotion::Vec g_manualWorld;
bool g_haveManualInput = false;
bool g_shifted = false;
bool g_jumpPressed = false;
int g_directionalRootScale = 1;
locomotion::Vec g_dragPrevious, g_dragCurrent, g_dragShown;
LARGE_INTEGER g_inputTime{}, g_bodyTime{};

float Elapsed(LARGE_INTEGER& last) {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float seconds = last.QuadPart && freq.QuadPart
        ? static_cast<float>(double(now.QuadPart - last.QuadPart) / freq.QuadPart) : 0;
    last = now;
    return std::clamp(seconds, 0.0f, 0.05f); // never catch up a pause with a turn
}

bool CanWalk(const uint8_t* item) {
    if (*reinterpret_cast<const int16_t*>(item + off::item_hit_points) <= 0 ||
        LaraWaterStatus() != 0)
        return false;
    // Ground locomotion only. Do not turn Lara away from a ladder, lever,
    // pickup, airborne state or scripted animation that owns her orientation.
    // These are the shared classic Lara states: walk, run, stop, fast-back,
    // turn right/left, back, fast-turn and step right/left.
    switch (*reinterpret_cast<const int16_t*>(item + off::item_anim_state)) {
    case 0: case 1: case 2: case 5: case 6: case 7:
    case 16: case 20: case 21: case 22: return true;
    default: return false;
    }
}

template <typename T>
T* Ptr(uint32_t rva) { return reinterpret_cast<T*>(g_boundBase + rva); }

// Is this the ONE call that builds the scene view?
//
// phd_GenerateW2V is shared by the inventory, the pickup spin, shadows, photo
// mode and the muzzle flash, all of which must keep their own camera. The scene
// call is identified by where it returns to -- the instruction after the call
// inside S_InitialisePolyList -- which is exact and needs no assumptions about
// what the engine is doing at the time.
bool IsSceneCall(const void* ret) {
    return g_boundDll && g_boundBase &&
           reinterpret_cast<uint64_t>(ret) == g_boundBase + g_boundDll->w2vSceneReturn;
}

bool Gate() {
    if (!g_boundDll || !g_boundBase)               return false;
    if (!Cfg().enabled || !g_runtimeEnabled)       return false;
    if (!VR().active() || !VR().poseValid())       return false;
    // The inventory ring and the title screen draw a scene of their own.
    if (InInventory() || InTitle() || InCutscene()) return false;

    const int32_t type = *Ptr<int32_t>(g_boundDll->camera + off::camera_type);
    if (type == kCamFixed || type >= kCamCinematic) return false;
    return true;
}

// Replace the scene camera POSITION with Lara's head. The orientation is
// deliberately left almost alone -- see below.
//
// The head position is computed from the engine's own animated skeleton rather
// than by calling GetJointAbsPosition, for two reasons. That function walks the
// matrix stack, and calling it from inside the camera hook would mean pushing
// and popping the engine's stack mid-frame for no reason. More importantly it
// answers for the CURRENT simulation tick, and the renderer draws somewhere
// between two ticks -- so a camera built from it steps at the tick rate while
// the world moves smoothly, which reads as the camera juddering against the
// world every time Lara moves.
//
// So both copies of the joint matrix are interpolated by frame_frac, exactly as
// DrawLara interpolates the body it draws. Joint matrices already carry Lara's
// body rotation but not her world position, which is added separately -- also
// interpolated, from pos_prev to pos.
int32_t Lerp(int32_t prev, int32_t cur, int32_t frac) {
    return static_cast<int32_t>(prev + (int64_t(cur) - prev) * frac / 256);
}

bool Anchor(PHD_3DPOS& pose) {
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item) return false;

    const int joint = Cfg().firstPersonJoint;
    if (joint < 0 || joint > 31) return false;

    int32_t frac = *Ptr<int32_t>(g_boundDll->frameFrac);
    if (frac < 0)   frac = 0;
    if (frac > 256) frac = 256;

    const auto* jPrev = reinterpret_cast<const int32_t*>(
        item + off::item_joints_prev + joint * off::joint_stride);
    const auto* jCur  = reinterpret_cast<const int32_t*>(
        item + off::item_joints_cur  + joint * off::joint_stride);
    const auto& posCur  = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const auto& posPrev = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos_prev);
    const int32_t body[3] = { Lerp(posPrev.x_pos, posCur.x_pos, frac),
                              Lerp(posPrev.y_pos, posCur.y_pos, frac),
                              Lerp(posPrev.z_pos, posCur.z_pos, frac) };

    // A point inside the skull rather than the neck pivot the joint sits on.
    const int32_t local[3] = { Cfg().firstPersonAnchorX,
                               Cfg().firstPersonAnchorY,
                               Cfg().firstPersonAnchorZ };

    int32_t head[3];
    for (int row = 0; row < 3; ++row) {
        int64_t v = Lerp(jPrev[row * 4 + 3], jCur[row * 4 + 3], frac);
        for (int col = 0; col < 3; ++col)
            v += int64_t(Lerp(jPrev[row * 4 + col], jCur[row * 4 + col], frac)) * local[col];
        head[row] = body[row] + static_cast<int32_t>(v >> kFixedShift);
    }

    // Sanity, because a bad joint index or a half-built skeleton would put the
    // camera somewhere absurd and the player inside the world. Lara's own
    // origin is about a metre from her head in any pose, so anything further
    // than four sectors away is not her head: report failure and let the frame
    // render from the game camera instead.
    const int64_t dx = int64_t(head[0]) - body[0];
    const int64_t dy = int64_t(head[1]) - body[1];
    const int64_t dz = int64_t(head[2]) - body[2];
    if (dx * dx + dy * dy + dz * dz > int64_t(4096) * 4096) {
        if (!g_loggedLost) {
            g_loggedLost = true;
            LogF("firstperson: joint %d resolved to (%d,%d,%d), which is %d,%d,%d "
                 "from Lara at (%d,%d,%d) -- not anchoring. Wrong joint index?",
                 joint, head[0], head[1], head[2],
                 int(dx), int(dy), int(dz), body[0], body[1], body[2]);
        }
        return false;
    }

    pose.x_pos = head[0];
    pose.y_pos = head[1];
    pose.z_pos = head[2];

    // The scene hook supplies the stable tracking-to-world yaw below. The
    // headset supplies pitch/roll exactly once through the stereo layer.
    pose.x_rot = 0;
    pose.z_rot = 0;

    if (!g_loggedFirst) {
        g_loggedFirst = true;
        LogF("firstperson: anchored to joint %d at (%d,%d,%d) -- Lara at (%d,%d,%d), "
             "frac %d/256, yaw %d from %s (room %d)",
             joint, head[0], head[1], head[2], body[0], body[1], body[2], frac,
             pose.y_rot, Cfg().firstPersonYawFromLara ? "Lara's body" : "the game camera",
             *reinterpret_cast<const int16_t*>(item + off::item_room_number));
    }
    return true;
}

// Hide the head by clearing its mesh_bits bit, which is how the ENGINE hides a
// body part rather than anything invented here.
//
// The classic renderer's own draw loop in DrawLara tests `mesh_bits & bit` per
// mesh and simply skips the head. The HD renderer reaches the same result one
// step further in: DrawCreatureHD zeroes the joint matrix of every mesh whose
// bit is clear, collapsing its triangles to a point -- but only when its second
// argument is non-zero, and DrawLaraHD passes zero for the body. So the detour
// below passes one instead, and the same cleared bit then hides the head in
// both renderers.
//
// Visibility overrides compose: first person clears only the head bit, while a
// roll clears the full mask. The original mask is restored when both overrides
// end so switching views during a roll cannot leave Lara partly hidden.
void ApplyMeshVisibility() {
    if (!g_boundDll || !g_boundBase) return;
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item) {
        g_meshOverride = false;
        g_meshItem = nullptr;
        return;
    }
    if (g_meshOverride && item != g_meshItem) {
        // The old item belongs to a level that was unloaded; never dereference
        // it. Capture the new Lara independently if an override is still active.
        g_meshOverride = false;
        g_meshItem = nullptr;
    }

    auto& bits = *reinterpret_cast<uint32_t*>(item + off::item_mesh_bits);
    if (!g_headHidden && !g_rollHidden) {
        if (g_meshOverride && item == g_meshItem) bits = g_meshBaseBits;
        g_meshOverride = false;
        g_meshItem = nullptr;
        return;
    }
    if (!g_meshOverride) {
        g_meshOverride = true;
        g_meshItem = item;
        g_meshBaseBits = bits;
    }
    uint32_t visible = g_meshBaseBits;
    if (g_headHidden) visible &= ~kHeadMeshBit;
    if (g_rollHidden) visible = 0;
    bits = visible;
}

void SetHeadHidden(bool hide) {
    if (hide == g_headHidden) return;
    g_headHidden = hide;
    ApplyMeshVisibility();
}

bool IsRollState(const uint8_t* item) {
    if (!item) return false;
    // Shared classic Lara state IDs: roll end, standing-roll start, underwater
    // roll and airborne roll. The first two cover the ordinary B-button roll.
    switch (*reinterpret_cast<const int16_t*>(item + off::item_anim_state)) {
    case 23: case 45: case 66: case 68: return true;
    default: return false;
    }
}

void SetRollHidden(bool hide) {
    if (hide == g_rollHidden) return;
    g_rollHidden = hide;
    ApplyMeshVisibility();
}

// Is this draw about to use one of the head geometries?
//
// mesh_bits hides meshes of Lara's BODY, and her face, sunglasses and cutscene
// head are not part of it -- each is a separate GEOM_INFO that DrawLaraHD copies
// into objects[Lara].geom just before calling DrawCreatureHD. Comparing the mesh
// pointer identifies them without depending on the order of the calls or on
// which of them the outfit happens to make.
bool DrawingHeadGeometry(const uint8_t* item) {
    const int16_t obj = *reinterpret_cast<const int16_t*>(item + off::item_object_number);
    if (obj < 0) return false;

    const uint8_t* entry = Ptr<uint8_t>(g_boundDll->objects)
                         + static_cast<uint32_t>(obj) * off::object_stride;
    const void* mesh = *reinterpret_cast<void* const*>(entry + off::object_geom + off::geom_mesh);
    if (!mesh) return false;

    auto matches = [mesh](uint32_t rva, int count) {
        if (!rva) return false;
        const uint8_t* g = Ptr<uint8_t>(rva);
        for (int i = 0; i < count; ++i)
            if (mesh == *reinterpret_cast<void* const*>(g + i * off::geom_stride + off::geom_mesh))
                return true;
        return false;
    };
    return matches(g_boundDll->gLaraHead, kLaraHeadGeoms)
        || matches(g_boundDll->gActorHead, kActorHeadGeoms);
}

void __cdecl Detour_DrawCreatureHD(void* item, int32_t useMeshBits) {
    if (g_active && g_rollHidden && g_boundDll && g_boundBase &&
        item == *Ptr<void*>(g_boundDll->laraItem)) return;
    if (g_headHidden && g_boundDll && g_boundBase &&
        item == *Ptr<void*>(g_boundDll->laraItem)) {
        if (DrawingHeadGeometry(static_cast<const uint8_t*>(item))) {
            ++g_headSkips;
            return;              // the face, the sunglasses: not drawn at all
        }
        // The body and the back holster arrive with zero. Passing one puts them
        // through the engine's own mesh_bits filter, which drops the head mesh.
        if (useMeshBits == 0) {
            ++g_headDraws;
            useMeshBits = 1;
        }
    }
    g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item, useMeshBits);
}

// The braid. Drawn outside Lara's skeleton entirely, by its own function, so
// neither mesh_bits nor the geometry test above can reach it -- and from inside
// her head it sweeps through the view.
void __cdecl Detour_DrawHair(int32_t arg) {
    if (g_active && (g_headHidden || g_rollHidden)) {
        ++g_hairSkips;
        return;
    }
    g_hDrawHair.Original<Fn_DrawHair>()(arg);
}

void TurnBodyToHead(uint8_t* item, float dt) {
    if (!Cfg().firstPersonBodyFollowsHead || !CanWalk(item))
        return;
    using namespace locomotion;
    auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
    const float delta = Wrap(g_heading.World(VR().HeadYawRadians()) - Radians(pos.y_rot));
    const float dead = std::clamp(Cfg().firstPersonBodyDeadzoneDegrees, 0.0f, 90.0f) * Pi / 180;
    const float move = std::copysign(std::max(0.0f, std::fabs(delta) - dead), delta);
    const float limit = std::max(0.0f, Cfg().firstPersonBodyTurnDegreesPerFrame) * 60 * dt * Pi / 180;
    pos.y_rot = Angle(Radians(pos.y_rot) + std::clamp(move, -limit, limit));
}

// PDB coll_info, shared by all three games. This is a private query object;
// never reuse laracoll, whose old position belongs to the animation tick.
struct RoomCollision {
    int32_t floorSamples[18];
    int32_t radius, badPos, badNeg, badCeiling;
    int32_t shift[3], old[3];
    int16_t oldState, oldAnim, oldFrame, facing, quadrant, type;
    int16_t* trigger;
    uint8_t tiltX, tiltZ, hitBaddie, hitStatic;
    uint16_t flags;
};
static_assert(sizeof(RoomCollision) == 144);
static_assert(offsetof(RoomCollision, facing) == 118);
static_assert(offsetof(RoomCollision, trigger) == 128);
using Fn_GetCollisionInfo = void(__cdecl*)(RoomCollision*, int32_t, int32_t,
                                          int32_t, int16_t, int32_t);
using Fn_UpdateLaraRoom = void(__cdecl*)(uint8_t*, int32_t);

locomotion::Vec DragBody(uint8_t* item) {
    using namespace locomotion;
    if (!CanWalk(item) || g_shifted || !Cfg().firstPersonRoomscaleMove ||
        !Cfg().positionalTracking || !Cfg().firstPersonHeadTranslation) return {};
    Vec pending;
    VR().HeadFloorOffset(pending.x, pending.z);
    const float scale = LiveWorldUnitsPerMetre();
    if (scale <= 1) return {};
    // Exclude motion already simulated but not yet displayed by interpolation.
    pending = pending - Rotate(g_dragCurrent - g_dragShown, -g_heading.base);
    const Vec requested = Rotate(DragRequest(pending,
        Cfg().firstPersonRoomscaleDeadzoneMetres), g_heading.base) * scale;
    // Small sweeps cannot jump across a wall or skip a room boundary. A large
    // tracking discontinuity is left pending for recenter, never teleported.
    const float distance = Length(requested);
    if (distance < 1 || distance > scale * 2) return {};
    auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
    const Vec initial{float(pos.x_pos), float(pos.z_pos)};
    const int count = std::clamp(static_cast<int>(std::ceil(distance / 32)), 1, 128);
    const Vec step = requested * (1.0f / count);
    Vec remainder{};
    for (int i = 0; i < count; ++i) {
        remainder = remainder + step;
        const int dx = static_cast<int>(std::round(remainder.x));
        const int dz = static_cast<int>(std::round(remainder.z));
        remainder = remainder - Vec{float(dx), float(dz)};
        RoomCollision coll{};
        coll.radius = 100;
        coll.badPos = 384; coll.badNeg = -384; coll.badCeiling = 0;
        coll.flags = 5; // slopes are walls, lava is a pit (as lara_col_stop)
        coll.old[0] = pos.x_pos; coll.old[1] = pos.y_pos; coll.old[2] = pos.z_pos;
        coll.facing = Angle(std::atan2(step.x, step.z));
        const int x = pos.x_pos + dx, z = pos.z_pos + dz;
        const int16_t room = *reinterpret_cast<int16_t*>(item + off::item_room_number);
        reinterpret_cast<Fn_GetCollisionInfo>(g_boundBase + g_boundDll->getCollisionInfo)(
            &coll, x, pos.y_pos, z, room, 762);
        // Preserve ledge safety and let the normal Lara collision routine
        // settle floor height and select falling/sliding states afterwards.
        if (coll.floorSamples[0] < -384 || coll.floorSamples[0] > 384 ||
            coll.floorSamples[1] >= 0 || coll.type == 8 || coll.type == 16 || coll.type == 32)
            break;
        const int acceptedX = x + coll.shift[0] - pos.x_pos;
        const int acceptedZ = z + coll.shift[2] - pos.z_pos;
        if (Length(Vec{float(acceptedX), float(acceptedZ)}) > 64) break;
        pos.x_pos += acceptedX; pos.z_pos += acceptedZ;
        reinterpret_cast<Fn_UpdateLaraRoom>(g_boundBase + g_boundDll->updateLaraRoom)(item, -381);
        if (!acceptedX && !acceptedZ) break;
    }
    const Vec actual = Vec{float(pos.x_pos), float(pos.z_pos)} - initial;
    g_dragCurrent = g_dragCurrent + actual * (1 / scale);
    return actual * (1 / scale);
}

// Classic sidestep/backpedal root motion advances at walking speed. Keep the
// skeleton at one animation tick (multiple ticks made the first-person body
// and animated head visibly stutter), then scale only that tick's horizontal
// displacement before the native collision routine sees it. At about 45 units
// the resulting sweep remains shorter than Lara's 100-unit collision radius.
void __cdecl Detour_AnimateLara(uint8_t* item) {
    auto original = g_hAnimateLara.Original<Fn_AnimateLara>();
    const int scale = item && item == g_headingItem
        ? std::clamp(g_directionalRootScale, 1, 3) : 1;
    if (scale == 1) {
        original(item);
        return;
    }
    auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
    const int32_t oldX = pos.x_pos, oldZ = pos.z_pos;
    original(item); // exactly one skeletal/animation-frame update
    pos.x_pos = oldX + (pos.x_pos - oldX) * scale;
    pos.z_pos = oldZ + (pos.z_pos - oldZ) * scale;
    auto& speed = *reinterpret_cast<int16_t*>(item + off::item_speed);
    speed = static_cast<int16_t>(std::clamp<int>(speed * scale, -32768, 32767));
}

void __cdecl Detour_LaraAboveWater(uint8_t* item, void* nativeCollision) {
    using namespace locomotion;
    g_dragPrevious = g_dragCurrent;
    if (item && item == g_headingItem && g_active && g_haveHeading && Gate()) {
        const int state = *reinterpret_cast<int16_t*>(item + off::item_anim_state);
        const bool ground = CanWalk(item);
        const bool jump = IsJumpSteeringState(state) && LaraWaterStatus() == 0 &&
            *reinterpret_cast<int16_t*>(item + off::item_hit_points) > 0;
        const float head = g_heading.World(VR().HeadYawRadians());
        auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
        auto* analog = Ptr<int16_t>(g_boundDll->analogInput);
        auto& input = *Ptr<uint32_t>(g_boundDll->input);
        if ((ground || jump) && g_haveManualInput) {
            // This runs AFTER inputGet's axial deadzones and LaraControl's
            // direction-bit conversion. Keep a single heading through stop,
            // compression and forward-jump; neither old camera yaw nor the
            // per-axis deadzone may rotate the requested direction.
            analog[2] = analog[3] = Angle(head); // camTurn, oldCamTurn
            if (Length(g_manualWorld) > 0 && !g_shifted) {
                const Vec directionalWorld = MovementWorld(g_manualLocal, head);
                const float magnitude = std::sqrt(float(analog[0]) * analog[0] +
                                                   float(analog[1]) * analog[1]);
                if (magnitude > 0) {
                    // Native ground gaits are cardinal relative to Lara/HMD.
                    // Cardinalising the analog vector as well as its action
                    // bit prevents ModernControlsRotation from turning a
                    // forward-dominant diagonal away from the headset.
                    const Vec steeringWorld = (ground || state == 15)
                        ? directionalWorld : g_manualWorld;
                    const Vec decoded = SimulationStick(steeringWorld, head, magnitude);
                    analog[0] = static_cast<int16_t>(std::round(decoded.x));
                    analog[1] = static_cast<int16_t>(std::round(decoded.z));
                }
                // Keep Lara facing the HMD and choose a native animation for
                // the direction relative to it. Ground-left/right are real
                // sidesteps; during compression they become native side-jump
                // directions. The airborne forward-jump path keeps the launch
                // heading established here.
                if (ground || state == 15) {
                    const bool preparingJump = (ground && g_jumpPressed) || state == 15;
                    const uint32_t action = MovementAction(g_manualLocal, preparingJump);
                    pos.y_rot = Angle(head);
                    *Ptr<int16_t>(g_boundDll->lara + 252) = 0; // turn_rate
                    // AnimateLara consumes move_angle before the native
                    // collision routine gets a chance to set it. Publishing
                    // the selected direction here prevents side/back gaits
                    // from taking one slow forward step every frame.
                    *Ptr<int16_t>(g_boundDll->lara + 254) =
                        Angle(MovementYaw(g_manualLocal, head));
                    input = (input & ~Directions) | action;
                    if (ground)
                        g_directionalRootScale =
                            DirectionalRootScale(action, preparingJump);
                }
            }
        }
        const Vec dragged = DragBody(item);
        if (Cfg().firstPersonDriftLog) {
            static uint64_t last = 0;
            static int lastState = -1;
            const uint64_t now = GetTickCount64();
            if (now - last >= 500 || (jump && state != lastState)) {
                last = now;
                Vec pending;
                VR().HeadFloorOffset(pending.x, pending.z);
                LogF("locomotion: state=%d head=%.1f body=%.1f cam=%.1f "
                     "manual=(%+.2f,%+.2f) pending=(%+.3f,%+.3f)m "
                     "drag=(%+.3f,%+.3f)m input=%08X",
                     state, head * 180 / Pi, Radians(pos.y_rot) * 180 / Pi,
                     Radians(analog[2]) * 180 / Pi, g_manualWorld.x, g_manualWorld.z,
                     pending.x, pending.z, dragged.x, dragged.z, input);
            }
            lastState = state;
        }
    }
    g_hLaraAboveWater.Original<Fn_LaraAboveWater>()(item, nativeCollision);
    g_directionalRootScale = 1;
}

void UpdateLocomotion(PHD_3DPOS& pose) {
    using namespace locomotion;
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const auto& prev = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos_prev);
    const int frac = std::clamp(*Ptr<int32_t>(g_boundDll->frameFrac), 0, 256);
    const Vec body{static_cast<float>(Lerp(prev.x_pos, pos.x_pos, frac)),
                   static_cast<float>(Lerp(prev.z_pos, pos.z_pos, frac))};
    const float scale = LiveWorldUnitsPerMetre();
    const bool relocated = Length(body - g_previousBody) > std::max(1024.0f, scale * 2);
    if (g_haveHeading && g_headingItem == item && !relocated) {
        // Consume only roomscale displacement actually visible this frame.
        // Native stick movement is absent from these counters. Applying the
        // same interpolation as the body keeps both body and view smooth.
        const Vec shown = g_dragPrevious + (g_dragCurrent - g_dragPrevious) * (frac / 256.0f);
        const Vec used = Rotate(shown - g_dragShown, -g_heading.base);
        VR().ConsumeHeadFloorOffset(used.x, used.z);
        g_dragShown = shown;
    }
    if (!g_haveHeading || g_headingItem != item || relocated) {
        const float facing = g_headingItem == item && !relocated
            ? g_lastHeadWorld : Radians(pos.y_rot);
        g_heading.Align(facing, VR().HeadYawRadians());
        g_haveHeading = true;
        g_headingItem = item;
        g_haveManualInput = false;
        g_dragPrevious = g_dragCurrent = g_dragShown = {};
        g_inputTime = g_bodyTime = {};
        VR().RecenterHead();
        LogF("locomotion: aligned base=%.1f body=%.1f controls=%s",
             g_heading.base * 180 / Pi, Radians(pos.y_rot) * 180 / Pi,
             NewControls() ? "modern" : "tank");
    }
    g_previousBody = body;
    if (!CanWalk(item)) {
        // Physical movement during an interaction must not queue a walk when
        // the animation releases control. Height remains tracked.
        Vec pending;
        VR().HeadFloorOffset(pending.x, pending.z);
        VR().ConsumeHeadFloorOffset(pending.x, pending.z);
    }
    TurnBodyToHead(item, Elapsed(g_bodyTime));
    pose.y_rot = Angle(g_heading.base);
    g_lastHeadWorld = g_heading.World(VR().HeadYawRadians());
}

void __cdecl Detour_GenerateW2V(PHD_3DPOS* pose) {
    if (pose && IsSceneCall(_ReturnAddress())) {
        auto* item = g_boundDll && g_boundBase
            ? *Ptr<uint8_t*>(g_boundDll->laraItem) : nullptr;
        if (Gate() && Anchor(*pose)) {
            g_active = true;
            ++g_anchored;
        } else {
            g_active = false;
            ++g_skipped;
        }
        // Roll visibility belongs only to the active first-person gameplay
        // scene. The title, inventory and other UI scenes reuse this camera
        // path and Lara's last animation state, so evaluating the state before
        // Gate() could leak a zero mesh mask into their visual passes.
        SetRollHidden(g_active && IsRollState(item));
        // WHEN THE NEUTRAL IS TAKEN.
        //
        // Not at the first pose the runtime produces -- that is usually while
        // the headset is on a desk or before the player has straightened up,
        // and everything positional is measured from it, so standing up
        // afterwards floats the camera above Lara's head. Taken when first
        // person actually engages, the player is in position and looking at the
        // game. The recenter key re-takes it whenever they like.
        if (g_active && !g_neutralTaken) {
            g_neutralTaken = true;
            VR().RecenterHead();
            Log("firstperson: taking the neutral head position now that first "
                "person is live -- press the recenter key to set it again");
        }

        // Once per frame, before anything is drawn.
        if (g_active) UpdateLocomotion(*pose);
        else {
            g_haveHeading = false;
            g_haveManualInput = false;
            g_dragPrevious = g_dragCurrent = g_dragShown = {};
            g_neutralTaken = false;
        }
        SetHeadHidden(g_active && Cfg().firstPersonHideHead);
        ApplyMeshVisibility();
    }
    g_hGenerateW2V.Original<Fn_GenerateW2V>()(pose);
}

bool Install(const GameDllLayout& d, uint64_t base) {
    if (d.phdGenerateW2V == 0 || d.w2vSceneReturn == 0 ||
        d.frameFrac == 0 || d.laraItem == 0 || d.analogInput == 0 ||
        !d.input || !d.laraAboveWater || !d.animateLara ||
        !d.getCollisionInfo || !d.updateLaraRoom)
        return false;
    const uint8_t movementPrologue[] = {0x48, 0x89, 0x5C, 0x24,
        static_cast<uint8_t>(d.module[4] == L'1' ? 0x08 : d.module[4] == L'2' ? 0x10 : 0x18)};
    if (!g_hLaraAboveWater.Install(reinterpret_cast<void*>(base + d.laraAboveWater),
            reinterpret_cast<void*>(&Detour_LaraAboveWater), 5,
            movementPrologue, sizeof(movementPrologue), "LaraAboveWater")) return false;
    const uint8_t animate1[] = {0x48, 0x89, 0x7C, 0x24, 0x20};
    const uint8_t animate2[] = {0x57, 0x41, 0x54, 0x41, 0x57};
    const uint8_t animate3[] = {0x57, 0x41, 0x54, 0x41, 0x55};
    const uint8_t animate2Retail[] = {0x40, 0x57, 0x41, 0x54, 0x41, 0x57};
    const uint8_t animate3Retail[] = {0x40, 0x57, 0x41, 0x54, 0x41, 0x55};
    const bool stock = d.timestamp == 0x6A4B48FF || d.timestamp == 0x6A4B4915 ||
                       d.timestamp == 0x6A4B490D;
    const uint8_t* animatePrologue = d.module[4] == L'1' ? animate1 :
        d.module[4] == L'2' ? (stock ? animate2 : animate2Retail) :
                              (stock ? animate3 : animate3Retail);
    const size_t animateBytes = d.module[4] == L'1' || stock ? 5 : 6;
    if (!g_hAnimateLara.Install(reinterpret_cast<void*>(base + d.animateLara),
            reinterpret_cast<void*>(&Detour_AnimateLara), animateBytes,
            animatePrologue, animateBytes, "AnimateLara")) return false;
    if (!g_hGenerateW2V.Install(
            reinterpret_cast<void*>(base + d.phdGenerateW2V),
            reinterpret_cast<void*>(&Detour_GenerateW2V),
            5, kGenerateW2VPrologue, sizeof(kGenerateW2VPrologue),
            "phd_GenerateW2V"))
        return false;

    // Head hiding is optional: if this one will not install, first person still
    // works, so it is reported rather than fatal.
    if (d.drawCreatureHD != 0 &&
        !g_hDrawCreatureHD.Install(
            reinterpret_cast<void*>(base + d.drawCreatureHD),
            reinterpret_cast<void*>(&Detour_DrawCreatureHD),
            5, kDrawCreatureHDPrologue, sizeof(kDrawCreatureHDPrologue),
            "DrawCreatureHD"))
        Log("firstperson: DrawCreatureHD could not be hooked -- the head will "
            "still be hidden in the classic renderer but not in the HD one.");

    if (d.drawHair != 0 &&
        !g_hDrawHair.Install(
            reinterpret_cast<void*>(base + d.drawHair),
            reinterpret_cast<void*>(&Detour_DrawHair),
            11, kDrawHairPrologue, sizeof(kDrawHairPrologue),
            "DrawHair", kDrawHairRipFixups,
            sizeof(kDrawHairRipFixups) / sizeof(kDrawHairRipFixups[0])))
        Log("firstperson: DrawHair could not be hooked -- the braid will still "
            "sweep through the view.");
    return true;
}

void Remove() {
    SetRollHidden(false);
    SetHeadHidden(false);          // give her complete mesh back before letting go
    g_hLaraAboveWater.Remove();
    g_hAnimateLara.Remove();
    g_hDrawHair.Remove();
    g_hDrawCreatureHD.Remove();
    g_hGenerateW2V.Remove();
    g_active     = false;
    g_haveHeading = false;
    g_headingItem = nullptr;
    g_haveManualInput = false;
    g_directionalRootScale = 1;
    g_dragPrevious = g_dragCurrent = g_dragShown = {};
    g_neutralTaken = false;
    g_boundDll   = nullptr;
    g_boundBase  = 0;
}

} // namespace

void FirstPersonUpdate() {
    if (!g_runtimeInitialized) {
        g_runtimeEnabled = false;
        g_runtimeInitialized = true;
        Log("firstperson: startup view is always third person; press Y+LT to switch views");
    }

    // Re-take the neutral head position on request. Edge triggered: held down,
    // it would re-capture every frame while the head drifts.
    if (Cfg().firstPersonRecenterKey) {
        static bool held = false;
        const bool down = (GetAsyncKeyState(Cfg().firstPersonRecenterKey) & 0x8000) != 0;
        if (down && !held) {
            VR().RecenterHead();
            g_dragPrevious = g_dragCurrent = g_dragShown = {};
            Log("firstperson: head position recentered; world heading preserved");
        }
        held = down;
    }

    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();

    // Keep the detours installed while third person is selected so Y+LT can
    // engage first person without patching live code at the moment of input.
    // Gate() makes every detour a pass-through until the runtime mode is on.
    const bool want = Cfg().enabled && d && base
                   && d->phdGenerateW2V != 0;

    if (g_boundDll && (!want || d != g_boundDll || base != g_boundBase)) {
        LogF("firstperson: unhooking %S (anchored %u frames, stood down %u, "
             "%u mesh_bits draws, %u face/glasses skipped, %u braid skipped)",
             g_boundDll->module, g_anchored, g_skipped, g_headDraws,
             g_headSkips, g_hairSkips);
        Remove();
    }
    if (!want || g_boundDll) return;
    if (base == g_failedBase) return;

    g_boundDll  = d;
    g_boundBase = base;
    if (!Install(*d, base)) {
        LogF("firstperson: phd_GenerateW2V could not be hooked in %S -- the "
             "camera stays third person. A prologue mismatch here means the "
             "game was patched; re-run tools\\verify_addresses.py.", d->module);
        Remove();
        g_failedBase = base;
        return;
    }
    LogF("firstperson: hooked %S (%s) -- runtime camera switch ready (Y+LT, "
         "startup third person, joint %d). Fixed and cinematic cameras keep "
         "their own framing.", d->module, d->name, Cfg().firstPersonJoint);
}

void FirstPersonToggle() {
    if (!g_runtimeInitialized) {
        g_runtimeEnabled = false;
        g_runtimeInitialized = true;
    }

    g_runtimeEnabled = !g_runtimeEnabled;
    // A view change starts with a clean heading and roomscale neutral. This is
    // also the immediate stand-down path: no simulation tick between the chord
    // and the next camera draw can inherit first-person steering.
    SetRollHidden(false);
    SetHeadHidden(false);
    g_active = false;
    g_haveHeading = false;
    g_headingItem = nullptr;
    g_haveManualInput = false;
    g_shifted = false;
    g_jumpPressed = false;
    g_directionalRootScale = 1;
    g_dragPrevious = g_dragCurrent = g_dragShown = {};
    g_inputTime = g_bodyTime = {};
    g_neutralTaken = false;
    LogF("firstperson: Y+LT switched to %s",
         g_runtimeEnabled ? "FIRST PERSON" : "third person");
}

void FirstPersonShutdown() {
    if (g_boundDll) Remove();
    g_loggedFirst = false;
    g_loggedLost  = false;
    g_neutralTaken = false;
    g_failedBase  = 0;
    g_anchored    = 0;
    g_skipped     = 0;
    g_headDraws   = 0;
    g_headSkips   = 0;
    g_hairSkips   = 0;
    g_runtimeEnabled = false;
    g_runtimeInitialized = false;
}

bool FirstPersonActive() { return g_active; }

void FirstPersonInput(float& leftX, float& leftY, float& rightX, bool shifted,
                      bool jumpPressed) {
    using namespace locomotion;
    g_haveManualInput = false;
    g_shifted = shifted;
    g_jumpPressed = jumpPressed;
    if (!g_active || !g_haveHeading || !Gate()) {
        g_inputTime = {};
        return;
    }
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item || item != g_headingItem) return;
    const float change = StickTurn(rightX, Cfg().firstPersonTurnDeadzone,
                                  Cfg().firstPersonTurnDegreesPerSecond, Elapsed(g_inputTime));
    g_heading.Turn(change);
    VR().PivotHeadFloorOffset(change);
    const float headWorld = g_heading.World(VR().HeadYawRadians());
    g_lastHeadWorld = headWorld;
    const int state = *reinterpret_cast<const int16_t*>(item + off::item_anim_state);
    const bool ground = CanWalk(item);
    const bool jump = IsJumpSteeringState(state) && LaraWaterStatus() == 0
        && *reinterpret_cast<const int16_t*>(item + off::item_hit_points) > 0;
    if (!ground && !jump) return;
    rightX = 0;
    Vec manual{leftX, leftY};
    if (Length(manual) < 0.20f || shifted) manual = {};
    g_manualLocal = manual;
    const float inputYaw = Radians(*Ptr<int16_t>(g_boundDll->analogInput + 4));
    g_manualWorld = Rotate(manual, Cfg().firstPersonMoveWithHead ? headWorld : inputYaw);
    g_haveManualInput = true;
    if (NewControls()) {
        const Vec result = Limit(Rotate(g_manualWorld, -inputYaw));
        leftX = result.x;
        leftY = result.z;
    } else {
        leftX = manual.x;
        leftY = manual.z;
    }
    // Physical displacement never enters XInput. The simulation hook moves
    // Lara through native collision queries independently of this stick.
    return;
}

} // namespace tr
