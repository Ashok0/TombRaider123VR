#include "FirstPerson.h"
#include "GameDll.h"
#include "Engine.h"
#include "Config.h"
#include "InlineHook.h"
#include "VRSystem.h"
#include "Log.h"
#include "LocomotionMath.h"
#include "FirstPersonStabilization.h"
#include "FirstPersonClearance.h"
#include "FirstPersonVisibility.h"
#include "FirstPersonActionIcon.h"
#include "MotionGunMath.h"
#include "MotionGunInput.h"

#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <cstring>

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
constexpr uint32_t item_goal_state    = 20;  // int16
constexpr uint32_t item_anim_number   = 24;  // int16
constexpr uint32_t item_room_number = 28;    // int16
constexpr uint32_t item_speed       = 34;    // int16
constexpr uint32_t item_hit_points    = 38;  // int16
constexpr uint32_t item_pos         = 88;    // PHD_3DPOS, this tick
constexpr uint32_t item_pos_prev    = 108;   // PHD_3DPOS, the previous tick
constexpr uint32_t item_flags       = 484;   // uint16, gravity_status bit 3
constexpr uint32_t item_next_active = 32;    // int16, native enemy candidate chain
constexpr uint32_t item_stride      = 0xE50;
// The animated skeleton, as GetJointAbsPosition reads it: one 3x4 matrix of
// int32 per joint, 48 bytes apart, rotation in 1/16384 fixed point and the
// translation column scaled the same way. Two copies, one per simulation tick,
// which is what makes a smooth frame possible between them.
constexpr uint32_t item_joints_prev = 0x1F0;
constexpr uint32_t item_joints_cur  = 0x820;
constexpr uint32_t joint_stride     = 48;
// lara_info (432 bytes) and lara_arm (24 bytes), identical in TR1/2/3.
constexpr uint32_t lara_left_arm    = 272;
constexpr uint32_t lara_right_arm   = 296;
constexpr uint32_t lara_gun_status  = 2;
constexpr uint32_t lara_target      = 240;
constexpr uint32_t arm_lock         = 12;
constexpr uint32_t arm_y_rot        = 14;
constexpr uint32_t arm_x_rot        = 16;
constexpr uint32_t arm_z_rot        = 18;
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
constexpr uint32_t kArmMeshBits = firstperson::ArmMeshBits;

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
typedef void (__cdecl* Fn_LaraGun)();
typedef void (__cdecl* Fn_AnimateLara)(uint8_t*);
typedef void (__cdecl* Fn_CalculateLaraMatrices)(uint8_t*);
typedef void (__cdecl* Fn_DrawActionIndicators)();
// TR1-3 writes its skin palette to the global `joints` array. Unlike the
// TR4-5 routine, GetJoints takes only ITEM_INFO* and returns no bone count.
typedef void (__cdecl* Fn_GetJoints)(uint8_t*);
typedef int32_t (__cdecl* Fn_FireWeapon)(int32_t,void*,void*,const int16_t*);
typedef void (__cdecl* Fn_DrawGunFlash)(int32_t,int32_t,int32_t);
struct ShotVector {
    int32_t x,y,z;
    int16_t room,pad;
};
typedef int32_t (__cdecl* Fn_GetTargetOnLOS)(
    ShotVector*,ShotVector*,int32_t,int32_t);

hook::InlineHook g_hGenerateW2V;
hook::InlineHook g_hDrawCreatureHD;
hook::InlineHook g_hDrawHair;
hook::InlineHook g_hLaraAboveWater;
hook::InlineHook g_hLaraGun;
hook::InlineHook g_hAnimateLara;
hook::InlineHook g_hCalculateLaraMatrices;
hook::InlineHook g_hDrawActionIndicators;
hook::InlineHook g_hGetJoints;
hook::InlineHook g_hFireWeapon;
hook::InlineHook g_hDrawGunFlash;
hook::InlineHook g_hGetTargetOnLOS;
hook::InlineHook g_hFireHarpoon;
hook::InlineHook g_hFireRocket;
hook::InlineHook g_hFireGrenade;
hook::InlineHook g_hAnimateShotgun;

// Lara's head is mesh 14 of 15 in all three games -- the same index the camera
// anchors to, because the HD skeleton's first meshes line up with the classic
// ones. `mesh_bits` is indexed the same way: DrawLaraHD itself writes 0x600 for
// the right hand and 0x3000 for the left.
constexpr uint32_t kHeadMeshBit = firstperson::HeadMeshBit;

// 48 89 5C 24 08   mov [rsp+8], rbx   -> 5 bytes, PIC, instruction-aligned.
// The same window as PrintRoomsList and DrawSkyHD, and identical in all three
// DLLs and both builds (tools\verify_addresses.py checks it).
const uint8_t kGenerateW2VPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

// 48 89 5C 24 10   mov [rsp+0x10], rbx   -> 5 bytes, PIC, instruction-aligned.
// Identical in all three DLLs and both builds. (DrawLara's own prologue is NOT
// -- it differs in every DLL -- which is one reason the head is hidden through
// mesh_bits and this function rather than by hooking DrawLara.)
const uint8_t kDrawCreatureHDPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };
const uint8_t kLaraMatricesTR1Prologue[] = { 0x40, 0x55, 0x56, 0x41, 0x56 };
const uint8_t kLaraMatricesTR23Prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };
const uint8_t kActionIndicatorsPrologue[] = { 0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x54 };
const uint8_t kGetJointsPrologue[] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x53, 0x10 };
const uint8_t kLaraGunTR1Prologue[] = {0x40,0x57,0x48,0x83,0xEC,0x20};
const uint8_t kLaraGunTR2Prologue[] = {0x40,0x55,0x57,0x41,0x54};
const uint8_t kLaraGunTR3Prologue[] = {0x40,0x55,0x41,0x54,0x41,0x55,0x41,0x56};
const uint8_t kFireWeaponTR1Prologue[] = {0x40,0x55,0x53,0x57,0x41,0x54};
const uint8_t kFireWeaponTR2Prologue[] = {0x40,0x55,0x53,0x41,0x54};
const uint8_t kFireWeaponTR3Prologue[] = {0x40,0x55,0x53,0x56,0x57};
const uint8_t kTargetLOSTR12Prologue[] = {0x48,0x89,0x5C,0x24,0x18};
const uint8_t kTargetLOSTR3Prologue[] = {0x40,0x55,0x53,0x56,0x57};
const uint8_t kFireHarpoonStockPrologue[] =
    {0x48,0x83,0xEC,0x48,0x83,0x3D};
const uint8_t kFireHarpoonRetailPrologue[] =
    {0x48,0x83,0xEC,0x58,0x83,0x3D};
const int kFireHarpoonStockRipFixups[] = {6};
const int kFireHarpoonRetailRipFixups[] = {6};
const uint8_t kFireExplosivePrologue[] =
    {0x4C,0x8B,0xDC,0x48,0x81,0xEC,0x88,0x00,0x00,0x00};
const uint8_t kFireGrenadePrologue[] =
    {0x4C,0x8B,0xDC,0x48,0x81,0xEC,0x88,0x00,0x00,0x00};
const uint8_t kAnimateShotgunPrologue[] = {0x89,0x4C,0x24,0x08,0x55};
const uint8_t kDrawGunFlashTR1Prologue[] = {0x48,0x83,0xEC,0x28,0xF6,0x05};
const int kDrawGunFlashTR1RipFixups[] = {6};
const uint8_t kDrawGunFlashTR23Prologue[] =
    {0x48,0x8B,0xC4,0x48,0x81,0xEC,0x88,0,0,0};

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
bool     g_crouchHidden = false;
bool     g_ledgeArmsOnly = false;
bool     g_meshOverride = false;
uint8_t* g_meshItem = nullptr;
uint32_t g_meshBaseBits = 0;
unsigned g_headDraws   = 0;       // Lara draws routed through the mesh_bits path
unsigned g_headSkips   = 0;       // face / sunglasses draws dropped
unsigned g_hairSkips   = 0;       // braid draws dropped
bool     g_loggedFirst = false;
bool     g_loggedVisibleGunAim = false;
bool     g_loggedActionIcon = false;
bool     g_loggedMotion = false;
bool     g_nativeEquipRequested = false;
int      g_nativeEquipStatus = -1;
bool     g_nativeEquipHoldMode = false;
bool     g_drawAwaitingLTRelease = false;
uint8_t  g_lastRawLT = 0;
int      g_lastGunTraceStatus = -1;
unsigned g_motionHandPasses[2] = {};
unsigned g_motionJointCalls[2] = {};
unsigned g_motionCorrections[2] = {};
uint32_t g_motionLastMask = 0;
const char* g_motionPoseFailure[2] = {"not-built","not-built"};
HHOOK g_calibrationMessageHook=nullptr;
HWND g_calibrationWindow=nullptr;
bool g_calibrationActive=false;
motiongun::CalibrationKeys g_calibrationKeys{};
int g_calibrationCommands[64]{};
unsigned g_calibrationCommandCount=0;
bool     g_neutralTaken = false;   // the neutral is taken once first person is live
bool     g_loggedLost  = false;
unsigned g_anchored    = 0;
unsigned g_skipped     = 0;

locomotion::Heading g_heading;
stabilization::RenderTurn g_renderTurn;
stabilization::GroundEye g_groundEye;
int g_lastClimbCameraState = -1;
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
bool g_stabilizeRoot = false, g_hardStopRoot = false;
uint32_t g_stabilizeAction = 0;
locomotion::Vec g_stabilizeDirection{};
stabilization::RootMotion g_rootMotion;
PHD_3DPOS g_scenePose{};
bool g_scenePoseValid = false;
int g_renderArm = -1;
int g_renderWrist = -1;
float g_handSkinPalette[32*12]{};
motiongun::Vec g_handSkinCentre{};
int g_firingHand = -1;
int16_t g_firingBaseAim[2]{};
PHD_3DPOS g_firingPose{};
motiongun::Vec g_firingDirection{};
unsigned g_autoAimTraceShots=0;
motiongun::TriggerInput g_gunTriggers;
motiongun::EquipInput g_gunEquip;
int g_triggerWeapon=0;
locomotion::Vec g_dragPrevious, g_dragCurrent, g_dragShown;
LARGE_INTEGER g_bodyTime{};

double TurnTime() {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return freq.QuadPart ? double(now.QuadPart) / double(freq.QuadPart) : 0;
}

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
    return locomotion::IsGroundLocomotionState(
        *reinterpret_cast<const int16_t*>(item + off::item_anim_state));
}

template <typename T>
T* Ptr(uint32_t rva) { return reinterpret_cast<T*>(g_boundBase + rva); }

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
    const int water = LaraWaterStatus();
    if (water == 1 || water == 2) return false; // native underwater/surface camera

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

bool CanHardStop(const uint8_t* item) {
    if (!CanWalk(item)) return false;
    return locomotion::CanHardStopState(
        *reinterpret_cast<const int16_t*>(item + off::item_anim_state),
        *reinterpret_cast<const int16_t*>(item + off::item_goal_state),
        (*reinterpret_cast<const uint16_t*>(item + off::item_flags) & 0x8u) != 0);
}

