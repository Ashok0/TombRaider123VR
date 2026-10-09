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
float WristBasisError(const tr::motiongun::Basis& a,const tr::motiongun::Basis& b) {
    float error=0;
    for(int row=0;row<3;++row) for(int col=0;col<3;++col) {
        const float delta=std::fabs(a.r[row][col]-b.r[row][col]);
        if(!std::isfinite(delta)) return 1000;
        error=std::max(error,delta);
    }
    return error;
}
void TestWristTwistRecovery() {
    using namespace tr::firstperson;
    constexpr float rad=3.14159265358979323846f/180;
    // Exercise the actual constraint used by SolveArm: a controller can return
    // to exactly the same pose after winding around the decomposition boundary.
    // Its palm and forearm must recover without a first-person toggle.
    for(float yaw:{0.f,73.f,-141.f}) for(float bend:{0.f,30.f,80.f})
    for(float start:{-179.f,-150.f,-120.f,-60.f,0.f,60.f,120.f,150.f,179.f}) for(int direction:{-1,1}) {
        const auto reference=RotationMatrix(AxisRotation({0,1,0},yaw*rad));
        const auto axis=tr::motiongun::Transform(reference,{0,0,1});
        const auto swing=AxisRotation(tr::motiongun::Transform(reference,{1,0,0}),bend*rad);
        auto target=[&](float angle) {
            return RotationMatrix(RotationProduct(RotationProduct(swing,
                AxisRotation(axis,angle*rad)),RotationOf(reference)));
        };
        ArmTwistState state{};
        Basis wrist{},forearm{},initial{},initialForearm{};
        ConstrainWrist(reference,target(start),axis,state,initial,initialForearm);
        for(int loop=0;loop<4;++loop) {
            Basis previous=initial,previousForearm=initialForearm;
            for(int step=1;step<=360;++step) {
                ConstrainWrist(reference,target(start+direction*(loop*360+step)),axis,state,wrist,forearm);
                Check(WristBasisError(wrist,previous)<.036f &&
                      WristBasisError(forearm,previousForearm)<.036f,
                      "wrist and forearm remain continuous across wrap and limit recovery boundaries");
                previous=wrist;previousForearm=forearm;
            }
            Check(WristBasisError(wrist,initial)<.0001f &&
                  WristBasisError(forearm,initialForearm)<.0001f,
                  "palm and forearm recover after a complete controller rotation without toggling first person");
            for(int draw=0;draw<20;++draw) {
                ConstrainWrist(reference,target(start),axis,state,wrist,forearm);
                Check(WristBasisError(wrist,initial)<.0001f &&
                      WristBasisError(forearm,initialForearm)<.0001f,
                      "holding the recovered pose cannot leave the wrist stuck or accumulate per-eye drift");
            }
        }
    }
    // Keep the established response throughout the normal range and adjacent
    // anatomical-limit plateau; only the far wrap seam is softened.
    for(int angle=-135;angle<=135;++angle) {
        const auto reference=IdentityBasis();
        const tr::motiongun::Vec axis{0,0,1};
        ArmTwistState state{};Basis wrist{},forearm{};
        ConstrainWrist(reference,RotationMatrix(AxisRotation(axis,angle*rad)),axis,state,wrist,forearm);
        const float limited=float(std::clamp(angle,-90,90))*rad;
        Check(WristBasisError(wrist,RotationMatrix(AxisRotation(axis,limited)))<.0001f &&
              WristBasisError(forearm,RotationMatrix(AxisRotation(axis,limited*.8f)))<.0001f,
              "normal wrist angles, anatomical limits and forearm roll sharing retain their response");
    }
    for(int direction:{-1,1}) {
        const auto reference=IdentityBasis();
        const tr::motiongun::Vec axis{0,0,1};
        ArmTwistState state{};
        Basis wrist{},forearm{},previous{};
        ConstrainWrist(reference,RotationMatrix(AxisRotation(axis,direction*179.8f*rad)),
                       axis,state,previous,forearm);
        for(int sample=0;sample<100;++sample) {
            const float angle=direction*(sample%2 ? 179.8f : 180.2f)*rad;
            const auto target=RotationMatrix(AxisRotation(axis,angle));
            ConstrainWrist(reference,target,axis,state,wrist,forearm);
            Check(WristBasisError(wrist,previous)<.015f,"wrap-boundary tracking jitter does not flip the palm");
            previous=wrist;
        }
        // At a 180-degree swing the twist is undefined. Once a valid pose
        // returns, it must replace this history, including abrupt pose jumps.
        for(float swing:{179.9999f,180.f,180.0001f}) {
            const auto singular=RotationMatrix(RotationProduct(AxisRotation({1,0,0},swing*rad),
                AxisRotation(axis,120*rad)));
            ConstrainWrist(reference,singular,axis,state,wrist,forearm);
            Check(WristBasisError(wrist,wrist)==0 && WristBasisError(forearm,forearm)==0,
                  "singular wrist swing stays finite");
        }
        for(float angle:{-70.f,0.f,70.f}) {
            const auto target=RotationMatrix(AxisRotation(axis,angle*rad));
            ConstrainWrist(reference,target,axis,state,wrist,forearm);
            Check(WristBasisError(wrist,target)<.0001f,
                  "valid reachable wrist pose immediately recovers after a singularity or pose discontinuity");
        }
    }
    // Exercise the complete two-bone solver for both bend directions, with a
    // native animation roll. Verify recovery of the visible forearm and hand,
    // exact grip position, and unchanged weapon orientation bypass.
    for(int side:{-1,1}) {
        const auto identity=IdentityBasis();
        Frame native[3]={{identity,{0,0,0}},{identity,{0,0,100}},
                         {RotationMatrix(AxisRotation({0,0,1},73*rad)),{0,0,200}}};
        const tr::motiongun::Vec axis{-side*.6f,0,.8f};
        const auto reference=Multiply(Align({0,1,0},axis),identity);
        ArmTwistState state{};
        Frame initial[3]{},correction[3]{};
        Frame target{reference,{0,0,160}};
        Check(SolveArm(native,target,{float(side),0,0},initial,&state,&identity),"two-bone wrist recovery fixture solves");
        for(int step=1;step<=1080;++step) {
            target.basis=Multiply(RotationMatrix(AxisRotation(axis,side*step*rad)),reference);
            Check(SolveArm(native,target,{float(side),0,0},correction,&state,&identity),
                  "two-bone solver accepts repeated controller loops");
        }
        for(int joint=1;joint<3;++joint) {
            const auto actual=Multiply(correction[joint],native[joint]);
            const auto expected=Multiply(initial[joint],native[joint]);
            Check(WristBasisError(actual.basis,expected.basis)<.0001f,
                  "complete IK forearm and wrist recover despite native animation roll");
            const auto offset=Sub(actual.origin,expected.origin);
            Check(Dot(offset,offset)<.0001f,"wrist recovery keeps elbow and grip positions fixed");
        }
        target.basis=Multiply(RotationMatrix(AxisRotation(axis,155*rad)),reference);
        Check(SolveArm(native,target,{float(side),0,0},correction,&state,&identity,false) &&
              WristBasisError(Multiply(correction[2],native[2]).basis,target.basis)<.0001f,
              "unconstrained armed wrist preserves the exact tracked orientation");
    }
}
void TestMountCameraHandoff() {
    using namespace tr;
    for(int game:{1,2,3}) for(int mount:{19,54}) for(int rise:{256,513,769}) {
        IKSetup(game);Field<int16_t>(18)=2;Field<int16_t>(24)=11;
        g_headingLevel=0;g_previousBody={};g_dragPrevious=g_dragCurrent=g_dragShown={};
        g_groundEye={};g_groundEye.Apply({0,0,0},0,{0,-718,100},0);
        g_lastClimbCameraState=mount;
        auto& current=Field<PHD_3DPOS>(off::item_pos);
        auto& previous=Field<PHD_3DPOS>(off::item_pos_prev);
        current={0,1000-rise,0};previous={0,1000,0};
        const auto fit=g_groundEye.local;
        g_mountRootHeight.Reset();g_blockCamera.Reset();
        testViewRight=testViewForward=testViewRise=0;
        // All these rendered frames belong to one simulation tick. The head
        // has already finished the climb while pos_prev still precedes it.
        for(int fraction:{0,0,16,32,64,128,192,255,256}) {
            *Ptr<int32_t>(dll.frameFrac)=fraction;
            PHD_3DPOS eye{0,current.y_pos-700,100};
            UpdateLocomotion(eye);
            Check(eye.y_pos==current.y_pos-718,
                  "mount-completion height holds across every render of the same native root pair");
            Check(g_groundEye.local.y==fit.y,"mount recovery cannot change standing height calibration");
            // Run the real eye sweep with an independent native-query stub.
            // Both camera anchoring and room collision must agree on root Y.
            const uint64_t savedBase=g_boundBase;
            const auto savedDll=dll;
            g_boundBase=reinterpret_cast<uint64_t>(&NativeEyeCollision);
            Check(savedBase>g_boundBase && savedBase-g_boundBase<UINT32_MAX,
                  "mount collision callback fits synthetic module");
            dll.lara+=uint32_t(savedBase-g_boundBase);
            dll.frameFrac+=uint32_t(savedBase-g_boundBase);
            dll.getCollisionInfo=0;
            eyeSamples=game==3 ? 6 : 4;blockedEyeSample=-1;eyeObstacle=0;
            eyeQueryY=INT32_MIN;
            ClampRenderedHeadToCollision(item,eye);
            Check(eyeQueryY==current.y_pos,"post-mount clearance samples the same root height as the camera");
            g_boundBase=savedBase;dll=savedDll;
        }
        Check(g_mountRootHeight.active,"mount height stays latched while the native pair remains unchanged");
        // On the next native root pair, ordinary movement resumes interpolation.
        previous=current;current.y_pos+=32;
        for(int fraction:{0,64,128,256}) {
            *Ptr<int32_t>(dll.frameFrac)=fraction;
            PHD_3DPOS eye{0,Lerp(previous.y_pos,current.y_pos,fraction)-700,100};
            UpdateLocomotion(eye);
            Check(eye.y_pos==Lerp(previous.y_pos,current.y_pos,fraction)-718,
                  "mount handoff releases when the native root pair advances");
        }
    }
    for(int state:{3,19,54,7}) {
        IKSetup(1);g_headingLevel=0;g_previousBody={};
        g_dragPrevious=g_dragCurrent=g_dragShown={};g_lastClimbCameraState=2;
        g_mountRootHeight.Begin(0,0);Field<int16_t>(18)=int16_t(state);
        Field<int16_t>(24)=int16_t(state==7 ? 13 : 11);
        PHD_3DPOS eye{0,-700,100};UpdateLocomotion(eye);
        Check(!g_mountRootHeight.active,"leaving grounded locomotion clears the mount-height override");
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
