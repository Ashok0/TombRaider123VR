#include "Gamepad.h"
#include "Engine.h"
#include "GameDll.h"
#include "Config.h"
#include "VRSystem.h"
#include "Log.h"
#include "FirstPerson.h"
#include "InlineHook.h"
#include "WorldInventory.h"

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
int                g_lastZoom   = -1;   // 0/1, -1 = not yet logged
bool               g_viewToggleHeld = false;
inventory::Session g_worldInventory;
vr::HmdMatrix34_t g_inventoryAnchor{};

namespace ammoinput {
// Native DoHotkeys handles ammo flags, cycling order, cooldown and feedback.
// Verified against all four DLLs by tools/verify_inventory_ammo.py.
struct Build { uint32_t timestamp,hotkeys,app,level;uint8_t savedRegister; };
constexpr Build builds[]={
    {0x696B4999,0x16950,0x663F08,0,0x56},
    {0x696B499C,0x15320,0x66D1A0,0x65EC40,0x55},
    {0x68C12FDA,0x16550,0x664E48,0,0x56},
    {0x68C12FE9,0x15400,0x66D0E0,0x65EB80,0x55}
};
hook::InlineHook hotkeysHook;
const GameDllLayout* dll=nullptr;
const Build* build=nullptr;
uint64_t base=0,failedBase=0,queuedAt=0;
int queuedGun=0;
uint8_t* queuedItem=nullptr;
bool held=false,consumeHold=false;

bool Bound() { return dll && base && dll==GameDllBound() && base==GameDllBase(); }
bool Gameplay() {
    return Bound() && Cfg().enabled && Cfg().gamepadEnabled && VR().active() &&
        CurrentGame()==dll->game && !InInventory() && !InTitle() && !InFMV() &&
        !AppState(drva::app_off::photoMode);
}
int Gun() { return *reinterpret_cast<int16_t*>(base+dll->lara+4); }
int Status() { return *reinterpret_cast<int16_t*>(base+dll->lara+2); }
uint8_t* Item() { return *reinterpret_cast<uint8_t**>(base+dll->laraItem); }
int AmmoKey(int gun) {
    if(dll->game==0) return gun==4 ? 1 : gun==5 ? 4 : gun==6 ? 5 : -1;
    const int level=*reinterpret_cast<uint8_t*>(base+build->level);
    // TR5 reuses the pistol/shotgun hotkeys for HK/grappling gun in VCI.
    return gun==4 && level<11 ? 1 : gun==5 && level>=11 ? 0 : -1;
}
void __cdecl Hotkeys() {
    const int gun=queuedGun;queuedGun=0; // one native attempt per physical press
    const int key=gun && Gameplay() && GetTickCount64()-queuedAt<500 &&
        Gun()==gun && Status()==4 && Item()==queuedItem ? AmmoKey(gun) : -1;
    auto original=hotkeysHook.Original<void (__cdecl*)()>();
    if(key<0) { original();return; }
    auto* app=*reinterpret_cast<uint8_t**>(base+build->app);
    if(!app) { original();return; }
    // Scope the keyboard weapon keys and pad hotkey modifier to this one
    // native call. Other input and ammo counts are never written by the mod.
    uint8_t saved[6];std::memcpy(saved,app+0x89c,6);
    const uint8_t modifier=app[0x8f2];
    std::memset(app+0x89c,0,6);app[0x89c+key]=1;app[0x8f2]=0;
    original();
    std::memcpy(app+0x89c,saved,6);app[0x8f2]=modifier;
    LogF("ammo: X routed to native weapon hotkey (game=%d weapon=%d)",dll->game,gun);
}
void Reset() {
    hotkeysHook.Remove();dll=nullptr;build=nullptr;base=0;
    held=consumeHold=false;queuedGun=0;queuedItem=nullptr;
}
void Update() {
    const auto* current=GameDllBound();const auto address=GameDllBase();
    if(dll && (dll!=current || base!=address || !Cfg().enabled || !Cfg().gamepadEnabled)) Reset();
    if(dll || !current || !address || address==failedBase || !Cfg().enabled || !Cfg().gamepadEnabled) return;
    for(const auto& row:builds) if(row.timestamp==current->timestamp) {
        const uint8_t bytes[]={0x40,row.savedRegister,0x48,0x83,0xec,0x30};
        if(!hotkeysHook.Install(reinterpret_cast<void*>(address+row.hotkeys),
                reinterpret_cast<void*>(&Hotkeys),6,bytes,6,"InventoryAmmoHotkeys")) {
            failedBase=address;Log("ammo: native hotkey hook verification failed");return;
        }
        dll=current;base=address;build=&row;
        Log("ammo: armed X routing to native ammo hotkeys ready");return;
    }
}
void Apply(XGamepad& pad) {
    const bool down=(pad.wButtons&XB_BACK)!=0;
    if(!Gameplay()) { held=down;consumeHold=false;queuedGun=0;return; }
    const bool armed=Gun()>=1 && Gun()<=6 && Status()>=2 && Status()<=4;
    if(down && !held) {
        consumeHold=armed;
        if(armed && Status()==4 && hotkeysHook.installed() && AmmoKey(Gun())>=0) {
            queuedGun=Gun();queuedItem=Item();queuedAt=GetTickCount64();
        }
    }
    if(!down) consumeHold=false;
    // Keep swallowing the SAME press if holstering completes while X is held.
    if(down && (consumeHold || armed)) {
        consumeHold=true;
        pad.wButtons &= uint16_t(~XB_BACK);
        g_worldInventory.pending=false;
    }
    held=down;
}
} // namespace ammoinput

