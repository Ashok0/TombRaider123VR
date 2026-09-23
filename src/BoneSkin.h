// BoneSkin.h -- per-vertex chest deformation for TR1-3 HD Lara.
// Fits a front-of-torso region from the live mesh and patches three-weight
// skin shaders. Supported stock, retail, and Gold hooks are verified.
#pragma once

namespace tr {
// Install before the engine compiles shaders; failure keeps the rigid fallback.
void BoneSkinInstall();
void BoneSkinShutdown();
// Forget mesh calibration on game/level/context changes.
void BoneSkinResetRegion();
// True after a body draw has reached a patched program with a fitted region.
bool BoneSkinActive();
// Run after program/VAO binding and before glDrawElements. Clear persistent
// uniforms on non-body draws, and avoid double motion on the fallback handoff.
void BoneSkinAfterValidate(bool jointApplied = false);
} // namespace tr
