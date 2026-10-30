#include "FirstPerson.h"
#include "Gamepad.h"
#include "InventoryScene.h"
#include "HKScopeMath.h"
#include "Config.h"
#include "Engine.h"
#include "GameDll.h"
#include "InlineHook.h"
#include "Log.h"
#include "VRSystem.h"
#include "LocomotionMath.h"
#include "FirstPersonClearance.h"
#include "FirstPersonVisibility.h"
#include "FirstPersonStabilization.h"
#include "MotionGunMath.h"
#include "WeaponLaserMesh.h"
#include "FirstPersonIK.h"
#include "FirstPersonBodyIK.h"
#include "BodyFollower.h"
#include "JumpLanding.h"
#include "MotionGunInput.h"
#include "PhysicalHolster.h"
#include "PistolAnimationBlend.h"
#include "LedgePullGesture.h"
#include "SwitchPosePath.h"
#include "LedgeGrabFeedback.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace tr {
namespace {

struct PHD_3DPOS {
    int32_t x_pos, y_pos, z_pos;
    int16_t x_rot, y_rot, z_rot;
    int16_t padding;
};

struct PHD_VECTOR { int32_t x, y, z; };
struct ShotVector { int32_t x, y, z; int16_t room, padding; };
static_assert(sizeof(ShotVector)==16 && offsetof(ShotVector,room)==12);
static_assert(sizeof(PHD_3DPOS) == 20);
static_assert(sizeof(PHD_VECTOR) == 12);

namespace off {
constexpr uint32_t item_stride = 9416;
constexpr uint32_t item_next_active = 32;
constexpr uint32_t lara_target = 208;
constexpr uint32_t item_mesh_bits   = 12;
constexpr uint32_t item_floor       = 0;
constexpr uint32_t item_floor_prev  = 4;
constexpr uint32_t item_object      = 16;
constexpr uint32_t item_anim_state  = 18;
constexpr uint32_t item_goal_state  = 20;
constexpr uint32_t item_anim_number = 24;
constexpr uint32_t item_speed       = 34;
constexpr uint32_t item_hit_points  = 38;
constexpr uint32_t item_room        = 28;
constexpr uint32_t item_pos         = 96;
constexpr uint32_t item_pos_prev    = 6208;
constexpr uint32_t arm_lock         = 12;
constexpr uint32_t arm_y_rot        = 14;
constexpr uint32_t arm_x_rot        = 16;
constexpr uint32_t arm_z_rot        = 18;
constexpr uint32_t lara_left_arm    = 240;
constexpr uint32_t lara_right_arm   = 264;
constexpr uint32_t lara_head_y_rot  = 224;
constexpr uint32_t lara_torso_z_rot = 234;
constexpr uint32_t lara_turn_rate   = 220;
constexpr uint32_t lara_move_angle  = 222;
constexpr uint32_t lara_vehicle     = 38;
constexpr uint32_t lara_movement_flags = 68; // IsMoving is bit 5; CanMonkeySwing is bit 6.
constexpr uint32_t lara_skelebob = 350; // uint8: native TR5 X-ray rendering mode.
constexpr uint32_t camera_type      = 32;
constexpr uint32_t object_stride    = 3792;
constexpr uint32_t object_geom      = 112;
constexpr uint32_t geom_stride      = 120;
constexpr uint32_t geom_mesh        = 16;
} // namespace off

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kHeadMeshBit = 1u << 14;
constexpr uint32_t kArmMeshBits = 0x3f00; // upper arm, forearm, hand on both sides
constexpr int kLaraHeadGeoms = 4;
constexpr int32_t kCamFixed = 1;
constexpr int32_t kCamCinematic = 4;

using Fn_GenerateW2V = void (__cdecl*)(PHD_3DPOS*);
using Fn_DrawCreatureHD = void (__cdecl*)(uint8_t*, int32_t, int32_t);
using Fn_DrawHair = void (__cdecl*)(int32_t);
using Fn_GetJointAbsPositionLerp = void (__cdecl*)(uint8_t*, PHD_VECTOR*, int32_t, int32_t);
using Fn_AimWeapon = void (__cdecl*)(void*, uint8_t*);
using Fn_GetJoints = int32_t (__cdecl*)(uint8_t*, float*, int32_t);
using Fn_FireWeapon = int32_t (__cdecl*)(int32_t, void*, void*, const int16_t*);
using Fn_SetGunFlash = void (__cdecl*)(int32_t, int32_t);
using Fn_PistolHandler = void (__cdecl*)(int32_t);
using Fn_GetTargetOnLOS = int32_t (__cdecl*)(
    PHD_VECTOR*, PHD_VECTOR*, int32_t, int32_t);
using Fn_LaraAboveWater = void (__cdecl*)(uint8_t*, void*);
using Fn_AnimateLara = void (__cdecl*)(uint8_t*);

hook::InlineHook g_hGenerateW2V;
hook::InlineHook g_hDrawCreatureHD;
hook::InlineHook g_hDrawHair;
hook::InlineHook g_hAimWeapon;
hook::InlineHook g_hGetJoints;
hook::InlineHook g_hDrawToShadow;
struct ShadowDll { uint32_t timestamp, draw, torsoReturn, renderPass, viewRel, cast; };
constexpr ShadowDll kShadowDlls[] = {
    {0x696B4999,0xB8A00,0xB8AF0,0x4F2DD0,0x4948A0,0xC7A80},
    {0x696B499C,0xAC310,0xAC400,0x4EE720,0x4EF930,0xBC6C0},
    {0x68C12FDA,0xB9480,0xB9572,0x4F3D10,0x4957E0,0xC8750},
    {0x68C12FE9,0xAC510,0xAC602,0x4EE660,0x4EF870,0xBCAB0},
};
const ShadowDll* g_shadowDll=nullptr;
hook::InlineHook g_hFireWeapon;
hook::InlineHook g_hPistolHandler;
hook::InlineHook g_hAnimatePistols;
hook::InlineHook g_hSetGunFlash;
hook::InlineHook g_hGetTargetOnLOS;
hook::InlineHook g_hLaraAboveWater;
hook::InlineHook g_hGetCollisionInfo;
hook::InlineHook g_hAnimateLara;
hook::InlineHook g_hDrawActionIndicators;
struct ActionIconDll {
    uint32_t timestamp, draw, points, count, matrix, perspective, centerX, centerY, nearZ, farZ;
};
constexpr ActionIconDll kActionIconDlls[] = {
    {0x696B4999,0xD3390,0x52CCC0,0x6601BC,0x494800,0x4947E4,0x4947DC,0x4947E0,0x494748,0x494740},
    {0x696B499C,0xCAA10,0x529F40,0x65AC9C,0x4EF900,0x4EF8E0,0x4EF8D8,0x4EF8DC,0x4EF880,0x4EE934},
    {0x68C12FDA,0xD3EA0,0x52DC00,0x6610FC,0x495740,0x495724,0x49571C,0x495720,0x495688,0x495680},
    {0x68C12FE9,0xCAC70,0x529E80,0x65ABDC,0x4EF840,0x4EF820,0x4EF818,0x4EF81C,0x4EF7C0,0x4EE874},
};
const ActionIconDll* g_actionIconDll = nullptr;
bool g_loggedActionIcon = false;
const GameDllLayout* g_boundDll = nullptr;
uint64_t g_boundBase = 0;
uint64_t g_failedBase = 0;

bool g_runtimeEnabled = false;
bool g_runtimeInitialized = false;
bool g_active = false;
bool g_haveHeading = false;
float g_headingBase = 0.0f;
bool g_controlModeOwned=false;
uint32_t g_savedControlMode=0;

void UpdateControlMode(bool active) {
    if(!Base()) return;
    constexpr uint32_t modernControls=2u;
    auto& flags=*reinterpret_cast<uint32_t*>(Base()+L().app+drva::app_off::cfgFlags);
    if(active) {
        if(!g_controlModeOwned) {
            g_savedControlMode=flags&modernControls;
            g_controlModeOwned=true;
        }
        flags|=modernControls;
    } else if(g_controlModeOwned) {
        flags=(flags&~modernControls)|g_savedControlMode;
        g_controlModeOwned=false;
    }
}
float g_lastHeadWorld = 0.0f;
stabilization::RenderTurn g_renderTurn;
struct TurnTrace { double start=0; unsigned polls=0,views=0,moved=0; float maxStep=0; };
TurnTrace g_turnTrace;
LARGE_INTEGER g_bodyTime{};
uint8_t* g_headingItem = nullptr;
locomotion::Vec g_previousBody{};
locomotion::Vec g_dragPrevious{}, g_dragCurrent{}, g_dragShown{};
locomotion::Vec g_manualLocal{}, g_manualWorld{};
bool g_haveManualInput = false;
bool g_shifted = false;
bool g_jumpPressed = false;
struct GroundJumpIntent {
    bool held=false, pending=false;
    bool edgeWaiting=false, edgeBlocked=false;
    double until=0;
    const uint8_t* owner=nullptr;
    int level=-1;
    void Reset() { *this={}; }
    void Cancel() { pending=false; edgeBlocked|=edgeWaiting; edgeWaiting=false; }
    void Bind(const uint8_t* item,int currentLevel) {
        if (owner!=item || level!=currentLevel) Cancel();
        owner=item;level=currentLevel;
    }
    void Observe(bool pressed,bool eligible,double now) {
        if (!eligible) Cancel();
        else if (pressed && !held) { pending=true; until=now+.25; edgeBlocked=false; }
        if (!pressed) {
            // A quick release is a manual jump; releasing a long hold cancels.
            if (edgeWaiting) { pending=eligible && now<=until; edgeWaiting=false; }
            edgeBlocked=false;
        }
        held=pressed;
    }
    bool Wants(double now) {
        if (now>until) pending=false;
        return pending;
    }
} g_groundJump;
uint64_t g_groundMoveAction = 0; // Scoped stick intent, not the active native gait.
bool g_crouchDrive=false;
float g_crouchTravelYaw=0;
bool g_stabilizeRoot=false;
bool g_hardStopRoot=false; // Scoped to one ordinary above-water simulation tick.
uint64_t g_stabilizeAction=0;
locomotion::Vec g_stabilizeDirection{};
firstperson::LedgePullGesture g_ledgePull;
firstperson::LedgeGrabFeedback g_ledgeGrab;
stabilization::RootMotion g_rootMotion;
stabilization::GroundEye g_groundEye;
stabilization::StanceEye g_stanceEye;
stabilization::MountBodyTransition g_mountBodyTransition;
locomotion::Vec g_bodyVisualOffset{};
locomotion::Vec g_bodyWallOffset{};
locomotion::Vec g_ladderTorsoOffset{};
float g_lastBodyYaw=0;
int g_headingLevel=-1;
bool g_bodySkinScope=false, g_bodySkinReady=false;
bool g_bodyWristSeamScope=false;
uint64_t g_bodyWristSeamMask=0;
unsigned g_armIKSolved=0, g_armIKFailed=0;
float g_bodySkinPalette[33*12]{};
float g_bodyRenderPalette[33*12]{};
firstperson::ArmTwistState g_unarmedTwist[2]{};
uint64_t g_bodySkinMask=0;
int g_bodySkinCount=0;
// One body solve owns both sides of the pistol elbow seam. Weapon attachments
// have their own animated palettes and may omit shoulders/torso entirely.
struct PistolArmPose {
    bool valid=false;
    bool bindReference=false;
    const uint8_t* item=nullptr;
    int game=-1,pass=0,fraction=0;
    uint64_t serial=0;
    motiongun::Frame joint[6]{};
} g_pistolArmPose;
bool g_pistolBodyScope=false,g_pistolForearmScope=false;
bool g_pistolArmSolveReady=false;
struct PistolRollHistory {
    bool valid=false;
    const uint8_t* item=nullptr;
    uint64_t serial=0;
    firstperson::ArmRollContinuity previous[2]{},current[2]{};
    float minConfidence[2]{1,1};
} g_pistolRoll;

firstperson::ArmRollContinuity PistolRollSeed(int hand,const uint8_t* item) {
    if(!g_pistolRoll.valid || g_pistolRoll.item!=item) {
        g_pistolRoll={};g_pistolRoll.valid=true;g_pistolRoll.item=item;
        g_pistolRoll.serial=VR().PoseSerial();
    } else if(g_pistolRoll.serial!=VR().PoseSerial()) {
        for(int i=0;i<2;++i) g_pistolRoll.previous[i]=g_pistolRoll.current[i];
        g_pistolRoll.serial=VR().PoseSerial();
    }
    return g_pistolRoll.previous[hand];
}
firstperson::BodyFollower g_bodyFollower;
firstperson::BodySizeCalibration g_bodySize;
firstperson::TorsoPoseFilter g_torsoPose;
firstperson::GroundGaitReference g_groundGait;
int g_groundGaitObject=-1;
firstperson::JumpLanding g_jumpLanding;
firstperson::GroundGaitReference g_jumpBodyReference;
int g_jumpBodyObject=-1;
bool g_bodyLanding=false;
firstperson::PistolAnimationBlend g_pistolAnimation;
firstperson::BodyVisibilityRecovery g_bodyVisibility;
firstperson::AutoBodyCalibration g_autoBodyCalibration;
int g_bodySizeObject=-1;
firstperson::RoomscaleFollower g_roomscaleFollower;
firstperson::HeightCrouch g_heightCrouch;
LARGE_INTEGER g_roomscaleFollowTime{};
bool g_physicalCrouchCamera=false;

firstperson::BodyFollowSettings BodyFollowSettings() {
    firstperson::BodyFollowSettings s;
    s.yawSlack=Cfg().firstPersonBodyYawSlackDegrees*.01745329252f;
    s.leanRadius=Cfg().firstPersonBodyLeanRadiusMetres;s.leanRelease=s.leanRadius*.6f;
    s.followSpeed=Cfg().firstPersonBodyFollowSpeedMetres;return s;
}
void ResetBodyFollower() {
    g_pistolRoll={};
    g_pistolArmPose.valid=false;
    g_bodyFollower.Reset();g_roomscaleFollower={};g_roomscaleFollowTime={};
    g_bodySize.Reset();g_bodySizeObject=-1;
    g_torsoPose.Reset();
    g_groundGait.Reset();g_groundGaitObject=-1;
    g_jumpLanding.Reset();g_jumpBodyReference.Reset();g_jumpBodyObject=-1;
    g_bodyLanding=false;
    g_pistolAnimation.Reset();
    g_heightCrouch={};g_physicalCrouchCamera=false;
}

void ResetMovementStabilization() {
    ResetBodyFollower();
    g_bodyVisibility={};
    g_crouchDrive=false;
    g_groundJump.Reset();
    g_ledgePull.Reset(); g_ledgeGrab.Reset();
    g_renderTurn.Reset();
    g_turnTrace={};
    g_stabilizeRoot=false;
    g_hardStopRoot=false;
    g_groundMoveAction=0;
    g_rootMotion.Reset();
    g_groundEye.Reset();
    g_stanceEye.Reset();
    g_mountBodyTransition.Reset();
    g_bodyVisualOffset={};
    g_bodyWallOffset={};
    g_ladderTorsoOffset={};
}

bool g_meshOverride = false;
bool g_headHidden = false;
bool g_rollHidden = false;
bool g_crouchHidden = false;
bool g_underwaterHidden = false;
bool g_ledgeArmsOnly = false;
bool g_unarmedArmsHidden = false;
firstperson::UnarmedArmVisibility g_unarmedArmVisibility;
uint8_t* g_meshItem = nullptr;
uint32_t g_meshBaseBits = 0;
unsigned g_anchored = 0;
unsigned g_skipped = 0;
unsigned g_faceSkips = 0;
unsigned g_hairSkips = 0;
bool g_loggedFirst = false;
bool g_loggedLost = false;

// PDB and retail call sites were checked against the four supported DLLs.
// The two AnimatePistols return addresses distinguish the actual firing hand.
struct MotionDll {
    uint32_t timestamp, getJoints, fireWeapon, fireViewReturn;
    uint32_t rightFireReturn, leftFireReturn, app, pistolHandler;
    uint32_t getTargetOnLOS, hitLosReturn, missLosReturn;
    uint32_t setGunFlash, floatMatrixPtr, rightFlashReturn, leftFlashReturn;
    uint32_t findTargetPoint, lineOfSight;
    uint32_t animatePistols, getSpheres, nextItemActive, items;
};
constexpr MotionDll kMotionDlls[] = {
    {0x696B4999, 0xC09E0, 0x5E4B0, 0x5E5C4, 0x5A97F, 0x5AC2A, 0x663F08, 0x5A270,
     0x15860, 0x5E701, 0x5E79B, 0x8EDD0, 0x1B6D18, 0x2F763, 0x2F819, 0x5E2F0, 0x149F0, 0x5A680, 0xA3C30, 0x63DAB0, 0x699CA0},
    {0x696B499C, 0xB5880, 0x5B790, 0x5B8A4, 0x57CBF, 0x57F6A, 0x66D1A0, 0x576A0,
     0x132F0, 0x5BA0C, 0x5BAA3, 0x8D790, 0x1B2AC8, 0x306A3, 0x30756, 0x5B5D0, 0x12450, 0x579C0, 0x9CF90, 0x65AC92, 0x66D1A8},
    {0x68C12FDA, 0xC1440, 0x5E1B0, 0x5E2C6, 0x5A67F, 0x5A92A, 0x664E48, 0x59F70,
     0x15440, 0x5E401, 0x5E49B, 0x8F7E0, 0x1B7D18, 0x2F5D3, 0x2F689, 0x5DFF0, 0x145D0, 0x5A380, 0xA47D0, 0x63E9F0, 0x69ABE0},
    {0x68C12FE9, 0xB5A00, 0x5C4E0, 0x5C5F6, 0x58A0F, 0x58CBA, 0x66D0E0, 0x583F0,
     0x133A0, 0x5C759, 0x5C7F0, 0x8DAA0, 0x1B2AC8, 0x30B23, 0x30BD6, 0x5C320, 0x12500, 0x58710, 0x9D300, 0x65ABD2, 0x66D0E8},
};
const MotionDll* g_motionDll = nullptr;
struct CrouchGunDll { uint32_t timestamp, gun; uint8_t prologue[11]; };
constexpr CrouchGunDll kCrouchGunDlls[] = {
    {0x696B4999,0x5CF80,{0x48,0x83,0xEC,0x28,0x0F,0xB7,0x05,0xB9,0x5F,0x49,0x00}},
    {0x696B499C,0x5A2A0,{0x48,0x83,0xEC,0x28,0x0F,0xB7,0x05,0x99,0x45,0x49,0x00}},
    {0x68C12FDA,0x5CC80,{0x48,0x83,0xEC,0x28,0x0F,0xB7,0x05,0xF9,0x71,0x49,0x00}},
    {0x68C12FE9,0x5AFF0,{0x48,0x83,0xEC,0x28,0x0F,0xB7,0x05,0x89,0x37,0x49,0x00}},
};
// Outer DrawLara__HD owns the camera-inside-body rejection, before the
// existing DrawLaraHD jiggle scope or any per-hand DrawCreatureHD passes.
struct CrouchDrawDll { uint32_t timestamp, draw; };
constexpr CrouchDrawDll kCrouchDrawDlls[] = {
    {0x696B4999,0x1AE60}, {0x696B499C,0x17360},
    {0x68C12FDA,0x1AB40}, {0x68C12FE9,0x17490},
};
hook::InlineHook g_hCrouchDraw;
hook::InlineHook g_hCrouchGun;
bool g_crouchGunsReady=false;

// DrawLaraHD selects HAND_*_RUN (2/32/62) independently of the joint pose.
// Verified against both PDB and retail renderers and HAND_NAMES in the PDBs.
struct UnarmedHandDll { uint32_t timestamp, hands; };
constexpr UnarmedHandDll kUnarmedHandDlls[] = {
    {0x696B4999,0x52F3C0}, {0x696B499C,0x52C640},
    {0x68C12FDA,0x530300}, {0x68C12FE9,0x52C580},
};
const UnarmedHandDll* g_unarmedHandDll=nullptr;

// Separate, optional extension: a mismatch must not disable working dual guns.
struct LongGunDll {
    uint32_t timestamp, rifle, shotgun, special, crossbow, joint, initialise, items;
    uint32_t opticReturns[4];
};
constexpr LongGunDll kLongGunDlls[] = {
    {0x696B4999,0x55ED0,0x56420,0x56A70,0x57D30,0xA37C0,0x40840,0x699CA0,
     {0xE347F,0xE34C9,0xE3F6A,0xE3F88}},
    {0x696B499C,0x54D40,0x55150,0x557A0,0x558C0,0x9C9C0,0x45800,0x66D1A8,
     {0xD7A1F,0xD7A69,0xD84FA,0xD8518}},
    {0x68C12FDA,0x55BE0,0x56130,0x56770,0x57A20,0xA4360,0x40300,0x69ABE0,
     {0xE3F9F,0xE3FE9,0xE4A9E,0xE4ABC}},
    {0x68C12FE9,0x55A70,0x55E80,0x564C0,0x565E0,0x9CD30,0x46160,0x66D0E8,
     {0xD7C9F,0xD7CE9,0xD878E,0xD87AC}},
};
const LongGunDll* g_longGunDll=nullptr;
bool g_longGunHooksReady=false, g_opticShot=false;
int g_triggerWeapon=0;
hook::InlineHook g_hRifleHandler, g_hFireShotgun, g_hFireSpecial, g_hFireCrossbow;
hook::InlineHook g_hGunJoint, g_hInitialiseProjectile;
struct FlareDll { uint32_t timestamp, inHand, previousJoint, triggerDynamic; };
constexpr FlareDll kFlareDlls[]={
    {0x696B4999,0x5F7F0,0xA3920,0xAD150},{0x696B499C,0x5C9D0,0x9CB20,0xD0890},
    {0x68C12FDA,0x5F500,0xA44C0,0xADD00},{0x68C12FE9,0x5D700,0x9CE90,0xD0B30}
};
hook::InlineHook g_hFlareInHand,g_hFlarePreviousJoint,g_hFlareDynamic;
bool g_flareHooksReady=false;
// Native DYNAMIC: interleaved current/previous XYZ followed by light settings.
// Keep the original light, including burn/flicker, and move only its position.
struct FlareLightSlot { uint8_t* address=nullptr; uint8_t snapshot[40]{}; };
struct FlareLights {
    FlareLightSlot slots[2]{}; // TR4 may also emit a Z-mirrored copy.
    int count=0,level=-1;
    const uint8_t* item=nullptr;
    double time=0;
    float mirrorZSum=0;
} g_flareLights;
bool g_captureFlareLights=false;
bool g_loggedFlareLight=false;
struct FlareHandFrames { motiongun::Frame current{},previous{}; };
const FlareHandFrames* g_flareHandFrames=nullptr;
struct FlareHandHistory {
    const uint8_t* item=nullptr;
    int level=-1,age=-1;
    double time=0;
    FlareHandFrames frames{};
} g_flareHandHistory;
bool g_motionHooksReady = false;
bool g_scenePoseValid = false;
PHD_3DPOS g_scenePose{};
int g_renderArm = -1; // 0=left, 1=right
int g_renderHandJoint = -1;
// DrawLaraHD overwrites objects[item].geom for each body/hand/face pass.
// Only the right-HK GetJoints scope owns the correct immutable wrist binding.
// Cache that binding, never a world/controller pose (which would add lag).
motiongun::Frame g_hkScopeInverseBind{};
const uint8_t* g_hkScopeBindItem=nullptr;
struct WeaponLaserPose {
    const uint8_t* item=nullptr;
    uint64_t base=0,serial=0;
    int game=-1,weapon=-1;
    motiongun::Vec origin{},direction{};
} g_weaponLaser;
// Firing runs before drawing. Retain the mesh-to-controller calibration,
// never last frame's world ray, and apply it to the live controller pose.
struct WeaponAimBinding {
    const uint8_t* item=nullptr;
    uint64_t base=0;
    int game=-1,weapon=-1,level=-1;
    motiongun::Vec origin{},direction{}; // controller wrist space
} g_weaponAimBinding;
motiongun::TriggerInput g_gunTriggers;
motiongun::EquipInput g_gunEquip;
motiongun::HolsterInput g_holsters;
bool g_handFired[2]{};
int g_firingHand = -1;
int16_t g_firingAim[2] = {};
PHD_3DPOS g_firingPose{};
motiongun::Vec g_firingDirection{};
unsigned g_motionArmDraws[2] = {};
unsigned g_motionShots[2] = {};
uint64_t g_motionReportTime = 0;

struct MotionRange {
    float low = 0, high = 0;
    void Add(float value, bool first) {
        low = first ? value : std::min(low, value);
        high = first ? value : std::max(high, value);
    }
};
struct CameraMotionTrace {
    uint64_t start = 0;
    unsigned samples = 0;
    locomotion::Vec previousBody{}, previousEye{};
    MotionRange animatedSide, rootSideStep, eyeSideStep, trackedSide, yawError;
    int fracMin = 256, fracMax = 0;
};
CameraMotionTrace g_cameraMotionTrace{};
struct EyeContinuityTrace {
    bool valid=false;
    const uint8_t* item=nullptr;
    uint64_t report=0;
    motiongun::Vec eye{},root{},head{};
    locomotion::Vec drag{};
    float heading=0;
} g_eyeContinuity;

const uint8_t kGenerateW2VPrologue[] = { 0x40, 0x53, 0x55, 0x56, 0x57 };
const uint8_t kDrawCreatureDebug[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
const uint8_t kDrawCreatureRetail[] = { 0x48, 0x89, 0x5C, 0x24, 0x18 };
const uint8_t kDrawHairPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x20 };
const uint8_t kAimWeaponPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
const uint8_t kSetGunFlashTR4[] = {0x4C,0x8B,0xDC,0x48,0x81,0xEC,0x98,0,0,0};
const uint8_t kSetGunFlashTR5[] = {0x48,0x81,0xEC,0xA8,0,0,0};
const uint8_t kGetJointsPrologue[] = { 0x44, 0x89, 0x44, 0x24, 0x18 };
const uint8_t kFireWeaponPrologue[] = { 0x4C, 0x89, 0x44, 0x24, 0x18 };
const uint8_t kAnimatePistols[] = {0x48,0x89,0x5C,0x24,0x10};
const uint8_t kPistolHandlerTR4[] = { 0x48, 0x89, 0x5C, 0x24, 0x18 };
const uint8_t kPistolHandlerTR5[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };
const uint8_t kGetTargetOnLOSTR4[] = { 0x40, 0x55, 0x56, 0x57, 0x41, 0x56 };
const uint8_t kGetTargetOnLOSTR5[] = { 0x40, 0x55, 0x53, 0x57, 0x41, 0x55 };
const uint8_t kLaraAboveWaterTR4[] = { 0x48, 0x89, 0x6C, 0x24, 0x20 };
const uint8_t kLaraAboveWaterTR5[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
const uint8_t kAnimateLaraTR4[] = { 0x40, 0x53, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 };
const uint8_t kAnimateLaraTR5[] = { 0x40, 0x53, 0x55, 0x56, 0x41, 0x57 };
const uint8_t kCollisionDebug[] = { 0x4C, 0x8B, 0xDC, 0x45, 0x89, 0x4B, 0x20 };
const uint8_t kCollisionRetail[] = { 0x4C, 0x8B, 0xDC, 0x45, 0x89, 0x43, 0x18 };
const uint8_t kGetFloor[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
const uint8_t kGetHeightTR4[] = { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56 };
const uint8_t kGetHeightTR5[] = { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x55 };
const uint8_t kItemNewRoom[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };

template <typename T>
T* Ptr(uint32_t rva) {
    return reinterpret_cast<T*>(g_boundBase + rva);
}

float Wrap(float radians) {
    return std::remainder(radians, 2.0f * kPi);
}

float Radians(int16_t angle) {
    return angle * (2.0f * kPi / 65536.0f);
}

int16_t Angle(float radians) {
    return static_cast<int16_t>(static_cast<int>(
        Wrap(radians) * (65536.0f / (2.0f * kPi))));
}

float Elapsed(LARGE_INTEGER& last) {
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float seconds = last.QuadPart && freq.QuadPart
        ? static_cast<float>(double(now.QuadPart - last.QuadPart) /
                             double(freq.QuadPart))
        : 0.0f;
    last = now;
    return std::clamp(seconds, 0.0f, 0.05f);
}

double TurnTime() {
    LARGE_INTEGER now{},freq{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    return freq.QuadPart ? double(now.QuadPart)/double(freq.QuadPart) : 0;
}

void AdvanceStickTurn(double now) {
    const float turn=g_renderTurn.Step(now);
    if (turn!=0) {
        g_headingBase=Wrap(g_headingBase+turn);
        VR().PivotHeadFloorOffset(turn);
    }
    if (!Cfg().firstPersonDriftLog) { g_turnTrace={}; return; }
    auto& trace=g_turnTrace;
    if (!trace.start) trace.start=now;
    ++trace.views;
    if (turn!=0) ++trace.moved;
    trace.maxStep=std::max(trace.maxStep,std::fabs(turn)*180/kPi);
    if (now-trace.start>=1) {
        if (trace.moved) LogF("fp-turn: %.2fs polls=%u views=%u turned=%u max-step=%.3fdeg",
            now-trace.start,trace.polls,trace.views,trace.moved,trace.maxStep);
        trace={}; trace.start=now;
    }
}

bool IsSceneCall(const void* returnAddress) {
    return g_boundDll && g_boundBase &&
        reinterpret_cast<uint64_t>(returnAddress) ==
            g_boundBase + g_boundDll->w2vSceneReturn;
}

bool ScriptedCameraActive() {
    if (!g_boundDll || !g_boundBase) return false;
    const auto& d=*g_boundDll;
    // TR5 X-ray floor triggers set skelebob for the complete rendered effect.
    // Its side-on camera can alternate with chase/look types, so type alone
    // would repeatedly resume FP and corrupt the X-ray presentation. Suspend
    // through the normal camera gate, preserving the player's FP preference.
    if (d.game==1 && *Ptr<uint8_t>(d.lara+off::lara_skelebob)) return true;
    if (d.playingCutseq && *Ptr<int32_t>(d.playingCutseq)) return true;
    // Native handle_cutseq_triggering only uses this state while an ID is
    // active. Include fade-in/out without treating a stale trigger as a scene.
    if (d.cutseqNum && d.cutseqTrigger && *Ptr<int32_t>(d.cutseqNum) &&
        *Ptr<int32_t>(d.cutseqTrigger)) return true;
    if (d.useSpotCam && *Ptr<int32_t>(d.useSpotCam)) return true;
    return d.vonCroyCutscene && *Ptr<uint8_t>(d.vonCroyCutscene);
}

bool Gate(bool inventoryBackground=false) {
    if (!g_boundDll || !g_boundBase || !Cfg().enabled ||
        !Cfg().firstPerson || !g_runtimeEnabled)
        return false;
    if (!VR().active() || !VR().poseValid()) return false;
    if ((InInventory() && !inventoryBackground) || InFMV() || InTitle()) return false;
    if (ScriptedCameraActive()) return false;
    const GameDllLayout& dll = *g_boundDll;
    const auto* item=*Ptr<uint8_t*>(dll.laraItem);
    if (!item || !firstperson::UseHeadCamera(
        *reinterpret_cast<const int16_t*>(item+off::item_hit_points))) return false;
    // Read the active animation state, not its goal: native switches can
    // request STOP before the interaction animation has finished. Suspending
    // through Gate restores the camera, full mesh and input together; it does
    // not toggle the user's first-person preference.
    if(firstperson::SwitchUsesThirdPerson(dll.game,
        *reinterpret_cast<const int16_t*>(item+off::item_anim_state))) return false;
    const int water=LaraWaterStatus();
    // Suspend the whole FP path, not just its camera or body mask. Wading
    // remains eligible; native surface/underwater swimming uses third person.
    if (water==1 || water==2) return false;
    const int32_t type = *Ptr<int32_t>(g_boundDll->camera + off::camera_type);
    return type != kCamFixed && type < kCamCinematic;
}

void RestoreHeadMesh() {
    if (g_meshOverride && g_meshItem && g_boundDll && g_boundBase &&
        GameDllBound() == g_boundDll && GameDllBase() == g_boundBase) {
        uint8_t* current = *Ptr<uint8_t*>(g_boundDll->laraItem);
        if (current == g_meshItem) {
            *reinterpret_cast<uint32_t*>(g_meshItem + off::item_mesh_bits) =
                g_meshBaseBits;
        }
    }
    g_meshOverride = false;
    g_meshItem = nullptr;
    g_headHidden = g_rollHidden = g_crouchHidden = g_underwaterHidden = g_ledgeArmsOnly = false;
    g_unarmedArmsHidden=false;
}

// Native icons are world points projected into a flat HUD, not Lara meshes.
// At head height a nearby floor/switch marker can be below that panel or just
// behind the eye. Keep these existing, native-approved prompts readable. Do
// not create interactions, bypass LOS, or reveal distant/behind-player markers.
bool PlaceActionIcon(PHD_VECTOR& point, const float* matrix, float perspective,
                     float centerX, float centerY, float nearZ, float farZ) {
    if (!std::isfinite(perspective) || perspective <= 0 ||
        !std::isfinite(centerX) || !std::isfinite(centerY) || centerX <= 0 || centerY <= 0 ||
        !std::isfinite(nearZ) || !std::isfinite(farZ) || nearZ < 0 || farZ <= nearZ + 64)
        return false;
    for (int i=0;i<12;++i) if (!std::isfinite(matrix[i])) return false;
    // fw2v_matrix stores unit rotation rows and the camera WORLD position in
    // its fourth column (not the usual affine translation).
    for (int row=0;row<3;++row) for (int other=0;other<=row;++other) {
        float dot=0;
        for (int j=0;j<3;++j) dot+=matrix[row*4+j]*matrix[other*4+j];
        if (std::fabs(dot-(row==other ? 1.0f : 0.0f))>0.002f) return false;
    }
    const float delta[3]={float(point.x)-matrix[3],float(point.y)-matrix[7],float(point.z)-matrix[11]};
    if (delta[0]*delta[0]+delta[1]*delta[1]+delta[2]*delta[2]>1024.0f*1024.0f) return false;
    float view[3]={};
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) view[i]+=matrix[i*4+j]*delta[j];
    if (view[2]<-256 || view[2]>=farZ) return false;
    const float depth=std::max(256.0f,nearZ+32.0f);
    if (depth>=farZ) return false;
    const float maxX=0.70f*centerX/perspective, maxY=0.70f*centerY/perspective;
    const float denominator=std::max(depth,view[2]);
    const float x=std::clamp(view[0]/denominator,-maxX,maxX);
    const float y=std::clamp(view[1]/denominator,-maxY,maxY);
    if (view[2]>=depth && x==view[0]/denominator && y==view[1]/denominator) return false;
    const float adjusted[3]={x*denominator,y*denominator,denominator};
    int32_t result[3];
    for (int j=0;j<3;++j) {
        double world=matrix[j*4+3];
        for (int i=0;i<3;++i) world+=matrix[i*4+j]*adjusted[i];
        if (!std::isfinite(world) || world<double(INT32_MIN)+1 || world>double(INT32_MAX)-1) return false;
        result[j]=int32_t(std::lround(world));
    }
    point={result[0],result[1],result[2]};
    return true;
}

void __cdecl Detour_DrawActionIndicators() {
    using Fn = void (__cdecl*)();
    const auto original=g_hDrawActionIndicators.Original<Fn>();
    if (!g_active || !g_boundBase || !g_actionIconDll || !VR().poseValid()) {
        original(); return;
    }
    const auto& d=*g_actionIconDll;
    const int count=*Ptr<int32_t>(d.count);
    if (count<=0 || count>20) { original(); return; }
    auto* points=Ptr<PHD_VECTOR>(d.points);
    PHD_VECTOR saved[20];
    std::memcpy(saved,points,count*sizeof(PHD_VECTOR));
    bool changed=false;
    for (int i=0;i<count;++i)
        changed=PlaceActionIcon(points[i],Ptr<float>(d.matrix),float(*Ptr<int32_t>(d.perspective)),
            float(*Ptr<int32_t>(d.centerX)),float(*Ptr<int32_t>(d.centerY)),
            *Ptr<float>(d.nearZ),*Ptr<float>(d.farZ)) || changed;
    // Original keeps the player's Action Indicators setting, menu gates,
    // native icon artwork and HUD/stereo rendering. Restore before any logic
    // or subsequent eye/frame can observe our draw-local placement.
    original();
    std::memcpy(points,saved,count*sizeof(PHD_VECTOR));
    if (changed && !g_loggedActionIcon) {
        g_loggedActionIcon=true;
        Log("firstperson: nearby Action icon placement corrected on HUD");
    }
}

uint32_t VisibleMeshBits(uint32_t base, bool head, bool roll, bool crouch, bool ledge, bool underwater = false, bool hideArms = false) {
    if (roll || underwater) return 0;
    if (crouch) return base & firstperson::HandMeshBits;
    if (ledge) return base & kArmMeshBits;
    return base & ~(head ? kHeadMeshBit : 0u) & ~(hideArms ? kArmMeshBits : 0u);
}

void SetMeshVisibility(bool hideHead, bool hideRoll, bool hideCrouch = false,
                       bool ledgeArms = false, bool hideUnderwater = false, bool hideArms = false,
                       bool trackedHands = false) {
    // Tracked hands retain their arms through stance changes, while ledge
    // restrictions still hide the body. Keep the native snapshot so an
    // equip/holster transition can still restore the correct unarmed mask.
    if (trackedHands) hideRoll=hideArms=false;
    if ((!hideHead && !hideRoll && !hideCrouch && !ledgeArms && !hideUnderwater && !hideArms) || !g_boundDll || !g_boundBase) {
        RestoreHeadMesh();
        return;
    }
    uint8_t* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item) {
        g_meshOverride = false;
        g_meshItem = nullptr;
        g_headHidden = g_rollHidden = g_crouchHidden = g_underwaterHidden = g_ledgeArmsOnly = false;
        g_unarmedArmsHidden=false;
        return;
    }
    if (!g_meshOverride || item != g_meshItem) {
        g_meshOverride = true;
        g_meshItem = item;
        g_meshBaseBits = *reinterpret_cast<uint32_t*>(
            item + off::item_mesh_bits);
        g_headHidden = g_rollHidden = g_crouchHidden = g_underwaterHidden = g_ledgeArmsOnly = false;
        g_unarmedArmsHidden=false;
    }
    auto& bits = *reinterpret_cast<uint32_t*>(item + off::item_mesh_bits);
    const uint32_t expected = VisibleMeshBits(g_meshBaseBits,g_headHidden,
        g_rollHidden,g_crouchHidden,g_ledgeArmsOnly,g_underwaterHidden,g_unarmedArmsHidden);
    if (g_meshOverride && bits != expected) {
        // Preserve native visibility changes outside our owned mask. During
        // a hidden stance we own the full mask and retain its original snapshot.
        if (!g_rollHidden && !g_crouchHidden && !g_underwaterHidden && !g_ledgeArmsOnly) {
            const uint32_t owned=kHeadMeshBit | (g_unarmedArmsHidden ? kArmMeshBits : 0u);
            g_meshBaseBits = (bits & ~owned) | (g_meshBaseBits & owned);
        }
    }
    g_headHidden = hideHead;
    g_rollHidden = hideRoll;
    g_crouchHidden = hideCrouch;
    g_underwaterHidden = hideUnderwater;
    g_ledgeArmsOnly = ledgeArms;
    g_unarmedArmsHidden=hideArms;
    bits = VisibleMeshBits(g_meshBaseBits,hideHead,hideRoll,hideCrouch,ledgeArms,hideUnderwater,hideArms);
}

bool IsCrouchState(const uint8_t* item) {
    if (!item) return false;
    // Verified in both games' lara_control_routines: duck, all4s/crawl,
    // all4turnl/r, crawlb and duckl/r. Duck-roll already uses IsRollState.
    switch (*reinterpret_cast<const int16_t*>(item + off::item_anim_state)) {
    case 71: case 80: case 81: case 84: case 85: case 86:
    case 105: case 106: return true;
    default: return false;
    }
}

bool IsHandsOnlyBodyState(const uint8_t* item) {
    if(!item) return false;
    // TR4/5 lara_control_routines[25]: backjump. Side jumps stay visible.
    // Use the current animation state, not stick input or the requested goal,
    // so the body returns as soon as the directional jump ends. Camera and
    // locomotion still distinguish these jumps from actual crouching.
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    return IsCrouchState(item) || state==25;
}

bool IsNativeAirborneBodyState(const uint8_t* item) {
    if(!item) return false;
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    return state==3 || state==9 || state==15 || (state>=25 && state<=29);
}

bool ShowJumpHair(const uint8_t* item) {
    if(!g_active || !item || !g_boundDll || !g_boundBase ||
       item!=*Ptr<uint8_t*>(g_boundDll->laraItem)) return false;
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    return state==3 || state==15 || state==26 || state==27;
}

bool CanUseCrouchCamera(const uint8_t* item) {
    return IsCrouchState(item) && LaraWaterStatus()==0 &&
        *reinterpret_cast<const int16_t*>(item+off::item_hit_points)>0 &&
        !(*reinterpret_cast<const uint32_t*>(item+0x1820)&8) &&
        !(*Ptr<uint32_t>(g_boundDll->lara+off::lara_movement_flags)&0x20) &&
        (g_boundDll->game!=0 || *Ptr<int16_t>(g_boundDll->lara+off::lara_vehicle)<0);
}

bool CanSteerCrouch(const uint8_t* item) {
    if(!CanUseCrouchCamera(item) || *reinterpret_cast<const int16_t*>(item+22)!=0) return false;
    // Required interactions and crawl-to-hang/roll transitions stay native.
    switch(*reinterpret_cast<const int16_t*>(item+off::item_goal_state)) {
    case 2: case 71: case 80: case 81: case 84: case 85: case 86:
    case 105: case 106: return true;
    default:return false;
    }
}

void ApplyCrouchTravelHeading(uint8_t* item) {
    if(!g_crouchDrive || item!=g_headingItem || !CanSteerCrouch(item)) return;
    const bool backward=*reinterpret_cast<const int16_t*>(item+off::item_anim_state)==86;
    // Crawl-back clips have NEGATIVE velocity; their collision samples behind
    // Lara. Animate along body yaw, then let negative speed move her backward.
    const int16_t facing=Angle(g_crouchTravelYaw+(backward ? kPi : 0));
    reinterpret_cast<PHD_3DPOS*>(item+off::item_pos)->y_rot=facing;
    *Ptr<int16_t>(g_boundDll->lara+off::lara_move_angle)=facing;
    *Ptr<int16_t>(g_boundDll->lara+off::lara_turn_rate)=0;
}

bool IsRollState(const uint8_t* item) {
    if (!item) return false;
    switch (*reinterpret_cast<const int16_t*>(item + off::item_anim_state)) {
    case 23: case 45: case 66: case 68: case 72: return true;
    default: return false;
    }
}

bool GroundBodyAvailable(const uint8_t* item) {
    if (!item || *reinterpret_cast<const int16_t*>(item + off::item_hit_points) <= 0 ||
        LaraWaterStatus() != 0)
        return false;
    // Vehicle is an active item index / -1 sentinel ONLY in TR4. TR5 retains
    // the struct slot but InitialiseLara leaves it zero and LaraAboveWater
    // never reads it. Applying TR4's guard there disables all normal FP gait,
    // body turning and room-scale motion. A shared offset is not shared use.
    if (g_boundDll->game == 0 &&
        *Ptr<int16_t>(g_boundDll->lara + off::lara_vehicle) >= 0)
        return false;
    // MoveLaraPosition owns root/yaw while auto-aligning to a switch or
    // other interaction. The state can still be stop/walk/run at this point;
    // head following, roomscale drag and FP gait overrides must all yield.
    if ((*Ptr<uint32_t>(g_boundDll->lara + off::lara_movement_flags) & 0x20) != 0)
        return false;
    return true;
}

bool CanTurnBody(const uint8_t* item) {
    if (!GroundBodyAvailable(item)) return false;
    // Include TR4/5 sprint (73) in ground steering/body fitting. Otherwise
    // native modern controls turn the sprinting body away from the FP view.
    // Ladders, pickups, jumps and interactions retain their native facing.
    switch (*reinterpret_cast<const int16_t*>(item + off::item_anim_state)) {
    case 0: case 1: case 2: case 5: case 6: case 7:
    case 16: case 20: case 21: case 22: case 73: return true;
    default: return false;
    }
}

bool CanFitStandingBody(const uint8_t* item) {
    // Wall impacts retain the standing render frame. They must not drop the
    // torso offset for one animation, but native collision still owns steering.
    return CanTurnBody(item) || (GroundBodyAvailable(item) && g_groundEye.valid &&
        *reinterpret_cast<const int16_t*>(item + off::item_anim_state)==12);
}

bool CanSteerMonkeyBars(const uint8_t* item) {
    return item && LaraWaterStatus()==0 &&
        *reinterpret_cast<const int16_t*>(item+off::item_hit_points)>0 &&
        locomotion::IsMonkeyBarState(*reinterpret_cast<const int16_t*>(item+off::item_anim_state)) &&
        (*Ptr<uint32_t>(g_boundDll->lara+off::lara_movement_flags)&0x40)!=0;
}

bool OrdinaryJumpLanding(const uint8_t* item) {
    return g_active && Cfg().firstPersonMovementStabilization && g_groundEye.valid &&
        g_jumpLanding.owner==item && g_jumpLanding.level==AppState(drva::app_off::level) &&
        g_jumpLanding.ordinary && item && CanTurnBody(item) &&
        locomotion::IsLandingAnimation(g_boundDll->game,
            *reinterpret_cast<const int16_t*>(item+off::item_anim_state),
            *reinterpret_cast<const int16_t*>(item+off::item_anim_number));
}
bool UseLandingCamera(const uint8_t* item) {
    return item && CanTurnBody(item) && !OrdinaryJumpLanding(item) && locomotion::IsLandingAnimation(g_boundDll->game,
        *reinterpret_cast<const int16_t*>(item+off::item_anim_state),
        *reinterpret_cast<const int16_t*>(item+off::item_anim_number));
}

bool HideLandingHead(const uint8_t* item) {
    return g_active && g_boundDll && g_boundBase && item &&
        item==*Ptr<uint8_t*>(g_boundDll->laraItem) && UseLandingCamera(item);
}

uint32_t FirstPersonBodyMask(const uint8_t* item) {
    if(!g_active || !g_boundDll || !g_boundBase || !item ||
       item!=*Ptr<uint8_t*>(g_boundDll->laraItem)) { g_bodyVisibility={};return UINT32_MAX; }
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    const auto mask=g_bodyVisibility.Update(item,AppState(drva::app_off::level),TurnTime(),
        locomotion::IsLedgeHangState(state) || locomotion::IsLedgeMountState(state),
        UseLandingCamera(item),g_mountBodyTransition.active || g_torsoPose.recovering);
    return IsHandsOnlyBodyState(item) ? firstperson::HandMeshBits : mask;
}

bool CanModifyGroundMotion(const uint8_t* item) {
    if (!CanTurnBody(item) || (*reinterpret_cast<const uint32_t*>(item+0x1820)&8))
        return false;
    // A requested jump/interaction must keep its animation displacement even
    // before current_anim_state has caught up with goal_anim_state.
    switch (*reinterpret_cast<const int16_t*>(item+off::item_goal_state)) {
    case 0: case 1: case 2: case 5: case 6: case 7:
    case 16: case 20: case 21: case 22: case 73: return true;
    default: return false;
    }
}

void TurnBodyToHead(uint8_t* item, float dt) {
    if (!Cfg().firstPersonBodyFollowsHead || !CanTurnBody(item)) return;
    auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
    if(Cfg().firstPersonFullBodyIK && Cfg().firstPersonMotionGuns && g_bodyFollower.valid) {
        pos.y_rot=Angle(g_bodyFollower.yaw);
        return;
    }
    const float delta = Wrap(g_headingBase + VR().HeadYawRadians() - Radians(pos.y_rot));
    const float dead = std::clamp(Cfg().firstPersonBodyDeadzoneDegrees, 0.0f, 90.0f)
        * (kPi / 180.0f);
    const float move = std::copysign(std::max(0.0f, std::fabs(delta) - dead), delta);
    const float limit = std::max(0.0f, Cfg().firstPersonBodyTurnDegreesPerFrame)
        * 60.0f * dt * (kPi / 180.0f);
    pos.y_rot = Angle(Radians(pos.y_rot) + std::clamp(move, -limit, limit));
}

using GunVec = motiongun::Vec;
using GunBasis = motiongun::Basis;
using motiongun::Add;
using motiongun::Sub;
using motiongun::Transform;
struct GunPose {
    GunVec nativeHand{}, trackedHand{}, muzzle{}, direction{};
    GunBasis desired{};
    int16_t yaw = 0, pitch = 0;
    bool meshAligned=false;
};

struct LongShot {
    int weapon=0;
    GunPose gun{};
    int16_t baseAim[2]{};
    bool projectile=false;
    mutable bool fired=false; // Aggregate pellets/projectiles into one haptic burst.
};
const LongShot* g_longShot=nullptr;

int CurrentMotionWeapon() {
    return g_boundDll ? *Ptr<int16_t>(g_boundDll->lara+4) : 0;
}
bool PhysicalHolsterOwnsWeapon() {
    return g_holsters.active && CurrentMotionWeapon()==g_holsters.weapon;
}
bool LaserAttachmentEquipped(int weapon) {
    if(!g_boundDll || !g_boundBase) return false;
    const auto* lara=Ptr<uint8_t>(g_boundDll->lara);
    return (weapon==2 && (lara[0x166]&4)) ||
        (g_boundDll->game==0 && weapon==6 && (lara[0x163]&4));
}
bool MotionWeaponSupported(int gun) {
    return motiongun::DualWeapon(gun) ||
        (g_longGunHooksReady && motiongun::SupportedWeapon(gun));
}

HHOOK g_calibrationMessageHook=nullptr;
HWND g_calibrationWindow=nullptr;
bool g_calibrationActive=false;
motiongun::CalibrationKeys g_calibrationKeys{};
int g_calibrationCommands[64]{};
unsigned g_calibrationCommandCount=0;

const char* ControllerTrackingBlockedReason() {
    if (!Cfg().firstPersonMotionGuns) return "disabled-in-INI";
    if (!g_boundDll || !g_boundBase || GameDllBound()!=g_boundDll ||
        GameDllBase()!=g_boundBase) return "game-DLL-changing";
    if (!g_motionHooksReady || !g_motionDll) return "motion-hooks-unavailable";
    if (!g_scenePoseValid) return "no-scene-camera";
    if (!g_active) return "first-person-inactive";
    if (!g_haveHeading) return "no-heading";
    // Only the inventory's world draw may keep rendering the fitted body.
    // Gameplay/input callers still encounter the normal inventory gate.
    if (!Gate(InventorySceneDrawing() && InventorySceneReady() && WorldInventoryActive())) return "first-person-gate";
    if (!Cfg().positionalTracking) return "positional-tracking-disabled";
    if (!Cfg().firstPersonHeadTranslation) return "head-translation-disabled";
    // Classic graphics use a separate Lara renderer. Keep the old head-aim
    // path there until that renderer has its own tracked-arm implementation.
    uint8_t* app = *Ptr<uint8_t*>(g_motionDll->app);
    if (!app || !(*reinterpret_cast<const uint8_t*>(app + 0x9e4) & 1))
        return "classic-graphics-or-no-app";
    const uint8_t* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (item != g_headingItem) return "Lara-item-changed";
    return nullptr;
}

const char* MotionBlockedReason() {
    if (const auto* reason=ControllerTrackingBlockedReason()) return reason;
    const uint8_t* lara = Ptr<uint8_t>(g_boundDll->lara);
    const int16_t status = *reinterpret_cast<const int16_t*>(lara + 2);
    const int16_t gun = *reinterpret_cast<const int16_t*>(lara + 4);
    const bool physicalTransition=PhysicalHolsterOwnsWeapon() && (status==2 || status==3);
    if (status != 4 && !physicalTransition) return "guns-not-ready";
    if (!MotionWeaponSupported(gun)) return "weapon-not-supported";
    vr::HmdMatrix34_t pose{};
    if (motiongun::DualWeapon(gun) && !VR().ControllerPose(0,pose))
        return "left-controller-pose-missing";
    if (!VR().ControllerPose(1,pose)) return "right-controller-pose-missing";
    return nullptr;
}

bool MotionReady() {
    return MotionBlockedReason()==nullptr;
}

bool MotionTriggerMode() {
    if (!Cfg().firstPersonMotionGuns || !g_boundDll || !g_boundBase ||
        GameDllBound()!=g_boundDll || GameDllBase()!=g_boundBase ||
        !g_motionHooksReady || !g_motionDll || !g_active || !Gate() ||
        !Cfg().positionalTracking || !Cfg().firstPersonHeadTranslation) return false;
    const auto* app=*Ptr<uint8_t*>(g_motionDll->app);
    if (!app || !(app[0x9e4]&1)) return false;
    const auto* lara=Ptr<uint8_t>(g_boundDll->lara);
    int gun=*reinterpret_cast<const int16_t*>(lara+4);
    if (gun==0) gun=*reinterpret_cast<const int16_t*>(lara+8); // last equipped
    return MotionWeaponSupported(gun);
}

bool WeaponControlMode() {
    if (!g_boundDll || !g_boundBase || GameDllBound()!=g_boundDll ||
        GameDllBase()!=g_boundBase || !g_active || !Gate()) return false;
    int gun=*Ptr<int16_t>(g_boundDll->lara+4);
    if (!gun) gun=*Ptr<int16_t>(g_boundDll->lara+8);
    return MotionWeaponSupported(gun);
}

bool CrouchWeaponsEligible(const uint8_t* item) {
    if(!g_crouchGunsReady || item!=g_headingItem || !CanSteerCrouch(item) ||
       ControllerTrackingBlockedReason()) return false;
    int gun=CurrentMotionWeapon();
    const int requested=*Ptr<int16_t>(g_boundDll->lara+6);
    if(!gun) gun=*Ptr<int16_t>(g_boundDll->lara+8);
    if(!MotionWeaponSupported(gun) || (requested && !MotionWeaponSupported(requested))) return false;
    vr::HmdMatrix34_t pose{};
    return VR().ControllerPose(1,pose) && (!motiongun::DualWeapon(gun) || VR().ControllerPose(0,pose));
}

// Crawl controls need free hands, but independently tracked guns do not use
// Lara's crawling arm animation. Restore the real weapon state BEFORE LaraGun,
// which native LaraAboveWater calls once, after movement and collision.
struct CrouchWeaponScope;
CrouchWeaponScope* g_crouchWeaponScope=nullptr;
struct CrouchWeaponScope {
    uint8_t* item=nullptr;
    int16_t status=0;
    uint64_t input=0,mask=0;
    bool active=false;
    CrouchWeaponScope(uint8_t* candidate) {
        if(!CrouchWeaponsEligible(candidate)) return;
        item=candidate;status=*Ptr<int16_t>(g_boundDll->lara+2);
        if(status<0 || status>4) return;
        // Status 1 is the native hands-busy flag in ordinary crawling only.
        if(status==1 && !IsCrouchState(item)) return;
        active=true;g_crouchWeaponScope=this;
        input=*Ptr<uint64_t>(g_boundDll->input);
        mask=0x20 | (status>=2 ? 0x40 : 0); // Equip/fire must not become crawl interactions.
        *Ptr<uint64_t>(g_boundDll->input)&=~mask;
        *Ptr<int16_t>(g_boundDll->lara+2)=0;
    }
    void Finish() {
        if(!active) return;
        active=false;g_crouchWeaponScope=nullptr;
        auto& nativeStatus=*Ptr<int16_t>(g_boundDll->lara+2);
        // Preserve native busy/death/airborne transitions out of ordinary crawl.
        const bool ordinary=CanSteerCrouch(item) || (CanTurnBody(item) &&
            *reinterpret_cast<int16_t*>(item+22)==0);
        if(ordinary && nativeStatus<=1) nativeStatus=status==1 ? 0 : status;
        auto& nativeInput=*Ptr<uint64_t>(g_boundDll->input);
        nativeInput=(nativeInput&~mask)|(input&mask);
    }
    ~CrouchWeaponScope() { Finish(); }
};

void __cdecl Detour_CrouchGun() {
    if(g_crouchWeaponScope) g_crouchWeaponScope->Finish();
    auto original=g_hCrouchGun.Original<void (__cdecl*)()>();
    auto* item=g_boundDll && g_boundBase ? *Ptr<uint8_t*>(g_boundDll->laraItem) : nullptr;
    if(!CrouchWeaponsEligible(item)) { original();return; }
    auto& state=*reinterpret_cast<int16_t*>(item+off::item_anim_state);
    const auto saved=state;
    // Only the weapon dispatcher sees standing state. The actual crouch clip,
    // root, collision, camera and render state remain crouched. This removes
    // native long-gun draw restrictions while retaining ammo/recoil/fire rate.
    state=2;
    original();
    state=saved;
}

const char* g_gunPoseFailure[2] = {"not-built","not-built"};
unsigned g_headAimFallbacks[2]{};
void ReportMotionState(const char* where) {
    if (!Cfg().firstPersonMotionGuns || !g_boundDll || !g_boundBase ||
        GameDllBound()!=g_boundDll || GameDllBase()!=g_boundBase) return;
    const auto* reason=MotionBlockedReason();
    const auto* lara=Ptr<uint8_t>(g_boundDll->lara);
    vr::HmdMatrix34_t pose{};
    float r=0,d=0,f=0;
    const bool left=VR().ControllerPose(0,pose), right=VR().ControllerPose(1,pose);
    const bool leftOffset=VR().FirstPersonControllerOffset(0,r,d,f);
    const bool rightOffset=VR().FirstPersonControllerOffset(1,r,d,f);
    const uint8_t* app=g_motionDll ? *Ptr<uint8_t*>(g_motionDll->app) : nullptr;
    LogF("firstperson: Touch state [%s] blocked=%s game=%d gun=%d status=%d HD=%d scene=%d active=%d heading=%d poses=%d/%d offsets=%d/%d build=%s/%s head-fallbacks=%u/%u",
        where,reason ? reason : "none",g_boundDll->game,
        int(*reinterpret_cast<const int16_t*>(lara+4)),
        int(*reinterpret_cast<const int16_t*>(lara+2)),
        app ? int(app[0x9e4]&1) : -1,int(g_scenePoseValid),int(g_active),
        int(g_haveHeading),int(left),int(right),int(leftOffset),int(rightOffset),
        g_gunPoseFailure[0],g_gunPoseFailure[1],
        g_headAimFallbacks[0],g_headAimFallbacks[1]);
}

bool CalibrationFocused() {
    return g_calibrationWindow && GetForegroundWindow()==g_calibrationWindow;
}

LRESULT CALLBACK CalibrationMessageHook(int code, WPARAM remove, LPARAM param) {
    // Only consume messages actually removed from this game's render/window
    // thread queue. Never hook other applications or use a global keyboard hook.
    if (code>=0 && remove==PM_REMOVE) {
        auto* msg=reinterpret_cast<MSG*>(param);
        if (msg->hwnd==g_calibrationWindow &&
            (msg->message==WM_KEYDOWN || msg->message==WM_KEYUP ||
             msg->message==WM_SYSKEYDOWN || msg->message==WM_SYSKEYUP)) {
            const int key=int(msg->wParam)-VK_F1;
            const bool down=msg->message==WM_KEYDOWN || msg->message==WM_SYSKEYDOWN;
            int command=-1;
            // Never dereference a game-DLL address from the message hook:
            // game switches can unload it before the next render update.
            const bool active=g_calibrationActive && CalibrationFocused() &&
                !InInventory() && !InTitle() && !InFMV();
            const bool controlKey=msg->wParam==VK_CONTROL ||
                msg->wParam==VK_LCONTROL || msg->wParam==VK_RCONTROL;
            // Ctrl is native Action/Fire. Reserve the modifier too while
            // calibration is available, so adjusting a gun cannot fire it.
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
                // Block the matching keyup too, even if Ctrl was released first.
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
                Log("gun calibration: Ctrl+F1..F6 position, Ctrl+Shift+F1..F6 angles; F7 save/restore");
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

void ReportMotionActivity() {
    if (!g_boundDll || !g_boundBase || GameDllBound()!=g_boundDll ||
        GameDllBase()!=g_boundBase || !Cfg().firstPersonMotionGuns) return;
    const uint64_t now = GetTickCount64();
    if (!g_motionReportTime) g_motionReportTime = now;
    if (now - g_motionReportTime < 5000) return;
    const bool ready = MotionReady();
    if (g_active || g_headAimFallbacks[0] || g_headAimFallbacks[1])
        ReportMotionState("periodic");
    if (ready || g_motionArmDraws[0] || g_motionArmDraws[1] ||
        g_motionShots[0] || g_motionShots[1]) {
        LogF("firstperson: Touch ready=%d arms L=%u R=%u shots L=%u R=%u",
             ready ? 1 : 0,
             g_motionArmDraws[0], g_motionArmDraws[1],
             g_motionShots[0], g_motionShots[1]);
    }
    g_motionArmDraws[0] = g_motionArmDraws[1] = 0;
    if (g_armIKSolved || g_armIKFailed)
        LogF("firstperson: arm IK palettes solved=%u failed=%u unarmed-enabled=%d",
             g_armIKSolved,g_armIKFailed,Cfg().firstPersonUnarmedIK ? 1 : 0);
    g_armIKSolved=g_armIKFailed=0;
    g_motionShots[0] = g_motionShots[1] = 0;
    g_headAimFallbacks[0] = g_headAimFallbacks[1] = 0;
    g_motionReportTime = now;
}

// Native joint 10 is the right gun and joint 13 the left gun. Query the
// interpolated native joint axes so the renderer's whole arm pass can be moved
// rigidly without touching Lara's simulation skeleton or doing IK.
GunVec JointPoint(uint8_t* item, int joint, int x, int y, int z) {
    PHD_VECTOR p{x, y, z};
    const int frac = std::clamp(*Ptr<int32_t>(g_boundDll->frameFrac), 0, 256);
    reinterpret_cast<Fn_GetJointAbsPositionLerp>(
        g_boundBase + g_boundDll->getJointAbsPositionLerp)(item, &p, joint, frac);
    return {float(p.x), float(p.y), float(p.z)};
}

void AttachPhysicalSwitchHand(int hand,motiongun::Frame& wrist,motiongun::Vec trackedGrip);

bool BuildControllerWrist(int hand, motiongun::Frame& out, GunBasis& controller, GunVec* gripLocal=nullptr) {
    if (hand < 0 || hand > 1) return false;
    if (const auto* reason=ControllerTrackingBlockedReason()) {
        g_gunPoseFailure[hand]=reason;
        return false;
    }
    auto rejected=[hand](const char* reason) {
        g_gunPoseFailure[hand]=reason; return false;
    };
    float right, down, forward;
    vr::HmdMatrix34_t tracked{};
    if (!VR().FirstPersonControllerOffset(hand, right, down, forward) ||
        !VR().ControllerPose(hand, tracked))
        return rejected("controller-offset-unavailable");
    if (!std::isfinite(right) || !std::isfinite(down) || !std::isfinite(forward) ||
        std::fabs(right)>2 || std::fabs(down)>2 || std::fabs(forward)>2)
        return rejected("controller-offset-over-2m");
    const float scale = LiveWorldUnitsPerMetre();
    if (!std::isfinite(scale) || scale <= 1) return rejected("invalid-world-scale");
    const auto& calibration=LiveMotionGunCalibration();
    controller = motiongun::CalibratedController(
        motiongun::ControllerBasis(tracked.m, g_headingBase),calibration);
    out.origin = motiongun::HandInWorld(
        {float(g_scenePose.x_pos), float(g_scenePose.y_pos), float(g_scenePose.z_pos)},
        right, down, forward, g_headingBase, scale);
    // Keep the recovered bind-pose pivot path. Calibrate the mesh grip in
    // local gun space so that point, rather than a displaced wrist origin,
    // stays on the tracked controller through wrist and physical body turns.
    out.basis = motiongun::GunBasis(controller);
    const auto trackedGrip=out.origin;
    out.origin = motiongun::GripFrame(out.basis, out.origin,
        calibration.gripForwardMetres*scale,
        calibration.raiseMetres*scale,calibration.rightMetres*scale).origin;

    if (gripLocal) *gripLocal=motiongun::GripPointLocal(calibration.gripForwardMetres*scale,
        calibration.raiseMetres*scale,calibration.rightMetres*scale);
    AttachPhysicalSwitchHand(hand,out,trackedGrip);
    motiongun::Frame inverse{};
    return motiongun::Inverse(out,inverse); // Reject invalid tracked rotations too.
}

// These are render-pose permissions, separate from gameplay body turning.
// Jumping can retain controllers; ledges/mounts keep their authored hand grips.
bool FullBodyGripState(const uint8_t* item) {
    if(!item) return false;
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    return locomotion::IsLedgeHangState(state) || locomotion::IsLedgeMountState(state);
}
bool FullBodyState(const uint8_t* item) {
    if(!item || *reinterpret_cast<const int16_t*>(item+off::item_hit_points)<=0 ||
       LaraWaterStatus()!=0 || (g_boundDll->game==0 &&
       *Ptr<int16_t>(g_boundDll->lara+off::lara_vehicle)>=0)) return false;
    if(CanFitStandingBody(item) || CanSteerCrouch(item) || FullBodyGripState(item)) return true;
    switch(*reinterpret_cast<const int16_t*>(item+off::item_anim_state)) {
    case 3: case 9: case 15: case 25: case 26: case 27: case 71: case 73: return true;
    default: return false;
    }
}
bool FlareIKReady() {
    // A held flare is weapon 7 with hands free (status 0). Draw/throw/pickup
    // animations keep their native hands. Do not classify flares as guns.
    if(!g_flareHooksReady || ControllerTrackingBlockedReason() || CurrentMotionWeapon()!=7 ||
       *Ptr<int16_t>(g_boundDll->lara+2)!=0 ||
       (!Cfg().firstPersonUnarmedIK && !Cfg().firstPersonFullBodyIK)) return false;
    const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    if(!FullBodyState(item) || FullBodyGripState(item)) return false;
    motiongun::Frame wrist{};GunBasis controller{};
    return BuildControllerWrist(0,wrist,controller) && BuildControllerWrist(1,wrist,controller);
}
bool FullBodyIKReady() {
    if(!Cfg().firstPersonFullBodyIK || ControllerTrackingBlockedReason()) return false;
    const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    const int status=*Ptr<int16_t>(g_boundDll->lara+2);
    const bool nativeGrip=status==1 && FullBodyGripState(item); // Hands-busy during pull-ups.
    const bool weaponTransition=(status==2 || status==3) && MotionWeaponSupported(CurrentMotionWeapon());
    if(!FullBodyState(item) || (status!=0 && !nativeGrip && !weaponTransition && !MotionReady()) ||
       (status==0 && !FlareIKReady() && (!Cfg().firstPersonUnarmedIK || CurrentMotionWeapon()<0 || CurrentMotionWeapon()>6))) return false;
    motiongun::Frame wrist{};GunBasis controller{};vr::HmdMatrix34_t head{};
    return VR().HeadPose(head) && BuildControllerWrist(0,wrist,controller) && BuildControllerWrist(1,wrist,controller);
}

bool UnarmedIKReady() {
    if(FlareIKReady()) return true;
    if (!Cfg().firstPersonUnarmedIK || ControllerTrackingBlockedReason()) return false;
    const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    // Full-body free jumps retain controller hands; ledges retain authored grips.
    if (!item || !(CanTurnBody(item) || (Cfg().firstPersonFullBodyIK &&
        FullBodyState(item) && !FullBodyGripState(item))) || *Ptr<int16_t>(g_boundDll->lara+2)!=0 ||
        CurrentMotionWeapon()<0 || CurrentMotionWeapon()>6) return false;
    motiongun::Frame wrist{}; GunBasis controller{};
    return BuildControllerWrist(0,wrist,controller) && BuildControllerWrist(1,wrist,controller);
}

bool BuildGunPose(int hand, GunPose& out) {
    if (hand<0 || hand>1) return false;
    if (const auto* reason=MotionBlockedReason()) {
        g_gunPoseFailure[hand]=reason; return false;
    }
    auto rejected=[hand](const char* reason) {
        g_gunPoseFailure[hand]=reason; return false;
    };
    motiongun::Frame wrist{}; GunBasis controller{};
    if (!BuildControllerWrist(hand,wrist,controller)) return false;
    out.desired=wrist.basis; out.trackedHand=wrist.origin;
    const int16_t gun = *Ptr<int16_t>(g_boundDll->lara + 4);
    uint8_t* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    const int joint = hand ? 10 : 13;
    out.nativeHand = JointPoint(item, joint, 0, 0, 0);
    const auto& body = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    if (std::fabs(out.nativeHand.x - body.x_pos) > 4096 ||
        std::fabs(out.nativeHand.y - body.y_pos) > 4096 ||
        std::fabs(out.nativeHand.z - body.z_pos) > 4096)
        return rejected("native-wrist-out-of-range");
    out.muzzle = Add(out.trackedHand,
        Transform(out.desired, motiongun::MuzzleLocal(gun, hand, g_boundDll->game)));
    GunVec ray{controller.r[0][2], controller.r[1][2],
                     controller.r[2][2]};
    out.meshAligned=false;
    const auto& binding=g_weaponAimBinding;
    if(hand==1 &&
       binding.item==item && binding.base==g_boundBase && binding.game==g_boundDll->game &&
       binding.weapon==gun && binding.level==AppState(drva::app_off::level)) {
        out.muzzle=Transform(wrist,binding.origin);
        ray=Transform(wrist.basis,binding.direction);
        const float length=std::sqrt(motiongun::Dot(ray,ray));
        if(!std::isfinite(length) || length<.001f) return rejected("nonfinite-laser-barrel");
        ray=motiongun::Scale(ray,1.f/length);
        out.meshAligned=true;
    }
    if (!std::isfinite(out.muzzle.x) || !std::isfinite(out.muzzle.y) ||
        !std::isfinite(out.muzzle.z)) return rejected("nonfinite-muzzle");
    out.direction = ray;
    const float flat = std::hypot(ray.x, ray.z);
    if (!std::isfinite(flat)) return rejected("nonfinite-barrel");
    out.yaw = Angle(std::atan2(ray.x, ray.z));
    out.pitch = Angle(std::atan2(-ray.y, flat));
    g_gunPoseFailure[hand]="ok";
    return true;
}

// The light projection is built before S_InitialisePolyList replaces the scene
// camera. Preserve its native matrix, then rebase only the receiver's origin.
bool g_shadowDepthScope=false, g_shadowReceiverPatched=false, g_loggedShadowReceiver=false;
mat4* g_shadowReceiver=nullptr;
mat4 g_nativeShadowReceiver{}, g_patchedShadowReceiver{};

void RestoreShadowReceiver() {
    if (g_shadowReceiver && g_shadowReceiverPatched &&
        !std::memcmp(g_shadowReceiver,&g_patchedShadowReceiver,sizeof(mat4))) {
        *g_shadowReceiver=g_nativeShadowReceiver;
        VidState().consts|=kShadow;
    }
    g_shadowReceiverPatched=false;
}

void __cdecl Detour_DrawToShadow(int32_t room) {
    RestoreShadowReceiver();g_shadowReceiver=nullptr;
    const bool previous=g_shadowDepthScope;g_shadowDepthScope=true;
    g_hDrawToShadow.Original<void (__cdecl*)(int32_t)>()(room);
    g_shadowDepthScope=previous;
    if (!previous && VidState().shadow) {
        g_shadowReceiver=VidState().shadow;
        g_nativeShadowReceiver=*g_shadowReceiver;
    }
}

void RebaseShadowReceiver(const PHD_3DPOS& native,const PHD_3DPOS& scene) {
    RestoreShadowReceiver();
    if (!g_shadowReceiver || !g_active || !g_scenePoseValid) return;
    const double delta[3]={double(scene.x_pos)-native.x_pos,
                           double(scene.y_pos)-native.y_pos,
                           double(scene.z_pos)-native.z_pos};
    g_patchedShadowReceiver=g_nativeShadowReceiver;
    // Column-major M * translate(C_firstPerson - C_native): for a world point
    // P, M_new*(P-C_firstPerson) == M_native*(P-C_native). No HMD rotation,
    // light-camera change, caster movement or per-eye offset belongs here.
    for (int row=0;row<4;++row) {
        double t=g_nativeShadowReceiver.m[12+row];
        for (int axis=0;axis<3;++axis) t+=g_nativeShadowReceiver.m[axis*4+row]*delta[axis];
        g_patchedShadowReceiver.m[12+row]=float(t);
    }
    *g_shadowReceiver=g_patchedShadowReceiver;g_shadowReceiverPatched=true;
    VidState().consts|=kShadow;
    if (!g_loggedShadowReceiver) {
        LogF("firstperson shadow: receiver origin native=(%d,%d,%d) FP=(%d,%d,%d); light camera isolated",
             native.x_pos,native.y_pos,native.z_pos,scene.x_pos,scene.y_pos,scene.z_pos);
        g_loggedShadowReceiver=true;
    }
}


bool DrawingNativeShadow() {
    // DrawCreatureHD's third argument selects joint interpolation, NOT the
    // shadow pass. DrawToShadow selects the DLL-wide gRenderPass=4 instead.
    return g_boundBase && g_shadowDll && *Ptr<int32_t>(g_shadowDll->renderPass)==4;
}

bool BodyVisualOffsetApplies(const uint8_t* item) {
    return g_active && g_scenePoseValid && g_boundDll && g_boundBase &&
        item && item==g_headingItem && item==*Ptr<uint8_t*>(g_boundDll->laraItem) &&
        (CanFitStandingBody(item) || g_bodyWallOffset.x!=0 || g_bodyWallOffset.z!=0);
}

locomotion::Vec BodyRenderOffset() {
    return g_bodyVisualOffset+g_bodyWallOffset;
}

bool ReadIKBind(const uint8_t* obj,int count,int bones,const int32_t* mapping,
                const float* poses,int joint,motiongun::Frame& bind) {
    int pivot=-1;
    for(int i=0;i<count;++i) if((mapping ? mapping[i] : i)==joint) { pivot=i;break; }
    motiongun::Frame inverseBind{};
    if(bones>0 && pivot>=0) inverseBind=motiongun::HdInverseBind(motiongun::ReadRows(poses+pivot*12));
    else {
        // The canonical 15-joint bind remains available when a separate HD
        // body/hand mesh omits a joint from its own palette.
        const auto* p=reinterpret_cast<const float*>(obj+1676)+joint*16;
        for(int row=0;row<3;++row) for(int col=0;col<3;++col) inverseBind.basis.r[row][col]=p[col*4+row];
        inverseBind.origin={p[12],p[13],p[14]};
    }
    return motiongun::Inverse(inverseBind,bind);
}
bool ReadIKJoint(const uint8_t* obj,const float* joints,int count,int bones,
                 const int32_t* mapping,const float* poses,int joint,motiongun::Frame& frame) {
    int pivot=-1;
    for(int i=0;i<count;++i) if((mapping ? mapping[i] : i)==joint) { pivot=i;break; }
    if(pivot<0) return false;
    motiongun::Frame bind{};
    if(!ReadIKBind(obj,count,bones,mapping,poses,joint,bind)) return false;
    frame=motiongun::Multiply(motiongun::ReadRows(joints+pivot*12),bind);
    return firstperson::Finite(frame);
}

// Read-only comparison of rendered arms against raw tracking. Keep all origins
// wrist-relative (elbow-relative for meshes omitting wrists) so camera rebasing
// cannot masquerade as mesh movement.
void TraceArmGlance(uint8_t* item,const float* joints,int count,int pass) {
    if(!Cfg().firstPersonDriftLog || !g_bodySkinScope || count<15 || count>33 ||
       (pass!=0 && pass!=2) ||
       (*reinterpret_cast<const uint32_t*>(item+off::item_mesh_bits)&0x49)!=0x49) return;
    using namespace motiongun;
    const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
    if(object<0) return;
    const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
    const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int32_t*>(geom+28);
    const auto* mapping=bones>0 ? *reinterpret_cast<const int32_t* const*>(geom+48) : nullptr;
    const auto* poses=bones>0 ? *reinterpret_cast<const float* const*>(geom+72) : nullptr;
    if(bones>0 && (bones!=count || !mapping || !poses)) return;
    const float units=LiveWorldUnitsPerMetre();if(units<=1) return;
    struct Sample {
        bool valid=false;const uint8_t* item=nullptr;int status=0,weapon=0,count=0,pivot=0;
        uint64_t serial=0,since=0,last=0;
        Frame arm[3]{},torso{},controller{},head{};
        float peak[10]{};unsigned frames=0;
    };
    static Sample samples[2][2];
    const auto angle=[](const Basis& a,const Basis& b) {
        const auto q=firstperson::RotationProduct(firstperson::RotationOf(a),
            firstperson::RotationInverse(firstperson::RotationOf(b)));
        return 114.59156f*std::atan2(firstperson::Length(q.v),std::fabs(q.w));
    };
    const auto now=GetTickCount64(),serial=VR().PoseSerial();
    const int status=*Ptr<int16_t>(g_boundDll->lara+2),weapon=CurrentMotionWeapon();
    for(int hand=0;hand<2;++hand) {
        Frame arm[3]{},torso{};bool complete=true;
        for(int j=0;j<2;++j) complete=ReadIKJoint(obj,joints,count,bones,mapping,poses,(hand ? 8 : 11)+j,arm[j]) && complete;
        const int pivot=ReadIKJoint(obj,joints,count,bones,mapping,poses,hand ? 10 : 13,arm[2]) ? 2 : 1;
        vr::HmdMatrix34_t controller{},head{};
        if(!complete || !ReadIKJoint(obj,joints,count,bones,mapping,poses,7,torso) ||
           !VR().ControllerPose(hand,controller) || !VR().HeadPose(head)) continue;
        const Vec wrist=arm[pivot].origin;
        for(auto& joint:arm) joint.origin=Scale(Sub(joint.origin,wrist),1000.f/units);
        torso.origin=Scale(Sub(torso.origin,wrist),1000.f/units);
        const Frame tracked=ReadRows(&controller.m[0][0]),hmd=ReadRows(&head.m[0][0]);
        auto& s=samples[pass==2 ? 1 : 0][hand];
        if(!s.valid || s.item!=item || s.status!=status || s.weapon!=weapon || s.count!=count || s.pivot!=pivot ||
           serial<s.serial || now-s.last>250) {
            s={};s.valid=true;s.item=item;s.status=status;s.weapon=weapon;s.count=count;s.pivot=pivot;s.since=now;
        } else if(serial==s.serial) {
            for(int j=0;j<2;++j) s.peak[9]=std::max(s.peak[9],firstperson::Length(Sub(arm[j].origin,s.arm[j].origin)));
            continue;
        } else {
            const float values[]={1000*firstperson::Length(Sub(tracked.origin,s.controller.origin)),
                angle(tracked.basis,s.controller.basis),angle(hmd.basis,s.head.basis),
                firstperson::Length(Sub(arm[0].origin,s.arm[0].origin)),
                firstperson::Length(Sub(arm[1].origin,s.arm[1].origin)),
                angle(arm[0].basis,s.arm[0].basis),angle(arm[1].basis,s.arm[1].basis),
                firstperson::Length(Sub(torso.origin,s.torso.origin)),angle(torso.basis,s.torso.basis)};
            for(int i=0;i<9;++i) s.peak[i]=std::max(s.peak[i],values[i]);
            ++s.frames;
        }
        s.serial=serial;s.last=now;s.controller=tracked;s.head=hmd;s.torso=torso;
        for(int j=0;j<3;++j) s.arm[j]=arm[j];
        if(now-s.since>=500) {
            LogF("arm-glance: t=%llu pose=%llu hand=%d pass=%d status=%d gun=%d joints=%d pivot=%s frames=%u head-step=%.3fdeg controller-step=(%.3fmm,%.3fdeg) arm-relative-step=(%.3f,%.3f)mm arm-turn=(%.3f,%.3f)deg torso-relative-step=(%.3fmm,%.3fdeg) redraw=%.3fmm",
                now,serial,hand,pass,status,weapon,count,pivot==2 ? "wrist" : "elbow",s.frames,s.peak[2],s.peak[0],s.peak[1],s.peak[3],s.peak[4],s.peak[5],s.peak[6],s.peak[7],s.peak[8],s.peak[9]);
            s.since=now;s.frames=0;for(float& value:s.peak) value=0;
        }
    }
}

bool PistolBodyPoseMatches(const uint8_t* item,int pass) {
    if(!g_boundDll || !g_boundBase) return false;
    const int status=*Ptr<int16_t>(g_boundDll->lara+2);
    return (MotionReady() || status==2 || status==3 || g_pistolAnimation.blending) && CurrentMotionWeapon()==1 && g_pistolArmPose.valid &&
        g_pistolArmPose.item==item && g_pistolArmPose.game==g_boundDll->game &&
        g_pistolArmPose.pass==pass && g_pistolArmPose.serial==VR().PoseSerial() &&
        g_pistolArmPose.fraction==*Ptr<int32_t>(g_boundDll->frameFrac);
}

bool PistolBodyArmPose(uint8_t* item,float* joints,int count,int pass,bool capture) {
    using namespace motiongun;
    if(!g_boundDll || !g_boundBase) return false;
    const int status=*Ptr<int16_t>(g_boundDll->lara+2);
    if((!MotionReady() && status!=2 && status!=3 && !g_pistolAnimation.blending) || CurrentMotionWeapon()!=1 || count<15 || count>33) return false;
    const int fraction=*Ptr<int32_t>(g_boundDll->frameFrac);
    if(capture) {
        // Solvers publish the complete chains, including joints omitted from
        // the visible body mesh (e.g. wrists owned by separate pistol meshes).
        if(!g_pistolArmSolveReady) return false;
        g_pistolArmPose.item=item;g_pistolArmPose.game=g_boundDll->game;
        g_pistolArmPose.pass=pass;g_pistolArmPose.fraction=fraction;
        g_pistolArmPose.serial=VR().PoseSerial();g_pistolArmPose.valid=true;
        return true;
    }
    if(!PistolBodyPoseMatches(item,pass)) return false;
    const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
    if(object<0) return false;
    const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
    const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int32_t*>(geom+28);
    const auto* mapping=bones>0 ? *reinterpret_cast<const int32_t* const*>(geom+48) : nullptr;
    const auto* poses=bones>0 ? *reinterpret_cast<const float* const*>(geom+72) : nullptr;
    if(bones>0 && (bones!=count || !mapping || !poses)) return false;
    // Rebind the body's solved joint frames into this attachment's own mesh.
    // Never copy skin matrices between meshes with different inverse binds.
    Frame result[33]{};bool found=false;
    for(int i=0;i<count;++i) {
        const int joint=mapping ? mapping[i] : i;
        if(joint<8 || joint>13) continue;
        Frame bind{},inverse{};
        if(bones>0) {
            inverse=HdInverseBind(ReadRows(poses+i*12));
            if(!Inverse(inverse,bind)) return false;
        } else if(!ReadIKBind(obj,count,bones,mapping,poses,joint,bind) || !Inverse(bind,inverse)) return false;
        result[i]=Multiply(g_pistolArmPose.joint[joint-8],inverse);
        if(!firstperson::Finite(result[i])) return false;
        found=true;
    }
    if(!found) return false;
    for(int i=0;i<count;++i) {
        const int joint=mapping ? mapping[i] : i;
        if(joint>=8 && joint<=13) WriteRows(result[i],joints+i*12);
    }
    return true;
}

#include "PhysicalHolster.inl"

motiongun::Vec BodyPaletteCameraOrigin(uint8_t* item,const motiongun::Frame& head) {
    // HD palettes contain world-axis joints relative to the render camera.
    // Deriving that origin from GetJointAbsPositionLerp subtracts an integer
    // gameplay joint from a floating-point animated joint. Their rounding and
    // animation interpolation differ, so a still camera acquires frame noise.
    float rotation[3][3]{},position[3]{};
    if(CameraViewFrame(rotation,position) &&
       std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]))
        return {position[0],position[1],position[2]};
    // Before the engine publishes its first valid camera frame, retain the
    // old safe reference. Native shadow palettes never enter this solver.
    return Sub(JointPoint(item,14,0,0,0),head.origin);
}

bool ApplyFullBodyIK(uint8_t* item,float* joints,int count) {
    using namespace motiongun;
    if(!FullBodyIKReady()) { g_groundGait.Reset();return false; }
    if(count<15 || count>33) return false;
    if(IsNativeAirborneBodyState(item)) {
        // Airborne feet must follow this frame's native animation. Standing
        // body fitting and mount-recovery filtering can otherwise pull them
        // toward a headset-relative pose or ease them out over several frames.
        // Fall through to arm-only IK, which leaves hips/legs/chest untouched.
        g_bodyFollower.Reset();g_torsoPose.Reset();g_groundGait.Reset();g_bodyLanding=false;g_pistolAnimation.Reset();
        return false;
    }
    const bool landing=UseLandingCamera(item);
    if(g_bodyLanding && !landing) {
        // The camera has returned from the animated landing neck to the
        // standing eye. A cached kneeling torso belongs to the old frame;
        // easing it in the new frame briefly places the body ahead of the eye.
        g_bodyFollower.Reset();g_torsoPose.Reset();
    }
    g_bodyLanding=landing;
    const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
    if(object<0) return false;
    const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
    const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int32_t*>(geom+28);
    const auto* mapping=bones>0 ? *reinterpret_cast<const int32_t* const*>(geom+48) : nullptr;
    const auto* poses=bones>0 ? *reinterpret_cast<const float* const*>(geom+72) : nullptr;
    if(bones>0 && (bones!=count || !mapping || !poses)) return false;
    Frame original[15]{},native[15]{},posed[15]{},correction[15]{},bind[15]{};
    float pistolBodyScale=1.f;
    const auto offset=BodyRenderOffset();
    const Vec fit=BodyVisualOffsetApplies(item) ? Vec{offset.x,0,offset.z} : Vec{};
    for(int joint=0;joint<15;++joint) {
        if(!ReadIKJoint(obj,joints,count,bones,mapping,poses,joint,original[joint])) return false;
        if(!ReadIKBind(obj,count,bones,mapping,poses,joint,bind[joint])) return false;
        native[joint]=original[joint];native[joint].origin=Sub(native[joint].origin,fit);
    }
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    const auto& current=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
    const auto& previous=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos_prev);
    const float fraction=std::clamp(*Ptr<int32_t>(g_boundDll->frameFrac),0,256)/256.f;
    const float nativeHeading=Radians(previous.y_rot)+Wrap(Radians(current.y_rot)-Radians(previous.y_rot))*fraction;
    const Vec simulationRoot{previous.x_pos+(current.x_pos-previous.x_pos)*fraction,
        previous.y_pos+(current.y_pos-previous.y_pos)*fraction,previous.z_pos+(current.z_pos-previous.z_pos)*fraction};
    const bool mainBody=g_bodySkinScope &&
        (*reinterpret_cast<const uint32_t*>(item+off::item_mesh_bits)&0x49)==0x49;
    const Vec cameraOffset=BodyPaletteCameraOrigin(item,native[14]);
    if(mainBody && (g_groundGaitObject!=object || !g_bodyFollower.valid)) {
        g_groundGait.Reset();g_groundGaitObject=object;
    }
    if(!CanFitStandingBody(item) || landing || !Cfg().firstPersonMovementStabilization) {
        if(mainBody) g_groundGait.Reset();
    } else {
        const Frame gaitRoot{firstperson::RotationMatrix(firstperson::AxisRotation({0,1,0},nativeHeading)),
                             Sub(simulationRoot,cameraOffset)};
        const bool sideBack=state==5 || state==16 || state==21 || state==22;
        if(!g_groundGait.Apply(native,gaitRoot,sideBack,mainBody,MotionReady() || UnarmedIKReady(),
                              {std::sin(nativeHeading),0,std::cos(nativeHeading)})) return false;
    }
    const bool ordinaryLanding=OrdinaryJumpLanding(item);
    const Frame landingRoot{firstperson::RotationMatrix(firstperson::AxisRotation({0,1,0},nativeHeading)),
                            Sub(simulationRoot,cameraOffset)};
    const bool nativeLanding=locomotion::IsLandingAnimation(g_boundDll->game,state,
        *reinterpret_cast<const int16_t*>(item+off::item_anim_number));
    if(mainBody && !nativeLanding && CanFitStandingBody(item)) {
        if(g_jumpBodyObject!=object) g_jumpBodyReference.Reset();
        g_jumpBodyObject=object;
        if(!g_jumpBodyReference.Apply(native,landingRoot,false,true,true,
                                      {std::sin(nativeHeading),0,std::cos(nativeHeading)})) return false;
    } else if(ordinaryLanding && g_jumpBodyObject==object && g_jumpBodyReference.valid) {
        // Remove the landing torso squat before live head/hand IK. Keep the
        // native foot contacts and authored button draw/holster arm poses.
        if(!g_jumpBodyReference.Apply(native,landingRoot,true,false,MotionReady() || UnarmedIKReady(),
                                      {std::sin(nativeHeading),0,std::cos(nativeHeading)})) return false;
    }
    const int anchorJoint=Cfg().firstPersonJoint;
    if(anchorJoint<0 || anchorJoint>=15) return false;
    const Vec anchorInPalette=Transform(native[anchorJoint],{float(Cfg().firstPersonAnchorX),float(Cfg().firstPersonAnchorY),
        float(locomotion::FirstPersonAnchorZ(state,Cfg().firstPersonAnchorZ,Cfg().firstPersonInteractionAnchorZ))});
    const Vec anchor=Add(anchorInPalette,cameraOffset);
    const float scale=LiveWorldUnitsPerMetre();
    locomotion::Vec view{};VR().FirstPersonViewOffset(view.x,view.z);
    view=locomotion::Rotate(view,g_headingBase)*scale;
    const Vec eye{float(g_scenePose.x_pos)+view.x,float(g_scenePose.y_pos)+VR().FirstPersonVerticalOffset()*scale,
                  float(g_scenePose.z_pos)+view.z};
    Vec headDelta=Sub(eye,anchor);
    if(!firstperson::Finite({firstperson::IdentityBasis(),headDelta}) || firstperson::Length(headDelta)>4096) return false;
    const bool grip=FullBodyGripState(item) && !MotionReady();
    const float headYaw=Wrap(g_headingBase+VR().HeadYawRadians());
    const Vec nativeRoot=Add(native[0].origin,cameraOffset);
    const bool moving=g_haveManualInput && !g_shifted && locomotion::Length(g_manualLocal)>.05f;
    float facing=headYaw;
    if(moving && !Cfg().firstPersonMoveWithHead) facing=std::atan2(g_manualWorld.x,g_manualWorld.z);
    // Forward controllers support the body's existing facing, independent of
    // gaze. Idle head turns should neither drag the chest nor redefine forward.
    bool handsForward=false;
    float r[2]{},d[2]{},f[2]{},headR=0,headF=0;
    VR().FirstPersonViewOffset(headR,headF);
    if(VR().FirstPersonControllerOffset(0,r[0],d[0],f[0]) &&
       VR().FirstPersonControllerOffset(1,r[1],d[1],f[1])) {
        const Vec leftHint=motiongun::HandInWorld({},r[0]-headR,0,f[0]-headF,g_headingBase,1);
        const Vec rightHint=motiongun::HandInWorld({},r[1]-headR,0,f[1]-headF,g_headingBase,1);
        const Vec handHint=Scale(Add(leftHint,rightHint),.5f);
        const float bodyYaw=g_bodyFollower.valid ? g_bodyFollower.yaw : nativeHeading;
        const float headDrop=VR().FirstPersonHeightDrop();
        vr::HmdMatrix34_t leftPose{},rightPose{};
        const float handHeightLimit=g_bodyFollower.supportedHands ? .6f : .5f;
        if(Cfg().firstPersonBodyHandYawWeight>0 && d[0]-headDrop<handHeightLimit && d[1]-headDrop<handHeightLimit &&
           VR().ControllerPose(0,leftPose) && VR().ControllerPose(1,rightPose)) {
            const auto leftBasis=ControllerBasis(leftPose.m,g_headingBase);
            const auto rightBasis=ControllerBasis(rightPose.m,g_headingBase);
            handsForward=firstperson::HandsSupportFacing(leftHint,rightHint,
                Transform(leftBasis,{0,0,1}),Transform(rightBasis,{0,0,1}),bodyYaw,g_bodyFollower.supportedHands);
            if(moving && handsForward)
                facing=Wrap(facing+std::clamp(Wrap(std::atan2(handHint.x,handHint.z)-facing),-.35f,.35f)*Cfg().firstPersonBodyHandYawWeight);
        }
    }
    // Collision retraction belongs to the shared eye/body frame, not the
    // inferred physical step. Apply it immediately once to the whole body.
    const Vec wallShift{g_bodyWallOffset.x,0,g_bodyWallOffset.z};
    const Vec physicalEye=Sub(eye,wallShift);
    Vec followerGoal=Sub(Add(nativeRoot,headDelta),wallShift),followerNative=nativeRoot;
    if(Cfg().firstPersonMovementStabilization && g_groundEye.valid) {
        // Center the standing footprint under the eye in the BODY's facing.
        // Keeping this offset in artificial yaw leaves the hips on the old
        // side of the eye when the player physically turns around.
        const float bodyYaw=g_bodyFollower.valid ? g_bodyFollower.yaw : nativeHeading;
        const auto standingOffset=locomotion::Rotate(g_groundEye.bodyLocal,bodyYaw);
        followerGoal={physicalEye.x-standingOffset.x,simulationRoot.y,physicalEye.z-standingOffset.z};
        followerNative=simulationRoot;
    }
    if(grip) g_bodyFollower.Reset();
    else g_bodyFollower.Update(VR().PoseSerial(),TurnTime(),g_headingBase,headYaw,facing,
        Scale(followerGoal,1/scale),Scale(followerNative,1/scale),moving,BodyFollowSettings(),Scale(physicalEye,1/scale),handsForward,
        Cfg().firstPersonMovementStabilization && g_groundEye.valid);
    const float heading=grip || !g_bodyFollower.valid ? nativeHeading : g_bodyFollower.yaw;
    const Basis body{{{std::cos(heading),0,std::sin(heading)},{0,1,0},{-std::sin(heading),0,std::cos(heading)}}};
    Frame inverseBody{};Inverse({body,{}},inverseBody);
    vr::HmdMatrix34_t head{};if(!VR().HeadPose(head)) return false;
    const Basis headRotation=Multiply(ControllerBasis(head.m,g_headingBase),inverseBody.basis);
    const Vec forward{std::sin(heading),0,std::cos(heading)};
    if(grip) {
        if(!firstperson::SolveBody(native,headDelta,headRotation,forward,scale,posed)) return false;
    } else {
        const int joint=Cfg().firstPersonJoint;
        if(joint<0 || joint>=15) return false;
        // Eye, anchor and controller targets all use this render camera frame.
        // Do not mix gameplay integer joint queries into visual placement.
        if(joint==14 && g_bodyFollower.valid) {
            if(mainBody && (g_bodySizeObject!=object ||
               (g_bodySize.valid && std::fabs(g_bodySize.units-scale)>.01f))) {
                g_bodySize.Reset();g_bodySizeObject=object;
            }
            // Remove only physical height displacement from the eye. The
            // comfortable scene-camera height stays fixed during calibration.
            if(Cfg().firstPersonBodyScale==0 && !g_bodySize.valid && mainBody && state==2 &&
               !g_autoBodyCalibration.pending && !g_mountBodyTransition.active &&
               CanTurnBody(item) && !UseLandingCamera(item) && !g_stanceEye.transition &&
               std::fabs(VR().FirstPersonHeightDrop())<.06f &&
               g_bodySize.Capture(native,anchorInPalette,
                   anchorInPalette.y+headDelta.y-VR().FirstPersonVerticalOffset()*scale,scale)) {
                g_torsoPose.Reset();
                LogF("firstperson: standing body scale=%.3f (eye/floor fit; fixed until recenter or model change)",g_bodySize.factor);
            }
            const float bodyScale=Cfg().firstPersonBodyScale>0 ? Cfg().firstPersonBodyScale :
                g_bodySize.valid ? g_bodySize.factor : 1.f;
            pistolBodyScale=bodyScale;
            Frame sized[15]{};firstperson::ScaleBody(native,bodyScale,sized);
            Frame inverseHead{};Inverse(native[14],inverseHead);
            const Vec sizedAnchor=Transform(sized[14],Transform(inverseHead,anchorInPalette));
            Vec rootOffset=Add(Sub(Scale(g_bodyFollower.root,scale),nativeRoot),wallShift);rootOffset.y=0;
            const bool grounded=CanFitStandingBody(item) || CanUseCrouchCamera(item);
            const bool standing=ordinaryLanding || ((state==2 || state==12) && !UseLandingCamera(item));
            const float upright=standing ? firstperson::StandingPostureWeight(VR().FirstPersonPostureDrop()) : 0;
            if(!firstperson::SolveFollowingBody(sized,sizedAnchor,Add(anchorInPalette,headDelta),
                Wrap(heading-nativeHeading),headRotation,forward,scale,rootOffset,
                firstperson::BodyChestTwist(Wrap(headYaw-heading),g_bodyFollower.supportedHands),
                grounded,g_bodyFollower.phase,standing ? 0.f : g_bodyFollower.stepWeight,posed,upright)) return false;
        } else if(!firstperson::SolveBodyAtEye(native,joint,anchorInPalette,Add(anchorInPalette,headDelta),
            Wrap(heading-nativeHeading),headRotation,forward,posed)) return false;
    }
    Frame filtered[15]{};
    for(int j=0;j<15;++j) { filtered[j]=posed[j];filtered[j].origin=Scale(posed[j].origin,1/scale); }
    const Basis referenceYaw=firstperson::RotationMatrix(firstperson::AxisRotation({0,1,0},g_headingBase));
    // Standing contacts follow intentional footprint travel and turns. The
    // follower already rejects subtle HMD motion; pinning this reference to
    // the simulation root would freeze a completed roomscale follow again.
    const Basis stanceYaw=firstperson::RotationMatrix(firstperson::AxisRotation({0,1,0},heading));
    const Vec stanceRoot=!grip && g_bodyFollower.valid ?
        Vec{g_bodyFollower.root.x*scale+wallShift.x,simulationRoot.y,g_bodyFollower.root.z*scale+wallShift.z} : simulationRoot;
    const Frame reference{stanceYaw,Scale(Sub(stanceRoot,cameraOffset),1/scale)};
    const Frame eyeReference{referenceYaw,Scale(Sub(eye,cameraOffset),1/scale)};
    const bool quiet=ordinaryLanding || ((state==2 || state==12) && !UseLandingCamera(item));
    const Vec animationRoot=Sub(simulationRoot,cameraOffset);
    const Vec contacts[2]={
        firstperson::YawVector(Scale(Sub(native[3].origin,animationRoot),1/scale),-nativeHeading),
        firstperson::YawVector(Scale(Sub(native[6].origin,animationRoot),1/scale),-nativeHeading)};
    if(!g_torsoPose.Update(VR().PoseSerial(),TurnTime(),reference,eyeReference,quiet,grip,filtered,forward,
        quiet,VR().FirstPersonPostureDrop(),contacts)) return false;
    for(int j=0;j<15;++j) { posed[j]=filtered[j];posed[j].origin=Scale(filtered[j].origin,scale); }
    static uint64_t nextStanceLog=0;
    if(g_bodySkinScope && Cfg().firstPersonDriftLog && GetTickCount64()>=nextStanceLog &&
       (*reinterpret_cast<const uint32_t*>(item+off::item_mesh_bits)&0x49)==0x49) {
        nextStanceLog=GetTickCount64()+5000;
        LogF("firstperson: body stance state=%d posture=%.3fm idle=%d recovery=%d hands=%d planted=%d follow=%.2f gait=%.2f knee-bend=(%.1f,%.1f)deg",
            state,VR().FirstPersonPostureDrop(),quiet,g_torsoPose.recovering,g_bodyFollower.supportedHands,g_torsoPose.planted,g_bodyFollower.stepWeight,
            quiet ? 0.f : g_bodyFollower.stepWeight,
            firstperson::KneeBendDegrees(posed,1),firstperson::KneeBendDegrees(posed,4));
    }
    const int weaponStatus=*Ptr<int16_t>(g_boundDll->lara+2);
    const bool nativeWeaponArms=(weaponStatus==2 || weaponStatus==3) && !MotionReady();
    for(int hand=0;!nativeWeaponArms && hand<2;++hand) {
        const int first=hand ? 8 : 11;
        Frame chain[3]={posed[first],posed[first+1],posed[first+2]},changes[3]{},target=native[first+2];
        if(!grip) {
            GunBasis controller{};if(!BuildControllerWrist(hand,target,controller)) return false;
            // All tracked hands share the torso/legs' actual render origin.
            // Animated integer wrist queries must not reintroduce frame noise
            // for unarmed hands or other equipped weapons.
            target.origin=Sub(target.origin,cameraOffset);
        }
        const float side=hand ? 1.f : -1.f;
        Vec pole{side*.6f*std::cos(heading)-.25f*std::sin(heading),1.f,
                 -side*.6f*std::sin(heading)-.25f*std::cos(heading)};
        if(grip) pole=Sub(native[first+1].origin,native[first].origin);
        const Frame armBind[3]={bind[first],bind[first+1],bind[first+2]};
        if(!grip && MotionReady() && CurrentMotionWeapon()==1 &&
           !firstperson::PistolArmReference(posed[7],bind[7],armBind,pistolBodyScale,chain)) return false;
        auto roll=g_pistolBodyScope ? PistolRollSeed(hand,item) : firstperson::ArmRollContinuity{};
        if(grip ? !firstperson::SolveArm(chain,target,pole,changes,nullptr,nullptr,false)
                : !firstperson::SolveTrackedArm(chain,target,pole,armBind,changes,true,g_pistolBodyScope ? &roll : nullptr)) return false;
        if(g_pistolBodyScope) {
            g_pistolRoll.current[hand]=roll;
            g_pistolRoll.minConfidence[hand]=std::min(g_pistolRoll.minConfidence[hand],roll.confidence);
        }
        for(int i=0;i<3;++i) posed[first+i]=Multiply(changes[i],chain[i]);
    }
    if(CurrentMotionWeapon()==1 && !grip) {
        // Main body owns the blend; attachments consume its published arm pose.
        if(g_bodySkinScope && (*reinterpret_cast<const uint32_t*>(item+off::item_mesh_bits)&0x49)==0x49) {
            const bool physical=PhysicalHolsterOwnsWeapon();
            // Physical grabs stay exactly controller-driven. Record the live
            // pose without blending so a later button equip can start smoothly.
            if(physical) g_pistolAnimation.Reset();
            g_pistolAnimation.Apply(item,physical ? 0 : weaponStatus,VR().PoseSerial(),TurnTime(),posed);
        }
    } else g_pistolAnimation.Reset();
    // Validate every correction before touching a palette. Helpers sharing a
    // native joint receive the same transform, including nonidentity bindings.
    for(int joint=0;joint<15;++joint) {
        Frame inverse{};if(!Inverse(original[joint],inverse)) return false;
        correction[joint]=Multiply(posed[joint],inverse);
        if(!firstperson::Finite(correction[joint])) return false;
    }
    for(int i=0;i<count;++i) {
        const int joint=mapping ? mapping[i] : i;
        if(joint>=0 && joint<15) WriteRows(Multiply(correction[joint],ReadRows(joints+i*12)),joints+i*12);
    }
    if(g_pistolBodyScope) {
        for(int joint=8;joint<=13;++joint) g_pistolArmPose.joint[joint-8]=posed[joint];
        g_pistolArmPose.bindReference=true;
        g_pistolArmSolveReady=true;
    }
    return true;
}

// Pose rendered arms without changing simulation joints. Shadow palettes use
// their own camera-relative origin and must not inherit eye-view body fitting.
bool ApplyArmIK(uint8_t* item, float* joints, int count, bool shadow=false) {
    const bool armed=!shadow && MotionReady();
    if ((!armed && !UnarmedIKReady()) || count<15 || count>33) return false;
    const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
    if (object<0) return false;
    const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
    const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int32_t*>(geom+28);
    const auto* mapping=bones>0 ? *reinterpret_cast<const int32_t* const*>(geom+48) : nullptr;
    const auto* poses=bones>0 ? *reinterpret_cast<const float* const*>(geom+72) : nullptr;
    if (bones>0 && (bones!=count || !mapping || !poses)) return false;
    motiongun::Frame correction[6]{};
    motiongun::Frame solvedArms[6]{};
    bool solved[2]{};
    bool bindReference[2]{};
    for (int hand=0;hand<2;++hand) {
        const int first=hand ? 8 : 11;
        motiongun::Frame native[3]{};
        bool present[3]{};int reference=-1;
        for (int segment=0;segment<3;++segment) {
            motiongun::Frame inverse{};
            present[segment]=ReadIKJoint(obj,joints,count,bones,mapping,poses,
                first+segment,native[segment]) && motiongun::Inverse(native[segment],inverse);
            if (present[segment]) reference=segment;
        }
        // Attachment palettes can contain only forearm/wrist bones. Body
        // palettes can omit the separate hands. Recover missing chain points
        // from native animation in the same camera-relative frame; never
        // require an unrelated mesh to contain a complete arm skeleton.
        if (reference<0) continue;
        if (!present[0] || !present[1] || !present[2]) {
            const auto worldReference=JointPoint(item,first+reference,0,0,0);
            for(int segment=0;segment<3;++segment) if(!present[segment])
                native[segment]={firstperson::IdentityBasis(),Add(native[reference].origin,
                    Sub(JointPoint(item,first+segment,0,0,0),worldReference))};
        }
        motiongun::Frame target{}; GunBasis controller{};
        if (!BuildControllerWrist(hand,target,controller)) {
            // Single weapons require only the right controller. Losing the
            // other controller must not detach the tracked right wrist.
            if (armed && hand==0 && !motiongun::DualWeapon(CurrentMotionWeapon())) continue;
            return false;
        }
        // Native palettes are camera-relative. Convert the shared gun world
        // target using the native wrist and undo the visible body's eye fit.
        const auto trackedOrigin=target.origin;
        const auto nativeWrist=JointPoint(item,first+2,0,0,0);
        const auto& body=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
        if (std::fabs(nativeWrist.x-body.x_pos)>4096 ||
            std::fabs(nativeWrist.y-body.y_pos)>4096 ||
            std::fabs(nativeWrist.z-body.z_pos)>4096) return false;
        target.origin=Add(native[2].origin,Sub(target.origin,nativeWrist));
        if (!shadow && BodyVisualOffsetApplies(item)) {
            const auto offset=BodyRenderOffset();
            target.origin=Sub(target.origin,{offset.x,0,offset.z});
        }
        if(armed && CurrentMotionWeapon()==1 && g_pistolBodyScope) {
            motiongun::Frame head{},inverse{};
            if(ReadIKJoint(obj,joints,count,bones,mapping,poses,14,head) && motiongun::Inverse(head,inverse)) {
                const auto offset=BodyRenderOffset();
                if(BodyVisualOffsetApplies(item)) head.origin=Sub(head.origin,{offset.x,0,offset.z});
                target.origin=Sub(trackedOrigin,Sub(JointPoint(item,14,0,0,0),head.origin));
            }
        }
        const float heading=Radians(reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos)->y_rot);
        const float side=hand ? 1.f : -1.f;
        const GunVec pole{side*.6f*std::cos(heading)-.25f*std::sin(heading),
                          1.f,-side*.6f*std::sin(heading)-.25f*std::cos(heading)};
        motiongun::Frame bind[3]{};
        // Transport roll across the elbow using authored upper/forearm axes.
        // Forearm-only attachments need no absent upper-arm bind; their
        // forearm/wrist result remains identical to the complete body chain.
        for(int segment=present[0] ? 0 : 1;segment<3;++segment)
            if(!ReadIKBind(obj,count,bones,mapping,poses,first+segment,bind[segment])) return false;
        motiongun::Frame chain[3]={native[0],native[1],native[2]};
        if(armed && CurrentMotionWeapon()==1 && g_pistolBodyScope) {
            motiongun::Frame torso{},torsoBind{};
            if(ReadIKJoint(obj,joints,count,bones,mapping,poses,7,torso) &&
               ReadIKBind(obj,count,bones,mapping,poses,7,torsoBind)) {
                if(!ReadIKBind(obj,count,bones,mapping,poses,first,bind[0]) ||
                   !firstperson::PistolArmReference(torso,torsoBind,bind,1.f,chain)) return false;
                bindReference[hand]=true;
            }
        }
        motiongun::Frame changes[3]{};
        auto roll=bindReference[hand] ? PistolRollSeed(hand,item) : firstperson::ArmRollContinuity{};
        if (!firstperson::SolveTrackedArm(chain,target,pole,bind,changes,present[0],bindReference[hand] ? &roll : nullptr)) return false;
        if(bindReference[hand]) {
            g_pistolRoll.current[hand]=roll;
            g_pistolRoll.minConfidence[hand]=std::min(g_pistolRoll.minConfidence[hand],roll.confidence);
        }
        for(int i=0;i<3;++i) {
            if(!bindReference[hand]) {
                correction[hand*3+i]=changes[i];
                continue;
            }
            motiongun::Frame inverse{};if(!motiongun::Inverse(native[i],inverse)) return false;
            correction[hand*3+i]=motiongun::Multiply(motiongun::Multiply(changes[i],chain[i]),inverse);
        }
        for(int i=0;i<3;++i) solvedArms[first-8+i]=motiongun::Multiply(correction[hand*3+i],native[i]);
        solved[hand]=true;
    }
    // Commit only after both arms solve. Duplicate HD helper bones mapped to
    // a segment receive the same correction, preserving blended vertices.
    for (int i=0;i<count;++i) {
        const int joint=mapping ? mapping[i] : i;
        const int index=joint>=11 && joint<=13 ? joint-11 :
                        joint>=8 && joint<=10 ? joint-8+3 : -1;
        if (index>=0 && solved[index/3]) motiongun::WriteRows(motiongun::Multiply(correction[index],
            motiongun::ReadRows(joints+i*12)),joints+i*12);
    }
    if(!shadow && g_pistolBodyScope && solved[0] && solved[1]) {
        for(int i=0;i<6;++i) g_pistolArmPose.joint[i]=solvedArms[i];
        g_pistolArmPose.bindReference=bindReference[0] && bindReference[1];
        g_pistolArmSolveReady=true;
    }
    return solved[0] || solved[1];
}

// Ladder eyes use the short interaction anchor to stay outside the wall.
// Move only the upper body behind that eye; keep the pelvis/feet authored and
// reconnect both arms to their original rung grips, without controller IK.
bool ApplyLadderTorsoClearance(uint8_t* item,float* joints,int count) {
    using namespace motiongun;
    if (!locomotion::IsLadderState(*reinterpret_cast<const int16_t*>(item+off::item_anim_state)) ||
        (g_ladderTorsoOffset.x==0 && g_ladderTorsoOffset.z==0) || count<15 || count>33) return false;
    const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
    if(object<0) return false;
    const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
    const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int32_t*>(geom+28);
    const auto* mapping=bones>0 ? *reinterpret_cast<const int32_t* const*>(geom+48) : nullptr;
    const auto* poses=bones>0 ? *reinterpret_cast<const float* const*>(geom+72) : nullptr;
    if(bones>0 && (bones!=count || !mapping || !poses)) return false;
    const Frame shift{firstperson::IdentityBasis(),{g_ladderTorsoOffset.x,0,g_ladderTorsoOffset.z}};
    Frame corrections[6]{};
    for(int hand=0;hand<2;++hand) {
        const int first=hand ? 8 : 11;
        Frame original[3]{},chain[3]{},changes[3]{};
        bool present[3]{};int reference=-1;
        for(int i=0;i<3;++i) {
            Frame inverse{};
            present[i]=ReadIKJoint(obj,joints,count,bones,mapping,poses,first+i,original[i]) &&
                Inverse(original[i],inverse);
            if(present[i]) reference=i;
        }
        if(reference<0) continue; // This attachment may contain only one arm.
        if(!present[0] || !present[1] || !present[2]) {
            const auto world=JointPoint(item,first+reference,0,0,0);
            for(int i=0;i<3;++i) if(!present[i])
                original[i]={firstperson::IdentityBasis(),Add(original[reference].origin,
                    Sub(JointPoint(item,first+i,0,0,0),world))};
        }
        // Keep the chest clear, but do not drag shoulders beyond the arms'
        // native reach. Moving a nearly straight arm back used to stretch it.
        const Frame armShift{shift.basis,Scale(shift.origin,
            firstperson::ArmClearanceFraction(original,shift.origin))};
        for(int i=0;i<3;++i) chain[i]=Multiply(armShift,original[i]);
        if(!firstperson::SolveArm(chain,original[2],Sub(original[1].origin,original[0].origin),
                                 changes,nullptr,nullptr,false)) return false;
        for(int i=0;i<3;++i) corrections[hand*3+i]=Multiply(changes[i],armShift);
    }
    for(int i=0;i<count;++i) {
        const int joint=mapping ? mapping[i] : i;
        if(joint<7 || joint>14) continue;
        const float* b=joints+i*12;
        if(b[0]==0 && b[1]==0 && b[2]==0 && b[4]==0 && b[5]==0 && b[6]==0 &&
           b[8]==0 && b[9]==0 && b[10]==0) continue;
        const Frame& correction=joint>=11 && joint<=13 ? corrections[joint-11] :
            joint>=8 && joint<=10 ? corrections[joint-8+3] : shift;
        WriteRows(Multiply(correction,ReadRows(joints+i*12)),joints+i*12);
    }
    return true;
}

int32_t __cdecl Detour_GetJoints(uint8_t* item, float* joints, int32_t pass) {
    g_renderHandJoint = -1;
    g_bodySkinReady=false;
    const int32_t count = g_hGetJoints.Original<Fn_GetJoints>()(item, joints, pass);
    // Keep the native shadow body/root and all visibility/physics state.
    // Only free unarmed arms follow the controllers; their targets are rebased
    // through the light-pass wrist, not the scene-camera or eye-fit offset.
    if (DrawingNativeShadow()) {
        if(joints && item && g_boundDll && g_boundBase &&
           item==*Ptr<uint8_t*>(g_boundDll->laraItem) && UnarmedIKReady())
            ApplyArmIK(item,joints,count,true);
        return count;
    }
    if (g_renderArm==1) { g_weaponLaser.item=nullptr;g_weaponAimBinding.item=nullptr; }
    if (g_renderArm<0 && joints && count>0 && count<=33 && g_active &&
        g_boundDll && item && item==g_headingItem &&
        item==*Ptr<uint8_t*>(g_boundDll->laraItem)) {
        motiongun::Vec holsterCamera{};
        const bool captureHolsters=HolsterCameraOffset(item,joints,count,holsterCamera);
        if (BodyVisualOffsetApplies(item))
            stabilization::OffsetBodyPalette(joints,count,BodyRenderOffset());
        ApplyLadderTorsoClearance(item,joints,count);
        const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
        if (g_bodySkinScope && object>=0) {
            const auto* geom=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride+off::object_geom;
            const int bones=*reinterpret_cast<const int32_t*>(geom+28);
            const auto* mapping=bones>0 ? *reinterpret_cast<const int32_t* const*>(geom+48) : nullptr;
            if (bones<=0 || (bones==count && mapping)) {
                g_bodySkinMask=firstperson::SkinJointMask(
                    *reinterpret_cast<const uint32_t*>(item+off::item_mesh_bits),mapping,count);
                g_bodyWristSeamMask=g_bodyWristSeamScope ?
                    firstperson::SkinJointMask(firstperson::HandMeshBits,mapping,count)&~g_bodySkinMask : 0;
                std::memcpy(g_bodySkinPalette,joints,count*12*sizeof(float));
                g_bodySkinCount=count; g_bodySkinReady=true;
            }
        }
        if(g_pistolBodyScope) { g_pistolArmSolveReady=false;g_pistolArmPose.valid=false; }
        bool solved=g_pistolForearmScope && PistolBodyArmPose(item,joints,count,pass,false);
        if(g_pistolForearmScope && Cfg().firstPersonDriftLog) {
            static uint64_t nextPistolPoseLog=0;
            const uint64_t now=GetTickCount64();
            if(now>=nextPistolPoseLog) {
                nextPistolPoseLog=now+5000;
                LogF("firstperson: pistol forearms source=%s joints=%d full-body=%d reference=%s roll-min=(%.4f,%.4f) roll-recovery=(%d,%d)",
                     solved ? "body-IK" : "attachment-fallback",count,Cfg().firstPersonFullBodyIK,
                     solved && g_pistolArmPose.bindReference ? "model-bind" : "animated",
                     g_pistolRoll.minConfidence[0],g_pistolRoll.minConfidence[1],
                     g_pistolRoll.current[0].recovering,g_pistolRoll.current[1].recovering);
                g_pistolRoll.minConfidence[0]=g_pistolRoll.minConfidence[1]=1.f;
            }
        }
        if(!solved) solved=ApplyFullBodyIK(item,joints,count);
        if(!solved && (MotionReady() || UnarmedIKReady())) {
            if ((solved=ApplyArmIK(item,joints,count))) ++g_armIKSolved;
            else ++g_armIKFailed;
        }
        if(solved && g_pistolBodyScope) PistolBodyArmPose(item,joints,count,pass,true);
        if (g_bodySkinReady) std::memcpy(g_bodyRenderPalette,joints,count*12*sizeof(float));
        if(solved) TraceArmGlance(item,joints,count,pass);
        if (captureHolsters) CaptureHolsterAnchors(item,joints,count,holsterCamera);
    }
    if (g_renderArm < 0 || count < 15 || !joints || !MotionReady() ||
        item != *Ptr<uint8_t*>(g_boundDll->laraItem)) return count;
    GunPose gun{};
    if (!BuildGunPose(g_renderArm, gun)) return count;
    const int nativeJoint = g_renderArm ? 10 : 13;
    const int object = *reinterpret_cast<const int16_t*>(item + off::item_object);
    if (object < 0) return count;
    const uint8_t* obj = Ptr<uint8_t>(g_boundDll->objects) +
        object * off::object_stride;
    const uint8_t* geom = obj + off::object_geom;
    const int bones = *reinterpret_cast<const int32_t*>(geom + 28);
    int pivot = nativeJoint;
    motiongun::Frame inverseBind{};
    if (bones > 0) {
        if (bones != count || count > 256) return count;
        const auto mapping = *reinterpret_cast<const int32_t* const*>(geom + 48);
        const auto poses = *reinterpret_cast<const float* const*>(geom + 72);
        if (!mapping || !poses) return count;
        pivot = -1;
        for (int i=0; i<count; ++i)
            if (mapping[i] == nativeJoint) { pivot=i; break; }
        if (pivot < 0) return count;
        inverseBind = motiongun::HdInverseBind(
            motiongun::ReadRows(poses + pivot*12));
    } else {
        if (count > 33 || pivot >= count) return count;
        // object_info::BindMatrix is column-major mat4[33].
        const float* bind = reinterpret_cast<const float*>(obj + 1676) + pivot*16;
        for (int row=0; row<3; ++row)
            for (int col=0; col<3; ++col)
                inverseBind.basis.r[row][col]=bind[col*4+row];
        inverseBind.origin={bind[12],bind[13],bind[14]};
    }
    const auto palette = motiongun::ReadRows(joints + pivot*12);
    motiongun::Frame bind{}, correction{};
    if (!motiongun::Inverse(inverseBind, bind)) return count;
    const auto wrist = motiongun::Multiply(palette, bind);
    motiongun::Frame desired{gun.desired,
        Add(wrist.origin, Sub(gun.trackedHand, gun.nativeHand))};
    // Use the solved wrist across both pistol forearm/hand seams. Re-deriving
    // its origin from the independently animated gun palette adds wobble back.
    if(PistolBodyPoseMatches(item,pass))
        desired=g_pistolArmPose.joint[g_renderArm ? 2 : 5];
    if (!motiongun::PaletteCorrection(palette, inverseBind, desired, correction))
        return count;
    if (g_renderArm==1 && g_boundDll->game==1 && CurrentMotionWeapon()==5) {
        if (g_hkScopeBindItem!=item)
            Log("HK scope: captured wrist binding from right-hand HK draw");
        g_hkScopeInverseBind=inverseBind;
        g_hkScopeBindItem=item;
    }
    // MaskJoints runs after us. Move the whole palette, including HD helper
    // bones, so weighted hand/gun vertices receive the same rigid transform.
    for (int joint=0; joint<count; ++joint) {
        float* bone=joints + joint*12;
        motiongun::WriteRows(motiongun::Multiply(correction,
            motiongun::ReadRows(bone)), bone);
    }
    if (g_renderArm==1 && g_unarmedHandDll && g_unarmedHandDll->hands) {
        // Match the actual drawn mesh against the native hand bank. TR5 uses
        // weapon slot 2 for BOTH revolver and Desert Eagle, so game/weapon IDs
        // alone cannot select the right aperture. Face/body draws replace geom.
        const auto meshPointer=*reinterpret_cast<const void* const*>(geom+off::geom_mesh);
        const auto* hands=Ptr<uint8_t>(g_unarmedHandDll->hands);
        const weaponlaser::Model* model=nullptr;
        const int weapon=CurrentMotionWeapon(),game=g_boundDll->game;
        if(meshPointer) for(int bank:{0,30,60}) for(const auto& candidate:weaponlaser::models) {
            if(weaponlaser::Find(candidate.handIndex,game,weapon) && meshPointer==
               *reinterpret_cast<const void* const*>(hands+(bank+candidate.handIndex)*off::geom_stride+off::geom_mesh))
                model=&candidate;
        }
        float rotation[3][3]{},camera[3]{};
        if(model && CameraViewFrame(rotation,camera)) {
            const auto mesh=motiongun::ReadRows(joints+pivot*12);
            const auto origin=Add(motiongun::Transform(mesh,model->front),{camera[0],camera[1],camera[2]});
            const auto direction=motiongun::Transform(mesh.basis,Sub(model->front,model->rear));
            if(model->HasLaser()) {
                g_weaponLaser.origin=origin;g_weaponLaser.direction=direction;
                g_weaponLaser.base=g_boundBase;
                g_weaponLaser.serial=VR().PoseSerial();
                g_weaponLaser.game=game;g_weaponLaser.weapon=weapon;
                g_weaponLaser.item=item;
            }
            motiongun::Frame toWrist{};
            if(motiongun::Inverse({gun.desired,gun.trackedHand},toWrist)) {
                g_weaponAimBinding={item,g_boundBase,game,weapon,AppState(drva::app_off::level),
                    Transform(toWrist,origin),Transform(toWrist.basis,direction)};
            }
        }
    }
    ++g_motionArmDraws[g_renderArm];
    g_renderHandJoint = pivot;
    return count;
}

void __cdecl Detour_SetGunFlash(int32_t weapon, int32_t left) {
    const auto original = g_hSetGunFlash.Original<Fn_SetGunFlash>();
    const uint64_t caller = reinterpret_cast<uint64_t>(_ReturnAddress()) - g_boundBase;
    const int hand = g_motionDll && MotionWeaponSupported(weapon)
        ? (caller == g_motionDll->rightFlashReturn && !left ? 1 :
           caller == g_motionDll->leftFlashReturn && left == 1 ? 0 : -1) : -1;
    GunPose gun{};
    if (hand < 0 || !BuildGunPose(hand, gun)) { original(weapon,left); return; }
    float* fm = *Ptr<float*>(g_motionDll->floatMatrixPtr);
    int32_t* im = *Ptr<int32_t*>(g_boundDll->phdMxptr);
    if (!fm || !im) { original(weapon,left); return; }
    float savedFloat[12];
    int32_t savedInt[12];
    std::memcpy(savedFloat,fm,sizeof(savedFloat));
    std::memcpy(savedInt,im,sizeof(savedInt));
    const auto native = motiongun::ReadRows(fm);
    const motiongun::Frame desired{gun.desired,
        Add(native.origin, Sub(gun.trackedHand,gun.nativeHand))};
    motiongun::WriteRows(desired,fm);
    for (int i=0; i<12; ++i)
        im[i]=int32_t(std::lround(fm[i]*16384.0f));
    // Native SetGunFlash applies the same HD MuzzleLocal offset, flash spin
    // and weapon-specific orientation. Never touch enemy or mirror callers.
    original(weapon,left);
    std::memcpy(fm,savedFloat,sizeof(savedFloat));
    std::memcpy(im,savedInt,sizeof(savedInt));
}

bool AssistGunShot(const GunPose& gun, void* target, motiongun::Vec& direction) {
    // A magnified reticle must not silently steer onto an enemy off-axis.
    if (FirstPersonHKScopeAiming()) return false;
    if (!target || !g_headingItem || !g_motionDll ||
        *reinterpret_cast<const int16_t*>(static_cast<uint8_t*>(target)+off::item_hit_points)<=0)
        return false;
    using FindTarget = void (__cdecl*)(void*, ShotVector*);
    using LineOfSight = int32_t (__cdecl*)(ShotVector*, ShotVector*);
    using GetFloor = void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
    ShotVector end{};
    reinterpret_cast<FindTarget>(g_boundBase+g_motionDll->findTargetPoint)(target,&end);
    motiongun::Vec candidate{};
    if (!motiongun::AssistedDirection(gun.muzzle,gun.direction,
            {float(end.x),float(end.y),float(end.z)},candidate)) return false;
    ShotVector start{int32_t(std::lround(gun.muzzle.x)),
        int32_t(std::lround(gun.muzzle.y)),int32_t(std::lround(gun.muzzle.z)),
        *reinterpret_cast<const int16_t*>(g_headingItem+off::item_room),0};
    reinterpret_cast<GetFloor>(g_boundBase+g_boundDll->getFloor)(
        start.x,start.y,start.z,&start.room);
    if (!reinterpret_cast<LineOfSight>(g_boundBase+g_motionDll->lineOfSight)(&start,&end))
        return false;
    direction=candidate;
    return true;
}

uint8_t* SelectGunTarget(const GunPose& gun, motiongun::Vec& ray) {
    using namespace motiongun;
    if (!g_motionDll) return nullptr;
    const auto& d=*g_motionDll;
    if (!d.nextItemActive || !d.getSpheres || !d.findTargetPoint || !d.lineOfSight) return nullptr;
    auto* items=*Ptr<uint8_t*>(d.items);
    auto* lara=*Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!items || !lara) return nullptr;
    struct Sphere { int32_t x,y,z,r; };
    using Fn_Spheres=int32_t (__cdecl*)(uint8_t*,Sphere*,int32_t);
    using Fn_TargetPoint=void (__cdecl*)(uint8_t*,ShotVector*);
    using Fn_LOS=int32_t (__cdecl*)(ShotVector*,ShotVector*);
    using Fn_Floor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
    ShotVector source{int32_t(std::lround(gun.muzzle.x)),
                      int32_t(std::lround(gun.muzzle.y)),
                      int32_t(std::lround(gun.muzzle.z)),
                      *reinterpret_cast<int16_t*>(lara+off::item_room),0};
    reinterpret_cast<Fn_Floor>(g_boundBase+g_boundDll->getFloor)(source.x,source.y,source.z,&source.room);
    auto visible=[&](Vec point,int16_t room) {
        ShotVector start=source;
        ShotVector end{int32_t(std::lround(point.x)),int32_t(std::lround(point.y)),
                       int32_t(std::lround(point.z)),room,0};
        return reinterpret_cast<Fn_LOS>(g_boundBase+d.lineOfSight)(&start,&end)!=0;
    };
    uint8_t* direct=nullptr;
    uint8_t* assisted=nullptr;
    Vec assistRay{};
    float nearest=20480.f, bestCos=-1.f, bestDistance=8192.f*8192.f;
    const bool allowAssist=!gun.meshAligned && !FirstPersonHKScopeAiming();
    int index=*Ptr<int16_t>(d.nextItemActive);
    for (int visited=0; index>=0 && index<4096 && visited<4096; ++visited) {
        auto* item=items+index*off::item_stride;
        index=*reinterpret_cast<int16_t*>(item+off::item_next_active);
        if (item==lara || *reinterpret_cast<int16_t*>(item+off::item_hit_points)<=0) continue;
        const auto& pos=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
        const Vec delta=Sub({float(pos.x_pos),float(pos.y_pos),float(pos.z_pos)},gun.muzzle);
        if (Dot(delta,delta)>20480.f*20480.f) continue;
        const int object=*reinterpret_cast<int16_t*>(item+off::item_object);
        if (object<0) continue;
        const auto* definition=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
        // Active items include dropped flares, projectiles and effects with
        // positive hit_points. Their item.data is NOT a CREATURE_INFO pointer
        // (the reported flare held 0x40f); HitTarget dereferences it as one.
        // Native enemy targeting uses baddie slots. Our wider ray scan must
        // require a loaded intelligent object before querying combat spheres.
        // World props still receive the ordinary native LOS/breakable path.
        // OBJECT_INFO +80: loaded bit 0, intelligent bit 1 in both TR4/5 PDBs.
        if ((*reinterpret_cast<const uint32_t*>(definition+80)&3u)!=3u) continue;
        const int meshes=*reinterpret_cast<const int16_t*>(definition);
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
        if (!allowAssist || bodyDistance==std::numeric_limits<float>::max() ||
            !AssistedDirection(gun.muzzle,gun.direction,bodyPoint,candidate)) continue;
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

int32_t FireWeaponForHand(int hand, int32_t weapon, void* target, void* extra,
                         const int16_t* aim) {
    const bool longShot=g_longShot && g_longShot->weapon==weapon && hand==1;
    const bool separateTriggers=!longShot && hand>=0 && g_gunTriggers.active && MotionTriggerMode();
    if(hand>=0 && g_active && g_holsters.active && weapon==g_holsters.weapon &&
       (!g_holsters.Has(hand) || g_holsters.hand[hand].blockTrigger ||
        g_holsters.hand[hand].claimed)) return 0;
    // Native return 0 skips ammo/hits, muzzle flash, shell, sound and shot stats.
    // Keep unknown callers and non-motion modes entirely native.
    if (separateTriggers && !g_gunTriggers.pending[hand]) return 0;
    GunPose gun{};
    const int previousHand = g_firingHand;
    bool assisted=false, tracked=false;
    int hpBefore=0;
    if (hand >= 0 && aim && (longShot ? (gun=g_longShot->gun,true) : BuildGunPose(hand, gun))) {
        if (separateTriggers) g_gunTriggers.Consume(hand);
        tracked=true;
        g_firingDirection=gun.direction;
        target=SelectGunTarget(gun,g_firingDirection);
        if (target) hpBefore=*reinterpret_cast<const int16_t*>(
            static_cast<uint8_t*>(target)+off::item_hit_points);
        g_firingHand = hand;
        assisted=motiongun::Dot(motiongun::Sub(g_firingDirection,gun.direction),
            motiongun::Sub(g_firingDirection,gun.direction))>1e-8f;
        // Shotgun randomises each pellet BEFORE FireWeapon. Subtract the
        // pre-volley aim, not that pellet's aim, to retain all native spread.
        g_firingAim[0] = longShot ? g_longShot->baseAim[0] : aim[0];
        g_firingAim[1] = longShot ? g_longShot->baseAim[1] : aim[1];
        g_firingPose.x_pos = int32_t(std::lround(gun.muzzle.x));
        g_firingPose.y_pos = int32_t(std::lround(gun.muzzle.y));
        g_firingPose.z_pos = int32_t(std::lround(gun.muzzle.z));
        g_firingPose.x_rot = gun.pitch;
        g_firingPose.y_rot = gun.yaw;
        if (assisted) {
            constexpr float angleUnits=32768.f/kPi;
            const auto ray=g_firingDirection;
            g_firingPose.y_rot=int16_t(std::lround(std::atan2(ray.x,ray.z)*angleUnits));
            g_firingPose.x_rot=int16_t(std::lround(std::atan2(-ray.y,
                std::sqrt(ray.x*ray.x+ray.z*ray.z))*angleUnits));
        }
        if (!g_motionShots[hand]) {
            LogF("firstperson: Touch %s muzzle=(%d,%d,%d) yaw=%d pitch=%d",
                 hand ? "right" : "left", g_firingPose.x_pos,
                 g_firingPose.y_pos, g_firingPose.z_pos,
                 int(g_firingPose.y_rot), int(g_firingPose.x_rot));
        }
        ++g_motionShots[hand];
    }
    // Do not turn a queued controller shot into a head-aimed shot on pose loss.
    if (separateTriggers && !tracked) return 0;
    const int32_t result = g_hFireWeapon.Original<Fn_FireWeapon>()(
        weapon, target, extra, aim);
    if (tracked && result) {
        g_handFired[hand]=true;
        if (longShot) g_longShot->fired=true;
        else VR().GunShotHaptic(hand);
    }
    if (tracked && g_motionShots[hand]<=100)
        LogF("firstperson: Touch %s shot target=%p assist=%d hp=%d->%d native=%d",
            hand ? "right" : "left",target,int(assisted),hpBefore,
            target ? int(*reinterpret_cast<const int16_t*>(
                static_cast<uint8_t*>(target)+off::item_hit_points)) : 0,result);
    g_firingHand = previousHand;
    return result;
}

// One trigger request owns the complete native firing operation, not one
// FireWeapon call (a shotgun operation contains six of those).
template<class Fire>
void RunLongShot(int weapon, bool projectile, Fire fire) {
    if (CurrentMotionWeapon()!=weapon || !MotionTriggerMode()) { fire(nullptr); return; }
    if(g_holsters.active && weapon==g_holsters.weapon && (!g_holsters.Has(1) || g_holsters.hand[1].claimed ||
       g_holsters.hand[1].blockTrigger)) return;
    if (g_gunTriggers.active && !g_opticShot && !g_gunTriggers.pending[1]) return;
    LongShot shot{};
    shot.weapon=weapon; shot.projectile=projectile;
    if (!BuildGunPose(1,shot.gun)) return; // Never silently shoot from the head.
    if (g_gunTriggers.active && !g_opticShot) g_gunTriggers.Consume(1);
    auto* arm=Ptr<uint8_t>(g_boundDll->lara+off::lara_left_arm);
    int16_t saved[3]{};
    std::memcpy(saved,arm+off::arm_lock,sizeof(saved));
    const auto& body=*reinterpret_cast<const PHD_3DPOS*>(g_headingItem+off::item_pos);
    *reinterpret_cast<int16_t*>(arm+off::arm_lock)=1;
    *reinterpret_cast<int16_t*>(arm+off::arm_y_rot)=int16_t(shot.gun.yaw-body.y_rot);
    *reinterpret_cast<int16_t*>(arm+off::arm_x_rot)=
        int16_t(shot.gun.pitch-(projectile ? body.x_rot : 0));
    shot.baseAim[0]=shot.gun.yaw;
    shot.baseAim[1]=shot.gun.pitch;
    const auto* previous=g_longShot;
    g_longShot=&shot;
    fire(&shot);
    if (shot.fired) VR().GunShotHaptic(1);
    g_longShot=previous;
    std::memcpy(arm+off::arm_lock,saved,sizeof(saved));
}

void __cdecl Detour_FireShotgun() {
    RunLongShot(4,false,[](const LongShot*) {
        g_hFireShotgun.Original<void (__cdecl*)()>()();
    });
}

void __cdecl Detour_FireGrenade() {
    RunLongShot(5,true,[](const LongShot*) {
        g_hFireSpecial.Original<void (__cdecl*)()>()();
    });
}

void __cdecl Detour_FireHK(int32_t running) {
    RunLongShot(5,false,[&](const LongShot*) {
        g_hFireSpecial.Original<void (__cdecl*)(int32_t)>()(running);
    });
}

void __cdecl Detour_FireCrossbow(PHD_3DPOS* pose) {
    // TR5's launcher requires a valid native laser/grapple target. Its
    // non-optic null-pose branch is not a usable native firing path.
    if (!pose && g_boundDll && g_boundDll->game==1 && MotionTriggerMode()) {
        g_gunTriggers.Consume(1); // Don't carry an invalid attempt into later optic mode.
        return;
    }
    RunLongShot(6,true,[&](const LongShot* shot) {
        PHD_3DPOS tracked{};
        auto* argument=pose;
        if (shot && pose) {
            tracked=*pose;
            tracked.x_pos=int32_t(std::lround(shot->gun.muzzle.x));
            tracked.y_pos=int32_t(std::lround(shot->gun.muzzle.y));
            tracked.z_pos=int32_t(std::lround(shot->gun.muzzle.z));
            // The native optic LOS has already selected/quantised the valid
            // grapple anchor from our muzzle. Preserve its attachment angle.
            if (!g_opticShot) {
                tracked.x_rot=shot->gun.pitch;
                tracked.y_rot=shot->gun.yaw;
                tracked.z_rot=0;
            }
            argument=&tracked;
        }
        // Keep TR4's null-pose branch: it owns normal ammunition consumption.
        g_hFireCrossbow.Original<void (__cdecl*)(PHD_3DPOS*)>()(argument);
    });
}

uint8_t* __cdecl Detour_FlareDynamic(int32_t x,int32_t y,int32_t z,int32_t falloff,
                                    int32_t red,int32_t green,int32_t blue) {
    auto* light=g_hFlareDynamic.Original<uint8_t* (__cdecl*)(int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int32_t)>()(
        x,y,z,falloff,red,green,blue);
    // A pooled slot can be reused by any effect. Invalidate that ownership even
    // outside the held-flare call; never move a thrown flare or a gun flash.
    for(auto& slot:g_flareLights.slots) if(slot.address==light) slot.address=nullptr;
    if(g_captureFlareLights && light && g_flareLights.count<2)
        g_flareLights.slots[g_flareLights.count++].address=light;
    return light;
}

void UpdateFlareLightPosition() {
    if(!g_flareLights.count) return;
    const double elapsed=TurnTime()-g_flareLights.time;
    if(g_flareLights.item!=g_headingItem || g_flareLights.level!=AppState(drva::app_off::level) ||
       elapsed<0 || elapsed>.15 || !FlareIKReady()) {g_flareLights={};return;}
    motiongun::Frame wrist{};GunBasis controller{};
    if(!BuildControllerWrist(0,wrist,controller)) return;
    const auto tip=Transform(wrist,{11,-64,41});
    for(int i=0;i<g_flareLights.count;++i) {
        auto& slot=g_flareLights.slots[i];
        if(!slot.address) continue;
        // Check before writing: native reset/reuse or a transition invalidates
        // the slot. DLL unload clears this cache before removing the hooks.
        if(std::memcmp(slot.address,slot.snapshot,40) || !slot.address[0x18]) {
            slot.address=nullptr;continue;
        }
        const int32_t position[]={int32_t(std::lround(tip.x)),int32_t(std::lround(tip.y)),
            int32_t(std::lround(i ? g_flareLights.mirrorZSum-tip.z : tip.z))};
        if(!i && !g_loggedFlareLight) {
            const auto* old=reinterpret_cast<const int32_t*>(slot.snapshot);
            LogF("firstperson flare light: native=(%d,%d,%d) tracked tip=(%d,%d,%d); render-time room/object light attached",
                 old[0],old[2],old[4],position[0],position[1],position[2]);
            g_loggedFlareLight=true;
        }
        for(int axis=0;axis<3;++axis) {
            // Both endpoints use this render's tracked pose. Interpolating the
            // 30 Hz animation here would lag behind the controller-driven mesh.
            std::memcpy(slot.address+axis*8,&position[axis],4);
            std::memcpy(slot.address+axis*8+4,&position[axis],4);
        }
        std::memcpy(slot.snapshot,slot.address,40);
    }
}

bool FlareJointPoint(uint8_t* item,PHD_VECTOR* point,int32_t joint,bool previous) {
    if(!g_flareHandFrames || item!=g_headingItem || !point || joint!=13) return false;
    const auto& wrist=previous ? g_flareHandFrames->previous : g_flareHandFrames->current;
    const auto p=Transform(wrist,{float(point->x),float(point->y),float(point->z)});
    *point={int32_t(std::lround(p.x)),int32_t(std::lround(p.y)),int32_t(std::lround(p.z))};
    return true;
}

void __cdecl Detour_FlarePreviousJoint(uint8_t* item,PHD_VECTOR* point,int32_t joint) {
    if(FlareJointPoint(item,point,joint,true)) return;
    g_hFlarePreviousJoint.Original<void (__cdecl*)(uint8_t*,PHD_VECTOR*,int32_t)>()(item,point,joint);
}

void __cdecl Detour_FlareInHand(int32_t age) {
    const auto* saved=g_flareHandFrames;
    g_flareLights={};
    FlareHandFrames frames{};GunBasis controller{};
    if(FlareIKReady() && BuildControllerWrist(0,frames.current,controller)) {
        auto& h=g_flareHandHistory;
        const int level=AppState(drva::app_off::level);
        const double now=TurnTime();
        const bool continuous=h.item==g_headingItem && h.level==level &&
            now>=h.time && now-h.time<.15 && (age==h.age || age==h.age+1);
        frames.previous=continuous ? (age==h.age ? h.frames.previous : h.frames.current) : frames.current;
        h={g_headingItem,level,age,now,frames};
        g_flareHandFrames=&frames;
    } else { g_flareHandHistory={};g_flareHandFrames=nullptr; }
    // Native code keeps age, ignition, burn-out, sparks/light and TR4 mirror
    // reflection. Replace its current AND previous left-wrist queries only
    // inside this call; otherwise light interpolation pulls back to Lara.
    const bool savedCapture=g_captureFlareLights;
    g_captureFlareLights=true;
    g_hFlareInHand.Original<void (__cdecl*)(int32_t)>()(age);
    g_captureFlareLights=savedCapture;
    // Capture even when the simulation-time camera gate is unavailable: the
    // hand/light eligibility is checked again with the actual render camera.
    g_flareLights.item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    g_flareLights.level=AppState(drva::app_off::level);
    g_flareLights.time=TurnTime();
    for(auto& slot:g_flareLights.slots) if(slot.address) std::memcpy(slot.snapshot,slot.address,40);
    if(g_flareLights.count==2 && g_flareLights.slots[0].address && g_flareLights.slots[1].address)
        g_flareLights.mirrorZSum=float(*reinterpret_cast<const int32_t*>(g_flareLights.slots[0].snapshot+16))+
            float(*reinterpret_cast<const int32_t*>(g_flareLights.slots[1].snapshot+16));
    else if(g_flareLights.count==2) g_flareLights={}; // Pool exhausted/reused: no valid mirror plane.
    g_flareHandFrames=saved;
}

void __cdecl Detour_GunJoint(uint8_t* item, PHD_VECTOR* point, int32_t joint) {
    if(FlareJointPoint(item,point,joint,false)) return;
    if (g_longShot && item==g_headingItem && point && joint==10) {
        const auto& gun=g_longShot->gun;
        const auto p=gun.meshAligned ? gun.muzzle : Add(gun.trackedHand,Transform(gun.desired,
            {float(point->x),float(point->y),float(point->z)}));
        *point={int32_t(std::lround(p.x)),int32_t(std::lround(p.y)),int32_t(std::lround(p.z))};
        return;
    }
    g_hGunJoint.Original<void (__cdecl*)(uint8_t*,PHD_VECTOR*,int32_t)>()(item,point,joint);
}

void __cdecl Detour_InitialiseProjectile(int16_t index) {
    if (g_longShot && g_longShot->projectile && g_longGunDll && index>=0) {
        auto* items=*Ptr<uint8_t*>(g_longGunDll->items);
        if (items) {
            auto* item=items+size_t(index)*9416;
            const int object=*reinterpret_cast<int16_t*>(item+off::item_object);
            const int expected=g_boundDll->game==1 ? 0x158 :
                g_longShot->weapon==5 ? 0x16d : 0x168;
            if (object==expected) {
                g_longShot->fired=true;
                auto& pos=*reinterpret_cast<PHD_3DPOS*>(item+off::item_pos);
                const auto muzzle=g_longShot->gun.muzzle;
                if (g_boundDll->game==1) {
                    pos.x_pos=int32_t(std::lround(muzzle.x));
                    pos.y_pos=int32_t(std::lround(muzzle.y));
                    pos.z_pos=int32_t(std::lround(muzzle.z));
                } // TR4 already used the tracked joint and native floor guard.
                auto& room=*reinterpret_cast<int16_t*>(item+off::item_room);
                room=*reinterpret_cast<const int16_t*>(g_headingItem+off::item_room);
                using Floor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
                reinterpret_cast<Floor>(g_boundBase+g_boundDll->getFloor)(
                    pos.x_pos,pos.y_pos,pos.z_pos,&room);
                // InitialiseItem now links the projectile into the correct
                // room and seeds its previous position at the tracked muzzle.
            }
        }
    }
    g_hInitialiseProjectile.Original<void (__cdecl*)(int16_t)>()(index);
}

void __cdecl Detour_RifleHandler(int32_t weapon) {
    g_hRifleHandler.Original<Fn_PistolHandler>()(weapon);
    if (!MotionReady()) return;
    auto* lara=Ptr<uint8_t>(g_boundDll->lara);
    for (uint32_t offset=off::lara_head_y_rot;offset<=off::lara_torso_z_rot;offset+=2)
        *reinterpret_cast<int16_t*>(lara+offset)=0;
}

int32_t FireWeaponForCaller(uint64_t caller,int32_t weapon,void* target,void* extra,
                            const int16_t* aim) {
    // AnimatePistols skips its right call for the revolver/Desert Eagle (2).
    // Its shared left-arm call fires that RIGHT-hand gun, or the left pistol/Uzi.
    const int hand = g_longShot && g_longShot->weapon==weapon ? 1 :
        g_motionDll && (motiongun::DualWeapon(weapon) || weapon==motiongun::Revolver)
        ? (caller == g_motionDll->rightFireReturn && motiongun::DualWeapon(weapon) ? 1 :
           caller == g_motionDll->leftFireReturn ? (weapon==motiongun::Revolver ? 1 : 0) : -1) : -1;
    return FireWeaponForHand(hand,weapon,target,extra,aim);
}

int32_t __cdecl Detour_FireWeapon(int32_t weapon, void* target, void* extra,
                                  const int16_t* aim) {
    return FireWeaponForCaller(reinterpret_cast<uint64_t>(_ReturnAddress())-g_boundBase,
        weapon,target,extra,aim);
}

int32_t GetTargetOnLOSForCaller(uint64_t caller,PHD_VECTOR* source,PHD_VECTOR* dest,
                               int32_t flags,int32_t mode) {
    bool opticCaller=false;
    if (g_longGunHooksReady && g_longGunDll && g_firingHand<0)
        for (auto address:g_longGunDll->opticReturns) opticCaller|=caller==address;
    const int weapon=CurrentMotionWeapon();
    if (opticCaller && (weapon==motiongun::Revolver || weapon==5 || weapon==6) && MotionTriggerMode()) {
        GunPose gun{};
        if (!BuildGunPose(1,gun))
            return g_hGetTargetOnLOS.Original<Fn_GetTargetOnLOS>()(source,dest,flags,0);
        ShotVector start{int32_t(std::lround(gun.muzzle.x)),int32_t(std::lround(gun.muzzle.y)),
            int32_t(std::lround(gun.muzzle.z)),
            *reinterpret_cast<int16_t*>(g_headingItem+off::item_room),0};
        using Floor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
        reinterpret_cast<Floor>(g_boundBase+g_boundDll->getFloor)(start.x,start.y,start.z,&start.room);
        const auto endpoint=Add(gun.muzzle,motiongun::Scale(gun.direction,20000.f));
        ShotVector end{int32_t(std::lround(endpoint.x)),int32_t(std::lround(endpoint.y)),
            int32_t(std::lround(endpoint.z)),start.room,0};
        if (mode && g_gunTriggers.active) mode=g_gunTriggers.Consume(1) ? mode : 0;
        const bool previous=g_opticShot;
        g_opticShot=mode!=0;
        const int result=g_hGetTargetOnLOS.Original<Fn_GetTargetOnLOS>()(
            reinterpret_cast<PHD_VECTOR*>(&start),reinterpret_cast<PHD_VECTOR*>(&end),flags,mode);
        g_opticShot=previous;
        // Native binocular code validates ammo/cadence before passing fire
        // mode. Scoped revolver/HK shots bypass FireWeapon; a miss still fires.
        // Launchers report successful creation through RunLongShot instead.
        if (mode && (weapon==motiongun::Revolver || (g_boundDll->game==1 && weapon==5)))
            VR().GunShotHaptic(1);
        // Do not overwrite the scene camera, whose vectors the native caller
        // passes directly. The native reticle uses its own LOS result.
        return result;
    }
    if (source && dest && g_firingHand >= 0 && g_motionDll &&
        (caller == g_motionDll->hitLosReturn ||
         caller == g_motionDll->missLosReturn)) {
        // GenerateW2V already rebased BOTH native branches to the muzzle.
        // Preserve the sphere-hit segment and its native HitTarget fallback.
        // Only a miss gets the full-length spread-adjusted impact ray.
        const bool confirmedHit=caller==g_motionDll->hitLosReturn;
        const auto endpoint=motiongun::CollisionEndpoint(confirmedHit,
            {float(dest->x),float(dest->y),float(dest->z)},
            {float(source->x),float(source->y),float(source->z)},g_firingDirection);
        if (!confirmedHit) {
            dest->x=int32_t(std::lround(endpoint.x));
            dest->y=int32_t(std::lround(endpoint.y));
            dest->z=int32_t(std::lround(endpoint.z));
        }
        const PHD_VECTOR requested = *dest;
        const int32_t result =
            g_hGetTargetOnLOS.Original<Fn_GetTargetOnLOS>()(
                source, dest, flags, mode);
        if (g_motionShots[g_firingHand] == 1)
            LogF("firstperson: Touch %s LOS src=(%d,%d,%d) ray=(%d,%d,%d) result=%d sphere=%d",
                 g_firingHand ? "right" : "left",
                 source->x, source->y, source->z,
                 requested.x, requested.y, requested.z, result,int(confirmedHit));
        return result;
    }
    return g_hGetTargetOnLOS.Original<Fn_GetTargetOnLOS>()(
        source, dest, flags, mode);
}

int32_t __cdecl Detour_GetTargetOnLOS(PHD_VECTOR* source,PHD_VECTOR* dest,
                                     int32_t flags,int32_t mode) {
    return GetTargetOnLOSForCaller(reinterpret_cast<uint64_t>(_ReturnAddress())-g_boundBase,
        source,dest,flags,mode);
}

void __cdecl Detour_AnimatePistols(int32_t weapon) {
    const auto original=g_hAnimatePistols.Original<void (__cdecl*)(int32_t)>();
    if (!MotionReady() || !g_gunTriggers.WantsShot() ||
        weapon!=*Ptr<int16_t>(g_boundDll->lara+4)) {
        original(weapon); return;
    }
    auto& target=*Ptr<void*>(g_boundDll->lara+off::lara_target);
    static uint64_t nextLog=0;
    if (target && GetTickCount64()>=nextLog) {
        const int left=*Ptr<int16_t>(g_boundDll->lara+off::lara_left_arm+off::arm_lock);
        const int right=*Ptr<int16_t>(g_boundDll->lara+off::lara_right_arm+off::arm_lock);
        if (!left || !right) {
            LogF("firstperson: controller free fire bypasses native target lock=%d/%d pending=%d/%d",
                 left,right,int(g_gunTriggers.pending[0]),int(g_gunTriggers.pending[1]));
            nextLog=GetTickCount64()+5000;
        }
    }
    motiongun::ScopedControllerAim aim(target,true);
    original(weapon);
}

void __cdecl Detour_PistolHandler(int32_t weapon) {
    const bool separate=g_gunTriggers.active && MotionTriggerMode() && MotionReady();
    auto* lara=separate ? Ptr<uint8_t>(g_boundDll->lara) : nullptr;
    int16_t beforeFlash[2]{};
    if (separate) {
        for (int hand=0;hand<2;++hand)
            beforeFlash[hand]=*reinterpret_cast<int16_t*>(lara+
                (hand ? off::lara_right_arm : off::lara_left_arm)+20);
        g_handFired[0]=g_handFired[1]=false;
    }
    g_hPistolHandler.Original<Fn_PistolHandler>()(weapon);
    if (separate) {
        auto& left=*reinterpret_cast<int16_t*>(lara+off::lara_left_arm+20);
        auto& right=*reinterpret_cast<int16_t*>(lara+off::lara_right_arm+20);
        // Pistols (1) and Uzis (3) already write the matching arm's counter.
        // Only the revolver (2), handled by RifleHandler, uses the shared
        // left-arm firing branch with a right-hand flash.
        if (!g_handFired[0]) left=beforeFlash[0];
        if (!g_handFired[1]) right=beforeFlash[1];
    }
    if (!MotionReady()) return;
    // PistolHandler copies the averaged arm aim into Lara's torso and head.
    // The FP camera anchors to the animated head on the next render, so a
    // physical hand movement otherwise drags the entire view. Keep the arms'
    // independent aim, but leave torso/head neutral for the camera skeleton.
    lara = Ptr<uint8_t>(g_boundDll->lara);
    for (uint32_t offset = off::lara_head_y_rot;
         offset <= off::lara_torso_z_rot; offset += sizeof(int16_t))
        *reinterpret_cast<int16_t*>(lara + offset) = 0;
}

void __cdecl Detour_AimWeapon(void* weapon, uint8_t* arm) {
    g_hAimWeapon.Original<Fn_AimWeapon>()(weapon, arm);
    if (!g_active || !g_haveHeading || !Gate()) return;
    uint8_t* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (item != g_headingItem) return;
    uint8_t* lara = Ptr<uint8_t>(g_boundDll->lara);
    if (arm != lara + off::lara_left_arm && arm != lara + off::lara_right_arm) return;
    const int status=*reinterpret_cast<const int16_t*>(lara+2);
    if(CurrentMotionWeapon()==1 && (status==2 || status==3)) return;
    // TR5 RICH3 grapple animation state 2 is a terminal raised loop with no
    // holster transition. AnimateShotgun enters it when arm.lock is forced,
    // even with no fire input. Preserve the engine's lock for this weapon;
    // controller rendering/optic shots do not require a persistent aim lock.
    const bool preserveNativeLock=g_boundDll->game==1 && CurrentMotionWeapon()==6;
    // Native rifles use left_arm as their shared aim state; the actual grip
    // and trigger belong to the right controller, including the revolver.
    const int hand = !motiongun::DualWeapon(CurrentMotionWeapon()) ? 1 :
        arm == lara + off::lara_left_arm ? 0 : 1;
    GunPose gun{};
    if (BuildGunPose(hand, gun)) {
        const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
        if (!preserveNativeLock) *reinterpret_cast<int16_t*>(arm + off::arm_lock) = 1;
        *reinterpret_cast<int16_t*>(arm + off::arm_y_rot) =
            Angle(Radians(gun.yaw) - Radians(pos.y_rot));
        *reinterpret_cast<int16_t*>(arm + off::arm_x_rot) = gun.pitch;
        *reinterpret_cast<int16_t*>(arm + off::arm_z_rot) = 0;
        return;
    }
    if (!Cfg().firstPersonHeadAim) return;
    if (Cfg().firstPersonMotionGuns && !g_headAimFallbacks[hand]++)
        ReportMotionState("head-aim-fallback");
    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const int16_t yaw = Angle(g_headingBase + VR().HeadYawRadians() - Radians(pos.y_rot));
    const int16_t pitch = Angle(VR().HeadPitchRadians());
    // PistolHandler and RifleHandler both call here BEFORE animating/firing.
    // Their native code uses these angles for the visible arms, torso and shot
    // direction. Lock=1 prevents long guns from adding torso rotation twice.
    // Set both arms: the revolver path aims the left arm but fires the right.
    for (const uint32_t offset : { off::lara_left_arm, off::lara_right_arm }) {
        uint8_t* a = lara + offset;
        if (!preserveNativeLock) *reinterpret_cast<int16_t*>(a + off::arm_lock) = 1;
        *reinterpret_cast<int16_t*>(a + off::arm_y_rot) = yaw;
        *reinterpret_cast<int16_t*>(a + off::arm_x_rot) = pitch;
        *reinterpret_cast<int16_t*>(a + off::arm_z_rot) = 0;
    }
}

// COLL_INFO from both TR4/5 PDBs. Never reuse laracoll: its saved position
// and trigger pointer belong to the native simulation tick.
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
static_assert(offsetof(RoomCollision, flags) == 140);
using Fn_GetCollisionInfo = void(__cdecl*)(RoomCollision*, int32_t, int32_t,
                                          int32_t, int16_t, int32_t);
using Fn_GetFloor = void*(__cdecl*)(int32_t, int32_t, int32_t, int16_t*);
using Fn_GetHeight = int32_t(__cdecl*)(void*, int32_t, int32_t, int32_t);
using Fn_ItemNewRoom = void(__cdecl*)(int16_t, int16_t);

thread_local RoomCollision* g_roomscaleNativeCollision=nullptr;
thread_local uint8_t* g_roomscaleNativeItem=nullptr;
RoomCollision* g_nativeLaraCollision=nullptr;

struct RoomscaleCollisionScope {
    RoomCollision* previous=g_roomscaleNativeCollision;
    uint8_t* previousItem=g_roomscaleNativeItem;
    explicit RoomscaleCollisionScope(uint8_t* item) {
        g_roomscaleNativeCollision=nullptr;g_roomscaleNativeItem=nullptr;
        if(!g_nativeLaraCollision || !item || item!=g_headingItem || !g_active || !g_haveHeading || !Gate() ||
           !CanTurnBody(item) || g_shifted || g_jumpPressed || !VR().poseValid() ||
           !Cfg().firstPersonRoomscaleMove || !Cfg().positionalTracking || !Cfg().firstPersonHeadTranslation ||
           (g_haveManualInput && locomotion::Length(g_manualLocal)>.0001f) ||
           (*Ptr<uint64_t>(g_boundDll->input)&(locomotion::Directions|locomotion::Walk|0x10u))) return;
        // Keep the same allowance while pausing between physical steps. A
        // STOP query must not push a supported root back from the front edge
        // after roomscale has moved it there. Stick/Walk retains native rules.
        g_roomscaleNativeCollision=g_nativeLaraCollision;
        g_roomscaleNativeItem=item;
    }
    ~RoomscaleCollisionScope() {
        g_roomscaleNativeCollision=previous;g_roomscaleNativeItem=previousItem;
    }
};

void __cdecl Detour_GetCollisionInfo(RoomCollision* coll,int32_t x,int32_t y,int32_t z,
                                    int16_t room,int32_t height) {
    const bool roomscale=coll && coll==g_roomscaleNativeCollision && g_roomscaleNativeItem &&
        height==762 && CanTurnBody(g_roomscaleNativeItem) &&
        !(*reinterpret_cast<const uint32_t*>(g_roomscaleNativeItem+0x1820)&8);
    const int badPos=coll ? coll->badPos : 0;
    const uint16_t lavaPit=coll ? coll->flags&4u : 0;
    if(roomscale) { coll->badPos=32512;coll->flags&=uint16_t(~4u); }
    g_hGetCollisionInfo.Original<Fn_GetCollisionInfo>()(coll,x,y,z,room,height);
    if(roomscale) {
        coll->badPos=badPos;
        coll->flags=uint16_t((coll->flags&~4u)|lavaPit);
    }
}

void UpdateLaraRoom(uint8_t* item) {
    // TR4/5 inline TR1-3's UpdateLaraRoom(item, -381) in LaraAboveWater.
    // Reproduce that exact sequence after each accepted room-scale sweep.
    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    int16_t room = *reinterpret_cast<int16_t*>(item + off::item_room);
    void* floor = reinterpret_cast<Fn_GetFloor>(g_boundBase + g_boundDll->getFloor)(
        pos.x_pos, pos.y_pos - 381, pos.z_pos, &room);
    *reinterpret_cast<int32_t*>(item + off::item_floor_prev) =
        *reinterpret_cast<int32_t*>(item + off::item_floor);
    *reinterpret_cast<int32_t*>(item + off::item_floor) =
        reinterpret_cast<Fn_GetHeight>(g_boundBase + g_boundDll->getHeight)(
            floor, pos.x_pos, pos.y_pos - 381, pos.z_pos);
    if (room != *reinterpret_cast<int16_t*>(item + off::item_room))
        reinterpret_cast<Fn_ItemNewRoom>(g_boundBase + g_boundDll->itemNewRoom)(
            *Ptr<int16_t>(g_boundDll->lara), room);
}

locomotion::Vec DragBody(uint8_t* item) {
    using namespace locomotion;
    if (!CanTurnBody(item) || g_shifted || !Cfg().firstPersonRoomscaleMove ||
        !Cfg().positionalTracking || !Cfg().firstPersonHeadTranslation) return {};
    if (*reinterpret_cast<const uint32_t*>(item+0x1820)&8) return {};
    if (!g_boundDll->getCollisionInfo || !g_boundDll->getFloor ||
        !g_boundDll->getHeight || !g_boundDll->itemNewRoom) return {};
    Vec pending;
    VR().HeadFloorOffset(pending.x, pending.z);
    if(Cfg().firstPersonFullBodyIK && Cfg().firstPersonMovementStabilization)
        VR().FirstPersonViewOffset(pending.x,pending.z);
    const float scale = LiveWorldUnitsPerMetre();
    if (!std::isfinite(scale) || scale <= 1) return {};
    pending = pending - Rotate(g_dragCurrent - g_dragShown, -g_headingBase);
    Vec request=DragRequest(pending,Cfg().firstPersonRoomscaleDeadzoneMetres);
    if(Cfg().firstPersonFullBodyIK) {
        const auto follow=g_roomscaleFollower.Request({pending.x,0,pending.z},Elapsed(g_roomscaleFollowTime),BodyFollowSettings());
        request={follow.x,follow.z};
    }
    const Vec requested=Rotate(request,g_headingBase)*scale;
    const float distance = Length(requested);
    // Do not turn tracking loss/teleports into a physical walk across a level.
    if (!std::isfinite(distance) || distance < 1 || distance > scale * 2) return {};
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
        // Use lara_col_run's drop allowance. The STOP probe treats a ledge
        // within the front radius as a wall, leaving Lara grounded while the
        // tracked eye/body render continues over the edge.
        coll.badPos = 32512; coll.badNeg = -384; coll.badCeiling = 0;
        coll.flags = 1; // slopes remain walls; drops go to native fall handling
        coll.old[0] = pos.x_pos; coll.old[1] = pos.y_pos; coll.old[2] = pos.z_pos;
        coll.facing = Angle(std::atan2(step.x, step.z));
        const int x = pos.x_pos + dx, z = pos.z_pos + dz;
        const int16_t room = *reinterpret_cast<int16_t*>(item + off::item_room);
        reinterpret_cast<Fn_GetCollisionInfo>(g_boundBase + g_boundDll->getCollisionInfo)(
            &coll, x, pos.y_pos, z, room, 762);
        if (coll.floorSamples[0] < -384 ||
            coll.floorSamples[1] >= 0 || coll.type == 8 || coll.type == 16 || coll.type == 32)
            break;
        const int acceptedX = x + coll.shift[0] - pos.x_pos;
        const int acceptedZ = z + coll.shift[2] - pos.z_pos;
        if (!AcceptCollisionDragStep({float(dx),float(dz)},
            {float(acceptedX),float(acceptedZ)})) break;
        pos.x_pos += acceptedX; pos.z_pos += acceptedZ;
        UpdateLaraRoom(item);
        // Stop at the first unsupported step. LaraAboveWater runs immediately
        // after this sweep; its collision routine starts the native fall.
        // Do not sweep across a gap or snap Y to the lower floor ourselves.
        if (coll.floorSamples[0] > 384) break;
        if (!acceptedX && !acceptedZ) break;
    }
    const Vec actual = Vec{float(pos.x_pos), float(pos.z_pos)} - initial;
    g_dragCurrent = g_dragCurrent + actual * (1 / scale);
    return actual * (1 / scale);
}

// Catch the native turn180 animation command before input/body-follow can
// pull Lara back toward the old VR heading. Native code already turns Lara;
// rotate only the VR frame and pending movement, exactly once per half-turn.
void AdvanceLaraAnimation(uint8_t* item) {
    const bool observe=item && item==g_headingItem && g_active && g_haveHeading &&
        Gate() && LaraWaterStatus()==0;
    const int beforeState=observe ? *reinterpret_cast<const int16_t*>(item+off::item_anim_state) : -1;
    const int beforeAnimation=observe ? *reinterpret_cast<const int16_t*>(item+off::item_anim_number) : -1;
    const int16_t beforeYaw=observe ? reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos)->y_rot : 0;
    g_hAnimateLara.Original<Fn_AnimateLara>()(item);
    if (!observe || !Gate() || LaraWaterStatus()!=0) return;
    const int afterState=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    const int afterAnimation=*reinterpret_cast<const int16_t*>(item+off::item_anim_number);
    const int16_t afterYaw=reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos)->y_rot;
    const float turn=locomotion::NativeRollTurn(beforeState,afterState,beforeAnimation,afterAnimation,beforeYaw,afterYaw);
    if (turn==0) return;
    g_headingBase=Wrap(g_headingBase+turn);
    VR().PivotHeadFloorOffset(turn);
    g_manualWorld=locomotion::Rotate(g_manualWorld,turn);
    g_lastHeadWorld=Wrap(g_headingBase+VR().HeadYawRadians());
    g_lastBodyYaw=Radians(afterYaw);
    auto* analog=Ptr<int16_t>(g_boundDll->analogInput);
    analog[2]=analog[3]=Angle(g_lastHeadWorld);
    g_rootMotion.Reset();
    g_renderTurn.Reset();
    LogF("firstperson: native roll turned VR heading 180 degrees state=%d->%d anim=%d->%d",
         beforeState,afterState,beforeAnimation,afterAnimation);
}

