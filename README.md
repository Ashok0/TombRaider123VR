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

The original stereo, culling, first-person camera and initial roomscale work was
written with Claude Code (Opus 5). The final roomscale/rotation rewrite was
implemented with OpenAI Codex (GPT-6), using `README2.md` as the failure handoff
and BeefRaider XR as reference for keeping artificial rotation separate from
tracked HMD rotation. `README2.md` is retained as historical debugging context;
its "where it stands" section describes the failed predecessor, not this tree.

For the original build, every address, struct offset and function signature in
`src/Engine.h` and `src/GameDll.cpp` was extracted from the shipped PDBs by the
tools in `tools\`. The current Aspyr retail build shipped without PDBs, so its
addresses were carried across by `tools\port_build.py`.
`tools\verify_addresses.py` checks the PDB build and both PDB-less builds
(Aspyr retail and Tomb Raider Gold), and fails if any disagrees. The locomotion
rewrite also has independent maths tests and verifies its newly required
`analogInput` address directly from the input-update instruction stream.

## VR Mod Features

- Native stereo 3D, 6DOF head tracking, per-eye asymmetric frustums.
- All three games (TR1, TR2, TR3) through one build.
- Head-driven room culling, so geometry does not vanish when you look away
  from the game camera.
- World-locked HUD and inventory rather than a flat overlay pinned to your face.
- FMV cutscenes captured offscreen and replayed as real geometry, so they
  keystone and roll correctly instead of sitting flat.
- Touch controllers presented to the game as an Xbox pad, with decoupled
  head/aim pitch.
- Ceiling clearance clamp, so standing up in a crawlspace does not put your head
  through the ceiling.
- Sky at optical infinity, so the HD dome does not sit a few metres away in
  stereo or paint over distant geometry.
- First person: the camera rides Lara's animated head instead of the chase
  camera, interpolated so it does not judder against the world, with her head
  hidden and the rest of her body still visible.
- Roomscale: leaning and ducking move the viewpoint, turning round turns Lara,
  and walking about the room walks her through the engine's own collision.
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

Verified working: the mod loads, binds, installs all six exe hooks, brings up
OpenVR, creates the stereo target and submits stereo frames with correct per-eye
separation. What has **not** been done is a full playthrough of all three games,
so per-level and per-cutscene issues are likely to remain.

The culling fix is confirmed working in play: geometry no longer disappears when
you look away from the game camera. `PortalCulling=0` returns the stock
behaviour exactly. See "Culling Fix" below for what it does, how it differs from
the TR4-6 attempt, and how to read its log lines.

The sky fix hooks `DrawSkyHD` in the live game DLL. `SkyAtInfinity=0` returns
the stock finite-dome stereo. See "Sky at infinity" below.

First person is confirmed working in the headset on TR1: the camera rides Lara's
animated head rather than the chase camera, with her head, face, sunglasses and
braid hidden and the rest of her body still drawn. It is off by default
(`FirstPerson=1` turns it on), and her animations move your head for you. See
"First person" below.

Around it: positional tracking is measured from a captured neutral, physical and
right-stick turning share a stable VR-world heading, HMD-relative stick movement
stays aligned after either kind of turn, and walking about the room moves Lara
by direct displacement through native collision queries. The final rewrite has passed
the automated and binary checks described below and is awaiting a headset run.
See "Roomscale and rotation in first person".

Supported builds:

| build | exe PE timestamp | address table | in the headset |
|---|---|---|---|
| earlier build that shipped with PDBs | `0x6A4B4928` | `kBuildStock`, read from the PDBs | working |
| current Aspyr retail (Steam) | `0x6A4B7C52` | `kBuildAspyrRetail`, carried across without PDBs | working (TR1 tested) |
| Tomb Raider Gold (modders' patch of Aspyr retail) | `0x6A4B7C52` | the same row as Aspyr retail | working |

The mod picks the address table for each module by its PE timestamp and refuses
to patch anything it does not recognise. On Aspyr retail, a run of Tomb Raider I
installed all nine hooks at the retail addresses, bound `tomb1.dll`, brought up
OpenVR and submitted stereo frames with correct eye separation. TR2 and TR3 on
retail pass every static check but have not yet been played; their first run
should log `gamedll: bound to tomb2.dll` (or `tomb3.dll`). See "Supporting the
Aspyr retail and Tomb Raider Gold builds" below.

### Troubleshooting: the game runs on the monitor and SteamVR never starts

Almost always an old `TombRaiderVR.dll` in the game folder, one that predates
support for the exe you are running. Look in `TombRaiderVR.log` next to it:

```
engine: unrecognised build (PE timestamp 0x6A4B7C52, ...)
hook[validate_draw]: prologue mismatch at ... Wrong game build; not patching.
hooks: at least one hook failed -- rolling all of them back
TombRaiderVR: hook installation failed
```

That is exactly what happened the first time Aspyr retail was tried. The DLL in
the Steam folder was a build from before retail support existed, so the fix
was not in the code but in deploying the current build. The mod never reached
OpenVR, so SteamVR was never started and the game ran flat. Rebuild with the game
closed (a build copies the DLL into the game folder), or copy
`build\x64\Release\TombRaiderVR.dll` in by hand. A working start logs
`engine: build identified as ...` for your build, six `hook[...]` lines with no
mismatch, and `vr: up and running`.

Current builds refuse an unknown exe with `refusing to patch an executable no
address table describes`. If you see that, the exe is genuinely new; see
"Supporting the Aspyr retail and Tomb Raider Gold builds" for how to add it.

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

Everything except the room culling was built with these alone — no decompiler.
That worked *because* of the PDBs; see "Ghidra / re-mcp" below for the one place
symbols and types were not enough.

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

Six inline hooks in `tomb123.exe`:

| hook | job |
|---|---|
| `vid_setPass` | classify each pass as world-space or 2D |
| `validate_draw` | write per-eye projection/view, force the dirty bits, restore |
| `ogl_draw` | issue every draw twice, once per eye half |
| `ogl_present` | submit to the compositor, mirror, `WaitGetPoses` |
| `ogl_setRenderTarget` | latch whether the engine is drawing to the backbuffer |
| `fmvShow` | exact "a video is on screen this frame" signal |

...and three in whichever of `tomb1/2/3.dll` is running:

| hook | job |
|---|---|
| `PrintRoomsList` | expand the draw list along the head's frustum before it is drawn |
| `S_GetObjectBounds` | second-guess "this item is off screen" from the head |
| `DrawSkyHD` | mark every sky draw so stereo can put the dome at optical infinity |
| `phd_GenerateW2V` | first person: put the scene camera in Lara's head before the view matrix is built |
| `DrawCreatureHD` | first person: hide the head mesh through `mesh_bits`, and skip the face and sunglasses draws |
| `DrawHair` | first person: skip the braid |

The culling pair and `DrawSkyHD` are each the same function in all three DLLs,
with the same 5-byte position-independent prologue, so one table serves all of
them. `DrawSkyHD` is independent of `PortalCulling`: it installs whenever
`SkyAtInfinity=1`.

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

### Culling Fix

**Symptom.** Turn your head away from the game camera and the world empties out:
walls, floors and whole rooms simply are not drawn. It is the single most
disorienting thing a VR port of this engine can do, because it happens exactly
when you look around — the one thing VR is for.

**Cause.** The engine draws only the rooms its portal traversal reaches from the
**game camera**, carrying a screen rectangle that is clipped at every doorway. A
room is submitted only if some chain of doorways lands on the game camera's
screen. That is right for a monitor and wrong for a headset: you see wider than
the game camera, and you can look somewhere it is not pointing at all. The
culling lives in the game DLL, not the exe, which is why nothing the stereo
layer does can reach it.

**Fix.** `src\PortalCull.cpp` hooks `PrintRoomsList` — the last moment before
the draw list is consumed — and runs the same traversal a second time from the
tracked head, in world space, with the headset's frustum, appending what it
finds. Nothing the engine listed is ever removed, so `PortalCulling=0` is the
stock behaviour exactly, and with your head aligned to the game camera the
result is what the engine would have drawn anyway.

**The frustum shrinks at every doorway.** The portal quad is transformed into
eye space, clipped against the planes arriving from the previous room, and a new
plane is built from the head through each surviving edge. A room enters the list
only if it can really be seen through that chain of openings — not merely
because it is nearby or connected. The engine's own back-face test is kept
unchanged, with the head substituted for the camera; that substitution is free,
because the world→eye transform is orthogonal, so `dot(A·n, A·(p−c) + t)` is
`dot(n, p − headPos)` and the comparison survives being done in eye space.

#### Where the code is

| file | what it holds |
|---|---|
| `src\PortalCull.cpp` | the traversal, both DLL hooks, the draw-list surgery |
| `src\PortalGeom.h` | the frustum maths — dependency-free, so the self-test can include it |
| `src\GameDll.cpp` | the three-row address table these run against |
| `tests\selftest.cpp` | 26 hand-worked checks on the geometry |
| `tools\verify_addresses.py` | re-derives every address and both hook prologues from the PDBs |

#### How this differs from TR4-6

The TR4-6 mod expanded the list by **portal hops**: take every room already
listed, add everything one doorway away, repeat *N* times. It worked, and every
row below is something it had to live with that a real traversal does not.

| | TR4-6 (`RoomCull.cpp`) | here (`PortalCull.cpp`) |
|---|---|---|
| criterion | hop count `N` | whether the head can see through the doorways |
| depth | `PortalHops`, tuned per level | decided by the geometry; the budgets only bound the worst case |
| flip rooms | added, then excluded by a hand-kept list | unreachable by construction |
| addresses | `FUN_18002ea30`, `DAT_18063dc60` | `PrintRoomsList`, `draw_rooms`, by name, from the PDBs |

The flip-room point is the interesting one. A flip map's inactive half is a real
entry in the room array at the same world position as its live twin, so a
mechanism that adds rooms the traversal never reached will happily add both and
draw one over the other. TR4-6 hit exactly that and needed
`DrawAllRoomsExclude=215` and then a `flipped_room` check to suppress it. No
live room's portals name a storage room, so a real traversal cannot reach one —
the same reason the engine's own never draws one. There is no exclusion list
here because there is nothing to exclude.

#### The two levers behind the room list

Fixing the list alone is not enough, because the same camera-shaped assumption
is baked in twice more:

* **`ROOM_INFO::left/right/top/bottom`**, the per-room clip rect. `PrintRooms`
  copies it into `phd_left/right/top/bottom` and `CheckClipping` turns it into a
  scissor box, so a room reached down a corridor is pixel-clipped to where the
  game camera saw its doorway. Every listed room's rect is widened to the whole
  target (`CullWidenBounds`). On the modern renderer this is belt and braces —
  the stereo hook already replaces the scissor per draw — but on the classic
  renderer the same rect drives vertex clipping in `calc_roomvert`.
* **`S_GetObjectBounds`**, which answers 1 / −1 / 0 for an item's bounding box.
  Zero is reached two ways and both are the game camera's opinion: every corner
  behind `phd_znear`, or the projected rectangle missing the screen rect. An
  enemy behind the camera fails the first, one beside it the second. Without
  `CullObjects` the added rooms draw with their furniture, enemies and pickups
  missing. Only the zero answer is second-guessed, and only ever upward to −1
  ("visible, clip it") — never the other way, and never in the inventory or on
  the title screen.

TR2 and TR3 additionally keep an `outside` flag and a separate screen rect for
the sky, grown as the traversal meets outdoor rooms. Both are maintained the way
the engine maintains them. TR1 has no such state at all, and those five columns
of the address table are zero for it.

#### One space, one sign

The whole thing turns on a single relationship, so it is worth stating plainly.
The game's culling works in **phd view space** — `v = R·(p − camPos)`, X right,
Y down, **+Z forward** (`SetRoomBounds` tests `z < 1` for "behind the camera").
The mod's matrices work in the space `mView_packed` defines, which is the same
transform with its **third rotation row negated**, i.e. −Z forward. That is not
inferred: `vid_setViewMatrix` (RVA `0x0000A5E0`) builds it from the same
`int[12]` and negates exactly `m[8]`, `m[9]`, `m[10]`, leaving every other term
alone.

So `eye = HeadView · N · phd` with `N = diag(1, 1, −1)`, and the whole chain
collapses to one 3×3 and two translations. Get that sign backwards and the
traversal culls the half of the world you are looking at.

#### What it costs, and what to watch

The traversal is integer-free float work over portal quads — a few hundred
portals a frame at most — so its own cost is nothing. What costs is the extra
geometry it lets through, which is the point. The health report prints it:

```
cull: 12.4 rooms/frame from the engine + 3.1 added by the head frustum, 4.2 items rescued/frame
```

`added = 0` forever means either the head never left the game camera's cone or
the traversal is not running; the `cull: head-frustum portal traversal live`
line tells the two apart. `CullDumpKey` prints the whole list with the added
rooms starred, which turns "that wall is missing" into a room number.

#### Settings

All in `[VR]`, all documented at length in `TombRaiderVR.ini` itself.

| key | default | what it is for |
|---|---|---|
| `PortalCulling` | `1` | the whole feature; `0` is stock behaviour |
| `CullFovMarginDegrees` | `8` | angle added to each half of the culling frustum — covers canted displays and the few ms between the pose that culls and the pose that renders |
| `CullMaxDepth` | `16` | doorways deep, worst-case bound only |
| `CullMaxPortals` | `4096` | portals per frame, worst-case bound only |
| `CullFarUnits` | `0` | optional distance limit, off by default; a frame-rate lever, not a fix |
| `CullWidenBounds` | `1` | widen the per-room clip rect (see above) |
| `CullObjects` | `1` | extend the fix to items (see above) |
| `CullDumpKey` | `0` | virtual-key code that dumps the draw list to the log |

Neither budget is a visibility criterion. The frustum closing at successive
doorways is what ends the traversal; the budgets exist so a pathological level
cannot spend the whole frame in here, and the log says so if either is hit.

#### One thing not to try again

Widening the projection the exe hands the game changes **nothing** about what is
culled. TR4-6 swept it to 200% of screen each way, and 20× on TR6, and got
neither extra geometry nor a frame-rate change. The DLL builds its cull planes
from its own matrices and never looks at the projection matrix; on TR1-3 a
portal beside or behind the camera fails the near-plane test inside
`SetRoomBounds` before any rectangle is consulted. Do not re-run that
experiment.

`preserveProjOffset` is kept but is expected to be permanently inert: there is no
`vid_setPerspOffset` on this engine and `ogl_setPersp` zeroes `mProj[1].e02/.e12`
explicitly (verified at `0x0000FAD0`). If the `projoffset=` counter in the health
report is ever non-zero, something is happening that this analysis says cannot.

### Sky at infinity

**Symptom.** Outdoors, the sky is a painted sphere a few metres away. Distant
cliffs sit *behind* it, and leaning or IPD makes the dome slide. On a monitor
the same mesh is fine.

**Cause.** `DrawSkyHD` in `tomb1/2/3.dll` already does the classic
monitor-correct thing: it copies the current matrix, **zeros its translation**,
and draws `hd_sky` through `GetRoomShader` / `app.setPass` / `app.draw`. The
dome is centred on the game camera, so there is no parallax from walking. That
is optical infinity for one viewpoint.

The stereo path then composes the eye transform `E` — IPD plus any 6DOF head
offset — into the projection (`EyeOffsetMode=3`: `P' = P * E`). The dome's
vertices still sit at a finite mesh radius, so that translation gives them
stereo disparity equal to a few metres, and `glDepthRange` is still the engine's
own, so the dome's depth values can occlude farther world geometry.

