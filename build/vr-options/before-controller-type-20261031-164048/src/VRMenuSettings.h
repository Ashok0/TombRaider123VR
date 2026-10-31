#pragma once
#include <array>
#include <string>
namespace tr::vrmenu {
constexpr int SettingCount=13;
struct Setting {
    const wchar_t* key;
    const wchar_t* label;
    float minimum,maximum,step;
    bool toggle;
    float (*fallback)();
};
const std::array<Setting,SettingCount>& Settings();
using Values=std::array<float,SettingCount>;
Values ReadSettings();
bool SaveSettings(const Values& values,const Values& original,std::wstring& error);
bool ValidSettings(const Values& values);
}
