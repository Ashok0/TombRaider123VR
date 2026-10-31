#pragma once
#include "VRMenuSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace tr::vrmenu {
constexpr int RowsPerPage=5;
enum class Event { None,Open,Change,Save,Cancel };
struct Model {
    Values values{},original{};
    bool open=false,drain=false,consume=false;
    int selected=0,page=0,lastDirection=0;
    uint16_t previous=0;
    uint64_t repeat=0;
    void Open() {
        open=drain=consume=true;
        selected=page=lastDirection=0;
    }
    Event Input(bool available,uint16_t buttons,int16_t x,int16_t y,uint64_t now) {
        constexpr uint16_t A=0x1000,B=0x2000;
        const auto pressed=uint16_t(buttons & ~previous);previous=buttons;
        consume=false;
        if(!available) { open=drain=false;lastDirection=0;return Event::None; }
        if(drain) {
            consume=true;
            if(!buttons && std::abs(int(x))<9000 && std::abs(int(y))<9000) drain=false;
            return Event::None;
        }
        if(!open) return Event::None;
        consume=true;
        if(pressed&B) { open=false;drain=true;return Event::Cancel; }
        if(pressed&0x8000) return Event::Save; // Y saves from any page.
        if(pressed&A) {
            if(selected==SettingCount) return Event::Save;
            if(selected==SettingCount+1) { open=false;drain=true;return Event::Cancel; }
        }
        int direction=0;
        if(buttons&1) direction=-1;else if(buttons&2) direction=1;
        else if(buttons&4) direction=-2;else if(buttons&8) direction=2;
        else if(std::abs(int(y))>std::abs(int(x)) && std::abs(int(y))>16000) direction=y>0 ? -1 : 1;
        else if(std::abs(int(x))>16000) direction=x>0 ? 2 : -2;
        const bool fire=direction && (direction!=lastDirection || now>=repeat);
        if(direction!=lastDirection) repeat=now+400;
        else if(fire) repeat=now+130;
        lastDirection=direction;
        if(fire && std::abs(direction)==1) {
            selected=std::clamp(selected+direction,0,SettingCount+1);
            if(selected<SettingCount) page=selected/RowsPerPage;
            return Event::Change;
        }
        if(selected<SettingCount && ((fire && std::abs(direction)==2) || ((pressed&A) && Settings()[selected].toggle))) {
            const auto& s=Settings()[selected];float v=values[selected];
            v=s.toggle ? 1-v : std::clamp(v+(direction>0 ? s.step : -s.step),s.minimum,s.maximum);
            if(selected==1) v=std::max(v,values[2]+.05f);
            if(selected==2) v=std::min(v,values[1]-.05f);
            values[selected]=std::clamp(v,s.minimum,s.maximum);
            return Event::Change;
        }
        return Event::None;
    }
};
}
