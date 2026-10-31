#include "VRNativeMenu.h"
#include "VRNativeMenuModel.h"
#include "VRMenuModel.h"
#include "VRMenuSettings.h"
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
struct Extra {
    uint32_t stamp,options,subpage,selection,text,textCount,ringDraw,color,font,left,active,optionsMenu;
    uint8_t bytes[5];
};
#include "VRNativeMenuExtra.inl"

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
    const Extra* extra=nullptr;
    hook::InlineHook title,draw;
    vrmenu::OptionsSelection selection;
    vrmenu::ControllerPage page;
    bool scope=false,attempted=false,ringOpen=false;
    std::array<void*,6> list{};
    Option6 option{};
    Option6 controllerOption{};
    alignas(8) std::array<uint8_t,160> controllerMenu{};
    std::array<void*,4> texts{};
    std::array<std::array<char,128>,4> textValues{};
    void* menu6=nullptr;
};
Slot g_slots[6];
constexpr const wchar_t* Modules[]={L"tomb1.dll",L"tomb2.dll",L"tomb3.dll",L"tomb4.dll",L"tomb5.dll",L"tomb6.dll"};
constexpr char Label[]="VR Options";
bool Available() { return Cfg().enabled && VR().active() && InTitle(); }
template<class T> T& At(Slot& s,uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(s.module)+rva); }
template<class T> T& Field(void* p,size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+off); }
uint32_t HookRva(const Slot& s) { return s.extra->options ? s.extra->options : s.row->title; }
void OpenPage(Slot& s) { s.page.Open(vrmenu::ReadSettings()[0]!=0); }
bool PageInput(Slot& s,uint64_t input) {
    if(s.page.Input(input)==vrmenu::Action::Change) {
        auto original=vrmenu::ReadSettings(),values=original;
        values[0]=s.page.index ? 0.f : 1.f;
        std::wstring error;
        s.page.saveFailed=!vrmenu::SaveSettings(values,original,error);
        if(!s.page.saveFailed) s.page.index=values[0]!=0;
    }
    return s.page.open;
}
void Text45(Slot& s,float x,float y,uint8_t color,const char* text,uint16_t flags=8,float scale=.9f) {
    auto* record=s.draw.Original<void*(*)(float,float,uint8_t,const char*,uint16_t)>()(x,y,color,text,flags);
    if(record) Field<float>(record,16)=Field<float>(record,20)=scale;
}
void DrawPage45(Slot& s) {
    Text45(s,0,28,6,Label,8,1);
    Text45(s,21,77,1,"Controller Type",4);
    Text45(s,-20,77,1,s.page.Value(),2);
    Text45(s,0,125,2,"Left/Right: change   Back: return");
    Text45(s,0,157,s.page.saveFailed ? 3 : 2,s.page.Message());
}

template<int G> void* AddString(float x,float y,uint8_t color,const char* text,uint16_t flags) {
    auto& s=g_slots[G];
    using Fn=void*(*)(float,float,uint8_t,const char*,uint16_t);
    auto original=s.draw.Original<Fn>();
    if(s.scope && y>=45 && y<=221) {
        if(s.selection.vr && (color==1 || color==9)) color=color==9 ? 3 : 2;
        // Thirteen rows fit in the same native Options area at 14.4px spacing.
        // Apply equally to labels and values; submenus retain their own layout.
        y=45+(y-45)*.9f;
    }
    return original(x,y,color,text,flags);
}
template<int G> int Options(int inventory) {
    auto& s=g_slots[G];const auto& r=*s.row;const auto& e=*s.extra;
    if(!Available()) { s.page.open=false;s.selection={};return s.title.Original<int(*)(int)>()(inventory); }
    if(s.page.open) {
        PageInput(s,At<uint64_t>(s,r.dbinput));
        At<uint64_t>(s,r.dbinput)=At<uint64_t>(s,r.input)=0;
        if(s.page.open) { DrawPage45(s);return 0; }
    }
    const bool root=At<int32_t>(s,e.subpage)==0;
    if(!root) s.selection.vr=false;
    if(root) {
        auto& input=At<uint64_t>(s,r.dbinput);
        if(s.selection.Input(At<int32_t>(s,e.selection),input)) {
            OpenPage(s);At<uint64_t>(s,r.input)=input=0;DrawPage45(s);return 0;
        }
    }
    s.scope=root;
    const int result=s.title.Original<int(*)(int)>()(inventory);
    s.scope=false;
    if(root) Text45(s,0,219,s.selection.vr ? 1 : 2,Label);
    if(result || At<int32_t>(s,e.subpage)!=0) s.selection={};
    return result;
}

