#include "VRNativeMenu.h"
#include "VRNativeMenuModel.h"
#include "VRMenu.h"
#include "InlineHook.h"
#include "Engine.h"
#include "Config.h"
#include "VRSystem.h"
#include "Log.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <cstring>

namespace tr {
namespace {
// Fields are verified against the PDB builds and matched retail instructions.
// TR1-3: RingInit, inventory_options, mode, saved selection, ring, inputs, list.
// TR4-5: TitleOptions, AddString, submenu, selection, fade, inputs, title reset.
// TR6: MainMenuFunc, unused, MainMenu, ring velocity, QR state, unused, inputDB.
struct Row {
    int game;uint32_t stamp,title,draw,menu,choice,transition,input,dbinput,reset;
    uint8_t titleSize,drawSize,titleBytes[8],drawBytes[8];
};
#include "VRNativeMenuAddresses.inl"

struct Option6 {
    Option6* next;Option6* last;
    uint32_t flags;int32_t textId;
    uint8_t* active;uint8_t* var;const char* text;
    uint8_t leftRight,disabled,pad[6];
};
static_assert(sizeof(Option6)==56);
struct Slot {
    // First member shares Slot's natural pointer alignment.
    std::array<uint8_t,3272> item{};
    HMODULE module=nullptr;
    const Row* row=nullptr;
    hook::InlineHook title,draw;
    vrmenu::TitleSelection selection;
    bool scope=false,attempted=false,ringOpen=false;
    std::array<void*,6> list{};
    Option6 option{};
    void* menu6=nullptr;
};
Slot g_slots[6];
constexpr const wchar_t* Modules[]={L"tomb1.dll",L"tomb2.dll",L"tomb3.dll",L"tomb4.dll",L"tomb5.dll",L"tomb6.dll"};
constexpr char Label[]="VR OPTIONS";
bool Available() { return Cfg().enabled && VR().active() && InTitle(); }
template<class T> T& At(Slot& s,uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(s.module)+rva); }
template<class T> T& Field(void* p,size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+off); }

template<int G> void* AddString(float x,float y,uint8_t color,const char* text,uint16_t flags) {
    auto& s=g_slots[G];
    using Fn=void*(*)(float,float,uint8_t,const char*,uint16_t);
    auto original=s.draw.Original<Fn>();
    if(s.scope && x==0 && flags==0x28 && y>=-90 && y<=-18) {
        if(s.selection.vr) color=2;
        if(y==-54) original(x,y,s.selection.vr ? 1 : 2,Label,flags);
        if(y>=-54) y+=18;
    }
    return original(x,y,color,text,flags);
}
template<int G> int TitleOptions() {
    auto& s=g_slots[G];const auto& r=*s.row;
    const bool root=Available() && At<int32_t>(s,r.menu)==0;
    if(!root || At<int32_t>(s,r.transition) || At<int16_t>(s,r.reset)) s.selection.vr=false;
    if(root && !At<int32_t>(s,r.transition) && !At<int16_t>(s,r.reset)) {
        auto& input=At<uint64_t>(s,r.dbinput);
        if(VRMenuBlocksInput()) { input=0;At<uint64_t>(s,r.input)=0; }
        else if(s.selection.Input(At<uint64_t>(s,r.choice),input)) {
            VRMenuOpen();At<uint64_t>(s,r.input)=0;
        }
    }
    s.scope=root;
    const int result=s.title.Original<int(*)()>()();
    s.scope=false;
    return result;
}