The PDBs say this is the only sky draw path. There is no `DrawFlatSky`,
`DrawSkySegment` or `SkyDrawPhase` in these DLLs. `tools\xrefs.py` against
`tomb1.dll` reports a single caller:

```
DrawSkyHD            <- PrintRoomsList (x1)
```

The exe PDB names the shader blobs (`ogl_SKY_vs`, `ogl_SKY_HD_vs`, and the
matching pixel shaders) but no extra draw entry point. `hd_sun` is 24 bytes of
data, not a function. One hook covers every sky triangle the remaster issues.

**Fix.** `src\Sky.cpp` hooks `DrawSkyHD` and sets a flag for the life of that
call. Every `validate_draw` that runs underneath it uses the **rotation** of `E`
only (looking around still turns the sky; leaning and IPD do not). Every
`ogl_draw` underneath it sets `glDepthRange(1, 1)` for that draw and restores
the engine's range afterwards, so the fragments land on the far plane and cannot
win a depth test against the world. `SkyAtInfinity=0` is the stock behaviour
exactly: the hook is not installed.

The stolen window is the same 5-byte PIC `mov [rsp+8], rbx` as `PrintRoomsList`,
and it is the same bytes in all three DLLs:

| DLL | `DrawSkyHD` RVA | size |
|---|---|---|
| `tomb1.dll` | `0x0006E480` | 558 |
| `tomb2.dll` | `0x000A0760` | 558 |
| `tomb3.dll` | `0x000EAA80` | 558 |

