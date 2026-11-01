#pragma once
#include "MotionGunMath.h"
#include <cstring>

namespace tr::firstperson {
struct ButtonSurface { float left,right,top,bottom,z; };
struct ButtonProfile { int game; const char* name; ButtonSurface face; float backZ; };
constexpr ButtonProfile ButtonProfiles[]={
    {2,"PUSH_SWITCH",{-47,47,-29,29,-39.975f},-12.612f},
    {2,"PUSH_SWITCH_EMPRTOMB",{-46,46,-31,31,-41.23f},-10.25f},
    {2,"PUSH_SWITCH_FLOATING",{-47,47,-29,29,-34.132f},-3.076f},
    {2,"PUSH_SWITCH_KEEL",{-47,47,-29,29,-43.339f},-12.36f},
    {2,"PUSH_SWITCH_LIVING",{-47,47,-29,29,-39.975f},-12.612f},
    {2,"PUSH_SWITCH_MONASTRY",{-46,46,-31,31,-34.061f},-3.082f},
    {3,"PUSH_SWITCH",{-47,47,-29,29,-40.69f},-9.655f},
    {3,"PUSH_SWITCH_CITY",{-88,96,-92,92,-60.0f},-31.983f},
    {3,"PUSH_SWITCH_COMPOUND",{-47,47,-29,29,-41.963f},-10.928f},
    {3,"PUSH_SWITCH_MINES_CHAMBER_ANTARC",{-45,45,-29,29,-37.51f},-12.554f},
    {3,"PUSH_SWITCH_QUADCHAS",{-54,54,-47,81,-34.0f},-17.0f},
    {3,"PUSH_SWITCH_SEWER_ROOFS_AREA51",{-45,45,-29,29,-37.169f},-12.213f},
    {3,"PUSH_SWITCH_TOWER",{-45,45,-29,29,-37.792f},-12.837f},
    {3,"PUSH_SWITCH_TRIBOSS_SHORE_RAPIDS",{-54,54,-47,81,-34.0f},-17.0f},
};
inline const ButtonProfile* FindButtonProfile(int game,const char* name) {
    if(name) for(const auto& p:ButtonProfiles) if(p.game==game && !std::strcmp(p.name,name)) return &p;
    return nullptr;
}
inline bool ButtonTouch(ButtonSurface face,motiongun::Vec previous,motiongun::Vec current,float scale) {
    const auto finite=[](motiongun::Vec p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    if(!finite(previous) || !finite(current) || !std::isfinite(scale) || scale<=1) return false;
    const auto delta=motiongun::Sub(current,previous);
    if(motiongun::Dot(delta,delta)>scale*scale*.4f*.4f) return false;
    const float plane=face.z-scale*.025f;
    if(previous.z>=plane || current.z<plane || current.z>plane+scale*.12f) return false;
    const float t=(plane-previous.z)/(current.z-previous.z);
    const float x=previous.x+delta.x*t,y=previous.y+delta.y*t;
    return x>=face.left && x<=face.right && y>=face.top && y<=face.bottom;
}
}
