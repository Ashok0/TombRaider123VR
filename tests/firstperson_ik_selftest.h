// Production hooks, not a parallel model of the IK implementation.
namespace {
float ikNative[32*12]{};
void __cdecl NativeIKJoints(uint8_t*) {
    std::memcpy(tr::Ptr<float>(dll.joints),ikNative,sizeof(ikNative));
}
void IKSetup(int game=1) {
    using namespace tr;
    ArmSetup(game,1);
    dll.objects=2048; dll.joints=6500; dll.renderPass=200; dll.frameFrac=204;
    dll.laraHands=4500;
    testConfig.firstPersonMotionGuns=true; testConfigFlags=1;
    g_scenePoseValid=true; g_scenePose={0,-650,80}; g_bodyVisualOffset={};
    g_unarmedTwist[0]={};g_unarmedTwist[1]={};
    g_bodySkinScope=true;
    g_hGetJoints.m_trampoline=reinterpret_cast<void*>(&NativeIKJoints);
    auto* bind=Ptr<float>(dll.objects+192);
    std::fill(ikNative,ikNative+384,0.f);
    for (int i=0;i<15;++i) {
        bind[i*16]=bind[i*16+5]=bind[i*16+10]=bind[i*16+15]=1;
        auto frame=motiongun::Frame{firstperson::IdentityBasis(),{float(i*10),-700,0}};
        if (i>=8 && i<=13) {
            const int segment=(i-8)%3; const float side=i<11 ? 1.f : -1.f;
            frame.origin={side*(segment ? 300.f : 180.f),-550.f+segment*170.f,0};
        }
        motiongun::WriteRows(frame,ikNative+i*12);
        for (uint32_t offset:{off::item_joints_cur,off::item_joints_prev}) {
            auto* native=reinterpret_cast<int32_t*>(item+offset+i*48);
            for (int j=0;j<12;++j) native[j]=int32_t(ikNative[i*12+j]*16384);
        }
    }
}
void TestFirstPersonParity() {
    using namespace tr;
    for (int game:{1,2,3}) {
        for (int state:{0,1,2,3,7,9,15,16,21,22,25,26,27}) {
            IKSetup(game); Field<int16_t>(18)=int16_t(state);
            Check(UnarmedIKReady(),"ground/jump/fall unarmed controller IK available in all games");
            Check(!HideUnarmedArms(item),"tracked arms do not blink with head-pitch visibility");
            uint8_t savedItem[sizeof(item)]; std::memcpy(savedItem,item,sizeof(item));
            Detour_GetJoints(item);
            uint32_t mask=0;const auto* render=FirstPersonBodySkin(mask);
            const auto* physics=FirstPersonNativeBodySkin();
            Check(render && physics && render!=physics,"separate render and physics palettes are scoped to Lara");
            Check(!std::memcmp(physics,ikNative,15*12*sizeof(float)),"physics sees complete native animation");
            Check(!std::memcmp(item,savedItem,sizeof(item)),"IK never edits simulation skeleton, position, animation or masks");
            Check(!std::memcmp(render,physics,8*12*sizeof(float)),"arm IK preserves torso and legs exactly");
            for (int hand=0;hand<2;++hand) {
                motiongun::Frame target{};motiongun::Basis controller{};
                Check(BuildControllerWrist(hand,target,controller),"shared controller wrist target valid");
                const auto wrist=motiongun::ReadRows(render+(hand ? 10 : 13)*12);
                const auto error=motiongun::Sub(wrist.origin,target.origin);
                Check(motiongun::Dot(error,error)<.01f,"IK wrist reaches the same calibrated target as gun tracking");
            }
            Check(std::memcmp(render+8*12,physics+8*12,6*12*sizeof(float))!=0,"controller motion deforms arm chains");
            g_bodySkinScope=false;
            Check(!FirstPersonBodySkin(mask) && !FirstPersonNativeBodySkin(),"palette overrides cannot escape the draw scope");
        }
        for (int state:{10,19,30,31,36,37,38,40,41,45,54,55,56,57}) {
            IKSetup(game);Field<int16_t>(18)=int16_t(state);
            Check(!UnarmedIKReady(),"grips, pickups, blocks, switches and rolls keep authored hands");
        }
        for (int missing=0;missing<6;++missing) {
            IKSetup(game);
            if (missing==0) testPoseAvailable=false;
            if (missing==1) testConfigFlags=0;
            if (missing==2) testConfig.firstPersonUnarmedIK=false;
            if (missing==3) *Ptr<int16_t>(dll.lara+2)=4;
            if (missing==4) *Ptr<uint16_t>(dll.lara+60)=0x2000;
            if (missing==5) *Ptr<int32_t>(dll.renderPass)=4;
            Check(!UnarmedIKReady(),"IK safely yields for unavailable tracking, weapons, interactions and shadows");
        }
        // HD outfits remap palette slots and can duplicate an arm segment.
        IKSetup(game);
        int mapping[16];float poses[16*12]{};
        for (int i=0;i<16;++i) {
            mapping[i]=i==15 ? 9 : i;
            auto inverse=motiongun::Frame{firstperson::IdentityBasis(),{}};
            motiongun::WriteRows(inverse,poses+i*12);
            auto joint=motiongun::ReadRows(ikNative+mapping[i]*12);
            if (i==15) { motiongun::Frame bind{};motiongun::Inverse(motiongun::HdInverseBind(inverse),bind);joint=motiongun::Multiply(joint,bind); }
            motiongun::WriteRows(motiongun::Multiply(joint,motiongun::HdInverseBind(inverse)),ikNative+i*12);
        }
        auto* hdGeom=Ptr<uint8_t>(dll.objects+88);
        *reinterpret_cast<int*>(hdGeom+28)=16;
        *reinterpret_cast<int**>(hdGeom+48)=mapping;
        *reinterpret_cast<float**>(hdGeom+72)=poses;
        Detour_GetJoints(item);uint32_t hdMask=0;
        const auto* hd=FirstPersonBodySkin(hdMask);
        float duplicateError=0;
        if (hd) for(int i=0;i<12;++i) duplicateError+=std::fabs(hd[9*12+i]-hd[15*12+i]);
        Check(hd && duplicateError<.001f,
              "duplicated HD arm helper bones receive the same IK correction");
        for (int hand=0;hand<2;++hand) {
            motiongun::Frame joint{},target{};motiongun::Basis controller{};
            Check(ReadIKJoint(Ptr<uint8_t>(dll.objects),hd,16,16,mapping,poses,hand ? 10 : 13,joint) &&
                  BuildControllerWrist(hand,target,controller),"HD inverse-bind recovery remains valid after IK");
            const auto e=motiongun::Sub(joint.origin,target.origin);
            Check(motiongun::Dot(e,e)<.01f,"HD and legacy palettes reach identical controller targets");
        }
        IKSetup(game);*Ptr<int32_t>(dll.renderPass)=4;
        g_bodyVisualOffset={100,200};Detour_GetJoints(item);
        Check(!std::memcmp(Ptr<float>(dll.joints),ikNative,sizeof(ikNative)) && !FirstPersonNativeBodySkin(),
              "shadow skeleton bypasses both body fit and controller IK");
        // A degenerate second arm cannot commit a partially solved first arm.
        IKSetup(game);std::memcpy(ikNative+9*12,ikNative+8*12,12*sizeof(float));
        Detour_GetJoints(item);uint32_t mask=0;
        Check(!std::memcmp(FirstPersonBodySkin(mask),FirstPersonNativeBodySkin(),15*12*sizeof(float)),
              "failed IK solve leaves both arms native");
        // Full descriptor substitution handles shared mesh pointers without
        // changing a different hand/material selection or any later draw.
        for (int restIndex:{1,6}) {
            IKSetup(game);Field<uint32_t>(12)=0x600;
            auto* rest=Ptr<uint8_t>(dll.laraHands)+restIndex*104;
            auto* run=rest+104;auto* geom=Ptr<uint8_t>(dll.objects+88);
            std::memset(rest,1,104);std::memset(run,2,104);
            *reinterpret_cast<void**>(rest+16)=*reinterpret_cast<void**>(run+16)=reinterpret_cast<void*>(0x1234);
            std::memcpy(geom,run,104);
            { UnarmedRestHandScope scope(item,1);Check(!std::memcmp(geom,rest,104),"running uses relaxed bare/gloved hand descriptor"); }
            Check(!std::memcmp(geom,run,104),"running hand descriptor restored after eye draw");
            geom[0]=3;
            { UnarmedRestHandScope scope(item,1);Check(geom[0]==3,"mesh-pointer alias cannot replace unrelated hand geometry"); }
        }
    }
    // Native alignment owns input and yaw while Lara still reports a gait.
    for (int state:{0,1,2,3,15,16,21,22}) {
        Reset(state,{1,0},true);*Ptr<uint16_t>(dll.lara+60)=0x2000;
        float x=.6f,y=-.4f,r=.8f;FirstPersonInput(x,y,r,false,false);
        Check(x==.6f && y==-.4f && r==.8f && !g_haveManualInput && !CanWalk(item),
              "interaction alignment preserves raw controls and suppresses body following");
        *Ptr<uint32_t>(dll.input)=0x40;Field<PHD_3DPOS>(88).y_rot=1234;
        Detour_LaraAboveWater(item,nullptr);
        Check(*Ptr<uint32_t>(dll.input)==0x40 && Field<PHD_3DPOS>(88).y_rot==1234,
              "unarmed Action and native switch alignment survive the simulation hook");
    }
    for (int state:{40,41}) {
        Reset(state,{},true);Field<int16_t>(20)=2;
        Check(!Gate(),"switch stays third person until current animation finishes, even with idle goal");
        Field<int16_t>(18)=2;Check(Gate(),"first person resumes after switch animation completes");
    }
    Reset(0,{},true);dll.module=L"tomb3.dll";*Ptr<uint16_t>(dll.lara+60)=4;
    Check(!Gate(),"TR3 special switch extra-animation bank also uses third-person camera");
    *Ptr<uint16_t>(dll.lara+60)=0;Check(Gate(),"extra-animation exit restores first person");
    for (const auto clip : {std::pair<int,int>{7,13},{2,24},{2,31},{2,82},{1,92},{2,99}}) {
        Reset(clip.first,{},true);Field<int16_t>(24)=int16_t(clip.second);
        dll.frameFrac=204;g_headingLevel=0;g_previousBody={};g_dragPrevious=g_dragCurrent=g_dragShown={};
        g_groundEye={};g_groundEye.Apply({0,0,0},0,{0,-700,70},0);
        const auto before=g_groundEye;g_bodyVisualOffset={100,100};
        PHD_3DPOS eye{30,-480,150}; const auto native=eye;
        UpdateLocomotion(eye);FitBodyToRenderedEye(item,eye,native);
        Check(eye.x_pos==native.x_pos && eye.y_pos==native.y_pos && eye.z_pos==native.z_pos,
              "landing eye follows native head on all axes instead of remaining inside torso");
        Check(!g_bodyVisualOffset.x && !g_bodyVisualOffset.z &&
              !std::memcmp(&before,&g_groundEye,sizeof(before)),"landing does not contaminate standing eye/body calibration");
    }
    Check(!locomotion::IsLandingAnimation(58,99) && !locomotion::IsLandingAnimation(2,11),
          "TR1 water exit and ordinary idle are not landing poses");
    // Repeated mount -> grounded -> free-jump transitions must not leave a
    // zero persistent mask suppressing the unmasked HD body on recovery.
    for (int state:{54,2,3,2,54,2}) {
        ArmSetup(1,state);Field<uint32_t>(12)=0;g_headHidden=true;
        g_ledgeArmsOnly=locomotion::IsLedgeArmsOnlyState(state);
        Detour_DrawCreatureHD(item,0);
        Check(armDrawPalette && (armDrawBits&0x3f00u)==0x3f00u &&
            ((armDrawBits&(1u<<7))!=0)==!g_ledgeArmsOnly,
            "mount recovery restores body and arms despite stale persistent visibility");
    }
}
} // namespace
