// Config.h -- INI-backed tunables, read once at startup from TombRaiderVR.ini
// next to the DLL.
//
// The sign conventions are exposed deliberately. The engine renders Y-flipped
// (ogl_setPerspAngles writes e11 = -1/tanY, ogl_setScissor flips Y against
// gTargetHeight) and TR world space is Y-down, while OpenVR is Y-up in metres.
// That is two independent flips, and which way they compose is the single most
// likely thing to be wrong on first run -- so it is a config toggle rather than
// a recompile.
#pragma once
#include "MotionGunMath.h"

namespace tr {

struct Config {
    bool  enabled          = true;
    // TR4 load-time enhancement of native sunray/effect textures, without DDS replacements.
    bool  tr4Effects = true;
    float tr4SunrayStrength = 1.0f;
    float tr4EffectStrength = 1.0f;

    // Mono head-tracking bring-up mode.
    //
    // No stereo target, no FBO_default redirection, no draw duplication, no
    // compositor submission. The head pose is composed onto the game camera and
    // the frame renders normally to the monitor. The engine keeps its own
    // projection, so field of view is untouched.
    //
    // This is the mode to prove tracking, handedness and world scale in before
    // turning stereo on -- it isolates the view maths from everything else. It
    // also injects on every world-space pass rather than only backbuffer-
    // targeted ones, so it still works if the scene renders offscreen first.
    //
    // OpenVR is initialised as a Background app in this mode, so it reads poses
    // without becoming the VR scene application.
    bool  monoTracking     = false;

    // --- ceiling clearance ---------------------------------------------------
    //
    // Stop the tracked head rising through the ceiling in low tunnels and
    // crawlspaces. The game camera is already near the ceiling there, and our
    // positional offset pushes the eye straight through it.
    //
    // This CLAMPS, it never moves you: the head's height is capped so the eye
    // stays ceilingMarginUnits below the room's ceiling, and everything below
    // that cap behaves exactly as before. Nothing is added to your motion,
    // which is the whole reason it is a clamp rather than a camera that drops
    // to torso height on its own -- unrequested vertical motion is a comfort
    // problem, and a clamp can only ever subtract.
    //
    // Headroom comes from the camera room's bounding box, so it is exact in a
    // uniformly low room and over-generous in a low pocket off a tall hall,
    // where the clamp simply will not engage.
    // Where the head's positional offset lives: WORLD space (true, the default)
    // or the game camera's frame (false, the old behaviour).
    //
    // This is what makes PositionalTracking=1 behave like 1 for the HEADSET and
    // like 0 for the ANALOG STICK, which is the whole point of it.
    //
    // In the camera's frame the offset is rotated by the camera, so rotating the
    // view sweeps the eye on an arc of radius |offset| while you sit perfectly
    // still -- the look stick walks your viewpoint sideways into walls, and camera
    // pitch turns a forward lean into rise and fall. World space instead
    // integrates the offset from what the HEADSET did:
    //
    //     offsetWorld += R_camera^T * (thisFrame - lastFrame)
    //
    // so stick rotation moves nothing, head motion moves you in the direction you
    // are facing at the moment you move, and camera translation still carries you
    // along. See WorldLockOffset in VRSystem.cpp for the derivation and the cost:
    // the offset is state, so it can drift from your physical centre, which
    // recentreKey and an automatic re-anchor on camera jumps take care of.
    bool  headOffsetWorld     = true;

    // Re-anchor the head offset to the game camera. Numpad 5 -- the middle of the
    // numpad tuning cluster, and the only key in it that was free.
    int   recentreKey         = 0x65;   // VK_NUMPAD5

    bool  ceilingClearance    = true;

    // How close the eye may get to the ceiling, in world units. 128 is about
    // 0.30 m at the default scale. Raise it if you still clip, lower it if the
    // cap feels like it arrives too early.
    float ceilingMarginUnits  = 128.0f;

    // Positional head tracking is enabled in the known-good Steam profile.
    bool  positionalTracking = true;

    // Seated tracking origin rather than standing.
    //
    // This matters more than it looks. Standing space is absolute room
    // coordinates: your head is ~1.6 m off the floor, which at 423 units/m is
    // ~680 TR units of vertical offset applied to the camera before you have
    // moved at all. Seated space is relative to the seated zero, so at rest the
    // offset is ~0 and the view starts where the game put it.
    bool  seatedOrigin     = true;

    // TR world units per real-world metre. Lara is ~762 units tall and reads as
    // roughly 1.8 m, giving ~423. Raise it to shrink the world (and the
    // apparent IPD), lower it to grow the world.
    float worldUnitsPerMetre = 423.0f;

    // Multiplier on the eye separation ONLY, leaving world size alone.
    //
    // WorldUnitsPerMetre is the physically honest control: it changes apparent
    // size and stereo depth together, because they are the same thing. IpdScale
    // is the cheat -- it strengthens depth without shrinking the world. Reach
    // for it only if the scale feels right but the depth still reads flat.
    float ipdScale         = 1.0f;

    // Step per hotkey press, multiplicative. 1.25 = 25% a press, so three
    // presses roughly double. 5% steps proved far too small to judge against --
    // scale differences only become obvious at something like 2x.
    float scaleStep        = 1.25f;

    // Live tuning hotkeys. Numpad +/- adjust world scale, Numpad * / adjust
    // IpdScale, Numpad 0 resets both to the ini values. Every change is logged
    // so the value that felt right can be copied back into this file.
    // 0 disables a key.
    int   scaleUpKey       = 0x6B;   // VK_ADD       (numpad +)
    int   scaleDownKey     = 0x6D;   // VK_SUBTRACT  (numpad -)
    int   ipdUpKey         = 0x6A;   // VK_MULTIPLY  (numpad *)
    int   ipdDownKey       = 0x6F;   // VK_DIVIDE    (numpad /)
    int   resetTuningKey   = 0x60;   // VK_NUMPAD0

    // Per-eye render target size. 0 = ask OpenVR for the recommended size.
    int   eyeWidth         = 0;
    int   eyeHeight        = 0;
    float superSample      = 1.0f;

    // Sign conventions -- flip these first if the image is inverted or the eyes
    // are swapped. See the header comment.
    bool  flipProjectionY  = true;   // match the engine's e11 = -1/tanY
    bool  flipViewY        = true;   // OpenVR Y-up  ->  TR Y-down
    bool  swapEyes         = false;

    // Vertically flip the V texture bounds when handing the eye texture to the
    // compositor.
    //
    // Default OFF, and the reasoning matters. It is tempting to tie this to
    // FlipProjectionY on the grounds that "the engine renders Y-flipped", but
    // that gets it exactly backwards: the engine's e11 = -1/tanY exists to
    // CANCEL TR's Y-down world, so the result is an ordinary, correctly
    // oriented GL render with the origin at lower-left -- which is what the
    // compositor expects with default bounds. Inverting V on top of that turns
    // a correct image upside down.
    //
    // Symptom guide:
    //   upside down in the HMD but fine on the monitor .. this setting
    //   upside down in BOTH ......................... FlipProjectionY
    bool  flipSubmitV      = false;

    // Keep the 2D/ortho layer (HUD, menus, subtitles, fades) on the flat path
    // rather than reprojecting it. vid_setPass tells us which passes those are.
    bool  flatHud          = true;

    // Distance in metres at which the 2D layer (HUD, menus, subtitles) sits.
    // 0 = leave it alone, which double-visions: see VRSystem::HudNdcShiftX.
    float hudDepthMetres   = 4.0f;

