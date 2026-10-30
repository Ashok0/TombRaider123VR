#include "InventoryScene.h"
#include "Gamepad.h"
#include "GameDll.h"
#include "Engine.h"
#include "Config.h"
#include "InlineHook.h"
#include "GL.h"
#include "Log.h"
#include <cstring>

namespace tr {
namespace inventoryscene {
struct Build {
    uint32_t timestamp, background, rooms, flush, app;
    uint8_t roomsHome;
};
// PDB symbols and retail call-site matches; independently checked by
// tools/verify_inventory_scene.py. No gameplay tick or present is called.
constexpr Build builds[]={
    {0x696B4999,0xD1300,0x2F910,0xB1B50,0x663F08,0x08},
    {0x696B499C,0xC45D0,0x30860,0xA5460,0x66D1A0,0x10},
    {0x68C12FDA,0xD1E60,0x2F780,0xB26C0,0x664E48,0x08},
    {0x68C12FE9,0xC4870,0x30CE0,0xA5750,0x66D0E0,0x10}
};
hook::InlineHook backgroundHook;
const GameDllLayout* layout=nullptr;
uint64_t base=0, failedBase=0;
const Build* build=nullptr;
bool drawing=false;
unsigned frames=0;

template<class T> T* Ptr(uint32_t rva) { return reinterpret_cast<T*>(base+rva); }

bool SceneAvailable() {
    if(!layout || !base || !build || !WorldInventoryActive()) return false;
    const int count=*Ptr<int16_t>(layout->numberRooms);
    const int room=*Ptr<int16_t>(layout->camera+12);
    return count>0 && count<=1024 && room>=0 && room<count &&
        *Ptr<uint8_t*>(layout->room)!=nullptr;
}

void ClearMenuDepth() {
    // The level and menu have different camera origins. Keep scene colour,
    // but start a fresh depth layer for the inventory's own 3D meshes. Clear
    // BOTH eye halves, then restore GL state so engine state caching agrees.
    const GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
    GLboolean mask=GL_FALSE;
    glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthMask(mask);
    if(scissor) glEnable(GL_SCISSOR_TEST);
}

void __cdecl DrawBackground() {
    if(drawing || !SceneAvailable()) {
        backgroundHook.Original<void (__cdecl*)()>()();
        return;
    }
    // InventoryDrawPhase has already run S_InitialisePolyList: target, camera,
    // fog, matrix stacks and clear are ready. Replace S_DisplayMonoScreen's
    // captured picture with the same scene draw used by DrawPhaseGame.
    struct Scope {
        Scope() { drawing=true; }
        ~Scope() { drawing=false; }
    } scope;
    auto* rooms=*Ptr<uint8_t*>(layout->room);
    const int count=*Ptr<int16_t>(layout->numberRooms);
    for(int i=0;i<count;++i) {
        // DrawPhaseGame resets drawn and WaterPlaneDrawn before each frame.
        // Leaving these bits set makes rooms disappear on the second draw.
        auto& drawn=*reinterpret_cast<uint16_t*>(rooms+i*304+112);
        drawn &= 0xFFFCu;
    }
    const int room=*Ptr<int16_t>(layout->camera+12);
    reinterpret_cast<void (__cdecl*)(int)>(base+build->rooms)(room);
    reinterpret_cast<void (__cdecl*)()>(base+build->flush)();
    ClearMenuDepth();
    if(++frames==1)
        Log("inventory: live stereo level draw replaces native pause background; gameplay simulation remains paused");
}
} // namespace inventoryscene

bool InventorySceneReady() {
    using namespace inventoryscene;
    return backgroundHook.installed() && layout==GameDllBound() && base==GameDllBase();
}
bool InventorySceneDrawing() { return inventoryscene::drawing; }

void InventorySceneShutdown() {
    using namespace inventoryscene;
    backgroundHook.Remove();layout=nullptr;base=0;build=nullptr;drawing=false;frames=0;
}

void InventorySceneUpdate() {
    using namespace inventoryscene;
    const auto* dll=GameDllBound();
    const auto currentBase=GameDllBase();
    if(layout && (layout!=dll || base!=currentBase || !Cfg().enabled || !Cfg().worldInventory))
        InventorySceneShutdown();
    if(!Cfg().enabled || !Cfg().worldInventory || !dll || !currentBase || layout || currentBase==failedBase) return;
    const Build* found=nullptr;
    for(const auto& row:builds) if(row.timestamp==dll->timestamp) { found=&row;break; }
    if(!found) return;
    uint8_t roomsBytes[]={0x48,0x89,0x5c,0x24,found->roomsHome};
    const uint8_t flushBytes[]={0x40,0x53,0x55,0x56,0x57,0x41,0x54};
    uint8_t backgroundBytes[]={0x48,0x83,0xec,0x48,0x48,0x8b,0x05,0,0,0,0};
    const int32_t appDisp=int32_t(found->app-(found->background+11));
    std::memcpy(backgroundBytes+7,&appDisp,4);
    const int rip=7;
    if(std::memcmp(reinterpret_cast<void*>(currentBase+found->rooms),roomsBytes,sizeof(roomsBytes)) ||
       std::memcmp(reinterpret_cast<void*>(currentBase+found->flush),flushBytes,sizeof(flushBytes)) ||
       !backgroundHook.Install(reinterpret_cast<void*>(currentBase+found->background),
           reinterpret_cast<void*>(&DrawBackground),11,backgroundBytes,11,"InventoryWorldBackground",&rip,1)) {
        failedBase=currentBase;
        Log("inventory: level background hook unavailable for this binary; native background retained");
        return;
    }
    layout=dll;base=currentBase;build=found;frames=0;
    LogF("inventory: %s live world background ready",dll->name);
}
} // namespace tr
