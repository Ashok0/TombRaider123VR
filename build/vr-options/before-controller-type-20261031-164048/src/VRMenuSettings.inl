// Included in Config.cpp inside namespace tr; shares the actual loaded INI path.
namespace vrmenu {
const std::array<Setting,SettingCount>& Settings() {
    static const std::array<Setting,SettingCount> settings{{
        {L"ValveIndexControllers",L"Controller type",0,1,1,true,[]{return Cfg().valveIndexControllers ? 1.f : 0.f;}},
        {L"ValveIndexGripPress",L"Index grip: squeeze to grab",.05f,1,.05f,false,[]{return Cfg().valveIndexGripPress;}},
        {L"ValveIndexGripRelease",L"Index grip: release threshold",0,.95f,.05f,false,[]{return Cfg().valveIndexGripRelease;}},
        {L"DpadShift",L"Right-stick click + left stick: D-pad",0,1,1,true,[]{return Cfg().dpadShift ? 1.f : 0.f;}},
        {L"DpadShiftDeadzone",L"D-pad stick deadzone",.1f,.9f,.05f,false,[]{return Cfg().dpadShiftDeadzone;}},
        {L"FirstPersonTurnDegreesPerSecond",L"First-person turn speed (deg/sec)",30,360,15,false,[]{return Cfg().firstPersonTurnDegreesPerSecond;}},
        {L"FirstPersonTurnDeadzone",L"First-person turn deadzone",.05f,.9f,.05f,false,[]{return Cfg().firstPersonTurnDeadzone;}},
        {L"FirstPersonMotionGunGripForwardMetres",L"Hand alignment: back (metres)",-.5f,.5f,.005f,false,[]{return LiveMotionGunCalibration().gripForwardMetres;}},
        {L"FirstPersonMotionGunRaiseMetres",L"Hand alignment: up (metres)",-.5f,.5f,.005f,false,[]{return LiveMotionGunCalibration().raiseMetres;}},
        {L"FirstPersonMotionGunRightMetres",L"Hand alignment: right (metres)",-.5f,.5f,.005f,false,[]{return LiveMotionGunCalibration().rightMetres;}},
        {L"FirstPersonMotionGunPitchDegrees",L"Hand alignment: pitch (degrees)",-90,90,1,false,[]{return LiveMotionGunCalibration().pitchDegrees;}},
        {L"FirstPersonMotionGunYawDegrees",L"Hand alignment: yaw (degrees)",-90,90,1,false,[]{return LiveMotionGunCalibration().yawDegrees;}},
        {L"FirstPersonMotionGunRollDegrees",L"Hand alignment: roll (degrees)",-180,180,1,false,[]{return LiveMotionGunCalibration().rollDegrees;}}
    }};
    return settings;
}
bool ValidSettings(const Values& values) {
    for(int i=0;i<SettingCount;++i) {
        const auto& s=Settings()[i];const float v=values[i];
        if(!std::isfinite(v) || v<s.minimum || v>s.maximum || (s.toggle && v!=0 && v!=1)) return false;
    }
    return values[2]<values[1];
}
Values ReadSettings() {
    Values result{};
    for(int i=0;i<SettingCount;++i) {
        const auto& s=Settings()[i];float value=GetFloat(s.key,s.fallback(),g_iniPath.c_str());
        if(!std::isfinite(value)) value=s.fallback();
        result[i]=s.toggle ? (value!=0 ? 1.f : 0.f) : std::clamp(value,s.minimum,s.maximum);
    }
    result[2]=std::min(result[2],result[1]-.05f);
    return result;
}
bool SaveSettings(const Values& values,const Values& original,std::wstring& error) {
    error.clear();
    if(g_iniPath.empty() || !ValidSettings(values)) { error=L"Invalid controller settings. Nothing saved.";return false; }
    bool changed=false;for(int i=0;i<SettingCount;++i) changed|=values[i]!=original[i];
    if(!changed) return true;
    // Edit a copy beside the INI, then replace it atomically. Write only changed
    // controller keys, retaining comments and unrelated or externally edited settings.
    const std::wstring temp=g_iniPath+L".vr-options.tmp",backup=g_iniPath+L".vr-options.bak";
    if(!CopyFileW(g_iniPath.c_str(),temp.c_str(),FALSE)) { error=L"Could not prepare INI. Nothing saved.";return false; }
    bool ok=true;
    const bool gripChanged=values[1]!=original[1] || values[2]!=original[2];
    for(int i=0;i<SettingCount && ok;++i) if(values[i]!=original[i] || ((i==1 || i==2) && gripChanged)) {
        wchar_t text[48]{};swprintf_s(text,L"%.6g",values[i]);
        ok=WritePrivateProfileStringW(L"VR",Settings()[i].key,text,temp.c_str())!=FALSE;
    }
    // The all-null cache-flush form returns zero on this runtime even after
    // successful writes. Validate the saved values instead of treating that
    // sentinel as a failed save.
    if(ok) {
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,temp.c_str());
        for(int i=0;i<SettingCount && ok;++i) if(values[i]!=original[i] || ((i==1 || i==2) && gripChanged))
            ok=std::fabs(GetFloat(Settings()[i].key,-99999,temp.c_str())-values[i])<.0001f;
    }
    if(ok) ok=ReplaceFileW(g_iniPath.c_str(),temp.c_str(),backup.c_str(),0,nullptr,nullptr)!=FALSE;
    if(!ok) { DeleteFileW(temp.c_str());error=L"Save failed. Your existing INI was kept.";return false; }
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,g_iniPath.c_str());
    Log("VR OPTIONS: controller settings saved; restart required; previous INI in .vr-options.bak");
    return true;
}
}
