# Remaining nine gameplay parity differences

The nine gameplay differences identified in the
[October 28 audit](collection-gap-audit-20261028.md) now have implementations
adapted to TR1-3. The reference is the captured TR4/5 source in
`build/collection-parity/reference-snapshot/src`. Existing jump-direction,
directional-jump hiding, posture, arm-stability, inventory and roomscale
corrections remain included.

| # | Port | Behavior |
|---|---|---|
| 1 | Mount/landing visibility recovery | HD eye draws retain arms-only visibility during mounting and hide the chest on hard/native landings. Restrictions persist for 350 ms and until torso recovery finishes. New Lara/level, recenter and clock reset clear the cache. Native shadow draws retain their complete mask. |
| 2 | TR3 crouch/crawl masking | All native TR3 duck/crawl/turn states retain hands and weapons while hiding the torso, legs and forearms. Empty-hand tracking now remains eligible during crouching, so the outer visibility gate cannot erase the hands. Applies with full-body IK enabled or disabled; native crawl-to-hang and other interactions keep their own control. |
| 3 | Water-exit camera/grips | TR1 state 58 and TR2/3 state 55 use the short mount anchor, arms-only mask, native grip constraints, and mount-to-standing height handoff. TR1 twist/water-out states are no longer misclassified as ladder camera states. |
| 4 | TR3 sprint | State 73 participates in grounded heading, body fitting, gait matching and root-motion eligibility only in TR3. Native sprint animation, speed, gravity and collision remain authoritative. |
| 5 | Physical ledge pull-up | Both hands must move down at least 22 cm in tracking space and relative to the headset over 100-850 ms. The request lasts 900 ms and needs a 15 cm raise before retrying. Only the stationary native hang-loop qualifies. Explicit movement, drop, missing tracking, recenter or a pause cancels the gesture. Native TR1-3 grab rules are retained; a temporary camera-relative heading makes Forward point into the ledge regardless of gaze. |
| 6 | Ledge-catch feedback | Half-tile horizontal travel or vertical excursion qualifies, including airborne reach/up-jump states. No ten-tick minimum. Native ledge alignment cannot create a large-flight sample; item/level changes and teleports discard history. Both controllers use the shared 80 ms shot/impact scheduler. |
| 7 | Holster proximity feedback | Refreshed proximity sustains gentle 700-microsecond pulses at 60 ms intervals. Shots and transfers take priority, with all effects sharing the 5 ms pulse limit. Leaving the zone, lost tracking or reset cancels hover feedback. |
| 8 | Costume cuff seams | Body shader draws preserve blended vertices supported by both a hidden wrist and visible forearm. Pure hidden-hand geometry remains clipped. Hidden wrist helpers are mapped through each outfit's palette; the permission is scoped to the body draw and cleared for subsequent passes. Existing rigid tracked hands, native shadows and wrist caps remain. |
| 9 | Ordinary-jump landing stabilization | The native simulation observer distinguishes intentional same-height/upward jumps (or drops no greater than 128 units) from ledge falls and hard landings. Ordinary landings retain the standing camera and pre-jump torso reference, with live controller IK and native foot contacts. Falls and animation 24 keep their native landing presentation. No native root, animation time, gravity or damage is changed. |

No new setting is required. Full-body IK remains enabled in the installed
profile. Positional tracking, HD rendering and the existing tracked-hand
options retain their established requirements. This port does not claim to
resolve the separately listed diagnostic, 33-joint asset-coverage or classic
renderer limitations.

## Verification

- Release x64 builds successfully. Final suites pass: 1,146,402 movement,
  camera and IK checks; 67,387 tracking checks; 15,654 body/holster checks;
  1,888 inventory checks; 1,704 physics/render checks; core tests with zero
  failures. Native address validation passes all 3,677 checks.
- Production movement, camera, visibility, IK and input-hook regressions,
  including all previous inventory/roomscale-related fixtures.
- Direct production render tests for all three games, armed/unarmed hands,
  body IK on/off, remapped wrist helpers, native shadows and recovery exit.
- Ordinary jump versus drop/hard-fall camera tests and a length-preserving
  landing squat with forward torso pitch through the actual body-palette solver.
- Real OpenGL pixel checks for cuff preservation and hidden-hand clipping;
  actual native hand shader pairs compile with the added shader interface.
- Gesture tests at 30/60/90/144 Hz, head-only movement, crouching, tracking
  loss, native clearance rejection and restoration of scoped input/heading.
- Ten negative controls individually disable corrections (including separate
  landing camera and torso controls) and all trigger their expected regression.
  Results are retained in
  `build/remaining-parity/negative-controls.json`.
- `tools/verify_remaining_parity.py` checks the installed hang/water-exit/sprint
  mappings across 39 TR1, 47 TR2 and 54 TR3 non-cutscene animation banks.

Build, test and deployment evidence lives in `build/remaining-parity`.
Headset gameplay validation is still pending: automated tests exercise actual
mod code with synthetic native callbacks and OpenGL, not a complete native
game session or every costume/level.
