#pragma once
#include "MotionGunInput.h"

namespace tr::motiongun {
// Native equipment remains shared; this mask owns physical hand occupancy.
// Distances are supplied in metres from the rendered holster, not the HMD.
struct HolsterInput {
    static constexpr float ReachMetres=.30f;
    static constexpr float WithdrawMetres=.35f;
    struct Hand {
        bool down=false, claimed=false, drawing=false, blockTrigger=false;
        int slot=-1, weapon=0;
        float startDistance=0;
    } hand[2];
    bool active=false, request=false;
    int weapon=0;
    unsigned mask=0;
    void Reset() { *this={}; }
    bool Has(int h) const { return !active || (mask&(1u<<h)); }
    bool WeaponEligible(int h,int available,int status,int nativeWeapon) const {
        const bool armed=active ? (mask&(1u<<h))!=0 :
            status==4 && (h==1 || DualWeapon(nativeWeapon));
        return (status==0 || status==4) && SupportedWeapon(available) &&
            (h==1 || DualWeapon(available)) &&
            (!armed || available==nativeWeapon) &&
            (status!=4 || available==nativeWeapon) &&
            (!active || !mask || available==weapon);
    }
    bool InReach(int slot,float distance) const {
        return slot>=0 && distance>=0 && distance<=ReachMetres;
    }
    bool Hover(int h,int slot,int available,float distance,int status,int nativeWeapon) const {
        return InReach(slot,distance) && WeaponEligible(h,available,status,nativeWeapon);
    }
    void Cancel(int h,bool down) {
        const bool block=hand[h].blockTrigger;
        hand[h]={};hand[h].down=down;hand[h].blockTrigger=block;
    }
    // slot/distance refer to the latched slot while claimed, otherwise nearest.
    bool Poll(int h,bool down,bool allowed,int slot,int available,float distance,
              int status,int nativeWeapon) {
        auto& p=hand[h];
        const bool rising=down&&!p.down;
        if(!allowed) { const bool consumed=p.claimed;Cancel(h,down);return consumed; }
        const bool armed=active ? (mask&(1u<<h))!=0 :
            status==4 && (h==1 || DualWeapon(nativeWeapon));
        const bool inReach=InReach(slot,distance);
        if(!p.claimed && down && inReach && (armed || rising) &&
           WeaponEligible(h,available,status,nativeWeapon)) {
            p.claimed=true;p.drawing=!armed;p.slot=slot;p.weapon=available;
            p.startDistance=distance;p.blockTrigger=true;
        }
        const bool consumed=p.claimed;
        if(p.claimed) {
            // Withdrawal must leave the grab volume, with at least 8 cm travel.
            const bool draw=p.drawing && down && distance>=WithdrawMetres &&
                distance-p.startDistance>=.08f;
            const bool stow=!p.drawing && !down && inReach;
            if(draw || stow) {
                if(!active) {
                    active=true;weapon=p.weapon;
                    mask=status==4 ? (DualWeapon(weapon)?3u:2u) : 0u;
                }
                const unsigned before=mask;
                if(draw) mask|=1u<<h; else mask&=~(1u<<h);
                if((before==0)!=(mask==0)) request=true;
                p.drawing=false;
            }
            if(!down) { p.claimed=p.drawing=false;p.slot=-1; }
        }
        p.down=down;
        return consumed;
    }
    bool CanFire(int h,bool trigger) {
        auto& p=hand[h];
        if(!trigger && !p.claimed) p.blockTrigger=false;
        return Has(h) && !p.claimed && !p.blockTrigger;
    }
};
} // namespace tr::motiongun
