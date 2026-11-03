#include "Sky.h"
#include "SkyBindings.h"
#include <intrin.h>
#include <cstring>
#include "GameDll.h"
#include "Config.h"
#include "InlineHook.h"
#include "Log.h"

#include <cstdint>

namespace tr {
namespace {

hook::InlineHook g_hDrawSkyHD,g_hClassicMesh;
const sky::Binding* g_classic=nullptr;

typedef void (__cdecl* Fn_DrawSkyHD)(void);

// Identical in tomb1/2/3.dll, 5 bytes, PIC, instruction-aligned.
//   48 89 5C 24 08     mov [rsp+8], rbx
const uint8_t kDrawSkyHDPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

const GameDllLayout* g_boundDll  = nullptr;
uint64_t             g_boundBase = 0;
uint64_t             g_failedBase = 0;

bool g_inSky       = false;
bool g_loggedFirst = false;

struct SkyScope {
    bool previous=g_inSky;
    explicit SkyScope() { g_inSky=true; }
    ~SkyScope() { g_inSky=previous; }
};
void __cdecl Detour_DrawSkyHD() {
    const SkyScope scope;
    g_hDrawSkyHD.Original<Fn_DrawSkyHD>()();
}
using Fn_InsMesh=void(__cdecl*)(void*,int32_t);
void DrawClassicMesh(void* mesh,int32_t clip,uint64_t caller) {
    const auto original=g_hClassicMesh.Original<Fn_InsMesh>();
    if(caller!=g_boundBase+g_classic->skyReturn) { original(mesh,clip);return; }
    // ins_mesh queues geometry: flush preceding world work outside the scope,
    // then the horizon inside it. No ordinary object can inherit infinity depth.
    const auto flush=reinterpret_cast<Fn_DrawSkyHD>(g_boundBase+g_classic->flush);
    flush();
    const SkyScope scope;
    original(mesh,clip);
    flush();
}
void __cdecl Detour_InsMesh(void* mesh,int32_t clip) {
    DrawClassicMesh(mesh,clip,reinterpret_cast<uint64_t>(_ReturnAddress()));
}
bool InstallClassic(const sky::Binding& b,uint64_t base) {
    const auto* call=reinterpret_cast<const uint8_t*>(base+b.skyReturn-5);
    int32_t displacement=0;std::memcpy(&displacement,call+1,sizeof(displacement));
    if(*call!=0xE8 || base+b.skyReturn+displacement!=base+b.entry) {
        Log("sky: Classic horizon call-site mismatch; keeping native drawing");return false;
    }
    const uint8_t prologue[]={0x48,0x89,0x5C,0x24,0x18};
    return g_hClassicMesh.Install(reinterpret_cast<void*>(base+b.entry),
        reinterpret_cast<void*>(&Detour_InsMesh),5,prologue,sizeof(prologue),"Classic horizon mesh");
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
    g_hClassicMesh.Remove();g_classic=nullptr;
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
    g_classic=sky::Find(d->timestamp);
    if(g_classic) {
        if(InstallClassic(*g_classic,base))
            LogF("sky: Classic horizon in %S uses optical infinity",d->module);
        else g_classic=nullptr;
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
