// PortalCull.h -- make the engine's visible set follow the HEAD instead of the
// game camera.
//
// THE PROBLEM
//
// The engine decides what to draw by walking portals out from the camera's
// room, carrying a screen-space rectangle that is clipped at every doorway. A
// room is drawn only if some chain of doorways projects onto the camera's
// screen. That is exactly right for a flat monitor and exactly wrong for a
// headset: the head sees wider than the game camera, and it can look somewhere
// the game camera is not pointing at all. Turn far enough and the geometry that
// should be there was never submitted.
//
// WHAT THIS DOES, AND HOW IT DIFFERS FROM THE TR4-6 MOD
//
// The TR4-6 version of this file (RoomCull.cpp) expanded the draw list by
// PORTAL HOPS: take every room already in the list, add everything one doorway
// away, repeat N times. It worked, but N is not a visibility criterion --
// too small and rooms are still missing, too large and the whole level is
// drawn -- and because it added rooms the traversal had not reached it also
// added flip-map STORAGE rooms, which are real entries at the same world
// position as their live twin. That produced geometry drawn over geometry, and
// needed a hand-maintained exclusion list to suppress.
//
// This does the actual thing instead: it runs the engine's own portal
// traversal, from the head, in world space, with a real frustum.
//
//   * The apex is the tracked head, not the game camera.
//   * The frustum is the headset's, widened to a symmetric superset that
//     contains both eyes.
//   * At every doorway the frustum is CLIPPED to the portal opening -- the
//     portal quad is cut against the incoming planes and a new plane is built
//     from the head through each surviving edge -- so a room enters the list
//     only if it can really be seen through that chain of doorways.
//
// That gives three properties the hop expansion could not have:
//
//   1. No hop count. Depth is decided by the geometry: the frustum shrinks at
//      every doorway and traversal stops when it closes. A corridor you can see
//      down for six rooms adds six; a wall next to you adds none.
//   2. No flip-room problem. The traversal only ever crosses portals belonging
//      to rooms it has reached, and no live room's portals name a storage room,
//      so a storage room is unreachable by construction -- the same reason the
//      engine's own traversal never draws one. There is no exclusion list here
//      because there is nothing to exclude.
//   3. Nothing is ever removed. The engine's list is built first and we only
//      append, so with the head aligned to the game camera the result is what
//      the engine would have drawn, plus the margin the headset adds.
//
// It also carries the fix through the two places behind the room list where the
// same camera-shaped assumption is baked in:
//
//   * ROOM_INFO's left/right/top/bottom clip rect, which PrintRooms turns into
//     a scissor. Widened to the full target for every room in the final list.
//   * S_GetObjectBounds, which rejects an item whose bounding box misses the
//     game camera's screen rect or sits behind its near plane. Without this,
//     rooms behind you would draw with all their furniture, enemies and pickups
//     missing.
//
// WHERE IT HOOKS, AND WHY THERE
//
// PrintRoomsList in the game DLL: the last moment before the draw list is
// consumed, reached from both DrawRooms and DrawRoomsAroundHere, and the same
// 406-byte function with the same 5-byte position-independent prologue in all
// three DLLs. GetVisibleRooms would have been the more obvious point but its
// prologue opens with a RIP-relative load in TR2 and TR3 and with a clean
// one in TR1, so hooking it would have meant three different stolen-byte
// windows and two displacement fixups to buy nothing.
#pragma once

namespace tr {

// Install or drop the game-DLL hooks to match the DLL that is currently bound.
// Cheap and idempotent; call once per frame, after GameDllUpdate().
void PortalCullUpdate();

// Remove the hooks. Called from RemoveHooks.
void PortalCullShutdown();

// Hotkey poll for CullDumpKey. Call once per frame.
void PortalCullPollKey();

// Counters for the periodic health report, since the last call.
//
//   rooms      rooms the engine's own traversal found
//   added      rooms this file appended on top of them
//   items      items S_GetObjectBounds rejected and the head test rescued
//   frames     frames sampled, so the caller can print per-frame averages
//   truncated  frames that hit a budget (draw list full, portal budget spent)
struct PortalCullStats {
    unsigned rooms     = 0;
    unsigned added     = 0;
    unsigned items     = 0;
    unsigned frames    = 0;
    unsigned truncated = 0;
};
PortalCullStats PortalCullTakeStats();

} // namespace tr
