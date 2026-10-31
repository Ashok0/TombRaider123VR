// Isolated native menu input and INI persistence checks; no game/VR runtime.
#include "../src/Config.cpp"
#include "../src/Engine.h"
#include "../src/VRMenuModel.h"
#include "../src/VRNativeMenuModel.h"
#include <fstream>
#include <limits>
#include <cstdio>
namespace tr {
uint64_t Base() { return 0; }
int AppFlag(uint32_t) { return 0; }
const Layout& L() { static Layout layout{};return layout; }
}
int main() {
    using namespace tr::vrmenu;
    int n=0;auto check=[&](bool yes,const char* why) { ++n;if(!yes) { std::printf("FAIL: %s\n",why);std::exit(1); } };
    wchar_t cwd[MAX_PATH]{};GetCurrentDirectoryW(MAX_PATH,cwd);
    const std::wstring ini=std::wstring(cwd)+L"\\vr-menu-test.ini";
    { std::ofstream f("vr-menu-test.ini");f<<"; keep comment\n[VR]\nValveIndexControllers=0\nValveIndexGripPress=0.63\nValveIndexGripRelease=0.31\nDpadShift=0\nFirstPersonTurnDegreesPerSecond=123\nFirstPersonMotionGunPitchDegrees=-17\n[Other]\nCustom=77\n"; }
    tr::LoadConfig(ini.c_str());
    check(SettingCount==1 && std::wstring(Settings()[0].label)==L"Controller Type","only Controller Type is exposed");
    auto original=ReadSettings(),next=original;std::wstring error;next[0]=1;
    check(SaveSettings(next,original,error),"Index selection saves");
    check(ReadSettings()[0]==1,"Index selection persists");
    check(GetFileAttributesW((ini+L".vr-options.bak").c_str())!=INVALID_FILE_ATTRIBUTES,"previous INI backed up");
    std::ifstream f("vr-menu-test.ini");std::string contents((std::istreambuf_iterator<char>(f)),{});f.close();
    for(const auto* value:{"keep comment","ValveIndexGripPress=0.63","ValveIndexGripRelease=0.31","DpadShift=0","FirstPersonTurnDegreesPerSecond=123","FirstPersonMotionGunPitchDegrees=-17","Custom=77"})
        check(contents.find(value)!=std::string::npos,"hidden and unrelated settings remain unchanged");
    original=ReadSettings();next[0]=0;check(SaveSettings(next,original,error) && ReadSettings()[0]==0,"Touch selection saves with an existing backup");
    original=ReadSettings();next[0]=.5f;check(!SaveSettings(next,original,error),"non-boolean controller type rejected");
    next[0]=std::numeric_limits<float>::quiet_NaN();check(!SaveSettings(next,original,error),"nonfinite controller type rejected");
    check(ReadSettings()==original,"rejected writes preserve INI");
    ControllerPage page;
    check(page.Input(0x100000)==Action::None,"closed page leaves native Select alone");
    page.Open(false);check(std::string(page.Value())=="Meta Touch","Touch name uses requested text");
    check(page.Input(0x40000000)==Action::Change,"native Left adjusts controller type");
    check(page.Input(0x80000000)==Action::Change,"native Right adjusts controller type");
    check(page.Input(0x100000)==Action::Change,"native Select adjusts controller type");
    check(page.Input(0x30000000)==Action::None,"one-setting page has no other selectable settings");
    check(page.Input(0x30100000)==Action::Change,"Select with vertical input changes only Controller Type");
    check(page.Input(0x300000)==Action::Back && !page.open,"Back takes precedence and returns one level");
    page.Open(true);check(std::string(page.Value())=="Valve Index","Index name uses requested text");
    OptionsSelection selection;uint64_t input=0x20000000;
    check(!selection.Input(12,input) && selection.vr && input==0,"Down after native setting 12 selects VR Options");
    input=0x40000000;check(!selection.Input(12,input) && input==0,"VR row cannot toggle underlying native setting");
    input=0x100000;check(selection.Input(12,input) && input==0,"VR Select opens child without native action");
    input=0x10000000;selection.Input(12,input);check(!selection.vr && input==0,"Up returns to native setting 12 without skipping it");
    for(int choice=1;choice<=12;++choice) { input=0x100000;check(!selection.Input(choice,input) && input==0x100000,"all original native actions remain available"); }
    input=0x20100000;selection.Input(12,input);check(selection.vr && input==0,"Down plus Select cannot activate hidden native action");
    input=0x200000;selection.Input(12,input);check(input==0x200000,"parent Back remains native");
    input=0;selection.Input(1,input);check(!selection.vr,"returning to a reset Options list clears virtual selection");
    std::printf("PASS: %d Controller Type checks\n",n);
}
