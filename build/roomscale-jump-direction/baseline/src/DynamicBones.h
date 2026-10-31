// DynamicBones.h -- Lara-only damped spring chest motion for TR1-3.
// Ported from TR4/5 with TR1-3 item offsets and a fixed 32-slot GPU palette.
#pragma once

namespace tr {
// Run after GameDllUpdate at the frame boundary. Rebind the Lara draw hook,
// reset across games/levels, and advance once from the captured torso pose.
void DynamicBonesUpdate();
void DynamicBonesShutdown();

// Bracket every validate_draw. Observe before editing; restore after upload.
// Only the first 15 skeleton slots participate in body classification.
void DynamicBonesObserveDraw();
void DynamicBonesApplyToDraw();
void DynamicBonesRestoreDraw();
bool DynamicBonesAppliedToDraw();

// Shader-path inputs. Body selection is scoped to DrawLaraHD; the solved
// world offset has the gravity equilibrium removed, so rest keeps the mesh.
bool DynamicBonesRenderBody();
bool DynamicBonesWorldOffset(float out[3]);
bool DynamicBonesTorsoFrame(float out[12]);
bool DynamicBonesLaraForward(float out[3]);
bool DynamicBonesDisplacement(float out[2][3]);
} // namespace tr