// Lara's body collision can stop at a wall while the avatar-fit eye sits beyond
// it. Trace from her collision origin toward the rendered head in short steps,
// using the same room collision query as roomscale movement. Leave enough
// space for both eyes and the near plane at the last clear point.
void ClampHeadToCollision(const uint8_t* item, const int32_t body[3],
                          int32_t head[3], bool airborne, double eyeY) {
    const int32_t dx = head[0] - body[0], dz = head[2] - body[2];
    const float distance = std::hypot(float(dx), float(dz));
    if (distance < 1.0f) return;
    if (distance > 512.0f) {
        head[0] = body[0];
        head[2] = body[2];
        return;
    }

    const int steps = std::clamp(static_cast<int>(std::ceil(distance / 16.0f)), 1, 32);
    int32_t clearX = body[0], clearZ = body[2];
    const int16_t room = *reinterpret_cast<const int16_t*>(item + off::item_room_number);
    for (int i = 1; i <= steps; ++i) {
        const int32_t x = body[0] + static_cast<int32_t>(int64_t(dx) * i / steps);
        const int32_t z = body[2] + static_cast<int32_t>(int64_t(dz) * i / steps);
        RoomCollision coll{};
        coll.radius = 64;
        // Airborne Lara can be more than 384 units above the floor. Keep the
        // wall query active without treating the drop below her as a wall.
        coll.badPos = airborne ? 4096 : 384;
        coll.badNeg = airborne ? -4096 : -384;
        coll.badCeiling = 0;
        coll.flags = airborne ? 0 : 5;
        coll.old[0] = clearX; coll.old[1] = body[1]; coll.old[2] = clearZ;
        coll.facing = locomotion::Angle(std::atan2(float(x - clearX), float(z - clearZ)));
        reinterpret_cast<Fn_GetCollisionInfo>(g_boundBase + g_boundDll->getCollisionInfo)(
            &coll, x, body[1], z, room, 762);
        if ((airborne && firstperson::AirborneEyeBlocked(
                coll.floorSamples, body[1], eyeY, coll.hitStatic)) ||
            (!airborne && (coll.floorSamples[0] < -384 ||
                           coll.floorSamples[0] > 384 ||
                           coll.floorSamples[1] >= 0)) ||
            coll.type == 8 || coll.type == 16 || coll.type == 32 ||
            coll.shift[0] || coll.shift[2]) {
            head[0] = clearX;
            head[2] = clearZ;
            return;
        }
        clearX = x;
        clearZ = z;
    }
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

    const int state = *reinterpret_cast<const int16_t*>(item + off::item_anim_state);
    const int anchorZ = locomotion::FirstPersonAnchorZ(
        state, Cfg().firstPersonAnchorZ, Cfg().firstPersonInteractionAnchorZ);

    // A point inside the skull rather than the neck pivot the joint sits on.
    // The avatar-fit forward offset is retracted while Lara is constrained
    // against a ledge wall or movable block; those interactions own her body
    // position, so the viewpoint must stay on her side of the contact plane.
    const int32_t local[3] = { Cfg().firstPersonAnchorX,
                               Cfg().firstPersonAnchorY,
                               anchorZ };

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
uint32_t VisibleMeshBits(uint32_t base, bool head, bool roll,
                         bool crouch, bool ledge) {
    if (roll || crouch) return 0;
    if (ledge) return base & kArmMeshBits;
    return head ? base & ~kHeadMeshBit : base;
}

void SetMeshVisibility(bool hideHead, bool hideRoll,
                       bool hideCrouch = false, bool ledgeArms = false) {
    if (!g_boundDll || !g_boundBase) return;
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item) {
        g_meshOverride = false;
        g_meshItem = nullptr;
        g_headHidden = g_rollHidden = g_crouchHidden = g_ledgeArmsOnly = false;
        return;
    }
    if (g_meshOverride && item != g_meshItem) {
        // The old item belongs to a level that was unloaded; never dereference
        // it. Capture the new Lara independently if an override is still active.
        g_meshOverride = false;
        g_meshItem = nullptr;
    }

    auto& bits = *reinterpret_cast<uint32_t*>(item + off::item_mesh_bits);
    if (!hideHead && !hideRoll && !hideCrouch && !ledgeArms) {
        if (g_meshOverride && item == g_meshItem) bits = g_meshBaseBits;
        g_meshOverride = false;
        g_meshItem = nullptr;
        g_headHidden = g_rollHidden = g_crouchHidden = g_ledgeArmsOnly = false;
        return;
    }
    if (!g_meshOverride) {
        g_meshOverride = true;
        g_meshItem = item;
        g_meshBaseBits = bits;
    } else if (!g_rollHidden && !g_crouchHidden && !g_ledgeArmsOnly) {
        // The game can change Lara's mesh mask when weapons are drawn or
        // holstered. Preserve those native changes instead of replaying the
        // mask captured when first person was first entered. During a roll our
        // zero mask owns every bit, so keep the last pre-roll snapshot.
        const uint32_t expected = VisibleMeshBits(g_meshBaseBits, g_headHidden,
            g_rollHidden, g_crouchHidden, g_ledgeArmsOnly);
        if (bits != expected)
            g_meshBaseBits = (bits & ~kHeadMeshBit)
                           | (g_meshBaseBits & kHeadMeshBit);
    }
    g_headHidden = hideHead;
    g_rollHidden = hideRoll;
    g_crouchHidden = hideCrouch;
    g_ledgeArmsOnly = ledgeArms;
    bits = VisibleMeshBits(g_meshBaseBits, hideHead, hideRoll,
                           hideCrouch, ledgeArms);
}

bool IsRollState(const uint8_t* item) {
    if (!item) return false;
    // Shared classic Lara state IDs: roll end, standing-roll start, underwater
    // roll and airborne roll. The first two cover the ordinary B-button roll.
    switch (*reinterpret_cast<const int16_t*>(item + off::item_anim_state)) {
    case 23: case 45: case 66: case 68: case 72: return true;
    default: return false;
    }
}

bool IsCrouchState(const uint8_t* item) {
    if (!item || !g_boundDll || g_boundDll->module[4] != L'3') return false;
    // TR3's lara_control_routines table: duck, crawl and their turn states.
    switch (*reinterpret_cast<const int16_t*>(item + off::item_anim_state)) {
    case 71: case 80: case 81: case 84: case 85: case 86:
    case 89: case 90: return true;
    default: return false;
    }
}

bool HeadAimFor(uint8_t* item) {
    return item && Cfg().firstPersonHeadAim && g_active && g_haveHeading &&
           Gate() && item == g_headingItem &&
           item == *Ptr<uint8_t*>(g_boundDll->laraItem);
}

void WriteHeadAim(uint8_t* item) {
    uint8_t* lara = Ptr<uint8_t>(g_boundDll->lara);
    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const int16_t yaw = locomotion::Angle(
        g_heading.World(VR().HeadYawRadians()) - locomotion::Radians(pos.y_rot));
    const int16_t pitch = locomotion::Angle(VR().HeadPitchRadians());
    for (const uint32_t offset : { off::lara_left_arm, off::lara_right_arm }) {
        uint8_t* a = lara + offset;
        *reinterpret_cast<int16_t*>(a + off::arm_lock) = 1;
        *reinterpret_cast<int16_t*>(a + off::arm_y_rot) = yaw;
        *reinterpret_cast<int16_t*>(a + off::arm_x_rot) = pitch;
        *reinterpret_cast<int16_t*>(a + off::arm_z_rot) = 0;
    }
}

void __cdecl Detour_CalculateLaraMatrices(uint8_t* item) {
    if (g_boundDll && g_boundBase && HeadAimFor(item) &&
        *Ptr<int16_t>(g_boundDll->lara + off::lara_gun_status) == 4 &&
        !*Ptr<uint8_t*>(g_boundDll->lara + off::lara_target)) {
        // This pose is visual only. Native target tracking reads arm.lock, and
        // forcing that lock in the weapon simulation changes auto-targeting.
        // Restore native arm state before the next weapon simulation tick.
        uint8_t* lara = Ptr<uint8_t>(g_boundDll->lara);
        uint64_t left, right;
        std::memcpy(&left, lara + off::lara_left_arm + off::arm_lock, sizeof(left));
        std::memcpy(&right, lara + off::lara_right_arm + off::arm_lock, sizeof(right));
        WriteHeadAim(item);
        g_hCalculateLaraMatrices.Original<Fn_CalculateLaraMatrices>()(item);
        std::memcpy(lara + off::lara_left_arm + off::arm_lock, &left, sizeof(left));
        std::memcpy(lara + off::lara_right_arm + off::arm_lock, &right, sizeof(right));
        if (!g_loggedVisibleGunAim) {
            g_loggedVisibleGunAim = true;
            Log("firstperson: no-target gun pose follows HMD; native auto-aim preserved");
        }
        return;
    }
    g_hCalculateLaraMatrices.Original<Fn_CalculateLaraMatrices>()(item);
}

void __cdecl Detour_DrawActionIndicators() {
    const auto original = g_hDrawActionIndicators.Original<Fn_DrawActionIndicators>();
    if (!g_active || !g_boundDll || !g_boundBase || !VR().poseValid()) {
        original();
        return;
    }
    const auto& d = *g_boundDll;
    const int count = *Ptr<int32_t>(d.nActionIndicator);
    if (count <= 0 || count > 20) { original(); return; }
    auto* points = Ptr<actionicon::Point>(d.actionIndicator);
    actionicon::Point saved[20];
    std::memcpy(saved, points, count * sizeof(saved[0]));
    const float nearZ = float(*Ptr<int32_t>(d.phdZNear)) / 16384.0f;
    const float farZ = float(*Ptr<int32_t>(d.phdZFar)) / 16384.0f;
    bool changed = false;
    for (int i = 0; i < count; ++i)
        changed = actionicon::Place(points[i], Ptr<int32_t>(d.w2vMatrix),
            float(*Ptr<int32_t>(d.phdPersp)),
            float(*Ptr<int32_t>(d.phdCenterX)),
            float(*Ptr<int32_t>(d.phdCenterY)), nearZ, farZ) || changed;
    original();
    std::memcpy(points, saved, count * sizeof(saved[0]));
    if (changed && !g_loggedActionIcon) {
        g_loggedActionIcon = true;
        Log("firstperson: nearby native Action icon kept visible on HUD");
    }
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

bool MotionReady();
void __cdecl Detour_DrawCreatureHD(void* item, int32_t useMeshBits) {
    const bool lara=g_active && g_boundDll && g_boundBase &&
        item==*Ptr<void*>(g_boundDll->laraItem);
    if (lara && MotionReady()) {
        if (DrawingHeadGeometry(static_cast<const uint8_t*>(item))) return;
        if (!useMeshBits || g_rollHidden) return;
        auto& bits=*reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(item)+off::item_mesh_bits);
        const uint32_t saved=bits;
        g_motionLastMask=saved;
        const uint32_t hands=motiongun::HandOnlyMask(saved);
        const int prior=g_renderArm;
        for (int hand=0;hand<2;++hand) {
            const uint32_t mask=hand ? 0x400u : 0x2000u;
            vr::HmdMatrix34_t pose{};
            if (!(hands&mask) || !VR().ControllerPose(hand,pose)) continue;
            ++g_motionHandPasses[hand];
            bits=mask; g_renderArm=hand; g_renderWrist=-1;
            g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item,1);
            g_renderWrist=-1;
        }
        bits=saved; g_renderArm=prior;
        return;
    }
    if (g_active && (g_rollHidden || g_crouchHidden) && g_boundDll && g_boundBase &&
        item == *Ptr<void*>(g_boundDll->laraItem)) return;
    if ((g_headHidden || g_ledgeArmsOnly) && g_boundDll && g_boundBase &&
        item == *Ptr<void*>(g_boundDll->laraItem)) {
        if (DrawingHeadGeometry(static_cast<const uint8_t*>(item))) {
            ++g_headSkips;
            return;              // the face, the sunglasses: not drawn at all
        }
        // A zero flag means this native body pass uses all joints, even if the
        // persistent mask is stale or empty. Match TR4/5: mask this draw only,
        // preserving the full hanging arms and the native hand transforms.
        auto& bits=*reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(item)+off::item_mesh_bits);
        const uint32_t saved=bits;
        bits=firstperson::HdDrawMeshBits(saved,useMeshBits!=0,g_ledgeArmsOnly);
        if (useMeshBits==0) ++g_headDraws;
        g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item,1);
        bits=saved;
        return;
    }
    g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item, useMeshBits);
}