void AccelerateBackpedalStart(uint8_t* item,uint64_t action) {
    // Animation 41 holds a slow 2-unit step for 16 frames before the normal
    // 10-unit backward loop. Shorten that command-free startup, rather than
    // boosting persistent velocity or borrowing another gait's displacement.
    if ((action&locomotion::Directions)!=locomotion::Back || !g_boundDll->anims ||
        *reinterpret_cast<const int16_t*>(item+off::item_anim_state)!=16 ||
        *reinterpret_cast<const int16_t*>(item+off::item_goal_state)!=16 ||
        *reinterpret_cast<const int16_t*>(item+off::item_anim_number)!=41 ||
        *reinterpret_cast<const int16_t*>(item+22)!=0) return;
    const auto* anims=*Ptr<const uint8_t*>(g_boundDll->anims);
    if (!anims) return;
    const auto* start=anims+41*48;
    const int16_t first=*reinterpret_cast<const int16_t*>(start+28);
    const int16_t last=*reinterpret_cast<const int16_t*>(start+30);
    auto& frame=*reinterpret_cast<int16_t*>(item+off::item_anim_number+2);
    if (*reinterpret_cast<const int16_t*>(start+10)!=16 ||
        *reinterpret_cast<const int16_t*>(start+40)!=0 ||
        first<0 || last<=first || frame<first || frame>last) return;
    // Native AnimateLara still runs once, adds its own frame and performs the
    // normal transition to the backward loop, including its native velocity.
    frame=int16_t(std::min(int(frame)+3,int(last)));
}

