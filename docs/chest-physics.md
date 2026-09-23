# Chest physics

HD Lara in TR1, TR2, and TR3 uses the TR4/5 mod's damped spring solver and
per-vertex chest mask. The defaults match that implementation: stiffness 630,
damping 9.5, landing impulse 0.2, and chest strength 2.5. Existing INIs without
these keys use those defaults. Classic graphics are not deformed.

Add settings under the existing `[VR]` section in `TombRaiderVR.ini`:

```ini
DynamicBones=1
DynamicBonesApply=1
DynamicBonesShader=1
DynamicBonesChestStrength=2.5
```

`DynamicBones=0` disables the feature. Strength scales the visible bounce.
The default engine-state drive responds to landings using Lara's airborne flag
and deepest fall speed. `DynamicBonesAirGravity=1` keeps takeoff motion quiet.
`DynamicBonesDriveMode=0` selects the source mod's filtered acceleration drive.
Restart the game after changing settings.

The shader fits a chest region from the live mesh and fades deformation toward
the shoulders, back, and waist. It takes about 30 body draws to establish the
front direction. The source implementation's whole-torso fallback remains active
until the shader and region are ready; `DynamicBonesShader=0` forces that debug
view. `DynamicBonesApply=0` measures without changing the mesh.

## Validation

The port uses TR1-3's five-argument `shader_init`, the actual `DrawLaraHD` scope,
ITEM_INFO offsets, 74-program table, and fixed 32-slot joint upload. Body scoring
uses the first 15 skeleton slots, excluding padding. Stock and current retail/Gold
hook windows are checked by `tools/verify_addresses.py`. To extend an existing
manifest with the two new symbols without rematching all functions, run:

```powershell
python tools/port_physics_addresses.py build/current-retail-verify build/current-gold-verify
python tools/verify_addresses.py build/current-retail-verify build/current-gold-verify
cmd /c tests/build_selftest.cmd
cmd /c tests/build_physics_selftest.cmd
```

The physics test exercises the production solver at 60/90/120 Hz, reads synthetic
TR1-3 item state, checks NPC/attachment rejection and palette restoration, and
compiles the actual patched vertex sources using a hidden OpenGL context. The
runner accepts additional absolute executable paths to test retail and Gold too.

## In-game check

Visual headset testing is still required for the actual outfits. In each game,
try standing and running jumps, a long fall, walking, and turning. Confirm that
landings bounce while walking/turning remain quiet; the back and shoulders should
stay attached. Check first person and third person, classic/HD switching,
inventory, loading a save, changing outfits, and switching games.

Look for `dynbones: hooked`, the `boneskin:` shader compilation summary, mesh
measurements, and `boneskin: live` in `TombRaiderVR.log`. If a region looks wrong,
`DynamicBonesRegionDebug=30` temporarily pushes that region forward for inspection;
return it to 0 afterward. Report the game/outfit and log for tuning.
