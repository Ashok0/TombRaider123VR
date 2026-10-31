namespace {
void TestJumpBodyCentering() {
    using namespace tr;
    using namespace tr::motiongun;
    for(int game:{1,2,3}) for(bool armed:{false,true}) for(float heading:{0.f,.8f,-2.f})
    for(bool cameraMatrix:{false,true}) {
        BodyRig(game,armed);
        if(!cameraMatrix) dll.w2vMatrix=0;
        g_pistolBodyScope=armed;
        testConfig.firstPersonFullBodyIK=testConfig.firstPersonMovementStabilization=true;
        testConfig.firstPersonBodyScale=1;
        g_heading.base=heading;testHeadYaw=0;
        g_groundEye.Apply({0,0,0},heading,{0,-850,0},heading);
        g_dragCurrent=g_dragPrevious=g_dragShown={};g_previousBody={600,400};
        testViewRight=testFloorRight=.18f;testViewForward=testFloorForward=-.12f;
        testControllerOffset[0].x+=testViewRight;testControllerOffset[1].x+=testViewRight;
        testControllerOffset[0].z+=testViewForward;testControllerOffset[1].z+=testViewForward;
        Field<uint32_t>(off::item_mesh_bits)=0x3fff;
        float source[384];std::memcpy(source,ikNative,sizeof(source));
        auto render=[&](int state,int fraction,int step) {
            testViewRight=.18f+(state==2 ? 0.f : fraction*.0001f);
            testViewForward=-.12f-(state==2 ? 0.f : fraction*.00005f);
            testControllerOffset[0]={testViewRight-.25f,.35f,testViewForward+.1f};
            testControllerOffset[1]={testViewRight+.25f,.35f,testViewForward+.1f};
            Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(state);
            Field<uint16_t>(off::item_flags)=state==2 ? 0 : 8;
            auto& current=Field<PHD_3DPOS>(off::item_pos);
            auto& previous=Field<PHD_3DPOS>(off::item_pos_prev);
            previous={600+step*30,-step*40,400+step*20};current=previous;
            current.x_pos+=30;current.y_pos-=40;current.z_pos+=20;
            current.y_rot=previous.y_rot=locomotion::Angle(heading);
            *Ptr<int32_t>(dll.frameFrac)=fraction;
            const motiongun::Vec root{float(Lerp(previous.x_pos,current.x_pos,fraction)),
                           float(Lerp(previous.y_pos,current.y_pos,fraction)),
                           float(Lerp(previous.z_pos,current.z_pos,fraction))};
            // Native skinning follows the collision root, not the remaining
            // physical tracking offset after a roomscale step.
            std::memcpy(ikNative,source,sizeof(source));
            for(int j=0;j<15;++j) {
                ikNative[j*12+3]+=root.x;ikNative[j*12+7]+=root.y;ikNative[j*12+11]+=root.z;
            }
            PHD_3DPOS eye{int32_t(root.x),int32_t(root.y-850),int32_t(root.z)};
            const auto animated=eye;UpdateLocomotion(eye);FitBodyToRenderedEye(item,eye,animated);
            g_scenePose=eye;g_scenePoseValid=true;
            const auto physical=locomotion::Rotate({testViewRight,testViewForward},heading)*LiveWorldUnitsPerMetre();
            const auto before=std::string(reinterpret_cast<char*>(item),sizeof(item));
            ++testVR.m_poseSerial;
            for(int pass:{0,2}) {
                *Ptr<int32_t>(dll.renderPass)=pass;g_bodySkinScope=true;Detour_GetJoints(item);
                Frame hips{};
                Check(ReadIKJoint(Ptr<uint8_t>(dll.objects),g_bodyRenderPalette,15,0,nullptr,nullptr,0,hips),
                    "jump-centering fixture resolves rendered pelvis");
                if(std::fabs(hips.origin.x-root.x-physical.x)>=1 || std::fabs(hips.origin.z-root.z-physical.z)>=1)
                    std::fprintf(stderr,"centering game=%d state=%d pass=%d root=(%.1f,%.1f) hips=(%.1f,%.1f) tracking=(%.1f,%.1f)\n",
                        game,state,pass,root.x,root.z,hips.origin.x,hips.origin.z,physical.x,physical.z);
                Check(std::fabs(hips.origin.x-root.x-physical.x)<1 && std::fabs(hips.origin.z-root.z-physical.z)<1,
                    "roomscale body stays under physical player through standing, jump and landing");
                if(state!=2) {
                    for(int j:{0,1,2,3,4,5,6,7,14}) {
                        const auto actual=ReadRows(g_bodyRenderPalette+j*12),native=ReadRows(ikNative+j*12);
                        Check(WristBasisError(actual.basis,native.basis)<.0001f &&
                              std::fabs(actual.origin.y-native.origin.y)<.001f,
                            "airborne centering preserves native limb rotations and vertical animation");
                    }
                    if(MotionReady() || UnarmedIKReady()) for(int hand:{0,1}) {
                        Frame target{},wrist{};Basis controller{};
                        Check(BuildControllerWrist(hand,target,controller) &&
                            ReadIKJoint(Ptr<uint8_t>(dll.objects),g_bodyRenderPalette,15,0,nullptr,nullptr,hand?10:13,wrist),"tracked wrist fixture resolves");
                        if(firstperson::Length(Sub(wrist.origin,target.origin))>=.1f)
                            std::fprintf(stderr,"wrist game=%d armed=%d state=%d hand=%d target=(%.1f,%.1f,%.1f) actual=(%.1f,%.1f,%.1f) ready=%d/%d\n",
                                game,int(armed),state,hand,target.origin.x,target.origin.y,target.origin.z,wrist.origin.x,wrist.origin.y,wrist.origin.z,int(MotionReady()),int(UnarmedIKReady()));
                        Check(
                            firstperson::Length(Sub(wrist.origin,target.origin))<.1f,
                            "airborne body shift does not move tracked wrists away from controllers");
                    }
                }
                Check(before==std::string(reinterpret_cast<char*>(item),sizeof(item)),
                    "body centering leaves native root, skeleton, velocity and gravity untouched");
            }
        };
        render(2,256,0);
        for(int state:{3,9,25,26,27,28,29}) for(int fraction:{0,64,128,192,256}) render(state,fraction,3);
        render(2,256,0);
    }
    for(int exclusion=0;exclusion<11;++exclusion) {
        BodyRig(3,false);testConfig.firstPersonFullBodyIK=true;
        g_groundEye.Apply({0,0,0},0,{0,-850,0},0);
        Field<int16_t>(off::item_anim_state)=3;Field<uint16_t>(off::item_flags)=8;
        if(exclusion==0) g_active=false;
        if(exclusion==1) testConfig.firstPersonFullBodyIK=false;
        if(exclusion==2) testConfig.firstPersonMovementStabilization=false;
        if(exclusion==3) testConfig.positionalTracking=false;
        if(exclusion==4) testConfig.firstPersonHeadTranslation=false;
        if(exclusion==5) testVR.m_poseValid=false;
        if(exclusion==6) g_groundEye.Reset();
        if(exclusion==7) testWater=1;
        if(exclusion==8) Field<int16_t>(off::item_hit_points)=0;
        if(exclusion==9) *Ptr<int16_t>(dll.lara+40)=1;
        if(exclusion==10) Field<int16_t>(off::item_anim_state)=10;
        Check(!CanFitAirborneBody(item),"airborne centering excludes inactive tracking, opt-outs, water, death, vehicles and ledge grips");
    }
    g_pistolBodyScope=false;
    testViewRight=testViewForward=testFloorRight=testFloorForward=testHeadYaw=0;
    testControllerOffset[0]=testControllerOffset[1]={};
}
}