#### Where the code is

| file | what it holds |
|---|---|
| `src\Sky.cpp` | the `DrawSkyHD` hook and the in-sky flag |
| `src\Hooks.cpp` | rotation-only `E` at inject time; far-plane depth around `ogl_draw` |
| `src\GameDll.cpp` | the three-row address table those RVAs live in |
| `tools\verify_addresses.py` | re-derives the three RVAs and the prologue from the PDBs |

#### What to watch

```
sky: hooked tomb1.dll (Tomb Raider I) -- sky draws use rotation-only eye transform (optical infinity) and far-plane depth
sky: first DrawSkyHD draw (shader N) -- rotation-only eye transform, far-plane depth
```

The health report's `sky=` count is per injected eye. Outdoors it should be
non-zero; `sky=0` forever with the "hooked" line present means `DrawSkyHD` is
not running (indoors, or a title screen).

The GPU verify sample skips sky draws on purpose. Both eyes share a
translation-free `E` there, so sampling one would report zero separation and
look like an injection failure.

#### Settings

All in `[VR]`, documented in `TombRaiderVR.ini`.

| key | default | what it is for |
|---|---|---|
| `SkyAtInfinity` | `1` | the whole feature; `0` is stock finite-dome stereo |

### First person

**Confirmed working in the headset** on Aspyr retail / Gold, TR1. TR2 and TR3
use the same hook and the same table and should work, but have not been played.
Off by default: `FirstPerson=1` in `[VR]`.

