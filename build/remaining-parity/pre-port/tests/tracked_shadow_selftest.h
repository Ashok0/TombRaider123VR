// Shadow palettes keep native placement while arm/wrist transforms track VR.
namespace {
void __cdecl NativeTrackedShadowDraw(void* target,int32_t masked) {
    using namespace tr;
    Detour_GetJoints(static_cast<uint8_t*>(target));
    uint32_t visible=0,wrists=0;
    const auto* saved=FirstPersonShadowSkin(visible,wrists);
    Check(saved!=nullptr,"shadow draw retains a complete solved palette before native masking");
    if(!saved) return;
    const bool hd=*Ptr<int32_t>(dll.objects+off::object_geom+28)>0;
    Check(visible==((masked ? 0x3600u : 0x7fffu)|(hd ? 0x8000u : 0u)),
          "shadow visibility uses native attachment mask and keeps full head/body on unmasked draws");
    Check((wrists&0x2400u)==0x2400u,"both wrist joints are identified for rigid shadow skinning");
    Check(((wrists&0x8000u)!=0)==hd,"remapped wrist helpers participate in full-size shadow skinning");
    float before[384];std::memcpy(before,saved,sizeof(before));
    // Reproduce native DrawCreatureHD zeroing bones after the GetJoints hook.
    std::memset(Ptr<float>(dll.joints),0,8*12*sizeof(float));
    Check(!std::memcmp(before,saved,sizeof(before)),"native zero-bone masking cannot shrink the saved shadow skin");
    uint32_t eyeMask=0;
    Check(!FirstPersonBodySkin(eyeMask) && !FirstPersonNativeBodySkin(),
          "shadow skin remains separate from eye visibility and jiggle palettes");
}
void TestTrackedShadows() {
    using namespace tr;
    using namespace tr::motiongun;
    for(int game:{1,2,3}) for(bool armed:{false,true}) for(bool hd:{false,true}) {
        if(armed) ReadyArmedIK(game);else IKSetup(game);
        int mapping[16];float poses[16*12]{};
        const int count=hd ? 16 : 15;
        if(hd) {
            for(int i=0;i<16;++i) {
                mapping[i]=i==15 ? 10 : i;
                WriteRows({firstperson::IdentityBasis(),{}},poses+i*12);
                if(i<15) WriteRows(Multiply(ReadRows(ikNative+i*12),HdInverseBind(ReadRows(poses+i*12))),ikNative+i*12);
            }
            // Duplicate HD wrist helpers must cast the same tracked gun pose.
            std::memcpy(ikNative+15*12,ikNative+10*12,12*sizeof(float));
            auto* geom=Ptr<uint8_t>(dll.objects+off::object_geom);
            *reinterpret_cast<int32_t*>(geom+28)=16;
            *reinterpret_cast<int32_t**>(geom+48)=mapping;
            *reinterpret_cast<float**>(geom+72)=poses;
        }
        const auto originalScene=g_scenePose;
        float worldPalette[384];std::memcpy(worldPalette,ikNative,sizeof(worldPalette));
        uint8_t nativeItem[sizeof(item)];std::memcpy(nativeItem,item,sizeof(item));
        g_bodyVisualOffset={400,-300};g_bodyWallOffset={-80,40};
        *Ptr<int32_t>(dll.renderPass)=4;g_shadowDepthScope=true;
        // Native shadow palettes have world axes and a pass-specific origin.
        // Neither that origin nor the eye-only body fitting may move the wrist.
        for(const auto origin:{motiongun::Vec{},motiongun::Vec{3000,-400,500}}) {
            for(int i=0;i<count;++i) {
                auto frame=ReadRows(worldPalette+i*12);frame.origin=Sub(frame.origin,origin);
                WriteRows(frame,ikNative+i*12);
            }
            for(int sample=0;sample<4;++sample) {
                testControllerOffset[0]={-.12f,.03f*sample,.04f};
                testControllerOffset[1]={.10f+.03f*sample,.02f,.05f};
                const auto rotation=firstperson::RotationMatrix(firstperson::AxisRotation({0,0,1},sample*.7f));
                for(int row=0;row<3;++row) for(int col=0;col<3;++col)
                    testControllerPose[1].m[row][col]=rotation.r[row][col];
                Detour_GetJoints(item);
                const auto* palette=Ptr<float>(dll.joints);
                for(int hand=0;hand<2;++hand) {
                    Frame target{};Basis controller{};
                    Check(BuildControllerWrist(hand,target,controller),"tracked shadow has a controller target");
                    Frame wrist{};
                    Check(ReadIKJoint(Ptr<uint8_t>(dll.objects),palette,count,hd ? count : 0,
                          hd ? mapping : nullptr,hd ? poses : nullptr,hand ? 10 : 13,wrist),"shadow wrist bind recovery succeeds");
                    const auto delta=Sub(wrist.origin,Sub(target.origin,origin));
                    if(Dot(delta,delta)>=.01f || WristBasisError(wrist.basis,target.basis)>=.001f)
                        std::fprintf(stderr,"shadow mismatch game=%d armed=%d hd=%d sample=%d hand=%d origin=(%.1f %.1f %.1f) delta=(%.1f %.1f %.1f) ready=%d/%d gate=%d\n",
                            game,armed,hd,sample,hand,origin.x,origin.y,origin.z,delta.x,delta.y,delta.z,
                            MotionReady(),UnarmedIKReadyForPass(true),Gate());
                    Check(Dot(delta,delta)<.01f && WristBasisError(wrist.basis,target.basis)<.001f,
                          "shadow wrists/pistols follow controller position and rotation in native light space");
                }
                for(int joint:{0,1,2,3,4,5,6,7,14})
                    Check(!std::memcmp(palette+joint*12,ikNative+joint*12,12*sizeof(float)),
                          "tracked shadow retains native head, torso and legs without eye-view body offsets");
                if(hd) Check(!std::memcmp(palette+10*12,palette+15*12,12*sizeof(float)),"HD wrist helpers share tracked shadow pose");
                uint32_t mask=0;int wrist=-1;
                Check(!FirstPersonBodySkin(mask) && !FirstPersonHandSkin(wrist) && !FirstPersonNativeBodySkin(),
                      "shadow correction cannot expose eye visibility shaders or a jiggle sample");
                Check(!std::memcmp(nativeItem,item,sizeof(item)) && !std::memcmp(&originalScene,&g_scenePose,sizeof(g_scenePose)),
                      "shadow IK does not mutate simulation, visibility or first-person camera");
                float before[384];std::memcpy(before,palette,sizeof(before));Detour_GetJoints(item);
                Check(!std::memcmp(before,palette,sizeof(before)),"repeated shadow builds do not accumulate IK");
            }
        }
        std::memcpy(ikNative,worldPalette,sizeof(ikNative));
        g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&NativeTrackedShadowDraw);
        const auto oldBits=Field<uint32_t>(off::item_mesh_bits);
        Field<uint32_t>(off::item_mesh_bits)=0x3600u;
        for(int masked:{0,1}) {
            Detour_DrawCreatureHD(item,masked);
            uint32_t visible=0,wrists=0;
            Check(!FirstPersonShadowSkin(visible,wrists),"tracked shadow palette cannot leak beyond its draw scope");
        }
        Field<uint32_t>(off::item_mesh_bits)=oldBits;
        for(int unavailable=0;unavailable<10;++unavailable) {
            const auto status=*Ptr<int16_t>(dll.lara+2);
            const auto state=Field<int16_t>(18);
            const auto oldUnarmed=testConfig.firstPersonUnarmedIK;
            if(unavailable==0) g_active=false;
            if(unavailable==1) testConfigFlags=0;
            if(unavailable==2) testMissingHand=1;
            if(unavailable==3) g_scenePoseValid=false;
            if(unavailable==4) testConfig.firstPersonMotionGuns=false;
            if(unavailable==5) *Ptr<uint16_t>(dll.lara+60)=0x2000;
            if(unavailable==6) Field<int16_t>(18)=40;
            if(unavailable==7) testWater=1;
            if(unavailable==8) { if(armed) *Ptr<int16_t>(dll.lara+2)=2;else testConfig.firstPersonUnarmedIK=false; }
            if(unavailable==9) Field<int16_t>(18)=int16_t(game==1 ? 19 : 56);
            Detour_GetJoints(item);
            Check(!std::memcmp(Ptr<float>(dll.joints),ikNative,sizeof(ikNative)),
                  "third person, classic, missing tracking and native interactions preserve authored shadow arms");
            g_active=g_scenePoseValid=true;testConfigFlags=1;testMissingHand=-1;testConfig.firstPersonMotionGuns=true;
            *Ptr<uint16_t>(dll.lara+60)=0;Field<int16_t>(18)=state;testWater=0;
            *Ptr<int16_t>(dll.lara+2)=status;testConfig.firstPersonUnarmedIK=oldUnarmed;
        }
        uint8_t other[sizeof(item)];std::memcpy(other,item,sizeof(item));Detour_GetJoints(other);
        Check(!std::memcmp(Ptr<float>(dll.joints),ikNative,sizeof(ikNative)),"other shadow casters retain their native skeleton");
        *Ptr<int32_t>(dll.renderPass)=0;g_shadowDepthScope=false;testControllerOffset[0]=testControllerOffset[1]={};
        if(armed) SetMotionHooks(false);
    }
}
}
