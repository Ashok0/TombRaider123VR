// Included after SwitchJoint. These native slots share SwitchCollision/control.
struct SpecialSwitchProfile {
    int game,slot,joints,joint;
    const char* name;
    bool wheel;
    float x,y,z,radius; // handle half-width or wheel centre/radius, mesh space
};
constexpr SpecialSwitchProfile kSpecialSwitches[]={
    {2,93,2,1,"SMALL_SWITCH",false,45,-67,-39,0},
    {3,118,2,1,"SMALL_SWITCH",false,45,-73,-42,0},
    {3,118,2,1,"SMALL_SWITCH_ROOFS_COMPOUND_AREA51",false,45,-73,-42,0},
    {3,118,2,1,"SMALL_SWITCH_TOWER",false,80,-82,-43,0},
    {3,118,2,1,"SMALL_SWITCH_HOUSE",false,0,-80,-67,0},
    {2,92,1,0,"AIRLOCK_SWITCH",true,.15f,.9f,-79,117},
    {3,117,1,0,"AIRLOCK_SWITCH_ANTARC",true,.15f,.9f,-68,116},
    {3,117,1,0,"AIRLOCK_SWITCH_UNDERSEA",true,.15f,.9f,-68,116},
    {3,117,1,0,"AIRLOCK_SWITCH_SHORE",true,.9f,.75f,-2,112},
};
const SpecialSwitchProfile* SpecialSwitch(uint8_t* item) {
    if(!g_boundDll || !g_switchDll || !g_boundDll->objects) return nullptr;
    const int game=g_boundDll->module[4]-L'0',slot=*reinterpret_cast<int16_t*>(item+16);
    if((game==2 && slot!=92 && slot!=93) || (game==3 && slot!=117 && slot!=118) || (game!=2 && game!=3)) return nullptr;
    const auto* object=Ptr<uint8_t>(g_boundDll->objects)+slot*off::object_stride;
    const char* name=*reinterpret_cast<const char* const*>(object+off::object_geom);
    if(!name || *reinterpret_cast<const int32_t*>(object+off::object_geom+28) ||
       *reinterpret_cast<const uint64_t*>(object+64)!=g_boundBase+g_switchDll->collision ||
       *reinterpret_cast<const uint64_t*>(object+24)!=g_boundBase+g_switchDll->control) return nullptr;
    for(const auto& p:kSpecialSwitches) if(game==p.game && slot==p.slot && !std::strcmp(name,p.name) &&
        *reinterpret_cast<const int16_t*>(object)==p.joints) return &p;
    return nullptr;
}
bool SpecialGripPoint(uint8_t* item,motiongun::Vec hand,motiongun::Vec& local,motiongun::Vec& grip) {
    const auto* p=SpecialSwitch(item);if(!p) return false;
    const auto* object=Ptr<uint8_t>(g_boundDll->objects)+p->slot*off::object_stride;
    alignas(16) uint8_t probe[off::item_stride];std::memcpy(probe,item,sizeof(probe));
    SwitchPoseMatrices(probe,{*reinterpret_cast<int16_t*>(item+24),*reinterpret_cast<int16_t*>(item+26),0});
    auto frame=SwitchJoint(probe,p->joint);const auto& pos=*reinterpret_cast<PHD_3DPOS*>(item+off::item_pos);
    frame.origin=Add(frame.origin,{float(pos.x_pos),float(pos.y_pos),float(pos.z_pos)});
    motiongun::Frame inverse{},bind{},inverseBind{};
    if(!SwitchFinite(frame) || !motiongun::Inverse(frame,inverse) ||
       !ReadIKBind(object,p->joints,0,nullptr,nullptr,p->joint,bind) || !motiongun::Inverse(bind,inverseBind)) return false;
    const auto at=motiongun::Transform(bind,motiongun::Transform(inverse,hand));
    motiongun::Vec contact{std::clamp(at.x,-p->x,p->x),p->y,p->z};
    if(p->wheel) {
        const float x=at.x-p->x,y=at.y-p->y,r=std::hypot(x,y);
        if(!std::isfinite(r) || r<1) return false;
        contact={p->x+p->radius*x/r,p->y+p->radius*y/r,p->z};
    }
    local=motiongun::Transform(inverseBind,contact);grip=motiongun::Transform(frame,local);return true;
}
bool BuildAirlockPath(firstperson::SwitchPosePath& path,const firstperson::SwitchAnim* animations,
                      int base,firstperson::SwitchPose start) {
    // TR2: preview only the turn. Native control owns the following door/wheel
    // movement and deactivation. TR3 uses normal reversible switch transitions.
    path.count=0;if(!animations || base<0 || start.animation!=base || start.state!=0) return false;
    const auto& idle=animations[base];const auto& turn=animations[base+1];
    if(!idle.frames || !turn.frames || idle.state!=0 || idle.first!=idle.last || start.frame!=idle.first ||
       turn.state!=1 || turn.last-turn.first!=140 || turn.jump!=base+2 ||
       turn.velocity || turn.acceleration || (turn.interpolation&255)!=1) return false;
    path.poses[path.count++]=start;
    for(int f=turn.first;f<=turn.last;++f) path.poses[path.count++]={int16_t(base+1),int16_t(f),1};
    return true;
}
bool BuildAirlockTrajectory(uint8_t* item,motiongun::Vec local) {
    auto& p=g_switchPreview;alignas(16) uint8_t probe[off::item_stride];std::memcpy(probe,item,sizeof(probe));
    const auto& pos=*reinterpret_cast<PHD_3DPOS*>(item+off::item_pos);motiongun::Frame inverse{};float samples[256]{};
    for(int i=0;i<p.path.count;++i) {
        // TR3's final idle resets the wheel angle after the completed turn.
        // That reset belongs to native completion, not a backwards hand turn.
        if(g_boundDll->module[4]==L'3' && i==p.path.count-1) {
            p.points[i]=p.points[i-1];samples[i]=samples[i-1];break;
        }
        SwitchPoseMatrices(probe,p.path.poses[i]);auto frame=SwitchJoint(probe,0);
        frame.origin=Add(frame.origin,{float(pos.x_pos),float(pos.y_pos),float(pos.z_pos)});
        if(!SwitchFinite(frame)) return false;
        if(!i) {p.valveFrame=frame;if(!motiongun::Inverse(frame,inverse)) return false;}
        if(SwitchLength(Sub(frame.origin,p.valveFrame.origin))>8) return false;
        p.points[i]=motiongun::Transform(frame,local);const auto v=motiongun::Transform(inverse,p.points[i]);
        samples[i]=std::atan2(v.y,v.x);
    }
    p.valveLocal=local;return p.valveTurn.Build(samples,p.path.count);
}
bool RegripSpecialPreview(uint8_t* item,motiongun::Vec hand) {
    auto& p=g_switchPreview;motiongun::Vec local{},grip{};const float progress=p.valveTurn.progress;
    if(!SpecialGripPoint(item,hand,local,grip) || !BuildAirlockTrajectory(item,local)) return false;
    p.valveTurn.progress=progress;p.selected=p.valveTurn.Pose();p.grip=p.points[p.selected];
    motiongun::Frame inverse{};if(!motiongun::Inverse(p.valveFrame,inverse)) return false;
    const auto v=motiongun::Transform(inverse,p.grip);p.valveTurn.Grab(std::atan2(v.y,v.x));return true;
}
