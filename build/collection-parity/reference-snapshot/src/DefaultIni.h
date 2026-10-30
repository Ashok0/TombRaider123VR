// DefaultIni.h -- the stock TombRaiderVR.ini, embedded.
//
// GENERATED FILE. Do not edit by hand: change TombRaiderVR.ini in the repo root
// and run  python tools\\gen_default_ini.py
//
// The DLL writes this out when no ini exists beside it, so a fresh install gets
// the documented, working configuration rather than bare struct defaults -- the
// comments in the template carry most of what was learned tuning this thing,
// and a generated key=value dump would throw all of it away.
//
// Source: TombRaiderVR.ini, 60029 bytes, 1194 lines.
#pragma once

namespace tr {

// Newlines are LF here; the writer expands them to CRLF on the way out.
// Split into adjacent literals only because MSVC caps one at 16380 bytes;
// the concatenation is a single continuous string and the seams carry no bytes.
inline const char* DefaultIniText() {
    return R"INI(; TombRaiderVR.ini -- place next to TombRaiderVR.dll
;
; If the very first run looks wrong, the fix is almost always one of the two
; sign toggles below. The engine renders Y-flipped (ogl_setPerspAngles writes
; e11 = -1/tanY) and TR world space is Y-down, while OpenVR is Y-up. That is two
; independent flips, so they are exposed rather than hard-coded.
;
;   image upside down in the headset ....... FlipProjectionY
;   world tilts the wrong way when you lean  FlipViewY
;   depth inverted / eyes crossed .......... SwapEyes

[VR]
Enabled=1

; Mode = mono | stereo
;
; mono is the bring-up mode. It composes the head pose onto the game camera and
; renders normally to the monitor: no stereo target, no draw duplication, no
; compositor submission, and the engine keeps its own projection so the field of
; view is unchanged. OpenVR runs as a Background app, so SteamVR just needs to be
; running and tracking -- the game does not become the VR scene application.
;
; Prove tracking, handedness and world scale here first, then switch to stereo.
Mode=stereo

; Rotation-only head tracking. The safe first test: the camera can pivot but
; never be displaced, so a wrong WorldUnitsPerMetre cannot put you inside a wall.
; Set to 1 once looking around behaves correctly.
;
; At 1 the ANALOG STICK still behaves as though this were 0 -- rotating the view
; does not displace your eye at all. Only the headset does. See HeadOffsetFrame.
PositionalTracking=1

; Seated tracking origin rather than standing. Standing space is absolute room
; coordinates -- your head at ~1.6 m becomes ~680 TR units of camera offset
; before you have moved at all. Leave this at 1.
SeatedOrigin=1

; HeadOffsetFrame = world | camera
;
; Where your head's DISPLACEMENT from the game camera lives. world is the default
; and is what makes PositionalTracking=1 behave like 1 for the HEADSET and like 0
; for the ANALOG STICK.
;
; camera is the old behaviour: the offset is carried in the game camera's frame,
; so the camera ROTATES it. Sit perfectly still, turn with the look stick, and
; your viewpoint travels on an arc of radius |offset| -- 0.4 m of lean is 338
; units of sideways travel over a 180-degree turn, straight into walls the game's
; own camera collision thinks are clear. Camera pitch does it vertically, turning
; a forward lean into rise and fall.
;
; world keeps the offset in world units and moves it only by what the HEADSET did:
;
;   offsetWorld += R_camera^T * (thisFrame - lastFrame)
;
; Turn with the stick and nothing moves, because your head did not move. Move your
; head and you get the full 6DOF displacement, applied in the direction you are
; facing at the moment you move -- so leaning forward always goes into the screen.
; Camera translation still carries you along, exactly as it does with positional
; tracking off.
;
; THE COST, and it is real: the offset becomes accumulated state, so it can drift
; away from your physical centre. Lean 0.5 m out, turn 90 degrees with the stick,
; lean back to centre, and your eye sits 0.71 m from the camera -- the two legs
; cancelled in different world directions. RecentreKey below clears it, and a
; camera JUMP re-anchors automatically, which covers level loads, cutscene cuts
; and flybys.
HeadOffsetFrame=world

; Re-anchor the head offset to the game camera. Numpad 5 -- the middle of the
; tuning cluster, and the only key in it that was free. 0 disables it.
;
; Press it when the world-locked offset has drifted and you want your physical
; centre to mean the camera again. It never jumps the view by more than the drift
; it is removing, and it logs:
;   vr: head offset re-anchored to the game camera
RecentreKey=0x65

; Stop the tracked head rising through the ceiling in low tunnels and
; crawlspaces. 1 = on and the default; 0 = the stock uncapped head.
;
; The game camera already sits near the ceiling in those places, and the
; positional offset pushes the eye straight through it.
;
; This CLAMPS, it never moves you. The head's height is capped so the eye stays
; CeilingMarginUnits below the room's ceiling, and everything below that cap
; behaves exactly as before -- ducking, leaning and every rotation pass through
; untouched. That is deliberate: a camera that drops to torso height on its own
; would be unrequested vertical motion, which is a comfort problem, and a clamp
; can only ever subtract motion you would have had.
;
; The headroom comes from the camera room's BOUNDING BOX, so it is exact in a
; uniformly low room -- which is where a head clips through in the first place
; -- and over-generous in a low pocket off a tall hall, where the clamp simply
; does not engage. Wrong in the safe direction: it can fail to clamp, but it can
; never clamp somewhere roomy.
;
; With HeadOffsetFrame=world this clamps the OFFSET THAT IS USED rather than your
; tracking-space height -- which was only the same quantity while the camera had
; no pitch. It is applied on the way out and leaves the accumulated offset alone,
; so walking into a taller room gives your real height straight back.
;
; Logs once the first time it bites:
;   vr: ceiling clamp active -- headroom 892 units, eye held 764 units above ...
CeilingClearance=1

; How close the eye may get to the ceiling, in world units. 128 is about 0.30 m
; at the default scale. Raise it if you still clip through, lower it if the cap
; feels like it arrives too early.
CeilingMarginUnits=128

; TR world units per metre -- the main scale control.
;
; RAISE IT if the world feels too big and depth feels flat. Those are the same
; symptom: your IPD is fixed, so a world rendered too large leaves the eye
; separation proportionally too small, which reads as both "enormous" and
; "no depth". Lowering it grows the world and weakens depth.
;
; 423 comes from Lara's 762-unit height at ~1.8 m, which is a starting point,
; not gospel. Tune it live with the hotkeys below and copy back what feels right.
;
; NOTE: "too big and flat" was ALSO the signature of the uViewMatrix translation
; bug -- see EyeOffsetMode below. Rule that out before chasing this number.
WorldUnitsPerMetre=423

; Multiplier on eye separation only, leaving world size alone. Use this only if
; the size feels right but depth still reads flat. 1.0 = physically honest.
IpdScale=1.0

; Live tuning hotkeys, logged so you can copy the final value back here.
;   numpad +      SMALLER world, STRONGER depth   (raises WorldUnitsPerMetre)
;   numpad -      BIGGER world, WEAKER depth
;   numpad * / /  IpdScale up / down
;   numpad 0      reset both to the values in this file
;
; Step per press. 1.25 = 25%, so three presses roughly double. Scale differences
; are hard to judge below about 2x, which is why this is not a fine adjustment.
ScaleStep=1.25
ScaleUpKey=0x6B
ScaleDownKey=0x6D
IpdUpKey=0x6A
IpdDownKey=0x6F
ResetTuningKey=0x60

; 0 = use the size OpenVR recommends.
EyeWidth=0
EyeHeight=0
SuperSample=1.0

FlipProjectionY=1
FlipViewY=1
SwapEyes=0

; Vertically flip the eye texture when handing it to the compositor.
; Leave at 0. The engine's Y-negated projection already cancels TR's Y-down
; world, so the eye texture is an ordinary GL render and the compositor wants
; default V bounds.
;   upside down in the HMD but fine on the monitor .. set this to 1
;   upside down in BOTH ......................... change FlipProjectionY instead
FlipSubmitV=0

; --- per-eye rendering ------------------------------------------------------

; Where the per-eye TRANSLATION is applied. Leave this at 3.
;
;   0 = uViewMatrix translation column. BROKEN: 38 of the engine's 125 vertex
;       shaders transform position as dot(uViewMatrix[i].xyz, p.xyz) -- rotation
;       ONLY, never reading the translation column. Invisible to them, so: no
;       parallax, no depth, everything at infinity (reads oversized), and dead
;       IpdScale / WorldUnitsPerMetre keys.
;   1 = uModelMatrix translation column. ALSO BROKEN: skinned geometry goes
)INI"
           R"INI(;       through uJoints[72*3], not uModelMatrix, so characters do not move with
;       the world and float outside the map.
;   2 = uProjMatrix + translate(d). Depth and scale work, but the world swims
;       when you rotate your head: d = (R_e - I)*t_g + t_e, and t_g is ~82000
;       units, so any head rotation makes the first term thousands of units.
;   3 = uProjMatrix composed with the WHOLE eye transform, view matrix left
;       untouched. THE FIX. Untouched, both shader families agree on the
;       view-space position; E is metres-scale so head rotation cannot blow up.
EyeOffsetMode=3

; Apply the per-eye view matrix at all. 0 leaves the game's own camera in both
; eyes, so the only remaining difference is the frustum shear -- a constant
; sideways shift that carries no depth. Diagnostic only.
PerEyeView=1

; Per-eye projection:
;   0 = the engine's own. NOT a clean control -- it also drops the HMD field of
;       view and squeezes a 1.78-aspect frustum into a 0.93-aspect viewport, so
;       it changes two things at once.
;   1 = the HMD's true asymmetric frustum. Correct. Leave it here.
;   2 = HMD field of view, symmetrised: same total extent per axis, shear forced
;       to zero. Single-variable test for the shear. Expect double vision -- the
;       headset optics assume the asymmetric render and misplace each eye by
;       ~15 degrees without it.
PerEyeProjection=1

; --- diagnostics (all off) --------------------------------------------------

; Red bar burned into the left half of the eye target, blue into the right.
; Reads eye mapping directly:
;   red left, blue right ... halves map to eyes correctly
;   both bars in both eyes . submit bounds are being ignored
;   same colour both eyes .. both eyes are getting the same half
;   no bars at all ......... what you see is not this texture
EyeMarkers=0

; Yaw the RIGHT eye's view by this many degrees. 0 = off.
; Tests whether the view matrix drives rendering at all, with an effect far too
; large to mistake for subtlety. At 20 the two eyes visibly look in different
; directions. Rotation is honoured by every shader; translation is not.
DebugEyeYawDegrees=0

; Leave the 2D layer (HUD, menus, subtitles, fades) on the flat path. It is
; still drawn into both eyes, just without per-eye matrices.
FlatHud=1

; Distance in metres at which the flat 2D panel sits. Raise it to push the HUD
; and menus further away.
;
; 0 = leave the layer exactly as the engine drew it, which double-visions: the
; 2D layer uses the engine's ortho projection, identical in both eyes, but the
; headset optics apply a fixed ~15-degree per-eye correction assuming an
; asymmetric render, so an unshifted image gets pulled apart.
HudDepthMetres=4.0

; Horizontal angular width of the 2D panel in degrees, as seen from the game
; camera. The engine's 2D layer fills the flat screen; across the headset's ~94
; degree field of view that would be overwhelming, so it goes on a panel this
; wide instead. Vertical follows the screen aspect. Raise for a bigger panel,
; lower for a smaller one. Ignored when HudLockToHead=1.
HudSizeDegrees=55.0

; 0 = the 2D layer is a quad fixed in the GAME camera's frame, so it stays put
;     in the world while your head turns. This is what you want.
; 1 = old behaviour: welded to your head, swings with every rotation.
HudLockToHead=0

; Negate Y when lifting the 2D layer onto the panel. Leave at 1.
; The engine's ortho already flips Y (TR 2D space is Y-down), and the per-eye
; projection expects Y-DOWN input, so without this the layer is flipped twice.
; That inverts it AND reverses triangle winding, so backface culling removes the
; whole HUD -- leaving only elements drawn with culling off, upside down.
HudFlipY=1

; --- placement carried in the projection ------------------------------------

; Carry the engine's own projection OFFSET through the per-eye substitution.
;
; vid_setPerspOffset writes e02/e12 -- the two shear terms -- and nothing else.
; A shear of s shifts the image by -s at every depth, so it is how the engine
; places a draw on screen without moving its geometry.
;
; The inventory is laid out entirely with it. Measured, one frame: nine item
; draws, each with an identity model matrix, an identity view matrix and one
; joint, differing ONLY in the shear -- 2.0250, 1.3500, 0.6750, 0.0000, -0.6750,
; -1.3500, evenly spaced 0.675 apart. That spacing IS the horizontal bar, and
; the values past +/-1 are the items scrolled off the sides.
;
; Writing the eye frustum over the top discarded all of it, so every item got
; the same shear and landed in the same place. That was the stacked inventory.
;
; The offset is re-applied as a SKEW of the engine's camera space, not as a
; shift of the headset's NDC. A projection carrying shear s is exactly the same
; projection without it applied to space pre-skewed by (s/scale)*z, so the skew
; reproduces the engine's placement whatever else we have done to the matrix --
; and it has to go innermost, after the per-eye transform, because the engine's
; shear multiplies the ENGINE's view-space z.
;
; Both details were got wrong first time round and both were visible. Adding the
; shear onto the eye frustum instead put it on the far side of the head
; transform, so the offset swung with head orientation and the row sheared as
; you looked around; and it matched the NDC coordinate rather than the ANGLE,
; which matters because the frustums differ -- measured, engine tanX 1.119 vs
; eye 1.108, but engine tanY 0.629 vs eye 1.197. The row came out right to 1%
; while the vertical placement was thrown 1.9x too far, into the lens
; distortion. That was the fishbowl.
;
; ogl_setPersp and ogl_setPerspAngles both zero the shear explicitly, so this
; adds exactly zero during gameplay. 0 = old behaviour, for A/B.
PreserveProjOffset=1

; Trim on the whole offset. 1.0 reproduces the engine's layout exactly, so the
; inventory row spans the same ANGLE it does flat -- and on a 96 degree game
; frustum that is a wide row to sweep your eyes across in a headset. Lower it to
; pull the items in toward the centre (0.7 is a reasonable first try); 0
; collapses them back into one stack, which is what the bug looked like.
ProjOffsetScale=1.0

; --- the ortho-3D layer (the inventory) -------------------------------------

; vid_setOrtho3D copies mProj[0] -- the ORTHO matrix -- into mProj[1] and
; repoints vid_state.proj at it, so by pointer alone (which is all the world/2D
; test has) such a pass looks world-space. Handing it a perspective frustum
; would divide an ortho layout by a depth it was never built for.
;
; NOT what caused the stacked inventory -- that was PreserveProjOffset above.
; This path has never been observed to fire in TR4 or TR5; the projoffset= and
; ortho3D= counters in the health report say whether it ever does.
;
; 1 = also check the matrix itself (e32/e33 tell ortho from perspective apart)
;     and keep the engine's own projection for those passes.
; 0 = old behaviour, for A/B.
Ortho3D=1

; Distance in metres at which the ortho-3D layer converges. 0 = no shift at all:
; the layer is left exactly as the engine drew it, which still fixes the
; stacking but double-visions the way an untouched HUD does.
Ortho3DDepthMetres=2.0

; 1 = head-locked convergence, and the default HERE even though the HUD defaults
;     the other way. An ortho projection has no perspective divide, so shifting
;     it is pure convergence: relative x, y AND z all survive untouched. The
;     HUD's world-locked panel discards its z input, which is harmless for flat
;     overlays and wrong for these -- they are meshes with real depth, and
;     flattening them makes every item z-fight with itself.
; 0 = world-locked panel, same construction as the HUD's but with the z column
;     filled in so ortho depth lands in a slab instead of on one plane.
Ortho3DLockToHead=1

; Horizontal angular width of the world-locked panel, in degrees. Ignored when
; Ortho3DLockToHead=1.
)INI"
           R"INI(Ortho3DSizeDegrees=55.0

