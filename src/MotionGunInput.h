#pragma once
#include <cstdint>

namespace tr::motiongun {

// SteamVR can keep controller polling the game after its desktop mirror loses
// Win32 foreground focus. Native gun input keeps running in that situation,
// so the persistent draw adapter must follow VR gameplay, not desktop focus.
inline bool VrGunInputEnabled(bool chordConsumed, bool motionReady,
                             bool desktopForeground) {
    (void)desktopForeground;
    return !chordConsumed && motionReady;
}

// Shared trigger state only. TR1/2/3 weapon IDs and dual/single hands are
// selected by the running game in FirstPerson.cpp.

// LT queues a shot on release to distinguish draw/holster. RT sustains its
// request while held; native LaraGun still controls the weapon's fire rate.
struct TriggerInput {
    bool active=false, waitRelease=false, leftHeld=false, rightHeld=false;
    bool leftWaitRelease=false, rightWaitRelease=false;
    bool leftCanTap=false, leftGesture=false, longFired=false, pending[2]={};
    uint64_t leftSince=0, equipUntil=0;

    void Reset() { *this={}; }
    void Update(bool enabled, bool ready, bool left, bool right, uint64_t now) {
        if (!enabled) { Reset(); return; }
        if (!active) {
            active=true;
            leftWaitRelease=left; rightWaitRelease=right;
        }
        // A hit animation or temporary loss of tracking can re-enable this
        // adapter while one trigger is still held. Suppress that hand until
        // release, but do not block a fresh press on the other controller.
        if (!left) leftWaitRelease=false;
        if (!right) rightWaitRelease=false;
        waitRelease=leftWaitRelease || rightWaitRelease;
        if (!ready) pending[0]=pending[1]=false;
        if (!leftWaitRelease && left && !leftHeld) {
            leftSince=now; leftCanTap=ready; leftGesture=true; longFired=false;
        }
        // Use elapsed wall-clock time, including a release poll which may
        // arrive after the threshold without any intervening held poll.
        if (leftGesture && (left || leftHeld) && !longFired &&
            now-leftSince>=500) {
            longFired=true; leftCanTap=false;
            pending[0]=pending[1]=false;
            equipUntil=now+150;
        }
        if (!leftWaitRelease && !left && leftHeld && !longFired &&
            leftCanTap && ready)
            pending[0]=true;
        if (!rightWaitRelease && right && ready &&
            !longFired && now>=equipUntil)
            pending[1]=true;
        leftHeld=left; rightHeld=right;
        if (!left) { longFired=false; leftCanTap=false; leftGesture=false; }
    }
    bool Equip(uint64_t now) const { return active && now<equipUntil; }
    bool WantsShot() const { return active && (pending[0] || pending[1]); }
    bool Consume(int hand) {
        if (!active || hand<0 || hand>1 || !pending[hand]) return false;
        pending[hand]=false;
        return true;
    }
};

// Adapt the gesture to the game's two native draw styles without modifying
// its settings. Hold mode needs sustained LT until the NEXT long gesture;
// sending only a short pulse makes Lara holster as soon as drawing completes.
struct EquipInput {
    bool initialized=false, wasHold=false, desiredArmed=false, requestHeld=false;
    bool acknowledged=false;
    int requestStatus=0;
    void Reset() { *this={}; }
    bool Update(bool holdMode, int gunStatus, bool request) {
        if (!initialized || wasHold!=holdMode) {
            initialized=true; wasHold=holdMode;
            desiredArmed=gunStatus==2 || gunStatus==4; // drawing / ready
        }
        if (request && !requestHeld) {
            // Inventory/native weapon transitions can leave the cached intent
            // out of sync. A fresh gesture follows settled native state; keep
            // toggling intent only while a draw/holster/action is in progress.
            desiredArmed=gunStatus==0 ? true : gunStatus==4 ? false : !desiredArmed;
            requestStatus=gunStatus;
            acknowledged=false;
        }
        requestHeld=request;
        if (request && gunStatus!=requestStatus) acknowledged=true;
        // In toggle mode stop the pulse once the native state acknowledges it.
        return holdMode ? desiredArmed : request && !acknowledged;
    }
};

// Native masked gun passes include forearm and hand. Keep only the hand
// (the equipped gun is part of that mesh), preserving the full bone palette.
inline uint32_t HandOnlyMask(uint32_t nativeMask) {
    return nativeMask==0x600 ? 0x400 : nativeMask==0x3000 ? 0x2000 :
        nativeMask==0x3600 ? 0x2400 : 0;
}
} // namespace tr::motiongun
