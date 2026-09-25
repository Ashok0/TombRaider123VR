#include "Gamepad.h"
#include "Engine.h"
#include "GameDll.h"
#include "Config.h"
#include "FirstPerson.h"
#include "VRSystem.h"
#include "Log.h"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace tr {
namespace {

// --- XInput, declared locally so no SDK header is needed ---------------------
#pragma pack(push, 1)
struct XGamepad {
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX;
    int16_t  sThumbLY;
    int16_t  sThumbRX;
    int16_t  sThumbRY;
};
struct XState {
    uint32_t dwPacketNumber;
    XGamepad Gamepad;
};
#pragma pack(pop)

// Standard XInput button bits, exactly as inputUpdate() decodes them.
enum : uint16_t {
    XB_DPAD_UP        = 0x0001,
    XB_DPAD_DOWN      = 0x0002,
    XB_DPAD_LEFT      = 0x0004,
    XB_DPAD_RIGHT     = 0x0008,
    XB_START          = 0x0010,
    XB_BACK           = 0x0020,
    XB_LEFT_THUMB     = 0x0040,
    XB_RIGHT_THUMB    = 0x0080,
    XB_LEFT_SHOULDER  = 0x0100,
    XB_RIGHT_SHOULDER = 0x0200,
    XB_A              = 0x1000,
    XB_B              = 0x2000,
    XB_X              = 0x4000,
    XB_Y              = 0x8000,
};

typedef uint32_t (__stdcall* Fn_XInputGetState)(uint32_t, XState*);

Fn_XInputGetState* g_slot     = nullptr;   // the engine's _XInputGetState global
Fn_XInputGetState  g_original = nullptr;   // the real one, for a physical pad
bool               g_installed = false;
uint32_t           g_packet    = 0;
uint64_t           g_lastRaw[2] = { 0, 0 };
bool               g_loggedOnce = false;
int                g_lastWater  = -2;
bool               g_viewToggleHeld = false;

int16_t Axis(float v) {
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    const float s = v * 32767.0f;
    return static_cast<int16_t>(s);
}

uint8_t Trig(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < 0.0f) v = 0.0f;
    return static_cast<uint8_t>(v * 255.0f);
}