// Match TR1-3: one animation tick, scaling only horizontal root displacement.
// Native collision still runs afterwards, and vertical/gravity motion is intact.
void __cdecl Detour_AnimateLara(uint8_t* item) {
    auto original = &AdvanceLaraAnimation;
    // Gravity/goal changes can precede current_anim_state. Check both sides
    // of AnimateLara: a grounded tick can execute a jump or relocation command.
    const bool groundMotion=item && item==g_headingItem && CanModifyGroundMotion(item);
    const uint64_t action=groundMotion ? g_groundMoveAction : 0;
    const bool stabilize=g_stabilizeRoot && groundMotion;
    const bool hardStop=g_hardStopRoot && groundMotion;
    if (!action && !stabilize && !hardStop) {
        if (g_stabilizeRoot) g_rootMotion.Reset();
        // Native control can apply residual turn-rate before AnimateLara.
        // Align BEFORE animation displacement and AFTER any gait transition,
        // so the following native collision uses that same travel direction.
        const bool crawl=g_crouchDrive && item==g_headingItem && CanSteerCrouch(item);
        PHD_3DPOS before{};
        if(crawl) before=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
        ApplyCrouchTravelHeading(item);
        original(item);
        if(crawl && CanSteerCrouch(item)) {
            auto& pos=*reinterpret_cast<PHD_3DPOS*>(item+off::item_pos);
            const float dx=float(int64_t(pos.x_pos)-before.x_pos),dz=float(int64_t(pos.z_pos)-before.z_pos);
            const float distance=std::hypot(dx,dz);
            // Crawl-back stopping clips 277/279 are state 80 but still have
            // negative velocity. Animation can change state/sign this tick.
            // Preserve its actual distance and Y, orienting XZ BEFORE native
            // collision. Never multiply speed or touch relocation commands.
            if(distance>0 && distance<=128) {
                pos.x_pos=before.x_pos+int32_t(std::lround(std::sin(g_crouchTravelYaw)*distance));
                pos.z_pos=before.z_pos+int32_t(std::lround(std::cos(g_crouchTravelYaw)*distance));
            }
        }
        ApplyCrouchTravelHeading(item);
        return;
    }
    auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
    const int32_t oldX = pos.x_pos, oldZ = pos.z_pos;
    if (action) AccelerateBackpedalStart(item,action);
    original(item);
    const double distance=std::hypot(double(pos.x_pos)-oldX,double(pos.z_pos)-oldZ);
    if (!CanModifyGroundMotion(item) || distance>256) {
        // Leave native jumps, falls, interactions and authored relocations
        // untouched. Validate BEFORE applying the directional multiplier.
        g_rootMotion.Reset();
        return;
    }
    const bool changingGait=action && !locomotion::GroundGaitMatchesAction(
        *reinterpret_cast<const int16_t*>(item+off::item_anim_state),action);
    if (hardStop || changingGait) {
        // Keep the stopping animation, Y/gravity and subsequent collision.
        // Room-scale drag already happened BEFORE this snapshot, so only the
        // animation's residual horizontal movement is canceled, not real steps.
        pos.x_pos=oldX; pos.z_pos=oldZ;
        *reinterpret_cast<int16_t*>(item+off::item_speed)=0;
        g_rootMotion.Reset();
        return;
    }
    // Input may ask for side/back while the native run is still stopping.
    // Only the resulting, matching gait supplies the directional velocity.
    // Cancel outgoing movement above, before its collision routine runs, and
    // start the new gait on the very tick AnimateLara transitions into it.
    const int scale=locomotion::DirectionalRootScale(action,false);
    if (distance*scale>256) { g_rootMotion.Reset(); return; }
    pos.x_pos = oldX + (pos.x_pos - oldX) * scale;
    pos.z_pos = oldZ + (pos.z_pos - oldZ) * scale;
    // Scale this displacement only. Native gravity animation reuses item_speed
    // from the previous tick; writing our boosted/filtered speed back can feed
    // it into a fall and multiply it again while a ground state is still set.
    // Native animation has run exactly once; its collision routine has NOT.
    // Never straighten a collision-resolved position or touch vertical motion.
    if (stabilize) {
        locomotion::Vec step;
        if (g_rootMotion.Step({float(pos.x_pos-oldX),float(pos.z_pos-oldZ)},
            g_stabilizeDirection,*reinterpret_cast<int16_t*>(item+off::item_anim_state),
            g_stabilizeAction,step)) {
            pos.x_pos=oldX+int32_t(step.x); pos.z_pos=oldZ+int32_t(step.z);
        }
    }
}

