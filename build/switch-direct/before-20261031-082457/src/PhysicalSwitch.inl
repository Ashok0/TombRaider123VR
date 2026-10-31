// Included inside FirstPerson.cpp's anonymous namespace. Only ordinary wall
// levers use this path; all switch activation, commands and level-specific
// callbacks remain in the verified native SwitchCollision handler.
struct SwitchDll {
    uint32_t timestamp,collision,testPosition,movePosition,bounds,getBounds,items,count;
    uint32_t animate,calcMatrices,changes,ranges,commands,control;
};
#include "SwitchAddresses.inl"
const SwitchDll* g_switchDll=nullptr;
float SwitchLength(motiongun::Vec v) { return std::sqrt(motiongun::Dot(v,v)); }
bool SwitchFinite(const motiongun::Frame& frame) {
    float rows[12];motiongun::WriteRows(frame,rows);
    for(float value:rows) if(!std::isfinite(value)) return false;
    return true;
}
hook::InlineHook g_hSwitchCollision,g_hSwitchTestPosition,g_hSwitchMovePosition;
using Fn_SwitchCollision=void (__cdecl*)(int16_t,uint8_t*,void*);
using Fn_SwitchPosition=int32_t (__cdecl*)(const void*,uint8_t*,uint8_t*);
using Fn_SwitchBounds=int16_t* (__cdecl*)(uint8_t*);
struct SwitchCandidate {
    int16_t index=-1,state=-1;
    int level=-1;
    uint8_t* items=nullptr;
    uint64_t seen=0;
    float distance=0;
};
SwitchCandidate g_switchCandidate{},g_switchGrab{};
bool g_switchCompleted=false;
bool g_switchGripDown=false,g_switchGripClaimed=false;
uint8_t* g_switchCommit=nullptr;
void RestoreSwitchPreview();

void ResetPhysicalSwitch() {
    RestoreSwitchPreview();
    g_switchCandidate={};g_switchGrab={};g_switchCompleted=false;
    g_switchGripDown=g_switchGripClaimed=false;
}

bool PhysicalSwitchReady() {
    return g_switchDll && g_active && g_scenePoseValid && Gate() &&
        GameDllBound()==g_boundDll && GameDllBase()==g_boundBase &&
        Cfg().positionalTracking && Cfg().firstPersonHeadTranslation &&
        LaraWaterStatus()==0 && *Ptr<int16_t>(g_boundDll->lara+2)==0 &&
        !InteractionAligning() && VR().poseValid();
}

bool SwitchHand(motiongun::Vec& world) {
    vr::HmdMatrix34_t hand{};
    float right,down,forward;
    if (!VR().ControllerPose(1,hand) || !VR().poseValid() ||
        !VR().FirstPersonControllerOffset(1,right,down,forward)) return false;
    const float scale=LiveWorldUnitsPerMetre();
    if (!std::isfinite(scale) || scale<=1 || !std::isfinite(right+down+forward) ||
        std::fabs(right)>2 || std::fabs(down)>2 || std::fabs(forward)>2) return false;
    world=motiongun::HandInWorld({float(g_scenePose.x_pos),float(g_scenePose.y_pos),float(g_scenePose.z_pos)},
        right,down,forward,g_heading.base,scale);
    return std::isfinite(hand.m[1][3]);
}

uint8_t* SwitchItem(const SwitchCandidate& candidate) {
    if (!g_switchDll || !g_boundDll || !g_boundBase || candidate.index<0 ||
        GameDllBound()!=g_boundDll || GameDllBase()!=g_boundBase ||
        candidate.level!=AppFlag(drva::app_off::level) ||
        candidate.items!=*Ptr<uint8_t*>(g_switchDll->items) ||
        candidate.index>=*Ptr<int32_t>(g_switchDll->count)) return nullptr;
    return candidate.items ? candidate.items+candidate.index*off::item_stride : nullptr;
}

struct SwitchPreview {
    bool active=false,attached=false;
    firstperson::SwitchPosePath path{};
    motiongun::Vec points[256]{},grip{},lastHand{};
    int selected=0,joint=0;
    uint64_t lastTime=0;
    int16_t animation=0,frame=0;
    int32_t onScreen=0;
    int32_t savedMatrices[792]{};
} g_switchPreview;
using Fn_SwitchItem=void (__cdecl*)(uint8_t*);
using Fn_SwitchControl=void (__cdecl*)(int16_t);