; Thickness of the world-locked panel's depth slab, in metres: ortho depth maps
; onto Ortho3DDepthMetres plus or minus half of this. Too thin and the item
; meshes z-fight; too thick and they visibly separate in depth. Ignored when
; Ortho3DLockToHead=1.
Ortho3DSlabMetres=0.5

; Distance in metres for full-screen passes that bypass uProjMatrix entirely.
; Five of the engine's shaders write gl_Position = vec4(aCoord, 1.0) -- straight
; to clip space, no matrix at all -- and pre-rendered video runs through that
; path. No matrix can move them, so the viewport is shifted per eye instead.
; 0 disables the shift entirely.
VideoDepthMetres=6.0

; 0 = the video panel is anchored to the GAME camera, so it stays put in the
;     world while your head turns, and leaves view if you turn away.
; 1 = welded to your head.
VideoLockToHead=0

; Horizontal angular width of the video panel in degrees, seen from the game
; camera. Also controls how flat it looks: the panel's four corners are
; projected and the viewport fitted to their bounding box, which gives correct
; position and perspective SIZE. What is left is keystone -- an off-axis quad
; should go trapezoidal and a bounding box stays rectangular -- and that error
; grows with width. 50-70 is a good cinema size; above ~90 it shows at the edges.
VideoSizeDegrees=60.0

