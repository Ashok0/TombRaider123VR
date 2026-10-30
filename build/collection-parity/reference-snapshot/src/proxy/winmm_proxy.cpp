// winmm_proxy.cpp -- an auto-loading shim for TombRaiderVR.dll.
//
// tomb456.exe statically imports WINMM.dll, so a winmm.dll dropped in the game
// folder is loaded by the Windows loader before the game runs a single
// instruction. That gets the mod in without an injector: launch from Steam
// normally.
//
// THE GOTCHA THIS FILE EXISTS TO AVOID
//
// The obvious implementation is a PE export forwarder:
//
//     #pragma comment(linker, "/export:timeGetTime=winmm.timeGetTime")
//
// That is circular. The loader resolves the "winmm" in the forwarder string by
// the normal search order, and the application directory comes before
// System32 -- so it finds *this* DLL and forwards to itself. The usual patch is
// to bake an absolute path into the forwarder
// ("C:\Windows\System32\winmm.timeGetTime"), which works but hardcodes a path
// and breaks on any non-standard Windows directory.
//
// Instead this loads the real winmm.dll by explicit, unambiguous path from
// GetSystemDirectory() and thunks each export through a function pointer. No
// hardcoded paths, no circularity, and the forwarding is ordinary code that can
// be stepped through.
//
// Only the ten functions tomb456.exe actually imports are forwarded. Anything
// else that loaded this DLL expecting a full winmm would be disappointed, which
// is why it belongs in the game folder and nowhere else.
//
// Signatures use ABI-compatible opaque types rather than including <mmsystem.h>.
// Defining functions with the same names as that header's declarations invites
// mismatch errors for no benefit; every parameter here is a pointer or a 32/64-
// bit integer, which is all the x64 calling convention cares about.

#include <windows.h>
#include <cstring>
#include <cwchar>

// Supplied by the linker: the base of this module. Cheaper and more reliable
// than GetModuleHandle by name, which is precisely the lookup we cannot trust
// here (a bare "winmm" resolves to whichever copy the loader found first).
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

HMODULE g_realWinmm = nullptr;
HMODULE g_mod       = nullptr;
INIT_ONCE g_once    = INIT_ONCE_STATIC_INIT;

void LogLine(const char* msg) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *(slash + 1) = L'\0';
    wcscat_s(path, L"TombRaiderVR-proxy.log");

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

BOOL CALLBACK InitProxy(PINIT_ONCE, PVOID, PVOID*) {
    // 1. The real winmm, by absolute path. Never by bare name -- that is what
    //    would find this DLL instead.
    wchar_t sys[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        LogLine("proxy: GetSystemDirectory failed");
        return TRUE;
    }
    wcscat_s(sys, L"\\winmm.dll");
    g_realWinmm = LoadLibraryW(sys);
    LogLine(g_realWinmm ? "proxy: real winmm loaded from System32"
                        : "proxy: FAILED to load the real winmm");

    // 2. The mod, from this DLL's own directory. Absent is not an error -- the
    //    game should still run normally with only the proxy present.
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH);
    if (wchar_t* slash = wcsrchr(self, L'\\')) *(slash + 1) = L'\0';
    wcscat_s(self, L"TombRaiderVR.dll");

    if (GetFileAttributesW(self) == INVALID_FILE_ATTRIBUTES) {
        LogLine("proxy: TombRaiderVR.dll not found beside the proxy, forwarding only");
        return TRUE;
    }

    g_mod = LoadLibraryW(self);
    LogLine(g_mod ? "proxy: TombRaiderVR.dll loaded"
                  : "proxy: TombRaiderVR.dll FAILED to load");
    return TRUE;
}

// Deliberately not called from DllMain: LoadLibrary under the loader lock is a
// documented deadlock risk, and the mod spins up a worker thread. The first
// forwarded call happens after the loader has finished, which is a safe moment.
inline void EnsureInit() {
    InitOnceExecuteOnce(&g_once, InitProxy, nullptr, nullptr);
}

template <typename Fn>
Fn Resolve(const char* name) {
    EnsureInit();
    if (!g_realWinmm) return nullptr;
    return reinterpret_cast<Fn>(GetProcAddress(g_realWinmm, name));
}

// MMRESULT is a UINT; MMSYSERR_ERROR == 1 is the generic failure.
constexpr UINT kMMError = 1;

} // namespace

