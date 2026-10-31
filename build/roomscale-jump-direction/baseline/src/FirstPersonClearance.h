#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace tr::firstperson {
// World Y points down. Clamp the final tracked eye, then remove the tracking
// contribution again because stereo adds it after the scene camera hook.
inline double RollEyeAboveFloor(double anchorY,double trackedRise,int32_t bodyY,
                                int32_t relativeFloor,double clearance=64) {
    if (!std::isfinite(anchorY) || !std::isfinite(trackedRise)) return anchorY;
    const double floor=double(bodyY)+(relativeFloor==-32512 ? 0 : relativeFloor);
    return std::min(anchorY-trackedRise,floor-clearance)+trackedRise;
}
// GetCollisionInfo fills four floor/ceiling samples in TR1/2 and six in TR3.
// The shared struct reserves all six in every game; unused zeroed entries are
// not geometry (zero ceiling would invent a plane at bodyY - 762).
// An eye may look over a drop even while Lara's feet are
// grounded, but cannot enter a raised floor or ceiling at its own height.
inline bool EyeBlocked(const int32_t samples[18], int sampleCount, int32_t bodyY,
                               double eyeY, bool hitStatic, int capsuleHeight=762) {
    if (hitStatic) return true;
    for (int i = 0; i < sampleCount * 3; i += 3) {
        const int32_t floor = samples[i], ceiling = samples[i + 1];
        if (floor == -32512 || ceiling == -32512 ||
            double(bodyY) + floor <= eyeY + 64 ||
            double(bodyY) - capsuleHeight + ceiling >= eyeY - 64)
            return true;
    }
    return false;
}
} // namespace tr::firstperson
