# Tomb Raider Remastered VR prototype

Milestone 2: OpenXR headset output and tracked 6DoF camera are implemented. Tested with SteamVR/OpenXR on 2026-09-08; the user confirmed the game is visible in the headset. TR1 only, remastered graphics. The previously confirmed SBS prototype is preserved in `milestone1/`.

Milestone 5: TR2 support, confirmed working in the headset on 2026-09-08. Every module-relative address the hooks use lives in a per-game profile, and each game DLL is hooked as it loads.

Milestone 6 (not yet tested in a headset): a TR3 profile on the same footing, so stereo, headtracking and the camera modes should work across all three games.

Milestone 4 (not yet tested in a headset): three camera modes - classic, over the shoulder, and first person - plus an in-headset options panel on its own quad layer.

Milestone 3: frames drawn outside the gameplay loop - title screen, menus, load screens, cutscenes - are presented on a world-locked virtual screen instead of being dropped. Tested with SteamVR/OpenXR on 2026-09-08; the user confirmed the virtual screen presents correctly in the headset.

## Launch

Start SteamVR and connect/wake the headset, then run:

```powershell
powershell -ExecutionPolicy Bypass -File ".\launch.ps1"
```

Select TR1 and enter Lara's Home. Select its photograph with Enter, then confirm with Enter again. OpenXR starts automatically. Menus and other non-gameplay frames appear on the virtual screen in the headset; the monitor still shows the same image, so either display can be used to navigate them.

The launcher checks the executable/TR1/TR2/TR3 SHA-256 hashes before loading the workspace DLL. No mod files are installed into the game directory. All hooks are process-local. Close and restart the game normally to remove them. Restart before loading a newly built DLL. Switching between TR1 and TR2 in-session no longer needs a restart: each game DLL is hooked when it loads.

## Eye resolution

Gameplay renders directly into per-eye color/depth framebuffers at OpenXR's recommended eye dimensions, independent of the desktop window. With SteamVR reporting 4808 x 4904, both source eyes now render at 4808 x 4904 instead of upscaling a 2560 x 1440 window image. The desktop mirror fits these images into its two halves. Cinema frames retain the window resolution.

The bridge polls OpenXR's recommended dimensions once per second and replaces both swapchains when the runtime reports a change. If SteamVR keeps reporting the old size after a settings change, toggle F8 off/on to reinitialize OpenXR. Live settings-change behavior still needs headset validation. Higher resolutions increase GPU work; the first TR2 classic runtime test at 4808 x 4904 ran around 30-32 fps.

`research/test_native_eye.cmd` verifies rendering beyond the window bounds, growing/shrinking targets, recovery after window capture overwrites a shared texture, and host/framebuffer/viewport/scissor restoration. Live TR2 logs confirm both source eyes match the 4808 x 4904 runtime swapchains; headset visual confirmation is pending.

## Controls

| Control | Action |
|---|---|
| Headset movement | Tracked position and rotation, including leaning |
| F7 | Recenter headset position/yaw and clear manual camera offsets |
| F8 | Toggle OpenXR; toggling off/on also retries runtime initialization |
| F6 | Toggle monitor-only SBS; disable OpenXR first to use this mode alone |
| F9 | Force the virtual screen during gameplay, for in-engine cutscenes that render through the gameplay loop |
| F10 | Open/close the VR options panel |
| F11 | Cycle camera mode: classic, over the shoulder, first person |
| Numpad 4 / 6 | Camera left / right |
| Numpad 2 / 8 | Camera backward / forward |
| Numpad + / - | Camera up / down |
| Numpad 1 / 3 | Yaw |
| Numpad 7 / 9 | Pitch |
| Numpad * / / | Roll |
| Shift | Faster manual translation |

While the options panel is open the numeric keypad drives it instead of the manual camera: 8/2 select a row, 4/6 change the value, 5 activates. Every row is also an ini key, so the panel is for tuning and the ini is for the value you settle on - panel changes are not written back.

Keyboard hotkeys require game focus. Use Num Lock for numeric keypad controls. Normal gameplay controls remain available. Camera modes include tracked third person and head-anchored first person; motion controllers are not implemented. Recenter while looking in your intended neutral direction. Recenter preserves physical pitch/roll so the tracking frame remains gravity-aligned; the gameplay camera itself can still pitch and rotate.

Optional settings: copy `tombvr.example.ini` to `build/tombvr.ini` before launching. `OpenXR=1` enables automatic initialization; `OpenXR=0` starts with it disabled. `UnitsPerMeter=512` is the initial world-scale estimate, adjustable from 64 to 4096. It is not a measured physical calibration. `CinemaDistanceCm=250` and `CinemaWidthCm=300` set the virtual screen distance and width in centimetres; the height follows the game image aspect ratio.

