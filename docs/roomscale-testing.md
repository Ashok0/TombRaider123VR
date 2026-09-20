# Roomscale headset validation

This revision has automated math tests and binary address checks. Gameplay,
stereo quality and performance still need a headset session on TR1, TR2 and TR3.

Use Modern controls, FirstPerson=1, PositionalTracking=1,
FirstPersonHeadTranslation=1, FirstPersonBodyFollowsHead=1,
FirstPersonBodyDeadzoneDegrees=0, FirstPersonMoveWithHead=1 and
FirstPersonRoomscaleMove=1. Set FirstPersonDriftLog=1 for the test session.
Old controller-steering settings are ignored; the HMD sets movement direction.

1. Load a level, stand comfortably and press END. Check head height and stereo.
2. Without touching either stick, turn physically 90 degrees left, right, then
   180 degrees. Lara should follow your heading. Push the left stick forward
   after each turn: movement should follow the HMD without gradually circling.
3. Repeat using only right-stick turns. Then combine physical and stick turns.
   Check that looking up/down or tilting your head does not alter forward.
4. With sticks centered, step forward, backward, left and right. Repeat after
   a 90-degree physical turn and after a 90-degree stick turn. Once the engine
   catches up, Lara should stop; moving back toward neutral cancels a step.
5. Step toward a wall. Lara's collision should block her, and the log's pending
   step should remain. Step back or use END to cancel it. Head leaning itself
   remains unrestricted and may place the view through nearby geometry.
6. Lean sideways and turn with the right stick: there should be no large orbit
   around the old neutral. Try walking with the stick and physically together.
7. Open/close inventory, use the R3 D-pad shift, trigger a fixed camera, climb,
   use a lever, jump and swim. Check for stale movement on return. Swim and
   interaction body facing remain engine-owned; test native controls there.
8. Check head height, stereo and room visibility after each transition. Compare
   performance while facing different directions. Inspect `locomotion:` lines
   in TombRaiderVR.log for stable `base`, expected `headWorld`, and room counts.

The right stick defaults to smooth turning at 120 degrees/second. Adjust
FirstPersonTurnDegreesPerSecond to taste. The roomscale deadzone defaults to
12 cm; decrease it if small physical steps do not engage movement. The engine
still owns gait and stopping distance, so this needs gameplay tuning rather
than claiming exact one-to-one collision movement from the math tests alone.
