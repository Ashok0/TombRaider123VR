#include "CollectionParity.h"
#include "Config.h"
#include "Engine.h"
#include "FirstPerson.h"
#include "GameDll.h"
#include "Gamepad.h"
#include "InlineHook.h"
#include "VRSystem.h"
#include "GL.h"
#include "Log.h"
#include <cstring>

namespace tr {
namespace collection {
struct HookSite {
    uint32_t address=0;unsigned span=0;uint8_t bytes[32]{};
    unsigned relocationCount=0;int relocations[2]{};
    bool Install(hook::InlineHook& hook,uint64_t base,void* detour,const char* name) const {
        return address && hook.Install(reinterpret_cast<void*>(base+address),detour,span,
            bytes,span,name,relocations,relocationCount);
    }
};
struct ExeBuild {
    uint32_t timestamp;HookSite update,messages;uint32_t active,gate;
    unsigned gateLength;uint8_t gateBytes[32];
};
struct DllBuild {
    int game;uint32_t timestamp;HookSite background,initialize,flare;
    uint32_t rooms,flush,dynamics,numberDynamics,app;
};
#include "CollectionAddresses.inl"

hook::InlineHook updateHook,messagesHook,backgroundHook,initializeHook,flareHook;
bool focusAttempted=false,drawing=false;
int32_t* windowActive=nullptr;
const GameDllLayout* layout=nullptr;
const DllBuild* build=nullptr;
uint64_t base=0,failedBase=0;
template<class T> T* Ptr(uint32_t rva) { return reinterpret_cast<T*>(base+rva); }

bool PollingAllowed() {
    return Cfg().enabled && Cfg().gamepadEnabled && VR().active() && CurrentGame()>=0 && CurrentGame()<3;
}
struct FocusScope {
    int32_t saved=0;bool overridden=false;
    void Restore() { if(overridden) *windowActive=saved;overridden=false; }
    void AfterMessages() {
        if(windowActive && PollingAllowed()) { saved=*windowActive;*windowActive=1;overridden=true; }
    }
    ~FocusScope() { Restore(); }
};
FocusScope* focusScope=nullptr;
void __cdecl Messages() {
    if(focusScope) focusScope->Restore();
    messagesHook.Original<void (__cdecl*)()>()();
    if(focusScope) focusScope->AfterMessages();
}
void __cdecl InputUpdate() {
    FocusScope scope;auto* previous=focusScope;focusScope=&scope;
    updateHook.Original<void (__cdecl*)()>()();focusScope=previous;
}
void InstallFocus() {
    if(focusAttempted || !Base()) return;
    focusAttempted=true;
    for(const auto& row:exeBuilds) if(row.timestamp==L().timestamp) {
        if(std::memcmp(reinterpret_cast<void*>(Base()+row.gate),row.gateBytes,row.gateLength)) break;
        if(row.messages.Install(messagesHook,Base(),reinterpret_cast<void*>(&Messages),"VRPadMessages") &&
           row.update.Install(updateHook,Base(),reinterpret_cast<void*>(&InputUpdate),"VRPadUpdate")) {
            windowActive=reinterpret_cast<int32_t*>(Base()+row.active);
            Log("pad: verified background VR controller polling installed");return;
        }
        updateHook.Remove();messagesHook.Remove();break;
    }
    Log("pad: background polling hook verification failed; native behavior retained");
}

// Native DYNAMIC is 20 bytes in both TR2 and TR3. Keep the exact allocated
// record, including its brightness/flicker, and never animate a recycled slot.
struct FlareLight {
    int index=-1;uint8_t snapshot[20]{};uint64_t updated=0;
} light;
void RefreshFlareLight() {
    if(!build || !build->dynamics || light.index<0 || GetTickCount64()-light.updated>150) return;
    const int count=*Ptr<int32_t>(build->numberDynamics);
    if(light.index>=count || count>64) { light={};return; }
    auto* record=Ptr<uint8_t>(build->dynamics)+20*light.index;
    if(std::memcmp(record,light.snapshot,20)) { light={};return; }
    int32_t tip[3]{};
    if(!FirstPersonFlareTip(tip)) { light={};return; }
    std::memcpy(record,tip,12);std::memcpy(light.snapshot,record,20);
}
int __cdecl Flare(int32_t* point,int age,int held) {
    int32_t tracked[3]{};
    const bool follow=held && FirstPersonFlareTip(tracked);
    const int before=build && build->numberDynamics ? *Ptr<int32_t>(build->numberDynamics) : -1;
    const int result=flareHook.Original<int (__cdecl*)(int32_t*,int,int)>()(follow ? tracked : point,age,held);
    if(follow && before>=0 && before<64 && *Ptr<int32_t>(build->numberDynamics)>before) {
        light.index=before;light.updated=GetTickCount64();
        auto* record=Ptr<uint8_t>(build->dynamics)+20*before;
        // Native flare lighting offsets its point upwards; the tracked tip is
        // already the rendered light origin and must not receive that bias.
        std::memcpy(record,tracked,12);std::memcpy(light.snapshot,record,20);
    } else if(held) light={};
    return result;
}
void __cdecl Initialize(int clear) {
    initializeHook.Original<void (__cdecl*)(int)>()(clear);
    RefreshFlareLight(); // Camera is current, before room/object light collection.
}
bool SceneAvailable() {
    if(!layout || layout!=GameDllBound() || base!=GameDllBase() || !build || !base || !WorldInventoryActive() || !Cfg().worldInventory || !InventorySceneCameraReady()) return false;
    const int count=*Ptr<int16_t>(layout->numberRooms),room=*Ptr<int16_t>(layout->camera+12);
    return count>0 && count<=1024 && room>=0 && room<count && *Ptr<void*>(layout->room) &&
        build->app && *Ptr<void*>(build->app);
}
void __cdecl PreserveBackground(int) {}
struct SceneScope {
    SceneScope() { drawing=true; }
    ~SceneScope() {
        drawing=false;
        // TR1-3 initialization ALWAYS clears colour and depth (its argument
        // selects the clear colour). Rebuild the native menu camera, viewport,
        // projection, fog and matrix stack, preserving the just-rendered world.
        // APP::clear is used synchronously on this render thread. Restore the
        // callback before returning to any native menu drawing.
        using Clear=void (__cdecl*)(int);
        auto* clear=reinterpret_cast<Clear*>(*Ptr<uint8_t*>(build->app)+0x138);
        const auto original=*clear;
        *clear=&PreserveBackground;
        reinterpret_cast<void (__cdecl*)(int)>(base+build->initialize.address)(1);
        *clear=original;
    }
};
void ClearMenuDepth() {
    const GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);GLboolean mask=GL_FALSE;
    glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);glClear(GL_DEPTH_BUFFER_BIT);glDepthMask(mask);
    if(scissor) glEnable(GL_SCISSOR_TEST);
}
bool DrawBackground() {
    if(drawing || !SceneAvailable()) return false;
    {
        SceneScope scope;
        // Initialize the gameplay camera and clear its target, without simulation.
        reinterpret_cast<void (__cdecl*)(int)>(base+build->initialize.address)(0);
        auto* rooms=*Ptr<uint8_t*>(layout->room);
        const int count=*Ptr<int16_t>(layout->numberRooms);
        for(int i=0;i<count;++i) { rooms[i*168+78]=0;rooms[i*168+79]=0; }
        int16_t room=*Ptr<int16_t>(layout->camera+12);
        if(layout->getFloor && layout->w2vMatrix) {
            const auto* camera=Ptr<int32_t>(layout->w2vMatrix);
            reinterpret_cast<void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*)>(base+layout->getFloor)(
                camera[3],camera[7],camera[11],&room);
        }
        if(room<0 || room>=count) return false;
        reinterpret_cast<void (__cdecl*)(int)>(base+build->rooms)(room);
        // S_CmdFlush submits queued geometry only. S_OutputPolyList also presents,
        // polls messages and advances frame timing: it must run only at native end
        // of frame, after the menu has been drawn.
        reinterpret_cast<void (__cdecl*)()>(base+build->flush)();
    }
    ClearMenuDepth();
    return true;
}
void __cdecl BufferBackground(int opacity) {
    if(!DrawBackground()) backgroundHook.Original<void (__cdecl*)(int)>()(opacity);
}
void __cdecl InventoryBackground() {
    if(!DrawBackground()) backgroundHook.Original<void (__cdecl*)()>()();
}
void ResetDll() {
    backgroundHook.Remove();initializeHook.Remove();flareHook.Remove();
    layout=nullptr;build=nullptr;base=0;drawing=false;light={};
}
} // namespace collection

