// Physical TR1/2 posture exercises production input and draw hooks.
namespace {
uint32_t physicalDrawMask=0;
int physicalDrawCalls=0,physicalHairCalls=0;
void __cdecl ObservePhysicalBodyDraw(void*,int32_t) {
    physicalDrawMask|=Field<uint32_t>(tr::off::item_mesh_bits);
    ++physicalDrawCalls;
}
void __cdecl ObservePhysicalHair(int32_t) { ++physicalHairCalls; }
void PhysicalBodySetup(int game,bool armed=false) {
    using namespace tr;
    BodyRig(game,armed);
    testConfig.firstPersonFullBodyIK=testConfig.firstPersonPhysicalCrouch=true;
    g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&ObservePhysicalBodyDraw);
    g_hDrawHair.m_trampoline=reinterpret_cast<void*>(&ObservePhysicalHair);
    testHands[0]={};testHands[1]={};g_original=nullptr;g_viewToggleHeld=false;
    physicalDrawMask=0;physicalDrawCalls=physicalHairCalls=0;
}
void TestPhysicalBodyHide() {
    using namespace tr;
    for(int game:{1,2}) for(bool armed:{false,true}) {
        PhysicalBodySetup(game,armed);
        const auto nativeBits=Field<uint32_t>(off::item_mesh_bits);
        for(float drop:{0.f,.35f,.54f}) {
            testViewRise=-drop;FirstPersonPhysicalDuck();
            Check(!HidePhysicalCrouchBody(item),"TR1/2 partial squat retains body");
        }
        testViewRise=-.55f;
        Check(!FirstPersonPhysicalDuck() && HidePhysicalCrouchBody(item),
            "TR1/2 hides at TR3 threshold without requesting native Duck");
        Check(!PhysicalCrouchEligible(item) && !g_physicalCrouchCamera,
            "TR1/2 never enters native crouch latch or camera compensation");
        Check(FirstPersonBodyMask(item)==firstperson::HandMeshBits,
            "physical TR1/2 crouch uses same hands-only mask as TR3");
        Detour_DrawCreatureHD(item,0);
        if(armed) {
            Field<uint32_t>(off::item_mesh_bits)=0x3600;
            Detour_DrawCreatureHD(item,1);
            Field<uint32_t>(off::item_mesh_bits)=nativeBits;
        }
        Check(physicalDrawMask==firstperson::HandMeshBits && physicalDrawCalls>0,
            "native eye draws hide torso, legs and forearms but preserve hands");
        Check(Field<uint32_t>(off::item_mesh_bits)==nativeBits,
            "draw mask does not modify persistent native meshes");
        Detour_DrawHair(0);Check(physicalHairCalls==0,"physical crouch hides braid");
        *Ptr<int32_t>(dll.renderPass)=4;physicalDrawMask=0;
        Detour_DrawCreatureHD(item,0);Detour_DrawHair(0);
        Check(physicalDrawMask==nativeBits && physicalHairCalls==1,
            "native shadow keeps body and hair");
        *Ptr<int32_t>(dll.renderPass)=0;
        testViewRise=-.3f;FirstPersonPhysicalDuck();
        Check(HidePhysicalCrouchBody(item),"body remains hidden between thresholds");
        testViewRise=-.23f;FirstPersonPhysicalDuck();
        Check(!HidePhysicalCrouchBody(item) && FirstPersonBodyMask(item)!=firstperson::HandMeshBits,
            "physical rise restores body at shared release threshold");
        for(bool jump:{false,true}) {
            testViewRise=-.6f;FirstPersonPhysicalDuck();
            Check(HidePhysicalCrouchBody(item),"fresh physical crouch hides body again");
            testHands[0].grip=jump?0.f:1.f;testHands[1].btnLower=jump;
            XState pad{};Detour_XInputGetState(0,&pad);
            Check(!HidePhysicalCrouchBody(item) && g_heightCrouch.standingOverride,
                "VR Duck/Stand or Jump restores body while headset stays low");
            Check((pad.Gamepad.wButtons & (jump?XB_A:XB_LEFT_SHOULDER))!=0,
                "body visibility does not consume native VR button action");
            testHands[0]={};testHands[1]={};
            for(int i=0;i<5;++i) Detour_XInputGetState(0,&pad);
            Check(!HidePhysicalCrouchBody(item),"released stand button cannot immediately rehide body");
            Field<int16_t>(off::item_anim_state)=3;Field<uint16_t>(off::item_flags)=8;
            FirstPersonPhysicalDuck();
            Field<int16_t>(off::item_anim_state)=2;Field<uint16_t>(off::item_flags)=0;
            FirstPersonPhysicalDuck();
            Check(!HidePhysicalCrouchBody(item),"stand override survives jump and landing while HMD stays low");
            testViewRise=0;FirstPersonPhysicalDuck();
        }
        // Pressing a control while upright must not disable the next squat.
        FirstPersonPhysicalDuck(true,true);FirstPersonPhysicalDuck(true,false);
        testViewRise=-.6f;FirstPersonPhysicalDuck();
        Check(HidePhysicalCrouchBody(item),"standing button press does not suppress a later crouch");
        ResetBodyFollower();Check(!HidePhysicalCrouchBody(item),"view/level/recenter reset clears physical hiding");
    }
    for(int game:{1,2}) for(int excluded=0;excluded<12;++excluded) {
        PhysicalBodySetup(game);testViewRise=-.6f;FirstPersonPhysicalDuck();
        switch(excluded) {
        case 0:g_active=false;break;
        case 1:testConfig.firstPersonPhysicalCrouch=false;break;
        case 2:testConfig.firstPersonFullBodyIK=false;break;
        case 3:testConfig.positionalTracking=false;break;
        case 4:testConfig.firstPersonHeadTranslation=false;break;
        case 5:testVR.m_poseValid=false;break;
        case 6:testWater=1;break;
        case 7:Field<int16_t>(off::item_hit_points)=0;break;
        case 8:testInventory=1;break;
        case 9:*Ptr<uint16_t>(dll.lara+60)|=0x2000;break;
        case 10:Field<uint16_t>(off::item_flags)=8;break;
        case 11:Field<int16_t>(off::item_anim_state)=56;break;
        }
        Check(!HidePhysicalCrouchBody(item),"physical hiding releases immediately outside live grounded first person");
    }
    PhysicalBodySetup(3);testViewRise=-.6f;
    Check(FirstPersonPhysicalDuck(true,true) && g_physicalCrouchCamera && !HidePhysicalCrouchBody(item),
        "TR3 keeps native crouch even when stand override button is passed");
    testViewRise=-.23f;Check(!FirstPersonPhysicalDuck(),"TR3 still releases on physical rise");
    Reset(2,{},true);SetMotionHooks(false);testViewRise=0;
}
}
