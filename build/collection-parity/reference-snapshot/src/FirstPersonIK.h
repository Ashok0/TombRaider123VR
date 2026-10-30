// Render-only two-bone IK. No simulation, collision or animation state writes.
#pragma once
#include "MotionGunMath.h"
#include <algorithm>

namespace tr::firstperson {
using namespace motiongun;

inline bool Unit(Vec v, Vec& result) {
    const float n=Dot(v,v);
    if (!std::isfinite(n) || n<1.e-10f) return false;
    result=Scale(v,1.f/std::sqrt(n)); return true;
}
inline Basis IdentityBasis() { return {{{1,0,0},{0,1,0},{0,0,1}}}; }

// Shortest-arc rotation, including antiparallel and zero-cross-product cases.
inline Basis Align(Vec from, Vec to) {
    Vec a{},b{};
    if (!Unit(from,a) || !Unit(to,b)) return IdentityBasis();
    Vec axis=Cross(a,b); float s=std::sqrt(Dot(axis,axis));
    const float c=std::clamp(Dot(a,b),-1.f,1.f);
    if (s<1.e-6f) {
        if (c>0) return IdentityBasis();
        Unit(Cross(a,std::fabs(a.x)<.8f ? Vec{1,0,0} : Vec{0,1,0}),axis);
        s=0;
    } else axis=Scale(axis,1.f/s);
    const float u[3]={axis.x,axis.y,axis.z};
    Basis r{};
    for (int i=0;i<3;++i) for (int j=0;j<3;++j)
        r.r[i][j]=(i==j ? c : 0)+(1-c)*u[i]*u[j];
    r.r[0][1]-=s*axis.z; r.r[0][2]+=s*axis.y;
    r.r[1][0]+=s*axis.z; r.r[1][2]-=s*axis.x;
    r.r[2][0]-=s*axis.y; r.r[2][1]+=s*axis.x;
    return r;
}

// Retain a measured twist only for the 180-degree swing singularity, where
// twist is undefined. Defined poses must never inherit accumulated full turns.
struct ArmTwistState { bool valid=false; float last=0; };
struct ArmRotation { float w=1; Vec v{}; };
inline ArmRotation RotationProduct(ArmRotation a, ArmRotation b) {
    return {a.w*b.w-Dot(a.v,b.v),Add(Add(Scale(b.v,a.w),Scale(a.v,b.w)),Cross(a.v,b.v))};
}
inline ArmRotation RotationInverse(ArmRotation q) { return {q.w,Scale(q.v,-1)}; }
inline ArmRotation UnitRotation(ArmRotation q) {
    const float length=std::sqrt(q.w*q.w+Dot(q.v,q.v));
    return length>1.e-6f ? ArmRotation{q.w/length,Scale(q.v,1.f/length)} : ArmRotation{};
}
inline ArmRotation RotationOf(const Basis& b) {
    // Joint frames may carry stretch. Extract an orthonormal rotation first.
    Vec x{},y{},z{};
    if (!Unit({b.r[0][0],b.r[1][0],b.r[2][0]},x)) return {};
    const Vec column{b.r[0][1],b.r[1][1],b.r[2][1]};
    if (!Unit(Sub(column,Scale(x,Dot(x,column))),y)) return {};
    z=Cross(x,y);
    const float m[3][3]={{x.x,y.x,z.x},{x.y,y.y,z.y},{x.z,y.z,z.z}};
    float q[4]{};const float trace=m[0][0]+m[1][1]+m[2][2];
    if (trace>0) {
        const float s=2*std::sqrt(trace+1);q[0]=s*.25f;
        q[1]=(m[2][1]-m[1][2])/s;q[2]=(m[0][2]-m[2][0])/s;q[3]=(m[1][0]-m[0][1])/s;
    } else {
        int i=m[1][1]>m[0][0] ? 1 : 0;if(m[2][2]>m[i][i]) i=2;
        const int j=(i+1)%3,k=(i+2)%3;
        const float s=2*std::sqrt(std::max(0.f,1+m[i][i]-m[j][j]-m[k][k]));
        if(s<1.e-6f) return {};
        q[0]=(m[k][j]-m[j][k])/s;q[i+1]=s*.25f;
        q[j+1]=(m[j][i]+m[i][j])/s;q[k+1]=(m[k][i]+m[i][k])/s;
    }
    return UnitRotation({q[0],{q[1],q[2],q[3]}});
}
inline Basis RotationMatrix(ArmRotation q) {
    q=UnitRotation(q);const float x=q.v.x,y=q.v.y,z=q.v.z,w=q.w;
    return {{{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)},
             {2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)},
             {2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)}}};
}
inline ArmRotation AxisRotation(Vec axis,float angle) {
    return {std::cos(angle*.5f),Scale(axis,std::sin(angle*.5f))};
}
inline void ConstrainWrist(const Basis& reference,const Basis& target,Vec axis,
                           ArmTwistState& state,Basis& wrist,Basis& forearmRoll) {
    constexpr float pi=3.14159265358979323846f;
    const auto referenceRotation=RotationOf(reference);
    auto relative=RotationProduct(RotationOf(target),RotationInverse(referenceRotation));
    // q = swing * twist about the elbow-to-wrist axis. A 180-degree swing
    // has no defined twist; retain the previous value through that singularity.
    const float projected=Dot(relative.v,axis);
    const float norm=std::hypot(relative.w,projected);
    ArmRotation twist{};
    float angle=state.valid ? state.last : 0;
    if(norm>1.e-5f) {
        twist={relative.w/norm,Scale(axis,projected/norm)};
        angle=std::remainder(2*std::atan2(projected,relative.w),2*pi);
    } else twist=AxisRotation(axis,angle);
    auto swing=UnitRotation(RotationProduct(relative,RotationInverse(twist)));
    if(swing.w<0) { swing.w=-swing.w;swing.v=Scale(swing.v,-1); }
    constexpr float limit=pi*.5f;
    float roll=std::clamp(angle,-limit,limit);
    // The principal twist is periodic. Clamping it directly would jump from
    // +90 to -90 at the +/-180 seam; unwrapping it instead can permanently
    // pin the wrist after a full turn. Soften only the far, unreachable part
    // (135..180 degrees) to meet at zero at that seam. Normal poses and the
    // adjacent limit plateau stay unchanged. Every defined pose now has one
    // result, independent of earlier motion or how many eyes/draws use it.
    constexpr float seamStart=pi*.75f;
    if(std::fabs(angle)>seamStart)
        roll*=(pi-std::fabs(angle))/(pi-seamStart);
    state.last=angle;state.valid=true;
    const float swingAngle=2*std::atan2(std::sqrt(Dot(swing.v,swing.v)),swing.w);
    constexpr float maxBend=55*pi/180;
    if(swingAngle>maxBend) {
        Vec bendAxis{};Unit(swing.v,bendAxis);swing=AxisRotation(bendAxis,maxBend);
    }
    // 80% of pronation/supination goes through the forearm; the wrist keeps
    // only 20%. Positions stay fixed and weapons never use these constraints.
    forearmRoll=RotationMatrix(AxisRotation(axis,roll*.8f));
    wrist=RotationMatrix(RotationProduct(RotationProduct(swing,AxisRotation(axis,roll)),referenceRotation));
}

