#include "GameDll.h"
#include "Engine.h"
#include "FirstPerson.h"
#include "LocomotionMath.h"
#include "Log.h"

#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cwchar>

namespace tr {
namespace {

// --- struct offsets, identical across tomb1/2/3.dll -------------------------
//
// From tools\typedump.py. Only the offsets actually read are named; the structs
// are not redeclared, because a partial C++ struct that has to stay in step with
// three DLLs is a liability and an offset with the tool that produced it written
// beside it is not.
namespace off {
// lara_info, 432 bytes
constexpr uint32_t lara_water_status = 12;   // int16
constexpr uint32_t item_anim_state   = 18;   // int16
constexpr uint32_t item_flags        = 484;  // uint16, gravity_status bit 3

// camera_info, 128 bytes. camera.pos is a game_vector at +0:
//   +0 x (int32)  +4 y (int32)  +8 z (int32)  +12 room_number (int16)
constexpr uint32_t camera_pos_y       = 4;    // int32
constexpr uint32_t camera_pos_room    = 12;   // int16

// ROOM_INFO, 168 bytes
constexpr uint32_t room_stride     = 168;
constexpr uint32_t room_maxceiling = 56;   // int32, the ceiling's Y
} // namespace off

// Every RVA read out of the matching PDB, by name. Re-derived and diffed by
// tools\verify_addresses.py, which fails the build check if any of them moves.
//
//   python tools\pdbdump.py PDB\tomb1.dll draw_rooms w2v_matrix PrintRoomsList
//
// One block per build, three rows per block, indexed [build][gGame]. Column
// order matches GameDllLayout in the header. The five `outside*` entries
// are zero for TR1 because TR1's renderer has no such state: TR2 and TR3 track
// a separate screen rect for the sky, TR1 does not.
constexpr GameDllLayout kDlls[][3] = {
    // Stock build -- read out of the PDBs in PDB\.
    {
        { L"tomb1.dll", "Tomb Raider I",   0x6A4B48FF,
          /* lara            */ 0x0033BC00,
          /* camera          */ 0x0041DF60,
          /* room            */ 0x0041E088,
          /* number_rooms    */ 0x0041DF50,
          /* draw_rooms      */ 0x0041DDC0,
          /* number_draw_..  */ 0x0041DF54,
          /* w2v_matrix      */ 0x002C8C40,
          /* phd_mxptr       */ 0x0010E928,
          /* phd_winxmax     */ 0x002C8C04,
          /* phd_winymax     */ 0x002C8C00,
          /* outside         */ 0, 0, 0, 0, 0,
          /* PrintRoomsList  */ 0x0006E950,
          /* S_GetObjectB..  */ 0x00063DF0,
          /* DrawSkyHD       */ 0x0006E480,
          /* phd_GenerateW2V */ 0x000735B0,
          /* w2v_scene_retn  */ 0x00063586,
          /* frame_frac      */ 0x002C8BF8,
          /* lara_item       */ 0x0033BDB0,
          /* DrawCreatureHD  */ 0x0006BA60,
          /* GetJoints       */ 0x0006B370,
          /* joints          */ 0x002FC940,
          /* LaraGun         */ 0x00027D60,
          /* FireWeapon      */ 0x00029010,
          /* fire_w2v_return */ 0x000291EB,
          /* right_fire_return */ 0x00027B39,
          /* left_fire_return */ 0x00027CC6,
          /* GetTargetOnLOS  */ 0x00028EC0,
          /* hit_los_return  */ 0x0002938C,
          /* miss_los_return */ 0x0002952E,
          /* DrawLaraHD      */ 0x0006D9A0,
          /* DrawHair        */ 0x00055430,
          /* gLaraHead       */ 0x003E1460,
          /* gActorHead      */ 0x003E7040,
          /* objects         */ 0x0041E0C0,
          /* analogInput     */ 0x0041DFE0,
          /* input           */ 0x0041DD94,
          /* LaraAboveWater  */ 0x00020EA0,
          /* AnimateLara     */ 0x00029E70,
          /* GetCollisionInfo*/ 0x00006690,
          /* UpdateLaraRoom  */ 0x00007590,
          /* GetFloor        */ 0x00009800,
          /* CalculateLaraMatrices */ 0x00013310,
          /* DrawActionIndicators */ 0x00072860,
          /* nActionIndicator */ 0x003D6000,
          /* ActionIndicator */ 0x003E2620,
          /* phd_persp */ 0x002C8C70,
          /* phd_centerx */ 0x002C8C24,
          /* phd_centery */ 0x002C8C28,
          /* phd_znear */ 0x002C8BB4,
          /* phd_zfar */ 0x002C8BFC,
          /* next_item_free */ 0x003ED9EC,
          /* items */ 0x0041E098,
          /* FireHarpoon/Rocket/Grenade */ 0, 0, 0,
          /* ItemNewRoom */ 0x00020800,
          /* AnimateShotgun */ 0x000268F0,
          /* DrawGunFlash  */ 0x00010040,
          /* next_item_active  */ 0x003EC6D0,
          /* GetSpheres        */ 0x00043C50,
          /* find_target_point */ 0x00028CA0,
          /* LOS               */ 0x00058E50 },

        { L"tomb2.dll", "Tomb Raider II",  0x6A4B4915,
          /* lara            */ 0x0037AD40,
          /* camera          */ 0x00433160,
          /* room            */ 0x0045D220,
          /* number_rooms    */ 0x00433070,
          /* draw_rooms      */ 0x00432EE0,
          /* number_draw_..  */ 0x0043307C,
          /* w2v_matrix      */ 0x00307D60,
          /* phd_mxptr       */ 0x001490F8,
          /* phd_winxmax     */ 0x00307DA0,
          /* phd_winymax     */ 0x00307D9C,
          /* outside         */ 0x0042CE94,
          /* outside_left    */ 0x00538070,
          /* outside_right   */ 0x00538060,
          /* outside_top     */ 0x00538068,
          /* outside_bottom  */ 0x00538064,
          /* PrintRoomsList  */ 0x000A0C30,
          /* S_GetObjectB..  */ 0x00095740,
          /* DrawSkyHD       */ 0x000A0760,
          /* phd_GenerateW2V */ 0x000A7EC0,
          /* w2v_scene_retn  */ 0x00094ED6,
          /* frame_frac      */ 0x00307D98,
          /* lara_item       */ 0x0037AEF0,
          /* DrawCreatureHD  */ 0x0009D9D0,
          /* GetJoints       */ 0x0009D2E0,
          /* joints          */ 0x0033BA80,
          /* LaraGun         */ 0x0004B3F0,
          /* FireWeapon      */ 0x0004CDF0,
          /* fire_w2v_return */ 0x0004CFD0,
          /* right_fire_return */ 0x00049949,
          /* left_fire_return */ 0x00049C0B,
          /* GetTargetOnLOS  */ 0x0004CAC0,
          /* hit_los_return  */ 0x0004D155,
          /* miss_los_return */ 0x0004D204,
          /* DrawLaraHD      */ 0x0009FC80,
          /* DrawHair        */ 0x00087BE0,
          /* gLaraHead       */ 0x00422900,
          /* gActorHead      */ 0x004284C0,
          /* objects         */ 0x0045D240,
          /* analogInput     */ 0x004330A0,
          /* input           */ 0x004331E0,
          /* LaraAboveWater  */ 0x0003E9C0,
          /* AnimateLara     */ 0x00054290,
          /* GetCollisionInfo*/ 0x0000A3C0,
          /* UpdateLaraRoom  */ 0x0000B370,
          /* GetFloor        */ 0x0000DE90,
          /* CalculateLaraMatrices */ 0x0001CE80,
          /* DrawActionIndicators */ 0x000A51A0,
          /* nActionIndicator */ 0x004174A0,
          /* ActionIndicator */ 0x00423AA0,
          /* phd_persp */ 0x00307D90,
          /* phd_centerx */ 0x00307D54,
          /* phd_centery */ 0x00307D58,
          /* phd_znear */ 0x00307D14,
          /* phd_zfar */ 0x00307D50,
          /* next_item_free */ 0x0042CA08,
          /* items */ 0x00530A40,
          /* FireHarpoon/Rocket/Grenade */ 0x00046EE0, 0, 0,
          /* ItemNewRoom */ 0x0003E560,
          /* AnimateShotgun */ 0x000482A0,
          /* DrawGunFlash  */ 0x00018360,
          /* next_item_active  */ 0x0042B6E6,
          /* GetSpheres        */ 0x000791D0,
          /* find_target_point */ 0x0004C890,
          /* LOS               */ 0x0008B390 },

        { L"tomb3.dll", "Tomb Raider III", 0x6A4B490D,
          /* lara            */ 0x003D1C40,
          /* camera          */ 0x00491FA0,
          /* room            */ 0x00492020,
          /* number_rooms    */ 0x00491150,
          /* draw_rooms      */ 0x00490FC0,
          /* number_draw_..  */ 0x00491154,
          /* w2v_matrix      */ 0x0035EC80,
          /* phd_mxptr       */ 0x0019DA98,
          /* phd_winxmax     */ 0x0035EC30,
          /* phd_winymax     */ 0x0035EC2C,
          /* outside         */ 0x0048B708,
          /* outside_left    */ 0x005977B0,
          /* outside_right   */ 0x005977A0,
          /* outside_top     */ 0x005977A8,
          /* outside_bottom  */ 0x005977A4,
          /* PrintRoomsList  */ 0x000EAF50,
          /* S_GetObjectB..  */ 0x000DFB70,
          /* DrawSkyHD       */ 0x000EAA80,
          /* phd_GenerateW2V */ 0x000F3F60,
          /* w2v_scene_retn  */ 0x000DF306,
          /* frame_frac      */ 0x0035EC24,
          /* lara_item       */ 0x003D1DF0,
          /* DrawCreatureHD  */ 0x000E7CC0,
          /* GetJoints       */ 0x000E75D0,
          /* joints          */ 0x0037E960,
          /* LaraGun         */ 0x00069220,
          /* FireWeapon      */ 0x0006ABE0,
          /* fire_w2v_return */ 0x0006AD88,
          /* right_fire_return */ 0x000672C8,
          /* left_fire_return */ 0x000675AB,
          /* GetTargetOnLOS  */ 0x0006A9D0,
          /* hit_los_return  */ 0x0006AEB1,
          /* miss_los_return */ 0x0006B0E0,
          /* DrawLaraHD      */ 0x000E9FA0,
          /* DrawHair        */ 0x000CEBA0,
          /* gLaraHead       */ 0x00476880,
          /* gActorHead      */ 0x0047A040,
          /* objects         */ 0x004BC060,
          /* analogInput     */ 0x00491F40,
          /* input           */ 0x00491F74,
          /* LaraAboveWater  */ 0x00058810,
          /* AnimateLara     */ 0x00076F90,
          /* GetCollisionInfo*/ 0x00016D30,
          /* UpdateLaraRoom  */ 0x00017F20,
          /* GetFloor        */ 0x00022810,
          /* CalculateLaraMatrices */ 0x00031290,
          /* DrawActionIndicators */ 0x000EF680,
          /* nActionIndicator */ 0x0046B420,
          /* ActionIndicator */ 0x00477A20,
          /* phd_persp */ 0x0035EC58,
          /* phd_centerx */ 0x0035EC50,
          /* phd_centery */ 0x0035EC54,
          /* phd_znear */ 0x0035EBF4,
          /* phd_zfar */ 0x0035EC28,
          /* next_item_free */ 0x004822A8,
          /* items */ 0x004BC048,
          /* FireHarpoon/Rocket/Grenade */ 0x00062D60, 0x000639F0, 0x00064B10,
          /* ItemNewRoom */ 0x00053640,
          /* AnimateShotgun */ 0x00065B90,
          /* DrawGunFlash  */ 0x0002B8A0,
          /* next_item_active  */ 0x0048B1E0,
          /* GetSpheres        */ 0x000B2E70,
          /* find_target_point */ 0x0006A7C0,
          /* LOS               */ 0x000D2680 },
    },

    // Aspyr retail build, shipped without PDBs (exe row: kBuildAspyrRetail in
    // Engine.h). The Tomb Raider Gold mod patches these DLLs in place without
    // moving anything or changing their timestamps, so it uses these rows too.
    // Carried across by tools\port_build.py, which maps each global through the
    // RIP-relative references to it in functions matched between the builds.
    // Every global won its vote unanimously except tomb2 `room`, at 152 of 154,
    // and all three hook prologues are byte-identical to the stock ones.
    // Struct layouts are unchanged: the functions reading the fields below
    // (PrintRooms, SetRoomBounds, CalculateCamera, S_GetObjectBounds...) use the
    // same displacements in both builds.
    {
        { L"tomb1.dll", "Tomb Raider I", 0x6A4B7C28,
          /* lara            */ 0x0033CB40,
          /* camera          */ 0x0041EEA0,
          /* room            */ 0x0041EFC8,
          /* number_rooms    */ 0x0041EE90,
          /* draw_rooms      */ 0x0041ED00,
          /* number_draw_..  */ 0x0041EE94,
          /* w2v_matrix      */ 0x002C9B80,
          /* phd_mxptr       */ 0x0010F928,
          /* phd_winxmax     */ 0x002C9B44,
          /* phd_winymax     */ 0x002C9B40,
          /* outside         */ 0, 0, 0, 0, 0,
          /* PrintRoomsList  */ 0x0006ED60,
          /* S_GetObjectB..  */ 0x00063E30,
          /* DrawSkyHD       */ 0x0006E890,
          /* phd_GenerateW2V */ 0x00073A40,
          /* w2v_scene_retn  */ 0x000635B8,
          /* frame_frac      */ 0x002C9B38,
          /* lara_item       */ 0x0033CCF0,
          /* DrawCreatureHD  */ 0x0006BE70,
          /* GetJoints       */ 0x0006B780,
          /* joints          */ 0x002FD880,
          /* LaraGun         */ 0x00027F00,
          /* FireWeapon      */ 0x000291B0,
          /* fire_w2v_return */ 0x00029393,
          /* right_fire_return */ 0x00027CCB,
          /* left_fire_return */ 0x00027E5A,
          /* GetTargetOnLOS  */ 0x00029060,
          /* hit_los_return  */ 0x0002952C,
          /* miss_los_return */ 0x000296D0,
          /* DrawLaraHD      */ 0x0006DDB0,
          /* DrawHair        */ 0x000553E0,
          /* gLaraHead       */ 0x003E23A0,
          /* gActorHead      */ 0x003E7F80,
          /* objects         */ 0x0041F000,
          /* analogInput     */ 0x0041EF20,
          /* input           */ 0x0041ECD4,
          /* LaraAboveWater  */ 0x00020FE0,
          /* AnimateLara     */ 0x0002A010,
          /* GetCollisionInfo*/ 0x00006690,
          /* UpdateLaraRoom  */ 0x00007570,
          /* GetFloor        */ 0x00009820,
          /* CalculateLaraMatrices */ 0x00013340,
          /* DrawActionIndicators */ 0x00072CE0,
          /* nActionIndicator */ 0x003D6F40,
          /* ActionIndicator */ 0x003E3560,
          /* phd_persp */ 0x002C9BB0,
          /* phd_centerx */ 0x002C9B64,
          /* phd_centery */ 0x002C9B68,
          /* phd_znear */ 0x002C9AF4,
          /* phd_zfar */ 0x002C9B3C,
          /* next_item_free */ 0x003EE92C,
          /* items */ 0x0041EFD8,
          /* FireHarpoon/Rocket/Grenade */ 0, 0, 0,
          /* ItemNewRoom */ 0x00020940,
          /* AnimateShotgun */ 0x00026A50,
          /* DrawGunFlash  */ 0x00010070,
          /* next_item_active  */ 0x003ED610,
          /* GetSpheres        */ 0x00043B30,
          /* find_target_point */ 0x00028E40,
          /* LOS               */ 0x00058D60 },

        { L"tomb2.dll", "Tomb Raider II", 0x6A4B7C3F,
          /* lara            */ 0x0037AC80,
          /* camera          */ 0x004330A0,
          /* room            */ 0x0045D160,
          /* number_rooms    */ 0x00432FB0,
          /* draw_rooms      */ 0x00432E20,
          /* number_draw_..  */ 0x00432FBC,
          /* w2v_matrix      */ 0x00307CA0,
          /* phd_mxptr       */ 0x001490F8,
          /* phd_winxmax     */ 0x00307CE0,
          /* phd_winymax     */ 0x00307CDC,
          /* outside         */ 0x0042CDD4,
          /* outside_left    */ 0x00537FB0,
          /* outside_right   */ 0x00537FA0,
          /* outside_top     */ 0x00537FA8,
          /* outside_bottom  */ 0x00537FA4,
          /* PrintRoomsList  */ 0x000A0AF0,
          /* S_GetObjectB..  */ 0x00095230,
          /* DrawSkyHD       */ 0x000A0620,
          /* phd_GenerateW2V */ 0x000A7DC0,
          /* w2v_scene_retn  */ 0x000949B8,
          /* frame_frac      */ 0x00307CD8,
          /* lara_item       */ 0x0037AE30,
          /* DrawCreatureHD  */ 0x0009D890,
          /* GetJoints       */ 0x0009D1A0,
          /* joints          */ 0x0033B9C0,
          /* LaraGun         */ 0x0004B420,
          /* FireWeapon      */ 0x0004CE20,
          /* fire_w2v_return */ 0x0004D002,
          /* right_fire_return */ 0x0004997A,
          /* left_fire_return */ 0x00049C3E,
          /* GetTargetOnLOS  */ 0x0004CAF0,
          /* hit_los_return  */ 0x0004D195,
          /* miss_los_return */ 0x0004D245,
          /* DrawLaraHD      */ 0x0009FB40,
          /* DrawHair        */ 0x00087650,
          /* gLaraHead       */ 0x00422840,
          /* gActorHead      */ 0x00428400,
          /* objects         */ 0x0045D180,
          /* analogInput     */ 0x00432FE0,
          /* input           */ 0x00433120,
          /* LaraAboveWater  */ 0x0003E950,
          /* AnimateLara     */ 0x00054300,
          /* GetCollisionInfo*/ 0x0000A3C0,
          /* UpdateLaraRoom  */ 0x0000B350,
          /* GetFloor        */ 0x0000DE80,
          /* CalculateLaraMatrices */ 0x0001CEB0,
          /* DrawActionIndicators */ 0x000A50D0,
          /* nActionIndicator */ 0x004173E0,
          /* ActionIndicator */ 0x004239E0,
          /* phd_persp */ 0x00307CD0,
          /* phd_centerx */ 0x00307C94,
          /* phd_centery */ 0x00307C98,
          /* phd_znear */ 0x00307C54,
          /* phd_zfar */ 0x00307C90,
          /* next_item_free */ 0x0042C948,
          /* items */ 0x00530980,
          /* FireHarpoon/Rocket/Grenade */ 0x00046EB0, 0, 0,
          /* ItemNewRoom */ 0x0003E4F0,
          /* AnimateShotgun */ 0x000482A0,
          /* DrawGunFlash  */ 0x00018390,
          /* next_item_active  */ 0x0042B626,
          /* GetSpheres        */ 0x00078C10,
          /* find_target_point */ 0x0004C8C0,
          /* LOS               */ 0x0008AD60 },

        { L"tomb3.dll", "Tomb Raider III", 0x6A4B7C37,
          /* lara            */ 0x003D4B80,
          /* camera          */ 0x00494EE0,
          /* room            */ 0x00494F60,
          /* number_rooms    */ 0x00494090,
          /* draw_rooms      */ 0x00493F00,
          /* number_draw_..  */ 0x00494094,
          /* w2v_matrix      */ 0x00361BC0,
          /* phd_mxptr       */ 0x001A0A98,
          /* phd_winxmax     */ 0x00361B70,
          /* phd_winymax     */ 0x00361B6C,
          /* outside         */ 0x0048E648,
          /* outside_left    */ 0x0059A6F0,
          /* outside_right   */ 0x0059A6E0,
          /* outside_top     */ 0x0059A6E8,
          /* outside_bottom  */ 0x0059A6E4,
          /* PrintRoomsList  */ 0x000ECC60,
          /* S_GetObjectB..  */ 0x000E14C0,
          /* DrawSkyHD       */ 0x000EC790,
          /* phd_GenerateW2V */ 0x000F5D60,
          /* w2v_scene_retn  */ 0x000E0C48,
          /* frame_frac      */ 0x00361B64,
          /* lara_item       */ 0x003D4D30,
          /* DrawCreatureHD  */ 0x000E99D0,
          /* GetJoints       */ 0x000E92E0,
          /* joints          */ 0x003818A0,
          /* LaraGun         */ 0x00069510,
          /* FireWeapon      */ 0x0006AEF0,
          /* fire_w2v_return */ 0x0006B09A,
          /* right_fire_return */ 0x000675B8,
          /* left_fire_return */ 0x0006789B,
          /* GetTargetOnLOS  */ 0x0006ACD0,
          /* hit_los_return  */ 0x0006B1C1,
          /* miss_los_return */ 0x0006B3F2,
          /* DrawLaraHD      */ 0x000EBCB0,
          /* DrawHair        */ 0x000D04D0,
          /* gLaraHead       */ 0x004797C0,
          /* gActorHead      */ 0x0047CF80,
          /* objects         */ 0x004BEFA0,
          /* analogInput     */ 0x00494E80,
          /* input           */ 0x00494EB4,
          /* LaraAboveWater  */ 0x00058A20,
          /* AnimateLara     */ 0x00076F50,
          /* GetCollisionInfo*/ 0x00016F20,
          /* UpdateLaraRoom  */ 0x00018110,
          /* GetFloor        */ 0x00022910,
          /* CalculateLaraMatrices */ 0x00031360,
          /* DrawActionIndicators */ 0x000F13F0,
          /* nActionIndicator */ 0x0046E360,
          /* ActionIndicator */ 0x0047A960,
          /* phd_persp */ 0x00361B98,
          /* phd_centerx */ 0x00361B90,
          /* phd_centery */ 0x00361B94,
          /* phd_znear */ 0x00361B34,
          /* phd_zfar */ 0x00361B68,
          /* next_item_free */ 0x004851E8,
          /* items */ 0x004BEF88,
          /* FireHarpoon/Rocket/Grenade */ 0x00062F80, 0x00063C50, 0x00064DA0,
          /* ItemNewRoom */ 0x000537D0,
          /* AnimateShotgun */ 0x00065E50,
          /* DrawGunFlash  */ 0x0002B970,
          /* next_item_active  */ 0x0048E120,
          /* GetSpheres        */ 0x000B45B0,
          /* find_target_point */ 0x0006AAC0,
          /* LOS               */ 0x000D3F10 },
    },
};

const GameDllLayout* g_dll  = nullptr;
uint64_t         g_base = 0;
bool             g_loggedStamp   = false;
bool             g_loggedNoRooms = false;

template <typename T>
T Read(uint32_t rva) {
    return *reinterpret_cast<T*>(g_base + rva);
}

} // namespace

bool GameDllUpdate() {
    // WHICH DLL, AND WHY NOT "THE ONE THAT IS LOADED"
    //
    // ALL THREE game DLLs are resident for the whole session. WinMain loads them
    // unconditionally at startup, one after another, setting gGame to 0, 1 and 2
    // around each call so that each DLL's initialiser sees its own index:
    //
    //   00007924  mov  [gGame], r15d      ; 0
    //   0000792B  call LoadLibraryA       ; "tomb1.dll"
    //   00007991  mov  [gGame], r14d      ; 1
    //   00007998  call LoadLibraryA       ; "tomb2.dll"
    //   000079FE  mov  [gGame], r13d      ; 2
    //   00007A05  call LoadLibraryA       ; "tomb3.dll"
    //   00007ABB  mov  [gGame], eax       ; the game actually selected
    //
    // So GetModuleHandleW succeeds for all three at all times, and picking "the
    // first one that is loaded" silently always picks tomb1.dll. That was the
    // first version of this function, and it was WRONG: playing TR2 or TR3, it
    // read Lara's water status and the camera's room out of TR1's globals. It
    // was caught by a log line reading `gGame=1` (Tomb Raider II) next to
    // `bound to tomb1.dll` -- the two disagreeing is the whole symptom.
    //
    // gGame is therefore the selector, and it is authoritative: WinMain indexes
    // its own per-game dispatch table with it (`movsxd rax, [gGame]; shl rax, 5;
    // call [rax + rsi + 0x41d218]` at 0x00007BF2), so 0/1/2 map to TR1/TR2/TR3
    // by the engine's own definition rather than by our assumption.
    const int game = CurrentGame();
    if (game < 0 || game >= 3) {
        // Before a game is chosen, or mid-transition. Report unbound rather than
        // guess: both callers treat that as "unknown" and fail safe.
        g_dll  = nullptr;
        g_base = 0;
        return false;
    }

    // Already bound to the right one, and it has not moved. Every build's row
    // for a given game names the same module, so kDlls[0] stands in for all.
    const wchar_t* module = kDlls[0][game].module;
    if (g_dll && std::wcscmp(g_dll->module, module) == 0 &&
        GetModuleHandleW(module) == reinterpret_cast<HMODULE>(g_base))
        return true;

    g_dll  = nullptr;
    g_base = 0;

    HMODULE h = GetModuleHandleW(module);
    if (!h) return false;

    const uint64_t base = reinterpret_cast<uint64_t>(h);

    // Pick the row by the DLL's own PE timestamp, and refuse a DLL no row
    // describes. This used to accept a mismatch on the grounds that nothing
    // here is WRITTEN -- true of the two readers in this file, but PortalCull
    // writes draw_rooms, number_draw_rooms and every listed room's clip rect
    // through this same row, and Sky hooks through it. Unbound is the safe
    // answer: every consumer already treats it as "stand down".
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    const GameDllLayout* row = nullptr;
    for (const auto& build : kDlls) {
        if (build[game].timestamp == stamp) {
            row = &build[game];
            break;
        }
    }
    if (!row) {
        if (!g_loggedStamp) {
            g_loggedStamp = true;
            LogF("gamedll: %S has PE timestamp 0x%08X, which no address table "
                 "describes. Not binding: ceiling clamp, water-state detection, "
                 "culling fix and sky fix stand down for this DLL.", module, stamp);
        }
        return false;
    }
    const GameDllLayout& d = *row;

    g_dll  = &d;
    g_base = base;

    // Logged on every rebind, not once: the player can switch games from the
    // title screen without restarting, and which DLL we are reading is exactly
    // the thing that silently went wrong before. gGame is printed alongside so
    // the two can be checked against each other at a glance.
    LogF("gamedll: bound to %S (%s, gGame=%d) at %p",
         d.module, d.name, game, (void*)base);
    return true;
}

const GameDllLayout* GameDllBound() { return g_dll; }
uint64_t             GameDllBase()  { return g_base; }

int LaraWaterStatus() {
    if (!g_dll) return -1;
    return Read<int16_t>(g_dll->lara + off::lara_water_status);
}

bool CameraHeadroom(float& units) {
    units = 0.0f;
    if (!g_dll) return false;
    const auto* item=Read<uint8_t*>(g_dll->laraItem);
    static const uint8_t* lastItem=nullptr;
    static uint64_t resumeClampAt=0;
    static bool reportedJump=false;
    const uint64_t now=GetTickCount64();
    if (item!=lastItem) {
        lastItem=item;
        resumeClampAt=0;
        reportedJump=false;
    }
    if (item) {
        const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
        const uint16_t flags=*reinterpret_cast<const uint16_t*>(item+off::item_flags);
        if (locomotion::IsJumpOrFallState(state) || (flags&0x8u)) {
            resumeClampAt=now+250;
            if (!reportedJump) {
                reportedJump=true;
                LogF("vr: ceiling clamp paused for jump state=%d first=%d",
                     state,int(FirstPersonActive()));
            }
        } else if (now>=resumeClampAt) {
            reportedJump=false;
        }
        if (now<resumeClampAt) return false;
    }

    // `room` is a POINTER to the rooms array, null until a level is loaded.
    auto* rooms = Read<uint8_t*>(g_dll->room);
    const int16_t numRooms = Read<int16_t>(g_dll->numberRooms);
    if (!rooms || numRooms <= 0) {
        if (!g_loggedNoRooms) {
            g_loggedNoRooms = true;
            Log("gamedll: no level loaded yet -- ceiling clamp stands down until "
                "there is one (this is normal at the title screen)");
        }
        return false;
    }

    int16_t roomNo = Read<int16_t>(g_dll->camera + off::camera_pos_room);
    int32_t eye[3]{};
    const bool firstPersonEye=FirstPersonSceneEye(eye);
    if (FirstPersonActive() && !firstPersonEye) return false;
    if (firstPersonEye) {
        if (roomNo < 0 || roomNo >= numRooms || !g_dll->getFloor) return false;
        using Fn_GetFloor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
        if (!reinterpret_cast<Fn_GetFloor>(g_base+g_dll->getFloor)(
                eye[0],eye[1],eye[2],&roomNo)) return false;
    }
    if (roomNo < 0 || roomNo >= numRooms) return false;

    const int32_t camY = firstPersonEye ? eye[1] :
        Read<int32_t>(g_dll->camera + off::camera_pos_y);
    const int32_t ceil =
        *reinterpret_cast<int32_t*>(rooms + static_cast<uint32_t>(roomNo) * off::room_stride
                                          + off::room_maxceiling);

    // TR world space is Y-DOWN: the ceiling has a SMALLER Y than anything below
    // it, so headroom is (view Y - ceiling Y) and is positive when the view
    // is below the ceiling. Getting this backwards would produce a negative
    // number that the check below rejects, which is the intended failure.
    const int32_t headroom = camY - ceil;

    // Sanity. A sector is 1024 units and Lara is about 762 tall, so a real
    // headroom is a few hundred to a few thousand units. Anything outside that
    // means a level transition or stale room, so report "unknown" rather
    // than clamp the player's head on a bad number.
    if (headroom <= 0 || headroom > 32768) return false;

    units = static_cast<float>(headroom);
    return true;
}

void GameDllShutdown() {
    g_dll  = nullptr;
    g_base = 0;
}

} // namespace tr