The camera rides Lara's animated head. Her animations then move your head for
you -- through every roll, swan dive and grab -- which is not something the game
was ever designed to do, so treat it as a different way to play rather than a
better camera.

#### One hook, and why the camera is the only thing touched

The stereo layer already computes `finalView = eyeView * gameView`, so it never
needs to know where the game camera is. Move the GAME's camera and everything
downstream follows on its own: the stereo view, the head-frustum culling, item
visibility, both renderers. Nothing in `Hooks.cpp` knows this feature exists.

So the camera portion of `src\FirstPerson.cpp` hooks exactly one function,
`phd_GenerateW2V`, which turns a camera pose into `w2v_matrix`, and rewrites the
pose it is handed. The engine's own chase camera still runs and still decides
which room the camera is in. The separate locomotion portion deliberately writes
Lara's facing during safe ground states and transforms XInput movement; those
gameplay changes are documented under "Roomscale and rotation in first person".

**Only the scene call.** `phd_GenerateW2V` has seven callers -- the inventory,
the pickup spin, shadows, photo mode, the muzzle flash -- and every one of them
must keep its own camera. The scene call is the one inside
`S_InitialisePolyList`, and the hook recognises it by the exact address it
returns to (`w2vSceneReturn` in the table). That is why a new column exists for
what is not a symbol at all: `verify_addresses.py` checks that the five bytes
before it really are an `E8 rel32` to `phd_GenerateW2V`, in all three DLLs and
both builds, which is the same property the runtime gate depends on. A wrong
value there means first person never engages -- the safe direction.

Fixed and cinematic cameras (`camera.type` 1 and 4+) are left alone, because
those framings are placed deliberately by the level.

#### The anchor, and why it is not GetJointAbsPosition

The engine has `GetJointAbsPosition(ITEM_INFO*, PHD_VECTOR*, int32)`, which is
exactly "where is joint N in the world". The first version called it. It was
wrong twice over: it walks the engine's matrix stack, which is not something to
be doing from inside the camera hook, and it answers for the CURRENT simulation
tick -- while frames are drawn *between* ticks. The camera stepped at tick rate
while the world moved smoothly, which reads as the camera lurching every time
Lara moves.

Reading that function's disassembly gave the better answer. It takes the joint
straight out of `ITEM_INFO`: one 3x4 `int32` matrix per joint, 48 bytes apart,
rotation in 1/16384 fixed point, in TWO copies -- `+0x1F0` for the previous tick
and `+0x820` for the current one. The mod interpolates between them with the
engine's own `frame_frac` (0..256), which is the same value `DrawLara` uses to
draw the body, and adds Lara's world position interpolated from `pos_prev` to
`pos`. The result is smooth by construction and touches no engine state at all.

The offset `(0, -32, 16)` is a point inside the skull relative to the joint's
neck pivot, so the viewpoint sits behind the eyes rather than in her throat.

A sanity check rejects an anchor further than four sectors from Lara: a wrong
joint index then falls back to the game camera and says so in the log, rather
than putting the player inside the world.

#### What headset testing changed

The first implementation exposed three independent problems. The anchor needed
simulation interpolation; tracked position needed a captured neutral rather than
the runtime's floor/seated origin; and using Lara's body yaw directly made the
right stick appear dead under modern controls. Those fixes made the basic
first-person camera and positional tracking work in the headset.

The later locomotion tests found a fourth problem: adding the engine camera yaw
to tracking-space HMD yaw forms a feedback loop. Lara turns, the chase camera
follows, the changed camera yaw moves Lara's target again, and forward movement
curves into a circle. The final implementation no longer uses the chase camera
as a heading source. A stable tracking-to-world heading now drives rendering,
body following and movement conversion together; its full design is below.

Head rotation is always tracked. Head translation is on by default in first
person and measured from the captured neutral. Lara's animated head pitch and
roll are discarded because an animation must not tilt the player's horizon.
`PortalCull.cpp` uses the same final head transform, so culling agrees with both
eyes.

#### Hiding the head, three different ways

With the camera inside her skull, what saves the view is only the near clip
plane: her head is still drawn, and pieces of it cross the view as she moves.
Hiding it turned out to need three separate mechanisms, because the engine draws
the head in three different ways. This was worked out by decompiling `DrawLara`,
`DrawLaraHD` and `DrawCreatureHD` -- 2771 bytes of the middle one, which is where
Ghidra earned its place over reading assembly.

**The head mesh: the engine's own switch.** Every one of Lara's 15 meshes has a
bit in `ITEM_INFO::mesh_bits`. The classic renderer's draw loop in `DrawLara`
tests that bit per mesh and skips it; the HD renderer reaches the same result one
step further in, with `DrawCreatureHD` zeroing the joint matrix of any mesh whose
bit is clear so its triangles collapse to a point. `DrawLaraHD` uses this itself
-- `0x600` to draw only the right hand, `0x3000` for the left. So hiding the head
is clearing bit 14, and one mechanism covers both renderers.

The only catch is that `DrawCreatureHD` honours `mesh_bits` only when its second
argument is non-zero, and the body draw passes zero. The hook passes one instead.

This is worth stating plainly because the other TR1-3 attempt did it the hard
way: caching index buffers and removing every triangle weighted to the head bone.
That works, but the engine already had a switch for it.

**The face and the sunglasses: skip the draw.** Neither is part of the body mesh,
so no `mesh_bits` bit reaches them. `DrawLaraHD` copies a separate `GEOM_INFO`
into `objects[Lara].geom` immediately before each call -- `gLaraHead[0]` for the
animated face, `gLaraHead[1]` for the sunglasses, `gActorHead` for the cutscene
head. The hook compares that mesh pointer against those arrays and drops the
draw. Comparing the geometry rather than counting calls means it does not depend
on the order of the draws, or on which of them a given outfit produces.

**The braid: its own hook.** Drawn outside the skeleton entirely by `DrawHair`,
so neither of the above can see it -- and from inside her head it sweeps through
the view. It gets a third hook, which does nothing but decline to draw.