// The braid. Drawn outside Lara's skeleton entirely, by its own function, so
// neither mesh_bits nor the geometry test above can reach it -- and from inside
// her head it sweeps through the view.
void __cdecl Detour_DrawHair(int32_t arg) {
    if (g_active && (g_headHidden || g_rollHidden || g_crouchHidden ||
                     g_ledgeArmsOnly)) {
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

bool MotionWeaponSupported(int gun) {
    if (!g_boundDll) return false;
    const int last = g_boundDll->module[4] == L'1' ? 4 :
                     g_boundDll->module[4] == L'2' ? 7 : 8;
    return gun >= 1 && gun <= last;
}

bool DualMotionWeapon(int gun) {
    return gun == 1 || gun == 3 ||
        (gun == 2 && g_boundDll && g_boundDll->module[4] != L'3');
}

const char* MotionBlockedReason() {
    if (!Cfg().firstPersonMotionGuns) return "disabled-in-INI";
    if (!g_active || !g_scenePoseValid || !g_haveHeading)
        return "first-person-camera-not-ready";
    if (!g_boundDll || !g_boundBase || GameDllBound() != g_boundDll ||
        GameDllBase() != g_boundBase) return "game-DLL-changing";
    if (!g_hGetJoints.installed() || !g_hDrawCreatureHD.installed() ||
        !g_hFireWeapon.installed() || !g_hGetTargetOnLOS.installed())
        return "motion-hooks-unavailable";
    if (!g_boundDll->nextItemActive || !g_boundDll->getSpheres ||
        !g_boundDll->findTargetPoint || !g_boundDll->los)
        return "motion-targeting-unavailable";
    if (!Cfg().positionalTracking || !Cfg().firstPersonHeadTranslation)
        return "head-translation-disabled";
    if (!(AppFlag(drva::app_off::cfg_flags) & 1))
        return "classic-graphics";
    const auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    const auto* lara = Ptr<uint8_t>(g_boundDll->lara);
    if (!item || item != g_headingItem) return "Lara-item-changed";
    if (*reinterpret_cast<const int16_t*>(lara + off::lara_gun_status) != 4)
        return "guns-not-ready";
    const int gun = *reinterpret_cast<const int16_t*>(lara + 4);
    if (!MotionWeaponSupported(gun)) return "weapon-not-supported";
    if (g_boundDll->module[4]==L'2' &&
        ((gun==6 && !g_hAnimateShotgun.installed()) ||
         (gun==7 && !g_hFireHarpoon.installed()))) return "projectile-hook-unavailable";
    if (g_boundDll->module[4]==L'3' &&
        ((gun==6 && !g_hFireRocket.installed()) ||
         (gun==7 && !g_hFireGrenade.installed()) ||
         (gun==8 && !g_hFireHarpoon.installed()))) return "projectile-hook-unavailable";
    vr::HmdMatrix34_t pose{};
    if (!VR().ControllerPose(1, pose)) return "right-controller-pose-missing";
    if (DualMotionWeapon(gun) && !VR().ControllerPose(0, pose))
        return "left-controller-pose-missing";
    return nullptr;
}

bool MotionReady() {
    return MotionBlockedReason()==nullptr;
}

bool CalibrationFocused() {
    return g_calibrationWindow && GetForegroundWindow()==g_calibrationWindow;
}

LRESULT CALLBACK CalibrationMessageHook(int code, WPARAM remove, LPARAM param) {
    if (code>=0 && remove==PM_REMOVE) {
        auto* msg=reinterpret_cast<MSG*>(param);
        if (msg->hwnd==g_calibrationWindow &&
            (msg->message==WM_KEYDOWN || msg->message==WM_KEYUP ||
             msg->message==WM_SYSKEYDOWN || msg->message==WM_SYSKEYUP)) {
            const int key=int(msg->wParam)-VK_F1;
            const bool down=msg->message==WM_KEYDOWN || msg->message==WM_SYSKEYDOWN;
            const bool active=g_calibrationActive && CalibrationFocused() &&
                !InInventory() && !InTitle() && !InCutscene();
            const bool controlKey=msg->wParam==VK_CONTROL ||
                msg->wParam==VK_LCONTROL || msg->wParam==VK_RCONTROL;
            int command=-1;
            const bool consumed=controlKey
                ? g_calibrationKeys.ControlEvent(down,active,
                    (GetKeyState(VK_MENU)&0x8000)!=0)
                : g_calibrationKeys.Event(key,down,
                    (GetKeyState(VK_CONTROL)&0x8000)!=0,
                    (GetKeyState(VK_SHIFT)&0x8000)!=0,
                    (GetKeyState(VK_MENU)&0x8000)!=0,active,
                    down && (msg->lParam & (LPARAM(1)<<30)),command);
            if (consumed) {
                if (command>=0 && g_calibrationCommandCount<64)
                    g_calibrationCommands[g_calibrationCommandCount++]=command;
                msg->message=WM_NULL; msg->wParam=0; msg->lParam=0;
            }
        }
    }
    return CallNextHookEx(g_calibrationMessageHook,code,remove,param);
}

void PollMotionGunCalibration() {
    g_calibrationActive=Cfg().firstPersonMotionGunHotkeys &&
        g_boundDll && GameDllBound()==g_boundDll && GameDllBase()==g_boundBase &&
        MotionReady();
    if (!g_calibrationMessageHook && Cfg().firstPersonMotionGuns &&
        Cfg().firstPersonMotionGunHotkeys) {
        HWND window=GetForegroundWindow();
        DWORD process=0;
        const DWORD thread=GetWindowThreadProcessId(window,&process);
        if (window && process==GetCurrentProcessId() && thread==GetCurrentThreadId()) {
            g_calibrationWindow=window;
            g_calibrationMessageHook=SetWindowsHookExW(
                WH_GETMESSAGE,CalibrationMessageHook,nullptr,thread);
            if (g_calibrationMessageHook)
                Log("gun calibration: Ctrl+F1..F6 position, Ctrl+Shift+F1..F6 angles, F7 save/restore");
        }
    }
    if (g_calibrationActive && CalibrationFocused()) {
        for (unsigned i=0;i<g_calibrationCommandCount;++i) {
            const int command=g_calibrationCommands[i];
            if (command==6) {
                const bool saved=SaveMotionGunCalibration();
                MessageBeep(saved ? MB_OK : MB_ICONERROR);
            } else if (command==13) {
                RestoreMotionGunCalibration();
                MessageBeep(MB_OK);
            } else AdjustMotionGunCalibration(command);
        }
    }
    g_calibrationCommandCount=0;
}

motiongun::Vec NativeJointPoint(const uint8_t* item, int joint) {
    const int frac = std::clamp(*Ptr<int32_t>(g_boundDll->frameFrac), 0, 256);
    const auto* prev = reinterpret_cast<const int32_t*>(
        item + off::item_joints_prev + joint * off::joint_stride);
    const auto* cur = reinterpret_cast<const int32_t*>(
        item + off::item_joints_cur + joint * off::joint_stride);
    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const auto& old = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos_prev);
    return {
        float(Lerp(old.x_pos,pos.x_pos,frac)) + Lerp(prev[3],cur[3],frac)/16384.0f,
        float(Lerp(old.y_pos,pos.y_pos,frac)) + Lerp(prev[7],cur[7],frac)/16384.0f,
        float(Lerp(old.z_pos,pos.z_pos,frac)) + Lerp(prev[11],cur[11],frac)/16384.0f
    };
}

struct GunPose {
    motiongun::Basis basis{};
    motiongun::Vec hand{}, nativeHand{}, muzzle{}, direction{};
    int16_t yaw=0, pitch=0;
};

bool BuildGunPose(int hand, GunPose& out) {
    if (hand < 0 || hand > 1 || !MotionReady()) return false;
    auto rejected=[hand](const char* why) {
        g_motionPoseFailure[hand]=why;
        return false;
    };
    float right=0, down=0, forward=0;
    vr::HmdMatrix34_t tracked{};
    if (!VR().FirstPersonControllerOffset(hand,right,down,forward) ||
        !VR().ControllerPose(hand,tracked) ||
        std::fabs(right)>2 || std::fabs(down)>2 || std::fabs(forward)>2)
        return rejected("controller-offset-unavailable-or-over-2m");
    const float scale=LiveWorldUnitsPerMetre();
    if (!std::isfinite(scale) || scale<=1) return rejected("invalid-world-scale");
    const auto& calibration=LiveMotionGunCalibration();
    const auto controller=motiongun::CalibratedController(
        motiongun::ControllerBasis(tracked.m,g_heading.base),calibration);
    const motiongun::Vec physical=motiongun::HandInWorld(
        {float(g_scenePose.x_pos),float(g_scenePose.y_pos),float(g_scenePose.z_pos)},
        right,down,forward,g_heading.base,scale);
    out.basis=motiongun::GunBasis(controller);
    out.hand=motiongun::GripFrame(out.basis,physical,
        calibration.gripForwardMetres*scale,
        calibration.raiseMetres*scale,
        calibration.rightMetres*scale).origin;
    const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    out.nativeHand=NativeJointPoint(item,hand ? 10 : 13);
    const auto& body=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
    if (std::fabs(out.nativeHand.x-body.x_pos)>4096 ||
        std::fabs(out.nativeHand.y-body.y_pos)>4096 ||
        std::fabs(out.nativeHand.z-body.z_pos)>4096)
        return rejected("native-wrist-out-of-range");
    const int gun=*Ptr<int16_t>(g_boundDll->lara+4);
    out.muzzle=motiongun::Add(out.hand,motiongun::Transform(
        out.basis,motiongun::MuzzleLocal(gun,hand,g_boundDll->module[4]-L'1')));
    out.direction={controller.r[0][2],controller.r[1][2],controller.r[2][2]};
    float yaw=0,pitch=0;
    if (!motiongun::DirectionAngles(out.direction,yaw,pitch)) return rejected("invalid-barrel-direction");
    out.yaw=locomotion::Angle(yaw);
    out.pitch=locomotion::Angle(pitch);
    if (!std::isfinite(out.hand.x) || !std::isfinite(out.hand.y) ||
        !std::isfinite(out.hand.z)) return rejected("nonfinite-hand");
    g_motionPoseFailure[hand]="pose-ok";
    return true;
}

void __cdecl Detour_DrawGunFlash(int32_t weapon,int32_t unused,int32_t joint) {
    const auto original=g_hDrawGunFlash.Original<Fn_DrawGunFlash>();
    const int hand=joint==13 ? 0 : joint==10 ? 1 : -1;
    if (hand<0 || !g_boundDll ||
        weapon!=*Ptr<int16_t>(g_boundDll->lara+4)) {
        original(weapon,unused,joint);
        return;
    }
    GunPose gun{};
    if (!BuildGunPose(hand,gun)) {
        original(weapon,unused,joint);
        return;
    }
    int32_t* matrix=*Ptr<int32_t*>(g_boundDll->phdMxptr);
    if (!matrix) {
        original(weapon,unused,joint);
        return;
    }
    int32_t saved[12];
    std::memcpy(saved,matrix,sizeof(saved));
    if (!motiongun::RetargetFlashMatrix(matrix,
            gun.basis,motiongun::Sub(gun.hand,gun.nativeHand))) {
        original(weapon,unused,joint);
        return;
    }
    original(weapon,unused,joint);
    std::memcpy(matrix,saved,sizeof(saved));
}

void __cdecl Detour_GetJoints(uint8_t* item) {
    g_renderWrist=-1;
    g_hGetJoints.Original<Fn_GetJoints>()(item);
    if (g_renderArm>=0 && g_renderArm<2) ++g_motionJointCalls[g_renderArm];
    if (g_renderArm<0) return;
    if (!item || !g_boundDll || !g_boundDll->joints || !MotionReady() ||
        item!=*Ptr<uint8_t*>(g_boundDll->laraItem)) {
        if (g_renderArm>=0 && g_renderArm<2)
            g_motionPoseFailure[g_renderArm]="joint-pass-unavailable";
        return;
    }
    float* joints=Ptr<float>(g_boundDll->joints);
    GunPose gun{};
    if (!BuildGunPose(g_renderArm,gun)) return;
    const int nativeJoint=g_renderArm ? 10 : 13;
    const int object=*reinterpret_cast<const int16_t*>(item+off::item_object_number);
    if (object<0) { g_motionPoseFailure[g_renderArm]="invalid-object"; return; }
    const uint8_t* obj=Ptr<uint8_t>(g_boundDll->objects)+
        object*off::object_stride;
    const uint8_t* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int32_t*>(geom+28);
    // The remaster's legacy path still fills Lara's 15 classic joint slots.
    const int count=bones>0 ? bones : 15;
    if (count<15 || count>32) {
        g_motionPoseFailure[g_renderArm]="bone-count-unavailable";
        return;
    }
    int pivot=nativeJoint;
    motiongun::Frame inverseBind{};
    if (bones>0) {
        const auto* mapping=*reinterpret_cast<const int32_t* const*>(geom+48);
        const auto* poses=*reinterpret_cast<const float* const*>(geom+72);
        if (!mapping || !poses) { g_motionPoseFailure[g_renderArm]="bind-pose-missing"; return; }
        pivot=-1;
        for (int i=0;i<count;++i)
            if (mapping[i]==nativeJoint) { pivot=i; break; }
        if (pivot<0) { g_motionPoseFailure[g_renderArm]="wrist-bone-missing"; return; }
        inverseBind=motiongun::HdInverseBind(
            motiongun::ReadRows(poses+pivot*12));
    } else {
        if (pivot>=count) { g_motionPoseFailure[g_renderArm]="legacy-bone-count"; return; }
        const float* bind=reinterpret_cast<const float*>(obj+192)+pivot*16;
        for (int row=0;row<3;++row)
            for (int col=0;col<3;++col)
                inverseBind.basis.r[row][col]=bind[col*4+row];
        inverseBind.origin={bind[12],bind[13],bind[14]};
    }
    const auto palette=motiongun::ReadRows(joints+pivot*12);
    motiongun::Frame bind{},correction{};
    if (!motiongun::Inverse(inverseBind,bind)) { g_motionPoseFailure[g_renderArm]="bind-inverse-invalid"; return; }
    const auto wrist=motiongun::Multiply(palette,bind);
    const motiongun::Frame desired{gun.basis,motiongun::Add(
        wrist.origin,motiongun::Sub(gun.hand,gun.nativeHand))};
    if (!motiongun::PaletteCorrection(palette,inverseBind,desired,correction)) {
        g_motionPoseFailure[g_renderArm]="palette-correction-invalid"; return;
    }
    for (int joint=0;joint<count;++joint) {
        float* bone=joints+joint*12;
        motiongun::WriteRows(motiongun::Multiply(correction,
            motiongun::ReadRows(bone)),bone);
    }
    // DrawCreatureHD zeroes masked entries AFTER this hook. Preserve the
    // corrected palette for the rigid tracked-wrist shader and sealing rim.
    std::memcpy(g_handSkinPalette,joints,sizeof(g_handSkinPalette));
    g_handSkinCentre=desired.origin;
    g_renderWrist=pivot;
    ++g_motionCorrections[g_renderArm];
    g_motionPoseFailure[g_renderArm]="corrected";
    if (!g_loggedMotion) {
        g_loggedMotion=true;
        Log("firstperson: HD gun hand follows tracked controller");
    }
}

bool MotionTriggerMode() {
    if (!(Cfg().firstPersonMotionGuns && g_active && g_boundDll && g_boundBase &&
        GameDllBound()==g_boundDll && GameDllBase()==g_boundBase &&
        (AppFlag(drva::app_off::cfg_flags)&1))) return false;
    if (!Cfg().positionalTracking || !Cfg().firstPersonHeadTranslation ||
        !g_hFireWeapon.installed() || !g_hGetTargetOnLOS.installed()) return false;
    const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item || *reinterpret_cast<const int16_t*>(item+off::item_hit_points)<=0)
        return false;
    const int gun=*Ptr<int16_t>(g_boundDll->lara+4);
    const int last=*Ptr<int16_t>(g_boundDll->lara+8);
    // Keep input intent through temporary tracking loss. MotionReady pauses
    // shots, but must not reset equip intent or latch held RT off on recovery.
    return MotionWeaponSupported(gun ? gun : last);
}