bool WorldInventoryAllowed() {
    const int game=CurrentGame();
    return Cfg().enabled && Cfg().gamepadEnabled && Cfg().worldInventory &&
        !Cfg().monoTracking && VR().active() && VR().poseValid() &&
        (game==0 || game==1) && !InTitle() && !InFMV() &&
        !AppState(drva::app_off::photoMode);
}

bool ApplyWorldInventory(XGamepad& pad,bool& shifted) {
    inventory::Input in{pad.wButtons,pad.sThumbLX/32767.f,pad.sThumbLY/32767.f,
        pad.bLeftTrigger,pad.bRightTrigger,shifted};
    bool owned=false;
    if(g_worldInventory.Update(in,WorldInventoryAllowed(),InInventory(),CurrentGame(),
        GetTickCount64(),Cfg().dpadShiftDeadzone,owned)) {
        VR().LevelHeadPose(g_inventoryAnchor);
        Log("inventory: unarmed X prepared level eye-height anchor with horizontal facing");
    }
    if(owned) {
        pad.wButtons=in.buttons;
        pad.sThumbLX=pad.sThumbLY=pad.sThumbRX=pad.sThumbRY=0;
        pad.bLeftTrigger=pad.bRightTrigger=0;
        shifted=true;
    }
    return owned;
}

struct PadFocusBuild { uint32_t timestamp,update,messages,active,cookie; };
constexpr PadFocusBuild kPadFocusBuilds[]={
    {0x696B49A7,0x5C10,0x5A70,0x698668,0x167080},
    {0x68639C21,0x5C30,0x5A90,0x69A808,0x169080},
    {0x68C12FEB,0x5C30,0x5A90,0x69A808,0x169080}
};
hook::InlineHook g_hPadUpdate,g_hPadMessages;
int32_t* g_padWindowActive=nullptr;
bool g_padFocusAttempted=false;

bool VRPadPollingAllowed() {
    const int game=CurrentGame();
    return Cfg().enabled && Cfg().gamepadEnabled && VR().active() && (game==0 || game==1);
}

// Native inputUpdate pumps messages, then skips ALL pads when isActive==0.
// Override that flag only after the pump and only until the pad update returns.
// Window activation, keyboard/mouse messages and all other callers see the real
// state. Do not steal desktop focus or suppress WM_ACTIVATEAPP/inputReset.
struct PadFocusScope {
    int32_t saved=0;
    bool overridden=false;
    void Restore() {
        if(overridden) *g_padWindowActive=saved;
        overridden=false;
    }
    void AfterMessages() {
        if(g_padWindowActive && VRPadPollingAllowed()) {
            saved=*g_padWindowActive;
            *g_padWindowActive=1;
            overridden=true;
        }
    }
    ~PadFocusScope() { Restore(); }
};
PadFocusScope* g_padFocusScope=nullptr;

void __cdecl Detour_PadMessages() {
    if(g_padFocusScope) g_padFocusScope->Restore();
    g_hPadMessages.Original<void (__cdecl*)()>()();
    if(g_padFocusScope) g_padFocusScope->AfterMessages();
}