    // Horizontal angular width of the 2D panel, in degrees, as seen from the
    // game camera. The engine's 2D layer fills the flat screen; mapped onto the
    // headset's ~94 degree field of view that would be overwhelming, so it is
    // placed on a panel of this width instead. Vertical follows the screen
    // aspect. Only used when hudLockToHead is false.
    float hudSizeDegrees   = 55.0f;

    // false (default): the 2D layer is a quad fixed in the GAME camera's frame,
    //   so it stays put in the world while your head turns. Built as
    //       Q = P_persp * E * L * P_o
    //   where L lifts ortho NDC onto a plane at hudDepthMetres, E is the per-eye
    //   head transform, and P_persp is the real per-eye projection.
    //
    // true: the old behaviour -- a flat per-eye NDC shift, so the panel is
    //   welded to your head and swings with every rotation. Kept because a few
    //   passes (fades, full-screen effects) genuinely want to be screen-locked.
    bool  hudLockToHead    = false;

    // --- the ortho-3D layer (the inventory) ---------------------------------
    //
    // vid_setOrtho3D (RVA 0x0000B880) copies the ORTHO matrix into mProj[1] and
    // points vid_state.proj at it, so pointer identity -- which is all
    // IsWorldPass() has -- reports "world space" for a pass that is
    // orthographic. Substituting a per-eye perspective frustum there would
    // divide an ortho layout by a depth it was never built for.
    //
    // This is NOT what stacked the inventory; preserveProjOffset above was.
    // The path has never been seen to fire in TR4 or TR5 -- the ortho3D=
    // counter in the health report is what says whether it ever does.
    //
    // true  = classify by matrix CONTENT as well, and leave the engine's own
    //         ortho projection in place for those passes.
    // false = the old behaviour, kept so the difference can be A/B'd.
    // Carry the engine's own projection OFFSET through the per-eye
    // substitution. vid_setPerspOffset (RVA 0x0000B8D0) writes e02 and e12 --
    // m[8] and m[9], the two shear terms -- and nothing else. A shear of s
    // shifts ndc by -s at every depth, so it is how the engine places a draw on
    // screen without touching its geometry.
    //
    // The inventory is laid out entirely with it. Measured, one frame of it:
    // nine item draws, every one with an identity model matrix, an identity
    // view matrix and a single joint, differing ONLY in e02 -- 2.0250, 1.3500,
    // 0.6750, 0.0000, -0.6750, -1.3500, evenly spaced 0.675 apart. That even
    // spacing IS the horizontal bar, and the values past +/-1 are the items
    // scrolled off the sides of the screen.
    //
    // Overwriting the projection with the eye frustum discarded all of it, so
    // every item got the same shear and landed in the same place -- the stack.
    // Adding the engine's shear to the eye's own keeps both: the frustum
    // asymmetry the headset optics need AND the engine's placement.
    //
    // ogl_setPersp and ogl_setPerspAngles both explicitly zero e02/e12, so this
    // is exactly zero during gameplay and costs nothing there. 0 = old
    // behaviour, for A/B.
    bool  preserveProjOffset = true;

    // Trim on the whole offset. 1.0 reproduces the engine's layout exactly, so
    // the inventory row spans the same ANGLE it does flat -- which on a 96
    // degree game frustum is a wide row to sweep your eyes across. Lower it to
    // pull the elements in toward the centre; 0 collapses them back into one
    // stack, which is what the bug looked like.
    float projOffsetScale    = 1.0f;

    bool  ortho3D            = true;

    // Distance in metres at which the ortho-3D layer converges. 0 = no shift at
    // all: the layer is left exactly as the engine drew it, which still fixes
    // the stacking but double-visions the way an untouched HUD does.
    float ortho3DDepthMetres = 2.0f;

    // true (default): a flat per-eye NDC shift, the same operation as
    //   hudLockToHead. An ortho projection has no perspective divide, so moving
    //   m[12] is pure convergence -- every relative x, y AND z survives it.
    //   That last one is why this defaults the opposite way to the HUD: these
    //   are 3D meshes carrying real depth, and the HUD's panel discards the z
    //   input, which would leave every item z-fighting with itself.
    //
    // false: the world-locked panel, built exactly like the HUD's
    //   Q = P_persp * E * L * P_o, except L's z column is filled in so ortho
    //   depth lands in a slab instead of collapsing onto one plane.
    bool  ortho3DLockToHead  = true;

    // Horizontal angular width of the world-locked ortho-3D panel, in degrees,
    // as seen from the game camera. Ignored when ortho3DLockToHead is true.
    float ortho3DSizeDegrees = 55.0f;

    // Thickness of the world-locked panel's depth slab, in metres: ortho NDC z
    // maps onto ortho3DDepthMetres +/- half of this. Too thin and the item
    // meshes z-fight; too thick and they separate visibly in depth. Ignored
    // when ortho3DLockToHead is true.
    float ortho3DSlabMetres  = 0.5f;

    // Distance in metres for full-screen passes that bypass uProjMatrix.
    //
    // Five of the engine's shaders write gl_Position = vec4(aCoord, 1.0) --
    // straight to clip space, no matrix at all. Pre-rendered video cutscenes go
    // through that path, which is why they stay double-visioned when everything
    // else is fixed: identical in both eyes, no frustum shear, so the headset
    // optics pull the two copies apart.
    //
    // No matrix can move them, so the viewport is shifted per eye instead.
    // 0 disables the shift entirely.
    float videoDepthMetres = 6.0f;

    // false (default): the panel is anchored to the GAME camera. A viewport
    //   shift cannot reproject a quad, but it can put it where the panel centre
    //   belongs: transform a point straight ahead at videoDepthMetres by the eye
    //   transform, project it, and place the viewport there. Head rotation then
    //   moves it across your view, and it leaves view if you turn away -- which
    //   is what world-locked means. Roll and foreshortening are not
    //   representable this way, but for a flat video panel they do not read.
    //
    // true: welded to your head, the previous behaviour.
    bool  videoLockToHead  = false;

    // Horizontal angular width of the video panel, in degrees, seen from the
    // game camera.
    //
    // This also decides how much the panel can distort. Projecting only the
    // panel's CENTRE gives translation and nothing else, so the quad keeps a
    // constant angular size wherever it sits -- at the edge of a ~94 degree
    // field of view it should shrink and foreshorten, and instead it stretches.
    // Projecting the four CORNERS and fitting the viewport to their bounding
    // box recovers the size term, so the panel behaves like real geometry.
    //
    // What remains is keystone: a quad seen off-axis should go trapezoidal, and
    // a bounding box stays rectangular. That error grows with panel width, so a
    // narrower panel is also a flatter-looking one. 50-70 is a good cinema size;
    // much above 90 and the keystone starts to show at the edges.
    float videoSizeDegrees = 60.0f;

    // Capture the video pass into an offscreen texture and replay it as a real
    // quad, instead of shifting the viewport.
    //
    // The viewport fit leaves keystone: an off-axis quad should be trapezoidal
    // and a bounding box is rectangular. Replaying as actual geometry removes
    // that completely -- the quad keystones, foreshortens and rolls exactly like
    // the world, because it IS world geometry.
    //
    // Costs one framebuffer bind and two quad draws per video frame, and only
    // during video: the branch is gated on a pass having no uProjMatrix at all,
    // which never happens in gameplay. It also rasterises the video once rather
    // than twice, which nearly pays for itself. Falls back to the viewport fit
    // if the shader API or the framebuffer is unavailable.
    bool  videoOffscreen   = true;

    // Flip the captured video vertically on replay. Only needed if the cutscene
    // comes out upside down.
    bool  videoFlipV       = false;

