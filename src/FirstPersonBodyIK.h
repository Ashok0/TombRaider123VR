// Headset/controller-only body inference. Input/output are joint frames, not
// skinning matrices. Free body follows the eye; constrained grips retain native feet.
#pragma once
#include "FirstPersonIK.h"

namespace tr::firstperson {
inline float Length(Vec v) { return std::sqrt(Dot(v,v)); }
inline Vec LimitLength(Vec v,float limit) {
    const float n=Length(v);return n>limit ? Scale(v,limit/n) : v;
}
inline bool Finite(const Frame& f) {
    if (!std::isfinite(f.origin.x) || !std::isfinite(f.origin.y) || !std::isfinite(f.origin.z)) return false;
    for (const auto& row:f.basis.r) for(float value:row) if(!std::isfinite(value)) return false;
    return true;
}
inline void ScaleBody(const Frame (&native)[15],float factor,Frame (&sized)[15]) {
    // The lower foot supplies the floor during a stride. Averaging with the
    // swinging foot would make the planted foot rise as the other knee lifts.
    const Vec pivot{native[0].origin.x,std::max(native[3].origin.y,native[6].origin.y),native[0].origin.z};
    for(int j=0;j<15;++j) {
        sized[j]=native[j];sized[j].origin=Add(pivot,Scale(Sub(native[j].origin,pivot),factor));
        for(auto& row:sized[j].basis.r) for(auto& v:row) v*=factor;
    }
}
inline float KneeBendDegrees(const Frame (&pose)[15],int first) {
    const Vec a=Sub(pose[first+1].origin,pose[first].origin);
    const Vec b=Sub(pose[first+2].origin,pose[first+1].origin);
    const float lengths=Length(a)*Length(b);
    return lengths>1.e-6f ? std::acos(std::clamp(Dot(a,b)/lengths,-1.f,1.f))*57.29577951f : 0;
}
inline Basis LimitRotation(Basis rotation,float radians) {
    auto q=RotationOf(rotation);
    if(q.w<0) { q.w=-q.w;q.v=Scale(q.v,-1); }
    const float angle=2*std::atan2(Length(q.v),q.w);
    Vec axis{};
    return angle>radians && Unit(q.v,axis) ? RotationMatrix(AxisRotation(axis,radians)) : rotation;
}

// Native climbing animation owns the contact and hand orientation. Headset
// fitting may move the shoulder only within reach, never elongate either bone.
inline bool SolveGripArm(const Frame (&native)[3],Vec shoulder,Vec fallbackPole,Frame (&posed)[3]) {
    for(const auto& joint:native) if(!Finite(joint)) return false;
    if(!Finite({IdentityBasis(),shoulder})) return false;
    const Vec offset=Sub(shoulder,native[0].origin);
    const Vec shift=Scale(offset,ArmClearanceFraction(native,offset));
    Frame chain[3]{},changes[3]{};
    for(int i=0;i<3;++i) { chain[i]=native[i];chain[i].origin=Add(chain[i].origin,shift); }
    const Vec upper=Sub(native[1].origin,native[0].origin);
    Vec axis{};if(!Unit(Sub(native[2].origin,native[0].origin),axis) && !Unit(upper,axis)) return false;
    const Vec bend=Sub(upper,Scale(axis,Dot(upper,axis)));
    const float length=Length(upper)+Length(Sub(native[2].origin,native[1].origin));
    // A straight authored elbow has no meaningful bend plane; tiny animation
    // noise must not choose an arbitrary world axis or flip the elbow inward.
    Vec pole{};
    if(!Unit(Sub(fallbackPole,Scale(axis,Dot(fallbackPole,axis))),pole))
        Unit(Cross(axis,std::fabs(axis.x)<.8f ? Vec{1,0,0} : Vec{0,1,0}),pole);
    Vec authored{};
    if(Unit(bend,authored) && length>1.e-3f) {
        float weight=std::clamp((Length(bend)/length-.02f)/.06f,0.f,1.f);
        weight=weight*weight*(3-2*weight);
        const float angle=std::acos(std::clamp(Dot(pole,authored),-1.f,1.f));
        pole=Transform(LimitRotation(Align(pole,authored),angle*weight),pole);
    }
    if(!SolveArm(chain,native[2],pole,changes,nullptr,nullptr,false,{},true)) return false;
    for(int i=0;i<3;++i) posed[i]=Multiply(changes[i],chain[i]);
    posed[2]=native[2];
    return true;
}

inline bool SolveBody(const Frame (&native)[15],Vec headDelta,
                      const Basis& headRotation,Vec forward,float scale,
                      Frame (&posed)[15]) {
    if (!std::isfinite(scale) || scale<=1 || !Finite({headRotation,headDelta})) return false;
    for (const auto& joint:native) { Frame inverse{};if(!Finite(joint) || !Inverse(joint,inverse)) return false; }
    float upper[2]{},lower[2]{};
    for(int leg=0;leg<2;++leg) {
        const int first=1+leg*3;
        upper[leg]=Length(Sub(native[first+1].origin,native[first].origin));
        lower[leg]=Length(Sub(native[first+2].origin,native[first+1].origin));
        if(upper[leg]<1.e-3f || lower[leg]<1.e-3f) return false;
    }
    const float legLength=std::min(upper[0]+lower[0],upper[1]+lower[1]);
    headDelta=LimitLength(headDelta,scale*.85f);
    Vec shift=LimitLength({headDelta.x*.35f,0,headDelta.z*.35f},scale*.20f);
    shift.y=std::clamp(headDelta.y*.9f,-scale*.12f,legLength*.65f);
    // Start at the feasible animated pelvis and move only as far as BOTH legs
    // can reach. This keeps feet planted without stretching either leg, even
    // on steps, in a split running stride, or with unequal bone lengths.
    auto reachable=[&](float t) {
        for(int leg=0;leg<2;++leg) {
            const int first=1+leg*3;
            const float d=Length(Sub(native[first+2].origin,Add(native[first].origin,Scale(shift,t))));
            if(d>upper[leg]+lower[leg]+.001f || d<std::fabs(upper[leg]-lower[leg])-.001f) return false;
        }
        return true;
    };
    // The first intersection along this ray is the physical limit. Sampling
    // also catches a path through the inner (fully folded) unreachable sphere.
    float valid=0;
    for(int step=1;step<=32;++step) {
        const float next=step/32.f;
        if(reachable(next)) { valid=next;continue; }
        float hi=next;
        for(int i=0;i<16;++i) { const float mid=(valid+hi)*.5f;if(reachable(mid)) valid=mid;else hi=mid; }
        break;
    }
    shift=Scale(shift,valid);
    for(int i=0;i<15;++i) posed[i]=native[i];
    posed[0].origin=Add(native[0].origin,shift);
    const Vec spine=Sub(native[14].origin,native[7].origin);
    if(Length(spine)<1.e-3f) return false;
    constexpr float radians=3.14159265358979323846f/180.f;
    const Basis lean=LimitRotation(Align(spine,Add(spine,Sub(headDelta,shift))),35*radians);
    // Share a bounded fraction of head yaw with the chest. Pelvis orientation
    // remains with locomotion so looking sideways does not rotate planted feet.
    const Vec facing=Transform(headRotation,forward);
    const float yaw=std::clamp(std::atan2(Cross(forward,facing).y,Dot(forward,facing))*.35f,-20*radians,20*radians);
    const Basis torso=Multiply(RotationMatrix(AxisRotation({0,1,0},yaw)),lean);
    const Vec pivot=native[7].origin;
    const Frame chest{torso,Sub(Add(pivot,shift),Transform(torso,pivot))};
    for(int i=7;i<15;++i) posed[i]=Multiply(chest,native[i]);
    posed[14].basis=Multiply(headRotation,native[14].basis);
    for(int leg=0;leg<2;++leg) {
        const int first=1+leg*3;
        Frame chain[3]={native[first],native[first+1],native[first+2]},correction[3]{};
        for(auto& joint:chain) joint.origin=Add(joint.origin,shift);
        // Keep the authored knee side when it has a clear bend; use forward
        // for near-straight legs, where the native pole is underdetermined.
        Vec axis{};Unit(Sub(native[first+2].origin,native[first].origin),axis);
        const Vec knee=Sub(native[first+1].origin,native[first].origin);
        Vec pole=Sub(knee,Scale(axis,Dot(knee,axis)));
        if(Length(pole)<legLength*.05f) pole=forward;
        if(!SolveArm(chain,native[first+2],pole,correction,nullptr,nullptr,false)) return false;
        for(int j=0;j<3;++j) posed[first+j]=Multiply(correction[j],chain[j]);
        // Never accumulate numerical changes to the floor contact/orientation.
        posed[first+2]=native[first+2];
    }
    for(const auto& joint:posed) if(!Finite(joint)) return false;
    return true;
}
// Free-standing/airborne body: solve from this frame's native skeleton and
// absolute eye target, never yesterday's solved pose or a partial root offset.
// Feet carry their animated stride into the tracked footprint. Vertical IK
// keeps floor height when reachable; extreme heights release contact rather
// than stretching legs or pulling the torso away from the headset.
inline bool SolveBodyAtEye(const Frame (&native)[15],int anchorJoint,Vec anchor,Vec eye,
                           float yawDelta,const Basis& headRotation,Vec forward,
                           Frame (&posed)[15]) {
    if(anchorJoint<0 || anchorJoint>=15 || !std::isfinite(yawDelta) ||
       !Finite({headRotation,anchor}) || !Finite({IdentityBasis(),eye})) return false;
    Frame inverseAnchor{};
    if(!Inverse(native[anchorJoint],inverseAnchor)) return false;
    const Vec localAnchor=Transform(inverseAnchor,anchor);
    const Basis yaw=RotationMatrix(AxisRotation({0,1,0},yawDelta));
    const Frame pivot{yaw,Sub(anchor,Transform(yaw,anchor))};
    Frame footprint[15]{};
    for(int joint=0;joint<15;++joint) {
        Frame inverse{};
        if(!Finite(native[joint]) || !Inverse(native[joint],inverse)) return false;
        footprint[joint]=Multiply(pivot,native[joint]);
        posed[joint]=footprint[joint];
    }
    posed[14].basis=Multiply(headRotation,posed[14].basis);
    const Vec shift=Sub(eye,Transform(posed[anchorJoint],localAnchor));
    for(auto& joint:posed) joint.origin=Add(joint.origin,shift);
    for(int leg=0;leg<2;++leg) {
        const int first=1+leg*3;
        Frame chain[3]={posed[first],posed[first+1],posed[first+2]},correction[3]{};
        const float upper=Length(Sub(chain[1].origin,chain[0].origin));
        const float lower=Length(Sub(chain[2].origin,chain[1].origin));
        if(upper<1.e-3f || lower<1.e-3f) return false;
        Frame foot=footprint[first+2];foot.origin=Add(foot.origin,{shift.x,0,shift.z});
        Vec delta=Sub(foot.origin,chain[0].origin),axis{};
        float distance=Length(delta);
        if(!Unit(delta,axis) && !Unit(Sub(chain[2].origin,chain[0].origin),axis)) axis={0,1,0};
        const float reachable=std::clamp(distance,std::fabs(upper-lower)+.001f,upper+lower-.001f);
        if(std::fabs(reachable-distance)>.0001f) foot.origin=Add(chain[0].origin,Scale(axis,reachable));
        const Vec knee=Sub(chain[1].origin,chain[0].origin);
        Vec pole=Sub(knee,Scale(axis,Dot(knee,axis)));
        if(Length(pole)<(upper+lower)*.05f) pole=forward;
        if(!SolveArm(chain,foot,pole,correction,nullptr,nullptr,false)) return false;
        for(int i=0;i<3;++i) posed[first+i]=Multiply(correction[i],chain[i]);
        posed[first+2]=foot;
    }
    for(const auto& joint:posed) if(!Finite(joint)) return false;
    return true;
}
// Lazy pelvis/footprint with a hard eye target. Preserve the authored torso
// shape with bounded leaning; sparse neck joints absorb small head movement.
// Hands are solved separately.
inline bool SolveFollowingBody(const Frame (&native)[15],Vec anchor,Vec eye,float yawDelta,
                               const Basis& headRotation,Vec forward,float scale,Vec rootOffset,
                               float headTwist,bool grounded,float stepPhase,float stepWeight,
                               Frame (&posed)[15],float standingWeight=0) {
    if(!std::isfinite(scale) || scale<=1 || !Finite({headRotation,eye}) ||
       !Finite({IdentityBasis(),rootOffset})) return false;
    for(const auto& joint:native) { Frame inverse{};if(!Finite(joint) || !Inverse(joint,inverse)) return false; }
    Frame inverseHead{};Inverse(native[14],inverseHead);
    const Vec localEye=Transform(inverseHead,anchor);
    const Basis yaw=RotationMatrix(AxisRotation({0,1,0},yawDelta));
    const Frame turn{yaw,Sub(Add(native[0].origin,rootOffset),Transform(yaw,native[0].origin))};
    Frame footprint[15]{};
    for(int j=0;j<15;++j) footprint[j]=posed[j]=Multiply(turn,native[j]);
    Frame head{Multiply(headRotation,footprint[14].basis),{}};
    head.origin=Sub(eye,Transform(head.basis,localEye));
    // Position drives posture; orientation drives only the head. Rotating the
    // eye-to-neck offset must not masquerade as a lean/crouch and bend the chest.
    const Vec postureHead=Sub(eye,Transform(footprint[14].basis,localEye));
    const Vec displacement=Sub(postureHead,footprint[14].origin);
    const Vec spine=Sub(footprint[14].origin,footprint[0].origin);
    if(Length(spine)<.001f) return false;
    Vec lean{displacement.x,0,displacement.z};
    const float distance=Length(lean),neckSlack=.04f*scale;
    lean=distance>neckSlack ? Scale(lean,(distance-neckSlack)/distance) : Vec{};
    // Lara has one torso joint, not a multi-bone human spine. A two-bone arm
    // solve folds this chest like an elbow and can lift it into the camera.
    // Preserve the authored spine shape and apply a bounded coherent lean.
    const Basis bend=LimitRotation(Align(spine,Add(spine,lean)),.34906585f);
    const float heightShift=std::copysign(std::max(0.f,std::fabs(displacement.y)-.015f*scale),displacement.y);
    Vec pelvisShift{0,heightShift,0};
    const Vec bentHead=Add(footprint[0].origin,Transform(bend,spine));
    const Vec residual=Sub(postureHead,Add(bentHead,pelvisShift));
    // Absorb small movements in the neck; only an exhausted lean pulls the
    // hips under the head. Keep head position exact even at extreme tracking.
    const float residualLength=Length(residual);
    if(residualLength>neckSlack) pelvisShift=Add(pelvisShift,
        Scale(residual,(residualLength-neckSlack)/residualLength));
    posed[0].origin=Add(footprint[0].origin,pelvisShift);
    posed[0].basis=footprint[0].basis;
    posed[7].origin=Add(posed[0].origin,Transform(bend,Sub(footprint[7].origin,footprint[0].origin)));
    posed[7].basis=Multiply(bend,footprint[7].basis);
    posed[14]=head;
    // Caller supplies the chest contribution after neck slack/controller
    // support. Twist around the torso axis, keeping its pivot and hips fixed.
    Vec spineAxis{};if(!Unit(Transform(bend,spine),spineAxis)) return false;
    const Basis twist=RotationMatrix(AxisRotation(spineAxis,-std::clamp(headTwist,-.34906585f,.34906585f)));
    posed[7].basis=Multiply(twist,posed[7].basis);
    if(grounded && standingWeight>0) {
        // Idle upright stance is a leg constraint, not a consequence of the
        // eye/neck deadzone. Raise the hips to almost full knee extension;
        // keep the tracked head exact and let the sparse neck absorb the
        // small remaining height difference. Choose the lower of the two
        // feasible hip heights so neither leg stretches on uneven contacts.
        float standingY=-INFINITY;
        bool reachable=true;
        for(int first:{1,4}) {
            const float a=Length(Sub(footprint[first+1].origin,footprint[first].origin));
            const float b=Length(Sub(footprint[first+2].origin,footprint[first+1].origin));
            const Vec hipOffset=Sub(footprint[first].origin,footprint[0].origin);
            const Vec hip=Add(posed[0].origin,hipOffset),foot=footprint[first+2].origin;
            const float horizontal=(hip.x-foot.x)*(hip.x-foot.x)+(hip.z-foot.z)*(hip.z-foot.z);
            const float reach=(a+b)*.99995f;
            if(horizontal>=reach*reach) { reachable=false;break; }
            standingY=std::max(standingY,foot.y-std::sqrt(reach*reach-horizontal)-hipOffset.y);
        }
        if(reachable) {
            // The size fit normally leaves only millimetres to correct. Do
            // not pull the torso into the eye for an extreme manual scale.
            const float neckClearance=std::fabs(footprint[7].origin.y-footprint[14].origin.y)*.35f;
            const float maxRaise=std::clamp(posed[7].origin.y-head.origin.y-neckClearance,0.f,scale*.25f);
            const float lift=std::clamp(standingY-posed[0].origin.y,-maxRaise,scale*.10f)*
                std::clamp(standingWeight,0.f,1.f);
            posed[0].origin.y+=lift;posed[7].origin.y+=lift;pelvisShift.y+=lift;
        }
    }
    Frame chestInverse{},pelvisInverse{};Inverse(footprint[7],chestInverse);Inverse(footprint[0],pelvisInverse);
    const Frame chest=Multiply(posed[7],chestInverse),pelvis=Multiply(posed[0],pelvisInverse);
    for(int j=8;j<14;++j) posed[j]=Multiply(chest,footprint[j]);
    for(int leg=0;leg<2;++leg) {
        const int first=1+leg*3;
        Frame chain[3]={Multiply(pelvis,footprint[first]),Multiply(pelvis,footprint[first+1]),
                        Multiply(pelvis,footprint[first+2])},correction[3]{};
        Frame foot=footprint[first+2];
        if(!grounded) foot.origin=Add(foot.origin,pelvisShift);
        else {
            const float wave=std::sin(stepPhase+leg*3.14159265359f);
            foot.origin=Add(foot.origin,Scale(forward,wave*.055f*scale*stepWeight));
            foot.origin.y-=std::max(0.f,wave)*.035f*scale*stepWeight;
        }
        const float a=Length(Sub(chain[1].origin,chain[0].origin)),b=Length(Sub(chain[2].origin,chain[1].origin));
        if(a<.001f || b<.001f) return false;
        Vec direction{};const Vec toFoot=Sub(foot.origin,chain[0].origin);
        const float length=Length(toFoot);if(!Unit(toFoot,direction)) direction={0,1,0};
        const float feasible=std::clamp(length,std::fabs(a-b)+.001f,a+b-.001f);
        foot.origin=Add(chain[0].origin,Scale(direction,feasible));
        if(!SolveArm(chain,foot,forward,correction,nullptr,nullptr,false)) return false;
        for(int j=0;j<3;++j) posed[first+j]=Multiply(correction[j],chain[j]);
        posed[first+2]=foot;
    }
    for(const auto& joint:posed) if(!Finite(joint)) return false;
    return true;
}
// Repose the torso against stable leg geometry. Positions are metres; the
// tracked head stays exact and arms are subsequently solved to controllers.
inline bool ReposeTorso(Frame (&pose)[15],const Frame (&target)[4],Vec forward,const Frame* planted=nullptr) {
    Frame result[15];for(int j=0;j<15;++j) result[j]=pose[j];
    const Frame* legs=planted ? planted : pose;
    Frame inverseHip{},inverseChest{};
    if(!Inverse(legs[0],inverseHip) || !Inverse(pose[7],inverseChest)) return false;
    result[0]=target[0];result[7]=target[1];
    if(planted) {
        // Y-down: lower the pelvis at full extension rather than drag feet.
        const Frame hip=Multiply(result[0],inverseHip);
        float lower=0;
        for(int leg=0;leg<2;++leg) {
            const int first=1+leg*3;
            const float a=Length(Sub(legs[first+1].origin,legs[first].origin));
            const float b=Length(Sub(legs[first+2].origin,legs[first+1].origin));
            const Vec socket=Transform(hip,legs[first].origin),foot=target[2+leg].origin;
            const float horizontal=(socket.x-foot.x)*(socket.x-foot.x)+(socket.z-foot.z)*(socket.z-foot.z);
            const float reach=a+b-.000001f;
            if(horizontal<reach*reach)
                lower=std::max(lower,foot.y-std::sqrt(reach*reach-horizontal)-socket.y);
        }
        result[0].origin.y+=lower;result[7].origin.y+=lower;
    }
    const Frame hip=Multiply(result[0],inverseHip),chest=Multiply(result[7],inverseChest);
    for(int j=8;j<14;++j) result[j]=Multiply(chest,pose[j]);
    for(int leg=0;leg<2;++leg) {
        const int first=1+leg*3;
        Frame chain[3]={Multiply(hip,legs[first]),Multiply(hip,legs[first+1]),Multiply(hip,legs[first+2])},changes[3]{};
        Frame foot=target[2+leg];
        const float a=Length(Sub(chain[1].origin,chain[0].origin)),b=Length(Sub(chain[2].origin,chain[1].origin));
        Vec delta=Sub(foot.origin,chain[0].origin),direction{};
        const float d=Length(delta);
        if(a<.001f || b<.001f || !Unit(delta,direction)) return false;
        const float feasible=std::clamp(d,std::fabs(a-b)+.000001f,a+b-.000001f);
        if(std::fabs(feasible-d)>.000001f) foot.origin=Add(chain[0].origin,Scale(direction,feasible));
        if(!SolveArm(chain,foot,forward,changes,nullptr,nullptr,false)) return false;
        for(int j=0;j<3;++j) result[first+j]=Multiply(changes[j],chain[j]);
        result[first+2]=foot;
    }
    for(const auto& joint:result) if(!Finite(joint)) return false;
    for(int j=0;j<15;++j) pose[j]=result[j];
    return true;
}
} // namespace tr::firstperson
