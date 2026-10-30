# TR1-3 versus TR4/5 source audit — October 28, 2026

This audits the current working trees at `C:\dev\TombRaider123VR` and
`C:\dev\TombRaider456VR`, including their uncommitted changes. The previous
14-feature implementation list was not proof of complete behavioral parity.
The findings below distinguish implemented fixes, remaining gameplay
differences, diagnostic/coverage limitations, and collection-specific behavior.
Source review and automated tests cannot establish that every outfit, level
or native render ordering looks correct in the headset.

The reference repository changed during this review, adding ordinary-jump
landing stabilization. The final comparison is pinned to the copy in
`build/collection-parity/reference-snapshot/src`; the JSON records its capture
time and hashes, so later edits to the live reference cannot silently change
what this audit covered.

`tools/audit_collection_parity.py` records every source file and its hashes,
including identical, changed and collection-only files, plus scalar config
differences in `build/collection-parity/source-audit.json`. A missing filename
or differently named hook is not, by itself, a missing feature.

## Corrections included in this update

| Finding | Correction and evidence |
|---|---|
| Left/right/back takeoff direction is rotated twice | Added the scoped analog-axis guard around native `LaraAboveWater`. The native `lara_as_compress` rotation was verified in all three PDB DLLs. Native digital direction, blocked takeoff, speed, collision and gravity remain authoritative. TR1's armed rules and TR2/3 flare IDs are adapted separately. Regression covers all games, directions, multiple headings, blocked launches, tank controls and armed/flare cases. |
| Directional-jump body visibility | During current states 25/26/27, HD eye draws retain hands/weapons and hide the body, forearms and hair. Per-draw masks restore immediately; full native shadows remain. Applies with full-body IK on or off. Side-jump hiding is an explicit user-requested extension: current TR4/5 hides backjumps but leaves side jumps visible. |
| Sidestep/backpedal arm wobble | Preserved the existing, previously undeployed `GroundGaitReference` port. It rejects native torso sway before controller arm solving, retaining moving feet and live head/controller tracking. Production tests cover TR1/2/3, armed/unarmed, 15/32-joint palettes and repeated stereo draws. |
| Looking down bends knees | Added the reference eye-to-neck posture compensation, captured on both initial neutral and recenter. Looking down around a stationary neck no longer counts as pelvis lowering. Actual crouch and raw headset/view translation remain. The new real-tracking regression failed before the correction with a false 0.161865 m crouch. |
| Backjump appears to jump again after its apex | Ported the backjump camera's interpolated collision-root arc with the existing calibrated standing eye offset. It has no temporal filter and does not modify physics. Uncalibrated midair entry and disabled stabilization retain the native camera. |
| Left turns treated as landings | Removed state 7 / animation 13 from the landing whitelist. It is a looping turn in all 140 installed non-cutscene animation tables, not a landing. This prevents a head turn from selecting the animated landing eye and releasing standing posture. |
| Base-camera crate/wall clearance not deployed | Included the pre-existing camera-arm port alongside the new fixes. Base-eye clearance works with free physical leaning enabled; native collection-specific collision sample counts and mount-height handling remain. |

The installed DLL at the start of this audit was the roomscale repair
(`31ACCB7E4B1EAE8B262AD047B86FB867AB581C2E76CEA9A2FAE55E84871DD6CA`).
The later `build/camera-arm-port` build had not been deployed. The new
deployment receipt is `build/collection-parity/deployment-jump-posture-repair.json`.

## Gameplay differences identified by the audit

These were still present at the audit snapshot. All nine are now addressed by
the subsequent [remaining parity port](remaining-parity-port.md). The table
below preserves the pre-port findings and their source evidence.

