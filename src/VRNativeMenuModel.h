#pragma once
#include <cstdint>
namespace tr::vrmenu {
// TR1-3 translate navigation into inventory-specific bits before Options runs.
inline uint64_t Options123Buttons(uint32_t input) {
    uint64_t buttons=input&0x300000;
    if(input&0x2000) buttons|=0x10000000;
    if(input&0x4000) buttons|=0x20000000;
    if(input&0x8000) buttons|=0x40000000;
    if(input&0x10000) buttons|=0x80000000;
    return buttons;
}
struct Options123Selection {
    bool vr=false;
    bool Input(int choice,uint32_t& input) {
        constexpr uint32_t up=0x2000,down=0x4000,change=0x18000,confirm=0x100000;
        if(choice!=12) vr=false;
        if(input&0x200000) { vr=false;input&=~(up|down|change|confirm);return false; }
        if(vr) {
            const auto buttons=input;
            input&=~(up|down|change|confirm);
            if(buttons&up) vr=false;
            else if(!(buttons&down) && (buttons&confirm)) return true;
        } else if(choice==12 && (input&down) && !(input&up)) {
            vr=true;input&=~(up|down|change|confirm);
        }
        return false;
    }
};
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
