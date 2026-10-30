#include "../src/BodyFollower.h"
#include "../src/PistolAnimationBlend.h"
#include "../src/PhysicalHolster.h"
#include <cstdio>
#include <cstdlib>
int checks=0;
void Check(bool ok,const char* what) { ++checks;if(!ok) { std::printf("FAIL: %s\n",what);std::exit(1); } }
namespace {
void TestBodyFollower() {
    using namespace tr::firstperson;
    BodyFollowSettings settings;
    AutoBodyCalibration autoCal;
    Check(autoCal.Observe(1,0) && autoCal.pending,"new level requests fresh body calibration");
    Check(!autoCal.Ready(false,{},0) && !autoCal.Ready(true,{},1) && !autoCal.Ready(true,{},1.2) &&
          autoCal.Ready(true,{},1.26),"calibration waits for settled standing");
    Check(autoCal.Observe(1,1) && !autoCal.Observe(1,2),"water entry invalidates calibration only once");
    Check(!autoCal.Ready(false,{},2) && !autoCal.Ready(true,{},3) &&
          !autoCal.Ready(true,{.04f,0,0},3.2) && !autoCal.Ready(true,{.04f,0,0},3.3) &&
          autoCal.Ready(true,{.04f,0,0},3.46),"water return waits for steady dry-ground tracking");
    Check(autoCal.Observe(2,0),"reused player identity still recalibrates on level change");
    for(float hz:{30.f,72.f,90.f,144.f}) {
        BodyFollower body;double now=1;uint64_t serial=1;
        body.Update(serial,now,0,0,0,{},{},false,settings);
        for(int frame=0;frame<int(hz);++frame) {
            now+=1/hz;body.Update(++serial,now,0,.7f,.7f,{.025f,0,0},{},false,settings);
            Check(std::fabs(body.yaw)<.00001f && Length(body.root)<.00001f,
                  "head turns below 45 degrees and subtle translation leave pelvis state planted");
        }
        for(int frame=0;frame<int(hz*2);++frame) {
            now+=1/hz;const auto old=body;
            body.Update(++serial,now,0,1.6f,1.6f,{.4f,0,0},{},false,settings);
            Check(std::fabs(BodyAngle(1.6f-body.yaw)-settings.yawSlack)<.00001f,
                  "physical yaw outside slack is resolved in the same sample without delayed catch-up");
            Check(Length(Sub(body.root,old.root))<=settings.followSpeed/hz+.00001f,
                  "pelvis position catch-up has a bounded speed at every refresh rate");
            const auto once=body;
            body.Update(serial,now+.001,0,1.6f,1.6f,{.4f,0,0},{},false,settings);
            Check(Length(Sub(body.root,once.root))<.000001f && body.yaw==once.yaw,
                  "a second eye or holster pass cannot advance the follower again");
        }
        Check(std::fabs(BodyAngle(1.6f-body.yaw)-settings.yawSlack)<.003f &&
              std::fabs(body.root.x-.38f)<.003f,"visual catch-up settles under the head with two-centimetre residual slack");
        const float before=body.yaw;
        body.Update(serial,now+.001,.8f,2.4f,2.4f,{.4f,0,0},{},false,settings);
        Check(std::fabs(BodyAngle(body.yaw-before-.8f))<.00001f,"stick turn rotates body and headset frame together immediately");
        for(int frame=0;frame<int(hz*2);++frame) {
            now+=1/hz;body.Update(++serial,now,.8f,2.4f,-.4f,{.4f,0,0},{},true,settings);
        }
        Check(std::fabs(BodyAngle(body.yaw+.4f))<.01f,"locomotion facing overrides idle head yaw slack");
        body.Update(++serial,now+1,.8f,2.4f,2.4f,{8,0,0},{8,0,0},false,settings);
        Check(body.root.x==8 && body.yaw==2.4f,"tracking gaps and teleports reset rather than drag yesterday's body through the level");
    }
    BodyFollower wrap;
    wrap.Update(1,1,0,3.1f,3.1f,{},{},false,settings);
    wrap.Update(2,1.01,0,-3.1f,-3.1f,{},{},false,settings);
    Check(wrap.yaw==3.1f,"head crossing pi retains the short-angle deadzone");
    for(float direction:{-1.f,1.f}) {
        BodyFollower turner;
        turner.Update(1,1,0,0,0,{0,0,-.3f},{},false,settings);
        for(int n=1;n<=180;++n) {
            const float head=direction*n*.01745329252f;
            const Vec goal=YawVector({0,0,-.3f},turner.yaw);
            turner.Update(n+1,1+n/90.,0,head,head,goal,{},false,settings,{},false,true);
            Check(std::fabs(BodyAngle(head-turner.yaw))<=settings.yawSlack+.00001f,
                  "rapid roomscale turns remain inside angular slack every sample");
            Check(Length(Sub(turner.root,YawVector({0,0,-.3f},turner.yaw)))<.00001f,
                  "roomscale turn carries the footprint around the eye in the same sample");
        }
        const auto stopped=turner;
        turner.Update(182,3.01,0,direction*3.14159265f,direction*3.14159265f,
            turner.root,{},false,settings,{},false,true);
        Check(std::fabs(BodyAngle(turner.yaw-stopped.yaw))<.00001f && Length(Sub(turner.root,stopped.root))<.00001f,
              "body does not continue rotating or sliding after physical turn stops");
    }
    BodyFollower consumed;
    consumed.Update(1,1,0,0,0,{.3f,0,0},{},false,settings);
    consumed.Update(2,1.02,0,0,0,{.3f,0,0},{.1f,0,0},false,settings);
    Check(std::fabs(consumed.root.x-.3f)<.00001f,
          "native root consuming tracked displacement cannot translate the rendered body twice");
    BodyFollower pivot;
    pivot.Update(1,1,0,0,0,{0,0,-.3f},{},false,settings);
    pivot.Update(1,1.01,1.570796327f,1.570796327f,1.570796327f,{0,0,-.3f},{},false,settings,{});
    Check(std::fabs(pivot.root.x+.3f)<.00001f && std::fabs(pivot.root.z)<.00001f,
          "artificial turn rotates the body footprint around the headset, not around the hip root");
    RoomscaleFollower room;
    Check(Length(room.Request({.14f,0,0},.033f,settings))==0,"leaning inside the physical radius cannot move native collision root");
    const auto step=room.Request({.3f,0,0},.033f,settings);
    Check(step.x>0 && step.x<=.9f*.033f+.00001f,"physical room-scale request is speed limited before collision");
    Check(room.Request({.12f,0,0},.033f,settings).x>0,"room-scale catch-up continues through hysteresis band");
    Check(Length(room.Request({.089f,0,0},.033f,settings))==0 && !room.following,"room-scale catch-up stops inside release radius");
    Check(Length(room.Request({3,0,0},.033f,settings))==0,"tracking teleports cannot generate collision movement");
    HeightCrouch crouch;
    Check(!crouch.Update(true,.34f) && crouch.Update(true,.36f) && crouch.Update(true,.28f) &&
          !crouch.Update(true,.22f) && !crouch.Update(false,.5f),"native crouch has separate enter/release thresholds and an opt-out");
    Check(HandsSupportFacing({-.2f,0,.4f},{.2f,0,.4f},{0,0,1},{0,0,1},0),
          "two stationary forward controllers support body facing independent of gaze");
    Check(!HandsSupportFacing({-.2f,0,-.1f},{.2f,0,-.1f},{0,0,1},{0,0,1},0) &&
          !HandsSupportFacing({-.2f,0,.4f},{.2f,0,.4f},{1,0,0},{0,0,1},0),
          "hands behind the body or aiming apart do not lock its facing");
    BodyFollower supported;
    supported.Update(1,1,0,0,0,{},{},false,settings);
    for(int n=1;n<=180;++n) {
        supported.Update(n+1,1+n/90.,0,1.1f,1.1f,{.02f,0,0},{},false,settings,{},true);
        Check(supported.yaw==0 && Length(supported.root)==0,
              "looking aside with both hands forward cannot rotate or move the body");
    }
    supported.Update(182,3.01,0,1.1f,1.1f,{},{},true,settings,{},true);
    Check(supported.yaw>0,"manual locomotion still overrides controller-supported facing");

    Frame native[15]{};
    const Vec p[15]={{0,-500,0},{-100,-500,0},{-100,-260,70},{-100,0,0},
        {100,-500,0},{100,-260,70},{100,0,0},{0,-650,0},
        {180,-700,0},{240,-460,30},{180,-250,70},{-180,-700,0},{-240,-460,30},{-180,-250,70},{0,-850,0}};
    for(int i=0;i<15;++i) native[i]={IdentityBasis(),p[i]};
    // A deadzone must be continuous at its boundary. Idle animation hovering
    // near one degree used to apply the entire angle in a single filter step.
    for(float hz:{30.f,60.f,90.f,144.f}) {
        TorsoPoseFilter filter;
        Frame pose[15]{};
        const Frame reference{IdentityBasis(),{}},eye{IdentityBasis(),{0,-.85f,0}};
        for(int j=0;j<15;++j) { pose[j]=native[j];pose[j].origin=Scale(pose[j].origin,.001f); }
        Check(filter.Update(1,1,reference,eye,true,false,pose,{0,0,1},true),
              "idle angular boundary fixture acquires standing pose");
        Basis previous=pose[7].basis;
        for(int n=1;n<=int(hz*2);++n) {
            for(int j=0;j<15;++j) { pose[j]=native[j];pose[j].origin=Scale(pose[j].origin,.001f); }
            const float angle=(1.f+.01f*std::sin(n*.7f))*.0174532925f;
            pose[7].basis=RotationMatrix(AxisRotation({0,1,0},angle));
            Check(filter.Update(n+1,1+n/double(hz),reference,eye,true,false,pose,{0,0,1},true),
                  "native idle rotation straddling deadzone solves");
            const auto q=RotationProduct(RotationOf(pose[7].basis),RotationInverse(RotationOf(previous)));
            Check(2*std::atan2(Length(q.v),std::fabs(q.w))<.0002f,
                  "tiny motion over angular deadzone cannot produce whole-angle torso pulse");
            previous=pose[7].basis;
        }
    }
    for(float hz:{30.f,72.f,90.f,144.f}) {
        Frame rest[15]{};
        Check(SolveFollowingBody(native,p[14],p[14],0,IdentityBasis(),{0,0,1},1000,{},0,true,0,0,rest,1),
              "standing-contact fixture begins with extended legs");
        for(auto& joint:rest) joint.origin=Scale(joint.origin,.001f);
        TorsoPoseFilter filter;
        const Frame reference{IdentityBasis(),{}},eye{IdentityBasis(),{0,-.85f,0}};
        Frame pose[15]{};for(int j=0;j<15;++j) pose[j]=rest[j];
        Check(filter.Update(1,1,reference,eye,true,false,pose,{0,0,1},true),"standing contacts acquire");
        Frame initial[15]{};for(int j=0;j<15;++j) initial[j]=pose[j];
        uint64_t serial=1;double now=1;
        for(int n=0;n<int(hz);++n) {
            for(int j=0;j<15;++j) pose[j]=rest[j];
            const float noise=std::sin(n*.47f)*.02f;
            for(int j=0;j<7;++j) pose[j].origin.x+=noise;
            pose[0].origin.y+=.02f+noise;pose[7].origin.y+=.02f+noise;
            pose[14].origin.y+=noise;now+=1/hz;
            Check(filter.Update(++serial,now,reference,eye,true,false,pose,{0,0,1},true,.065f+noise),
                  "planted stance accepts native idle and head noise at every refresh rate");
            for(int j=0;j<7;++j) Check(Length(Sub(pose[j].origin,initial[j].origin))<.0001f,
                  "standing pelvis, thighs, knees and ankles stay fixed without tracking the head");
        }
        for(int n=0;n<int(hz);++n) {
            for(int j=0;j<15;++j) pose[j]=rest[j];
            pose[0].origin.y+=.20f;pose[7].origin.y+=.20f;
            pose[14].origin.y+=.20f;now+=1/hz;
            Check(filter.Update(++serial,now,reference,eye,true,false,pose,{0,0,1},true,.20f),
                  "real physical crouch releases the standing hip hold");
            for(int j:{3,6}) Check(Length(Sub(pose[j].origin,initial[j].origin))<.00001f,
                  "physical crouch keeps feet on their original floor contacts");
            for(int first:{1,4}) for(int j=0;j<2;++j)
                Check(std::fabs(Length(Sub(pose[first+j+1].origin,pose[first+j].origin))-
                                Length(Sub(rest[first+j+1].origin,rest[first+j].origin)))<.00001f,
                      "planted crouch preserves thigh and shin lengths");
        }
        Check(KneeBendDegrees(pose,1)>30 && KneeBendDegrees(pose,4)>30,
              "standing contact lock does not prevent deliberate crouching");
        for(int n=0;n<int(hz);++n) {
            for(int j=0;j<15;++j) pose[j]=rest[j];
            pose[0].origin.x+=.15f;pose[7].origin.x+=.15f;now+=1/hz;
            Check(filter.Update(++serial,now,reference,eye,true,false,pose,{0,0,1},true),
                  "large lean moves the pelvis while feet remain planted");
            for(int j:{3,6}) Check(Length(Sub(pose[j].origin,initial[j].origin))<.00001f,
                  "lean reach limits lower hips instead of pulling floor contacts around");
        }
        Check(pose[0].origin.x>initial[0].origin.x+.04f,"deliberate lean exhausts stationary hip slack");
        for(int j=0;j<15;++j) pose[j]=rest[j];
        pose[3].origin.z+=.1f;now+=1/hz;
        Check(filter.Update(++serial,now,reference,eye,false,false,pose,{0,0,1},false) && !filter.planted,
              "locomotion, body catch-up and airborne poses release contacts immediately");
        Check(pose[3].origin.z>.09f,"foot can swing again after leaving stationary state");
        filter.Mount();Vec last=pose[7].origin;now+=1;
        for(int n=0;n<int(hz);++n) {
            for(int j=0;j<15;++j) pose[j]=rest[j];
            pose[0].origin.z+=.24f;pose[7].origin.z+=.24f;now+=1/hz;
            Check(filter.Update(++serial,now,reference,eye,true,false,pose,{0,0,1},true),
                  "mount recovery survives animation gaps before replanting");
            Check(Length(Sub(pose[7].origin,last))<=1.2f/hz+.0001f,
                  "mount-to-standing torso cannot pop on first or subsequent frame");
            last=pose[7].origin;
        }
        Check(std::fabs(pose[7].origin.z-.24f)<.004f,"mount recovery settles to new pose");
    }
    // STOP can be reported while both feet still belong to a jump/sprint
    // palette. They must not become permanent airborne standing contacts.
    for(float lift:{.08f,.18f,.3f}) {
        TorsoPoseFilter filter;Frame pose[15]{};
        const Frame reference{IdentityBasis(),{}},eye{IdentityBasis(),{0,-.85f,0}};
        Vec contacts[2]={{-.1f,-lift,0},{.1f,-lift,0}};
        for(int j=0;j<15;++j) { pose[j]=native[j];pose[j].origin=Scale(pose[j].origin,.001f);pose[j].origin.y-=lift; }
        Check(filter.Update(1,1,reference,eye,true,false,pose,{0,0,1},true,0,contacts),"transient airborne standing fixture solves");
        contacts[0].y=contacts[1].y=0;
        for(int n=0;n<180;++n) {
            for(int j=0;j<15;++j) { pose[j]=native[j];pose[j].origin=Scale(pose[j].origin,.001f); }
            // The first correct palette can share the previous pose serial.
            Check(filter.Update(n+1,1+n/90.,reference,eye,true,false,pose,{0,0,1},true,0,contacts),
                  "standing contacts refresh when native sprint or directional-jump feet settle");
        }
        Check(std::fabs(pose[3].origin.y)<.0001f && std::fabs(pose[6].origin.y)<.0001f,
              "settled native feet cannot remain latched in the air after sprint or jump");
    }
    for(float lean:{-.12f,0.f,.12f}) for(float drop:{0.f,.03f,.15f,.3f}) for(float twist:{-.7f,0.f,.7f}) {
        Frame posed[15]{};
        const Basis head=RotationMatrix(AxisRotation({0,1,0},twist));
        const Vec eye=Add(p[14],{lean*1000,drop*1000,0});
        Check(SolveFollowingBody(native,p[14],eye,0,head,{0,0,1},1000,{},BodyChestTwist(twist,false),true,0,0,posed),
              "lazy-body pose solves standing, leaning, crouching and independent head turns");
        Check(Length(Sub(posed[14].origin,eye))<.01f,"head remains a hard positional constraint while pelvis lags");
        if(lean==0 && drop==0 && twist!=0) {
            const auto front=Transform(posed[7].basis,{0,0,1});
            const float chestYaw=std::atan2(front.x,front.z);
            Check(std::fabs(chestYaw)>.01f && std::fabs(chestYaw)<std::fabs(twist) &&
                  std::fabs(posed[0].basis.r[0][2])<.00001f,
                  "torso takes part of the head twist while pelvis facing stays fixed");
        }
        for(int foot:{3,6}) {
            const float drift=Length(Sub(posed[foot].origin,p[foot]));
            if(drift>=.05f) std::printf("foot drift %.3f: lean %.2f drop %.2f twist %.2f foot %d\n",drift,lean,drop,twist,foot);
            Check(drift<.05f,"moderate physical crouch and lean retain both planted foot contacts");
        }
        for(int first:{1,4}) for(int j=0;j<2;++j) {
            Check(std::fabs(Length(Sub(posed[first+j+1].origin,posed[first+j].origin))-
                            Length(Sub(native[first+j+1].origin,native[first+j].origin)))<.03f,
                  "lazy-body leg IK never stretches thighs or shins");
            Frame inverse{};Inverse(native[first+j],inverse);
            Check(Length(Sub(Transform(Multiply(posed[first+j],inverse),native[first+j+1].origin),posed[first+j+1].origin))<.03f,
                  "pelvis bend and planted-foot corrections keep leg skinning connected");
        }
        Check(std::fabs(Length(Sub(posed[7].origin,posed[0].origin))-150)<.03f &&
              std::fabs(Length(Sub(posed[14].origin,posed[7].origin))-200)<41,
              "torso retains native length and sparse neck absorbs bounded positional slack");
        if(drop>=.15f) Check(posed[0].origin.y>p[0].y+drop*500,"moderate headset lowering lowers pelvis");
    }
    // Real eye clearance, curved native spine and nonidentity head axes: the
    // old arm-like spine solve changed neutral chest orientation in this pose.
    Frame curved[15];for(int j=0;j<15;++j) curved[j]=native[j];
    curved[7].origin.z=-60;
    curved[14].basis=RotationMatrix(AxisRotation({1,0,0},.15f));
    const Vec eyeLocal{0,-32,140},anchor=Transform(curved[14],eyeLocal);
    for(float yaw:{-.5f,-.2f,0.f,.2f,.5f}) for(float pitch:{-.2f,0.f,.2f}) {
        const Basis look=Multiply(RotationMatrix(AxisRotation({0,1,0},yaw)),
                                  RotationMatrix(AxisRotation({1,0,0},pitch)));
        for(float jitter:{-.01f,0.f,.01f}) {
            const Vec eye=Add(anchor,{jitter*1000,jitter*500,0});Frame posed[15]{};
            Check(SolveFollowingBody(curved,anchor,eye,0,look,{0,0,1},1000,{},BodyChestTwist(yaw,true),true,0,0,posed),
                  "real eye offset and curved authored spine solve through small head movements");
            Check(Length(Sub(Transform(posed[14],eyeLocal),eye))<.01f,
                  "actual local eye stays exactly on camera through yaw pitch and translation");
            for(int j:{0,7}) {
                Check(Length(Sub(posed[j].origin,curved[j].origin))<.01f,
                      "stationary forward hands and small head motion leave hip and chest positions fixed");
                for(int r=0;r<3;++r) for(int c=0;c<3;++c)
                    Check(std::fabs(posed[j].basis.r[r][c]-curved[j].basis.r[r][c])<.0001f,
                          "small head motion cannot fold or rotate the chest");
            }
            Check(posed[7].origin.y-eye.y>100,"chest stays below the camera instead of folding into eye height");
        }
    }
    Frame stepping[15]{};
    Check(SolveFollowingBody(native,p[14],p[14],0,IdentityBasis(),{0,0,1},1000,{},0,true,1.570796327f,1,stepping) &&
          stepping[3].origin.y<p[3].y-20 && std::fabs(stepping[6].origin.y-p[6].y)<.05f,
          "catch-up gait lifts one inferred foot while the opposite foot remains grounded");
    for(float height:{650.f,750.f,950.f}) {
        const Vec eyeLocal{0,-32,140},anchor=Transform(native[14],eyeLocal),eye{0,-height,140};
        BodySizeCalibration size;
        Check(size.Capture(native,anchor,eye.y,423),"standing size fits the available eye-to-foot height");
        const float captured=size.factor;
        Check(size.Capture(native,anchor,eye.y+200,423) && size.factor==captured,
              "later crouching cannot recalculate or shrink calibrated body size");
        Frame scaled[15]{},posed[15]{},unscaled[15]{};ScaleBody(native,size.factor,scaled);
        const Vec sizedAnchor=Transform(scaled[14],eyeLocal);
        Check(SolveFollowingBody(scaled,sizedAnchor,eye,0,IdentityBasis(),{0,0,1},423,{},0,true,0,0,posed,1),
              "scaled standing body reaches eye with feet on the floor");
        Check(Length(Sub(Transform(posed[14],eyeLocal),eye))<.01f,"resizing keeps the configured eye exactly at the camera");
        Check(SolveFollowingBody(native,anchor,eye,0,IdentityBasis(),{0,0,1},423,{},0,true,0,0,unscaled),
              "unscaled mismatch fixture solves for comparison");
        for(int first:{1,4}) {
            const float leg=Length(Sub(scaled[first+1].origin,scaled[first].origin))+Length(Sub(scaled[first+2].origin,scaled[first+1].origin));
            Check(Length(Sub(posed[first+2].origin,posed[first].origin))/leg>.999f && KneeBendDegrees(posed,first)<2,
                  "upright knees stay within two degrees of straight rather than accepting a visible resting bend");
            Check(std::fabs(posed[first+2].origin.y)<.02f,"size calibration keeps both feet at native floor level");
            if(height==650) Check(Length(Sub(unscaled[first+2].origin,unscaled[first].origin))/
                (Length(Sub(native[first+1].origin,native[first].origin))+Length(Sub(native[first+2].origin,native[first+1].origin)))<.7f,
                "fixture reproduces the permanent deep knee bend of the previous unscaled solver");
        }
        Frame crouchedPose[15]{};
        Check(SolveFollowingBody(scaled,sizedAnchor,Add(eye,{0,100,0}),0,IdentityBasis(),{0,0,1},423,{},0,true,0,0,crouchedPose),
              "physical crouching still lowers the calibrated body");
        Check(crouchedPose[0].origin.y>posed[0].origin.y+80 && std::fabs(crouchedPose[3].origin.y)<.02f &&
              std::fabs(crouchedPose[6].origin.y)<.02f,"fixed body proportions allow real knee bending with planted feet");
        Check(KneeBendDegrees(crouchedPose,1)>30 && KneeBendDegrees(crouchedPose,4)>30,
              "upright correction releases for real physical crouching");
        for(float drop:{-5.f,0.f,5.f,10.f}) {
            Frame upright[15]{};
            Check(SolveFollowingBody(scaled,sizedAnchor,Add(eye,{0,drop,0}),0,IdentityBasis(),{0,0,1},423,{},0,true,0,0,upright,1),
                  "upright body handles ordinary standing head-height variation");
            Check(KneeBendDegrees(upright,1)<2 && KneeBendDegrees(upright,4)<2 &&
                  std::fabs(upright[0].origin.y-posed[0].origin.y)<.02f,
                  "neck allowance absorbs small head-height changes without rebending the knees");
            Check(Length(Sub(Transform(upright[14],eyeLocal),Add(eye,{0,drop,0})))<.02f,
                  "straightening standing legs never displaces the tracked eye");
        }
        for(int j=0;j<15;++j) for(int r=0;r<3;++r)
            Check(std::fabs(scaled[j].basis.r[r][r]-size.factor)<.0001f,
                  "body fitting scales mesh geometry as well as bone positions");
    }
    Frame stride[15]{},smallStride[15]{};for(int j=0;j<15;++j) stride[j]=native[j];
    stride[3].origin.y-=80;
    ScaleBody(stride,.7f,smallStride);
    Check(smallStride[6].origin.y==native[6].origin.y && smallStride[3].origin.y<0,
          "raising the swing foot cannot lift the planted foot during body scaling");
}
} // namespace

