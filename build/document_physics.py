from pathlib import Path
p=Path('src/BoneSkin.cpp'); s=p.read_text(); a=s.index('// WHICH SHADER.'); b=s.index('const char kAnchor',a)
s=s[:a]+'''// TR1-3's three-weight skin sources use one model-space aCoord, joint indices
// in aLight.xyz, and weights in aColor.xyz. Match source structure instead of
// hard-coding TR4/5's program indices. The physics self-test compiles all eight
// distinct matching sources in each supported TR1-3 executable.
// Two-matrix shaders use per-joint coordinates and must not receive this mask.
// Patch after the third weighted contribution, before projection/fog outputs.
'''+s[b:]; p.write_text(s)
p=Path('src/DynamicBones.cpp'); s=p.read_text(); a=s.index('// FINDING THE BODY'); b=s.index('struct Capture',a)
s=s[:a]+'''// DrawLaraHD also draws hands, face, and attachments. Preserve the TR4/5
// discriminator: a body has many distinct joint transforms, while attachments
// repeat a few. TR1-3 always uploads 32 slots, so score only Lara's 15 skeleton
// slots; stale/padded slots 15..31 must never win the comparison.
'''+s[b:]
a=s.index('    // Score this draw'); b=s.index('    float extent',a)
s=s[:a]+'''    // Rank each draw within Lara's scope, ignoring the GPU palette padding.
    const int n = kLaraJoints;
'''+s[b:]
a=s.index('    // A body-like draw'); b=s.index('    g_drawIsBody =',a)
s=s[:a]+'''    // Same majority-of-distinct-joints discriminator as the TR4/5 source.
'''+s[b:]
a=s.index('    // A SKELETON PALETTE'); b=s.index('    const bool full',a)
s=s[:a]+'''    // Prefer a fully populated skeleton over repeated attachment transforms.
'''+s[b:]
p.write_text(s)
Path('src/DynamicBones.h').write_text('''// DynamicBones.h -- Lara-only damped spring chest motion for TR1-3.
// Ported from TR4/5 with TR1-3 item offsets and a fixed 32-slot GPU palette.
#pragma once

namespace tr {
// Run after GameDllUpdate at the frame boundary. Rebind the Lara draw hook,
// reset across games/levels, and advance once from the captured torso pose.
void DynamicBonesUpdate();
void DynamicBonesShutdown();

// Bracket every validate_draw. Observe before editing; restore after upload.
// Only the first 15 skeleton slots participate in body classification.
void DynamicBonesObserveDraw();
void DynamicBonesApplyToDraw();
void DynamicBonesRestoreDraw();
bool DynamicBonesAppliedToDraw();

// Shader-path inputs. Body selection is scoped to DrawLaraHD; the solved
// world offset has the gravity equilibrium removed, so rest keeps the mesh.
bool DynamicBonesRenderBody();
bool DynamicBonesWorldOffset(float out[3]);
bool DynamicBonesTorsoFrame(float out[12]);
bool DynamicBonesLaraForward(float out[3]);
bool DynamicBonesDisplacement(float out[2][3]);
} // namespace tr
''')
Path('src/BoneSkin.h').write_text('''// BoneSkin.h -- per-vertex chest deformation for TR1-3 HD Lara.
// Fits a front-of-torso region from the live mesh and patches three-weight
// skin shaders. Supported stock, retail, and Gold hooks are verified.
#pragma once

namespace tr {
// Install before the engine compiles shaders; failure keeps the rigid fallback.
void BoneSkinInstall();
void BoneSkinShutdown();
// Forget mesh calibration on game/level/context changes.
void BoneSkinResetRegion();
// True after a body draw has reached a patched program with a fitted region.
bool BoneSkinActive();
// Run after program/VAO binding and before glDrawElements. Clear persistent
// uniforms on non-body draws, and avoid double motion on the fallback handoff.
void BoneSkinAfterValidate(bool jointApplied = false);
} // namespace tr
''')
p=Path('src/Config.cpp'); s=p.read_text(); anchor='    // A zero or negative interval'; s=s.replace(anchor,'''    LogF("config: chest physics=%s shader=%d apply=%d drive=%d strength=%.2f",
         g_cfg.dynamicBones ? "on" : "off", g_cfg.dynamicBonesShader,
         g_cfg.dynamicBonesApply, g_cfg.dynamicBonesDriveMode,
         g_cfg.dynamicBonesChestStrength);

'''+anchor); p.write_text(s)
# Reuse portable toolchain discovery from the existing runner.
runner=Path('tests/build_selftest.cmd').read_text()
a=runner.index('cl /nologo'); b=runner.index('set RC=%ERRORLEVEL%',a)
runner=runner[:a]+'''cl /nologo /std:c++17 /permissive- /EHsc /W4 /MT /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I "%ROOT%\\src" "%ROOT%\\tests\\physics_selftest.cpp" "%ROOT%\\src\\GL.cpp" "%ROOT%\\src\\InlineHook.cpp" "%ROOT%\\src\\Log.cpp" /Fe:physics_selftest.exe /link opengl32.lib user32.lib gdi32.lib
if errorlevel 1 goto build_failed

if not "%~1"=="" goto supplied_images
.\\physics_selftest.exe "%ROOT%\\PDB\\tomb123.exe"
goto finished
:supplied_images
.\\physics_selftest.exe %*
:finished
'''+runner[b:]
Path('tests/build_physics_selftest.cmd').write_text(runner)
p=Path('README.md'); s=p.read_text(); marker='- All three games (TR1, TR2, TR3) through one build.'; s=s.replace(marker,marker+'\n- HD Lara chest physics ported from TR4/5, with the same landing response and\n  tuning controls. Enabled by default; see [chest physics](docs/chest-physics.md).'); p.write_text(s)
Path('docs/chest-physics.md').write_text('''# Chest physics

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
''')
print('Documentation and portable test runner updated')