    // Native stereo for TR6 (Angel of Darkness).
    //
    // TR6's PDB identifies App_Render_Scene as the complete render-only scene
    // boundary. Running that function once per eye lets the whole offscreen
    // chain complete normally, then copies its final composite into the
    // corresponding half of the VR target. Simulation still advances once.
    // The two passes reuse TR6's intermediate targets sequentially; the left
    // result is copied out before the right pass overwrites them.
    //
    // This is currently verified for the stock tomb6.dll build whose PDB is in
    // the development symbol set. An unknown DLL build falls back to AER.
    bool  nativeStereoGame6 = true;

    // Alternate-eye fallback for TR6 (Angel of Darkness).
    //
    // TR4 and TR5 draw straight to the backbuffer, so every world draw can be
    // issued twice into the two halves of one double-wide target. TR6 cannot
    // work that way: it renders the scene into the engine's own 2560x1440
    // offscreen textures and composites at the end, and the composite samples
    // those whole with 0-1 UVs. Widening them would break that sampling, and
    // duplicating individual draws into halves of a target the composite reads
    // entire would corrupt it. Measured: 572970 of 579945 world draws offscreen,
    // against roughly 9% in TR4/5.
    //
    // So each FRAME is rendered entirely as one eye, mono, through the engine's
    // normal path, and lands in that eye's half of the stereo target. The other
    // half keeps the previous frame. Nothing is resized and nothing is
    // duplicated.
    //
    // Used only when NativeStereoGame6 is disabled or its exact render hook is
    // unavailable. Each eye then updates at half the rendered frame rate.
    bool  alternateEyeGame6 = true;

    // How many offscreen world draws a frame needs before the AER fallback
    // engages. Native stereo uses the actual App_Render_Scene call instead.
    //
    // AER is only worth its half-rate cost when there is an offscreen 3D scene
    // to reach. The TR6 main menu and the FMVs are 2D: they draw straight to the
    // backbuffer, so the ordinary per-draw duplication handles them at FULL rate
    // and looks better doing it. Gameplay runs ~800 offscreen world draws a
    // frame; menus and video are near zero, so the count separates them cleanly
    // with no extra symbols to find.
    //
    // The decision is latched once per frame from the previous frame's count, so
    // it stays stable for the whole frame and costs one frame at a transition.
    int   alternateEyeMinOffscreen = 50;

    // Do not use the offscreen video panel in TR6.
    //
    // TR6 renders the scene into custom offscreen targets and composites to the
    // backbuffer through a clip-space-direct shader -- indistinguishable from an
    // FMV quad by the uid[0] < 0 test. Capturing it turns the whole game into a
    // small floating panel. TR6's actual FMVs use the viewport-shift path
    // instead, which suits a full-screen video regardless.
    bool  videoSkipGame6   = true;


    // Negate Y when lifting the 2D layer onto the panel. Leave at 1.
    //
    // The engine's ortho matrix already flips Y, because TR's 2D space is
    // Y-down, so P_o emits ordinary GL NDC with Y up. The per-eye projection is
    // built with FlipProjectionY and expects Y-DOWN input, so feeding it Y-up
    // flips the image a second time -- and reverses triangle winding, which
    // backface culling then removes entirely. That reads as "the HUD vanished
    // except the one element drawn with culling off, and that one is upside
    // down". This cancels the extra flip.
    bool  hudFlipY         = true;

    // Duplicate every world-space draw into both viewport halves. Turning this
    // off leaves per-eye matrix injection running but draws once -- useful for
    // isolating whether a problem is in the matrices or in the duplication.
    bool  duplicateDraws   = true;

    // Mirror the left eye back to the game window.
    bool  mirrorToWindow   = true;

    float nearClip         = 0.0f;   // 0 = keep whatever the engine set
    float farClip          = 0.0f;

    bool  verboseFirstFrame = true;

    // Present the Oculus Touch controllers to the game as an Xbox pad.
    //
    // The game resolves XInput through GetProcAddress into a function-pointer
    // global, so there is no import to hook and no virtual-pad driver needed --
    // we overwrite that pointer. inputUpdate() polls it every frame and switches
    // app.input_type to INPUT_TYPE_XB on the first input, so the game adopts the
    // Xbox control scheme and prompts by itself. A real pad, if plugged in, is
    // merged rather than replaced.
    bool  gamepadEnabled   = true;

    // Log the raw OpenVR legacy button masks whenever they change. Touch's
    // button ids differ between runtimes, so if something lands in the wrong
    // place this says which mask it actually set.
    // Hold R3 to turn the LEFT stick into a D-pad.
    //
    // R3 is the modifier rather than L3 because L3 is Sprint, and sprint in
    // this game is held WHILE running forward -- so "hold L3, push the stick
    // forward" is already a gesture in play, and reusing it for D-pad up would
    // mean choosing between them. R3 is the only spare input on the pad: it
    // emitted RIGHT_THUMB and nothing else. A plain R3 click still emits
    // RIGHT_THUMB, so nothing that relied on it is lost -- the D-pad only
    // appears once the left stick is actually deflected.
    // Hold Y + LT for this long to send the Xbox Menu button (XInput START).
    //
    // Touch has no Start or Back of its own, so both have to come from
    // somewhere. The System button on the left hand's lower face sends one of
    // them (see gamepadMenuUsesBack); this chord reaches the other without
    // spending another button.
    //
    // A deliberate hold: Y is Action and LT is Equip, so the pair does occur in
    // ordinary play, and the duration is the only thing separating the chord
    // from a real grab-and-draw. One second is short enough to be comfortable
    // but IS within reach of normal play -- if it ever fires when you did not
    // mean it, this is the number to raise.
    //
    // It sends a PRESS, not a latch -- Menu toggles the pause screen itself, and
    // a held START would never look like a clean press to the game. 0 disables.
    float menuChordSeconds = 1.0f;

    // How long the synthesised Menu press is held, in seconds. Long enough for
    // the game to sample it across several frames, short enough not to read as
    // a second press.
    float menuChordPressSeconds = 0.15f;

    bool  dpadShift        = true;

    // Touch X opens a paused, tracking-space anchored native inventory in
    // TR4/5. Native X ammo switching is preserved. Requires stereo VR.
    bool  worldInventory   = true;

    // How far the left stick must travel before a shifted press registers.
    float dpadShiftDeadzone = 0.5f;

    bool  gamepadLogButtons = false;

    // Right stick turns only. Its vertical axis is dropped, so the game camera
    // never pitches from the stick.
    //
    // In VR the headset already owns pitch: you look up by looking up. Stick
    // pitch is then a second, conflicting source for the same axis, and it is
    // the uncomfortable one -- a vertical rotation the inner ear did not ask
    // for is the strongest simulator-sickness trigger there is, worse than yaw.
    //
    // ON by default. It does change what the pad can do -- the engine aims and
    // reads its look camera from the pitch this suppresses, so a shot lined up
    // by tilting the stick has to be lined up by tilting your head instead --
    // but decoupledPitchChord hands the stick back on demand, so nothing is
    // actually out of reach, and comfort is the right thing to default to in a
    // headset. Set to 0 for the stock two-axis stick.
    bool  decoupledPitch    = true;

