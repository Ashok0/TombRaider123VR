#pragma once
#include <cstdint>
namespace tr::vrmenu {
// The extra Options row follows the twelve existing native settings. Their
// indices and action handlers remain unchanged.
struct OptionsSelection {
    bool vr=false;
    bool Input(int choice,uint64_t& input) {
        constexpr uint64_t up=0x10000000,down=0x20000000,confirm=0x100000;
        if(choice!=12) vr=false;
        if(vr) {
            if(input&up) { vr=false;input&=~(up|down|confirm); }
            else if(input&down) input&=~(down|confirm);
            else if(input&confirm) { input&=~confirm;return true; }
            input&=~uint64_t(0xc0000000);
        } else if(choice==12 && (input&down) && !(input&up)) {
            vr=true;input&=~(up|down|confirm|0xc0000000ull);
        }
        return false;
    }
};
}