; 1 = capture the video pass into an offscreen texture and replay it as a REAL
;     quad. Removes keystone entirely: it foreshortens, keystones and rolls like
;     the world does, because it is world geometry. Costs one framebuffer bind
;     and two quad draws per video frame, and only during video -- the branch
;     never runs in gameplay. It also rasterises the video once instead of
;     twice, so it nearly pays for itself.
; 0 = viewport fit (corner bounding box). Correct position and size, residual
;     keystone off-axis.
VideoOffscreen=1

; Flip the captured video vertically on replay. Only if it comes out upside down.
VideoFlipV=0

; In TR6, use the offscreen video panel ONLY while a video is actually playing.
; Leave at 1.
;
; TR6 composites its offscreen scene to the backbuffer through a
; clip-space-direct shader -- the same signature as an FMV quad -- so the shader
; test alone would capture the whole game and turn it into a floating panel.
; Hooking fmvShow gives an exact "a video is on screen this frame" signal, so
; TR6's cutscenes get the real-geometry panel (no keystone, no tilt on head
; roll) while its gameplay and menus are left alone.
VideoSkipGame6=1

; Native stereo for TR6 (Angel of Darkness). Leave at 1.
;
; TR4/TR5 draw straight to the backbuffer, so every world draw can be issued
; twice into the two halves of one double-wide target. TR6 cannot work that way:
; it renders the scene into the engine's own 2560x1440 offscreen textures and
; composites at the end, and the composite samples those whole with 0-1 UVs.
; Measured: 572970 of 579945 world draws offscreen, against ~9% in TR4/5.
;
; The stock tomb6.dll's debug symbols expose App_Render_Scene as a complete,
; render-only boundary. The mod runs that entire offscreen/postprocess chain
; once for each eye, copying each finished composite into its half of the VR
; target. Input, physics, audio and game state still advance only once.
;
; The hook is byte- and build-checked. An unknown tomb6.dll is never patched;
; AlternateEyeGame6 below is used automatically instead.
NativeStereoGame6=1

; Alternate-eye fallback for TR6. Leave at 1.
;
; Used only when NativeStereoGame6=0 or the exact TR6 render hook is unavailable.
; Each frame is then rendered as one eye, so each eye updates at half rate.
AlternateEyeGame6=1

; How many offscreen world draws a frame needs before the AER fallback engages.
; Native stereo uses the actual App_Render_Scene call and ignores this setting.
;
; The TR6 main menu and the FMVs are 2D -- they draw straight to the backbuffer,
; so ordinary per-draw duplication handles them at full rate. Gameplay runs
; ~800 offscreen world draws a frame; menus and video are near zero.
;
; Raise it if a menu still drops to half rate; lower it if gameplay does not
; engage. 0 forces AER on for every TR6 frame when it is the selected fallback.
AlternateEyeMinOffscreen=50

; --- controllers ------------------------------------------------------------

; Present the Oculus Touch controllers to the game as an Xbox pad.
;
; The game resolves XInput through GetProcAddress into a function-pointer
; global, so there is no import to hook and no virtual-pad driver (ViGEm etc.)
; needed -- we overwrite that pointer. inputUpdate() polls it every frame and
; switches to INPUT_TYPE_XB on the first input, so the game adopts the Xbox
; control scheme and button prompts on its own. A real pad, if plugged in, is
; merged rather than replaced.
;
;   Move    left stick              Jump    A  (right hand, LOWER)
;   Look    right stick             Roll    B  (right hand, UPPER)
;   Action  Y or BOTH GRIPS        System  X  (left hand, LOWER)
;   Walk    left stick + RIGHT GRIP Duck    LEFT GRIP
;   Sneak  RIGHT GRIP + Y
;   Equip   left trigger (hold)     Shoot   right trigger
;   Sprint  left stick click        Photo   both stick clicks (L3+R3)
;
; Walk is the right grip rather than a face button so it can be held while the
; left thumb keeps moving. The game binds Walk to XInput X, so the grip emits
; only X. TR6 binds Sneak to RIGHT_SHOULDER: hold Y with the right grip to emit
; Sneak instead of Walk/Action. Both grips hold Action during gameplay without
; also sending Duck or Walk; Photo Mode keeps its native L3+R3 chord.
GamepadEnabled=1

; Log the raw OpenVR legacy button masks whenever they change. Touch's button
; ids differ between runtimes, so if a button lands in the wrong place this says
; which mask it actually set. Leave at 0 for normal play.
GamepadLogButtons=0

