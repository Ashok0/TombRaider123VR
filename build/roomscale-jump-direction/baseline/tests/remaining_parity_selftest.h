namespace {
int landingState=3,landingAnimation=14,landingY=-100;
bool landingGravity=true;
void __cdecl NativeParityLanding(uint8_t*,void*) {
    Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(landingState);
    Field<int16_t>(off::item_anim_number)=int16_t(landingAnimation);
    Field<PHD_3DPOS>(off::item_pos).y_pos=landingY;
    Field<uint16_t>(off::item_flags)=landingGravity ? 8 : 0;
}
uint32_t cuffSeams=0,cuffVisible=0;
void __cdecl NativeParityCuff(void* target,int32_t) {
    Detour_GetJoints(static_cast<uint8_t*>(target));
    cuffSeams=FirstPersonBodyWristSeamMask();
    FirstPersonBodySkin(cuffVisible);
}
void TestRemainingParity() {
    using namespace tr;using namespace tr::firstperson;
    BodyVisibilityRecovery visibility;
    Check(visibility.Update(item,1,0,true,false,false)==ArmMeshBits,"mount begins arms-only recovery");
    Check(visibility.Update(item,1,.349,false,false,false)==ArmMeshBits,"mount mask survives initial standing palettes");
    Check(visibility.Update(item,1,.351,false,false,true)==ArmMeshBits,"torso recovery extends the visibility hold");
    Check(visibility.Update(item,1,.4,false,false,false)==UINT32_MAX,"settled torso releases visibility after 350 ms");
    Check((visibility.Update(item,1,1,false,true,false)&(1u<<7))==0,"hard landing hides chest");
    Check(visibility.Update(item,2,1.01,false,false,false)==UINT32_MAX,"new level discards old landing visibility");
    visibility.Update(item,2,2,true,false,false);
    Check(visibility.Update(module,2,2.1,false,false,false)==UINT32_MAX,"different Lara cannot inherit visibility hold");
    visibility.Update(item,2,3,true,false,false);
    Check(visibility.Update(item,2,2,false,false,false)==UINT32_MAX,"backward clock clears recovery");

    for(int game:{1,2,3}) for(bool armed:{false,true}) for(bool full:{false,true}) {
        BodyRig(game,armed);testConfig.firstPersonFullBodyIK=full;
        g_headHidden=true;g_rollHidden=g_crouchHidden=g_ledgeArmsOnly=g_unarmedArmsHidden=false;
        g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&NativeJumpVisibility);
        for(int state:{19,54,game==1?58:55}) {
            g_bodyVisibility={};Field<int16_t>(off::item_anim_state)=int16_t(state);
            Field<uint32_t>(off::item_mesh_bits)=0x7fff;jumpDrawBits=0;
            Detour_DrawCreatureHD(item,0);
            Check((jumpDrawBits&~ArmMeshBits)==0 && (jumpDrawBits&0x1200u)==0x1200u,
                "mount and water-exit eye draws retain native grip arms without torso");
            Check(Field<uint32_t>(off::item_mesh_bits)==0x7fff,"mount masking is draw-local");
            Field<int16_t>(off::item_anim_state)=2;jumpDrawBits=0;
            Detour_DrawCreatureHD(item,0);
            Check((jumpDrawBits&~ArmMeshBits)==0,"first standing draw does not flash the recovering torso");
            g_bodyVisibility.until=TurnTime()-1;g_mountBodyTransition.Reset();g_torsoPose.recovering=false;
            jumpDrawBits=0;Detour_DrawCreatureHD(item,0);
            Check((jumpDrawBits&0xffu)==0xffu,"body returns when recovery and hold finish");
        }
        for(int state:{71,80,81,84,85,86,89,90}) {
            Field<int16_t>(off::item_anim_state)=int16_t(state);g_bodyVisibility={};
            Field<int16_t>(off::item_goal_state)=int16_t(state);
            if(game==3 && !armed) Check(UnarmedIKReady(),"empty hands remain tracked throughout TR3 crouch/crawl");
            g_meshOverride=false;g_meshItem=nullptr;Field<uint32_t>(off::item_mesh_bits)=0x7fff;
            SetMeshVisibility(true,false,IsCrouchState(item));
            jumpDrawBits=0;Field<uint32_t>(off::item_mesh_bits)=0x7fff;Detour_DrawCreatureHD(item,0);
            Field<uint32_t>(off::item_mesh_bits)=0x3600;Detour_DrawCreatureHD(item,1);
            Check(game==3 ? jumpDrawBits==HandMeshBits : (jumpDrawBits&0xffu)==0xffu,
                "only TR3 crouch/crawl states hide the body while retaining both hands");
            *Ptr<int32_t>(dll.renderPass)=4;jumpDrawBits=0;Detour_DrawCreatureHD(item,0);
            Check(jumpDrawBits==UINT32_MAX,"crouch and transition restrictions never hide the native shadow");
            *Ptr<int32_t>(dll.renderPass)=0;
        }
    }
    for(int game:{1,2,3}) {
        BodyRig(game,false);testConfig.firstPersonFullBodyIK=true;
        const int waterOut=game==1?58:55;
        Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(waterOut);
        *Ptr<int16_t>(dll.lara+2)=1;
        Check(FullBodyGripState(item) && FullBodyIKReady(),"water exit keeps full-body native grip even with hands busy");
        Check(locomotion::FirstPersonAnchorZ(waterOut,144,16,game)==16 &&
            !locomotion::IsClimbingCameraState(waterOut,game),"water exit uses short mount anchor without ladder camera sweep");
        Check(!locomotion::IsLedgeMountState(game==1?55:58,game),"water-exit IDs do not leak across collections");
        Check(!locomotion::IsClimbingCameraState(57,1) && !locomotion::IsConstrainedInteractionState(57,1),
            "TR1 twist does not inherit TR2/3 ladder camera restrictions");
        *Ptr<int16_t>(dll.lara+2)=0;
        Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=73;
        Check(CanWalk(item)==(game==3) && FullBodyIKReady()==(game==3),"sprint keeps heading and body fitting only in TR3");
    }
    for(float heading:{-2.4f,0.f,1.3f}) {
        Reset(73,{0,1},true,heading);dll.module=L"tomb3.dll";testConfigFlags=2;testHeadYaw=0;
        nativeSpeed=90;Tick();const auto& p=Field<PHD_3DPOS>(off::item_pos);
        Check(std::abs(p.x_pos-int(std::round(std::sin(heading)*90)))<=1 &&
            std::abs(p.z_pos-int(std::round(std::cos(heading)*90)))<=1 && Field<int16_t>(off::item_anim_state)==73,
            "TR3 sprint retains native speed and animation while following VR heading");
    }

    for(int state:{3,9,11,12,25,26,27,28,29}) for(int travel:{511,512}) {
        LedgeCatchHaptics flight;
        flight.Before(true,item,0,state,true,0,0,0);flight.After(true,state,true,0,0,0);
        flight.Before(true,item,0,state,true,travel,0,0);
        Check(flight.After(true,10,true,9999,9999,9999)==(travel==512),
            "half-tile catches qualify without minimum airtime, including reach and up-jump");
        Check(!flight.After(true,10,false,0,0,0),"confirmed catch feedback fires once");
    }
    LedgeCatchHaptics excursion;
    for(int y:{0,-256,256}) { excursion.Before(true,item,0,28,true,0,y,0);excursion.After(true,28,true,0,y,0); }
    Check(excursion.After(true,10,false,0,256,0),"catch uses full vertical excursion across apex");

    GunHaptics hover;int light[2]{},strong[2]{};uint64_t previous[2]{};
    for(uint64_t now=100;now<600;++now) {
        if(now<350 && now%10==0) hover.Hover(0,true,now);
        if(now==150) hover.Shot(0,now);
        auto emit=[&](int hand,unsigned short duration) {
            Check(!previous[hand] || now-previous[hand]>=5,"hover and shot share the 5 ms pulse budget");previous[hand]=now;
            if(duration==700) { ++light[hand];Check(now<440,"expired proximity samples cannot leave a rumble tail"); }
            else {Check(duration==3999 && now>=150 && now<230,"strong shots take priority over gentle hover");++strong[hand];}
        };
        hover.Update(now,emit);hover.Update(now,emit);
    }
    Check(light[0]>=4 && !light[1] && strong[0]==16 && !strong[1],"sustained proximity stays gentle and hand-specific");
    hover.Hover(1,true,1000);hover.Hover(1,false,1000);
    hover.Update(1000,[&](int,unsigned short){Check(false,"leaving holster stops hover immediately");});
    hover.Hover(0,true,2000);hover.Reset();
    hover.Update(2000,[&](int,unsigned short){Check(false,"recenter/shutdown cancels hover");});

    for(int game:{1,2,3}) for(bool hd:{false,true}) {
        BodyRig(game,true);int mapping[16]{};float poses[16*12]{};
        if(hd) {
            for(int j=0;j<16;++j) { mapping[j]=j==15?10:j;motiongun::WriteRows({IdentityBasis(),{}},poses+j*12); }
            auto* geom=Ptr<uint8_t>(dll.objects+off::object_geom);
            *reinterpret_cast<int*>(geom+28)=16;*reinterpret_cast<int**>(geom+48)=mapping;
            *reinterpret_cast<float**>(geom+72)=poses;
        }
        g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&NativeParityCuff);
        Field<uint32_t>(off::item_mesh_bits)=0x7fff;Detour_DrawCreatureHD(item,0);
        Check(cuffSeams==(HandMeshBits|(hd?0x8000u:0u)) && !(cuffSeams&cuffVisible),
            "body cuff mask maps hidden wrist helpers through the current costume palette");
        Check(FirstPersonBodyWristSeamMask()==0,"cuff permission cannot escape its body draw");
        Field<uint32_t>(off::item_mesh_bits)=0x3600;Detour_DrawCreatureHD(item,1);
        Check(cuffSeams==0,"rigid hands do not inherit body cuff clipping exceptions");
    }

    for(int game:{1,2,3}) for(int scenario=0;scenario<8;++scenario) for(int animation:{31,82,92}) {
        Reset(scenario==4?3:15,{},true);dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
        testHeadYaw=0;dll.frameFrac=204;*Ptr<int32_t>(dll.frameFrac)=256;
        g_previousBody={};g_dragPrevious=g_dragCurrent=g_dragShown={};
        g_groundEye.Reset();g_groundEye.Apply({0,0,0},0,{0,-700,80},0);
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeParityLanding);
        landingState=scenario==2?9:3;landingAnimation=14;landingY=-100;landingGravity=true;
        Detour_LaraAboveWater(item,nullptr);
        landingState=animation==92?1:2;landingAnimation=scenario==3?24:animation;
        if(scenario==3) landingState=2;
        landingY=scenario==0?-128:scenario==1?129:scenario==7?128:0;landingGravity=false;
        if(scenario==5) testLevel=1;
        Detour_LaraAboveWater(item,nullptr);
        const bool ordinary=scenario==0 || scenario>=6;
        Check(OrdinaryJumpLanding(item)==ordinary && UseLandingCamera(item)!=ordinary,
            "only intentional ordinary landings retain standing presentation; drops, falls, hard landings and level changes stay native");
        if(scenario!=5) {
            PHD_3DPOS eye{30,landingY-440,20};UpdateLocomotion(eye);
            Check(eye.y_pos==landingY-(ordinary?700:440),"ordinary landing camera removes native neck dip without changing hard-fall camera");
        }
        Check(Field<PHD_3DPOS>(off::item_pos).y_pos==landingY && Field<int16_t>(off::item_anim_number)==landingAnimation,
            "landing presentation leaves native root, animation and gravity untouched");
    }
    // Fit the production palette through takeoff and a landing torso squat.
    for(int game:{1,2,3}) for(bool armed:{false,true}) {
        BodyRig(game,armed);testConfig.firstPersonFullBodyIK=true;testConfig.firstPersonBodyScale=1;
        g_groundEye.Reset();g_groundEye.Apply({0,0,0},0,{0,-850,0},0);
        g_bodySkinScope=true;g_pistolBodyScope=armed;Field<uint32_t>(off::item_mesh_bits)=0x3fff;
        auto draw=[&]() { ++testVR.m_poseSerial;g_bodyFollower.time=g_torsoPose.time=TurnTime()-1./90.;Detour_GetJoints(item); };
        draw();draw();Check(g_jumpBodyReference.valid,"standing draw captures pre-jump torso reference");
        float standing[384];std::memcpy(standing,g_bodyRenderPalette,sizeof(standing));
        Field<int16_t>(off::item_anim_state)=15;ObserveJumpLanding(item);
        Field<int16_t>(off::item_anim_state)=3;Field<uint16_t>(off::item_flags)=8;ObserveJumpLanding(item);draw();
        Check(g_jumpBodyReference.valid,"airborne arm-only solve retains pre-jump reference");
        Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=2;
        Field<int16_t>(off::item_anim_number)=82;Field<uint16_t>(off::item_flags)=0;ObserveJumpLanding(item);
        for(int j=0;j<15;++j) if(j==0 || j==1 || j==4 || j>=7) ikNative[j*12+7]+=180;
        // Analytic two-link squat with the fixture's original limb lengths:
        // lower both hip sockets, bend knees forward, leave feet on the floor.
        const float thigh2=240.f*240+70.f*70,calf2=260.f*260+70.f*70;
        const float along=(thigh2-calf2+320.f*320)/(2*320.f);
        for(int knee:{2,5}) {
            ikNative[knee*12+7]=-320+along-(-260);
            ikNative[knee*12+11]=std::sqrt(thigh2-along*along)-70;
        }
        // Native landing also pitches the torso. A vertical-only fixture
        // would let the upright leg solver conceal a missing torso reference.
        const auto rotation=RotationMatrix(AxisRotation({1,0,0},-.4f));
        const motiongun::Vec pivot{0,-320,0};
        const motiongun::Frame bend{rotation,motiongun::Sub(pivot,motiongun::Transform(rotation,pivot))};
        for(int j=7;j<15;++j) {
            motiongun::Frame frame{},bind{},inverse{};
            Check(ReadIKJoint(Ptr<uint8_t>(dll.objects),ikNative,15,0,nullptr,nullptr,j,frame) &&
                ReadIKBind(Ptr<uint8_t>(dll.objects),15,0,nullptr,nullptr,j,bind) && motiongun::Inverse(bind,inverse),
                "landing fixture preserves each native costume binding");
            motiongun::WriteRows(motiongun::Multiply(motiongun::Multiply(bend,frame),inverse),ikNative+j*12);
        }
        draw();Check(OrdinaryJumpLanding(item),"actual landing observer selects upright body recovery");
        for(int j:{0,7,14}) {
            const auto before=motiongun::ReadRows(standing+j*12),after=motiongun::ReadRows(g_bodyRenderPalette+j*12);
            Check(firstperson::Length(motiongun::Sub(after.origin,before.origin))<.1f,
                "ordinary landing removes native torso squat before head/controller IK");
        }
        Check(!std::memcmp(g_bodySkinPalette,ikNative,15*12*sizeof(float)),"native physics palette still contains the original landing animation");
        g_pistolBodyScope=false;
    }
    testControllerOffset[0]=testControllerOffset[1]={};testLevel=0;testViewRise=0;
}
}
