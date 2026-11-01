// Shared by TR2/3 and TR4/5. Only the version-verified native undraw routine
// owns allocation, burn age, hand mesh removal, weapon selection and cleanup.
struct FlareThrowDll {
    uint32_t timestamp;int game;
    uint32_t undraw,undrawSize;uint64_t undrawHash;
    uint32_t create,createSize;uint64_t createHash;
    uint32_t nextFree,items;int16_t object;
};
constexpr FlareThrowDll kFlareThrowDlls[]={
#include "FlareThrowAddresses.inl"
};
const FlareThrowDll* g_flareThrowDll=nullptr;
flarethrow::Gesture g_flareGesture;
struct PhysicalFlareRelease {
    uint8_t* lara=nullptr;
    uint8_t* items=nullptr;
    uint64_t base=0;
    int level=-1;
    double time=0;
    int32_t point[3]{};
    flarethrow::Launch launch{};
} g_flareRelease;
const PhysicalFlareRelease* g_flareSpawn=nullptr;
bool g_flareSpawnSeen=false;

void ResetPhysicalFlareThrow() {g_flareGesture.Cancel();g_flareRelease={};}

void InstallPhysicalFlareThrow(uint32_t timestamp,bool hooks) {
    ResetPhysicalFlareThrow();g_flareThrowDll=nullptr;
    if(!hooks) return;
    for(const auto& d:kFlareThrowDlls) if(d.timestamp==timestamp &&
        flarethrow::Fingerprint(Ptr<uint8_t>(d.undraw),d.undrawSize)==d.undrawHash &&
        flarethrow::Fingerprint(Ptr<uint8_t>(d.create),d.createSize)==d.createHash) {
        g_flareThrowDll=&d;
        Log("firstperson: automatic left-controller swing flare throw ready");return;
    }
    Log("firstperson: physical flare release unavailable; native throw retained");
}

bool PhysicalFlareSpawnPoint(uint8_t* item,int32_t* point,int32_t joint) {
    if(!g_flareSpawn || g_flareSpawnSeen || item!=g_flareSpawn->lara || !point || joint!=13 ||
       point[0]!=-16 || point[1]!=32 || point[2]!=42) return false;
    std::memcpy(point,g_flareSpawn->point,sizeof(g_flareSpawn->point));
    g_flareSpawnSeen=true;return true;
}

void PhysicalFlareMotion(bool allowed) {
    const bool ready=allowed && g_flareThrowDll && FlareIKReady();
    vr::HmdMatrix34_t tracked{},head{};motiongun::Frame wrist{};motiongun::Basis basis{};
    const bool valid=ready && VR().ControllerPose(0,tracked) && VR().HeadPose(head) && BuildControllerWrist(0,wrist,basis);
    auto* item=valid ? *Ptr<uint8_t*>(g_boundDll->laraItem) : nullptr;
    auto* items=valid ? *Ptr<uint8_t*>(g_flareThrowDll->items) : nullptr;
    const int level=valid ? FlareLevel() : -1;
    if(!valid || !items || (g_flareRelease.lara && (item!=g_flareRelease.lara || items!=g_flareRelease.items ||
       level!=g_flareRelease.level || g_boundBase!=g_flareRelease.base))) {
        ResetPhysicalFlareThrow();return;
    }
    if(g_flareRelease.time) return; // keep the captured release until simulation consumes it
    g_flareRelease.lara=item;g_flareRelease.items=items;
    g_flareRelease.level=level;g_flareRelease.base=g_boundBase;
    motiongun::Vec velocity{};
    const float yaw=VR().HeadYawRadians();
    const auto event=g_flareGesture.Update(true,VR().PoseSerial(),TurnTime(),
        {tracked.m[0][3],tracked.m[1][3],tracked.m[2][3]},
        {head.m[0][3],head.m[1][3],head.m[2][3]}, {std::sin(yaw),0,-std::cos(yaw)},velocity);
    if(event!=flarethrow::Event::Release) return;
    const auto point=motiongun::Transform(wrist,{-16,32,42});
    if(!flarethrow::Finite(point) || std::fabs(point.x)>16000000 || std::fabs(point.y)>16000000 ||
       std::fabs(point.z)>16000000 ||
       !flarethrow::LaunchVelocity(velocity,FlareHeading(),LiveWorldUnitsPerMetre(),g_flareRelease.launch)) {
        ResetPhysicalFlareThrow();return;
    }
    g_flareRelease.point[0]=int32_t(std::lround(point.x));
    g_flareRelease.point[1]=int32_t(std::lround(point.y));
    g_flareRelease.point[2]=int32_t(std::lround(point.z));
    g_flareRelease.time=TurnTime();
    Log("firstperson flare throw: swing detected; release queued");
}

