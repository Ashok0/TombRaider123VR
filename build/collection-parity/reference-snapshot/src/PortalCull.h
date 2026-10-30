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
// WHAT THIS REPLACES
//
// RoomCull.cpp expanded the draw list by PORTAL HOPS: take every room already
// in the list, add everything one doorway away, repeat N times, optionally
// testing the connecting portal against the head's frustum first. It worked,
// and three things followed from doing it that way:
//
//   * N is not a visibility criterion. Too small and rooms are still missing;
//     too large and the level is drawn. It had to be tuned, and the right value
//     differed per level.
//   * The frustum never NARROWED. A portal that is on screen but that you
//     cannot actually see through still passed, and every room beyond it was
//     added unclipped. That is the room-215 class of bug, and it is why
//     DrawAllRoomsExclude existed.
//   * It added rooms the traversal had never reached, flip-map STORAGE rooms
//     included -- the inactive half of a flooded/drained pair, sitting at the
//     same world position as its live twin -- so it needed a flipped_room check
//     to suppress geometry drawn over geometry.
//
// WHAT THIS DOES INSTEAD
//
// It runs the engine's own portal traversal a second time, from the head, in
// world space, with a real frustum.
//
//   * The apex is the tracked head, not the game camera.
//   * The frustum is the headset's, widened to a symmetric superset that
//     contains both eyes.
//   * At every doorway the frustum is CLIPPED to the portal opening -- the
//     portal quad is cut against the incoming planes and a new plane is built
//     from the head through each surviving edge -- so a room enters the list
//     only if it can really be seen through that chain of doorways.
//   * The engine's own back-face test is kept, with the head substituted for
//     the camera.
//
// That gives three properties the hop expansion could not have:
//
//   1. No hop count. Depth is decided by the geometry: the frustum shrinks at
//      every doorway and traversal stops when it closes.
//   2. No flip-room problem and no exclude list. The traversal only crosses
//      portals belonging to rooms it has reached, and no live room's portals
//      name a storage room, so a storage room is unreachable by construction --
//      the same reason the engine's own traversal never draws one.
//   3. Nothing is ever removed. The engine's list is built first and we only
//      append, so with the head aligned to the game camera the result is what
//      the engine would have drawn, plus the margin the headset adds.
//
// It also carries the fix through the two places behind the room list where the
// same camera-shaped assumption is baked in:
//
//   * ROOM_INFO's clip rect at +76..+82, which PrintRooms turns into a scissor.
//     Widened to the full target for every room in the final list -- the same
//     thing DrawAllRoomsClip did, and for the same reason.
//   * S_GetObjectBounds, which rejects an item whose bounding box misses the
//     game camera's screen rect or sits behind its near plane. RoomCull did
//     nothing about this, so rooms it forced in drew with their furniture,
//     enemies and pickups missing.
//
// WHERE IT HOOKS
//
// PrintRoomsList in the game DLL -- the same function RoomCull hooked as
// "DrawRoomList", now reached by name out of the PDB rather than by signature.
// It is the last moment before the draw list is consumed, which is what makes
// appending safe: at the producer there is a second traversal pass still to run
// over the enlarged list, and that pass overran it.
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
