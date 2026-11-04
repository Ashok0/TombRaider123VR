#pragma once
#include "BodyFollower.h"

namespace tr::firstperson {
// Pin the upper-body reference, not just the pelvis position. Native chest
// pitch/translation and the idle filter otherwise still slide the visible
// torso backwards after RUN becomes STOP. Feet retain their animation targets.
struct GroundTorso {
    const void* owner=nullptr;
    float units=0,hipBelowEye=0;
    bool valid=false;
    Basis hip{},chest{};
    Vec chestOffset{};
    void Reset() { *this={}; }
    bool Apply(const void* model,Frame (&pose)[15],const Basis& yaw,Vec eye,float scale,float drop) {
        Frame inverse{};
        if(!Inverse({yaw,pose[0].origin},inverse) || scale<=1) return false;
        if(owner!=model || std::fabs(scale-units)>.01f) Reset();
        const float weight=StandingPostureWeight(drop);
        if(!valid) {
            // Do not calibrate an intentional physical crouch as upright.
            if(weight<1) return true;
            const auto root=Multiply(inverse,pose[0]),torso=Multiply(inverse,pose[7]);
            hip=root.basis;chest=torso.basis;chestOffset=torso.origin;
            hipBelowEye=pose[0].origin.y-eye.y;
            owner=model;units=scale;valid=true;
        }
        if(weight<=0) return true;
        Frame target[4]={pose[0],pose[7],pose[3],pose[6]};
        const Vec fixedOrigin{pose[0].origin.x,eye.y+hipBelowEye,pose[0].origin.z};
        const Frame root{yaw,fixedOrigin};
        Frame fixedHip=Multiply(root,{hip,{}}),fixedChest=Multiply(root,{chest,chestOffset});
        // The headset carries real vertical travel; native run/stop bob does not.
        // Intentional physical crouch blends back to the live posture solver.
        target[0].origin=Add(pose[0].origin,Scale(Sub(fixedHip.origin,pose[0].origin),weight));
        target[0].basis=BlendRotation(pose[0].basis,fixedHip.basis,weight);
        target[1].basis=BlendRotation(pose[7].basis,fixedChest.basis,weight);
        for(int i=0;i<2;++i) for(int col=0;col<3;++col) {
            const auto& b=pose[i ? 7 : 0].basis;
            const float size=std::sqrt(b.r[0][col]*b.r[0][col]+b.r[1][col]*b.r[1][col]+b.r[2][col]*b.r[2][col]);
            for(int row=0;row<3;++row) target[i].basis.r[row][col]*=size;
        }
        target[1].origin=Add(pose[7].origin,Scale(Sub(fixedChest.origin,pose[7].origin),weight));
        return ReposeTorso(pose,target,Transform(yaw,{0,0,1}));
    }
};
}
