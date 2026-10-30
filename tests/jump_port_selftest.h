namespace {
#include "jump_port_assets.h"
int portAnimations=0,portCommands=0,portCompression=0;
int portFloor=0,portCeiling=-1024;
bool portDirectionClear=true,portLand=false;
void __cdecl PortClearance(tr::RoomCollision* c,int32_t,int32_t,int32_t,int16_t,int32_t height) {
    Check(height==762 && c->radius==100,"jump preparation queries the native standing hull");
    c->floorSamples[0]=portFloor;c->floorSamples[1]=portCeiling;
}
void __cdecl PortCompression(uint8_t* target,void*) {
    using namespace tr;using namespace tr::locomotion;
    Check(target==item,"compression controls the current Lara only");++portCompression;
    if(!portDirectionClear) return;
    const uint32_t bits=*Ptr<uint32_t>(dll.input);
    const int goal=(bits&Forward)?3:(bits&Left)?27:(bits&Right)?26:(bits&Back)?25:15;
    Field<int16_t>(20)=int16_t(goal);
    const float yaw=Radians(Field<PHD_3DPOS>(off::item_pos).y_rot);
    *Ptr<int16_t>(dll.lara+254)=Angle(yaw+(goal==25?Pi:goal==27?-Pi/2:goal==26?Pi/2:0));
}
void __cdecl PortAnimate(uint8_t*) {
    using namespace tr;using namespace tr::locomotion;
    ++portAnimations;
    auto& state=Field<int16_t>(18);auto& goal=Field<int16_t>(20);
    auto& clip=Field<int16_t>(24);auto& frame=Field<int16_t>(26);
    auto enter=[&](int next,int at) {clip=int16_t(next);state=animationTable[next].state;frame=int16_t(at);};
    ++frame;
    if(state==2 && goal==15) enter(73,animationTable[73].first);
    else if(state==1 && goal==3) {const int next=frame<=11?18:16;enter(next,animationTable[next].first);}
    if(frame>animationTable[clip].last) {
        const int old=clip;const auto a=animationTable[clip];
        if(old==16 || old==18 || old==74 || old==76 || old==78 || old==80 || old==91) {
            ++portCommands;Field<uint16_t>(off::item_flags)|=8;
            Field<int16_t>(34)=old==91?2:old==16 || old==18?75:50;
            Field<int16_t>(36)=old==91?-110:-100;
        }
        enter(a.jump,a.jumpFrame);
    }
    if(Field<uint16_t>(off::item_flags)&8) {
        auto& p=Field<PHD_3DPOS>(off::item_pos);auto& fallSpeed=Field<int16_t>(36);
        fallSpeed+=6;p.y_pos+=fallSpeed;
        const float yaw=Radians(*Ptr<int16_t>(dll.lara+254));const int speed=Field<int16_t>(34);
        p.x_pos+=(int(std::sin(yaw)*16384)*speed)>>14;
        p.z_pos+=(int(std::cos(yaw)*16384)*speed)>>14;
    }
}
void __cdecl PortAboveWater(uint8_t* target,void*) {
    using namespace tr;using namespace tr::locomotion;
    ++ticks;
    auto& state=Field<int16_t>(18);auto& goal=Field<int16_t>(20);
    auto& bits=*Ptr<uint32_t>(dll.input);auto* axes=Ptr<int16_t>(dll.analogInput);
    const int status=*Ptr<int16_t>(dll.lara+2),weapon=*Ptr<int16_t>(dll.lara+4),game=BoundGame();
    const bool armed=status==4 || (status==2 && (game==1 || weapon!=(game==2?8:9)));
    // Verified in native TR1/2/3: STOP's armed steering injects a turn bit
    // before compression, which prioritizes Left/Right over Back.
    if(state==2 && armed && (axes[0] || axes[1])) {
        const float delta=Wrap(std::atan2(float(axes[0]),float(axes[1]))+
            Radians(axes[2])-Radians(Field<PHD_3DPOS>(off::item_pos).y_rot));
        if(std::fabs(delta)>.02f) bits|=delta>0?Right:Left;
    }
    if(state==15) PortCompression(target,nullptr);
    else if((state==1 || state==2) && (bits&0x10u)) goal=state==1?3:15;
    Detour_AnimateLara(target);++collisions;
    if(portLand) {
        Field<uint16_t>(off::item_flags)&=~8u;goal=2;
        Field<PHD_3DPOS>(off::item_pos).y_pos=0;
    }
}
void PortSetup(int game,int state,Vec direction,float heading=0) {
    using namespace tr;
    Reset(state,direction,true,heading);EnableAnimations(game);testHeadYaw=0;
    dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
    for(int i=0;i<104;++i) {
        const auto& a=kJumpAssets[game-1][i];auto& b=animationTable[i];b={};
        b.state=int16_t(a.state);b.first=int16_t(a.first);b.last=int16_t(a.last);
        b.jump=int16_t(a.jump);b.jumpFrame=int16_t(a.jumpFrame);b.commands=int16_t(a.commands);
        b.velocity=a.velocity;b.acceleration=a.acceleration;
    }
    constexpr uint32_t bias=0x10000000;g_boundBase-=bias;
    for(size_t at=offsetof(GameDllLayout,lara);at<sizeof(GameDllLayout);at+=4) {
        auto& address=*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(&dll)+at);
        if(address) address+=bias;
    }
    dll.getCollisionInfo=uint32_t(reinterpret_cast<uint64_t>(&PortClearance)-g_boundBase);
    g_nativeJumpCompression=&PortCompression;
    g_hAnimateLara.m_trampoline=reinterpret_cast<void*>(&PortAnimate);
    g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&PortAboveWater);
    testConfigFlags=2;testConfig.gamepadEnabled=true;
    Field<int16_t>(24)=state==1?0:state==15?73:103;
    Field<int16_t>(26)=animationTable[Field<int16_t>(24)].first;
    Field<int16_t>(20)=int16_t(state);
    portAnimations=portCommands=portCompression=0;portFloor=0;portCeiling=-1024;
    portDirectionClear=true;portLand=false;
}
void TestJumpPort() {
    using namespace tr;using namespace tr::locomotion;
    for(int game:{1,2,3}) for(bool first:{false,true}) for(bool armed:{false,true})
    for(int start:{2,15}) for(Vec direction:{Vec{},Vec{0,1},Vec{0,-1},Vec{-1,0},Vec{1,0}})
    for(float heading:{0.f,.7f,-2.9f}) {

        PortSetup(game,start,direction,heading);
        *Ptr<int16_t>(dll.lara+2)=armed?4:0;*Ptr<int16_t>(dll.lara+4)=1;
        auto* axes=Ptr<int16_t>(dll.analogInput);axes[0]=int16_t(direction.x*20000);axes[1]=int16_t(direction.z*20000);
        axes[2]=axes[3]=Angle(heading);
        *Ptr<uint32_t>(dll.input)=0x80000010u|MovementAction(direction,true);
        if(!first) {g_active=g_runtimeEnabled=g_haveHeading=false;g_headingItem=nullptr;}
        Detour_LaraAboveWater(item,nullptr);
        if(first) {
            const int expected=direction.x<0?79:direction.x>0?81:direction.z<0?75:direction.z>0?77:28;
            Check(Field<int16_t>(24)==expected && (Field<uint16_t>(off::item_flags)&8),
                  "TR1-3 first-person native airborne animation starts on the input tick");
            Check(portCommands==1 && portCompression==1,"native jump selection and takeoff commands execute once");
            if(Length(direction)>0) {
                const float yaw=MovementYaw(direction,heading);const auto& p=Field<PHD_3DPOS>(off::item_pos);
                Check(std::fabs(p.x_pos-std::sin(yaw)*50)<2 && std::fabs(p.z_pos-std::cos(yaw)*50)<2,
                      "armed/unarmed back and side jumps travel in the requested world direction");
                Check(std::hypot(float(axes[0]),float(axes[1]))>19000,"takeoff restores live analog steering");
            }
        } else Check(!(Field<uint16_t>(off::item_flags)&8) && portCommands==0,
                     "third person keeps native compression timing");
        Check(ticks==1 && collisions==1 && portAnimations==1,"jump runs exactly one simulation/collision tick");
        Check(*Ptr<uint32_t>(dll.input)&0x80000000u,"jump preserves unrelated native input");
    }
    for(int game:{1,2,3}) for(bool first:{false,true}) for(int stride:{0,10,11,21}) {
        PortSetup(game,1,{0,1});Field<int16_t>(26)=int16_t(stride);*Ptr<uint32_t>(dll.input)=0x11;
        if(!first) {g_active=g_runtimeEnabled=g_haveHeading=false;g_headingItem=nullptr;}
        Detour_LaraAboveWater(item,nullptr);
        Check(Field<int16_t>(24)==(stride+1<=11?(first?19:18):(first?17:16)),
              "running jump preserves native leading-leg animation and third-person timing");
        dll.getFloor=uint32_t(reinterpret_cast<uint64_t>(&AutoJumpFloor)-g_boundBase);
        dll.getHeight=uint32_t(reinterpret_cast<uint64_t>(&AutoJumpHeight)-g_boundBase);
        Check(RunningJumpLookahead()==(first?47:376),"ledge lookahead matches each camera mode's takeoff delay");
    }
    for(int game:{1,2,3}) for(int blocker=0;blocker<12;++blocker) {
        PortSetup(game,2,{0,-1});Field<int16_t>(20)=15;
        if(blocker==0) portCeiling=-99;
        if(blocker==1) portFloor=128;
        if(blocker==2) portFloor=-128;
        if(blocker==3) Field<uint16_t>(off::item_flags)=8;
        if(blocker==4) Field<int16_t>(22)=19;
        if(blocker==5) testWater=1;
        if(blocker==6) *Ptr<int16_t>(dll.lara+40)=1;
        if(blocker==7) animationTable[74].commands=0;
        if(blocker==8) g_active=false;
        if(blocker==9) g_nativeJumpCompression=nullptr;
        if(blocker==10) testInventory=1;
        if(blocker==11) Field<int16_t>(38)=0;
        const auto before=std::string(reinterpret_cast<char*>(item),sizeof(item));
        SkipFirstPersonJumpPreparation(item);
        Check(before==std::string(reinterpret_cast<char*>(item),sizeof(item)),"blocked/ineligible takeoff retains native animation and physics");
    }
    for(int game:{1,2,3}) for(bool first:{false,true}) for(int clip:{24,31,82,92,99}) {
        PortSetup(game,2,{});const auto a=animationTable[clip];
        Field<int16_t>(18)=a.state;Field<int16_t>(20)=clip==92?1:2;Field<int16_t>(24)=int16_t(clip);
        Field<int16_t>(26)=a.first;Field<int16_t>(36)=87;
        Field<PHD_3DPOS>(off::item_pos)={123,456,789};
        if(!first) g_active=false;
        const bool eligible=first && clip!=24 && !(game==1 && clip==99);
        Check(FinishFirstPersonLanding(item)==eligible,"soft landing recovery excludes hard falls, TR1 water exits and third person");
        Check(Field<int16_t>(24)==(eligible?(clip==92?0:103):clip),"soft landing enters stable idle or running loop immediately");
        const auto& p=Field<PHD_3DPOS>(off::item_pos);
        Check(p.x_pos==123 && p.y_pos==456 && p.z_pos==789 && Field<int16_t>(36)==87 && Field<int16_t>(38)==1000,
              "landing recovery leaves position, native velocity and damage untouched");
    }
    for(int game:{1,2,3}) {
        PortSetup(game,2,{0,-1});*Ptr<uint32_t>(dll.input)=0x12;portLand=true;
        Detour_LaraAboveWater(item,nullptr);
        // Contact on a later airborne tick is handled before the next input poll.
        Field<int16_t>(18)=25;Field<int16_t>(20)=2;Field<int16_t>(24)=75;Field<uint16_t>(off::item_flags)=8;
        *Ptr<uint32_t>(dll.input)=0;Detour_LaraAboveWater(item,nullptr);
        Check(Field<int16_t>(24)==103 && !(Field<uint16_t>(off::item_flags)&8),"native ground contact ends recovery on that tick");
        portLand=false;*Ptr<uint32_t>(dll.input)=0x12;g_jumpPressed=true;
        Detour_LaraAboveWater(item,nullptr);
        Check(Field<int16_t>(24)==75 && (Field<uint16_t>(off::item_flags)&8),"first post-contact jump needs no recovery wait");
    }
    for(int game:{1,2,3}) for(int camera:{0,1,3,4,5}) {
        AutoJumpSetup(game,true);UpdateControlMode(false);testConfigFlags=0;
        FirstPersonControlModeUpdate();Check(testConfigFlags&2,"first person owns Modern Controls over saved Tank preference");
        AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);g_groundJump.until=TurnTime()-1;
        *Ptr<int32_t>(dll.camera+off::camera_type)=camera;
        g_active=false;UpdateGroundJumpCameraContext(item);FirstPersonControlModeUpdate();
        if(camera==4) Check(!g_groundJump.edgeWaiting && !(testConfigFlags&2),"actual cinematic releases held jump and saved controls");
        else {
            Check(g_groundJump.edgeWaiting,"playable camera change retains held edge jump");
            if(camera==1 || camera==5) Check(testConfigFlags&2,"playable fixed/heavy override retains owned Modern Controls");
            *Ptr<int32_t>(dll.camera+off::camera_type)=0;g_haveHeading=false;g_previousBody={};
            dll.frameFrac=0x10000000+204;*Ptr<int32_t>(dll.frameFrac)=256;
            PHD_3DPOS eye{0,-700,80};UpdateLocomotion(eye);g_active=true;
            Check(g_groundJump.edgeWaiting && g_groundJump.held,"same-body camera recovery preserves expired-tap long hold");
            testConfigFlags=2;
            for(int tick=0;tick<110 && autoJumpLaunch<0;++tick) {AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);}
            Check(autoJumpLaunch>=0,"original held jump still launches after camera override and heading recovery");
        }
        UpdateControlMode(false);
    }
    Reset(2,{},true);
}
}