void __cdecl Detour_PadUpdate() {
    PadFocusScope scope;
    auto* previous=g_padFocusScope;
    g_padFocusScope=&scope;
    g_hPadUpdate.Original<void (__cdecl*)()>()();
    g_padFocusScope=previous;
}

void InstallPadFocusHooks() {
    if(g_padFocusAttempted || !Base()) return;
    g_padFocusAttempted=true;
    for(const auto& b:kPadFocusBuilds) if(b.timestamp==L().timestamp) {
        const auto base=Base();
        // Verify the native message call and following focus comparison too;
        // a matching function prologue alone cannot establish their meaning.
        uint8_t gate[]={0xe8,0,0,0,0,0x83,0x3d,0,0,0,0,0};
        const int32_t messageDisp=int32_t(b.messages-(b.update+0x18));
        const int32_t activeDisp=int32_t(b.active-(b.update+0x1f));
        std::memcpy(gate+1,&messageDisp,4);std::memcpy(gate+7,&activeDisp,4);
        if(std::memcmp(reinterpret_cast<void*>(base+b.update+0x13),gate,sizeof(gate))) break;
        const auto install=[&](hook::InlineHook& h,uint32_t rva,void* detour,const char* name) {
            uint8_t prologue[]={0x48,0x83,0xec,0x78,0x48,0x8b,0x05,0,0,0,0};
            const int32_t cookieDisp=int32_t(b.cookie-(rva+11));
            std::memcpy(prologue+7,&cookieDisp,4);
            const int rip=7;
            return h.Install(reinterpret_cast<void*>(base+rva),detour,11,prologue,11,name,&rip,1);
        };
        if(install(g_hPadMessages,b.messages,reinterpret_cast<void*>(&Detour_PadMessages),"VRPadMessages") &&
           install(g_hPadUpdate,b.update,reinterpret_cast<void*>(&Detour_PadUpdate),"VRPadUpdate")) {
            g_padWindowActive=reinterpret_cast<int32_t*>(base+b.active);
            Log("pad: TR4/5 VR controller polling survives desktop focus loss; native window messages retained");
            return;
        }
        g_hPadUpdate.Remove();g_hPadMessages.Remove();
        break;
    }
    Log("pad: VR background controller polling unavailable; native focus behavior retained");
}

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
//   Walk        LS + RB           Sneak   RB+Y   Photo  L3 + R3
//   System      X                 D-pad   R3 + left stick
//   Action      LB + RB (both grips)           Menu Y + LT held 3s
//
// Two of these are deliberately not the flat-screen defaults, because they suit
// VR hands better:
//
//   Walk is the RIGHT GRIP rather than a face button, so it can be held while
//   the left thumb keeps moving. The game binds Walk to XInput X. TR6 binds
//   Sneak to RIGHT_SHOULDER, so emitting both on every grip press made Walk and
//   Sneak fire together. RIGHT_SHOULDER is emitted only for the RB+Y Sneak
//   chord. Photo Mode's native chord is L3+R3, not LB+RB.
//
//   System is the left hand's lower face button, sending BACK. Touch has no
//   Start or Back of its own, and putting either on a chord made it awkward to
//   reach mid-play.
// Menu chord state. Wall clock rather than frames: the poll rate belongs to
// the game, not to us, so "three seconds" must not become "three seconds at
// whatever frame rate happens to be running".
uint64_t g_menuChordSince = 0;   // when the chord was first seen held, 0 = not
uint64_t g_menuPressUntil = 0;   // synthesised START is held until this tick
bool     g_menuChordFired = false;

bool ApplyViewChords(XGamepad& pad) {
    // Same ownership in gameplay, inventory and title. At the launcher (-1),
    // remember the view selection for the next TR4/5 game. START is graphics.
    const bool fpChords = Cfg().firstPerson && CurrentGame() != 2;
    const bool yHeld = (pad.wButtons & XB_Y) != 0;
    const bool viewToggle = fpChords && yHeld && pad.bLeftTrigger > 30;
    const bool graphicsToggle = fpChords && !viewToggle && yHeld &&
        pad.bRightTrigger > 30;
    if (viewToggle && !g_viewToggleHeld) FirstPersonToggle();
    g_viewToggleHeld = viewToggle;
    if (viewToggle) {
        pad.wButtons &= static_cast<uint16_t>(~XB_Y);
        pad.bLeftTrigger = pad.bRightTrigger = 0;
    } else if (graphicsToggle) {
        pad.wButtons &= static_cast<uint16_t>(~XB_Y);
        pad.wButtons |= XB_START;
        pad.bRightTrigger = 0;
    }
    if (fpChords) {
        g_menuChordSince = 0;
        g_menuChordFired = false;
        g_menuPressUntil = 0;
    }
    return viewToggle || graphicsToggle;
}

