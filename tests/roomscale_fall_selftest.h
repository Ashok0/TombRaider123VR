namespace {
tr::RoomCollision* roomscaleTestNative=nullptr;
int roomscaleNativeQueries=0;
int roomscaleObservedDrop=0,roomscaleObservedFlags=0;
bool roomscaleTestWall=false;
void __cdecl RoomscaleTerrain(tr::RoomCollision* coll,int32_t x,int32_t,int32_t,int16_t,int32_t) {
    // A platform ends at x=128. Native STOP regards a drop within its 100-unit
    // front radius as a wall. Small accepted drags must survive the SECOND
    // query in native collision to ever get the root beyond that edge.
    coll->floorSamples[0]=x>=128 ? 1024 : 0;
    roomscaleObservedDrop=coll->badPos;roomscaleObservedFlags=coll->flags;
    coll->floorSamples[1]=-2000;
    coll->shift[0]=coll->shift[1]=coll->shift[2]=0;coll->type=0;
    if(coll==roomscaleTestNative) ++roomscaleNativeQueries;
    if(roomscaleTestWall) { coll->type=8;coll->shift[0]=coll->old[0]-x; }
    else if(x<128 && x+coll->radius>=128 && coll->badPos<1024) {
        coll->type=1;coll->shift[0]=128-coll->radius-x;
    }
}
void __cdecl RoomscaleRoom(uint8_t*,int32_t) {}
void __cdecl RoomscaleNativeStop(uint8_t* target,void* raw) {
    using namespace tr;
    Check(target==item && raw==roomscaleTestNative,"native collision receives the original item and object");
    auto& pos=Field<PHD_3DPOS>(off::item_pos);
    if(Field<uint16_t>(off::item_flags)&8) { Field<int16_t>(36)+=6;pos.y_pos+=Field<int16_t>(36);return; }
    auto* coll=static_cast<RoomCollision*>(raw);
    coll->radius=100;coll->badPos=384;coll->badNeg=-384;coll->badCeiling=0;coll->flags=5;
    coll->facing=pos.y_rot;
    coll->old[0]=pos.x_pos;coll->old[1]=pos.y_pos;coll->old[2]=pos.z_pos;
    reinterpret_cast<Fn_GetCollisionInfo>(g_boundBase+dll.getCollisionInfo)(coll,pos.x_pos,pos.y_pos,pos.z_pos,0,762);
    // Relevant native fall branch: TR1 STOP tests >100; TR2 STOP and TR3's
    // LaraFallen test >384. With no modern auto-grab, each selects animation
    // 34, state/goal 3, zero fall speed and gravity bit 8.
    const int fallThreshold=dll.module[4]==L'1' ? 100 : 384;
    if(coll->floorSamples[0]>fallThreshold && coll->type!=8) {
        Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=3;
        Field<int16_t>(off::item_anim_number)=34;Field<int16_t>(36)=0;
        Field<uint16_t>(off::item_flags)|=8;
    } else pos.x_pos+=coll->shift[0];
    Check(coll->badPos==384 && coll->flags==5,"native collision policy restored after query");
}
void RoomscaleFixture(int game,bool fullBody) {
    using namespace tr;
    Reset(2,{},true);dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
    const auto base=reinterpret_cast<uint64_t>(GetModuleHandle(nullptr));
    const auto shift=uint32_t(reinterpret_cast<uint64_t>(module)-base);
    for(auto* field:{&dll.laraItem,&dll.camera,&dll.lara,&dll.input,&dll.analogInput}) *field+=shift;
    dll.getCollisionInfo=uint32_t(reinterpret_cast<uint64_t>(&Detour_GetCollisionInfo)-base);
    g_hGetCollisionInfo.m_trampoline=reinterpret_cast<void*>(&RoomscaleTerrain);
    dll.updateLaraRoom=uint32_t(reinterpret_cast<uint64_t>(&RoomscaleRoom)-base);g_boundBase=base;
    g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&RoomscaleNativeStop);
    Field<PHD_3DPOS>(off::item_pos).y_rot=16384;testHeadYaw=locomotion::Pi*.5f;
    testConfig.firstPersonRoomscaleMove=true;testConfig.firstPersonFullBodyIK=fullBody;
    g_dragCurrent=g_dragPrevious=g_dragShown={};roomscaleNativeQueries=0;roomscaleTestWall=false;
}
void TestRoomscaleNativeFall() {
    using namespace tr;
    for(int game:{1,2,3}) for(bool fullBody:{false,true}) for(bool patched:{false,true}) {
        RoomscaleFixture(game,fullBody);RoomCollision coll{};roomscaleTestNative=&coll;
        if(!patched) dll.getCollisionInfo=uint32_t(reinterpret_cast<uint64_t>(&RoomscaleTerrain)-g_boundBase);
        for(int tick=0;tick<30 && !(Field<uint16_t>(off::item_flags)&8);++tick) {
            // Pause after every small physical step. A zero-drag standing
            // query must not undo the preceding accepted step at the lip.
            const bool pause=tick%2!=0;
            testFloorRight=pause ? 0.f : fullBody ? .22f : .05f;
            g_dragShown=g_dragCurrent;
            LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&g_roomscaleFollowTime);
            g_roomscaleFollowTime.QuadPart-=frequency.QuadPart/30;
            const int before=Field<PHD_3DPOS>(off::item_pos).x_pos;
            Detour_LaraAboveWater(item,&coll);
            Check(Field<PHD_3DPOS>(off::item_pos).x_pos>=before,"standing collision must not pull physical steps back from the ledge");
        }
        if(!patched) {
            Check(!(Field<uint16_t>(off::item_flags)&8) && Field<PHD_3DPOS>(off::item_pos).x_pos==28,
                "unpatched native STOP pins the root at its front radius and reproduces the floating report");
            continue;
        }
        Check(roomscaleNativeQueries>1 && Field<int16_t>(off::item_anim_state)==3 &&
            (Field<uint16_t>(off::item_flags)&8),"slow physical steps reach native falling state in TR1, TR2 and TR3");
        Check(Field<PHD_3DPOS>(off::item_pos).y_pos==0,"roomscale hands off to gravity without snapping to the lower floor");
        const int x=Field<PHD_3DPOS>(off::item_pos).x_pos;
        Detour_LaraAboveWater(item,&coll);
        Check(Field<PHD_3DPOS>(off::item_pos).y_pos>0 && Field<PHD_3DPOS>(off::item_pos).x_pos==x,
            "native gravity continues on the following tick without airborne roomscale dragging");
    }
    for(int game:{1,2,3}) for(int guard=0;guard<15;++guard) {
        RoomscaleFixture(game,true);RoomCollision coll{},other{};
        coll.badPos=other.badPos=384;coll.flags=other.flags=0x1f;
        if(guard==1) testConfig.firstPersonRoomscaleMove=false;
        if(guard==2) testConfig.positionalTracking=false;
        if(guard==3) testConfig.firstPersonHeadTranslation=false;
        if(guard==4) g_manualLocal={.5f,0};
        if(guard==5) *Ptr<uint32_t>(dll.input)=locomotion::Forward;
        if(guard==6) *Ptr<uint32_t>(dll.input)=locomotion::Walk;
        if(guard==7) g_jumpPressed=true;
        if(guard==8) g_shifted=true;
        if(guard==9) testWater=1;
        if(guard==10) Field<int16_t>(off::item_hit_points)=0;
        if(guard==11) Field<uint16_t>(off::item_flags)|=8;
        if(guard==12) g_active=false;
        if(guard==13) testVR.m_poseValid=false;
        if(guard==14) Field<int16_t>(off::item_anim_state)=3;
        {
            RoomscaleCollisionScope scope(item,&coll);
            Detour_GetCollisionInfo(&other,60,0,0,0,762);
            Check(roomscaleObservedDrop==384 && roomscaleObservedFlags==0x1f,"other objects and private camera/sweep queries keep native policy");
            Detour_GetCollisionInfo(&coll,60,0,0,0,100);
            Check(roomscaleObservedDrop==384 && roomscaleObservedFlags==0x1f,"nonstanding collision capsule keeps native policy");
            Detour_GetCollisionInfo(&coll,60,0,0,0,762);
            Check(roomscaleObservedDrop==(guard==0?32512:384) && roomscaleObservedFlags==(guard==0?0x1b:0x1f),
                "only eligible stationary roomscale changes native drop and lava-pit policy");
            Check(coll.badPos==384 && coll.flags==0x1f,"native slope, baddie and collision settings survive scoped adjustment");
        }
        Check(!g_roomscaleNativeCollision && !g_roomscaleNativeItem,"collision scope is cleared after every native tick");
    }
    RoomscaleFixture(1,true);RoomCollision coll{};roomscaleTestNative=&coll;roomscaleTestWall=true;
    testFloorRight=.22f;LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&g_roomscaleFollowTime);
    g_roomscaleFollowTime.QuadPart-=frequency.QuadPart/30;
    Detour_LaraAboveWater(item,&coll);
    Check(Field<PHD_3DPOS>(off::item_pos).x_pos==0 && !(Field<uint16_t>(off::item_flags)&8),"solid walls still block the complete roomscale/native collision pipeline");
    Field<uint16_t>(off::item_flags)|=8;roomscaleTestWall=false;
    const auto airborneDrag=DragBody(item);
    Check(airborneDrag.x==0 && airborneDrag.z==0,"gravity already enabled prevents dragging even if the animation still says STOP");
    roomscaleTestNative=nullptr;testHeadYaw=0;
}
}
