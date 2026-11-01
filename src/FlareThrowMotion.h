#pragma once
#include <cmath>
#include <cstdint>
#include "MotionGunMath.h"
#include "LocomotionMath.h"

namespace tr::flarethrow {
using motiongun::Vec;
inline bool Finite(Vec p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
enum class Event { Idle, Tracking, Release };
// Raw tracking-space samples only. Camera turns, body animation, roomscale
// collision corrections and stick movement must never become throwing speed.
struct Gesture {
    struct Sample { Vec p{},relative{}; double time=0; } samples[32]{};
    int count=0;
    uint64_t serial=0;
    bool armed=false;
    double quietSince=-1,swingSince=-1;
    Vec swingOrigin{},swingDirection{};
    void Cancel() { *this={}; }
    Event Update(bool allowed,uint64_t pose,double now,Vec p,Vec head,Vec forward,Vec& velocity) {
        velocity={};
        if(!allowed || !Finite(p) || !Finite(head) || !Finite(forward) || !std::isfinite(now)) {
            Cancel();return Event::Idle;
        }
        const Vec relative=motiongun::Sub(p,head);
        if(count) {
            const double dt=now-samples[count-1].time;
            if(dt<0 || dt>.1) { Cancel();return Event::Idle; }
            // Multiple input polls for the same VR frame must not advance a gesture.
            if(pose==serial || dt==0) return Event::Tracking;
            const auto delta=motiongun::Sub(p,samples[count-1].p);
            const auto relativeDelta=motiongun::Sub(relative,samples[count-1].relative);
            const float limit=.04f+15.f*float(dt);
            if(motiongun::Dot(delta,delta)>limit*limit || motiongun::Dot(relativeDelta,relativeDelta)>limit*limit) {
                Cancel();return Event::Idle;
            }
        }
        while(count && (count==32 || now-samples[0].time>.08)) {
            for(int i=1;i<count;++i) samples[i-1]=samples[i];
            --count;
        }
        samples[count++]={p,relative,now};serial=pose;
        if(count<2 || now-samples[0].time<.025) return Event::Tracking;
        // Regress head-relative intent over 80 ms. Use the last 40 ms of actual
        // hand motion for flight, so acceleration into the throw stays responsive.
        Vec relativeMean{},relativeNum{};double t=0,den=0;
        for(int i=0;i<count;++i) {
            relativeMean=motiongun::Add(relativeMean,samples[i].relative);t+=samples[i].time-now;
        }
        relativeMean=motiongun::Scale(relativeMean,1.f/count);t/=count;
        for(int i=0;i<count;++i) {
            const double dt=samples[i].time-now-t;den+=dt*dt;
            relativeNum=motiongun::Add(relativeNum,motiongun::Scale(motiongun::Sub(samples[i].relative,relativeMean),float(dt)));
        }
        if(den<=1e-8) return Event::Tracking;
        int flightStart=0;
        while(flightStart<count-2 && now-samples[flightStart].time>.04) ++flightStart;
        velocity=motiongun::Scale(motiongun::Sub(p,samples[flightStart].p),float(1/(now-samples[flightStart].time)));
        const Vec intent=motiongun::Scale(relativeNum,float(1/den));
        const float speed=std::sqrt(motiongun::Dot(intent,intent));
        if(!armed) {
            if(speed<=.45f) {
                if(quietSince<0) quietSince=now;
                if(now-quietSince>=.15) armed=true;
            } else quietSince=-1;
            return Event::Tracking;
        }
        // A shoulder/elbow arc can approach the HMD throughout a valid throw.
        // Reject backwards windup by velocity, not increasing head-hand distance.
        const bool backswing=motiongun::Dot(intent,forward)<-.35f*speed;
        const bool inFront=motiongun::Dot(relative,forward)>=-.1f;
        if(speed<.9f || motiongun::Dot(velocity,velocity)<.9f*.9f || backswing || !inFront) {
            swingSince=-1;return Event::Tracking;
        }
        const Vec direction=motiongun::Scale(intent,1.f/speed);
        if(swingSince<0 || motiongun::Dot(direction,swingDirection)<.7f) {
            if(speed<1.2f) return Event::Tracking;
            swingSince=now;swingOrigin=samples[count-2].relative;swingDirection=direction;
        }
        const auto travel=motiongun::Sub(relative,swingOrigin);
        if(speed<1.2f || now-swingSince<.035 || motiongun::Dot(travel,travel)<.10f*.10f) return Event::Tracking;
        armed=false;quietSince=swingSince=-1;count=0;
        return Event::Release; // a new throw needs a quiet hand again
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
