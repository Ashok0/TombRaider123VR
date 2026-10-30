# TR4/5 collection parity port

The [October 28 source audit](collection-gap-audit-20261028.md) supersedes any
interpretation of this feature list as complete behavioral parity. It records
the directional-jump, posture, arm-stability and camera corrections, plus the
remaining integration and rendering gaps found in the current working trees.
The [subsequent nine-gap port](remaining-parity-port.md) addresses all nine
gameplay differences listed by that audit.

All 14 main gaps from `C:\dev\TombRaider456VR` have implementations in TR1–3.
The shared solvers were carried across; native hooks, weapon IDs, skeleton
bindings, room layouts and hand/holster enums were adapted to this collection.

| # | Gap | TR1–3 implementation |
|---|---|---|
| 1 | Full-body IK | Head/controller driven torso, hips and legs; fixed body-size calibration, planted feet, lean allowance, yaw slack and body following. Native airborne legs remain authored. Enabled by default. |
| 2 | Physical holsters | Grip within 30 cm of a rendered thigh/back holster; withdraw beyond 35 cm with at least 8 cm travel to draw. Release the grip inside the zone to stow. Independent pistol hands, single weapons in the right hand, hover/transfer haptics and trigger-release protection. |
| 3 | Pistol transitions | Full-body mode blends button-driven draw/holster transitions over 160 ms. Physical transfers stay controller driven. |
| 4 | Spatial inventory | Unarmed X opens a level, eye-height menu over a live stereo redraw of paused scenery. Left stick navigates without R3. Entry/exit drains prevent accidental actions. Armed X is consumed: TR1–3 have no selectable alternate ammo variants. |
| 5 | Rigid arm reach | Tracked arm solving retains bone lengths and permits shoulder reach when the controller exceeds the original chain's range. |
| 6 | Arm roll continuity | Authored upper-arm/elbow roll transport with wrist continuity and singularity handling. |
| 7 | Shared pistol poses | The body solves from model bind poses and publishes complete arm frames to forearm and hand attachments; native recoil does not independently displace them. |
| 8 | Controller flares | TR2/3 held flare mesh and dynamic light follow the left controller. Native age/flicker remain; dropped flares and lost tracking fall back to native behavior. TR1 has no flares. |
| 9 | Background pad polling | Native input polling continues while desktop focus is elsewhere; actual focus is restored after the poll. |
| 10 | Roomscale ledge departure | Collision sweeps accept the first unsupported step and stop there, letting native collision select falling. Wall and ceiling rejection remain. |
| 11 | Optional tracked camera wall collision | Base-eye clearance always applies. `FirstPersonWallCollision=0` permits physical leaning through walls; `1` also clamps that leaning. Native body collision and vertical clearance remain. |
| 12 | Physical crouch | TR3-only option: crouch at a 35 cm headset drop, release at 23 cm. Physical lowering is applied once, without adding native Duck camera lowering. |
| 13 | Calibration lifecycle | Fresh third-person neutral after a level loads; full-body calibration settles after level/water transitions. Y+LT resets tracking yaw and neutral for the headset and both hands together. Runtime recentering preserves first-person world heading. |
| 14 | Diagnostics | Pose-serial arm-motion samples and compositor frame timing/reprojection summaries under `FirstPersonDriftLog`. |

## Settings

These entries belong in `[VR]`. Full-body IK is enabled by default; physical
crouch and wall collision for physical leaning remain opt-in. HD graphics, positional tracking
and the existing `FirstPersonMotionGuns` hooks are required for tracked bodies,
holsters and flares. Startup remains third person; Y+LT switches views.

```ini
FirstPersonFullBodyIK=1
FirstPersonBodyYawSlackDegrees=45
FirstPersonBodyLeanRadiusMetres=0.15
FirstPersonBodyFollowSpeedMetres=0.9
FirstPersonBodyHandYawWeight=0.15
FirstPersonBodyScale=0
FirstPersonPhysicalCrouch=0
FirstPersonWallCollision=0
WorldInventory=1
```

Set `FirstPersonFullBodyIK=0` to disable body fitting. `FirstPersonBodyScale=0`
automatically fits standing size; a positive value overrides that scale.
Physical crouch can be enabled separately for TR3. End (the default recenter
key) resets head/body calibration while preserving world heading. Physical
holster grips take priority over the two-grip Action chord when in a holster
zone. Buttons retain their ordinary behavior outside those zones.

## Native build verification

