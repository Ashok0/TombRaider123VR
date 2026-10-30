#include "GameDll.h"
#include "Engine.h"
#include "Log.h"

#include <windows.h>
#include <cmath>
#include <cstdint>

namespace tr {
namespace {

// --- struct offsets, identical in tomb4.dll and tomb5.dll -------------------
//
// From tools\typedump.py. Only the offsets actually read are named; the structs
// are not redeclared, because a partial C++ struct that has to stay in step with
// two DLLs is a liability and an offset with the tool that produced it written
// beside it is not.
namespace off {
// lara_info. water_status is at +12, the same place TR1-3's PDBs put it -- which
// is the confirmation that the old memory-diff hunt found the right field.
constexpr uint32_t lara_water_status = 12;   // int16

// camera_info, 112 bytes. camera.pos is a GAME_VECTOR at +0:
//   +0 x (int32)  +4 y (int32)  +8 z (int32)  +12 room_number (int16)
constexpr uint32_t camera_pos_y    = 4;    // int32
constexpr uint32_t camera_pos_room = 12;   // int16

// ROOM_INFO, 304 bytes
constexpr uint32_t room_stride     = 304;
constexpr uint32_t room_maxceiling = 56;   // int32, the ceiling's Y
} // namespace off

// PrintRoomsList's prologue. Different in the two DLLs, which is why it travels
// in the row rather than sitting beside the hook:
//
//   TR4  48 8B C4     mov rax, rsp
//        41 54        push r12          -> 5 bytes, both position-independent
//   TR5  48 89 5C 24 10  mov [rsp+0x10], rbx  -> 5 bytes, position-independent
//
// Verified against the real .text by tools\verify_addresses.py, which also
// checks that each window ends on an instruction boundary and contains no
// RIP-relative operand.
const uint8_t kPrintRoomsListTR4[] = { 0x48, 0x8B, 0xC4, 0x41, 0x54 };
const uint8_t kPrintRoomsListTR5[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };

// DrawSkyHD, DrawVCIHeadset and DrawLabyrinthFishEye open with the same
// instruction in both DLLs, `mov [rsp+N], rbx`, but the retail compiler picked
// a different home slot for rbx than the debug build did:
//
//                          debug (PDB)         retail
//   DrawSkyHD              48 89 5C 24 08      48 89 5C 24 18
//   DrawVCIHeadset         48 89 5C 24 10      48 89 5C 24 08
//   DrawLabyrinthFishEye   48 89 5C 24 10      48 89 5C 24 08
//
// All are 5 complete position-independent bytes; DrawSkyHD's retail prologue is
// followed by push rbp/rsi/rdi, so its 5-byte steal still ends on a boundary.
const uint8_t kSaveRbx08[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
const uint8_t kSaveRbx10[] = { 0x48, 0x89, 0x5C, 0x24, 0x10 };
const uint8_t kSaveRbx18[] = { 0x48, 0x89, 0x5C, 0x24, 0x18 };

// Every RVA read out of the matching PDB, by name.
//
//   python tools\pdbdump.py <dir>\tomb4.dll draw_rooms w2v_matrix PrintRoomsList DrawSkyHD
//
// ONLY BUILDS WITH A PDB APPEAR HERE. The old table carried a second pair of
// rows for the 2025-09-10 build, derived by applying a uniform per-DLL shift to
// the addresses of the newer one and marked UNVERIFIED. That was a reasonable
// thing to do when the alternative was nothing; it is not reasonable now that
// the alternative is running pdbdump against that build's own PDB. An unknown
// build is reported and the culling stands down, which is the honest failure.
constexpr GameDllLayout kDlls[] = {
    { 0, 0x696B4999, L"tomb4.dll", "Tomb Raider IV",
      /* lara            */ 0x004F2E40,
      /* lara_item       */ 0x004F3000,
      /* camera          */ 0x00663E40,
      /* room            */ 0x00663FC8,
      /* number_rooms    */ 0x00660810,
      /* draw_rooms      */ 0x0063DC60,
      /* number_draw_..  */ 0x0063DDFC,
      /* w2v_matrix      */ 0x00494840,
      /* phd_mxptr       */ 0x001B6D10,
      /* phd_winxmax     */ 0x004947D0,
      /* phd_winymax     */ 0x004947CC,
      /* outside         */ 0x0063DC20,
      /* outside_left    */ 0x0084E170,
      /* outside_right   */ 0x0084E160,
      /* outside_top     */ 0x0084E168,
      /* outside_bottom  */ 0x0084E164,
      /* BinocularOn     */ 0x001BB244,
      /* BinocularRange  */ 0x001BB24C,
      /* PrintRoomsList  */ 0x000C5160,
      /* S_GetObjectB..  */ 0x000B8210,
      /* DrawSkyHD       */ 0x000C4CA0,
      /* DrawLaraHD      */ 0x000C40F0,
      /* phd_GenerateW2V */ 0x000D50B0,
      /* w2v scene return*/ 0x000B725E,
      /* frame_frac      */ 0x00494870,
      /* DrawCreatureHD  */ 0x000C1520,
      /* DrawHair        */ 0x000F06F0,
      /* gLaraHeads      */ 0x00532440,
      /* objects         */ 0x00699CC0,
      /* GetJointAbs...  */ 0x000A3A80,
      /* AimWeapon       */ 0x0005E400,
      /* LaraAboveWater  */ 0x0004BCF0,
      /* AnimateLara     */ 0x000624E0,
      /* analogInput     */ 0x00663EC0,
      /* input           */ 0x00663F00,
      /* GetCollisionInfo*/ 0x0000C3C0,
      /* GetFloor        */ 0x00012910,
      /* GetHeight       */ 0x00012DF0,
      /* ItemNewRoom     */ 0x00040D00,
      /* playing_cutseq  */ 0x001BB1A4,
      /* cutseq_num      */ 0x001BB19C,
      /* cutseq_trig     */ 0x001BB1D4,
      /* bUseSpotCam     */ 0x001BB1A8,
      /* bVoncroyCutScene*/ 0x006601C0,
      /* DrawNormalBin.. */ 0x000D1B00,
      /* DrawVCIHeadset  */ 0x000D17B0,
      /* DrawLabyrinth.. */ 0x000D1440,
      /* DrawNormalLas.. */ 0x000D1E40,
      /* DoInfraRedQuad  */ 0x000D21B0,
      kPrintRoomsListTR4, sizeof(kPrintRoomsListTR4),
      kSaveRbx08, kSaveRbx10, kSaveRbx10, /* anims */ 0x00699CA8 },

    { 1, 0x696B499C, L"tomb5.dll", "Tomb Raider V",
      /* lara            */ 0x004EE740,
      /* lara_item       */ 0x004EE900,
      /* camera          */ 0x0066D0E0,
      /* room            */ 0x0065EC28,
      /* number_rooms    */ 0x0065B2F0,
      /* draw_rooms      */ 0x0063D6C0,
      /* number_draw_..  */ 0x0063D860,
      /* w2v_matrix      */ 0x004EF940,
      /* phd_mxptr       */ 0x001B2AD0,
      /* phd_winxmax     */ 0x004EF8CC,
      /* phd_winymax     */ 0x004EF8C8,
      /* outside         */ 0x0063D6A8,
      /* outside_left    */ 0x00847A64,
      /* outside_right   */ 0x00847A40,
      /* outside_top     */ 0x00847A5C,
      /* outside_bottom  */ 0x00847A58,
      /* BinocularOn     */ 0x001B6C90,
      /* BinocularRange  */ 0x001B6C98,
      /* PrintRoomsList  */ 0x000B9CA0,
      /* S_GetObjectB..  */ 0x000ABB20,
      /* DrawSkyHD       */ 0x000B97F0,
      /* DrawLaraHD      */ 0x000B8C50,
      /* phd_GenerateW2V */ 0x000A1170,
      /* w2v scene return*/ 0x000AAB6E,
      /* frame_frac      */ 0x004EF988,
      /* DrawCreatureHD  */ 0x000B63C0,
      /* DrawHair        */ 0x000EE600,
      /* gLaraHeads      */ 0x0052FE00,
      /* objects         */ 0x006971C0,
      /* GetJointAbs...  */ 0x0009CC80,
      /* AimWeapon       */ 0x0005B6E0,
      /* LaraAboveWater  */ 0x0004A600,
      /* AnimateLara     */ 0x0005EFB0,
      /* analogInput     */ 0x0065ABA0,
      /* input           */ 0x0065AC40,
      /* GetCollisionInfo*/ 0x00009440,
      /* GetFloor        */ 0x000101F0,
      /* GetHeight       */ 0x000106D0,
      /* ItemNewRoom     */ 0x00045C00,
      /* playing_cutseq  */ 0x001B6BE4,
      /* cutseq_num      */ 0x001B6BDC,
      /* cutseq_trig     */ 0x001B6C14,
      /* bUseSpotCam     */ 0x001B6BC0,
      /* bVoncroyCutScene*/ 0,
      /* DrawNormalBin.. */ 0x000C4DD0,
      /* DrawVCIHeadset  */ 0x000C4A80,
      /* DrawLabyrinth.. */ 0x000C4710,
      /* DrawNormalLas.. */ 0x000C5110,
      /* DoInfraRedQuad  */ 0x000C5480,
      kPrintRoomsListTR5, sizeof(kPrintRoomsListTR5),
      kSaveRbx08, kSaveRbx10, kSaveRbx10, /* anims */ 0x0066D1B0 },

    // --- retail Steam builds (2025-09-10), no PDB ------------------------------
    //
    // Ported with tools\port_addresses.py from the rows above. Every data
    // address was agreed by all of its references in matched code (4 to 600
    // votes each, no dissent), and the neighbour relationships survive intact:
    // phd_winxmax/ymax 4 apart, lara_item 0x1C0 above lara, BinocularRange 8
    // above BinocularOn, the outside_* rect in the same order. Struct layouts
    // were checked the same way -- ROOM_INFO 304 bytes, lara +12, camera +4/+12,
    // ITEM_INFO +6176 all appear unchanged in hundreds of matched instructions
    // and never once remapped.
    //
    // DrawSkyHD, DrawVCIHeadset and DrawLabyrinthFishEye changed shape too much
    // to pair by body; each is placed by its unique call site (PrintRoomsList's
    // first call, and DrawBinoculars' two calls that bracket DrawNormalBinocs),
    // whose surrounding call sequences agree one-for-one between the builds.
    { 0, 0x68C12FDA, L"tomb4.dll", "Tomb Raider IV (retail)",
      /* lara            */ 0x004F3D80,
      /* lara_item       */ 0x004F3F40,
      /* camera          */ 0x00664D80,
      /* room            */ 0x00664F08,
      /* number_rooms    */ 0x00661750,
      /* draw_rooms      */ 0x0063EBA0,
      /* number_draw_..  */ 0x0063ED3C,
      /* w2v_matrix      */ 0x00495780,
      /* phd_mxptr       */ 0x001B7D10,
      /* phd_winxmax     */ 0x00495710,
      /* phd_winymax     */ 0x0049570C,
      /* outside         */ 0x0063EB60,
      /* outside_left    */ 0x0084F0B0,
      /* outside_right   */ 0x0084F0A0,
      /* outside_top     */ 0x0084F0A8,
      /* outside_bottom  */ 0x0084F0A4,
      /* BinocularOn     */ 0x001BC184,
      /* BinocularRange  */ 0x001BC18C,
      /* PrintRoomsList  */ 0x000C5DC0,
      /* S_GetObjectB..  */ 0x000B8C90,
      /* DrawSkyHD       */ 0x000C58D0,
      /* DrawLaraHD      */ 0x000C4D20,
      /* phd_GenerateW2V */ 0x000D5BD0,
      /* w2v scene return*/ 0x000B7CC0,
      /* frame_frac      */ 0x004957B0,
      /* DrawCreatureHD  */ 0x000C1F50,
      /* DrawHair        */ 0x000F10D0,
      /* gLaraHeads      */ 0x00533380,
      /* objects         */ 0x0069AC00,
      /* GetJointAbs...  */ 0x000A4620,
      /* AimWeapon       */ 0x0005E100,
      /* LaraAboveWater  */ 0x0004B9D0,
      /* AnimateLara     */ 0x00062200,
      /* analogInput     */ 0x00664E00,
      /* input           */ 0x00664E40,
      /* GetCollisionInfo*/ 0x0000BF10,
      /* GetFloor        */ 0x00012540,
      /* GetHeight       */ 0x00012A20,
      /* ItemNewRoom     */ 0x000407C0,
      /* playing_cutseq  */ 0x001BC0E4,
      /* cutseq_num      */ 0x001BC0DC,
      /* cutseq_trig     */ 0x001BC114,
      /* bUseSpotCam     */ 0x001BC0E8,
      /* bVoncroyCutScene*/ 0x00661100,
      /* DrawNormalBin.. */ 0x000D2630,
      /* DrawVCIHeadset  */ 0x000D22E0,
      /* DrawLabyrinth.. */ 0x000D1FA0,
      /* DrawNormalLas.. */ 0x000D2970,
      /* DoInfraRedQuad  */ 0x000D2CD0,
      kPrintRoomsListTR4, sizeof(kPrintRoomsListTR4),
      kSaveRbx18, kSaveRbx08, kSaveRbx08, /* anims */ 0x0069ABE8 },

    { 1, 0x68C12FE9, L"tomb5.dll", "Tomb Raider V (retail)",
      /* lara            */ 0x004EE680,
      /* lara_item       */ 0x004EE840,
      /* camera          */ 0x0066D020,
      /* room            */ 0x0065EB68,
      /* number_rooms    */ 0x0065B230,
      /* draw_rooms      */ 0x0063D600,
      /* number_draw_..  */ 0x0063D7A0,
      /* w2v_matrix      */ 0x004EF880,
      /* phd_mxptr       */ 0x001B2AD0,
      /* phd_winxmax     */ 0x004EF80C,
      /* phd_winymax     */ 0x004EF808,
      /* outside         */ 0x0063D5E8,
      /* outside_left    */ 0x008479A4,
      /* outside_right   */ 0x00847980,
      /* outside_top     */ 0x0084799C,
      /* outside_bottom  */ 0x00847998,
      /* BinocularOn     */ 0x001B6BD0,
      /* BinocularRange  */ 0x001B6BD8,
      /* PrintRoomsList  */ 0x000BA040,
      /* S_GetObjectB..  */ 0x000ABD20,
      /* DrawSkyHD       */ 0x000B9B50,
      /* DrawLaraHD      */ 0x000B8FB0,
      /* phd_GenerateW2V */ 0x000A14D0,
      /* w2v scene return*/ 0x000AAD50,
      /* frame_frac      */ 0x004EF8C8,
      /* DrawCreatureHD  */ 0x000B6510,
      /* DrawHair        */ 0x000EE760,
      /* gLaraHeads      */ 0x0052FD40,
      /* objects         */ 0x00697100,
      /* GetJointAbs...  */ 0x0009CFF0,
      /* AimWeapon       */ 0x0005C430,
      /* LaraAboveWater  */ 0x0004B2F0,
      /* AnimateLara     */ 0x0005F9B0,
      /* analogInput     */ 0x0065AAE0,
      /* input           */ 0x0065AB80,
      /* GetCollisionInfo*/ 0x000094A0,
      /* GetFloor        */ 0x000102F0,
      /* GetHeight       */ 0x000107D0,
      /* ItemNewRoom     */ 0x00046560,
      /* playing_cutseq  */ 0x001B6B24,
      /* cutseq_num      */ 0x001B6B1C,
      /* cutseq_trig     */ 0x001B6B54,
      /* bUseSpotCam     */ 0x001B6B00,
      /* bVoncroyCutScene*/ 0,
      /* DrawNormalBin.. */ 0x000C5040,
      /* DrawVCIHeadset  */ 0x000C4CF0,
      /* DrawLabyrinth.. */ 0x000C49B0,
      /* DrawNormalLas.. */ 0x000C5380,
      /* DoInfraRedQuad  */ 0x000C56E0,
      kPrintRoomsListTR5, sizeof(kPrintRoomsListTR5),
      kSaveRbx18, kSaveRbx08, kSaveRbx08, /* anims */ 0x0066D0F0 },
};

// TR6 is a different engine, so it cannot share GameDllLayout. Its embedded
// AMX native table names IsPointInWater, and mapGetPlayerPosition independently
// shows where the live player pointer is stored and that its position begins at
// +0x40. The water query itself reads gmapGMXCur. No private PDB is available
// for tomb6.dll, so every row is build-gated by PE timestamp.
//
// TR6's ceiling reader uses the rendered chase-camera matrix and the GMX room
// headers because TR6 has no TR4/5 w2v_matrix or ROOM_INFO layout. Using the
// matrix position for both the world offset and the room query is important:
// gcamCamera is a separate legacy camera that can temporarily describe another
// point during TR6's offscreen render chain.
//
// THE RETAIL ROW. Ported from the debug row with tools\port_addresses.py, then
// every entry was re-checked individually, because tomb6.dll changed more
// between the two builds than tomb4/tomb5 did:
//
//   * Globals: gcamCamera (41 references), player pointer (281), gmapGMXCur
//     (364) and the camera matrix (28) each agreed unanimously. fxCamDist's
//     RIP-relative read resolves to the new gcamCamera, and both bounds
//     helpers' cookie reads to the new __security_cookie.
//   * IsPointInWater has no unwind entry; it is the sole callee of the AMX
//     wrapper registered under the name "IsPointInWater", and its body reads the
//     new gmapGMXCur with the same +0x7A0 / +0x1A0 room offsets.
//   * Return addresses were paired call site by call site on the same callee,
//     by address order and surrounding-code similarity. The two animated-object
//     sites were confirmed in the decompiler by their distinctive field offsets
//     (+0x3C4/+0x3D0 dynamic, +0x3DC static).
//   * The final scene-object renderer was restructured: retail inlines the
//     mesh-part loop into the character path (new function +0x1B06A0), so it
//     has SIX AABB/OBB pairs to the debug build's five. The inlined copy is the
//     same test the debug build reached through its call, so it is allowlisted:
//
//       path                         debug AABB/OBB     retail AABB/OBB
//       mesh parts, joints < 4       1B0066 / 1B0081    1B0595 / 1B05AB
//       mesh parts, joints >= 4      1B0155 / 1B016B    1B0605 / 1B061B
//       characters                   1B0858 / 1B0877    1B0808 / 1B082D
//       mesh parts (inlined)            (via call)      1B08BD / 1B08D9
//       static objects               1B09B8 / 1B0A03    1B1062 / 1B10B1
//       main scene                   1B1BFC / 1B1C1D    1B2289 / 1B22AA
//
//   * TR6 struct offsets used here and in Hooks.cpp (+0x40, +0xA0, +0xB0,
//     +0xE0, +0x1A0, +0x218, +0x7A0, the 400-byte camera view) are unchanged in
//     every matched instruction that uses them.
constexpr Tr6Layout kTr6Builds[] = {
    { 0x696B49A4, "tomb6.dll v1.0.2a debug (2026-01-17)",
      /* playerPointer   */ 0x00CB42D8,
      /* currentGmx      */ 0x04CC7238,
      /* isPointInWater  */ 0x0014E180,
      /* cameraMatrix    */ 0x0029DCC0,
      /* renderScene     */ 0x001B1CA0,
      /* calculate       */ 0x001A6DB0,
      /* clippedObb      */ 0x001A4380,
      /* clippedAabb     */ 0x001A4610,
      /* drawProjShadows */ 0x001B8270,
      /* effectsUpdate   */ 0x001A28D0,
      /* fxCamDist       */ 0x00107890,
      /* fxBoundsClip    */ 0x0019F8E0,
      /* fxNodeBoundsClip*/ 0x0019FA30,
      /* gcamCamera      */ 0x002FD540,
      /* fxProcessBox    */ 0x00109020, 55064,
      /* fxLightBoundsRet*/ 0x00115FDB,
      /* fxNodeBoundsRet */ 0x0014E545,
      /* mainCalcRet     */ 0x001B1E9D,
      /* reflCalcRet     */ 0x001B0F20,
      /* mainRoomGrpObb  */ 0x001B1814,
      /* clipRoomObb     */ 0x001AF8AD,
      /* characterObb    */ 0x001A6058,
      /* animDynamicObb  */ 0x001A6368,
      /* animStaticObb   */ 0x001A66DD,
      /* waterObb        */ { 0x001A6BB3, 0x001A6C37, 0x001A6CFB },
      /* sceneObjectAabb */ { 0x001B0066, 0x001B0155, 0x001B0858, 0x001B09B8,
                              0x001B1BFC },
      /* sceneObjectObb  */ { 0x001B0081, 0x001B016B, 0x001B0877, 0x001B0A03,
                              0x001B1C1D },
      /* fxCamDist       */ { 0x48, 0x83, 0xEC, 0x28, 0xF3, 0x0F, 0x10, 0x0D,
                              0xA4, 0x5C, 0x1F, 0x00 },
      /* fxBoundsClip    */ { 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x05,
                              0x55, 0xE7, 0x0E, 0x00 },
      /* fxNodeBoundsClip*/ { 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x05,
                              0x05, 0xE6, 0x0E, 0x00 } },

    { 0x68C12FE6, "tomb6.dll retail (2025-09-10)",
      /* playerPointer   */ 0x00CB3238,
      /* currentGmx      */ 0x04CC6198,
      /* isPointInWater  */ 0x0014E580,
      /* cameraMatrix    */ 0x0029CCD0,
      /* renderScene     */ 0x001B2330,
      /* calculate       */ 0x001A7290,
      /* clippedObb      */ 0x001A47B0,
      /* clippedAabb     */ 0x001A4A40,
      /* drawProjShadows */ 0x001B8920,
      /* effectsUpdate   */ 0x001A2CC0,
      /* fxCamDist       */ 0x00107620,
      /* fxBoundsClip    */ 0x0019FCA0,
      /* fxNodeBoundsClip*/ 0x0019FDF0,
      /* gcamCamera      */ 0x002FC4A0,
      /* fxProcessBox    */ 0x00108DB0, 55112,
      /* fxLightBoundsRet*/ 0x00115D9B,
      /* fxNodeBoundsRet */ 0x0014E946,
      /* mainCalcRet     */ 0x001B253D,
      /* reflCalcRet     */ 0x001B15C0,
      /* mainRoomGrpObb  */ 0x001B1EB4,
      /* clipRoomObb     */ 0x001AFDCD,
      /* characterObb    */ 0x001A6518,
      /* animDynamicObb  */ 0x001A6819,
      /* animStaticObb   */ 0x001A6B7D,
      /* waterObb        */ { 0x001A7093, 0x001A7117, 0x001A71DB },
      /* sceneObjectAabb */ { 0x001B0595, 0x001B0605, 0x001B0808, 0x001B08BD,
                              0x001B1062, 0x001B2289 },
      /* sceneObjectObb  */ { 0x001B05AB, 0x001B061B, 0x001B082D, 0x001B08D9,
                              0x001B10B1, 0x001B22AA },
      /* fxCamDist       */ { 0x48, 0x83, 0xEC, 0x28, 0xF3, 0x0F, 0x10, 0x0D,
                              0x74, 0x4E, 0x1F, 0x00 },
      /* fxBoundsClip    */ { 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x05,
                              0x95, 0xD3, 0x0E, 0x00 },
      /* fxNodeBoundsClip*/ { 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x05,
                              0x45, 0xD2, 0x0E, 0x00 } },
};

constexpr bool Tr6ListHas(const uint32_t* list, uint32_t rva) {
    for (int i = 0; i < Tr6Layout::kMaxSceneObjectSites; ++i)
        if (list[i] != 0 && list[i] == rva) return true;
    return false;
}

// Nearby ClippedOBB_CPP calls belong to other scene views or to map/gameplay
// work and must stay stock. One from each side of the renderer, per build.
static_assert(!Tr6ListHas(kTr6Builds[0].sceneObjectObbReturn, 0x001B2732)
           && !Tr6ListHas(kTr6Builds[0].sceneObjectObbReturn, 0x0013F5A6)
           && !Tr6ListHas(kTr6Builds[1].sceneObjectObbReturn, 0x001B2DBD)
           && !Tr6ListHas(kTr6Builds[1].sceneObjectObbReturn, 0x0013FAA7),
              "TR6 final-object allowlist includes a non-render call site");

// The inlined retail pair is the one the debug build does not have.
static_assert(Tr6ListHas(kTr6Builds[1].sceneObjectAabbReturn, 0x001B08BD)
           && Tr6ListHas(kTr6Builds[1].sceneObjectObbReturn,  0x001B08D9)
           && !Tr6ListHas(kTr6Builds[0].sceneObjectObbReturn, 0x001B08D9),
              "TR6 final-object allowlist changed");

constexpr uint32_t kTr6PlayerPositionOff  = 0x00000040;

constexpr uint32_t kTr6GmxRoomsOff        = 0x000001A0;
constexpr uint32_t kTr6GmxRoomCountOff    = 0x000007A0;
constexpr uint32_t kTr6RoomBoundsMinOff   = 0x000000A0;
constexpr uint32_t kTr6RoomBoundsMaxOff   = 0x000000B0;
constexpr uint32_t kTr6RoomIsFlipOff      = 0x00000218;
constexpr int32_t  kTr6MaxRooms           = 192;

using FnTr6IsPointInWater = int(__fastcall*)(const float* position,
                                             float* waterHeight);

const GameDllLayout* g_dll  = nullptr;
uint64_t             g_base = 0;
bool                 g_loggedNoRooms = false;
uint32_t             g_warnedStamp[2] = { 0, 0 };
bool                 g_loggedTr6WaterBinding = false;
bool                 g_warnedTr6WaterBuild = false;
bool                 g_loggedTr6CameraFrame = false;

template <typename T>
T Read(uint32_t rva) {
    return *reinterpret_cast<T*>(g_base + rva);
}

uint32_t ModuleStamp(uint64_t base) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

int Tr6LaraWaterStatus() {
    HMODULE module = GetModuleHandleW(L"tomb6.dll");
    if (!module) return -1;

    const uint64_t base = reinterpret_cast<uint64_t>(module);
    const uint32_t stamp = ModuleStamp(base);
    const Tr6Layout* tr6 = Tr6LayoutFor(stamp);
    if (!tr6) {
        if (!g_warnedTr6WaterBuild) {
            g_warnedTr6WaterBuild = true;
            LogF("tr6 water: tomb6.dll build 0x%08X is unsupported; automatic "
                 "stick-pitch restore stands down. Known builds:", stamp);
            Tr6LogKnownBuilds("tr6 water:");
        }
        return -1;
    }

    // IsPointInWater assumes a level exists and immediately dereferences
    // gmapGMXCur. Its AMX wrapper normally guarantees that precondition; the
    // XInput hook can run at the title screen too, so enforce it here. A null
    // player is equally normal while a level is loading.
    const auto* gmx = *reinterpret_cast<const void* const*>(
        base + tr6->currentGmx);
    const auto* player = *reinterpret_cast<const uint8_t* const*>(
        base + tr6->playerPointer);
    if (!gmx || !player) return -1;

    // Copy the position before entering game code. The query only reads XYZ;
    // the fourth float in the player's position vector is not part of its API.
    const float* livePosition = reinterpret_cast<const float*>(
        player + kTr6PlayerPositionOff);
    const float position[3] = {
        livePosition[0], livePosition[1], livePosition[2]
    };
    float waterHeight = 0.0f;
    const auto isPointInWater = reinterpret_cast<FnTr6IsPointInWater>(
        base + tr6->isPointInWater);

    if (!g_loggedTr6WaterBinding) {
        g_loggedTr6WaterBinding = true;
        LogF("tr6 water: bound Lara position and IsPointInWater "
             "(build 0x%08X, player +0x%X, query +0x%X)",
             stamp, tr6->playerPointer, tr6->isPointInWater);
    }

    // TR4/TR5 expose a five-value water_status enum. TR6 exposes a geometric
    // predicate instead, so normalise it to the only distinction the input
    // policy needs: 0 = dry, 1 = in water.
    return isPointInWater(position, &waterHeight) ? 1 : 0;
}

bool Tr6CameraViewFrame(float rot[3][3], float pos[3]);

bool Tr6CameraHeadroom(float& units) {
    HMODULE module = GetModuleHandleW(L"tomb6.dll");
    if (!module) return false;

    const uint64_t base = reinterpret_cast<uint64_t>(module);
    const Tr6Layout* tr6 = Tr6LayoutFor(ModuleStamp(base));
    if (!tr6) return false;

    auto** currentGmx = reinterpret_cast<uint8_t**>(
        base + tr6->currentGmx);
    if (!currentGmx || !*currentGmx) return false;

    const int32_t roomCount = *reinterpret_cast<int32_t*>(
        *currentGmx + kTr6GmxRoomCountOff);
    if (roomCount <= 0 || roomCount > kTr6MaxRooms) return false;

    auto** rooms = reinterpret_cast<uint8_t**>(
        *currentGmx + kTr6GmxRoomsOff);

    // WorldLockOffset has just read this same frame. Read it again instead of
    // mixing its position with gcamCamera: even a small disagreement makes the
    // headroom refer to a different room than the eye displacement.
    float cameraRot[3][3]{};
    float camera[3]{};
    if (!Tr6CameraViewFrame(cameraRot, camera)) return false;

    float availableHeadroom = 0.0f;
    for (int32_t room = 0; room < roomCount; ++room) {
        const uint8_t* header = rooms[room];
        if (!header || *reinterpret_cast<const int32_t*>(
                           header + kTr6RoomIsFlipOff) != 0) {
            continue;
        }

        const float* boundsMin = reinterpret_cast<const float*>(
            header + kTr6RoomBoundsMinOff);
        const float* boundsMax = reinterpret_cast<const float*>(
            header + kTr6RoomBoundsMaxOff);
        if (camera[0] < boundsMin[0] || camera[0] > boundsMax[0]
            || camera[1] < boundsMin[1] || camera[1] > boundsMax[1]
            || camera[2] < boundsMin[2] || camera[2] > boundsMax[2]) {
            continue;
        }

        // TR coordinates are Y-down, so BoundsMin.y is the room ceiling.
        const float headroom = camera[1] - boundsMin[1];
        if (headroom > availableHeadroom && headroom <= 32768.0f) {
            availableHeadroom = headroom;
        }
    }

    if (!(availableHeadroom > 0.0f)) return false;
    units = availableHeadroom;
    return true;
}

bool Tr6CameraViewFrame(float rot[3][3], float pos[3]) {
    HMODULE module = GetModuleHandleW(L"tomb6.dll");
    if (!module) return false;

    const uint64_t base = reinterpret_cast<uint64_t>(module);
    const Tr6Layout* tr6 = Tr6LayoutFor(ModuleStamp(base));
    if (!tr6) return false;

    // TR6's camera is a conventional column-major world-to-view matrix. The
    // game renders down -Z, while WorldLockOffset and the TR4/TR5 reader use
    // phd's +Z-forward camera frame. Negating row 2 converts only that frame
    // convention; the camera position is recovered from the original rigid
    // view as -R^T*t before the conversion.
    const float* view = reinterpret_cast<const float*>(
        base + tr6->cameraMatrix);
    const float tx = view[12];
    const float ty = view[13];
    const float tz = view[14];
    pos[0] = -(view[0] * tx + view[1] * ty + view[2]  * tz);
    pos[1] = -(view[4] * tx + view[5] * ty + view[6]  * tz);
    pos[2] = -(view[8] * tx + view[9] * ty + view[10] * tz);

    for (int row = 0; row < 3; ++row) {
        const float sign = (row == 2) ? -1.0f : 1.0f;
        for (int col = 0; col < 3; ++col) {
            rot[row][col] = sign * view[col * 4 + row];
        }
    }

    // The global is zero before the first gameplay camera update. Check all
    // three rows because a partially written or non-rigid matrix would turn
    // head translation into an arbitrary world displacement.
    for (int row = 0; row < 3; ++row) {
        const float len2 = rot[row][0] * rot[row][0]
                         + rot[row][1] * rot[row][1]
                         + rot[row][2] * rot[row][2];
        if (!(len2 > 0.9f && len2 < 1.1f)) return false;
    }
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1])
        || !std::isfinite(pos[2])) {
        return false;
    }