bool OrdinaryGroundAnimation(const uint8_t* item) {
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    const int animation=*reinterpret_cast<const int16_t*>(item+off::item_anim_number);
    return (state==1 && (animation==0 || animation==6 || animation==8 || animation==10)) ||
        (state==0 && ((animation>=1 && animation<=5) || animation==7 || animation==9 || animation==20 || animation==21)) ||
        (state==2 && (animation==11 || animation==103)) ||
        (state==73 && animation>=223 && animation<=225) || // native sprint loop/startups
        (state==16 && animation>=38 && animation<=41) ||
        (state==22 && (animation==65 || animation==66)) ||
        (state==21 && (animation==67 || animation==68));
}

bool GroundJumpReady(const uint8_t* item) {
    // Shared VR input behavior: no first-person runtime/head-heading gate.
    if (!item || !g_boundDll || !g_boundBase || !Cfg().enabled || !Cfg().gamepadEnabled ||
        !VR().active() || !VR().poseValid() || g_shifted || InInventory() || InTitle() || InFMV() ||
        ScriptedCameraActive() || (g_boundDll->game!=0 && g_boundDll->game!=1) ||
        item!=*Ptr<uint8_t*>(g_boundDll->laraItem) || !CanTurnBody(item) ||
        (*reinterpret_cast<const uint32_t*>(item+0x1820)&8u) ||
        *reinterpret_cast<const int16_t*>(item+22)!=0 || !OrdinaryGroundAnimation(item)) return false;
    // Sprint's jump is its native dive. Leave that control and its momentum intact.
    if (*reinterpret_cast<const int16_t*>(item+off::item_anim_state)==73) return false;
    const int camera=*Ptr<int32_t>(g_boundDll->camera+off::camera_type);
    if (camera==kCamFixed || camera>=kCamCinematic) return false;
    const int goal=*reinterpret_cast<const int16_t*>(item+off::item_goal_state);
    switch(goal) {
    case 0: case 1: case 2: case 3: case 5: case 6: case 7: case 15:
    case 16: case 20: case 21: case 22: case 73: return true;
    default: return false;
    }
}