// Largest fraction of a shoulder translation that retains the native arm's
// reach to its fixed grip. Use double precision near a fully extended arm.
inline float ArmClearanceFraction(const Frame (&arm)[3],Vec offset) {
    const Vec reach=Sub(arm[2].origin,arm[0].origin);
    const Vec upper=Sub(arm[1].origin,arm[0].origin),lower=Sub(arm[2].origin,arm[1].origin);
    const double length=std::sqrt(double(Dot(upper,upper)))+std::sqrt(double(Dot(lower,lower)));
    const double reach2=Dot(reach,reach);
    const double radius2=std::max(reach2,length*length*.99*.99);
    const double a=Dot(offset,offset),b=-2.0*Dot(reach,offset),c=reach2-radius2;
    if(a<1.e-8) return 1.f;
    if(!std::isfinite(a+b+c)) return 0.f;
    return float(std::clamp((-b+std::sqrt(std::max(0.0,b*b-4*a*c)))/(2*a),0.0,1.0));
}

// Feet, ledge grips and armed wrists preserve the exact target orientation.
// Unarmed hands alone use the anti-twist wrist constraints.
inline bool SolveArm(const Frame (&native)[3], const Frame& target,
                     Vec preferredBend, Frame* corrections, ArmTwistState* twistState=nullptr,
                     const Basis* neutralWrist=nullptr, bool constrainWrist=true, Vec gripLocal={},
                     bool rigidReach=false) {
    const Vec upper=Sub(native[1].origin,native[0].origin);
    const Vec lower=Sub(native[2].origin,native[1].origin);
    const float upperLength=std::sqrt(Dot(upper,upper)),lowerLength=std::sqrt(Dot(lower,lower));
    if (!std::isfinite(upperLength) || !std::isfinite(lowerLength) ||
        upperLength<1.e-3f || lowerLength<1.e-3f) return false;
    Vec points[3]{};
    const auto placeBones=[&](Vec endpoint) {
        float a=upperLength,b=lowerLength;
        const Vec delta=Sub(endpoint,native[0].origin);
        float distance=std::sqrt(Dot(delta,delta));
        if (!std::isfinite(distance)) return false;
        Vec direction{};
        if (!Unit(delta,direction) && !Unit(Sub(native[2].origin,native[0].origin),direction))
            direction={0,0,1};
        Vec bend{};
        if (!Unit(Sub(preferredBend,Scale(direction,Dot(preferredBend,direction))),bend))
            Unit(Cross(direction,std::fabs(direction.x)<.8f ? Vec{1,0,0} : Vec{0,1,0}),bend);
        points[0]=native[0].origin;
        if (rigidReach) {
            // Tracked wrists stay on the controllers. Beyond the reachable
            // annulus, translate the shoulder by only the excess instead of
            // scaling either rigid mesh. A tiny margin avoids a locked elbow
            // and the unequal-length folded-arm singularity. This is draw-only;
            // native grips and legs retain their existing anchored-root solve.
            const float margin=std::min(a,b)*.001f;
            const float reachable=std::clamp(distance,std::fabs(a-b)+margin,a+b-margin);
            points[0]=Add(points[0],Scale(direction,distance-reachable));
            distance=reachable;
        } else {
            if (distance>a+b) { const float stretch=distance/(a+b); a*=stretch; b*=stretch; }
            if (distance<std::fabs(a-b)) a=b=(a+b)*.5f;
        }
        const float along=distance>1.e-4f ? (a*a-b*b+distance*distance)/(2*distance) : 0;
        const float height=std::sqrt(std::max(0.f,a*a-along*along));
        points[1]=Add(points[0],Add(Scale(direction,along),Scale(bend,height)));
        points[2]=endpoint;
        return true;
    };
    if (!placeBones(target.origin)) return false;
    ArmTwistState localTwist{};
    Basis wrist=target.basis,forearmRoll=IdentityBasis();
    Basis lowerSwing=Align(lower,Sub(points[2],points[1]));
    Vec lowerAxis{};
    if (!Unit(Sub(points[2],points[1]),lowerAxis)) return false;
    // A body-relative neutral wrist, independent of the animated hand pose,
    // makes the same controller pose produce the same result in walk and run.
    const Basis rest=neutralWrist ? *neutralWrist : GunBasis(IdentityBasis());
    const Vec restForward{rest.r[0][1],rest.r[1][1],rest.r[2][1]};
    Basis reference=Multiply(Align(restForward,lowerAxis),rest);
    if (constrainWrist) {
        ConstrainWrist(reference,target.basis,lowerAxis,
                       twistState ? *twistState : localTwist,wrist,forearmRoll);
        if (Dot(gripLocal,gripLocal)>0) {
            // The gun target already puts this LOCAL grip point on the physical
            // controller. Wrist limits change its rotation: keeping the old
            // wrist origin would orbit the grip around the wrong pivot.
            // Preserve the bounded hand orientation, then reconnect the bones
            // to the wrist position that keeps that same grip point fixed.
            const Vec grip=Transform(target,gripLocal);
            if (!placeBones(Sub(grip,Transform(wrist,gripLocal))) ||
                !Unit(Sub(points[2],points[1]),lowerAxis)) return false;
            lowerSwing=Align(lower,Sub(points[2],points[1]));
            reference=Multiply(Align(restForward,lowerAxis),rest);
            // Distribute twist around the reconnected forearm's actual axis.
            // Do not feed this second decomposition back into wrist history.
            auto redistribution=twistState ? *twistState : localTwist;
            Basis unused{};
            ConstrainWrist(reference,wrist,lowerAxis,redistribution,unused,forearmRoll);
        }
        // Remove native animation roll before adding controller roll. Otherwise
        // the running animation can reintroduce a knot even with wrist limits.
        const auto animated=RotationOf(Multiply(lowerSwing,native[2].basis));
        const auto neutralize=RotationProduct(RotationOf(reference),RotationInverse(animated));
        const auto roll=UnitRotation({neutralize.w,Scale(lowerAxis,Dot(neutralize.v,lowerAxis))});
        forearmRoll=Multiply(forearmRoll,RotationMatrix(roll));
    }
    for (int i=0;i<3;++i) {
        Frame desired{wrist,points[2]}, inv{};
        if (i<2) {
            const Vec old=Sub(native[i+1].origin,native[i].origin);
            const Vec next=Sub(points[i+1],points[i]);
            Vec axis{}; if (!Unit(old,axis)) return false;
            const float ratio=rigidReach ? 1.f : std::sqrt(Dot(next,next)/Dot(old,old));
            const float u[3]={axis.x,axis.y,axis.z};
            auto stretch=IdentityBasis();
            for (int row=0;row<3;++row) for (int col=0;col<3;++col)
                stretch.r[row][col]+=(ratio-1)*u[row]*u[col];
            desired={Multiply(Multiply(Align(old,next),stretch),native[i].basis),points[i]};
            if(i==1) desired.basis=Multiply(forearmRoll,desired.basis);
        }
        if (!Inverse(native[i],inv)) return false;
        corrections[i]=Multiply(desired,inv);
        for (const auto& row:corrections[i].basis.r)
            for (float v:row) if (!std::isfinite(v)) return false;
        const auto& p=corrections[i].origin;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    }
    return true;
}

