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

// Feet, ledge grips and armed wrists preserve the exact target orientation.
// Unarmed hands alone use the anti-twist wrist constraints.
inline bool SolveArm(const Frame (&native)[3], const Frame& target,
                     Vec preferredBend, Frame* corrections, ArmTwistState* twistState=nullptr,
                     const Basis* neutralWrist=nullptr, bool constrainWrist=true) {
    const Vec upper=Sub(native[1].origin,native[0].origin);
    const Vec lower=Sub(native[2].origin,native[1].origin);
    float a=std::sqrt(Dot(upper,upper)),b=std::sqrt(Dot(lower,lower));
    if (!std::isfinite(a) || !std::isfinite(b) || a<1.e-3f || b<1.e-3f) return false;
    const Vec delta=Sub(target.origin,native[0].origin);
    const float distance=std::sqrt(Dot(delta,delta));
    if (!std::isfinite(distance)) return false;
    Vec direction{};
    if (!Unit(delta,direction) && !Unit(Sub(native[2].origin,native[0].origin),direction))
        direction={0,0,1};
    Vec bend{};
    if (!Unit(Sub(preferredBend,Scale(direction,Dot(preferredBend,direction))),bend))
        Unit(Cross(direction,std::fabs(direction.x)<.8f ? Vec{1,0,0} : Vec{0,1,0}),bend);
    // Preserve segment lengths within reach. Outside it, extend both segments
    // proportionally so the wrist stays exactly at the calibrated controller.
    if (distance>a+b) { const float stretch=distance/(a+b); a*=stretch; b*=stretch; }
    if (distance<std::fabs(a-b)) a=b=(a+b)*.5f;
    const float along=distance>1.e-4f ? (a*a-b*b+distance*distance)/(2*distance) : 0;
    const float height=std::sqrt(std::max(0.f,a*a-along*along));
    const Vec elbow=Add(native[0].origin,Add(Scale(direction,along),Scale(bend,height)));
    const Vec points[3]={native[0].origin,elbow,target.origin};
    ArmTwistState localTwist{};
    Basis wrist=target.basis,forearmRoll=IdentityBasis();
    const Basis lowerSwing=Align(lower,Sub(points[2],points[1]));
    Vec lowerAxis{};
    if (!Unit(Sub(points[2],points[1]),lowerAxis)) return false;
    // A body-relative neutral wrist, independent of the animated hand pose,
    // makes the same controller pose produce the same result in walk and run.
    const Basis rest=neutralWrist ? *neutralWrist : GunBasis(IdentityBasis());
    const Vec restForward{rest.r[0][1],rest.r[1][1],rest.r[2][1]};
    const Basis reference=Multiply(Align(restForward,lowerAxis),rest);
    if (constrainWrist) {
        ConstrainWrist(reference,target.basis,lowerAxis,
                       twistState ? *twistState : localTwist,wrist,forearmRoll);
        // Remove native animation roll before adding controller roll. Otherwise
        // the running animation can reintroduce a knot even with wrist limits.
        const auto animated=RotationOf(Multiply(lowerSwing,native[2].basis));
        const auto neutralize=RotationProduct(RotationOf(reference),RotationInverse(animated));
        const auto roll=UnitRotation({neutralize.w,Scale(lowerAxis,Dot(neutralize.v,lowerAxis))});
        forearmRoll=Multiply(forearmRoll,RotationMatrix(roll));
    }
    for (int i=0;i<3;++i) {
        Frame desired{wrist,target.origin}, inv{};
        if (i<2) {
            const Vec old=Sub(native[i+1].origin,native[i].origin);
            const Vec next=Sub(points[i+1],points[i]);
            Vec axis{}; if (!Unit(old,axis)) return false;
            const float ratio=std::sqrt(Dot(next,next)/Dot(old,old));
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
} // namespace tr::firstperson