namespace {
void TestPistolAnimationBlend() {
    using namespace tr::firstperson;
    const auto make=[](Frame (&pose)[15],float angle,float travel) {
        for(auto& joint:pose) joint={IdentityBasis(),{travel,0,0}};
        for(int first:{8,11}) {
            pose[first]={RotationMatrix(AxisRotation({1,0,0},angle)),{travel+(first==8 ? 180.f : -180.f),0,0}};
            pose[first+1]={pose[first].basis,Transform(pose[first],{0,200,0})};
            pose[first+2]={pose[first].basis,Transform(pose[first+1],{0,180,0})};
        }
    };
    for(int hz:{60,90,120}) for(int animation:{2,3}) {
        PistolAnimationBlend blend;
        Frame pose[15]{};make(pose,0,0);blend.Apply(&blend,0,1,1,pose);
        float oldWristZ=pose[10].origin.z;
        double now=1.01;uint64_t serial=2;
        for(int phase=0;phase<2;++phase) {
            const int status=phase ? (animation==2 ? 4 : 0) : animation;
            const float targetAngle=phase ? -.6f : 1.4f;
            const double start=now;
            for(int tick=0;tick<=hz/4;++tick,++serial) {
                now=start+tick/double(hz);
                make(pose,targetAngle,float(tick*20));
                const auto desiredWrist=pose[10];
                blend.Apply(&blend,status,serial,now,pose);
                Check(std::fabs(pose[10].origin.z-oldWristZ)<2.1f*380*1.5f/(hz*.16f)+1,
                      "pistol animation handoff has no one-frame wrist snap at 60/90/120 Hz");
                if(tick==0) Check(std::fabs(pose[10].origin.z-oldWristZ)<.01f,
                                  "draw/holster and return to controllers start at last rendered hand pose");
                for(int first:{8,11}) for(int j=0;j<2;++j)
                    Check(std::fabs(Length(Sub(pose[first+j+1].origin,pose[first+j].origin))-(j ? 180 : 200))<.02f,
                          "smooth pistol handoff preserves connected arm bone lengths");
                const auto rendered=pose[10];oldWristZ=rendered.origin.z;
                make(pose,targetAngle,float(tick*20));
                blend.Apply(&blend,status,serial,now,pose);
                Check(Length(Sub(rendered.origin,pose[10].origin))<.01f,
                      "repeated stereo/attachment draws cannot advance the handoff");
                if(tick/double(hz)>=.16) Check(!blend.blending && Length(Sub(pose[10].origin,desiredWrist.origin))<.01f,
                    "handoff finishes in 160 ms and restores exact native or tracked target");
            }
            now+=.01;
        }
        make(pose,1,1000);blend.Apply(nullptr,2,++serial,now+.01,pose);
        Check(!blend.blending,"changed character cannot inherit old pistol handoff");
        blend.Reset();Check(!blend.valid,"session reset clears pistol handoff");
    }
}
} // namespace

