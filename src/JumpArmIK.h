// Keep tracked arms in the same eye-relative pose across native jump squats.
#pragma once
#include "FirstPersonBodyIK.h"
#include <cstdint>

namespace tr::firstperson {
struct JumpArmIK {
    struct Sample {
        bool valid=false,jumping=false,recovering=false;
        double started=0;
        Frame chain[3]{},from[3]{};
    } seed{},current{};
    const void* owner=nullptr;
    uint64_t serial=0;
    double time=0;
    bool mode=false;
    void Reset() { *this={}; }
    bool Apply(const void* item,uint64_t frame,double now,bool jumping,bool mainBody,
               const Frame& reference,Frame (&chain)[3]) {
        Frame inverse{};
        if(!std::isfinite(now) || !Inverse(reference,inverse)) { Reset();return false; }
        if(owner!=item || now<time || now-time>.25) { Reset();owner=item;serial=frame;mode=jumping;time=now; }
        // Hand meshes consume the body's chain, never replace its history
        // with a differently bound attachment or advance it for another eye.
        if(!mainBody) {
            if(current.valid) for(int i=0;i<3;++i) chain[i]=Multiply(reference,current.chain[i]);
            return true;
        }
        if(serial!=frame || mode!=jumping) { seed=current;serial=frame;mode=jumping;time=now; }
        current=seed;
        if(jumping) {
            if(seed.valid) for(int i=0;i<3;++i) chain[i]=Multiply(reference,seed.chain[i]);
            current.recovering=false;
        } else {
            if(seed.valid && seed.jumping) {
                current.started=time;current.recovering=true;
                for(int i=0;i<3;++i) current.from[i]=seed.chain[i];
            }
            if(current.recovering) {
                const float t=std::clamp(float((time-current.started)/.18),0.f,1.f);
                const float w=t*t*(3-2*t);
                Frame from[3]{},to[3]{};
                for(int i=0;i<3;++i) {
                    from[i]=Multiply(reference,current.from[i]);to[i]=chain[i];
                    chain[i].origin=Add(Scale(from[i].origin,1-w),Scale(to[i].origin,w));
                    chain[i].basis=BlendRotation(from[i].basis,to[i].basis,w);
                }
                // Interpolate bone directions rather than shortening the arm
                // by averaging endpoints on different sides of a squat/turn.
                for(int i=1;i<3;++i) {
                    const Vec a=Sub(from[i].origin,from[i-1].origin),b=Sub(to[i].origin,to[i-1].origin);
                    const float length=Length(a);
                    if(length>.001f) {
                        const Vec direction=Transform(BlendRotation(IdentityBasis(),Align(a,b),w),Scale(a,1/length));
                        chain[i].origin=Add(chain[i-1].origin,Scale(direction,length*(1-w)+Length(b)*w));
                    }
                }
                if(t>=1) current.recovering=false;
            }
        }
        for(int i=0;i<3;++i) current.chain[i]=Multiply(inverse,chain[i]);
        current.valid=true;current.jumping=jumping;
        return true;
    }
};
}