## Game profiles

`tomb1.dll`, `tomb2.dll` and `tomb3.dll` are the same engine built at different offsets, so every
module-relative address the hooks touch is gathered into a `Profile` in
`src/prototype.cpp` rather than spelled out at the use site.

The game DLLs are `LoadLibrary`'d as the player picks a game, and an earlier one
can stay loaded afterwards, so there is no single "current" DLL to bind to.
`install()` therefore re-checks every `SwapBuffers` and hooks **each** supported DLL
as it appears, at its own base. Because every patched call site belongs to exactly
one module, entry runs through a per-game thunk that selects that game's profile
and its original function pointers - which is what makes the active game
unambiguous no matter which DLLs are resident. Binding once to whichever DLL was
loaded first is the bug this replaced: it always found TR1 and left TR2 unhooked.

Host addresses are not profiled: `tomb123.exe` is the same module for all three
games, so the projection dispatch and indexed-mesh filter are installed once.

`research/verify_call_sites.py` re-checks statically that every call site a profile
names really does hold an `E8 rel32` to the function it claims - the same test
`patchCall` applies at runtime. Run it after changing any profile.

The TR2 addresses were derived by matching instruction streams between the two
DLLs; the method, the full table and the confidence for each entry are in
`research/tr2-addresses.md`, and the tools that produced it sit beside it. They
were cross-checked against an independently produced set of matches and agree
everywhere the two overlap. TR2 stereo and the corrected head-anchored first-person path have since been confirmed working by the user.

Two safeguards apply, because the two kinds of address fail differently. `patchCall`
already verifies that a call site really targets the function it names, so a wrong
*code* address refuses to patch and logs, leaving that game in cinema-only
behaviour rather than crashing. A wrong *data* address has no such protection and
would be a silent bad write, so `verifyProfileOnce` checks that the profile's
world-to-view global actually holds a rotation - rows within 5% of unit length at
the 1/16384 fixed-point scale - and logs `SUSPECT` if not. Check the log for that
line before trusting a new profile.

`research/map_game.py <n>` derives a profile for a game from the TR1 reference and
prints the row to paste in; it is what produced the TR2 and TR3 entries.

## Camera modes

All three run through the same hook, which receives the engine's already-interpolated render camera pose immediately before the world-to-view matrix is generated. Only that pose is rewritten; the renderer re-resolves the camera's room from it, so no room bookkeeping is needed. Fixed and cinematic cameras (`CAMERA_INFO.type` 1 and above 3) are left alone in every mode, because those framings are placed deliberately by the level.

- **Classic** is the game's own third-person camera, unchanged.
- **Over the shoulder** moves the camera along its own view axis until it is `ShoulderDistance` units from Lara, then offsets it by `ShoulderRight`/`ShoulderUp`. It only ever moves *closer*: the engine has already collision-checked the longer ray, so pulling in along it cannot push the camera through geometry. The view angles are left as the game set them, which is what produces the off-centre framing.
- **First person** anchors the stereo midpoint to Lara's animated head (joint 14), using the same previous/current bone and body interpolation as the renderer. The anchor is a point `(0, -32, 16)` inside the head relative to its neck pivot. Lara's body yaw supplies turning, while the headset supplies relative orientation; animation pitch/roll do not tilt the view. Headset translation and manual camera offsets are ignored in this mode, while rotated per-eye separation preserves stereo depth. The old fixed `EyeHeight`/`EyeForward` controls are no longer used.

Head hiding is active only for a successfully anchored first-person gameplay eye. Classic graphics skips the head mesh; remastered graphics draws cached index buffers with triangles influenced by head bone 14 removed. The separate animated-face, eyewear and hair draws are skipped entirely, because the facial mesh uses additional joints beyond head joint 14. The native close-camera whole-body hiding check is bypassed only around Lara's first-person draw, allowing the remaining body to render. Original index buffers and GL bindings are restored after every filtered draw. Classic/shoulder cameras, menus, fixed cameras and other characters keep their normal rendering. F11 cycles the same camera selection exposed by F10.

Tests: `research/test_firstperson.cmd` covers animated anchor interpolation, native rounding, translation cancellation and rotated stereo baselines across 1000 poses. `research/test_firstperson_mesh.cmd` uses a synthetic skinned mesh to verify head removal, retained body indices, cache reuse, restored GL bindings and inactive-mode pass-through. Both pass, as do the existing VR math tests. The user confirmed the head-anchored 3DoF camera and classic head hiding in the headset. The initial remastered filter missed separate facial draws; the revised path filters Lara's skeletal submissions and suppresses dedicated face/eyewear draws entirely; the user confirmed the corrected TR1 face hiding in the headset.