void BuildStateFromHands(const VRSystem::HandState h[2], XState& out,
                         bool& shifted, bool gameplay, bool& touchGripAction) {
    std::memset(&out, 0, sizeof(out));
    out.dwPacketNumber = ++g_packet;

    const VRSystem::HandState& L = h[0];
    const VRSystem::HandState& R = h[1];
    const unsigned holsterGrips=FirstPersonHolsterGrips(L.grip>.5f,R.grip>.5f,L.valid,R.valid,
        gameplay && !L.btnUpper);
    touchGripAction = gameplay && !holsterGrips && L.grip > 0.5f && R.grip > 0.5f;
    const bool switchGrip = FirstPersonSwitchGrip(R.grip > 0.5f,
        gameplay && R.valid && !(holsterGrips&2) && !touchGripAction && !L.btnUpper);

    // Set once the Menu chord fires, and honoured where the triggers are
    // written further down. Declared here because that assignment happens after
    // this block -- setting out.Gamepad.bLeftTrigger from inside the chord would
    // simply be overwritten.
    bool suppressLeftTrigger = false;

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

    // --- Menu chord: Y + LT held ------------------------------------------
    //
    // Touch has no Start or Back of its own. The System button above sends one
    // of them; this reaches the other without spending a second button.
    //
    // Y is Action and LT is Equip, so the pair happens in normal play -- hence
    // the long hold. It fires ONCE per hold: after firing, the chord must be
    // released before it can fire again, so leaning on it does not machine-gun
    // the pause screen.
    //
    // Y and LT are suppressed from the moment it fires until release. They were
    // already sent for the three seconds it took to arm, which cannot be undone,
    // but there is no reason to keep grabbing and drawing weapons afterwards.
    const bool fpChords = Cfg().firstPerson && CurrentGame() != 2;
    if (!fpChords && Cfg().menuChordSeconds > 0.0f) {
        const bool     chord = L.btnUpper && L.trigger > 0.5f;
        const uint64_t now   = GetTickCount64();

        if (!chord) {
            g_menuChordSince = 0;
            g_menuChordFired = false;
        } else {
            if (g_menuChordSince == 0) g_menuChordSince = now;
            const uint64_t needed = (uint64_t)(Cfg().menuChordSeconds * 1000.0f);
            if (!g_menuChordFired && now - g_menuChordSince >= needed) {
                g_menuChordFired = true;
                g_menuPressUntil = now +
                    (uint64_t)(Cfg().menuChordPressSeconds * 1000.0f);
                Log("pad: Menu (START) sent -- Y + LT held");
            }
        }

        if (now < g_menuPressUntil) b |= XB_START;

        if (g_menuChordFired) {
            b &= (uint16_t)~XB_Y;          // stop Action
            suppressLeftTrigger = true;    // stop Equip
        }
    }

    // Both grips hold Action during gameplay. Their individual Duck/Walk bits
    // must not interrupt a block push. In menus their normal bindings remain.
    const bool leftGrip = L.grip > 0.5f;
    if (leftGrip && !touchGripAction && !(holsterGrips&1)) b |= XB_LEFT_SHOULDER;
    if(FirstPersonPhysicalCrouch(gameplay && !touchGripAction)) b|=XB_LEFT_SHOULDER;

    // Right grip is the physical VR "RB" and normally emits only XInput X,
    // which is Walk. TR6 binds Sneak to XInput RIGHT_SHOULDER. Holding Y with
    // the right grip consumes both ordinary actions and emits only Sneak.
    // Dual-grip Action takes priority over that chord in gameplay. In first
    // person, preserve Y for holstering even while the right grip is held.
    const bool rightGrip = R.grip > 0.5f;
    const bool sneakChord = rightGrip && L.btnUpper && !touchGripAction &&
        !(gameplay && FirstPersonActive());
    if (rightGrip && !touchGripAction && !switchGrip && !(holsterGrips&2)) b |= XB_X;
    if (sneakChord) {
        b &= static_cast<uint16_t>(~(XB_X | XB_Y));
        b |= XB_RIGHT_SHOULDER;
    }

    out.Gamepad.wButtons      = b;
    out.Gamepad.bLeftTrigger  = suppressLeftTrigger ? 0
                                                    : Trig(L.trigger);  // Equip
    out.Gamepad.bRightTrigger = Trig(R.trigger);   // Shoot
    shifted                  = (Cfg().dpadShift && R.stickClick && !photoChord);
    out.Gamepad.sThumbLX      = shifted ? 0 : Axis(L.stickX);
    out.Gamepad.sThumbLY      = shifted ? 0 : Axis(L.stickY);
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

void BuildState(XState& out, bool& shifted, bool gameplay, bool& touchGripAction) {
    VRSystem::HandState h[2];
    VR().ReadControllers(h);
    BuildStateFromHands(h, out, shifted, gameplay, touchGripAction);
}

bool GameplayInputActive() {
    const int game = CurrentGame();
    return game >= 0 && game <= 2 &&
        !InInventory() && !InTitle() && !InFMV();
}

void ApplyMergedChords(XGamepad& pad, bool gameplay,
                       bool touchGripAction, bool& shifted) {
    // Native Photo Mode is L3+R3 in TR4/5/6. Preserve that chord even when
    // stick drift would otherwise turn R3 into a D-pad shift or move Lara.
    constexpr uint16_t photoMask = XB_LEFT_THUMB | XB_RIGHT_THUMB;
    if ((pad.wButtons & photoMask) == photoMask) {
        pad.sThumbLX = pad.sThumbLY = 0;
        pad.sThumbRX = pad.sThumbRY = 0;
        shifted = false;
    }

    const bool actionConsumed = ApplyViewChords(pad);
    if (FirstPersonGunTriggers(pad.bLeftTrigger,pad.bRightTrigger,actionConsumed,
                               (pad.wButtons & XB_Y)!=0))
        pad.wButtons &= static_cast<uint16_t>(~XB_Y);
    // Add synthetic Action AFTER Y+trigger handling so holding grips with a
    // trigger cannot switch the view or graphics. This also supports a physical
    // Xbox pad's LB+RB chord when one is merged with Touch input.
    constexpr uint16_t gripMask = XB_LEFT_SHOULDER | XB_RIGHT_SHOULDER;
    const bool gripAction = gameplay && (touchGripAction ||
        (pad.wButtons & gripMask) == gripMask);
    if (gripAction) {
        pad.wButtons &= static_cast<uint16_t>(~(gripMask | XB_X));
        if (!actionConsumed) pad.wButtons |= XB_Y;
    }
}

uint32_t __stdcall Detour_XInputGetState(uint32_t userIndex, XState* state) {
    FirstPersonControlModeUpdate();
    if (!state) return ERROR_BAD_ARGUMENTS;

    // Only pad 0 is ours. Let the engine see the other three as absent, or as
    // whatever real hardware reports.
    if (userIndex != 0) {
        return g_original ? g_original(userIndex, state)
                          : ERROR_DEVICE_NOT_CONNECTED;
    }

    if (!Cfg().gamepadEnabled || !VR().active()) {
        g_worldInventory.Reset();
        return g_original ? g_original(userIndex, state)
                          : ERROR_DEVICE_NOT_CONNECTED;
    }

    XState mine{};
    bool shifted = false;
    const bool gameplay = GameplayInputActive();
    bool touchGripAction = false;
    BuildState(mine, shifted, gameplay, touchGripAction);

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

    ammoinput::Apply(mine.Gamepad);
    const bool inventoryOwned=ApplyWorldInventory(mine.Gamepad,shifted);
    if(!inventoryOwned) ApplyMergedChords(mine.Gamepad, gameplay, touchGripAction, shifted);
    else {
        // Reset trigger/chord history too; a held gun trigger before opening
        // must not survive the menu as a queued shot or a missed fresh press.
        FirstPersonGunTriggers(mine.Gamepad.bLeftTrigger,mine.Gamepad.bRightTrigger,true,false);
        g_viewToggleHeld=false;
        g_menuChordSince=g_menuPressUntil=0;
        g_menuChordFired=false;
    }

    float fpLeftX = mine.Gamepad.sThumbLX / 32767.0f;
    float fpLeftY = mine.Gamepad.sThumbLY / 32767.0f;
    float fpRightX = mine.Gamepad.sThumbRX / 32767.0f;
    const bool jumpPressed = !inventoryOwned && (mine.Gamepad.wButtons & XB_A) != 0;
    FirstPersonInput(fpLeftX, fpLeftY, fpRightX, shifted, jumpPressed);
    mine.Gamepad.sThumbLX = Axis(fpLeftX);
    mine.Gamepad.sThumbLY = Axis(fpLeftY);
    mine.Gamepad.sThumbRX = Axis(fpRightX);

    // Hold RT + RB to decouple pitch for as long as both are held. Touch's
    // physical RB now reports Walk as X rather than always leaking the Sneak
    // shoulder bit, so either X (Walk) or RIGHT_SHOULDER (a physical Xbox RB)
    // satisfies the chord after controller states have been merged.
    //
    // 30 is XInput's own XINPUT_GAMEPAD_TRIGGER_THRESHOLD -- what the platform
    // calls a pressed trigger -- rather than a number invented here.
    const bool chord = Cfg().decoupledPitchChord &&
                       mine.Gamepad.bRightTrigger > 30 &&
                       (mine.Gamepad.wButtons
                        & (XB_X | XB_RIGHT_SHOULDER)) != 0;

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
        const char* waterName = (CurrentGame() == 2 && water >= 0)
            ? (water ? "IN WATER" : "above water")
            : ((water >= 0 && water <= 4) ? kNames[water] : "unknown");
        LogF("pad: water_status=%d (%s) -- stick pitch %s", water,
             waterName,
             swimming ? "RESTORED for swimming" : "decoupled");
    }

    // Zoom (binoculars, or a weapon combined with a laser sight) is the same
    // exception for the same reason: the game's own zoom camera reads stick Y
    // directly for vertical aim once it has taken over, and the head cannot
    // stand in for it there any more than it can underwater. Checked from
    // BinocularOn/BinocularRange in the game DLL -- the same two fields the
    // game's own ProcessLooking reads to decide whether the stick means "look"
    // or "aim the zoom camera" -- so this is the game's own boundary, not an
    // approximation of one.
    const bool zooming = Cfg().decoupledPitchZoomOff && IsOpticsZoomed();
    const int  zoomLog = zooming ? 1 : 0;
    if (zoomLog != g_lastZoom) {
        g_lastZoom = zoomLog;
        LogF("pad: optics zoom %s -- stick pitch %s", zooming ? "ENTERED" : "left",
             zooming ? "RESTORED for the zoom camera" : "decoupled");
    }

    if (Cfg().decoupledPitch && !chord && !swimming && !zooming) mine.Gamepad.sThumbRY = 0;

    *state = mine;
    return ERROR_SUCCESS;
}

} // namespace

