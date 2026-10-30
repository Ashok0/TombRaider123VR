// BoneSkin.h -- move the chest per vertex, leaving the back and shoulders still.
//
// WHY THE JOINT ALONE WAS NOT ENOUGH
//
// DynamicBones' first visible output displaced joint 7, TORSO. A joint
// transform moves every vertex attached to that joint by the same amount, and
// TORSO carries the chest, the back, the backpack and the torso side of both
// shoulder seams. So the back and backpack bounced with the chest, and the
// shoulders stretched against arms that stayed put. No setting fixes that: a
// joint has no notion of "front".
//
// WHAT THE SHADER KNOWS THAT THE JOINT DOES NOT
//
// Lara's HD body is skinned in the vertex shader, three joints per vertex:
//
//   aCoord       one bind-pose position, in MODEL space
//   aLight.xyz   the three joints it follows
//   aColor.xyz   how much it follows each
//
// So per vertex the shader knows both how much of it belongs to TORSO and where
// on the body it sits. The patch adds a weight from that position -- inside a
// chest-height band, in front of the torso's depth midline, and inside a
// lateral limit short of the shoulders -- and moves the vertex by the solved
// offset times that weight times its TORSO share. Back, backpack, arms,
// shoulder tops and armpits sit outside the region, or belong to other joints,
// and stay exactly where the animation put them.
//
// HOW IT GETS IN
//
// The engine builds each of its 202 programs with one call to
//   shader_init(Shader* shader, int fvf, const char* vs, const char* fs)
// and the hook substitutes the vs argument for the index+weight skinning
// family: shaders 9..23, built from 5 sources, which include her body programs
// 12, 20 and 21. That mapping was decoded from all 202 calls in init_ogl.
// Every other argument, and every other shader, passes through untouched.
// Nothing global is swapped, so there is no engine state to restore.
//
// TWO MISTAKES THIS REPLACES, so neither is repeated:
//
//   * The first build assumed shader_init took no arguments. The detour's own
//     calls clobbered the argument registers before calling the original,
//     which wrote through a junk Shader pointer and crashed at startup.
//   * The next patched the OTHER skinning family, the two-matrix one. 63
//     programs were patched and none of them was Lara's, so nothing moved --
//     and because BoneSkinActive() only asked whether anything was patched,
//     the whole-torso view stood down too. It now only stands down once a body
//     draw has actually gone through a patched program, and a body drawn with
//     an unpatched program is logged.
//
// Every patched source is test-compiled first. One that fails is logged and
// the ORIGINAL goes to the engine instead, so a patch mistake costs the effect
// on that pass, never Lara herself.
//
// WHERE THE CHEST IS
//
// Not assumed. The first body draw reads its own vertex buffer back through the
// bound VAO, collects every vertex that mainly follows TORSO, and fits the
// region to that measured shape; front and back are told apart from Lara's
// facing angle rather than guessed. The shape is logged as a depth-by-height
// map, so a wrong fit is visible in the log rather than argued about.
//
// Stock build only: the shader_init address comes from the stock build's PDB,
// and the HD-pack builds have no PDB to take it from. Their layout rows leave
// it at 0, which reads as "not available here".
#pragma once

// Also patches tracked-hand skinning independently of chest-physics settings.
// Mixed hand/forearm vertices use the corrected wrist rigidly during that hand's
// draw; the fragment shader trims the hidden forearm at the blend seam. The
// default/off uniform retains native rendering for all other draws.

namespace tr {

// Hook shader_init. Called from InstallHooks after the core hooks are in, and
// best-effort: failure costs the per-vertex path and nothing else.
void BoneSkinInstall();

// Remove the hook. Called from RemoveHooks.
void BoneSkinShutdown();

// True when patched shaders exist and the per-vertex path is configured on --
// in which case DynamicBones stops editing joint 7 directly.
bool BoneSkinActive();

// Called after validate_draw has bound the program and the mesh's VAO, and
// before glDrawElements. Uploads the chest offset for Lara's body draws and
// clears it for every other draw sharing a patched program.
void BoneSkinAfterValidate();

} // namespace tr
