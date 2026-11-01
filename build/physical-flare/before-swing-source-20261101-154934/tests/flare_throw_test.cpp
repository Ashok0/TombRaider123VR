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
    creates=cleanups=meshRemovals=losCalls=0;vrSystem={};vrSystem.pose.m[1][3]=.6f;
    layout={Rva(&body),Rva(laraMemory),Rva(reinterpret_cast<void*>(&Floor))};
    dll={};dll.game=game;dll.undraw=Rva(reinterpret_cast<void*>(&Undraw));
    dll.items=Rva(&items);dll.nextFree=Rva(&nextFree);dll.object=game==2?152:game==3?179:game==4?372:354;
    g_flareThrowDll=&dll;
}
void Queue(float speed=2){
    for(int i=0;i<7;++i){now+=.01;++vrSystem.serial;vrSystem.pose.m[0][3]=speed*float(i)*.01f;
        Check(PhysicalFlareGrip(i!=6,true),"prepare/release consumes left grip");}
    Check(g_flareRelease.time>0,"release queues simulation work");
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
    Reset(title);Queue(.1f);CommitPhysicalFlareThrow();Check(*reinterpret_cast<int16_t*>(pool+34)==0 && *reinterpret_cast<int16_t*>(pool+36)==0,"slow release drops");
    Reset(title);fallback=true;Queue();CommitPhysicalFlareThrow();Check(*reinterpret_cast<int16_t*>(pool+34)==50 && *reinterpret_cast<int16_t*>(pool+36)==-50,"native collision fallback preserved");
    for(int reason=0;reason<10;++reason){
        Reset(title);Queue();
        switch(reason){case 0:nextFree=-1;break;case 1:wall=true;break;case 2:floorPresent=false;break;
        case 3:now+=.151;break;case 4:++level;break;case 5:ready=false;break;
        case 6:body=pool;break;case 7:items=nullptr;break;case 8:g_flareThrowDll=nullptr;break;
        case 9:PhysicalFlareGrip(false,false);break;}
        CommitPhysicalFlareThrow();Check(creates==0 && cleanups==0,"unsafe/stale release cancelled");
        Check(*reinterpret_cast<int16_t*>(laraMemory+4)==(game==2?8:game==3?9:7),"cancel keeps held flare");
    }
    Reset(title);Check(PhysicalFlareGrip(true,true),"armed gesture");++level;
    now+=.01;++vrSystem.serial;Check(!PhysicalFlareGrip(true,true),"level transition drains held grip");
    Check(!PhysicalFlareGrip(false,true) && !g_flareRelease.time,"transition cannot release");
    Reset(title);PhysicalFlareGrip(true,true);tracking=false;now+=.01;
    Check(!PhysicalFlareGrip(false,true) && !g_flareRelease.time,"tracking loss cancels");
    Reset(title);PhysicalFlareGrip(true,true);ResetPhysicalFlareThrow();now+=.01;
    Check(!PhysicalFlareGrip(false,true) && !g_flareRelease.time,"recenter cancels");
}
void TestMotion(){
    using namespace flarethrow;
    for(int hz:{30,60,72,90,120,144,240})for(float speed:{-6.f,-2.f,0.f,1.f,5.f}){
        Gesture g;Vec v{};const double dt=1.0/hz;
        for(int i=0;i<30;++i){auto e=g.Update(i!=29,true,i,1+i*dt,{float(i*dt)*speed,0,0},v);Check(e==(i==29?Event::Release:Event::Held),"sampling rate independent gesture");}
        Check(std::fabs(v.x-speed)<.001,"regression preserves physical speed");
        Launch l;Check(LaunchVelocity(v,0,768,l),"finite launch");
        Check(l.speed==int16_t(std::lround(std::fabs(speed)*768/30)),"native 30 Hz velocity units");
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
        Gesture g;Vec v{};g.Update(true,true,1,1,{0,0,0},v);
        auto e=g.Update(false,reason!=0,reason==1?1:2,reason==2?1.2:reason==3?.9:1.06,
            {reason==4?10.f:0.f,reason==5?NAN:0,0},v);
        Check(e==Event::Idle,"loss/chord/gap/teleport cannot trigger release");
    }
    Gesture g;Vec v{};g.Update(true,true,1,1,{},v);g.Update(true,false,2,1.01,{},v);
    Check(g.Update(true,true,3,1.02,{},v)==Event::Idle,"chord cancellation drains grip");
    Check(g.Update(false,true,4,1.03,{},v)==Event::Idle,"chord release does not throw");
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
