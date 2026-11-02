// Only focused switch/button tests execute. No game DLL is loaded.
#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main
namespace {
bool buttonReject=false,buttonYOnly=false;
void __cdecl FakeButtonCollision(int16_t index,uint8_t* lara,void*) {
    Check(index==0 && (*Ptr<uint32_t>(dll.input)&0x40),"touch supplies native Action and item index");
    Check(Field<int16_t>(18)==2 && Field<int16_t>(24)==(dll.module[4]==L'3' ? 103 : 11),"native standing pose supplied");
    Check(Detour_SwitchTestPosition(nullptr,leverItem,lara)==1 && Detour_SwitchMovePosition(nullptr,leverItem,lara)==1,"button alignment bypass is scoped");
    if(buttonReject) return;
    ++leverActivations;LeverField<int16_t>(20)=1-LeverField<int16_t>(18);LeverField<uint16_t>(484)=3;
    std::memset(lara+18,41,10);std::memset(lara+88,42,40);
    std::memset(Ptr<uint8_t>(dll.lara+256),0,12);*Ptr<int16_t>(dll.lara+2)=1;
    *Ptr<int16_t>(dll.lara+58)=23;*Ptr<uint16_t>(dll.lara+60)&=~0x2000;
}
void __cdecl FakeButtonMatrices(uint8_t* target) {
    // Authored idle root (1,-567,477), child offset (0,0,-23).
    const float a=Radians(reinterpret_cast<PHD_3DPOS*>(target+88)->y_rot),c=std::cos(a),s=std::sin(a);
    motiongun::Frame root{{{{c,0,s},{0,1,0},{-s,0,c}}},{}};
    root.origin=motiongun::Transform(root.basis,{1,-567,477});auto child=root;
    child.origin=Add(root.origin,motiongun::Transform(root.basis,{0,0,-23}));
    if(!*reinterpret_cast<int16_t*>(target+18)) {
        // Packed native Y/Z angles are 511/1024 turns in the toggled idle.
        const float b=511*2*3.141592653589793f/1024,cb=std::cos(b),sb=std::sin(b);
        motiongun::Frame y{{{{cb,0,sb},{0,1,0},{-sb,0,cb}}},{}},z{{{{cb,-sb,0},{sb,cb,0},{0,0,1}}},{}};
        child=motiongun::Multiply(child,buttonYOnly ? y : motiongun::Multiply(y,z));
    }
    float rows[24];motiongun::WriteRows(root,rows);motiongun::WriteRows(child,rows+12);
    auto* fixed=reinterpret_cast<int32_t*>(target+off::item_joints_cur);
    for(int i=0;i<24;++i) fixed[i]=int32_t(std::round(rows[i]*16384));
    leverMatrixPtr+=12;
}
void TestButtons() {
    for(const auto& profile:firstperson::ButtonProfiles) for(int state:{0,1}) for(int hand:{0,1}) for(int yaw:{0,1,2,3}) {
        Reset(2,{},true);ResetPhysicalButtons();dll.module=profile.game==2 ? L"tomb2.dll" : L"tomb3.dll";
        const auto base=std::min(reinterpret_cast<uint64_t>(module),reinterpret_cast<uint64_t>(&FakeButtonMatrices))&~0xffffull;
        const auto delta=uint32_t(reinterpret_cast<uint64_t>(module)-base);g_boundBase=base;
        for(auto* f:{&dll.laraItem,&dll.camera,&dll.analogInput,&dll.input,&dll.lara}) *f+=delta;
        const auto rva=[&](const void* p) {const auto n=reinterpret_cast<uint64_t>(p)-base;Check(n<0x100000000ull,"button fixture RVA fits");return uint32_t(n);};
        SwitchDll table{};table.items=rva(&leverItems);table.count=rva(&leverCount);
        table.collision=rva(reinterpret_cast<void*>(&FakeButtonCollision));table.control=rva(reinterpret_cast<void*>(&FakeLeverControl));
        table.calcMatrices=rva(reinterpret_cast<void*>(&FakeButtonMatrices));g_switchDll=&table;
        dll.objects=rva(leverObjects);dll.phdMxptr=rva(&leverMatrixPtr);dll.los=rva(reinterpret_cast<void*>(&FakeLeverSight));dll.getFloor=rva(reinterpret_cast<void*>(&FakeLeverFloor));
        g_hSwitchCollision.m_trampoline=reinterpret_cast<void*>(&FakeButtonCollision);
        g_hSwitchTestPosition.m_trampoline=g_hSwitchMovePosition.m_trampoline=reinterpret_cast<void*>(&FakeLeverPosition);
        std::memset(leverObjects,0,sizeof(leverObjects));std::memset(leverItem,0,sizeof(leverItem));
        *reinterpret_cast<int16_t*>(leverObjects)=15;
        for(int j:{10,13}) { auto* b=reinterpret_cast<float*>(leverObjects+192)+16*j;b[0]=b[5]=b[10]=b[15]=1;b[12]=j==10 ? -69.f : 69.f;b[13]=-5;b[14]=4; }
        buttonYOnly=std::strstr(profile.name,"QUADCHAS") || std::strstr(profile.name,"TRIBOSS");
        const int slot=profile.game==2 ? 103 : 128;auto* object=leverObjects+slot*off::object_stride;
        *reinterpret_cast<int16_t*>(object)=2;*reinterpret_cast<const char**>(object+off::object_geom)=profile.name;
        *reinterpret_cast<Fn_SwitchCollision*>(object+64)=FakeButtonCollision;*reinterpret_cast<Fn_SwitchControl*>(object+24)=FakeLeverControl;
        for(int j=0;j<2;++j) {auto* b=reinterpret_cast<float*>(object+192)+16*j;b[0]=b[5]=b[10]=b[15]=1;b[14]=j ? 23.f : 0.f;}
        LeverField<int16_t>(16)=int16_t(slot);LeverField<int16_t>(18)=int16_t(state);LeverField<int16_t>(42)=900;
        LeverField<PHD_3DPOS>(88).y_rot=int16_t(yaw*16384);
        Field<int16_t>(24)=profile.game==2 ? 11 : 103;g_scenePose={0,-700,-32,0,0,0,0};g_scenePoseValid=true;
        testControllerOffset[0]=testControllerOffset[1]={};testMissingHand=1-hand;leverSightBlocked=false;buttonReject=false;
        g_switchGripClaimed=false;leverActivations=0;
        motiongun::Vec initial{};Check(ButtonHandPoint(hand,initial),"left/right calibrated palm available");
        motiongun::Frame inv{},frame{};firstperson::ButtonSurface face{};const uint8_t* definition=nullptr;
        Check(ButtonObject(leverItem,definition)==&profile,"exact game/slot/mesh/native handler accepted");
        Check(ButtonFrame(leverItem,object,profile,inv,face) && motiongun::Inverse(inv,frame),"native fixed-point button frame");
        const float x=(face.left+face.right)*.5f,y=(face.top+face.bottom)*.5f,outside=face.z-160;
        const auto from=motiongun::Transform(frame,{x,y,outside});auto& p=LeverField<PHD_3DPOS>(88);
        p.x_pos=int32_t(std::round(initial.x-from.x));p.y_pos=int32_t(std::round(initial.y-from.y));p.z_pos=int32_t(std::round(initial.z-from.z));Field<PHD_3DPOS>(88)=p;
        Check(ButtonFrame(leverItem,object,profile,inv,face) && motiongun::Inverse(inv,frame) && leverMatrixPtr==leverStack,"pose sampling restores matrix stack");
        uint64_t now=GetTickCount64();auto tick=[&](float z) {
            auto d=Sub(motiongun::Transform(frame,{x,y,z}),initial);testControllerOffset[hand]={d.x/1000,d.y/1000,d.z/1000};
            const int before=leverActivations,left=testHapticShots[0],right=testHapticShots[1];
            AdvancePhysicalButton(item,nullptr,++now);
            Check(testHapticShots[0]==left && testHapticShots[1]-right==leverActivations-before,
                "each successful button press acknowledges exactly once on the right, regardless of touching hand");
        };
        uint8_t savedItem[sizeof(item)],savedLara[432];std::memcpy(savedItem,item,sizeof(item));std::memcpy(savedLara,Ptr<uint8_t>(dll.lara),432);
        *Ptr<uint32_t>(dll.input)=0x800;*Ptr<uint32_t>(dll.input+4)=0xdeadbeef;
        tick(face.z);Check(!leverActivations,"initial hand overlap is not a press");
        buttonReject=true;tick(outside);tick(face.z);buttonReject=false;
        Check(!leverActivations,"native rejection gives neither activation nor haptic acknowledgement");
        tick(outside);tick(face.z);
        Check(leverActivations==1 && LeverField<int16_t>(20)==1-state,"touch toggles native button from both idle positions");
        Check(!std::memcmp(savedItem,item,sizeof(item)) && !std::memcmp(savedLara,Ptr<uint8_t>(dll.lara),432),"touch restores Lara position, animation and globals");
        Check(LeverField<int16_t>(42)==900 && *Ptr<uint32_t>(dll.input)==0x800 && *Ptr<uint32_t>(dll.input+4)==0xdeadbeef,"timer and adjacent input preserved");
        LeverField<uint16_t>(484)=0;tick(face.z-30);tick(face.z);Check(leverActivations==1,"jitter cannot repeat a reset button");
        tick(outside);tick(face.z);Check(leverActivations==2,"withdrawal rearms button");
        for(int guard=0;guard<11;++guard) {
            ResetPhysicalButtons();LeverField<uint16_t>(484)=0;const int before=leverActivations;tick(outside);
            if(guard==0) leverSightBlocked=true;
            if(guard==1) *Ptr<int16_t>(dll.lara+2)=1;
            if(guard==2) g_active=false;
            if(guard==3) g_switchGripClaimed=true;
            if(guard==4) LeverField<uint16_t>(484)=4;
            if(guard==5) LeverField<uint16_t>(44)=0x100;
            if(guard==6) Field<uint16_t>(484)=8;
            if(guard==7) {testMissingHand=hand;tick(face.z);testMissingHand=1-hand;}
            if(guard==8) now+=300;
            if(guard==9) dll.module=L"tomb1.dll";
            if(guard==10) LeverField<int16_t>(16)=int16_t(WallSwitchObject());
            tick(face.z);Check(leverActivations==before,"touch respects occlusion, mode, native availability and tracking guards");
            leverSightBlocked=false;*Ptr<int16_t>(dll.lara+2)=0;g_active=true;g_switchGripClaimed=false;
            LeverField<uint16_t>(44)=0;Field<uint16_t>(484)=0;dll.module=profile.game==2 ? L"tomb2.dll" : L"tomb3.dll";LeverField<int16_t>(16)=int16_t(slot);
        }
        ResetPhysicalButtons();g_switchDll=nullptr;
    }
    g_hSwitchCollision.m_trampoline=g_hSwitchTestPosition.m_trampoline=g_hSwitchMovePosition.m_trampoline=nullptr;
}
}
#ifndef BUTTON_TOUCH_NO_MAIN
int main() { TestPhysicalSwitch();TestButtons();std::printf("PASS: %d physical-switch/button checks (TR1-3)\n",checks); }

#endif