Her body stays drawn throughout: look down and she is there. The bit is OR'd back
and every skip stops the moment first person stands down -- a cutscene, the
inventory, `FirstPerson=0`, an unhook -- so nothing outside this mode ever sees a
headless Lara. The unhook line counts what was dropped:

```
firstperson: unhooking tomb1.dll (anchored 41233 frames, stood down 120,
             41233 mesh_bits draws, 82466 face/glasses skipped, 41233 braid skipped)
```

#### A hook window that is not position independent

`DrawHair` is the first hook in this mod whose stolen bytes cannot simply be
copied:

```
48 83 EC 28              sub rsp, 0x28
48 8B 05 <disp32>        mov rax, [rip + disp32]     <- RIP-relative
```

Eleven bytes, because five would end inside that second instruction, and the
displacement means the copy in the trampoline would resolve to the wrong address.
`InlineHook` already supports displacement fixups, so the source declares the
disp32 at offset 7 and it is rewritten when the window is copied. Only the first
seven bytes are compared, because the displacement itself differs per DLL.

`verify_addresses.py` was extended to match: a RIP-relative operand inside a hook
window is no longer simply forbidden, it is allowed exactly where the source
declares a fixup for it, and the declared offsets are checked against the real
instruction encoding. Planting a wrong offset fails all three DLLs, which is the
test that says the check is real.

Hooking `DrawLara` itself was considered and rejected: its prologue differs in
every one of the three DLLs, where `DrawCreatureHD`'s and `DrawHair`'s are
identical across all of them. Going through `mesh_bits` avoids needing it.

#### Settings

| key | default | what it is for |
|---|---|---|
| `FirstPerson` | `0` | the whole feature |
| `FirstPersonJoint` | `14` | Lara's head joint, the same in all three games |
| `FirstPersonAnchorX/Y/Z` | `0,-32,16` | where in the skull the viewpoint sits; -Y is up, +Z towards her face |
| `FirstPersonYawFromLara` | `0` | obsolete; stable VR heading now owns first-person yaw |
| `FirstPersonHeadTranslation` | `1` | lets your own leaning move the viewpoint relative to neutral |
| `FirstPersonHideHead` | `1` | hide the head mesh, the face, the sunglasses and the braid |

#### Still not implemented: controller-driven arms

Arms that follow the motion controllers -- the VRIK part -- are the next piece of
work, and the lever for it is already identified:
`GetJoints(ITEM_INFO*, float*)` builds the matrices the renderer consumes, and
`lara_info` carries the real aim state (`left_arm`/`right_arm` angles, `torso_*`,
`head_*`, `target`) that `AimWeapon` and `FireWeapon` work from.

#### Prior art

The head-anchored camera, the `(0,-32,16)` offset and joint 14 all come from an
independent TR1-3 Remastered attempt (`tomb123-vr-dev-source`), which got there
without symbols by matching instruction streams. The arm IK design that stage 3
would follow is BeefRaiderXR's, which can do it the easy way: it is built on
OpenLara, an open-source re-implementation, so it edits the skeleton in its own
source rather than someone else's binary. OpenLara is BSD-2-licensed.

### Roomscale and rotation in first person

First person puts the camera in Lara's animated head. Roomscale gives the
player's real movement a gameplay meaning: lean and duck move the viewpoint,
turning physically turns the world and Lara, the right stick turns the same
world heading, and physical steps directly drag Lara through native collision
queries. Physical displacement never generates analog-stick input or walking
animations. Manual stick movement remains a separate path.

#### Positional tracking and its neutral

An OpenVR position is absolute within the tracking universe. In a standing
universe its origin is normally on the floor, so applying it directly on top of
Lara's already-correct head position lifts the camera by the player's entire
height -- roughly 700 Tomb Raider units at the default scale.

`VRSystem` therefore captures the current HMD position as a neutral and applies
only displacement from it. The capture happens when first person becomes live,
after a valid pose and Lara head anchor both exist. Leaning, ducking and stepping
then move the viewpoint, while simply being 1.7 metres tall does not. Pressing
END (`FirstPersonRecenterKey`) captures a new positional neutral without changing
the world heading.

The horizontal locomotion offset stays in tracking space as `(x, -z)`: positive
X is right and OpenVR negative Z is forward. Height is excluded. The old version
rotated this displacement into the head's current frame, which meant the same
physical position changed meaning when the player looked around.

#### One stable heading for the view, body and controls

The final design has one persistent `trackingToWorldYaw`. On entry it is aligned
without moving the view:

```text
trackingToWorldYaw = LaraYaw - HmdTrackingYaw
HmdWorldYaw        = trackingToWorldYaw + HmdTrackingYaw
```

Physical turning changes `HmdTrackingYaw`. Right-stick turning changes
`trackingToWorldYaw`. The scene camera receives `trackingToWorldYaw`, and the
stereo layer composes the tracked HMD rotation on top exactly once. Movement and
body following read the same `HmdWorldYaw`.

The chase camera is deliberately absent from these equations. The failed design
used `cameraYaw + HmdTrackingYaw`; Lara turned, the chase camera followed her,
the changed camera yaw moved her target again, and forward movement curved into
a circle. Keeping the tracking-to-world transform as independent state removes
that feedback loop.

HMD yaw is extracted from row 2 of the inverse OpenVR pose: that row is the
headset's back axis in tracking space. Projecting it onto the floor gives yaw
without letting pitch or roll steer Lara. This also avoids the pose-versus-view
matrix sign errors encountered by earlier controller-yaw experiments.

Right-stick turning is smooth by default, uses elapsed time rather than frames,
has its own dead zone, and clamps a long pause to 50 ms of catch-up. A stick turn
also rotates the positional neutral around the current HMD position. Without
that pivot, turning while leaning away from neutral would orbit the camera around
the old neutral. Translation is refreshed immediately so culling and both eyes
use the same origin in that frame.

When inventory, a fixed camera, a cinematic camera or a cutscene takes control,
first person and locomotion stand down. Returning with the same Lara preserves
the last world heading and captures a fresh positional neutral. A changed item
or a relocation of more than two metres/1024 units realigns from Lara instead
of trying to catch up stale state.

#### Body facing and jumps

