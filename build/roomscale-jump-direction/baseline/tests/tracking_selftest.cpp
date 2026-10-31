// Real VRSystem eye transforms, independent of the input-hook stubs.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <openvr.h>
#include "../src/StereoMath.h"
#include "../src/BodyFollower.h"
#define private public
#include "../src/VRSystem.h"
#undef private
#include "../src/VRSystem.cpp"
namespace tr {
int CurrentGame() { return -1; }
int32_t AppFlag(uint32_t) { return 0; }
const GameDllLayout* GameDllBound() { return nullptr; }
uint64_t GameDllBase() { return 0; }
void WorldInventoryRecenter() {}
Config testConfig;
bool testFirstPerson=false;
const Config& Cfg() { return testConfig; }
bool FirstPersonActive() { return testFirstPerson; }
float testOriginYaw=0;
int testOriginChanges=0;
void FirstPersonTrackingOriginChanged(float yaw) {
    testOriginYaw=yaw;++testOriginChanges;VR().RecenterHead();
}
bool CameraHeadroom(float&) { return false; }
float LiveWorldUnitsPerMetre() { return 423; }
float LiveIpdScale() { return 1; }
}
int checks=0;
void Near(double got,double expected,const char* what,double tolerance=.0002) {
    ++checks;
    if (!std::isfinite(got) || std::fabs(got-expected)>tolerance) {
        std::fprintf(stderr,"FAIL: %s: got %.9f expected %.9f\n",what,got,expected);
        std::exit(1);
    }
}
int main() {
    using namespace tr;
    auto& vr=VR();
    testConfig.positionalTracking=true;
    testConfig.firstPersonHeadTranslation=true;
    testConfig.firstPersonMovementStabilization=true;
    testConfig.flipViewY=testConfig.flipProjectionY=true;
    vr.m_system=reinterpret_cast<vr::IVRSystem*>(1);
    vr.m_poseValid=vr.m_haveNeutral=true;
    const double signs[3]={1,-1,1};
    for (bool fp:{false,true}) for (float neck:{0.f,.15f,.4f})
    for (int yaw=-170;yaw<=170;yaw+=10) for (int pitch:{-65,0,65}) for (int roll:{-40,0,40}) {
        testFirstPerson=fp;testConfig.firstPersonRoomscaleNeckMetres=neck;
        const double a=yaw*3.141592653589793/180,b=pitch*3.141592653589793/180,c=roll*3.141592653589793/180;
        const double cy=cos(a),sy=sin(a),cp=cos(b),sp=sin(b),cr=cos(c),sr=sin(c);
        // Independent head->tracking yaw/pitch/roll rotation; no production Mul/InvertRigid.
        const double r[3][3]={{cy*cr+sy*sp*sr,-cy*sr+sy*sp*cr,sy*cp},
                              {cp*sr,cp*cr,-sp},
                              {-sy*cr+cy*sp*sr,sy*sr+cy*sp*cr,cy*cp}};
        const double raw[3]={1.0+.12*sin(a),1.7+.05*sin(b),.7+.12*cos(a)};
        const float neutral[3]={1,1.7f,.82f};
        for (int i=0;i<3;++i) {
            vr.m_headPosRaw[i]=float(raw[i]);vr.m_headNeutral[i]=neutral[i];
            for (int j=0;j<3;++j) vr.m_headFromTracking.r[i][j]=float(r[j][i]);
        }
        vr.RefreshHeadTranslation();
        for (int eye=0;eye<2;++eye) {
            const double eyePosition=eye ? .031 : -.031;
            vr.m_eyeFromHead[eye]=Affine::Identity();
            vr.m_eyeFromHead[eye].r[0][3]=float(-eyePosition);
            const auto actual=vr.EyeView(eye ? Eye::Right : Eye::Left);
            for (int i=0;i<3;++i) {
                double translation=i==0 ? -eyePosition : 0;
                for (int j=0;j<3;++j) {
                    Near(actual.r[i][j],r[j][i]*signs[i]*signs[j],"rigid eye rotation");
                    translation-=r[j][i]*(double(vr.m_headPosRaw[j])-neutral[j]);
                }
                Near(actual.r[i][3],translation*signs[i]*423,"tracked eye position without neck-induced world motion");
            }
            const float tangents[4]={eye ? -.8391f : -1.3764f,eye ? 1.3764f : .8391f,-1.4281f,.9657f};
            for (int i=0;i<4;++i) vr.m_rawProj[eye][i]=tangents[i];
            mat4 p{};vr.EyeProjection(eye ? Eye::Right : Eye::Left,16,32768,p);
            Near(p.m[0],2./(tangents[1]-tangents[0]),"runtime horizontal optical scale");
            Near(p.m[5],-2./(tangents[3]-tangents[2]),"runtime vertical optical scale");
            Near(p.m[8],(tangents[1]+tangents[0])/(tangents[1]-tangents[0]),"asymmetric horizontal shear");
            Near(p.m[9],(tangents[3]+tangents[2])/(tangents[3]-tangents[2]),"asymmetric vertical shear keeps its sign");
        }
        // A controller held fixed in tracking space must not drift as the HMD rotates.
        vr.m_controllerPoseValid[0]=true;
        vr.m_controllerPose[0]={};
        vr.m_controllerPose[0].m[0][3]=1.2f;
        vr.m_controllerPose[0].m[1][3]=1.2f;
        vr.m_controllerPose[0].m[2][3]=.3f;
        float right,down,forward;
        if (!vr.FirstPersonControllerOffset(0,right,down,forward)) return 2;
        Near(right,1.2f-neutral[0],"controller right offset stays world-stable");
        Near(down,neutral[1]-1.2f,"controller height stays world-stable");
        Near(forward,neutral[2]-.3f,"controller forward offset stays world-stable");
    }
    for (int hand=0;hand<2;++hand) {
        vr.m_poseValid=vr.m_controllerPoseValid[hand]=true;
        vr.m_headPosRaw[1]=1.7f; vr.m_controllerPose[hand].m[1][3]=1.5f;
        float down=0;
        Near(vr.ControllerHeightBelowHead(hand,down),true,"gesture has tracked hand");
        Near(down,.2,"gesture uses live head-relative height");
        vr.m_headPosRaw[1]-=.3f; vr.m_controllerPose[hand].m[1][3]-=.3f;
        vr.ControllerHeightBelowHead(hand,down);
        Near(down,.2,"ducking head and hands together cannot pull");
        vr.m_controllerPose[hand].m[1][3]-=.18f;
        vr.ControllerHeightBelowHead(hand,down);
        Near(down,.38,"physical hand pull is measured in tracking metres");
        vr.m_controllerPoseValid[hand]=false;
        Near(vr.ControllerHeightBelowHead(hand,down),false,"missing hand disables gesture");
        vr.m_controllerPoseValid[hand]=true; vr.m_poseValid=false;
        Near(vr.ControllerHeightBelowHead(hand,down),false,"missing head disables gesture");
    }
    vr.m_poseValid=true;testFirstPerson=true;
    const auto setHeading=[&](float yaw,float horizontal) {
        vr.m_headFromTracking=Affine::Identity();
        vr.m_headFromTracking.r[2][0]=-std::sin(yaw)*horizontal;
        vr.m_headFromTracking.r[2][2]=std::cos(yaw)*horizontal;
    };
    vr.m_headingYawValid=vr.m_headingYawVertical=false;
    setHeading(.7f,1);Near(vr.HeadYawRadians(),.7,"reliable heading tracked");
    for(float projected:{.34f,.01f,0.f,.4f}) {
        setHeading(-2.f,projected);Near(vr.HeadYawRadians(),.7,"vertical gaze holds torso heading with hysteresis");
    }
    setHeading(-.8f,.46f);Near(vr.HeadYawRadians(),-.8,"looking up releases vertical heading hold");
    for(auto type:{vr::VREvent_SeatedZeroPoseReset,vr::VREvent_StandingZeroPoseReset}) {
        setHeading(.6f,1);vr.HeadYawRadians();
        vr::VREvent_t event{};event.eventType=type;vr.HandleRuntimeEvent(event);
        vr.HandleRuntimeEvent(event); // Coalesced reset notifications.
        vr.m_poseValid=false;vr.ApplyRuntimeRecenter();
        Near(vr.m_runtimeRecenterPending,true,"origin reset waits for valid new pose");
        vr.m_poseValid=true;setHeading(-.4f,1);
        vr.m_headPosRaw[0]=.2f;vr.m_headPosRaw[1]=1.23f;vr.m_headPosRaw[2]=.5f;
        vr.m_controllerPoseValid[0]=true;vr.m_controllerPose[0].m[1][3]=.93f;
        vr.ApplyRuntimeRecenter();
        Near(testOriginYaw,1,"origin reset compensates changed tracking yaw");
        Near(vr.HeadVerticalOffset(),0,"System reset recalibrates headset height");
        float right,down,forward;vr.FirstPersonControllerOffset(0,right,down,forward);
        Near(down,.3,"controller uses same recalibrated neutral as headset");
        const int count=testOriginChanges;vr.ApplyRuntimeRecenter();
        Near(testOriginChanges,count,"runtime reset applies once");
    }
    vr::VREvent_t dashboard{};dashboard.eventType=vr::VREvent_DashboardActivated;
    vr.HandleRuntimeEvent(dashboard);Near(vr.m_runtimeRecenterPending,false,"dashboard alone does not recalibrate height");
    testFirstPerson=false;
    for(float yaw:{-2.8f,-1.f,0.f,.6f,2.8f}) for(float pitch:{-.8f,0.f,.8f}) {
        const float c=std::cos(yaw),s=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
        vr.m_trackingFromRuntime=Affine::Identity();
        vr.m_rawHeadPose={{{c,s*sp,s*cp,1.2f},{0,cp,-sp,1.7f},{-s,c*sp,c*cp,.8f}}};
        vr.m_headFromTracking=InvertRigid(FromHmd(vr.m_rawHeadPose));
        vr.m_headingYawValid=vr.m_headingYawVertical=false;
        Affine before[2];
        for(int hand=0;hand<2;++hand) {
            vr.m_controllerPoseValid[hand]=true;
            vr.m_controllerPose[hand]={{{1,0,0,hand?.5f:-.5f},{0,1,0,1.2f},{0,0,1,.2f}}};
            before[hand]=Mul(InvertRigid(FromHmd(vr.m_rawHeadPose)),FromHmd(vr.m_controllerPose[hand]));
        }
        vr.RecenterView();
        Near(vr.HeadYawRadians(),0,"view toggle resets artificial tracking yaw");
        Near(vr.HeadVerticalOffset(),0,"view toggle captures current standing height");
        Near(vr.m_rawHeadPose.m[1][1],cp,"yaw reset retains actual headset pitch");
        for(int hand=0;hand<2;++hand) {
            const auto after=Mul(InvertRigid(FromHmd(vr.m_rawHeadPose)),FromHmd(vr.m_controllerPose[hand]));
            for(int r=0;r<3;++r) for(int col=0;col<4;++col)
                Near(after.r[r][col],before[hand].r[r][col],"view reset preserves head/controller relative pose");
        }
        vr::HmdMatrix34_t anchor{};vr.LevelHeadPose(anchor);
        Near(anchor.m[1][1],1,"inventory anchor removes head pitch");
        Near(anchor.m[1][3],1.7f,"inventory anchor stays at actual eye height");
        const auto first=vr.m_trackingFromRuntime;vr.RecenterView();
        for(int r=0;r<3;++r) for(int col=0;col<4;++col)
            Near(vr.m_trackingFromRuntime.r[r][col],first.r[r][col],"repeated view reset is idempotent");
    }
    vr.m_poseValid=false;const auto prior=vr.m_trackingFromRuntime;vr.RecenterView();
    Near(vr.m_viewRecenterPending,true,"untracked view toggle defers yaw reset");
    for(int r=0;r<3;++r) for(int col=0;col<4;++col)
        Near(vr.m_trackingFromRuntime.r[r][col],prior.r[r][col],"untracked toggle cannot mutate tracking frame");
    // Move the eyes around a stationary neck. Head pitch must move the view,
    // but cannot masquerade as pelvis lowering in the body solver.
    vr.m_poseValid=true;testFirstPerson=true;
    for(float neutralPitch:{-.25f,0.f,.25f}) {
        const auto poseAt=[&](float pitch,float crouch) {
            const float c=std::cos(pitch),s=std::sin(pitch);
            const float eyeY=1.62f+.08f*c+.12f*s-crouch;
            vr.m_rawHeadPose={{{1,0,0,0},{0,c,-s,eyeY},{0,s,c,0}}};
            vr.m_headFromTracking=InvertRigid(FromHmd(vr.m_rawHeadPose));
            vr.m_headPosRaw[0]=vr.m_headPosRaw[2]=0;vr.m_headPosRaw[1]=eyeY;
            vr.RefreshHeadTranslation();
        };
        poseAt(neutralPitch,0);vr.RecenterHead();
        Near(vr.FirstPersonPostureDrop(),0,"posture neutral captured on recenter");
        for(float pitch:{-1.5f,-1.2f,-.8f,-.4f}) for(float crouch:{0.f,.15f,.3f}) {
            poseAt(pitch,crouch);
            Near(vr.FirstPersonPostureDrop(),crouch,"neck rotation is removed from body crouch estimate");
            Near(firstperson::StandingPostureWeight(vr.FirstPersonPostureDrop()),
                firstperson::StandingPostureWeight(crouch),"looking down retains upright leg constraint");
            const auto view=vr.EyeView(Eye::Left);
            const float height=vr.FirstPersonHeightDrop();
            Near(height,vr.m_headNeutral[1]-vr.m_headPosRaw[1],"raw eye translation remains tracked");
            Near(vr.FirstPersonVerticalOffset(),height,"body fitting still knows actual rendered eye height");
            (void)vr.FirstPersonPostureDrop();
            const auto after=vr.EyeView(Eye::Left);
            for(int r=0;r<3;++r) for(int col=0;col<4;++col)
                Near(after.r[r][col],view.r[r][col],"posture compensation does not alter the view transform");
        }
    }
    vr.m_poseValid=false;Near(vr.FirstPersonPostureDrop(),0,"lost tracking cannot invent a crouch");
    std::printf("PASS: %d real tracking/eye/projection checks\n",checks);
}
