// FirstPerson.h -- first-person camera and controls for Tomb Raider IV/V.
#pragma once
#include <cstdint>
#include "MotionGunMath.h"

namespace tr {
void FirstPersonUpdate();
// Force Modern Controls while FP is active; restore the entry mode on exit.
void FirstPersonControlModeUpdate();
void FirstPersonToggle();
void FirstPersonRecenter();
// Runtime System recenter changed the tracking basis; preserve world heading.
void FirstPersonTrackingOriginChanged(float yawCorrection);
void FirstPersonShutdown();
bool FirstPersonActive();
bool FirstPersonPhysicalCrouch(bool allowed);
// Consume Right Grip only while a nearby physical wall lever is grabbed.
bool FirstPersonSwitchGrip(bool down, bool allowed);
// Bit 0/1 consume left/right Grip only during a physical holster transfer.
unsigned FirstPersonHolsterGrips(bool left,bool right,bool leftValid,bool rightValid,bool allowed);
// True only while the native light camera draws its shadow map.
bool FirstPersonShadowPass();
// Unmasked palette only within Lara's first-person body draw.
const float* FirstPersonBodyPalette(uint64_t& mask,int& count);
// Complete eye-view palette, including unarmed IK; physics uses the native capture above.
const float* FirstPersonRenderBodyPalette(uint64_t& mask,int& count);
// Hidden wrist palette slots whose mixed costume/forearm surfaces remain visible.
uint64_t FirstPersonBodyWristSeamMask();
// Render-only HK optic pose; no native zoom, weapon or camera state mutations.
bool FirstPersonHKScopePose(motiongun::Frame& lens, motiongun::Frame& camera,float* radiusMetres=nullptr);
bool FirstPersonDrawingTrackedHands();
// Palette index only while a successfully corrected tracked-hand draw is active.
int FirstPersonTrackedHandJoint();
bool FirstPersonHKScopeAiming();
// Current equipped laser weapon, from tracked muzzle in FP or native wrist in TP.
bool WeaponLaserAim(motiongun::Vec& muzzle,motiongun::Vec& direction,int16_t& room);
bool FirstPersonCalibrationKeyReserved(int virtualKey);
// Called after view/graphics chords and physical-pad merge. Returns Y ownership.
bool FirstPersonGunTriggers(uint8_t& left, uint8_t& right, bool chordConsumed, bool y=false);
void FirstPersonInput(float& leftX, float& leftY, float& rightX, bool shifted,
                      bool jumpPressed = false);
} // namespace tr
