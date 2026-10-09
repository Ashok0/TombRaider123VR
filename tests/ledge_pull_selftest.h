// Exercise the detector and the actual native input scope independently.
void TestLedgePullGesture() {
    using tr::firstperson::LedgePullGesture;
    for (int hz:{30,60,90,144}) {
        LedgePullGesture pull;
        bool fired=false;
        for (int i=0;i<=hz/2;++i) {
            const float down=float(i)/hz*.5f;
            fired=pull.Update(true,true,down,down,double(i)/hz)||fired;
        }
        Check(fired,"two hands pulling 18 cm request a native climb at every sample rate");
        for (int i=hz/2+1;i<=hz*2;++i) {
            const bool request=pull.Update(true,true,.25f,.25f,double(i)/hz);
            if (i>hz) Check(!request,"held low hands cannot repeat a blocked climb");
        }
        for (int i=1;i<=hz/2;++i)
            pull.Update(true,true,.25f-float(i)/hz*.5f,.25f-float(i)/hz*.5f,2.+double(i)/hz);
        fired=false;
        for (int i=1;i<=hz/2;++i)
            fired=pull.Update(true,true,float(i)/hz*.5f,float(i)/hz*.5f,2.5+double(i)/hz)||fired;
        Check(fired,"raising both hands permits retrying a blocked ledge");
        for (int mode=0;mode<6;++mode) {
            pull.Reset();
            for (int i=0;i<hz*3;++i) {
                const float t=float(i)/hz;
                const float left=mode==0 ? t*.5f : mode==1 ? 0.f : mode==2 ? t*.1f : .01f*std::sin(t*20);
                const float right=mode==0 ? 0.f : mode==1 ? t*.5f : left;
                Check(!pull.Update(mode!=4,mode!=5,left,right,double(i)/hz),
                      "one hand, slow drift, jitter, ineligible and untracked motion cannot climb");
            }
        }
    }
    LedgePullGesture pull;
    pull.Update(true,true,0,0,0);
    Check(!pull.Update(true,true,.5f,.5f,.03),"tracking discontinuity only rebaselines");
    pull.Update(true,false,0,0,.06);
    Check(!pull.Update(true,true,.8f,.8f,.09),"reacquired tracking cannot complete an old pull");
    Check(!pull.Update(true,true,1.1f,1.1f,1),"pause cannot complete an old pull");
    Check(!pull.Update(true,true,0,0,.5),"backwards clock resets gesture");

    for (const wchar_t* game:{L"tomb1.dll",L"tomb2.dll",L"tomb3.dll"}) {
        for (int state=0;state<90;++state) {
            Reset(state,{},true); dll.module=game;
            *Ptr<uint32_t>(dll.input)=0x40u|StepLeft;
            bool injected=false;
            for (int i=0;i<16;++i) {
                testHandDown[0]=testHandDown[1]=float(i)*.02f;
                {
                    LedgePullInputScope scope(item,double(i)/30);
                    injected=injected||((*Ptr<uint32_t>(dll.input)&Directions)==Forward);
                    Check((*Ptr<uint32_t>(dll.input)&0x40u)!=0,"gesture preserves existing grab");
                }
                Check(*Ptr<uint32_t>(dll.input)==(0x40u|StepLeft),"gesture direction never leaks out of native tick");
            }
            Check(injected==(state==10 || state==30 || state==31),
                  "only ordinary ledge hanging/shimmy can gesture in TR1, TR2 and TR3");
        }
        for (int exclusion=0;exclusion<13;++exclusion) {
            Reset(10,{},true); dll.module=game;
            *Ptr<uint32_t>(dll.input)=0x40u;
            if (exclusion==0) *Ptr<uint32_t>(dll.input)=0;
            if (exclusion==1) *Ptr<uint32_t>(dll.input)|=Back;
            if (exclusion==2) *Ptr<uint32_t>(dll.input)|=0x10;
            if (exclusion==3) *Ptr<uint32_t>(dll.input)|=0x100;
            if (exclusion==4) g_active=false;
            if (exclusion==5) testPoseAvailable=false;
            if (exclusion==6) testWater=1;
            if (exclusion==7) Field<int16_t>(off::item_goal_state)=19;
            if (exclusion==8) Field<int16_t>(off::item_hit_points)=0;
            if (exclusion==9) *Ptr<uint16_t>(dll.lara+60)=0x2000;
            if (exclusion==10) testConfig.gamepadEnabled=false;
            if (exclusion==11) g_shifted=true;
            if (exclusion==12) g_headingLevel=-1;
            const auto original=*Ptr<uint32_t>(dll.input);
            for (int i=0;i<16;++i) {
                testHandDown[0]=testHandDown[1]=float(i)*.02f;
                LedgePullInputScope scope(item,double(i)/30);
                Check(*Ptr<uint32_t>(dll.input)==original,"drop, controls and unavailable first person cancel gesture");
            }
        }
        for (int mode=0;mode<6;++mode) {
            Reset(10,{},true); dll.module=game; testConfigFlags=2;
            *Ptr<uint16_t>(dll.lara+60)=mode==1 ? 0 : 0x8000u;
            if (mode==2) *Ptr<uint32_t>(dll.input)=0x1000u;
            if (mode==3 || mode==4) testLevelType=3;
            if (mode==4) *Ptr<uint32_t>(dll.input)=0x40u;
            if (mode==5) *Ptr<uint32_t>(dll.input)=Back;
            bool injected=false;
            for (int i=0;i<16;++i) {
                testHandDown[0]=testHandDown[1]=i*.02f;
                LedgePullInputScope scope(item,double(i)/30);
                injected=injected||((*Ptr<uint32_t>(dll.input)&Forward)!=0);
                if (mode==0) Check((*Ptr<uint32_t>(dll.input)&0x40u)==0,
                    "modern auto-grab gesture never synthesizes Action");
            }
            Check(injected==(mode==0 || mode==4),
                  "modern auto-grab permits gesture, while drop and native tank override take precedence");
        }
        // Native callback owns whether the ledge has clearance. Test both results
        // through the production above-water detour with a pending pull.
        for (bool clear:{false,true}) {
            Reset(10,{},true); dll.module=game; floorAllowsEntry=clear;
            g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(+[](uint8_t*,void*) {
                Check((*Ptr<uint32_t>(dll.input)&(Forward|0x40u))==(Forward|0x40u),
                      "native hang collision sees forward and retained grab");
                if (floorAllowsEntry) Field<int16_t>(off::item_goal_state)=19;
                *Ptr<uint32_t>(dll.input)|=0x8000u;
            });
            *Ptr<uint32_t>(dll.input)=0x40u;
            const double now=TurnTime();
            for (int i=0;i<=10;++i) g_ledgePull.Update(true,true,i*.02f,i*.02f,now-(10-i)/30.);
            testHandDown[0]=testHandDown[1]=.20f;
            const auto before=Field<PHD_3DPOS>(off::item_pos);
            Detour_LaraAboveWater(item,nullptr);
            Check(Field<int16_t>(off::item_goal_state)==(clear ? 19 : 10),"native clearance controls mount decision");
            Check(*Ptr<uint32_t>(dll.input)==(0x40u|0x8000u),"input cleanup preserves native unrelated changes");
            Check(std::memcmp(&before,&Field<PHD_3DPOS>(off::item_pos),sizeof(before))==0,
                  "gesture never moves or rotates Lara directly");
            *Ptr<uint32_t>(dll.input)=0;
            LedgePullInputScope released(item,TurnTime());
            Check(!g_ledgePull.valid && *Ptr<uint32_t>(dll.input)==0,"releasing grab immediately cancels pending request");
        }
    }
}