    if (!g_loggedTr6CameraFrame) {
        g_loggedTr6CameraFrame = true;
        LogF("tr6 camera: world frame bound at tomb6.dll+0x%X; "
             "world-space head offset and ceiling clamp active",
             tr6->cameraMatrix);
    }
    return true;
}

} // namespace

const Tr6Layout* Tr6LayoutFor(uint32_t timestamp) {
    for (const Tr6Layout& b : kTr6Builds)
        if (b.timestamp == timestamp) return &b;
    return nullptr;
}

void Tr6LogKnownBuilds(const char* prefix) {
    for (const Tr6Layout& b : kTr6Builds)
        LogF("%s   0x%08X  %s", prefix, b.timestamp, b.name);
}

bool GameDllUpdate() {
    // WHICH DLL, AND WHY NOT "THE ONE THAT IS LOADED"
    //
    // Both game DLLs are resident for the whole session, so GetModuleHandleW
    // succeeds for both at all times and "the first one that resolves" always
    // means tomb4.dll. CurrentGame() is the selector the engine itself uses:
    // 0 = TR4, 1 = TR5, 2 = TR6.
    //
    // TR6 has no row. It is a different engine -- different room structures,
    // different renderer -- and its culling is not addressed by any of this.
    const int game = CurrentGame();
    if (game != 0 && game != 1) {
        g_dll  = nullptr;
        g_base = 0;
        return false;
    }

    // Already bound to the right one, and it has not moved.
    if (g_dll && g_dll->game == game &&
        GetModuleHandleW(g_dll->module) == reinterpret_cast<HMODULE>(g_base))
        return true;

    g_dll  = nullptr;
    g_base = 0;

    const wchar_t* module = (game == 0) ? L"tomb4.dll" : L"tomb5.dll";
    HMODULE h = GetModuleHandleW(module);
    if (!h) return false;

    const uint64_t base  = reinterpret_cast<uint64_t>(h);
    const uint32_t stamp = ModuleStamp(base);

    for (const GameDllLayout& d : kDlls) {
        if (d.game != game || d.timestamp != stamp) continue;
        g_dll  = &d;
        g_base = base;
        // Logged on every rebind, not once: the player can switch games without
        // restarting, and which DLL we are reading is exactly the thing that
        // goes wrong silently. The game index is printed alongside so the two
        // can be checked against each other at a glance.
        LogF("gamedll: bound to %S (%s, build 0x%08X, game=%d) at %p",
             d.module, d.name, stamp, game, (void*)base);
        return true;
    }

    // Unknown build. Report it once per stamp, name what IS known, and stand
    // down -- every address below would otherwise be a guess, and one of them
    // is a hook target.
    if (g_warnedStamp[game] != stamp) {
        g_warnedStamp[game] = stamp;
        LogF("gamedll: %S is build 0x%08X, which has no address table. Room "
             "culling, the ceiling clamp and the swimming exception are all "
             "inactive for it. Known builds:", module, stamp);
        for (const GameDllLayout& d : kDlls)
            if (d.game == game) LogF("gamedll:   0x%08X  (%s)", d.timestamp, d.name);
        Log("gamedll: to add one, run tools\\pdbdump.py against that build's PDB, "
            "or tools\\port_addresses.py from a known build if it has none.");
    }
    return false;
}

