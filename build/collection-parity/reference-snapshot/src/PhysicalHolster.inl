// Included after the IK palette readers. The existing outer Lara draw hook
// scopes mesh selection; no second hook competes with DynamicBones' DrawLaraHD.
struct HolsterAnchor {
    motiongun::Vec point{};
    bool valid=false;
};
HolsterAnchor g_holsterAnchor[3]{};
uint8_t g_holsterKinds[3]{};
bool g_holsterVisible[3]{};
uint8_t* g_holsterItem=nullptr;
int g_holsterLevel=-1;
uint64_t g_holsterSeen=0,g_holsterRequestTime=0;
bool g_holsterRenderScope=false,g_holsterWasReady=false;

void ResetPhysicalHolsters() {
    VR().HolsterHoverHaptic(0,false);VR().HolsterHoverHaptic(1,false);
    g_holsters.Reset();g_holsterItem=nullptr;g_holsterLevel=-1;
    g_holsterSeen=g_holsterRequestTime=0;g_holsterWasReady=false;
    for(auto& a:g_holsterAnchor) a={};
    for(auto& k:g_holsterKinds) k=0;
    for(auto& visible:g_holsterVisible) visible=false;
}
void CancelHolsterTracking() {
    VR().HolsterHoverHaptic(0,false);VR().HolsterHoverHaptic(1,false);
    g_holsterSeen=0;
    for(int hand=0;hand<2;++hand) g_holsters.Cancel(hand,true);
}
int HolsterWeapon(int kind) {
    switch(kind) {
    case 1:return 1;case 2:return 3;case 3:case 4:return 2;
    case 5:case 11:return 4;case 6:case 9:case 12:case 15:return 5;
    case 7:case 8:case 10:case 13:case 14:case 16:return 6;
    default:return 0;
    }
}
int WeaponHolster(int weapon) {
    switch(weapon) {
    case 1:return 1;case 3:return 2;case 2:return g_boundDll->game==0?4:3;
    case 4:return 5;case 5:return g_boundDll->game==0?6:9;
    case 6:return g_boundDll->game==0?7:10;default:return 0;
    }
}
int HolsterSlot(int hand,int weapon) { return weapon>=4 ? 2 : hand; }

// GetJointAbsPosition and GetJoints use the same world axes. Recover their
// camera translation before body fit/IK, then apply it to the final palette.
bool HolsterCameraOffset(uint8_t* item,const float* joints,int count,motiongun::Vec& offset) {
    if(!g_holsterRenderScope || !g_bodySkinScope || count<15 || count>33) return false;
    const uint32_t mask=*reinterpret_cast<const uint32_t*>(item+off::item_mesh_bits);
    if((mask&0x12)!=0x12) return false; // body, never a hand/hip/back attachment
    const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
    if(object<0) return false;
    const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
    const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int32_t*>(geom+28);
    const auto* mapping=bones>0?*reinterpret_cast<const int32_t* const*>(geom+48):nullptr;
    const auto* poses=bones>0?*reinterpret_cast<const float* const*>(geom+72):nullptr;
    if(bones>0 && (bones!=count || !mapping || !poses)) return false;
    motiongun::Frame joint{};
    if(!ReadIKJoint(obj,joints,count,bones,mapping,poses,0,joint)) return false;
    offset=Sub(JointPoint(item,0,0,0,0),joint.origin);
    return true;
}
void CaptureHolsterAnchors(uint8_t* item,const float* joints,int count,motiongun::Vec offset) {
    const int object=*reinterpret_cast<const int16_t*>(item+off::item_object);
    const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
    const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int32_t*>(geom+28);
    const auto* mapping=bones>0?*reinterpret_cast<const int32_t* const*>(geom+48):nullptr;
    const auto* poses=bones>0?*reinterpret_cast<const float* const*>(geom+72):nullptr;
    // Canonical TRM holster coordinates: grips at the upper outside thighs;
    // long weapons use their upper back mounting point, reachable over shoulder.
    const motiongun::Vec grip[]={{-90,15,-20},{90,15,-20},{58,-210,-65}};
    const int bone[]={1,4,7};
    for(int slot=0;slot<3;++slot) {
        auto& anchor=g_holsterAnchor[slot];anchor.valid=false;
        motiongun::Frame joint{},canonicalBind{},inverseBind{};
        if(!ReadIKJoint(obj,joints,count,bones,mapping,poses,bone[slot],joint) ||
           !ReadIKBind(obj,0,0,nullptr,nullptr,bone[slot],canonicalBind) ||
           !motiongun::Inverse(canonicalBind,inverseBind)) continue;
        const auto skin=motiongun::Multiply(joint,inverseBind);
        anchor.point=Add(offset,Add(skin.origin,Transform(skin.basis,grip[slot])));
        anchor.valid=std::isfinite(anchor.point.x+anchor.point.y+anchor.point.z);
    }
    g_holsterSeen=GetTickCount64();
}

