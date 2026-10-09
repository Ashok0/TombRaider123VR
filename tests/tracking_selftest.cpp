// Real VRSystem eye transforms, independent of the input-hook stubs.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <openvr.h>
#include "../src/StereoMath.h"
#define private public
#include "../src/VRSystem.h"
#undef private
#include "../src/VRSystem.cpp"
namespace tr {
Config testConfig;
bool testFirstPerson=false;
const Config& Cfg() { return testConfig; }
bool FirstPersonActive() { return testFirstPerson; }
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
    std::printf("PASS: %d real tracking/eye/projection checks\n",checks);
}
