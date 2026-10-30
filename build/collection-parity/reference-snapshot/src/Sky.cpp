#include "Sky.h"
#include "GameDll.h"
#include "Config.h"
#include "InlineHook.h"
#include "Log.h"

#include <cstdint>

namespace tr {
namespace {

hook::InlineHook g_hDrawSkyHD;

typedef void (__cdecl* Fn_DrawSkyHD)(void);

// The expected prologue travels with the row: `mov [rsp+N], rbx`, 5 bytes, PIC,
// instruction-aligned, but N is 8 in the debug DLLs and 0x18 in retail.
constexpr size_t kDrawSkyHDPrologueLen = 5;

const GameDllLayout* g_boundDll  = nullptr;
uint64_t             g_boundBase = 0;
uint64_t             g_failedBase = 0;

bool g_inSky       = false;
bool g_loggedFirst = false;

void __cdecl Detour_DrawSkyHD() {
    g_inSky = true;
    g_hDrawSkyHD.Original<Fn_DrawSkyHD>()();
    g_inSky = false;
}

bool Install(const GameDllLayout& d, uint64_t base) {
    if (d.drawSkyHD == 0 || !d.drawSkyHDPrologue) return false;
    return g_hDrawSkyHD.Install(
        reinterpret_cast<void*>(base + d.drawSkyHD),
        reinterpret_cast<void*>(&Detour_DrawSkyHD),
        5, d.drawSkyHDPrologue, kDrawSkyHDPrologueLen,
        "DrawSkyHD");
}

void Remove() {
    g_inSky = false;
    g_hDrawSkyHD.Remove();
    g_boundDll  = nullptr;
    g_boundBase = 0;
}

} // namespace

void SkyUpdate() {
    const GameDllLayout* d = GameDllBound();
    const uint64_t base = GameDllBase();

    const bool want = Cfg().enabled && Cfg().skyAtInfinity && d && base
                   && d->drawSkyHD != 0;

    // Rebind whenever the DLL or its base changes, same as PortalCullUpdate --
    // the player can switch games from the title screen without restarting.
    if (g_boundDll && (!want || d != g_boundDll || base != g_boundBase)) {
        LogF("sky: unhooking %S", g_boundDll->module);
        Remove();
    }
    if (!want || g_boundDll) return;
    if (base == g_failedBase) return;

    g_boundDll  = d;
    g_boundBase = base;
    if (!Install(*d, base)) {
        LogF("sky: DrawSkyHD could not be hooked in %S -- the sky keeps its "
             "mesh-radius stereo depth. A prologue mismatch here means the "
             "game was patched; re-run tools\\verify_addresses.py.", d->module);
        Remove();
        g_failedBase = base;
        return;
    }
    if (!g_loggedFirst) {
        g_loggedFirst = true;
        LogF("sky: hooked %S (%s) -- sky draws use rotation-only eye transform "
             "(optical infinity) and far-plane depth", d->module, d->name);
    } else {
        LogF("sky: hooked %S (%s)", d->module, d->name);
    }
}

void SkyShutdown() {
    if (g_boundDll) Remove();
    g_loggedFirst = false;
    g_failedBase  = 0;
}

bool SkyPassActive() {
    return g_inSky;
}

} // namespace tr
