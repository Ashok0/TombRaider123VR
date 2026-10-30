#pragma once
#include <algorithm>
#include <cmath>

namespace tr::firstperson {
// Observe native simulation travel, never controller motion or camera offsets.
struct LedgeGrabFeedback {
    struct Position { float x=0,y=0,z=0; };
    Position start{},last{};
    float low=0,high=0;
    bool tracking=false,large=false;
    void Reset() { *this={}; }
    static bool JumpState(int state) {
        return state==3 || state==9 || state==11 || state==12 ||
               (state>=25 && state<=29);
    }
    void Sample(bool enabled,int state,bool gravity,Position position) {
        if (!enabled || !gravity || !JumpState(state) ||
            !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
            Reset(); return;
        }
        const float dx=position.x-last.x,dy=position.y-last.y,dz=position.z-last.z;
        // A relocation must not masquerade as a large jump.
        if (tracking && dx*dx+dy*dy+dz*dz>2048.f*2048.f) Reset();
        if (!tracking) { start=position;low=high=position.y;tracking=true; }
        last=position;low=std::min(low,position.y);high=std::max(high,position.y);
        const float x=position.x-start.x,z=position.z-start.z;
        // Half a native tile of horizontal travel or vertical excursion.
        // Sample BEFORE collision, so the ledge alignment snap cannot qualify.
        large=large || x*x+z*z>=512.f*512.f || high-low>=512.f;
    }
    bool Finish(bool enabled,int state,bool gravity) {
        // LaraTestHangJump can leave gravity set until the next hang tick.
        // State 10 is a ledge catch; monkey-bar hang (75) is a different state.
        const bool caught=enabled && tracking && large && state==10;
        if (!enabled || state==10 || !gravity || !JumpState(state)) Reset();
        return caught;
    }
};
} // namespace tr::firstperson