Idle ground states follow the HMD heading. Manual Modern-control movement gives
facing to the game's directional locomotion. Direct roomscale dragging leaves
facing under HMD control, so a physical sideways step moves Lara sideways while
she keeps looking where the headset looks.

The previous jump-button yaw snap was incomplete. `lara_as_compress` (state 15)
and `lara_as_forwardjump` (state 3) both recompute direction from `analogInput`.
The old input transform stopped in those states, allowing the chase-camera
heading to take over after physical rotation.

The new `LaraAboveWater` hook runs at the simulation boundary, after
`LaraControl` has decoded Modern controls. It publishes HMD-world yaw as
`camTurn`/`oldCamTurn` and corrects the signed analog vector after the game's
per-axis deadzones. This continues through compression and forward flight.
A directional jump aligns body yaw and `lara.move_angle` and clears residual
`lara.turn_rate` before launch. Airborne steering remains an engine operation
using that consistent input frame. Swimming, climbing, hanging and scripted
interactions retain their native controls.

#### Manual analog movement

The merged Touch/Xbox left stick supplies manual intent. Forward means HMD
forward under Modern controls, including after physical and right-stick turns.
The gamepad pass converts it to the current engine input frame; the simulation
pass corrects the decoded vector so per-axis deadzones cannot bend its direction.
Roomscale displacement is never added to this stick. Right-stick yaw updates
the persistent VR heading and is consumed during ground locomotion, compression
and forward jumps.

Tank controls keep their native manual actions. Physical body dragging works
in both control schemes, including diagonal movement and concurrent stick use.

#### Direct roomscale body dragging

`VRSystem::HeadFloorOffset` estimates floor translation of the neck pivot by
subtracting the change in a horizontal neck-to-HMD offset from tracked head
translation. `FirstPersonRoomscaleNeckMetres` defaults to 0.15 m. Thus turning
about that pivot does not request a step, while camera tracking still shows the
actual head movement. This is an estimate from HMD tracking, not a body tracker;
the setting can be adjusted or disabled with zero.

Physical displacement beyond the 2 cm default lean allowance becomes a distance
in game units. Before the normal above-water simulation, `DragBody` sweeps that
distance in steps of at most 32 game units through `GetCollisionInfo`, using
Lara's standing height (762) and radius (100). It applies accepted horizontal
position and collision slide, updates room membership with `UpdateLaraRoom`,
and lets the normal simulation handle floor settling, enemies and triggers.
Large drops, excessive floor rises and inadequate headroom block the drag.
The movement has no walk/run/sidestep input, speed ramp or gait dependency.

Only displacement actually accepted by the collision query consumes the tracked
neutral. Separate cumulative roomscale counters are interpolated with the same
`frame_frac` as Lara's body, keeping camera displacement and body rendering in
step. Manual travel cannot consume physical displacement. Motion already
simulated but not yet rendered is excluded from the next drag request so it
cannot be applied twice.

Dragging is restricted to ordinary ground states. Jumping, swimming, climbing
and interactions do not receive physical displacement. Pending horizontal
motion is cleared there to avoid movement on returning to ground. R3 D-pad
shift suppresses drag. END captures a fresh neutral and clears interpolation
history. Wall collision blocks Lara's body; tracked camera leaning itself can
still cross nearby geometry.

#### Why earlier revisions failed

| symptom | cause | current handling |
|---|---|---|
| movement curves after turning | chase-camera yaw fed back into VR heading | persistent tracking-to-world heading |
| physical sidesteps become forward walking | Modern controls convert analog direction into forward-run plus body rotation | direct collision-tested body displacement, no synthesized stick |
| jump works initially but angles after physical rotation | compression and flight lost the input transform and used the old camera frame | correct decoded input at the simulation boundary throughout both states |
| turning in place produces movement | headset traces an arc around the neck and exceeds the translation deadzone | subtract the estimated rotational arc from the body request |
| physical steps drift or depend on frame rate | fixed neutral consumption or attribution from manual movement | consume only accepted drag, using body interpolation |

#### Implementation and address verification

The feature is deliberately split by ownership:

| file | responsibility |
|---|---|
| `src/VRSystem.cpp` | raw tracked position, neutral, HMD yaw, step consumption and turn pivot |
| `src/FirstPerson.cpp` | stable heading, simulation steering, collision body drag, interpolation and diagnostics |
| `src/Gamepad.cpp` | merge Touch/physical pads, then apply first-person input transformation |
| `src/LocomotionMath.h` | tested frame-independent vector, angle, step and heading maths |
| `src/StereoMath.h` | floor-projected yaw extraction from the inverse HMD pose |

Locomotion adds `analogInput` to every `GameDllLayout` row. The checked RVAs are:

| DLL | PDB build | Aspyr retail / Gold |
|---|---:|---:|
| `tomb1.dll` | `0x0041DFE0` | `0x0041EF20` |
| `tomb2.dll` | `0x004330A0` | `0x00432FE0` |
| `tomb3.dll` | `0x00491F40` | `0x00494E80` |

`tools/verify_addresses.py` checks the PDB symbol, the 52-byte
`ANALOG_INPUT_INFO` layout and `camTurn` offset. `tools/verify_locomotion.py`
independently locates the unique `S_UpdateInput` instruction sequence that
passes `&analogInput` to the application's input function, and verifies all six
table values against the actual DLLs.

The address table also carries `input`, `LaraAboveWater`, `GetCollisionInfo`
and `UpdateLaraRoom`. Their PDB symbols and shared `coll_info`/`lara_info`
layouts are verified. The retail collision and room functions were matched
against the symbol-bearing build and checked through native callers. The
simulation hook validates each game's five-byte prologue before installation.

With `FirstPersonDriftLog=1`, simulation diagnostics report `state`, `head`,
`body`, `cam`, the manual world vector, pending physical displacement, accepted
`drag` in metres and decoded action bits. Lines are emitted twice a second and
on entry to compression/forward-jump. During a pure physical step, `manual`
should remain zero and `drag` should follow the physical direction. During a
forward jump, `head` and `cam` should agree even after turning.

#### Settings