    // Hold RT + RB to INVERT decoupledPitch for as long as both are held. On by
    // default; it costs nothing when unused.
    //
    // Inverts rather than forces, so it earns its place whichever way round the
    // setting is: off, the chord decouples while held; ON, the chord hands the
    // stick pitch back while held, which is the case that matters once you play
    // with decoupledPitch=1 all the time and want the stick for one shot.
    //
    // Note what RB is on Touch. There is no physical shoulder button -- the
    // RIGHT grip synthesises XB_X for Walk. The pitch chord accepts that Walk
    // bit as the physical RB signal, so it is still right grip + right trigger
    // and reads as "walk and shoot". Set this to 0 if you would rather that
    // combination leave pitch alone.
    bool  decoupledPitchChord = true;

    // Hand stick pitch back automatically while Lara is in water.
    //
    // Swimming is the one place the decoupled stick is not a comfort win but a
    // straight loss: TR steers the swim with the LOOK axis, so suppressing
    // pitch removes the ability to dive or surface at all -- and the head
    // cannot stand in for it, because the head turns the VIEW while the stick
    // turns LARA.
    //
    // Read from lara.water_status in the game DLL, so it follows Lara and not
    // the camera. An earlier attempt used the camera room's water flag and
    // failed exactly where it mattered: at the surface the camera sits in the
    // AIR room above the water, so it reported dry through an entire swim.
    bool  decoupledPitchWaterOff = true;

    // Hand stick pitch back automatically while looking through an optic --
    // binoculars, or a weapon combined with a laser sight.
    //
    // The same failure as swimming, for the same reason: the game's own zoom
    // camera (BinocularCamera_TR4/TR5) reads the right stick's Y axis directly
    // for vertical aim while zoomed, and the head cannot stand in for it --
    // decoupledPitch exists to let the HEAD look around independently of where
    // LARA is aimed, and inside a scope those two are supposed to be the same
    // thing. Without this, holding the zoom (right stick click, held) gets you
    // a scope that pans sideways but never up or down.
    //
    // Read from BinocularOn / BinocularRange in the game DLL -- the same two
    // fields the game's own ProcessLooking checks to decide whether the stick
    // means "look" or "aim the zoom camera", so this follows the game's own
    // notion of "currently zoomed" rather than a guess at one.
    bool  decoupledPitchZoomOff = true;
    bool  worldLaserSight = true; // native laser ray drawn in stereo world space

    // Hold the head CENTRE at the game camera while looking through an optic, so
    // the LASER SIGHT hits what it points at. On by default.
    //
    // NOT a stereo switch. The eyes keep straddling the centre by half an IPD,
    // so the world keeps all of its depth; only the 6DOF displacement is held
    // off, through the same line PositionalTracking=0 uses.
    //
    // THE BUG IT FIXES. The dot is not a world object. DrawBinoculars' LaserSight
    // branch takes target_mesh_ptr, scales it to the screen rect and writes it
    // into raw_vbuf with z = 0, reached from S_OutputPolyList -- it is a
    // screen-centre crosshair in the 2D overlay pass. The shot is real geometry:
    // BinocularCamera_TR4/TR5 raycast camera.pos -> camera.target through
    // GetTargetOnLOS and the impact comes out of TriggerRicochetSpark at the
    // clipped hit point. So the crosshair and the impact are two points on ONE
    // LINE out of camera.pos -- which is why they agree perfectly on a flat
    // screen, and why they cannot agree in VR: the mod draws the crosshair on the
    // flat panel at hudDepthMetres while the impact is at the wall, and two
    // points on a line project to the same pixel only from an eye that is ON that
    // line. Measured error is h*(D/Z - 1) for a head offset h: 0.3 m of head at a
    // 10 m target with a 4 m panel is 0.45 m, and it reads as vertical because
    // seated your lateral offset is nearly zero while your vertical one is not.
    //
    // WHY THE IPD CAN STAY, which is the whole reason this does not cost depth:
    // the crosshair sits at (0, 0, -Z) in the camera's frame -- on the aim axis
    // for ANY panel depth -- and with the head centre back on camera.pos the
    // MIDPOINT between the eyes is on the aim line even though neither pupil is.
    // The two eyes' errors are equal and opposite, so they cancel in the fused
    // direction and what is left is the dot appearing to float in front of the
    // wall instead of resting on it. A vergence artefact, not an aiming error.
    //
    // WHAT IT COSTS: raising an optic moves your viewpoint to the game camera,
    // which is unrequested motion -- the one thing this mod otherwise refuses to
    // do. It is bounded by how far your head is from the camera, it happens on a
    // deliberate button press, and it is inherent: putting the eye on the aim
    // line means moving it there. 0 gives the stock behaviour back, with the
    // misalignment.
    bool  opticsHeadAtCamera = true;

    // Switch off the engine's optic overlays. Both on by default.
    //
    //   hideBinocularOverlay  DrawNormalBinocs, DrawVCIHeadset,
    //                         DrawLabyrinthFishEye -- the binocular vignette,
    //                         TR5's VCI visor, the Labyrinth fisheye
    //   hideScopeOverlay      DrawNormalLaserSight -- the scope frame, which
    //                         also carries its reticle lines
    //
    // These are flat full-screen artwork: DrawBinoculars scales the mesh to the
    // screen rect and writes it into raw_vbuf with z = 0, so each is a 2D quad
    // that the mod's 2D path lands on the world-locked panel at hudDepthMetres.
    // A vignette imitates the edge of your vision, and as a rectangle floating
    // at 4 m inside a 94-degree field of view it cannot: your real peripheral
    // vision is wide open around it. No placement fixes that, because the thing
    // being imitated is the headset's own field stop.
    //
    // THE AIMING DOT SURVIVES BOTH. It is not part of any of those meshes --
    // DrawBinoculars draws it separately after DrawGameInfo as a DefaultSprites
    // sprite at the centre of the screen rect, gated on LaserSightActive and
    // coloured through LaserSightCol, so it still turns green on a target and
    // still pulses. That is the whole reason these are four targeted stubs
    // rather than one suppression of DrawBinoculars.
    //
    //   hideOpticsTint        DoInfraRedQuad -- one untextured quad over the
    //                         whole screen rect, vertex colour 0xFF5050FF. That
    //                         is the transparent red pane behind the laser dot,
    //                         and DrawBinoculars draws it whenever LaserSight is
    //                         set, not only in infra-red mode. Stubbing it takes
    //                         the VCI headset's infra-red tint with it, because
    //                         the engine draws both through this one function.
    //
    // Split in two because "the binoculars look wrong" and "the scope looks
    // wrong" are separate complaints, and the scope's reticle lines are
    // something a player might want to keep after the binocular circles are
    // gone. Both 0 is stock behaviour and patches nothing.
    bool  hideBinocularOverlay = true;
    bool  hideScopeOverlay     = true;
    bool  hideOpticsTint       = true;

    // Which XInput button the left hand's lower face button sends.
    //
    // true  = BACK  -- the System menu. This is the default.
    // false = START -- the pause/inventory menu.
    //
    // inputUpdate() decodes BACK to internal key 0x62 and START to 0x63; which
    // one a given game screen treats as "System" lives in the game DLLs, so this
    // stays switchable.
    bool  gamepadMenuUsesBack = true;

    // --- room culling --------------------------------------------------------
    //
    // The engine draws only what its visibility pass reaches from the GAME
    // CAMERA. Look somewhere that camera is not pointing and the geometry that
    // should be there was never submitted. TR4/5 fix this in PortalCull.cpp by
    // running their room-portal traversal from the tracked head and appending
    // its results. TR6 calculates a private head-visible room list, restores
    // the stock list before simulation continues, then unions the two only in
    // mapDrawRoomList while render data is prepared. Its downstream Calculate
    // calls receive the same head-centred HMD frustum. Turning this off is stock
    // behaviour exactly in all three games.
    //
    // THIS REPLACES PortalHops, DrawAllRooms, PortalHeadTest, PortalHeadMargin,
    // DrawAllRoomsExclude and DrawAllRoomsClip. Those keys are still READ, only
    // so that an old ini gets a log line telling it they are gone rather than
    // silently doing nothing.
    bool  portalCulling         = true;