Lara's position and rotation are interpolated with the same factor the engine uses for the camera, so the derived modes do not judder at tick boundaries. Her current pose is at `ITEM_INFO+0x58`; the previous pose is taken to be the adjacent 20-byte block at `+0x6c`, which matches the layout the engine's own position interpolation uses. The rotation half of that inference is guarded: an implausible per-tick turn falls back to the uninterpolated rotation.

## Options panel

`F10` shows a panel drawn with GDI into a bottom-up DIB, uploaded as a texture and submitted as a second quad layer above whichever main layer the frame is using.

The panel bitmap is uploaded in the mod's shared OpenGL context. Uploads explicitly save, clear and restore pixel buffer bindings and pixel row/skip settings, and a failed upload remains dirty for retry. The game context and its texture bindings are restored before gameplay resumes. The first four uploads check GPU pixels against the bitmap; the first four menu swapchain copies check source and destination pixels. Live checks matched across all three runtime swapchain images with no GL errors in those checks. The user confirmed that the F10 panel and the Escape menu work in the headset in TR1 Lara's Home.

The panel is world-locked like the virtual screen, re-anchored in front of the viewer each time it is opened and on recenter. Its bitmap is redrawn only when a value changes. `research/test_menu.cmd` tests bitmap uploads with nondefault pack/unpack settings and bound pixel buffers, including state restoration and changed menu values.

## Virtual screen

The gameplay hook only covers the TR1 room render. Every other presented frame - the trilogy launcher, title and option menus, load screens, and cutscenes that do not use the gameplay draw loop - is captured from the game backbuffer at `SwapBuffers` and submitted as a mono `XrCompositionLayerQuad`, with no projection layer and an opaque black surround.

The screen is world-locked in the `LOCAL` reference space: it is placed at eye height, squared to the head's heading, and does not follow head rotation. It re-anchors in front of the viewer whenever the cinema is entered (that is, whenever a run of non-gameplay frames begins) and on F7. Its swapchain is created lazily at the game backbuffer resolution and recreated if that resolution changes.

In-engine cutscenes that do render through the gameplay draw loop stay in stereo with the game's own cutscene camera. F9 forces those onto the virtual screen instead; it suppresses the stereo path for as long as it is on.

## Verified

- SteamVR advertises XR_KHR_opengl_enable and requires OpenGL 4.3 or newer. The game creates OpenGL 3.2, so the bridge creates a shared OpenGL 4.3 context for OpenXR.
- Both runtime eye swapchains were created successfully, using GL_SRGB8_ALPHA8. In this runtime configuration they are 4808 x 4904 each. The game eye render is currently copied/upscaled to that size; this does not increase source detail.
- Session reached VISIBLE and FOCUSED. Thousands of projection-layer frames were accepted, with varying headset position/orientation. The user confirmed headset game visibility.
- Native headset eye poses supply IPD exactly once. Main-world projection uses each runtime eye's asymmetric FOV. The original depth coefficients are preserved.
- CPU camera transformation precedes the main room visibility pass. A conservative symmetric CPU culling projection contains each asymmetric eye view.
- Math tests cover translation-axis conversion, eye offset/IPD, yaw direction, recentering, more than 3000 orientation round trips, both signs of perspective W, asymmetric FOV edges, and unchanged depth coefficients.
- Recorded stereo samples retained the same simulation tick and Lara position across both eye renders. No OpenXR errors were found in the saved runtime log.

The virtual screen is a mono quad composition layer in `LOCAL` space, submitted with no projection layer. Image orientation, facing and re-anchoring were confirmed by direct observation in the headset, not by an automated check.

Evidence: `research/openxr-validation.json`, `research/openxr-test.log`, `research/openxr-mirror.jpg`. These are development observations, not exhaustive compatibility or comfort validation.

## Classic graphics fix

Classic graphics (F1) now enables the engine's existing smooth-render interpolation flag during stereo rendering, restoring its previous value afterward. Both eyes retain the same simulation state; the fix does not raise the simulation tick rate. Classic room clipping rectangles are reprojected from the CPU visibility projection into each headset eye's asymmetric projection, with outward rounding to avoid cutting visible geometry. Orthographic UI passes keep their original clipping.

The scissor callback is installed only after the engine API table is initialized, avoiding an early-startup null-pointer access. `ClassicFix=0` in the `[VR]` ini section disables the new behavior for comparison; the default is `1`.