// FireWeapon tests ONLY the supplied target's spheres. Its miss LOS handles
// scenery, not enemy damage. Acquire per hand even when Lara's head/body lock
// is empty, then leave spread, obstruction, ammo and damage to FireWeapon.
uint8_t* SelectGunTarget(const GunPose& gun, motiongun::Vec& ray) {
    using namespace motiongun;
    const auto& d=*g_boundDll;
    if (!d.nextItemActive || !d.getSpheres || !d.findTargetPoint || !d.los) return nullptr;
    auto* items=*Ptr<uint8_t*>(d.items);
    auto* lara=*Ptr<uint8_t*>(d.laraItem);
    if (!items || !lara) return nullptr;
    struct Sphere { int32_t x,y,z,r; };
    using Fn_Spheres=int32_t (__cdecl*)(uint8_t*,Sphere*,int32_t);
    using Fn_TargetPoint=void (__cdecl*)(uint8_t*,ShotVector*);
    using Fn_LOS=int32_t (__cdecl*)(ShotVector*,ShotVector*);
    using Fn_Floor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
    ShotVector source{int32_t(std::lround(gun.muzzle.x)),
                      int32_t(std::lround(gun.muzzle.y)),
                      int32_t(std::lround(gun.muzzle.z)),
                      *reinterpret_cast<int16_t*>(lara+off::item_room_number),0};
    reinterpret_cast<Fn_Floor>(g_boundBase+d.getFloor)(source.x,source.y,source.z,&source.room);
    auto visible=[&](Vec point,int16_t room) {
        ShotVector start=source;
        ShotVector end{int32_t(std::lround(point.x)),int32_t(std::lround(point.y)),
                       int32_t(std::lround(point.z)),room,0};
        return reinterpret_cast<Fn_LOS>(g_boundBase+d.los)(&start,&end)!=0;
    };
    uint8_t* direct=nullptr;
    uint8_t* assisted=nullptr;
    Vec assistRay{};
    float nearest=20480.f, bestCos=-1.f, bestDistance=8192.f*8192.f;
    int index=*Ptr<int16_t>(d.nextItemActive);
    for (int visited=0; index>=0 && visited<4096; ++visited) {
        auto* item=items+index*off::item_stride;
        index=*reinterpret_cast<int16_t*>(item+off::item_next_active);
        if (item==lara || *reinterpret_cast<int16_t*>(item+off::item_hit_points)<=0) continue;
        const auto& pos=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
        const Vec delta=Sub({float(pos.x_pos),float(pos.y_pos),float(pos.z_pos)},gun.muzzle);
        if (Dot(delta,delta)>20480.f*20480.f) continue;
        const int object=*reinterpret_cast<int16_t*>(item+off::item_object_number);
        if (object<0) continue;
        const int meshes=*reinterpret_cast<int16_t*>(Ptr<uint8_t>(d.objects)+object*off::object_stride);
        Sphere spheres[256]{};
        if (meshes<=0 || meshes>256) continue;
        const int count=reinterpret_cast<Fn_Spheres>(g_boundBase+d.getSpheres)(item,spheres,1);
        if (count<=0 || count>256) continue;
        ShotVector targetPoint{};
        reinterpret_cast<Fn_TargetPoint>(g_boundBase+d.findTargetPoint)(item,&targetPoint);
        const Vec nativePoint{float(targetPoint.x),float(targetPoint.y),float(targetPoint.z)};
        Vec bodyPoint{};
        float bodyDistance=std::numeric_limits<float>::max();
        for (int i=0;i<count;++i) {
            const auto& sphere=spheres[i];
            if (sphere.r<=0) continue;
            const Vec centre{float(sphere.x),float(sphere.y),float(sphere.z)};
            float distance=0;
            if (ShotSphere(gun.muzzle,gun.direction,centre,float(sphere.r),distance) &&
                distance<nearest && visible(Add(gun.muzzle,Scale(gun.direction,distance)),targetPoint.room)) {
                direct=item;
                nearest=distance;
            }
            // Pick a real hit sphere near the native animated body target,
            // rather than an arbitrary height above the creature's origin.
            const Vec offset=Sub(centre,nativePoint);
            const float distance2=Dot(offset,offset);
            if (distance2<bodyDistance) { bodyDistance=distance2; bodyPoint=centre; }
        }
        Vec candidate{};
        if (bodyDistance==std::numeric_limits<float>::max() ||
            !AssistedDirection(gun.muzzle,gun.direction,bodyPoint,candidate,
                               Cfg().firstPersonAutoAimDegrees)) continue;
        const float cosine=Dot(candidate,gun.direction);
        const Vec offset=Sub(bodyPoint,gun.muzzle);
        const float distance2=Dot(offset,offset);
        if ((cosine>bestCos || (cosine==bestCos && distance2<bestDistance)) &&
            visible(bodyPoint,targetPoint.room)) {
            bestCos=cosine; bestDistance=distance2;
            assisted=item; assistRay=candidate;
        }
    }
    if (direct) return direct; // Direct barrel hits take priority over assistance.
    if (assisted) ray=assistRay;
    return assisted;
}

