## Tomb Raider I-III Remastered VR Mod

Native stereoscopic 3D with 6DOF head tracking for *Tomb Raider I-III Remastered*
(`tomb123.exe`), covering all three games.

This is a port of the [TR4-6 VR mod](https://github.com/Ashok0/TombRaider456VR)
to the earlier remaster. The stereo architecture is the same one; what is new is
that Tomb Raider I-III Remastered **ships private PDBs next to the executable**,
so almost none of this was reverse engineering in the usual sense. Where the
TR4-6 work read a decompiler, this reads the compiler's own symbols and type
information.

## AI Usage

Written with Claude Code (Opus 5). Every address, struct offset and function
signature in `src/Engine.h` and `src/GameDll.cpp` was extracted from the shipped
PDBs by the tools in `tools\`, and `tools\verify_addresses.py` re-derives all
139 of them and fails if any disagrees.

## VR Mod Features

- Native stereo 3D, 6DOF head tracking, per-eye asymmetric frustums.
- All three games (TR1, TR2, TR3) through one build.
- World-locked HUD and inventory rather than a flat overlay pinned to your face.
- FMV cutscenes captured offscreen and replayed as real geometry, so they
  keystone and roll correctly instead of sitting flat.
- Touch controllers presented to the game as an Xbox pad, with decoupled
  head/aim pitch.
- Ceiling clearance clamp, so standing up in a crawlspace does not put your head
  through the ceiling.
- Live IPD and world-scale tuning on the numpad.

## Installation

Copy three files into the game folder
(`steamapps\common\Tomb Raider I-III Remastered\`):

| file | what it is |
|---|---|
| `winmm.dll` | the proxy that loads the mod |
| `TombRaiderVR.dll` | the mod |
| `openvr_api.dll` | OpenVR runtime, x64 — SteamVR ships one at `steamapps\common\SteamVR\bin\win64\` |

Launch from Steam as normal. `TombRaiderVR.ini` is written next to the DLL on
first run and is heavily commented; that file, not this one, is the reference
for the settings.

## Status

Verified working: the mod loads, binds, installs all six hooks, brings up
OpenVR, creates the stereo target and submits stereo frames with correct per-eye
separation. What has **not** been done is a full playthrough of all three games,
so per-level and per-cutscene issues are likely to remain.

One TR4-6 feature is deliberately **not** carried over: the portal/room-culling
expansion that stopped geometry disappearing when you look away from the game
camera. See "Not carried over" below.

---

## Development Notes

### Why this port was mostly transcription

`tomb123.exe` and `tomb1/2/3.dll` all ship with private PDBs. That changes the
character of the work completely:

```
python tools\pdbdump.py  PDB\tomb123.exe                  name -> RVA, 2815 symbols
python tools\typedump.py PDB\tomb123.exe RenderState      full struct layouts
python tools\funcsig.py  PDB\tomb123.exe ogl_draw         real C prototypes
python tools\disasm.py   PDB\tomb123.exe validate_draw    symbol-annotated asm
python tools\xrefs.py    PDB\tomb123.exe                  who calls whom
python tools\verify_addresses.py                          re-derive and diff everything
```

`pdbdump.py` and `typedump.py` drive `dbghelp.dll` through `ctypes` — no SDK, no
external dependency. `disasm.py` and `xrefs.py` add `capstone` and `pefile`.

The whole mod was built with these alone — no decompiler. That worked *because*
of the PDBs; see "Ghidra / re-mcp" below for where it stops working.

The first three questions that decided whether the port was viable at all were
answered in about ten minutes:

- `tomb123.exe` imports **OPENGL32.dll** and **WINMM.dll** — same renderer, same
  injection vector as TR4-6.
- Its PDB contains `vid_setPass`, `validate_draw`, `ogl_draw`, `ogl_present` and
  `fmvShow` — the same engine, by the same names.
- `mProj` is still `mat4[2]` with `[1]` the perspective matrix, and
  `mView_packed` is still a row-major 3×4 affine in 1/16384 fixed point with its
  third rotation row negated.

So `StereoMath.h`, `VRSystem`, `StereoRenderer`, `InlineHook`, `GL`, `Config`,
`Gamepad`, `VideoPanel` and the `winmm` proxy carried over essentially
unchanged. `Engine.h`, `Engine.cpp` and `Hooks.cpp` were rewritten.

### How TR1-3's engine differs from TR4-6's

This is the part that matters if you are porting anything else between them.

| | TR4-6 | TR1-3 |
|---|---|---|
| `RenderState` | 240 bytes, 29 members | **152 bytes, 21 members**, different offsets |
| | separate `vb` / `ib` | one `mesh` pointer |
| `Shader` | 80 bytes, `uid[18]`, 202 shaders | **60 bytes, `uid[12]`, 74 shaders** |
| draw entry points | `ogl_draw` **and** `ogl_drawVB` | **`ogl_draw` only** |
| `vid_setPass` | 4 args (incl. `blend`) | **3 args** |
| frustum shear lever | `vid_setPerspOffset` | **does not exist** |
| render-target latch | `ogl_rt` global | **does not exist** — must be hooked |
| offscreen scene | TR6 only, needed alternate-eye | **none** — all three draw to the backbuffer |
| `consts` fog bit | bit 5 | **bit 3** |
| `consts` contacts bit | — | **bit 4** |

The `consts` dirty-flag table was the one piece that genuinely had to be read out
of the disassembly, because it is encoded in `validate_draw`'s control flow
rather than in any type. Each bit test was matched to the `uid[]` index and the
`vid_state` member its guarded `glUniform*` call used:

```
bit  test site   uid   vid_state member    upload
 0   0x0000F086  [0]   proj      (+72)     glUniformMatrix4fv(n=1)
 1   0x0000F0B3  [1]   view      (+80)     glUniform4fv(n=4)
 2   0x0000F0E5  [2]   shadow    (+88)     glUniformMatrix4fv(n=1)
16   0x0000F119  [3]   model     (+96)     glUniform4fv(n=4)
17   0x0000F14C  [4]   params    (+104)    glUniform4fv(n=1)
 3   0x0000F17F  [5]   fogColor  (+112)    glUniform4fv(n=1)
 4   0x0000F20A  [6]   mContacts (global)  glUniform4fv(n=16)
18   0x0000F22E  [7]   joints    (+120)    glUniform4fv(n=96)
19   0x0000F253  [8]   LPos      (+128)    glUniform4fv(n=4)
20   0x0000F278  [9]   LCol      (+136)    glUniform4fv(n=4)
21   0x0000F29D  [10]  ambient   (+144)    glUniform4fv(n=6)
```

That set is exactly the `0x3F001F` mask a shader switch forces (the immediate at
`0x0000F053`), which is the cross-check that says the table is complete. The
self-test asserts it.

### What it hooks

Six inline hooks, all in `tomb123.exe`:

| hook | job |
|---|---|
| `vid_setPass` | classify each pass as world-space or 2D |
| `validate_draw` | write per-eye projection/view, force the dirty bits, restore |
| `ogl_draw` | issue every draw twice, once per eye half |
| `ogl_present` | submit to the compositor, mirror, `WaitGetPoses` |
| `ogl_setRenderTarget` | latch whether the engine is drawing to the backbuffer |
| `fmvShow` | exact "a video is on screen this frame" signal |

`tools\xrefs.py` disassembles all 1017 named functions and reports:

```
validate_draw        <- ogl_draw (x1)
ogl_draw             <- (no direct call in .text -- reached through the APP vtable)
vid_setPass          <- (no direct call in .text -- reached through the APP vtable)
ogl_present          <- (no direct call in .text -- reached through the APP vtable)
fmvShow              <- (no direct call in .text -- reached through the APP vtable)
ogl_setRenderTarget  <- (no direct call in .text -- reached through the APP vtable)
```

Two things follow, and both are load-bearing. `validate_draw` has **exactly one
caller**, so matrix injection can never run outside the per-eye duplication loop
— in TR4-6 that property needed two hooks, here it is free. And the other five
are never called from inside the exe at all: `vidInit` and `init_ogl` install
them into the `APP` struct and the game DLLs call through those pointers, so
patching the function bodies catches every call from all three DLLs without
hooking anything inside them.

### `ogl_setRenderTarget`, and a 13-byte prologue

TR4-6 could read an `ogl_rt` global to know whether the engine was drawing to the
backbuffer. TR1-3 has no such global, so the state is maintained by hooking
`ogl_setRenderTarget` and reproducing the engine's own branch — `test edi, edi`
at `0x0001059A`, where `edi` is the sign-extended second argument: zero binds
`FBO_default`, anything else binds `FBO_custom`.

Its prologue is also where the one real hooking bug happened. `push rsi` at the
top of that function is `40 56` — a **two-byte** REX-prefixed encoding, not the
one-byte `56`. Counting it as one byte put the stolen window at 12 bytes, ending
*inside* `sub rsp, 0x30`, which would have had the trampoline return into the
middle of an instruction. `tools\verify_addresses.py` now checks every hook
window against the real `.text` for three properties: the bytes match, the window
ends on an instruction boundary, and no instruction in it has a RIP-relative
operand (because `Hooks.cpp` passes no displacement fixups).

### `gGame` selects the game DLL — the loader does not

All three game DLLs are resident for the entire session. `WinMain` loads them
unconditionally, setting `gGame` to 0, 1 and 2 around each call so each DLL's
initialiser sees its own index:

```
00007924  mov  [gGame], r15d      ; 0
0000792B  call LoadLibraryA       ; "tomb1.dll"
00007991  mov  [gGame], r14d      ; 1
00007998  call LoadLibraryA       ; "tomb2.dll"
000079FE  mov  [gGame], r13d      ; 2
00007A05  call LoadLibraryA       ; "tomb3.dll"
00007ABB  mov  [gGame], eax       ; the game actually selected
```

So `GetModuleHandleW` succeeds for all three at all times, and "bind to whichever
DLL is loaded" silently always binds `tomb1.dll`. The first version of
`GameDll.cpp` did exactly that, and it was caught by its own log:

```
engine: now running Tomb Raider II (gGame=1)
gamedll: bound to tomb1.dll (Tomb Raider I)      <-- wrong DLL
```

Playing TR2 or TR3 it would have read Lara's water status and the camera's room
out of TR1's globals. `gGame` is the selector, and it is authoritative — `WinMain`
indexes its own per-game dispatch table with it (`movsxd rax, [gGame]; shl rax,
5; call [rax + rsi + 0x41d218]` at `0x00007BF2`), so 0/1/2 mean TR1/TR2/TR3 by
the engine's own definition. Both values are now printed together so they can be
checked against each other at a glance.

### The game DLLs also have symbols

`tomb1.dll`, `tomb2.dll` and `tomb3.dll` ship PDBs too, with `lara`, `camera`,
`room`, `number_rooms`, `GetRoomBounds` and full types for `lara_info` (432
bytes), `camera_info` (128) and `ROOM_INFO` (168). **All three DLLs have
identical struct layouts** — only the addresses of the globals differ — which is
why `GameDll.cpp` carries one struct description and a three-row address table.

That is what makes the ceiling clamp and the water-state check exact here rather
than approximate. Headroom is `camera.pos.y - room.maxceiling`: TR world space is
Y-down, so the ceiling has the *smaller* Y and a positive result means the camera
is below it. Anything outside 0..32768 units is reported as "unknown" rather than
clamped on, because a wrong clamp is worse than none.

### Not carried over

The TR4-6 mod expanded the engine's visible set along portal connectivity so
geometry did not vanish when you turned your head away from the game camera. That
is **not** in this build, and its settings are absent rather than present and
inert.

The culling lives in the game DLL, not the executable. TR4-6 implemented it
against reverse-engineered addresses inside `tomb4.dll` and `tomb5.dll`; TR1-3
would need the same work three more times. It is tractable — those DLLs ship
PDBs, and `GetRoomBounds` is right there by name — but it is not done. Until it
is, geometry outside the game camera's frustum is not drawn.

One measured finding from that work does carry over: widening the projection the
engine hands the game changes nothing about what is culled, because the DLL
builds its cull planes independently. Do not re-run that experiment.

`preserveProjOffset` is kept but is expected to be permanently inert: there is no
`vid_setPerspOffset` on this engine and `ogl_setPersp` zeroes `mProj[1].e02/.e12`
explicitly (verified at `0x0000FAD0`). If the `projoffset=` counter in the health
report is ever non-zero, something is happening that this analysis says cannot.

### Ghidra / re-mcp

The mod as it stands was written without a decompiler, because the PDBs made one
unnecessary. That stops being true for the room-culling work, which is
algorithm-shaped rather than layout-shaped: following `GetRoomBounds`'s portal
traversal and the draw-list structure in raw disassembly is exactly what a
decompiler is for.

`.mcp.json` configures [`re-mcp`](https://pypi.org/project/re-mcp/)'s Ghidra
backend for this repo:

```json
{ "mcpServers": { "re-mcp": {
    "command": "re-mcp-ghidra", "args": ["stdio"],
    "env": { "GHIDRA_INSTALL_DIR": "C:\\Ghidra\\" } } } }
```

Two things about that config are worth knowing, because the TR4-6 repo's copy
implies otherwise:

- **`GHIDRA_PROJECT_PATH` does nothing.** Nothing in `re_mcp` or
  `re_mcp_ghidra` reads it. It is dropped here rather than carried over.
- **There is no single project to point at.** The backend's `open()` creates one
  project *per binary*, alongside the binary:
  `<binary dir>\ghidra_projects\<file name>.gpr`, with the program stored at `/`
  under its full file name (`re_mcp_ghidra\session.py`). So opening
  `PDB\tomb123.exe` yields `PDB\ghidra_projects\tomb123.exe.gpr`.

`tools\build_ghidra_projects.cmd` pre-imports all four binaries into exactly
those paths with `analyzeHeadless`. It is worth running once: `open()` reuses an
existing `.gpr` instead of importing, so the first MCP call is instant rather
than a multi-minute auto-analysis — and the analysis happens with each PDB
sitting next to its binary, so Ghidra applies the real symbols and types rather
than inventing `FUN_`/`DAT_` names.

Delete `PDB\ghidra_projects` to start over.

## Building

Visual Studio 2019 or newer, x64:

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch_openvr.ps1
msbuild TombRaiderVR.sln /p:Configuration=Release /p:Platform=x64
```

Produces `TombRaiderVR.dll` and `winmm.dll` in `build\x64\Release\`, and copies
the mod DLL into the game folder. Override the destination with
`/p:GameDir="..."`, or disable the copy with `/p:DeployToGame=false`.

`openvr_api.dll` is resolved with `LoadLibrary` at runtime, so only the OpenVR
header is needed to build and the DLL loads fine on a machine with no VR runtime
installed.

### Verification

```powershell
python tools\verify_addresses.py     # 139 checks against the PDBs
tests\build_selftest.cmd             # matrix maths, consts bits, hook mechanism
```

`verify_addresses.py` re-derives every address, struct offset, structural
relationship and hook prologue from the PDBs and diffs them against the source.
Run it after any game patch: if it passes, the addresses are still right; if it
fails, it names what moved.

## Repository layout

```
src\             the mod
  Engine.h/.cpp    address map, struct layouts, module binding
  Hooks.cpp        the stereo injection layer
  GameDll.cpp      the two pieces of state that live in tomb1/2/3.dll
  StereoMath.h     matrix maths against this engine's conventions
  proxy\           the winmm shim
tools\           PDB extraction, disassembly, verification
tests\           self-test
PDB\             tomb123.exe + tomb1/2/3.dll and their PDBs
third_party\     OpenVR headers
```

`PDB\` is what makes the tooling reproducible; the address tables are only as
checkable as the symbols they came from.
