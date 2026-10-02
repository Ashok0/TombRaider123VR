#pragma once
#include <algorithm>
#include <cmath>
#include "LocomotionMath.h"

namespace tr::stabilization {
// Input polls latch turn velocity. Rendered scene views integrate it, so a
// faster controller poll cannot add extra yaw or jitter to the world.
struct RenderTurn {
    double sampleTime = 0, frameTime = 0;
    float rate = 0;
    bool valid = false;
    void Reset() { *this = {}; }
    void Sample(float next, double now) {
        if (!std::isfinite(next) || !std::isfinite(now)) { Reset(); return; }
        if (!valid || rate == 0 || rate * next < 0 ||
            now < sampleTime || now - sampleTime > 0.1)
            frameTime = now;
        rate = next;
        sampleTime = now;
        valid = true;
    }
    float Step(double now) {
        if (!valid) return 0;
        if (!std::isfinite(now) || now < frameTime) { Reset(); return 0; }
        const double dt = std::clamp(now - frameTime, 0.0, 0.05);
        frameTime = now;
        if (now - sampleTime > 0.1) return 0;
        return rate * static_cast<float>(dt);
    }
};

// Filter animation root motion once per simulation tick. The requested
// direction stays exact; gait changes and reversals start at native speed.
struct RootMotion {
    bool valid = false;
    int gait = -1;
    uint32_t action = 0;
    float speed = 0;
    locomotion::Vec direction{}, remainder{};
    void Reset() { *this = {}; }
    bool Step(locomotion::Vec native, locomotion::Vec requested, int state,
              uint32_t intent, locomotion::Vec& result) {
        using namespace locomotion;
        const float sample = Length(native), n = Length(requested);
        if (!std::isfinite(sample) || !std::isfinite(n) ||
            n < 0.001f || sample > 256) { Reset(); return false; }
        if (sample < 0.001f) { Reset(); result = {}; return true; }
        const Vec next = requested * (1 / n);
        if (!valid || gait != state || action != intent ||
            next.x * direction.x + next.z * direction.z < 0.5f) {
            speed = sample;
            remainder = {};
        } else speed += 0.25f * (sample - speed);
        valid = true;
        gait = state;
        action = intent;
        direction = next;
        const Vec precise = next * speed + remainder;
        result = {std::round(precise.x), std::round(precise.z)};
        remainder = precise - result;
        return true;
    }
};

struct Point { float x = 0, y = 0, z = 0; };
// Keep a standing eye reference across grounded animation frames. Physical
// body turns do not rotate this offset a second time; artificial turns do.
struct GroundEye {
    bool valid = false;
    Point local{};
    void Reset() { *this = {}; }
    void Resume(bool sameBody,float oldYaw,float newYaw,float scriptedBodyTurn) {
        if (!sameBody) { Reset(); return; }
        if (!valid) return;
        // A new tracking neutral changes the coordinate basis, not Lara's
        // calibrated eye position. Only a scripted turn of her body should
        // rotate that offset in world space during a camera handoff.
        const auto flat=locomotion::Rotate({local.x,local.z},
            oldYaw+scriptedBodyTurn-newYaw);
        local.x=flat.x; local.z=flat.z;
    }
    Point Apply(Point root, float artificialYaw, Point animated) {
        using namespace locomotion;
        const float height=animated.y-root.y;
        // The first grounded frame after a high vault can still contain the
        // pull-up skeleton while Lara's root is already on top of the crate.
        // Never latch that near-feet/high-above-root pose as standing height.
        const bool standing=std::isfinite(height) && height<=-500 && height>=-950;
        if (standing && (!valid || local.y>-500 || local.y<-950)) {
            const auto flat = Rotate({animated.x - root.x,
                                      animated.z - root.z}, -artificialYaw);
            local = {flat.x, height, flat.z};
            valid = true;
        }
        // Until a standing pose exists, use the actual animated joint. A
        // guessed height can put the camera above Lara after a crate mount.
        if (!valid) return animated;
        const auto flat = Rotate({local.x, local.z}, artificialYaw);
        return {root.x + flat.x, root.y + local.y, root.z + flat.z};
    }
};
} // namespace tr::stabilization
