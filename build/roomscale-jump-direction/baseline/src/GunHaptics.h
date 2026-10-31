#pragma once
#include <cstdint>
#include <algorithm>

namespace tr {
// Legacy OpenVR pulses must be at least 5 ms apart. Sustain a strong impact/shot
// burst over rendered frames without sleeping or queuing a long rumble tail.
struct GunHaptics {
    uint64_t until[2]{}, next[2]{};
    uint64_t hoverUntil[2]{}, hoverNext[2]{};
    void Reset() { *this={}; }
    void Burst(int hand,uint64_t now,uint64_t duration) {
        if (hand<0 || hand>1) return;
        until[hand]=std::max(until[hand],now+duration);
    }
    void Shot(int hand,uint64_t now) { Burst(hand,now,80); }
    void LedgeCatch(uint64_t now) { Shot(0,now);Shot(1,now); }
    void Hover(int hand,bool active,uint64_t now) {
        if(hand<0 || hand>1) return;
        hoverUntil[hand]=active ? now+100 : 0;
    }
    template<class Pulse> void Update(uint64_t now,Pulse pulse) {
        for (int hand=0;hand<2;++hand) {
            if (now<next[hand]) continue;
            if(now<until[hand]) pulse(hand,3999);
            else if(now<hoverUntil[hand] && now>=hoverNext[hand]) {
                pulse(hand,700);hoverNext[hand]=now+60;
            } else continue;
            next[hand]=now+5;
        }
    }
};
}