; Right stick turns only. Its vertical axis is dropped, so the game camera never
; pitches from the stick. 1 = on and the default; 0 = the stock two-axis stick.
;
; In VR the headset already owns pitch: you look up by looking up. Stick pitch is
; then a second source for the same axis, fighting the first, and it is the
; uncomfortable one -- a vertical rotation your inner ear did not ask for is the
; strongest simulator-sickness trigger there is, worse than yaw. Yaw is left
; alone because turning on the spot with the stick is how you play seated.
;
; It does genuinely take something away: the engine aims and reads its look
; camera from the pitch this suppresses, so a shot lined up by tilting the stick
; has to be lined up by tilting your head instead. It is ON by default anyway,
; because DecoupledPitchChord below hands the stick pitch back whenever you hold
; RT+RB -- nothing is out of reach, and comfort is the right default in a
; headset. Set this to 0 for the stock two-axis stick.
;
; Applied to whatever right stick reaches the game, including a physical pad
; merged in alongside the Touch controllers. The binding line in the log says
; "look=Rstick(yaw only; hold RT+RB for pitch)" when both are on.
DecoupledPitch=1

; Hold RT + RB to get stick pitch BACK for as long as both are held. 1 = on,
; and it costs nothing when unused.
;
; This only ever hands pitch back -- it never takes it away. With
; DecoupledPitch=0 the stick already pitches and the chord does nothing at all;
; with DecoupledPitch=1 you keep the comfortable head-only default and can still
; reach for the stick for the one shot that wants it, then let go.
;
; A momentary control has to mean ONE thing. An earlier version inverted the
; setting instead, which made the same chord decouple pitch or restore it
; depending on a value you cannot see while playing.
;
; KNOW WHAT RB IS HERE. Touch has no physical shoulder buttons: the RIGHT GRIP
; synthesises XB_X for Walk. This pitch chord recognizes that Walk bit as the
; physical RB signal, so it is still right grip + right trigger and reads as
)INI"
           R"INI(; "walk and shoot". Set this to 0 if you would rather walk-and-shoot leave pitch
; alone.
;
; The chord only ever ADDS the suppression. Shoot and Walk still do their jobs
; while it is held; nothing is taken away to pay for it.
DecoupledPitchChord=1

; Hand stick pitch back automatically while Lara is in the water. Leave at 1.
;
; The one place DecoupledPitch=1 is not a comfort win but a straight loss: TR
; steers the swim with the LOOK axis, so suppressing pitch removes the ability
; to dive or surface at all -- and the head cannot stand in for it, because the
; head turns the VIEW while the stick turns LARA herself.
;
; Read from Lara's own water_status in the game DLL, so it follows HER and not
; the camera. An earlier attempt read the camera room's water flag instead and
; failed exactly where it mattered: during a surface swim the camera sits in
; the air room above the water and reports dry for the whole swim.
DecoupledPitchWaterOff=1

; Hand stick pitch back automatically while looking through an optic --
; binoculars, or a weapon combined with a laser sight. Leave at 1.
;
; The same failure as swimming, for the same reason: the game's own zoom
; camera reads the right stick's Y axis directly for vertical aim once it has
; taken over, and the head cannot stand in for it there either. Without this,
; holding the zoom (hold the right stick click) gets you a scope that pans
; sideways but never up or down.
;
; Read from the same two fields the game's own look-handling code checks to
; decide whether the stick means "look around" or "aim the zoom camera", so
; this follows the game's own notion of "currently zoomed" rather than a guess.
DecoupledPitchZoomOff=1

; Hold the head CENTRE at the game camera while looking through an optic, so the
; LASER SIGHT hits what it points at. Leave at 1.
;
; THIS IS NOT A STEREO SWITCH. Your eyes still straddle the centre by half an IPD
; each, so the world keeps all of its depth. What is held off is the 6DOF
; displacement, through the same line PositionalTracking=0 uses.
;
; WHAT IT FIXES. The laser dot is not an object in the world -- it is a
; screen-centre crosshair drawn in the 2D overlay pass. The bullet is real
; geometry, fired along a ray out of the game camera. So the dot and the impact
; are two points on ONE LINE through the camera, which is exactly why they agree
; perfectly on a flat screen: your eye is on that line. In VR it is not, so the
; dot (on the panel at HudDepthMetres) and the impact (on the wall) pull apart --
; about h*(D/Z - 1), or 0.45 m for 0.3 m of head offset, a 10 m target and a 4 m
; panel. It reads as purely vertical because seated your sideways offset is
; nearly nothing while your height offset is not.
;
; Put the head centre back on the line and the two agree at EVERY distance, with
; no need to know where the shot landed. The leftover per-eye 32 mm is equal and
; opposite, so it cancels in what you see and leaves the dot floating slightly in
; front of the wall rather than resting on it. That is a depth artefact, not an
; aiming error.
;
; THE COST: raising an optic moves your viewpoint to the game camera. That is
; motion you did not ask for -- bounded by how far your head is from the camera,
; on a deliberate button press, and unavoidable, since putting your eye on the
; aim line means moving it there. Logged both ways:
;   optics: head centre HELD AT THE GAME CAMERA -- laser sight and bullet agree ...
;
; 0 gives the stock behaviour back, misalignment included.
OpticsHeadAtCamera=1

; Switch off the engine's optic overlays. Both default to 1.
;
;   HideBinocularOverlay   the binocular vignette, TR5's VCI visor, and the
;                          Labyrinth fisheye
;   HideScopeOverlay       the scope frame, which also carries its reticle lines
;   HideOpticsTint         the transparent red pane behind the laser dot
;
; These are flat full-screen artwork -- the engine scales the mesh to the screen
; rect and writes it with z = 0, so each one is a 2D quad that lands on the same
; world-locked panel as the rest of the 2D layer, at HudDepthMetres. A vignette
; is meant to be the edge of your vision, and as a rectangle floating at 4 m
; inside a 94-degree field of view it cannot be: your real peripheral vision is
; wide open around it. No placement fixes that, because the thing it imitates is
; the headset's own field stop.
;
; HideOpticsTint is the one that is easy to miss. The engine lays a single
; untextured quad over the whole screen rect, coloured 0xFF5050FF, whenever the
; laser sight is up -- not only in infra-red mode. That is the transparent red
; rectangle sitting behind the dot, and in VR it is a red pane hanging at
; HudDepthMetres. Stubbing it takes the VCI headset's infra-red tint with it,
; because the engine draws both through that one function.
;
; THE AIMING DOT IS KEPT by all three. It is not part of any of those meshes --
; the engine draws it separately, as a sprite at the centre of the screen rect --
; so it still turns green on a target and still pulses.
;
; Set HideScopeOverlay=0 if you want the scope's crosshair LINES back and only
; the binocular circles gone; HideOpticsTint=0 if you want the red wash back.
; All three 0 is stock behaviour and patches nothing.
;
; Logged once when it takes effect:
;   overlay: 5 optic overlay draw(s) stubbed in tomb5.dll (Tomb Raider V) ...
;
; A "prologue mismatch" line instead means the game was patched and that overlay
; was left alone rather than risk writing into the wrong function.
HideBinocularOverlay=1
HideScopeOverlay=1
HideOpticsTint=1
; Draw a red beam from a drawn weapon with a combined/built-in laser sight.
; Works without zoom; native scoped aim remains supported too.
WorldLaserSight=1

; Hold R3 (right stick click) to turn the LEFT stick into a D-pad.
;
; R3 is the modifier rather than L3 because L3 is Sprint, and sprint here is
; held WHILE running forward -- "hold L3, push the stick forward" is already a
; gesture in play, so reusing it for D-pad up would mean choosing between them.
; R3 was the only spare input on the pad.
;
; A plain R3 click, with the stick centred, still emits RIGHT_THUMB exactly as
; before -- the D-pad only appears once the left stick is actually deflected,
; so nothing that relied on R3 is lost. The left stick's movement axes are
; zeroed while shifted, so you do not walk and press a direction at once.
;
; Only the dominant axis fires, so a flick cannot emit up and left together and
; move a menu selection twice.
DpadShift=1

