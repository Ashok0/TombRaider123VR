// Sky.h -- draw Classic and Remastered skies at optical infinity.
//
// DrawSkyHD in tomb1/2/3.dll already zeros the camera translation of the
// matrix stack so the sky dome stays centred on the game camera. That is
// "at infinity" on a monitor. In VR the stereo path then composes the eye
// transform E, whose translation is IPD plus any 6DOF head offset, and the
// dome gets stereo disparity equal to its mesh radius -- a painted sphere
// a few metres away, sitting in front of distant geometry.
//
// DrawSkyHD is scoped directly. TR2/3 Classic horizons use the verified
// DrawRooms -> ins_mesh call site, with command flushes bracketing that mesh.
// TR1 has no separate Classic horizon. SkyPassActive() stays true through
// actual sky submission, including deferred commands. Hooks.cpp then uses the rotation of E only, and pushes those
// fragments to the far plane, so the sky fuses at infinity and never
// occludes the world.
#pragma once

namespace tr {

// Install or drop the sky hooks to match the DLL currently bound.
// Cheap and idempotent; call once per frame, after GameDllUpdate().
void SkyUpdate();

// Remove the hook. Called from RemoveHooks.
void SkyShutdown();

// True only during a verified sky draw/command flush.
bool SkyPassActive();

} // namespace tr
