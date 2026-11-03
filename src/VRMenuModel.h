#pragma once
#include <cstdint>
#include "VRMenuSettings.h"
namespace tr::vrmenu {
enum class Action { None,Change,Back };
struct ControllerPage {
    bool open=false,index=false,bright=false,hybrid=false,saveFailed=false;
    int selected=0;
    void Open(bool useIndex,bool brighter=false,bool hybridControls=false) {
        open=true;index=useIndex;bright=brighter;hybrid=hybridControls;selected=0;saveFailed=false;
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
        if(buttons&0xc0100000) return Action::Change;
        return Action::None;
    }
    bool Enabled(int row) const { return row==0 ? index : row==1 ? bright : hybrid; }
    const char* Name(int row) const {
        return row==0 ? "Controller Type" : row==1 ? "Color Scheme" : "Hybrid Modern/Tank Controls";
    }
    const char* Value(int row=0) const {
        return row==0 ? (index ? "Valve Index" : "Meta Touch") :
            row==1 ? (bright ? "Brighter" : "Dark") : (hybrid ? "On" : "Off");
    }
    const char* Message(bool tr6=false) const {
        if(saveFailed) return "Could not save setting";
        return selected==0 ? "Controller changes require a restart" : selected==1 ? "Colors apply immediately" :
            tr6 ? "TR4/5 only; TR6 uses native controls" : "Third person only; applies immediately";
    }
};
}
