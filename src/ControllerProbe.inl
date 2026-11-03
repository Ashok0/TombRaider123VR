// Included inside namespace tr by VRSystem.cpp. No action refreshes, synthetic
// input, runtime binding changes or extra haptics are performed by this probe.
namespace {
std::wstring ProbeWide(const std::string& path) {
    const int count=MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,nullptr,0);
    if(count<=0) return {};
    std::wstring result(static_cast<size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,&result[0],count);
    result.resize(static_cast<size_t>(count-1));return result;
}
void ProbeFile(const std::string& path) {
    const auto wide=ProbeWide(path);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const bool ok=!wide.empty() && GetFileAttributesExW(wide.c_str(),GetFileExInfoStandard,&data);
    const DWORD error=ok ? 0 : GetLastError();
    LogF("input-probe: file=%s exists=%d bytes=%llu win32-error=%lu",path.c_str(),
        ok && !(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY),
        (static_cast<unsigned long long>(data.nFileSizeHigh)<<32)|data.nFileSizeLow,error);
}
void ProbeProperty(vr::IVRSystem& system,vr::TrackedDeviceIndex_t device,vr::ETrackedDeviceProperty property,const char* name) {
    char value[256]{};vr::ETrackedPropertyError error=vr::TrackedProp_Success;
    system.GetStringTrackedDeviceProperty(device,property,value,sizeof(value),&error);
    LogF("input-probe: device=%u %s=%.255s property-error=%d",device,name,value,int(error));
}
}

void VRSystem::PollControllerProbe() {
    DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
    const bool down=(GetAsyncKeyState(VK_F7)&0x8000)!=0;
    const bool modified=((GetAsyncKeyState(VK_CONTROL)|GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_MENU))&0x8000)!=0;
    const auto event=m_controllerProbe.Tick(GetTickCount64(),down,foreground==GetCurrentProcessId(),modified);
    if(event==ControllerProbe::None) return;
    if(event==ControllerProbe::Started) {
        m_probeReadStart=m_indexInput.Diagnostic().reads;
        m_indexInput.Capture(true);
        Log("input-probe: BEGIN F7 capture (20 seconds). Move both sticks, press A/B and stick clicks, pull triggers and squeeze grips. F7 stops early.");
        GamepadControllerProbe(true,false);
        LogControllerProbe(true);
    } else {
        LogControllerProbe(event==ControllerProbe::Stopped);
        GamepadControllerProbe(false,event==ControllerProbe::Stopped);
        if(event==ControllerProbe::Stopped) {
            const auto& d=m_indexInput.Diagnostic();
            for(int hand=0;hand<2;++hand)
                LogF("input-probe: SUMMARY %s active-seen=%02x pressed-seen=%02x peak-stick=%.3f,%.3f trigger=%.3f grip-force=%.3f",
                    hand ? "right" : "left",d.activeSeen[hand],d.pressedSeen[hand],
                    d.peak[hand][0],d.peak[hand][1],d.peak[hand][2],d.peak[hand][3]);
            Log("input-probe: masks: bit 0=stick 1=trigger 2=grip 3=A 4=B 5=stick-click. Legacy raw states may be unavailable in action mode.");
            if(!Cfg().valveIndexControllers)
                Log("input-probe: HINT Index mode is OFF in the running process. Select Index in VR Options, then fully exit and restart the game.");
            else if(!m_input || !m_indexInput.Ready())
                Log("input-probe: HINT Index input initialization failed. Check interface/stage/error and both binding files above.");
            else if(d.reads==m_probeReadStart)
                Log("input-probe: HINT no action polls reached the input reader. Check GamepadEnabled, game-pad hook and game polling above.");
            else if(!d.activeSeen[0] && !d.activeSeen[1])
                Log("input-probe: HINT no active actions observed. Check SteamVR input availability, controller roles/type, bound sources and current application binding.");
            m_indexInput.Capture(false);
            Log("input-probe: END. Send this complete TombRaiderVR.log before launching the game again (startup replaces the log).");
        }
    }
}

