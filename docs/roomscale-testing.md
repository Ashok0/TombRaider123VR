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
   With guns holstered, look down at the torso at 45, 90, 180 and 270 degrees
   in both directions. Repeat after a fixed-camera handoff. A full-circle
   endpoint alone cannot detect the intermediate-angle centering regression.
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
12. Die from enemy damage and a fall. The native death camera should take over
    immediately, with Lara's full body visible; loading a live save should
    restore first person and normal hand placement.
13. Hang, shimmy, release, fall, land and regrab repeatedly. Check both arms and
    shoulders during the transition, then draw/holster guns and switch views.
    No skin should stretch toward the old ledge or the room origin. Repeat with
    motion guns disabled and check that nearby enemies remain fully visible.
14. Stand at the very edge of a crate facing forward, sideways and backward.
    Turn physically and with the stick, then jump off and land. Repeat several
    times without toggling views. The torso should stay aligned; looking over
    the edge must not retract the eye as though the empty drop were a wall.
    Test real walls and low ceilings too. Logs should not show a stationary
    headset's pending roomscale offset growing toward two metres at an edge.

The neck-pivot correction is an estimate from the headset. If turning in place
still translates the body, adjust FirstPersonRoomscaleNeckMetres and recenter.
Zero disables that correction. The tests cannot substitute for headset play.

For B-roll turnaround, roll from standing and while holding forward in first
person. The view should turn 180 degrees once, and forward should follow Lara's
new facing direction. Repeat twice to face the original direction. Repeat after
physical and stick turns, with a sideways lean, and near a wall or ledge. Check
that a cancelled roll does not flip the view and that third-person rolls remain
unchanged. The body should reappear centered when its roll visibility ends.