bool OwnText(Slot& s,int i) {
    auto* p=s.texts[i];
    return p && (Field<uint32_t>(p,0)&1) && Field<char*>(p,72) &&
        std::strcmp(Field<char*>(p,72),s.textValues[i].data())==0;
}
void ClearTexts(Slot& s) {
    if(!s.extra->textCount) return;
    for(int i=0;i<4;++i) {
        if(OwnText(s,i)) {
            Field<uint32_t>(s.texts[i],0)&=~1u;
            auto& count=At<int16_t>(s,s.extra->textCount);if(count>0) --count;
        }
        s.texts[i]=nullptr;
    }
}
void DrawPage123(Slot& s) {
    char value[128]{};sprintf_s(value,"Controller Type: < %s >",s.page.Value());
    const char* labels[]={Label,value,"Left/Right: change   Back: return",s.page.Message()};
    constexpr int16_t ys[]={-70,-38,-6,26};
    using Print=void*(*)(int16_t,int16_t,int16_t,const char*);
    auto print=reinterpret_cast<Print>(reinterpret_cast<uintptr_t>(s.module)+s.extra->text);
    for(int i=0;i<4;++i) {
        if(!OwnText(s,i)) s.texts[i]=nullptr;
        if(!s.texts[i]) s.texts[i]=print(0,ys[i],i==1 ? 1 : 0,labels[i]);
        else strcpy_s(Field<char*>(s.texts[i],72),512,labels[i]);
        strcpy_s(s.textValues[i].data(),s.textValues[i].size(),labels[i]);
        if(s.texts[i]) { Field<uint32_t>(s.texts[i],0)|=0x30;Field<int32_t>(s.texts[i],80)=0x10000; }
    }
}

