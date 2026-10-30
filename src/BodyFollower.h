#pragma once
#include "FirstPersonBodyIK.h"
#include <cstdint>

namespace tr::firstperson {
inline float BodyAngle(float angle) { return std::remainder(angle,6.28318530718f); }
inline Vec Flat(Vec v) { return {v.x,0,v.z}; }
inline Vec YawVector(Vec v,float yaw) {
    return {v.x*std::cos(yaw)+v.z*std::sin(yaw),v.y,-v.x*std::sin(yaw)+v.z*std::cos(yaw)};
}
struct BodyFollowSettings {
    float yawSlack=.785398163f;
    float leanRadius=.15f, leanRelease=.09f, followSpeed=.9f;
};
// Capture once in an ordinary standing pose. Size is not a per-frame IK
// degree of freedom: ducking, walking and leaning must never shrink the body.
struct BodySizeCalibration {
    bool valid=false;
    float factor=1,units=0;
    void Reset() { *this={}; }
    bool Capture(const Frame (&native)[15],Vec anchor,float standingEyeY,float worldUnits) {
        if(valid) return true;
        if(!std::isfinite(standingEyeY+worldUnits) || worldUnits<=1) return false;
        for(const auto& joint:native) if(!Finite(joint)) return false;
        const float floor=std::max(native[3].origin.y,native[6].origin.y);
        const float available=floor-standingEyeY;
        float relaxedHipHeight=0;
        for(int first:{1,4}) {
            const float a=Length(Sub(native[first+1].origin,native[first].origin));
            const float b=Length(Sub(native[first+2].origin,native[first+1].origin));
            if(a<.001f || b<.001f) return false;
            // Near full extension defines standing stature. Even 1% of leg
            // length left unused produces a visible ~16-degree knee bend.
            relaxedHipHeight+=(a+b)*.99995f+native[first].origin.y-native[0].origin.y;
        }
        const float stature=relaxedHipHeight*.5f+native[0].origin.y-anchor.y;
        if(!std::isfinite(stature+available) || stature<worldUnits*.5f || available<worldUnits*.5f) return false;
        factor=std::clamp(available/stature,.5f,1.5f);units=worldUnits;valid=true;return true;
    }
};
// Evaluate hands in the BODY frame, never the current gaze frame. Looking
// aside cannot turn two stationary forward controllers into a new body hint.
inline bool HandsSupportFacing(Vec left,Vec right,Vec leftAim,Vec rightAim,float yaw,bool held=false) {
    const Vec forward{std::sin(yaw),0,std::cos(yaw)};
    Vec a{},b{};
    return Unit(Flat(leftAim),a) && Unit(Flat(rightAim),b) &&
        Dot(Flat(left),forward)>(held ? .10f : .15f) && Dot(Flat(right),forward)>(held ? .10f : .15f) &&
        Dot(a,forward)>(held ? .65f : .8f) && Dot(b,forward)>(held ? .65f : .8f);
}
inline float BodyChestTwist(float headTwist,bool handsForward) {
    if(handsForward) return 0;
    const float excess=std::max(0.f,std::fabs(headTwist)-.523598776f); // Neck owns first 30 degrees.
    return std::copysign(std::min(excess*.35f,.34906585f),headTwist);
}
inline float StandingPostureWeight(float drop) {
    const float t=std::clamp((drop-.10f)/.10f,0.f,1.f);
    return 1-t*t*(3-2*t);
}
// World positions are metres. Update once per tracked pose, independent of
// skin/hand/holster passes and stereo eyes. Artificial turns are applied whole.
struct BodyFollower {
    bool valid=false,turning=false,stepping=false,supportedHands=false;
    uint64_t serial=0;
    double time=0;
    float yaw=0,base=0,phase=0,stepWeight=0;
    Vec root{},nativeRoot{};
    void Reset() { *this={}; }
    void Update(uint64_t frame,double now,float artificialYaw,float headYaw,float travelYaw,
                Vec goal,Vec native,bool moving,const BodyFollowSettings& s,Vec turnPivot={},bool handsForward=false,
                bool pivotFootprint=false) {
        if(!std::isfinite(now+artificialYaw+headYaw+travelYaw) ||
           !Finite({IdentityBasis(),goal}) || !Finite({IdentityBasis(),native})) { Reset();return; }
        if(!valid || now<time || now-time>.25 || Length(Sub(native,nativeRoot))>2) {
            valid=true;turning=stepping=false;serial=frame;time=now;
            yaw=headYaw;base=artificialYaw;root=Flat(goal);nativeRoot=native;stepWeight=0;
            supportedHands=handsForward;return;
        }
        const float turn=BodyAngle(artificialYaw-base);
        if(turn!=0) {
            yaw=BodyAngle(yaw+turn);
            root=Add(Flat(turnPivot),YawVector(Sub(root,Flat(turnPivot)),turn));
            if(pivotFootprint) goal=Add(Flat(turnPivot),YawVector(Sub(goal,Flat(turnPivot)),turn));
        }
        base=artificialYaw;
        if(serial==frame) return;
        supportedHands=handsForward;
        const float dt=float(std::clamp(now-time,0.0,.05));time=now;serial=frame;
        // Artificial travel carries the body; physical root consumption must
        // not count the same HMD displacement a second time.
        if(moving) root=Add(root,Flat(Sub(native,nativeRoot)));
        nativeRoot=native;
        const float delta=BodyAngle(headYaw-yaw);
        // Angular slack, without a timed chase: every degree beyond the
        // boundary is carried in this pose. Stopping the head stops the body.
        const bool supported=handsForward && !moving && !turning && std::fabs(delta)<1.3962634f;
        const float change=moving ? BodyAngle(travelYaw-yaw) : supported ? 0.f :
            delta-std::clamp(delta,-s.yawSlack,s.yawSlack);
        turning=std::fabs(change)>.00001f;
        yaw=BodyAngle(yaw+change);
        if(pivotFootprint && change!=0) {
            root=Add(Flat(turnPivot),YawVector(Sub(root,Flat(turnPivot)),change));
            goal=Add(Flat(turnPivot),YawVector(Sub(goal,Flat(turnPivot)),change));
        }
        const Vec offset=Flat(Sub(goal,root));const float distance=Length(offset);
        // Once an actual step starts, bring the visual body back under the
        // player. Collision-root hysteresis can retain a larger lean margin;
        // retaining it here as well leaves a permanent torso displacement.
        const float settleRadius=std::min(s.leanRelease,.02f);
        if(!stepping && distance>std::min(s.leanRadius,.04f)) stepping=true;
        if(stepping && distance<=settleRadius+.001f) stepping=false;
        Vec move{};
        if(distance>0 && (moving || stepping)) {
            const float excess=std::max(0.f,distance-(moving ? 0.f : settleRadius));
            move=Scale(offset,std::min(excess,s.followSpeed*dt)/distance);root=Add(root,move);
        }
        const float stride=Length(move)+std::fabs(change)*.12f;
        phase=std::fmod(phase+stride*15.f,6.28318530718f);
        const float weight=!moving && stride>.0001f ? 1.f : 0.f;
        stepWeight+=(weight-stepWeight)*(1-std::exp(-12*dt));
    }
};
struct RoomscaleFollower {
    bool following=false;
    Vec Request(Vec offset,float dt,const BodyFollowSettings& s) {
        offset=Flat(offset);const float distance=Length(offset);
        if(!std::isfinite(distance) || distance>2 || !std::isfinite(dt)) { following=false;return {}; }
        if(!following && distance>s.leanRadius) following=true;
        if(following && distance<=s.leanRelease+.001f) following=false;
        if(!following || distance==0) return {};
        return Scale(offset,std::min(std::max(0.f,distance-s.leanRelease),s.followSpeed*std::clamp(dt,0.f,.05f))/distance);
    }
};
struct HeightCrouch {
    bool active=false;
    bool Update(bool enabled,float drop) {
        if(!enabled || !std::isfinite(drop)) active=false;
        // Allow a visible IK squat before native Duck hides the body.
        else if(!active && drop>=.55f) active=true;
        else if(active && drop<=.23f) active=false;
        return active;
    }
};
// TR3 Modern Controls have a built-in crouch toggle (no TR4/5 hold setting).
// Present physical posture as the native latch during Lara simulation, with
// no repeated button press that could toggle it back off. Native clearance and
// crawl/duck/stand animations still own the actual transition.
struct PhysicalCrouchInputScope {
    static constexpr uint32_t Duck=uint32_t(1)<<29, Latch=0x40;
    uint32_t* input=nullptr;
    uint32_t* flags=nullptr;
    uint32_t savedDuck=0,savedLatch=0;
    bool modern=false,held=false;
    PhysicalCrouchInputScope(uint32_t* nativeInput,uint32_t* nativeFlags,bool modernControls,bool physicalHeld)
        :input(nativeInput),flags(nativeFlags),modern(modernControls),held(physicalHeld) {
        if(!input || !flags) { input=flags=nullptr;return; }
        savedDuck=*input&Duck;savedLatch=*flags&Latch;
        if(modern) {
            *flags=(*flags&~Latch)|((held || savedDuck) ? Latch : 0);
            *input&=~Duck;
        } else if(held) *input|=Duck;
    }
    ~PhysicalCrouchInputScope() {
        if(!input) return;
        *input=(*input&~Duck)|savedDuck;
        // A physical hold must not leave a new toggle behind outside the tick.
        // Do not undo a native latch clear (fall/crawl-to-hang), or re-latch
        // after the user stands up. Preserve all unrelated native flag edits.
        if(modern && held && (*flags&Latch)) *flags=(*flags&~Latch)|savedLatch;
    }
    PhysicalCrouchInputScope(const PhysicalCrouchInputScope&)=delete;
    PhysicalCrouchInputScope& operator=(const PhysicalCrouchInputScope&)=delete;
};
// Side/back gaits swing the authored torso even with fixed tracking. Keep the
// last ordinary ground upper-body reference in Lara's root/yaw frame, then
// solve real headset lean and controller motion on top. Animated feet remain
// targets, rather than freezing a whole walking palette.
struct GroundGaitReference {
    bool valid=false;
    Frame local[15]{};
    void Reset() { *this={}; }
    bool Apply(Frame (&pose)[15],const Frame& root,bool sideways,bool capture,bool trackedArms,Vec forward) {
        Frame inverse{};
        if(!Inverse(root,inverse)) return false;
        if(capture && (!valid || !sideways)) {
            for(int j=0;j<15;++j) local[j]=Multiply(inverse,pose[j]);
            valid=true;
        }
        if(!valid || !sideways) return true;
        Frame stable[15]{};
        for(int j=0;j<15;++j) stable[j]=Multiply(root,local[j]);
        const Frame targets[4]={stable[0],stable[7],pose[3],pose[6]};
        if(!ReposeTorso(pose,targets,forward)) return false;
        pose[14]=stable[14];
        if(trackedArms) for(int j=8;j<14;++j) pose[j]=stable[j];
        return true;
    }
};

// Filter only inferred body motion, never tracked head/controller targets.
struct TorsoPoseFilter {
    bool valid=false,mountPending=false,recovering=false,lastGrip=false;
    bool planted=false,standingValid=false;
    uint64_t serial=0;
    double time=0;
    float cadence=1.f/90.f;
    Frame cached[4]{},eyeRelative[4]{},stance[7]{},standingHip{};
    Vec nativeContacts[2]{};
    bool contactsValid=false;
    void Reset() { *this={}; }
    void Mount() { mountPending=true; }
    bool Update(uint64_t frame,double now,const Frame& reference,const Frame& eye,
                bool idle,bool grip,Frame (&pose)[15],Vec forward,bool plant=false,float postureDrop=0,
                const Vec* contacts=nullptr) {
        constexpr int joint[4]={0,7,3,6};
        Frame inverse{},inverseEye{};
        if(!Inverse(reference,inverse) || !Inverse(eye,inverseEye)) return false;
        Frame desired[4]{};
        for(int i=0;i<4;++i) desired[i]=Multiply(inverse,pose[joint[i]]);
        const bool fresh=!valid || now<time || (now-time>.25 && !mountPending);
        // Keep contacts AND leg geometry: solving from fresh head-driven legs
        // every frame reintroduces knee jitter even if the feet are filtered.
        const bool hold=plant && idle && !grip && !recovering && !mountPending && !lastGrip;
        if(fresh || !hold) { planted=standingValid=false;contactsValid=false; }
        // STOP may arrive before the sprint/jump palette finishes blending.
        // Only native animation contacts can invalidate the standing latch;
        // tracked eye movement must never feed this comparison.
        if(hold && contacts && contactsValid &&
           (Length(Sub(contacts[0],nativeContacts[0]))>.01f ||
            Length(Sub(contacts[1],nativeContacts[1]))>.01f)) planted=standingValid=false;
        if(hold) {
            if(!planted) {
                for(int j=0;j<7;++j) stance[j]=Multiply(inverse,pose[j]);
                if(contacts) { nativeContacts[0]=contacts[0];nativeContacts[1]=contacts[1];contactsValid=true; }
                planted=true;
            }
            const float support=StandingPostureWeight(postureDrop);
            if(!standingValid && support==1) { standingHip=desired[0];standingValid=true; }
            if(standingValid) {
                const Vec before=desired[0].origin;
                const Vec lean=Flat(Sub(before,standingHip.origin));
                const float distance=Length(lean);
                // The follower and neck already provide roomscale slack.
                // This last stage rejects only residual pose noise; another
                // large lean radius here leaves the hips behind real steps.
                const Vec excess=distance>.02f ? Scale(lean,(distance-.02f)/distance) : Vec{};
                desired[0].origin.x=standingHip.origin.x+excess.x;
                desired[0].origin.z=standingHip.origin.z+excess.z;
                desired[0].origin.y+=(standingHip.origin.y-desired[0].origin.y)*support;
                desired[0].basis=standingHip.basis;
                desired[1].origin=Add(desired[1].origin,Sub(desired[0].origin,before));
            }
            desired[2]=stance[3];desired[3]=stance[6];
        }
        if(fresh) { valid=true;serial=frame;time=now;for(int i=0;i<4;++i) cached[i]=desired[i]; }
        // Simulation can finish a mount between two draws of one tracking
        // sample. Do not swallow that transition by updating lastGrip below
        // without first transporting the cached pose into the new frame.
        if(!fresh && (serial!=frame || grip!=lastGrip || (!grip && mountPending))) {
            const double elapsed=now-time;
            float dt=float(std::clamp(elapsed,0.,.05));time=now;serial=frame;
            if(elapsed>0 && elapsed<=.05) cadence=dt;
            if(!grip && (mountPending || lastGrip)) {
                for(int i=0;i<4;++i) cached[i]=Multiply(inverse,Multiply(eye,eyeRelative[i]));
                recovering=true;mountPending=false;
                dt=std::min(dt,cadence);
            }
            bool settled=true;
            for(int i=0;i<4;++i) {
                if(grip || (!idle && !recovering)) { cached[i]=desired[i];continue; }
                const Vec delta=Sub(desired[i].origin,cached[i].origin);
                const float distance=Length(delta),dead=recovering ? 0.f : .003f;
                if(distance>dead) cached[i].origin=Add(cached[i].origin,
                    Scale(delta,std::min((distance-dead)*(1-std::exp(-12*dt)),1.2f*dt)/distance));
                Frame invCached{};Inverse({cached[i].basis,{}},invCached);
                const Basis deltaRotation=Multiply(desired[i].basis,invCached.basis);
                const auto q=RotationOf(deltaRotation);
                const float angle=2*std::atan2(Length(q.v),std::fabs(q.w));
                const float angularDead=recovering ? 0.f : .0174533f;
                if(angle>angularDead) {
                    // Follow only the excess, as the positional deadzone does.
                    // Applying the WHOLE angle at the boundary produces a
                    // repeated ~0.2-degree pulse from tiny idle animation.
                    const Basis change=LimitRotation(deltaRotation,std::min((angle-angularDead)*(1-std::exp(-12*dt)),4.f*dt));
                    cached[i].basis=Multiply(change,cached[i].basis);
                }
                const float oldScale=Length(Transform(cached[i].basis,{1,0,0}));
                const float newScale=Length(Transform(desired[i].basis,{1,0,0}));
                if(oldScale>1.e-5f) for(auto& row:cached[i].basis.r) for(auto& v:row) v*=newScale/oldScale;
                if(Length(Sub(desired[i].origin,cached[i].origin))>.003f || angle>.02f) settled=false;
            }
            if(recovering && settled) recovering=false;
        }
        lastGrip=grip;
        if(grip) {
            for(int i=0;i<4;++i) { cached[i]=desired[i];eyeRelative[i]=Multiply(inverseEye,pose[joint[i]]); }
            return true;
        }
        Frame target[4]{},legReference[7]{};
        for(int i=0;i<4;++i) target[i]=Multiply(reference,cached[i]);
        if(planted) {
            for(int j=0;j<7;++j) legReference[j]=Multiply(reference,stance[j]);
            target[2]=legReference[3];target[3]=legReference[6];
        }
        if(!ReposeTorso(pose,target,forward,planted ? legReference : nullptr)) return false;
        for(int i=0;i<4;++i) eyeRelative[i]=Multiply(inverseEye,pose[joint[i]]);
        return true;
    }
};
struct AutoBodyCalibration {
    int level=-1,water=-1;
    bool pending=false,waiting=false;
    double since=0;
    Vec sample{};
    bool Observe(int nextLevel,int nextWater) {
        const bool enteredWater=(nextWater==1 || nextWater==2 || nextWater==4) && water!=1 && water!=2 && water!=4;
        const bool changed=nextLevel!=level || enteredWater;
        level=nextLevel;water=nextWater;
        if(changed) { pending=true;waiting=false; }
        return changed;
    }
    bool Ready(bool standing,Vec head,double now) {
        if(!pending) return false;
        if(!standing || !std::isfinite(now) || !Finite({IdentityBasis(),head})) { waiting=false;return false; }
        if(!waiting || now<since || Length(Sub(head,sample))>.025f) {
            waiting=true;since=now;sample=head;return false;
        }
        if(now-since<.25) return false;
        pending=waiting=false;return true;
    }
};
} // namespace tr::firstperson
