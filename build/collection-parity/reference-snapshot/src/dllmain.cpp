#include "Config.h"
#include "Engine.h"
#include "Hooks.h"
#include "Log.h"
#include "VRSystem.h"

#include <windows.h>
#include <string>

namespace {

HMODULE g_self = nullptr;

std::wstring SelfDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}

// Real work happens off the loader lock. DllMain must not call LoadLibrary
// (VRSystem::Init does, for openvr_api.dll) or start GL work.
DWORD WINAPI StartupThread(LPVOID) {
    const std::wstring dir = SelfDir();
    tr::LogOpen((dir + L"\\TombRaiderVR.log").c_str());
    Log("TombRaiderVR: starting");

    // Create the ini on first run so a fresh install has something to read
    // and, more usefully, something to READ ABOUT -- the template is heavily
    // commented and those comments are most of the accumulated knowledge here.
    const std::wstring iniPath = dir + L"\\TombRaiderVR.ini";
    if (tr::EnsureConfigFile(iniPath.c_str())) {
        LogF("config: no ini found, wrote the stock one to %S", iniPath.c_str());
    }

    tr::LoadConfig(iniPath.c_str());
    tr::WarnIgnoredOptions();

    if (!tr::Cfg().enabled) {
        Log("TombRaiderVR: disabled by config, not hooking");
        return 0;
    }

    if (!tr::Bind()) {
        Log("TombRaiderVR: could not bind to tomb456.exe, aborting");
        return 0;
    }

    // OpenVR is deliberately NOT initialised here; ogl_present brings it up on
    // the first rendered frame instead.
    //
    // The reason is resilience, not correctness: SteamVR can then be started
    // after the game, and a runtime that is still coming up gets retried rather
    // than failing the session. Hooks only need the exe, which is fully mapped
    // by the time this thread runs, so they can go in immediately.
    //
    // (Historical note, so nobody re-derives it: this deferral was originally
    // added on the theory that early-loader timing caused
    // VRInitError_Init_VRClientDLLNotFound (102). That theory was wrong -- the
    // real cause was the winmm proxy shadowing exports vrclient_x64.dll needed,
    // fixed in src\proxy. The deferral was kept on its own merits.)
    if (!tr::InstallHooks()) {
        Log("TombRaiderVR: hook installation failed");
        return 0;
    }

    Log("TombRaiderVR: hooks installed; OpenVR will be brought up on the first frame");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr))
            CloseHandle(t);
        break;

    case DLL_PROCESS_DETACH:
        // Only unwind on an explicit FreeLibrary. On process teardown the GL
        // context and the compositor are already going away and touching them
        // is a good way to hang the exit path.
        tr::RemoveHooks();
        tr::VR().Shutdown();
        tr::LogClose();
        break;
    }
    return TRUE;
}