| key | default | purpose |
|---|---|---|
| `FirstPerson` | `0` | enables the head anchor and this locomotion path |
| `PositionalTracking` | `1` | required for leaning and physical-step movement |
| `FirstPersonHeadTranslation` | `1` | track displacement from the captured neutral |
| `FirstPersonRecenterKey` | `0x23` | END resets position without changing world heading |
| `FirstPersonBodyFollowsHead` | `1` | follow HMD heading while idle or physically dragging on the ground |
| `FirstPersonBodyDeadzoneDegrees` | `0` | permitted body/head yaw difference |
| `FirstPersonBodyTurnDegreesPerFrame` | `4` | 60 Hz legacy rate, scaled by elapsed time |
| `FirstPersonTurnDegreesPerSecond` | `120` | smooth right-stick turning speed |
| `FirstPersonTurnDeadzone` | `0.25` | right-stick turn dead zone |
| `FirstPersonMoveWithHead` | `1` | Modern-control left-stick forward follows HMD heading |
| `FirstPersonRoomscaleMove` | `1` | physical floor displacement directly drags the body |
| `FirstPersonRoomscaleDeadzoneMetres` | `0.02` | small lean allowance before body dragging |
| `FirstPersonRoomscaleNeckMetres` | `0.15` | estimated horizontal neck-to-HMD distance |
| `FirstPersonRoomscaleFullMetres` | `0.45` | legacy stick-ramp setting; ignored |
| `FirstPersonDriftLog` | `0` | simulation state, heading, manual input and body-drag diagnostics |

`FirstPersonYawFromLara` and `FirstPersonRoomscaleDriftMetres` remain readable for
old INI files but are ignored by the new path. Old experimental moving-dead-zone,
head-smoothing, controller-steering and fixed stick-drift settings are also
ignored.

#### Validation and current limits

`tests/build_selftest.cmd` checks independent physical/stick turns, render and
movement heading agreement, pitch/roll isolation, neck-pivot rotation, lateral
drag at arbitrary physical/artificial headings, and consistent steering in
compression and forward flight. These checks validate math and state selection;
they do not run the game engine or a headset.

`tools/verify_addresses.py` passes all 424 PDB/address/layout checks.
`tools/verify_locomotion.py` checks the PDB and installed retail input, simulation
hook, collision and room-update addresses against their native callers.

The 2026-09-20 direct-drag revision was built in Release/x64 and installed in
the Steam game folder. Its SHA-256 is
`2339BBC4D4A6C3906FFFBED8D5F7CFBC9F01174C14B00ABD325AE0E80DB8BC2B`.
The previous DLL and INI are preserved beside the installed files with suffix
`.pre-direct-drag-20260920-153753`. The installed deadzone is now 0.02 m;
other existing settings were preserved. The new neck-pivot setting uses its
0.15 m default when absent from the INI. Disk exhaustion on C: required building
under `E:\CodexBuilds\TombRaider123VR-roomdrag`; the resulting DLL and matching
PDB were also copied to `build\x64\Release`. The build had no compiler errors
or warnings; MSBuild reported one temporary-directory layout warning.

The new direct-drag revision needs headset validation, especially walls, room
boundaries, floor changes, turning in place and jumping after 90/180-degree
physical turns. The test sequence is in `docs/roomscale-testing.md`. Full-body
tracking is unavailable, so the neck pivot is an adjustable estimate. Physical
drag is grounded only; a large tracking discontinuity requires END to recenter.

### Supporting the Aspyr retail and Tomb Raider Gold builds (no PDBs)