void PrepareRing(Slot& s,void** list) {
    std::copy_n(list,5,s.list.begin());
    // Reuse the native controls object's mesh and transform. The clone has its
    // own identity and label, so it never invokes or changes Controls itself.
    std::memcpy(s.item.data(),list[2],s.item.size());
    Field<const char*>(s.item.data(),0)=Label;
    Field<int32_t>(s.item.data(),3264)=1;
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
    const auto input=At<uint32_t>(s,r.dbinput);
    At<uint32_t>(s,r.input)=At<uint32_t>(s,r.dbinput)=0;
    if(!s.ringOpen) { OpenPage(s);s.ringOpen=true; }
    else PageInput(s,input);
    if(!s.page.open) {
        // Native Back closes the selected object and returns to the ring. It
        // does not select an underlying game action or leave the title screen.
        ClearTexts(s);At<uint32_t>(s,r.dbinput)=0x200000;s.ringOpen=false;
    } else DrawPage123(s);
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
    auto* tail=head;int count=0;
    for(auto* p=head; p && count<14; p=p->next,++count) {
        if(p==&s.option) return true;
        tail=p;
    }
    if(Field<uint8_t>(menu,106)!=12 || count!=12 || !tail) return false;
    // The engine owns its fixed option pool. Our extra node is outside it and
    // uses a literal label, so language reloads cannot overwrite another row.
    s.option={nullptr,tail,0x80000000u,-1,nullptr,nullptr,Label,0,0,{}};
    tail->next=&s.option;
    ++Field<uint8_t>(menu,106);
    s.menu6=menu;
    return true;
}
void DrawPage6(Slot& s) {
    const auto& e=*s.extra;
    sprintf_s(s.textValues[0].data(),s.textValues[0].size(),"Controller Type: %s",s.page.Value());
    s.controllerOption={nullptr,nullptr,0x80000000u,-1,nullptr,nullptr,s.textValues[0].data(),1,0,{}};
    auto* menu=s.controllerMenu.data();
    Field<uint8_t>(menu,106)=1;Field<uint8_t>(menu,107)=0;
    Field<Option6*>(menu,120)=&s.controllerOption;
    auto base=reinterpret_cast<uintptr_t>(s.module);
    reinterpret_cast<void(*)(void*,int)>(base+e.ringDraw)(menu,0);
    // menuRing establishes the native orthographic/font render pass. Keep its
    // row, and use the same font calls for the heading and restart notice.
    const auto oldFont=At<int32_t>(s,e.font);const auto oldColor=At<uint32_t>(s,e.color);
    auto text=reinterpret_cast<void(*)(int,int,const char*,int)>(base+e.text);
    At<int32_t>(s,e.font)=4;At<uint32_t>(s,e.color)=0xc07ebefe;
    text(At<int32_t>(s,e.left)+40,32,Label,0);
    At<int32_t>(s,e.font)=2;At<uint32_t>(s,e.color)=0xfefefefe;
    text(At<int32_t>(s,e.left)+40,400,s.page.Message(),0);
    At<int32_t>(s,e.font)=oldFont;At<uint32_t>(s,e.color)=oldColor;
}
uint8_t Options6(void* menu,uint8_t event) {
    auto& s=g_slots[5];const auto& r=*s.row;const auto& e=*s.extra;
    auto original=s.title.Original<uint8_t(*)(void*,uint8_t)>();
    if(!Available() || menu!=At<void*>(s,e.optionsMenu) || !InsertOption6(s,menu))
        return original(menu,event);
    if(s.page.open) {
        if(event==6) PageInput(s,0x200000);
        else if(event==3 || event==4 || event==5) PageInput(s,0x100000);
        if(event==11) DrawPage6(s);
        return 0;
    }
    auto& selected=Field<uint8_t>(menu,107);
    if(event==5 && selected==12) {
        if(At<float>(s,r.choice)==0) OpenPage(s);
        return 0;
    }
    return original(menu,event);
}