void PrepareRing(Slot& s,void** list) {
    std::copy_n(list,5,s.list.begin());
    // Reuse the native controls object's mesh and transform. The clone has its
    // own identity and label, so it never invokes or changes Controls itself.
    std::memcpy(s.item.data(),list[2],s.item.size());
    Field<const char*>(s.item.data(),0)=Label;
    s.list[5]=s.item.data();s.ringOpen=false;
}
template<int G> void RingInit(void* ring,int16_t type,void** list,int16_t count,int16_t current) {
    auto& s=g_slots[G];const auto& r=*s.row;
    if(Available() && At<int32_t>(s,r.menu)==1 && type==1 && count==5 && list==&At<void*>(s,r.reset)) {
        PrepareRing(s,list);list=s.list.data();count=6;
    }
    current=std::clamp<int16_t>(current,0,count>0 ? count-1 : 0);
    if(type==1) At<int16_t>(s,r.choice)=current;
    using Fn=void(*)(void*,int16_t,void**,int16_t,int16_t);
    s.title.Original<Fn>()(ring,type,list,count,current);
}
template<int G> void InventoryOptions(void* item) {
    auto& s=g_slots[G];const auto& r=*s.row;
    if(item!=s.item.data()) { s.draw.Original<void(*)(void*)>()(item);return; }
    At<uint32_t>(s,r.input)=At<uint32_t>(s,r.dbinput)=0;
    if(!s.ringOpen) s.ringOpen=VRMenuOpen();
    if(!s.ringOpen || !VRMenuIsOpen()) {
        // Native Back closes the selected object and returns to the ring. It
        // does not select an underlying game action or leave the title screen.
        At<uint32_t>(s,r.dbinput)=0x200000;s.ringOpen=false;
    }
}
void UpdateRing(Slot& s) {
    const auto& r=*s.row;void* ring=&At<uint8_t>(s,r.transition);
    if(!Available() || At<int32_t>(s,r.menu)!=1) return;
    auto* list=Field<void**>(ring,0);
    auto* motion=Field<uint8_t*>(ring,88);
    // The first presentation can occur after RingInit. Wait for the opening
    // motion to finish, then extend that already-created title ring once.
    if(list!=&At<void*>(s,r.reset) || Field<int16_t>(ring,8)!=1 ||
       Field<int16_t>(ring,22)!=5 || Field<int16_t>(ring,14) || !motion ||
       Field<int16_t>(motion,0)!=0 || Field<int16_t>(motion,2)!=1) return;
    PrepareRing(s,list);
    Field<void**>(ring,0)=s.list.data();Field<int16_t>(ring,22)=6;
    const int step=65536/6;
    Field<int16_t>(ring,24)=int16_t(step);
    Field<int16_t>(ring,46)=int16_t(-16384-Field<int16_t>(ring,18)*step);
}

bool InsertOption6(Slot& s,void* menu) {
    if(!menu) return false;
    auto* head=Field<Option6*>(menu,120);
    for(auto* p=head; p; p=p->next) if(p==&s.option) return true;
    if(Field<uint8_t>(menu,106)!=5 || !head || !head->next || !head->next->next) return false;
    auto* load=head->next;auto* options=load->next;
    // The engine owns its fixed option pool. Our extra node is outside it and
    // uses a literal label, so language reloads cannot overwrite another row.
    s.option={options,load,0x80000000u,-1,nullptr,nullptr,Label,0,0,{}};
    load->next=&s.option;options->last=&s.option;
    ++Field<uint8_t>(menu,106);
    if(Field<uint8_t>(menu,107)>=2) ++Field<uint8_t>(menu,107);
    s.menu6=menu;
    return true;
}
uint8_t MainMenu6(void* menu,uint8_t event) {
    auto& s=g_slots[5];const auto& r=*s.row;
    auto original=s.title.Original<uint8_t(*)(void*,uint8_t)>();
    if(!Available() || menu!=At<void*>(s,r.menu) || At<int32_t>(s,r.transition) || !InsertOption6(s,menu))
        return original(menu,event);
    if(VRMenuBlocksInput()) {
        At<uint32_t>(s,r.dbinput)=0;
        if(event!=11) return 0; // Keep the native title drawing while settings are open.
    }
    auto& selected=Field<uint8_t>(menu,107);
    if(event==5) {
        if(selected==2) { if(At<float>(s,r.choice)==0) VRMenuOpen();return 0; }
        // Only the action dispatch uses legacy indices. Native drawing and
        // navigation see the actual six-node list at all times.
        const auto saved=selected;
        if(selected>2) --selected;
        const auto result=original(menu,event);
        selected=saved;return result;
    }
    return original(menu,event);
}

