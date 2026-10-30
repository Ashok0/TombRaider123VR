namespace {
int catchTicks=0,catchAt=12,catchState=10,catchStepX=100,catchStepY=0;
bool catchKills=false;
void __cdecl NativeCatchMovement(uint8_t* target,void*) {
    Check(target==item,"ledge impact observes Lara's normal native collision tick");
    ++catchTicks;
    auto& pos=Field<PHD_3DPOS>(off::item_pos);
    if (catchTicks<=catchAt) { pos.x_pos+=catchStepX;pos.y_pos+=catchStepY; }
    if (catchTicks>=catchAt) {
        Field<int16_t>(off::item_anim_state)=int16_t(catchState);
        Field<int16_t>(off::item_goal_state)=int16_t(catchState);
        Field<uint16_t>(off::item_flags)=0;
        if (catchKills) Field<int16_t>(off::item_hit_points)=0;
    }
}
void TestLedgeCatchHaptics() {
    using tr::LedgeCatchHaptics;
    for (int game:{1,2,3}) for (bool firstPerson:{false,true})
    for (int scenario=0;scenario<11;++scenario) {
        Reset(28,{},true);
        dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
        if (!firstPerson) { g_active=g_haveHeading=g_runtimeEnabled=false;g_headingItem=nullptr; }
        Field<uint16_t>(off::item_flags)=8;
        catchTicks=0;catchAt=12;catchState=10;catchStepX=100;catchStepY=0;catchKills=false;
        if (scenario==1) { catchStepX=0;catchStepY=60;Field<int16_t>(18)=9; }
        if (scenario==2) catchAt=4; // Small airtime, even with distance.
        if (scenario==3) catchStepX=10;
        if (scenario==4) catchState=2;
        if (scenario==5) catchState=75;
        if (scenario==6) catchState=56;
        if (scenario==7) catchKills=true;
        if (scenario==8) testWater=1;
        if (scenario==9) testConfig.enabled=false;
        if (scenario==10) catchStepX=4096; // Teleport, not jump travel.
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeCatchMovement);
        const int expected=scenario<=1 ? 1 : 0;
        for (int tick=0;tick<catchAt+8;++tick) {
            Detour_LaraAboveWater(item,nullptr);
            Check(testLedgeCatches==(tick+1>=catchAt ? expected : 0),
                  "one impact only on confirmed large-jump/fall ledge catch in either view");
        }
        Check(Field<PHD_3DPOS>(off::item_pos).x_pos==catchAt*catchStepX &&
              Field<PHD_3DPOS>(off::item_pos).y_pos==catchAt*catchStepY,
              "haptic observer cannot alter native jump or grab position");
    }
    LedgeCatchHaptics flight;
    auto sample=[&](int tick,const void* owner,int level,int endState,bool enabled=true) {
        flight.Before(enabled,owner,level,28,true,tick*100,0,0);
        return flight.After(enabled,endState,endState==28,tick*100+100,0,0);
    };
    for (int i=0;i<12;++i) Check(!sample(i,item,1,28),"air travel alone never rumbles");
    Check(!sample(12,item,2,10),"level changes cannot reuse impact history");
    flight.Reset();
    for (int i=0;i<12;++i) sample(i,item,1,28);
    Check(!sample(12,module,1,10),"different Lara cannot reuse impact history");
    flight.Reset();
    for (int i=0;i<12;++i) sample(i,item,1,28);
    sample(12,item,1,28,false);
    Check(!sample(13,item,1,10),"tracking/gameplay interruption clears impact history");
    flight.Reset();
    for (int i=0;i<20;++i) {
        flight.Before(true,item,1,28,true,0,0,0);
        flight.After(true,28,true,0,0,0);
    }
    Check(!flight.After(true,10,false,0,1000,0),"ledge collision snap cannot manufacture large flight travel");
    tr::GunHaptics rumble;
    rumble.LedgeCatch(100);
    rumble.Shot(0,101);
    Check(rumble.until[0]==220 && rumble.until[1]==220,"gun shots cannot shorten a ledge impact burst");
    int pulses[2]{};
    for (uint64_t time=100;time<=230;++time) {
        auto emit=[&](int hand,unsigned short duration) {
            Check(duration==3999,"ledge catch uses maximum legacy OpenVR pulse strength");
            ++pulses[hand];
        };
        rumble.Update(time,emit);rumble.Update(time,emit);
    }
    Check(pulses[0]==24 && pulses[1]==24,"120 ms impact reaches both hands once per allowed pulse interval");
    rumble.LedgeCatch(300);rumble.Reset();
    rumble.Update(300,[&](int,unsigned short) { Check(false,"shutdown cancels ledge rumble"); });
}
}