void Restore(Slot& s) {
    if(!s.row || !s.extra) return;
    const auto& r=*s.row;
    if(r.game<=3) {
        ClearTexts(s);
        void* ring=&At<uint8_t>(s,r.transition);
        if(Field<void**>(ring,0)==s.list.data()) {
            Field<void**>(ring,0)=&At<void*>(s,r.reset);
            Field<int16_t>(ring,22)=5;
            Field<int16_t>(ring,18)=std::min<int16_t>(Field<int16_t>(ring,18),4);
            Field<int16_t>(ring,24)=65536/5;
            Field<int16_t>(ring,46)=int16_t(-16384-Field<int16_t>(ring,18)*(65536/5));
        }
        if(At<int16_t>(s,r.choice)>4) At<int16_t>(s,r.choice)=0;
    } else if(r.game==6 && s.menu6 && At<void*>(s,s.extra->optionsMenu)==s.menu6) {
        auto* p=Field<Option6*>(s.menu6,120);
        for(int i=0;p && i<14;++i,p=p->next) if(p==&s.option) {
            p->last->next=p->next;if(p->next) p->next->last=p->last;
            --Field<uint8_t>(s.menu6,106);
            auto& selected=Field<uint8_t>(s.menu6,107);
            if(selected>=12) selected=11;
            break;
        }
    }
}
void Detach(Slot& s,bool mapped) {
    if(mapped) { Restore(s);s.draw.Remove();s.title.Remove(); }
    else { s.draw.Abandon();s.title.Abandon(); }
    s.module=nullptr;s.row=nullptr;s.extra=nullptr;s.attempted=s.scope=s.ringOpen=false;
    s.selection={};s.page={};s.texts={};s.menu6=nullptr;
}
void Attach(Slot& s,int index,HMODULE module) {
    s.module=module;s.attempted=true;
    auto* base=reinterpret_cast<uint8_t*>(module);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return;
    const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(pe->Signature!=IMAGE_NT_SIGNATURE) return;
    for(const auto& r:Rows) if(r.game==index+1 && r.stamp==pe->FileHeader.TimeDateStamp) { s.row=&r;break; }
    for(const auto& e:Extras) if(e.stamp==pe->FileHeader.TimeDateStamp) { s.extra=&e;break; }
    if(!s.row || !s.extra) { LogF("VR Options: unsupported native menu TR%d stamp %08X",index+1,pe->FileHeader.TimeDateStamp);return; }
    const auto& r=*s.row;
    for(const auto address:{r.title,r.draw,r.menu,r.choice,r.transition,r.input,r.dbinput,r.reset})
        if(address && address+8>=pe->OptionalHeader.SizeOfImage) { s.row=nullptr;return; }
    const auto& e=*s.extra;
    for(const auto address:{e.options,e.subpage,e.selection,e.text,e.textCount,e.ringDraw,e.color,e.font,e.left,e.active,e.optionsMenu})
        if(address && address+8>=pe->OptionalHeader.SizeOfImage) { s.row=nullptr;return; }
    const auto target=HookRva(s);
    const auto size=s.extra->options ? 5 : r.titleSize;
    const auto* bytes=s.extra->options ? s.extra->bytes : r.titleBytes;
    if(std::memcmp(base+target,bytes,size) ||
       (r.draw && std::memcmp(base+r.draw,r.drawBytes,r.drawSize))) {
        LogF("VR OPTIONS: native title signature mismatch TR%d",index+1);s.row=nullptr;return;
    }
    void* titleHooks[]={reinterpret_cast<void*>(&RingInit<0>),reinterpret_cast<void*>(&RingInit<1>),reinterpret_cast<void*>(&RingInit<2>),
        reinterpret_cast<void*>(&Options<3>),reinterpret_cast<void*>(&Options<4>),reinterpret_cast<void*>(&Options6)};
    void* drawHooks[]={reinterpret_cast<void*>(&InventoryOptions<0>),reinterpret_cast<void*>(&InventoryOptions<1>),reinterpret_cast<void*>(&InventoryOptions<2>),
        reinterpret_cast<void*>(&AddString<3>),reinterpret_cast<void*>(&AddString<4>),nullptr};
    if(r.draw && !s.draw.Install(base+r.draw,drawHooks[index],r.drawSize,r.drawBytes,r.drawSize,"VR native menu draw")) return;
    if(!s.title.Install(base+target,titleHooks[index],size,bytes,size,"VR native options")) { s.draw.Remove();return; }
    LogF("VR Options: native Controller Type page installed for TR%d",index+1);
}
}
void VRNativeMenuUpdate() {
    for(int i=0;i<6;++i) {
        auto& s=g_slots[i];const auto module=GetModuleHandleW(Modules[i]);
        if(s.module && (module!=s.module || (s.title.installed() && At<uint8_t>(s,HookRva(s))!=0xe9))) Detach(s,false);
        if(module && !s.attempted && Cfg().enabled && VR().active()) Attach(s,i,module);
        if(s.title.installed()) {
            if(i<3) UpdateRing(s);
            if(!InTitle()) { Restore(s);s.selection={};s.page={};s.ringOpen=false; }
        }
    }
}
void VRNativeMenuShutdown() {
    for(int i=0;i<6;++i) if(g_slots[i].module) Detach(g_slots[i],GetModuleHandleW(Modules[i])==g_slots[i].module);
}
}
