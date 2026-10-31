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
// A mount can relocate the native root before pos_prev catches up. Once the
// camera accepts that destination height, keep it for the entire root pair,
// not just the first rendered frame of the grounded state.
struct MountRootHeight {
    bool active=false;
    int32_t from=0,to=0;
    void Reset() { *this={}; }
    void Begin(int32_t previous,int32_t current) { active=true;from=previous;to=current; }
    int32_t Apply(int32_t previous,int32_t current,int32_t interpolated) {
        if(active && previous==from && current==to) return current;
        Reset();return interpolated;
    }
};
// Remove only the mount-to-standing discontinuity. A bounded residual carries
// incoming animation velocity and decays to zero. Live locomotion keeps moving
// the target and HMD tracking is added later.
struct MountCameraTransition {
    const void* owner=nullptr;
    int level=-1;
    bool have=false,mounting=false,active=false;
    Point last{},offset{},velocity{},sample{},lastTarget{},tangent{};
    double time=0,start=0,sampleTime=0;
    float duration=0;
    void Reset() { *this={}; }
    // Remember the view that survived collision/floor correction, so the next
    // handoff starts where the player actually was, not behind a wall.
    void Presented(Point view,double now) {
        if(!have || !mounting || !std::isfinite(now) || !std::isfinite(view.x) ||
           !std::isfinite(view.y) || !std::isfinite(view.z)) return;
        // Ignore sub-frame duplicate passes. Use the collision-corrected
        // animation camera; tracked HMD motion is composed later.
        const double dt=now-sampleTime;
        if(dt>=1.0/240) {
            velocity=dt<=.1 ? Point{float((view.x-sample.x)/dt),
                float((view.y-sample.y)/dt),float((view.z-sample.z)/dt)} : Point{};
            sample=view;sampleTime=now;
        } else if(dt==0) sample=view;
        last=view;
    }
    Point Apply(const void* item,int currentLevel,bool mount,bool standing,Point target,
                double now,float seconds,float units) {
        if(!item || (!mount && !standing) || !std::isfinite(now) ||
           !std::isfinite(seconds) || seconds<=0 || !std::isfinite(units) || units<=1 ||
           !std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) {
            Reset();return target;
        }
        if(owner!=item || level!=currentLevel || !have || now<time || now-time>.25) {
            Reset();owner=item;level=currentLevel;have=true;mounting=mount;
            last=lastTarget=sample=target;time=sampleTime=now;
            return target;
        }
        time=now;
        if(mount) {
            if(!mounting) { sample=target;sampleTime=now;velocity={}; }
            mounting=true;active=false;last=lastTarget=target;return target;
        }
        if(mounting) {
            mounting=false;
            offset={last.x-target.x,last.y-target.y,last.z-target.z};
            const float distance2=offset.x*offset.x+offset.y*offset.y+offset.z*offset.z;
            // Large relocations are camera cuts, not animation handoffs.
            active=distance2>1 && distance2<=units*units*.75f*.75f;
            start=now;
            duration=std::clamp(seconds,.03f,.4f);
            if(now-sampleTime>.05) velocity={};
            // Quintic Hermite tangents in [-2.5, 0] times the residual
            // preserve motion toward the destination without overshooting.
            const auto slope=[&](float gap,float speed) {
                const float travel=std::clamp(speed,-units*3,units*3)*duration;
                return gap>0 ? std::clamp(travel,-2.5f*gap,0.f) :
                    gap<0 ? std::clamp(travel,0.f,-2.5f*gap) : 0.f;
            };
            tangent={slope(offset.x,velocity.x),slope(offset.y,velocity.y),slope(offset.z,velocity.z)};
        } else if(active) {
            const Point step{target.x-lastTarget.x,target.y-lastTarget.y,target.z-lastTarget.z};
            if(step.x*step.x+step.y*step.y+step.z*step.z>units*units*.75f*.75f) {
                Reset();return target;
            }
        }
        lastTarget=target;
        if(active) {
            const float t=float(std::clamp((now-start)/duration,0.0,1.0));
            const float weight=1-t*t*t*(10+t*(-15+6*t));
            const float motion=t*(1-t)*(1-t)*(1-t)*(1+3*t);
            target.x+=offset.x*weight+tangent.x*motion;
            target.y+=offset.y*weight+tangent.y*motion;
            target.z+=offset.z*weight+tangent.z*motion;
            if(t>=1) active=false;
        }
        last=target;
        return target;
    }
};
// Smooth stance height relative to the body, leaving native vertical travel intact.
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

// Keep a standing eye reference across grounded animation frames. Physical
// body turns do not rotate this offset a second time; artificial turns do.
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
        const bool standing=!followAnimatedHeight && std::isfinite(height) &&
            height<=-500 && height>=-950;
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
        // Hard-landing kneels retain stable X/Z but follow the animated neck
        // vertically. Never save the impact/recovery pose as standing height.
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
        // Fit the scene eye before wall clearance. The caller separately
        // carries the rendered body with the retracted eye; standing and
        // tracking calibration never absorb that temporary retraction.
        return eyeFromRoot-Rotate(bodyLocal,bodyYaw)
             + Rotate(view-floor,artificialYaw)*units;
    }
};
} // namespace tr::stabilization
