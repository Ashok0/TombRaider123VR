// FirstPerson.h -- put the eye where Lara's eye is.
//
// The stereo layer composes finalView = eyeView * gameView, so it never needs
// to know where the game camera is: move the GAME's camera and everything
// downstream follows -- the stereo view, the head-frustum culling, the item
// bounds, both renderers. That is what this does, by rewriting the camera pose
// the engine hands to phd_GenerateW2V for the scene render.
//
// It changes nothing about gameplay. The engine's own chase camera still runs,
// still collides, still decides which room the camera is in; only the pose used
// to build the view matrix for the frame is replaced, and only for the one call
// that builds the scene view.
#pragma once

namespace tr {

// Install or drop the hook to match the bound game DLL and the config. Cheap
// and idempotent; call once per frame beside SkyUpdate().
void FirstPersonUpdate();

// Drop the hook. Called from RemoveHooks.
void FirstPersonShutdown();

// True while the scene camera is being anchored to Lara's head, i.e. what the
// headset is showing is first person. False at the title screen, in the
// inventory, during fixed and cinematic cameras, and whenever the anchor could
// not be resolved.
bool FirstPersonActive();

} // namespace tr