void RestoreSwitchPreview() {
    if(g_switchPreview.active) if(auto* item=SwitchItem(g_switchGrab)) {
        *reinterpret_cast<int16_t*>(item+24)=g_switchPreview.animation;
        *reinterpret_cast<int16_t*>(item+26)=g_switchPreview.frame;
        *reinterpret_cast<int32_t*>(item+488)=g_switchPreview.onScreen;
        std::memcpy(item+off::item_joints_prev,g_switchPreview.savedMatrices,sizeof(g_switchPreview.savedMatrices));
    }
    g_switchPreview.active=g_switchPreview.attached=false;
}

// Build matrices on a private item: no AnimateItem, sounds, commands, or triggers.
bool SwitchPoseMatrices(uint8_t* probe,firstperson::SwitchPose pose) {
    *reinterpret_cast<int16_t*>(probe+24)=pose.animation;
    *reinterpret_cast<int16_t*>(probe+26)=pose.frame;
    // CalcItemMatrices pushes the native matrix stack; preserve its caller.
    auto& matrixPointer=*Ptr<int32_t*>(g_boundDll->phdMxptr);
    auto* savedPointer=matrixPointer;
    reinterpret_cast<Fn_SwitchItem>(g_boundBase+g_switchDll->calcMatrices)(probe);
    matrixPointer=savedPointer;
    return true;
}

motiongun::Frame SwitchJoint(const uint8_t* item,int joint) {
    const auto* fixed=reinterpret_cast<const int32_t*>(item+off::item_joints_cur)+joint*12;
    float rows[12];
    for(int i=0;i<12;++i) rows[i]=fixed[i]/16384.f;
    return motiongun::ReadRows(rows);
}

int WallSwitchObject() {
    return g_boundDll->module[4]==L'1' ? 55 : g_boundDll->module[4]==L'2' ? 104 : 129;
}

bool BeginSwitchPreview(uint8_t* item,motiongun::Vec hand,uint64_t now) {
    using namespace motiongun;
    if(!g_boundDll->anims || !g_switchDll->calcMatrices || !g_switchDll->animate ||
       !g_switchDll->changes || !g_switchDll->ranges || !g_switchDll->commands) return false;
    auto& p=g_switchPreview;
    const int object=*reinterpret_cast<int16_t*>(item+16);
    if(object<0) return false;
    const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
    if(*reinterpret_cast<Fn_SwitchControl const*>(obj+24)!=
        reinterpret_cast<Fn_SwitchControl>(g_boundBase+g_switchDll->control)) return false;
    // Independent HD animation skeletons need their own grip-path sampling.
    if(*reinterpret_cast<const int32_t*>(obj+off::object_geom+28)>0) return false;
    const int joints=*reinterpret_cast<const int16_t*>(obj);
    if(joints<1 || joints>33) return false;
    const int state=*reinterpret_cast<int16_t*>(item+18);
    p.animation=*reinterpret_cast<int16_t*>(item+24);p.frame=*reinterpret_cast<int16_t*>(item+26);
    if(!p.path.Build(*Ptr<firstperson::SwitchAnim*>(g_boundDll->anims),
        *Ptr<firstperson::SwitchChange*>(g_switchDll->changes),*Ptr<firstperson::SwitchRange*>(g_switchDll->ranges),
        *Ptr<int16_t*>(g_switchDll->commands),{p.animation,p.frame,int16_t(state)},1-state)) return false;
    alignas(16) uint8_t probe[off::item_stride];std::memcpy(probe,item,sizeof(probe));
    const auto& pos=*reinterpret_cast<PHD_3DPOS*>(item+off::item_pos);
    const Vec origin{float(pos.x_pos),float(pos.y_pos),float(pos.z_pos)};
    SwitchPoseMatrices(probe,p.path.poses[0]);
    Frame start[33]{};
    for(int j=0;j<joints;++j) start[j]=SwitchJoint(probe,j);
    SwitchPoseMatrices(probe,p.path.poses[p.path.count-1]);
    float best=0;Vec local{};p.joint=-1;
    const float scale=LiveWorldUnitsPerMetre();
    for(int j=0;j<joints;++j) {
        Frame inverse{};if(!Inverse(start[j],inverse)) continue;
        const auto end=SwitchJoint(probe,j);
        const Vec at=Add(inverse.origin,Transform(inverse.basis,Sub(hand,origin)));
        const Vec delta=Sub(Add(end.origin,Transform(end.basis,at)),Sub(hand,origin));
        const float travel=SwitchLength(delta);
        if(!SwitchFinite(end) || travel<scale*.04f || travel>scale*.8f ||
           delta.y*(state==1 ? 1.f : -1.f)<scale*.03f) continue;
        const float score=travel/(1+SwitchLength(Sub(Sub(hand,origin),start[j].origin))/scale);
        if(score>best) { best=score;p.joint=j;local=at; }
    }
    if(p.joint<0) return false;
    for(int i=0;i<p.path.count;++i) {
        SwitchPoseMatrices(probe,p.path.poses[i]);
        const auto frame=SwitchJoint(probe,p.joint);
        p.points[i]=Add(origin,Add(frame.origin,Transform(frame.basis,local)));
        if(!SwitchFinite(frame) || SwitchLength(Sub(p.points[i],hand))>scale) return false;
    }
    p.onScreen=*reinterpret_cast<int32_t*>(item+488);
    std::memcpy(p.savedMatrices,item+off::item_joints_prev,sizeof(p.savedMatrices));
    p.active=p.attached=true;p.selected=0;p.grip=p.points[0];p.lastHand=hand;p.lastTime=now;
    return true;
}

