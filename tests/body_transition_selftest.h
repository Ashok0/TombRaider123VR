namespace {
#include "sidearm_animation_steps.inl"
void TestBodyTransitions() {
    using namespace tr;using namespace tr::motiongun;
    for(int game:{1,2,3}) for(bool fullBody:{false,true})
    for(float yaw:{0.f,1.5707963f,-1.5707963f}) for(float cm:{0.f,5.f,11.f,20.f}) {
        BodyRig(game,false);g_holsters.active=true;g_holsters.weapon=1;testConfig.firstPersonFullBodyIK=fullBody;testConfig.firstPersonBodyScale=1;
        testConfig.firstPersonBodyForwardCm=cm;g_groundEye.valid=true;g_groundEye.bodyLocal={};
        g_haveManualInput=false;g_heading.base=yaw;
        Field<PHD_3DPOS>(off::item_pos).y_rot=Field<PHD_3DPOS>(off::item_pos_prev).y_rot=locomotion::Angle(yaw);
        auto* obj=Ptr<uint8_t>(dll.objects);
        int step=0;Frame standingChest{};
        for(int status:{0,2,4,3,0,0}) {
            *Ptr<int16_t>(dll.lara+2)=int16_t(status);
            Field<int16_t>(off::item_anim_state)=step<2 ? 1 : 2;
            Field<uint32_t>(off::item_mesh_bits)=0x3fff;g_bodySkinScope=true;
            for(int j=0;j<15;++j) {
                const auto* bind=Ptr<float>(dll.objects+192)+j*16;
                const motiongun::Vec point{-bind[12],-bind[13],-bind[14]};
                Frame animated{firstperson::IdentityBasis(),Add(point,{step*13.f,0,-step*39.f})};
                if(j>=7) {
                    const Basis pitch=firstperson::RotationMatrix(firstperson::AxisRotation({1,0,0},step*.055f));
                    const motiongun::Vec pivot{step*13.f,-650,-step*39.f};
                    animated=Multiply({pitch,Sub(Add(pivot,{0,0,-step*23.f}),Transform(pitch,pivot))},animated);
                }
                WriteRows(Multiply(animated,{firstperson::IdentityBasis(),Scale(point,-1)}),ikNative+j*12);
                for(auto offset:{off::item_joints_cur,off::item_joints_prev}) {
                    auto* joint=reinterpret_cast<int32_t*>(item+offset+j*48);
                    float rows[12];WriteRows(animated,rows);
                    for(int k=0;k<12;++k) joint[k]=int32_t(rows[k]*16384);
                }
            }
            ++testVR.m_poseSerial;
            if(fullBody) {
                float palette[384];std::memcpy(palette,ikNative,sizeof(palette));
                Check(ApplyFullBodyIK(item,palette,15),"transition regression exercises complete body IK");
            }
            Detour_GetJoints(item);uint32_t mask=0;const auto* render=FirstPersonBodySkin(mask);
            Frame hip{};Check(render && ReadIKJoint(obj,render,15,0,nullptr,nullptr,0,hip),"transition pelvis is readable");
            Check(std::fabs(hip.origin.x-std::sin(yaw)*cm*10)<.05f && std::fabs(hip.origin.z-std::cos(yaw)*cm*10)<.05f,
                  "run-to-stop animation cannot move body from its configured position");
            Frame chest{};ReadIKJoint(obj,render,15,0,nullptr,nullptr,7,chest);
            if(step==0) standingChest=chest;
            Check(std::fabs(chest.origin.x-standingChest.origin.x)<.06f && std::fabs(chest.origin.z-standingChest.origin.z)<.06f && std::fabs(chest.origin.y-standingChest.origin.y)<.06f,
                  "native running chest and stop blend cannot move the visible torso backwards");
            for(int r=0;r<3;++r) for(int c=0;c<3;++c) Check(std::fabs(chest.basis.r[r][c]-standingChest.basis.r[r][c])<.001f,
                  "chest mesh cannot pitch back into the headset on stopping");
            float sphere[4]{};uint64_t clearanceMask=0;
            Check(FirstPersonBodyClearance(sphere,clearanceMask) && sphere[3]==200 && clearanceMask==0x40ff,
                  "clearance affects body and excludes tracked arms/hands");
            Ptr<int32_t>(dll.w2vMatrix)[3]=32;
            float otherEye[4]{};uint64_t otherMask=0;FirstPersonBodyClearance(otherEye,otherMask);
            Check(std::fabs(otherEye[0]+32-sphere[0])<.01f && otherMask==clearanceMask,"clearance is stable between stereo origins");
            Ptr<int32_t>(dll.w2vMatrix)[3]=0;
            for(int excluded=0;excluded<4;++excluded) {
                const bool activeBefore=g_active,scopeBefore=g_bodySkinScope;const int armBefore=g_renderArm;
                if(excluded==0) g_active=false;
                if(excluded==1) g_bodySkinScope=false;
                if(excluded==2) g_renderArm=1;
                if(excluded==3) *Ptr<int32_t>(dll.renderPass)=4;
                Check(!FirstPersonBodyClearance(sphere,clearanceMask) && sphere[3]==0 && clearanceMask==0,
                      "third-person, non-body, tracked hand and shadow draws cannot inherit clearance");
                g_active=activeBefore;g_bodySkinScope=scopeBefore;g_renderArm=armBefore;*Ptr<int32_t>(dll.renderPass)=0;
            }
            for(int hand=0;hand<2;++hand) {
                Frame wrist{},shown{};Basis controller{};BuildControllerWrist(hand,wrist,controller);
                Check(ReadIKJoint(obj,render,15,0,nullptr,nullptr,hand?10:13,shown) && firstperson::Length(Sub(shown.origin,wrist.origin))<.05f,
                      "draw and holster preserve live controller wrists");
            }
            Check(!std::memcmp(FirstPersonNativeBodySkin(),ikNative,15*12*sizeof(float)),"transition physics stays native");
            ++step;
        }
    }
    for(int game:{1,2,3}) for(bool fullBody:{false,true}) for(int weapon:{1,2,3}) {
        BodyRig(game,false);testConfig.firstPersonFullBodyIK=fullBody;testConfig.firstPersonBodyScale=1;
        testConfig.firstPersonBodyForwardCm=11;testConfig.firstPersonBodyHandYawWeight=0;
        g_groundEye.valid=true;g_groundEye.bodyLocal={};g_haveManualInput=false;
        *Ptr<int16_t>(dll.lara+4)=int16_t(weapon);g_holsters.active=false;
        auto* obj=Ptr<uint8_t>(dll.objects);
        auto render=[&](int status,bool repeat) {
            *Ptr<int16_t>(dll.lara+2)=int16_t(status);Field<uint32_t>(off::item_mesh_bits)=0x3fff;
            g_pistolBodyScope=g_bodySkinScope=true;g_pistolForearmScope=false;
            if(!repeat) ++testVR.m_poseSerial;
            Detour_GetJoints(item);uint32_t mask=0;const auto* palette=FirstPersonBodySkin(mask);
            std::array<Frame,15> result{};
            for(int j=0;j<15;++j) Check(palette && ReadIKJoint(obj,palette,15,0,nullptr,nullptr,j,result[j]),"sidearm body joints are readable");
            if(PistolBodyPoseMatches(item,0)) {
                Field<uint32_t>(off::item_mesh_bits)=0x3600;g_pistolBodyScope=false;g_pistolForearmScope=true;
                Detour_GetJoints(item);palette=FirstPersonBodySkin(mask);
                for(int j=8;j<14;++j) {
                    Frame shown{};ReadIKJoint(obj,palette,15,0,nullptr,nullptr,j,shown);
                    Check(firstperson::Length(Sub(shown.origin,result[j].origin))<.05f,"attachments share the animated sidearm wrists");
                }
            }
            g_pistolBodyScope=g_pistolForearmScope=false;
            return result;
        };
        ExerciseSidearmAnimation(render,[&] {
            for(int hand=0;hand<2;++hand) testControllerOffset[hand].x+=.05f;
        },[&] {
            for(int j=8;j<14;++j) {
                ikNative[j*12+11]+=30;
                for(auto offset:{off::item_joints_cur,off::item_joints_prev})
                    reinterpret_cast<int32_t*>(item+offset+j*48)[11]+=30*16384;
            }
        });
    }
    for(int game:{1,2,3}) for(bool armed:{false,true}) {
        PhysicalBodySetup(game,armed);
        for(int state:{23,45,74}) {
            Field<int16_t>(off::item_anim_state)=int16_t(state);
            const bool roll=state!=74 || game==3;
            Check(IsRollState(item)==roll,"sprint somersault exists in TR3; ordinary rolls are shared");
            SetMeshVisibility(true,roll);
            Check(g_rollHidden==roll,"tracked controllers do not cancel somersault hiding");
            physicalDrawCalls=0;Detour_DrawCreatureHD(item,0);
            if(roll) Check(physicalDrawCalls==0,"somersault suppresses HD body even with weapon tracking");
        }
        SetMeshVisibility(false,false);
        for(int restIndex:{1,6}) for(int status:{2,3}) {
            IKSetup(game);*Ptr<int16_t>(dll.lara+2)=int16_t(status);Field<uint32_t>(off::item_mesh_bits)=0x3600;
            auto* rest=Ptr<uint8_t>(dll.laraHands)+restIndex*104;auto* open=rest+3*104;
            auto* geom=Ptr<uint8_t>(dll.objects+88);
            std::memset(rest,1,104);std::memset(open,4,104);
            *reinterpret_cast<void**>(rest+16)=reinterpret_cast<void*>(0x1234);
            std::memcpy(geom,open,104);
            { UnarmedRestHandScope scope(item,1);Check(!std::memcmp(geom,rest,104),"draw/holster keeps relaxed fingers instead of open hands"); }
            Check(!std::memcmp(geom,open,104),"native hand descriptor is restored after rendering");
        }
    }
}
}