void UpdateGroundJumpCameraContext(const uint8_t* item) {
    // Third-person stereo draws must not discard a tap between input and simulation.
    g_groundJump.Bind(item,AppState(drva::app_off::level));
    if (!GroundJumpReady(item)) g_groundJump.Cancel();
}

bool EnterGroundIdle(uint8_t* item) {
    if (!g_boundDll->anims) return false;
    const auto* anims=*Ptr<const uint8_t*>(g_boundDll->anims);
    if (!anims) return false;
    const auto* idle=anims+11*48;
    const int16_t first=*reinterpret_cast<const int16_t*>(idle+28);
    const int16_t last=*reinterpret_cast<const int16_t*>(idle+30);
    if (*reinterpret_cast<const int16_t*>(idle+10)!=2 || *reinterpret_cast<const int16_t*>(idle+40)!=0 || first<0 || last<=first) return false;
    *reinterpret_cast<int16_t*>(item+off::item_anim_number)=11;
    *reinterpret_cast<int16_t*>(item+off::item_anim_number+2)=first;
    *reinterpret_cast<int16_t*>(item+off::item_anim_state)=2;
    *reinterpret_cast<int16_t*>(item+off::item_goal_state)=2;
    *reinterpret_cast<int16_t*>(item+off::item_speed)=0;
    g_rootMotion.Reset();
    return true;
}

void PrepareGroundDirection(uint8_t* item,uint64_t action) {
    using namespace locomotion;
    // Shorten ordinary forward/side/back gait handoffs in both directions.
    // Without Forward here, every sidestep -> forward change waits for a stop.
    // Native lara_as_stop performs its floor/ceiling checks and chooses the
    // target animation itself.
    // Never splice a landing, step-up/down, jump or interaction animation.
    if (!g_boundDll->anims || !CanModifyGroundMotion(item) ||
        (*Ptr<uint64_t>(g_boundDll->input)&(0x10|0x40|0x100|0x1000)) ||
        *reinterpret_cast<const int16_t*>(item+22)!=0) return; // required state
    const auto direction=action&Directions;
    if (direction!=Forward && direction!=StepLeft && direction!=StepRight && direction!=Back) return;
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    const int animation=*reinterpret_cast<const int16_t*>(item+off::item_anim_number);
    const int goal=*reinterpret_cast<const int16_t*>(item+off::item_goal_state);
    const bool stopping=animation==38 || animation==39 || animation==66 || animation==68;
    // Run <-> sprint is still forward travel. Do not reset to standing while
    // native control is entering/exiting sprint or it can never reach full speed.
    if (GroundGaitMatchesAction(state,action) && !stopping && GroundGaitMatchesAction(goal,action)) return;
    if (OrdinaryGroundAnimation(item)) EnterGroundIdle(item);
}

// Native run -> jump clips 16/18 each move forward for seven grounded frames
// before their end command enables gravity. Read their run-up from the loaded
// table, and allow one more running tick when deciding whether to keep waiting.
// Installed TR4/5 PDP dispatch/commands are checked by verify_jump_priority.py.
int RunningJumpLookahead() {
    if (!g_boundDll->anims || !g_boundDll->getFloor || !g_boundDll->getHeight) return 0;
    const auto* anims=*Ptr<const uint8_t*>(g_boundDll->anims);
    if (!anims) return 0;
    int distance=0;
    for (int clip:{16,18}) {
        const auto* a=anims+clip*48;
        const int frames=*reinterpret_cast<const int16_t*>(a+30)-*reinterpret_cast<const int16_t*>(a+28)+1;
        const int speed=*reinterpret_cast<const int32_t*>(a+12)>>16;
        if (*reinterpret_cast<const int16_t*>(a+10)!=3 || frames!=7 || speed!=47 ||
            *reinterpret_cast<const int32_t*>(a+16)!=0) return 0;
        distance=(frames+1)*speed;
    }
    return distance;
}