bool WorldInventoryActive() {
    g_worldInventory.Sync(WorldInventoryAllowed(),InInventory(),CurrentGame(),GetTickCount64());
    return g_worldInventory.open;
}

Affine WorldInventoryEyeView(Eye eye) {
    return VR().AnchoredEyeView(eye,g_inventoryAnchor);
}

void WorldInventoryRecenter() {
    if(g_worldInventory.pending || g_worldInventory.open) VR().LevelHeadPose(g_inventoryAnchor);
}

void GamepadUpdate() {
    FirstPersonControlModeUpdate();
    ammoinput::Update();
    if (!Cfg().gamepadEnabled) return;
    InstallPadFocusHooks();

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
             "walk=LS+RB(R grip) "
             "sneak=RB+Y duck=LB(L grip) equip=LT shoot=RT sprint=L3 "
             "photo=L3+R3%s",
             Cfg().decoupledPitch
                 ? (Cfg().decoupledPitchChord
                        ? "(yaw only; hold RT+RB for pitch)"
                        : "(yaw only, pitch decoupled)")
                 : "",
             Cfg().gamepadMenuUsesBack ? "BACK" : "START",
             Cfg().dpadShift ? " dpad=R3+Lstick" : "");
        Log("pad: FP motion guns: LT=left pistol/Uzi, RT=right/single gun; press fires, hold repeats; LT draws from unarmed, Y holsters; physical holsters use matching grip; desktop focus does not change bindings");
    }
}

void GamepadShutdown() {
    ammoinput::Reset();
    g_worldInventory.Reset();
    g_hPadUpdate.Remove();g_hPadMessages.Remove();
    g_padWindowActive=nullptr;g_padFocusScope=nullptr;g_padFocusAttempted=false;
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
