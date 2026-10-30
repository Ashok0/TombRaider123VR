namespace {
float autoJumpYaw=0;
int autoJumpEdge=4096,autoJumpDrop=1024,autoJumpLaunch=-1,autoJumpQueries=0;
bool autoJumpMissing=false,autoJumpReject=false,autoJumpNarrow=false;
void* __cdecl AutoJumpFloor(int32_t,int32_t,int32_t,int16_t* room) {
    ++autoJumpQueries;*room=9;return autoJumpMissing ? nullptr : item;
}
int32_t __cdecl AutoJumpHeight(void*,int32_t x,int32_t,int32_t z) {
    const float travel=x*std::sin(autoJumpYaw)+z*std::cos(autoJumpYaw);
    return travel>=autoJumpEdge && (!autoJumpNarrow || travel<autoJumpEdge+60) ? autoJumpDrop : 0;
}
void __cdecl NativeAutoJumpTick(uint8_t* target,void*) {
    ++ticks;++collisions;
    auto& state=Field<int16_t>(off::item_anim_state);
    auto& frame=Field<int16_t>(off::item_frame_number);
    auto& body=Field<PHD_3DPOS>(off::item_pos);
    jumpObservedInput=*Ptr<uint32_t>(dll.input);
    if(state==1 && (jumpObservedInput&0x10) && !autoJumpReject) {
        state=Field<int16_t>(off::item_goal_state)=3;
        Field<int16_t>(off::item_anim_number)=16;frame=360;
    } else if(state==3) ++frame;
    if(state==3 && frame==367) {
        autoJumpLaunch=int(std::lround(body.x_pos*std::sin(autoJumpYaw)+body.z_pos*std::cos(autoJumpYaw)));
        Field<uint16_t>(off::item_flags)|=8;
    }
    // Native run and run-up clips move 47 units per tick until the jump command.
    if(autoJumpLaunch<0 && state!=2) {
        const float yaw=Radians(body.y_rot);
        body.x_pos+=(int(std::sin(yaw)*16384.f)*47)>>14;
        body.z_pos+=(int(std::cos(yaw)*16384.f)*47)>>14;
    }
    Check(target==item,"automatic jump calls Lara's native simulation");
}
void AutoJumpSetup(int game,bool first) {
    Reset(1,{0,1},true);EnableAnimations(game);
    dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
    testConfigFlags=2;
    if(!first) { g_active=g_runtimeEnabled=g_haveHeading=false;g_headingItem=nullptr; }
    // Synthetic globals live above the executable's callbacks. Rebase both
    // into one positive-RVA range, as a real loaded game DLL would provide.
    constexpr uint32_t bias=0x10000000;
    g_boundBase-=bias;
    for(size_t at=offsetof(GameDllLayout,lara);at<sizeof(GameDllLayout);at+=sizeof(uint32_t)) {
        auto& address=*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(&dll)+at);
        if(address) address+=bias;
    }
    dll.getFloor=uint32_t(reinterpret_cast<uint64_t>(&AutoJumpFloor)-g_boundBase);
    dll.getHeight=uint32_t(reinterpret_cast<uint64_t>(&AutoJumpHeight)-g_boundBase);
    for(int index:{16,18}) {
        animationTable[index].state=3;animationTable[index].velocity=47<<16;
        animationTable[index].first=360;animationTable[index].last=366;
    }
    autoJumpYaw=0;autoJumpEdge=4096;autoJumpDrop=1024;autoJumpLaunch=-1;autoJumpQueries=0;
    autoJumpMissing=autoJumpReject=autoJumpNarrow=false;
    g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeAutoJumpTick);
}
void AutoJumpPoll(bool down=true,float x=0,float y=1) {
    float r=0;FirstPersonInput(x,y,r,false,down);
    *Ptr<uint32_t>(dll.input)=locomotion::Forward|(down ? 0x10u : 0u)|0x80000000u;
}
void TestAutomaticJump() {
    for(int game:{1,2,3}) for(bool first:{false,true})
    for(float yaw:{0.f,.65f,-.65f,1.5707963f,-1.5707963f,2.4f,-2.4f,3.1415926f})
    for(int edge:{1024,2048,4096}) {
        AutoJumpSetup(game,first);autoJumpEdge=edge;autoJumpYaw=yaw;
        g_heading.base=yaw;Field<PHD_3DPOS>(off::item_pos).y_rot=Angle(yaw);
        for(int tick=0;tick<110 && autoJumpLaunch<0;++tick) {
            AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);
            Check((*Ptr<uint32_t>(dll.input)&0x80000010u)==0x80000010u,
                  "edge waiting restores physical Jump and preserves other input bits");
            if(!tick) {
                Check(Field<int16_t>(18)==1 && g_groundJump.edgeWaiting,"held Jump waits far from the edge");
                g_groundJump.until=TurnTime()-1;
            }
        }
        Check(autoJumpLaunch>=0 && edge-autoJumpLaunch>0 && edge-autoJumpLaunch<=49,
              "TR1-3 held Jump launches in the last supported running tick across views and headings");
        Check(g_groundJump.edgeBlocked && !g_groundJump.edgeWaiting,"takeoff consumes a long automatic hold");
        Check(Field<int16_t>(off::item_room_number)==0,"edge queries never change Lara's room");
        Field<int16_t>(18)=Field<int16_t>(20)=1;Field<int16_t>(24)=0;Field<uint16_t>(off::item_flags)=0;
        AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);
        Check(!(jumpObservedInput&0x10),"holding through landing does not queue another jump");
    }
    for(int game:{1,2,3}) for(int terrain:{0,256,384,-512,-32512,1024}) {
        AutoJumpSetup(game,true);autoJumpEdge=200;autoJumpDrop=terrain;autoJumpMissing=terrain==1024;
        AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);
        Check(bool(jumpObservedInput&0x10)==(terrain==-32512),"only a verified drop triggers auto jump, not a step/wall/missing floor query");
    }
    for(int game:{1,2,3}) for(int mode=0;mode<6;++mode) {
        AutoJumpSetup(game,true);AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);
        if(mode==1) g_groundJump.until=TurnTime()-1;
        AutoJumpPoll(mode>=2,mode==3 ? 1.f : 0.f,mode==2 || mode==3 ? 0.f : 1.f);
        if(mode==4) g_groundJump.Bind(item,999);
        if(mode==5) {testWater=1;g_groundJump.Observe(true,false,TurnTime());testWater=0;}
        Detour_LaraAboveWater(item,nullptr);
        Check(bool(jumpObservedInput&0x10)==(mode==0),"quick release jumps; long release, stop, side movement, level and water cancel hold");
        if(mode==3) Check((jumpObservedInput&Directions)==StepRight,"cancelled hold preserves native sidestep movement");
    }
    AutoJumpSetup(1,true);autoJumpEdge=200;autoJumpNarrow=true;
    AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);
    Check(jumpObservedInput&0x10,"sampling whole run-up detects a narrow gap with solid ground beyond it");
    AutoJumpSetup(1,true);autoJumpEdge=350;autoJumpReject=true;
    AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);
    Check(g_groundJump.edgeWaiting && !g_groundJump.edgeBlocked,"native clearance rejection retains queued hold");
    AutoJumpSetup(1,true);Field<int16_t>(18)=Field<int16_t>(20)=2;Field<int16_t>(24)=103;
    AutoJumpPoll();Detour_LaraAboveWater(item,nullptr);
    Check(Field<int16_t>(18)==2 && !(jumpObservedInput&0x10),"forward Jump from standing waits for the run loop");
    for(int fallback=0;fallback<5;++fallback) {
        AutoJumpSetup(3,false);
        if(fallback==0) testConfigFlags=0;
        if(fallback==1) dll.getHeight=0;
        if(fallback==2) animationTable[16].last++;
        if(fallback==3) animationTable[18].velocity=48<<16;
        AutoJumpPoll();if(fallback==4) *Ptr<uint32_t>(dll.input)|=Walk;
        Detour_LaraAboveWater(item,nullptr);
        Check((jumpObservedInput&0x10) && !g_groundJump.edgeWaiting,"Tank Controls, unsupported tables and Walk retain manual/native Jump");
    }
    Reset(2,{},true);
}
}