int32_t __cdecl Detour_FireWeapon(int32_t weapon,void* target,void* extra,
                                  const int16_t* aim) {
    const auto original=g_hFireWeapon.Original<Fn_FireWeapon>();
    if (!MotionTriggerMode() || !aim || weapon!=*Ptr<int16_t>(g_boundDll->lara+4))
        return original(weapon,target,extra,aim);
    const uint64_t caller=reinterpret_cast<uint64_t>(_ReturnAddress())-g_boundBase;
    const int hand=weapon>=4 ? 1 :
        caller==g_boundDll->rightFireReturn && DualMotionWeapon(weapon) ? 1 :
        caller==g_boundDll->leftFireReturn ?
            (DualMotionWeapon(weapon) ? 0 : 1) : -1;
    if (hand<0) return original(weapon,target,extra,aim);
    if (weapon<=3 && g_gunTriggers.active && !g_gunTriggers.pending[hand])
        return 0;
    GunPose gun{};
    if (!BuildGunPose(hand,gun)) return 0; // Pose loss must not fire from Lara's head.
    if (weapon<=3 && g_gunTriggers.active) g_gunTriggers.Consume(hand);
    motiongun::Vec ray=gun.direction;
    const bool nativeTarget=target!=nullptr;
    target=SelectGunTarget(gun,ray);
    const bool trace=g_autoAimTraceShots++<100;
    const int hpBefore=target ? *reinterpret_cast<int16_t*>(
        static_cast<uint8_t*>(target)+off::item_hit_points) : 0;
    float yaw=0,pitch=0;
    if (!motiongun::DirectionAngles(ray,yaw,pitch)) return 0;
    g_firingPose={};
    g_firingPose.x_pos=int32_t(std::lround(gun.muzzle.x));
    g_firingPose.y_pos=int32_t(std::lround(gun.muzzle.y));
    g_firingPose.z_pos=int32_t(std::lround(gun.muzzle.z));
    g_firingPose.y_rot=locomotion::Angle(yaw);
    g_firingPose.x_rot=locomotion::Angle(pitch);
    if (weapon>=4) {
        const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
        const auto& body=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
        const auto* arm=Ptr<uint8_t>(g_boundDll->lara+off::lara_left_arm);
        g_firingBaseAim[0]=int16_t(body.y_rot+
            *reinterpret_cast<const int16_t*>(arm+off::arm_y_rot));
        g_firingBaseAim[1]=*reinterpret_cast<const int16_t*>(arm+off::arm_x_rot);
    } else {
        g_firingBaseAim[0]=aim[0];
        g_firingBaseAim[1]=aim[1];
    }
    const int prior=g_firingHand;
    g_firingHand=hand;
    const int32_t result=original(weapon,target,extra,aim);
    g_firingHand=prior;
    if (trace) {
        const int hpAfter=target ? *reinterpret_cast<int16_t*>(
            static_cast<uint8_t*>(target)+off::item_hit_points) : 0;
        LogF("firstperson: tracked shot hand=%d native-target=%d gun-target=%d "
             "assist=%d result=%d hp=%d->%d",hand,int(nativeTarget),int(target!=nullptr),
             int(motiongun::Dot(motiongun::Sub(ray,gun.direction),
                               motiongun::Sub(ray,gun.direction))>1e-8f),result,hpBefore,hpAfter);
    }
    return result;
}

int32_t __cdecl Detour_GetTargetOnLOS(ShotVector* source,ShotVector* dest,
                                      int32_t flags,int32_t mode) {
    const auto original=g_hGetTargetOnLOS.Original<Fn_GetTargetOnLOS>();
    if (g_firingHand<0 || !source || !dest || !g_boundDll || !g_boundBase)
        return original(source,dest,flags,mode);
    const uint64_t caller=reinterpret_cast<uint64_t>(_ReturnAddress())-g_boundBase;
    const bool hit=caller==g_boundDll->hitLosReturn;
    if (!hit && caller!=g_boundDll->missLosReturn)
        return original(source,dest,flags,mode);
    const int32_t dx=g_firingPose.x_pos-source->x;
    const int32_t dy=g_firingPose.y_pos-source->y;
    const int32_t dz=g_firingPose.z_pos-source->z;
    source->x=g_firingPose.x_pos;
    source->y=g_firingPose.y_pos;
    source->z=g_firingPose.z_pos;
    source->room=*reinterpret_cast<int16_t*>(
        *Ptr<uint8_t*>(g_boundDll->laraItem)+off::item_room_number);
    using Fn_GetFloor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
    reinterpret_cast<Fn_GetFloor>(g_boundBase+g_boundDll->getFloor)(
        source->x,source->y,source->z,&source->room);
    if (hit) {
        dest->x+=dx; dest->y+=dy; dest->z+=dz;
    } else {
        const auto endpoint=motiongun::Add(
            {float(source->x),float(source->y),float(source->z)},
            motiongun::Scale(g_firingDirection,20480.0f));
        dest->x=int32_t(std::lround(endpoint.x));
        dest->y=int32_t(std::lround(endpoint.y));
        dest->z=int32_t(std::lround(endpoint.z));
    }
    return original(source,dest,flags,mode);
}

// The harpoon, rocket and grenade bypass FireWeapon. They allocate a native
// ITEM_INFO and call AddActiveItem before returning, so real ammo, animation,
// effects and collision remain owned by the game. Retarget only that newly
// allocated missile, using the same tracked barrel and narrow native target
// assist as hitscan shots.
void RetargetProjectile(int16_t index,int weapon,int object,const GunPose& gun) {
    if (*Ptr<int16_t>(g_boundDll->nextItemFree)==index) return;
    auto* items=*Ptr<uint8_t*>(g_boundDll->items);
    if (!items) return;
    auto* missile=items+size_t(index)*3664;
    if (*reinterpret_cast<int16_t*>(missile+off::item_object_number)!=object)
        return;
    int16_t room=*reinterpret_cast<int16_t*>(missile+off::item_room_number);
    const int16_t oldRoom=room;
    using Fn_GetFloor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
    reinterpret_cast<Fn_GetFloor>(g_boundBase+g_boundDll->getFloor)(
        int32_t(std::lround(gun.muzzle.x)),
        int32_t(std::lround(gun.muzzle.y)),
        int32_t(std::lround(gun.muzzle.z)),&room);
    if (room<0 || room>=*Ptr<int16_t>(g_boundDll->numberRooms)) return;
    motiongun::Vec ray=gun.direction;
    auto* target=*Ptr<uint8_t*>(g_boundDll->lara+off::lara_target);
    if (target && *reinterpret_cast<int16_t*>(target+off::item_hit_points)>0) {
        const auto& p=*reinterpret_cast<const PHD_3DPOS*>(target+off::item_pos);
        motiongun::Vec assisted{};
        if (motiongun::AssistedDirection(gun.muzzle,ray,
                {float(p.x_pos),float(p.y_pos-256),float(p.z_pos)},assisted,
                Cfg().firstPersonAutoAimDegrees))
            ray=assisted;
    }
    const float flat=std::hypot(ray.x,ray.z);
    if (!std::isfinite(flat) || flat<0.01f) return;
    auto& pos=*reinterpret_cast<PHD_3DPOS*>(missile+off::item_pos);
    pos.x_pos=int32_t(std::lround(gun.muzzle.x));
    pos.y_pos=int32_t(std::lround(gun.muzzle.y));
    pos.z_pos=int32_t(std::lround(gun.muzzle.z));
    pos.y_rot=locomotion::Angle(std::atan2(ray.x,ray.z));
    pos.x_rot=locomotion::Angle(std::atan2(-ray.y,flat));
    std::memcpy(missile+off::item_pos_prev,&pos,sizeof(pos));
    if ((weapon==7 && g_boundDll->module[4]==L'2') ||
        (weapon==8 && g_boundDll->module[4]==L'3')) {
        // FireHarpoon derives these two velocities from its launch pitch.
        *reinterpret_cast<int16_t*>(missile+off::item_speed)=
            int16_t(std::lround(150*flat));
        *reinterpret_cast<int16_t*>(missile+36)=
            int16_t(std::lround(150*ray.y));
    } else if (weapon==7 && g_boundDll->module[4]==L'3') {
        // Grenade uses native speed 128 and derives vertical speed from pitch.
        *reinterpret_cast<int16_t*>(missile+36)=
            int16_t(std::lround(128*ray.y));
    }
    if (room!=oldRoom)
        reinterpret_cast<void (__cdecl*)(int16_t,int16_t)>(
            g_boundBase+g_boundDll->itemNewRoom)(index,room);
}

void FireProjectile(hook::InlineHook& hook,int weapon,int object) {
    const auto original=hook.Original<void (__cdecl*)()>();
    if (!g_boundDll || !g_boundBase || !g_boundDll->nextItemFree ||
        !g_boundDll->items || !g_boundDll->itemNewRoom || !MotionReady() ||
        *Ptr<int16_t>(g_boundDll->lara+4)!=weapon) {
        original(); return;
    }
    GunPose gun{};
    const int16_t index=*Ptr<int16_t>(g_boundDll->nextItemFree);
    if (index<0 || !BuildGunPose(1,gun)) { original(); return; }
    original();
    RetargetProjectile(index,weapon,object,gun);
}

void __cdecl Detour_FireHarpoon() {
    FireProjectile(g_hFireHarpoon,g_boundDll && g_boundDll->module[4]==L'2' ? 7 : 8,
                   g_boundDll && g_boundDll->module[4]==L'2' ? 0xF9 : 0x136);
}
void __cdecl Detour_FireRocket() { FireProjectile(g_hFireRocket,6,0x135); }
void __cdecl Detour_FireGrenade() { FireProjectile(g_hFireGrenade,7,0x137); }
void __cdecl Detour_AnimateShotgun(int weapon) {
    const auto original=g_hAnimateShotgun.Original<void (__cdecl*)(int)>();
    if (weapon!=6 || !g_boundDll || g_boundDll->module[4]!=L'2' ||
        !g_boundDll->nextItemFree || !g_boundDll->items ||
        !g_boundDll->itemNewRoom || !MotionReady()) {
        original(weapon); return;
    }
    GunPose gun{};
    const int16_t index=*Ptr<int16_t>(g_boundDll->nextItemFree);
    if (index<0 || !BuildGunPose(1,gun)) { original(weapon); return; }
    original(weapon);
    RetargetProjectile(index,6,0xF8,gun);
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
    const bool stabilize = g_stabilizeRoot && item && item == g_headingItem &&
        CanWalk(item);
    const bool hardStop = g_hardStopRoot && item && item == g_headingItem &&
        CanHardStop(item);
    if (scale == 1 && !stabilize && !hardStop) {
        original(item);
        return;
    }
    auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
    const int32_t oldX = pos.x_pos, oldZ = pos.z_pos;
    original(item); // exactly one skeletal/animation-frame update
    if (hardStop && CanHardStop(item) &&
        std::hypot(double(pos.x_pos)-oldX, double(pos.z_pos)-oldZ) <= 256) {
        pos.x_pos = oldX; pos.z_pos = oldZ;
        *reinterpret_cast<int16_t*>(item + off::item_speed) = 0;
        g_rootMotion.Reset();
        return;
    }
    pos.x_pos = oldX + (pos.x_pos - oldX) * scale;
    pos.z_pos = oldZ + (pos.z_pos - oldZ) * scale;
    auto& speed = *reinterpret_cast<int16_t*>(item + off::item_speed);
    speed = static_cast<int16_t>(std::clamp<int>(speed * scale, -32768, 32767));
    if (stabilize && CanWalk(item)) {
        locomotion::Vec step;
        if (g_rootMotion.Step({float(pos.x_pos-oldX), float(pos.z_pos-oldZ)},
                g_stabilizeDirection,
                *reinterpret_cast<int16_t*>(item + off::item_anim_state),
                g_stabilizeAction, step)) {
            pos.x_pos = oldX + static_cast<int32_t>(step.x);
            pos.z_pos = oldZ + static_cast<int32_t>(step.z);
            speed = static_cast<int16_t>(std::copysign(
                std::round(g_rootMotion.speed), float(speed)));
        }
    } else if (stabilize) g_rootMotion.Reset();
}