void AttachPhysicalSwitchHand(int hand,motiongun::Frame& wrist,motiongun::Vec trackedGrip) {
    if(hand==1 && g_switchGripClaimed && g_switchPreview.attached && SwitchItem(g_switchGrab) && PhysicalSwitchReady())
        wrist.origin=Add(wrist.origin,Sub(g_switchPreview.grip,trackedGrip));
}

bool UpdateSwitchPreview(uint8_t* item,motiongun::Vec hand,uint64_t now) {
    auto& p=g_switchPreview;
    if(!p.active) return false;
    const float scale=LiveWorldUnitsPerMetre();
    if(now<p.lastTime || now-p.lastTime>250 || SwitchLength(Sub(hand,p.lastHand))>scale*.4f) {
        RestoreSwitchPreview();g_switchGripClaimed=false;return false;
    }
    p.lastTime=now;p.lastHand=hand;
    float nearest=1.e30f;int selected=p.selected;
    for(int i=0;i<p.path.count;++i) {
        const auto delta=Sub(hand,p.points[i]);const float distance=motiongun::Dot(delta,delta);
        // Prefer the earlier pose where the authored clip has stationary frames.
        if(distance<nearest-.01f) { nearest=distance;selected=i; }
    }
    if(nearest>scale*scale*.25f*.25f) { RestoreSwitchPreview();g_switchGripClaimed=false;return false; }
    p.selected=selected;p.grip=p.points[selected];
    SwitchPoseMatrices(item,p.path.poses[selected]);
    std::memcpy(item+off::item_joints_prev,item+off::item_joints_cur,1584); // no automatic interpolation away from the grabbed pose
    // Completion is geometric, not elapsed playback time. Stationary tail poses
    // must not prevent reaching the endpoint or cause a second activation.
    const auto end=p.points[p.path.count-1];
    return selected>0 && SwitchLength(Sub(p.grip,end))<=scale*.008f &&
        SwitchLength(Sub(hand,p.points[0]))>=scale*.03f;
}

