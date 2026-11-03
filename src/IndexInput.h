// Explicit SteamVR actions keep Index face buttons separate from grip force.
#pragma once
#include <openvr.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace tr {
class IndexInput {
    enum { Stick,Trigger,Grip,Lower,Upper,StickClick,Count };
    vr::VRActionSetHandle_t set_=vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t actions_[2][Count]{},haptic_[2]{};
    bool ready_=false,gripped_[2]{};
public:
    // Observations only: diagnostics never refresh actions or change grip state.
    struct Diagnostics {
        vr::EVRInputError initError=vr::VRInputError_InvalidHandle,updateError=vr::VRInputError_InvalidHandle;
        char initStage[80]="not initialized";
        uint64_t reads=0;
        vr::InputAnalogActionData_t analog[2][3]{};
        vr::InputDigitalActionData_t digital[2][3]{};
        vr::EVRInputError errors[2][6]{};
        unsigned activeSeen[2]{},pressedSeen[2]{};
        float peak[2][4]{}; // absolute stick X/Y, trigger, grip force
    };
private:
    Diagnostics diagnostic_{};
    bool capture_=false;
    vr::EVRInputError InitResult(vr::EVRInputError error,const char* stage) {
        diagnostic_.initError=error;
        std::snprintf(diagnostic_.initStage,sizeof(diagnostic_.initStage),"%s",stage);
        return error;
    }
public:
    bool Ready() const { return ready_; }
    const Diagnostics& Diagnostic() const { return diagnostic_; }
    void Capture(bool enabled) {
        capture_=enabled;
        if(enabled) for(int hand=0;hand<2;++hand) {
            diagnostic_.activeSeen[hand]=diagnostic_.pressedSeen[hand]=0;
            for(auto& peak:diagnostic_.peak[hand]) peak=0;
        }
    }
    static const char* ActionName(int index) {
        const char* names[]={"stick","trigger","grip","lower","upper","stickclick","haptic"};
        return index>=0 && index<7 ? names[index] : "unknown";
    }
    vr::VRActionHandle_t Action(int hand,int index) const {
        return index==6 ? haptic_[hand] : actions_[hand][index];
    }
    vr::VRActionSetHandle_t ActionSet() const { return set_; }
    void Reset() { *this={}; }
    template<class Input> vr::EVRInputError Init(Input& input,const char* manifest) {
        Reset();
        auto error=input.SetActionManifestPath(manifest);
        if(error!=vr::VRInputError_None) return InitResult(error,"SetActionManifestPath");
        error=input.GetActionSetHandle("/actions/game",&set_);
        if(error!=vr::VRInputError_None) return InitResult(error,"GetActionSetHandle /actions/game");
        const char* names[]={"stick","trigger","grip","lower","upper","stickclick"};
        for(int hand=0;hand<2;++hand) {
            const char* side=hand ? "right" : "left";
            for(int i=0;i<Count;++i) {
                char path[80]{};
                std::snprintf(path,sizeof(path),"/actions/game/in/%s_%s",side,names[i]);
                error=input.GetActionHandle(path,&actions_[hand][i]);
                if(error!=vr::VRInputError_None) return InitResult(error,path);
            }
            char path[80]{};
            std::snprintf(path,sizeof(path),"/actions/game/out/%s_haptic",side);
            error=input.GetActionHandle(path,&haptic_[hand]);
            if(error!=vr::VRInputError_None) return InitResult(error,path);
        }
        ready_=true;
        return InitResult(vr::VRInputError_None,"handles resolved");
    }
    template<class Input,class Hand> vr::EVRInputError Read(Input& input,Hand (&out)[2],float press,float release) {
        out[0]={};out[1]={};
        ++diagnostic_.reads;
        for(int h=0;h<2;++h) {
            for(auto& a:diagnostic_.analog[h]) a={};
            for(auto& d:diagnostic_.digital[h]) d={};
            for(auto& e:diagnostic_.errors[h]) e=vr::VRInputError_NoData;
        }
        if(!ready_) return diagnostic_.updateError=vr::VRInputError_InvalidHandle;
        vr::VRActiveActionSet_t set{};set.ulActionSet=set_;
        auto error=input.UpdateActionState(&set,sizeof(set),1);
        diagnostic_.updateError=error;
        if(error!=vr::VRInputError_None) { gripped_[0]=gripped_[1]=false;return error; }
        press=std::isfinite(press) ? std::clamp(press,.05f,1.f) : .55f;
        release=std::isfinite(release) ? std::clamp(release,0.f,press-.01f) : std::min(.35f,press-.01f);
        for(int hand=0;hand<2;++hand) {
            vr::InputAnalogActionData_t analog[3]{};
            vr::InputDigitalActionData_t digital[3]{};
            vr::EVRInputError handError=vr::VRInputError_None;
            for(int i=0;i<3;++i) {
                const auto e=input.GetAnalogActionData(actions_[hand][i],&analog[i],sizeof(analog[i]),vr::k_ulInvalidInputValueHandle);
                diagnostic_.analog[hand][i]=analog[i];diagnostic_.errors[hand][i]=e;
                if(e!=vr::VRInputError_None) handError=e;
            }
            for(int i=0;i<3;++i) {
                const auto e=input.GetDigitalActionData(actions_[hand][i+3],&digital[i],sizeof(digital[i]),vr::k_ulInvalidInputValueHandle);
                diagnostic_.digital[hand][i]=digital[i];diagnostic_.errors[hand][i+3]=e;
                if(e!=vr::VRInputError_None) handError=e;
            }
            if(capture_) {
                for(int i=0;i<3;++i) {
                    if(diagnostic_.errors[hand][i]==vr::VRInputError_None && analog[i].bActive)
                        diagnostic_.activeSeen[hand]|=1u<<i;
                    if(diagnostic_.errors[hand][i+3]==vr::VRInputError_None && digital[i].bActive) {
                        diagnostic_.activeSeen[hand]|=1u<<(i+3);
                        if(digital[i].bState) diagnostic_.pressedSeen[hand]|=1u<<(i+3);
                    }
                }
                const float values[]={analog[0].x,analog[0].y,analog[1].x,analog[2].x};
                for(int i=0;i<4;++i) {
                    const int action=i<2 ? 0 : i-1;
                    if(analog[action].bActive && diagnostic_.errors[hand][action]==vr::VRInputError_None && std::isfinite(values[i]))
                        diagnostic_.peak[hand][i]=std::max(diagnostic_.peak[hand][i],std::fabs(values[i]));
                }
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
