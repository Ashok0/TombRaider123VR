namespace {
int dragQueries=0,roomUpdates=0;bool dragWall=false;
void __cdecl DragCollision(tr::RoomCollision* coll,int32_t x,int32_t,int32_t,int16_t,int32_t) {
    ++dragQueries;
    Check(coll->badPos==32512 && coll->badNeg==-384 && coll->flags==1,"roomscale permits drops while retaining native wall and slope constraints");
    coll->floorSamples[0]=x>=64?600:0;coll->floorSamples[1]=-2000;
    if(dragWall) { coll->type=8;coll->shift[0]=-32; }
}
void __cdecl DragRoom(uint8_t* target,int32_t offset) {
    Check(target==item && offset==-381,"accepted roomscale sweep updates native room");++roomUpdates;
}
void TestRoomscaleLedge() {
    using namespace tr;
    for(int game:{1,2,3}) for(bool blocked:{false,true}) {
        Reset(2,{},true);dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
        const auto base=reinterpret_cast<uint64_t>(GetModuleHandle(nullptr));
        const auto shift=uint32_t(reinterpret_cast<uint64_t>(module)-base);
        for(auto* field:{&dll.laraItem,&dll.camera,&dll.lara,&dll.input,&dll.analogInput}) *field+=shift;
        dll.getCollisionInfo=uint32_t(reinterpret_cast<uint64_t>(&DragCollision)-base);
        dll.updateLaraRoom=uint32_t(reinterpret_cast<uint64_t>(&DragRoom)-base);g_boundBase=base;
        testConfig.firstPersonRoomscaleMove=true;testFloorRight=.2f;
        g_dragCurrent=g_dragPrevious=g_dragShown={};dragQueries=roomUpdates=0;dragWall=blocked;
        const auto result=DragBody(item);
        if(blocked) Check(result.x==0 && roomUpdates==0,"wall still rejects physical roomscale step");
        else Check(result.x>=.064f && result.x<.1f && roomUpdates==dragQueries &&
            Field<int16_t>(off::item_anim_state)==2 && !(Field<uint16_t>(off::item_flags)&8),
            "roomscale stops at first unsupported sweep and leaves fall selection to native collision");
    }
    testFloorRight=testFloorForward=0;
}
void BodyRig(int game,bool armed) {
    using namespace tr;
    ReadyArmedIK(game);
    if(!armed) { *Ptr<int16_t>(dll.lara+2)=0;testConfig.firstPersonUnarmedIK=true; }
    Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=2;
    const motiongun::Vec points[15]={{0,-500,0},{-100,-500,0},{-100,-260,70},{-100,0,0},
        {100,-500,0},{100,-260,70},{100,0,0},{0,-650,0},
        {180,-700,0},{240,-460,30},{180,-250,70},{-180,-700,0},{-240,-460,30},{-180,-250,70},{0,-850,0}};
    for(int i=0;i<15;++i) {
        auto* bind=Ptr<float>(dll.objects+192)+i*16;
        bind[12]=-points[i].x;bind[13]=-points[i].y;bind[14]=-points[i].z;
        motiongun::WriteRows({firstperson::IdentityBasis(),{}},ikNative+i*12);
        for(auto offset:{off::item_joints_cur,off::item_joints_prev}) {
            auto* p=reinterpret_cast<int32_t*>(item+offset+i*48);
            p[3]=int32_t(points[i].x*16384);p[7]=int32_t(points[i].y*16384);p[11]=int32_t(points[i].z*16384);
        }
    }
    testHeadYaw=testViewRise=testViewRight=testViewForward=0;
    testConfig.firstPersonJoint=14;testConfig.firstPersonAnchorX=testConfig.firstPersonAnchorY=testConfig.firstPersonAnchorZ=0;
    g_scenePose={0,-850,0};g_groundEye.Reset();g_bodyVisualOffset=g_bodyWallOffset={};
    testControllerOffset[0]={-.25f,.35f,.1f};testControllerOffset[1]={.25f,.35f,.1f};
    testVR.m_rawHeadPose={{{1,0,0,0},{0,1,0,1.7f},{0,0,1,0}}};
}
void TestGaitArmStability() {
    using namespace tr;using namespace tr::motiongun;
    for(int game:{1,2,3}) for(bool armed:{false,true}) for(int count:{15,32}) for(int gait:{5,16,21,22}) {
        BodyRig(game,armed);testConfig.firstPersonFullBodyIK=true;testConfig.firstPersonBodyScale=1;
        testConfig.firstPersonMovementStabilization=true;
        g_bodySkinScope=true;g_pistolBodyScope=armed;Field<uint32_t>(off::item_mesh_bits)=0x3fff;
        ResetBodyFollower();
        auto* obj=Ptr<uint8_t>(dll.objects);auto* geom=obj+off::object_geom;
        Frame native[15]{},bind[32]{},inverse[32]{};
        int mapping[32]{};float poses[32*12]{};
        for(int j=0;j<15;++j) Check(ReadIKJoint(obj,ikNative,15,0,nullptr,nullptr,j,native[j]),"gait fixture reads native joint");
        for(int i=0;i<count;++i) {
            const int j=mapping[i]=count==15 ? i : (i*7)%15;
            Check(ReadIKBind(obj,15,0,nullptr,nullptr,j,bind[i]) && Inverse(bind[i],inverse[i]),"gait fixture reads model binding");
            auto raw=inverse[i];for(int r=0;r<3;++r) {raw.basis.r[r][1]=inverse[i].basis.r[r][2];raw.basis.r[r][2]=-inverse[i].basis.r[r][1];}
            WriteRows(raw,poses+i*12);
            WriteRows(Multiply(native[j],inverse[i]),ikNative+i*12);
        }
        if(count>15) {
            *reinterpret_cast<int*>(geom+28)=count;*reinterpret_cast<int**>(geom+48)=mapping;
            *reinterpret_cast<float**>(geom+72)=poses;
        }
        auto draw=[&]() {++testVR.m_poseSerial;g_bodyFollower.time=g_torsoPose.time=TurnTime()-1./90.;Detour_GetJoints(item);};
        draw();Check(g_groundGait.valid,"standing production draw captures gait reference");
        Field<int16_t>(off::item_anim_state)=int16_t(gait);g_haveManualInput=true;
        g_manualLocal=g_manualWorld=gait==21?locomotion::Vec{1,0}:gait==22?locomotion::Vec{-1,0}:locomotion::Vec{0,-1};
        draw();float baseline[384];std::memcpy(baseline,g_bodyRenderPalette,sizeof(baseline));
        for(int sample=1;sample<=24;++sample) {
            const float wave=std::sin(sample*.6f);
            const motiongun::Vec travel{sample*6*g_manualWorld.x,0,sample*6*g_manualWorld.z};
            auto& current=Field<PHD_3DPOS>(off::item_pos);auto& previous=Field<PHD_3DPOS>(off::item_pos_prev);
            current.x_pos=previous.x_pos=int(travel.x);current.z_pos=previous.z_pos=int(travel.z);
            g_scenePose.x_pos=int(travel.x);g_scenePose.z_pos=int(travel.z);
            Ptr<int32_t>(dll.w2vMatrix)[3]=int(travel.x);Ptr<int32_t>(dll.w2vMatrix)[11]=int(travel.z);
            const auto rotation=firstperson::RotationMatrix(firstperson::AxisRotation({0,0,1},wave*.12f));
            const Frame sway{rotation,Sub(Add(native[0].origin,{wave*18,0,wave*9}),Transform(rotation,native[0].origin))};
            for(int i=0;i<count;++i) {
                const int j=mapping[i];auto frame=native[j];
                if(j==0 || j>=7) frame=Multiply(sway,frame);
                if(j==3 || j==6) frame.origin.z+=(j==3?1:-1)*wave*45;
                WriteRows(Multiply(frame,inverse[i]),ikNative+i*12);
            }
            draw();bool feetMoved=false;
            for(int i=0;i<count;++i) {
                const int j=mapping[i];const auto actual=Multiply(ReadRows(g_bodyRenderPalette+i*12),bind[i]);
                const auto base=Multiply(ReadRows(baseline+i*12),bind[i]);
                if(j>=7 && j<=13) Check(firstperson::Length(Sub(actual.origin,base.origin))<.05f && WristBasisError(actual.basis,base.basis)<.001f,
                    "side/back production arm palettes reject native torso sway with stationary controllers");
                if(j==3 || j==6) feetMoved|=firstperson::Length(Sub(actual.origin,base.origin))>1;
            }
            Check(feetMoved,"side/back stabilization preserves animated footsteps");
            float repeated[384];std::memcpy(repeated,g_bodyRenderPalette,sizeof(repeated));Detour_GetJoints(item);
            for(int i=0;i<count*12;++i) Check(std::fabs(repeated[i]-g_bodyRenderPalette[i])<.001f,"stereo redraw cannot advance upper-body reference");
        }
        testControllerOffset[1].x+=.07f;testViewRight=.15f;draw();
        Frame target{};Basis controller{};Check(BuildControllerWrist(1,target,controller),"gait hand target remains live");
        const motiongun::Vec camera{float(Ptr<int32_t>(dll.w2vMatrix)[3]),0,float(Ptr<int32_t>(dll.w2vMatrix)[11])};
        for(int i=0;i<count;++i) {
            const auto actual=Multiply(ReadRows(g_bodyRenderPalette+i*12),bind[i]);
            if(mapping[i]==10) Check(firstperson::Length(Sub(Add(actual.origin,camera),target.origin))<.05f,"gait stabilization does not delay controller movement");
            if(mapping[i]==14) Check(std::fabs(actual.origin.x-.15f*LiveWorldUnitsPerMetre())<.05f,"gait stabilization preserves real headset lean");
        }
        Field<int16_t>(off::item_anim_state)=3;Field<uint16_t>(off::item_flags)=8;draw();
        Check(!g_groundGait.valid,"jump releases upright gait reference");
        *reinterpret_cast<int*>(geom+28)=0;g_pistolBodyScope=false;
    }
}
void TestCollectionIntegration() {
    using namespace tr;
    TestGaitArmStability();
    TestRoomscaleLedge();
    for(int game:{1,2,3}) {
        BodyRig(game,false);testConfig.firstPersonFullBodyIK=true;
        Check(FullBodyIKReady(),"full-body gate admits standing Lara in every collection game");
        float palette[384]{};std::memcpy(palette,ikNative,sizeof(palette));
        Check(ApplyFullBodyIK(item,palette,15),"production full-body palette solves standing rig");
        Check(g_bodySize.valid,"standing rig captures fixed body scale");
        for(int hand=0;hand<2;++hand) {
            motiongun::Frame wrist{},target{};motiongun::Basis controller{};
            Check(ReadIKJoint(Ptr<uint8_t>(dll.objects),palette,15,0,nullptr,nullptr,hand?10:13,wrist) &&
                BuildControllerWrist(hand,target,controller),"full-body fitted wrists are readable");
            Check(firstperson::Length(motiongun::Sub(wrist.origin,target.origin))<.05f,
                "full-body fitting leaves each wrist at calibrated controller target");
        }
        for(int foot:{3,6}) {
            motiongun::Frame joint{};ReadIKJoint(Ptr<uint8_t>(dll.objects),palette,15,0,nullptr,nullptr,foot,joint);
            Check(std::fabs(joint.origin.y)<.1f,"full-body fit plants both feet at native floor");
        }
        {
            int mapping[16]{};float inversePoses[16*12]{};
            auto* obj=Ptr<uint8_t>(dll.objects);auto* geom=obj+off::object_geom;
            for(int i=0;i<16;++i) {
                const int joint=i==10?9:i==13?12:i==15?9:i;mapping[i]=joint;
                motiongun::Frame bind{},inverse{};ReadIKBind(obj,0,0,nullptr,nullptr,joint,bind);motiongun::Inverse(bind,inverse);
                inverse.basis={{{1,0,0},{0,0,-1},{0,1,0}}};
                motiongun::WriteRows(inverse,inversePoses+i*12);
                motiongun::WriteRows({firstperson::IdentityBasis(),{}},palette+i*12);
            }
            *reinterpret_cast<int*>(geom+28)=16;*reinterpret_cast<int**>(geom+48)=mapping;
            *reinterpret_cast<float**>(geom+72)=inversePoses;
            Check(ApplyFullBodyIK(item,palette,16),"HD body with separately owned wrists still solves complete full-body rig");
            *reinterpret_cast<int*>(geom+28)=0;
        }
        Field<int16_t>(off::item_anim_state)=3;Field<uint16_t>(off::item_flags)=8;
        std::memcpy(palette,ikNative,sizeof(palette));
        Check(!ApplyFullBodyIK(item,palette,15) && !std::memcmp(palette,ikNative,sizeof(palette)),"airborne pose never fits hips or legs to standing floor");

        BodyRig(game,true);g_pistolBodyScope=true;
        std::memcpy(palette,ikNative,sizeof(palette));
        Check(ApplyArmIK(item,palette,15),"model-bind pistol body arm solve succeeds");
        motiongun::Frame first[6];std::memcpy(first,g_pistolArmPose.joint,sizeof(first));
        for(int i=8;i<=13;++i) { ikNative[i*12+3]+=40;ikNative[i*12+7]-=35; }
        std::memcpy(palette,ikNative,sizeof(palette));
        Check(ApplyArmIK(item,palette,15),"animated native pistol chains still solve from model bind");
        for(int i=0;i<6;++i) Check(firstperson::Length(motiongun::Sub(first[i].origin,g_pistolArmPose.joint[i].origin))<.01f &&
            WristBasisError(first[i].basis,g_pistolArmPose.joint[i].basis)<.001f,"pistol recoil pose cannot displace shared rendered arm chain");
        g_pistolBodyScope=false;

        ReadyArmedIK(game);
        g_holsterItem=item;g_holsterLevel=0;g_holsterSeen=GetTickCount64();g_holsters.game=game;
        for(int hand=0;hand<2;++hand) {
            const auto p=testControllerOffset[hand];
            g_holsterAnchor[hand]={motiongun::HandInWorld({float(g_scenePose.x_pos),float(g_scenePose.y_pos),float(g_scenePose.z_pos)},
                p.x,p.y,p.z,g_heading.base,LiveWorldUnitsPerMetre()),true};
            g_holsterKinds[hand]=1;g_holsterVisible[hand]=true;
            testHands[hand]={};testHands[hand].valid=true;testHands[hand].grip=1;
        }
        XState gripPad{};bool shift=false,action=false;BuildState(gripPad,shift,true,action);
        Check(g_holsters.hand[0].claimed && g_holsters.hand[1].claimed && !action &&
            !(gripPad.Gamepad.wButtons&(XB_LEFT_SHOULDER|XB_RIGHT_SHOULDER|XB_X)),
            "simultaneous holster grips take priority over two-grip Action, duck and walk");
        testHands[0]={};testHands[1]={};
        // Native DrawLaraHD's bare outfits, including the 32-bit mask boundary.
        const uint32_t bareOutfits[]={2,7,14,15,16,17,18,19,20,21,22,23,27,28,29,30,31,32};
        for(uint32_t outfit=0;outfit<=33;++outfit) for(int status:{2,3,4})
        for(unsigned heldMask:{0u,1u,2u,3u}) {
            uint8_t app[0x354]{};app[0x34c]=app[0x34d]=11;
            std::memcpy(app+0x348,&outfit,sizeof(outfit));
            uint8_t original[sizeof(app)];std::memcpy(original,app,sizeof(app));
            g_holsters.active=true;g_holsters.weapon=1;g_holsters.mask=heldMask;
            *Ptr<int16_t>(dll.lara+2)=int16_t(status);
            bool bare=false;for(auto candidate:bareOutfits) bare|=candidate==outfit;
            {
                HolsterRenderScope scope(item,app);
                for(int hand=0;hand<2;++hand) {
                    const bool held=(heldMask&(1u<<hand))!=0;
                    Check(app[0x34c+hand]==(held ? 11 : bare ? 1 : 6),
                        "single-pistol empty hand preserves the outfit's bare/gloved rest mesh through draw and holster");
                    Check(app[0x34e+hand]==(held ? 0 : 1),
                        "only the stowed pistol appears in its holster");
                }
            }
            Check(!std::memcmp(app,original,sizeof(app)),
                "physical holster draw scope restores native outfit, hand and holster metadata");
        }

        ReadyArmedIK(game);g_armedMenuHold=false;
        XGamepad pad{};pad.wButtons=XB_BACK|XB_A|XB_X;ApplyArmedMenu(pad);
        Check(pad.wButtons==(XB_A|XB_X),"armed X consumes menu only, preserving walk and jump");
        *Ptr<int16_t>(dll.lara+2)=0;pad.wButtons=XB_BACK;ApplyArmedMenu(pad);
        Check(!pad.wButtons,"same X press cannot open inventory when native holster finishes");
        pad={};ApplyArmedMenu(pad);pad.wButtons=XB_BACK;ApplyArmedMenu(pad);
        Check(pad.wButtons==XB_BACK,"fresh unarmed X reaches native inventory");

        IKSetup(game);testConfig.firstPersonPhysicalCrouch=true;testConfig.firstPersonFullBodyIK=true;
        testViewRise=-.54f;Check(!FirstPersonPhysicalDuck(),"partial squat retains visible legs before native crouch");
        testViewRise=-.56f;Check(FirstPersonPhysicalDuck()==(game==3),"physical crouch activates only for TR3 at 55 cm drop");
        testViewRise=-.28f;Check(FirstPersonPhysicalDuck()==(game==3),"physical crouch has release hysteresis");
        if(game==3) {
            g_groundEye.Reset();g_groundEye.Apply({0,0,0},0,{0,-700,0},0);
            Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=71;
            PHD_3DPOS eye{0,-336,0};UpdateLocomotion(eye);
            Check(eye.y_pos==-700,"physical crouch keeps standing camera baseline; HMD supplies drop once");
        }
        testViewRise=-.22f;Check(!FirstPersonPhysicalDuck(),"physical crouch releases at 23 cm");
        testViewRise=0;
        if(game>1) {
            ReadyArmedIK(game);testConfig.firstPersonUnarmedIK=true;*Ptr<int16_t>(dll.lara+2)=0;
            testFlareReady=true;*Ptr<int16_t>(dll.lara+4)=game==2?8:9;
            int32_t tip[3]{};Check(FirstPersonFlareTip(tip),"TR2/3 held flare has a tracked light tip");
            GunPose left{},right{};Check(BuildGunPose(0,left) && BuildGunPose(1,right),"held flare and empty opposite hand both retain tracked rendering");
            testMissingHand=0;Check(!FirstPersonFlareTip(tip),"lost left controller disables held-flare override");
        }
    }
    testViewRise=0;testControllerOffset[0]=testControllerOffset[1]={};testFlareReady=false;
}
}
