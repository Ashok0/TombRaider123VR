#pragma once
#include <cstdint>

namespace tr::firstperson {
// GetCollisionInfo reports six floor/ceiling samples relative to Lara's
// collision capsule. An eye may look over a drop even while Lara's feet are
// grounded, but cannot enter a raised floor or ceiling at its own height.
inline bool EyeBlocked(const int32_t samples[18], int32_t bodyY,
                               double eyeY, bool hitStatic) {
    if (hitStatic) return true;
    for (int i = 0; i < 18; i += 3) {
        const int32_t floor = samples[i], ceiling = samples[i + 1];
        if (floor == -32512 || ceiling == -32512 ||
            double(bodyY) + floor <= eyeY + 64 ||
            double(bodyY) - 762 + ceiling >= eyeY - 64)
            return true;
    }
    return false;
}
} // namespace tr::firstperson
