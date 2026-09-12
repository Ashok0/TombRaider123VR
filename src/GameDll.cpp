#include "GameDll.h"
#include "Engine.h"
#include "Log.h"

#include <windows.h>
#include <cstdint>
#include <cmath>

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
// Column order matches GameDllLayout in the header. The five `outside*` entries
// are zero for TR1 because TR1's renderer has no such state: TR2 and TR3 track
// a separate screen rect for the sky, TR1 does not.
constexpr GameDllLayout kDlls[] = {
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
      /* DrawSkyHD       */ 0x0006E480 },

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
      /* DrawSkyHD       */ 0x000A0760 },

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
      /* DrawSkyHD       */ 0x000EAA80 },
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
    if (game < 0 || game >= static_cast<int>(sizeof(kDlls) / sizeof(kDlls[0]))) {
        // Before a game is chosen, or mid-transition. Report unbound rather than
        // guess: both callers treat that as "unknown" and fail safe.
        g_dll  = nullptr;
        g_base = 0;
        return false;
    }

    // Already bound to the right one, and it has not moved.
    if (g_dll == &kDlls[game] &&
        GetModuleHandleW(g_dll->module) == reinterpret_cast<HMODULE>(g_base))
        return true;

    g_dll  = nullptr;
    g_base = 0;

    const GameDllLayout& d = kDlls[game];
    HMODULE h = GetModuleHandleW(d.module);
    if (!h) return false;

    const uint64_t base = reinterpret_cast<uint64_t>(h);

    // Confirm the build before trusting any address in it. A mismatched stamp is
    // reported once and then accepted: unlike the exe, nothing here is WRITTEN --
    // both reads are into a struct whose layout is identical across all three
    // DLLs -- so the downside of a stale address is a bad number that the sanity
    // checks in CameraHeadroom reject, not a corrupted process.
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    if (stamp != d.timestamp && !g_loggedStamp) {
        g_loggedStamp = true;
        LogF("gamedll: %S has PE timestamp 0x%08X, expected 0x%08X. Ceiling "
             "clamp and water-state detection may be reading the wrong "
             "globals; both fail safe if so.", d.module, stamp, d.timestamp);
    }

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

void GameDllShutdown() {
    g_dll  = nullptr;
    g_base = 0;
}

} // namespace tr
