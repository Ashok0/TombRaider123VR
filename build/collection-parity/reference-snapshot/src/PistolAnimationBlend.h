#pragma once
#include "FirstPersonIK.h"

namespace tr::firstperson {
// Render-only handoff. Cache joint-local transforms so camera translation and
// body motion carry the arms without leaving a frozen hand in world space.
struct PistolAnimationBlend {
    bool valid=false,blending=false;
    const void* item=nullptr;
    int mode=0;
    uint64_t serial=0;
    double time=0,started=0;
    Frame last[6]{},from[6]{};
    void Reset() { *this={}; }
    void Apply(const void* owner,int status,uint64_t frame,double now,Frame (&pose)[15]) {
        Frame local[6]{};
        for(int i=0;i<6;++i) {
            const int parent=i%3 ? i+7 : 7;
            Frame inverse{};if(!Inverse(pose[parent],inverse)) { Reset();return; }
            local[i]=Multiply(inverse,pose[i+8]);
        }
        const int next=status==2 || status==3 ? status : 0;
        if(!valid || item!=owner || now<time || now-time>.25) {
            Reset();valid=true;item=owner;mode=next;serial=frame;time=now;
        } else if(next!=mode) {
            // The last displayed pose is also the start of a reversed/cancelled
            // transition. Never consult the controller while native arms own it.
            for(int i=0;i<6;++i) from[i]=last[i];
            mode=next;started=now;blending=true;serial=frame;time=now;
        } else if(serial!=frame) { serial=frame;time=now; }
        const float t=blending ? std::clamp(float((time-started)/.16),0.f,1.f) : 1.f;
        const float weight=t*t*(3-2*t);
        for(int i=0;i<6;++i) {
            Frame result=local[i];
            if(weight<1) {
                auto a=RotationOf(from[i].basis),b=RotationOf(local[i].basis);
                if(a.w*b.w+Dot(a.v,b.v)<0) { b.w=-b.w;b.v=Scale(b.v,-1); }
                result.basis=RotationMatrix({a.w+(b.w-a.w)*weight,Add(Scale(a.v,1-weight),Scale(b.v,weight))});
                for(int col=0;col<3;++col) {
                    const auto length=[col](const Basis& basis) {
                        return std::sqrt(basis.r[0][col]*basis.r[0][col]+basis.r[1][col]*basis.r[1][col]+basis.r[2][col]*basis.r[2][col]);
                    };
                    const float size=length(from[i].basis)*(1-weight)+length(local[i].basis)*weight;
                    for(int row=0;row<3;++row) result.basis.r[row][col]*=size;
                }
                result.origin=Add(Scale(from[i].origin,1-weight),Scale(local[i].origin,weight));
                // Preserve the child bone's reach while blending its direction.
                Vec direction{};
                if(i%3 && Unit(result.origin,direction)) result.origin=Scale(direction,
                    std::sqrt(Dot(from[i].origin,from[i].origin))*(1-weight)+
                    std::sqrt(Dot(local[i].origin,local[i].origin))*weight);
            }
            last[i]=result;
            const int parent=i%3 ? i+7 : 7;
            pose[i+8]=Multiply(pose[parent],result);
        }
        if(t>=1) blending=false;
    }
};
}
