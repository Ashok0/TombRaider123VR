// Focused production-hook fixture; no full suite or native game DLL executes.
#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main
namespace {
uint32_t seenHopInput=0;
int16_t seenHopAxes[4]{};
bool captureOnly=false;
void __cdecl BackHopAboveWater(uint8_t* target,void* collision) {
    seenHopInput=*Ptr<uint32_t>(dll.input);
    std::memcpy(seenHopAxes,Ptr<int16_t>(dll.analogInput),sizeof(seenHopAxes));
    if(captureOnly) return;
    auto& state=Field<int16_t>(18);auto& goal=Field<int16_t>(20);
    if(state==2 && (seenHopInput&Back) && !(seenHopInput&Walk)) {
        if(!floorAllowsEntry) return;
        const auto* axes=Ptr<int16_t>(dll.analogInput);
        Check(!axes[0] && !axes[1] && axes[2]==Field<PHD_3DPOS>(88).y_rot,
            "hop bypasses analog turning and uses Lara facing, armed or unarmed");
        state=5;goal=2;Field<int16_t>(24)=88;
    }
    if(state!=5) { NativeAboveWater(target,collision);return; }
    goal=2;const bool previous=useAnimationTable;useAnimationTable=false;nativeSpeed=37;
    NativeAboveWater(target,collision);useAnimationTable=previous;
}
void HopSetup(int game,bool modern,bool armed,bool smooth,float head,int clip) {
    Reset(clip==103 ? 2 : clip==40 ? 16 : clip==12 ? 6 : clip==13 ? 7 : 20,{0,-1},smooth,head);EnableAnimations(game);
    dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
    testConfigFlags=modern ? 2 : 0;testHeadYaw=0;
    g_heightCrouch={};g_physicalCrouchCamera=false;g_jumpLanding.Reset();
    Field<int16_t>(24)=int16_t(clip);Field<int16_t>(26)=animationTable[clip].first;
    testConfig.firstPersonMoveWithHead=true;testConfig.gamepadEnabled=true;
    testConfig.hybridModernTankControls=true;
    *Ptr<int16_t>(dll.lara+2)=armed ? 4 : 0;*Ptr<int16_t>(dll.lara+4)=armed ? 1 : 0;
    g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&BackHopAboveWater);
    seenHopInput=0;captureOnly=false;
    g_active=g_runtimeEnabled=g_haveHeading=false;g_headingItem=nullptr;
    Field<PHD_3DPOS>(88).y_rot=Angle(head);
    Ptr<int16_t>(dll.analogInput)[2]=Ptr<int16_t>(dll.analogInput)[3]=Angle(head+1.2f);
}
void HopInput(float x,float z,bool grip,bool shifted=false) {
    float turn=0;FirstPersonInput(x,z,turn,shifted,false,grip);
    auto* axes=Ptr<int16_t>(dll.analogInput);axes[0]=int16_t(x*32767);axes[1]=int16_t(z*32767);
    *Ptr<uint32_t>(dll.input)=Back|Walk;
}
}
int main() {
    Check(!Config{}.hybridModernTankControls,"Hybrid Modern/Tank Controls defaults Off");
    for(int game:{1,2,3}) for(bool modern:{false,true}) for(bool armed:{false,true})
    for(bool smooth:{false,true}) for(float head:{0.f,.7f}) for(int clip:{103,40,12,13,44,69}) {
        HopSetup(game,modern,armed,smooth,head,clip);
        testConfig.hybridModernTankControls=false;captureOnly=true;
        float x=.1f,z=-1,turn=.3f;
        FirstPersonInput(x,z,turn,false,false,true);
        Check(!g_thirdPersonBackHopPressed && x==.1f && z==-1 && turn==.3f,
            "disabled hybrid preserves third-person stick input without queuing a hop");
        auto* axes=Ptr<int16_t>(dll.analogInput);axes[0]=1234;axes[1]=-23456;
        *Ptr<uint32_t>(dll.input)=Back|Walk;
        const int16_t originalAxes[4]={axes[0],axes[1],axes[2],axes[3]};
        const int originalState=Field<int16_t>(18),originalAnim=Field<int16_t>(24);
        Detour_LaraAboveWater(item,nullptr);
        Check(seenHopInput==(Back|Walk) && !std::memcmp(seenHopAxes,originalAxes,sizeof(originalAxes)),
            "disabled hybrid forwards native Walk/direction bits and camera-relative analog axes");
        Check(Field<int16_t>(18)==originalState && Field<int16_t>(24)==originalAnim,
            "disabled hybrid does not force a standing or hop animation");
        g_thirdPersonBackHopPressed=true;
        { ThirdPersonBackHopScope stale(item);
          Check(*Ptr<uint32_t>(dll.input)==(Back|Walk) && !std::memcmp(axes,originalAxes,sizeof(originalAxes)),
              "disabled hybrid also rejects a previously queued hop at simulation time"); }
    }

    for(int game:{1,2,3}) for(bool modern:{false,true}) for(bool armed:{false,true})
    for(bool smooth:{false,true}) for(float head:{0.f,.7f}) for(int clip:{103,40,12,13,44,69}) {
        HopSetup(game,modern,armed,smooth,head,clip);HopInput(0,-1,true);
        Detour_LaraAboveWater(item,nullptr);
        Check(Field<int16_t>(18)==5 && (seenHopInput&(Directions|Walk))==Back,"RG/back enters native hop from idle or backward walk");
        Check(std::fabs(Length(beforeCollision)-37)<1,"hop displacement is neither canceled nor tripled");
        Check(Dot(beforeCollision,Rotate({0,-1},head))>36,"third-person hop moves backward relative to Lara");
        Check(ticks==1 && collisions==1 && Field<PHD_3DPOS>(88).y_pos==7,"one native animation/collision tick preserves height");
        Check(Ptr<int16_t>(dll.analogInput)[1]!=0 && Ptr<int16_t>(dll.analogInput)[2]==Angle(head+1.2f) && *Ptr<uint32_t>(dll.input)==(Back|Walk),"native input and chase-camera axes restored after simulation");
        for(const Vec stick:{Vec{},Vec{0,1},Vec{1,0},Vec{0,-1}}) {
            HopInput(stick.x,stick.z,false);*Ptr<uint32_t>(dll.input)=0;Detour_LaraAboveWater(item,nullptr);
            Check(Field<int16_t>(18)==5 && std::fabs(Length(beforeCollision)-37)<1,"release/reversal preserves unfinished hop motion");
            Check(Dot(beforeCollision,Rotate({0,-1},head))>36,"unfinished hop retains travel direction");
        }
        wall=true;const auto before=Field<PHD_3DPOS>(88);Detour_LaraAboveWater(item,nullptr);
        Check(Field<PHD_3DPOS>(88).x_pos==before.x_pos && Field<PHD_3DPOS>(88).z_pos==before.z_pos,"native wall collision remains final");
    }
    for(int game:{1,2,3}) {
        for(bool hybrid:{false,true}) for(bool grip:{false,true}) {
            HopSetup(game,true,false,true,0,103);
            testConfig.hybridModernTankControls=hybrid;
            g_active=g_runtimeEnabled=g_haveHeading=true;g_headingItem=item;
            HopInput(0,-1,grip);Detour_LaraAboveWater(item,nullptr);
            Check(Field<int16_t>(18)==16 && (seenHopInput&Walk),"first-person movement is unchanged with RG held or released");
        }
        if(game==3) {
            HopSetup(game,true,false,true,0,103);HopInput(0,-1,true);Field<int16_t>(22)=71;
            Detour_LaraAboveWater(item,nullptr);
            Check(Field<int16_t>(18)==5 && Field<int16_t>(22)==71,"standing queued DUCK permits hop without clearing native queue");
        }
        for(const Vec stick:{Vec{},Vec{0,1},Vec{1,0},Vec{-1,0}}) {
            HopSetup(game,true,false,true,0,103);captureOnly=true;HopInput(stick.x,stick.z,true);
            Detour_LaraAboveWater(item,nullptr);
            Check(seenHopInput==(Back|Walk),"neutral, forward and sideways sticks do not activate hop");
        }
        HopSetup(game,true,false,true,0,103);HopInput(0,-1,true);floorAllowsEntry=false;Detour_LaraAboveWater(item,nullptr);
        Check(Field<int16_t>(18)==2 && Field<PHD_3DPOS>(88).z_pos==0,"native rear-floor rejection prevents hop");
        for(int guard=0;guard<16;++guard) {
            HopSetup(game,true,false,true,0,103);captureOnly=true;
            if(guard==0) g_active=true;
            if(guard==1) Field<int16_t>(18)=Field<int16_t>(20)=86;
            if(guard==2) Field<int16_t>(18)=Field<int16_t>(20)=80;
            if(guard==3) g_runtimeEnabled=true;
            if(guard==9) Field<int16_t>(22)=19;
            if(guard==10) Field<uint16_t>(off::item_flags)|=8;
            if(guard==11) Field<int16_t>(24)=24;
            if(guard==13) testConfig.gamepadEnabled=false;
            if(guard==14) *Ptr<uint16_t>(dll.lara+60)|=0x2000;
            HopInput(0,-1,guard!=15,guard==12);
            auto& input=*Ptr<uint32_t>(dll.input);
            if(guard==4) input|=firstperson::PhysicalCrouchInputScope::Duck;
            if(guard==5) input|=0x40;
            if(guard==6) input|=0x4000;
            if(guard==7) input|=0x100;
            if(guard==8) input|=0x10;
            Detour_LaraAboveWater(item,nullptr);
            Check((seenHopInput&Walk)!=0,"first person (including suspended), vents, jump, Action, roll and shifted input are untouched");
        }
    }
    std::printf("PASS: %d third-person hop and first-person preservation checks (TR1/2/3)\n",checks);
}