void __cdecl Detour_LaraGun() {
    bool injected=false;
    bool observed=false;
    uint32_t beforeInput=0;
    int beforeStatus=-1;
    if (g_active && g_haveHeading && Gate() && g_boundDll &&
        g_boundBase && GameDllBound()==g_boundDll &&
        GameDllBase()==g_boundBase &&
        *Ptr<uint8_t*>(g_boundDll->laraItem)==g_headingItem) {
        auto& input=*Ptr<uint32_t>(g_boundDll->input);
        observed=true;
        beforeInput=input;
        beforeStatus=*Ptr<int16_t>(g_boundDll->lara+off::lara_gun_status);
        if (g_nativeEquipRequested &&
            (beforeStatus==g_nativeEquipStatus ||
             (g_nativeEquipHoldMode &&
              (beforeStatus==0 || beforeStatus==2 || beforeStatus==4))) &&
            !(input&0x20u)) {
            // Animation and collision handlers run between LaraAboveWater's
            // entry and LaraGun. Set the Draw Guns bit at its actual consumer.
            input|=0x20u;
            injected=true;
        }
    }
    g_hLaraGun.Original<Fn_LaraGun>()();
    if (observed && g_boundDll) {
        const int afterStatus=*Ptr<int16_t>(g_boundDll->lara+off::lara_gun_status);
        if (injected && beforeStatus==0 && afterStatus==2)
            g_drawAwaitingLTRelease=true;
        if ((injected && beforeStatus==0) || beforeStatus!=afterStatus ||
            beforeStatus!=g_lastGunTraceStatus)
            LogF("firstperson: LaraGun status %d -> %d input=%08X applied=%08X LT=%u draw-release=%d hp=%d state=%d",
                 beforeStatus,afterStatus,beforeInput,
                 beforeInput|(injected ? 0x20u : 0u),unsigned(g_lastRawLT),
                 int(g_drawAwaitingLTRelease),
                 int(*reinterpret_cast<const int16_t*>(g_headingItem+off::item_hit_points)),
                 int(*reinterpret_cast<const int16_t*>(g_headingItem+off::item_anim_state)));
        g_lastGunTraceStatus=afterStatus;
        if (injected) *Ptr<uint32_t>(g_boundDll->input)&=~0x20u;
    }
}

void __cdecl Detour_LaraAboveWater(uint8_t* item, void* nativeCollision) {
    using namespace locomotion;
    g_dragPrevious = g_dragCurrent;
    g_directionalRootScale = 1;
    g_stabilizeRoot = g_hardStopRoot = false;
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
            g_hardStopRoot = ground && CanHardStop(item) &&
                !g_shifted && !g_jumpPressed &&
                Length(g_manualLocal) <= 0.0001f &&
                !(input & (Directions | 0x10u | 0x100u));
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
                    if (ground && !preparingJump &&
                        Cfg().firstPersonMovementStabilization) {
                        g_stabilizeRoot = true;
                        g_stabilizeDirection = directionalWorld;
                        g_stabilizeAction = action;
                    }
                }
            }
        }
        if (!g_stabilizeRoot) g_rootMotion.Reset();
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
    g_stabilizeRoot = g_hardStopRoot = false;
}

void UpdateLocomotion(PHD_3DPOS& pose) {
    using namespace locomotion;
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    const int state = *reinterpret_cast<const int16_t*>(item + off::item_anim_state);
    const int nativeEyeY = pose.y_pos;
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
        g_rootMotion.Reset();
        const float facing = g_headingItem == item && !relocated
            ? g_lastHeadWorld : Radians(pos.y_rot);
        g_heading.Align(facing, VR().HeadYawRadians());
        g_haveHeading = true;
        g_headingItem = item;
        g_haveManualInput = false;
        g_dragPrevious = g_dragCurrent = g_dragShown = {};
        g_renderTurn.Reset();
        g_groundEye.Reset();
        g_bodyTime = {};
        VR().RecenterHead();
        LogF("locomotion: aligned base=%.1f body=%.1f controls=%s",
             g_heading.base * 180 / Pi, Radians(pos.y_rot) * 180 / Pi,
             NewControls() ? "modern" : "tank");
    }
    g_previousBody = body;
    const float turn = g_renderTurn.Step(TurnTime());
    if (turn != 0) {
        g_heading.Turn(turn);
        VR().PivotHeadFloorOffset(turn);
    }
    if (CanWalk(item)) {
        // A mount can finish with a one-tick root step while the native head
        // joint is already at the new height. Interpolating the old root puts
        // the stabilized eye inside Lara for that transition frame.
        const float nativeHeight=float(nativeEyeY-pos.y_pos);
        const bool mountFinished=g_lastClimbCameraState==19 &&
            nativeHeight>=-950.0f && nativeHeight<=-500.0f;
        const float bodyY = mountFinished ? float(pos.y_pos) :
            static_cast<float>(Lerp(prev.y_pos, pos.y_pos, frac));
        const auto eye = g_groundEye.Apply(
            {body.x, bodyY, body.z}, g_heading.base,
            {float(pose.x_pos), float(pose.y_pos), float(pose.z_pos)});
        pose.x_pos = static_cast<int32_t>(std::lround(eye.x));
        pose.y_pos = static_cast<int32_t>(std::lround(eye.y));
        pose.z_pos = static_cast<int32_t>(std::lround(eye.z));
    }
    if (state != g_lastClimbCameraState &&
        (state == 19 || g_lastClimbCameraState == 19)) {
        LogF("firstperson: pull-up state=%d anim=%d rootY=%d nativeEyeY=%d "
             "viewY=%d standingEye=%d storedHeight=%.0f",
             state, *reinterpret_cast<const int16_t*>(item + off::item_anim_number),
             pos.y_pos, nativeEyeY, pose.y_pos, g_groundEye.valid ? 1 : 0,
             g_groundEye.local.y);
    }
    g_lastClimbCameraState = state;
    if (!CanWalk(item) && !Cfg().firstPersonMovementStabilization) {
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

void ClampRenderedHeadToCollision(const uint8_t* item, PHD_3DPOS& pose) {
    const bool ground = CanWalk(item);
    const int state = *reinterpret_cast<const int16_t*>(item + off::item_anim_state);
    // Forward/standing jump, compression, wall impact, side/back jumps and
    // their falling transitions need a clear eye after ground walking stops.
    const bool jump = LaraWaterStatus() == 0 &&
        *reinterpret_cast<const int16_t*>(item + off::item_hit_points) > 0 &&
        (state == 3 || state == 9 || state == 12 || state == 15 ||
         (state >= 25 && state <= 29));
    // Wall climbing can briefly place the head joint beyond the contact wall.
    // Pull-up and hanging retain their native animated eye and retracted anchor.
    const bool climb=locomotion::IsClimbingCameraState(state);
    if (!ground && !jump && !climb) return;

    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const auto& prev = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos_prev);
    const int frac = std::clamp(*Ptr<int32_t>(g_boundDll->frameFrac), 0, 256);
    const int32_t body[3] = {Lerp(prev.x_pos, pos.x_pos, frac),
                             Lerp(prev.y_pos, pos.y_pos, frac),
                             Lerp(prev.z_pos, pos.z_pos, frac)};

    locomotion::Vec tracked{};
    if (Cfg().positionalTracking && Cfg().firstPersonHeadTranslation) {
        VR().FirstPersonViewOffset(tracked.x, tracked.z);
        tracked = locomotion::Rotate(tracked, g_heading.base) * LiveWorldUnitsPerMetre();
    }
    const int32_t offsetX = static_cast<int32_t>(std::lround(tracked.x));
    const int32_t offsetZ = static_cast<int32_t>(std::lround(tracked.z));
    int32_t renderedHead[3] = {pose.x_pos + offsetX, pose.y_pos,
                               pose.z_pos + offsetZ};
    // AS_SPLAT (12) is a grounded wall-impact reaction, not an airborne fall.
    const bool airborne = !ground && state != 15 && state != 12;
    double eyeY = pose.y_pos;
    if (airborne && Cfg().positionalTracking && Cfg().firstPersonHeadTranslation)
        eyeY -= VR().HeadVerticalOffset() * LiveWorldUnitsPerMetre();
    if (!std::isfinite(eyeY) || std::fabs(eyeY - pose.y_pos) > 4096) return;
    ClampHeadToCollision(item, body, renderedHead, airborne, eyeY);
    // The stereo layer adds tracking after this scene pose. Move the anchor by
    // the same amount in reverse so the actual eye centre stays on the clear
    // side of the wall, even when the player physically leans toward it.
    pose.x_pos = renderedHead[0] - offsetX;
    pose.z_pos = renderedHead[2] - offsetZ;
}

void __cdecl Detour_GenerateW2V(PHD_3DPOS* pose) {
    const void* caller=_ReturnAddress();
    if (pose && IsSceneCall(caller)) {
        const bool wasActive = g_active;
        auto* item = g_boundDll && g_boundBase
            ? *Ptr<uint8_t*>(g_boundDll->laraItem) : nullptr;
        if (Gate() && Anchor(*pose)) {
            g_active = true;
            ++g_anchored;
        } else {
            g_active = false;
            ++g_skipped;
        }
        if (wasActive && !g_active) VR().RecenterHead();
        // Roll visibility belongs only to the active first-person gameplay
        // scene. The title, inventory and other UI scenes reuse this camera
        // path and Lara's last animation state, so evaluating the state before
        // Gate() could leak a zero mesh mask into their visual passes.
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
        if (g_active) {
            UpdateLocomotion(*pose);
            ClampRenderedHeadToCollision(item, *pose);
            g_scenePose=*pose;
            g_scenePoseValid=true;
        }
        else {
            g_scenePoseValid=false;
            g_haveHeading = false;
            g_haveManualInput = false;
            g_rootMotion.Reset();
            g_renderTurn.Reset();
            g_groundEye.Reset();
            g_lastClimbCameraState = -1;
            g_dragPrevious = g_dragCurrent = g_dragShown = {};
            g_neutralTaken = false;
        }
        const int state = item
            ? *reinterpret_cast<const int16_t*>(item + off::item_anim_state) : -1;
        SetMeshVisibility(g_active && Cfg().firstPersonHideHead,
                          g_active && IsRollState(item),
                          g_active && IsCrouchState(item),
                          g_active && locomotion::IsLedgeHangState(state));
        // One line per jump distinguishes missed VR frames from a camera that
        // advances in coarse vertical steps. It also runs in third person.
        struct JumpViewTrace {
            bool live=false;
            int frames=0, repeatedY=0, lateFrames=0, maxStepY=0;
            int lastY=0, level=-1, firstPerson=0;
            uint64_t lastMs=0;
        };
        static JumpViewTrace jumpTrace;
        const bool jumping=state==3 || state==9 || state==15 ||
            (state>=25 && state<=29);
        const int level=AppFlag(drva::app_off::level);
        const uint64_t nowMs=GetTickCount64();
        if (jumpTrace.live && (!jumping || jumpTrace.level!=level ||
                               jumpTrace.firstPerson!=int(g_active))) {
            LogF("firstperson: jump view level=%d first=%d frames=%d "
                 "repeatedY=%d lateFrames=%d maxStepY=%d",
                 jumpTrace.level,jumpTrace.firstPerson,jumpTrace.frames,
                 jumpTrace.repeatedY,jumpTrace.lateFrames,jumpTrace.maxStepY);
            jumpTrace={};
        }
        if (jumping && item && pose) {
            if (!jumpTrace.live) {
                jumpTrace.live=true;
                jumpTrace.level=level;
                jumpTrace.firstPerson=int(g_active);
            } else {
                const int step=std::abs(pose->y_pos-jumpTrace.lastY);
                if (step==0) ++jumpTrace.repeatedY;
                jumpTrace.maxStepY=std::max(jumpTrace.maxStepY,step);
                if (nowMs-jumpTrace.lastMs>28) ++jumpTrace.lateFrames;
            }
            ++jumpTrace.frames;
            jumpTrace.lastY=pose->y_pos;
            jumpTrace.lastMs=nowMs;
        }
    }
    if (pose && g_firingHand>=0 && g_boundDll && g_boundBase &&
        reinterpret_cast<uint64_t>(caller)==
            g_boundBase+g_boundDll->fireW2VReturn) {
        const int16_t spreadYaw=int16_t(pose->y_rot-g_firingBaseAim[0]);
        const int16_t spreadPitch=int16_t(pose->x_rot-g_firingBaseAim[1]);
        pose->x_pos=g_firingPose.x_pos;
        pose->y_pos=g_firingPose.y_pos;
        pose->z_pos=g_firingPose.z_pos;
        pose->y_rot=int16_t(g_firingPose.y_rot+spreadYaw);
        pose->x_rot=int16_t(g_firingPose.x_rot+spreadPitch);
        g_firingDirection=motiongun::ShotForward(
            locomotion::Radians(pose->y_rot),locomotion::Radians(pose->x_rot));
    }
    g_hGenerateW2V.Original<Fn_GenerateW2V>()(pose);
}