// Every other winmm export, as a PE forwarder straight to the real DLL.
//
// This is not optional politeness. A proxy takes over the base name
// "winmm.dll" for the entire process, and the loader binds every later module's
// winmm imports against it. SteamVR's vrclient_x64.dll imports timeSetEvent and
// timeKillEvent; with those missing the import bind failed, the vrclient load
// was rejected, and OpenVR reported VRInitError_Init_VRClientDLLNotFound (102)
// -- which looked for all the world like a broken SteamVR install.
//
// Generated by tools\gen_winmm_forwards.ps1. See tools\proxyvrtest.cpp for the
// regression test.
#include "winmm_forwards.inc"

// --- thunked exports --------------------------------------------------------
// The exact ten tomb456.exe imports, confirmed with dumpbin /IMPORTS.

extern "C" {

typedef DWORD (WINAPI* PFN_timeGetTime)(void);
__declspec(dllexport) DWORD WINAPI timeGetTime(void) {
    static PFN_timeGetTime fn = Resolve<PFN_timeGetTime>("timeGetTime");
    return fn ? fn() : GetTickCount();
}

typedef UINT (WINAPI* PFN_timeBeginPeriod)(UINT);
__declspec(dllexport) UINT WINAPI timeBeginPeriod(UINT uPeriod) {
    static PFN_timeBeginPeriod fn = Resolve<PFN_timeBeginPeriod>("timeBeginPeriod");
    return fn ? fn(uPeriod) : kMMError;
}

typedef UINT (WINAPI* PFN_timeEndPeriod)(UINT);
__declspec(dllexport) UINT WINAPI timeEndPeriod(UINT uPeriod) {
    static PFN_timeEndPeriod fn = Resolve<PFN_timeEndPeriod>("timeEndPeriod");
    return fn ? fn(uPeriod) : kMMError;
}

typedef UINT (WINAPI* PFN_timeGetDevCaps)(void*, UINT);
__declspec(dllexport) UINT WINAPI timeGetDevCaps(void* ptc, UINT cbtc) {
    static PFN_timeGetDevCaps fn = Resolve<PFN_timeGetDevCaps>("timeGetDevCaps");
    return fn ? fn(ptc, cbtc) : kMMError;
}

typedef UINT (WINAPI* PFN_waveOutOpen)(void*, UINT, const void*, DWORD_PTR, DWORD_PTR, DWORD);
__declspec(dllexport) UINT WINAPI waveOutOpen(void* phwo, UINT uDeviceID, const void* pwfx,
                                              DWORD_PTR dwCallback, DWORD_PTR dwInstance,
                                              DWORD fdwOpen) {
    static PFN_waveOutOpen fn = Resolve<PFN_waveOutOpen>("waveOutOpen");
    return fn ? fn(phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen) : kMMError;
}

typedef UINT (WINAPI* PFN_waveOutClose)(void*);
__declspec(dllexport) UINT WINAPI waveOutClose(void* hwo) {
    static PFN_waveOutClose fn = Resolve<PFN_waveOutClose>("waveOutClose");
    return fn ? fn(hwo) : kMMError;
}

typedef UINT (WINAPI* PFN_waveOutReset)(void*);
__declspec(dllexport) UINT WINAPI waveOutReset(void* hwo) {
    static PFN_waveOutReset fn = Resolve<PFN_waveOutReset>("waveOutReset");
    return fn ? fn(hwo) : kMMError;
}

typedef UINT (WINAPI* PFN_waveOutWrite)(void*, void*, UINT);
__declspec(dllexport) UINT WINAPI waveOutWrite(void* hwo, void* pwh, UINT cbwh) {
    static PFN_waveOutWrite fn = Resolve<PFN_waveOutWrite>("waveOutWrite");
    return fn ? fn(hwo, pwh, cbwh) : kMMError;
}

typedef UINT (WINAPI* PFN_waveOutPrepareHeader)(void*, void*, UINT);
__declspec(dllexport) UINT WINAPI waveOutPrepareHeader(void* hwo, void* pwh, UINT cbwh) {
    static PFN_waveOutPrepareHeader fn = Resolve<PFN_waveOutPrepareHeader>("waveOutPrepareHeader");
    return fn ? fn(hwo, pwh, cbwh) : kMMError;
}

typedef UINT (WINAPI* PFN_waveOutUnprepareHeader)(void*, void*, UINT);
__declspec(dllexport) UINT WINAPI waveOutUnprepareHeader(void* hwo, void* pwh, UINT cbwh) {
    static PFN_waveOutUnprepareHeader fn =
        Resolve<PFN_waveOutUnprepareHeader>("waveOutUnprepareHeader");
    return fn ? fn(hwo, pwh, cbwh) : kMMError;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