bool RunningJumpEdgeAhead(const uint8_t* item,int distance) {
    const auto& p=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
    const float yaw=Radians(p.y_rot);
    // Match AnimateLara's signed 14-bit sin/cos displacement, including its
    // per-tick truncation. Rounding a single long ray can mistime diagonals.
    const int dx=(int(std::sin(yaw)*16384.f)*47)>>14;
    const int dz=(int(std::cos(yaw)*16384.f)*47)>>14;
    int floorY=p.y_pos;
    // Sample the whole run-up, not just its endpoint: a narrow gap can have
    // solid ground on its far side. Query local room copies; never move Lara.
    for (int step=0;step<=distance;step+=47) {
        int16_t room=*reinterpret_cast<const int16_t*>(item+off::item_room);
        const int x=p.x_pos+dx*(step/47),z=p.z_pos+dz*(step/47);
        void* floor=reinterpret_cast<Fn_GetFloor>(g_boundBase+g_boundDll->getFloor)(x,floorY-381,z,&room);
        if (!floor) return false; // Missing geometry is not a verified ledge.
        const int height=reinterpret_cast<Fn_GetHeight>(g_boundBase+g_boundDll->getHeight)(floor,x,floorY-381,z);
        if (height==-32512 || height-floorY>384) return step>0;
        if (height-floorY < -384) return false; // Wall/up-step, not a drop.
        floorY=height;
    }
    return false;
}

// Port of TR1-3's ground jump priority, using TR4/5's 64-bit input mask.
// Native control/collision still own compression, takeoff and clearance.
struct GroundJumpInputScope {
    uint8_t* item;
    uint64_t* input=nullptr;
    bool injected=false;
    bool suppressed=false,waiting=false,withheld=false;
    int lookahead=0;
    explicit GroundJumpInputScope(uint8_t* target,double now):item(target) {
        g_groundJump.Bind(item,AppState(drva::app_off::level));
        if (!GroundJumpReady(item)) { g_groundJump.Cancel(); return; }
        input=Ptr<uint64_t>(g_boundDll->input);
        if (*input&(0x100u|0x1000u)) { g_groundJump.Cancel();input=nullptr;return; }
        const bool forward=(g_active && g_haveHeading && g_haveManualInput)
            ? locomotion::MovementAction(g_manualLocal)==locomotion::Forward
            : (*input&locomotion::Directions)==locomotion::Forward;
        const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
        // First-person VR already forces Modern Controls. Classic controls
        // have a separate stride-readiness gate; keep their native timing.
        const bool runIntent=NewControls() && forward && !(*input&locomotion::Walk) && (state==0 || state==1 || state==2);
        if (g_groundJump.held && !g_groundJump.edgeBlocked && runIntent &&
            (g_groundJump.edgeWaiting || g_groundJump.Wants(now)) && (lookahead=RunningJumpLookahead())) {
            waiting=g_groundJump.edgeWaiting=true;
        } else if (g_groundJump.edgeWaiting) g_groundJump.Cancel();
        if (waiting || (g_groundJump.edgeBlocked && g_groundJump.held)) {
            withheld=true;suppressed=(*input&0x10u)!=0; *input&=~uint64_t(0x10u); return;
        }
        if (g_groundJump.Wants(now) && !(*input&0x10u)) { *input|=0x10u;injected=true; }
    }
    void Prepare() {
        if (waiting) {
            if (!GroundJumpReady(item)) { g_groundJump.Cancel(); return; }
            // Startup gaits must reach the real run loop before dispatching.
            if (*reinterpret_cast<const int16_t*>(item+off::item_anim_state)==1 &&
                *reinterpret_cast<const int16_t*>(item+off::item_anim_number)==0 &&
                RunningJumpEdgeAhead(item,lookahead)) {
                *input|=0x10u; injected=!suppressed;
                // Consume only after native simulation actually accepts it.
                // A collision/clearance rejection may leave Lara running.
                g_groundMoveAction=0; g_stabilizeRoot=false; g_rootMotion.Reset();
            }
            return;
        }
        if (!input || !(*input&0x10u) || !GroundJumpReady(item)) return;
        const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
        const int animation=*reinterpret_cast<const int16_t*>(item+off::item_anim_number);
        // Preserve running momentum and the idle loop's always-open dispatch.
        if ((state==1 && animation==0) || (state==2 && animation==103)) return;
        // Walk/start/stop gaits have no immediate jump dispatch. Restart the
        // verified idle entry even on its last frame (185+1 misses compression).
        EnterGroundIdle(item);
    }
    ~GroundJumpInputScope() {
        if (!input) return;
        // A requested goal isn't proof of takeoff; wait for the actual state.
        if (!GroundJumpReady(item)) g_groundJump.Cancel();
        if (injected) *input&=~uint64_t(0x10u);
        if (suppressed) *input|=0x10u;
    }
};

#include "PhysicalSwitch.inl"

bool UpdateLedgePull(uint8_t* item, uint64_t now) {
    // IsLedgeHangState is a visibility group which also includes monkey bars.
    // Native TR4/5 lara_col_hang mounts from AS_HANG (10), animation 96 only.
    bool eligible=item && item==g_headingItem && g_active && g_haveHeading && Gate() &&
        Cfg().gamepadEnabled && (g_boundDll->game==0 || g_boundDll->game==1) &&
        item==*Ptr<uint8_t*>(g_boundDll->laraItem) && LaraWaterStatus()==0 &&
        *reinterpret_cast<int16_t*>(item+off::item_anim_state)==10 &&
        *reinterpret_cast<int16_t*>(item+off::item_anim_number)==96 &&
        !(*reinterpret_cast<uint32_t*>(item+0x1820)&8);
    if (eligible) {
        // Explicit stick movement, jump, roll or native modern Drop wins.
        eligible=(*Ptr<uint64_t>(g_boundDll->input)&
            (locomotion::Directions|0x10|0x100|0x1000))==0;
    }
    vr::HmdMatrix34_t head{},left{},right{};
    eligible=eligible && VR().HeadPose(head) && VR().ControllerPose(0,left) && VR().ControllerPose(1,right);
    const bool wasLatched=g_ledgePull.latched;
    const bool pull=g_ledgePull.Update(eligible,now,head.m[1][3],left.m[1][3],right.m[1][3]);
    if (pull && !wasLatched) Log("firstperson: two-hand ledge pull requested");
    return pull;
}

// TR4/5 ModernControlsGunRotation injects Left/Right from camTurn even
// with both analog axes zero. VRIK deliberately retains head/body yaw slack:
// feeding HMD yaw here starts native turn/fastturn animations under still
// controllers. Supply body facing only during stationary armed simulation.
// Restore the camera-relative heading before subsequent input/aim consumers.
struct IdleTrackedGunHeadingScope {
    int16_t* heading=nullptr;
    int16_t saved=0;
    explicit IdleTrackedGunHeadingScope(uint8_t* item) {
        if(!item || item!=g_headingItem || !Cfg().firstPersonFullBodyIK ||
           !MotionReady() || !NewControls() || !CanModifyGroundMotion(item) ||
           g_shifted || g_jumpPressed || locomotion::IsLandingAnimation(g_boundDll->game,
               *reinterpret_cast<const int16_t*>(item+off::item_anim_state),
               *reinterpret_cast<const int16_t*>(item+off::item_anim_number)) ||
           (g_haveManualInput && locomotion::Length(g_manualLocal)>.0001f)) return;
        const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
        if(state!=2 && state!=6 && state!=7 && state!=20) return;
        if(*Ptr<uint64_t>(g_boundDll->input)&(locomotion::Directions|0x10u|0x100u|0x1000u)) return;
        auto* analog=Ptr<int16_t>(g_boundDll->analogInput);
        if(analog[0] || analog[1]) return;
        heading=analog+2;saved=*heading;
        *heading=reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos)->y_rot;
    }
    ~IdleTrackedGunHeadingScope() { if(heading) *heading=saved; }
};

// Unarmed lara_as_compress calls ModernControlsRotation before selecting the
// directional jump. Our FP input already supplies body-relative Left/Right/Back;
// rotating toward that same stick applies its direction twice. Guns-out uses
// a separate dispatcher which does not rotate, hence the armed/unarmed split.
struct DirectionalJumpAxesScope {
    int16_t* axes=nullptr;
    int16_t saved[2]{};
    explicit DirectionalJumpAxesScope(uint8_t* item) {
        if(!item || item!=g_headingItem || !g_active || !g_haveHeading || !Gate() ||
           !Cfg().gamepadEnabled || !NewControls() || !g_haveManualInput || g_shifted ||
           LaraWaterStatus()!=0 || *reinterpret_cast<const int16_t*>(item+off::item_hit_points)<=0 ||
           *reinterpret_cast<const int16_t*>(item+off::item_anim_state)!=15) return;
        const auto direction=locomotion::MovementAction(g_manualLocal,true)&locomotion::Directions;
        if(direction!=locomotion::Left && direction!=locomotion::Right && direction!=locomotion::Back) return;
        const int status=*Ptr<int16_t>(g_boundDll->lara+2),weapon=CurrentMotionWeapon();
        // Preserve the native guns-out branch, including its flare/torch exceptions.
        if(weapon!=8 && (status==4 || (status==2 && weapon!=7))) return;
        axes=Ptr<int16_t>(g_boundDll->analogInput);
        saved[0]=axes[0];saved[1]=axes[1];axes[0]=axes[1]=0;
        // Keep digital direction/Jump and native floor checks. Restore analog
        // input after this one simulation call so airborne steering stays native.
    }
    ~DirectionalJumpAxesScope() { if(axes) { axes[0]=saved[0];axes[1]=saved[1]; } }
};

void ObserveJumpLanding(uint8_t* item) {
    const bool eligible=item && g_active && Cfg().firstPersonMovementStabilization && Gate() &&
        item==*Ptr<uint8_t*>(g_boundDll->laraItem) && LaraWaterStatus()==0 &&
        *reinterpret_cast<const int16_t*>(item+off::item_hit_points)>0 &&
        !(*Ptr<uint32_t>(g_boundDll->lara+off::lara_movement_flags)&0x20);
    if(!eligible) { g_jumpLanding.Reset();return; }
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    const int animation=*reinterpret_cast<const int16_t*>(item+off::item_anim_number);
    g_jumpLanding.Observe(item,AppState(drva::app_off::level),true,state,animation,
        reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos)->y_pos,
        (*reinterpret_cast<const uint32_t*>(item+0x1820)&8)!=0,CanTurnBody(item),
        locomotion::IsLandingAnimation(g_boundDll->game,state,animation),
        (*Ptr<uint64_t>(g_boundDll->input)&0x10u)!=0);
}

void __cdecl Detour_LaraAboveWater(uint8_t* item, void* nativeCollision) {
    using namespace locomotion;
    GroundJumpInputScope jumpInput(item,TurnTime());
    g_dragPrevious = g_dragCurrent;
    g_crouchDrive=false;
    g_groundMoveAction = 0;
    g_stabilizeRoot=false;
    g_hardStopRoot=false;
    if (item && item == g_headingItem && g_active && g_haveHeading && Gate()) {
        const int state = *reinterpret_cast<int16_t*>(item + off::item_anim_state);
        const bool ground = CanTurnBody(item);
        const bool jump = IsJumpSteeringState(state) && LaraWaterStatus() == 0 &&
            *reinterpret_cast<int16_t*>(item + off::item_hit_points) > 0;
        if(CanSteerCrouch(item) && g_haveManualInput && !g_shifted && Length(g_manualLocal)>0) {
            const float head=Wrap(g_headingBase+VR().HeadYawRadians());
            const Vec world=Cfg().firstPersonMoveWithHead ? Rotate(g_manualLocal,head) : g_manualWorld;
            g_crouchDrive=true;g_crouchTravelYaw=std::atan2(world.x,world.z);
            ApplyCrouchTravelHeading(item);
            auto* analog=Ptr<int16_t>(g_boundDll->analogInput);
            const float magnitude=std::min(32767.f,Length(g_manualLocal)*32767.f);
            const Vec decoded=SimulationStick(world,head,magnitude);
            analog[0]=int16_t(std::round(decoded.x));analog[1]=int16_t(std::round(decoded.z));
            analog[2]=analog[3]=Angle(head);
            // Duck/all4s require Forward or Back to enter a crawl. Side-turn
            // bits alone rotate in place. Use native crawl along the LS vector,
            // preserving Duck/Action and every other non-direction input bit.
            auto& input=*Ptr<uint64_t>(g_boundDll->input);
            input=(input&~Directions)|(state==86 ? Back : Forward);
        }
        if (CanSteerMonkeyBars(item) && g_haveManualInput && !g_shifted && Length(g_manualLocal)>0) {
            // Native monkey controls read camTurn even though the rendered VR
            // camera is decoupled from it. Rebuild their inputs AFTER native
            // conversion, from the original LS intent and current VR heading.
            const float head=Wrap(g_headingBase+VR().HeadYawRadians());
            const Vec world=Cfg().firstPersonMoveWithHead ? Rotate(g_manualLocal,head) : g_manualWorld;
            auto* analog=Ptr<int16_t>(g_boundDll->analogInput);
            auto& input=*Ptr<uint64_t>(g_boundDll->input);
            const float magnitude=std::min(32767.f,std::hypot(float(analog[0]),float(analog[1])));
            const Vec decoded=SimulationStick(world,head,magnitude);
            analog[0]=int16_t(std::round(decoded.x));analog[1]=int16_t(std::round(decoded.z));
            analog[2]=analog[3]=Angle(head);
            const auto& pos=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
            // Modern hang2/swing use Forward to request traversal and steer
            // toward atan2(axes)+camTurn. Tank mode selects native body-local
            // traverse/half-turn animations; StepLeft/Right are not turn bits.
            const uint64_t action=NewControls() ? Forward :
                (MovementAction(Rotate(world,-Radians(pos.y_rot)))&Directions);
            input=(input&~Directions)|action;
            // Do not set Lara yaw, move_angle, speed or ground gait: her native
            // animation and ceiling collision retain ownership of the grip.
        }
        if ((ground || jump) && g_haveManualInput) {
            const float head = Wrap(g_headingBase + VR().HeadYawRadians());
            auto* analog = Ptr<int16_t>(g_boundDll->analogInput);
            auto& input = *Ptr<uint64_t>(g_boundDll->input);
            // The LS deadzone has already produced zero manual input. Don't
            // brake keyboard/D-pad movement, shifted controls, jumps or rolls.
            g_hardStopRoot=ground && CanModifyGroundMotion(item) && !g_shifted && !(g_jumpPressed && !jumpInput.withheld) &&
                Length(g_manualLocal)<=0.0001f && !(input & (Directions | 0x10 | 0x100));
            // Runs after native input conversion. All subsequent rotation,
            // animation and collision now agree on a single HMD/world heading.
            analog[2] = analog[3] = Angle(head);
            if (Length(g_manualWorld) > 0 && !g_shifted) {
                const Vec directionalWorld = MovementWorld(g_manualLocal, head);
                const float magnitude = std::min(32767.0f, std::hypot(
                    static_cast<float>(analog[0]), static_cast<float>(analog[1])));
                if (magnitude > 0) {
                    const Vec steeringWorld = (ground || state == 15)
                        ? directionalWorld : g_manualWorld;
                    const Vec decoded = SimulationStick(steeringWorld, head, magnitude);
                    analog[0] = static_cast<int16_t>(std::round(decoded.x));
                    analog[1] = static_cast<int16_t>(std::round(decoded.z));
                }
                if (ground || state == 15) {
                    const bool preparingJump = (ground && !jumpInput.withheld && (g_jumpPressed || (input&0x10u))) || state == 15;
                    const uint64_t action = MovementAction(g_manualLocal, preparingJump);
                    auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
                    pos.y_rot = Angle(head);
                    *Ptr<int16_t>(g_boundDll->lara + off::lara_turn_rate) = 0;
                    // AnimateLara consumes this BEFORE the collision routine
                    // sets it. Without it, side/back gaits step forward each tick.
                    *Ptr<int16_t>(g_boundDll->lara + off::lara_move_angle) =
                        Angle(MovementYaw(g_manualLocal, head));
                    input = (input & ~Directions) | action;
                    if (ground && !preparingJump)
                        g_groundMoveAction = action;
                    if (ground && !preparingJump && Cfg().firstPersonMovementStabilization) {
                        g_stabilizeRoot=true;
                        g_stabilizeDirection=directionalWorld;
                        g_stabilizeAction=action;
                    }
                }
            }
        }
    }
    if (!g_stabilizeRoot) g_rootMotion.Reset();
    // Physical steps never enter XInput or advance the walk animation.
    if (item && item == g_headingItem && g_active && g_haveHeading && Gate()) {
        const Vec dragged = DragBody(item);
        if (Cfg().firstPersonDriftLog) {
            static uint64_t last = 0;
            static int lastState = -1;
            const uint64_t now = GetTickCount64();
            const int state = *reinterpret_cast<int16_t*>(item + off::item_anim_state);
            if (now - last >= 500 || state != lastState) {
                Vec pending; VR().HeadFloorOffset(pending.x, pending.z);
                const auto& pos = *reinterpret_cast<PHD_3DPOS*>(item + off::item_pos);
                LogF("locomotion: state=%d head=%.1f body=%.1f manual=(%+.2f,%+.2f) "
                     "pending=(%+.3f,%+.3f)m drag=(%+.3f,%+.3f)m input=%016llX",
                     state, Wrap(g_headingBase + VR().HeadYawRadians()) * 180 / kPi,
                     Radians(pos.y_rot) * 180 / kPi, g_manualWorld.x, g_manualWorld.z,
                     pending.x, pending.z, dragged.x, dragged.z,
                     static_cast<unsigned long long>(*Ptr<uint64_t>(g_boundDll->input)));
                last = now;
            }
            lastState = state;
        }
    }
    jumpInput.Prepare();
    ObserveJumpLanding(item);
    if (g_groundMoveAction) PrepareGroundDirection(item,g_groundMoveAction);
    const bool ledgePull=UpdateLedgePull(item,GetTickCount64());
    uint64_t savedLedgeInput=0;
    int16_t savedLedgeHeading=0;
    if (ledgePull) {
        auto& input=*Ptr<uint64_t>(g_boundDll->input);
        savedLedgeInput=input;
        input|=locomotion::Forward|0x40; // native Forward + Action/grab
        auto* analog=Ptr<int16_t>(g_boundDll->analogInput);
        savedLedgeHeading=analog[2];
        // lara_as_hang remaps Forward by camTurn in modern controls. Make
        // this request face the ledge regardless of HMD/chase-camera yaw.
        analog[2]=reinterpret_cast<PHD_3DPOS*>(item+off::item_pos)->y_rot;
    }
    const bool grabFeedback=item && item==g_headingItem && g_active && g_haveHeading && Gate() &&
        Cfg().gamepadEnabled && (g_boundDll->game==0 || g_boundDll->game==1) && LaraWaterStatus()==0 &&
        item==*Ptr<uint8_t*>(g_boundDll->laraItem);
    if (grabFeedback) {
        const auto& position=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
        g_ledgeGrab.Sample(true,*reinterpret_cast<int16_t*>(item+off::item_anim_state),
            (*reinterpret_cast<uint32_t*>(item+0x1820)&8)!=0,
            {float(position.x_pos),float(position.y_pos),float(position.z_pos)});
    } else g_ledgeGrab.Reset();
    {
        IdleTrackedGunHeadingScope idleGunHeading(item);
        CrouchWeaponScope crouchWeapons(item);
        DirectionalJumpAxesScope jumpAxes(item);
        RoomscaleCollisionScope roomscaleCollision(item);
        g_hLaraAboveWater.Original<Fn_LaraAboveWater>()(item, nativeCollision);
    }
    ObserveJumpLanding(item);
    if (grabFeedback && g_ledgeGrab.Finish(Gate() && LaraWaterStatus()==0,
            *reinterpret_cast<int16_t*>(item+off::item_anim_state),
            (*reinterpret_cast<uint32_t*>(item+0x1820)&8)!=0))
        VR().LedgeGrabHaptic();
    if (ledgePull) {
        // No synthetic run/Action or camera heading leaks to later handlers.
        auto& input=*Ptr<uint64_t>(g_boundDll->input);
        constexpr uint64_t owned=locomotion::Directions|0x40;
        input=(input&~owned)|(savedLedgeInput&owned);
        Ptr<int16_t>(g_boundDll->analogInput)[2]=savedLedgeHeading;
    }
    g_crouchDrive=false;
    g_groundMoveAction = 0;
    g_stabilizeRoot=false;
    g_hardStopRoot=false;
}

void UpdateLocomotion(PHD_3DPOS& pose) {
    using namespace locomotion;
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const auto& prev = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos_prev);
    const float t = std::clamp(*Ptr<int32_t>(g_boundDll->frameFrac), 0, 256) / 256.0f;
    const Vec body{prev.x_pos + (pos.x_pos - prev.x_pos) * t,
                   prev.z_pos + (pos.z_pos - prev.z_pos) * t};
    const bool relocated = Length(body - g_previousBody) >
        std::max(1024.0f, LiveWorldUnitsPerMetre() * 2);
    const int level=AppState(drva::app_off::level);
    const bool sameBody=g_headingItem==item && g_headingLevel==level;
    if (g_haveHeading && sameBody && !relocated) {
        // Consume only body drag actually visible at this render fraction.
        // Stick/animation movement must never cancel genuine physical leaning.
        const Vec shown = g_dragPrevious + (g_dragCurrent - g_dragPrevious) * t;
        const Vec used = Rotate(shown - g_dragShown, -g_headingBase);
        VR().ConsumeHeadFloorOffset(used.x, used.z);
        g_dragShown = shown;
    }
    if (!g_haveHeading || !sameBody || relocated) {
        ResetBodyFollower();
        g_groundJump.Reset();
        g_ledgePull.Reset(); g_ledgeGrab.Reset();
        g_mountBodyTransition.Reset();
        g_renderTurn.Reset(); g_turnTrace={}; g_rootMotion.Reset();
        const float oldBase=g_headingBase;
        const float bodyTurn=sameBody ? Wrap(Radians(pos.y_rot)-g_lastBodyYaw) : 0;
        const float facing = sameBody && !relocated
            ? Wrap(g_lastHeadWorld+bodyTurn) : Radians(pos.y_rot);
        g_headingBase = Wrap(facing - VR().HeadYawRadians());
        g_groundEye.Resume(sameBody,oldBase,g_headingBase,bodyTurn);
        g_stanceEye.Reset();
        g_headingLevel=level;
        g_cameraMotionTrace = {};
        g_haveHeading = true;
        g_headingItem = item;
        g_haveManualInput = false;
        g_dragPrevious = g_dragCurrent = g_dragShown = {};
        g_bodyTime = {};
        VR().RecenterFirstPersonHead();
    }
    g_previousBody = body;
    // One shared heading for this rendered camera, body following, culling,
    // eyes and tracked guns. Controller polling never advances it separately.
    AdvanceStickTurn(TurnTime());
    if (!CanTurnBody(item) && !Cfg().firstPersonMovementStabilization) {
        // Legacy animated-camera compensation only. With a stable/raw-tracked
        // view, consuming neck-corrected displacement while hanging invents a
        // head offset and carries it into the next standing frame.
        Vec pending; VR().HeadFloorOffset(pending.x, pending.z);
        VR().ConsumeHeadFloorOffset(pending.x, pending.z);
    }
    TurnBodyToHead(item, Elapsed(g_bodyTime));
    g_lastBodyYaw=Radians(pos.y_rot);
    pose.y_rot = Angle(g_headingBase);
    g_lastHeadWorld = Wrap(g_headingBase + VR().HeadYawRadians());
}

void ClampRenderedHeadToCollision(const uint8_t* item, PHD_3DPOS& pose) {
    if (!g_boundDll->getCollisionInfo) return;
    const bool ground = CanFitStandingBody(item);
    const bool crouch = CanUseCrouchCamera(item);
    const int capsuleHeight=crouch ? 400 : 762;
    const int state = *reinterpret_cast<const int16_t*>(item + off::item_anim_state);
    // Ground rolls also need a sweep around their stable eye. Interactions
    // use the retracted anchor instead, and third person never reaches here.
    const bool jump = LaraWaterStatus() == 0 &&
        *reinterpret_cast<const int16_t*>(item + off::item_hit_points) > 0 &&
        (state == 3 || state == 9 || state == 12 || state == 15 ||
         (state >= 25 && state <= 29));
    const bool roll=locomotion::IsGroundRollState(state) && LaraWaterStatus()==0 &&
        *reinterpret_cast<const int16_t*>(item+off::item_hit_points)>0;
    if (!ground && !jump && !roll && !crouch) return;

    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const auto& prev = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos_prev);
    const int frac = std::clamp(*Ptr<int32_t>(g_boundDll->frameFrac), 0, 256);
    const auto lerp = [frac](int32_t a, int32_t b) {
        return int32_t(int64_t(a) + (int64_t(b) - a) * frac / 256);
    };
    const int32_t body[3] = {lerp(prev.x_pos, pos.x_pos),
                             lerp(prev.y_pos, pos.y_pos),
                             lerp(prev.z_pos, pos.z_pos)};

    locomotion::Vec tracked{};
    if (Cfg().positionalTracking && Cfg().firstPersonHeadTranslation && !VR().headAtCamera()) {
        VR().FirstPersonViewOffset(tracked.x, tracked.z);
        tracked = locomotion::Rotate(tracked, g_headingBase) * LiveWorldUnitsPerMetre();
    }
    if (!std::isfinite(tracked.x) || !std::isfinite(tracked.z) ||
        std::fabs(tracked.x) > 4096 || std::fabs(tracked.z) > 4096) return;
    // Always protect the game's forward eye anchor. The option controls only
    // physical head translation: free leaning must not let ordinary stick
    // movement place the untracked camera inside a crate or wall.
    const bool collideTracking=Cfg().firstPersonWallCollision;
    const int32_t offsetX = collideTracking ? int32_t(std::lround(tracked.x)) : 0;
    const int32_t offsetZ = collideTracking ? int32_t(std::lround(tracked.z)) : 0;
    const int64_t eyeX = int64_t(pose.x_pos) + offsetX;
    const int64_t eyeZ = int64_t(pose.z_pos) + offsetZ;
    if (eyeX < INT32_MIN || eyeX > INT32_MAX ||
        eyeZ < INT32_MIN || eyeZ > INT32_MAX) return;
    int32_t eye[3] = {int32_t(eyeX), pose.y_pos, int32_t(eyeZ)};

    const int16_t room = *reinterpret_cast<const int16_t*>(item + off::item_room);
    double eyeY=pose.y_pos;
    if (Cfg().positionalTracking && Cfg().firstPersonHeadTranslation && !VR().headAtCamera())
        eyeY+=VR().FirstPersonVerticalOffset()*LiveWorldUnitsPerMetre();
    if (!std::isfinite(eyeY) || std::fabs(eyeY-pose.y_pos)>4096) return;
    auto clampVertical=[&](int32_t x,int32_t z) {
        // The rendered headset can be lower than the scene anchor (ducking).
        // Query the final horizontal eye, then keep that rendered centre clear
        // of floor/ceiling without changing Lara, tracking neutral or calibration.
        RoomCollision coll{};
        coll.radius=64; coll.badPos=32512; coll.badNeg=-4096;
        coll.old[0]=x; coll.old[1]=body[1]; coll.old[2]=z;
        coll.facing=Angle(g_lastHeadWorld);
        reinterpret_cast<Fn_GetCollisionInfo>(g_boundBase+g_boundDll->getCollisionInfo)(
            &coll,x,body[1],z,room,capsuleHeight);
        if (coll.floorSamples[0]!=-32512 && coll.floorSamples[1]!=-32512) {
            const double floor=double(body[1])+coll.floorSamples[0];
            const double ceiling=double(body[1])-capsuleHeight+coll.floorSamples[1];
            if (floor>ceiling) {
                // Stay strictly inside the horizontal sweep's 64-unit test.
                const double margin=std::min(crouch ? 65.0 : 64.0,(floor-ceiling)*0.5);
                const double corrected=std::clamp(eyeY,ceiling+margin,floor-margin);
                const double anchor=pose.y_pos+corrected-eyeY;
                if (anchor>=INT32_MIN && anchor<=INT32_MAX) {
                    pose.y_pos=int32_t(std::lround(anchor));
                    eyeY=corrected;
                }
            }
        }
    };
    if (!collideTracking) {
        // Keep physical leaning independent of the base-camera sweep. Retain
        // stance/landing vertical protection at Lara's valid position.
        if (roll || crouch || UseLandingCamera(item)) clampVertical(body[0],body[2]);
    }
    // A stance transition/physical rise under a low ceiling must lower the
    // eye before the horizontal sweep, not retract it to Lara then pop forward.
    else if(crouch) clampVertical(body[0],body[2]);
    const double sweepY=collideTracking ? eyeY : double(pose.y_pos);
    firstperson::ClampEyeToWall(body, eye,
        [&](int32_t x, int32_t z, int32_t clearX, int32_t clearZ) {
            RoomCollision coll{};
            coll.radius = 64;
            coll.badPos = 32512; // A deep floor drop is not a wall at eye height.
            coll.badNeg = -4096;
            coll.badCeiling = 0;
            coll.flags = 0; // Eye clearance permits empty space beyond a ledge.
            coll.old[0] = clearX; coll.old[1] = body[1]; coll.old[2] = clearZ;
            coll.facing = Angle(std::atan2(float(x - clearX), float(z - clearZ)));
            reinterpret_cast<Fn_GetCollisionInfo>(g_boundBase + g_boundDll->getCollisionInfo)(
                &coll, x, body[1], z, room, capsuleHeight);
            return firstperson::EyeBlocked(coll.floorSamples,body[1],sweepY,coll.hitStatic!=0,capsuleHeight) ||
                coll.type == 8 || coll.type == 16 || coll.type == 32 ||
                coll.shift[0] || coll.shift[2];
        });
    // With tracked collision enabled, undo the physical translation which
    // stereo adds later. Otherwise keep it entirely outside this correction.
    pose.x_pos = eye[0] - offsetX;
    pose.z_pos = eye[2] - offsetZ;
    if (collideTracking && (roll || crouch || UseLandingCamera(item))) clampVertical(eye[0],eye[2]);
}

// Diagnostic only: distinguish animated lateral sway from actual root motion
// and physical translation. Extrema over a full second avoid aliasing a gait
// cycle with a slow periodic log sample. Never modify the camera or input here.
void TraceForwardCamera(const uint8_t* item, const float body[3],
                        const PHD_VECTOR& head, int frac, uint64_t now,
                        const PHD_3DPOS* rendered = nullptr) {
    const int state = *reinterpret_cast<const int16_t*>(item + off::item_anim_state);
    if (!Cfg().firstPersonDriftLog || !g_haveManualInput || g_shifted ||
        g_manualLocal.z <= 0.2f || std::fabs(g_manualLocal.x) > g_manualLocal.z ||
        (state != 0 && state != 1)) {
        g_cameraMotionTrace = {};
        return;
    }
    using namespace locomotion;
    auto& trace = g_cameraMotionTrace;
    const bool first = trace.samples == 0;
    if (first) trace.start = now;
    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const auto& prev = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos_prev);
    const float bodyYaw = Radians(prev.y_rot) +
        Wrap(Radians(pos.y_rot) - Radians(prev.y_rot)) * (frac / 256.0f);
    const float headYaw = Wrap(g_headingBase + VR().HeadYawRadians());
    const Vec bodyWorld{body[0], body[2]};
    const Vec headWorld{float(head.x), float(head.z)};
    Vec pending; VR().FirstPersonViewOffset(pending.x, pending.z);
    if (!Cfg().firstPersonHeadTranslation || !Cfg().positionalTracking) pending={};
    const Vec cameraWorld = rendered ? Vec{float(rendered->x_pos),float(rendered->z_pos)} : headWorld;
    const Vec eyeWorld = cameraWorld + Rotate(pending, g_headingBase) * LiveWorldUnitsPerMetre();
    trace.animatedSide.Add(Rotate(headWorld - bodyWorld, -bodyYaw).x, first);
    trace.trackedSide.Add(Rotate(pending, -VR().HeadYawRadians()).x, first);
    trace.yawError.Add(Wrap(headYaw - bodyYaw) * 180 / kPi, first);
    trace.rootSideStep.Add(first ? 0 : Rotate(bodyWorld - trace.previousBody, -headYaw).x, first);
    trace.eyeSideStep.Add(first ? 0 : Rotate(eyeWorld - trace.previousEye, -headYaw).x, first);
    trace.fracMin = std::min(trace.fracMin, frac);
    trace.fracMax = std::max(trace.fracMax, frac);
    trace.previousBody = bodyWorld; trace.previousEye = eyeWorld;
    ++trace.samples;
    if (now - trace.start >= 1000) {
        LogF("fp-forward: game=%d state=%d samples=%u elapsed=%llums frac=%d..%d "
             "animSide=%.2f..%.2fu rootSideStep=%.2f..%.2fu "
             "eyeSideStep=%.2f..%.2fu trackedSide=%.4f..%.4fm bodyYawError=%.2f..%.2fdeg",
             g_boundDll->game + 4, state, trace.samples,
             static_cast<unsigned long long>(now - trace.start), trace.fracMin, trace.fracMax,
             trace.animatedSide.low, trace.animatedSide.high,
             trace.rootSideStep.low, trace.rootSideStep.high,
             trace.eyeSideStep.low, trace.eyeSideStep.high,
             trace.trackedSide.low, trace.trackedSide.high,
             trace.yawError.low, trace.yawError.high);
        trace = {};
    }
}

// Diagnostic only. Separate real HMD movement, locomotion, consumed roomscale
// displacement and camera corrections; never filter the tracked eye.
void TraceEyeContinuity(const uint8_t* item,const PHD_3DPOS& pose,const float (&body)[3]) {
    auto& trace=g_eyeContinuity;
    if(!Cfg().firstPersonDriftLog) { trace={};return; }
    vr::HmdMatrix34_t hmd{};if(!VR().HeadPose(hmd)) { trace.valid=false;return; }
    const float units=LiveWorldUnitsPerMetre();
    locomotion::Vec view{};VR().FirstPersonViewOffset(view.x,view.z);
    const auto flat=locomotion::Rotate(view,g_headingBase)*units;
    const motiongun::Vec eye{pose.x_pos+flat.x,pose.y_pos+VR().FirstPersonVerticalOffset()*units,pose.z_pos+flat.z};
    const motiongun::Vec root{body[0],body[1],body[2]},head{hmd.m[0][3],hmd.m[1][3],hmd.m[2][3]};
    const uint64_t now=GetTickCount64();
    if(trace.valid && trace.item==item && std::fabs(Wrap(g_headingBase-trace.heading))<.001f) {
        const auto raw=locomotion::Rotate({head.x-trace.head.x,trace.head.z-head.z},g_headingBase)*units;
        const auto consumed=(g_dragShown-trace.drag)*units;
        const motiongun::Vec expected{root.x-trace.root.x+raw.x-consumed.x,
            root.y-trace.root.y+(head.y-trace.head.y)*(Cfg().flipViewY ? -units : units),
            root.z-trace.root.z+raw.z-consumed.z};
        const auto residual=motiongun::Sub(motiongun::Sub(eye,trace.eye),expected);
        if((firstperson::Length(residual)>4 || firstperson::Length(motiongun::Sub(head,trace.head))>.04f) &&
           now-trace.report>=50) {
            LogF("fp-eye: state=%d anim=%d gun=%d status=%d camera=%d residual=(%.1f,%.1f,%.1f) wall=(%.1f,%.1f) root=(%.1f,%.1f,%.1f) hmd=(%.3f,%.3f,%.3f) neutral=(%.3f,%.3f,%.3f)",
                *reinterpret_cast<const int16_t*>(item+off::item_anim_state),*reinterpret_cast<const int16_t*>(item+off::item_anim_number),
                CurrentMotionWeapon(),*Ptr<int16_t>(g_boundDll->lara+2),*Ptr<int32_t>(g_boundDll->camera+off::camera_type),
                residual.x,residual.y,residual.z,g_bodyWallOffset.x,g_bodyWallOffset.z,root.x,root.y,root.z,
                head.x,head.y,head.z,head.x-view.x,head.y+VR().FirstPersonHeightDrop(),head.z+view.z);
            trace.report=now;
        }
    }
    trace.valid=true;trace.item=item;trace.eye=eye;trace.root=root;trace.head=head;
    trace.drag=g_dragShown;trace.heading=g_headingBase;
}