// Pistol animation palettes interpolate matrix elements, including scale and
// shear. They must not supply lengths or material axes to tracked arms. Build
// the reference chain from the model's bind pose and the current solved torso;
// this follows real body motion without retaining any weapon animation.
inline bool PistolArmReference(const Frame& torso,const Frame& torsoBind,
                               const Frame (&bind)[3],float scale,Frame (&chain)[3]) {
    if(!std::isfinite(scale) || scale<=0) return false;
    auto basis=RotationMatrix(RotationProduct(RotationOf(torso.basis),
                                              RotationInverse(RotationOf(torsoBind.basis))));
    for(auto& row:basis.r) for(float& value:row) value*=scale;
    const Frame placement{basis,Sub(torso.origin,Transform(basis,torsoBind.origin))};
    for(int i=0;i<3;++i) {
        chain[i]=Multiply(placement,bind[i]);
        Frame inverse{};if(!Inverse(chain[i],inverse)) return false;
    }
    return Dot(Sub(chain[1].origin,chain[0].origin),Sub(chain[1].origin,chain[0].origin))>1.e-6f &&
        Dot(Sub(chain[2].origin,chain[1].origin),Sub(chain[2].origin,chain[1].origin))>1.e-6f;
}

// Controller arms use the exact wrist, including roll beyond +/-90 degrees.
// Match forearm roll to the hand through the mesh's REST joint relationship,
// not through an animated gun/recoil pose or an assumed shared local axis.
// Transport that roll back through the elbow to the upper arm. Both changes
// are axial twists: they cannot move joints, change reach or redirect the gun.
// Native feet/grips use SolveArm.
struct ArmRollContinuity {
    bool valid=false,recovering=false;
    float confidence=1;
    Basis wrist{},forearm{};
    Vec axis{};
};

