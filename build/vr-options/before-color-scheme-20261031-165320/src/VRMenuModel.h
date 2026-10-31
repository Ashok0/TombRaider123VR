#pragma once
#include <cstdint>
namespace tr::vrmenu {
enum class Action { None,Change,Back };
struct ControllerPage {
    bool open=false,index=false,saveFailed=false;
    void Open(bool useIndex) { open=true;index=useIndex;saveFailed=false; }
    Action Input(uint64_t buttons) {
        if(!open) return Action::None;
        if(buttons&0x200000) { open=false;return Action::Back; }
        if(buttons&0xc0100000) return Action::Change;
        return Action::None;
    }
    const char* Value() const { return index ? "Valve Index" : "Meta Touch"; }
    const char* Message() const {
        return saveFailed ? "Could not save Controller Type" : "Restart the game to apply changes";
    }
};
}
