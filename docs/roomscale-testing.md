# Direct roomscale headset validation

Use FirstPerson=1, PositionalTracking=1, FirstPersonHeadTranslation=1,
FirstPersonBodyFollowsHead=1, FirstPersonMoveWithHead=1 and
FirstPersonRoomscaleMove=1. Set FirstPersonDriftLog=1 during testing.
FirstPersonRoomscaleDeadzoneMetres=0.02 gives a 2 cm lean allowance;
FirstPersonRoomscaleNeckMetres=0.15 estimates the horizontal neck-to-HMD pivot.

1. Load a level, stand comfortably, press END and check head height/stereo.
2. With both sticks centered, move physically left/right, forward/backward,
   then diagonally. Lara's body should drag with you without starting a walk
   animation. Test slowly and briskly. Stopping should stop displacement.
3. Turn physically 90/180/360 degrees while staying in place. Lara should turn
   without a forward walk, and the view should remain centred in her body at
   intermediate angles rather than returning only at 360 degrees. Repeat
   physical side steps after each rotation.
4. Repeat with right-stick turns and combined physical/artificial rotation.
   Lean sideways while turning: the eye should not orbit the old neutral or
   drift off-centre from Lara before the rotation reaches 360 degrees.
5. Test manual forward, backward, left and right separately after physical and
   artificial turns. Lara should face the HMD: forward uses her forward gait,
   backward uses backpedal, and left/right use the native sidestep animations.
   Side/back motion must travel in the selected world direction rather than
   creeping forward, and should be close to forward-running pace. The body,
   animated head and camera must remain as smooth as forward movement, without
   frame skipping. Test diagonals on both sides; the stronger axis should
   select the gait.
6. Hold manual forward and jump from standing and running at several physical
   and artificial headings. Check preparation, launch, flight and landing. A
   forward jump should continue in the HMD-forward direction. Also verify that
   lateral/back input during jump preparation selects the native side/back jump.
7. Combine physical dragging with analog-stick movement. Manual travel must
   not consume the tracked offset or add a second copy of the physical step.
8. Approach walls, corners, doors, stairs and ledges slowly. Body drag should
   respect collision and room transitions. Large drops and insufficient
   headroom block dragging. Tracked camera leaning can still enter geometry.
9. Open/close inventory, use R3 D-pad shift, enter fixed cameras, climb, operate
   switches, jump and swim. Returning to ground must not release queued drag.
10. Test both Modern and Tank controls for physical body dragging and the four
    manual first-person directions.
11. Check TR1, TR2 and TR3. Inspect locomotion logs: manual stays zero during
    pure physical movement; drag reports accepted physical movement in metres;
    head and cam agree throughout compression (state 15) and forward flight (3).

The neck-pivot correction is an estimate from the headset. If turning in place
still translates the body, adjust FirstPersonRoomscaleNeckMetres and recenter.
Zero disables that correction. The tests cannot substitute for headset play.
