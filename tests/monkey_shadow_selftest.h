// Production input hooks with independently calculated native monkey targets.
namespace {
int monkeyCalls=0;
void __cdecl NativeMonkey(uint8_t* target,void*) {
    Check(target==item,"monkey callback receives Lara");++monkeyCalls;
}
void TestMonkeyControls() {
    using namespace tr;
    for (bool modern:{false,true}) for (int state:{75,76,77,78,79,82,83})
    for (float head:{-2.8f,-1.5f,0.f,.7f,2.8f}) for (float body:{-2.f,0.f,2.f})
    for (float camera:{-2.5f,0.f,1.8f}) for (bool withHead:{false,true})
    for (Vec stick:{Vec{0,1},{0,-1},{-1,0},{1,0},{.6f,.8f},{-.8f,-.6f}}) {
        Reset(state,{},true,head);dll.module=L"tomb3.dll";
        testConfigFlags=modern ? 2 : 0;testConfig.firstPersonMoveWithHead=withHead;
        *Ptr<uint16_t>(dll.lara+60)=0x40;Field<PHD_3DPOS>(88).y_rot=Angle(body);
        *Ptr<int16_t>(dll.analogInput+4)=Angle(camera);
        const float actualCamera=Radians(*Ptr<int16_t>(dll.analogInput+4));
        const float actualBody=Radians(Field<PHD_3DPOS>(88).y_rot);
        float x=stick.x,y=stick.z,r=0;
        FirstPersonInput(x,y,r,false,false);
        Check(g_haveManualInput,"monkey states capture raw stick intent");
        Ptr<int16_t>(dll.analogInput)[0]=12000;Ptr<int16_t>(dll.analogInput)[1]=16000;
        *Ptr<uint32_t>(dll.input)=0x40u|0x1000u|Back|Left; // Native Action/drop must survive.
        uint8_t original[sizeof(item)];std::memcpy(original,item,sizeof(item));
        uint8_t lara[432];std::memcpy(lara,Ptr<uint8_t>(dll.lara),sizeof(lara));
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeMonkey);monkeyCalls=0;
        Detour_LaraAboveWater(item,nullptr);
        const float yaw=withHead ? head : actualCamera;
        const Vec world{stick.x*std::cos(yaw)+stick.z*std::sin(yaw),
                        stick.z*std::cos(yaw)-stick.x*std::sin(yaw)};
        const Vec local{world.x*std::cos(actualBody)-world.z*std::sin(actualBody),
                        world.z*std::cos(actualBody)+world.x*std::sin(actualBody)};
        const uint32_t action=modern ? Forward : std::fabs(local.x)>std::fabs(local.z) ?
            (local.x<0 ? StepLeft : StepRight) : (local.z<0 ? Back : Forward);
        Check(*Ptr<uint32_t>(dll.input)==(0x40u|0x1000u|action),"monkey controls preserve Action/drop and choose native traversal bits");
        Check(*Ptr<int16_t>(dll.analogInput+4)==Angle(head) &&
              *Ptr<int16_t>(dll.analogInput+6)==Angle(head),"monkey controls replace hidden chase-camera yaw");
        const auto* analog=Ptr<int16_t>(dll.analogInput);
        const float length=std::sqrt(world.x*world.x+world.z*world.z);
        Check(std::fabs(analog[0]-(world.x*std::cos(head)-world.z*std::sin(head))*20000/length)<2 &&
              std::fabs(analog[1]-(world.z*std::cos(head)+world.x*std::sin(head))*20000/length)<2,
              "monkey analog direction matches headset reference and retains magnitude");
        Check(!std::memcmp(original,item,sizeof(item)) && !std::memcmp(lara,Ptr<uint8_t>(dll.lara),sizeof(lara)),
              "monkey input cannot overwrite native root, yaw, speed, pose or grip state");
        Check(monkeyCalls==1 && !g_groundMoveAction && !g_stabilizeRoot,
              "native monkey simulation runs once without ground-motion scaling");
    }
    for (int excluded=0;excluded<13;++excluded) {
        Reset(75,{},true);dll.module=L"tomb3.dll";*Ptr<uint16_t>(dll.lara+60)=0x40;
        if(excluded==0)dll.module=L"tomb1.dll";
        if(excluded==1)dll.module=L"tomb2.dll";
        if(excluded==2)*Ptr<uint16_t>(dll.lara+60)=0;
        if(excluded==3)testWater=1;
        if(excluded==4)Field<int16_t>(38)=0;
        if(excluded==5)g_active=false;
        if(excluded==6)*Ptr<int32_t>(dll.camera+32)=1;
        if(excluded==7)*Ptr<uint16_t>(dll.lara+60)|=0x2000;
        if(excluded==8)Field<int16_t>(18)=19;
        if(excluded==9)Field<int16_t>(18)=56;
        float x=excluded==10 ? .05f : .8f,y=0,r=0;
        FirstPersonInput(x,y,r,excluded==11,false);
        if(excluded==12)g_haveManualInput=false; // Keyboard/D-pad only.
        *Ptr<uint32_t>(dll.input)=0x40|Back;
        int16_t analog[4]={12000,16000,1234,4321};std::memcpy(Ptr<int16_t>(dll.analogInput),analog,sizeof(analog));
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeMonkey);
        Detour_LaraAboveWater(item,nullptr);
        Check(*Ptr<uint32_t>(dll.input)==(0x40|Back) &&
              !std::memcmp(analog,Ptr<int16_t>(dll.analogInput),sizeof(analog)),
              "monkey override excludes other games, non-monkey states, deadzone, shifted input and unavailable gameplay");
    }
}
} // namespace