// Build the pad state from the two controllers.
//
// The mapping targets the scheme the game expects:
//   Move        left stick        Jump    A      Roll   B
//   Look        right stick       Action  Y      Shoot  RT
//   Duck        LB                Equip   LT     Sprint L3
//   Walk        LS + RB           System  X      Photo  L3 + R3
//   D-pad       R3 + left stick   Graphics Y + RT
//   View        Y + LT           Action  LB + RB (both grips)
//
// Two of these are deliberately not the flat-screen defaults, because they suit
// VR hands better:
//
//   Walk is the RIGHT GRIP rather than a face button, so it can be held while
//   the left thumb keeps moving. The game binds Walk to XInput X, so the grip
//   emits X. It emits RIGHT_SHOULDER as well so the optional RT + RB pitch
//   chord stays reachable; X and RB never collide in practice.
//
//   System is the left hand's lower face button, sending BACK. Touch has no
//   Start or Back of its own, and putting either on a chord made it awkward to
//   reach mid-play.
void BuildState(XState& out, bool& shifted, bool gameplay, bool& gripAction) {
    VRSystem::HandState h[2];
    VR().ReadControllers(h);

    std::memset(&out, 0, sizeof(out));
    out.dwPacketNumber = ++g_packet;

    const VRSystem::HandState& L = h[0];
    const VRSystem::HandState& R = h[1];
    gripAction = gameplay && L.grip > 0.5f && R.grip > 0.5f;

    uint16_t b = 0;
    if (R.btnLower)   b |= XB_A;               // Jump
    if (R.btnUpper)   b |= XB_B;               // Roll
    if (L.btnUpper)   b |= XB_Y;               // Action
    if (L.stickClick) b |= XB_LEFT_THUMB;      // Sprint

    // --- D-pad shift --------------------------------------------------------
    //
    // Hold R3 and the left stick emits D-pad directions instead of movement.
    // The axes are zeroed while shifted, or you would walk and press a
    // direction at the same time -- which is the whole point of a shift.
    //
    // Only the DOMINANT axis fires, so the stick cannot emit up and left at
    // once. A real D-pad allows diagonals, but this is mostly for menus and
    // inventory, where a diagonal reads as two navigation events and moves the
    // selection twice for one flick.
    //
    // A plain R3 click -- held with the stick centred -- still emits
    // RIGHT_THUMB exactly as before, so whatever the game binds it to survives.
    const bool photoChord = L.stickClick && R.stickClick;
    uint16_t dpad = 0;
    if (Cfg().dpadShift && R.stickClick && !photoChord) {
        const float dz = Cfg().dpadShiftDeadzone;
        if (std::fabs(L.stickX) > std::fabs(L.stickY)) {
            if (L.stickX >  dz) dpad = XB_DPAD_RIGHT;
            if (L.stickX < -dz) dpad = XB_DPAD_LEFT;
        } else {
            if (L.stickY >  dz) dpad = XB_DPAD_UP;
            if (L.stickY < -dz) dpad = XB_DPAD_DOWN;
        }
    }
    b |= dpad;
    if (R.stickClick && !dpad) b |= XB_RIGHT_THUMB;

    // System (BACK) on the left hand's lower face button, or START if the ini
    // asks for the pause menu there instead.
    if (L.btnLower) {
        b |= Cfg().gamepadMenuUsesBack ? XB_BACK : XB_START;
    }

    // The two-grip gameplay chord is a second Action button. Do not send its
    // constituent Duck/Walk buttons, or they can interrupt a block push.
    if (!gripAction && L.grip > 0.5f) b |= XB_LEFT_SHOULDER;

    // Walk modifier. X is what the game binds Walk to; RIGHT_SHOULDER also
    // keeps the optional RT+RB pitch chord reachable.
    if (!gripAction && R.grip > 0.5f)
        b |= static_cast<uint16_t>(XB_X | XB_RIGHT_SHOULDER);

    out.Gamepad.wButtons      = b;
    out.Gamepad.bLeftTrigger  = Trig(L.trigger);    // Equip
    out.Gamepad.bRightTrigger = Trig(R.trigger);   // Shoot
    shifted = (Cfg().dpadShift && R.stickClick && !photoChord);
    float lx = L.stickX, ly = L.stickY;

    out.Gamepad.sThumbLX      = shifted ? 0 : Axis(lx);
    out.Gamepad.sThumbLY      = shifted ? 0 : Axis(ly);
    out.Gamepad.sThumbRX      = Axis(R.stickX);
    out.Gamepad.sThumbRY      = Axis(R.stickY);

    // One-shot dump of the raw legacy masks. Touch's button ids vary between
    // runtimes, so if a button lands in the wrong place this says which mask it
    // actually set rather than costing a play session to find out.
    if (Cfg().gamepadLogButtons) {
        for (int i = 0; i < 2; ++i) {
            if (h[i].rawPressed != g_lastRaw[i]) {
                g_lastRaw[i] = h[i].rawPressed;
                LogF("pad: %s raw=0x%016llX stick=(%+.2f,%+.2f) trig=%.2f grip=%.2f",
                     i == 0 ? "L" : "R",
                     static_cast<unsigned long long>(h[i].rawPressed),
                     h[i].stickX, h[i].stickY, h[i].trigger, h[i].grip);
            }
        }
    }
}

