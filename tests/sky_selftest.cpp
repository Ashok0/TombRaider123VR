// Actual TR1-3 sky hooks with deferred draw submission, no full self-test.
#include <cstdint>
#include <cstddef>
#define private public
#include "../src/InlineHook.h"
#undef private
#include "../src/Sky.cpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <utility>
namespace {
int checks=0;
void Check(bool ok,const char* label) { ++checks;if(!ok) { std::printf("FAIL: %s\n",label);std::exit(1); } }
tr::Config settings;
std::vector<int> queued;
std::vector<std::pair<int,bool>> submitted;
void __cdecl Flush() { for(int mesh:queued) submitted.push_back({mesh,tr::SkyPassActive()});queued.clear(); }
void __cdecl NativeMesh(void* mesh,int32_t clip) { Check(clip==7,"native mesh argument intact");queued.push_back(*static_cast<int*>(mesh)); }
void __cdecl NativeHD() { Check(tr::SkyPassActive(),"HD sky remains scoped"); }
}
void Log(const char*) {}
void LogF(const char*,...) {}
namespace tr {
const Config& Cfg() { return settings; }
const GameDllLayout* GameDllBound() { return nullptr; }
uint64_t GameDllBase() { return 0; }
}
namespace hook {
InlineHook::~InlineHook() {}
void InlineHook::Remove() { m_trampoline=nullptr; }
bool InlineHook::Install(void*,void*,size_t,const uint8_t*,size_t,const char*,const int*,size_t) { return true; }
}
int main() {
    using namespace tr;
    const auto base=reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    sky::Binding binding{};binding.flush=uint32_t(reinterpret_cast<uint64_t>(&Flush)-base);binding.skyReturn=0x1234;
    g_boundBase=base;g_classic=&binding;g_hClassicMesh.m_trampoline=reinterpret_cast<void*>(&NativeMesh);
    g_hDrawSkyHD.m_trampoline=reinterpret_cast<void*>(&NativeHD);
    Check(!sky::Find(0) && !sky::Find(0x6A4B48FF) && !sky::Find(0x6AA1567E),"unknown builds and TR1 do not gain an invented Classic horizon hook");
    for(const auto& b:sky::Bindings) Check(sky::Find(b.timestamp)==&b && b.entry && b.flush && b.skyReturn,"all six TR2/3 builds covered");
    int horizon=2,ordinary=3;
    for(int eye:{0,1}) {
        (void)eye;submitted.clear();queued={1};g_inSky=false;
        DrawClassicMesh(&horizon,7,base+binding.skyReturn);
        DrawClassicMesh(&ordinary,7,base+binding.skyReturn+1);
        Check(queued==std::vector<int>{3},"ordinary mesh remains queued with native batching");Flush();
        Check(submitted==std::vector<std::pair<int,bool>>{{1,false},{2,true},{3,false}},"only the Classic horizon gets infinity depth, including deferred submission");
        Check(!SkyPassActive(),"Classic scope restored before next eye");
        Detour_DrawSkyHD();Check(!SkyPassActive(),"HD scope restored");
        g_inSky=true;Detour_DrawSkyHD();Check(SkyPassActive(),"nested HD scope preserves enclosing sky state");g_inSky=false;
    }
    unsigned char call[5]={0x90,0,0,0,0};sky::Binding invalid{};invalid.skyReturn=5;
    Check(!InstallClassic(invalid,reinterpret_cast<uint64_t>(call)),"mismatched call opcode refuses patching");
    call[0]=0xe8;invalid.entry=6;
    Check(!InstallClassic(invalid,reinterpret_cast<uint64_t>(call)),"call to a different mesh routine refuses patching");
    Remove();Check(!SkyPassActive() && !g_classic && !g_boundBase,"unbind clears Classic state");
    std::printf("TR1-3 sky hooks: %d checks passed\n",checks);
}