bool Install(const GameDllLayout& d, uint64_t base) {
    if (d.phdGenerateW2V == 0 || d.w2vSceneReturn == 0 ||
        d.frameFrac == 0 || d.laraItem == 0 || d.analogInput == 0 ||
        !d.input || !d.laraAboveWater || !d.laraGun || !d.animateLara ||
        !d.getCollisionInfo || !d.updateLaraRoom ||
        !d.calculateLaraMatrices)
        return false;
    const uint8_t movementPrologue[] = {0x48, 0x89, 0x5C, 0x24,
        static_cast<uint8_t>(d.module[4] == L'1' ? 0x08 : d.module[4] == L'2' ? 0x10 : 0x18)};
    if (!g_hLaraAboveWater.Install(reinterpret_cast<void*>(base + d.laraAboveWater),
            reinterpret_cast<void*>(&Detour_LaraAboveWater), 5,
            movementPrologue, sizeof(movementPrologue), "LaraAboveWater")) return false;
    bool gunInstalled=false;
    if (d.module[4]==L'1')
        gunInstalled=g_hLaraGun.Install(reinterpret_cast<void*>(base+d.laraGun),
            reinterpret_cast<void*>(&Detour_LaraGun),6,
            kLaraGunTR1Prologue,sizeof(kLaraGunTR1Prologue),"LaraGun");
    if (d.module[4]==L'2')
        gunInstalled=g_hLaraGun.Install(reinterpret_cast<void*>(base+d.laraGun),
            reinterpret_cast<void*>(&Detour_LaraGun),5,
            kLaraGunTR2Prologue,sizeof(kLaraGunTR2Prologue),"LaraGun");
    if (d.module[4]==L'3')
        gunInstalled=g_hLaraGun.Install(reinterpret_cast<void*>(base+d.laraGun),
            reinterpret_cast<void*>(&Detour_LaraGun),8,
            kLaraGunTR3Prologue,sizeof(kLaraGunTR3Prologue),"LaraGun");
    if (!gunInstalled) return false;
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
    if (d.module[4] == L'1') {
        if (!g_hCalculateLaraMatrices.Install(
                reinterpret_cast<void*>(base + d.calculateLaraMatrices),
                reinterpret_cast<void*>(&Detour_CalculateLaraMatrices), 5,
                kLaraMatricesTR1Prologue, sizeof(kLaraMatricesTR1Prologue),
                "CalculateLaraMatrices")) return false;
    } else {
        if (!g_hCalculateLaraMatrices.Install(
                reinterpret_cast<void*>(base + d.calculateLaraMatrices),
                reinterpret_cast<void*>(&Detour_CalculateLaraMatrices), 5,
                kLaraMatricesTR23Prologue, sizeof(kLaraMatricesTR23Prologue),
                "CalculateLaraMatrices")) return false;
    }
    if (d.drawActionIndicators && d.nActionIndicator && d.actionIndicator &&
        d.phdPersp && d.phdCenterX && d.phdCenterY && d.phdZNear && d.phdZFar &&
        !g_hDrawActionIndicators.Install(
            reinterpret_cast<void*>(base + d.drawActionIndicators),
            reinterpret_cast<void*>(&Detour_DrawActionIndicators), 6,
            kActionIndicatorsPrologue, sizeof(kActionIndicatorsPrologue),
            "DrawActionIndicators"))
        Log("firstperson: Action icon draw hook unavailable");
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
    if (d.getJoints && !g_hGetJoints.Install(
            reinterpret_cast<void*>(base+d.getJoints),
            reinterpret_cast<void*>(&Detour_GetJoints),7,
            kGetJointsPrologue,sizeof(kGetJointsPrologue),"GetJoints"))
        Log("firstperson: tracked-hand joint hook unavailable");
    bool fireInstalled=false;
    if (d.fireWeapon && d.module[4]==L'1')
        fireInstalled=g_hFireWeapon.Install(
            reinterpret_cast<void*>(base+d.fireWeapon),
            reinterpret_cast<void*>(&Detour_FireWeapon),
            6,kFireWeaponTR1Prologue,sizeof(kFireWeaponTR1Prologue),"FireWeapon");
    if (d.fireWeapon && d.module[4]==L'2')
        fireInstalled=g_hFireWeapon.Install(
            reinterpret_cast<void*>(base+d.fireWeapon),
            reinterpret_cast<void*>(&Detour_FireWeapon),
            5,kFireWeaponTR2Prologue,sizeof(kFireWeaponTR2Prologue),"FireWeapon");
    if (d.fireWeapon && d.module[4]==L'3')
        fireInstalled=g_hFireWeapon.Install(
            reinterpret_cast<void*>(base+d.fireWeapon),
            reinterpret_cast<void*>(&Detour_FireWeapon),
            5,kFireWeaponTR3Prologue,sizeof(kFireWeaponTR3Prologue),"FireWeapon");
    if (!fireInstalled)
        Log("firstperson: tracked-gun firing hook unavailable");
    bool flashInstalled=false;
    if (d.drawGunFlash) {
        if (d.module[4]==L'1')
            flashInstalled=g_hDrawGunFlash.Install(
                reinterpret_cast<void*>(base+d.drawGunFlash),
                reinterpret_cast<void*>(&Detour_DrawGunFlash),
                11,kDrawGunFlashTR1Prologue,sizeof(kDrawGunFlashTR1Prologue),
                "DrawGunFlash",kDrawGunFlashTR1RipFixups,1);
        else
            flashInstalled=g_hDrawGunFlash.Install(
                reinterpret_cast<void*>(base+d.drawGunFlash),
                reinterpret_cast<void*>(&Detour_DrawGunFlash),
                10,kDrawGunFlashTR23Prologue,sizeof(kDrawGunFlashTR23Prologue),
                "DrawGunFlash");
    }
    if (!flashInstalled) Log("firstperson: tracked-gun flash hook unavailable");
    bool losInstalled=false;
    if (d.getTargetOnLOS && d.module[4]!=L'3')
        losInstalled=g_hGetTargetOnLOS.Install(
            reinterpret_cast<void*>(base+d.getTargetOnLOS),
            reinterpret_cast<void*>(&Detour_GetTargetOnLOS),
            5,kTargetLOSTR12Prologue,sizeof(kTargetLOSTR12Prologue),"GetTargetOnLOS");
    if (d.getTargetOnLOS && d.module[4]==L'3')
        losInstalled=g_hGetTargetOnLOS.Install(
            reinterpret_cast<void*>(base+d.getTargetOnLOS),
            reinterpret_cast<void*>(&Detour_GetTargetOnLOS),
            5,kTargetLOSTR3Prologue,sizeof(kTargetLOSTR3Prologue),"GetTargetOnLOS");
    if (!losInstalled)
        Log("firstperson: tracked-gun LOS hook unavailable");
    bool harpoonInstalled=false;
    if (d.fireHarpoon && (d.timestamp==0x6A4B4915 ||
                          d.timestamp==0x6A4B490D))
        harpoonInstalled=g_hFireHarpoon.Install(
            reinterpret_cast<void*>(base+d.fireHarpoon),
            reinterpret_cast<void*>(&Detour_FireHarpoon),11,
            kFireHarpoonStockPrologue,sizeof(kFireHarpoonStockPrologue),
            "FireHarpoon",kFireHarpoonStockRipFixups,1);
    else if (d.fireHarpoon)
        harpoonInstalled=g_hFireHarpoon.Install(
            reinterpret_cast<void*>(base+d.fireHarpoon),
            reinterpret_cast<void*>(&Detour_FireHarpoon),11,
            kFireHarpoonRetailPrologue,sizeof(kFireHarpoonRetailPrologue),
            "FireHarpoon",kFireHarpoonRetailRipFixups,1);
    if (d.fireHarpoon && !harpoonInstalled)
        Log("firstperson: tracked harpoon hook unavailable");
    if (d.fireRocket && !g_hFireRocket.Install(
            reinterpret_cast<void*>(base+d.fireRocket),
            reinterpret_cast<void*>(&Detour_FireRocket),10,
            kFireExplosivePrologue,sizeof(kFireExplosivePrologue),"FireRocket"))
        Log("firstperson: tracked rocket hook unavailable");
    if (d.fireGrenade && !g_hFireGrenade.Install(
            reinterpret_cast<void*>(base+d.fireGrenade),
            reinterpret_cast<void*>(&Detour_FireGrenade),10,
            kFireGrenadePrologue,sizeof(kFireGrenadePrologue),"FireGrenade"))
        Log("firstperson: tracked grenade hook unavailable");
    if (d.module[4]==L'2' && d.animateShotgun &&
        !g_hAnimateShotgun.Install(
            reinterpret_cast<void*>(base+d.animateShotgun),
            reinterpret_cast<void*>(&Detour_AnimateShotgun),5,
            kAnimateShotgunPrologue,sizeof(kAnimateShotgunPrologue),
            "AnimateShotgun"))
        Log("firstperson: tracked TR2 grenade hook unavailable");

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
    SetMeshVisibility(false, false); // give her complete mesh back before letting go
    g_calibrationActive=false;
    g_hLaraAboveWater.Remove();
    g_hLaraGun.Remove();
    g_hAnimateLara.Remove();
    g_hCalculateLaraMatrices.Remove();
    g_hDrawActionIndicators.Remove();
    g_hDrawHair.Remove();
    g_hDrawCreatureHD.Remove();
    g_hGetJoints.Remove();
    g_hGetTargetOnLOS.Remove();
    g_hFireHarpoon.Remove();
    g_hFireRocket.Remove();
    g_hFireGrenade.Remove();
    g_hAnimateShotgun.Remove();
    g_hFireWeapon.Remove();
    g_hDrawGunFlash.Remove();
    g_hGenerateW2V.Remove();
    g_active     = false;
    g_scenePoseValid=false;
    g_renderArm=g_firingHand=-1;
    g_gunTriggers.Reset();
    g_gunEquip.Reset();
    g_triggerWeapon=0;
    g_nativeEquipRequested=false;
    g_nativeEquipStatus=-1;
    g_nativeEquipHoldMode=false;
    g_drawAwaitingLTRelease=false;
    g_lastRawLT=0;
    g_lastGunTraceStatus=-1;
    g_haveHeading = false;
    g_headingItem = nullptr;
    g_haveManualInput = false;
    g_directionalRootScale = 1;
    g_rootMotion.Reset();
    g_dragPrevious = g_dragCurrent = g_dragShown = {};
    g_neutralTaken = false;
    g_boundDll   = nullptr;
    g_boundBase  = 0;
}

} // namespace

