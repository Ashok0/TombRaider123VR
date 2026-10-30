// Exercise the actual simulation/draw detours. The callback models the
// rotation-before-digital-jump branch verified in all three native DLLs.
namespace {
bool jumpAxesMasked=false,jumpFloorClear=true;
float jumpTravel=0;
uint32_t jumpDigital=0,jumpDrawBits=0;
int jumpDrawCalls=0;
void __cdecl NativeDirectionalJump(uint8_t* target,void*) {
    auto* axes=tr::Ptr<int16_t>(dll.analogInput);
    jumpAxesMasked=axes[0]==0 && axes[1]==0;
    jumpDigital=*tr::Ptr<uint32_t>(dll.input)&tr::locomotion::Directions;
    auto& pos=Field<tr::PHD_3DPOS>(tr::off::item_pos);
    float facing=tr::locomotion::Radians(pos.y_rot);
    if(!jumpAxesMasked) facing+=std::atan2(float(axes[0]),float(axes[1]))*.5f;
    const float offset=jumpDigital==tr::locomotion::Left ? -tr::locomotion::Pi/2 :
        jumpDigital==tr::locomotion::Right ? tr::locomotion::Pi/2 : tr::locomotion::Pi;
    jumpTravel=tr::locomotion::Wrap(facing+offset);
    if(!jumpFloorClear) return;
    Field<int16_t>(tr::off::item_anim_state)=int16_t(jumpDigital==tr::locomotion::Left?27:jumpDigital==tr::locomotion::Right?26:25);
    Field<uint16_t>(tr::off::item_flags)|=8;
    *tr::Ptr<int16_t>(dll.lara+254)=tr::locomotion::Angle(jumpTravel);
    tr::Detour_AnimateLara(target);
}
void __cdecl NativeJumpVisibility(void* target,int32_t masked) {
    ++jumpDrawCalls;
    jumpDrawBits|=masked ? *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(target)+tr::off::item_mesh_bits) : UINT32_MAX;
}
void TestDirectionalJumps() {
    using namespace tr;using namespace tr::locomotion;
    for(int game:{1,2,3}) for(bool calibrated:{false,true}) for(bool stabilized:{false,true}) for(int state:{25,26,27}) {
        Reset(state,{},stabilized);testHeadYaw=0;
        dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
        dll.frameFrac=204;g_dragPrevious=g_dragCurrent=g_dragShown={};g_previousBody={};
        g_groundEye.Reset();
        if(calibrated) g_groundEye.Apply({0,0,0},0,{0,-700,80},0);
        Field<PHD_3DPOS>(off::item_pos)={40,-100,-100};
        for(int fraction:{0,32,128,255,256}) for(int neckBob:{-200,0,150}) {
            *Ptr<int32_t>(dll.frameFrac)=fraction;
            PHD_3DPOS eye{200,-700+neckBob,250};const auto animated=eye;
            UpdateLocomotion(eye);
            if(calibrated && stabilized) {
                Check(eye.x_pos==Lerp(0,40,fraction) && eye.y_pos==Lerp(0,-100,fraction)-700 &&
                    eye.z_pos==Lerp(0,-100,fraction)+80,
                    "backjump eye follows native collision arc without a second animated-neck launch");
                Check(g_groundEye.local.y==-700,"backjump cannot recalibrate the standing eye");
            } else Check(eye.x_pos==animated.x_pos && eye.y_pos==animated.y_pos && eye.z_pos==animated.z_pos,
                "uncalibrated midair entry and disabled stabilization keep the native backjump view");
        }
    }
    for(int game:{1,2,3}) for(float head:{-2.7f,-.8f,0.f,.9f,2.8f})
    for(Vec stick:{Vec{-1,0},Vec{1,0},Vec{0,-1}}) for(bool clear:{false,true}) {
        Reset(15,stick,true,head);testHeadYaw=0;testConfigFlags=2;
        dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeDirectionalJump);
        auto* axes=Ptr<int16_t>(dll.analogInput);axes[0]=12345;axes[1]=-23456;
        *Ptr<uint32_t>(dll.input)=0x10u;jumpFloorClear=clear;nativeSpeed=50;
        Detour_LaraAboveWater(item,nullptr);
        Check(jumpAxesMasked,"modern directional jump does not apply native stick yaw a second time");
        const uint32_t direction=stick.x<0?Left:stick.x>0?Right:Back;
        Check(jumpDigital==direction,"digital left/right/back intent reaches native compress");
        const float expected=Wrap(head+(stick.x<0?-Pi/2:stick.x>0?Pi/2:Pi));
        Check(std::fabs(Wrap(jumpTravel-expected))<.0002f,"directional takeoff is relative to VR heading exactly once");
        Check(axes[0]!=0 || axes[1]!=0,"temporary jump axis suppression is restored after simulation");
        const auto& pos=Field<PHD_3DPOS>(off::item_pos);
        Check(clear ? std::fabs(std::hypot(float(pos.x_pos),float(pos.z_pos))-50)<1 : ticks==0,
            "native jump speed and blocked takeoff remain under native ownership");
    }
    for(int game:{1,2,3}) for(int status:{0,2,4}) for(int weapon:{1,8,9})
    for(bool modern:{false,true}) {
        Reset(15,{-1,0},true);testHeadYaw=0;testConfigFlags=modern?2:0;
        dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
        *Ptr<int16_t>(dll.lara+2)=int16_t(status);*Ptr<int16_t>(dll.lara+4)=int16_t(weapon);
        auto* axes=Ptr<int16_t>(dll.analogInput);axes[0]=123;axes[1]=-456;
        const bool nativeArmed=status==4 || (status==2 && (game==1 || weapon!=(game==2?8:9)));
        {
            DirectionalJumpAxesScope guard(item);
            Check((axes[0]==0 && axes[1]==0)==(modern && !nativeArmed),
                "jump guard respects each game's native armed/flare rules and tank controls");
        }
        Check(axes[0]==123 && axes[1]==-456,"jump guard restores exact analog values");
    }
    for(int game:{1,2,3}) for(bool armed:{false,true}) for(bool fullBody:{false,true}) {
        BodyRig(game,armed);testConfig.firstPersonFullBodyIK=fullBody;
        g_headHidden=true;g_rollHidden=g_crouchHidden=g_ledgeArmsOnly=g_unarmedArmsHidden=false;
        g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&NativeJumpVisibility);
        for(int state:{25,2,26,2,27,2}) {
            Field<int16_t>(off::item_anim_state)=int16_t(state);
            Field<uint32_t>(off::item_mesh_bits)=0x7fff;
            jumpDrawCalls=0;jumpDrawBits=0;
            Detour_DrawCreatureHD(item,0);
            Check(Field<uint32_t>(off::item_mesh_bits)==0x7fff,"body pass restores native item mask");
            Field<uint32_t>(off::item_mesh_bits)=0x3600;
            Detour_DrawCreatureHD(item,1);
            Check(jumpDrawCalls>0,"jump draw keeps controller hands available");
            Check(state==2 ? (jumpDrawBits&0xffu)==0xffu : (jumpDrawBits&~0x2400u)==0,
                "side/back jump hides body and forearms then restores them on exit");
            Check((jumpDrawBits&0x2400u)==0x2400u,"jump masking preserves both tracked hands");
            Check(Field<uint32_t>(off::item_mesh_bits)==0x3600,"jump visibility never persists in native attachment mask");
            *Ptr<int32_t>(dll.renderPass)=4;jumpDrawBits=0;
            Detour_DrawCreatureHD(item,0);
            Check(jumpDrawBits==UINT32_MAX,"hidden jumping body still casts a complete native shadow");
            *Ptr<int32_t>(dll.renderPass)=0;
        }
    }
    testControllerOffset[0]=testControllerOffset[1]={};
}
}
