# Chest physics port recovery note — 2026-09-23

The requested TR4/5 to TR1/2/3 chest-physics port is implemented, built, and deployed.

- Release DLL: build/x64/Release/TombRaiderVR.dll
- Installed folder: C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider I-III Remastered
- Installed SHA256: D4DCB51D0339652811B56722F8949A9FC04E3E6942A5AEA6AD4B45281D539095
- Previous DLL backup: C:\dev\TombRaider123VR\build\pre-chest-physics-20260923-005722
- Existing installed INI preserved byte-for-byte. No DynamicBones overrides were present, so enabled defaults apply.
- 1029 address/layout/hook checks passed across stock, retail, and Gold.
- 613 production physics/GL checks passed, including eight distinct patched shader sources in EACH executable variant.
- Existing tests/build_selftest.cmd passed with zero failures. Final Release build succeeded; git diff --check passed.
- Code and controls documented in docs/chest-physics.md. No commit made.

Next: user headset testing of HD Lara's chest motion and outfit fitting in all three games. No game runtime/headset test was performed in this session.

Port details: shader_init is FIVE arguments (Shader*, cull, fvf, vs, fs); stock RVA EC10, retail/Gold EC40. DrawLaraHD stock RVAs 6D9A0/9FC80/E9FA0; retail/Gold 6DDB0/9FB40/EBCB0. ITEM_INFO fallspeed +36, pos.y_rot +102, uint16 gravity_status bit 3 at +484. GPU palette is fixed 32 joints; classify first 15. Reset region on game/level/item/context changes, restore temporary joints and dirty upload, and avoid double deformation on fallback transition.

Original pending files from the crashed session were four untracked physics source files and changes to tools/port_build.py and tools/verify_addresses.py. The old full matcher had crashed; tools/port_physics_addresses.py extends the existing build/current-*-verify manifests with focused function matching. These directories and JVM crash logs predated this recovery. Build outputs were already tracked by this repository and now reflect the new DLL.