    // Angle added to each half of the culling frustum, in degrees.
    //
    // Two things need covering and neither is large: culling runs once from the
    // head rather than once per eye, so the couple of degrees a canted display
    // puts between the two frusta has to be allowed for, and the pose that
    // culls a frame is a few milliseconds older than the pose that renders it.
    // Shared by the TR4/5 portal traversal and TR6 Calculate-camera path.
    float cullFovMarginDegrees  = 8.0f;

    // How many doorways deep the traversal may go, and how many portals it may
    // look at in one frame.
    //
    // NEITHER IS A VISIBILITY CRITERION -- the frustum shrinking at every
    // doorway is what stops the traversal, and these only bound the worst case
    // so a pathological level cannot spend the frame in here. If the log ever
    // reports budgets being hit during normal play, raise them.
    int   cullMaxDepth          = 16;
    int   cullMaxPortals        = 4096;

    // Optional distance limit in world units, 0 for none. A sector is 1024.
    //
    // Default off deliberately. The engine's own phd_zfar is 65536 units, which
    // culls nothing, and a room you can see down a long corridor is a room you
    // should be able to see. This exists as a frame-rate lever, not a fix.
    float cullFarUnits          = 0.0f;

    // Widen every listed room's clip rectangle to the whole target. Leave on.
    //
    // This is the old DrawAllRoomsClip, and it is still required for the same
    // reason: the engine's portal-clipped rects are screen boxes computed for
    // the game camera's view, and we render from the HMD's, so under head
    // rotation they scissor the wrong part of the screen.
    bool  cullWidenBounds       = true;

    // Extend the same fix to items.
    //
    // S_GetObjectBounds rejects an item whose bounding box misses the game
    // camera's screen rect or sits behind its near plane, which is every item
    // in every room this feature adds. Without it the added rooms draw with
    // their furniture, enemies and pickups missing -- which is what the hop
    // expansion did, because it never touched this. Answers are only ever
    // promoted from "invisible" to "visible, clip it", never the other way.
    bool  cullObjects           = true;

    // Room indices to watch, and how many were given.
    //
    // Any listed room that the traversal appends is reported once per distinct
    // route, with the doorway it came through and how deep it was. This is the
    // targeted form of the dump key: a room that only misbehaves for a moment
    // is hard to catch with a hotkey, and "does the traversal ever reach 215,
    // and how" is a question that should not need good reflexes to answer.
    //
    // Empty by default. It costs one integer compare per appended room.
    int   cullWatchRooms[16] = {};
    int   cullWatchCount     = 0;

    // Virtual-key code that dumps the current draw list to the log, 0 to
    // disable. Prints which rooms the engine found and which the head frustum
    // added, which is the first thing worth knowing about any culling glitch.
    //
    // A FUNCTION key, deliberately. The numpad is the wrong place for a
    // diagnostic: with Num Lock OFF the physical numpad 3 emits VK_NEXT rather
    // than VK_NUMPAD3, so GetAsyncKeyState(VK_NUMPAD3) never sees the press and
    // the dump silently does nothing. Measured -- a whole test session produced
    // no block for exactly that reason.
    int   cullDumpKey           = 0x77;   // VK_F8

    // Draw the HD sky at optical infinity. 1 = on and the default.
    //
    // DrawSkyHD already zeros the translation of the matrix it draws the dome
    // through, so the dome stays centred on the game camera -- "at infinity"
    // on a monitor. The stereo path then composes the per-eye transform, whose
    // translation is IPD plus any 6DOF head offset, and the dome gets stereo
    // disparity equal to its mesh radius: a painted sphere a few metres away,
    // sitting in front of distant geometry.
    //
    // On: those draws use the rotation of the eye transform only (head look
    // still turns the sky; leaning and IPD do not) and the fragments are
    // pushed to the far plane so they never occlude the world. Off: stock
    // finite-dome stereo, for A/B.
    bool  skyAtInfinity         = true;

    // --- first person (TR4/TR5) -------------------------------------------
    // Startup remains third person; Y+LT toggles the runtime mode.
    bool  firstPerson           = true; // feature enabled; startup view remains third person
    int   firstPersonJoint      = 14;
    int   firstPersonAnchorX    = 0;
    int   firstPersonAnchorY    = -32;
    int   firstPersonAnchorZ    = 144;
    int   firstPersonInteractionAnchorZ = 16;
    bool  firstPersonHeadTranslation = true;
    // Let physical head translation pass through walls without pushback.
    // The base eye anchor and Lara's native movement retain collision checks.
    bool  firstPersonWallCollision = false;
    bool  firstPersonMovementStabilization = true; // stable ground eye + pre-collision root motion
    float firstPersonRoomscaleNeckMetres = 0.15f;
    bool  firstPersonRoomscaleMove = true;
    float firstPersonRoomscaleDeadzoneMetres = 0.02f;
    int   firstPersonRecenterKey = 0x23; // End; RecentreKey remains available too
    bool  firstPersonDriftLog = true;
    bool  firstPersonHideHead   = true;
    bool  firstPersonMoveWithHead = true;
    bool  firstPersonBodyFollowsHead = true;
    float firstPersonBodyDeadzoneDegrees = 0.0f;
    float firstPersonBodyTurnDegreesPerFrame = 4.0f;
    bool  firstPersonHeadAim = true;
    bool  firstPersonMotionGuns = true;
    bool  firstPersonUnarmedIK = true;
    bool  firstPersonFullBodyIK = true; // Headset/controller inferred torso and legs.
    float firstPersonBodyYawSlackDegrees = 45;
    float firstPersonBodyYawSpeedDegrees = 120;
    float firstPersonBodyLeanRadiusMetres = .15f;
    float firstPersonBodyFollowSpeedMetres = .9f;
    float firstPersonBodyHandYawWeight = .15f;
    float firstPersonBodyScale = 0; // 0: calibrated standing fit; 1: original model size.
    bool firstPersonPhysicalCrouch = false; // Optional native Duck from HMD height.
    float firstPersonMotionGunGripForwardMetres = 0.2032f; // eight inches back
    float firstPersonMotionGunRaiseMetres = -0.06985f; // 2.75 inches controller-local down
    float firstPersonMotionGunRightMetres = 0;
    float firstPersonMotionGunPitchDegrees = -30.0f;
    float firstPersonMotionGunYawDegrees = 0;
    float firstPersonMotionGunRollDegrees = 0;
    bool firstPersonMotionGunHotkeys = true;
    bool firstPersonHKScope = true;
    bool firstPersonHKScopeMeshFit = true;
    float firstPersonHKScopeForwardMetres = 0.18f;
    float firstPersonHKScopeUpMetres = 0.24f;
    float firstPersonTurnDegreesPerSecond = 120.0f;
    float firstPersonTurnDeadzone = 0.25f;

    // --- Chest physics for TR4/TR5, from TR6's dynamic bones (DynamicBones.h)
    //
    // This began as a measurement -- run TR6's spring model off TR4/TR5's torso
    // joint and report what comes out, because whether 30 Hz source animation
    // gives a spring anything smooth to chase at headset frame rates was
    // cheaper to answer with a log line than with a shader. The answer was yes,
    // so it is now a feature and ON by default, matching the shipped ini.
    //
    // This is the master switch: it runs the solver AND is what allows the
    // skinning shader to be patched (BoneSkin.cpp). Off means no chest motion
    // at all, whatever dynamicBonesShader and dynamicBonesApply say.
    bool  dynamicBones          = true;