; TR4/5 stereo VR: X opens the paused native inventory in front of the
; headset, anchored there until closed. With weapons active X retains native
; ammo switching. Release X, then LS navigates, A selects/uses, B backs out.
; Native 3D items over the live head-tracked level; gameplay remains paused.
WorldInventory=1

; Hold Y + LT for this many seconds to send the Xbox Menu button (XInput START).
;
; Touch has no Start or Back of its own, so both have to come from somewhere.
; The System button on the left hand's lower face sends one of them (see
; GamepadMenuUsesBack); this chord reaches the other without spending a button.
;
; The hold is what separates the chord from real play: Y is Action and LT is
; Equip, so the pair does occur naturally. One second is comfortable but IS
; within reach of normal play -- raise this if it ever fires unintentionally.
;
; It sends a PRESS, not a latch -- Menu toggles the pause screen itself, and a
; held START would never look like a clean press to the game. It fires once per
; hold; release and re-hold to send another. Y and LT are suppressed from the
; moment it fires until you let go, so it does not keep grabbing afterwards.
;
; 0 disables the chord entirely.
MenuChordSeconds=1.0

; How long the synthesised Menu press is held, in seconds.
MenuChordPressSeconds=0.15

; How far the left stick must travel before a shifted press registers, 0-1.
; Raise it if directions trigger too easily, lower it if they feel stiff.
DpadShiftDeadzone=0.5

; Which XInput button the left hand's LOWER face button sends.
;   1 = BACK  -- the System menu. Default.
)INI"
           R"INI(;   0 = START -- the pause/inventory menu.
; inputUpdate() decodes BACK to internal key 0x62 and START to 0x63; which one a
; given screen treats as System lives in the game DLLs, so this stays switchable.
GamepadMenuUsesBack=1

; --- reverse engineering ----------------------------------------------------

; Log the DLL-side return address of each distinct call into vid_setPass and
; ogl_drawVB, as "module+RVA".
;
; The game DLLs ship without PDBs, so this is how we find their render code
; without searching ~1800 unnamed functions: the DLL must call across into the
; engine to draw, and the return address at our hook is a code address inside
; the DLL. One gameplay frame gives exact RVAs to open in Ghidra.
;
; Deduplicated and capped at 64 sites per hook, but still verbose. Off for play.
LogCallsites=0

; --- room culling -----------------------------------------------------------
;
; THE PROBLEM. The engines decide what to draw from the GAME CAMERA. That is
; right for a monitor and wrong for a headset: you see wider than the game
; camera, and you can look somewhere it is not pointing at all. Turn far enough
; and the geometry that should be there was never drawn.
;
; TR4/5 FIX. Their room traversal is run a second time from the tracked HEAD,
; with the headset frustum, and anything it finds is appended. At every doorway
; the frustum is clipped to the opening, so a room is added only if it really is
; visible through that chain.
;
; TR6 FIX. mapCalcVisibleRooms first runs normally for gameplay, then a second
; time into private storage with a tracked-head camera. The stock globals are
; restored before mapProcess continues. Only mapDrawRoomList sees the union of
; stock and head-visible rooms, and it uses the same head camera while preparing
; render data. Downstream SYS_DRAW_CRP::Calculate calls also use that camera.
;
; PortalCulling=0 gives stock behaviour exactly in all three games.
;
; WHAT HAPPENED TO PortalHops AND FRIENDS. They are gone, and so is the whole
; problem they were managing. Hop expansion added every room N doorways away
; with no visibility test, so N had to be tuned per level, rooms you could not
; actually see got drawn anyway (the room-215 bug, and DrawAllRoomsExclude with
; it), and because it reached rooms the traversal never would, it also reached
; flip-map STORAGE rooms and needed a flipped_room check to stop them being
; drawn over the live geometry. A real traversal cannot reach a storage room --
; no live room's portals name one -- so none of that machinery survives. If your
; ini still sets those keys the log will say so once, on startup.
;
; Watch it work in TombRaiderVR.log:
;   gamedll: bound to tomb4.dll (Tomb Raider IV, build 0x696B4999, game=0) ...
;   cull: hooked tomb4.dll (Tomb Raider IV)
;   cull: head-frustum portal traversal live on Tomb Raider IV -- ...
;   cull: 31.2 rooms/frame from the engine + 8.4 added by the head frustum, ...
; TR6 instead logs:
;   tr6 cull: upstream visibility hooks installed ...
;   tr6 cull: render preparation received 8 stock rooms + 6 head-only rooms ...
PortalCulling=1

; Angle added to each half of the culling frustum, in degrees.
;
; Covers two small things: culling runs once from the head rather than once per
; eye, so the couple of degrees a canted display puts between the two frusta has
; to be allowed for, and the pose that culls a frame is a few milliseconds older
; than the pose that renders it. Used by both the TR4/5 and TR6 fixes.
;
; Raise it if geometry pops in at the very edge of vision when you turn quickly.
; Every degree costs a little more draw.
CullFovMarginDegrees=8

; How many doorways deep the traversal may go, and how many portals it may look
; at in one frame.
;
; NEITHER IS A VISIBILITY CRITERION. The frustum shrinking at every doorway is
; what stops the traversal; these only bound the worst case so a pathological
; level cannot spend the whole frame in here. If the log ever says
;   *** A BUDGET WAS HIT -- raise CullMaxPortals/CullMaxDepth ***
; during normal play, raise them. TR5's largest level is 242 rooms, so the
; defaults have a lot of headroom.
CullMaxDepth=16
CullMaxPortals=4096

; Optional distance limit in world units, 0 for none. One sector is 1024.
;
; Off by default on purpose. The engine's own phd_zfar is 65536 units, which
; culls nothing, and a room you can see down a long corridor is a room you
; should be able to see. This is a frame-rate lever, not a fix -- set it and
; distant rooms will pop.
CullFarUnits=0

; Widen every listed room's clip rectangle to the whole target. Leave at 1.
;
; This is the old DrawAllRoomsClipRect and it is still required for the same
; reason: the engine's portal-clipped rects are screen boxes computed for the
; GAME CAMERA's view, and we render from the HMD's, so under head rotation they
; scissor the wrong part of the screen entirely.
CullWidenBounds=1

; Extend the same fix to items: furniture, enemies, pickups, statics.
;
; The engine rejects an item whose bounding box misses the game camera's screen
; rect or sits behind its near plane -- which is every item in every room this
; feature adds. Hop expansion never touched this, so the rooms it forced in drew
; EMPTY. With CullObjects=0 you get that behaviour back.
;
; Answers are only ever promoted from "invisible" to "visible, clip it", never
; the other way. No menu gate is needed: the only callers of the engine's test
; are world-drawing paths, and the inventory ring does not go through it.
CullObjects=1

; Virtual-key code that dumps the current draw list to the log, 0 = disabled.
; Replaces RoomDumpKey. Prints which rooms the engine found and which the head
; frustum added, starred -- which turns "that wall is missing" into a room
; number.
;
; Each added room is then listed again with the doorway it came through and how
; many doorways deep it was, so a room that should not be there can be traced
; back along the sightline that let it in:
;
;   cull dump:   room 215   via room 188   3 doorway(s) deep
;
; A function key on purpose. Do not move this to the numpad: with Num Lock OFF
; the physical numpad keys emit navigation codes instead (numpad 3 becomes
; VK_NEXT), GetAsyncKeyState never sees the press, and the dump does nothing at
; all with no clue as to why.
CullDumpKey=0x77

