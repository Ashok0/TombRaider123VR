// Sky.h -- draw the remaster's HD sky at optical infinity.
//
// DrawSkyHD in tomb1/2/3.dll already zeros the camera translation of the
// matrix stack so the sky dome stays centred on the game camera. That is
// "at infinity" on a monitor. In VR the stereo path then composes the eye
// transform E, whose translation is IPD plus any 6DOF head offset, and the
// dome gets stereo disparity equal to its mesh radius -- a painted sphere
// a few metres away, sitting in front of distant geometry.
//
// The hook wraps DrawSkyHD and SkyPassActive() is true for every draw it
// issues. Hooks.cpp then uses the rotation of E only, and pushes those
// fragments to the far plane, so the sky fuses at infinity and never
// occludes the world.
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