uint32_t __stdcall Detour_XInputGetState(uint32_t userIndex, XState* state) {
    if (!state) return ERROR_BAD_ARGUMENTS;

    // Only pad 0 is ours. Let the engine see the other three as absent, or as
    // whatever real hardware reports.
    if (userIndex != 0) {
        return g_original ? g_original(userIndex, state)
                          : ERROR_DEVICE_NOT_CONNECTED;
    }

    if (!Cfg().gamepadEnabled || !VR().active()) {
        g_viewToggleHeld = false;
        return g_original ? g_original(userIndex, state)
                          : ERROR_DEVICE_NOT_CONNECTED;
    }

    XState mine{};
    bool shifted = false;
    const bool gameplay = !InInventory() && !InTitle() && !InCutscene();
    bool gripAction = false;
    BuildState(mine, shifted, gameplay, gripAction);

    // Merge a physical pad if one is plugged in, so it keeps working.
    if (g_original) {
        XState real{};
        if (g_original(0, &real) == ERROR_SUCCESS) {
            mine.Gamepad.wButtons = static_cast<uint16_t>(
                mine.Gamepad.wButtons | real.Gamepad.wButtons);
            if (real.Gamepad.bLeftTrigger  > mine.Gamepad.bLeftTrigger)
                mine.Gamepad.bLeftTrigger  = real.Gamepad.bLeftTrigger;
            if (real.Gamepad.bRightTrigger > mine.Gamepad.bRightTrigger)
                mine.Gamepad.bRightTrigger = real.Gamepad.bRightTrigger;
            if (std::abs((int)real.Gamepad.sThumbLX) > std::abs((int)mine.Gamepad.sThumbLX))
                mine.Gamepad.sThumbLX = real.Gamepad.sThumbLX;
            if (std::abs((int)real.Gamepad.sThumbLY) > std::abs((int)mine.Gamepad.sThumbLY))
                mine.Gamepad.sThumbLY = real.Gamepad.sThumbLY;
            if (std::abs((int)real.Gamepad.sThumbRX) > std::abs((int)mine.Gamepad.sThumbRX))
                mine.Gamepad.sThumbRX = real.Gamepad.sThumbRX;
            if (std::abs((int)real.Gamepad.sThumbRY) > std::abs((int)mine.Gamepad.sThumbRY))
                mine.Gamepad.sThumbRY = real.Gamepad.sThumbRY;
        }
    }

    // Photo Mode's native controller binding is L3+R3. Leave the buttons intact
    // and only neutralise the axes while both are clicked, so stick drift cannot
    // move Lara or the camera on the same poll that enters Photo Mode.
    constexpr uint16_t photoMask = XB_LEFT_THUMB | XB_RIGHT_THUMB;
    const bool photoChord = (mine.Gamepad.wButtons & photoMask) == photoMask;
    if (photoChord) {
        mine.Gamepad.sThumbLX = mine.Gamepad.sThumbLY = 0;
        mine.Gamepad.sThumbRX = mine.Gamepad.sThumbRY = 0;
        shifted = false;
    }

    // Y+LT switches the complete first-person package on its rising edge.
    // Y+RT becomes the Xbox Menu/Start button, which is the game's native
    // classic/remastered graphics toggle. The view chord takes priority if
    // both triggers happen to be down. Consume the constituent actions so a
    // view/graphics switch cannot also use Action, draw guns or shoot.
    const bool yHeld = (mine.Gamepad.wButtons & XB_Y) != 0;
    const bool viewToggle = gameplay && yHeld && mine.Gamepad.bLeftTrigger > 30;
    const bool graphicsToggle = gameplay && !viewToggle && yHeld &&
                                mine.Gamepad.bRightTrigger > 30;
    if (viewToggle && !g_viewToggleHeld) FirstPersonToggle();
    g_viewToggleHeld = viewToggle;
    if (viewToggle) {
        mine.Gamepad.wButtons &= static_cast<uint16_t>(~XB_Y);
        mine.Gamepad.bLeftTrigger = 0;
    } else if (graphicsToggle) {
        mine.Gamepad.wButtons &= static_cast<uint16_t>(~XB_Y);
        mine.Gamepad.wButtons |= XB_START;
        mine.Gamepad.bRightTrigger = 0;
    }
    // Add the grip Action after Y+trigger handling. Holding both grips while
    // equipping or shooting must not switch the view or graphics mode.
    if (gripAction && !viewToggle && !graphicsToggle)
        mine.Gamepad.wButtons |= XB_Y;

    // Transform the MERGED state so a physical Xbox pad has the same heading
    // and cannot reintroduce the engine's right-stick camera orbit.
    float lx = mine.Gamepad.sThumbLX / 32767.0f;
    float ly = mine.Gamepad.sThumbLY / 32767.0f;
    float rx = mine.Gamepad.sThumbRX / 32767.0f;
    const bool jumpPressed = (mine.Gamepad.wButtons & XB_A) != 0;
    FirstPersonInput(lx, ly, rx, shifted, jumpPressed);
    mine.Gamepad.sThumbLX = Axis(lx);
    mine.Gamepad.sThumbLY = Axis(ly);
    mine.Gamepad.sThumbRX = Axis(rx);

    // Hold RT + RB to decouple pitch for as long as both are held. Read off the
    // MERGED state for the same reason the suppression below is applied here: a
    // physical pad's RT and RB have to reach it too.
    //
    // 30 is XInput's own XINPUT_GAMEPAD_TRIGGER_THRESHOLD -- what the platform
    // calls a pressed trigger -- rather than a number invented here.
    const bool chord = Cfg().decoupledPitchChord &&
                       mine.Gamepad.bRightTrigger > 30 &&
                       (mine.Gamepad.wButtons & XB_RIGHT_SHOULDER) != 0;

    // Applied AFTER the merge, deliberately. Zeroing it back in BuildState
    // would only cover the Touch controllers -- the merge above takes whichever
    // source is larger, so a physical pad plugged in would put stick pitch
    // straight back and the setting would appear not to work.
    //
    // The chord is a RELEASE, never a trigger: it hands stick pitch back while
    // held, and does nothing at all when decoupledPitch is off. Only this
    // setting decides whether pitch is taken away in the first place.
    //
    // Written twice wrongly before this. An OR made the chord dead weight for
    // anyone running decoupledPitch=1 -- the only people who want it. An XOR
    // then made it take pitch AWAY when the setting was off, so the chord meant
    // opposite things depending on a value you cannot see while playing. A
    // momentary control has to do one thing.
    //
    // Shoot and Walk are left intact, so holding it costs none of the actions
    // the two buttons already do.
    // Swimming is the exception, and it is not a comfort call. TR steers the
    // swim with the LOOK axis, so suppressing pitch removes the ability to dive
    // or surface at all -- and the head cannot stand in for it, because the head
    // turns the VIEW while the stick turns LARA.
    //
    // lara.water_status is Lara's OWN state, so it goes true the moment she is
    // in the water and false the moment she is out, whatever the camera is
    // doing. An earlier attempt read the camera room's water flag and failed
    // exactly where it mattered: during a surface swim the camera sits in the
    // AIR room above the water and reported dry for the entire swim.
    //
    // Anything but ABOVE_WATER counts -- SURFACE and WADE included, since both
    // steer with the look axis too.
    const int  water    = Cfg().decoupledPitchWaterOff ? LaraWaterStatus() : -1;
    const bool swimming = (water > 0);
    if (water != g_lastWater) {
        g_lastWater = water;
        static const char* kNames[5] = { "above water", "UNDERWATER",
                                         "SURFACE", "flycheat", "WADE" };
        LogF("pad: water_status=%d (%s) -- stick pitch %s", water,
             (water >= 0 && water <= 4) ? kNames[water] : "unknown",
             swimming ? "RESTORED for swimming" : "decoupled");
    }

    if (Cfg().decoupledPitch && !chord && !swimming) mine.Gamepad.sThumbRY = 0;

    *state = mine;
    return ERROR_SUCCESS;
}

} // namespace

