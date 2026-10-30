// Tracking-space gesture and production native-input scope regressions.
void TestLedgePullGesture() {
    using tr::firstperson::LedgePullGesture;
    for (int hz:{30,60,90,144}) {
        LedgePullGesture pull;bool fired=false;
        for(int i=0;i<=hz/2;++i) {
            const float down=float(i)/hz*.6f;
            fired=pull.Update(true,uint64_t(i*1000/hz),1.7f,1.7f-down,1.7f-down)||fired;
        }
        Check(fired,"two tracked hands pulling 22 cm request a climb at every sample rate");
        for(int i=hz/2+1;i<=hz*2;++i) {
            const bool active=pull.Update(true,uint64_t(i*1000/hz),1.7f,1.4f,1.4f);
            if(i*1000/hz>1500) Check(!active,"held low hands cannot repeat a blocked climb");
        }
        for(int i=1;i<=hz/2;++i) pull.Update(true,2000+i*1000/hz,1.7f,1.4f+float(i)/hz*.6f,1.4f+float(i)/hz*.6f);
        fired=false;
        for(int i=1;i<=hz/2;++i) fired=pull.Update(true,2500+i*1000/hz,1.7f,1.7f-float(i)/hz*.6f,1.7f-float(i)/hz*.6f)||fired;
        Check(fired,"raising both hands rearms a blocked pull");
        for(int mode=0;mode<7;++mode) {
            pull.Reset();
            for(int i=0;i<hz*2;++i) {
                const float t=float(i)/hz,travel=std::min(.3f,t*.6f);
                float head=1.7f,left=1.7f,right=1.7f;
                if(mode==0) left-=travel; // One hand only.
                if(mode==1) head+=travel; // Head rises, hands stationary.
                if(mode==2) { head-=travel;left-=travel;right-=travel; } // Crouch together.
                if(mode==3) left=right=1.7f-t*.1f; // Slow drift.
                if(mode==4) left=right=1.7f+.01f*std::sin(t*20);
                if(mode==5) left=right=1.7f-travel;
                if(mode==6) left=right=NAN;
                Check(!pull.Update(mode!=5,uint64_t(i*1000/hz),head,left,right),
                    "one hand, head-only motion, crouching, drift, jitter and invalid tracking cannot pull");
            }
        }
    }
    LedgePullGesture pull;
    pull.Update(true,0,1.7f,1.7f,1.7f);
    Check(!pull.Update(true,30,1.7f,1.1f,1.1f),"tracking discontinuity rebases the gesture");
    pull.Update(false,60,0,0,0);
    Check(!pull.Update(true,90,1.7f,.8f,.8f),"reacquired tracking cannot complete an old pull");
    Check(!pull.Update(true,1000,1.7f,.5f,.5f),"pause clears old pull history");
    Check(!pull.Update(true,500,1.7f,1.7f,1.7f),"backwards clock resets gesture");
    auto hands=[](float down) {
        testVR.m_rawHeadPose.m[1][3]=1.7f;
        testControllerPose[0].m[1][3]=testControllerPose[1].m[1][3]=1.7f-down;
    };
    for(const wchar_t* game:{L"tomb1.dll",L"tomb2.dll",L"tomb3.dll"}) {
        for(int state=0;state<90;++state) {
            Reset(state,{},true);dll.module=game;Field<int16_t>(off::item_anim_number)=96;
            *Ptr<uint32_t>(dll.input)=0x40u;bool injected=false;
            for(int i=0;i<16;++i) {
                hands(i*.02f);
                { LedgePullInputScope scope(item,double(i)/30);
                  injected|=(*Ptr<uint32_t>(dll.input)&Directions)==Forward;
                  Check((*Ptr<uint32_t>(dll.input)&0x40u)!=0,"pull preserves native held grab"); }
                Check(*Ptr<uint32_t>(dll.input)==0x40u,"synthetic forward ends with the native tick");
            }
            Check(injected==(state==10),"only stationary native hang-loop accepts a physical pull");
        }
        for(int exclusion=0;exclusion<19;++exclusion) {
            Reset(10,{},true);dll.module=game;Field<int16_t>(off::item_anim_number)=96;
            *Ptr<uint32_t>(dll.input)=0x40u;
            if(exclusion==0) *Ptr<uint32_t>(dll.input)=0;
            if(exclusion==1) *Ptr<uint32_t>(dll.input)|=Back;
            if(exclusion==2) *Ptr<uint32_t>(dll.input)|=0x10;
            if(exclusion==3) *Ptr<uint32_t>(dll.input)|=0x100;
            if(exclusion==4) g_active=false;
            if(exclusion==5) testPoseAvailable=false;
            if(exclusion==6) testWater=1;
            if(exclusion==7) Field<int16_t>(off::item_goal_state)=19;
            if(exclusion==8) Field<int16_t>(off::item_hit_points)=0;
            if(exclusion==9) *Ptr<uint16_t>(dll.lara+60)=0x2000;
            if(exclusion==10) testConfig.gamepadEnabled=false;
            if(exclusion==11) g_shifted=true;
            if(exclusion==12) g_headingLevel=-1;
            if(exclusion==13) Field<int16_t>(off::item_anim_number)=95;
            if(exclusion==14) *Ptr<uint32_t>(dll.input)|=StepLeft;
            if(exclusion==15) g_manualLocal={1,0};
            if(exclusion==16) testMissingHand=1;
            if(exclusion==17) testVR.m_poseValid=false;
            if(exclusion==18) Field<uint16_t>(off::item_flags)=8;
            const auto original=*Ptr<uint32_t>(dll.input);
            for(int i=0;i<16;++i) { hands(i*.02f);LedgePullInputScope scope(item,double(i)/30);
                Check(*Ptr<uint32_t>(dll.input)==original,"explicit movement/drop or unavailable hanging cancels pull"); }
        }
        for(int mode=0;mode<6;++mode) {
            Reset(10,{},true);dll.module=game;testConfigFlags=2;Field<int16_t>(off::item_anim_number)=96;
            *Ptr<uint16_t>(dll.lara+60)=mode==1 ? 0 : 0x8000u;
            if(mode==2) *Ptr<uint32_t>(dll.input)=0x1000u;
            if(mode==3 || mode==4) testLevelType=3;
            if(mode==4) *Ptr<uint32_t>(dll.input)=0x40u;
            if(mode==5) *Ptr<uint32_t>(dll.input)=Back;
            bool injected=false;
            for(int i=0;i<16;++i) { hands(i*.02f);LedgePullInputScope scope(item,double(i)/30);
                injected|=(*Ptr<uint32_t>(dll.input)&Forward)!=0; }
            Check(injected==(mode==0 || mode==4),"modern grab latch and native tank override keep their collection-specific rules");
        }
        for(bool clear:{false,true}) {
            Reset(10,{},true);dll.module=game;floorAllowsEntry=clear;Field<int16_t>(off::item_anim_number)=96;
            *Ptr<uint32_t>(dll.input)=0x40u;
            Field<PHD_3DPOS>(off::item_pos).y_rot=8000;Ptr<int16_t>(dll.analogInput)[2]=-12000;
            g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(+[](uint8_t*,void*) {
                Check((*Ptr<uint32_t>(dll.input)&(Forward|0x40u))==(Forward|0x40u),"native hang receives forward and held grab");
                Check(Ptr<int16_t>(dll.analogInput)[2]==8000,"pull faces the ledge independent of head yaw");
                if(floorAllowsEntry) Field<int16_t>(off::item_goal_state)=19;
                *Ptr<uint32_t>(dll.input)|=0x8000u;
            });
            const auto now=uint64_t(TurnTime()*1000);
            for(int i=0;i<=15;++i) g_ledgePull.Update(true,now-(15-i)*33,1.7f,1.7f-i*.02f,1.7f-i*.02f);
            hands(.30f);const auto before=Field<PHD_3DPOS>(off::item_pos);
            Detour_LaraAboveWater(item,nullptr);
            Check(Field<int16_t>(off::item_goal_state)==(clear ? 19 : 10),"native clearance alone decides whether to mount");
            Check(*Ptr<uint32_t>(dll.input)==(0x40u|0x8000u) && Ptr<int16_t>(dll.analogInput)[2]==-12000,
                "pull restores heading/input and preserves unrelated native changes");
            Check(!std::memcmp(&before,&Field<PHD_3DPOS>(off::item_pos),sizeof(before)),"gesture cannot teleport or rotate Lara");
            *Ptr<uint32_t>(dll.input)=0;LedgePullInputScope released(item,TurnTime());
            Check(!g_ledgePull.latched && !g_ledgePull.count,"releasing native grab immediately cancels pull history");
        }
    }
}