// The tracked hand owns reach, independently of the native Action-icon zone.
// Keep Lara on the usable side and floor, within a physical arm's reach.
bool SwitchReach(uint8_t* target,uint8_t* lara,motiongun::Vec hand,float& distance,bool grabbed) {
    const int object=*reinterpret_cast<const int16_t*>(target+16);
    const int state=*reinterpret_cast<const int16_t*>(target+off::item_anim_state);
    if (object!=WallSwitchObject() || (state!=0 && state!=1) ||
        (*reinterpret_cast<const uint16_t*>(target+off::item_flags)&6) ||
        (*reinterpret_cast<const uint16_t*>(target+44)&0x100) ||
        *reinterpret_cast<const int16_t*>(lara+off::item_anim_state)!=2 ||
        *reinterpret_cast<const int16_t*>(lara+off::item_anim_number)!=11 ||
        (*reinterpret_cast<const uint16_t*>(lara+off::item_flags)&8)) return false;
    const auto& pos=*reinterpret_cast<const PHD_3DPOS*>(target+off::item_pos);
    if (pos.x_rot || pos.z_rot) return false;
    const int16_t savedAnimation=*reinterpret_cast<int16_t*>(target+24),savedFrame=*reinterpret_cast<int16_t*>(target+26);
    if(grabbed && g_switchPreview.active) {
        *reinterpret_cast<int16_t*>(target+24)=g_switchPreview.animation;
        *reinterpret_cast<int16_t*>(target+26)=g_switchPreview.frame;
    }
    const auto* nativePointer=reinterpret_cast<Fn_SwitchBounds>(g_boundBase+g_switchDll->getBounds)(target);
    int16_t nativeBounds[6]{};
    if(nativePointer) std::memcpy(nativeBounds,nativePointer,sizeof(nativeBounds));
    *reinterpret_cast<int16_t*>(target+24)=savedAnimation;*reinterpret_cast<int16_t*>(target+26)=savedFrame;
    if (!nativePointer) return false;
    const auto* bounds=Ptr<int16_t>(g_switchDll->bounds);
    for (int i=0;i<3;++i) if (nativeBounds[i*2]>nativeBounds[i*2+1] ||
        nativeBounds[i*2]<-4096 || nativeBounds[i*2+1]>4096) return false;
    const auto& body=*reinterpret_cast<const PHD_3DPOS*>(lara+off::item_pos);
    const float height=float(body.y_pos)-float(pos.y_pos);
    if (height<bounds[2] || height>bounds[3]) return false;
    const auto local=locomotion::Rotate({hand.x-pos.x_pos,hand.z-pos.z_pos},-locomotion::Radians(pos.y_rot));
    const auto bodyLocal=locomotion::Rotate({float(body.x_pos)-float(pos.x_pos),float(body.z_pos)-float(pos.z_pos)},-locomotion::Radians(pos.y_rot));
    if (bodyLocal.z>nativeBounds[4] || local.z>nativeBounds[5]+16) return false;
    const float scale=LiveWorldUnitsPerMetre();
    const float dx=bodyLocal.x-std::clamp(bodyLocal.x,float(nativeBounds[0]),float(nativeBounds[1]));
    const float dz=bodyLocal.z-std::clamp(bodyLocal.z,float(nativeBounds[4]),float(nativeBounds[5]));
    if (!std::isfinite(scale) || scale<=1 || dx*dx+dz*dz>scale*scale*.9f*.9f) return false;
    const float p[3]={local.x,hand.y-pos.y_pos,local.z};distance=0;
    for(int i=0;i<3;++i) {
        const float d=p[i]-std::clamp(p[i],float(nativeBounds[2*i]),float(nativeBounds[2*i+1]));
        distance+=d*d;
    }
    const float radius=scale*(grabbed ? .40f : .18f);
    return std::isfinite(distance) && distance<=radius*radius;
}

int32_t __cdecl Detour_SwitchTestPosition(const void* bounds,uint8_t* target,uint8_t* lara) {
    if (g_switchCommit && target==g_switchCommit && lara==*Ptr<uint8_t*>(g_boundDll->laraItem)) return 1;
    return g_hSwitchTestPosition.Original<Fn_SwitchPosition>()(bounds,target,lara);
}
int32_t __cdecl Detour_SwitchMovePosition(const void* offset,uint8_t* target,uint8_t* lara) {
    if (g_switchCommit && target==g_switchCommit && lara==*Ptr<uint8_t*>(g_boundDll->laraItem)) return 1;
    return g_hSwitchMovePosition.Original<Fn_SwitchPosition>()(offset,target,lara);
}