namespace {
tr::mat4 nativeShadow{},liveShadow{};
int shadowCalls=0;
void __cdecl NativeShadow(int32_t room) {
    using namespace tr;
    Check(room==7,"shadow hook preserves native room argument");
    Check(FirstPersonShadowPass(),"entire native shadow build has light-camera ownership");
    Check(!std::memcmp(&liveShadow,&nativeShadow,sizeof(liveShadow)),
          "next shadow build restores native receiver before rebuilding");
    ++shadowCalls;
    *Ptr<int32_t>(dll.renderPass)=4;
    Detour_GetJoints(item);
    Check(!std::memcmp(Ptr<float>(dll.joints),ikNative,sizeof(ikNative)),
          "shadow caster retains native skeleton despite active controller IK and body fit");
    Check(!FirstPersonNativeBodySkin(),"native shadow cannot feed the jiggle sampling palette");
    *Ptr<int32_t>(dll.renderPass)=0;
    testRenderState.shadow=&liveShadow;
}
void TestShadowPlacement() {
    using namespace tr;
    IKSetup();g_bodyVisualOffset={400,-300};
    g_shadowReceiver=nullptr;g_shadowReceiverPatched=false;g_shadowDepthScope=false;
    g_hDrawToShadow.m_trampoline=reinterpret_cast<void*>(&NativeShadow);
    for (bool perspective:{false,true}) for (float yaw:{-2.5f,-.4f,0.f,1.8f}) {
        nativeShadow={};
        // Native light matrix has rotation, anisotropic scale and translation.
        nativeShadow.m[0]=std::cos(yaw)*.002f;nativeShadow.m[8]=std::sin(yaw)*.002f;
        nativeShadow.m[1]=.0003f;nativeShadow.m[5]=-.004f;nativeShadow.m[9]=.0007f;
        nativeShadow.m[2]=-std::sin(yaw)*.003f;nativeShadow.m[10]=std::cos(yaw)*.003f;
        nativeShadow.m[12]=.4f;nativeShadow.m[13]=-.2f;nativeShadow.m[14]=.1f;
        nativeShadow.m[3]=perspective ? .0001f : 0;
        nativeShadow.m[7]=perspective ? .0002f : 0;
        nativeShadow.m[11]=perspective ? .0004f : 0;nativeShadow.m[15]=1;
        RestoreShadowReceiver();g_shadowReceiver=nullptr;liveShadow=nativeShadow;
        shadowCalls=0;Detour_DrawToShadow(7);
        Check(shadowCalls==1 && !FirstPersonShadowPass(),"native shadow runs once and releases its render scope");
        const PHD_3DPOS native{3200,-550,1300,0,1234,0};
        for (int repeat=0;repeat<8;++repeat) for (int dx:{-600,0,450})
        for (int dy:{-700,0,240}) for (int dz:{-800,0,350}) {
            PHD_3DPOS scene{native.x_pos+dx,native.y_pos+dy,native.z_pos+dz,
                            int16_t(repeat*400),int16_t(repeat*700),int16_t(repeat*130)};
            testRenderState.consts=0;RebaseShadowReceiver(native,scene);
            Check((testRenderState.consts&kShadow)!=0,"receiver update is marked dirty for GPU upload");
            Check(!std::memcmp(liveShadow.m,nativeShadow.m,12*sizeof(float)),
                  "camera correction preserves native light angle and projection basis");
            for (const auto point:{std::array<double,3>{3500,-50,1600},{2900,-1200,500},{4000,0,1900}}) {
                double before[4]{},after[4]{};
                const double oldOrigin[3]={double(native.x_pos),double(native.y_pos),double(native.z_pos)};
                const double newOrigin[3]={double(scene.x_pos),double(scene.y_pos),double(scene.z_pos)};
                for(int row=0;row<4;++row) {
                    before[row]=nativeShadow.m[12+row];after[row]=liveShadow.m[12+row];
                    for(int axis=0;axis<3;++axis) {
                        before[row]+=nativeShadow.m[axis*4+row]*(point[axis]-oldOrigin[axis]);
                        after[row]+=liveShadow.m[axis*4+row]*(point[axis]-newOrigin[axis]);
                    }
                    Check(std::fabs(before[row]-after[row])<.00001,
                          "fixed world point retains native shadow coordinates under camera translation/rotation and repeated rebasing");
                }
                Check(std::fabs(before[0]/before[3]-after[0]/after[3])<.00002 &&
                      std::fabs(before[1]/before[3]-after[1]/after[3])<.00002,
                      "projected shadow UV stays world-locked for perspective and orthographic lights");
            }
        }
        Detour_DrawToShadow(7);
        Check(shadowCalls==2 && !std::memcmp(&liveShadow,&nativeShadow,sizeof(liveShadow)),
              "successive frames cannot accumulate camera displacement into the native receiver");
        PHD_3DPOS scene{3700,-700,1700};RebaseShadowReceiver(native,scene);
        g_active=false;RebaseShadowReceiver(native,scene);
        Check(!std::memcmp(&liveShadow,&nativeShadow,sizeof(liveShadow)),
              "third-person/menu/switch handoff restores native shadow placement");g_active=true;
        RebaseShadowReceiver(native,scene);g_scenePoseValid=false;RebaseShadowReceiver(native,scene);
        Check(!std::memcmp(&liveShadow,&nativeShadow,sizeof(liveShadow)),
              "invalid scene pose restores native shadow placement");g_scenePoseValid=true;
        RebaseShadowReceiver(native,scene);testConfigFlags=0;RebaseShadowReceiver(native,scene);
        Check(!std::memcmp(&liveShadow,&nativeShadow,sizeof(liveShadow)),
              "classic graphics leaves native shadow receiver untouched");testConfigFlags=1;
        RebaseShadowReceiver(native,scene);liveShadow.m[0]+=1;const auto overwritten=liveShadow;
        RestoreShadowReceiver();
        Check(!std::memcmp(&liveShadow,&overwritten,sizeof(liveShadow)),
              "restoration never overwrites a newer engine-owned matrix");
        g_shadowReceiver=nullptr;g_shadowReceiverPatched=false;
    }
    testRenderState={};g_shadowReceiver=nullptr;g_shadowReceiverPatched=false;
}
} // namespace
