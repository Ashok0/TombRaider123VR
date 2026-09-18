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
constexpr uint32_t item_mesh_bits   = 12;    // uint32, one bit per mesh
constexpr uint32_t item_object_number = 16;  // int16
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

void __cdecl Detour_GenerateW2V(PHD_3DPOS* pose) {
    if (pose && IsSceneCall(_ReturnAddress())) {
        if (Gate() && Anchor(*pose)) {
            g_active = true;
            ++g_anchored;
        } else {
            g_active = false;
            ++g_skipped;
        }
        // Once per frame, before anything is drawn.
        SetHeadHidden(g_active && Cfg().firstPersonHideHead);
    }
    g_hGenerateW2V.Original<Fn_GenerateW2V>()(pose);
}

bool Install(const GameDllLayout& d, uint64_t base) {
    if (d.phdGenerateW2V == 0 || d.w2vSceneReturn == 0 ||
        d.frameFrac == 0 || d.laraItem == 0)
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
    g_headDraws   = 0;
    g_headSkips   = 0;
    g_hairSkips   = 0;
}

bool FirstPersonActive() { return g_active; }

} // namespace tr
