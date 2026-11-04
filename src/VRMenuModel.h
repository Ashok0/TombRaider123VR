#pragma once
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include "VRMenuSettings.h"
namespace tr::vrmenu {
enum class Action { None,Change,Back };
struct ControllerPage {
    bool open=false,index=false,bright=false,hybrid=false,saveFailed=false;
    int selected=0;
    float bodyForwardCm=0;
    mutable char bodyValue[32]{};
    void Open(bool useIndex,bool brighter=false,bool hybridControls=false,float bodyForward=0) {
        open=true;index=useIndex;bright=brighter;hybrid=hybridControls;selected=0;saveFailed=false;
        bodyForwardCm=bodyForward;
    }
    Action Input(uint64_t buttons) {
        if(!open) return Action::None;
        if(buttons&0x200000) { open=false;return Action::Back; }
        // Consume navigation before changes, avoiding an accidental toggle when
        // a direction and Select arrive together. Opposite directions cancel.
        const auto vertical=buttons&0x30000000;
        if(vertical) {
            if(vertical!=0x30000000) {
                selected=(selected+(vertical==0x10000000 ? SettingCount-1 : 1))%SettingCount;
                saveFailed=false;
            }
            return Action::None;
        }
        if((buttons&0xc0000000)==0xc0000000) return Action::None;
        if(buttons&0xc0100000) return Action::Change;
        return Action::None;
    }
    bool Enabled(int row) const { return row==0 ? index : row==1 ? bright : hybrid; }
    float ChangedValue(uint64_t buttons) const {
        if(selected!=3) return Enabled(selected) ? 0.f : 1.f;
        if(buttons&0x100000) return 0; // Select restores the exact default.
        return std::clamp(bodyForwardCm+(buttons&0x40000000 ? -1.f : 1.f),0.f,20.f);
    }
    const char* Name(int row) const {
        return row==0 ? "Controller Type" : row==1 ? "Color Scheme" : row==2 ? "Hybrid Modern/Tank Controls" : "Body Position";
    }
    const char* Value(int row=0) const {
        if(row==3) {
            if(bodyForwardCm==0) return "Default (0 cm)";
            std::snprintf(bodyValue,sizeof(bodyValue),"Forward %.0f cm",bodyForwardCm);return bodyValue;
        }
        return row==0 ? (index ? "Valve Index" : "Meta Touch") :
            row==1 ? (bright ? "Brighter" : "Dark") : (hybrid ? "On" : "Off");
    }
    const char* Message(bool tr6=false) const {
        if(saveFailed) return "Could not save setting";
        if(selected==3) return tr6 ? "TR4/5 first-person body only" : "Left/Right: 1 cm   Select: default";
        return selected==0 ? "Controller changes require a restart" : selected==1 ? "Colors apply immediately" :
            tr6 ? "TR4/5 only; TR6 uses native controls" : "Third person only; applies immediately";
    }
};
}
