#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include "MotionGunMath.h"

namespace tr::worldlaser {
// Read-only native laser output. Verified by tools/verify_world_laser.py.
struct Build { uint32_t stamp, sight, active, light, sightCheck, activeCheck, lightCheck, los, objects, losCall, objectsCall; };
constexpr Build builds[]={
    {0x696B4999,0x1BB248,0x1BB14E,0x510960,0x158D9,0x16086,0x16095,0x149F0,0x155D0,0x158B3,0x1593F},
    {0x696B499C,0x1B6C94,0x1B6B8A,0x50DC40,0x13372,0x1400C,0x1410C,0x12450,0x13030,0x13341,0x1342D},
    {0x68C12FDA,0x1BC188,0x1BC08E,0x5118A0,0x154B9,0x15C78,0x15C87,0x145D0,0x151B0,0x15493,0x15521},
    {0x68C12FE9,0x1B6BD4,0x1B6ACA,0x50DB80,0x13422,0x140D0,0x141D0,0x12500,0x130E0,0x133F1,0x134DB}
};
inline bool Verify(const uint8_t* base,const Build& b) {
    const auto* sight=base+b.sightCheck;
    const auto* active=base+b.activeCheck;
    const auto* light=base+b.lightCheck;
    auto target=[](uint32_t at,const uint8_t* p,int displacement,int size) {
        int32_t offset=0;std::memcpy(&offset,p+displacement,4);
        return int64_t(at)+size+offset;
    };
    return base[b.losCall]==0xe8 && target(b.losCall,base+b.losCall,1,5)==b.los &&
        base[b.objectsCall]==0xe8 && target(b.objectsCall,base+b.objectsCall,1,5)==b.objects &&
        sight[0]==0x44 && sight[1]==0x39 && (sight[2]==0x35 || sight[2]==0x3d) &&
        target(b.sightCheck,sight,3,7)==b.sight &&
        active[0]==0xc6 && active[1]==0x05 && active[6]==1 &&
        target(b.activeCheck,active,2,7)==b.active &&
        light[0]==0x66 && light[1]==0xc7 && light[2]==0x05 && light[7]==1 && light[8]==2 &&
        target(b.lightCheck,light,3,9)==b.light;
}
struct Ray { motiongun::Vec start,end; };
struct Point { int32_t x,y,z;int16_t room,padding; };
// Query room/static/object geometry, never GetTargetOnLOS (which also fires,
// damages targets, updates scope lights and handles ammo).
template<class Floor,class LOS,class Objects>
bool Trace(motiongun::Vec muzzle,motiongun::Vec direction,int16_t room,Ray& ray,
           Floor floor,LOS los,Objects objects) {
    using namespace motiongun;
    const float length=std::sqrt(Dot(direction,direction));
    if(!std::isfinite(length) || length<.001f) return false;
    const auto distant=Add(muzzle,Scale(direction,20000.f/length));
    for(float v:{muzzle.x,muzzle.y,muzzle.z,distant.x,distant.y,distant.z})
        if(!std::isfinite(v) || std::fabs(v)>1e7f) return false;
    Point start{int32_t(std::lround(muzzle.x)),int32_t(std::lround(muzzle.y)),int32_t(std::lround(muzzle.z)),room,0};
    floor(start.x,start.y,start.z,&start.room);
    Point end{int32_t(std::lround(distant.x)),int32_t(std::lround(distant.y)),int32_t(std::lround(distant.z)),start.room,0};
    los(&start,&end); // Also builds the native room list consumed by ObjectOnLOS2.
    Point hit{};void* mesh=nullptr;
    const int object=objects(&start,&end,&hit,&mesh);
    ray={muzzle,{float(end.x),float(end.y),float(end.z)}};
    if(object!=999) {
        const Vec objectEnd{float(hit.x),float(hit.y),float(hit.z)};
        const auto delta=Sub(objectEnd,muzzle),clipped=Sub(ray.end,muzzle);
        if(Dot(delta,direction)>=0 && Dot(delta,delta)<=Dot(clipped,clipped)) ray.end=objectEnd;
    }
    return Dot(Sub(ray.end,muzzle),Sub(ray.end,muzzle))>1;
}
inline bool Read(const uint8_t* base,const Build& b,Ray& ray) {
    int32_t sight=0;std::memcpy(&sight,base+b.sight,4);
    if(!sight || !base[b.active]) return false;
    // GetTargetOnLOS writes HD_SpotLight from its final object/room hit,
    // not the unmodified camera.target argument. No extra raycast or shot.
    std::memcpy(&ray.start,base+b.light+0x14,12);
    std::memcpy(&ray.end,base+b.light+0x2c,12);
    const float values[]={ray.start.x,ray.start.y,ray.start.z,ray.end.x,ray.end.y,ray.end.z};
    for(float v:values) if(!std::isfinite(v) || std::fabs(v)>1e7f) return false;
    const auto d=motiongun::Sub(ray.end,ray.start);
    const float length2=motiongun::Dot(d,d);
    return length2>1 && length2<65536.f*65536.f;
}
}