    // Which joint carries the chest. CONFIRMED, not inherited lore: tomb5.dll
    // SkinUseMatrix (RVA 0x00127528) is 14 byte-pairs and only four are
    // populated -- (1,2), (4,5), (8,9), (11,12) -- which are the two knees and
    // two elbows, the four places the skin blends between rigid meshes. That
    // pins the canonical order:
    //
    //   0 HIPS  1 THIGH_R  2 CALF_R  3 FOOT_R  4 THIGH_L  5 CALF_L  6 FOOT_L
    //   7 TORSO  8 UARM_R  9 LARM_R  10 HAND_R  11 UARM_L  12 LARM_L
    //   13 HAND_L  14 HEAD
    int   dynamicBonesTorsoJoint = 7;

    // Where the chest sits ON THE BIND-POSE MODEL, in world units. NOT an
    // offset from the joint origin -- that was the v1 reading and it was
    // wrong.
    //
    // The palette holds SKINNING matrices, bone * inverse-bind, so a
    // translation column is not a bone position. The measured body draw
    // proves it: forearm to hand came out 2.0 units apart and hips to torso
    // 0.5, which is impossible for bone origins. It is the signature of a
    // skinning matrix, because a child bone sharing its parent's orientation
    // produces an identical translation.
    //
    // What such a matrix DOES map correctly is a point in bind-pose model
    // space to its skinned world position, which is exactly what the anchor
    // needs to be. Lara's model origin is at the hips with +Y DOWN, so the
    // chest is around 170 units of NEGATIVE Y, a little forward in Z, and
    // mirrored roughly 34 units either side of centre. Still an estimate --
    // but an estimate in the right space and at the right magnitude, where
    // the v1 values (24, -40, 16) sat barely off the hip origin.
    float dynamicBonesAnchorX   = 34.0f;
    float dynamicBonesAnchorY   = -170.0f;
    float dynamicBonesAnchorZ   = 45.0f;

    // Spring constant and damping. k is omega^2, so 630 is a natural
    // frequency of 4 Hz, and damping 6 is a ratio near 0.12.
    //
    // THE FREQUENCY IS WHAT MAKES IT VISIBLE, NOT THE STRENGTH. The previous
    // 1900 / 22 was a 6.9 Hz flutter at ratio 0.25 that died away in 0.11
    // seconds -- over before the eye registers it, and landing exactly during
    // Lara's own landing-crouch animation, which hid the rest. Logs confirmed
    // every jump was detected and displaced ~14 units; it simply could not be
    // seen. At 4 Hz / 0.12 a running jump rings for about 0.8 s, long enough
    // to read as a bounce.
    //
    // Resting sag is gravity / stiffness, so this raises it from 2.8 to 8.6
    // units. The log's resting L/R Y reads ~8.6 as a result; that is correct.
    //
    // The v1 values (180 / 12) belonged to a world-space position spring and
    // do not carry over. That formulation lagged by c*V/k under constant
    // velocity, which at Lara's running speed was 160 units against an
    // 18-unit clamp -- the reason both measured sessions reported disp_peak
    // pinned at exactly 18.00 and never settling.
    float dynamicBonesStiffness = 630.0f;
    float dynamicBonesDamping   = 9.5f;

    // Along world Y, which is DOWN in TR4/TR5, so positive pulls downward.
    // 5400 is the engine's own gravity expressed per second squared: TR4/TR5
    // accelerate by 6 units per frame at 30 fps, and 6 * 30^2 = 5400. Against
    // the stiffness above that is a resting droop of about 8.6 units.
    float dynamicBonesGravity   = 5400.0f;

    // How much of the parent joint's acceleration the bone feels. 1.0 is the
    // physical answer; lower tames the finite-difference noise that comes
    // from differentiating a joint matrix twice.
    float dynamicBonesDriveScale = 1.0f;

    // Ignore drive accelerations below this, in world units per second
    // squared. Gravity is 5400 for scale.
    //
    // Walking bobs the torso every step and an unfiltered solver answers all
    // of it, so she jiggled while strolling. A jump or a landing accelerates
    // the torso by roughly an order of magnitude more than a footfall, so a
    // threshold separates them. This is the knob to turn if walking still
    // registers (raise it) or jumps stop registering (lower it).
    //
    // Applied to the drive term only. Gravity and the spring are never
    // thresholded: they are what bring the bone back to rest, and gating them
    // would strand it wherever the last jump left it.
    float dynamicBonesDriveDeadzone = 4000.0f;

    // What drives the bone.
    //
    //   1 = engine state (default). Reads Lara's own gravity_status and
    //       fallspeed from the game DLL. Airborne makes the bone weightless,
    //       grounded restores gravity, and a landing adds a kick sized by how
    //       far she fell. Every jump responds, and walking cannot.
    //   0 = acceleration. Differentiates the torso joint twice, then smooths,
    //       ceilings and deadzones the result. Kept for comparison: its
    //       filters compound on short events, so landings registered only
    //       when an impact happened to straddle frame boundaries favourably.
    //
    // DynamicBonesDriveSmoothing, DriveMax and DriveDeadzone below only apply
    // in mode 0.
    int   dynamicBonesDriveMode = 1;

    // Fraction of gravity the bone feels while Lara is airborne, engine mode.
    //
    // 1 holds the chest still for the whole jump, so it only answers the
    // landing. 0 is the physical answer -- in freefall the torso falls with the
    // chest, so the chest goes weightless and rises off its sag by about 15
    // units on the way up. That rise reads as the chest moving as she jumps,
    // which is not wanted here, so the default trades the physics for the look.
    float dynamicBonesAirGravity = 1.0f;

    // Landing kick, engine mode: the bone gets fallspeed * 30 * this, in units
    // per second, downward. fallspeed is per game tick and the game ticks at
    // 30 Hz. The deepest fallspeed of the jump is used, not the one on the
    // landing tick, because the engine zeroes it as she lands.
    //
    // Simulated through this solver at the default 4 Hz tuning, the bone's
    // absolute position reaches about +18 units for a small hop, +23 standing,
    // +28 running and +34 for a long fall (fallspeed 140), all inside the
    // 40-unit MaxDisplace clamp; only extreme falls touch it. Takeoff lifts it
    // to about -6 on every jump. Raise this for more bounce on landing.
    float dynamicBonesLandImpulse = 0.2f;

    // Low-pass on the acceleration estimate, 0..1. Lower is smoother.
    //
    // Position differentiated twice is amplified by 1/dt^2, about 2900 at
    // these frame rates, so 128 units of anchor movement in one frame -- an
    // ordinary measured value -- arrives as roughly 370,000 units per second
    // squared. Raw, that is all spike and no signal. A jump lasts tens of
    // frames and survives this filter easily; single-frame noise does not.
    float dynamicBonesDriveSmoothing = 0.25f;

    // Hard ceiling on the drive acceleration, world units per second squared.
    //
    // 20000 is not arbitrary. One frame at the ceiling gives the bone
    // dv = a*dt, and a spring of this stiffness answers that with a peak of
    // dv/sqrt(k) -- so 54000 produced 22.9 units against an 18-unit clamp and
    // guaranteed saturation on its own, which is exactly what the logs showed.
    // 20000 peaks near 8.5 units and leaves the clamp as a genuine backstop
    // rather than the normal operating point.
    float dynamicBonesDriveMax  = 20000.0f;

