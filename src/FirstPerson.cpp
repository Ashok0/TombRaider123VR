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

hook::InlineHook g_hGenerateW2V;
hook::InlineHook g_hDrawCreatureHD;
hook::InlineHook g_hDrawHair;

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
bool     g_headHidden  = false;   // mesh_bits bit 14 is currently cleared
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
locomotion::Vec g_roomRequest;
float g_roomAllocation = 0;
float g_lastHeadWorld = 0;
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
    if (!Cfg().enabled || !Cfg().firstPerson)      return false;
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
// The bit is cleared for as long as first person is anchoring and put back the
// moment it stops, so an unhook, a cutscene or FirstPerson=0 all restore her
// head. It is ORed back rather than restored from a saved copy, because the
// game owns that field and may write it between frames.
void SetHeadHidden(bool hide) {
    if (hide == g_headHidden) return;
    if (!g_boundDll || !g_boundBase) return;
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item) return;

    auto& bits = *reinterpret_cast<uint32_t*>(item + off::item_mesh_bits);
    if (hide) bits &= ~kHeadMeshBit;
    else      bits |=  kHeadMeshBit;
    g_headHidden = hide;
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
    if (g_headHidden) {
        ++g_hairSkips;
        return;
    }
    g_hDrawHair.Original<Fn_DrawHair>()(arg);
}

void TurnBodyToHead(uint8_t* item, float dt) {
    if (!Cfg().firstPersonBodyFollowsHead || !CanWalk(item)) return;
    using namespace locomotion;
    auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
    const float delta = Wrap(g_heading.World(VR().HeadYawRadians()) - Radians(pos.y_rot));
    const float dead = std::clamp(Cfg().firstPersonBodyDeadzoneDegrees, 0.0f, 90.0f) * Pi / 180;
    const float move = std::copysign(std::max(0.0f, std::fabs(delta) - dead), delta);
    const float limit = std::max(0.0f, Cfg().firstPersonBodyTurnDegreesPerFrame) * 60 * dt * Pi / 180;
    pos.y_rot = Angle(Radians(pos.y_rot) + std::clamp(move, -limit, limit));
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
    if (!g_haveHeading || g_headingItem != item || relocated) {
        const float facing = g_headingItem == item && !relocated
            ? g_lastHeadWorld : Radians(pos.y_rot);
        g_heading.Align(facing, VR().HeadYawRadians());
        g_haveHeading = true;
        g_headingItem = item;
        g_roomAllocation = 0;
        g_inputTime = g_bodyTime = {};
        VR().RecenterHead();
        LogF("locomotion: aligned base=%.1f body=%.1f controls=%s",
             g_heading.base * 180 / Pi, Radians(pos.y_rot) * 180 / Pi,
             NewControls() ? "modern" : "tank");
    } else if (scale > 1 && g_roomAllocation > 0 && CanWalk(item)) {
        Vec pending;
        VR().HeadFloorOffset(pending.x, pending.z);
        const Vec travel = (body - g_previousBody) * (1 / scale);
        // Bound consumption by both the outstanding step and the last input
        // request. Moving the head back cancels a step, even mid-animation.
        const Vec worldPending = Rotate(pending, g_heading.base);
        if (Dot(worldPending, g_roomRequest) > 0) {
            const Vec used = Rotate(Consumed(worldPending, travel, g_roomAllocation), -g_heading.base);
            VR().ConsumeHeadFloorOffset(used.x, used.z);
        }
    }
    g_previousBody = body;
    if (!CanWalk(item)) {
        // Physical movement during an interaction must not queue a walk when
        // the animation releases control. Height remains tracked.
        Vec pending;
        VR().HeadFloorOffset(pending.x, pending.z);
        VR().ConsumeHeadFloorOffset(pending.x, pending.z);
        g_roomAllocation = 0;
    }
    TurnBodyToHead(item, Elapsed(g_bodyTime));
    pose.y_rot = Angle(g_heading.base);
    g_lastHeadWorld = g_heading.World(VR().HeadYawRadians());
}

void __cdecl Detour_GenerateW2V(PHD_3DPOS* pose) {
    if (pose && IsSceneCall(_ReturnAddress())) {
        if (Gate() && Anchor(*pose)) {
            g_active = true;
            ++g_anchored;
        } else {
            g_active = false;
            ++g_skipped;
        }
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
            g_roomAllocation = 0;
            g_neutralTaken = false;
        }
        SetHeadHidden(g_active && Cfg().firstPersonHideHead);
    }
    g_hGenerateW2V.Original<Fn_GenerateW2V>()(pose);
}

bool Install(const GameDllLayout& d, uint64_t base) {
    if (d.phdGenerateW2V == 0 || d.w2vSceneReturn == 0 ||
        d.frameFrac == 0 || d.laraItem == 0 || d.analogInput == 0)
        return false;
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
    SetHeadHidden(false);          // give her head back before letting go
    g_hDrawHair.Remove();
    g_hDrawCreatureHD.Remove();
    g_hGenerateW2V.Remove();
    g_active     = false;
    g_haveHeading = false;
    g_headingItem = nullptr;
    g_roomAllocation = 0;
    g_neutralTaken = false;
    g_boundDll   = nullptr;
    g_boundBase  = 0;
}

} // namespace

