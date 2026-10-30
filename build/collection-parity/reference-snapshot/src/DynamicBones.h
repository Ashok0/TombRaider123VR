// DynamicBones.h -- TR6's dynamic-bone spring solver, measured against TR4/TR5.
//
// WHAT TR6 ACTUALLY HAS
//
// 6\DATA\CHAR\LARA_HD.CHR stores its skeleton's bone names inline, and Lara's
// list ends:
//
//   ... SPINE_1 SPINE_2 THORAX NECK HEAD
//   PONY1_DYNAMIC ... PONY10_DYNAMIC
//   SHLDER_L BICEP_L ... LITTLE3_R
//   JUG_L_DYNAMIC  JUG_R_DYNAMIC
//
// The `_DYNAMIC` suffix is the marker that hands a bone to the generic spring
// solver -- CharSkeleton::setup_springs_system at tomb6.dll+0x15E430, then
// SpringSystem::update (+0x16E7E0), ::collide (+0x171C10), ::deflect,
// ::interpolate, ::teleport. So there is no bespoke system to lift: the chest
// is two extra skeleton bones run through the same solver as the ponytail.
//
// WHY IT CANNOT BE PORTED AS CODE
//
// SpringSystem operates on CharSkeleton bone arrays fed by AOD asset data
// (deflector groups, per-outfit spring setups). TR4/TR5 have none of it. Lara
// there is still the classic 15-node rigid hierarchy -- SkinUseMatrix is 28
// bytes (14 x short), SkinJoints 56 -- hips, thigh/calf/foot x2, torso, arms,
// head. The chest is polygons inside the rigid TORSO mesh, and the HD outfit
// meshes (4\ITEM\OUTFIT_*.TRM, 20 of them) carry no bone names at all; they
// bind to those same 15 nodes. Copying the solver across gives you a solver
// with nothing to solve.
//
// The braid is not a counterexample. TR4/TR5 animate it with HairControl /
// HairAdvance / CalcHairMatrices, a CPU spring chain that draws its own
// meshes -- not a bone the skinning path ever sees.
//
// WHAT THIS FILE IS
//
// The measurement step, not the feature. It reimplements TR6's spring model
// (damped spring to a rest point carried by a parent frame, gravity, a
// displacement clamp, and the teleport snap that SpringSystem::teleport
// exists for) and drives it from TR4/TR5's torso joint -- then logs what
// comes out. It changes nothing on screen.
//
// It exists to answer three questions before anyone hooks a shader:
//
//   1. How many joints does a TR4/TR5 Lara draw actually use, and are there
//      free slots above it? ANSWERED: the body draw uploads 15, the classic
//      hierarchy, leaving 57 of the renderer's 72 slots free. Frames also
//      carry 33-slot palettes for attachments, 13 of whose entries are one
//      repeated filler matrix, so the body has to be picked by structure
//      rather than by draw order -- see DynamicBones.cpp.
//
//      The palette holds SKINNING matrices (bone * inverse-bind), not
//      bone-to-world transforms. Measured proof: forearm to hand 2.0 units,
//      hips to torso 0.5 -- impossible as origins, and exactly what a
//      skinning matrix gives when a child shares its parent's orientation.
//      Anchors therefore live in bind-pose model space.
//
//      Joint 7 is TORSO, confirmed from tomb5.dll SkinUseMatrix rather than
//      assumed: its only four populated byte-pairs are the knees and elbows,
//      which pins the canonical 15-node order.
//
//   2. Is the drive signal clean at VR frame rates? ANSWERED, and favourably:
//      dup=0%% across every measured session. Not one rendered frame repeated
//      the previous joint matrix, which fits an engine carrying
//      CalcLaraMatricesHDAnim and HDAnims -- the remaster interpolates poses
//      per render frame rather than holding 30 Hz keyframes. The risk that
//      would have killed Route A is not there.
//
//   3. Do TR6's constants transfer? No, and neither did the first
//      formulation. A world-space position spring lags by c*V/k under
//      constant velocity, which at Lara's running speed pinned the bone on
//      its clamp permanently. The solver is now driven by the parent joint's
//      ACCELERATION in the parent's own frame, so steady running produces no
//      displacement and it rests at gravity equilibrium -- measured at 2.82
//      units against a predicted gravity/stiffness of 5400/1900 = 2.84.
//
// Scope: TR4 and TR5 only. TR6 has the real thing already.
#pragma once

namespace tr {

// Install or drop the DrawLaraHD hook to match the DLL currently bound, then
// advance the solver one frame. Cheap and idempotent; call once per frame,
// after GameDllUpdate().
void DynamicBonesUpdate();

// Called per draw from validate_draw. Latches the torso joint while the
// original DrawLaraHD is on the stack; does nothing otherwise.
//
// Identifying Lara by scope rather than by heuristic matters. Several
// characters are skinned through the same shaders in the same frame, and
// "the draw with the most joints" would silently follow the wrong one in any
// level with a bigger NPC in it.
void DynamicBonesObserveDraw();

//// --- for BoneSkin.cpp, the per-vertex path --------------------------------
//
// True while the draw validate_draw is about to issue is one of Lara's body
// draws. Unlike the solver's latch this is NOT limited to the locked shader:
// the latch exists for the solver's continuity, but every body draw needs the
// deformation or chunks of the chest drawn by another material would stay put.
bool DynamicBonesRenderBody();

// The chest displacement for this frame in WORLD space, measured FROM REST and
// scaled by DynamicBonesDebugScale.
//
// From rest matters once the offset is applied to the chest alone. The solver
// settles at gravity / stiffness below its anchor; with the whole torso moving
// together that sag was invisible, but applied to just the chest it would leave
// her permanently drooping below the authored mesh. Subtracting it means zero
// standing still, a rise while airborne, and ringing about zero on landing.
bool DynamicBonesWorldOffset(float out[3]);

// The latched torso joint matrix, 12 floats row-major 3x4. False before the
// first body draw of the session.
bool DynamicBonesTorsoFrame(float out[12]);

// Lara's facing as a world-space unit vector, from lara_item->pos.y_rot.
bool DynamicBonesLaraForward(float out[3]);

// Put the solved displacement into the joint palette the draw is about to
// upload, and take it out again once that upload has happened.
//
// THIS IS THE ONE THING HERE THAT CHANGES WHAT YOU SEE, and it is a debug
// view rather than the feature: displacing joint 7 moves everything weighted
// to TORSO, so the whole upper body wobbles instead of just the chest. That
// is the point -- it makes the solver's output visible without any shader
// work, so the anchor position, the motion's direction and its magnitude can
// be judged by eye before anything more expensive gets built.
//
// Off unless DynamicBonesApply=1.
//
// Apply writes into the engine's own joint array, so Restore MUST run after
// the upload and before anything else reads it. Hooks.cpp does that with a
// scope guard so every return path out of validate_draw is covered; leaving
// the engine's array modified would corrupt whatever object drew next.
void DynamicBonesApplyToDraw();
void DynamicBonesRestoreDraw();

// Remove the hook and reset the solver. Called from RemoveHooks.
void DynamicBonesShutdown();

// The solved displacement for one frame, in the TORSO JOINT'S OWN FRAME and
// in world units: out[0] is the left bone, out[1] the right, each x/y/z.
//
// False when there is no live solution -- nothing bound, Lara not drawn this
// frame, or the solver still settling after a teleport snap. This is the seam
// the shader path would consume; nothing reads it yet.
bool DynamicBonesDisplacement(float out[2][3]);

} // namespace tr
