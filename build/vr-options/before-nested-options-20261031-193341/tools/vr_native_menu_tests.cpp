// Tests native menu ownership, cleanup and save failures without loading games.
#include "../src/VRNativeMenu.cpp"
#include <cstdio>
#include <cstdlib>
namespace tr {
uint64_t Base() { return 0; }
int AppFlag(uint32_t) { return 0; }
const Layout& L() { static Layout layout{};return layout; }
const Config& Cfg() { static Config config{};return config; }
VRSystem& VR() { static VRSystem runtime;return runtime; }
namespace vrmenu {
bool saveOK=true;Values saved{};
Values ReadSettings() { return saved; }
bool SaveSettings(const Values& v,const Values&,std::wstring&) { if(saveOK) saved=v;return saveOK; }
}
}
int main() {
    using namespace tr;
    int n=0;auto check=[&](bool yes,const char* why) { ++n;if(!yes) { std::printf("FAIL: %s\n",why);std::exit(1); } };
    Slot slot;
    alignas(8) uint8_t items[5][3272]{};void* list[5];
    for(int i=0;i<5;++i) { list[i]=items[i];Field<int16_t>(items[i],8)=int16_t(95+i); }
    PrepareRing(slot,list);
    for(int i=0;i<5;++i) check(slot.list[i]==list[i],"native ring objects remain unchanged");
    check(slot.list[5]==slot.item.data(),"VR ring entry has independent storage");
    check(std::strcmp(Field<const char*>(slot.item.data(),0),"VR Options")==0,"ring label uses exact capitalization");
    check(Field<int32_t>(slot.item.data(),3264)==1,"new ring object recomputes its own transform");
    alignas(8) uint8_t menu[160]{};Option6 options[12]{};
    for(int i=0;i<12;++i) { options[i].next=i<11 ? &options[i+1] : nullptr;options[i].last=i ? &options[i-1] : nullptr; }
    Field<Option6*>(menu,120)=options;Field<uint8_t>(menu,106)=12;Field<uint8_t>(menu,107)=3;
    check(InsertOption6(slot,menu),"TR6 adds VR Options to native Options list");
    check(Field<uint8_t>(menu,106)==13 && Field<uint8_t>(menu,107)==3,"original Options selection is not shifted");
    check(options[11].next==&slot.option && slot.option.last==&options[11] && !slot.option.next,"VR Options follows existing options");
    check(std::strcmp(slot.option.text,"VR Options")==0 && slot.option.textId==-1,"literal label survives localization");
    check(InsertOption6(slot,menu) && Field<uint8_t>(menu,106)==13,"no duplicate row on repeated draws");
    alignas(8) uint8_t memory[512]{};Row row{};Extra extra{};row.game=6;extra.optionsMenu=256;
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
    PreparePage6(slot);
    check(Field<uint8_t>(slot.controllerMenu.data(),106)==2 && Field<uint8_t>(slot.controllerMenu.data(),107)==1,"TR6 native page exposes and selects second row");
    check(slot.controllerOptions[0].next==&slot.controllerOptions[1] && slot.controllerOptions[1].last==&slot.controllerOptions[0] && !slot.controllerOptions[1].next,"TR6 child row links are bounded and independent");
    check(std::strcmp(slot.controllerOptions[1].text,"Color Scheme: Brighter")==0,"TR6 draws the saved bright choice");
    check(!PageInput(slot,0x200000),"Back closes native child page");
    row.game=1;extra.textCount=300;At<int16_t>(slot,300)=2;
    alignas(8) uint8_t text[88]{};char label[512]="VR Options";
    Field<uint32_t>(text,0)=1;Field<char*>(text,72)=label;slot.texts[0]=text;
    strcpy_s(slot.textValues[0].data(),128,"VR Options");ClearTexts(slot);
    check(!(Field<uint32_t>(text,0)&1) && At<int16_t>(slot,300)==1,"closing page releases its persistent native text");
    Field<uint32_t>(text,0)=1;strcpy_s(label,"Another menu");slot.texts[0]=text;ClearTexts(slot);
    check((Field<uint32_t>(text,0)&1) && At<int16_t>(slot,300)==1,"cleanup cannot remove a reused native text slot");
    std::printf("PASS: %d native submenu checks\n",n);
}