; Draw the HD sky at optical infinity. 1 = on and the default.
;
; DrawSkyHD already zeros the translation of the matrix it draws the dome
; through, so the dome stays centred on the game camera -- that is "at
; infinity" on a monitor. The stereo path then composes the per-eye
; transform, whose translation is IPD plus any 6DOF head offset, and the dome
; gets stereo disparity equal to its mesh radius: a painted sphere a few
; metres away, sitting in front of distant geometry.
;
; On: those draws use the rotation of the eye transform only (looking around
; still turns the sky; leaning and IPD do not) and the fragments are pushed
; to the far plane so they never occlude the world. Off: stock finite-dome
; stereo, for A/B.
SkyAtInfinity=1

; TR4 HD sunrays and selected smoke/fire/spray/splash cells are enhanced in
; memory. No New Effects DDS installation is required; TR5/6 are unchanged.
; Restart the game after changing these load-time settings. Disable this if
; stacking with replacement effect textures is too bright.
TR4Effects=1
; Strength 0 = original, 1 = tuned enhancement, 2 = stronger. Range: 0..2.
TR4SunrayStrength=1.0
TR4EffectStrength=1.0

; --- first person (TR4/TR5) -----------------------------------------------
;
; Set this to 1 to enable the mode. Y + LT then switches between the original
; camera and first person, including when selected in a menu. Y + RT switches
; classic/remastered graphics. At 0, the old Y + LT hold chord is unchanged.
; Startup is always third person. Fixed/cinematic cameras
; keep their authored framing. TR6 uses a different engine and is unsupported.
FirstPerson=1

)INI"
           R"INI(; Animated head joint and eye offset in that joint's local coordinates.
; Negative Y is up; positive Z moves the eye forward.
FirstPersonJoint=14
FirstPersonAnchorX=0
FirstPersonAnchorY=-32
FirstPersonAnchorZ=144

; Hanging, ledge pull-up, climbing and push/pull hold Lara against geometry.
; Retract only the forward eye offset in those states; never extend a custom
; normal anchor farther into a wall.
FirstPersonInteractionAnchorZ=16

; Tracked rotation is always active. This controls physical leaning/ducking.
; Translation is measured from your headset position on entering first person,
; so standing tracking space does not add your height above Lara's head.
FirstPersonHeadTranslation=1

; Allow physical head leaning through walls without tracking pushback.
; Base camera and Lara still collide. Set to 1 to also clamp tracked leaning.
FirstPersonWallCollision=0

; Stabilize standing/walk/side/back eye position and horizontal root motion.
; Uses raw HMD tracking without a second physical-yaw orbit. 0 restores legacy.
FirstPersonMovementStabilization=1

; Neck-to-head distance for room-scale BODY movement. With stabilization off
; it also compensates the rendered eye arc. Metres (0-0.4).
; 0 disables neck-arc compensation; default is 15 cm.
FirstPersonRoomscaleNeckMetres=0.15

; Physical steps move Lara through native wall/ledge collision checks, without
; synthesizing stick input. Only movement actually rendered consumes tracking.
FirstPersonRoomscaleMove=1
; Keep a small lean allowance before Lara follows (metres), matching TR1-3.
FirstPersonRoomscaleDeadzoneMetres=0.02
; End: recapture the neutral position without changing your viewing heading.
; The existing RecentreKey (Numpad 5 by default) also works in first person.
FirstPersonRecenterKey=0x23
; Optional room-scale/heading diagnostics in TombRaiderVR.log.
FirstPersonDriftLog=1

; Hide the head, face attachments, and braid while the camera is anchored.
; Rolls temporarily hide the full body regardless of this setting.
FirstPersonHideHead=1

; Under modern controls, rotate movement into the HMD viewing direction.
FirstPersonMoveWithHead=1

; Turn Lara toward your physical/stick heading during ordinary ground movement.
; Scripted interactions, climbing and airborne animations retain their facing.
; The rate uses the same 60 Hz reference as the TR1-3 first-person setting.
FirstPersonBodyFollowsHead=1
FirstPersonBodyDeadzoneDegrees=0
FirstPersonBodyTurnDegreesPerFrame=4

; Both gun arms and firing direction follow headset yaw/pitch in first person.
FirstPersonHeadAim=1
; Controller tracking enabled to match the known-good Steam profile.
FirstPersonMotionGuns=1
; Unarmed HD arms bend to controller wrists using the same gun calibration.
; Requires motion guns/tracking above. Set 0 for the prior look-down-only arms.
FirstPersonUnarmedIK=1
; Full-body inference from headset/controllers in TR4/5 HD first person.
; Torso/hips follow head movement; leg IK retains native animated foot targets.
; No waist/foot trackers required. Set 0 to restore the earlier arm-only mode.
FirstPersonFullBodyIK=0
; Mesh grip calibration: positive pulls the gun back along its barrel axis.
; This local grip point stays on the controller as the gun rotates.
; Known-good fit: 0.2032 = 8 inches back; raise -0.06985 = 2.75 inches down.
; This is 3.75 inches below the original +1-inch height. Pitch is -30 degrees.
; Raise is controller-local up. Restart after editing.
; Set both values to 0 for the uncalibrated bind-pose wrist placement.
FirstPersonMotionGunGripForwardMetres=0.2032
FirstPersonMotionGunRaiseMetres=-0.06985
FirstPersonMotionGunRightMetres=0
FirstPersonMotionGunPitchDegrees=-30
FirstPersonMotionGunYawDegrees=0
FirstPersonMotionGunRollDegrees=0
; Focus the game, enter FP, draw pistols/Uzis (HD graphics, both hands tracked).
; Ctrl+F1/F2 left/right; F3/F4 down/up; F5/F6 backward/forward (1/4 inch).
; Ctrl+Shift+F1/F2 yaw left/right; F3/F4 pitch down/up; F5/F6 roll left/right (1 degree).
; Ctrl+F7 saves to this INI with a .motion-gun-calibration.bak backup.
; Ctrl+Shift+F7 restores the last loaded/saved fit. Tap keys (no auto-repeat).
; Ctrl is reserved while calibration is available, preventing native Action/Fire.
FirstPersonMotionGunHotkeys=1

; TR5 HK-only physical lens (HD first person + motion guns required).
; Target 3x at 12 cm reference eye relief, 512px capture.
; Lens-only depth correction prevents near clipping without shifting its mesh fit.
; Crossing the eyepiece disables the lens; no floating close-eye overlay.
; Raise the rifle to either eye; no R3/native full-screen zoom is needed.
; MeshFit attaches to the measured HK rear aperture using its wrist inverse bind.
; Forward/Up offsets and the old 3 cm radius are manual fallback only (MeshFit=0).
; Other guns, grip calibration and third person are unchanged.
; Requires normal per-draw stereo, EyeOffsetMode=3, PerEyeProjection=1.
FirstPersonHKScope=1
FirstPersonHKScopeMeshFit=1
FirstPersonHKScopeForwardMetres=0.18
FirstPersonHKScopeUpMetres=0.24
; Wrist pivot is recovered from the mesh bind pose. Old GripBack/GripUp
; experimental compensation keys are ignored; no draw-time offset is latched.