void VRSystem::LogControllerProbe(bool details) {
    const auto& d=m_indexInput.Diagnostic();
    DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
    const auto focus=m_compositor ? m_compositor->GetCurrentSceneFocusProcess() : 0;
    LogF("input-probe: t=%llums game=%d VR=%d GamepadEnabled=%d running-Index=%d input-interface=%d ready=%d desktop-focus=%d scene-focus-is-us=%d input-available=%d steamvr-drawing-controllers=%d head-pose=%d hand-poses=%d,%d",
        static_cast<unsigned long long>(GetTickCount64()-m_controllerProbe.started),CurrentGame(),active(),
        Cfg().gamepadEnabled,Cfg().valveIndexControllers,m_input!=nullptr,m_indexInput.Ready(),
        foreground==GetCurrentProcessId(),focus==GetCurrentProcessId(),m_system ? m_system->IsInputAvailable() : false,
        m_system ? m_system->IsSteamVRDrawingControllers() : false,m_poseValid,m_controllerPoseValid[0],m_controllerPoseValid[1]);
    LogF("input-probe: Index init-stage=%s init-error=%s(%d) interface-error=%d polls=%llu update-error=%s(%d) grip-thresholds=%.3f/%.3f",
        d.initStage,InputErrorName(d.initError),int(d.initError),int(m_inputInterfaceError),
        static_cast<unsigned long long>(d.reads-m_probeReadStart),InputErrorName(d.updateError),int(d.updateError),
        Cfg().valveIndexGripPress,Cfg().valveIndexGripRelease);
    if(details) {
        LogF("input-probe: build=%s %s SDK=%s/%s",__DATE__,__TIME__,vr::IVRSystem_Version,vr::IVRInput_Version);
        const auto manifest=IndexManifestPath();
        ProbeFile(manifest);
        const auto slash=manifest.find_last_of("\\/");
        if(slash!=std::string::npos) {
            const auto directory=manifest.substr(0,slash+1);
            ProbeFile(directory+"bindings_index.json");
            const auto parent=manifest.find_last_of("\\/",slash-1);
            if(parent!=std::string::npos) {
                const auto ini=manifest.substr(0,parent+1)+"TombRaiderVR.ini";
                const auto wide=ProbeWide(ini);
                LogF("input-probe: ini=%s saved-Index=%d running-Index=%d (a difference requires restart)",
                    ini.c_str(),GetPrivateProfileIntW(L"VR",L"ValveIndexControllers",0,wide.c_str()),Cfg().valveIndexControllers);
            }
        }
        if(m_system) for(vr::TrackedDeviceIndex_t device=0;device<vr::k_unMaxTrackedDeviceCount;++device) {
            if(!m_system->IsTrackedDeviceConnected(device)) continue;
            LogF("input-probe: connected-device=%u class=%d role=%d",device,
                int(m_system->GetTrackedDeviceClass(device)),int(m_system->GetControllerRoleForTrackedDeviceIndex(device)));
            ProbeProperty(*m_system,device,vr::Prop_ModelNumber_String,"model");
            if(m_system->GetTrackedDeviceClass(device)==vr::TrackedDeviceClass_Controller) {
                ProbeProperty(*m_system,device,vr::Prop_ControllerType_String,"controller-type");
                ProbeProperty(*m_system,device,vr::Prop_InputProfilePath_String,"input-profile");
            }
        }
    }
    for(int hand=0;hand<2;++hand) {
        const char* side=hand ? "right" : "left";
        if(m_system) {
            const auto role=hand ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand;
            const auto device=m_system->GetTrackedDeviceIndexForControllerRole(role);
            vr::VRControllerState_t raw{};
            const bool connected=device!=vr::k_unTrackedDeviceIndexInvalid && m_system->IsTrackedDeviceConnected(device);
            const bool ok=connected && m_system->GetControllerState(device,&raw,sizeof(raw));
            LogF("input-probe: %s role-device=%u connected=%d legacy-readable=%d raw-buttons=%016llx axes=%.2f,%.2f/%.2f,%.2f/%.2f,%.2f/%.2f,%.2f/%.2f,%.2f",
                side,device,connected,ok,static_cast<unsigned long long>(raw.ulButtonPressed),
                raw.rAxis[0].x,raw.rAxis[0].y,raw.rAxis[1].x,raw.rAxis[1].y,raw.rAxis[2].x,raw.rAxis[2].y,
                raw.rAxis[3].x,raw.rAxis[3].y,raw.rAxis[4].x,raw.rAxis[4].y);
        }
        if(!Cfg().valveIndexControllers) continue;
        for(int i=0;i<6;++i) {
            const bool analog=i<3;
            const auto active=analog ? d.analog[hand][i].bActive : d.digital[hand][i-3].bActive;
            const auto origin=analog ? d.analog[hand][i].activeOrigin : d.digital[hand][i-3].activeOrigin;
            const float x=analog ? d.analog[hand][i].x : (d.digital[hand][i-3].bState ? 1.f : 0.f);
            const float y=analog ? d.analog[hand][i].y : 0.f;
            LogF("input-probe: %s_%s active=%d value=%.3f,%.3f origin=%llu error=%s(%d)",
                side,IndexInput::ActionName(i),active,x,y,static_cast<unsigned long long>(origin),
                InputErrorName(d.errors[hand][i]),int(d.errors[hand][i]));
        }
        if(!details || !m_input || !m_indexInput.Ready()) continue;
        for(int i=0;i<7;++i) {
            const auto action=m_indexInput.Action(hand,i);
            vr::InputBindingInfo_t bindings[4]{};uint32_t count=0;
            const auto error=m_input->GetActionBindingInfo(action,bindings,sizeof(bindings[0]),4,&count);
            LogF("input-probe: binding %s_%s handle=%llu sources=%u error=%s(%d)",
                side,IndexInput::ActionName(i),static_cast<unsigned long long>(action),count,InputErrorName(error),int(error));
            if(error==vr::VRInputError_None || error==vr::VRInputError_BufferTooSmall)
                for(uint32_t b=0;b<std::min(count,4u);++b)
                    LogF("input-probe: source device=%.127s input=%.127s mode=%.127s slot=%.127s type=%.31s",
                        bindings[b].rchDevicePathName,bindings[b].rchInputPathName,bindings[b].rchModeName,
                        bindings[b].rchSlotName,bindings[b].rchInputSourceType);
            vr::VRInputValueHandle_t origins[16]{};
            const auto originError=m_input->GetActionOrigins(m_indexInput.ActionSet(),action,origins,16);
            if(originError!=vr::VRInputError_None)
                LogF("input-probe: origins %s_%s error=%s(%d)",side,IndexInput::ActionName(i),InputErrorName(originError),int(originError));
            else for(auto origin:origins) if(origin!=vr::k_ulInvalidInputValueHandle) {
                vr::InputOriginInfo_t info{};
                const auto infoError=m_input->GetOriginTrackedDeviceInfo(origin,&info,sizeof(info));
                LogF("input-probe: origin=%llu device=%u component=%.127s error=%s(%d)",
                    static_cast<unsigned long long>(origin),info.trackedDeviceIndex,info.rchRenderModelComponentName,
                    InputErrorName(infoError),int(infoError));
            }
        }
    }
}
