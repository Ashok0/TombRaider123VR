#pragma once
#include "LocomotionMath.h"
#include <cmath>
#include <cstdint>
#include <limits>

namespace tr::firstperson {
// Feed translated copies to the native render-origin calculation. The same
// offset on both endpoints survives interpolation without moving collision,
// camera, animation, shadow or gameplay joint-query positions.
template<class Pose>
bool ClassicBodyPositions(const Pose& previous,const Pose& current,
                          locomotion::Vec offset,Pose& renderPrevious,Pose& renderCurrent) {
    if(!std::isfinite(offset.x) || !std::isfinite(offset.z) ||
       std::fabs(offset.x)>4096 || std::fabs(offset.z)>4096) return false;
    const int64_t x=std::lround(offset.x),z=std::lround(offset.z);
    const auto fits=[](int32_t value,int64_t delta) {
        return value+delta>=std::numeric_limits<int32_t>::min() &&
               value+delta<=std::numeric_limits<int32_t>::max();
    };
    if(!fits(previous.x_pos,x) || !fits(current.x_pos,x) ||
       !fits(previous.z_pos,z) || !fits(current.z_pos,z)) return false;
    renderPrevious=previous;renderCurrent=current;
    renderPrevious.x_pos+=int32_t(x);renderCurrent.x_pos+=int32_t(x);
    renderPrevious.z_pos+=int32_t(z);renderCurrent.z_pos+=int32_t(z);
    return true;
}
} // namespace tr::firstperson
