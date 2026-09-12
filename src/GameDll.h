// GameDll.h -- the two pieces of game state that live in tomb1/2/3.dll rather
// than in tomb123.exe.
//
// This replaces the TR4-6 mod's RoomCull.h. That file did two jobs: it defeated
// the engine's portal culling, and it exposed Lara's water status and the
// camera's headroom. Only the second job is carried over here; see Config.h for
// why the culling is not.
//
// WHY THIS IS EASY HERE AND WAS HARD THERE
//
// The TR4-6 game DLLs shipped without symbols, so the equivalent addresses were
// found by tracing call sites into the engine and reading a decompiler. TR1-3's
// DLLs ship full private PDBs, so every address and every struct offset below
// came out of dbghelp:
//
//   python tools\pdbdump.py  PDB\tomb1.dll
//   python tools\typedump.py PDB\tomb1.dll lara_info camera_info ROOM_INFO
//
// All three DLLs were checked and their STRUCT LAYOUTS ARE IDENTICAL -- 432-byte
// lara_info with water_status at +12, 128-byte camera_info, 168-byte ROOM_INFO
// with y/minfloor/maxceiling at +44/+52/+56. Only the addresses of the globals
// differ between the three, which is why there is one struct description and a
// three-row address table.
#pragma once

namespace tr {

// Resolve whichever of tomb1/2/3.dll is loaded. Cheap and idempotent; call once
// per frame. Returns true once a supported DLL has been bound.
bool GameDllUpdate();

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
