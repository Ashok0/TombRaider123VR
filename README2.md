# Roomscale and rotation — handoff notes

This is a working document for whoever picks up VR locomotion in this mod. It
covers one area only: **positional tracking, turning, and roomscale movement in
first person**. The main `README.md` covers the stereo layer, the address
tables, the culling and sky fixes, and first person itself.

It is written to be useful rather than flattering. Roughly half of what is
described here failed, and the failures are the more valuable half, because each
one cost a headset session to discover and several were things that looked
obviously correct in the code.

Everything below was developed against **Tomb Raider I-III Remastered**, Aspyr
retail / Tomb Raider Gold build (exe PE `0x6A4B7C52`), TR1, SteamVR.

---

## 1. What is in the build right now

All of this is verified working in the headset unless stated otherwise.

| feature | setting | state |
|---|---|---|
| Head translation from a captured neutral | `FirstPersonHeadTranslation=1` | **works** |
| Re-take the neutral | `FirstPersonRecenterKey=0x23` (END) | **works** |
| Lara's body turns to the head | `FirstPersonBodyFollowsHead=1` | **works, but drifts — see §4** |
| Walking in the room walks Lara | `FirstPersonRoomscaleMove=1` | **works** |
| Movement stick rotated into the look direction | `FirstPersonMoveWithHead=1` | **works, camera-relative controls only** |

The code lives in three places:

* `src/VRSystem.cpp` — the neutral, `HeadYawRadians()`, `HeadOffsetInHead()`,
  `DriftNeutralTowardHead()`, `RecenterHead()`.
* `src/FirstPerson.cpp` — `TurnBodyToHead()`, called once per frame from the
  `phd_GenerateW2V` hook, which is the one place the mod is guaranteed to run
  before anything is drawn.
* `src/Gamepad.cpp` — roomscale displacement turned into stick input, and the
  stick rotation, both inside the synthesised XInput state.

---

## 2. Things that are simply true about this game

Established by reading the binary or by measurement. A future attempt should
not have to rediscover these.

**The control scheme is readable, and it changes everything.** `app+0x854`
bit 1 is `new_controls`; bit 0 is `modern` (the HD renderer). `DrawLara` tests
both three instructions into its prologue:

```
0000FD09  mov  edx, dword ptr [rax + 0x854]
0000FD19  test dl, 1                          ; modern -- HD renderer
0000FD22  test dl, 2                          ; new_controls -- camera-relative
```

Under **camera-relative** controls the movement stick is a direction in the
game camera's frame, and **her facing has no effect on where she walks**. Under
**tank** controls the stick's X axis is *turning* and she walks along her own
facing. Code that is right for one is actively harmful in the other: rotating
the stick into the look direction fixes diagonal walking under the first and
stops her turning at all under the second. Read the flag; never assume.

**The game's camera does not follow the player's body.** Turning round in the
room leaves the engine's camera exactly where it was — measured, not assumed:

```
drift: camera=+3.8 head=+76.4 target=+80.0 lara=+80.0 err=+2.9 moving=1
```

`camera` stayed at `+3.8` through an entire session of physical turning.

**`HeadYawRadians()` is in the ROOM's frame, not the camera's.** It is derived
from the headset pose alone. Adding the camera's yaw to it to get a world
direction is the single most expensive mistake in this document (§4.4).

**Her facing is `ITEM_INFO::pos.y_rot`**, 16-bit, wrapping, and writing it is
gameplay state rather than rendering. The engine also writes it — animations
turn her — so anything the mod writes is in a per-frame argument with the game.

**The mod stands down when the game takes the camera.** Fixed cameras
(`camera.type == 1`) and cinematic/heavy ones (`>= 4`) keep their own framing,
so first person switches off for them. Anything the mod was steering goes stale
during that window and must be re-derived on the way back, not caught up
gradually.

---

## 3. What worked

### 3.1 Positional tracking needs a neutral, and the timing of it matters

Head translation was originally off in first person because turning it on put
the player a metre above Lara's head. That was never a tracking problem: a head
position only means something as a displacement *from* somewhere, and the
runtime's origin is not that somewhere. In a standing universe it is the floor,
so the head arrives ~1.7 m up, and at 423 units/metre that is ~700 world units
of camera lift against a Lara who is 762 units tall.

Capture a neutral, measure from it, and it behaves. **When** it is captured
matters as much: taken at the first pose the runtime produces, it is taken while
the headset is still on a desk. It is now taken when first person engages, with
END to re-take it. If the view floats or sinks, that is the neutral, not the
world scale.

### 3.2 Roomscale movement as synthesised stick input

Her position cannot be written — where she stands is the engine's business,
including collision, triggers and floor data. Offering the displacement to the
game as movement-stick input instead means the engine walks her there with all
of that intact.

The detail that makes it feel like stepping rather than a treadmill: the neutral
**drifts after the player along the floor** at the speed she travels, so a step
is gradually consumed and she stops, while a sustained lean keeps her walking.
Height is deliberately excluded from that drift, or standing up slowly counts as
walking.

### 3.3 Instrumentation, far earlier than it was actually done

The drift log prints, once a second, every quantity that could be steering her:
the head's offset from the neutral, what roomscale contributed, what the player
asked for, what the game was handed, and the three angles side by side. It
identified two separate root causes in one play session each, after three or
four rounds of reasoning had failed to. **Reach for this first, not last.**

---

## 4. What did not work, and why

### 4.1 Believing the decompiler's argument list

