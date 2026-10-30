#pragma once
#include "LocomotionMath.h"

namespace tr::stabilization {
// Input polls latch a velocity; only rendered views integrate it. No yaw EMA,
// catch-up motion or release tail. Times are monotonic seconds supplied by the
// caller so mismatched input/render cadence can be tested deterministically.
struct RenderTurn {
    double sampleTime=0, frameTime=0;
    float rate=0;
    bool valid=false;
    void Reset() { *this={}; }
    void Sample(float next,double now) {
        if (!std::isfinite(next) || !std::isfinite(now)) { Reset(); return; }
        if (!valid || rate==0 || rate*next<0 || now<sampleTime || now-sampleTime>0.1)
            frameTime=now; // Never charge time before a press/reversal/resume.
        rate=next; sampleTime=now; valid=true;
    }
    float Step(double now) {
        if (!valid) return 0;
        if (!std::isfinite(now) || now<frameTime) { Reset(); return 0; }
        const double dt=std::clamp(now-frameTime,0.0,0.05);
        frameTime=now;
        if (now-sampleTime>0.1) return 0; // Lost polling must not leave a latched turn.
        return rate*float(dt);
    }
};

// Simulation-step filter, never a render/headset pose filter. New gaits and
// direction reversals start at native speed; zero motion stops immediately.
struct RootMotion {
    bool valid=false;
    int gait=-1;
    uint64_t action=0;
    float speed=0;
    locomotion::Vec direction{}, remainder{};
    void Reset() { *this={}; }
    bool Step(locomotion::Vec native, locomotion::Vec requested, int state,
              uint64_t intent, locomotion::Vec& result) {
        using namespace locomotion;
        const float sample=Length(native), n=Length(requested);
        if (!std::isfinite(sample) || !std::isfinite(n) || n<0.001f || sample>256) {
            Reset(); return false; // Animation commands/teleports remain native.
        }
        if (sample<0.001f) { Reset(); result={}; return true; }
        const Vec next=requested*(1/n);
        if (!valid || gait!=state || action!=intent ||
            next.x*direction.x+next.z*direction.z<0.5f) {
            speed=sample; remainder={};
        } else speed+=0.25f*(sample-speed);
        valid=true; gait=state; action=intent; direction=next;
        const Vec precise=next*speed+remainder;
        result={std::round(precise.x),std::round(precise.z)};
        remainder=precise-result;
        return true;
    }
};

struct Point { float x=0,y=0,z=0; };
// A grounded state can precede the last interpolated climbing skeleton.
// Correct that temporary body translation without changing the camera's saved
// standing fit. Once the skeleton is standing-height, release the correction
// over 120 ms; ordinary running sway must not keep this mode alive indefinitely.
struct MountBodyTransition {
    bool active=false,settling=false;
    double start=0;
    void Reset() { *this={}; }
    void Begin() { active=true;settling=false; }
    locomotion::Vec Correction(locomotion::Vec residual,float height,double now) {
        if (!active) return {};
        if (!std::isfinite(height) || !std::isfinite(now) ||
            !std::isfinite(residual.x) || !std::isfinite(residual.z)) { Reset();return {}; }
        if (height < -950 || height > -500) { settling=false;return residual; }
        if (!settling || now<start) { start=now;settling=true; }
        const float weight=1.f-float(std::clamp((now-start)/.12,0.0,1.0));
        if (weight==0) Reset();
        return residual*weight;
    }
};
// Hold the eye offset in the artificial-yaw frame across grounded gaits.
// Physical body yaw must not orbit this origin a second time. Lara's
// interpolated world root and physical headset translation are NOT filtered.
// Keep this reference while native non-ground animation drives the eye; only
// a new camera session/item/relocation should discard the standing reference.
inline void OffsetBodyPalette(float* palette,int count,locomotion::Vec offset) {
    for (int joint=0;joint<count;++joint) {
        float* bone=palette+joint*12;
        // Padding/hidden zero matrices must remain zero.
        if (bone[0]==0 && bone[1]==0 && bone[2]==0 &&
            bone[4]==0 && bone[5]==0 && bone[6]==0 &&
            bone[8]==0 && bone[9]==0 && bone[10]==0) continue;
        bone[3]+=offset.x; bone[11]+=offset.z;
    }
}
// Smooth only the stance-height change, never physical tracking or collision
// movement. Crouched animation bob is replaced by a fixed low eye height.
struct StanceEye {
    bool valid=false,transition=false;
    float height=0;
    double time=0;
    void Reset() { *this={}; }
    float Apply(float target,bool crouched,double now) {
        if(!valid || !std::isfinite(now) || now<time) {
            valid=true;height=target;time=now;transition=crouched;return height;
        }
        if(crouched || transition) {
            const float step=float(std::clamp(now-time,0.0,.05))*3000.f;
            height+=std::clamp(target-height,-step,step);
            transition=crouched || height!=target;
        } else height=target;
        time=now;return height;
    }
};

struct GroundEye {
    bool valid = false;
    Point local{};
    locomotion::Vec bodyLocal{};
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
    Point Apply(Point root, float artificialYaw, Point animated,float bodyYaw,
                bool followAnimatedHeight=false) {
        using namespace locomotion;
        const float height=animated.y-root.y;
        // The first grounded frame after a high vault can still contain the
        // pull-up skeleton while Lara's root is already on top of the crate.
        // Never latch that near-feet/high-above-root pose as standing height.
        const bool standing=!followAnimatedHeight && std::isfinite(height) && height<=-500 && height>=-950;
        if (standing && (!valid || local.y>-500 || local.y<-950)) {
            const auto flat = Rotate({animated.x - root.x,
                                      animated.z - root.z}, -artificialYaw);
            local = {flat.x, height, flat.z};
            bodyLocal=Rotate({animated.x-root.x,animated.z-root.z},-bodyYaw);
            valid = true;
        }
        // Until a standing pose exists, use the actual animated joint. A
        // guessed height can put the camera above Lara after a crate mount.
        if (!valid) return animated;
        const auto flat = Rotate({local.x, local.z}, artificialYaw);
        // Landing kneels follow the neck vertically without changing the
        // standing reference or reintroducing sideways animation sway.
        return {root.x + flat.x, followAnimatedHeight ? animated.y : root.y + local.y,
                root.z + flat.z};
    }
    Point Apply(Point root,float artificialYaw,Point animated) {
        return Apply(root,artificialYaw,animated,artificialYaw);
    }
    locomotion::Vec BodyOffset(float artificialYaw,float bodyYaw,
                              locomotion::Vec view,locomotion::Vec floor,float units) const {
        return BodyOffsetAtEye(artificialYaw,bodyYaw,view,floor,units,
            locomotion::Rotate({local.x,local.z},artificialYaw));
    }
    locomotion::Vec BodyOffsetAtEye(float artificialYaw,float bodyYaw,
            locomotion::Vec view,locomotion::Vec floor,float units,
            locomotion::Vec eyeFromRoot) const {
        if (!valid) return {};
        using namespace locomotion;
        // The stabilized eye stays in its tracking frame while Lara turns.
        // Fit her rendered body beneath it, without changing camera/world or
        // collision positions. Subtract floor motion so genuine leaning and
        // roomscale steps are not mistaken for a rotation correction.
        // Use the scene eye BEFORE wall clearance for the standing fit.
        // The caller separately carries the rendered body with the wall-
        // retracted eye, without changing standing/tracking calibration.
        return eyeFromRoot-Rotate(bodyLocal,bodyYaw)
             + Rotate(view-floor,artificialYaw)*units;
    }
};
} // namespace tr::stabilization
