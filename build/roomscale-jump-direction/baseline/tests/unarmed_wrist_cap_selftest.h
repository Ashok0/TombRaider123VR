namespace {
int unarmedCapDraws=0;
bool expectUnarmedCap=true;
void __cdecl ObserveUnarmedCap(void* target,int32_t masked) {
    using namespace tr;
    Detour_GetJoints(static_cast<uint8_t*>(target));
    int pivot=-1;const float* palette=FirstPersonHandSkin(pivot);
    float centre[3]{};
    if(!expectUnarmedCap) {
        Check(!palette && !FirstPersonWristCentre(centre),"attached arms and native shadows cannot inherit floating wrist caps");
        return;
    }
    Check(masked==1 && g_unarmedHandScope && g_renderArm>=0 && g_renderArm<2,
        "unarmed floating draw enters a separate masked hand scope");
    Check(palette && FirstPersonWristCentre(centre),"unarmed floating hand exposes both skin and centre to the existing cap renderer");
    const int nativeJoint=g_renderArm ? 10 : 13;
    Check(Field<uint32_t>(off::item_mesh_bits)==(1u<<nativeJoint),"each cap pass draws only its own hand");
    uint32_t bodyMask=0;
    Check(!FirstPersonBodySkin(bodyMask) && !FirstPersonNativeBodySkin(),"floating hand selects rigid hand shader instead of body shader");
    const auto* obj=Ptr<uint8_t>(dll.objects);
    const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int*>(geom+28);
    const auto* mapping=bones>0 ? *reinterpret_cast<const int* const*>(geom+48) : nullptr;
    const auto* poses=bones>0 ? *reinterpret_cast<const float* const*>(geom+72) : nullptr;
    Check((mapping ? mapping[pivot] : pivot)==nativeJoint,"cap pivot follows current costume wrist mapping");
    motiongun::Frame wrist{},targetWrist{};motiongun::Basis controller{};
    Check(ReadIKJoint(obj,palette,bones>0?bones:15,bones,mapping,poses,nativeJoint,wrist),"sealed hand palette retains a readable wrist");
    Check(BuildControllerWrist(g_renderArm,targetWrist,controller),"unarmed tracked wrist target exists");
    const auto* w=Ptr<int32_t>(dll.w2vMatrix);
    targetWrist.origin=motiongun::Sub(targetWrist.origin,{float(w[3]),float(w[7]),float(w[11])});
    Check(firstperson::Length(motiongun::Sub(wrist.origin,targetWrist.origin))<.01f &&
        WristBasisError(wrist.basis,targetWrist.basis)<.0001f,"sealed unarmed wrist follows controller position and rotation");
    Check(firstperson::Length(motiongun::Sub({centre[0],centre[1],centre[2]},targetWrist.origin))<.01f,
        "cap centre and hand skin use the same transformed wrist");
    float saved[384]{};std::memcpy(saved,palette,sizeof(saved));
    std::memset(Ptr<float>(dll.joints),0,sizeof(saved)); // Native masking after GetJoints.
    Check(!std::memcmp(saved,palette,sizeof(saved)),"native hidden-bone zeroing cannot destroy unarmed sealing palette");
    GunPose gun{};Check(!BuildGunPose(g_renderArm,gun),"empty-hand rendering does not enable weapon pose or firing");
    ++unarmedCapDraws;
}
void TestUnarmedWristCaps() {
    using namespace tr;
    for(int game:{1,2,3}) for(bool hd:{false,true}) for(int state:{2,25,71,80,81,84,85,86,89,90}) {
        if(game!=3 && state>=71) continue;
        BodyRig(game,false);testConfig.firstPersonFullBodyIK=true;
        Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(state);
        if(state==2) { testConfig.firstPersonPhysicalCrouch=true;testViewRise=-.6f;FirstPersonPhysicalDuck(); }
        if(game==3 && state==2) continue; // Native crouch hides once the native state changes.
        int mapping[16]{};float poses[16*12]{};
        if(hd) {
            for(int i=0;i<16;++i) { mapping[i]=(i*7)%15;motiongun::WriteRows({firstperson::IdentityBasis(),{}},poses+i*12); }
            auto* geom=Ptr<uint8_t>(dll.objects+off::object_geom);
            *reinterpret_cast<int*>(geom+28)=16;*reinterpret_cast<int**>(geom+48)=mapping;
            *reinterpret_cast<float**>(geom+72)=poses;
        }
        Ptr<int32_t>(dll.w2vMatrix)[3]=73;Ptr<int32_t>(dll.w2vMatrix)[7]=-29;Ptr<int32_t>(dll.w2vMatrix)[11]=41;
        testControllerPose[0].m[0][0]=0;testControllerPose[0].m[0][2]=1;
        testControllerPose[0].m[2][0]=-1;testControllerPose[0].m[2][2]=0;
        g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&ObserveUnarmedCap);
        expectUnarmedCap=true;
        for(int eye:{0,1}) for(uint32_t bits:{0x7fffu,0x3600u,0x2000u,0x400u}) {
            (void)eye;const bool body=bits==0x7fff;
            Field<uint32_t>(off::item_mesh_bits)=bits;
            uint8_t original[sizeof(item)]{};std::memcpy(original,item,sizeof(item));
            unarmedCapDraws=0;Detour_DrawCreatureHD(item,body?0:1);
            Check(unarmedCapDraws==((bits==0x2000 || bits==0x400)?1:2),"both eye passes seal exactly the hands present in the native draw");
            Check(!std::memcmp(original,item,sizeof(item)),"floating wrist sealing leaves native item state unchanged");
            int pivot=-1;float centre[3]{};
            Check(!g_unarmedHandScope && g_renderArm<0 && !FirstPersonHandSkin(pivot) && !FirstPersonWristCentre(centre),
                "hand palette and wrist cap end with the unarmed draw scope");
        }
        expectUnarmedCap=false;*Ptr<int32_t>(dll.renderPass)=4;Detour_DrawCreatureHD(item,1);
    }
    for(int game:{1,2,3}) {
        BodyRig(game,false);expectUnarmedCap=false;
        g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&ObserveUnarmedCap);
        Detour_DrawCreatureHD(item,0); // Ordinary standing arms stay attached.
    }
    Reset(2,{},true);SetMotionHooks(false);testViewRise=0;
}
}
