// Isolated controller-menu tests and GDI previews; no game or VR runtime needed.
#include "../src/Config.cpp"
#include "../src/VRMenu.cpp"
#include "../src/VRNativeMenuModel.h"
#include <fstream>
#include <limits>
#include <cstdio>
namespace tr {
void VRNativeMenuUpdate() {}
void VRNativeMenuShutdown() {}
uint64_t Base() { return 0; }
int AppFlag(uint32_t) { return 0; }
const Layout& L() { static Layout layout{};return layout; }
VRSystem& VR() { static VRSystem runtime;return runtime; }
}
namespace {
int checks=0;
void Check(bool pass,const char* description) { ++checks;if(!pass) { std::printf("FAIL: %s\n",description);std::exit(1); } }
void Preview(const tr::vrmenu::Model& model,const wchar_t* path) {
    int width,height;const auto rgba=tr::RenderMenu(model,L"",width,height);
    Check(rgba.size()==size_t(width)*height,"GDI renders complete panel");
    std::vector<uint32_t> bgra(rgba.size());
    for(size_t i=0;i<rgba.size();++i) { auto c=rgba[i];bgra[i]=(c&0xff00ff00)|((c&255)<<16)|((c>>16)&255); }
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+DWORD(bgra.size()*4);
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-height;
    info.biPlanes=1;info.biBitCount=32;
    FILE* file=nullptr;_wfopen_s(&file,path,L"wb");Check(file!=nullptr,"preview opens");
    fwrite(&header,sizeof(header),1,file);fwrite(&info,sizeof(info),1,file);fwrite(bgra.data(),4,bgra.size(),file);fclose(file);
}
}
int main() {
    using namespace tr::vrmenu;
    wchar_t cwd[MAX_PATH]{};GetCurrentDirectoryW(MAX_PATH,cwd);
    const std::wstring ini=std::wstring(cwd)+L"\\vr-menu-test.ini";
    { std::ofstream file("vr-menu-test.ini");file<<"; keep this comment\n[VR]\nValveIndexControllers=0\nValveIndexGripPress=0.55\nValveIndexGripRelease=0.35\nUnrelatedUserSetting=keep-me\n[Other]\nOtherKey=77\n"; }
    tr::LoadConfig(ini.c_str());
    auto original=ReadSettings();Check(ValidSettings(original),"loaded defaults form a valid controller profile");
    auto draft=original;draft[0]=1;draft[1]=.8f;std::wstring error;
    const bool firstSave=SaveSettings(draft,original,error);
    if(!firstSave) std::printf("Save error %lu: %ls\n",GetLastError(),error.c_str());
    Check(firstSave,"controller settings save successfully");
    auto saved=ReadSettings();Check(saved[0]==1 && std::fabs(saved[1]-.8f)<.001f,"saved settings read back");
    wchar_t value[80]{};GetPrivateProfileStringW(L"VR",L"UnrelatedUserSetting",L"",value,80,ini.c_str());
    Check(std::wstring(value)==L"keep-me","unrelated settings survive save");
    Check(GetPrivateProfileIntW(L"Other",L"OtherKey",0,ini.c_str())==77,"other sections survive save");
    Check(GetFileAttributesW((ini+L".vr-options.bak").c_str())!=INVALID_FILE_ATTRIBUTES,"previous INI backed up");
    { std::ifstream file("vr-menu-test.ini");std::string content((std::istreambuf_iterator<char>(file)),{});Check(content.find("keep this comment")!=std::string::npos,"INI comments retained"); }
    draft=saved;draft[2]=draft[1];Check(!SaveSettings(draft,saved,error),"invalid grip hysteresis rejected");
    Check(ReadSettings()==saved,"failed validation leaves installed settings untouched");
    draft=saved;draft[3]=std::numeric_limits<float>::quiet_NaN();Check(!SaveSettings(draft,saved,error),"nonfinite input rejected");
    draft=saved;draft[0]=.5f;Check(!SaveSettings(draft,saved,error),"invalid toggle rejected");
    draft=saved;draft[0]=0;Check(SaveSettings(draft,saved,error),"second save safely replaces existing backup");
    saved=ReadSettings();
    Model model;model.original=model.values=saved;uint64_t tick=1000;
    auto input=[&](bool available,uint16_t b,int16_t x=0,int16_t y=0) { tick+=16;return model.Input(available,b,x,y,tick); };
    Check(input(false,0x80)==Event::None && !model.consume,"R3 in gameplay is untouched");
    Check(input(true,0x80)==Event::None,"held R3 cannot open on title transition");input(true,0);
    Check(input(true,0xc0)==Event::None && !model.consume,"photo-mode chord retains priority outside options");input(true,0);
    Check(input(true,0x80)==Event::None && !model.open && !model.consume,"R3 shortcut removed from title");
    model.Open();
    Check(input(true,0x1000)==Event::None && model.open && model.consume && model.values==saved,"native Select is drained without changing a setting");
    input(true,0);Check(input(true,0,32767)==Event::Change && model.values[0]!=saved[0],"horizontal stick changes selected setting");
    for(int i=0;i<SettingCount+1;++i) { input(true,0);input(true,0,0,-32767); }
    Check(model.selected==SettingCount+1,"navigation reaches Cancel through all pages");
    input(true,0);Check(input(true,0x1000)==Event::Cancel && !model.open && model.consume,"Cancel closes and consumes confirm");
    Check(ReadSettings()==saved,"Cancel never writes draft changes");
    input(true,0x1000);Check(model.consume && model.drain,"held confirm cannot activate native menu after close");
    input(true,0);input(true,0);Check(!model.consume,"native input resumes after release");
    model.Open();input(true,0);model.selected=SettingCount;
    Check(input(true,0x1000)==Event::Save && model.consume,"Save emits one owned save request");
    Check(input(true,0x1000)==Event::None,"held confirm does not repeat saves");
    input(true,0);model.selected=0;
    Check(input(true,0x8000)==Event::Save && model.consume,"Y saves from any page without native Action");
    input(false,0);Check(!model.open && !model.consume,"leaving title hides options and restores gameplay input");
    model={};model.values=model.original=saved;model.Open();input(true,0);model.selected=1;
    for(int i=0;i<40;++i) { input(true,0);input(true,0,-32767); }
    Check(ValidSettings(model.values) && model.values[1]>model.values[2],"repeated grip adjustments preserve release hysteresis");
    for(int i=0;i<SettingCount;++i) {
        model.selected=i;
        for(int step=0;step<500;++step) { input(true,0);input(true,0,32767); }
        Check(ValidSettings(model.values),"upper setting limit remains valid");
        for(int step=0;step<500;++step) { input(true,0);input(true,0,-32767); }
        Check(ValidSettings(model.values),"lower setting limit remains valid");
    }
    TitleSelection title;uint64_t choice=2,buttons=0x20000000;
    Check(!title.Input(choice,buttons) && title.vr && choice==2 && buttons==0,"Down from Load selects VR instead of Options");
    buttons=0x100000;Check(title.Input(choice,buttons) && buttons==0,"VR Select is intercepted before Load action");
    buttons=0x20000000;Check(!title.Input(choice,buttons) && !title.vr && choice==4 && buttons==0,"Down from VR selects Options");
    buttons=0x10000000;title.Input(choice,buttons);Check(title.vr && choice==2 && buttons==0,"Up from Options selects VR");
    buttons=0x10000000;title.Input(choice,buttons);Check(!title.vr && choice==2 && buttons==0,"Up from VR selects Load");
    for(auto nativeChoice:{1ull,2ull,4ull,8ull,16ull}) {
        choice=nativeChoice;buttons=0x100000;
        Check(!title.Input(choice,buttons) && choice==nativeChoice && buttons==0x100000,"original title actions retain their selection IDs");
    }
    choice=2;buttons=0x20100000;title.Input(choice,buttons);
    Check(title.vr && buttons==0,"navigation plus Select cannot accidentally launch a game");
    choice=1;buttons=0;title.Input(choice,buttons);Check(!title.vr,"title reset clears virtual selection");
    model={};model.values=saved;
    model.open=true;Preview(model,L"vr-menu-page1.bmp");model.page=1;model.selected=7;Preview(model,L"vr-menu-page2.bmp");
    model.page=2;model.selected=SettingCount;Preview(model,L"vr-menu-page3.bmp");
    std::printf("PASS: %d controller menu checks\n",checks);
}