void FitBodyToRenderedEye(const uint8_t* item,const PHD_3DPOS& pose,const PHD_VECTOR& animatedHead) {
    using namespace locomotion;
    g_bodyVisualOffset={};
    // The landing camera already follows the animated neck in all axes.
    // Standing body fitting would translate that same neck a second time.
    if (UseLandingCamera(item)) { g_mountBodyTransition.Reset(); return; }
    if (!CanFitStandingBody(item)) {
        const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
        if (IsLedgeMountState(state)) g_mountBodyTransition.Begin();
        else g_mountBodyTransition.Reset();
        return;
    }
    const auto& pos=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
    const auto& prev=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos_prev);
    const int frac=std::clamp(*Ptr<int32_t>(g_boundDll->frameFrac),0,256);
    Vec view{},floor{};
    if (Cfg().positionalTracking && Cfg().firstPersonHeadTranslation && !VR().headAtCamera()) {
        VR().FirstPersonViewOffset(view.x,view.z);
        VR().HeadFloorOffset(floor.x,floor.z);
    }
    const float bodyYaw=Radians(prev.y_rot)+Wrap(Radians(pos.y_rot)-Radians(prev.y_rot))*(frac/256.f);
    const Vec eyeFromRoot=CanTurnBody(item) ? Vec{float(pose.x_pos-int32_t(prev.x_pos+(int64_t(pos.x_pos)-prev.x_pos)*frac/256)),
                          float(pose.z_pos-int32_t(prev.z_pos+(int64_t(pos.z_pos)-prev.z_pos)*frac/256))}
        : Rotate({g_groundEye.local.x,g_groundEye.local.z},g_headingBase);
    g_bodyVisualOffset=g_groundEye.BodyOffsetAtEye(g_headingBase,bodyYaw,
        view,floor,LiveWorldUnitsPerMetre(),eyeFromRoot);
    if (Cfg().firstPersonMovementStabilization && g_groundEye.valid) {
        const float t=frac/256.f;
        const Vec root{prev.x_pos+(pos.x_pos-prev.x_pos)*t,prev.z_pos+(pos.z_pos-prev.z_pos)*t};
        const Vec animatedFromRoot{animatedHead.x-root.x,animatedHead.z-root.z};
        const Vec expected=Rotate(g_groundEye.bodyLocal,bodyYaw);
        const float height=animatedHead.y-(prev.y_pos+(pos.y_pos-prev.y_pos)*t);
        g_bodyVisualOffset=g_bodyVisualOffset-g_mountBodyTransition.Correction(
            animatedFromRoot-expected,height,GetTickCount64()/1000.0);
    } else g_mountBodyTransition.Reset();
    static uint64_t nextFitLog=0;
    if (Cfg().firstPersonDriftLog && GetTickCount64()>=nextFitLog) {
        nextFitLog=GetTickCount64()+5000;
        LogF("firstperson: resolved body fit yaw=%.1f base=%.1f offset=(%.1f,%.1f) pending=(%.3f,%.3f)m",
             bodyYaw*180/kPi,g_headingBase*180/kPi,g_bodyVisualOffset.x,g_bodyVisualOffset.z,floor.x,floor.z);
    }
}

bool Anchor(PHD_3DPOS& pose) {
    g_bodyWallOffset={};
    g_ladderTorsoOffset={};
    uint8_t* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item) return false;
    const int joint = Cfg().firstPersonJoint;
    if (joint < 0 || joint >= 33) return false;

    int frac = *Ptr<int32_t>(g_boundDll->frameFrac);
    frac = std::clamp(frac, 0, 256);
    const float t = frac / 256.0f;
    const auto& pos = *reinterpret_cast<const PHD_3DPOS*>(item + off::item_pos);
    const auto& old = *reinterpret_cast<const PHD_3DPOS*>(
        item + off::item_pos_prev);
    const float body[3] = {
        old.x_pos + (pos.x_pos - old.x_pos) * t,
        old.y_pos + (pos.y_pos - old.y_pos) * t,
        old.z_pos + (pos.z_pos - old.z_pos) * t
    };
    const int state = *reinterpret_cast<const int16_t*>(item + off::item_anim_state);
    PHD_VECTOR head{ Cfg().firstPersonAnchorX, Cfg().firstPersonAnchorY,
                     locomotion::FirstPersonAnchorZ(state, Cfg().firstPersonAnchorZ,
                                                    Cfg().firstPersonInteractionAnchorZ) };
    // Use the same interpolated, absolute joint query as the native renderer.
    // This routine saves/restores its matrix stack and adds Lara's world origin.
    reinterpret_cast<Fn_GetJointAbsPositionLerp>(
        g_boundBase + g_boundDll->getJointAbsPositionLerp)(item, &head, joint, frac);
    const double dx = double(head.x) - body[0];
    const double dy = double(head.y) - body[1];
    const double dz = double(head.z) - body[2];
    if (dx * dx + dy * dy + dz * dz > 4096.0 * 4096.0) {
        if (!g_loggedLost) {
            LogF("firstperson: joint %d is too far from Lara; not anchoring",
                 joint);
            g_loggedLost = true;
        }
        return false;
    }
    pose.x_pos = head.x;
    pose.y_pos = head.y;
    pose.z_pos = head.z;
    pose.x_rot = 0;
    UpdateLocomotion(pose);
    if (locomotion::IsLadderState(state)) {
        // The native head can pitch through a rung stroke. Define clearance
        // in Lara's facing frame, not that pitching head frame or HMD gaze.
        PHD_VECTOR neck{0,Cfg().firstPersonAnchorY,0};
        reinterpret_cast<Fn_GetJointAbsPositionLerp>(
            g_boundBase+g_boundDll->getJointAbsPositionLerp)(item,&neck,joint,frac);
        const float yaw=Radians(old.y_rot)+Wrap(Radians(pos.y_rot)-Radians(old.y_rot))*t;
        // Ladder-specific clearance: the standing 140+ unit offset pulled
        // the torso unnecessarily far from the wall and extended both arms.
        const auto safe=locomotion::Rotate({float(Cfg().firstPersonAnchorX),80.f},yaw);
        g_ladderTorsoOffset={head.x-neck.x-safe.x,head.z-neck.z-safe.z};
    }
    if (locomotion::IsGroundRollState(state) && LaraWaterStatus()==0) {
        // Keep the horizontal anchor in the new facing direction, but retain
        // the animated head height so the view drops with the roll. Clearance
        // below limits the final tracked eye before it can enter the floor.
        // Never capture a rolling joint into the standing calibration.
        const auto local=g_groundEye.valid ? g_groundEye.local : stabilization::Point{
            float(Cfg().firstPersonAnchorX),0,float(Cfg().firstPersonAnchorZ)};
        const auto flat=locomotion::Rotate({local.x,local.z},g_headingBase);
        pose.x_pos=int32_t(std::lround(body[0]+flat.x));
        pose.z_pos=int32_t(std::lround(body[2]+flat.z));
    } else if (Cfg().firstPersonMovementStabilization && state==25 &&
               LaraWaterStatus()==0 && g_groundEye.valid) {
        // Backjump's animated neck rises again just after the physics apex,
        // producing an apparent second jump in first person. Follow the
        // interpolated collision root with the existing standing eye offset.
        // No temporal filter: native ascent/descent and HMD tracking stay live.
        // Entering FP midair without calibration retains the native eye.
        const auto flat=locomotion::Rotate({g_groundEye.local.x,g_groundEye.local.z},g_headingBase);
        pose.x_pos=int32_t(std::lround(body[0]+flat.x));
        pose.y_pos=int32_t(std::lround(body[1]+g_groundEye.local.y));
        pose.z_pos=int32_t(std::lround(body[2]+flat.z));
    } else if (Cfg().firstPersonMovementStabilization && CanUseCrouchCamera(item)) {
        // Native crouch/crawl turns swing the animated head around Lara. Keep
        // the eye in the artificial-yaw frame, inside the 400-unit crouch hull.
        const auto flat=locomotion::Rotate(g_groundEye.valid ?
            locomotion::Vec{g_groundEye.local.x,g_groundEye.local.z} :
            locomotion::Vec{float(Cfg().firstPersonAnchorX),float(Cfg().firstPersonAnchorZ)},g_headingBase);
        pose.x_pos=int32_t(std::lround(body[0]+flat.x));
        pose.y_pos=int32_t(std::lround(body[1]-336)); // 64 units below capsule top.
        pose.z_pos=int32_t(std::lround(body[2]+flat.z));
    } else if (Cfg().firstPersonMovementStabilization && CanFitStandingBody(item) && !UseLandingCamera(item)) {
        // Physical head yaw rotates Lara, not the scene-camera origin. Only
        // artificial yaw changes this reference frame; HMD pose is applied
        // separately, identically for both eyes, culling and tracked guns.
        const auto eye=g_groundEye.Apply({body[0],body[1],body[2]},g_headingBase,
            {float(head.x),float(head.y),float(head.z)},
            Radians(old.y_rot)+Wrap(Radians(pos.y_rot)-Radians(old.y_rot))*t,
            !CanTurnBody(item) && !(Cfg().firstPersonFullBodyIK && state==12));
        pose.x_pos=int32_t(std::lround(eye.x));
        pose.y_pos=int32_t(std::lround(eye.y));
        pose.z_pos=int32_t(std::lround(eye.z));
    } else if (!Cfg().firstPersonMovementStabilization) g_groundEye.Reset();
    // Landings and other non-ground states use the full native animated eye, while
    // retaining the standing reference. Never pin the camera above/beside a
    // bending neck or save a kneeling pose as normal standing height.
    // On vault/pull-up completion the cached skeleton can still be
    // from the climb while Lara's root has moved onto the crate. Recapturing
    // then locks that transient high/sideways offset into every standing frame.
    // Item/level changes and explicit view toggles discard this calibration.
    if(Cfg().firstPersonMovementStabilization &&
       (CanUseCrouchCamera(item) || (CanTurnBody(item) && !UseLandingCamera(item))))
        pose.y_pos=int32_t(std::lround(body[1]+g_stanceEye.Apply(float(pose.y_pos)-body[1],CanUseCrouchCamera(item),TurnTime())));
    else g_stanceEye.Reset();
    if(Cfg().firstPersonFullBodyIK && Cfg().firstPersonPhysicalCrouch && g_groundEye.valid &&
       g_physicalCrouchCamera && (CanTurnBody(item) || CanUseCrouchCamera(item))) {
        // Physical lowering already enters through the HMD offset. Do not add
        // the native Duck camera drop a second time.
        pose.y_pos=int32_t(std::lround(body[1]+g_groundEye.local.y));
        if(!g_heightCrouch.active && !CanUseCrouchCamera(item)) g_physicalCrouchCamera=false;
    }
    // Keep standing calibration separate from clearance. The rendered body
    // must accompany camera retraction: a world-fixed torso would move into
    // the eye as the camera backs up. Native collision/shadows remain fixed.
    FitBodyToRenderedEye(item,pose,head);
    const locomotion::Vec unclampedEye{float(pose.x_pos),float(pose.z_pos)};
    ClampRenderedHeadToCollision(item, pose);
    g_bodyWallOffset=locomotion::Vec{float(pose.x_pos),float(pose.z_pos)}-unclampedEye;
    TraceEyeContinuity(item,pose,body);
    TraceForwardCamera(item, body, head, frac, GetTickCount64(), &pose);
    pose.z_rot = 0;
    if (!g_loggedFirst) {
        LogF("firstperson: native joint %d anchored; offset from body=(%.0f,%.0f,%.0f)",
             joint, dx, dy, dz);
        g_loggedFirst = true;
    }
    return true;
}

bool DrawingLaraHead(const uint8_t* item, bool includeHeadOnlyGeometry=false) {
    const int16_t object = *reinterpret_cast<const int16_t*>(
        item + off::item_object);
    const GameDllLayout& dll = *g_boundDll;
    if (object < 0) return false;
    if (!dll.objects) return false;
    const uint8_t* objectInfo = Ptr<uint8_t>(dll.objects) +
        static_cast<uint32_t>(object) * off::object_stride;
    const void* mesh = *reinterpret_cast<void* const*>(
        objectInfo + off::object_geom + off::geom_mesh);
    if (!mesh) return false; // Uninitialized geometry is not a face match.
    if (dll.gLaraHeads) {
        const uint8_t* heads = Ptr<uint8_t>(dll.gLaraHeads);
        for (int i = 0; i < kLaraHeadGeoms; ++i) {
            const void* headMesh = *reinterpret_cast<void* const*>(
                heads + i * off::geom_stride + off::geom_mesh);
            if (mesh == headMesh) return true;
        }
    }
    // Alternate head/face draws can use a different mesh descriptor. During
    // the landing kneel, recognize them by their actual skeleton as well.
    // Do not discard a body or hand palette that merely includes joint 14.
    if (includeHeadOnlyGeometry) {
        const auto* geom=objectInfo+off::object_geom;
        const int count=*reinterpret_cast<const int32_t*>(geom+28);
        const auto* mapping=*reinterpret_cast<const int32_t* const*>(geom+48);
        if (count>0 && count<=33 && mapping) {
            for (int i=0;i<count;++i) if (mapping[i]!=14) return false;
            return true;
        }
    }
    return false;
}

// Change only this eye-view hand draw's geometry. The native descriptor is
// restored before DrawLaraHD resumes; simulation, armed grips and shadows keep
// their original hand selection. Copy the whole descriptor, including binding
// and material pointers, rather than mixing a rest mesh with a run skeleton.
class UnarmedRestHandScope {
    uint8_t* geometry=nullptr;
    uint8_t saved[off::geom_stride]{};
public:
    UnarmedRestHandScope(uint8_t* item,int32_t useMeshBits) {
        if (!useMeshBits || !g_unarmedHandDll || !g_unarmedHandDll->hands ||
            DrawingNativeShadow() || !UnarmedIKReady() ||
            item!=*Ptr<uint8_t*>(g_boundDll->laraItem)) return;
        const uint32_t bits=*reinterpret_cast<const uint32_t*>(item+off::item_mesh_bits);
        // Native unarmed hand passes select forearm+hand, singly or together.
        if (!bits || (bits&~0x3600u)) return;
        const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
        if (object<0 || !g_boundDll->objects) return;
        auto* current=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride+off::object_geom;
        const auto mesh=*reinterpret_cast<const void* const*>(current+off::geom_mesh);
        if (!mesh) return;
        const auto* hands=Ptr<uint8_t>(g_unarmedHandDll->hands);
        for (int bank:{0,30,60}) {
            const auto* run=hands+(bank+2)*off::geom_stride;
            const auto* rest=hands+(bank+1)*off::geom_stride;
            if (mesh!=*reinterpret_cast<const void* const*>(run+off::geom_mesh) ||
                std::memcmp(current,run,off::geom_stride) ||
                !*reinterpret_cast<const void* const*>(rest+off::geom_mesh)) continue;
            geometry=current;
            std::memcpy(saved,current,sizeof(saved));
            std::memcpy(current,rest,sizeof(saved));
            break;
        }
    }
    ~UnarmedRestHandScope() { if (geometry) std::memcpy(geometry,saved,sizeof(saved)); }
    UnarmedRestHandScope(const UnarmedRestHandScope&)=delete;
    UnarmedRestHandScope& operator=(const UnarmedRestHandScope&)=delete;
};

void __cdecl Detour_CrouchDraw(uint8_t* item,int32_t drawType) {
    const auto original=g_hCrouchDraw.Original<void (__cdecl*)(uint8_t*,int32_t)>();
    if(DrawingNativeShadow() || !g_active || !g_scenePoseValid || !g_boundDll ||
       !g_boundBase || !g_motionDll || item!=*Ptr<uint8_t*>(g_boundDll->laraItem)) {
        original(item,drawType);return;
    }
    auto* app=*Ptr<uint8_t*>(g_motionDll->app);
    if(!app) { original(item,drawType);return; }
    auto& flags=*reinterpret_cast<uint32_t*>(app+drva::app_off::cfgFlags);
    const uint32_t modern=flags&2u;
    // This render-only routine gates its proximity rejection on modern
    // controls. Skip that branch, retaining the HD bit and every other flag.
    // This is a first-person visibility policy, independent of IK readiness:
    // wall impacts and temporarily missing controller poses must not bring
    // proximity hiding back. Explicit stance masks and shadow rendering remain.
    flags&=~2u;
    const HolsterRenderScope holsters(item,app);
    original(item,drawType);
    flags=(flags&~2u)|modern;
}

void __cdecl Detour_DrawCreatureHD(uint8_t* item, int32_t useMeshBits,
                                   int32_t renderPass) {
    if (DrawingNativeShadow()) {
        // First-person clipping/hand splitting belongs only to the eye view.
        // The light must see the intact native body, head and weapon meshes.
        const int previousArm=g_renderArm, previousJoint=g_renderHandJoint;
        const bool previousScope=g_bodySkinScope;
        g_renderArm=-1;g_renderHandJoint=-1;g_bodySkinScope=false;
        g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item,useMeshBits,renderPass);
        g_renderArm=previousArm;g_renderHandJoint=previousJoint;
        g_bodySkinScope=previousScope;
        return;
    }
    if (g_active && g_rollHidden && !MotionReady() && g_boundDll && g_boundBase &&
        item == *Ptr<uint8_t*>(g_boundDll->laraItem)) return;
    bool lara = false;
    if (g_active && item && g_boundDll) {
        const GameDllLayout& dll = *g_boundDll;
        lara = item == *Ptr<uint8_t*>(dll.laraItem);
    }
    const bool landingHead=lara && HideLandingHead(item);
    const uint32_t bodyMask=lara ? FirstPersonBodyMask(item) : UINT32_MAX;
    const bool armsOnly=bodyMask==firstperson::ArmMeshBits;
    const bool handsOnly=bodyMask==firstperson::HandMeshBits;
    const bool hideHead=Cfg().firstPersonHideHead || landingHead;
    if (lara && (hideHead || armsOnly || handsOnly || MotionReady()) &&
        DrawingLaraHead(item,landingHead || armsOnly || handsOnly)) {
        ++g_faceSkips;
        return;
    }
    const bool motion = lara && MotionReady();
    if(lara && !useMeshBits) g_pistolArmPose.valid=false;
    const bool fullBody = lara && FullBodyIKReady();
    // Untracked hidden stances retain their fallback. Tracked weapons still
    // obey transition draw masks and the swimming gate.
    if (lara && g_underwaterHidden && !motion && !fullBody) return;
    if (motion) {
        // Apply the same transition mask with full-body IK enabled or disabled.
        // Gun passes contain only hands and guns, avoiding duplicate forearms.
        // Keep the complete skeleton so wrist pivot and gun placement stay put.
        auto& bits = *reinterpret_cast<uint32_t*>(item + off::item_mesh_bits);
        if (!useMeshBits) {
            if(!g_underwaterHidden && !handsOnly) {
                const auto savedBits=bits;const int previousArm=g_renderArm;
                bits=bodyMask & ~firstperson::HeadMeshBit & ~0x2400u;
                g_renderArm=-1;g_bodySkinScope=true;g_bodySkinReady=false;
                const bool previousPistolScope=g_pistolBodyScope;
                const bool previousWristScope=g_bodyWristSeamScope;
                g_bodyWristSeamScope=true;
                g_pistolBodyScope=CurrentMotionWeapon()==1;
                g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item,1,renderPass);
                g_pistolBodyScope=previousPistolScope;
                g_bodyWristSeamScope=previousWristScope;
                g_bodySkinScope=g_bodySkinReady=false;g_renderArm=previousArm;bits=savedBits;
            }
            return;
        }
        const uint32_t handMask=motiongun::HandOnlyMask(bits);
        if(!handMask && bits && !(bits&~0x92u)) {
            if(armsOnly || handsOnly) return;
            // Hip and back weapon meshes share the body's fitted/IK skeleton.
            const bool previousScope=g_bodySkinScope;
            g_bodySkinScope=true;g_bodySkinReady=false;
            g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item,1,renderPass);
            g_bodySkinScope=previousScope;g_bodySkinReady=false;
            return;
        }
        if (!handMask) return;
        const uint32_t savedMotionBits=bits;
        const int previousArm=g_renderArm;
        const int previousHandJoint=g_renderHandJoint;
        // Native hand/weapon geometry also owns the forearms (0x600/0x3000).
        // The main body pass cannot restore vertices from a different mesh.
        // Render those forearms with the articulated IK palette before the
        // existing rigid hand/gun passes, using the original skin weights.
        const uint32_t forearms=bits&0x1200u&bodyMask;
        if (forearms && !g_underwaterHidden) {
            bits=forearms;g_renderArm=-1;g_renderHandJoint=-1;
            g_bodySkinScope=true;g_bodySkinReady=false;
            const bool previousPistolScope=g_pistolForearmScope;
            g_pistolForearmScope=CurrentMotionWeapon()==1;
            g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item,1,renderPass);
            g_pistolForearmScope=previousPistolScope;
            g_bodySkinScope=g_bodySkinReady=false;
        }
        // Long guns can put both hands in one native mesh pass. Split it
        // before applying the two independent wrist transforms.
        for (int hand=0;hand<2;++hand) {
            const uint32_t mask=hand ? 0x400 : 0x2000;
            vr::HmdMatrix34_t pose{};
            if (!(handMask&mask) || !VR().ControllerPose(hand,pose)) continue;
            bits=mask; g_renderArm=hand; g_renderHandJoint=-1;
            g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item,1,renderPass);
        }
        bits=savedMotionBits; g_renderArm=previousArm;
        g_renderHandJoint=previousHandJoint;
        return;
    }
    const UnarmedRestHandScope restHands(item,lara ? useMeshBits : 0);
    const int previousArm = g_renderArm;
    g_renderArm = -1;
    if (lara && (hideHead || bodyMask!=UINT32_MAX || g_unarmedArmsHidden || UnarmedIKReady() || fullBody)) {
        auto& bits = *reinterpret_cast<uint32_t*>(item + off::item_mesh_bits);
        const uint32_t savedBits = bits;
        // Native HD body passes use useMeshBits=0: their effective mask is ALL
        // joints, even if the item's persistent mask is zero/stale at startup.
        // Apply visibility to that effective mask for this draw alone.
        // Already-masked weapon passes must retain their native restrictions.
        bits = VisibleMeshBits(useMeshBits ? savedBits : UINT32_MAX,
            hideHead,false,false,armsOnly,false,g_unarmedArmsHidden && !fullBody) & bodyMask;
        g_bodySkinScope=true; g_bodySkinReady=false;
        const bool previousPistolBody=g_pistolBodyScope,previousPistolForearm=g_pistolForearmScope;
        const int weaponStatus=*Ptr<int16_t>(g_boundDll->lara+2);
        if(fullBody && CurrentMotionWeapon()==1 && (weaponStatus==2 || weaponStatus==3 ||
           g_pistolAnimation.mode!=0 || g_pistolAnimation.blending)) {
            g_pistolBodyScope=!useMeshBits;
            g_pistolForearmScope=useMeshBits && bits && !(bits&~0x3f00u);
        }
        g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item, 1, renderPass);
        g_pistolBodyScope=previousPistolBody;g_pistolForearmScope=previousPistolForearm;
        g_bodySkinScope=g_bodySkinReady=false;
        bits = savedBits;
    } else {
        g_hDrawCreatureHD.Original<Fn_DrawCreatureHD>()(item, useMeshBits, renderPass);
    }
    g_renderArm = previousArm;
}

void __cdecl Detour_DrawHair(int32_t argument) {
    const auto* item=g_boundDll && g_boundBase ? *Ptr<uint8_t*>(g_boundDll->laraItem) : nullptr;
    if (!DrawingNativeShadow() && g_active && !ShowJumpHair(item) && (HideLandingHead(item) || g_headHidden || g_rollHidden || g_crouchHidden || g_underwaterHidden || FirstPersonBodyMask(item)==firstperson::ArmMeshBits || IsHandsOnlyBodyState(item) || MotionReady())) {
        ++g_hairSkips;
        return;
    }
    g_hDrawHair.Original<Fn_DrawHair>()(argument);
}

void UpdateSceneCamera(PHD_3DPOS& pose) {
    // The inventory's scene is redrawn, not simulated. Preserve the last FP
    // camera/root while native inventory owns input. Running Anchor here
    // would consume room-scale movement; suspending FP would jump to chase
    // view. EyeView still supplies live headset rotation/translation.
    if(InventorySceneReady() && WorldInventoryActive() && g_active && g_scenePoseValid) {
        pose=g_scenePose;
        g_pistolArmPose.valid=false;
        return;
    }
    g_pistolArmPose.valid=false;
    if(!MotionReady() || CurrentMotionWeapon()!=1) g_pistolRoll={};
    if(Cfg().firstPersonFullBodyIK && g_boundDll && g_boundBase &&
       g_autoBodyCalibration.Observe(AppState(drva::app_off::level),LaraWaterStatus())) {
        ResetBodyFollower();g_groundEye.Reset();g_stanceEye.Reset();
        VR().RecenterFirstPersonHead();
    }
    const bool wasActive = g_active;
    if (Gate() && Anchor(pose)) {
        g_active = true;
        g_scenePose = pose;
        g_scenePoseValid = true;
        ++g_anchored;
    } else {
        if(wasActive && g_boundDll && g_boundBase && Cfg().firstPersonDriftLog)
            LogF("fp-eye: camera suspended type=%d water=%d scripted=%d poseValid=%d",
                 *Ptr<int32_t>(g_boundDll->camera+off::camera_type),LaraWaterStatus(),ScriptedCameraActive(),VR().poseValid());
        g_eyeContinuity.valid=false;
        g_active = false;
        g_scenePoseValid = false;
        g_haveHeading = false;
        // Suspend, don't toggle the user's preference. No queued shot or
        // equip gesture may leak out of swimming/cutscenes/menus into resumed play.
        g_gunTriggers.Reset();
        g_gunEquip.Reset();
        g_ledgePull.Reset(); g_ledgeGrab.Reset();
        // Keep item identity and last viewing heading across UI/cameras.
        g_haveManualInput = false;
        const int water=LaraWaterStatus();
        if (water==1 || water==2) {
            // Native swimming can change facing while FP is suspended. On
            // leaving water, anchor to Lara rather than the pre-swim heading.
            g_headingItem=nullptr;
        }
        g_groundMoveAction = 0;
        g_renderTurn.Reset(); g_turnTrace={}; g_rootMotion.Reset();
        if (!g_runtimeEnabled || !Cfg().firstPerson) g_groundEye.Reset();
        g_bodyVisualOffset={};
        g_bodyWallOffset={};
        g_ladderTorsoOffset={};
        g_mountBodyTransition.Reset();
        g_bodyTime = {};
        g_dragPrevious = g_dragCurrent = g_dragShown = {};
        ++g_skipped;
        g_cameraMotionTrace = {};
    }
    FirstPersonControlModeUpdate();
    if (!g_active) { g_stanceEye.Reset();ResetBodyFollower();g_bodyVisibility={}; }
    if (wasActive && !g_active) VR().RecenterThirdPersonHead();
    const auto* item = g_boundDll && g_boundBase
        ? *Ptr<uint8_t*>(g_boundDll->laraItem) : nullptr;
    UpdateGroundJumpCameraContext(item);
    if(g_active && item && locomotion::IsLedgeMountState(*reinterpret_cast<const int16_t*>(item+off::item_anim_state)))
        g_torsoPose.Mount();
    vr::HmdMatrix34_t calibrationHead{};
    const bool standing=g_active && item && g_groundEye.valid &&
        *reinterpret_cast<const int16_t*>(item+off::item_anim_state)==2 && CanTurnBody(item) &&
        !UseLandingCamera(item) && !g_mountBodyTransition.active && !g_stanceEye.transition && VR().HeadPose(calibrationHead);
    if(g_autoBodyCalibration.Ready(standing,{calibrationHead.m[0][3],calibrationHead.m[1][3],calibrationHead.m[2][3]},TurnTime())) {
        FirstPersonRecenter();
        Log("firstperson: automatic standing body calibration after level/water transition");
    }
    const bool unarmedMovement=g_active && item && CanTurnBody(item) &&
        *Ptr<int16_t>(g_boundDll->lara+2)==0 &&
        *Ptr<int16_t>(g_boundDll->lara+4)>=0 && *Ptr<int16_t>(g_boundDll->lara+4)<=6;
    const bool unarmedIK=UnarmedIKReady();
    const bool fullBodyIK=FullBodyIKReady();
    if(!fullBodyIK) g_bodyFollower.Reset();
    if (!unarmedIK) g_unarmedTwist[0]=g_unarmedTwist[1]={};
    const bool hideUnarmedArms=g_unarmedArmVisibility.Hide(unarmedMovement && !unarmedIK,VR().HeadPitchRadians());
    SetMeshVisibility(g_active && (Cfg().firstPersonHideHead || HideLandingHead(item)),
                      g_active && IsRollState(item),
                      g_active && IsHandsOnlyBodyState(item),
                      g_active && FirstPersonBodyMask(item)==firstperson::ArmMeshBits,
                      g_active && item && LaraWaterStatus()==1,hideUnarmedArms,MotionReady() || unarmedIK); // UNDERWATER, not SURFACE/WADE/FLYCHEAT
    UpdateFlareLightPosition(); // Before room/item light collection for this render.
}

void __cdecl Detour_GenerateW2V(PHD_3DPOS* pose) {
    const void* caller = _ReturnAddress();
    if (pose && IsSceneCall(caller)) {
        const PHD_3DPOS native=*pose;
        UpdateSceneCamera(*pose);
        RebaseShadowReceiver(native,*pose);
    }
    if (pose && g_firingHand >= 0 && g_motionDll &&
        reinterpret_cast<uint64_t>(caller) ==
            g_boundBase + g_motionDll->fireViewReturn) {
        const int16_t spreadPitch = int16_t(pose->x_rot - g_firingAim[1]);
        const int16_t spreadYaw = int16_t(pose->y_rot - g_firingAim[0]);
        pose->x_pos = g_firingPose.x_pos;
        pose->y_pos = g_firingPose.y_pos;
        pose->z_pos = g_firingPose.z_pos;
        pose->x_rot = int16_t(g_firingPose.x_rot + spreadPitch);
        pose->y_rot = int16_t(g_firingPose.y_rot + spreadYaw);
        // Native phd_GenerateW2V's third row is the actual shot direction:
        // (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch)).
        const float yaw = Radians(pose->y_rot);
        const float pitch = Radians(pose->x_rot);
        g_firingDirection = motiongun::ShotForward(yaw, pitch);
    }
    g_hGenerateW2V.Original<Fn_GenerateW2V>()(pose);
}

void RemoveLongGunHooks() {
    g_longGunHooksReady=false; g_longShot=nullptr; g_opticShot=false;
    g_triggerWeapon=0;
    g_hInitialiseProjectile.Remove(); g_hGunJoint.Remove();
    g_hFireCrossbow.Remove(); g_hFireSpecial.Remove();
    g_hFireShotgun.Remove(); g_hRifleHandler.Remove();
    g_longGunDll=nullptr;
}

bool InstallLongGunHooks(const GameDllLayout& d,uint64_t base) {
    for (const auto& row:kLongGunDlls)
        if (row.timestamp==d.timestamp) g_longGunDll=&row;
    if (!g_longGunDll) return false;
    const auto& r=*g_longGunDll;
    // Optic calls are also verified at runtime, not just in the binary audit.
    for (auto ret:r.opticReturns) {
        const auto* call=reinterpret_cast<const uint8_t*>(base+ret-5);
        int32_t delta=0; std::memcpy(&delta,call+1,4);
        if (*call!=0xe8 || int64_t(ret)+delta!=g_motionDll->getTargetOnLOS)
            return false;
    }
    const uint8_t rifle4[]={0x40,0x53,0x57,0x48,0x81,0xec,0x98,0,0,0};
    const uint8_t rifle5[]={0x40,0x53,0x56,0x57,0x41,0x56};
    const uint8_t shotgun[]={0x48,0x89,0x5c,0x24,0x18};
    const uint8_t grenade[]={0x41,0x55,0x48,0x81,0xec,0xb0,0,0,0};
    const uint8_t hk[]={0x89,0x4c,0x24,0x08,0x48,0x83,0xec,0x38};
    const uint8_t crossbow4[]={0x40,0x57,0x48,0x83,0xec,0x60};
    const uint8_t crossbow5[]={0x48,0x89,0x7c,0x24,0x18};
    const uint8_t helper[]={0x48,0x89,0x5c,0x24,0x08};
    auto install=[&](hook::InlineHook& h,uint32_t address,void* detour,
                     const uint8_t* bytes,size_t size,const char* name) {
        return h.Install(reinterpret_cast<void*>(base+address),detour,size,bytes,size,name);
    };
    const bool tr4=d.game==0;
    return install(g_hRifleHandler,r.rifle,reinterpret_cast<void*>(&Detour_RifleHandler),
            tr4 ? rifle4 : rifle5,tr4 ? sizeof(rifle4) : sizeof(rifle5),"MotionRifleHandler") &&
        install(g_hFireShotgun,r.shotgun,reinterpret_cast<void*>(&Detour_FireShotgun),
            shotgun,sizeof(shotgun),"MotionFireShotgun") &&
        install(g_hFireSpecial,r.special,tr4 ? reinterpret_cast<void*>(&Detour_FireGrenade) :
            reinterpret_cast<void*>(&Detour_FireHK),tr4 ? grenade : hk,
            tr4 ? sizeof(grenade) : sizeof(hk),"MotionFireSpecial") &&
        install(g_hFireCrossbow,r.crossbow,reinterpret_cast<void*>(&Detour_FireCrossbow),
            tr4 ? crossbow4 : crossbow5,tr4 ? sizeof(crossbow4) : sizeof(crossbow5),
            "MotionFireCrossbow") &&
        install(g_hGunJoint,r.joint,reinterpret_cast<void*>(&Detour_GunJoint),
            helper,sizeof(helper),"MotionGunJoint") &&
        install(g_hInitialiseProjectile,r.initialise,reinterpret_cast<void*>(&Detour_InitialiseProjectile),
            helper,sizeof(helper),"MotionInitialiseProjectile");
}