void FirstPersonUpdate() {
    PollMotionGunCalibration();
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
    g_calibrationActive=false;
    if (!g_runtimeInitialized) {
        g_runtimeEnabled = false;
        g_runtimeInitialized = true;
    }

    const bool wasActive = g_active;
    g_runtimeEnabled = !g_runtimeEnabled;
    // A view change starts with a clean heading and roomscale neutral. This is
    // also the immediate stand-down path: no simulation tick between the chord
    // and the next camera draw can inherit first-person steering.
    SetMeshVisibility(false, false);
    g_active = false;
    g_scenePoseValid=false;
    g_renderArm=g_firingHand=-1;
    g_gunTriggers.Reset();
    g_gunEquip.Reset();
    g_triggerWeapon=0;
    g_nativeEquipRequested=false;
    g_nativeEquipStatus=-1;
    if (wasActive && !g_runtimeEnabled) VR().RecenterHead();
    g_haveHeading = false;
    g_headingItem = nullptr;
    g_haveManualInput = false;
    g_shifted = false;
    g_jumpPressed = false;
    g_directionalRootScale = 1;
    g_rootMotion.Reset();
    g_dragPrevious = g_dragCurrent = g_dragShown = {};
    g_renderTurn.Reset();
    g_groundEye.Reset();
    g_bodyTime = {};
    g_neutralTaken = false;
    LogF("firstperson: Y+LT switched to %s",
         g_runtimeEnabled ? "FIRST PERSON" : "third person");
}

void FirstPersonShutdown() {
    g_calibrationActive=false;
    if (g_calibrationMessageHook) UnhookWindowsHookEx(g_calibrationMessageHook);
    g_calibrationMessageHook=nullptr;
    g_calibrationWindow=nullptr;
    g_calibrationKeys={};
    g_calibrationCommandCount=0;
    if (g_boundDll) Remove();
    g_loggedFirst = false;
    g_loggedVisibleGunAim = false;
    g_loggedActionIcon = false;
    g_loggedMotion = false;
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
const float* FirstPersonHandSkin(int& wristJoint) {
    wristJoint=g_renderWrist;
    return g_renderArm>=0 && g_renderWrist>=0 ? g_handSkinPalette : nullptr;
}
bool FirstPersonWristCentre(float out[3]) {
    if (g_renderArm<0 || g_renderWrist<0) return false;
    out[0]=g_handSkinCentre.x; out[1]=g_handSkinCentre.y; out[2]=g_handSkinCentre.z;
    return true;
}
bool FirstPersonCalibrationKeyReserved(int key) {
    if (!g_calibrationMessageHook || key<VK_F1 || key>VK_F7) return false;
    if (g_calibrationKeys.captured[key-VK_F1]) return true;
    return g_calibrationActive && CalibrationFocused() &&
        (GetAsyncKeyState(VK_CONTROL)&0x8000) &&
        !(GetAsyncKeyState(VK_MENU)&0x8000);
}
bool FirstPersonSceneEye(int32_t out[3]) {
    if (!out || !g_active || !g_scenePoseValid) return false;
    out[0]=g_scenePose.x_pos;
    out[1]=g_scenePose.y_pos;
    out[2]=g_scenePose.z_pos;
    return true;
}

void FirstPersonGunTriggers(uint8_t& left,uint8_t& right,bool chordConsumed) {
    g_lastRawLT=left;
    g_nativeEquipRequested=false;
    g_nativeEquipStatus=-1;
    g_nativeEquipHoldMode=false;
    DWORD process=0;
    GetWindowThreadProcessId(GetForegroundWindow(),&process);
    // A SteamVR game can keep receiving controller input while its desktop
    // mirror is not the foreground Win32 window. Native LaraGun still consumes
    // LT then; gating this adapter on GetForegroundWindow would drop the
    // persistent modern-controls draw bit and holster on LT release.
    const bool enabled=motiongun::VrGunInputEnabled(
        chordConsumed,MotionTriggerMode(),process==GetCurrentProcessId());
    const uint64_t now=GetTickCount64();
    static uint64_t nextMotionLog=0;
    if (Cfg().firstPersonMotionGuns && g_active && g_boundDll &&
        now>=nextMotionLog) {
        nextMotionLog=now+5000;
        const auto* lara=Ptr<uint8_t>(g_boundDll->lara);
        vr::HmdMatrix34_t pose{};
        LogF("firstperson: motion state enabled=%d blocked=%s status=%d gun=%d last=%d HD=%d controllers=%d/%d foreground=%d LT=%u RT=%u",
            int(enabled),MotionBlockedReason() ? MotionBlockedReason() : "none",
            int(*reinterpret_cast<const int16_t*>(lara+off::lara_gun_status)),
            int(*reinterpret_cast<const int16_t*>(lara+4)),
            int(*reinterpret_cast<const int16_t*>(lara+8)),
            int(AppFlag(drva::app_off::cfg_flags)&1),
            int(VR().ControllerPose(0,pose)),int(VR().ControllerPose(1,pose)),
            int(process==GetCurrentProcessId()),unsigned(left),unsigned(right));
        LogF("firstperson: tracked hands mask=%08X passes=%u/%u joints=%u/%u corrected=%u/%u reason=%s/%s",
             g_motionLastMask,g_motionHandPasses[0],g_motionHandPasses[1],
             g_motionJointCalls[0],g_motionJointCalls[1],
             g_motionCorrections[0],g_motionCorrections[1],
             g_motionPoseFailure[0],g_motionPoseFailure[1]);
        LogF("firstperson: trigger state pending=%d/%d dual=%d long=%d wait=%d/%d",
             int(g_gunTriggers.pending[0]),int(g_gunTriggers.pending[1]),
             int(g_gunTriggers.dualFireGesture),int(g_gunTriggers.longFired),
             int(g_gunTriggers.leftWaitRelease),int(g_gunTriggers.rightWaitRelease));
        g_motionHandPasses[0]=g_motionHandPasses[1]=0;
        g_motionJointCalls[0]=g_motionJointCalls[1]=0;
        g_motionCorrections[0]=g_motionCorrections[1]=0;
    }
    if (!enabled) {
        g_gunTriggers.Update(false,false,false,false,now);
        g_gunEquip.Reset();
        g_drawAwaitingLTRelease=false;
        g_triggerWeapon=0;
        return;
    }
    int weapon=*Ptr<int16_t>(g_boundDll->lara+4);
    if (!weapon) weapon=*Ptr<int16_t>(g_boundDll->lara+8);
    if (weapon!=g_triggerWeapon) {
        if (MotionWeaponSupported(g_triggerWeapon) &&
            MotionWeaponSupported(weapon)) {
            // A native weapon change during drawing must retain LT's armed
            // intent, while queued shots stay with their original weapon.
            g_gunTriggers.pending[0]=g_gunTriggers.pending[1]=false;
            g_gunTriggers.leftCanTap=false;
        } else {
            g_gunTriggers.Reset();
            g_gunEquip.Reset();
        }
        g_triggerWeapon=weapon;
    }
    const int status=*Ptr<int16_t>(g_boundDll->lara+off::lara_gun_status);
    const uint8_t nativeLeft=left;
    if (g_drawAwaitingLTRelease && nativeLeft<=30) {
        g_drawAwaitingLTRelease=false;
        g_gunTriggers.Reset();
    }
    g_gunTriggers.Update(true,MotionReady(),left>30,right>30,now);
    if (!DualMotionWeapon(weapon)) g_gunTriggers.pending[0]=false;
    const bool holdMode=NewControls() &&
        AppFlag(drva::app_off::level_type)!=3;
    bool equipRequest=false;
    if (status==0) {
        // First hold from holstered draws immediately. In modern controls the
        // native draw bit must then remain asserted even after LT is released.
        equipRequest=nativeLeft>30 &&
            !g_gunTriggers.leftWaitRelease && !g_drawAwaitingLTRelease;
    } else if (status==2) {
        // Require an LT release before the next gesture can holster or fire.
        g_gunTriggers.Reset();
        g_gunTriggers.Update(true,false,nativeLeft>30,right>30,now);
    } else if (status==4) {
        equipRequest=!g_drawAwaitingLTRelease && g_gunTriggers.Equip(now);
    }
    if (equipRequest && !g_gunEquip.requestHeld)
        LogF("firstperson: LT equip gesture native=%s status=%d gun=%d",
             holdMode ? "hold" : "toggle",status,weapon);
    g_nativeEquipRequested=g_gunEquip.Update(holdMode,status,equipRequest);
    g_nativeEquipStatus=status;
    g_nativeEquipHoldMode=holdMode;
    left=0;
    // Ready dual guns use independent LT-release and held-RT requests. Native
    // LaraGun still sets the rate; long guns keep their sustained RT path.
    if (status==4 && weapon<=3)
        right=g_gunTriggers.WantsShot() ? 255 : 0;
}

void FirstPersonInput(float& leftX, float& leftY, float& rightX, bool shifted,
                      bool jumpPressed) {
    using namespace locomotion;
    g_haveManualInput = false;
    g_shifted = shifted;
    g_jumpPressed = jumpPressed;
    if (!g_active || !g_haveHeading || !Gate()) {
        g_renderTurn.Reset();
        return;
    }
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item || item != g_headingItem) { g_renderTurn.Reset(); return; }
    const float dead = std::clamp(Cfg().firstPersonTurnDeadzone, 0.0f, 0.95f);
    const float magnitude = std::min(1.0f, std::fabs(rightX));
    float turnRate = 0;
    if (magnitude > dead) {
        const float strength = (magnitude - dead) / (1.0f - dead);
        turnRate = std::copysign(strength, rightX) *
            std::clamp(Cfg().firstPersonTurnDegreesPerSecond, 0.0f, 720.0f) *
            (Pi / 180.0f);
    }
    const float headWorld = g_heading.World(VR().HeadYawRadians());
    g_lastHeadWorld = headWorld;
    const int state = *reinterpret_cast<const int16_t*>(item + off::item_anim_state);
    const bool ground = CanWalk(item);
    const bool jump = IsJumpSteeringState(state) && LaraWaterStatus() == 0
        && *reinterpret_cast<const int16_t*>(item + off::item_hit_points) > 0;
    if (!ground && !jump) { g_renderTurn.Reset(); return; }
    g_renderTurn.Sample(turnRate, TurnTime());
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
