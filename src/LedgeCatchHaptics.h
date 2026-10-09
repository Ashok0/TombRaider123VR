#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tr {
// Native simulation samples only: HMD motion and repeated eye draws cannot
// turn a small grab into an impact. A classic tile is 1024 game units.
struct LedgeCatchHaptics {
    const void* owner=nullptr;
    int level=-1, ticks=0;
    bool airborne=false;
    double startX=0,startY=0,startZ=0,lastX=0,lastY=0,lastZ=0;
    double horizontal2=0,vertical=0;
    void Reset() { *this={}; }
    void ClearFlight() { airborne=false;ticks=0;horizontal2=vertical=0; }
    static bool AirState(int state,bool gravity) {
        return gravity && (state==3 || state==9 || (state>=25 && state<=29));
    }
    void Track(int32_t x,int32_t y,int32_t z) {
        const double dx=double(x)-lastX,dy=double(y)-lastY,dz=double(z)-lastZ;
        if (!airborne || dx*dx+dy*dy+dz*dz>1024.*1024.) {
            ClearFlight();airborne=true;startX=x;startY=y;startZ=z;
        }
        lastX=x;lastY=y;lastZ=z;
        const double sx=double(x)-startX,sz=double(z)-startZ;
        horizontal2=std::max(horizontal2,sx*sx+sz*sz);
        vertical=std::max(vertical,std::fabs(double(y)-startY));
    }
    void Before(bool enabled,const void* item,int currentLevel,int state,bool gravity,
                int32_t x,int32_t y,int32_t z) {
        if (!enabled) { Reset();return; }
        if (owner!=item || level!=currentLevel) {
            Reset();owner=item;level=currentLevel;
        }
        if (!AirState(state,gravity)) { ClearFlight();return; }
        Track(x,y,z);++ticks;
    }
    bool After(bool enabled,int state,bool gravity,int32_t x,int32_t y,int32_t z) {
        if (!enabled) { Reset();return false; }
        if (state==10) {
            // Use flight travel BEFORE collision snaps Lara onto the ledge.
            const bool impact=airborne && ticks>=10 &&
                (horizontal2>=768.*768. || vertical>=512.);
            ClearFlight();return impact;
        }
        if (AirState(state,gravity)) Track(x,y,z);
        else ClearFlight();
        return false;
    }
};
}