const GameDllLayout* GameDllBound() { return g_dll; }
uint64_t             GameDllBase()  { return g_base; }

int LaraWaterStatus() {
    if (CurrentGame() == 2) return Tr6LaraWaterStatus();
    if (!g_dll) return -1;
    return Read<int16_t>(g_dll->lara + off::lara_water_status);
}

bool IsOpticsZoomed() {
    if (!g_dll) return false;

    // The exact two fields ProcessLooking itself checks before deciding whether
    // the right stick drives ordinary look-around or the zoom camera --
    // decompiled and confirmed, not guessed. BinocularRange is a ramp counter
    // nonzero for the whole entering/leaving transition; BinocularOn goes
    // negative on the way out. Either being nonzero means the zoom camera, not
    // the normal one, currently owns the stick.
    return Read<int32_t>(g_dll->binocularOn)    != 0
        || Read<int32_t>(g_dll->binocularRange) != 0;
}

bool CameraHeadroom(float& units) {
    units = 0.0f;
    if (CurrentGame() == 2) return Tr6CameraHeadroom(units);
    if (!g_dll) return false;

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

    const int16_t roomNo = Read<int16_t>(g_dll->camera + off::camera_pos_room);
    if (roomNo < 0 || roomNo >= numRooms) return false;

    const int32_t camY = Read<int32_t>(g_dll->camera + off::camera_pos_y);
    const int32_t ceil =
        *reinterpret_cast<int32_t*>(rooms + static_cast<uint32_t>(roomNo) * off::room_stride
                                          + off::room_maxceiling);

    // TR world space is Y-DOWN: the ceiling has a SMALLER Y than anything below
    // it, so headroom is (camera Y - ceiling Y) and is positive when the camera
    // is below the ceiling. Getting this backwards would produce a negative
    // number that the check below rejects, which is the intended failure.
    const int32_t headroom = camY - ceil;

    // Sanity. A sector is 1024 units and Lara is about 762 tall, so a real
    // headroom is a few hundred to a few thousand units. Anything outside that
    // means we are reading a room the camera is not really in -- a cutscene
    // camera, a level transition, a stale index -- so report "unknown" rather
    // than clamp the player's head on a bad number.
    if (headroom <= 0 || headroom > 32768) return false;

    units = static_cast<float>(headroom);
    return true;
}

bool CameraViewFrame(float rot[3][3], float pos[3]) {
    if (CurrentGame() == 2) return Tr6CameraViewFrame(rot, pos);
    if (!g_dll) return false;

    const int32_t* m = reinterpret_cast<const int32_t*>(g_base + g_dll->w2vMatrix);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rot[i][j] = static_cast<float>(m[i * 4 + j]) * (1.0f / 16384.0f);
        }
        pos[i] = static_cast<float>(m[i * 4 + 3]);
    }

    // Before the first camera update the matrix is zeroed, and a zero rotation
    // would send every offset to the camera. One row of unit length tells "ready"
    // from "not yet"; the tolerance only has to cover 1/16384 quantisation.
    const float len2 = rot[0][0] * rot[0][0] + rot[0][1] * rot[0][1]
                     + rot[0][2] * rot[0][2];
    return len2 > 0.9f && len2 < 1.1f;
}

void GameDllShutdown() {
    g_dll  = nullptr;
    g_base = 0;
    g_loggedTr6WaterBinding = false;
    g_warnedTr6WaterBuild = false;
    g_loggedTr6CameraFrame = false;
}

} // namespace tr