void CommitPhysicalFlareThrow() {
    if(!g_flareRelease.time) return;
    const auto release=g_flareRelease;g_flareRelease={}; // consume exactly once
    const double elapsed=TurnTime()-release.time;
    if(!g_flareThrowDll || elapsed<0 || elapsed>.15 || !FlareIKReady() ||
       release.base!=g_boundBase || release.level!=FlareLevel() ||
       release.lara!=*Ptr<uint8_t*>(g_boundDll->laraItem) ||
       release.items!=*Ptr<uint8_t*>(g_flareThrowDll->items)) {
        Log("firstperson flare throw: release cancelled after player/tracking state changed");return;
    }
    auto* lara=release.lara;
    const auto& d=*g_flareThrowDll;
    const int16_t index=*Ptr<int16_t>(d.nextFree);
    if(!release.items || index<0 || index>=1024 || !FlareLOS()) {
        Log("firstperson flare throw: release cancelled; native allocation/LOS unavailable");return;
    }
    const auto& body=*reinterpret_cast<const PHD_3DPOS*>(lara+off::item_pos);
    // Trace from inside Lara, not from an HMD that could lean through a wall.
    ShotVector start{body.x_pos,body.y_pos-256,body.z_pos,*reinterpret_cast<int16_t*>(lara+28),0};
    ShotVector end{release.point[0],release.point[1],release.point[2],start.room,0};
    const motiongun::Vec reach{float(end.x-start.x),float(end.y-start.y),float(end.z-start.z)};
    const float limit=LiveWorldUnitsPerMetre()*3.f; // includes a fully raised overhand release
    if(motiongun::Dot(reach,reach)>limit*limit) {
        Log("firstperson flare throw: hand outside tracked reach");return;
    }
    using Floor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
    if(!reinterpret_cast<Floor>(g_boundBase+g_boundDll->getFloor)(start.x,start.y,start.z,&start.room)) {
        Log("firstperson flare throw: native start room unavailable");return;
    }
    using LOS=int32_t (__cdecl*)(ShotVector*,ShotVector*);
    if(!reinterpret_cast<LOS>(g_boundBase+FlareLOS())(&start,&end)) {
        Log("firstperson flare throw: hand blocked by room geometry");return;
    }

    // Skip authored windup/recovery, but execute the two native event frames.
    // The temporary goal avoids undraw_flare's full-body animation 189 branch.
    // Restore all body animation fields before returning to native simulation.
    uint8_t savedAnimation[10];std::memcpy(savedAnimation,lara+18,sizeof(savedAnimation));
    *reinterpret_cast<int16_t*>(lara+20)=0;
    auto& frame=*Ptr<int16_t>(g_boundDll->lara+(d.game<=3 ? 0x118 : 0xF8));
    *Ptr<int16_t>(g_boundDll->lara+2)=3;
    const auto undraw=reinterpret_cast<void (__cdecl*)()>(g_boundBase+d.undraw);
    g_flareSpawn=&release;g_flareSpawnSeen=false;
    frame=20;undraw();
    g_flareSpawn=nullptr;
    auto* flare=release.items+index*off::item_stride;
    if(g_flareSpawnSeen && *Ptr<int16_t>(d.nextFree)!=index &&
       *reinterpret_cast<int16_t*>(flare+16)==d.object &&
       !std::memcmp(flare+off::item_pos,release.point,sizeof(release.point))) {
        // Native CreateFlare has already chosen the room and initialized both
        // poses. Preserve its fallback if it rejected a blocked hand position.
        reinterpret_cast<PHD_3DPOS*>(flare+off::item_pos)->y_rot=release.launch.yaw;
        *reinterpret_cast<int16_t*>(flare+34)=release.launch.speed;
        *reinterpret_cast<int16_t*>(flare+36)=release.launch.fall;
        Log("firstperson flare throw: native flare launched from controller");
    } else Log("firstperson flare throw: native placement fallback retained");
    frame=32;undraw();
    std::memcpy(lara+18,savedAnimation,sizeof(savedAnimation));
}
