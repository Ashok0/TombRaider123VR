#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tr::inventory {

// XInput values. The native inventory still owns navigation, use and submenus.
constexpr uint16_t Up=1, Down=2, Left=4, Right=8, Back=0x20;
constexpr uint16_t L3=0x40, R3=0x80;
struct Input {
    uint16_t buttons=0;
    float x=0, y=0;
    uint8_t lt=0, rt=0;
    bool shifted=false;
};
inline uint16_t Direction(float x,float y,float deadzone) {
    if(std::fabs(x)>std::fabs(y)) return x>deadzone ? Right : x<-deadzone ? Left : 0;
    return y>deadzone ? Up : y<-deadzone ? Down : 0;
}

struct Session {
    bool pending=false, open=false, release=false, pressHeld=false;
    uint64_t started=0;
    int game=-1;

    void Reset() { *this={}; }
    void Sync(bool allowed,bool nativeOpen,int currentGame,uint64_t now) {
        if(!allowed || (game!=-1 && game!=currentGame)) { Reset(); return; }
        if(pending && now-started>=1000) pending=false;
        if(pending && nativeOpen) { pending=false; open=true; release=true; }
        if(open && !nativeOpen) {
            open=pending=false;
            release=true; // A/B/axes must not become a jump/roll/step on exit.
        }
    }

    // Returns true only on a new request. `owned` asks the caller to bypass
    // gameplay chords and axes for this poll, including entry/exit drains.
    bool Update(Input& in,bool allowed,bool nativeOpen,int currentGame,
                uint64_t now,float deadzone,bool& owned) {
        Sync(allowed,nativeOpen,currentGame,now);
        owned=false;
        if(!allowed) return false;
        deadzone=std::clamp(deadzone,.2f,.9f);
        const uint16_t direction=Direction(in.x,in.y,deadzone);
        const bool photo=(in.buttons&(L3|R3))==(L3|R3);
        // Touch X is mapped to XInput BACK (XInput X is Walk). Observe this
        // unarmed menu input without synthesizing it. Gamepad's armed-X
        // router removes BACK before this observer. TR1-3 have no selectable
        // alternate ammunition; BACK is not an ammo-change command.
        const bool pressed=(in.buttons&Back)!=0;
        const bool neutral=!(in.buttons & uint16_t(0xf0ff)) &&
            std::fabs(in.x)<deadzone*.5f && std::fabs(in.y)<deadzone*.5f &&
            in.lt<=30 && in.rt<=30;
        const bool begin=pressed && !pressHeld && !photo && !open && !release && !nativeOpen;
        pressHeld=pressed;
        if(begin) { pending=true; started=now; game=currentGame; }
        // Only take control once the native game actually enters inventory.
        // Ammo changes, shooting and movement remain untouched while pending.
        owned=open || release;
        if(!owned) return begin;
        if(release && neutral) release=false;
        if(release) {
            in.buttons=0;
            in.x=in.y=0;
        } else if(open) {
            // LS works without holding R3. Dominant-axis D-pad navigation
            // retains native repeat timing and all vertical submenu choices.
            in.buttons=uint16_t((in.buttons&~(L3|R3|0x10)) | direction);
            in.x=in.y=0;
        } else {
            // First neutral poll after exit is still owned.
            in.buttons=0; in.x=in.y=0;
        }
        in.lt=in.rt=0;
        return begin;
    }
};
} // namespace tr::inventory