; Smooth right-stick turning while first person is active.
FirstPersonTurnDegreesPerSecond=120
FirstPersonTurnDeadzone=0.25

; --- Chest physics for TR4/TR5, ported from TR6's dynamic bones ------------
;
; This started as a measurement and is now a feature: with DynamicBones=1 and
; DynamicBonesShader=1 (both the defaults) the solver runs and the chest is
; deformed per vertex in the skinning shader. Everything else in this section
; tunes or instruments it; the settings that only measure say so.
;
; TR6 drives Lara's chest with two ordinary skeleton bones -- JUG_L_DYNAMIC
; and JUG_R_DYNAMIC, named in 6\DATA\CHAR\LARA_HD.CHR -- fed to the same
; spring solver as PONY1..10_DYNAMIC. TR4/TR5 have no such bones: Lara there
; is the classic 15-node rigid hierarchy, and the chest is polygons inside
; the TORSO mesh. So the solver can be reimplemented but not ported, and
; whether it is worth reimplementing depends on whether 30 Hz source
; animation gives a spring anything smooth to chase at headset frame rates.
;
; The answer turned out to be yes, so this is the master switch for the whole
; section: it runs the solver, and it is what lets the skinning shader be
; patched. Off means no chest motion at all, whatever the settings below say.
;
; It also writes the measurement to the log:
;
;   dynbones: joints=N shader=S frames=F dup=D% snaps=K drive_peak=.. disp_peak=..
;
; dup= is the one that matters. It is the percentage of rendered frames whose
; torso matrix was bit-identical to the frame before. High means the
; animation is stepping slower than the headset and a spring driven straight
; off it will buzz rather than swing.
DynamicBones=1

; Which joint carries the chest. CONFIRMED from the engine, not lore:
; tomb5.dll SkinUseMatrix is 14 byte-pairs of which only four are filled --
; (1,2) (4,5) (8,9) (11,12), the two knees and two elbows, where the skin
; blends between rigid meshes. That pins the canonical order:
;
;   0 HIPS  1 THIGH_R  2 CALF_R  3 FOOT_R  4 THIGH_L  5 CALF_L  6 FOOT_L
;   7 TORSO  8 UARM_R  9 LARM_R  10 HAND_R  11 UARM_L  12 LARM_L
;   13 HAND_L  14 HEAD
DynamicBonesTorsoJoint=7

; Where the chest sits ON THE BIND-POSE MODEL, in world units -- NOT an
; offset from the joint origin.
;
; The palette holds skinning matrices (bone * inverse-bind), so a translation
; column is not a bone position: the measured body draw put forearm and hand
; 2.0 units apart and hips and torso 0.5, impossible for real origins. Such a
; matrix does correctly map a bind-pose point to its skinned world position,
; which is what the anchor is. Lara's model origin is at the hips with +Y
; DOWN, so the chest is ~170 units of negative Y, slightly forward in Z, and
; mirrored either side of centre in X.
DynamicBonesAnchorX=34
DynamicBonesAnchorY=-170
DynamicBonesAnchorZ=45

; Spring constant and damping. k is omega^2, so 630 is 4 Hz, and damping 6
; is a ratio near 0.12.
;
; Frequency is what makes it visible, not strength. The previous 1900/22 was
)INI"
           R"INI(; a 6.9 Hz flutter that died in 0.11 s -- over before the eye registers it,
; and landing during Lara's own landing crouch, which hid the rest. Logs
; showed every jump detected and displaced ~14 units; it just could not be
; seen. At 4 Hz / 0.12 a running jump rings for about 0.8 s. Resting sag is
; gravity / stiffness, so the log's resting Y now reads ~8.6, which is right.
;
; The first measured build used 180/12 with a world-space position spring.
; That formulation lags by c*V/k under constant velocity, which at Lara's
; running speed is 160 units against an 18-unit clamp -- why both sessions
; reported disp_peak pinned at exactly 18.00 and never settling. The solver
; is now driven by the parent joint's acceleration instead, so constant
; velocity produces no displacement at all, and these constants replace the
; old ones rather than merely retuning them.
DynamicBonesStiffness=630
DynamicBonesDamping=9.5

; Along world Y, which is DOWN in TR4/TR5, so positive pulls downward. 5400
; is the engine's own gravity per second squared: 6 units per frame at 30
; fps, and 6 * 30^2 = 5400.
DynamicBonesGravity=5400

; How much of the parent joint's acceleration the bone feels. 1.0 is the
; physical answer; lower tames the noise that comes from differentiating a
; joint matrix twice.
DynamicBonesDriveScale=1.0

; Ignore drive accelerations below this, in world units per second squared.
; Gravity is 5400 for scale.
;
; Walking bobs the torso every step and an unfiltered solver answers all of
; it, so she jiggled while strolling. A jump or landing accelerates the torso
; by roughly an order of magnitude more than a footfall. Raise this if
; walking still registers; lower it if jumps stop registering.
;
; Drive term only -- gravity and the spring are never thresholded, since they
; are what bring the bone back to rest.
DynamicBonesDriveDeadzone=4000

; What drives the bone.
;
;   1 = engine state (default). Reads Lara's own gravity_status and fallspeed
;       from the game DLL. Airborne makes the bone weightless, grounded
;       restores gravity, and landing adds a kick sized by how far she fell.
;       Every jump responds, and walking cannot.
;   0 = acceleration. Differentiates the torso joint twice, then smooths,
;       ceilings and deadzones it. Kept for comparison: those filters compound
;       on short events, so landings only registered when an impact happened
;       to straddle frame boundaries favourably.
;
; DriveSmoothing, DriveMax and DriveDeadzone below only apply in mode 0.
DynamicBonesDriveMode=1

; Fraction of gravity felt while airborne, engine mode.
;
; 1 holds the chest still for the whole jump, so it only answers the landing.
; 0 is the physical answer -- in freefall the torso falls with the chest, so it
; goes weightless and rises about 15 units on the way up. That reads as the
; chest moving as she jumps, so the default trades the physics for the look.
DynamicBonesAirGravity=1

; Landing kick, engine mode: fallspeed * 30 * this, units/s, downward. Uses the
; deepest fallspeed of the jump, since the engine zeroes it on the landing
; tick. Simulated at the default 4 Hz tuning, the bone reaches about +18 for a
; small hop, +23 standing, +28 running and +34 for a long fall -- all inside the
; 40-unit clamp. Takeoff lifts it to about -6 every jump. Raise for more bounce.
DynamicBonesLandImpulse=0.2

; Low-pass on the acceleration estimate, 0..1. Lower is smoother.
;
; Position differentiated twice is amplified by 1/dt^2, about 2900 at these
; frame rates, so 128 units of anchor movement in one frame -- an ordinary
; measured value -- arrives as roughly 370,000 units per second squared. Raw,
; that is all spike and no signal. A jump lasts tens of frames and survives
; this filter easily; single-frame noise does not.
DynamicBonesDriveSmoothing=0.25

; Hard ceiling on drive acceleration, world units per second squared.
;
; 20000 is not arbitrary. One frame at the ceiling gives dv = a*dt, and a
; spring of this stiffness answers with a peak of dv/sqrt(k) -- so the
; previous 54000 produced 22.9 units against an 18-unit clamp and guaranteed
; saturation by itself, which is what the logs showed. 20000 peaks near 8.5
; and leaves the clamp as a backstop rather than the normal state.
DynamicBonesDriveMax=20000