| # | Gap | Source evidence and likely effect |
|---|---|---|
| 1 | Mount/landing visibility recovery | Reference `FirstPersonBodyMask` / `BodyVisibilityRecovery` keeps an arms-only mount mask or chest-hidden landing mask for at least 350 ms and through torso recovery. TR1-3 lacks that recovery mask; `SetMeshVisibility` also releases its ledge restriction for tracked bodies. Torso geometry can reappear during the transition. |
| 2 | TR3 crouch/crawl body masking | Reference `IsHandsOnlyBodyState` applies hands-only rendering to crouch/crawl as well as backjump. TR1-3 still disables its crouch hide when motion/unarmed IK is ready. The new directional-jump mask deliberately does not change crouch behavior. |
| 3 | Water-exit camera/grip classification | Reference includes water-out state 55 in mount, constrained-anchor and native-grip handling. TR1-3's `IsLedgeMountState` only includes 19/54. TR2/3 water-out 55 misses that handling; TR1 water-out 58 is caught by the broad 56-61 camera-climb test but not the mount/grip test. A port needs per-game states, not a copied numeric whitelist. |
| 4 | TR3 sprint steering/body continuity | Reference `CanTurnBody` and `GroundGaitMatchesAction` include sprint state 73. TR1-3's ordinary-ground predicates omit it, including in TR3 where sprint exists. Sprint can leave the normal first-person heading/body-fit path. TR1/2 have no sprint to port. |
| 5 | Ledge pull gesture robustness | Reference requires both absolute and head-relative downward hand travel of 22 cm, retains 64 samples, requests the native climb for 900 ms and requires a 15 cm raise to rearm. TR1-3 uses only head-relative travel, 18 cm, a 500 ms request and 10 cm rearm. Raising only the headset can resemble a pull, and the shorter request can miss a native hang-loop entry window. |
| 6 | Large ledge-catch feedback eligibility | Reference qualifies a half-tile (512-unit) horizontal or vertical excursion without a minimum tick count and includes airborne reach/up-jump states 11/12. TR1-3 requires 10 ticks, 768 horizontal or 512 vertical units, and excludes 11/12. Some catches that rumble in TR4/5 will not here. Pulse length also differs: 120 ms here versus 80 ms there. |
| 7 | Sustained holster proximity feedback | Reference `GunHaptics::Hover` sustains light 700-microsecond pulses every 60 ms while refreshed. TR1-3 `HolsterHoverHaptic` gives one 25 ms strong burst on entry. Both provide feedback, but their behavior differs. |
| 8 | Blended costume cuff preservation | Reference `MotionHandSkin` / `FirstPersonBodyWristSeamMask` preserves mixed wrist/visible-forearm support below the usual 50% body visibility threshold. TR1-3 `HandSkin` still discards that body fragment. Its separate wrist-cap geometry closes cut rims but does not implement the same blended cuff policy. Outfit-specific seam gaps need a port and visual verification. |
| 9 | Ordinary-jump landing stabilization | The reference now has `JumpLanding`, sampled around native simulation, to distinguish an intentional same-height/upward jump landing from a drop or hard fall. `OrdinaryJumpLanding` keeps the standing camera and a saved upright torso reference for ordinary landings. TR1-3 still uses the native landing camera/torso for all whitelisted landing clips. This is separate from correcting the left-turn misclassification and the airborne backjump arc. |

## Diagnostics and coverage limitations

- **Per-room culling watch:** reference has `CullWatch`/`NoteWatched`; TR1-3
  has normal culling logs/dumps but no watched-room list. Its dump hotkey
  defaults to disabled rather than F8. This is a diagnostic gap, not missing
  portal traversal.
- **Palette capacity:** reference handles up to 33 joints and wider visibility
  masks; TR1-3's IK/body/hand paths stop at 32. This is a compatibility limit.
  This audit has not established an installed TR1-3 costume requiring 33;
  do not label every outfit broken or copy TR4/5 buffer sizes into native
  layouts without asset/layout verification.
- **Classic-renderer visibility:** the new hands-only directional-jump policy
  is implemented in the HD rendering path used by tracked arms/full-body IK.
  Classic Lara still follows its existing native mesh-mask path. This is not
  full visual parity across both renderers.
- **Documentation:** an old comment near the end of TR1-3 `Config.h` still says
  portal traversal has not been implemented. That comment is stale:
  `PortalCull.cpp` implements both room traversal and object-bounds handling.

## Implemented equivalents and intentional differences

