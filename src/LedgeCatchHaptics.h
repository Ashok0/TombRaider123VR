#pragma once
#include "LedgeGrabFeedback.h"
#include <cstdint>

namespace tr {
// Collection adapter preserves owner/level lifecycle and the existing native
// before/after hook. Collision alignment is never counted as airborne travel.
struct LedgeCatchHaptics {
    const void* owner=nullptr;
    int level=-1;
    firstperson::LedgeGrabFeedback flight;
    void Reset() { *this={}; }
    void Before(bool enabled,const void* item,int currentLevel,int state,bool gravity,
                int32_t x,int32_t y,int32_t z) {
        if(!enabled) { Reset();return; }
        if(owner!=item || level!=currentLevel) { Reset();owner=item;level=currentLevel; }
        flight.Sample(true,state,gravity,{float(x),float(y),float(z)});
    }
    bool After(bool enabled,int state,bool gravity,int32_t,int32_t,int32_t) {
        return flight.Finish(enabled,state,gravity);
    }
};
}
