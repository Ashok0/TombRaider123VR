// GameDll.h -- the game state that lives in tomb1/2/3.dll rather than in
// tomb123.exe, and the address table that reaches it.
//
// This file owns the BINDING (which of the three DLLs is live, where it is
// loaded, and every RVA inside it). Two small readers live here as well --
// Lara's water status and the camera's headroom -- because they are two field
// reads and nothing more. The room culling that uses the rest of the table is
// in PortalCull.cpp.
//
// WHY THIS IS EASY HERE AND WAS HARD IN TR4-6
//
// The TR4-6 game DLLs shipped without symbols, so the equivalent addresses were
// found by tracing call sites into the engine and reading a decompiler -- and
// the culling work there ended up against `FUN_18002ea30` and `DAT_18063dc60`.
// TR1-3's DLLs ship full private PDBs, so every address and every struct offset
// below came out of dbghelp, by name:
//
//   python tools\pdbdump.py  PDB\tomb1.dll  GetVisibleRooms draw_rooms
//   python tools\typedump.py PDB\tomb1.dll  lara_info camera_info ROOM_INFO
//   python tools\verify_addresses.py        re-derives and diffs all of them
//
// All three DLLs were checked and their STRUCT LAYOUTS ARE IDENTICAL -- 432-byte
// lara_info with water_status at +12, 128-byte camera_info, 168-byte ROOM_INFO
// with y/minfloor/maxceiling at +44/+52/+56 and door/left/right/top/bottom at
// +8/+80/+82/+84/+86. Only the addresses of the globals differ between the
// three, which is why there is one struct description and a three-row address
// table.
#pragma once

#include <cstdint>

namespace tr {

// One row per game DLL. Every RVA came out of that DLL's own PDB by name; a
// zero means "this build does not have that symbol", which is a real case --
// TR1 has no `outside` machinery at all, TR2 and TR3 do.
struct GameDllLayout {
    const wchar_t* module;      // file name as the loader knows it
    const char*    name;
    uint32_t       timestamp;   // PE TimeDateStamp

    // --- state read by LaraWaterStatus / CameraHeadroom ---------------------
    uint32_t lara;              // lara_info
    uint32_t camera;            // camera_info
    uint32_t room;              // ROOM_INFO*  (null until a level is loaded)
    uint32_t numberRooms;       // int16

    // --- the visible-set state the culling works on ------------------------
    uint32_t drawRooms;         // int16[200]  -- the draw list
    uint32_t numberDrawRooms;   // int32       -- how many of it are in use
    uint32_t w2vMatrix;         // int32[12]   -- world -> view, see PortalCull
    uint32_t phdMxptr;          // int32**     -- top of the matrix stack
    uint32_t phdWinXmax;        // int32       -- screen width  - 1
    uint32_t phdWinYmax;        // int32       -- screen height - 1

    // --- the sky/horizon clip rect. TR2 and TR3 only; all zero on TR1. ------
    uint32_t outside;           // int32, non-zero when an outdoor room is visible
    uint32_t outsideLeft;
    uint32_t outsideRight;
    uint32_t outsideTop;
    uint32_t outsideBottom;

    // --- hook targets ------------------------------------------------------
    uint32_t printRoomsList;    // void PrintRoomsList(void)
    uint32_t sGetObjectBounds;  // int  S_GetObjectBounds(int16* bounds)
};

// Resolve whichever of tomb1/2/3.dll is loaded. Cheap and idempotent; call once
// per frame. Returns true once a supported DLL has been bound.
bool GameDllUpdate();

// The row for the DLL currently bound, or null when nothing is bound. The
// pointer is stable for the life of the process; the BINDING is not, so callers
// that cache anything derived from it must re-check this every frame.
const GameDllLayout* GameDllBound();

// Load address of that DLL, or 0.
uint64_t GameDllBase();

// Lara's own water state, or -1 when no game DLL is bound.
//
//   0 ABOVE_WATER   1 UNDERWATER   2 SURFACE   3 FLYCHEAT   4 WADE
//
// Read straight from the DLL's `lara` global, so it is Lara's state rather than
// the camera's -- which is the whole point. The camera trails behind and above
// her and sits in the air room during a surface swim, so anything derived from
// the camera's room reports dry exactly when it matters most.
int LaraWaterStatus();

// World units from the game camera up to its room's ceiling.
//
// Returns false when it cannot be known -- no DLL bound, no level loaded, an
// out-of-range room index, or a result that fails its own sanity check. Callers
// MUST treat false as "unknown" and never as "no headroom": clamping the head to
// the floor because a menu was open would be worse than not clamping at all.
//
// Derived from the room's bounding box (ROOM_INFO::maxceiling) rather than from
// the floor data under the camera, so it is exact in a uniformly low room --
// tunnels, crawlspaces, the places the clamp exists for -- and over-generous in
// a room with one tall section. Over-generous is the safe direction: it clamps
// less than it could, never more.
bool CameraHeadroom(float& units);

// Drop the binding. Called from RemoveHooks.
void GameDllShutdown();

} // namespace tr
