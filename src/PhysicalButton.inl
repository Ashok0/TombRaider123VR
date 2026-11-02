// TR2/3 PUSH_SWITCH. Reuses the verified native switch hooks; no new RVAs.
struct ButtonHand {
    motiongun::Vec point{};
    uint64_t time=0;
    bool valid=false,latched=false;
    motiongun::Frame inverse{};
    firstperson::ButtonSurface face{};
};
ButtonHand g_buttonHands[2]{};
uint8_t* g_buttonItems=nullptr;
int g_buttonLevel=-1;
void ResetPhysicalButtons() {
    for(auto& h:g_buttonHands) h={};g_buttonItems=nullptr;g_buttonLevel=-1;
}
bool ButtonHandPoint(int hand,motiongun::Vec& point) {
    motiongun::Frame wrist{},bind{},inverse{};motiongun::Basis controller{};motiongun::Vec palm{};
    if(!TrackedControllerWrist(hand,wrist,controller,&palm)) return false;
    auto* lara=*Ptr<uint8_t*>(g_boundDll->laraItem);
    const int slot=*reinterpret_cast<int16_t*>(lara+off::item_object_number);
    if(slot<0 || !g_boundDll->objects) return false;
    const auto* object=Ptr<uint8_t>(g_boundDll->objects)+slot*off::object_stride;
    if(*reinterpret_cast<const int16_t*>(object)!=15 ||
       !ReadIKBind(object,15,0,nullptr,nullptr,hand ? 10 : 13,bind) || !motiongun::Inverse(bind,inverse)) return false;
    palm=motiongun::Transform(inverse,{hand ? 62.f : -62.f,40,0});
    point=motiongun::Transform(wrist,palm);
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}
bool ButtonReady(uint8_t* target) {
    const int state=*reinterpret_cast<int16_t*>(target+18);
    return (state==0 || state==1) && !(*reinterpret_cast<uint16_t*>(target+off::item_flags)&6) &&
        !(*reinterpret_cast<uint16_t*>(target+44)&0x100);
}
const firstperson::ButtonProfile* ButtonObject(uint8_t* target,const uint8_t*& object) {
    if(!g_switchDll || !g_boundDll || !g_boundDll->objects) return nullptr;
    const int game=g_boundDll->module[4]-L'0';
    const int slot=*reinterpret_cast<int16_t*>(target+16);
    if((game!=2 && game!=3) || slot!=(game==2 ? 103 : 128)) return nullptr;
    object=Ptr<uint8_t>(g_boundDll->objects)+slot*off::object_stride;
    if(*reinterpret_cast<const int16_t*>(object)!=2 ||
       *reinterpret_cast<const int32_t*>(object+off::object_geom+28) ||
       *reinterpret_cast<const uint64_t*>(object+64)!=g_boundBase+g_switchDll->collision ||
       *reinterpret_cast<const uint64_t*>(object+24)!=g_boundBase+g_switchDll->control) return nullptr;
    return firstperson::FindButtonProfile(game,*reinterpret_cast<const char* const*>(object+off::object_geom));
}
bool ButtonFrame(uint8_t* target,const uint8_t* object,const firstperson::ButtonProfile& profile,
                 motiongun::Frame& inverse,firstperson::ButtonSurface& face) {
    motiongun::Frame bind{},inverseBind{};
    if(!ReadIKBind(object,2,0,nullptr,nullptr,1,bind) || !motiongun::Inverse(bind,inverseBind)) return false;
    alignas(16) uint8_t probe[off::item_stride];std::memcpy(probe,target,sizeof(probe));
    SwitchPoseMatrices(probe,{*reinterpret_cast<int16_t*>(target+24),*reinterpret_cast<int16_t*>(target+26),0});
    auto world=motiongun::Multiply(SwitchJoint(probe,1),inverseBind);
    const auto& pos=*reinterpret_cast<const PHD_3DPOS*>(target+off::item_pos);
    world.origin=Add(world.origin,{float(pos.x_pos),float(pos.y_pos),float(pos.z_pos)});
    face=profile.face;
    // Native toggled buttons rotate their moving face by almost 180 degrees.
    // Use the exposed back face, with its own depth, when that happens.
    const auto root=SwitchJoint(probe,0);
    float facing=0;for(int i=0;i<3;++i) facing+=world.basis.r[i][2]*root.basis.r[i][2];
    if(facing<0) {
        for(int i=0;i<3;++i) { world.basis.r[i][1]*=-1;world.basis.r[i][2]*=-1; }
        face.top=-profile.face.bottom;face.bottom=-profile.face.top;face.z=-profile.backZ;
    }
    return SwitchFinite(world) && motiongun::Inverse(world,inverse);
}
bool ActivatePhysicalButton(int16_t index,uint8_t* target,uint8_t* lara,void* collision) {
    if(!ButtonReady(target)) return false;
    uint8_t animation[10],angles[12],position[40];
    std::memcpy(animation,lara+18,sizeof(animation));std::memcpy(position,lara+off::item_pos,sizeof(position));
    std::memcpy(angles,Ptr<uint8_t>(g_boundDll->lara+256),sizeof(angles));
    const auto general=*Ptr<int16_t>(g_boundDll->lara+58),status=*Ptr<int16_t>(g_boundDll->lara+2);
    const auto flags=*Ptr<uint16_t>(g_boundDll->lara+60);
    auto& input=*Ptr<uint32_t>(g_boundDll->input);const auto savedInput=input;auto* savedCommit=g_switchCommit;
    *reinterpret_cast<int16_t*>(lara+18)=2;
    *reinterpret_cast<int16_t*>(lara+24)=g_boundDll->module[4]==L'3' ? 103 : 11;
    g_switchCommit=target;input|=0x40;
    g_hSwitchCollision.Original<Fn_SwitchCollision>()(index,lara,collision);
    g_switchCommit=savedCommit;input=savedInput;
    std::memcpy(lara+18,animation,sizeof(animation));std::memcpy(lara+off::item_pos,position,sizeof(position));
    std::memcpy(Ptr<uint8_t>(g_boundDll->lara+256),angles,sizeof(angles));
    *Ptr<int16_t>(g_boundDll->lara+58)=general;*Ptr<int16_t>(g_boundDll->lara+2)=status;
    *Ptr<uint16_t>(g_boundDll->lara+60)=flags;
    const bool activated=(*reinterpret_cast<uint16_t*>(target+off::item_flags)&6)==2;
    // The native control loop owns animation, timer and linked-door triggers.
    if(activated) {
        VR().ButtonPressHaptic();
        LogF("vr-button: touched push button item=%d; native control continues",index);
    }
    return activated;
}
bool ButtonSight(uint8_t* lara,motiongun::Vec hand) {
    if(!g_boundDll->los || !g_boundDll->getFloor) return false;
    using Floor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
    using Sight=int32_t (__cdecl*)(ShotVector*,ShotVector*);
    ShotVector eye{g_scenePose.x_pos,g_scenePose.y_pos,g_scenePose.z_pos,
        *reinterpret_cast<const int16_t*>(lara+off::item_room_number),0};
    reinterpret_cast<Floor>(g_boundBase+g_boundDll->getFloor)(eye.x,eye.y,eye.z,&eye.room);
    ShotVector end{int32_t(std::lround(hand.x)),int32_t(std::lround(hand.y)),int32_t(std::lround(hand.z)),eye.room,0};
    return reinterpret_cast<Sight>(g_boundBase+g_boundDll->los)(&eye,&end)!=0;
}
void AdvancePhysicalButton(uint8_t* lara,void* collision,uint64_t now) {
    if(!PhysicalSwitchReady() || !lara || lara!=*Ptr<uint8_t*>(g_boundDll->laraItem) ||
       g_boundDll->module[4]==L'1' || g_switchGripClaimed || (*reinterpret_cast<uint16_t*>(lara+off::item_flags)&8)) {
        ResetPhysicalButtons();return;
    }
    const int state=*reinterpret_cast<int16_t*>(lara+18);
    if(state!=2 && !(g_boundDll->module[4]==L'3' && (state==71 || state==80))) { ResetPhysicalButtons();return; }
    auto* items=*Ptr<uint8_t*>(g_switchDll->items);const int count=*Ptr<int32_t>(g_switchDll->count);
    if(!items || count<1 || count>32768) { ResetPhysicalButtons();return; }
    const int level=AppFlag(drva::app_off::level);
    if(level!=g_buttonLevel || items!=g_buttonItems) ResetPhysicalButtons();
    g_buttonLevel=level;g_buttonItems=items;
    const float scale=LiveWorldUnitsPerMetre();const auto& body=*reinterpret_cast<const PHD_3DPOS*>(lara+off::item_pos);
    for(int hand=0;hand<2;++hand) {
        auto& h=g_buttonHands[hand];motiongun::Vec point{};
        if(!ButtonHandPoint(hand,point)) { h.valid=false;continue; }
        const auto previous=h;h.point=point;h.time=now;h.valid=true;
        if(h.latched) {
            if(motiongun::Transform(h.inverse,point).z<h.face.z-scale*.10f) h.latched=false;
            continue;
        }
        if(!previous.valid || now<=previous.time || now-previous.time>250 || SwitchLength(Sub(point,previous.point))>scale*.4f) continue;
        for(int i=0;i<count;++i) {
            auto* target=items+i*off::item_stride;const uint8_t* object=nullptr;const auto* profile=ButtonObject(target,object);
            if(!profile || !ButtonReady(target)) continue;
            const auto& pos=*reinterpret_cast<const PHD_3DPOS*>(target+off::item_pos);
            if(std::abs(float(body.y_pos)-pos.y_pos)>768 || std::hypot(float(body.x_pos)-pos.x_pos,float(body.z_pos)-pos.z_pos)>1800) continue;
            motiongun::Frame inverse{};firstperson::ButtonSurface face{};
            if(!ButtonFrame(target,object,*profile,inverse,face) ||
               !firstperson::ButtonTouch(face,motiongun::Transform(inverse,previous.point),motiongun::Transform(inverse,point),scale) ||
               !ButtonSight(lara,point)) continue;
            h.latched=true;h.inverse=inverse;h.face=face;
            ActivatePhysicalButton(int16_t(i),target,lara,collision);g_buttonHands[1-hand].valid=false;return;
        }
    }
}