The current Aspyr retail build differs from the build in `PDB\` in
`tomb123.exe` and all three game DLLs. Every image has a new PDB GUID. It was
relinked with a newer toolchain: `.fptable` appears,
`_RDATA` is gone, and constants are now loaded with `mov r, imm` where the old
compiler used `lea r, [reg+k]`. It shipped **without PDBs**, and the game's
`pdb\` folder is empty. Almost every address the mod uses moved, and nothing
could be read out of dbghelp this time.

This work was first done on the **Tomb Raider Gold** binaries, and only then on
the Aspyr retail build. Gold turns out to be an in-place byte patch of retail:
same sizes, same PE timestamps, same PDB GUIDs, so both select the same address
table. A byte diff of the two:

| image | bytes changed | where |
|---|---|---|
| `tomb123.exe` | 6 | `.data`, inside `texDesc`: 1920×1080 → 3840×2160, 2 → 4 |
| `tomb1.dll` | 35 | one 30-byte code patch, plus `.rdata` |
| `tomb2.dll` | 56 | the same code patch, plus `.rdata`/`.data` |
| `tomb3.dll` | 150 | the same code patch, four more patched functions, plus `.data` |

None of those bytes falls inside a hook window, a hooked function, or any
global the mod reads. `port_build.py` run separately on `retail\` and `gold\`
derives identical addresses, and `verify_addresses.py` passes against both.

**How the addresses were recovered.** `tools\port_build.py <dir>` carries the
symbols across from the PDB build in `PDB\` to the build in `<dir>` (here
`retail\` and `gold\`):

1. `.pdata` gives every non-leaf function's exact extent in both images.
2. Each function is reduced to one token per instruction. RIP-relative
   displacements and branch targets are masked. Struct displacements and
   immediates are kept.
3. Functions pair up by unique exact hash, then by unique mnemonic-only hash.
   Pairs propagate along call edges, and what remains is matched by sequence
   similarity (≥ 0.75).
4. Inside every matched pair, aligned RIP-relative references vote for
   old global → new global. The winner of the vote is the new address.

All 75 values the mod needs resolved. Every one was unanimous except tomb2
`room`, at 152 of 154 votes. `mView_packed` has no RIP-relative reference
anywhere, so it is inferred from its neighbours, which both shift by +3920. The
tool labels it as inferred.

**How they were checked, independently of the matching.** A match is not proof,
so `verify_addresses.py` re-checks what the mod depends on directly against the
new images:

| check | what it establishes |
|---|---|
| all 15 hook prologues byte-identical, instruction-aligned, RIP-free | the stolen-byte windows are still safe |
| `vidInit`/`init_ogl`/`appInit` install each hooked function at the same `APP` slot in both builds (+560, +368, +280, +352, +712) | the five exe hook targets are the functions the game DLLs actually call |
| `validate_draw` forces `0x3F001F` and runs the same 11 bit tests in the same order | `ConstBits` is unchanged |
| `vid_setViewMatrix` writes the same 25 offsets relative to `vid_state` | `mView_packed` really is `vid_state + 400` |
| every structural relationship `Engine.cpp` asserts holds on the new row | no typo in the row |
| every value in `Engine.h`/`GameDll.cpp` equals `<dir>\port_build.json` | the source says what the tool derived |

Also checked while porting: all 191 mapped references into `APP` keep their
field offset. `ogl_setRenderTarget` still branches on `test edi, edi` (now at
`0x0001046A`).

**What is inferred rather than proven.** There are no type records, so the
struct layouts (`RenderState`, `ROOM_INFO`, `lara_info`, `camera_info`) are
assumed unchanged. The evidence is that the functions touching the fields the mod
uses (`validate_draw`, `PrintRooms`, `SetRoomBounds`, `CalculateCamera`,
`S_GetObjectBounds`) use the same displacements in both builds. TR1's
`SetRoomBounds` now inlines a helper and touches more fields, but at the same
offsets.

**Where it lives.**

| file | what changed |
|---|---|
| `src\Engine.h` | `kBuildAspyrRetail`, the exe row (Aspyr retail and Gold), as hex literals with the evidence in the comment |
| `src\Engine.cpp` | `kBuilds` lists both builds |
| `src\GameDll.cpp` | `kDlls` is now `[build][gGame]`, selected by the DLL's own PE timestamp |
| `tools\port_build.py` | the matcher; prints paste-ready rows and writes `port_build.json` |
| `tools\verify_addresses.py` | checks rows for PDB-less builds (default dirs `retail\` and `gold\`) |

**Two safety bugs fixed along the way.** Both had let a mismatched build through:

- `IdentifyBuild` used to fall back to `StructuralCheckPasses` against the stock
  table for an unknown exe. That check compares the table's constants with each
  other and reads nothing from the image except `shaders[]`, so it passed for
  *every* exe. On the Aspyr retail exe it would have redirected `FBO_default` and the
  XInput slot at stale addresses. The prologue check doesn't cover those writes,
  and `vid_setPass` happens to keep RVA `0xABA0`, so it could not be relied on
  either. An unknown exe is now refused.
- `GameDllUpdate` accepted a DLL with an unexpected timestamp, on the grounds
  that nothing is written through that table. That stopped being true when
  `PortalCull` began writing `draw_rooms` and the room clip rects. An unknown DLL
  is now left unbound, and every consumer already treats that as "stand down".

The old fallback was visible in practice: the pre-retail DLL, run on the retail
exe, logged `structural check passed; proceeding with the stock addresses`. It
was saved only because five of the six stock hook sites happened not to match,
which rolled all of them back (see "Troubleshooting" above).

**Confirmed in the headset.** Gold was confirmed first. Aspyr retail was
confirmed next, once the current DLL was deployed. Its log shows the build
matched by timestamp, all six exe hooks at the new addresses (`validate_draw`
at `+0xEFE0`, `ogl_setRenderTarget` at `+0x103A0` with its 13-byte window, and
so on), the three `tomb1.dll` hooks, `vr: up and running`, head pose acquired,
and 26.13 world units of eye separation. It also shows a clean unhook of all
nine on exit.

The consequence is deliberate: after a future patch the mod does nothing until
a row is added. The log names the unrecognised timestamp. To add one, copy the
four new binaries into a directory, run `python tools\port_build.py <dir>`,
paste the rows, and make `python tools\verify_addresses.py <dir>` pass. If the
patch ships PDBs, use `pdbdump.py` as before.

### Ghidra / re-mcp

Everything except the culling was written without a decompiler, because the PDBs
made one unnecessary. The culling is where that stops: it is algorithm-shaped
rather than layout-shaped, and no amount of symbol and type information tells
you that `SetRoomBounds` rejects a portal when `dot(normal, vertex − camera)`
is non-negative, that its four `z < 1` tests are what make a portal behind the
camera unreachable, or that `PrintRoomsList` clears `bound_active` for every
listed room on its way out. Those came out of the decompiler, and they are what
the reimplementation had to agree with.

Worth knowing before reading either implementation: **TR1 and TR2/TR3 do not
share a traversal.** TR1 recurses (`GetRoomBounds` calls itself per portal); TR2 and
TR3 run a queue in `bound_list` with `bound_start`/`bound_end` and keep an
enqueue count in the upper bits of `bound_active`. Only bit 0 of that byte means
"already in `draw_rooms`" in both, which is why one line of ours works on all
three — and why writing our own traversal was simpler than driving theirs.

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
python tools\port_build.py retail    # only for a new PDB-less build: derive rows
python tools\verify_addresses.py     # 289 checks against the PDBs, plus ~200
                                     # more for each PDB-less build directory
tests\build_selftest.cmd             # matrix maths, consts bits, portal frustum,
                                     # hook mechanism
```

`verify_addresses.py` re-derives every address, struct offset, structural
relationship and hook prologue from the PDBs and diffs them against the source.
For each build without PDBs (the directories named on the command line, default
`retail\` and `gold\`), it checks that build's rows against `port_build.json` and against
the images themselves. Run it after any game patch: if it passes, the addresses
are still right; if it fails, it names what moved.

## Repository layout

```
src\             the mod
  Engine.h/.cpp    address map, struct layouts, module binding
  Hooks.cpp        the stereo injection layer
  GameDll.cpp      binding and address table for tomb1/2/3.dll
  PortalCull.cpp   head-driven room culling, hooked into the game DLL
  PortalGeom.h     the frustum maths behind it, tested by tests\
  Sky.cpp          DrawSkyHD hook: sky draws at optical infinity
  FirstPerson.cpp  phd_GenerateW2V hook: the camera rides Lara's head
  StereoMath.h     matrix maths against this engine's conventions
  proxy\           the winmm shim
tools\           PDB extraction, cross-build porting, disassembly, verification
tests\           self-test
PDB\             tomb123.exe + tomb1/2/3.dll and their PDBs (original build)
retail\          Aspyr retail binaries (no PDBs) + port_build.json
gold\            Tomb Raider Gold binaries (no PDBs) + port_build.json
third_party\     OpenVR headers
```

`PDB\` is what makes the tooling reproducible; the address tables are only as
checkable as the symbols they came from.
