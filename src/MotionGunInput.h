#pragma once
#include <cstdint>

namespace tr::motiongun {

// AnimatePistols refuses free fire when a native target remains but its arm
// lock has been lost. Controller aim chooses its own target in FireWeapon.
// Hide only this obsolete lock dependency for the duration of animation;
// preserve native target selection for the next normal simulation tick.
struct ScopedControllerAim {
    void*& target;
    void* saved;
    bool active;
    ScopedControllerAim(void*& nativeTarget,bool enabled)
        : target(nativeTarget),saved(nativeTarget),active(enabled) {
        if (active) target=nullptr;
    }
    ~ScopedControllerAim() { if (active) target=saved; }
    ScopedControllerAim(const ScopedControllerAim&)=delete;
    ScopedControllerAim& operator=(const ScopedControllerAim&)=delete;
};

// SteamVR can keep controller polling the game after its desktop mirror loses
// Win32 foreground focus. Native gun input keeps running in that situation,
// so the persistent draw adapter must follow VR gameplay, not desktop focus.
inline bool VrGunInputEnabled(bool chordConsumed, bool motionReady,
                             bool desktopForeground) {
    (void)desktopForeground;
    return !chordConsumed && motionReady;
}

// A press owns one shot, even if released before native animation can fire.
// Held repeats are renewed by input polls, but expire on release. Keeping
// these separate prevents a repeat queued during recoil from becoming an
// unwanted second shot after a tap. Native animation still owns fire rate.
struct TriggerInput {
    bool active=false, held[2]={}, pending[2]={}, pressPending[2]={};

    void Reset() { *this={}; }
    void Clear(int hand) { pending[hand]=pressPending[hand]=false; }
    void Update(bool enabled, bool ready, bool left, bool right, uint64_t /*now*/) {
        if (!enabled) { Reset(); return; }
        active=true;
        const bool down[2]={left,right};
        for (int hand=0;hand<2;++hand) {
            if (!ready) Clear(hand);
            else {
                if (down[hand] && !held[hand]) pressPending[hand]=true;
                pending[hand]=pressPending[hand] || down[hand];
            }
            held[hand]=down[hand];
        }
    }
    bool WantsShot() const { return active && (pending[0] || pending[1]); }
    bool Consume(int hand) {
        if (!active || hand<0 || hand>1 || !pending[hand]) return false;
        Clear(hand);
        return true;
    }
};

// LT draws only from unarmed; Y holsters drawing/ready guns. Remember a brief
// press until native animation acknowledges it. Native hold style needs a
// sustained synthetic LT; toggle style needs input only in the settled state.
struct EquipInput {
    bool desiredArmed=false, pending=false, leftHeld=false, yHeld=false;
    bool consumeY=false, blockLeft=false;
    void Reset() { *this={}; }
    bool Update(bool holdMode, int gunStatus, bool left, bool y=false) {
        if (!left) blockLeft=false;
        if (!y) consumeY=false;
        if (pending && (desiredArmed ? gunStatus==2 || gunStatus==4
                                    : gunStatus==3 || gunStatus==0)) pending=false;
        if (!pending) desiredArmed=gunStatus==2 || gunStatus==4;
        if (y && !yHeld && (gunStatus==2 || gunStatus==3 || gunStatus==4)) {
            desiredArmed=false; pending=true; consumeY=true;
        }
        if (consumeY && left) blockLeft=true;
        if (left && !leftHeld && !y && gunStatus==0) {
            desiredArmed=true; pending=true; blockLeft=true;
        }
        leftHeld=left; yHeld=y;
        return holdMode ? desiredArmed : pending &&
            (desiredArmed ? gunStatus==0 : gunStatus==4);
    }
};

// Native masked gun passes include forearm and hand. Keep only the hand
// (the equipped gun is part of that mesh). The hand shader restores the full
// corrected palette after native masking and clips by wrist weights instead.
inline uint32_t HandOnlyMask(uint32_t nativeMask) {
    return nativeMask==0x600 ? 0x400 : nativeMask==0x3000 ? 0x2000 :
        nativeMask==0x3600 ? 0x2400 : 0;
}
} // namespace tr::motiongun