struct HolsterRenderScope {
    uint8_t* app=nullptr;
    uint8_t saved[5]{};
    bool previous=false;
    HolsterRenderScope(uint8_t* item,uint8_t* nativeApp) {
        if(ControllerTrackingBlockedReason() || DrawingNativeShadow()) return;
        const int level=AppState(drva::app_off::level);
        if(item!=g_holsterItem || level!=g_holsterLevel) {
            ResetPhysicalHolsters();g_holsterItem=item;g_holsterLevel=level;
        }
        app=nativeApp;std::memcpy(saved,app+0x508,5);
        const auto* outfit=*reinterpret_cast<const uint8_t* const*>(app+0x4f8);
        const bool weaponMeshes=outfit && *reinterpret_cast<const int32_t*>(outfit+0x1c)==1;
        g_holsterVisible[0]=g_holsterVisible[1]=weaponMeshes && *reinterpret_cast<const int32_t*>(outfit+0x10)!=0;
        g_holsterVisible[2]=weaponMeshes && *reinterpret_cast<const int32_t*>(outfit+0x14)!=0;
        for(int slot=0;slot<3;++slot) g_holsterKinds[slot]=saved[slot+2];
        previous=g_holsterRenderScope;g_holsterRenderScope=true;
        const int status=*Ptr<int16_t>(g_boundDll->lara+2);
        // Button equip uses native animation/mesh timing. Physical transfers
        // use grip ownership immediately, including during native status 2/3.
        if(CurrentMotionWeapon()==1 && (status==2 || status==3) && !PhysicalHolsterOwnsWeapon()) return;
        if(!g_holsters.active || CurrentMotionWeapon()!=g_holsters.weapon ||
           (status!=4 && status!=2 && status!=3)) return;
        const int weapon=g_holsters.weapon;
        for(int hand=0;hand<2;++hand) {
            if(hand==0 && !motiongun::DualWeapon(weapon)) continue;
            const int slot=HolsterSlot(hand,weapon);
            const int kind=WeaponHolster(weapon);
            g_holsterKinds[slot]=uint8_t(kind);
            const bool held=g_holsters.Has(hand);
            app[0x50a+slot]=held ? 0 : uint8_t(kind);
            if(!held) app[0x508+hand]=1; // REST, with native outfit bank selection
            else if(motiongun::DualWeapon(weapon)) app[0x508+hand]=weapon==1?5:6;
        }
    }
    ~HolsterRenderScope() {
        if(app) { std::memcpy(app+0x508,saved,5);g_holsterRenderScope=previous; }
    }
    HolsterRenderScope(const HolsterRenderScope&)=delete;
    HolsterRenderScope& operator=(const HolsterRenderScope&)=delete;
};

