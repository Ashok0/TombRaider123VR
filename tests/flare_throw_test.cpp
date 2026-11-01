// Focused production gesture/commit tests. Native calls are stand-ins; no
// game DLL or full first-person suite is loaded or executed.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "FlareThrowMotion.h"
namespace vr {struct HmdMatrix34_t {float m[3][4]{};};}
namespace tr {
namespace {
int checks=0;
void Check(bool ok,const char* why) {++checks;if(!ok){std::printf("FAIL: %s\n",why);std::exit(1);}}
namespace off {
#ifdef FLARE_TR123
constexpr int item_stride=3664,item_pos=88;
#else
constexpr int item_stride=9416,item_pos=96;
#endif
}
struct PHD_3DPOS {int32_t x_pos,y_pos,z_pos;int16_t x_rot,y_rot,z_rot,pad;};
struct ShotVector {int32_t x,y,z;int16_t room,pad;};
struct Layout {uint32_t laraItem,lara,getFloor;} layout;
Layout* g_boundDll=&layout;
uint64_t g_boundBase=0;
template<class T>T* Ptr(uint32_t rva){return reinterpret_cast<T*>(g_boundBase+rva);}
uint32_t Rva(const void* p){const auto n=reinterpret_cast<uint64_t>(p)-g_boundBase;Check(n<=UINT32_MAX,"fixture RVA fits");return uint32_t(n);}
uint8_t bodyMemory[off::item_stride]{},pool[off::item_stride*2]{},laraMemory[512]{};
uint8_t* body=bodyMemory;uint8_t* items=pool;int16_t nextFree=0;
int level=1,game=4,creates=0,cleanups=0,meshRemovals=0,losCalls=0;
bool ready=true,tracking=true,wall=false,floorPresent=true,fallback=false;
double now=10;float heading=0;int16_t nativeAge=123;
struct FakeVR {
    vr::HmdMatrix34_t pose{};uint64_t serial=1;
    bool ControllerPose(int hand,vr::HmdMatrix34_t& out){Check(hand==0,"left controller only");out=pose;return tracking;}
    bool HeadPose(vr::HmdMatrix34_t& out){out={};out.m[1][3]=.6f;return tracking;}
    float HeadYawRadians(){return 0;}
    uint64_t PoseSerial(){return serial;}
} vrSystem;
FakeVR& VR(){return vrSystem;}
bool FlareIKReady(){return ready && *reinterpret_cast<int16_t*>(laraMemory+2)==0 &&
    *reinterpret_cast<int16_t*>(laraMemory+4)==(game==2?8:game==3?9:7);}
bool BuildControllerWrist(int hand,motiongun::Frame& w,motiongun::Basis& b){
    Check(hand==0,"left wrist only");b={{{1,0,0},{0,1,0},{0,0,1}}};w.basis=b;
    const auto p=locomotion::Rotate({vrSystem.pose.m[0][3],-vrSystem.pose.m[2][3]},heading);
    w.origin={p.x*768,-vrSystem.pose.m[1][3]*768,p.z*768};return tracking;
}
double TurnTime(){return now;}
float LiveWorldUnitsPerMetre(){return 768;}
int FlareLevel(){return level;}
float FlareHeading(){return heading;}
void Log(const char*){}
void* __cdecl Floor(int32_t,int32_t,int32_t,int16_t*){return floorPresent?bodyMemory:nullptr;}
int32_t __cdecl LOS(ShotVector* a,ShotVector*){++losCalls;Check(a->x==0 && a->y==-256 && a->z==0,"LOS starts inside body");return !wall;}
uint32_t FlareLOS(){return Rva(reinterpret_cast<void*>(&LOS));}
#include "PhysicalFlareThrow.inl"
void __cdecl Undraw(){
    auto& frame=*reinterpret_cast<int16_t*>(laraMemory+(game<=3?0x118:0xF8));
    Check(*reinterpret_cast<int16_t*>(body+20)==0,"native body-throw branch bypassed");
    if(frame==20){
        ++creates;++meshRemovals;
        int32_t p[]={-16,32,42};
        Check(!PhysicalFlareSpawnPoint(body,p,10),"right hand query remains native");
        Check(PhysicalFlareSpawnPoint(body,p,13),"native release queries controller position");
        Check(!PhysicalFlareSpawnPoint(body,p,13),"spawn override used once");
        auto* flare=items+nextFree*off::item_stride;nextFree=-1;
        *reinterpret_cast<int16_t*>(flare+16)=g_flareThrowDll->object;
        std::memcpy(flare+off::item_pos,p,sizeof(p));
        if(fallback) *reinterpret_cast<int32_t*>(flare+off::item_pos)+=80;
        *reinterpret_cast<int16_t*>(flare+34)=50;*reinterpret_cast<int16_t*>(flare+36)=-50;
        *reinterpret_cast<int16_t*>(flare+44)=nativeAge;
        frame=21;
    }else{
        Check(frame==32,"only native release and cleanup events run");++cleanups;frame=0;
        *reinterpret_cast<int16_t*>(laraMemory+2)=0;
        *reinterpret_cast<int16_t*>(laraMemory+4)=*reinterpret_cast<int16_t*>(laraMemory+8);
    }
    std::memset(body+24,7,4); // native idle clip/frame writes are restored by production scope
}
FlareThrowDll dll{};
void Reset(int title){
    game=title;g_flareGesture={};g_flareRelease={};g_flareSpawn=nullptr;
    body=bodyMemory;items=pool;std::memset(bodyMemory,0,sizeof(bodyMemory));
    std::memset(pool,0,sizeof(pool));std::memset(laraMemory,0,sizeof(laraMemory));
    *reinterpret_cast<int16_t*>(body+18)=2;*reinterpret_cast<int16_t*>(body+20)=2;
    *reinterpret_cast<int16_t*>(body+24)=103;*reinterpret_cast<int16_t*>(body+26)=115;
    *reinterpret_cast<int16_t*>(laraMemory+4)=game==2?8:game==3?9:7;
    nextFree=0;level=1;now=10;heading=0;ready=tracking=floorPresent=true;wall=fallback=false;
    creates=cleanups=meshRemovals=losCalls=0;vrSystem={};vrSystem.pose.m[1][3]=.6f;vrSystem.pose.m[0][3]=.25f;
    layout={Rva(&body),Rva(laraMemory),Rva(reinterpret_cast<void*>(&Floor))};
    dll={};dll.game=game;dll.undraw=Rva(reinterpret_cast<void*>(&Undraw));
    dll.items=Rva(&items);dll.nextFree=Rva(&nextFree);dll.object=game==2?152:game==3?179:game==4?372:354;
    g_flareThrowDll=&dll;
}
void Quiet(){
    for(int i=0;i<30;++i){now+=.01;++vrSystem.serial;PhysicalFlareMotion(true);}
    Check(g_flareGesture.armed,"settled hand becomes ready without buttons");
}
void Queue(){
    Quiet();
    for(int i=0;i<40 && !g_flareRelease.time;++i){now+=.01;++vrSystem.serial;vrSystem.pose.m[0][3]+=.02f;PhysicalFlareMotion(true);}
    Check(g_flareRelease.time>0,"swing alone queues simulation work");
}
void TestRuntime(int title){
    Reset(title);Queue();uint8_t animation[10];std::memcpy(animation,body+18,10);
    int32_t point[3];std::memcpy(point,g_flareRelease.point,sizeof(point));
    CommitPhysicalFlareThrow();
    Check(creates==1 && cleanups==1 && meshRemovals==1,"single native release and cleanup");
    Check(!std::memcmp(body+18,animation,10),"body animation restored");
    Check(!std::memcmp(pool+off::item_pos,point,sizeof(point)),"spawned at captured wrist");
    Check(*reinterpret_cast<int16_t*>(pool+34)==51 && *reinterpret_cast<int16_t*>(pool+36)==0,"controller speed applied");
    Check(*reinterpret_cast<int16_t*>(pool+44)==nativeAge,"native flare age retained");
    Check(*reinterpret_cast<int16_t*>(laraMemory+2)==0 && *reinterpret_cast<int16_t*>(laraMemory+4)==0,"native cleanup owns weapon state");
    CommitPhysicalFlareThrow();Check(creates==1,"no repeat on next gun tick");
    int32_t ordinary[]={-16,32,42};Check(!PhysicalFlareSpawnPoint(body,ordinary,13),"ordinary flare queries unchanged");
    Reset(title);Quiet();
    for(int i=0;i<100;++i){now+=.01;++vrSystem.serial;vrSystem.pose.m[0][3]+=.001f;PhysicalFlareMotion(true);}
    CommitPhysicalFlareThrow();Check(creates==0,"slow hand movement keeps the flare");
    Reset(title);fallback=true;Queue();CommitPhysicalFlareThrow();Check(*reinterpret_cast<int16_t*>(pool+34)==50 && *reinterpret_cast<int16_t*>(pool+36)==-50,"native collision fallback preserved");
    for(int reason=0;reason<10;++reason){
        Reset(title);Queue();
        switch(reason){case 0:nextFree=-1;break;case 1:wall=true;break;case 2:floorPresent=false;break;
        case 3:now+=.151;break;case 4:++level;break;case 5:ready=false;break;
        case 6:body=pool;break;case 7:items=nullptr;break;case 8:g_flareThrowDll=nullptr;break;
        case 9:PhysicalFlareMotion(false);break;}
        CommitPhysicalFlareThrow();Check(creates==0 && cleanups==0,"unsafe/stale release cancelled");
        Check(*reinterpret_cast<int16_t*>(laraMemory+4)==(game==2?8:game==3?9:7),"cancel keeps held flare");
    }
    Reset(title);Quiet();++level;now+=.01;++vrSystem.serial;PhysicalFlareMotion(true);
    Check(!g_flareGesture.armed && !g_flareRelease.time,"level transition cancels swing");
    Reset(title);Quiet();tracking=false;now+=.01;PhysicalFlareMotion(true);
    Check(!g_flareGesture.armed && !g_flareRelease.time,"tracking loss cancels");
    Reset(title);Quiet();ResetPhysicalFlareThrow();now+=.01;PhysicalFlareMotion(true);
    Check(!g_flareGesture.armed && !g_flareRelease.time,"recenter cancels");
    Reset(title);wall=true;Queue();CommitPhysicalFlareThrow();Check(creates==0,"wall keeps flare in hand");
    wall=false;Queue();CommitPhysicalFlareThrow();Check(creates==1,"settle and swing retries blocked throw");
}
void TestMotion(){
    using namespace flarethrow;
    const Vec forward{0,0,-1};
    auto settle=[&](Gesture& g,Vec p,double& time,uint64_t& serial,int hz){
        Vec v{};for(int i=0;i<hz/3;++i){time+=1.0/hz;g.Update(true,++serial,time,p,{},forward,v);}
        Check(g.armed,"quiet hand arms swing without buttons");
    };
    // A real underhand throw follows an arc about the shoulder, not a straight
    // line away from the HMD. In its forward half the hand approaches the head.
    for(int hz:{30,60,72,90,120,144})for(float rate:{3.f,5.f})for(int style=0;style<3;++style) {
        Gesture g;Vec v{};double time=1;uint64_t serial=0;
        auto hand=[style](float angle){
            if(style==2)return Vec{-.25f+.45f*std::sin(angle),-.25f,-.45f*std::cos(angle)};
            return Vec{-.25f,-.25f+(style==1?.45f:-.45f)*std::cos(angle),-.45f*std::sin(angle)};
        };
        settle(g,hand(-1.f),time,serial,hz);int releases=0;
        for(int i=1;i<hz;++i){const float angle=-1.f+rate*float(i)/hz;if(angle>1.5f)break;
            time+=1.0/hz;if(g.Update(true,++serial,time,hand(angle),{},forward,v)==Event::Release)++releases;
        }
        Check(releases==1,"natural underhand/overhand/sidearm arc throws without moving away from HMD");
    }
    for(int hz:{30,60,72,90,120,144,240})for(float speed:{-6.f,-2.f,2.f,5.f}){
        Gesture g;Vec v{},p{speed<0?-.25f:.25f,0,-.2f};double time=1;uint64_t serial=0;
        settle(g,p,time,serial,hz);int releases=0;
        for(int i=0;i<hz/2;++i){time+=1.0/hz;p.x+=speed/hz;
            if(g.Update(true,++serial,time,p,{},forward,v)==Event::Release){
                ++releases;Check(std::fabs(v.x-speed)<.2,"swing uses physical controller speed");
            }
        }
        Check(releases==1,"one automatic throw per sustained swing at all tested refresh rates");
    }
    for(int hz:{30,90,144})for(int mode=0;mode<7;++mode){
        Gesture g;Vec v{},p{.25f,-.5f,-.25f};double time=1;uint64_t serial=0;
        if(mode==3)p={0,0,.4f};if(mode==4)p={0,0,-.7f};
        if(mode!=5)settle(g,p,time,serial,hz);const auto origin=p;
        for(int i=1;i<hz;++i){time+=1.0/hz;const float t=float(i)/hz;Vec head{};
            switch(mode){
            case 0:head={2*t,0,0};p=motiongun::Add(origin,head);break; // roomscale walking
            case 1:head={0,-2*t,0};p=motiongun::Add(origin,head);break; // physical crouch
            case 2:p.x=origin.x+.015f*std::sin(t*50);break; // small wrist shake
            case 3:case 4:p.z=origin.z+3*t;break; // backswing behind/toward head
            case 5:p.x=origin.x+3*t;break; // tracking resumed while moving
            case 6:p.x=origin.x+.5f*t;break; // inspect nearby objects
            }
            Check(g.Update(true,++serial,time,p,head,forward,v)!=Event::Release,"ordinary motion/windup/reacquisition keeps flare");
        }
    }
    for(int axis=0;axis<3;++axis)for(int turn=0;turn<4;++turn){
        Vec v{};(&v.x)[axis]=2;Launch l;
        Check(LaunchVelocity(v,float(turn)*1.5707963268f,768,l),"cardinal launch valid");
        Check(axis==1 ? l.fall==-51&&l.speed==0 : l.fall==0&&l.speed==51,"vertical handedness and speed");
        if(axis==0) Check(uint16_t(l.yaw)==uint16_t((turn+1)*16384),"stick heading rotates physical rightward throw");
        if(axis==2) Check(uint16_t(l.yaw)==uint16_t((turn+2)*16384),"OpenVR back maps to game backward");
    }
    Launch l;Check(LaunchVelocity({100,0,0},0,768,l)&&l.speed==205,"throw speed capped");
    Check(!LaunchVelocity({NAN,0,0},0,768,l),"nonfinite rejected");
    for(int reason=0;reason<6;++reason){
        Gesture g;Vec v{},p{.25f,0,-.2f};double time=1;uint64_t serial=0;settle(g,p,time,serial,90);
        if(reason==0)g.Update(false,++serial,time+.01,p,{},forward,v);
        if(reason==1)g.Update(true,++serial,time+.2,p,{},forward,v);
        if(reason==2)g.Update(true,++serial,time-.1,p,{},forward,v);
        if(reason==3)g.Update(true,++serial,time+.01,{10,0,0},{},forward,v);
        if(reason==4)g.Update(true,++serial,time+.01,{NAN,0,0},{},forward,v);
        if(reason==5)g.Cancel();
        Check(!g.armed,"invalid tracking/chord/time/recenter cancels swing readiness");
    }
    Gesture duplicate;Vec v{};
    for(int i=0;i<100;++i)Check(duplicate.Update(true,1,1+i*.001,{.25f,0,-.2f},{},forward,v)!=Event::Release,"duplicate VR frame cannot throw");
    Check(!duplicate.armed,"duplicate frames cannot arm a throw");

}
} // namespace
} // namespace tr
int main(){using namespace tr;g_boundBase=reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));TestMotion();
#ifdef FLARE_TR123
TestRuntime(2);TestRuntime(3);
#else
TestRuntime(4);TestRuntime(5);
#endif
std::printf("PASS: %d focused physical-flare checks\n",checks);}
