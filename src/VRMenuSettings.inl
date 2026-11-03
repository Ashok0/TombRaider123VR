// Included in Config.cpp inside namespace tr; shares the actual loaded INI path.
namespace vrmenu {
const std::array<Setting,SettingCount>& Settings() {
    static const std::array<Setting,SettingCount> settings{{
        {L"ValveIndexControllers",L"Controller Type",0,1,1,true,[]{return Cfg().valveIndexControllers ? 1.f : 0.f;}},
        {L"VRBrighterColors",L"Color Scheme",0,1,1,true,[]{return Cfg().vrBrighterColors ? 1.f : 0.f;}},
        {L"HybridModernTankControls",L"Hybrid Modern/Tank Controls",0,1,1,true,[]{return Cfg().hybridModernTankControls ? 1.f : 0.f;}}
    }};
    return settings;
}
bool ValidSettings(const Values& values) {
    for(int i=0;i<SettingCount;++i) {
        const auto& s=Settings()[i];const float v=values[i];
        if(!std::isfinite(v) || v<s.minimum || v>s.maximum || (s.toggle && v!=0 && v!=1)) return false;
    }
    return true;
}
Values ReadSettings() {
    Values result{};
    for(int i=0;i<SettingCount;++i) {
        const auto& s=Settings()[i];float value=GetFloat(s.key,s.fallback(),g_iniPath.c_str());
        if(!std::isfinite(value)) value=s.fallback();
        result[i]=s.toggle ? (value!=0 ? 1.f : 0.f) : std::clamp(value,s.minimum,s.maximum);
    }
    return result;
}
bool SaveSettings(const Values& values,const Values& original,std::wstring& error) {
    error.clear();
    if(g_iniPath.empty() || !ValidSettings(values)) { error=L"Invalid VR settings. Nothing saved.";return false; }
    bool changed=false;for(int i=0;i<SettingCount;++i) changed|=values[i]!=original[i];
    if(!changed) return true;
    // Edit a copy beside the INI, then replace it atomically. Write only changed
    // setting keys, retaining comments and unrelated or externally edited settings.
    const std::wstring temp=g_iniPath+L".vr-options.tmp",backup=g_iniPath+L".vr-options.bak";
    if(!CopyFileW(g_iniPath.c_str(),temp.c_str(),FALSE)) { error=L"Could not prepare INI. Nothing saved.";return false; }
    bool ok=true;
    for(int i=0;i<SettingCount && ok;++i) if(values[i]!=original[i]) {
        wchar_t text[48]{};swprintf_s(text,L"%.6g",values[i]);
        ok=WritePrivateProfileStringW(L"VR",Settings()[i].key,text,temp.c_str())!=FALSE;
    }
    // The all-null cache-flush form returns zero on this runtime even after
    // successful writes. Validate the saved values instead of treating that
    // sentinel as a failed save.
    if(ok) {
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,temp.c_str());
        for(int i=0;i<SettingCount && ok;++i) if(values[i]!=original[i])
            ok=std::fabs(GetFloat(Settings()[i].key,-99999,temp.c_str())-values[i])<.0001f;
    }
    if(ok) ok=ReplaceFileW(g_iniPath.c_str(),temp.c_str(),backup.c_str(),0,nullptr,nullptr)!=FALSE;
    if(!ok) { DeleteFileW(temp.c_str());error=L"Save failed. Your existing INI was kept.";return false; }
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,g_iniPath.c_str());
    // Rendering reads this on every submit. Apply only after a successful save;
    // controller bindings still require a restart, so leave that config alone.
    if(values[1]!=original[1]) g_cfg.vrBrighterColors=values[1]!=0;
    if(values[2]!=original[2]) g_cfg.hybridModernTankControls=values[2]!=0;
    Log("VR Options saved; colors and Hybrid Modern/Tank Controls apply immediately, controller type requires restart");
    return true;
}
}
