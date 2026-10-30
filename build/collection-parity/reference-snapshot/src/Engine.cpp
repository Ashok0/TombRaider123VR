#include "Engine.h"
#include "Log.h"

#include <windows.h>

namespace tr {
namespace {

uint64_t      g_base   = 0;
const Layout* g_layout = nullptr;

// Every build there is an address table for.
const Layout* const kBuilds[] = { &kBuildStock, &kBuildHD1, &kBuildHD2 };

// Identify the host binary and pick its address table.
//
// Matching on the PE TimeDateStamp rather than a file name means a renamed or
// copied exe is still recognised -- the HD pack installs over tomb456.exe, so
// the name says nothing -- and an unknown build is refused outright instead of
// being patched with addresses belonging to a different one.
const Layout* IdentifyBuild(uint64_t base) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return nullptr;

    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    const Layout*  hit   = nullptr;
    for (const Layout* b : kBuilds) {
        if (b->timestamp == stamp) { hit = b; break; }
    }
    if (!hit) {
        LogF("engine: unknown build (PE timestamp 0x%08X). Supported builds:", stamp);
        for (const Layout* b : kBuilds)
            LogF("engine:   0x%08X  %s", b->timestamp, b->name);
        return nullptr;
    }

    // The distinctive feature of these builds: a single ~232 MB .data section.
    // Every global we touch lives inside it, so if it is not there we would be
    // writing into unmapped memory.
    const DWORD needed = hit->FBO_custom + 0x1000;
    if (nt->OptionalHeader.SizeOfImage < needed) {
        LogF("engine: SizeOfImage 0x%X is too small for our RVAs (need 0x%X)",
             nt->OptionalHeader.SizeOfImage, needed);
        return nullptr;
    }
    return hit;
}

} // namespace

bool Bind() {
    if (g_base) return true;

    // The name is only a fast path. The HD pack installs over tomb456.exe and
    // either build may be renamed, so GetModuleHandle(nullptr) -- the host
    // executable whatever it is called -- is the reliable answer, and the build
    // is identified from its PE timestamp below rather than its name.
    HMODULE h = GetModuleHandleW(L"tomb456.exe");
    if (!h) h = GetModuleHandleW(nullptr);
    if (!h) {
        Log("engine: GetModuleHandle failed");
        return false;
    }

    const uint64_t base   = reinterpret_cast<uint64_t>(h);
    const Layout*  layout = IdentifyBuild(base);
    if (!layout) {
        Log("engine: host module is not a supported build; refusing to patch");
        return false;
    }

    g_base   = base;
    g_layout = layout;
    LogF("engine: bound to %p (reference base %p, slide %+lld)",
         reinterpret_cast<void*>(base),
         reinterpret_cast<void*>(kReferenceImageBase),
         static_cast<long long>(base - kReferenceImageBase));
    LogF("engine: build identified as %s", layout->name);
    return true;
}

uint64_t Base() { return g_base; }

const Layout& L() { return g_layout ? *g_layout : kBuildStock; }

RenderState& VidState()     { return *reinterpret_cast<RenderState*>(Var(L().vid_state)); }
RenderState& VidStatePrev() { return *reinterpret_cast<RenderState*>(Var(L().vid_state_prev)); }
mat4*        Proj()         { return  reinterpret_cast<mat4*>(Var(L().mProj)); }
mat4&        ViewPacked()   { return *reinterpret_cast<mat4*>(Var(L().mView_packed)); }
Shader*      Shaders()      { return  reinterpret_cast<Shader*>(Var(L().shaders)); }
uint32_t*    OglTextures()  { return  reinterpret_cast<uint32_t*>(Var(L().ogl_textures)); }
uint32_t&    FboCustom()    { return *reinterpret_cast<uint32_t*>(Var(L().FBO_custom)); }
uint32_t&    FboDefault()   { return *reinterpret_cast<uint32_t*>(Var(L().FBO_default)); }
OglRenderTarget& Rt()       { return *reinterpret_cast<OglRenderTarget*>(Var(L().ogl_rt)); }
int32_t&     ScreenWidth()  { return *reinterpret_cast<int32_t*>(Var(L().gWidth)); }
int32_t&     ScreenHeight() { return *reinterpret_cast<int32_t*>(Var(L().gHeight)); }
int32_t&     TargetWidth()  { return *reinterpret_cast<int32_t*>(Var(L().gTargetWidth)); }
int32_t&     TargetHeight() { return *reinterpret_cast<int32_t*>(Var(L().gTargetHeight)); }

bool IsWorldPass() {
    return VidState().proj == &Proj()[1];
}


int CurrentGame() {
    if (!g_base) return -1;
    return *reinterpret_cast<int32_t*>(Var(L().gGame));
}

void* XInputGetStateSlot() {
    if (!g_base) return nullptr;
    return reinterpret_cast<void*>(Var(L().XInputGetState));
}

} // namespace tr
