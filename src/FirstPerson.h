// FirstPerson.h -- put the eye where Lara's eye is.
//
// The stereo layer composes finalView = eyeView * gameView, so it never needs
// to know where the game camera is: move the GAME's camera and everything
// downstream follows -- the stereo view, the head-frustum culling, the item
// bounds, both renderers. That is what this does, by rewriting the camera pose
// the engine hands to phd_GenerateW2V for the scene render.
//
// A stable tracking-to-world heading drives rendering and locomotion. The
// physical movement drags the body through native collision queries; the
// engine keeps animation and movement triggers.
#pragma once

namespace tr {

// Install or drop the hook to match the bound game DLL and the config. Cheap
// and idempotent; call once per frame beside SkyUpdate().
void FirstPersonUpdate();

// Toggle the complete first-person package at runtime. Third person remains
// the engine's original camera/input path; first person enables the head
// anchor, stable heading, roomscale drag and directional gait handling as one
// unit. Called by the merged-pad Y+LT chord.
void FirstPersonToggle();

// Drop the hook. Called from RemoveHooks.
void FirstPersonShutdown();

// True while the scene camera is being anchored to Lara's head, i.e. what the
// headset is showing is first person. False at the title screen, in the
// inventory, during fixed and cinematic cameras, and whenever the anchor could
// not be resolved.
bool FirstPersonActive();

// Called after merging physical and VR pads. Records manual intent for the
// simulation hook and applies HMD-relative steering through jump preparation
// and flight. Physical displacement never enters the gamepad axes.
void FirstPersonInput(float& leftX, float& leftY, float& rightX, bool shifted,
                      bool jumpPressed);

} // namespace tr
