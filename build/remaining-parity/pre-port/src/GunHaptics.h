#pragma once
#include <cstdint>
#include <algorithm>

namespace tr {
// Legacy OpenVR pulses must be at least 5 ms apart. Sustain a strong impact/shot
// burst over rendered frames without sleeping or queuing a long rumble tail.
struct GunHaptics {
    uint64_t until[2]{}, next[2]{};
    void Reset() { *this={}; }
    void Burst(int hand,uint64_t now,uint64_t duration) {
        if (hand<0 || hand>1) return;
        until[hand]=std::max(until[hand],now+duration);
    }
    void Shot(int hand,uint64_t now) { Burst(hand,now,80); }
    void LedgeCatch(uint64_t now) { Burst(0,now,120);Burst(1,now,120); }
    template<class Pulse> void Update(uint64_t now,Pulse pulse) {
        for (int hand=0;hand<2;++hand) {
            if (now>=until[hand] || now<next[hand]) continue;
            pulse(hand,3999);
            next[hand]=now+5;
        }
    }
};
}
