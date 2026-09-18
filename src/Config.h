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

namespace tr {

struct Config {
    bool  enabled          = true;

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
    bool  ceilingClearance    = true;

    // How close the eye may get to the ceiling, in world units. 128 is about
    // 0.30 m at the default scale. Raise it if you still clip, lower it if the
    // cap feels like it arrives too early.
    float ceilingMarginUnits  = 128.0f;

    // --- room culling --------------------------------------------------------
    //
    // The engine draws only the rooms its portal traversal reaches from the
    // GAME CAMERA. Look somewhere the game camera is not pointing and the
    // geometry that should be there was never submitted. PortalCull.cpp runs
    // the same traversal from the tracked head instead, with the headset's
    // frustum, and appends whatever it finds; nothing the engine listed is ever
    // removed, so turning this off returns the stock behaviour exactly.
    //
    // Off means the engine's culling stands and rooms will vanish as you turn
    // your head. It is on because that is the whole point; it is a toggle
    // because it is the only thing here that writes into the game DLL's own
    // draw list.
    bool  portalCulling         = true;

    // Angle added to each half of the culling frustum, in degrees.
    //
    // Two things need covering and neither is large: the traversal runs once
    // from the head rather than once per eye, so the couple of degrees a canted
    // display puts between the two frusta has to be allowed for, and the pose
    // that culls a frame is a few milliseconds older than the pose that renders
    // it. Raise it if geometry pops in at the very edge of vision when you turn
    // quickly; every degree costs a little more draw.
    float cullFovMarginDegrees  = 8.0f;

    // How many doorways deep the traversal may go, and how many portals it may
    // look at in one frame.
    //
    // NEITHER IS A VISIBILITY CRITERION -- the frustum shrinking at every
    // doorway is what stops the traversal, and these only bound the worst case
    // so a pathological level cannot spend the frame in here. If the log ever
    // reports budgets being hit during normal play, raise them; the numbers are
    // not tuned to anything except "far past what a level does".
    int   cullMaxDepth          = 16;
    int   cullMaxPortals        = 4096;

    // Optional distance limit in world units, 0 for none. A sector is 1024.
    //
    // Default off deliberately. The engine's own phd_zfar is 65536 units, which
    // culls nothing, and a room you can see down a long corridor is a room you
    // should be able to see. This exists as a frame-rate lever, not a fix.
    float cullFarUnits          = 0.0f;

    // Widen every listed room's clip rectangle to the whole target.
    //
    // The engine stores a per-room screen rect, clipped at each doorway, and
    // PrintRooms turns it into a scissor box -- so a room reached down a
    // corridor is pixel-clipped to where the GAME CAMERA saw its doorway. The
    // stereo path already overrides the scissor per draw, so on the modern
    // renderer this is belt and braces; on the classic renderer, where the same
    // rect drives vertex clipping, it is what makes the added rooms visible.
    bool  cullWidenBounds       = true;

    // Extend the same fix to items.
    //
    // S_GetObjectBounds rejects an item whose bounding box misses the game
    // camera's screen rect or sits behind its near plane, which is every item
    // in every room this feature adds. Without it, rooms behind you draw with
    // their furniture, enemies and pickups missing. Answers are only ever
    // promoted from "invisible" to "visible, clip it", never the other way.
    bool  cullObjects           = true;

    // Virtual-key code that dumps the current draw list to the log, 0 to
    // disable. Prints which rooms the engine found and which the head frustum
    // added, which is the first thing worth knowing about any culling glitch.
    int   cullDumpKey           = 0;

    // Draw the HD sky at optical infinity. 1 = on and the default.
    //
    // DrawSkyHD already zeros the camera translation of the matrix stack so
    // the sky dome stays centred on the game camera -- "at infinity" on a
    // monitor. The stereo path then composes the eye transform, whose
    // translation is IPD plus any 6DOF head offset, and the dome gets stereo
    // disparity equal to its mesh radius: a painted sphere a few metres away,
    // sitting in front of distant geometry.
    //
    // On: those draws use the rotation of the eye transform only (head look
    // still turns the sky; leaning and IPD do not) and the fragments are
    // pushed to the far plane so they never occlude the world. Off: stock
    // finite-dome stereo, for A/B.
    bool  skyAtInfinity         = true;

    // --- first person -------------------------------------------------------
    // Anchor the scene camera to Lara's animated head instead of the chase
    // camera. Off by default: it is a different game, not a better camera, and
    // it has the comfort profile of a game that was never designed for it --
    // her animations move your head for you.
    bool  firstPerson           = false;

