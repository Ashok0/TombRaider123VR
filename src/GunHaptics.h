#pragma once
#include <cstdint>

namespace tr {
// Legacy OpenVR pulses must be at least 5 ms apart. Sustain a strong shot
// burst over rendered frames without sleeping or queuing a long rumble tail.
struct GunHaptics {
    uint64_t until[2]{}, next[2]{};
    void Reset() { *this={}; }
    void Shot(int hand,uint64_t now) {
        if (hand<0 || hand>1) return;
        until[hand]=now+80;
    }
    template<class Pulse> void Update(uint64_t now,Pulse pulse) {
        for (int hand=0;hand<2;++hand) {
            if (now>=until[hand] || now<next[hand]) continue;
            pulse(hand,3999);
            next[hand]=now+5;
        }
    }
};
}
