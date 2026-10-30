#include "TR4Effects.h"
#include "TR4EffectsGL.h"
#include "Config.h"
#include "Engine.h"
#include "InlineHook.h"
#include "Log.h"
#include <new>

namespace tr {
namespace {
hook::InlineHook g_effectTextureLoad,g_effectTextureUpdate;
using Fn_Load=int32_t (__cdecl*)(const char*,int32_t,int32_t);
using Fn_Update=void (__cdecl*)(int32_t,int32_t,int32_t,const void*);
struct EffectsBuild { uint32_t timestamp,load,update; };
constexpr EffectsBuild kEffectsBuilds[]={
    {0x696B49A7,0xB1D0,0x14200},
    {0x68639C21,0xB490,0x14570}, {0x68C12FEB,0xB490,0x14570}
};
constexpr uint8_t kLoadPrologue[]={0x40,0x53,0x55,0x56,0x57};
constexpr uint8_t kUpdatePrologue[]={0x48,0x89,0x5C,0x24,0x08};
struct LoadScope {
    effects::Texture kind=effects::Texture::None;
    int index=0,layer=0,enhanced=0;
};
thread_local LoadScope* g_loading=nullptr;
struct ScopedLoad {
    LoadScope* previous;
    explicit ScopedLoad(LoadScope& scope):previous(g_loading) { g_loading=&scope; }
    ~ScopedLoad() { g_loading=previous; }
};

void __cdecl Detour_EffectTextureUpdate(int32_t index,int32_t layer,int32_t mip,const void* data) {
    const auto original=g_effectTextureUpdate.Original<Fn_Update>();
    std::vector<uint8_t> compressed;
    if(g_loading && g_loading->kind!=effects::Texture::None &&
        index==g_loading->index && layer==g_loading->layer && index>=0 && index<38) {
        try {
            const auto kind=g_loading->kind;
            if(effects::PrepareUpload(OglTextures()[index],layer,mip,kind,
                kind==effects::Texture::Sunrays?Cfg().tr4SunrayStrength:Cfg().tr4EffectStrength,
                data,compressed)) {
                data=compressed.data();++g_loading->enhanced;
            }
        } catch(const std::bad_alloc&) {
            Log("TR4 effects: scratch allocation failed; retaining native mip");
        }
    }
    // Exactly one native upload, with the same array index, layer and mip.
    original(index,layer,mip,data);
}
int32_t __cdecl Detour_EffectTextureLoad(const char* name,int32_t index,int32_t layer) {
    LoadScope scope;
    if(Cfg().enabled && Cfg().tr4Effects && name &&
        (!_stricmp(name,"TEX\\3115.DDS") || !_stricmp(name,"TEX\\EFFECT.DDS"))) {
        using GetPath=void (__cdecl*)(const char*,char*);
        const auto getPath=*reinterpret_cast<GetPath*>(Base()+L().app);
        char resolved[64]{};
        if(getPath) getPath(name,resolved);
        scope.kind=effects::Identify(resolved);
        scope.index=index;scope.layer=layer;
    }
    ScopedLoad active(scope);
    const int32_t result=g_effectTextureLoad.Original<Fn_Load>()(name,index,layer);
    if(scope.kind!=effects::Texture::None)
        LogF("TR4 effects: %s array %d layer %d: %d enhanced mips, load %s",name,index,layer,
            scope.enhanced,result?"OK":"failed");
    return result;
}
}
void TR4EffectsInstall() {
    if(!Cfg().enabled || !Cfg().tr4Effects) return;
    for(const auto& build:kEffectsBuilds) if(L().timestamp==build.timestamp) {
        if(!g_effectTextureUpdate.Install(Fn(build.update),reinterpret_cast<void*>(&Detour_EffectTextureUpdate),
            5,kUpdatePrologue,5,"TR4 ogl_texUpdate")) return;
        if(!g_effectTextureLoad.Install(Fn(build.load),reinterpret_cast<void*>(&Detour_EffectTextureLoad),
            5,kLoadPrologue,5,"TR4 vidLoadTexture")) {
            g_effectTextureUpdate.Remove();return;
        }
        Log("TR4 effects: native sunray/atlas upload enhancement ready (TR4 resources only)");
        return;
    }
}
void TR4EffectsShutdown() { g_effectTextureLoad.Remove();g_effectTextureUpdate.Remove(); }
}
