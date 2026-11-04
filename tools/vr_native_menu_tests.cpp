// Tests native menu ownership, cleanup and save failures without loading games.
#include <array>
#include <string>
#include <vector>
#define private public
#include "../src/VRSystem.h"
#include "../src/InlineHook.h"
#undef private
#include "../src/VRNativeMenu.cpp"
#include <cstdio>
#include <cstdlib>
namespace tr {
alignas(8) int32_t testApp[2048]{};
Config testConfig;
uint64_t Base() { return reinterpret_cast<uint64_t>(testApp); }
int AppFlag(uint32_t offset) { return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(testApp)+offset); }
const Layout& L() { static Layout layout{};return layout; }
const Config& Cfg() { return testConfig; }
VRSystem& VR() { static VRSystem runtime;return runtime; }
namespace vrmenu {
bool saveOK=true;Values saved{};
Values ReadSettings() { return saved; }
bool SaveSettings(const Values& v,const Values&,std::wstring&) { if(saveOK) saved=v;return saveOK; }
}
}
namespace {
alignas(8) uint8_t menuMemory[65536]{};
tr::Slot* testSlot=nullptr;
int nativeOptionsCalls=0;
int NativeOptions123() { ++nativeOptionsCalls;return 0; }
int NativeOptions45(int inventory) { ++nativeOptionsCalls;return inventory==7 ? 7 : 0; }
uint8_t NativeOptions6(void*,uint8_t) { ++nativeOptionsCalls;return 0; }
void* NativeText45(float,float,uint8_t,const char*,uint16_t) { return nullptr; }
void* NativeText123(int16_t,int16_t,int16_t,const char* value) {
    using namespace tr;auto& s=*testSlot;
    for(int i=0;i<64;++i) {
        auto* text=&At<uint8_t>(s,s.extra->textPool+i*88);
        if(Field<uint32_t>(text,0)&1) continue;
        Field<uint32_t>(text,0)=1;
        auto* buffer=&At<char>(s,s.extra->textBuffers+i*512);
        Field<char*>(text,72)=buffer;strcpy_s(buffer,512,value);
        ++At<int16_t>(s,s.extra->textCount);return text;
    }
    return nullptr;
}
void MenuContext(bool title,bool inventory) {
    std::fill(std::begin(tr::testApp),std::end(tr::testApp),inventory ? 1 : 0);
    tr::testApp[tr::drva::app_off::InTitle/4]=title ? 1 : 0;
}
template<class Check> void TestInGameMenus(Check check) {
    using namespace tr;
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto rva=[&](const void* p) { return uint32_t(reinterpret_cast<uintptr_t>(p)-base); };
    testConfig.enabled=true;VR().m_system=reinterpret_cast<vr::IVRSystem*>(1);
    for(int game=1;game<=6;++game) for(bool title:{false,true}) {
        std::memset(menuMemory,0,sizeof(menuMemory));MenuContext(title,!title);
        auto& s=g_slots[game-1];s.row=nullptr;s.extra=nullptr;
        s.selection={};s.options123={};s.page={};s.texts={};s.menu6=nullptr;
        Row row{};Extra extra{};row.game=game;
        row.input=rva(menuMemory+128);row.dbinput=rva(menuMemory+144);row.choice=rva(menuMemory+160);
        extra.subpage=rva(menuMemory+176);extra.selection=rva(menuMemory+192);
        extra.textCount=rva(menuMemory+208);extra.firstVisible=rva(menuMemory+224);
        extra.nativeTexts=rva(menuMemory+256);extra.textPool=rva(menuMemory+4096);extra.textBuffers=rva(menuMemory+12288);
        extra.text=rva(reinterpret_cast<void*>(&NativeText123));
        extra.active=rva(menuMemory+432);extra.optionsMenu=rva(menuMemory+448);
        s.module=reinterpret_cast<HMODULE>(base);s.row=&row;s.extra=&extra;testSlot=&s;
        nativeOptionsCalls=0;check(Available(),"native VR Options is available at title and during gameplay");
        if(game<=3) {
            s.title.m_trampoline=reinterpret_cast<void*>(&NativeOptions123);
            At<int16_t>(s,extra.selection)=12;At<int32_t>(s,extra.firstVisible)=3;
            auto call=[&]() { return game==1 ? Options123<0>() : game==2 ? Options123<1>() : Options123<2>(); };
            At<uint32_t>(s,row.dbinput)=0x4000;call();
            check(s.options123.vr && nativeOptionsCalls==1,"in-game TR123 Options reaches VR row");
            At<uint32_t>(s,row.dbinput)=0x100000;call();
            check(s.page.open && nativeOptionsCalls==1,"TR123 opens VR child without triggering another native option");
            UpdateMenuState(s);check(s.page.open && OwnText(s,0),"in-game frame update preserves child text and selection");
            At<uint32_t>(s,row.dbinput)=0x200000;call();
            check(!s.page.open && At<uint32_t>(s,row.dbinput)==0 && nativeOptionsCalls==2,
                  "TR123 child Back returns to Options without closing inventory");
            OpenPage(s);DrawPage123(s);
        } else if(game<=5) {
            s.title.m_trampoline=reinterpret_cast<void*>(&NativeOptions45);
            s.draw.m_trampoline=reinterpret_cast<void*>(&NativeText45);
            At<int32_t>(s,extra.selection)=12;
            auto call=[&](int inventory=1) { return game==4 ? Options<3>(inventory) : Options<4>(inventory); };
            At<uint64_t>(s,row.dbinput)=0x20000000;call();
            At<uint64_t>(s,row.dbinput)=0x100000;call();
            check(s.page.open && nativeOptionsCalls==1,"TR45 in-game Options opens VR child");
            UpdateMenuState(s);check(s.page.open,"TR45 in-game update cannot close the child each frame");
            At<uint64_t>(s,row.dbinput)=0x200000;call();
            check(!s.page.open && At<uint64_t>(s,row.dbinput)==0 && nativeOptionsCalls==2,
                  "TR45 child Back stays in parent Options");
            check(call(7)==7,"native Options return value and inventory argument are preserved");
            OpenPage(s);
        } else {
            s.title.m_trampoline=reinterpret_cast<void*>(&NativeOptions6);
            auto* menu=menuMemory+512;auto* options=reinterpret_cast<Option6*>(menuMemory+1024);
            for(int i=0;i<12;++i) { options[i].next=i<11 ? &options[i+1] : nullptr;options[i].last=i ? &options[i-1] : nullptr; }
            Field<Option6*>(menu,120)=options;Field<uint8_t>(menu,106)=12;Field<uint8_t>(menu,107)=12;
            At<void*>(s,extra.optionsMenu)=At<void*>(s,extra.active)=menu;
            Options6(menu,5);check(s.page.open && Field<uint8_t>(menu,106)==13,"TR6 pause Options inserts and opens VR row");
            MenuContext(title,false);UpdateMenuState(s);
            check(s.page.open && Field<uint8_t>(menu,106)==13,"TR6 active pause menu survives without inventory flag");
            Options6(menu,6);check(!s.page.open && nativeOptionsCalls==0,"TR6 child Back does not close parent menu");
            OpenPage(s);At<void*>(s,extra.active)=nullptr;
        }
        MenuContext(false,false);UpdateMenuState(s);
        check(!s.page.open && !s.selection.vr && !s.options123.vr,"resuming play discards old child page and virtual selection");
        if(game<=3) check(At<int16_t>(s,extra.textCount)==0,"resuming play releases owned VR menu text");
        if(game==6) check(Field<uint8_t>(menuMemory+512,106)==12,"resuming play restores original TR6 menu list");
        MenuContext(false,true);OpenPage(s);testConfig.enabled=false;UpdateMenuState(s);
        check(!Available() && !s.page.open,"disabling mod closes VR page inside in-game menus");
        testConfig.enabled=true;OpenPage(s);VR().m_system=nullptr;UpdateMenuState(s);
        check(!Available() && !s.page.open,"VR shutdown also cleans an in-game page");
        VR().m_system=reinterpret_cast<vr::IVRSystem*>(1);
        s.title.m_trampoline=s.draw.m_trampoline=nullptr;s.module=nullptr;s.row=nullptr;s.extra=nullptr;
    }
    VR().m_system=nullptr;MenuContext(false,false);
}
}
int main() {
    using namespace tr;
    int n=0;auto check=[&](bool yes,const char* why) { ++n;if(!yes) { std::printf("FAIL: %s\n",why);std::exit(1); } };
    Slot slot;
    vrmenu::Options123Selection parent;
    for(int choice=0;choice<13;++choice) {
        uint32_t input=0x100000;
        check(!parent.Input(choice,input) && input==0x100000,"all thirteen original Options actions remain native");
    }
    uint32_t input=0x4000;parent.Input(12,input);
    check(parent.vr && input==0,"native inventory Down enters VR Options after the last original row");
    input=0x18000;parent.Input(12,input);check(input==0,"VR row cannot adjust the underlying original setting");
    input=0x100000;check(parent.Input(12,input) && input==0,"native Select opens the nested page");
    input=0x2000;parent.Input(12,input);check(!parent.vr && input==0,"Up returns to the final original row without skipping it");
    input=0x104000;parent.Input(12,input);check(parent.vr && input==0,"Down plus Select cannot trigger an original action");
    input=0x300000;check(!parent.Input(12,input) && !parent.vr && input==0x200000,"Back takes precedence and stays native at parent level");
    check(vrmenu::Options123Buttons(0x2000)==0x10000000,"TR123 Up maps to child navigation");
    check(vrmenu::Options123Buttons(0x4000)==0x20000000,"TR123 Down maps to child navigation");
    check(vrmenu::Options123Buttons(0x8000)==0x40000000,"TR123 Left maps to child change");
    check(vrmenu::Options123Buttons(0x10000)==0x80000000,"TR123 Right maps to child change");
    check(vrmenu::Options123Buttons(0x300000)==0x300000,"native Confirm and Back retain their codes");
    for(const auto& r:Rows) if(r.game<=3) {
        for(const auto& e:Extras) if(r.stamp==e.stamp) {
            Slot addressSlot;addressSlot.row=&r;addressSlot.extra=&e;
            check(e.options && HookRva(addressSlot)!=r.title,"TR123 hooks the existing Options handler, never RingInit");
        }
    }
    alignas(8) uint8_t menu[160]{};Option6 options[12]{};
    for(int i=0;i<12;++i) { options[i].next=i<11 ? &options[i+1] : nullptr;options[i].last=i ? &options[i-1] : nullptr; }
    Field<Option6*>(menu,120)=options;Field<uint8_t>(menu,106)=12;Field<uint8_t>(menu,107)=3;
    check(InsertOption6(slot,menu),"TR6 adds VR Options to native Options list");
    check(Field<uint8_t>(menu,106)==13 && Field<uint8_t>(menu,107)==3,"original Options selection is not shifted");
    check(options[11].next==&slot.option && slot.option.last==&options[11] && !slot.option.next,"VR Options follows existing options");
    check(std::strcmp(slot.option.text,"VR Options")==0 && slot.option.textId==-1,"literal label survives localization");
    check(InsertOption6(slot,menu) && Field<uint8_t>(menu,106)==13,"no duplicate row on repeated draws");
    alignas(8) uint8_t memory[65536]{};Row row{};Extra extra{};row.game=6;extra.optionsMenu=256;
    slot.row=&row;slot.extra=&extra;slot.module=reinterpret_cast<HMODULE>(memory);At<void*>(slot,256)=menu;
    Field<uint8_t>(menu,107)=12;Restore(slot);
    check(!options[11].next && Field<uint8_t>(menu,106)==12,"cleanup restores native list");
    check(Field<uint8_t>(menu,107)==11,"cleanup leaves a valid native selection");
    OpenPage(slot);check(slot.page.open && !slot.page.index,"page opens from saved controller type");
    PageInput(slot,0x80000000);check(slot.page.index && vrmenu::saved[0]==1,"change saves before updating display");
    vrmenu::saveOK=false;PageInput(slot,0x40000000);
    check(slot.page.index && slot.page.saveFailed && vrmenu::saved[0]==1,"failed save retains previous value and shows native error");
    PageInput(slot,0x20000000);
    check(slot.page.selected==1 && !slot.page.saveFailed,"Down selects colors and clears stale error");
    PageInput(slot,0x80000000);
    check(!slot.page.bright && slot.page.saveFailed,"failed color save preserves the displayed color scheme");
    vrmenu::saveOK=true;PageInput(slot,0x80000000);
    check(slot.page.bright && vrmenu::saved[1]==1 && vrmenu::saved[0]==1,"color change saves only colors and preserves controller type");
    PageInput(slot,0x20000000);
    check(slot.page.selected==2 && !slot.page.hybrid,"Down selects the default-Off hybrid row");
    vrmenu::saveOK=false;PageInput(slot,0x100000);
    check(!slot.page.hybrid && slot.page.saveFailed && vrmenu::saved[2]==0,"failed hybrid save keeps the displayed Off choice");
    vrmenu::saveOK=true;PageInput(slot,0x80000000);
    check(slot.page.hybrid && vrmenu::saved[2]==1 && vrmenu::saved[0]==1 && vrmenu::saved[1]==1,
        "hybrid change saves only its own value and updates the display");
    PreparePage6(slot);
    check(Field<uint8_t>(slot.controllerMenu.data(),106)==4 && Field<uint8_t>(slot.controllerMenu.data(),107)==2,"TR6 native page exposes four settings and selects hybrid row");
    for(int i=0;i<vrmenu::SettingCount;++i)
        check(slot.controllerOptions[i].next==(i+1<vrmenu::SettingCount ? &slot.controllerOptions[i+1] : nullptr) &&
            slot.controllerOptions[i].last==(i ? &slot.controllerOptions[i-1] : nullptr),"TR6 child row links are bounded and independent");
    check(std::strcmp(slot.controllerOptions[1].text,"Color Scheme: Brighter")==0,"TR6 draws the saved bright choice");
    check(std::strcmp(slot.controllerOptions[2].text,"Hybrid Modern/Tank Controls: On")==0,"TR6 draws the hybrid value");
    PageInput(slot,0x20000000);
    check(slot.page.selected==3 && slot.page.bodyForwardCm==0,"Down reaches body offset at its original position");
    PageInput(slot,0x80000000);
    check(slot.page.bodyForwardCm==1 && vrmenu::saved[3]==1,"Right saves one centimetre forward");
    vrmenu::saveOK=false;PageInput(slot,0x80000000);
    check(slot.page.bodyForwardCm==1 && slot.page.saveFailed,"failed save cannot change displayed body position");
    vrmenu::saveOK=true;PageInput(slot,0x100000);
    check(slot.page.bodyForwardCm==0 && vrmenu::saved[3]==0,"Select restores default body position");
    PreparePage6(slot);
    check(std::strcmp(slot.controllerOptions[3].text,"Body Position: Default (0 cm)")==0,"TR6 body row displays its value");
    check(!PageInput(slot,0x200000),"Back closes native child page");
    row.game=1;row.transition=0x800;extra.textCount=300;
    extra.textPool=0x1000;extra.textBuffers=0x3000;extra.nativeTexts=512;extra.firstVisible=480;
    At<int16_t>(slot,300)=2;
    auto record=[&](int index,const char* label) {
        auto* text=&At<uint8_t>(slot,extra.textPool+index*88);
        auto* string=&At<char>(slot,extra.textBuffers+index*512);
        Field<uint32_t>(text,0)=1;Field<char*>(text,72)=string;strcpy_s(string,512,label);return text;
    };
    auto* text=record(0,"VR Options");auto* label=Field<char*>(text,72);slot.texts[0]=text;
    strcpy_s(slot.textValues[0].data(),128,"VR Options");ClearTexts(slot);
    check(!(Field<uint32_t>(text,0)&1) && At<int16_t>(slot,300)==1,"closing page releases its persistent native text");
    record(0,"Another menu");slot.texts[0]=text;ClearTexts(slot);
    check((Field<uint32_t>(text,0)&1) && At<int16_t>(slot,300)==1,"cleanup cannot remove a reused native text slot");
    check(!ValidText123(slot,reinterpret_cast<void*>(1)),"out-of-pool pointers are rejected before dereferencing");
    check(!ValidText123(slot,static_cast<uint8_t*>(text)+1),"misaligned pool pointers are rejected");
    Field<char*>(text,72)=label+1;check(!ValidText123(slot,text),"text records must own the expected bounded string buffer");
    Field<char*>(text,72)=label;
    for(int i=0;i<20;++i) At<void*>(slot,extra.nativeTexts+i*8)=record(i+1,"Native option");
    At<int16_t>(slot,300)=21;ClearNativeTexts123(slot);
    for(int i=0;i<20;++i) check(!At<void*>(slot,extra.nativeTexts+i*8),"opening child releases all native parent text references");
    check(At<int16_t>(slot,300)==1 && (Field<uint32_t>(text,0)&1),"parent cleanup preserves unrelated ring text");
    for(int i=0;i<Slot::TextCount;++i) { slot.texts[i]=record(i+27,"old");strcpy_s(slot.textValues[i].data(),128,"old"); }
    At<int16_t>(slot,300)=Slot::TextCount+1;vrmenu::saved={1,0,0};OpenPage(slot);
    PageInput(slot,vrmenu::Options123Buttons(0x4000));
    check(slot.page.selected==1,"TR123 native Down selects Color Scheme");
    PageInput(slot,vrmenu::Options123Buttons(0x10000));DrawPage123(slot);
    check(slot.page.bright && std::strcmp(Field<char*>(slot.texts[2],72),"> Color Scheme: Brighter")==0,"TR123 Right saves and draws the selected bright scheme");
    PageInput(slot,vrmenu::Options123Buttons(0x4000));
    check(slot.page.selected==2,"TR123 native Down reaches hybrid controls");
    PageInput(slot,vrmenu::Options123Buttons(0x10000));DrawPage123(slot);
    check(slot.page.hybrid && std::strcmp(Field<char*>(slot.texts[3],72),"> Hybrid Modern/Tank Controls: On")==0,
        "TR123 Right saves and draws the hybrid On value");
    for(bool index:{false,true}) for(bool bright:{false,true}) for(bool hybrid:{false,true}) for(int selected:{0,1,2,3}) {
        slot.page.Open(index,bright,hybrid);slot.page.selected=selected;DrawPage123(slot);
        const char* controller=Field<char*>(slot.texts[1],72);
        const char* colors=Field<char*>(slot.texts[2],72);
        const char* hybridText=Field<char*>(slot.texts[3],72);
        check(std::strstr(controller,index ? "Controller Type: Valve Index" : "Controller Type: Meta Touch") &&
              std::strstr(colors,bright ? "Color Scheme: Brighter" : "Color Scheme: Dark") &&
              std::strstr(hybridText,hybrid ? "Hybrid Modern/Tank Controls: On" : "Hybrid Modern/Tank Controls: Off"),
              "TR123 draws all saved values for every setting and selection combination");
        bool plain=true;
        for(auto* entry:slot.texts) if(std::strchr(Field<char*>(entry,72),'<')) plain=false;
        check(plain,"TR123 labels and values never enter the native angle-bracket icon decoder");
        check((controller[0]=='>')==(selected==0) && (colors[0]=='>')==(selected==1) && (hybridText[0]=='>')==(selected==2),
              "the active row stays marked without wrapping values in icon tokens");
    }
    slot.page.selected=3;slot.page.bodyForwardCm=20;DrawPage123(slot);
    check(std::strcmp(Field<char*>(slot.texts[4],72),"> Body Position: Forward 20 cm")==0,
          "TR123 draws the selected body distance as plain visible text");
    const auto savedBeforeBack=vrmenu::saved;
    check(!PageInput(slot,vrmenu::Options123Buttons(0x200000)) && vrmenu::saved==savedBeforeBack,"child Back closes without changing saved values");
    uint8_t ringBefore[96];std::memset(&At<uint8_t>(slot,row.transition),0xa5,96);std::memcpy(ringBefore,&At<uint8_t>(slot,row.transition),96);
    Restore(slot);
    check(std::memcmp(ringBefore,&At<uint8_t>(slot,row.transition),96)==0,"cleanup never changes original ring objects, count or selection");
    check(At<int16_t>(slot,300)==1,"all child text records are released on exit");
    for(int repeat=0;repeat<100;++repeat) {
        for(int i=0;i<Slot::TextCount;++i) { slot.texts[i]=record(i+27,"old");strcpy_s(slot.textValues[i].data(),128,"old"); }
        At<int16_t>(slot,300)=Slot::TextCount+1;DrawPage123(slot);ClearTexts(slot);
    }
    check(At<int16_t>(slot,300)==1,"repeated page visits do not exhaust the 64-slot native text pool");
    auto* background=record(13,"background");auto* highlight=record(15,"highlight");
    At<void*>(slot,extra.nativeTexts+13*8)=background;At<void*>(slot,extra.nativeTexts+15*8)=highlight;
    At<void*>(slot,extra.nativeTexts+16*8)=record(16,"left");At<void*>(slot,extra.nativeTexts+17*8)=record(17,"right");
    slot.texts[0]=record(27,"VR Options");strcpy_s(slot.textValues[0].data(),128,"VR Options");
    At<int16_t>(slot,300)=6;At<int32_t>(slot,extra.firstVisible)=3;slot.options123.vr=true;DrawOptionsRow123(slot);
    check(Field<int16_t>(background,58)==206 && Field<float>(highlight,16)==90.f,"extra row fits the native panel and receives its native highlight");
    check(!At<void*>(slot,extra.nativeTexts+16*8) && !At<void*>(slot,extra.nativeTexts+17*8),"virtual selection hides underlying native value arrows");
    At<int32_t>(slot,extra.firstVisible)=0;DrawOptionsRow123(slot);
    check(!slot.texts[0] && Field<int16_t>(background,58)==190,"earlier scrolling pages retain their original layout");
    TestInGameMenus(check);
    std::printf("PASS: %d native submenu checks\n",n);
}
