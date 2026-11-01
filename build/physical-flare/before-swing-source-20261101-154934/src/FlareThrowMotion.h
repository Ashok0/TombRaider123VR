#pragma once
#include <cmath>
#include <cstdint>
#include "MotionGunMath.h"
#include "LocomotionMath.h"

namespace tr::flarethrow {
using motiongun::Vec;
inline bool Finite(Vec p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
enum class Event { Idle, Held, Release };
// Raw tracking-space samples only. Camera turns, body animation, roomscale
// collision corrections and stick movement must never become throwing speed.
struct Gesture {
    struct Sample { Vec p{}; double time=0; } samples[32]{};
    int count=0;
    uint64_t serial=0;
    bool held=false,drain=false;
    void Cancel() { count=0;held=false;drain=true; }
    Event Update(bool down,bool allowed,uint64_t pose,double now,Vec p,Vec& velocity) {
        velocity={};
        if(!allowed || !Finite(p) || !std::isfinite(now)) {
            Cancel();drain=down;return Event::Idle;
        }
        if(drain) { if(!down) drain=false;return Event::Idle; }
        if(!held && !down) return Event::Idle;
        if(!held) { held=true;count=0; }
        if(count) {
            const double dt=now-samples[count-1].time;
            if(dt<0 || dt>.1) { Cancel();drain=down;return Event::Idle; }
            if(pose!=serial && dt>0) {
                const auto delta=motiongun::Sub(p,samples[count-1].p);
                const float limit=.04f+15.f*float(dt);
                if(motiongun::Dot(delta,delta)>limit*limit) {
                    Cancel();drain=down;return Event::Idle;
                }
            }
        }
        if(!count || (pose!=serial && now>samples[count-1].time)) {
            while(count && (count==32 || now-samples[0].time>.08)) {
                for(int i=1;i<count;++i) samples[i-1]=samples[i];
                --count;
            }
            samples[count++]={p,now};serial=pose;
        }
        if(down) return Event::Held;
        held=false;
        // Reject stale tracking, rather than turning tracking loss into a drop.
        if(!count || now-samples[count-1].time>.05) {count=0;return Event::Idle;}
        if(count>=2 && samples[count-1].time-samples[0].time>=.02) {
            // Linear regression over the last 80 ms reduces controller noise.
            Vec mean{};double t=0,den=0;Vec num{};
            for(int i=0;i<count;++i) {mean=motiongun::Add(mean,samples[i].p);t+=samples[i].time-now;}
            mean=motiongun::Scale(mean,1.f/count);t/=count;
            for(int i=0;i<count;++i) {
                const double dt=samples[i].time-now-t;den+=dt*dt;
                num=motiongun::Add(num,motiongun::Scale(motiongun::Sub(samples[i].p,mean),float(dt)));
            }
            if(den>1e-8) velocity=motiongun::Scale(num,float(1/den));
        }
        count=0;return Event::Release;
    }
};
struct Launch { int16_t yaw=0,speed=0,fall=0; };
inline bool LaunchVelocity(Vec raw,float heading,float units,Launch& out) {
    if(!Finite(raw) || !std::isfinite(heading) || !std::isfinite(units) || units<1 || units>10000) return false;
    const float speed=std::sqrt(motiongun::Dot(raw,raw));
    if(!std::isfinite(speed)) return false;
    if(speed<.25f) raw={}; // a gentle release is a drop
    else if(speed>8.f) raw=motiongun::Scale(raw,8.f/speed);
    const auto horizontal=locomotion::Rotate({raw.x,-raw.z},heading);
    const float factor=units/30.f; // native simulation is 30 Hz
    out.yaw=int16_t(uint16_t(int32_t(std::lround(std::atan2(horizontal.x,horizontal.z)*10430.37835f))));
    out.speed=int16_t(std::lround(locomotion::Length(horizontal)*factor));
    out.fall=int16_t(std::lround(-raw.y*factor));
    return true;
}
inline uint64_t Fingerprint(const uint8_t* bytes,uint32_t size) {
    uint64_t h=14695981039346656037ull;
    for(uint32_t i=0;i<size;++i) h=(h^bytes[i])*1099511628211ull;
    return h;
}
} // namespace tr::flarethrow