    // Which joint to anchor to, and where inside it. Joint 14 is Lara's head
    // in all three games. The offset is relative to the joint's neck pivot, in
    // world units, in the joint's own frame: -Y is up, +Z is forward, so the
    // default sits a little above and in front of the pivot -- roughly where
    // her eyes are rather than in her throat.
    int   firstPersonJoint      = 14;
    int   firstPersonAnchorX    = 0;
    int   firstPersonAnchorY    = -32;
    int   firstPersonAnchorZ    = 16;

    // Where the view's heading comes from. Off (default) keeps the engine's own
    // render camera yaw, which is what the right stick steers -- so the stick
    // keeps working and the view settles behind Lara as the chase camera does.
    // On takes Lara's body yaw instead: rigidly her heading, but under modern
    // controls the right stick only orbits the camera, so it would appear dead.
    bool  firstPersonYawFromLara = false;

    // Let the headset's tracked POSITION move the first-person viewpoint, on top
    // of Lara's head. Off by default, because it adds your real head's offset
    // from the tracking origin to hers -- if that origin is stale or at floor
    // level you float above her, which is exactly what it looks like. Rotation
    // is always tracked; this is only the translation.
    bool  firstPersonHeadTranslation = false;

    // Hide Lara's head while first person is anchoring, by clearing its
    // mesh_bits bit -- the same mechanism the engine uses to hide a body part.
    // Her body stays drawn, so looking down still shows her.
    bool  firstPersonHideHead = true;

    // Rotation-only head tracking. The safest possible first test: the camera
    // can pivot but can never be displaced into geometry, so a wrong world
    // scale cannot put you inside a wall. Turn positional on once looking
    // around behaves.
    bool  positionalTracking = false;

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
    // The path never fired in TR4 or TR5, but TR1-3 does call vid_setOrtho3D
    // (a real 78-byte function at RVA 0x0000A590), so it is expected to fire
    // here. The ortho3D= counter in the health report says whether it does.
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

    // NOTE: the TR4-6 mod carried AlternateEyeGame6 / AlternateEyeMinOffscreen /
    // VideoSkipGame6 here. They are deliberately absent.
    //
    // They existed for Angel of Darkness, which rendered its scene into the
    // engine's own offscreen textures and composited at the end, so per-draw
    // duplication could not reach it and each FRAME had to be one eye at half
    // the per-eye rate. TR1, TR2 and TR3 all draw straight to the backbuffer,
    // so ordinary per-draw duplication reaches everything and both eyes update
    // at full rate. There is nothing for those options to select.

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
    // RIGHT grip synthesises XB_X | XB_RIGHT_SHOULDER for Walk -- so this chord
    // is right grip + right trigger, which reads as "walk and shoot". That is a
    // combination people genuinely use, and it will engage the chord. Set this
    // to 0 if you would rather walk-and-shoot left pitch alone.
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

    // Which XInput button the left hand's lower face button sends.
    //
    // true  = BACK  -- the System menu. This is the default.
    // false = START -- the pause/inventory menu.
    //
    // inputUpdate() decodes BACK to internal key 0x62 and START to 0x63; which
    // one a given game screen treats as "System" lives in the game DLLs, so this
    // stays switchable.
    bool  gamepadMenuUsesBack = true;

    // NOTE: the TR4-6 mod carried the portal/room-culling options here
    // (PortalHops, DrawAllRooms, DrawAllRoomsExclude, PortalHeadTest,
    // PortalHeadMargin, DrawAllRoomsClipRect, RoomDumpKey). They are absent.
    //
    // That feature widened the engine's visible set so that geometry did not
    // disappear when you turned your head away from the game camera. The
    // culling lives in the GAME DLL, not the exe, so it was implemented against
    // reverse-engineered addresses inside tomb4.dll.
    //
    // The equivalent work for TR1-3 would mean locating the portal traversal in
    // tomb1.dll, tomb2.dll and tomb3.dll -- three separate DLLs. Those ship
    // PDBs too (PDB	omb1.pdb and friends), so it is a tractable follow-up
    // rather than a research problem, but it is not done, and shipping dead
    // options that silently do nothing would be worse than not shipping them.
    //
    // Until then, geometry outside the game camera's frustum is not drawn.

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
    int   eyeOffsetMode = 2;

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
