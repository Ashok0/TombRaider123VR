#pragma once

#include <algorithm>
#include <cmath>

namespace tr::headheight {

// Tracking poses use an absolute runtime origin. The rendered eye instead
// measures vertical displacement from the headset position captured at recenter.
// Keep the ceiling cap in that same coordinate system.
inline float Clamp(float rawY, float neutralY, float headroomUnits,
                   float marginUnits, float unitsPerMetre) {
    if (!std::isfinite(rawY) || !std::isfinite(neutralY) ||
        !std::isfinite(headroomUnits) || !std::isfinite(marginUnits) ||
        !std::isfinite(unitsPerMetre) || unitsPerMetre <= 0.0f)
        return rawY;
    const float maxRise = std::max(0.0f,
        (headroomUnits - marginUnits) / unitsPerMetre);
    return std::min(rawY, neutralY + maxRise);
}

} // namespace tr::headheight
