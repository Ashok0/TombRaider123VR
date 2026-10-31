// Explicit SteamVR actions keep Index face buttons separate from grip force.
#pragma once
#include <openvr.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tr {
class IndexInput {
    enum { Stick,Trigger,Grip,Lower,Upper,StickClick,Count };
    vr::VRActionSetHandle_t set_=vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t actions_[2][Count]{},haptic_[2]{};
    bool ready_=false,gripped_[2]{};
public:
    void Reset() { *this={}; }
    template<class Input> vr::EVRInputError Init(Input& input,const char* manifest) {
        Reset();
        auto error=input.SetActionManifestPath(manifest);
        if(error!=vr::VRInputError_None) return error;
        error=input.GetActionSetHandle("/actions/game",&set_);
        if(error!=vr::VRInputError_None) return error;
        const char* names[]={"stick","trigger","grip","lower","upper","stickclick"};
        for(int hand=0;hand<2;++hand) {
            const char* side=hand ? "right" : "left";
            for(int i=0;i<Count;++i) {
                char path[80]{};
                std::snprintf(path,sizeof(path),"/actions/game/in/%s_%s",side,names[i]);
                error=input.GetActionHandle(path,&actions_[hand][i]);
                if(error!=vr::VRInputError_None) return error;
            }
            char path[80]{};
            std::snprintf(path,sizeof(path),"/actions/game/out/%s_haptic",side);
            error=input.GetActionHandle(path,&haptic_[hand]);
            if(error!=vr::VRInputError_None) return error;
        }
        ready_=true;
        return vr::VRInputError_None;
    }
    template<class Input,class Hand> vr::EVRInputError Read(Input& input,Hand (&out)[2],float press,float release) {
        out[0]={};out[1]={};
        if(!ready_) return vr::VRInputError_InvalidHandle;
        vr::VRActiveActionSet_t set{};set.ulActionSet=set_;
        auto error=input.UpdateActionState(&set,sizeof(set),1);
        if(error!=vr::VRInputError_None) { gripped_[0]=gripped_[1]=false;return error; }
        press=std::isfinite(press) ? std::clamp(press,.05f,1.f) : .55f;
        release=std::isfinite(release) ? std::clamp(release,0.f,press-.01f) : std::min(.35f,press-.01f);
        for(int hand=0;hand<2;++hand) {
            vr::InputAnalogActionData_t analog[3]{};
            vr::InputDigitalActionData_t digital[3]{};
            vr::EVRInputError handError=vr::VRInputError_None;
            for(int i=0;i<3;++i) {
                const auto e=input.GetAnalogActionData(actions_[hand][i],&analog[i],sizeof(analog[i]),vr::k_ulInvalidInputValueHandle);
                if(e!=vr::VRInputError_None) handError=e;
            }
            for(int i=0;i<3;++i) {
                const auto e=input.GetDigitalActionData(actions_[hand][i+3],&digital[i],sizeof(digital[i]),vr::k_ulInvalidInputValueHandle);
                if(e!=vr::VRInputError_None) handError=e;
            }
            if(handError!=vr::VRInputError_None) { gripped_[hand]=false;error=handError;continue; }
            auto& h=out[hand];
            for(const auto& a:analog) h.valid|=a.bActive;
            for(const auto& d:digital) h.valid|=d.bActive;
            const auto value=[](float f,float lo,float hi) { return std::isfinite(f) ? std::clamp(f,lo,hi) : 0.f; };
            if(analog[Stick].bActive) {
                h.stickX=value(analog[Stick].x,-1,1);h.stickY=value(analog[Stick].y,-1,1);
            }
            if(analog[Trigger].bActive) h.trigger=value(analog[Trigger].x,0,1);
            const float force=analog[Grip].bActive ? value(analog[Grip].x,0,1) : 0.f;
            if(!analog[Grip].bActive || !std::isfinite(analog[Grip].x) || force<=release) gripped_[hand]=false;
            else if(force>=press) gripped_[hand]=true;
            h.grip=gripped_[hand] ? 1.f : 0.f;
            h.btnLower=digital[0].bActive && digital[0].bState;
            h.btnUpper=digital[1].bActive && digital[1].bState;
            h.stickClick=digital[2].bActive && digital[2].bState;
            // Preserve the existing diagnostic mask convention in action mode.
            if(h.btnLower) h.rawPressed|=vr::ButtonMaskFromId(vr::k_EButton_A);
            if(h.btnUpper) h.rawPressed|=vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu);
            if(h.stickClick) h.rawPressed|=vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
            if(h.grip>.5f) h.rawPressed|=vr::ButtonMaskFromId(vr::k_EButton_Grip);
            if(h.trigger>.5f) h.rawPressed|=vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
        }
        return error;
    }
    template<class Input> void Haptic(Input& input,int hand,unsigned short microseconds) const {
        if(ready_ && hand>=0 && hand<2)
            input.TriggerHapticVibrationAction(haptic_[hand],0,microseconds/1000000.f,150.f,1.f,vr::k_ulInvalidInputValueHandle);
    }
};
}
