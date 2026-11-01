#pragma once
#include "SwitchPosePath.h"
#include "MotionGunMath.h"
#include <algorithm>
#include <cmath>

namespace tr::firstperson {
struct ValveTurn {
    float angles[256]{},progress=0,travel=0,previous=0,direction=1;
    int count=0;
    bool Build(const float* samples,int size) {
        *this={};
        if(!samples || size<3 || size>256 || !std::isfinite(samples[0])) return false;
        float accumulated=0;
        for(int i=1;i<size;++i) {
            const float delta=std::remainder(samples[i]-samples[i-1],2*3.14159265358979323846f);
            if(!std::isfinite(delta) || std::fabs(delta)>.75f) return false;
            accumulated+=delta;angles[i]=accumulated;
        }
        if(std::fabs(accumulated)<1.f || std::fabs(accumulated)>12.6f) return false;
        direction=accumulated<0 ? -1.f : 1.f;
        for(int i=1;i<size;++i) {
            const float a=angles[i]*direction;
            // Remove small authored hand-settling reversals, not real turns.
            if(a<angles[i-1]-.05f) return false;
            angles[i]=std::max(angles[i-1],a);
        }
        count=size;return true;
    }
    void Grab(float angle) { previous=angle;travel=progress; }
    bool Track(float angle) {
        const float delta=std::remainder(angle-previous,2*3.14159265358979323846f);
        if(!count || !std::isfinite(delta) || std::fabs(delta)>.75f) return false;
        previous=angle;travel+=direction*delta;
        // Keep excess motion at the start stop as debt, so moving the wrong
        // way and returning cannot manufacture a turn or shift the grip.
        progress=std::clamp(travel,0.f,angles[count-1]);return true;
    }
    int Pose() const {
        if(!count || progress<.005f) return 0;
        int best=0;float distance=1.e30f;
        for(int i=0;i<count;++i) {
            const float d=std::fabs(angles[i]-progress);
            if(d<distance-1.e-5f) { distance=d;best=i; }
        }
        return best;
    }
    bool Complete() const { return count && progress>=angles[count-1]-.015f; }
};
}