extern bool g_switchGripClaimed;
unsigned UpdatePhysicalHolsterGrips(bool left,bool right,bool leftValid,bool rightValid,
                                   bool allowed,uint64_t now) {
    const bool down[]={left,right},valid[]={leftValid,rightValid};
    if(!g_active || !g_boundDll || !g_boundBase || GameDllBound()!=g_boundDll ||
       GameDllBase()!=g_boundBase || (g_holsterItem &&
       (g_holsterItem!=*Ptr<uint8_t*>(g_boundDll->laraItem) ||
        g_holsterLevel!=AppState(drva::app_off::level)))) {
        ResetPhysicalHolsters();return 0;
    }
    const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    const float scale=LiveWorldUnitsPerMetre();
    allowed=allowed && !ControllerTrackingBlockedReason() && item && CanTurnBody(item) &&
        LaraWaterStatus()==0 && g_holsterSeen && now>=g_holsterSeen &&
        now-g_holsterSeen<=150 && std::isfinite(scale) && scale>1;
    const int status=*Ptr<int16_t>(g_boundDll->lara+2),weapon=CurrentMotionWeapon();
    if(g_holsters.active && weapon!=g_holsters.weapon &&
       (status==4 || (status==0 && !g_holsters.mask && !g_holsters.request))) {
        ResetPhysicalHolsters();return 0;
    }
    unsigned consumed=0;
    for(int hand=0;hand<2;++hand) {
        float rightMetres=0,downMetres=0,forwardMetres=0;
        vr::HmdMatrix34_t pose{};
        const bool tracked=allowed && valid[hand] && !(hand==1 && g_switchGripClaimed) &&
            VR().ControllerPose(hand,pose) &&
            VR().FirstPersonControllerOffset(hand,rightMetres,downMetres,forwardMetres) &&
            std::isfinite(rightMetres+downMetres+forwardMetres) &&
            std::fabs(rightMetres)<2 && std::fabs(downMetres)<2 && std::fabs(forwardMetres)<2;
        const auto point=motiongun::HandInWorld(
            {float(g_scenePose.x_pos),float(g_scenePose.y_pos),float(g_scenePose.z_pos)},
            rightMetres,downMetres,forwardMetres,g_headingBase,scale);
        int slot=-1,available=0;float distance=100;
        const auto& grip=g_holsters.hand[hand];
        for(int candidate=0;candidate<3;++candidate) {
            if(!g_holsterVisible[candidate] || !g_holsterAnchor[candidate].valid ||
               (candidate!=hand && !(hand==1 && candidate==2))) continue;
            if(grip.claimed && candidate!=grip.slot) continue;
            int gun=HolsterWeapon(g_holsterKinds[candidate]);
            if(status==4 && candidate==HolsterSlot(hand,weapon) && (hand==1 || motiongun::DualWeapon(weapon))) gun=weapon;
            if(g_holsters.active && candidate==HolsterSlot(hand,g_holsters.weapon) &&
               (hand==1 || motiongun::DualWeapon(g_holsters.weapon))) gun=g_holsters.weapon;
            if(!MotionWeaponSupported(gun)) continue;
            if(!grip.claimed && !g_holsters.WeaponEligible(hand,gun,status,weapon)) continue;
            const auto delta=Sub(point,g_holsterAnchor[candidate].point);
            const float d=std::sqrt(motiongun::Dot(delta,delta))/scale;
            if(std::isfinite(d) && d<distance) { distance=d;slot=candidate;available=gun; }
        }
        const unsigned oldMask=g_holsters.mask;
        const bool wasActive=g_holsters.active;
        if(g_holsters.Poll(hand,down[hand],tracked && slot>=0,slot,available,distance,status,weapon))
            consumed|=1u<<hand;
        VR().HolsterHoverHaptic(hand,tracked &&
            g_holsters.Hover(hand,slot,available,distance,status,weapon));
        if(wasActive!=g_holsters.active || oldMask!=g_holsters.mask) {
            g_gunTriggers.Clear(hand);VR().GunShotHaptic(hand);
            LogF("firstperson: physical holster weapon=%d hands=%u",g_holsters.weapon,g_holsters.mask);
        }
    }
    return consumed;
}