void TestCollectionHolsters() {
    using tr::motiongun::HolsterInput;
    for(int game:{1,2,3}) for(int gun=1;gun<=(game==1?4:game==2?7:8);++gun) {
        const bool dual=gun<=3 && !(game==3 && gun==2);
        for(int hand:{0,1}) {
            HolsterInput h;h.game=game;
            if(!dual && hand==0) { Check(!h.Poll(hand,true,true,2,gun,.04f,0,gun),"single weapon rejects left pickup");continue; }
            const int slot=gun>=4?2:hand;
            Check(h.Hover(hand,slot,gun,.29f,0,gun),"every native weapon has a 30 cm grab zone");
            Check(h.Poll(hand,true,true,slot,gun,.04f,0,gun) && !h.active,"grip claims holster without drawing from jitter");
            h.Poll(hand,true,true,slot,gun,.4f,0,gun);
            Check(h.mask==(1u<<hand) && h.request,"withdrawal equips exactly the claimed hand");
            Check(!h.CanFire(hand,true),"withdrawal cannot fire held trigger");
            h.Poll(hand,false,true,slot,gun,.4f,4,gun);
            Check(!h.CanFire(hand,true) && h.CanFire(hand,false) && h.CanFire(hand,true),"release trigger after transfer enables fresh firing");
            Check(!h.CanFire(1-hand,true),"unoccupied hand cannot fire");
            h.request=false;h.Poll(hand,true,true,slot,gun,.04f,4,gun);
            h.Poll(hand,false,true,slot,gun,.04f,4,gun);
            Check(!h.mask && h.request,"returning last gun requests native holstering");
            h.Reset();h.game=game;
            h.Poll(hand,true,true,slot,gun,.04f,0,gun);h.Poll(hand,true,false,slot,gun,.4f,0,gun);
            h.Poll(hand,true,true,slot,gun,.4f,0,gun);
            Check(!h.active,"tracking loss cancels unfinished withdrawal until fresh grip");
        }
        if(dual) {
            HolsterInput h;h.game=game;
            h.Poll(0,true,true,0,gun,.04f,4,gun);h.Poll(0,false,true,0,gun,.04f,4,gun);
            Check(h.mask==2 && !h.request,"physical stow of button-drawn pair leaves right gun armed");
        }
    }
}
int main() { TestBodyFollower();TestPistolAnimationBlend();TestCollectionHolsters();
    std::printf("PASS: %d body, calibration, pistol blend and collection holster checks\n",checks); }