void Restore(Slot& s) {
    if(!s.row) return;
    const auto& r=*s.row;
    if(r.game<=3) {
        void* ring=&At<uint8_t>(s,r.transition);
        if(Field<void**>(ring,0)==s.list.data()) {
            Field<void**>(ring,0)=&At<void*>(s,r.reset);
            Field<int16_t>(ring,22)=5;
            Field<int16_t>(ring,18)=std::min<int16_t>(Field<int16_t>(ring,18),4);
            Field<int16_t>(ring,24)=65536/5;
            Field<int16_t>(ring,46)=int16_t(-16384-Field<int16_t>(ring,18)*(65536/5));
        }
        if(At<int16_t>(s,r.choice)>4) At<int16_t>(s,r.choice)=0;
    } else if(r.game==6 && s.menu6 && At<void*>(s,r.menu)==s.menu6) {
        auto* p=Field<Option6*>(s.menu6,120);
        for(int i=0;p && i<8;++i,p=p->next) if(p==&s.option) {
            p->last->next=p->next;p->next->last=p->last;
            --Field<uint8_t>(s.menu6,106);
            auto& selected=Field<uint8_t>(s.menu6,107);
            if(selected>=2) --selected;
            break;
        }
    }
}
void Detach(Slot& s,bool mapped) {
    if(mapped) { Restore(s);s.draw.Remove();s.title.Remove(); }
    else { s.draw.Abandon();s.title.Abandon(); }
    s.module=nullptr;s.row=nullptr;s.attempted=s.scope=s.ringOpen=false;
    s.selection={};s.menu6=nullptr;
}
void Attach(Slot& s,int index,HMODULE module) {
    s.module=module;s.attempted=true;
    auto* base=reinterpret_cast<uint8_t*>(module);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return;
    const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(pe->Signature!=IMAGE_NT_SIGNATURE) return;
    for(const auto& r:Rows) if(r.game==index+1 && r.stamp==pe->FileHeader.TimeDateStamp) { s.row=&r;break; }
    if(!s.row) { LogF("VR OPTIONS: unsupported native title TR%d stamp %08X",index+1,pe->FileHeader.TimeDateStamp);return; }
    const auto& r=*s.row;
    for(const auto address:{r.title,r.draw,r.menu,r.choice,r.transition,r.input,r.dbinput,r.reset})
        if(address && address+8>=pe->OptionalHeader.SizeOfImage) { s.row=nullptr;return; }
    if(std::memcmp(base+r.title,r.titleBytes,r.titleSize) ||
       (r.draw && std::memcmp(base+r.draw,r.drawBytes,r.drawSize))) {
        LogF("VR OPTIONS: native title signature mismatch TR%d",index+1);s.row=nullptr;return;
    }
    void* titleHooks[]={reinterpret_cast<void*>(&RingInit<0>),reinterpret_cast<void*>(&RingInit<1>),reinterpret_cast<void*>(&RingInit<2>),
        reinterpret_cast<void*>(&TitleOptions<3>),reinterpret_cast<void*>(&TitleOptions<4>),reinterpret_cast<void*>(&MainMenu6)};
    void* drawHooks[]={reinterpret_cast<void*>(&InventoryOptions<0>),reinterpret_cast<void*>(&InventoryOptions<1>),reinterpret_cast<void*>(&InventoryOptions<2>),
        reinterpret_cast<void*>(&AddString<3>),reinterpret_cast<void*>(&AddString<4>),nullptr};
    if(r.draw && !s.draw.Install(base+r.draw,drawHooks[index],r.drawSize,r.drawBytes,r.drawSize,"VR native menu draw")) return;
    if(!s.title.Install(base+r.title,titleHooks[index],r.titleSize,r.titleBytes,r.titleSize,"VR native title")) { s.draw.Remove();return; }
    LogF("VR OPTIONS: native title entry installed for TR%d",index+1);
}
}
void VRNativeMenuUpdate() {
    for(int i=0;i<6;++i) {
        auto& s=g_slots[i];const auto module=GetModuleHandleW(Modules[i]);
        if(s.module && (module!=s.module || (s.title.installed() && At<uint8_t>(s,s.row->title)!=0xe9))) Detach(s,false);
        if(module && !s.attempted && Cfg().enabled && VR().active()) Attach(s,i,module);
        if(s.title.installed()) {
            if(i<3) UpdateRing(s);
            if(!InTitle()) { s.selection={};s.ringOpen=false; }
        }
    }
}
void VRNativeMenuShutdown() {
    for(int i=0;i<6;++i) if(g_slots[i].module) Detach(g_slots[i],GetModuleHandleW(Modules[i])==g_slots[i].module);
}
}