void GamepadUpdate() {
    if (!Cfg().gamepadEnabled) return;

    if (!g_slot) {
        g_slot = reinterpret_cast<Fn_XInputGetState*>(XInputGetStateSlot());
        if (!g_slot) return;
    }

    // WinMain resolves XInput before the render loop starts, but install
    // defensively: if the slot is still null we simply try again next frame.
    Fn_XInputGetState cur = *g_slot;
    if (cur == &Detour_XInputGetState) return;          // already ours

    if (cur) g_original = cur;                          // remember the real one
    *g_slot = &Detour_XInputGetState;

    if (!g_installed) {
        g_installed = true;
        LogF("pad: Touch controllers presented as an Xbox pad "
             "(_XInputGetState %p -> %p, real %p)",
             (void*)g_slot, (void*)&Detour_XInputGetState, (void*)g_original);
    }
    if (!g_loggedOnce) {
        g_loggedOnce = true;
        LogF("pad: move=Lstick look=Rstick%s jump=A(R lower) roll=B(R upper) "
             "action=Y(L upper)/LB+RB(both grips) system=%s(L lower) "
             "walk=LS+RB(R grip) duck=LB(L grip) equip=LT shoot=RT "
             "sprint=L3 photo=L3+R3 "
             "graphics=Y+RT view=Y+LT%s",
             Cfg().decoupledPitch
                 ? (Cfg().decoupledPitchChord
                        ? "(yaw only; hold RT+RB for pitch)"
                        : "(yaw only, pitch decoupled)")
                 : "",
             Cfg().gamepadMenuUsesBack ? "BACK" : "START",
             Cfg().dpadShift ? " dpad=R3+Lstick" : "");
    }
}

void GamepadShutdown() {
    if (g_slot && *g_slot == &Detour_XInputGetState) {
        *g_slot = g_original;
        Log("pad: _XInputGetState restored");
    }
    g_slot = nullptr;
    g_original = nullptr;
    g_installed = false;
    g_viewToggleHeld = false;
}

} // namespace tr