void FirstPersonUpdate() {
    // Re-take the neutral head position on request. Edge triggered: held down,
    // it would re-capture every frame while the head drifts.
    if (Cfg().firstPersonRecenterKey) {
        static bool held = false;
        const bool down = (GetAsyncKeyState(Cfg().firstPersonRecenterKey) & 0x8000) != 0;
        if (down && !held) {
            VR().RecenterHead();
            g_roomAllocation = 0;
            Log("firstperson: head position recentered; world heading preserved");
        }
        held = down;
    }

    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();

    const bool want = Cfg().enabled && Cfg().firstPerson && d && base
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
    LogF("firstperson: hooked %S (%s) -- scene camera anchors to Lara's head "
         "(joint %d). Stable VR heading owns view/body/input; fixed and "
         "cinematic cameras keep their own framing.",
         d->module, d->name, Cfg().firstPersonJoint);
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
}

bool FirstPersonActive() { return g_active; }

bool FirstPersonInput(float& leftX, float& leftY, float& rightX, bool shifted) {
    using namespace locomotion;
    if (!g_active || !g_haveHeading || !Gate()) {
        g_roomAllocation = 0;
        g_inputTime = {};
        return false;
    }
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item || item != g_headingItem) {
        g_roomAllocation = 0;
        return false;
    }
    const float dt = Elapsed(g_inputTime);
    const float change = StickTurn(rightX, Cfg().firstPersonTurnDeadzone,
                                  Cfg().firstPersonTurnDegreesPerSecond, dt);
    g_heading.Turn(change);
    VR().PivotHeadFloorOffset(change);

    const float headWorld = g_heading.World(VR().HeadYawRadians());
    g_lastHeadWorld = headWorld;
    const bool canWalk = CanWalk(item);
    if (!canWalk) {
        // Keep native input during swimming, climbing and interactions. The
        // same stick still turns the VR world, while the engine receives it to
        // preserve state-specific steering. Ground locomotion consumes it to
        // prevent the chase-camera feedback loop.
        g_roomAllocation = 0;
        return false;
    }
    rightX = 0;

    const float inputYaw = Radians(*Ptr<int16_t>(g_boundDll->analogInput + 4));
    const bool modern = NewControls();
    Vec manual{leftX, leftY};
    if (Length(manual) < 0.20f || shifted) manual = {};
    Vec room;
    if (Cfg().firstPersonRoomscaleMove && Cfg().positionalTracking
        && Cfg().firstPersonHeadTranslation && canWalk && !shifted) {
        Vec pending;
        VR().HeadFloorOffset(pending.x, pending.z);
        room = RoomInput(pending, Cfg().firstPersonRoomscaleDeadzoneMetres,
                         Cfg().firstPersonRoomscaleFullMetres);
        const float n = Length(room);
        // XInput deadzone is applied again by the game. Cross it for a real
        // step, otherwise small displacements remain permanently unconsumed.
        if (n > 0) room = room * ((0.35f + 0.65f * n) / n);
        room = Rotate(room, g_heading.base);
    }

    bool walkModifier = false;
    Vec result = manual;
    if (modern) {
        const Vec manualWorld = Rotate(manual, Cfg().firstPersonMoveWithHead ? headWorld : inputYaw);
        const Vec combined = manualWorld + room;
        result = Limit(Rotate(combined, -inputYaw));
        // Attribution only when both contributions help, never consume steps
        // using an opposing manual command or collision motion.
        g_roomAllocation = Dot(combined, room) > 0
            ? Length(room) / std::max(0.0001f, Length(room) + Length(manualWorld)) : 0;
    } else {
        // Tank axes are actions, not a direction. Room-only sideways movement
        // uses the game's walk+left/right sidestep rather than turning Lara.
        auto& body = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
        if (canWalk && Cfg().firstPersonBodyFollowsHead && Length(manual) > 0)
            body.y_rot = Angle(headWorld);
        const float bodyYaw = Radians(body.y_rot);
        const Vec bodyRoom = Rotate(room, -bodyYaw);
        g_roomAllocation = 0;
        if (Length(manual) == 0 && Length(room) > 0) {
            if (std::fabs(bodyRoom.x) > std::fabs(bodyRoom.z)) {
                result = {bodyRoom.x, 0};
                walkModifier = true;
            } else result = {0, bodyRoom.z};
            g_roomAllocation = 1;
        }
    }
    g_roomRequest = room;
    if (shifted) {
        // A D-pad/menu gesture must neither walk Lara nor consume a step. Keep
        // the outstanding displacement so it resumes when the shift releases.
        g_roomAllocation = 0;
        result = {};
    }
    leftX = result.x;
    leftY = result.z;

    if (Cfg().firstPersonDriftLog) {
        static uint64_t last = 0;
        const uint64_t now = GetTickCount64();
        if (now - last >= 1000) {
            last = now;
            Vec pending;
            VR().HeadFloorOffset(pending.x, pending.z);
            const float bodyYaw = Radians(reinterpret_cast<PHD_3DPOS*>(item + off::item_pos)->y_rot);
            LogF("locomotion: base=%+.1f headWorld=%+.1f body=%+.1f inputYaw=%+.1f "
                 "pending=(%+.3f,%+.3f)m room=(%+.2f,%+.2f) pad=(%+.2f,%+.2f) "
                 "share=%.2f walk=%d rooms=%d",
                 g_heading.base * 180 / Pi, headWorld * 180 / Pi,
                 bodyYaw * 180 / Pi, inputYaw * 180 / Pi,
                 pending.x, pending.z, room.x, room.z, leftX, leftY,
                 g_roomAllocation, canWalk, *Ptr<int32_t>(g_boundDll->numberDrawRooms));
        }
    }
    return walkModifier;
}

} // namespace tr