bool Install(const GameDllLayout& d, uint64_t base) {
    if (!d.phdGenerateW2V) return false;
    if (!d.w2vSceneReturn) return false;
    if (!d.frameFrac) return false;
    if (!d.laraItem) return false;
    if (!d.getJointAbsPositionLerp || !d.aimWeapon) return false;
    if (!d.laraAboveWater || !d.animateLara || !d.analogInput || !d.input) return false;
    if (!d.getCollisionInfo || !d.getFloor || !d.getHeight || !d.itemNewRoom ||
        !d.playingCutseq || !d.cutseqNum || !d.cutseqTrigger || !d.useSpotCam ||
        (d.game==0 && !d.vonCroyCutscene)) return false;
    const bool retail = d.timestamp == 0x68C12FDA || d.timestamp == 0x68C12FE9;
    if (std::memcmp(reinterpret_cast<const void*>(base + d.getCollisionInfo),
                    retail ? kCollisionRetail : kCollisionDebug, sizeof(kCollisionDebug)) ||
        std::memcmp(reinterpret_cast<const void*>(base + d.getFloor), kGetFloor, sizeof(kGetFloor)) ||
        std::memcmp(reinterpret_cast<const void*>(base + d.getHeight),
                    d.game == 0 ? kGetHeightTR4 : kGetHeightTR5, sizeof(kGetHeightTR4)) ||
        std::memcmp(reinterpret_cast<const void*>(base + d.itemNewRoom), kItemNewRoom, sizeof(kItemNewRoom))) {
        Log("firstperson: native room-scale helper bytes mismatch; refusing to install");
        return false;
    }
    const uint8_t* movement = d.game == 0 ? kLaraAboveWaterTR4 : kLaraAboveWaterTR5;
    // LaraAboveWater uses the global laracoll, not its incoming RDX register.
    // Verify its initial old-X/old-Y stores and resolve that exact record.
    const auto* store=reinterpret_cast<const uint8_t*>(base+d.laraAboveWater+(d.game==0 ? 0x16 : 0x20));
    int32_t oldX=0,oldY=0;
    std::memcpy(&oldX,store+2,4);std::memcpy(&oldY,store+13,4);
    if(store[0]!=0x89 || store[1]!=0x05 || store[11]!=0x89 || store[12]!=0x05 ||
       store+6+oldX+4!=store+17+oldY) {
        Log("firstperson: native Lara collision record could not be verified");
        return false;
    }
    g_nativeLaraCollision=reinterpret_cast<RoomCollision*>(const_cast<uint8_t*>(store+6+oldX)-offsetof(RoomCollision,old));
    if (!g_hGetCollisionInfo.Install(
            reinterpret_cast<void*>(base+d.getCollisionInfo),
            reinterpret_cast<void*>(&Detour_GetCollisionInfo),sizeof(kCollisionDebug),
            retail ? kCollisionRetail : kCollisionDebug,sizeof(kCollisionDebug),"RoomscaleGetCollisionInfo")) return false;
    if (!g_hLaraAboveWater.Install(
            reinterpret_cast<void*>(base + d.laraAboveWater),
            reinterpret_cast<void*>(&Detour_LaraAboveWater), 5,
            movement, 5, "LaraAboveWater")) return false;
    const uint8_t* animation = d.game == 0 ? kAnimateLaraTR4 : kAnimateLaraTR5;
    const size_t animationBytes = d.game == 0 ? sizeof(kAnimateLaraTR4) : sizeof(kAnimateLaraTR5);
    if (!g_hAnimateLara.Install(
            reinterpret_cast<void*>(base + d.animateLara),
            reinterpret_cast<void*>(&Detour_AnimateLara), animationBytes,
            animation, animationBytes, "AnimateLara")) return false;
    if (!g_hAimWeapon.Install(
            reinterpret_cast<void*>(base + d.aimWeapon),
            reinterpret_cast<void*>(&Detour_AimWeapon), 5,
            kAimWeaponPrologue, sizeof(kAimWeaponPrologue), "AimWeapon"))
        return false;
    for (const auto& entry : kMotionDlls)
        if (entry.timestamp == d.timestamp) g_motionDll = &entry;
    g_unarmedHandDll=nullptr;
    for (const auto& entry:kUnarmedHandDlls)
        if (entry.timestamp==d.timestamp) g_unarmedHandDll=&entry;
    if (g_motionDll && !g_hGetJoints.Install(
            reinterpret_cast<void*>(base+g_motionDll->getJoints),
            reinterpret_cast<void*>(&Detour_GetJoints),5,
            kGetJointsPrologue,sizeof(kGetJointsPrologue),"GetJoints"))
        Log("firstperson: body palette correction unavailable; native masking retained");
    for (const auto& entry:kShadowDlls) {
        if (entry.timestamp!=d.timestamp) continue;
        g_shadowDll=&entry;
        const uint8_t tr4Debug[]={0x40,0x55,0x53,0x57,0x48,0x8d,0x6c,0x24,0xe0};
        const uint8_t tr4Retail[]={0x40,0x55,0x53,0x57,0x48,0x8d,0x6c,0x24,0xb9};
        const uint8_t tr5[]={0x40,0x55,0x53,0x57,0x41,0x55};
        const auto* bytes=d.game==1 ? tr5 : retail ? tr4Retail : tr4Debug;
        const size_t size=d.game==1 ? sizeof(tr5) : sizeof(tr4Debug);
        if (!g_hDrawToShadow.Install(reinterpret_cast<void*>(base+entry.cast),
                reinterpret_cast<void*>(&Detour_DrawToShadow),size,bytes,size,"FirstPersonDrawToShadow"))
            Log("firstperson: shadow receiver origin correction unavailable");
        Log("firstperson: intact shadows with unarmed controller arms; eye-view body offsets excluded");
    }
    if (Cfg().firstPersonMotionGuns) {
        if (g_motionDll &&
            !std::memcmp(reinterpret_cast<const void*>(base+g_motionDll->getSpheres),
                kGetJointsPrologue,sizeof(kGetJointsPrologue)) &&
            !std::memcmp(reinterpret_cast<const void*>(base+g_motionDll->findTargetPoint),
                kGetFloor,sizeof(kGetFloor)) &&
            !std::memcmp(reinterpret_cast<const void*>(base+g_motionDll->lineOfSight),
                kGetFloor,sizeof(kGetFloor)) &&
            g_hGetJoints.installed() &&
            g_hAnimatePistols.Install(
                reinterpret_cast<void*>(base+g_motionDll->animatePistols),
                reinterpret_cast<void*>(&Detour_AnimatePistols),5,
                kAnimatePistols,sizeof(kAnimatePistols),"AnimatePistols") &&
            g_hFireWeapon.Install(
                reinterpret_cast<void*>(base + g_motionDll->fireWeapon),
                reinterpret_cast<void*>(&Detour_FireWeapon), 5,
                kFireWeaponPrologue, sizeof(kFireWeaponPrologue), "FireWeapon") &&
            g_hPistolHandler.Install(
                reinterpret_cast<void*>(base + g_motionDll->pistolHandler),
                reinterpret_cast<void*>(&Detour_PistolHandler), 5,
                d.game == 0 ? kPistolHandlerTR4 : kPistolHandlerTR5,
                5, "PistolHandler") &&
            g_hGetTargetOnLOS.Install(
                reinterpret_cast<void*>(base + g_motionDll->getTargetOnLOS),
                reinterpret_cast<void*>(&Detour_GetTargetOnLOS), 6,
                d.game == 0 ? kGetTargetOnLOSTR4 : kGetTargetOnLOSTR5,
                6, "GetTargetOnLOS") &&
            g_hSetGunFlash.Install(
                reinterpret_cast<void*>(base + g_motionDll->setGunFlash),
                reinterpret_cast<void*>(&Detour_SetGunFlash),
                d.game == 0 ? sizeof(kSetGunFlashTR4) : sizeof(kSetGunFlashTR5),
                d.game == 0 ? kSetGunFlashTR4 : kSetGunFlashTR5,
                d.game == 0 ? sizeof(kSetGunFlashTR4) : sizeof(kSetGunFlashTR5),
                "SetGunFlash")) {
            g_motionHooksReady = true;
            Log("firstperson: Touch bind-pose wrist + native muzzle/flash hooks ready");
            if (Cfg().firstPersonUnarmedIK)
                Log("firstperson: unarmed HD arm IK enabled, using shared gun calibration");
        } else {
            g_hSetGunFlash.Remove();
            g_hGetTargetOnLOS.Remove();
            g_hPistolHandler.Remove();
            g_hAnimatePistols.Remove();
            g_hFireWeapon.Remove();
            g_motionDll = nullptr;
            Log("firstperson: Touch dual-gun hooks unavailable; head aim preserved");
        }
    }
    if (!g_hGenerateW2V.Install(
            reinterpret_cast<void*>(base + d.phdGenerateW2V),
            reinterpret_cast<void*>(&Detour_GenerateW2V), 5,
            kGenerateW2VPrologue, sizeof(kGenerateW2VPrologue),
            "phd_GenerateW2V"))
        return false;
    for (const auto& entry:kActionIconDlls) if (entry.timestamp==d.timestamp) {
        static constexpr uint8_t prologue[]={0x4c,0x8b,0xdc,0x41,0x56};
        if (g_hDrawActionIndicators.Install(reinterpret_cast<void*>(base+entry.draw),
                reinterpret_cast<void*>(&Detour_DrawActionIndicators),sizeof(prologue),
                prologue,sizeof(prologue),"DrawActionIndicators")) {
            g_actionIconDll=&entry;
            Log("firstperson: nearby Action icon HUD placement hook ready");
        } else Log("firstperson: Action icon hook unavailable; native prompts preserved");
    }
    if (d.drawCreatureHD) {
        const bool retail4 = d.timestamp == 0x68C12FDA;
        const bool retail5 = d.timestamp == 0x68C12FE9;
        const uint8_t* expected = kDrawCreatureDebug;
        if (retail4) expected = kDrawCreatureRetail;
        if (retail5) expected = kDrawCreatureRetail;
        if (!g_hDrawCreatureHD.Install(
            reinterpret_cast<void*>(base + d.drawCreatureHD),
            reinterpret_cast<void*>(&Detour_DrawCreatureHD), 5,
            expected, 5, "DrawCreatureHD"))
            Log("firstperson: head geometry hook unavailable");
    }
    if (d.drawHair) {
        if (!g_hDrawHair.Install(
            reinterpret_cast<void*>(base + d.drawHair),
            reinterpret_cast<void*>(&Detour_DrawHair), 5,
            kDrawHairPrologue, sizeof(kDrawHairPrologue), "DrawHair"))
            Log("firstperson: hair hook unavailable");
    }
    if (g_motionHooksReady &&
        (!g_hDrawCreatureHD.installed() || !g_hDrawHair.installed())) {
        g_motionHooksReady = false;
        g_hSetGunFlash.Remove();
        g_hGetTargetOnLOS.Remove();
        g_hPistolHandler.Remove();
        g_hAnimatePistols.Remove();
        g_hFireWeapon.Remove();
        g_motionDll = nullptr;
        Log("firstperson: Touch dual-gun mode disabled; body/hair hooks unavailable");
    }
    if (g_motionHooksReady) {
        if (InstallLongGunHooks(d,base)) {
            g_longGunHooksReady=true;
            Log("firstperson: Touch all-weapon hooks ready (single weapons use right controller)");
        } else {
            RemoveLongGunHooks();
            Log("firstperson: Touch additional weapon hooks unavailable; dual guns preserved");
        }
    }
    if(g_longGunHooksReady) for(const auto& entry:kFlareDlls) if(entry.timestamp==d.timestamp) {
        const uint8_t inHand[]={0x40,0x53,0x48,0x83,0xec,0x40};
        const uint8_t previous[]={0x48,0x89,0x5c,0x24,0x08};
        g_flareHooksReady=g_hFlarePreviousJoint.Install(reinterpret_cast<void*>(base+entry.previousJoint),
            reinterpret_cast<void*>(&Detour_FlarePreviousJoint),5,previous,5,"FlarePreviousWrist") &&
            g_hFlareInHand.Install(reinterpret_cast<void*>(base+entry.inHand),
            reinterpret_cast<void*>(&Detour_FlareInHand),6,inHand,6,"FlareTrackedHand") &&
            g_hFlareDynamic.Install(reinterpret_cast<void*>(base+entry.triggerDynamic),
            reinterpret_cast<void*>(&Detour_FlareDynamic),5,previous,5,"FlareDynamicLight");
        if(!g_flareHooksReady) {g_hFlareInHand.Remove();g_hFlarePreviousJoint.Remove();g_hFlareDynamic.Remove();}
        Log(g_flareHooksReady ? "firstperson: left-controller flare hand and render-time tip light ready" :
            "firstperson: flare tracking unavailable; native flare retained");
    }
    if(g_motionHooksReady) for(const auto& entry:kCrouchDrawDlls) if(entry.timestamp==d.timestamp) {
        const uint8_t prologue[]={0x48,0x8B,0xC4,0x48,0x89,0x58,0x08};
        if(!g_hCrouchDraw.Install(reinterpret_cast<void*>(base+entry.draw),
            reinterpret_cast<void*>(&Detour_CrouchDraw),7,prologue,7,"CrouchTrackedHandVisibility"))
            Log("firstperson: crouched hand visibility hook unavailable");
    }
    if(g_motionHooksReady) for(const auto& entry:kCrouchGunDlls) if(entry.timestamp==d.timestamp) {
        const int ripOffset=7;
        g_crouchGunsReady=g_hCrouchGun.Install(reinterpret_cast<void*>(base+entry.gun),
            reinterpret_cast<void*>(&Detour_CrouchGun),11,entry.prologue,11,
            "CrouchMotionGuns",&ripOffset,1);
        Log(g_crouchGunsReady ? "firstperson: crouched tracked weapons enabled" :
            "firstperson: crouched tracked weapons unavailable; native restrictions retained");
    }
    InstallPhysicalSwitches(d,base);
    return true;
}

void Remove() {
    g_flareHooksReady=false;g_flareHandFrames=nullptr;g_flareHandHistory={};
    g_flareLights={};g_captureFlareLights=g_loggedFlareLight=false;
    g_hFlareInHand.Remove();g_hFlarePreviousJoint.Remove();g_hFlareDynamic.Remove();
    UpdateControlMode(false);
    ResetPhysicalHolsters();
    RemovePhysicalSwitches();
    g_hCrouchDraw.Remove();
    g_crouchGunsReady=false;g_crouchWeaponScope=nullptr;g_hCrouchGun.Remove();
    g_hDrawActionIndicators.Remove();
    g_actionIconDll=nullptr;
    g_loggedActionIcon=false;
    ResetMovementStabilization();
    RemoveLongGunHooks();
    g_gunTriggers.Reset();
    g_gunEquip.Reset();
    g_calibrationActive=false;
    g_cameraMotionTrace = {};
    if (g_active) VR().RecenterThirdPersonHead();
    RestoreHeadMesh();
    g_motionHooksReady = false;
    g_firingHand = g_renderArm = g_renderHandJoint = -1;
    g_hkScopeBindItem=nullptr;
    g_weaponLaser={};
    g_weaponAimBinding={};
    g_scenePoseValid = false;
    g_motionArmDraws[0] = g_motionArmDraws[1] = 0;
    g_motionShots[0] = g_motionShots[1] = 0;
    g_motionReportTime = 0;
    g_headAimFallbacks[0] = g_headAimFallbacks[1] = 0;
    g_gunPoseFailure[0] = g_gunPoseFailure[1] = "not-built";
    g_hSetGunFlash.Remove();
    g_hGetTargetOnLOS.Remove();
    g_hPistolHandler.Remove();
    g_hAnimatePistols.Remove();
    g_hFireWeapon.Remove();
    RestoreShadowReceiver();g_shadowReceiver=nullptr;g_shadowDepthScope=g_loggedShadowReceiver=false;
    g_hDrawToShadow.Remove();g_shadowDll=nullptr;g_unarmedHandDll=nullptr;
    g_hGetJoints.Remove();
    g_motionDll = nullptr;
    g_hDrawHair.Remove();
    g_hDrawCreatureHD.Remove();
    g_hGenerateW2V.Remove();
    g_hAimWeapon.Remove();
    g_hLaraAboveWater.Remove();
    g_hGetCollisionInfo.Remove();
    g_nativeLaraCollision=nullptr;
    g_hAnimateLara.Remove();
    g_active = false;
    g_haveHeading = false;
    g_headingItem = nullptr;
    g_dragPrevious = g_dragCurrent = g_dragShown = {};
    g_haveManualInput = false;
    g_groundMoveAction = 0;
    g_bodyTime = {};
    g_boundDll = nullptr;
    g_boundBase = 0;
}

bool UpdateGunTriggers(uint8_t& left, uint8_t& right, bool enabled, uint64_t now, bool y=false) {
    if (!enabled) {
        g_gunTriggers.Reset(); g_gunEquip.Reset(); g_triggerWeapon=0;
        return false;
    }
    int weapon=CurrentMotionWeapon();
    if (!weapon) weapon=*Ptr<int16_t>(g_boundDll->lara+8);
    if (weapon!=g_triggerWeapon) {
        if (MotionWeaponSupported(g_triggerWeapon) && MotionWeaponSupported(weapon)) {
            // Native drawing/inventory may change the selected gun during an
            // LT press. Retain draw intent, but never transfer a
            // queued press or repeat to the newly selected gun.
            g_gunTriggers.Clear(0); g_gunTriggers.Clear(1);
        } else {
            g_gunTriggers.Reset();
        }
        g_triggerWeapon=weapon;
    }
    const auto* app=reinterpret_cast<const uint8_t*>(Base()+L().app);
    // LaraGun selects the setting for classic/modern controls, not graphics.
    const unsigned controlScheme=(app[0x9e4]>>1)&1;
    const bool holdMode=app[0x9ec+controlScheme]==0;
    const int status=*Ptr<int16_t>(g_boundDll->lara+2);
    const bool rawLeft=left>30;
    if(y || (status==0 && rawLeft && !g_gunEquip.leftHeld)) ResetPhysicalHolsters();
    if(g_holsters.active) {
        if((status==4 && weapon!=g_holsters.weapon) ||
           (g_holsterWasReady && status==0 && g_holsters.mask) ||
           (g_holsterRequestTime && now-g_holsterRequestTime>3000 && status!=4 && g_holsters.mask)) {
            ResetPhysicalHolsters();
        } else {
            if(status==4) g_holsterWasReady=true;
            if(!g_holsters.mask && status==0) g_holsterWasReady=false;
            if(g_holsters.request) {
                if(g_holsters.mask) *Ptr<int16_t>(g_boundDll->lara+6)=int16_t(g_holsters.weapon);
                g_gunEquip.desiredArmed=g_holsters.mask!=0;g_gunEquip.pending=true;
                g_holsters.request=false;g_holsterRequestTime=now;
            }
        }
    }
    const bool nativeEquip=g_gunEquip.Update(holdMode,status,rawLeft,y);
    const bool motion=MotionTriggerMode();
    g_gunTriggers.Update(motion,motion && status==4 && MotionReady() && g_gunEquip.desiredArmed && !g_gunEquip.consumeY,
        rawLeft && !g_gunEquip.blockLeft,right>30,now);
    if (!motiongun::DualWeapon(weapon)) g_gunTriggers.Clear(0);
    if(g_holsters.active) {
        if(!g_holsters.CanFire(0,rawLeft)) g_gunTriggers.Clear(0);
        if(!g_holsters.CanFire(1,right>30)) g_gunTriggers.Clear(1);
    }
    left=nativeEquip ? 255 : 0;
    // RT also owns native grab/Action while the guns are away or Lara's hands
    // are busy. Keep its analog value AND hold duration in those states.
    if (status==4 && motion) right=g_gunTriggers.WantsShot() ? 255 : 0;
    if ((!g_gunEquip.desiredArmed || g_gunEquip.consumeY) && status==4) right=0;
    return g_gunEquip.consumeY;
}

} // namespace

bool FirstPersonGunTriggers(uint8_t& left, uint8_t& right, bool chordConsumed, bool y) {
    // XInput is already polling the VR controllers. Desktop focus must not
    // change their meaning: returning raw LT/RT here makes LT native Equip
    // and RT native paired fire while the hands remain controller-tracked.
    // Gate() still excludes menus, scripted cameras and unsupported FP states.
    const bool enabled=!chordConsumed && WeaponControlMode();
    return UpdateGunTriggers(left,right,enabled,GetTickCount64(),y);
}

bool FirstPersonShadowPass() { return g_shadowDepthScope; }
bool WeaponLaserAim(motiongun::Vec& muzzle,motiongun::Vec& direction,int16_t& room) {
    using namespace motiongun;
    if(!Cfg().enabled || !Cfg().worldLaserSight || !g_boundDll || !g_boundBase ||
       g_boundDll!=GameDllBound() || g_boundBase!=GameDllBase() || CurrentGame()!=g_boundDll->game ||
       InTitle() || InInventory() || InFMV() || AppState(drva::app_off::photoMode) ||
       ScriptedCameraActive() || IsOpticsZoomed()) return false;
    const int weapon=CurrentMotionWeapon();
    const auto* lara=Ptr<uint8_t>(g_boundDll->lara);
    // Native ControlPhase: bit 4 is the combined attachment, not ownership of
    // a loose sight. The HK scope is not a laser attachment.
    const bool attached=LaserAttachmentEquipped(weapon);
    if(!attached || *reinterpret_cast<const int16_t*>(lara+2)!=4) return false;
    auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    if(!item || *reinterpret_cast<const int16_t*>(item+off::item_hit_points)<=0 ||
       LaraWaterStatus()==1 || LaraWaterStatus()==2) return false;
    room=*reinterpret_cast<const int16_t*>(item+off::item_room);
    if(g_active) {
        if(g_holsters.active && (!g_holsters.Has(1) || g_holsters.hand[1].claimed)) return false;
        GunPose gun{};if(!BuildGunPose(1,gun)) return false;
        // Present consumes the exact palette used for this frame's weapon.
        // Do not fall back to guessed offsets when an attachment wasn't drawn.
        if(g_weaponLaser.item!=item || g_weaponLaser.base!=g_boundBase ||
           g_weaponLaser.serial!=VR().PoseSerial() || g_weaponLaser.game!=g_boundDll->game ||
           g_weaponLaser.weapon!=weapon) return false;
        muzzle=g_weaponLaser.origin;direction=g_weaponLaser.direction;
    } else {
        if(!g_boundDll->getJointAbsPositionLerp) return false;
        const auto local=MuzzleLocal(weapon,1,g_boundDll->game);
        muzzle=JointPoint(item,10,int(local.x),int(local.y),int(local.z));
        direction=Sub(JointPoint(item,10,0,1024,0),JointPoint(item,10,0,0,0));
    }
    const float length=std::sqrt(Dot(direction,direction));
    if(!std::isfinite(length) || length<.001f) return false;
    direction=Scale(direction,1/length);
    return std::isfinite(muzzle.x) && std::isfinite(muzzle.y) && std::isfinite(muzzle.z);
}
bool FirstPersonDrawingTrackedHands() { return g_renderArm>=0; }
int FirstPersonTrackedHandJoint() {
    return g_renderArm>=0 && g_renderHandJoint>=0 && g_renderHandJoint<72 ? g_renderHandJoint : -1;
}

bool FirstPersonHKScopePose(motiongun::Frame& lens, motiongun::Frame& camera,float* radiusMetres) {
    if(g_holsters.active && !g_holsters.Has(1)) return false;
    if (!Cfg().firstPersonHKScope || !g_boundDll || g_boundDll->game!=1 ||
        CurrentMotionWeapon()!=5 || !MotionReady() || IsOpticsZoomed() ||
        Cfg().monoTracking || !Cfg().duplicateDraws || Cfg().eyeOffsetMode!=3 ||
        !Cfg().perEyeView || Cfg().perEyeProjection!=1) return false;
    GunPose gun{};
    if (!BuildGunPose(1,gun)) return false;
    const float scale=LiveWorldUnitsPerMetre();
    lens={gun.desired,Add(gun.trackedHand,Transform(gun.desired,
        {0,Cfg().firstPersonHKScopeForwardMetres*scale,Cfg().firstPersonHKScopeUpMetres*scale}))};
    float radius=.03f;
    if (Cfg().firstPersonHKScopeMeshFit) {
        // The live object geometry may now be Lara's face. Use the binding
        // copied inside the actual HK draw, with THIS frame's controller pose.
        const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
        if (!item || g_hkScopeBindItem!=item) {
            static uint64_t lastWait=0;
            const auto now=GetTickCount64();
            if (now-lastWait>=5000) {
                Log("HK scope: waiting for right-hand HK mesh binding");
                lastWait=now;
            }
            return false;
        }
        float radiusUnits=0;
        if (!hkscope::MeshLens({gun.desired,gun.trackedHand},g_hkScopeInverseBind,lens,radiusUnits)) return false;
        radius=radiusUnits/scale;
        if (!std::isfinite(radius) || radius<.001f || radius>.15f) return false;
    }
    if (radiusMetres) *radiusMetres=radius;
    // Optical axis uses the same calibrated barrel frame and muzzle as shots.
    camera={gun.desired,gun.muzzle};
    return true;
}

bool FirstPersonHKScopeAiming() {
    motiongun::Frame lens{},camera{};
    float rot[3][3]{},pos[3]{};
    if (!FirstPersonHKScopePose(lens,camera) || !CameraViewFrame(rot,pos)) return false;
    const auto world=hkscope::WorldView(rot,pos);
    for (int eye=0;eye<2;++eye) {
        const auto eyeWorld=InvertRigid(Mul(VR().EyeView(Eye(eye)),world));
        if (hkscope::EyeBox(lens,{eyeWorld.r[0][3],eyeWorld.r[1][3],eyeWorld.r[2][3]},LiveWorldUnitsPerMetre())) return true;
    }
    return false;
}

void FirstPersonControlModeUpdate() {
    // Only frame/input boundaries. The crouch draw temporarily clears this
    // bit to bypass native proximity hiding; getters must not reassert it.
    UpdateControlMode(g_active && g_runtimeEnabled && g_boundDll && g_boundBase &&
        CurrentGame()==g_boundDll->game && (CurrentGame()==0 || CurrentGame()==1) &&
        Cfg().enabled && Cfg().firstPerson && VR().active());
}

void FirstPersonUpdate() {
    FirstPersonControlModeUpdate();
    if (!MotionReady() || CurrentMotionWeapon()!=5) g_hkScopeBindItem=nullptr;
    if (!MotionReady() || g_weaponLaser.weapon!=CurrentMotionWeapon()) g_weaponLaser.item=nullptr;
    if (!MotionReady() || g_weaponAimBinding.weapon!=CurrentMotionWeapon()) g_weaponAimBinding.item=nullptr;
    if (g_gunTriggers.active && !MotionTriggerMode()) {
        g_gunTriggers.Reset();
    }
    if (!WeaponControlMode()) g_gunEquip.Reset();
    PollMotionGunCalibration();
    ReportMotionActivity();
    static bool recenterHeld = false;
    const bool recenterDown = Cfg().firstPersonRecenterKey &&
        (GetAsyncKeyState(Cfg().firstPersonRecenterKey) & 0x8000) != 0;
    if (recenterDown && !recenterHeld && FirstPersonActive()) FirstPersonRecenter();
    recenterHeld = recenterDown;
    if (!g_runtimeInitialized) {
        g_runtimeInitialized = true;
        g_runtimeEnabled = false;
        Log("firstperson: startup is third person; Y+LT switches views");
    }
    const GameDllLayout* dll = GameDllBound();
    const uint64_t base = GameDllBase();
    bool want = Cfg().enabled && Cfg().firstPerson;
    if (!dll) want = false;
    if (!base) want = false;
    if (want) want = (*dll).phdGenerateW2V != 0;
    if (g_boundDll) {
        bool changed = !want;
        if (dll != g_boundDll) changed = true;
        if (base != g_boundBase) changed = true;
        if (changed) Remove();
    }
    if (!want) return;
    if (g_boundDll) return;
    if (base == g_failedBase) return;
    g_boundDll = dll;
    g_boundBase = base;
    if (!Install(*dll, base)) {
        Log("firstperson: required camera/aim/locomotion hook failed; mode remains third person");
        Remove();
        g_failedBase = base;
        return;
    }
    LogF("firstperson: %s camera + head-aim + directional/room-scale locomotion ready; joint %d",
         (*dll).name, Cfg().firstPersonJoint);
}

void FirstPersonRecenter() {
    g_autoBodyCalibration.pending=g_autoBodyCalibration.waiting=false;
    ResetBodyFollower();
    CancelHolsterTracking();
    ResetPhysicalSwitch();
    g_groundJump.Reset();
    g_ledgePull.Reset(); g_ledgeGrab.Reset();
    g_renderTurn.Reset();
    g_turnTrace={};
    g_rootMotion.Reset();
    g_cameraMotionTrace = {};
    VR().RecenterFirstPersonHead();
    g_dragPrevious = g_dragCurrent = g_dragShown = {};
    Log("firstperson: head position and height recalibrated; shared controller neutral; world heading preserved");
}

void FirstPersonTrackingOriginChanged(float yawCorrection) {
    CancelHolsterTracking();
    const float oldBase=g_headingBase;
    g_headingBase=Wrap(g_headingBase+yawCorrection);
    g_groundEye.Resume(true,oldBase,g_headingBase,0);
    FirstPersonRecenter();
    g_lastHeadWorld=Wrap(g_headingBase+VR().HeadYawRadians());
}

void FirstPersonToggle() {
    UpdateControlMode(false);
    ResetPhysicalHolsters();
    ResetPhysicalSwitch();
    g_unarmedArmVisibility={};
    g_unarmedTwist[0]=g_unarmedTwist[1]={};
    ResetMovementStabilization();
    g_calibrationActive=false;
    g_cameraMotionTrace = {};
    if (!g_runtimeInitialized) {
        g_runtimeEnabled = false;
        g_runtimeInitialized = true;
    }
    g_runtimeEnabled = !g_runtimeEnabled;
    g_active = false;
    g_haveHeading = false;
    g_headingItem = nullptr;
    g_dragPrevious = g_dragCurrent = g_dragShown = {};
    g_shifted = g_jumpPressed = false;
    g_bodyTime = {};
    g_loggedFirst = false;
    g_haveManualInput = false;
    g_groundMoveAction = 0;
    RestoreHeadMesh();
    VR().RecenterView();
    LogF("firstperson: switched to %s",
         g_runtimeEnabled ? "FIRST PERSON" : "third person");
}

void FirstPersonShutdown() {
    UpdateControlMode(false);
    g_calibrationActive=false;
    if (g_calibrationMessageHook) UnhookWindowsHookEx(g_calibrationMessageHook);
    g_calibrationMessageHook=nullptr;
    g_calibrationWindow=nullptr;
    g_calibrationKeys={};
    g_calibrationCommandCount=0;
    if (g_boundDll) Remove();
    g_failedBase = 0;
    g_runtimeEnabled = false;
    g_runtimeInitialized = false;
    g_loggedFirst = false;
    g_loggedLost = false;
    g_anchored = 0;
    g_skipped = 0;
    g_faceSkips = 0;
    g_hairSkips = 0;
}

bool FirstPersonCalibrationKeyReserved(int key) {
    if (!g_calibrationMessageHook || key<VK_F1 || key>VK_F7) return false;
    if (g_calibrationKeys.captured[key-VK_F1]) return true;
    return g_calibrationActive && CalibrationFocused() &&
        (GetAsyncKeyState(VK_CONTROL)&0x8000) && !(GetAsyncKeyState(VK_MENU)&0x8000);
}

const float* FirstPersonBodyPalette(uint64_t& mask,int& count) {
    mask=g_bodySkinMask; count=g_bodySkinCount;
    return g_bodySkinScope && g_bodySkinReady && g_renderArm<0 ? g_bodySkinPalette : nullptr;
}
const float* FirstPersonRenderBodyPalette(uint64_t& mask,int& count) {
    return FirstPersonBodyPalette(mask,count) ? g_bodyRenderPalette : nullptr;
}
uint64_t FirstPersonBodyWristSeamMask() {
    return g_bodyWristSeamScope && g_bodySkinScope && g_bodySkinReady && g_renderArm<0 ? g_bodyWristSeamMask : 0;
}

bool FirstPersonSwitchGrip(bool down, bool allowed) {
    return UpdatePhysicalSwitchGrip(down,allowed,GetTickCount64());
}

unsigned FirstPersonHolsterGrips(bool left,bool right,bool leftValid,bool rightValid,bool allowed) {
    return UpdatePhysicalHolsterGrips(left,right,leftValid,rightValid,
        allowed,GetTickCount64());
}

bool FirstPersonActive() {
    return g_active;
}

bool FirstPersonPhysicalCrouch(bool allowed) {
    const auto* item=g_boundDll && g_boundBase ? *Ptr<uint8_t*>(g_boundDll->laraItem) : nullptr;
    const bool enabled=allowed && Cfg().firstPersonFullBodyIK && Cfg().firstPersonPhysicalCrouch &&
        !ControllerTrackingBlockedReason() && item && (CanTurnBody(item) || CanSteerCrouch(item));
    const bool crouched=g_heightCrouch.Update(enabled,VR().FirstPersonHeightDrop());
    if(crouched) g_physicalCrouchCamera=true;
    return crouched;
}

void FirstPersonInput(float& leftX, float& leftY, float& rightX, bool shifted,
                      bool jumpPressed) {
    using namespace locomotion;
    g_haveManualInput = false;
    g_shifted = shifted;
    g_jumpPressed = jumpPressed;
    const auto* jumpItem=(g_boundDll && g_boundBase) ? *Ptr<uint8_t*>(g_boundDll->laraItem) : nullptr;
    g_groundJump.Bind(jumpItem,AppState(drva::app_off::level));
    g_groundJump.Observe(jumpPressed,GroundJumpReady(jumpItem),TurnTime());
    if (!g_active || !g_haveHeading || !Gate()) {
        g_renderTurn.Reset();
        return;
    }
    auto* item = *Ptr<uint8_t*>(g_boundDll->laraItem);
    if (!item || item != g_headingItem) { g_renderTurn.Reset(); return; }
    const float dead =
        std::clamp(Cfg().firstPersonTurnDeadzone, 0.0f, 0.95f);
    const float magnitude = std::min(1.0f, std::fabs(rightX));
    float turnRate=0;
    if (magnitude > dead) {
        const float strength = (magnitude - dead) / (1.0f - dead);
        turnRate = std::copysign(strength, rightX) *
            std::clamp(Cfg().firstPersonTurnDegreesPerSecond, 0.0f, 720.0f) *
            (kPi / 180.0f);
    }
    g_renderTurn.Sample(turnRate,TurnTime());
    if (Cfg().firstPersonDriftLog) ++g_turnTrace.polls;
    g_lastHeadWorld = Wrap(g_headingBase + VR().HeadYawRadians());
    const int state = *reinterpret_cast<int16_t*>(item + off::item_anim_state);
    const bool jump = IsJumpSteeringState(state) && LaraWaterStatus() == 0 &&
        *reinterpret_cast<int16_t*>(item + off::item_hit_points) > 0;
    // VR already integrates RS above. Do not also send it to native crouch
    // steering, which rotates the body/camera a second time.
    if(CanUseCrouchCamera(item)) rightX=0;
    if (!CanTurnBody(item) && !jump && !CanSteerMonkeyBars(item) && !CanSteerCrouch(item)) return;
    rightX = 0.0f;
    const float headWorld = Wrap(g_headingBase + VR().HeadYawRadians());
    g_lastHeadWorld = headWorld;
    Vec manual{leftX, leftY};
    if (Length(manual) < 0.20f || shifted) manual = {};
    g_manualLocal = manual;
    // Input is camera-relative to analogInput.camTurn, which is NOT the
    // rendered camera yaw. Using the latter fed native steering back into FP.
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
}

} // namespace tr