    // Which directions the bone may move in.
    //
    //   0 = free. All three axes, the physically complete answer.
    //   1 = world vertical only, and the default.
    //
    // Free looks right for jumping and wrong for turning: the anchor sits off
    // centre and forward of the joint origin, so rotating the torso swings it
    // through an arc, and an arc is acceleration. In testing that threw the
    // whole upper body sideways whenever the stick turned her.
    //
    // Vertical-only keeps the jump response, which is what reads as the
    // effect, and discards the rotational sloshing that does not. World
    // vertical rather than the joint's own up, so it stays true while Lara
    // leans.
    int   dynamicBonesAxis      = 1;

    // How the solved motion reaches the screen.
    //
    //   1 = per vertex, in the skinning shader (default). Only the front of the
    //       chest moves; back, backpack, shoulders and armpits stay exactly
    //       where the animation put them. Stock build only.
    //   0 = the whole TORSO joint. Moves the back and backpack with the chest
    //       and stretches the shoulder seams. Kept as the fallback, and used
    //       automatically if the shader path cannot start -- the log says why.
    //
    // A joint transform moves every vertex attached to it by the same amount,
    // and has no idea which of them are the front; that is why 0 cannot be
    // tuned into 1. See BoneSkin.h.
    int   dynamicBonesShader    = 1;

    // Which way is the front of the torso in its own coordinates. 0 = work it
    // out from Lara's facing angle (default); 1 or -1 forces +Z or -Z. Only
    // worth forcing if the log reports the back bouncing instead of the chest.
    int   dynamicBonesForwardSign = 0;

    // How strongly the chest moves on the per-vertex path, as a multiple of the
    // solved motion. Shader path only.
    //
    // 1.0 moves the bust exactly as far as the whole-torso view moved the whole
    // upper body. That reads as much less, because so much less of her is
    // moving, so the default overstates it. Simulated first-bounce sizes at the
    // default spring and this value: about 16 units for a small hop, 28 for a
    // standing jump, 40 for a running one and 57 for a long fall, with the
    // second bounce under a third of the first. Raise for more; lower if it
    // looks rubbery.
    //
    // DynamicBonesMaxDisplace clamps the solver BEFORE this multiplies, so
    // raising this never changes where that clamp bites -- only falls past
    // about fallspeed 220 reach it, and they scale with everything else.
    float dynamicBonesChestStrength = 2.5f;

    // The chest region, measured from the torso mesh. Shader path only.
    //
    // HEIGHT IS NORMALLY MEASURED, NOT SET: the band is fitted to the bump the
    // bust makes in the mesh's front profile. Top and Bottom below are only the
    // fallback, used when an outfit shows no distinct bump -- fractions of the
    // torso height from the neck end downward. (They were the primary method
    // first, and 0.18..0.52 proved a quarter of the torso too high for this
    // mesh, which is why the bust barely moved.)
    float dynamicBonesChestTop    = 0.18f;
    float dynamicBonesChestBottom = 0.52f;
    // Depth: where the weight starts, as a fraction of torso depth behind the
    // front surface. 0.5 is the middle of the torso, so everything behind it --
    // the back and the backpack -- is untouched. Lower keeps the effect closer
    // to the surface.
    float dynamicBonesChestDepth  = 0.6f;
    // Width: half-width, as a fraction of torso width, before the weight fades
    // toward the armpits. Lower keeps the shoulders stiller.
    float dynamicBonesChestWidth  = 0.34f;

    // How much of the bust the fitted height band keeps, as a fraction of the
    // bump it makes in the mesh's front profile. The band is every height
    // staying within this much of the peak, so larger reaches further toward
    // the upper chest and the belly.
    float dynamicBonesChestBand   = 0.65f;

    // Push the selected region straight out of her front by this many units,
    // permanently. 0 = off. A calibration aid: set 30 or so, stand still, and
    // what bulges is exactly what will bounce.
    float dynamicBonesRegionDebug = 0.0f;

    // Put the solved displacement into the joint palette.
    //
    // THE FALLBACK, IGNORED WHENEVER THE PER-VERTEX PATH IS LIVE: DynamicBones
    // .cpp returns early on BoneSkinActive(), because doing both would move the
    // chest twice. It reaches the screen when dynamicBonesShader is 0, or when
    // the shader path cannot start on this build. Joint 7 is TORSO, so
    // everything weighted to it moves and the whole upper body wobbles instead
    // of just the chest -- coarse, but it also makes the solver visible by eye.
    bool  dynamicBonesApply     = true;

    // Overall multiplier on the solved motion, applied to BOTH paths -- the
    // whole-torso view and the per-vertex chest, where it stacks with
    // DynamicBonesChestStrength. 1.0 is the true amplitude and the value tuned
    // in-headset.
    //
    // This defaulted to 4 when the whole-torso view was only a debug aid and
    // needed exaggerating to be seen at all. Left at 4 it would have multiplied
    // the chest path too, handing a fresh install 4 x 1.5 = 6 times the tuned
    // strength.
    float dynamicBonesDebugScale = 1.0f;

    // How far apart two joint origins must be, in world units, to count as
    // separate joints when scoring which draw carries the body.
    //
    // This started at 0.05 and that was far too tight: padding slots differing
    // by a fraction of a unit each scored as their own joint, so a 33-slot
    // palette holding 13 identical fillers rated "21 distinct" and outranked a
    // real 15-of-15 skeleton. Bones on a body are tens of units apart.
    float dynamicBonesSeparation = 12.0f;

    // Displacement clamp, world units. TR6 bounds its bones with authored
    // deflector volumes and SpringSystem::collide; a radius is the honest
    // stand-in for a game whose assets carry none of that.
    //
    // Sized to the stiffness. At the 4 Hz tuning a running jump reaches +28
    // and a long fall +34, so the old 18 would have clipped even a small hop
    // (which peaks at exactly 18). 40 covers everything short of an extreme
    // fall, which is where a backstop belongs.
    float dynamicBonesMaxDisplace = 40.0f;

    // Anchor movement in a single frame beyond which the solver snaps instead
    // of integrating -- the job SpringSystem::teleport does in TR6. A level
    // load, a cutscene cut or a camera warp is not an acceleration, and a
    // spring that treats it as one flings the bone and spends a second
    // reeling it back.
    float dynamicBonesTeleport  = 900.0f;

    // How many measured frames between report lines.
    int   dynamicBonesReportFrames = 900;

    // Dump every joint's translation once per report interval. This is how
    // the torso index gets confirmed rather than assumed. Verbose.
    bool  dynamicBonesLogJoints = false;

    // Log the DLL-side return address of each distinct call into vid_setPass and
    // ogl_drawVB, as "module+RVA".
    //
    // The game DLLs ship without PDBs, so this is how we locate their render
    // code without searching 1791 unnamed functions: the DLL has to call across
    // into the engine to draw, and the return address at our hook is a code
    // address inside the DLL. One gameplay frame gives the exact RVAs to open in
    // Ghidra. Deduplicated and capped, but still verbose -- leave off for play.
    bool  logCallsites     = false;

    // Frame-graph tracer. Logs every render-target transition and how many
    // world-space vs 2D draws happen against each, for TraceFrames frames
    // starting at TraceStartFrame.
    //
    // This is how you find out where the 3D scene is actually drawn. Stereo
    // splits draws into two halves of one target and redirects FBO_default, and
    // both of those are only correct if the scene really is rendered to the
    // backbuffer rather than into one of the engine's offscreen array textures
    // and composited afterwards. Measure before assuming.
    int   traceFrames      = 0;

