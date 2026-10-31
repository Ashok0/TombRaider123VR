// Focused native menu structure/lifecycle checks; never loads a game DLL.
#include "../src/VRNativeMenu.cpp"
#include <cstdio>
#include <cstdlib>
namespace tr {
uint64_t Base() { return 0; }
int AppFlag(uint32_t) { return 0; }
const Layout& L() { static Layout layout{};return layout; }
const Config& Cfg() { static Config config{};return config; }
VRSystem& VR() { static VRSystem runtime;return runtime; }
bool VRMenuOpen() { return false; }
bool VRMenuIsOpen() { return false; }
bool VRMenuBlocksInput() { return false; }
}
int main() {
    using namespace tr;
    int n=0;
    auto check=[&](bool yes,const char* why) { ++n;if(!yes) { std::printf("FAIL: %s\n",why);std::exit(1); } };
    Slot slot;
    alignas(8) uint8_t items[5][3272]{};void* list[5];
    for(int i=0;i<5;++i) { list[i]=items[i];Field<int16_t>(items[i],8)=int16_t(95+i); }
    Field<uint32_t>(items[2],60)=0x1234;
    PrepareRing(slot,list);
    for(int i=0;i<5;++i) check(slot.list[i]==list[i],"original title-ring objects preserved in order");
    check(slot.list[5]==slot.item.data(),"sixth ring entry has independent object storage");
    check(Field<const char*>(slot.item.data(),0)==Label,"ring entry uses VR OPTIONS label");
    check(Field<int16_t>(slot.item.data(),8)==97 && Field<uint32_t>(slot.item.data(),60)==0x1234,"clone preserves valid native model and mesh masks");
    Field<uint32_t>(slot.item.data(),60)=0;
    check(Field<uint32_t>(items[2],60)==0x1234,"clone animation cannot mutate native controls object");
    alignas(8) uint8_t menu[160]{};Option6 options[5]{};
    for(int i=0;i<5;++i) { options[i].next=i<4 ? &options[i+1] : nullptr;options[i].last=i ? &options[i-1] : nullptr; }
    Field<Option6*>(menu,120)=options;Field<uint8_t>(menu,106)=5;Field<uint8_t>(menu,107)=2;
    check(InsertOption6(slot,menu),"TR6 inserts a native option");
    check(Field<uint8_t>(menu,106)==6 && Field<uint8_t>(menu,107)==3,"existing Options selection follows inserted row");
    check(options[1].next==&slot.option && slot.option.next==&options[2] && options[2].last==&slot.option,"TR6 row is exactly between Load and Options");
    check(slot.option.text==Label && slot.option.textId==-1,"literal label survives localization reload");
    check(InsertOption6(slot,menu) && Field<uint8_t>(menu,106)==6,"repeated draw never duplicates row");
    alignas(8) uint8_t memory[512]{};Row row{};row.game=6;row.menu=256;
    slot.row=&row;slot.module=reinterpret_cast<HMODULE>(memory);At<void*>(slot,row.menu)=menu;
    Restore(slot);
    check(options[1].next==&options[2] && options[2].last==&options[1],"shutdown restores original linked list");
    check(Field<uint8_t>(menu,106)==5 && Field<uint8_t>(menu,107)==2,"shutdown restores count and selection");
    std::printf("PASS: %d native menu checks\n",n);
}
