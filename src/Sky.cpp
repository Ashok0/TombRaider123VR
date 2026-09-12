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

// Identical in tomb1/2/3.dll, 5 bytes, PIC, instruction-aligned.
//   48 89 5C 24 08     mov [rsp+8], rbx
const uint8_t kDrawSkyHDPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

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
    if (d.drawSkyHD == 0) return false;
    return g_hDrawSkyHD.Install(
        reinterpret_cast<void*>(base + d.drawSkyHD),
        reinterpret_cast<void*>(&Detour_DrawSkyHD),
        5, kDrawSkyHDPrologue, sizeof(kDrawSkyHDPrologue),
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