void CommitPhysicalSwitch(int16_t index,uint8_t* target,uint8_t* lara,void* collision) {
    // Native SwitchCollision owns the lever, active-item list, sounds and
    // special level callbacks. Only its temporary Lara animation/auto-align
    // changes are suppressed; do not emulate door flags or trigger timing.
    const int steps=g_switchPreview.path.count-1;
    const auto grip=g_switchPreview.grip;
    RestoreSwitchPreview();
    uint8_t animation[10],angles[12],position[40];
    std::memcpy(position,lara+off::item_pos,sizeof(position));
    const auto general=*Ptr<int16_t>(g_boundDll->lara+58);
    std::memcpy(animation,lara+18,sizeof(animation));
    std::memcpy(angles,Ptr<uint8_t>(g_boundDll->lara+256),sizeof(angles));
    const auto status=*Ptr<int16_t>(g_boundDll->lara+2);
    const auto flags=*Ptr<uint16_t>(g_boundDll->lara+60);
    auto& input=*Ptr<uint32_t>(g_boundDll->input);
    const auto savedInput=input;
    g_switchCommit=target;input|=0x40;
    g_hSwitchCollision.Original<Fn_SwitchCollision>()(index,lara,collision);
    g_switchCommit=nullptr;input=savedInput;
    std::memcpy(lara+18,animation,sizeof(animation));
    std::memcpy(lara+off::item_pos,position,sizeof(position));
    *Ptr<int16_t>(g_boundDll->lara+58)=general;
    std::memcpy(Ptr<uint8_t>(g_boundDll->lara+256),angles,sizeof(angles));
    *Ptr<int16_t>(g_boundDll->lara+2)=status;
    *Ptr<uint16_t>(g_boundDll->lara+60)=flags;
    // The hand has already traversed every visible pose. Execute the native
    // completion commands once in this tick, without timed visual playback.
    for(int i=1;i<steps && (*reinterpret_cast<uint16_t*>(target+off::item_flags)&6)==2;++i) {
        if(i==1) {
            // Initialize native switch code bits/timer handling once. Repeating
            // control would incorrectly consume the timer for every pose.
            const int object=*reinterpret_cast<int16_t*>(target+16);
            const auto* obj=Ptr<uint8_t>(g_boundDll->objects)+object*off::object_stride;
            (*reinterpret_cast<Fn_SwitchControl const*>(obj+24))(index);
        } else reinterpret_cast<Fn_SwitchItem>(g_boundBase+g_switchDll->animate)(target);
    }
    SwitchPoseMatrices(target,{*reinterpret_cast<int16_t*>(target+24),*reinterpret_cast<int16_t*>(target+26),
        *reinterpret_cast<int16_t*>(target+18)});
    std::memcpy(target+off::item_joints_prev,target+off::item_joints_cur,1584);
    g_switchPreview.grip=grip;g_switchPreview.attached=true;
    LogF("vr-switch: pull committed item=%d state=%d goal=%d status=%u; native activation, FP retained",
        index,*reinterpret_cast<int16_t*>(target+18),*reinterpret_cast<int16_t*>(target+20),
        (*reinterpret_cast<uint16_t*>(target+off::item_flags)>>1)&3);
}

void __cdecl Detour_PhysicalSwitchCollision(int16_t index,uint8_t* lara,void* collision) {
    auto original=g_hSwitchCollision.Original<Fn_SwitchCollision>();
    if (!PhysicalSwitchReady() || lara!=*Ptr<uint8_t*>(g_boundDll->laraItem)) {
        RestoreSwitchPreview();
        g_switchCandidate={};g_switchCompleted=false;g_switchGripClaimed=false;
        original(index,lara,collision);return;
    }
    const uint64_t now=GetTickCount64();
    SwitchCandidate candidate{index,-1,AppFlag(drva::app_off::level),*Ptr<uint8_t*>(g_switchDll->items),now,0};
    auto* target=SwitchItem(candidate);
    motiongun::Vec hand{};float distance=0;
    const bool selected=g_switchGripClaimed && index==g_switchGrab.index && SwitchItem(g_switchGrab)==target;
    const bool inReach=target && SwitchHand(hand) && SwitchReach(target,lara,hand,distance,selected);
    if (inReach && !g_switchGripDown &&
        (g_switchCandidate.seen!=now || distance<g_switchCandidate.distance)) {
        candidate.state=*reinterpret_cast<int16_t*>(target+18);candidate.distance=distance;
        g_switchCandidate=candidate;
    }
    if (selected) {
        // Bounds and eligibility use the original idle animation, not the
        // arbitrary pose currently displayed by the tracked grab.
        const bool valid=inReach && *reinterpret_cast<int16_t*>(target+18)==g_switchGrab.state;
        if(!g_switchCompleted) {
            if(!valid || !g_switchGripDown) {
                RestoreSwitchPreview();g_switchGripClaimed=false;
            } else if(UpdateSwitchPreview(target,hand,now)) {
                g_switchCompleted=true;
                CommitPhysicalSwitch(index,target,lara,collision);return;
            }
        }
        // Continue native solid collision/indicator drawing without allowing
        // another Action binding to start a canned animation during this grab.
        auto& input=*Ptr<uint32_t>(g_boundDll->input);const auto saved=input;
        input&=~uint32_t(0x40);original(index,lara,collision);input=saved;return;
    }
    original(index,lara,collision);
}

