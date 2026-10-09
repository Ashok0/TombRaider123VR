#pragma once
#include <algorithm>
#include <cmath>

namespace tr::firstperson {
// Heights are metres below the live HMD, independent of camera calibration.
struct LedgePullGesture {
    bool valid=false, pulling=false, spent=false;
    float start[2]{}, previous[2]{}, fired[2]{};
    double last=0, began=0, until=0;
    void Reset() { *this={}; }
    static bool Hanging(int state) { return state==10 || state==30 || state==31; }
    bool Update(bool eligible, bool tracked, float left, float right, double now) {
        if (!eligible || !tracked || !std::isfinite(left) || !std::isfinite(right) ||
            !std::isfinite(now)) { Reset(); return false; }
        const float hands[2]={left,right};
        const double dt=now-last;
        if (!valid || dt<0 || dt>.25 ||
            std::fabs(left-previous[0])>.03+4*dt ||
            std::fabs(right-previous[1])>.03+4*dt) {
            Reset(); valid=true;
            for (int h=0;h<2;++h) start[h]=previous[h]=hands[h];
            last=now;
            return false;
        }
        last=now;
        for (int h=0;h<2;++h) previous[h]=hands[h];
        if (spent) {
            if (now<until) return true;
            // A blocked climb needs a fresh raise-and-pull, not held low hands.
            if (left<=fired[0]-.10f && right<=fired[1]-.10f) {
                spent=false; pulling=false;
                for (int h=0;h<2;++h) start[h]=hands[h];
            }
            return false;
        }
        if (!pulling) {
            for (int h=0;h<2;++h) start[h]=std::min(start[h],hands[h]);
            if (left-start[0]>=.04f || right-start[1]>=.04f) {
                pulling=true; began=now;
            }
        }
        if (pulling && now-began>.75) {
            pulling=false;
            for (int h=0;h<2;++h) start[h]=hands[h];
        }
        if (pulling && left-start[0]>=.18f && right-start[1]>=.18f) {
            spent=true; pulling=false; until=now+.5;
            for (int h=0;h<2;++h) fired[h]=hands[h];
            return true;
        }
        return false;
    }
};
}
