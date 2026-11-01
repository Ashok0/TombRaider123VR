// Only focused interactions execute; game DLLs are never loaded.
#define BUTTON_TOUCH_NO_MAIN
#include "button_touch_selftest.cpp"
#undef main
#include <filesystem>
#include <fstream>
namespace {
std::vector<char> assetData;
std::vector<firstperson::SwitchAnim> specialAnims;
firstperson::SwitchAnim* specialAnimPtr=nullptr;
firstperson::SwitchChange* specialChanges=nullptr;
firstperson::SwitchRange* specialRanges=nullptr;
int16_t* specialCommands=nullptr;
motiongun::Vec specialBone{};
int specialBase=0,specialJoints=0,specialControls=0,specialTicks=0,specialActivations=0;
firstperson::SwitchPosePath nativeSpecialPath{};
template<class T> T Asset(size_t at) {Check(at+sizeof(T)<=assetData.size(),"asset read bounds");T x;std::memcpy(&x,assetData.data()+at,sizeof(x));return x;}
void LoadSpecial(const std::filesystem::path& path,int slot) {
    std::ifstream in(path,std::ios::binary);Check(bool(in),"installed special switch asset exists");
    assetData.assign(std::istreambuf_iterator<char>(in),{});size_t offsets[7]{},at=0;uint32_t counts[7]{};
    const int strides[]={32,6,8,2,4,2,18};
    for(int i=0;i<7;++i) {counts[i]=Asset<uint32_t>(at);at+=4;offsets[i]=at;at+=counts[i]*strides[i];Check(at<=assetData.size(),"asset table bounds");}
    specialAnims.assign(counts[0],{});
    for(size_t i=0;i<specialAnims.size();++i) {
        auto& a=specialAnims[i];const auto p=offsets[0]+32*i;
        a.frames=reinterpret_cast<const int16_t*>(assetData.data()+offsets[5]+Asset<uint32_t>(p));
        std::memcpy(reinterpret_cast<char*>(&a)+8,assetData.data()+p+4,28);
    }
    specialAnimPtr=specialAnims.data();specialChanges=reinterpret_cast<firstperson::SwitchChange*>(assetData.data()+offsets[1]);
    specialRanges=reinterpret_cast<firstperson::SwitchRange*>(assetData.data()+offsets[2]);specialCommands=reinterpret_cast<int16_t*>(assetData.data()+offsets[3]);
    specialBase=-1;
    for(uint32_t i=0;i<counts[6];++i) {
        const size_t p=offsets[6]+i*18;if(Asset<uint32_t>(p)!=uint32_t(slot)) continue;
        specialJoints=Asset<int16_t>(p+4);specialBase=Asset<int16_t>(p+16);specialBone={};
        if(specialJoints==2) {const auto bone=offsets[4]+Asset<uint32_t>(p+8)*4;specialBone={float(Asset<int32_t>(bone+4)),float(Asset<int32_t>(bone+8)),float(Asset<int32_t>(bone+12))};}
    }
    Check(specialBase>=0,"requested native object found");
}
motiongun::Frame Rotation(const int16_t*& p) {
    const uint16_t a=uint16_t(*p++);float x=0,y=0,z=0;const float unit=2*Pi/1024;
    if(a&0xc000) {const float v=(a&1023)*unit;if((a>>14)==1)x=v;else if((a>>14)==2)y=v;else z=v;}
    else {const uint32_t v=(uint32_t(a)<<16)|uint16_t(*p++);x=((v>>20)&1023)*unit;y=((v>>10)&1023)*unit;z=(v&1023)*unit;}
    const float cx=std::cos(x),sx=std::sin(x),cy=std::cos(y),sy=std::sin(y),cz=std::cos(z),sz=std::sin(z);
    motiongun::Frame rx{{{{1,0,0},{0,cx,-sx},{0,sx,cx}}},{}},ry{{{{cy,0,sy},{0,1,0},{-sy,0,cy}}},{}},rz{{{{cz,-sz,0},{sz,cz,0},{0,0,1}}},{}};
    return motiongun::Multiply(motiongun::Multiply(ry,rx),rz);
}
void AssetMatrices(const int16_t* f,motiongun::Frame result[2]) {
    const auto* p=f+9;result[0]=Rotation(p);result[0].origin={float(f[6]),float(f[7]),float(f[8])};
    if(specialJoints==2) {auto child=Rotation(p);child.origin=specialBone;result[1]=motiongun::Multiply(result[0],child);}
}
void __cdecl SpecialMatrices(uint8_t* target) {
    const int a=*reinterpret_cast<int16_t*>(target+24),f=*reinterpret_cast<int16_t*>(target+26);const auto& anim=specialAnims[a];
    const int rate=anim.interpolation&255,width=uint16_t(anim.interpolation)>>8,n=(f-anim.first)/rate;
    const float frac=float((f-anim.first)%rate)/rate;
    motiongun::Frame first[2]{},second[2]{};AssetMatrices(anim.frames+n*width,first);
    if(frac>0) AssetMatrices(anim.frames+(n+1)*width,second);else for(int j=0;j<2;++j)second[j]=first[j];
    const float yaw=Radians(reinterpret_cast<PHD_3DPOS*>(target+88)->y_rot),c=std::cos(yaw),s=std::sin(yaw);
    motiongun::Frame turn{{{{c,0,s},{0,1,0},{-s,0,c}}},{}};
    auto* output=reinterpret_cast<int32_t*>(target+off::item_joints_cur);
    for(int j=0;j<specialJoints;++j) {
        float arows[12],brows[12];motiongun::WriteRows(first[j],arows);motiongun::WriteRows(second[j],brows);
        for(int k=0;k<12;++k) arows[k]+=(brows[k]-arows[k])*frac;
        motiongun::WriteRows(motiongun::Multiply(turn,motiongun::ReadRows(arows)),arows);
        for(int k=0;k<12;++k)output[j*12+k]=int32_t(std::round(arows[k]*16384));
    }
    leverMatrixPtr+=12;
}
void __cdecl SpecialAnimate(uint8_t* target) {
    ++specialTicks;auto& a=*reinterpret_cast<int16_t*>(target+24);auto& f=*reinterpret_cast<int16_t*>(target+26);auto& state=*reinterpret_cast<int16_t*>(target+18);
    ++f;const int goal=*reinterpret_cast<int16_t*>(target+20);
    if(state!=goal) for(int c=0;c<specialAnims[a].changeCount;++c) {
        const auto& change=specialChanges[specialAnims[a].changeIndex+c];if(change.goal!=goal)continue;
        bool done=false;for(int r=0;r<change.count;++r) {const auto& range=specialRanges[change.index+r];if(f>=range.first && f<=range.last) {a=range.animation;f=range.frame;state=specialAnims[a].state;done=true;break;}}
        if(done)break;
    }
    const auto& anim=specialAnims[a];if(f>anim.last) {
        int cmd=anim.commandIndex;for(int c=0;c<anim.commandCount;++c) {int op=specialCommands[cmd++];if(op==4)*reinterpret_cast<uint16_t*>(target+484)=5;else if(op==5)cmd+=2;else Check(false,"unexpected native completion command");}
        a=anim.jump;f=anim.jumpFrame;state=specialAnims[a].state;
    }
}
void __cdecl SpecialControl(int16_t) {++specialControls;LeverField<uint16_t>(44)|=0x3e00;SpecialAnimate(leverItem);}
void __cdecl SpecialCollision(int16_t index,uint8_t* lara,void*) {
    Check(index==0 && (*Ptr<uint32_t>(dll.input)&0x40),"native special Action supplied");
    Check(Detour_SwitchTestPosition(nullptr,leverItem,lara) && Detour_SwitchMovePosition(nullptr,leverItem,lara),"special use bypasses Action icon alignment");
    ++specialActivations;LeverField<int16_t>(20)=1-LeverField<int16_t>(18);LeverField<uint16_t>(484)=3;SpecialAnimate(leverItem);
    std::memset(lara+18,41,10);std::memset(lara+88,42,40);std::memset(Ptr<uint8_t>(dll.lara+256),42,12);
    *Ptr<int16_t>(dll.lara+2)=1;*Ptr<int16_t>(dll.lara+58)=23;*Ptr<uint16_t>(dll.lara+60)|=4;
}
void TestSpecials(const std::filesystem::path& gameRoot) {
    const char* levels[]={"BOAT","CRASH","ROOFS","TOWER","HOUSE","KEEL","ANTARC","LA/UNDERSEA","SHORE"};
    for(int profileIndex=0;profileIndex<9;++profileIndex) for(int state:{0,1}) for(int yaw:{0,1,2,3}) {
        const auto& profile=kSpecialSwitches[profileIndex];if(profile.wheel && profile.game==2 && state==1)continue;
        LoadSpecial(gameRoot/std::to_string(profile.game)/"DATA"/(std::string(levels[profileIndex])+".PDP"),profile.slot);
        Reset(2,{},true);ResetPhysicalButtons();g_parkedSwitches.clear();g_switchPreview={};g_switchGripDown=g_switchGripClaimed=false;
        dll.module=profile.game==2 ? L"tomb2.dll" : L"tomb3.dll";
        const auto base=std::min(reinterpret_cast<uint64_t>(module),reinterpret_cast<uint64_t>(&SpecialMatrices))&~0xffffull;
        const auto delta=uint32_t(reinterpret_cast<uint64_t>(module)-base);g_boundBase=base;
        for(auto* f:{&dll.laraItem,&dll.camera,&dll.analogInput,&dll.input,&dll.lara}) *f+=delta;
        auto rva=[&](const void* p){return uint32_t(reinterpret_cast<uint64_t>(p)-base);};
        SwitchDll table{};table.items=rva(&leverItems);table.count=rva(&leverCount);table.collision=rva(reinterpret_cast<void*>(&SpecialCollision));table.control=rva(reinterpret_cast<void*>(&SpecialControl));
        table.calcMatrices=rva(reinterpret_cast<void*>(&SpecialMatrices));table.animate=rva(reinterpret_cast<void*>(&SpecialAnimate));table.bounds=rva(leverReach);table.getBounds=rva(reinterpret_cast<void*>(&FakeLeverBounds));
        table.changes=rva(&specialChanges);table.ranges=rva(&specialRanges);table.commands=rva(&specialCommands);g_switchDll=&table;
        dll.anims=rva(&specialAnimPtr);dll.objects=rva(leverObjects);dll.phdMxptr=rva(&leverMatrixPtr);dll.los=rva(reinterpret_cast<void*>(&FakeLeverSight));dll.getFloor=rva(reinterpret_cast<void*>(&FakeLeverFloor));
        g_hSwitchCollision.m_trampoline=reinterpret_cast<void*>(&SpecialCollision);g_hSwitchTestPosition.m_trampoline=g_hSwitchMovePosition.m_trampoline=reinterpret_cast<void*>(&FakeLeverPosition);
        std::memset(leverObjects,0,sizeof(leverObjects));std::memset(leverItem,0,sizeof(leverItem));
        auto* palm=reinterpret_cast<float*>(leverObjects+192)+10*16;palm[0]=palm[5]=palm[10]=palm[15]=1;
        auto* object=leverObjects+profile.slot*2304;*reinterpret_cast<int16_t*>(object)=int16_t(profile.joints);
        *reinterpret_cast<int16_t*>(object+72)=int16_t(specialBase);*reinterpret_cast<const char**>(object+88)=profile.name;
        *reinterpret_cast<Fn_SwitchCollision*>(object+64)=SpecialCollision;*reinterpret_cast<Fn_SwitchControl*>(object+24)=SpecialControl;
        for(int j=0;j<profile.joints;++j) {auto* b=reinterpret_cast<float*>(object+192)+16*j;b[0]=b[5]=b[10]=b[15]=1;if(j){b[12]=-specialBone.x;b[13]=-specialBone.y;b[14]=-specialBone.z;}}
        const int idle=specialBase+(profile.wheel ? state : 1-state);
        LeverField<int16_t>(16)=int16_t(profile.slot);LeverField<int16_t>(18)=int16_t(state);LeverField<int16_t>(24)=int16_t(idle);LeverField<int16_t>(26)=specialAnims[idle].first;
        const auto* bounds=specialAnims[idle].frames;std::memcpy(leverBounds,bounds,12);
        auto& pos=LeverField<PHD_3DPOS>(88);pos.y_rot=int16_t(yaw*16384);Field<int16_t>(24)=profile.game==2 ? 11 : 103;
        Field<PHD_3DPOS>(88)=pos;g_scenePose={0,-700,-100,0,0,0,0};g_scenePoseValid=true;leverSightBlocked=false;
        testControllerOffset[1]={};testMissingHand=-1;leverMatrixPtr=leverStack;
        Check(SpecialSwitch(leverItem)==&profile,"special slot/mesh/handlers match");
        motiongun::Vec hand{},local{},grip{};SwitchHand(hand);
        // Place the hand on the actual mesh handle/rim via authored matrices.
        SwitchPoseMatrices(leverItem,{int16_t(idle),specialAnims[idle].first,int16_t(state)});
        auto frame=SwitchJoint(leverItem,profile.joint);motiongun::Frame bind{},inv{};ReadIKBind(object,profile.joints,0,nullptr,nullptr,profile.joint,bind);motiongun::Inverse(bind,inv);
        auto mesh=motiongun::Vec{profile.wheel ? profile.x+profile.radius : 0,profile.y,profile.z};
        auto contact=motiongun::Transform(frame,motiongun::Transform(inv,mesh));auto d=Sub(contact,hand);testControllerOffset[1]={d.x/1000,d.y/1000,d.z/1000};SwitchHand(hand);
        uint64_t now=GetTickCount64();float distance=0;
        Check(SwitchReach(leverItem,item,hand,distance,false),"visible handle/rim is reachable without icon");
        leverSightBlocked=true;Check(FindPhysicalSwitch(item,hand,now).index<0,"wall blocks special acquisition");leverSightBlocked=false;
        LeverField<uint16_t>(484)=4;Check(!SwitchReach(leverItem,item,hand,distance,false),"native inactive/used status blocks acquisition");LeverField<uint16_t>(484)=0;
        auto* nameField=reinterpret_cast<const char**>(object+88);*nameField="UNSUPPORTED";
        Check(!SpecialSwitch(leverItem),"unknown special mesh retains native controls");*nameField=profile.name;
        auto& extraBones=*reinterpret_cast<int32_t*>(object+88+28);extraBones=1;
        Check(!SpecialSwitch(leverItem),"independent skeleton retains native controls");extraBones=0;
        g_active=false;Check(!UpdatePhysicalSwitchGrip(true,true,now),"third-person mode cannot claim special grip");g_active=true;g_switchGripDown=false;

        if(!UpdatePhysicalSwitchGrip(true,true,++now)) {
            auto candidate=FindPhysicalSwitch(item,hand,now);
            std::fprintf(stderr,"grab failed %s state=%d yaw=%d ready=%d candidate=%d begin=%d paths=%d joint=%d\n",profile.name,state,yaw,PhysicalSwitchReady(),candidate.index,BeginSwitchPreview(leverItem,hand,now),g_switchPreview.path.count,g_switchPreview.joint);
            Check(false,"Right Grip acquires special object");
        }
        Check(g_switchPreview.active && g_switchPreview.valve==profile.wheel && leverMatrixPtr==leverStack,"authored trajectory built and stack restored");
        const auto originHand=hand,originalOffset=testControllerOffset[1];
        auto move=[&](int i){const auto target=Sub(g_switchPreview.points[i],g_switchPreview.handOffset);auto shift=Sub(target,originHand);testControllerOffset[1]=Add(originalOffset,{shift.x/1000,shift.y/1000,shift.z/1000});AdvancePhysicalSwitch(item,nullptr,++now);};
        specialActivations=specialTicks=specialControls=0;
        const int halfway=profile.wheel ? g_switchPreview.path.count*3/4 : g_switchPreview.path.count/2;
        for(int i=0;i<=halfway;++i) move(i);
        Check(!specialActivations && g_switchPreview.active,"partial hand travel does not activate");
        const float forward=g_switchPreview.valveTurn.progress;
        for(int i=halfway;i>=halfway-8;--i) move(i);
        if(profile.wheel) Check(g_switchPreview.valveTurn.progress<forward,"reverse hand motion unwinds partial wheel turn");
        for(int i=halfway-8;i<=halfway;++i) move(i);
        const auto held=LeverField<int16_t>(26);UpdatePhysicalSwitchGrip(false,true,++now);
        Check(g_parkedSwitches.size()==1 && LeverField<int16_t>(26)==held,"release retains partial progress");
        if(profile.wheel) {
            motiongun::Vec contactLocal{},newGrip{};
            const auto current=SwitchJoint(leverItem,0);
            const auto opposite=motiongun::Transform(current,{profile.x,profile.y+profile.radius,profile.z});
            const auto shift=Sub(opposite,originHand);
            testControllerOffset[1]=Add(originalOffset,{shift.x/1000,shift.y/1000,shift.z/1000});
        } // Regrip a different quadrant without changing accumulated rotation.
        Check(UpdatePhysicalSwitchGrip(true,true,++now),"regrab resumes special object");
        Check(LeverField<int16_t>(26)==held,"regrab leaves native pose unchanged");
        uint8_t savedItem[sizeof(item)],savedLara[432];std::memcpy(savedItem,item,sizeof(item));std::memcpy(savedLara,Ptr<uint8_t>(dll.lara),432);
        for(int i=g_switchPreview.selected;i<g_switchPreview.path.count && !g_switchCompleted;++i) move(i);
        if(!g_switchCompleted) std::fprintf(stderr,"incomplete %s state %d progress %f count %d selected %d\n",profile.name,state,g_switchPreview.valveTurn.progress,g_switchPreview.path.count,g_switchPreview.selected);
        Check(g_switchCompleted && specialActivations==1,"physical endpoint activates exactly once");
        Check(specialControls==1,"native timer/control initializes once");
        Check(!std::memcmp(savedItem,item,sizeof(item)) && !std::memcmp(savedLara,Ptr<uint8_t>(dll.lara),432),"native use preserves Lara pose and flags");
        if(profile.wheel && profile.game==2) Check(LeverField<int16_t>(24)==specialBase+1,"TR2 post-use movement left to native ticks");
        else Check((LeverField<uint16_t>(484)&6)==4,"native deactivation command completed");
        AdvancePhysicalSwitch(item,nullptr,++now);Check(specialActivations==1,"held grip cannot repeat activation");
        ResetPhysicalSwitch();g_switchDll=nullptr;
    }
    g_hSwitchCollision.m_trampoline=g_hSwitchTestPosition.m_trampoline=g_hSwitchMovePosition.m_trampoline=nullptr;
}
}
int main(int argc,char** argv) {if(argc!=2)return 2;TestPhysicalSwitch();TestButtons();TestSpecials(argv[1]);std::printf("PASS: %d focused interaction checks (TR1-3)\n",checks);}