| Area | Audit result |
|---|---|
| Core body math | `BodyFollower.h`, `FirstPersonBodyIK.h` and `FirstPersonIK.h` are identical after line-ending normalization. Remaining differences are integration/gating/render policy, not absent solvers. |
| Stereo and sky | Shared `StereoRenderer` is identical. Both have head-relative portal/object culling and sky-at-infinity handling with collection-specific native layouts. |
| Inventory | `CollectionParity.cpp` / `InventoryProjection.h` implement the reference inventory-scene role. TR1-3 must use its non-presenting command flush, restore the native ring camera and share an anchor between perspective meshes and labels. Calling the similarly named TR4/5 output routine here was incorrect and was repaired earlier. |
| Roomscale | Private sweeps and the scoped native standing-collision hook retain ledge departure and native falling. The previously deployed repair is included in this build and its regressions still run. |
| Pistol transitions, holsters and flares | Pistol blend and physical-transfer logic are present. Weapon IDs, inventory slots and dual/single-hand rules are adapted. Held flares/light tracking apply only to TR2/3; TR1 has none. Hover cadence remains the gap listed above. |
| Weapon aiming | Controller aim, per-hand assist, hitscan, native projectiles, muzzle effects and target filtering are implemented through TR1-3-specific hooks. Configurable assist and calibration UI are local features; differing weapon/muzzle offsets are not automatically errors. |
| Tracking/recenter | Shared headset/controller yaw reset, initial level neutral, runtime recenter and pose timing diagnostics are implemented. TR1-3 uses a unified neutral rather than the reference's separate third-/first-person neutral fields. The actual posture omission is fixed above. |
| Camera/input | View/graphics chords, jump priority, running jump assistance, roll heading, monkey bars, switch fallback, background pad polling and clearance are present. Remaining water/sprint/pull differences are listed explicitly above. |
| Effects/physics | `EnhancedEffects` serves TR1-3's native textures/effects; `TR4Effects` is not portable by filename. Dynamic body physics, native physics palettes and tracked shadows are implemented. |
| Reference-only weapons/overlays | HK scope, attached laser sights, crossbow ammo cycling, binocular/scope/tint overlays and zoom-specific pitch gating depend on TR4/5 items absent from TR1-3. TR3's Desert Eagle does not acquire TR4/5's detachable laser-sight system. |
| TR6 | Native/alternate-eye TR6 rendering, video bypass and TR6 culling are for a different engine and are not TR1-3 gaps. |
| Options/defaults | Third-person startup remains intentional. TR1-3 has direct X menu and Y+RT graphics controls instead of the reference's optional legacy long-hold menu chord when first-person support is disabled. Gun calibration keys live in a nested profile rather than six flat fields. `FirstPersonBodyYawSpeedDegrees` is parsed in the reference but has no consumer there, so its absence is not a missing working behavior. |

## Validation and limits

- Production locomotion/render/IK hooks: 1,145,888 checks pass, including the
  previously undeployed arm-stability and camera-clearance regressions.
- Actual VR tracking/eye/projection code: 67,387 checks pass.
- Body/calibration/pistol/holster math: 15,654 checks pass.
- Inventory/collection hooks: 1,888 checks pass, including real OpenGL colour
  and depth preservation. Core selftests pass with zero failures.
- Native PDB/address audit: all 3,677 checks pass across supported builds;
  generated collection hook metadata also verifies.
- Both jump corrections fail their regressions when individually disabled.
  The posture regression failed before the source correction.
- Landing classification verified against 39 TR1, 47 TR2 and 54 TR3 animation
  banks. Native jump rotation evidence is retained in
  `build/collection-parity/compress-tr1.txt` through `compress-tr3.txt`.
- Release x64 builds with automatic deployment disabled; the separate deployment
  step verifies hashes and keeps a rollback DLL. Existing INI settings remain.

Headset gameplay validation is still required. Prioritize all three directional
jumps, standing downward glances, side/back movement with empty hands and guns,
inventory entry/exit, and physically walking off ledges in each game. Automated
tests exercise real mod hooks with synthetic native callbacks; they do not run
the entire native game simulation or certify visual comfort.
