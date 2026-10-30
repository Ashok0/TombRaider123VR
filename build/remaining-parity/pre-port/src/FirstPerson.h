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
#include <cstdint>

namespace tr {
unsigned FirstPersonHolsterGrips(bool left,bool right,bool leftValid,bool rightValid,bool allowed);
void FirstPersonDrawLara(void* item,void* app,void (__fastcall* draw)(void*));
bool FirstPersonPhysicalDuck();
bool FirstPersonFlareTip(int32_t (&tip)[3]);

// Install or drop the hook to match the bound game DLL and the config. Cheap
// and idempotent; call once per frame beside SkyUpdate().
void FirstPersonUpdate();
// Force Modern Controls only in active first person; restore the entry mode on exit.
void FirstPersonControlModeUpdate();

// Toggle the complete first-person package at runtime. Third person remains
// the engine's original camera/input path; first person enables the head
// anchor, stable heading, roomscale drag and directional gait handling as one
// unit. Called by the merged-pad Y+LT chord.
void FirstPersonToggle();
// Right Grip owns a nearby wall lever until release; otherwise retain Walk.
bool FirstPersonSwitchGrip(bool down,bool allowed);
// Preserve world/body heading when the runtime resets its tracking origin.
void FirstPersonTrackingOriginChanged(float yawCorrection);

// Drop the hook. Called from RemoveHooks.
void FirstPersonShutdown();

// True while the scene camera is being anchored to Lara's head, i.e. what the
// headset is showing is first person. False at the title screen, in the
// inventory, during death, fixed and cinematic cameras, and whenever the anchor could
// not be resolved.
bool FirstPersonActive();
// Native light-camera rendering; bypass eye/HMD and orthographic-panel injection.
bool FirstPersonShadowPass();
bool FirstPersonCalibrationKeyReserved(int virtualKey);
// Corrected, unmasked palette for a tracked hand draw; null outside that scope.
const float* FirstPersonHandSkin(int& wristJoint);
// Full native palette during a masked first-person body draw only.
// Complete animation palette for jiggle sampling, before arm IK/visibility masking.
const float* FirstPersonNativeBodySkin();
const float* FirstPersonBodySkin(uint32_t& visibleJoints);
// Complete tracked light-pass palette before native attachment masking.
const float* FirstPersonShadowSkin(uint32_t& visibleJoints,uint32_t& wristJoints);
bool FirstPersonWristCentre(float out[3]);
// The last rendered scene eye, before stereo head translation. False when the
// first-person gameplay camera is unavailable.
bool FirstPersonSceneEye(int32_t out[3]);
// Returns true when physical Y was consumed to holster first-person guns.
bool FirstPersonGunTriggers(uint8_t& left, uint8_t& right, bool chordConsumed, bool y=false);

// Called after merging physical and VR pads. Records manual intent for the
// simulation hook and applies HMD-relative steering through jump preparation
// and flight. Physical displacement never enters the gamepad axes.
void FirstPersonInput(float& leftX, float& leftY, float& rightX, bool shifted,
                      bool jumpPressed);

} // namespace tr