Validation: the user confirmed smoother movement and restored geometry in TR1 Lara's Home. The rebuilt mod starts successfully after the initialization-order fix. Classic clipping tests pass 20,000 visible-ray containment cases; existing headset coordinate/projection tests also pass. An exact sustained 60 fps headset rate has not been independently measured. Other levels and TR2/TR3 remain unvalidated.

## Implementation

- `src/prototype.cpp`: validated call-site hooks, CPU camera and headset-projection integration, camera modes, independent SBS mode, input and eye capture.
- `src/vr_menu.cpp`: GDI panel rendering and keypad navigation over a table of settings.
- `src/firstperson_math.h`: animated head anchor and rotation-only stereo offsets.
- `src/firstperson_mesh.cpp`: cached removal of head-influenced triangles during Lara's first-person skeletal draws.
- `src/xr_bridge.cpp`: OpenXR initialization/events, shared GL context, predicted-display-time poses, swapchain acquisition/copy/release and submission.
- `src/xr_math.h`: coordinate, orientation and projection conversions.
- `src/inject.cpp`: x64 process-local DLL loader.
- `launch.ps1`: hash-checked launcher.
- `build.cmd`: MSVC x64 build. Close the game before overwriting a loaded DLL.
- `research/test_math.cmd`: compile and run the coordinate/projection tests.

Dependencies are copied locally under `deps/openxr/`; `build/openxr_loader.dll` is loaded by absolute path before using the delay-loaded API. Khronos references: https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetOpenGLGraphicsRequirementsKHR.html and https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrLocateViews.html .

Frame order: poll events, wait/begin OpenXR frame, locate both predicted views, render left and right without a second simulation update, capture both game images, copy through the shared context, release the XR images, then submit the projection layer. Each layer view carries the exact raw pose/FOV used to construct its camera. Manual/game-camera movement defines the game-world anchor; recentered tracking adds head/eye transforms. First-eye game timing and presentation remain suppressed as in milestone 1. No GPU depth layer is submitted.

## Remaining limitations

- Camera collision and controller aiming remain separate work. First-person head hiding is implemented; broader body/neck treatment remains to be tested.
- The camera modes need broader headset testing. First person in particular has not been checked against swimming, climbing, vehicles or cutscenes.
- First-person mesh filtering needs wider outfit/level coverage, particularly the neck boundary of remastered skinned meshes.
- First person follows the animated head position, including its bob. Optional bob reduction is not implemented. Swimming, climbing and scripted sequences need further visual testing.
- The panel is keyboard-driven and appears only in the headset; there is no motion-controller interaction and no pointer. Changes are not saved back to the ini.
- Which sequences reach the gameplay draw loop and which do not has not been surveyed, so it is not yet known which cutscenes appear in stereo and which appear flat. F9 forces any of them flat.
- FMV playback is only presented if the video reaches the same OpenGL backbuffer through `SwapBuffers`. This has not been confirmed against an actual FMV. If the game presents video by another path, the headset will hold the last captured image.
- The default screen size and distance were not tuned against hardware feedback; `CinemaDistanceCm` and `CinemaWidthCm` remain the adjustment.
- If the game stops presenting entirely, for example while blocking on a load, no OpenXR frames are submitted and the runtime will show its own hold or fade. Frame submission is not driven from an independent thread.
- In gameplay, UI remains baked into each eye image. A dedicated VR HUD and a 3D-projected menu are separate work; the virtual screen is a flat presentation surface, not an interactive VR menu.
- Shadow cameras, mirrors, underwater effects, large head offsets, other levels, scripted sequences and classic graphics need further testing. Per-eye culling covers the main room pass, not a complete audit of all auxiliary passes.
- GL synchronization currently uses glFinish for correctness, and full-size eye images are copied. Performance tuning, swapchain resolution controls and direct render targets remain future work.
- Runtime loss falls back to desktop; F8 off/on retries. Long-session runtime-loss/reconnection coverage is incomplete.
- Only the installed, hash-matched TR1, TR2 and TR3 builds are supported. The hook remains a development prototype.
- TR3 is untested in a headset. Its code addresses are verified statically by `research/verify_call_sites.py` and fail safe if wrong; its data addresses are guarded only by the world-to-view check, so read the log before playing.
- First person and head hiding are mapped for TR1 and TR2. TR2 now includes its Lara/object dispatch, classic head mesh, hair and separate remastered facial submissions. TR3 head hooks remain unmapped, so selecting First Person there retains the original camera; fixed eye offsets do not bypass that guard. TR2 headset validation of the new first-person hooks is pending.
- The nine skinned-draw call sites that TR1 uses for head filtering have no TR2 equivalents located yet.
