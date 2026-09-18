# Tomb Raider I-III Remastered VR — developer source package

This is a source-only package for developer review and collaboration. It contains the injected VR mod, its OpenXR build dependency, selected automated test sources, and no Tomb Raider game files or content assets.

## Included

- `src/`: the injector, game hooks, stereo camera, OpenXR bridge, menu, first-person mesh filter, and native-resolution rendering path.
- `deps/openxr/`: OpenXR headers and the loader import library used by the current build. The headers identify their license as `Apache-2.0 OR MIT`.
- `research/`: source for focused math, mesh, menu, and native-eye tests.
- `build.cmd`, `launch.ps1`, `tombvr.example.ini`, and the main README.

## Deliberately excluded

- Tomb Raider executables, DLLs, data files, textures, audio, video, or other game assets.
- Compiled injector/mod binaries and the OpenXR runtime loader DLL.
- Game captures, screenshots, logs, Ghidra databases, and copied reverse-engineering inputs.

## Build and run

Install the Visual Studio 2022 C++ x64 toolset and use an OpenXR runtime such as SteamVR. Run `build.cmd` from the package root; it writes outputs to `build/`. Then run `launch.ps1`, optionally passing `-GameDirectory` for a non-default legitimate game installation.

The mod is binary-build-specific: `src/prototype.cpp` contains fixed offsets and `launch.ps1` checks exact SHA-256 hashes before injection. It supports the matched Steam build only; a future game update will require remapping and validation. The package does not grant a license for Tomb Raider content or include any of it.