`src/CollectionAddresses.inl` contains exact prologue bytes and RIP relocations
for each supported EXE/DLL timestamp. Focus hooks also verify the native
message-pump/focus-test sequence before installation. Unknown/mismatched code
retains native behavior. The generated tables cover the PDB build, earlier
retail/Gold, October retail (`0x6AA156AA`) and EGS (`0x6AA156D9`);
identical DLL timestamps share verified rows.

`tools/collection_addresses.json` retains symbol-resolution evidence and binary
SHA-256 hashes. With the supported binaries in their existing verification
directories:

```powershell
python tools/generate_collection_addresses.py --check
python tools/verify_addresses.py build/current-retail-verify build/current-gold-verify build/current-october-verify build/current-egs-verify
```

To re-resolve the added symbols, run `tools/port_collection_addresses.py`, then
regenerate the table with `tools/generate_collection_addresses.py`.

## Validation

October 28 follow-up ports the crate-camera and side/back VRIK corrections.
The base camera now keeps clearance even with free physical leaning enabled,
including the visible near face of movable blocks while native floor collision
is temporarily absent. Collection-specific floor-sample counts, mounting root
height, climbing and crouched clearance are preserved.

The shared `GroundGaitReference` retains the last ordinary ground upper-body
pose in Lara's interpolated root/yaw frame during side/back gaits. Animated
feet stay leg-IK targets; head/controller tracking is applied afterward. Model
changes, tracking loss, jumps and leaving ordinary ground poses release the
reference. Native equip animation still owns button draw/holster arms.
Production palette regressions exercise armed/unarmed TR1/2/3, native and
32-joint HD palettes, root/camera travel, moving feet, live tracking and repeated
draws. Separate camera regressions exercise both physical-lean policies.
Disabling either correction reproduces its new regression failure.

Roomscale ledge departure now carries its drop allowance through the native
standing collision query as well as the private movement sweep. The native
`GetCollisionInfo` hook is restricted to Lara's exact collision object during
eligible physical ground movement, including pauses between physical steps.
It restores the query's settings afterwards; manual movement, Walk, jumps,
airborne states and unrelated collision queries retain native behavior.
Regression tests model the native STOP/fall branch for all three games,
reproduce the old front-radius pinning, and verify the native gravity handoff
with full-body IK enabled and disabled. Walls remain blocking.

The inventory repair uses `S_CmdFlush` for the background. Unlike TR4/5's
similarly named routine, TR1-3's `S_OutputPolyList` also presents the frame,
paces it and pumps messages. Calling it before drawing the ring caused a
second frame boundary. The binary audit now rejects that choice and verifies
that the selected command flush uploads/draws without ending the frame.

After background submission, native inventory initialization restores the
menu camera, projection, fog and viewport. Its unconditional colour/depth
clear is suppressed during this restoration, then both eyes' depth buffers
are cleared explicitly. Perspective ring meshes and labels now share the
same anchored layout and `HudDepthMetres`/`HudSizeDegrees`; meshes retain
their volume. `Ortho3DLockToHead` is not the control for the perspective ring.
The camera snapshot supports both first and third person, is not overwritten
by menu camera updates, and is invalidated on view changes, unload or a new level.
The collection test covers native clear suppression, camera restoration,
both eyes' preserved background, and ring/label projection across native
FOVs, screen aspects, world scales and moving eye transforms.

The Release x64 DLL builds with deployment disabled. The following pass:

- Core math, configuration and real inline-hook tests (`tests/build_selftest.cmd`).
- Production movement, IK, input, holster metadata, flare pose and camera hooks (`tests/build_locomotion_selftest.cmd`).
- Real tracking transforms, headset/hand recenter invariance and projection tests (`tests/build_tracking_selftest.cmd`).
- Body following, rigid proportions, body calibration, pistol transitions and every native weapon's holster rules (`tests/build_body_parity_selftest.cmd`).
- Actual inventory background detours with real OpenGL colour/depth buffers, native focus restoration, held/dropped flare light and recycled-slot protection (`tests/build_collection_hooks_selftest.cmd`).
- Existing physics and native shader compilation (`tests/build_physics_selftest.cmd`).
- Exact added hook windows and the existing 3,602-check PDB/binary address audit.

Headset gameplay validation remains outstanding. Check each game's outfits,
both pistol hands, single back weapons, draw/holster reversals, inventory entry
and exit while moving, focus changes, roomscale ledges, level/water transitions,
and TR2/3 flare pickup/drop. Exercise full-body mode with head turns, extended
arms, crouching, jumping, landing and ledge interactions; the automated tests
cannot establish visual comfort or every native render ordering in gameplay.