inline bool SolveTrackedArm(const Frame (&native)[3],const Frame& target,
                            Vec pole,const Frame (&bind)[3],Frame* corrections,
                            bool upperArmPresent=true,ArmRollContinuity* continuity=nullptr) {
    if(!SolveArm(native,target,pole,corrections,nullptr,nullptr,false,{},true)) return false;
    const auto forearm=Multiply(corrections[1],native[1]);
    Vec axis{};if(!Unit(Sub(target.origin,forearm.origin),axis)) return false;
    const auto restRelation=RotationProduct(RotationInverse(RotationOf(bind[2].basis)),
                                            RotationOf(bind[1].basis));
    const auto desired=RotationProduct(RotationOf(target.basis),restRelation);
    // Ordinary poses use the actual posed mesh axes. Replacing these with
    // rest axes globally adds a second, different swing/twist projection;
    // animated/partial palettes can then wobble on the opposite arm.
    const auto delta=RotationProduct(desired,RotationInverse(RotationOf(forearm.basis)));
    const ArmRotation twist{delta.w,Scale(axis,Dot(delta.v,axis))};
    auto axial=twist;
    if(twist.w*twist.w+Dot(twist.v,twist.v)<=1.e-10f) {
        // Only an undefined 180-degree wrist swing needs a rest-pose fallback.
        // It must not inherit native idle/recoil roll while tracking is still.
        const auto reference=RotationProduct(
            RotationOf(Align(Sub(bind[2].origin,bind[1].origin),axis)),RotationOf(bind[1].basis));
        const auto correction=RotationProduct(reference,RotationInverse(RotationOf(forearm.basis)));
        axial={correction.w,Scale(axis,Dot(correction.v,axis))};
    }
    if(axial.w*axial.w+Dot(axial.v,axial.v)>1.e-10f) {
        const auto roll=RotationMatrix(axial);
        const Frame aboutElbow{roll,Sub(forearm.origin,Transform(roll,forearm.origin))};
        corrections[1]=Multiply(aboutElbow,corrections[1]);
    }
    if(continuity) {
        // Near an antiparallel wrist swing, extracting axial twist amplifies
        // tiny torso/head movements. Carry the previous material frame through
        // the actual controller rotation and new bone direction in that region.
        // Well-conditioned poses retain the original solve; wrist tracking and
        // all joint positions remain exact, with no hand-position filtering.
        const float confidence=std::sqrt(twist.w*twist.w+Dot(twist.v,twist.v));
        continuity->confidence=confidence;
        if(continuity->valid && (confidence<.35f || continuity->recovering)) {
            const auto handDelta=RotationMatrix(RotationProduct(RotationOf(target.basis),
                RotationInverse(RotationOf(continuity->wrist))));
            const auto predicted=Multiply(Align(Transform(handDelta,continuity->axis),axis),
                                           Multiply(handDelta,continuity->forearm));
            const auto actual=Multiply(corrections[1],native[1]);
            const auto difference=RotationProduct(RotationOf(actual.basis),RotationInverse(RotationOf(predicted)));
            const float angle=std::remainder(2*std::atan2(Dot(difference.v,axis),difference.w),6.28318530718f);
            const float t=std::clamp((confidence-.15f)/.20f,0.f,1.f);
            const auto movedAxis=Transform(handDelta,continuity->axis);
            const float swing=std::atan2(std::sqrt(Dot(Cross(movedAxis,axis),Cross(movedAxis,axis))),Dot(movedAxis,axis));
            const auto handRotation=RotationOf(handDelta);
            const float handTurn=2*std::atan2(std::sqrt(Dot(handRotation.v,handRotation.v)),std::fabs(handRotation.w));
            // Do not release a stored ambiguity as a pop on leaving the band,
            // or keep creeping when all input is stationary. Reconciliation
            // follows actual bone/controller motion, never elapsed render calls.
            const float budget=2*swing+handTurn;
            const float applied=std::clamp(angle*t*t*(3-2*t),-budget,budget);
            continuity->recovering=confidence<.35f || std::fabs(angle-applied)>.001f;
            const auto wanted=Multiply(RotationMatrix(AxisRotation(axis,applied)),predicted);
            const auto change=RotationMatrix(RotationProduct(RotationOf(wanted),RotationInverse(RotationOf(actual.basis))));
            corrections[1]=Multiply({change,Sub(forearm.origin,Transform(change,forearm.origin))},corrections[1]);
        }
        continuity->valid=true;continuity->axis=axis;
        continuity->wrist=RotationMatrix(RotationOf(target.basis));
        continuity->forearm=RotationMatrix(RotationOf(Multiply(corrections[1],native[1]).basis));
    }
    if(upperArmPresent) {
        const auto upper=Multiply(corrections[0],native[0]);
        const auto lower=Multiply(corrections[1],native[1]);
        Vec upperAxis{};
        if(!Unit(Sub(lower.origin,upper.origin),upperAxis)) return false;
        // Carry the forearm's material frame back through the elbow bend.
        // Leaving the upper mesh in its animated roll winds the elbow seam
        // up when the controller rolls inward. Account for the authored bend
        // and independent mesh axes; copying Euler roll is not sufficient.
        const auto restBend=Align(Sub(bind[1].origin,bind[0].origin),
                                  Sub(bind[2].origin,bind[1].origin));
        const auto relation=RotationProduct(RotationInverse(RotationOf(bind[1].basis)),
            RotationProduct(RotationOf(restBend),RotationOf(bind[0].basis)));
        const auto transport=RotationOf(Align(axis,upperAxis));
        const auto desiredUpper=RotationProduct(transport,
            RotationProduct(RotationOf(lower.basis),relation));
        const auto upperDelta=RotationProduct(desiredUpper,RotationInverse(RotationOf(upper.basis)));
        const ArmRotation upperTwist{upperDelta.w,Scale(upperAxis,Dot(upperDelta.v,upperAxis))};
        if(upperTwist.w*upperTwist.w+Dot(upperTwist.v,upperTwist.v)>1.e-10f) {
            const auto roll=RotationMatrix(upperTwist);
            corrections[0]=Multiply({roll,Sub(upper.origin,Transform(roll,upper.origin))},corrections[0]);
        }
    }
    return true;
}
} // namespace tr::firstperson
