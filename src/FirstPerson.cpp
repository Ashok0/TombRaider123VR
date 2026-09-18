#include "FirstPerson.h"
#include "GameDll.h"
#include "Engine.h"
#include "Config.h"
#include "InlineHook.h"
#include "VRSystem.h"
#include "Log.h"

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
constexpr uint32_t item_room_number = 28;    // int16
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
} // namespace off

constexpr int kFixedShift = 14;   // 16384 == 1.0

// camera_info::type. The classic set, unchanged in the remaster: the engine
// uses 1 for a level-placed fixed camera and 4/5 for cinematic and "heavy"
// (trigger-driven) cameras. Those framings are chosen by the level designer and
// are left alone, exactly as the other TR1-3 first-person attempt does.
constexpr int32_t kCamFixed     = 1;
constexpr int32_t kCamCinematic = 4;

typedef void (__cdecl* Fn_GenerateW2V)(PHD_3DPOS*);

hook::InlineHook g_hGenerateW2V;

// 48 89 5C 24 08   mov [rsp+8], rbx   -> 5 bytes, PIC, instruction-aligned.
// The same window as PrintRoomsList and DrawSkyHD, and identical in all three
// DLLs and both builds (tools\verify_addresses.py checks it).
const uint8_t kGenerateW2VPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

const GameDllLayout* g_boundDll   = nullptr;
uint64_t             g_boundBase  = 0;
uint64_t             g_failedBase = 0;

bool     g_active      = false;   // anchored on the last scene camera
bool     g_loggedFirst = false;
bool     g_loggedLost  = false;
unsigned g_anchored    = 0;
unsigned g_skipped     = 0;

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
    if (InInventory() || InTitle())                return false;

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

    // ORIENTATION. The incoming pose is the engine's render camera, already
    // interpolated, and its yaw is what the right stick steers -- so keeping it
    // is what keeps the stick working, and it follows Lara round as the chase
    // camera settles behind her. Taking Lara's body yaw instead makes the stick
    // appear dead under modern controls, because there the stick orbits the
    // camera and only turns Lara when she moves.
    //
    // Pitch and roll are always dropped: the headset owns those, and letting an
    // animation tilt the horizon is the classic way to make someone ill.
    if (Cfg().firstPersonYawFromLara)
        pose.y_rot = reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos)->y_rot;
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

void __cdecl Detour_GenerateW2V(PHD_3DPOS* pose) {
    if (pose && IsSceneCall(_ReturnAddress())) {
        if (Gate() && Anchor(*pose)) {
            g_active = true;
            ++g_anchored;
        } else {
            g_active = false;
            ++g_skipped;
        }
    }
    g_hGenerateW2V.Original<Fn_GenerateW2V>()(pose);
}

bool Install(const GameDllLayout& d, uint64_t base) {
    if (d.phdGenerateW2V == 0 || d.w2vSceneReturn == 0 ||
        d.frameFrac == 0 || d.laraItem == 0)
        return false;
    return g_hGenerateW2V.Install(
        reinterpret_cast<void*>(base + d.phdGenerateW2V),
        reinterpret_cast<void*>(&Detour_GenerateW2V),
        5, kGenerateW2VPrologue, sizeof(kGenerateW2VPrologue),
        "phd_GenerateW2V");
}

void Remove() {
    g_hGenerateW2V.Remove();
    g_active     = false;
    g_boundDll   = nullptr;
    g_boundBase  = 0;
}

} // namespace

void FirstPersonUpdate() {
    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();

    const bool want = Cfg().enabled && Cfg().firstPerson && d && base
                   && d->phdGenerateW2V != 0;

    if (g_boundDll && (!want || d != g_boundDll || base != g_boundBase)) {
        LogF("firstperson: unhooking %S (anchored %u frames, stood down %u)",
             g_boundDll->module, g_anchored, g_skipped);
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
         "(joint %d). Her body yaw turns the view; the headset supplies look "
         "and lean. Fixed and cinematic cameras keep their own framing.",
         d->module, d->name, Cfg().firstPersonJoint);
}

void FirstPersonShutdown() {
    if (g_boundDll) Remove();
    g_loggedFirst = false;
    g_loggedLost  = false;
    g_failedBase  = 0;
    g_anchored    = 0;
    g_skipped     = 0;
}

bool FirstPersonActive() { return g_active; }

} // namespace tr