The `GetJoints` hook gated on its second parameter being null, because that is
how Ghidra prints the call. The parameter is dead; the compiler never
materialises it; the register holds whatever the previous call left there. The
gate rejected every real call and the diagnostic printed nothing at all.

**Lesson:** an unused parameter is where a decompiler's reconstruction is least
trustworthy.

### 4.2 Fixing the stale lean that was not happening

After physical turning, the head ends up 10–30 cm from the neutral, because a
head orbits the spine rather than spinning on the spot. That is real, and it
*can* feed roomscale a steady sideways push. Two builds went into clearing it
faster.

The log then showed roomscale contributing exactly nothing during the drift:

```
lean=(0.00, 0.00) dist=0.00  roomscale=(+0.00, +0.00)  player=1.00  final=(+1.00, -0.05)
```

**Lesson:** the fix was for a plausible mechanism that was not the active one.
Measure which mechanism is firing before choosing between them.

### 4.3 "These two features do the same thing, so disable one"

`MoveWithHead` (rotate the stick into the look direction) and
`BodyFollowsHead` (turn her to the look direction) look redundant. They are not:
under camera-relative controls her facing does not affect movement at all, so
the stick rotation is the *only* thing steering her. Disabling it produced the
clearest symptom of the whole exercise — after a physical turn, **no controller
angle whatsoever would make her walk forward**, because nothing the player
touched was in the equation.

**Lesson:** two things that produce the same effect in one mode may not be
alternatives in another. Enumerate the modes first.

### 4.4 Adding the camera's yaw to a room-frame angle — the feedback loop

`target = cameraYaw + HeadYawRadians()` reads naturally and is a control loop:

1. she walks, and the chase camera swings to follow her;
2. that swing changes `cameraYaw`, which moves the target;
3. she turns further; the camera swings further.

The result is a path that starts correct and curves into a circle, in whichever
direction the camera happened to start swinging — reported exactly as
*"sometimes to the left and sometimes to the right… in a circular pattern"*.

**Lesson:** any term derived from something that follows the output belongs
nowhere near the target. The symptom "starts right, then curves" means a loop,
not an offset, and no amount of sign-flipping will fix it.

### 4.5 The attempted fix for 4.4, which broke rendering

Decoupling the room from the world with a single offset (`world = room +
offset`, camera consulted only as a measurement) is, I still believe, the right
shape. The implementation regressed stereo and frame rate badly enough to be
reverted unexamined. The most likely cause is that her yaw was driven somewhere
extreme, and `PortalCull` expands the draw list from her heading — which would
explain the frame rate and the rendering together. **This was never confirmed**;
if you re-attempt it, log her yaw and the per-frame draw counts before playing.

### 4.6 Two sign errors in one function, in a row

`ControllerYawRadians` was wrong twice: first by negating both `atan2`
arguments, which turns the angle by 180° (she walked exactly backwards), then by
not negating the result, which mirrors it (pointing left walked her right).

The underlying trap: `HeadYawRadians` reads a **view transform**
(tracking→head), while a controller matrix is a **pose** (controller→head).
They are inverses, so the identical `atan2` on the identical column comes out
with the opposite sign.

**Lesson:** when two angles are read from matrices, write down which of them is
a pose and which is a view before comparing them. Better still, derive both from
one published number — two independent calculations of "the same" angle is how
these survived.

### 4.7 Dead zones cannot separate a glance from a turn

Welding her facing to the head means every glance while walking steers her, and
the path bends. A dead zone stops that, and then she sits up to its width off
the look direction, so walking leaves at an angle — the original complaint from
the other side. Collapsing the dead zone only while moving trades one for the
other.

Filtering the yaw (follow sustained turns, ignore brief ones) is better in
principle and was implemented, but never got a clean test before the work was
reverted. **Steering by the controller instead sidesteps the question entirely**
— pointing and looking become different acts — and that is the direction worth
taking.

---

## 5. Where it stands, and what to do next

The remaining known defect: **after turning physically, walking curves**. The
mechanism in §4.4 is understood but its fix is not in the tree.

Two routes, in the order I would try them:

1. **Controller steering** (`ControllerYawRadians`, removed in the revert; the
   history has it). Her facing and the stick rotation both follow where a
   controller points. Glances stop mattering, the filter becomes unnecessary,
   and the room-to-world offset problem is smaller because the controller and
   the head share a tracking frame.
2. **Redo the room-to-world offset from §4.5**, with logging of her yaw and the
   draw counts from the first frame, so the rendering regression is caught in
   the first seconds rather than after a play session.

If neither is wanted, `FirstPersonBodyFollowsHead=0` is a clean fallback:
physical turning stops turning her, nothing writes her facing, and the drift,
the circles and the steering conflicts all disappear. First person, positional
tracking, roomscale movement and stick turning all still work. That is a
defensible shipping state.

### Rules of thumb for this area

* **Log before theorising.** Every root cause here was found by measurement, and
  every wrong fix came from reasoning about plausible mechanisms.
* **Symptoms name their own class.** "Starts right then curves" is a feedback
  loop. "Exactly backwards" is 180°. "Mirrored" is one sign. "Perfect until I
  rotate" is a world-axis constant that should be in her frame. "No input
  changes it" means the thing you are adjusting is not in the equation.
* **One number, published once.** Anything two subsystems need — the steering
  angle above all — should be computed in one place and read by both.
* **Test physical turning separately from stick turning.** They travel different
  paths and only one of them was ever broken at a time.
* **Every change here is gameplay state.** Body turning writes her facing;
  roomscale writes stick input. Anything reading either will see it.
