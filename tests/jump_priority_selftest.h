namespace {
uint32_t jumpObservedInput=0;
int jumpObservedAnimation=-1;
void __cdecl NativeJumpConsumer(uint8_t* target,void*) {
    Check(target==item,"jump calls native movement for Lara");
    ++ticks;++collisions;
    jumpObservedInput=*Ptr<uint32_t>(dll.input);
    jumpObservedAnimation=Field<int16_t>(off::item_anim_number);
    if (!(jumpObservedInput&0x10u)) return;
    // Native lara_as_stop reads Jump; lara_as_walk never does. Running loop
    // dispatches at every frame, whereas run start/stop have no jump dispatch.
    if (Field<int16_t>(off::item_anim_state)==2 && floorAllowsEntry) {
        Field<int16_t>(off::item_anim_state)=15;
        Field<int16_t>(off::item_goal_state)=15;
        Field<int16_t>(off::item_anim_number)=73;
    } else if (Field<int16_t>(off::item_anim_state)==1 && jumpObservedAnimation==0) {
        Field<int16_t>(off::item_anim_state)=3;
        Field<int16_t>(off::item_goal_state)=3;
        Field<int16_t>(off::item_anim_number)=16;
    }
}
// Model the actual GetChange frame window, not instant success whenever
// lara_as_stop requests compression. TR3 standing entry ends at frame 186;
// incrementing that final frame misses its 185..186 dispatch window.
void __cdecl NativeStopFrameConsumer(uint8_t*,void*) {
    ++ticks;++collisions;
    auto& state=Field<int16_t>(off::item_anim_state);
    auto& goal=Field<int16_t>(off::item_goal_state);
    auto& animation=Field<int16_t>(off::item_anim_number);
    auto& frame=Field<int16_t>(off::item_frame_number);
    Check(state==2,"stop handoff reaches native standing control");
    goal=(*Ptr<uint32_t>(dll.input)&0x10u) ? 15 : 2;
    ++frame;
    if (goal==15 && (animation==103 || (animation==11 && frame>=185 && frame<=186))) {
        state=15;animation=73;
    } else if (frame>animationTable[animation].last) {
        const auto outgoing=animationTable[animation];
        animation=outgoing.jump;frame=outgoing.jumpFrame;
    }
}
void TestStopJumpHandoff() {
    const struct { int state,animation,last; } stops[]={
        {0,2,73},{0,3,89},{1,8,146},{1,10,184},{2,11,0},{2,103,0}};
    for (int game:{1,2,3}) for (bool firstPerson:{false,true})
    for (const auto& stop:stops) for (int framesLeft:{0,1,2})
    for (int goal:{2,15}) for (bool tap:{false,true}) {
        Reset(stop.state,{},true);EnableAnimations(game);
        const int animation=stop.animation;
        dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
        if (!firstPerson) { g_active=g_runtimeEnabled=g_haveHeading=false;g_headingItem=nullptr; }
        Field<int16_t>(24)=int16_t(animation);
        Field<int16_t>(26)=int16_t((stop.last ? stop.last : animationTable[animation].last)-framesLeft);
        Field<int16_t>(20)=int16_t(goal);
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeStopFrameConsumer);
        float x=0,y=0,rx=0;
        FirstPersonInput(x,y,rx,false,true);
        if (tap) FirstPersonInput(x,y,rx,false,false);
        *Ptr<uint32_t>(dll.input)=tap ? 0 : 0x10u;
        if (!firstPerson) for (int eye=0;eye<6;++eye) UpdateGroundJumpCameraContext(item);
        Detour_LaraAboveWater(item,nullptr);
        Check(Field<int16_t>(18)==15,
              "released A at final stop-to-idle frame starts compression without losing the pending request");
        Check(!g_groundJump.pending && bool(*Ptr<uint32_t>(dll.input)&0x10u)==!tap,
              "actual compression consumes buffer and injected input does not stick");
        Check(ticks==1 && collisions==1,"late stop jump advances native simulation once");
    }
    Reset(2,{},true);EnableAnimations(3);Field<int16_t>(24)=11;
    Field<int16_t>(26)=186;Field<int16_t>(20)=15;
    g_groundJump.Observe(true,true,TurnTime());
    {
        GroundJumpInputScope scope(item,TurnTime());
        // A native handler has requested compression but GetChange has not
        // entered it. Cleanup must not mistake that goal for an actual jump.
    }
    Check(g_groundJump.pending,"a pending jump goal is not proof of takeoff");
    Field<int16_t>(18)=15;
    { GroundJumpInputScope scope(item,TurnTime()); }
    Check(!g_groundJump.pending,"actual jump state cancels the ground buffer");
    Reset(2,{},true);EnableAnimations(3);Field<int16_t>(24)=11;
    g_active=g_runtimeEnabled=g_haveHeading=false;g_headingItem=nullptr;
    g_groundJump.Observe(true,true,TurnTime());
    for (int eye=0;eye<6;++eye) UpdateGroundJumpCameraContext(item);
    Check(g_groundJump.pending && g_groundJump.held,"third-person eye draws preserve both tap and press edge");
    *Ptr<int32_t>(dll.camera+off::camera_type)=kCamFixed;
    UpdateGroundJumpCameraContext(item);
    Check(!g_groundJump.pending && g_groundJump.held,"scripted camera cancels buffer without inventing another A press");
}
void TestJumpPriority() {
    TestStopJumpHandoff();
    const struct { int state,animation; } gaits[]={
        {1,0},{1,6},{1,8},{1,10},{0,1},{0,2},{0,3},{0,4},{0,5},
        {0,7},{0,9},{0,20},{0,21},{2,11},{2,103},{16,38},{16,39},
        {16,40},{16,41},{22,65},{22,66},{21,67},{21,68}};
    for (int game:{1,2,3}) for (bool firstPerson:{false,true})
    for (bool modern:{false,true}) for (const auto& from:gaits)
    for (const Vec direction:{Vec{0,1},Vec{0,-1},Vec{-1,0},Vec{1,0},Vec{}})
    for (bool tap:{false,true}) {
        Reset(from.state,direction,true); EnableAnimations(game);
        dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
        testConfigFlags=modern ? 2 : 0;
        if (!firstPerson) { g_active=g_runtimeEnabled=g_haveHeading=false;g_headingItem=nullptr; }
        Field<int16_t>(off::item_anim_number)=int16_t(from.animation);
        Field<int16_t>(off::item_frame_number)=7;
        Field<int16_t>(off::item_goal_state)=2; // A during a pending walking stop.
        Field<int16_t>(off::item_speed)=47;
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeJumpConsumer);
        const auto before=Field<PHD_3DPOS>(off::item_pos);
        float x=direction.x,y=direction.z,rx=0;
        FirstPersonInput(x,y,rx,false,true);
        if (tap) FirstPersonInput(x,y,rx,false,false);
        *Ptr<uint32_t>(dll.input)=(tap ? 0x40u : 0x50u) |
            (firstPerson ? 0 : MovementAction(direction,true)); // Native third-person directions.
        Detour_LaraAboveWater(item,nullptr);
        const bool running=from.state==1 && from.animation==0;
        Check(Field<int16_t>(off::item_anim_state)==(running ? 3 : 15),
              "A immediately supersedes an ordinary walking/gait transition");
        Check((jumpObservedInput&0x50u)==0x50u,"quick tap survives release and retains grab");
        Check((jumpObservedInput&Directions)==(MovementAction(direction,true)&Directions),
              "buffered jump uses native forward/back/side jump directions");
        Check(ticks==1 && collisions==1,"priority jump advances native simulation exactly once");
        Check(!g_stabilizeRoot && !g_hardStopRoot && !g_groundMoveAction,
              "jump cannot inherit boosted gait motion or ground braking");
        Check(std::memcmp(&before,&Field<PHD_3DPOS>(off::item_pos),sizeof(before))==0,
              "jump input never teleports Lara or changes gravity");
        Check(!g_groundJump.pending,"native compression/takeoff consumes the queued tap");
        Check(bool(*Ptr<uint32_t>(dll.input)&0x10u)==!tap,"only synthetic Jump is removed afterward");
        if (running) Check(jumpObservedAnimation==0 && Field<int16_t>(off::item_speed)==47,
                          "running jump keeps its native animation and momentum");
    }
    for (int reason=0;reason<19;++reason) {
        Reset(0,{0,1},true);EnableAnimations(1);Field<int16_t>(24)=1;
        if (reason==0) Field<uint16_t>(off::item_flags)=8;
        if (reason==1) { Field<int16_t>(18)=3;Field<int16_t>(20)=3; }
        if (reason==2) Field<int16_t>(24)=12; // authored step up
        if (reason==3) { Field<int16_t>(18)=2;Field<int16_t>(24)=24; }
        if (reason==4) Field<int16_t>(22)=19;
        if (reason==5) Field<int16_t>(20)=19;
        if (reason==6) testWater=1;
        if (reason==7) Field<int16_t>(38)=0;
        if (reason==8) *Ptr<uint16_t>(dll.lara+60)=0x2000;
        if (reason==9) testConfig.enabled=false;
        if (reason==10) g_shifted=true;
        if (reason==11) *Ptr<int32_t>(dll.camera+off::camera_type)=kCamFixed;
        if (reason==12) *Ptr<uint32_t>(dll.input)=0x100;
        if (reason==13) *Ptr<uint32_t>(dll.input)=0x1000;
        if (reason==14) dll.anims=0;
        if (reason==15) *Ptr<TestAnim*>(dll.anims)=nullptr;
        if (reason==16) animationTable[11].commands=1;
        if (reason==17) animationTable[11].state=3;
        if (reason==18) { dll.module=L"tomb3.dll";*Ptr<uint16_t>(dll.lara+60)=4; }
        *Ptr<uint32_t>(dll.input)|=0x10;
        uint8_t before[sizeof(item)];std::memcpy(before,item,sizeof(item));
        { GroundJumpInputScope scope(item,TurnTime());scope.Prepare(); }
        Check(!std::memcmp(before,item,sizeof(item)),"jump priority preserves special poses, drops and invalid tables");
    }
    GroundJumpIntent intent;
    intent.Observe(true,true,1);intent.Observe(false,true,1.01);
    Check(intent.Wants(1.02),"press/release between simulation ticks is retained");
    Check(!intent.Wants(1.26),"stale tap expires rather than firing after a pause");
    intent.Observe(true,false,2);intent.Observe(true,true,2.01);
    Check(!intent.Wants(2.02),"holding A through an airborne/ineligible state cannot queue another jump");
    intent.Observe(false,true,2.03);intent.Observe(true,true,2.04);
    Check(intent.Wants(2.05),"a fresh grounded press rearms jump");
    intent.Observe(false,false,2.06);
    Check(!intent.Wants(2.07),"leaving ordinary grounded motion cancels buffered Jump");
    intent.Bind(item,1);intent.Observe(true,true,3);
    intent.Bind(item,2);
    Check(!intent.Wants(3.01),"level change cannot inherit a jump request");
    intent.Observe(false,true,3.02);intent.Observe(true,true,3.03);
    intent.Bind(nullptr,2);
    Check(!intent.Wants(3.04),"changing Lara cancels a queued jump");
    Reset(0,{0,1},true);EnableAnimations(1);Field<int16_t>(24)=1;
    floorAllowsEntry=false;g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeJumpConsumer);
    *Ptr<uint32_t>(dll.input)=0x10;
    Detour_LaraAboveWater(item,nullptr);
    Check(Field<int16_t>(18)==2 && !(Field<uint16_t>(off::item_flags)&8),
          "native clearance can still refuse the jump without synthetic takeoff");
}
}
