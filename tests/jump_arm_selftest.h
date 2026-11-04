// Exercise TR1-3's actual render hooks through takeoff, flight and touchdown.
namespace {
void TestJumpArms() {
    using namespace tr;using namespace tr::motiongun;
    for(int game:{1,2,3}) for(bool fullBody:{false,true}) for(int count:{15,32}) {
        BodyRig(game,false);testConfig.firstPersonFullBodyIK=fullBody;
        testConfig.firstPersonBodyScale=1;testConfig.firstPersonMovementStabilization=true;
        g_groundEye.valid=true;g_groundEye.local={0,-850,0};
        Field<uint32_t>(off::item_mesh_bits)=0x7fff;
        auto* obj=Ptr<uint8_t>(dll.objects);auto* geom=obj+off::object_geom;
        Frame native[15]{},inverse[15]{},bind[15]{},baseline[15]{};
        for(int j=0;j<15;++j) {
            Check(ReadIKJoint(obj,ikNative,15,0,nullptr,nullptr,j,native[j]) &&
                  ReadIKBind(obj,15,0,nullptr,nullptr,j,bind[j]) && Inverse(bind[j],inverse[j]),
                  "jump fixture reads native joint frames and bindings");
        }
        int mapping[32]{};float poses[32*12]{};
        for(int i=0;i<count;++i) {
            const int j=mapping[i]=count==15 ? i : (i*7)%15;
            auto raw=inverse[j];
            for(int r=0;r<3;++r) {raw.basis.r[r][1]=inverse[j].basis.r[r][2];raw.basis.r[r][2]=-inverse[j].basis.r[r][1];}
            WriteRows(raw,poses+i*12);
        }
        *reinterpret_cast<int*>(geom+28)=count==15 ? 0 : count;
        *reinterpret_cast<int**>(geom+48)=mapping;*reinterpret_cast<float**>(geom+72)=poses;
        for(int step=0;step<5;++step) {
            const int state=step==0 || step==4 ? 2 : step==1 ? 15 : 28;
            Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(state);
            Field<int16_t>(off::item_anim_number)=state==28 ? 91 : 103;
            Field<uint16_t>(off::item_flags)=state==28 ? 8 : 0;
            ++testVR.m_poseSerial;
            for(int eye=0;eye<2;++eye) {
                const motiongun::Vec camera{300.f+6*eye,-200,-500};
                auto* w=Ptr<int32_t>(dll.w2vMatrix);
                w[3]=int(camera.x);w[7]=int(camera.y);w[11]=int(camera.z);
                for(int i=0;i<count;++i) {
                    const int j=mapping[i];Frame frame=native[j];
                    if(step && j>=7 && j<=13) {
                        const Frame turn{firstperson::RotationMatrix(firstperson::AxisRotation({1,0,0},step*.35f)),{0,120,-90}};
                        frame=Multiply(turn,frame);
                    }
                    frame.origin=Sub(frame.origin,camera);
                    WriteRows(Multiply(frame,inverse[j]),ikNative+i*12);
                }
                // Deliberately disagree with floating HD wrist interpolation.
                for(int j:{10,13}) for(auto offset:{off::item_joints_prev,off::item_joints_cur}) {
                    auto* p=reinterpret_cast<int32_t*>(item+offset+j*48);
                    p[11]=int32_t((native[j].origin.z+(step%2 ? 160 : -130))*16384);
                }
                Field<uint32_t>(off::item_mesh_bits)=0x7fff;g_bodySkinScope=true;
                Check(UnarmedIKReady(),"all three games retain tracked arms through takeoff and touchdown");
                Detour_GetJoints(item);
                uint32_t mask=0;const auto* render=FirstPersonBodySkin(mask);
                Check(render && (FirstPersonBodyMask(item)&firstperson::ArmMeshBits)==firstperson::ArmMeshBits,
                      "jump does not hide arms or hands");
                float saved[384];std::memcpy(saved,render,sizeof(saved));
                Check(!std::memcmp(FirstPersonNativeBodySkin(),ikNative,count*12*sizeof(float)),
                      "jump stabilization preserves the complete physics skeleton");
                Detour_GetJoints(item);render=FirstPersonBodySkin(mask);
                Check(!std::memcmp(saved,render,count*12*sizeof(float)),"redrawing a stereo frame cannot advance arm recovery");
                for(int i=0;i<count;++i) {
                    const int j=mapping[i];
                    auto actual=Multiply(ReadRows(render+i*12),bind[j]);actual.origin=Add(actual.origin,camera);
                    if(step==0 && eye==0) baseline[j]=actual;
                    if(j>=8 && j<=13) {
                        Check(firstperson::Length(Sub(actual.origin,baseline[j].origin))<.1f &&
                              WristBasisError(actual.basis,baseline[j].basis)<.001f,
                              "takeoff and landing native shoulder snaps cannot flash either eye's arms");
                    }
                    if(state==28 && j<8) Check(!std::memcmp(render+i*12,ikNative+i*12,12*sizeof(float)),
                                              "native airborne torso and legs remain animated");
                    if(j==10 || j==13) {
                        Frame target{};Basis controller{};BuildControllerWrist(j==10 ? 1 : 0,target,controller);
                        Check(firstperson::Length(Sub(actual.origin,target.origin))<.04f &&
                              WristBasisError(actual.basis,target.basis)<.001f,"tracked wrists stay exact across native animation changes");
                    }
                }
                Field<uint32_t>(off::item_mesh_bits)=0x3600;Detour_GetJoints(item);render=FirstPersonBodySkin(mask);
                for(int i=0;i<count;++i) if(mapping[i]>=8 && mapping[i]<=13)
                    for(int k=0;k<12;++k) Check(std::fabs(render[i*12+k]-saved[i*12+k])<.04f,
                        "hand attachments share the body's continuous arm pose");
            }
        }
        // Mount recovery must not turn the next straight-up jump into floating hands.
        Field<int16_t>(off::item_anim_state)=19;FirstPersonBodyMask(item);
        for(int state:{15,28}) {
            Field<int16_t>(off::item_anim_state)=int16_t(state);
            Check((FirstPersonBodyMask(item)&firstperson::ArmMeshBits)==firstperson::ArmMeshBits,
                  "previous ledge recovery cannot hide up-jump arms");
        }
        Field<int16_t>(off::item_anim_state)=28;
        testControllerOffset[0].x+=.1f;++testVR.m_poseSerial;
        Field<uint32_t>(off::item_mesh_bits)=0x7fff;Detour_GetJoints(item);
        Frame wrist{},target{};Basis controller{};
        Check(ReadIKJoint(obj,g_bodyRenderPalette,count,count==15 ? 0 : count,
                          count==15 ? nullptr : mapping,count==15 ? nullptr : poses,13,wrist) &&
              BuildControllerWrist(0,target,controller) &&
              firstperson::Length(Sub(Add(wrist.origin,{float(Ptr<int32_t>(dll.w2vMatrix)[3]),-200,-500}),target.origin))<.04f,
              "moving a controller during a jump is never delayed by arm continuity");
        for(int reason=0;reason<4;++reason) {
            if(reason==0) *Ptr<int16_t>(dll.lara+2)=1;
            if(reason==1) { *Ptr<int16_t>(dll.lara+2)=0;testWater=1; }
            if(reason==2) { testWater=0;testMissingHand=0; }
            if(reason==3) { testMissingHand=-1;testConfig.firstPersonUnarmedIK=false; }
            Check(!UnarmedIKReady(),"busy hands, water, tracking loss and disabled IK retain native ownership");
        }
    }
    for(int game:{1,2,3}) for(int restIndex:{1,6}) for(int variant:{1,3}) {
        IKSetup(game);Field<int16_t>(off::item_anim_state)=28;Field<uint32_t>(off::item_mesh_bits)=0x3600;
        auto* rest=Ptr<uint8_t>(dll.laraHands)+restIndex*off::geom_stride;
        auto* selected=rest+variant*off::geom_stride;auto* geom=Ptr<uint8_t>(dll.objects)+off::object_geom;
        std::memset(rest,1,off::geom_stride);std::memset(selected,2,off::geom_stride);
        *reinterpret_cast<void**>(rest+off::geom_mesh)=*reinterpret_cast<void**>(selected+off::geom_mesh)=reinterpret_cast<void*>(0x1234);
        std::memcpy(geom,selected,off::geom_stride);
        { UnarmedRestHandScope scope(item,1);Check(!std::memcmp(geom,rest,off::geom_stride),
            "bare and gloved jumping hands retain the standing hand pose"); }
        Check(!std::memcmp(geom,selected,off::geom_stride),"hand substitution restores the complete native descriptor");
        if(variant==3) {
            Field<int16_t>(off::item_anim_state)=11;
            { UnarmedRestHandScope scope(item,1);Check(!std::memcmp(geom,selected,off::geom_stride),
                "native reach keeps the authored open hand"); }
        }
    }
    firstperson::JumpArmIK history;
    const auto identity=firstperson::IdentityBasis();
    const Frame reference{identity,{200,-700,300}};
    const Frame initial[3]={{identity,{100,-600,300}},{identity,{150,-400,330}},{identity,{100,-200,370}}};
    Frame previous[3]{};
    for(int step=0;step<65;++step) {
        Frame chain[3];std::memcpy(chain,initial,sizeof(chain));
        if(step) {
            const auto turn=firstperson::RotationMatrix(firstperson::AxisRotation({1,0,0},1.f));
            for(auto& f:chain) {f.origin=Add(initial[0].origin,Transform(turn,Sub(f.origin,initial[0].origin)));f.origin.z+=180;f.basis=turn;}
        }
        Check(history.Apply(item,step,step/90.,step>=1 && step<=40,true,reference,chain),"jump recovery accepts render sample");
        if(step) for(int j=0;j<3;++j) Check(firstperson::Length(Sub(chain[j].origin,previous[j].origin))<90,
            "landing reconciles shoulders gradually");
        for(int j=1;j<3;++j) Check(std::fabs(firstperson::Length(Sub(chain[j].origin,chain[j-1].origin))-
            firstperson::Length(Sub(initial[j].origin,initial[j-1].origin)))<.01f,"landing recovery preserves bone lengths");
        if(step==64) Check(!history.current.recovering && std::fabs(chain[0].origin.z-initial[0].origin.z-180)<.01f,
            "landing blend finishes and returns to standing pose");
        std::memcpy(previous,chain,sizeof(chain));
    }
}
}
