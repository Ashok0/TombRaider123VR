// October 11 TR4/5 fixes exercised through TR1-3's production palette hooks.
namespace {
void ReadyArmedIK(int game) {
    IKSetup(game);SetMotionHooks(true);
    dll.nextItemActive=210;dll.getSpheres=dll.findTargetPoint=dll.los=1;
    *tr::Ptr<int16_t>(dll.lara+2)=4;
    tr::testConfig.firstPersonUnarmedIK=false;
    Check(tr::MotionReady(),"armed IK is available independently of the unarmed option");
}
int parityBodyDraws=0,parityForearmDraws=0,parityHandDraws=0;
bool parityNativeModern=false;
void __cdecl ObserveProximityDraw(uint8_t*) {
    Check(bool(tr::testConfigFlags&2)==parityNativeModern,"proximity gate follows eye versus native shadow scope");
    tr::testConfigFlags|=0x80;
}
void __cdecl ObserveParityDraw(void* target,int32_t masked) {
    using namespace tr;
    Check(masked==1,"first-person draws honor scoped visibility");
    Detour_GetJoints(static_cast<uint8_t*>(target));
    uint32_t skin=0;
    const auto bits=Field<uint32_t>(off::item_mesh_bits);
    if(g_renderArm<0) {
        Check(FirstPersonBodySkin(skin)!=nullptr,"body and forearms use articulated palette");
        Check(FirstPersonNativeBodySkin()!=nullptr,"body and forearms retain jiggle source");
        if(bits==0x1200) ++parityForearmDraws;
        else { ++parityBodyDraws;Check((bits&0x6480u)==0x80u,"body retains torso but excludes head and separate hands"); }
    } else {
        ++parityHandDraws;
        Check(!FirstPersonBodySkin(skin) && g_renderWrist>=0,"gun pass retains rigid calibrated wrist palette");
    }
}
void TestCollectionIKParity() {
    using namespace tr;
    using namespace tr::motiongun;
    using namespace tr::firstperson;
    constexpr float rad=3.14159265358979323846f/180;
    // An analytic chain with a known rest relationship tests forearm roll,
    // including rotations beyond the previous clamp and native recoil roll.
    for(int side:{-1,1}) for(float nativeRoll:{-130.f,0.f,95.f}) {
        Frame bind[3]={{IdentityBasis(),{0,0,0}},{IdentityBasis(),{0,0,100}},
                       {RotationMatrix(AxisRotation({0,0,1},.4f)),{0,0,200}}};
        Frame arm[3]={bind[0],bind[1],bind[2]};
        arm[1].basis=RotationMatrix(AxisRotation({0,0,1},nativeRoll*rad));
        Frame previous{};
        for(int angle=-540;angle<=540;++angle) {
            Frame target{Multiply(RotationMatrix(AxisRotation({0,0,1},side*angle*rad)),bind[2].basis),bind[2].origin};
            Frame correction[3]{};
            Check(SolveTrackedArm(arm,target,{float(side),0,0},bind,correction),"tracked solver accepts unrestricted wrist roll");
            const auto lower=Multiply(correction[1],arm[1]);
            const auto expected=RotationMatrix(AxisRotation({0,0,1},side*angle*rad));
            Check(WristBasisError(lower.basis,expected)<.001f,"forearm follows full wrist roll through rest relationship");
            Check(WristBasisError(Multiply(correction[2],arm[2]).basis,target.basis)<.001f,"wrist remains exact");
            Check(Dot(Sub(lower.origin,arm[1].origin),Sub(lower.origin,arm[1].origin))<.001f,"twist cannot move elbow");
            if(angle>-540) Check(WristBasisError(lower.basis,previous.basis)<.02f,"forearm has no discontinuity at roll seam");
            previous=lower;
        }
    }
    for(int game:{1,2,3}) {
        IKSetup(game);g_heading.base=.3f;testHeadYaw=.6f;
        g_groundEye.Reset();g_groundEye.Apply({0,0,0},.3f,{25,-700,90},.9f);
        const auto local=g_groundEye.local;
        const auto worldAnchor=locomotion::Rotate({local.x,local.z},g_heading.base);
        testHeadYaw=-.4f;testAllowRecenter=true;
        const int oldRecenter=testRecenterCalls;
        FirstPersonTrackingOriginChanged(1.f);
        testAllowRecenter=false;
        const auto afterAnchor=locomotion::Rotate({g_groundEye.local.x,g_groundEye.local.z},g_heading.base);
        Check(std::fabs(g_heading.World(testHeadYaw)-.9f)<.001f &&
              locomotion::Length(afterAnchor-worldAnchor)<.001f && g_groundEye.local.y==local.y,
              "System recenter preserves world heading and standing body anchor");
        Check(testRecenterCalls==oldRecenter+1,"origin callback captures shared neutral exactly once");
        testHeadYaw=0;
        for(int state:{2,12,56,80}) {
            IKSetup(game);Field<int16_t>(off::item_anim_state)=int16_t(state);
            testConfigFlags=3;testPoseAvailable=false;
            g_hCrouchDraw.m_trampoline=reinterpret_cast<void*>(&ObserveProximityDraw);
            parityNativeModern=false;Detour_CrouchDraw(item);
            Check(testConfigFlags==0x83,"proximity bypass retains modern controls after render even without tracking");
            *Ptr<int32_t>(dll.renderPass)=4;parityNativeModern=true;Detour_CrouchDraw(item);
        }
        IKSetup(game);Field<int16_t>(off::item_anim_state)=12;
        g_groundEye.Reset();g_groundEye.Apply({0,0,0},0,{0,-700,140},0);
        g_previousBody={};g_dragPrevious=g_dragCurrent=g_dragShown={};g_mountRootHeight.Reset();
        PHD_3DPOS impact{25,-540,80};UpdateLocomotion(impact);
        Check(impact.x_pos==0 && impact.z_pos==140 && impact.y_pos==-540 && g_groundEye.local.y==-700,
              "wall impact retains horizontal anchor and follows native neck height without recalibration");
        for(int armed:{0,1}) for(int angle:{-179,-120,-60,0,60,120,179}) {
            if(armed) ReadyArmedIK(game);else IKSetup(game);
            g_bodyVisualOffset={35,-20};g_bodyWallOffset={-70,-80};
            for(auto& pose:testControllerPose) {
                const auto basis=RotationMatrix(AxisRotation({0,0,1},angle*rad));
                for(int r=0;r<3;++r) for(int c=0;c<3;++c) pose.m[r][c]=basis.r[r][c];
            }
            g_bodySkinScope=true;Detour_GetJoints(item);
            uint32_t mask=0;const auto* render=FirstPersonBodySkin(mask);
            const auto* physics=FirstPersonNativeBodySkin();
            Check(render && physics,"armed and holstered palettes available during wall retraction");
            Check(std::fabs(physics[7*12+3]-(ikNative[7*12+3]-35))<.001f &&
                  std::fabs(physics[7*12+11]-(ikNative[7*12+11]-100))<.001f,
                  "jiggle torso follows rendered body wall retreat");
            for(int hand=0;hand<2;++hand) {
                Frame target{};Basis controller{};
                Check(BuildControllerWrist(hand,target,controller),"calibrated target available");
                const auto wrist=ReadRows(render+(hand ? 10 : 13)*12);
                const auto error=Sub(wrist.origin,target.origin);
                Check(Dot(error,error)<.01f && WristBasisError(wrist.basis,target.basis)<.001f,
                      "body wall retreat never shifts tracked wrist or clamps orientation");
                if(armed) {
                    const auto saved=wrist;
                    g_renderArm=hand;Detour_GetJoints(item);g_renderArm=-1;
                    const auto rigid=ReadRows(g_handSkinPalette+(hand ? 10 : 13)*12);
                    Check(Dot(Sub(saved.origin,rigid.origin),Sub(saved.origin,rigid.origin))<.01f &&
                          WristBasisError(saved.basis,rigid.basis)<.001f,"IK and rigid gun pass share exact wrist and pivot");
                }
            }
        }
        ReadyArmedIK(game);*Ptr<int16_t>(dll.lara+4)=game==3 ? 2 : 4;
        testMissingHand=0;
        Check(MotionReady(),"single gun requires only right controller");
        Detour_GetJoints(item);uint32_t singleMask=0;
        const auto* single=FirstPersonBodySkin(singleMask);
        Frame singleTarget{};Basis singleController{};
        Check(BuildControllerWrist(1,singleTarget,singleController),"right wrist target remains tracked");
        const auto singleError=Sub(ReadRows(single+10*12).origin,singleTarget.origin);
        Check(Dot(singleError,singleError)<.01f && !std::memcmp(single+11*12,ikNative+11*12,3*12*sizeof(float)),
              "missing left tracking preserves authored left arm and connected right arm");
        ReadyArmedIK(game);
        g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&ObserveParityDraw);
        parityBodyDraws=parityForearmDraws=parityHandDraws=0;
        Detour_DrawCreatureHD(item,0);
        Field<uint32_t>(off::item_mesh_bits)=0x3600;
        Detour_DrawCreatureHD(item,1);
        Check(parityBodyDraws==1 && parityForearmDraws==1 && parityHandDraws==2,
              "all collections draw body, weapon-mesh forearms and both tracked hands");
        Check(Field<uint32_t>(off::item_mesh_bits)==0x3600 && !g_bodySkinScope && g_renderArm==-1,
              "split render scopes restore mask and arm ownership");

        // Sparse HD attachment, nonidentity rest binds and duplicate helper.
        ReadyArmedIK(game);
        int mapping[16];float poses[16*12]{};
        for(int i=0;i<16;++i) {
            mapping[i]=i==15 ? 9 : (i==8 || i==11 || i==13 ? 0 : i);
            const Frame inverseBind{RotationMatrix(AxisRotation({0,1,0},.3f)),{8,-12,17}};
            WriteRows(inverseBind,poses+i*12);
            const auto native=ReadRows(ikNative+mapping[i]*12);
            WriteRows(Multiply(native,HdInverseBind(inverseBind)),Ptr<float>(dll.joints)+i*12);
        }
        std::memcpy(ikNative,Ptr<float>(dll.joints),sizeof(poses));
        auto* geom=Ptr<uint8_t>(dll.objects+off::object_geom);
        *reinterpret_cast<int*>(geom+28)=16;
        *reinterpret_cast<int**>(geom+48)=mapping;
        *reinterpret_cast<float**>(geom+72)=poses;
        g_bodySkinScope=true;Detour_GetJoints(item);
        uint32_t mask=0;const auto* sparse=FirstPersonBodySkin(mask);
        Frame wrist{},target{};Basis controller{};
        Check(ReadIKJoint(Ptr<uint8_t>(dll.objects),sparse,16,16,mapping,poses,10,wrist) &&
              BuildControllerWrist(1,target,controller),"sparse arm recovers missing shoulder and hand joints");
        Check(Dot(Sub(wrist.origin,target.origin),Sub(wrist.origin,target.origin))<.01f &&
              WristBasisError(wrist.basis,target.basis)<.001f,"nonidentity HD bind preserves exact wrist target");
        Check(!std::memcmp(sparse+9*12,sparse+15*12,12*sizeof(float)),"duplicate forearm helpers share correction");

        for(int state=56;state<=61;++state) {
            IKSetup(game);Field<int16_t>(off::item_anim_state)=int16_t(state);
            testConfig.firstPersonJoint=14;testConfig.firstPersonAnchorX=0;
            testConfig.firstPersonAnchorY=0;testConfig.firstPersonAnchorZ=140;
            testConfig.firstPersonInteractionAnchorZ=0;
            PHD_3DPOS eye{};Check(Anchor(eye),"ladder anchor valid");
            Check(g_ladderTorsoOffset.z==(game==1 ? 0.f : -80.f),"80-unit ladder clearance excludes TR1 reused states");
            const auto before=ReadRows(ikNative+7*12);
            Detour_GetJoints(item);const auto* palette=FirstPersonBodySkin(mask);
            Check(palette && !std::memcmp(palette,ikNative,7*12*sizeof(float)),"ladder pelvis and leg animation remain authored");
            Check(std::fabs(palette[7*12+11]-before.origin.z-(game==1 ? 0.f : -80.f))<.001f,
                  "ladder torso moves behind eye only in ladder games");
            for(int first:{8,11}) {
                const auto shoulder=ReadRows(palette+first*12),elbow=ReadRows(palette+(first+1)*12),hand=ReadRows(palette+(first+2)*12);
                const auto a=ReadRows(ikNative+first*12),b=ReadRows(ikNative+(first+1)*12),c=ReadRows(ikNative+(first+2)*12);
                Check(Dot(Sub(hand.origin,c.origin),Sub(hand.origin,c.origin))<.01f && WristBasisError(hand.basis,c.basis)<.001f,
                      "ladder hand preserves native rung grip and rotation");
                Check(std::fabs(std::sqrt(Dot(Sub(elbow.origin,shoulder.origin),Sub(elbow.origin,shoulder.origin)))-
                                std::sqrt(Dot(Sub(b.origin,a.origin),Sub(b.origin,a.origin))))<.01f,
                      "ladder shoulders never stretch upper arm");
                Check(std::fabs(std::sqrt(Dot(Sub(hand.origin,elbow.origin),Sub(hand.origin,elbow.origin)))-
                                std::sqrt(Dot(Sub(c.origin,b.origin),Sub(c.origin,b.origin))))<.01f,
                      "ladder shoulders never stretch forearm");
            }
            *Ptr<int32_t>(dll.renderPass)=4;Detour_GetJoints(item);
            Check(!std::memcmp(Ptr<float>(dll.joints),ikNative,sizeof(ikNative)),"ladder shadow preserves entire native skeleton");
            Field<int16_t>(off::item_anim_state)=2;Anchor(eye);
            Check(g_ladderTorsoOffset.x==0 && g_ladderTorsoOffset.z==0,"leaving ladder clears clearance");
        }
    }
}
}
