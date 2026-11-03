// Exercise the production INI parser and embedded first-run defaults only.
#include "Config.h"
#include "DefaultIni.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static int checks=0;
static void Check(bool ok,const char* message) {
    ++checks;
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1); }
}
int wmain(int argc,wchar_t** argv) {
    Check(argc==2,"provide a disposable test INI path");
    const wchar_t* path=argv[1];
    Check(!tr::Config{}.hybridModernTankControls,"struct fallback is Off");
    const char* text=tr::DefaultIniText();
    const char* key=std::strstr(text,"HybridModernTankControls=0\n");
    Check(key && !std::strstr(key+1,"HybridModernTankControls="),"first-run INI has exactly one Off default");
    FILE* file=nullptr;
    Check(_wfopen_s(&file,path,L"wb")==0 && file,"create disposable INI");
    Check(std::fwrite(text,1,std::strlen(text),file)==std::strlen(text),"write embedded first-run INI");
    std::fclose(file);
    tr::LoadConfig(path);
    Check(!tr::Cfg().hybridModernTankControls,"fresh install reads Off");
    Check(WritePrivateProfileStringW(L"VR",L"HybridModernTankControls",L"1",path)!=FALSE,"write opt-in");
    tr::LoadConfig(path);Check(tr::Cfg().hybridModernTankControls,"explicit On enables shortcut");
    Check(WritePrivateProfileStringW(L"VR",L"HybridModernTankControls",nullptr,path)!=FALSE,"remove key to emulate older INI");
    tr::LoadConfig(path);Check(!tr::Cfg().hybridModernTankControls,"missing key in an older INI resets to Off");
    Check(WritePrivateProfileStringW(L"VR",L"HybridModernTankControls",L"0",path)!=FALSE,"write explicit Off");
    tr::LoadConfig(path);Check(!tr::Cfg().hybridModernTankControls,"explicit Off preserves native controls");
    std::printf("Hybrid controls INI: %d checks passed\n",checks);
}
