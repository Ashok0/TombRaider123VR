#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tr::firstperson {

// GetCollisionInfo reports six floor/ceiling samples relative to Lara's
// collision capsule. An eye may look over a drop even while Lara's feet are
// grounded, but cannot enter a raised floor or ceiling at its own height.
inline bool EyeBlocked(const int32_t samples[18], int32_t bodyY,
                               double eyeY, bool hitStatic, int capsuleHeight=762) {
    if (hitStatic) return true;
    for (int i = 0; i < 18; i += 3) {
        const int32_t floor = samples[i], ceiling = samples[i + 1];
        if (floor == -32512 || ceiling == -32512 ||
            double(bodyY) + floor <= eyeY + 64 ||
            double(bodyY) - capsuleHeight + ceiling >= eyeY - 64)
            return true;
    }
    return false;
}

// Sweep the rendered eye from Lara's collision origin in short horizontal
// steps. The caller supplies the engine's collision query for each candidate.
// Keep the last clear point; do not move the eye through a blocked wall.
template <typename Blocked>
void ClampEyeToWall(const int32_t body[3], int32_t eye[3], Blocked blocked) {
    int64_t dx = int64_t(eye[0]) - body[0];
    int64_t dz = int64_t(eye[2]) - body[2];
    double distance = std::hypot(double(dx), double(dz));
    if (distance < 1.0) return;
    if (distance > 512.0) {
        // Saturate along the same ray. Resetting to the root at this boundary
        // teleports the view when a small tracked movement crosses it.
        dx=int64_t(std::lround(double(dx)*512.0/distance));
        dz=int64_t(std::lround(double(dz)*512.0/distance));
        eye[0]=body[0]+int32_t(dx);eye[2]=body[2]+int32_t(dz);
        distance=std::hypot(double(dx),double(dz));
    }

    const int steps = std::clamp(int(std::ceil(distance / 16.0)), 1, 32);
    int32_t clearX = body[0], clearZ = body[2];
    for (int i = 1; i <= steps; ++i) {
        const int32_t x = body[0] + int32_t(dx * i / steps);
        const int32_t z = body[2] + int32_t(dz * i / steps);
        if (blocked(x, z, clearX, clearZ)) {
            // Refine this final segment to integer precision. Returning the
            // previous 16-unit sample makes a tiny HMD change move the camera
            // by a whole sample when the sweep's step count changes.
            int32_t hitX=x,hitZ=z;
            for(int refine=0;refine<5;++refine) {
                const int32_t midX=clearX+(hitX-clearX)/2,midZ=clearZ+(hitZ-clearZ)/2;
                if(midX==clearX && midZ==clearZ) break;
                if(blocked(midX,midZ,clearX,clearZ)) { hitX=midX;hitZ=midZ; }
                else { clearX=midX;clearZ=midZ; }
            }
            eye[0] = clearX;
            eye[2] = clearZ;
            return;
        }
        clearX = x;
        clearZ = z;
    }
}

} // namespace tr::firstperson