    // Virtual-key code that triggers a capture, so the trace can be taken
    // during actual gameplay instead of at a guessed frame number. Default
    // 0x79 = F10. Set to 0 to disable and use traceStartFrame instead.
    int   traceKey         = 0x79;

    // Fallback frame-number trigger. 0 = only the hotkey fires a capture.
    int   traceStartFrame  = 0;

    // Per-draw state dump. The tracer above answers "which target is the scene
    // drawn into"; this answers the other question, the one a mislaid layer
    // needs: of the three matrices that can place an element -- projection,
    // view, model -- which one carries the difference between one element and
    // the next? Dump them per draw with the screen in question up, and the
    // answer is in the log rather than in a guess.
    //
    // Number of draws to capture when the hotkey is pressed. 0 = disabled.
    // One line per draw, not per eye. 300 covers a menu frame comfortably.
    int   dumpDraws        = 0;

    // Virtual-key code that arms the dump. 0x78 = F9.
    int   dumpKey          = 0x78;

    // Split the two independent sources of per-eye difference so they can be
    // tested apart. Both default on; turning either off is a diagnostic.
    //
    //   parallax  -- the +-13.4 unit view offset. THIS is depth.
    //   shear     -- proj[8] = -+0.2425, the asymmetric frustum. This is a
    //                constant sideways shift and carries NO depth at all.
    //
    // Every measurement so far confirms only that the halves DIFFER, which the
    // shear alone is enough to cause. Isolating them says which one is real:
    //
    //   PerEyeView=1 PerEyeProjection=0 -> if depth appears, the shear was
    //       fighting the parallax, and that configuration is the fix.
    //   PerEyeView=0 PerEyeProjection=1 -> if the halves still differ, the
    //       difference is pure shear and parallax is not reaching the pixels.
    //   PerEyeView=1 PerEyeProjection=1 -> current behaviour.
    bool  perEyeView       = true;

    // 0 = the engine's own projection. NOT a clean control: it also drops the
    //     HMD field of view and feeds a 1.78-aspect frustum into a 0.93-aspect
    //     viewport, so it changes two things at once and proves nothing about
    //     the shear on its own.
    // 1 = the HMD's true asymmetric frustum (correct, and the default).
    // 2 = the HMD's field of view, symmetrised: same total extent per axis,
    //     shear forced to zero. This is the clean single-variable test, because
    //     FOV and aspect stay exactly as in mode 1.
    //
    // Mode 2 exists because the shear contributes a CONSTANT disparity of
    // (r+l)/(r-l) - the pedestal - which for this HMD is 0.485 NDC, about 326 px.
    // The headset optics are meant to cancel it exactly. The depth cue riding on
    // top is only ~32 px at 2000 units. If that cancellation is not happening,
    // the eyes converge on the pedestal and everything collapses to one plane --
    // flat, oversized, and immune to IpdScale.
    int   perEyeProjection = 1;

    // Where the per-eye TRANSLATION is applied.
    //
    //   0 = uViewMatrix translation column (the original).
    //       BROKEN: 38 of the engine's 125 vertex shaders transform position as
    //       dot(uViewMatrix[i].xyz, p.xyz) -- rotation ONLY, never reading the
    //       translation column. For those passes the camera offset is baked into
    //       uModelMatrix on the CPU instead. So a per-eye translation here is
    //       invisible to them: no parallax, no depth, everything at infinity (so
    //       it reads oversized), IpdScale and WorldUnitsPerMetre inert. A per-eye
    //       ROTATION was always visible, which is why a 20-degree yaw test worked
    //       while every translation test failed.
    //
    //   1 = uModelMatrix translation column.
    //       ALSO BROKEN, differently: skinned geometry is transformed by
    //       uJoints[72*3], not uModelMatrix, so characters do not move with the
    //       world and end up floating outside the map. Chasing every transform
    //       path is a losing game.
    //
    //   3 = uProjMatrix composed with the WHOLE per-eye transform, and the view
    //       matrix left completely untouched. THE FIX.
    //
    //       With the game's own view matrix in place both shader families agree
    //       exactly: full-dot gives R_g*w + t_g, and rotation-only gives R_g*p
    //       with p = w + R_g^T*t_g, which is the same R_g*w + t_g. They only
    //       diverge once the view matrix is modified. So don't modify it --
    //       fold the eye transform E = [R_e | t_e] into the projection:
    //           clip = P * (R_e*v + t_e) = (P * E) * v
    //       One multiply, rotation and translation together, correct for both
    //       families by construction.
    //
    //       Mode 2's head-movement blow-up came from d = (R_e - I)*t_g + t_e:
    //       t_g is ~82000 units, so any head rotation made the first term
    //       thousands of units. E is metres-scale, so this cannot happen here.
    //
    //   2 = uProjMatrix, post-multiplied by translate(d). Gives correct depth and
    //       working scale keys, but the world swims when you rotate your head,
    //       for the reason above. Superseded by 3.
    //       Every one of the 125 shaders ends with uProjMatrix * vec4(view, 1.0),
    //       so this is the one matrix nothing can bypass -- joints included. A
    //       view-space shift d is exactly equivalent to P * translate(d):
    //           clip.x += m[0]*dx + m[4]*dy + m[8]*dz
    //       which yields the same delta/depth parallax as shifting the view.
    int   eyeOffsetMode = 3;

    // Yaw the RIGHT eye's view by this many degrees. Diagnostic only.
    //
    // Every link from the injection to the draw has been verified correct, and
    // the headset still shows no parallax. One of those verifications must be
    // wrong, so this stops arguing about a 26-unit translation and applies an
    // effect that cannot be subtle or misjudged: at 20 degrees the two halves
    // are looking in visibly different directions.
    //
    //   halves differ wildly -> the view matrix DOES drive rendering, and the
    //       fault is specific to the translation column.
    //   halves stay identical -> the view matrix reaches the GPU (proved by
    //       glGetUniformfv) but does not affect the draw at all, and the whole
    //       injection point is wrong however correct it reads.
    float debugEyeYawDegrees = 0.0f;

    // Burn a red bar into the left half of the eye target and a blue bar into
    // the right half. A direct eye-mapping check for when every measurable
    // thing upstream reads correct and the headset still shows mono. See
    // StereoRenderer::MarkEyes for how to read the result.
    bool  eyeMarkers       = false;
};

const Config& Cfg();
void LoadConfig(const wchar_t* iniPath);
const motiongun::Calibration& LiveMotionGunCalibration();
void AdjustMotionGunCalibration(int command);
bool SaveMotionGunCalibration();
void RestoreMotionGunCalibration();

// --- live tuning ------------------------------------------------------------
// Scale is a perceptual judgement, so it is adjustable in the headset rather
// than through an edit-restart cycle. These start from the ini values.
float LiveWorldUnitsPerMetre();
float LiveIpdScale();
void  AdjustWorldScale(float factor);   // multiplicative, e.g. 1.05f
void  AdjustIpdScale(float factor);
void  LogTuning(const char* why);

// Log any ini option that the current EyeOffsetMode silently ignores.
// Write the stock ini to `path` if nothing is there yet.
//
// Returns true only when a file was actually created. A fresh install then
// starts from the documented template rather than bare struct defaults, which
// matters because the comments in that template carry most of what was learned
// tuning this -- which settings are load-bearing, which were measured useless,
// and why.
//
// Never overwrites: an existing ini is somebody's tuned setup.
bool  EnsureConfigFile(const wchar_t* path);

void  WarnIgnoredOptions();
void  ResetTuning();

} // namespace tr