bool InventorySceneDrawing() { return collection::drawing; }
bool InventorySceneReady() { return collection::backgroundHook.installed() &&
    collection::layout==GameDllBound() && collection::base==GameDllBase(); }
bool CollectionFlareReady() { return collection::flareHook.installed() &&
    collection::layout==GameDllBound() && collection::base==GameDllBase(); }
void CollectionUpdate() {
    using namespace collection;
    if(!Cfg().enabled) { CollectionShutdown();return; }
    InstallFocus();
    const auto* current=GameDllBound();const auto address=GameDllBase();
    if(layout && (layout!=current || base!=address)) ResetDll();
    if(!current || !address || layout || failedBase==address) return;
    for(const auto& row:dllBuilds) if(row.timestamp==current->timestamp && row.game==current->module[4]-L'0') {
        layout=current;base=address;build=&row;
        if(!row.initialize.Install(initializeHook,base,reinterpret_cast<void*>(&Initialize),"VRSceneInitialize") ||
           !row.background.Install(backgroundHook,base,row.game==2 ? reinterpret_cast<void*>(&InventoryBackground) :
                reinterpret_cast<void*>(&BufferBackground),"InventoryWorldBackground")) {
            failedBase=address;ResetDll();Log("collection: verified scene hooks unavailable");return;
        }
        if(row.game!=1 && !row.flare.Install(flareHook,base,reinterpret_cast<void*>(&Flare),"ControllerFlareLight"))
            Log("collection: flare hook verification failed; native flare retained");
        LogF("collection: %s spatial inventory and held-flare hooks ready",current->name);
        return;
    }
}
void CollectionShutdown() {
    using namespace collection;
    ResetDll();updateHook.Remove();messagesHook.Remove();
    windowActive=nullptr;focusScope=nullptr;focusAttempted=false;failedBase=0;
}
}