; Which directions the bone may move in.
;
;   0 = free, all three axes
;   1 = world vertical only (default)
;
; Free looks right for jumping and wrong for turning: the anchor sits off
; centre and forward of the joint origin, so rotating the torso swings it
; through an arc, and an arc is acceleration -- which threw the whole upper
; body sideways on stick turns. Vertical-only keeps the jump response and
; discards the rotational sloshing. World vertical, not the joint own up, so
; it stays true while Lara leans.
DynamicBonesAxis=1

; How the solved motion reaches the screen.
;
;   1 = per vertex, in the skinning shader (default). Only the front of the
;       chest moves; back, backpack, shoulders and armpits stay where the
;       animation put them. Needs a build whose shader_init address is known:
;       the debug build, the retail exe and the HD Definitive Patch all are.
;   0 = the whole TORSO joint. Moves the back and backpack with the chest and
;       stretches the shoulder seams. The fallback, used automatically if the
;       shader path cannot start -- the log says why.
;
; A joint transform moves every vertex attached to it equally and cannot tell
; front from back, which is why 0 cannot be tuned into 1.
DynamicBonesShader=1

; Which way is the front of the torso. 0 = work it out from Lara's facing
; (default); 1 or -1 forces it. Only force it if the back bounces instead.
DynamicBonesForwardSign=0

; How strongly the chest moves on the per-vertex path, as a multiple of the
; solved motion. Shader path only.
;
; 1.0 moves the bust as far as the whole-torso view moved the whole upper body,
; which reads as much less because so much less of her moves. Simulated first
; bounce at this value and the default spring: about 16 units for a small hop,
; 28 standing, 40 running, 57 for a long fall, with the second bounce under a
; third of the first. Raise for more; lower if rubbery.
;
; DynamicBonesMaxDisplace clamps the solver BEFORE this multiplies, so raising
; this never moves where that clamp bites -- only falls past about fallspeed
; 220 reach it.
DynamicBonesChestStrength=2.5

; The chest region, measured from the torso mesh. Shader path only.
;
; Height is normally MEASURED: the band is fitted to the bump the bust makes in
; the mesh's front profile. Top/Bottom are only the fallback for an outfit with
; no distinct bump, as fractions of torso height from the neck end down.
DynamicBonesChestTop=0.18
DynamicBonesChestBottom=0.52
; Depth: where the weight starts, as a fraction of torso depth behind the front
; surface. Higher moves more of the breast, including its underside and sides;
; the back and backpack stay out either way.
DynamicBonesChestDepth=0.6
; Width: half-width fraction before the weight fades toward the armpits. Lower
; keeps the shoulders stiller.
DynamicBonesChestWidth=0.34
; How much of the bust the fitted height band keeps, as a fraction of the bump
; it makes in the front profile. Larger reaches further toward the upper chest
; and the belly.
DynamicBonesChestBand=0.65

; Permanently push the selected region out of her front by this many units.
; 0 = off. Calibration aid: set 30, stand still, and what bulges is exactly
; what will bounce.
DynamicBonesRegionDebug=0

; Put the solved displacement into the joint palette.
;
; THIS IS THE FALLBACK, AND IT IS IGNORED WHENEVER THE PER-VERTEX PATH IS
; LIVE -- doing both would move the chest twice. It only reaches the screen
; when DynamicBonesShader=0, or when the shader path cannot start on this
; build; the log says which. Joint 7 is TORSO, so everything weighted to it
; moves and the whole upper body wobbles rather than just the chest: coarse,
)INI"
           R"INI(; but it also makes the solver visible by eye -- anchor position, direction
; of motion and magnitude can all be judged from it.
DynamicBonesApply=1

; Overall multiplier on the solved motion, applied to BOTH paths -- the
; whole-torso view and the per-vertex chest, where it stacks with
; DynamicBonesChestStrength. 1 is the true amplitude and the value tuned
; in-headset. (It used to default to 4, a debug-view exaggeration that would
; have given the chest path 4 x 1.5 = 6 times the tuned strength.)
DynamicBonesDebugScale=1

; How far apart two joint origins must be, in world units, to count as
; separate joints when scoring which draw carries the body. Bones on a body
; are tens of units apart; padding slots differ by a fraction of one.
DynamicBonesSeparation=12

; Displacement clamp, world units. TR6 bounds its bones with authored
; deflector volumes; a radius is the stand-in for assets that carry none.
; Sized to the stiffness: at 4 Hz a running jump reaches +28 and a long fall
; +34, so the old 18 would clip even a small hop. 40 leaves only extreme falls
; touching it.
DynamicBonesMaxDisplace=40

; Anchor movement in one frame beyond which the solver snaps instead of
; integrating -- what SpringSystem::teleport does in TR6. A level load or a
; cutscene cut is not an acceleration.
DynamicBonesTeleport=900

; Frames between report lines.
DynamicBonesReportFrames=900

; Dump every joint's translation once per report interval, to confirm the
; torso index rather than assume it. Verbose.
DynamicBonesLogJoints=0

; Room indices to watch, comma or space separated. Empty = off.
;
; Any listed room the traversal appends is reported once per distinct route:
;
;   cull watch: room 215 reached via room 188, 3 doorway(s) deep -- the head
;   frustum can see through that chain of openings
;
; This is the targeted form of the dump key. A room that only misbehaves for a
; moment is hard to catch with a hotkey, and "does the traversal ever reach 215,
; and how" should not need good reflexes to answer. Silence means it never got
; added, which is itself the answer.
CullWatchRooms=

; --- measured, and no longer behind a key ------------------------------------
;
; Findings from the culling work whose own keys have since been removed. Each
; one rules something OUT, and without them the next person looking at a
; phantom texture re-runs them by hand.
;
; WIDENING THE PROJECTION DOES NOTHING. Swept to 200% of screen each way, and
; 20x on TR6: neither extra geometry nor a frame-rate change. The game DLL
; builds its cull planes from its own matrices and never consults the engine's
; projection matrix, and a portal beside or behind the camera fails the
; near-plane test inside SetRoomBounds before any rectangle is looked at.
;
; PROXIMITY IS THE WRONG CRITERION. Appending rooms by distance from the camera
; happily adds a stacked room that shares world space with the one you are
; standing in and that no portal reaches, which draws foreign geometry over your
; own. This is why the fix is a traversal and not a radius.
;
; ROOM BOXES OVERLAP NORMALLY. Rejecting an added room whose world bounding box
; overlaps one already drawn looked principled and was wrong: a TR room box is a
; rectangle around an irregular interior, and ordinary neighbours share a border
; of wall sectors, so plain adjacency shows a 2048x2048 overlap. It rejected six
; legitimate pairs at once.
;
; THE VIEW MATRIX'S TRANSLATION COLUMN IS THE CAMERA POSITION. It is not the
; -R*p a textbook view matrix carries. vid_setViewMatrix (tomb456.exe RVA
; 0x0000B960) scales the nine rotation terms by 1/16384, negates exactly the
; third row, and writes m[3], m[7] and m[11] through RAW. Using it as though it
; were -R*p produces an error that scales with world coordinates, so it behaves
; on a small level and fails on a large one.

; Per-draw state dump: which matrix carries the difference between one drawn
; element and the next -- projection, view, or model. Get the screen in
; question up, then press DumpKey. One line per draw, logged before any of our
; substitutions, so what appears is what the ENGINE set. 0 = disabled.
DumpDraws=0

; Virtual-key code that arms the dump. 0x78 = F9.
DumpKey=0x78
)INI";
}

} // namespace tr
