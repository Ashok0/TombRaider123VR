// Sky.h -- draw the remaster's HD sky at optical infinity.
//
// DrawSkyHD in tomb4/5.dll pushes a copy of the current matrix onto the
// matrix stack with its translation zeroed (confirmed in the disassembly:
// the pushed copy's three translation floats are overwritten with 0 right
// after the copy is made) and draws hd_sky through it. That keeps the dome
// centred on the game camera -- "at infinity" on a monitor, because there is
// no parallax from walking.
//
// In VR the stereo path then composes the eye transform E, whose translation
// is IPD plus any 6DOF head offset, on top of that. The dome's vertices still
// sit at a finite mesh radius, so E's translation gives them stereo disparity
// equal to a few metres -- a painted sphere sitting in front of distant
// geometry, and the engine's own depth range lets its fragments win a depth
// test against the world behind it.
//
// The hook wraps DrawSkyHD and SkyPassActive() is true for every draw it
// issues. Hooks.cpp then uses the rotation of E only, and pushes those
// fragments to the far plane, so the sky fuses at infinity and never
// occludes the world.
//
// TR6 (Angel of Darkness) has no row here: different engine, no PDB, and its
// scene is rendered offscreen and composited whole (see AlternateEyeActive
// in Hooks.cpp) rather than duplicated per draw, so this per-draw mechanism
// does not apply to it.
#pragma once

namespace tr {

// Install or drop the DrawSkyHD hook to match the DLL currently bound.
// Cheap and idempotent; call once per frame, after GameDllUpdate().
void SkyUpdate();

// Remove the hook. Called from RemoveHooks.
void SkyShutdown();

// True while the original DrawSkyHD is on the stack.
bool SkyPassActive();

} // namespace tr
