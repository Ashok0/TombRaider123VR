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
    check(SettingCount==3 && std::wstring(Settings()[0].label)==L"Controller Type" &&
        std::wstring(Settings()[1].label)==L"Color Scheme" &&
        std::wstring(Settings()[2].label)==L"Hybrid Modern/Tank Controls","all three requested settings are exposed");
    check(!tr::Cfg().hybridModernTankControls && ReadSettings()[2]==0,"older INIs show hybrid controls Off by default");
    check(!tr::Cfg().vrBrighterColors && ReadSettings()[1]==0,"old INIs without a color key retain the original dark look");
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
    original=ReadSettings();next=original;next[1]=1;
    check(SaveSettings(next,original,error) && tr::Cfg().vrBrighterColors,"brighter colors apply immediately after saving");
    check(ReadSettings()[1]==1 && ReadSettings()[0]==original[0],"color persists without changing controller type");
    tr::LoadConfig(ini.c_str());check(tr::Cfg().vrBrighterColors,"brighter choice survives restart");
    original=ReadSettings();next=original;next[1]=0;
    const auto goodPath=tr::g_iniPath;tr::g_iniPath=goodPath+L".missing";
    check(!SaveSettings(next,original,error) && tr::Cfg().vrBrighterColors,"failed color save leaves live rendering unchanged");
    tr::g_iniPath=goodPath;
    check(SaveSettings(next,original,error) && !tr::Cfg().vrBrighterColors,"switching back restores the original dark presentation immediately");
    original=ReadSettings();next=original;next[1]=.5f;
    check(!SaveSettings(next,original,error) && !tr::Cfg().vrBrighterColors,"invalid color choice cannot change live rendering");
    tr::LoadConfig(ini.c_str());check(!tr::Cfg().vrBrighterColors,"dark choice survives restart");
    original=ReadSettings();next=original;next[2]=1;
    check(SaveSettings(next,original,error) && tr::Cfg().hybridModernTankControls,"hybrid On applies immediately after a successful save");
    check(ReadSettings()[2]==1 && ReadSettings()[0]==original[0] && ReadSettings()[1]==original[1],
        "hybrid choice persists without changing controller type or colors");
    tr::LoadConfig(ini.c_str());check(tr::Cfg().hybridModernTankControls,"hybrid On survives restart");
    original=ReadSettings();next=original;next[2]=0;tr::g_iniPath=goodPath+L".missing";
    check(!SaveSettings(next,original,error) && tr::Cfg().hybridModernTankControls,"failed hybrid save leaves live movement setting unchanged");
    tr::g_iniPath=goodPath;
    check(SaveSettings(next,original,error) && !tr::Cfg().hybridModernTankControls,"hybrid Off applies immediately");
    original=ReadSettings();next=original;next[2]=.5f;
    check(!SaveSettings(next,original,error) && !tr::Cfg().hybridModernTankControls,"invalid hybrid choice is rejected");
    tr::LoadConfig(ini.c_str());check(!tr::Cfg().hybridModernTankControls,"hybrid Off survives restart");
    ControllerPage page;
    check(page.Input(0x100000)==Action::None,"closed page leaves native Select alone");
    page.Open(false);check(std::string(page.Value())=="Meta Touch","Touch name uses requested text");
    check(page.Input(0x40000000)==Action::Change,"native Left adjusts controller type");
    check(page.Input(0x80000000)==Action::Change,"native Right adjusts controller type");
    check(page.Input(0x100000)==Action::Change,"native Select adjusts controller type");
    check(page.Input(0x30000000)==Action::None && page.selected==0,"opposite navigation cancels");
    check(page.Input(0x20100000)==Action::None && page.selected==1,"navigation plus Select cannot accidentally toggle colors");
    check(std::string(page.Value(1))=="Dark","dark colors are the menu default");
    check(page.Input(0x80000000)==Action::Change && page.selected==1,"Right changes the selected color row");
    check(std::string(page.Message())=="Colors apply immediately","color row explains immediate application");
    check(page.Input(0x10000000)==Action::None && page.selected==0,"Up returns to Controller Type");
    check(page.Input(0x10000000)==Action::None && page.selected==2,"Up from first row wraps to hybrid controls");
    check(std::string(page.Name(2))=="Hybrid Modern/Tank Controls" && std::string(page.Value(2))=="Off",
        "hybrid row uses the requested label and readable Off value");
    check(page.Input(0x80000000)==Action::Change && page.selected==2,"Right adjusts hybrid controls");
    check(page.Input(0x30000000)==Action::None && page.selected==2,"opposite directions do not move the third row");
    check(page.Input(0x20100000)==Action::None && page.selected==0,"Down wraps and cannot toggle when Select arrives with navigation");
    check(page.Input(0x20000000)==Action::None && page.selected==1,"Down visits color row");
    check(page.Input(0x20000000)==Action::None && page.selected==2,"Down visits hybrid row");
    check(std::string(page.Message())=="Third person only; applies immediately","hybrid row explains live application");
    check(std::string(page.Message(true)).find("TR6 uses native controls")!=std::string::npos,"TR6 explains the option's game scope");
    page.hybrid=true;check(std::string(page.Value(2))=="On","enabled hybrid choice has readable On value");
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
    std::printf("PASS: %d VR settings checks\n",n);
}
