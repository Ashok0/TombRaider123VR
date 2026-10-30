#pragma once
#include <cstdint>

namespace tr::firstperson {
// Presentation only. Observe native simulation on both sides of a tick;
// never alter jump dispatch, gravity, collision, damage or animation time.
struct JumpLanding {
    const void* owner=nullptr;
    int level=-1;
    bool launched=false,airborne=false,fall=false,ordinary=false;
    int32_t takeoffY=0;
    void Reset() { *this={}; }
    void Observe(const void* item,int currentLevel,bool eligible,int state,int animation,
                 int32_t y,bool gravity,bool ground,bool landing,bool jump) {
        if(!eligible || !item) { Reset();return; }
        if(owner!=item || level!=currentLevel) { Reset();owner=item;level=currentLevel; }
        // Compression belongs to a ground jump even after the input is released.
        if(state==15 && !gravity) {
            launched=true;airborne=fall=ordinary=false;takeoffY=y;return;
        }
        if(gravity || state==3 || state==9 || (state>=25 && state<=29)) {
            airborne=true;ordinary=false;
            if(state==9) fall=true;
            return;
        }
        if(landing) {
            if(airborne) {
                // Y grows downward. A lower landing beyond half a click is
                // a drop, even if it began as an intentional jump.
                ordinary=launched && !fall && int64_t(y)-takeoffY<=128;
                launched=airborne=false;
            }
            if(state==2 && animation==24) ordinary=false;
            return;
        }
        ordinary=airborne=fall=false;
        launched=ground && jump;
        if(launched) takeoffY=y;
    }
};
} // namespace tr::firstperson
