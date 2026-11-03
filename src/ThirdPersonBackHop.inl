// Opt-in Hybrid Modern/Tank Controls: RG + LS down requests a third-person back-hop.
// Do not use the HMD heading or require a first-person camera anchor.
struct ThirdPersonBackHopScope {
    uint32_t* input=nullptr;
    int16_t* axes=nullptr;
    uint32_t savedDirections=0;
    int16_t savedAxes[4]{};
    explicit ThirdPersonBackHopScope(uint8_t* item) {
        using namespace locomotion;
        if(!Cfg().hybridModernTankControls || !g_thirdPersonBackHopPressed || g_active || g_runtimeEnabled ||
           !item || !g_boundDll || !g_boundBase || !Cfg().enabled || !Cfg().gamepadEnabled ||
           !VR().active() || !VR().poseValid() || g_shifted || g_jumpPressed ||
           InInventory() || InTitle() || InCutscene() || !GroundJumpControlsAvailable() ||
           item!=*Ptr<uint8_t*>(g_boundDll->laraItem) || !GroundBodyAvailable(item) ||
           !CanModifyGroundMotion(item)) return;
        auto* nativeInput=Ptr<uint32_t>(g_boundDll->input);
        constexpr uint32_t duck=firstperson::PhysicalCrouchInputScope::Duck;
        if(*nativeInput&(0x10u|0x40u|0x100u|0x1000u|0x4000u|duck)) return;
        const int required=*reinterpret_cast<const int16_t*>(item+22);
        // A stale required DUCK after standing in VCI must not block the chord.
        // Active posture/interaction goals and input were rejected above.
        if(required && !((BoundGame()==3) && required==71 && !(*Ptr<uint32_t>(g_boundDll->lara+0x40)&firstperson::PhysicalCrouchInputScope::Latch))) return;
        const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
        const int clip=*reinterpret_cast<const int16_t*>(item+off::item_anim_number);
        const bool turning=(state==6 && clip==12) || (state==7 && clip==13) ||
            (state==20 && (clip==44 || clip==69));
        const bool hopping=state==5 && (clip==88 || clip==89 || clip==90);
        if(!hopping && !OrdinaryGroundAnimation(item) && !turning) return;
        if(!hopping && !EnterGroundIdle(item)) return;
        input=nativeInput;savedDirections=*input&(Directions|Walk);
        *input=(*input&~(Directions|Walk))|Back;
        // Native modern controls otherwise rotate toward the backward stick
        // before interpreting Back. Keep camera input scoped to this tick.
        const int16_t yaw=reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos)->y_rot;
        axes=Ptr<int16_t>(g_boundDll->analogInput);
        for(int i=0;i<4;++i) savedAxes[i]=axes[i];
        axes[0]=axes[1]=0;axes[2]=axes[3]=yaw;
        *Ptr<int16_t>(g_boundDll->lara+252)=0;
        *Ptr<int16_t>(g_boundDll->lara+254)=int16_t(uint16_t(yaw)+0x8000u);
        // Native STOP performs rear-floor clearance and selects FASTBACK;
        // animation, displacement, gravity and wall collision remain native.
    }
    ~ThirdPersonBackHopScope() {
        if(!input) return;
        constexpr auto mask=locomotion::Directions|locomotion::Walk;
        *input=(*input&~mask)|savedDirections;
        for(int i=0;i<4;++i) axes[i]=savedAxes[i];
    }
};
