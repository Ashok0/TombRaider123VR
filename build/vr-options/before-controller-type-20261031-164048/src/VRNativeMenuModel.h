#pragma once
#include <cstdint>
namespace tr::vrmenu {
// The original five title actions keep their original bit values. Only the
// two edges around the inserted row need interception.
struct TitleSelection {
    bool vr=false;
    bool Input(uint64_t& choice,uint64_t& input) {
        constexpr uint64_t up=0x10000000,down=0x20000000,confirm=0x100000;
        if(choice!=2 && vr) vr=false;
        if(vr) {
            if(input&up) { vr=false;choice=2;input&=~(up|down|confirm); }
            else if(input&down) { vr=false;choice=4;input&=~(up|down|confirm); }
            else if(input&confirm) { input&=~confirm;return true; }
        } else if((choice==2 && (input&down) && !(input&up)) || (choice==4 && (input&up))) {
            vr=true;choice=2;input&=~(up|down|confirm);
        }
        return false;
    }
};
}