bool UpdatePhysicalSwitchGrip(bool down,bool allowed,uint64_t now) {
    const bool pressed=down && !g_switchGripDown;g_switchGripDown=down;
    if (!down || !allowed || !PhysicalSwitchReady()) {
        RestoreSwitchPreview();
        g_switchGrab={};g_switchCompleted=false;g_switchGripClaimed=false;
        if (!allowed || !PhysicalSwitchReady()) g_switchCandidate={};
        return false;
    }
    if (g_switchGripClaimed && (!SwitchItem(g_switchGrab) ||
        (!g_switchCompleted && (now<g_switchPreview.lastTime || now-g_switchPreview.lastTime>250)))) {
        RestoreSwitchPreview();
        g_switchCompleted=false;g_switchGripClaimed=false;
    }
    if (pressed && g_switchCandidate.index>=0 && now>=g_switchCandidate.seen && now-g_switchCandidate.seen<=100) {
        auto* target=SwitchItem(g_switchCandidate);
        auto* lara=*Ptr<uint8_t*>(g_boundDll->laraItem);
        motiongun::Vec hand{};float distance=0;
        if (target && SwitchHand(hand) && SwitchReach(target,lara,hand,distance,false) &&
            BeginSwitchPreview(target,hand,now)) {
            g_switchGrab=g_switchCandidate;g_switchGrab.state=*reinterpret_cast<int16_t*>(target+18);
            g_switchGripClaimed=true;
            LogF("vr-switch: attached item=%d joint=%d poses=%d; hand drives lever until end stop",
                g_switchGrab.index,g_switchPreview.joint,g_switchPreview.path.count);
        }
    }
    return g_switchGripClaimed;
}

void RemovePhysicalSwitches() {
    ResetPhysicalSwitch();g_switchCommit=nullptr;
    g_hSwitchCollision.Remove();g_hSwitchMovePosition.Remove();g_hSwitchTestPosition.Remove();g_switchDll=nullptr;
}
void InstallPhysicalSwitches(const GameDllLayout& d,uint64_t base) {
    for (const auto& row:kSwitchDlls) if (row.timestamp==d.timestamp) g_switchDll=&row;
    if (!g_switchDll) return;
    const int game=d.module[4]-L'1';
    const uint8_t animate[3][8]={{0x40,0x53,0x41,0x54,0x48,0x83,0xEC,0x38},
        {0x40,0x53,0x48,0x83,0xEC,0x40,0x48,0x89},
        {0x40,0x53,0x41,uint8_t(d.timestamp==0x6A4B490D ? 0x54 : 0x55),0x48,0x83,0xEC,0x48}};
    const uint8_t calc[3][8]={{0x48,0x89,0x4C,0x24,0x08,0x55,0x41,0x55},
        {0x48,0x89,0x6C,0x24,0x20,0x56,0x41,0x54},
        {0x48,0x89,0x4C,0x24,0x08,0x53,0x55,0x56}};
    if(std::memcmp(reinterpret_cast<void*>(base+g_switchDll->animate),animate[game],8) ||
       std::memcmp(reinterpret_cast<void*>(base+g_switchDll->calcMatrices),calc[game],8)) {
        g_switchDll=nullptr;Log("vr-switch: pose/completion byte checks failed; native interaction retained");return;
    }
    const uint8_t test[]={0x48,0x89,0x6C,0x24,uint8_t(game==2 ? 0x10 : 0x20)};
    const uint8_t move[]={0x48,0x89,0x5C,0x24,uint8_t(game==0 ? 0x08 : 0x10)};
    const uint8_t collide[]={0x48,0x89,0x5C,0x24,0x10};
    if (!g_hSwitchTestPosition.Install(reinterpret_cast<void*>(base+g_switchDll->testPosition),
            reinterpret_cast<void*>(&Detour_SwitchTestPosition),sizeof(test),test,sizeof(test),"VRSwitchTestPosition") ||
        !g_hSwitchMovePosition.Install(reinterpret_cast<void*>(base+g_switchDll->movePosition),
            reinterpret_cast<void*>(&Detour_SwitchMovePosition),sizeof(move),move,sizeof(move),"VRSwitchMovePosition") ||
        !g_hSwitchCollision.Install(reinterpret_cast<void*>(base+g_switchDll->collision),
            reinterpret_cast<void*>(&Detour_PhysicalSwitchCollision),sizeof(collide),collide,sizeof(collide),"VRWallSwitch")) {
        RemovePhysicalSwitches();Log("vr-switch: hook byte checks failed; native interaction retained");return;
    }
    Log("vr-switch: hand-attached wall levers enabled; pose follows grip, native completion at end stop");
}
