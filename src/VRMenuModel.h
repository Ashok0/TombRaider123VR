#pragma once
#include <cstdint>
namespace tr::vrmenu {
enum class Action { None,Change,Back };
struct ControllerPage {
    bool open=false,index=false,bright=false,saveFailed=false;
    int selected=0;
    void Open(bool useIndex,bool brighter=false) { open=true;index=useIndex;bright=brighter;selected=0;saveFailed=false; }
    Action Input(uint64_t buttons) {
        if(!open) return Action::None;
        if(buttons&0x200000) { open=false;return Action::Back; }
        // Consume navigation before changes, avoiding an accidental toggle when
        // a direction and Select arrive together. Opposite directions cancel.
        const auto vertical=buttons&0x30000000;
        if(vertical) {
            if(vertical!=0x30000000) { selected=1-selected;saveFailed=false; }
            return Action::None;
        }
        if(buttons&0xc0100000) return Action::Change;
        return Action::None;
    }
    const char* Name(int row) const { return row==0 ? "Controller Type" : "Color Scheme"; }
    const char* Value(int row=0) const {
        return row==0 ? (index ? "Valve Index" : "Meta Touch") : (bright ? "Brighter" : "Dark");
    }
    const char* Message() const {
        if(saveFailed) return "Could not save setting";
        return selected==0 ? "Controller changes require a restart" : "Colors apply immediately";
    }
};
}
