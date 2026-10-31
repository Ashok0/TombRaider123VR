namespace {
void TestClimbingArms() {
    using namespace tr;
    using namespace tr::motiongun;
    using namespace tr::firstperson;
    using V=motiongun::Vec;
    // Exercise grip reach independently of animation/renderer fixtures.
    for(float yaw:{-2.4f,0.f,1.2f}) for(float bend:{0.f,.001f,-.001f,20.f,90.f})
    for(float side:{-1.f,1.f}) for(V offset:{V{0,0,0},{500,300,-200},{-500,-400,200},{0,900,0}}) {
        const Frame rotation{RotationMatrix(AxisRotation({0,1,0},yaw)),{}};
        Frame arm[3]={
            {IdentityBasis(),{side*180,-700,0}},
            {IdentityBasis(),{side*(180+bend),-930,20}},
            {IdentityBasis(),{side*180,-1160,40}}};
        for(auto& frame:arm) frame=Multiply(rotation,frame);
        Frame solved[3]{};
        const V pole=Transform(rotation.basis,{side*.6f,1.f,-.25f});
        Check(SolveGripArm(arm,Add(arm[0].origin,Transform(rotation.basis,offset)),pole,solved),
            "climbing arm solves extreme headset offsets and near-straight native elbows");
        Check(!std::memcmp(&solved[2],&arm[2],sizeof(Frame)),"climbing wrist contact and orientation remain exactly authored");
        for(int i=0;i<2;++i) {
            Check(std::fabs(Length(Sub(solved[i+1].origin,solved[i].origin))-Length(Sub(arm[i+1].origin,arm[i].origin)))<.01f,
                "grip solve never stretches upper arm or forearm");
            for(V axis:{V{1,0,0},{0,1,0},{0,0,1}})
                Check(std::fabs(Length(Transform(solved[i].basis,axis))-1)<.0001f,"grip meshes retain rigid unit scale");
        }
    }
    // Near a straight native elbow, tiny alternating noise must not pick the
    // opposite side of the arm. Keep a small reachable shoulder displacement.
    Frame previous[3]{};
    for(int i=0;i<40;++i) {
        Frame arm[3]={ {IdentityBasis(),{0,0,0}}, {IdentityBasis(),{(i%2?1.f:-1.f)*.001f,-250,0}},
                       {IdentityBasis(),{0,-500,0}} },solved[3]{};
        Check(SolveGripArm(arm,{0,-25,0},{1,1,-.25f},solved),"straight-grip stability fixture solves");
        if(i) Check(Length(Sub(previous[1].origin,solved[1].origin))<.01f,"near-straight authored elbow noise cannot flip the bend plane");
        std::memcpy(previous,solved,sizeof(solved));
    }
    for(int game:{1,2,3}) for(int count:{15,32}) {
        BodyRig(game,false);testConfig.firstPersonFullBodyIK=true;
        *Ptr<int16_t>(dll.lara+2)=1; // Native hands-busy while hanging.
        auto* obj=Ptr<uint8_t>(dll.objects);auto* geom=obj+off::object_geom;
        Frame native[15]{},bind[32]{},inverse[32]{};
        int mapping[32]{};float poses[32*12]{};
        for(int j=0;j<15;++j) Check(ReadIKJoint(obj,ikNative,15,0,nullptr,nullptr,j,native[j]),"climbing fixture reads native skeleton");
        for(int hand=0;hand<2;++hand) {
            const int first=hand?8:11;const float side=hand?1.f:-1.f;
            native[first].origin={side*180,-700,0};
            native[first+1].origin={side*205,-940,30};
            native[first+2].origin={side*180,-1170,60};
        }
        for(int i=0;i<count;++i) {
            const int j=mapping[i]=count==15?i:(i*7)%15;
            Check(ReadIKBind(obj,15,0,nullptr,nullptr,j,bind[i]) && Inverse(bind[i],inverse[i]),"climbing fixture recovers model bind");
            auto raw=inverse[i];for(int r=0;r<3;++r) {raw.basis.r[r][1]=inverse[i].basis.r[r][2];raw.basis.r[r][2]=-inverse[i].basis.r[r][1];}
            WriteRows(raw,poses+i*12);
            WriteRows(Multiply(native[j],inverse[i]),ikNative+i*12);
        }
        if(count>15) {
            *reinterpret_cast<int*>(geom+28)=count;*reinterpret_cast<int**>(geom+48)=mapping;
            *reinterpret_cast<float**>(geom+72)=poses;
        }
        for(int state:{10,30,31,19,54,75,76,77,78,79,82,83}) {
            if(game!=3 && state>=75) continue;
            Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(state);
            Check(FullBodyGripState(item) && FullBodyIKReady(),"every ledge and TR3 monkey movement state retains grip IK");
            Check(FirstPersonBodyMask(item)==(locomotion::IsLedgeMountState(state,game) ? HandMeshBits : ArmMeshBits),
                "mounts show hands only; monkey traversal retains arms without a torso flash");
            for(V headOffset:{V{0,0,0},{.35f,.2f,-.3f},{-.35f,-.2f,.3f}}) {
                testViewRight=headOffset.x;testViewRise=headOffset.y;testViewForward=headOffset.z;
                uint8_t savedItem[sizeof(item)]{};std::memcpy(savedItem,item,sizeof(item));
                float firstEye[384]{};
                ++testVR.m_poseSerial;
                for(int eye=0;eye<2;++eye) {
                    Field<uint32_t>(off::item_mesh_bits)=ArmMeshBits;g_bodySkinScope=true;
                    Detour_GetJoints(item);
                    uint32_t mask=0;const auto* palette=FirstPersonBodySkin(mask);
                    Check(palette && g_torsoPose.lastGrip,"grip solver stays active through movement animation states");
                    for(int hand=0;hand<2;++hand) {
                        const int first=hand?8:11;Frame chain[3]{};
                        for(int j=0;j<3;++j) Check(ReadIKJoint(obj,palette,count,count>15?count:0,
                            count>15?mapping:nullptr,count>15?poses:nullptr,first+j,chain[j]),"rendered grip joints are valid");
                        Check(Length(Sub(chain[2].origin,native[first+2].origin))<.01f &&
                            WristBasisError(chain[2].basis,native[first+2].basis)<.0001f,"grip wrist keeps native world contact and orientation");
                        for(int j=0;j<2;++j) Check(std::fabs(Length(Sub(chain[j+1].origin,chain[j].origin))-
                            Length(Sub(native[first+j+1].origin,native[first+j].origin)))<.02f,"production climbing palette preserves arm lengths");
                    }
                    if(!eye) std::memcpy(firstEye,palette,count*12*sizeof(float));
                    else Check(!std::memcmp(firstEye,palette,count*12*sizeof(float)),"both eye passes receive identical climbing arms");
                }
                // Only the test's explicit draw mask was written to the item.
                Field<uint32_t>(off::item_mesh_bits)=*reinterpret_cast<uint32_t*>(savedItem+off::item_mesh_bits);
                Check(!std::memcmp(savedItem,item,sizeof(item)),"climbing visual solve never changes native simulation data");
            }
        }
        if(game!=3) for(int state:{76,77,78,79}) {
            Field<int16_t>(off::item_anim_state)=int16_t(state);
            Check(!FullBodyGripState(item),"TR3 monkey traversal IDs are not enabled in TR1/2");
        }
        *Ptr<int32_t>(dll.renderPass)=4;Detour_GetJoints(item);
        Check(!std::memcmp(Ptr<float>(dll.joints),ikNative,count*12*sizeof(float)),"climbing fitting leaves native shadow animation untouched");
    }
    Reset(2,{},true);SetMotionHooks(false);testViewRight=testViewRise=testViewForward=0;
}
}
