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
// TR1-3 retain legacy layout metadata here; only Extra.options is hooked.
// No inventory ring objects, lists, counts, transforms or selections are changed.
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
    uint32_t nativeTexts=0,firstVisible=0,textPool=0,textBuffers=0;
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
    HMODULE module=nullptr;
    const Row* row=nullptr;
    const Extra* extra=nullptr;
    hook::InlineHook title,draw;
    vrmenu::OptionsSelection selection;
    vrmenu::ControllerPage page;
    bool scope=false,attempted=false;
    vrmenu::Options123Selection options123;
    Option6 option{};
    std::array<Option6,vrmenu::SettingCount> controllerOptions{};
    alignas(8) std::array<uint8_t,160> controllerMenu{};
    static constexpr int TextCount=vrmenu::SettingCount+3; // heading, settings, navigation, help
    std::array<void*,TextCount> texts{};
    std::array<std::array<char,128>,TextCount> textValues{};
    void* menu6=nullptr;
};
Slot g_slots[6];
constexpr const wchar_t* Modules[]={L"tomb1.dll",L"tomb2.dll",L"tomb3.dll",L"tomb4.dll",L"tomb5.dll",L"tomb6.dll"};
constexpr char Label[]="VR Options";
// These hooks run inside the native Options handlers at both title and pause.
// Gameplay is not a reason to hide the VR submenu.
bool Available() { return Cfg().enabled && VR().active(); }
template<class T> T& At(Slot& s,uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(s.module)+rva); }
template<class T> T& Field(void* p,size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+off); }
uint32_t HookRva(const Slot& s) { return s.extra->options ? s.extra->options : s.row->title; }
void OpenPage(Slot& s) {
    const auto values=vrmenu::ReadSettings();s.page.Open(values[0]!=0,values[1]!=0,values[2]!=0);
}
bool PageInput(Slot& s,uint64_t input) {
    if(s.page.Input(input)==vrmenu::Action::Change) {
        auto original=vrmenu::ReadSettings(),values=original;
        const int row=s.page.selected;
        const bool enabled=s.page.Enabled(row);
        values[row]=enabled ? 0.f : 1.f;
        std::wstring error;
        s.page.saveFailed=!vrmenu::SaveSettings(values,original,error);
        if(!s.page.saveFailed) { s.page.index=values[0]!=0;s.page.bright=values[1]!=0;s.page.hybrid=values[2]!=0; }
    }
    return s.page.open;
}
void Text45(Slot& s,float x,float y,uint8_t color,const char* text,uint16_t flags=8,float scale=.9f) {
    auto* record=s.draw.Original<void*(*)(float,float,uint8_t,const char*,uint16_t)>()(x,y,color,text,flags);
    if(record) Field<float>(record,16)=Field<float>(record,20)=scale;
}
void DrawPage45(Slot& s) {
    Text45(s,0,28,6,Label,8,1);
    for(int row=0;row<vrmenu::SettingCount;++row) {
        const uint8_t color=s.page.selected==row ? 1 : 2;
        Text45(s,21,69+row*24.f,color,s.page.Name(row),4);
        Text45(s,-20,69+row*24.f,color,s.page.Value(row),2);
    }
    const float footer=69+vrmenu::SettingCount*24.f+5;
    Text45(s,0,footer,2,"Up/Down: select   Left/Right: change");
    Text45(s,0,footer+20,2,"Back: return");
    Text45(s,0,footer+48,s.page.saveFailed ? 3 : 2,s.page.Message());
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

bool ValidText123(Slot& s,void* p) {
    if(!p || !s.extra->textPool || !s.extra->textBuffers) return false;
    const auto first=reinterpret_cast<uintptr_t>(&At<uint8_t>(s,s.extra->textPool));
    const auto address=reinterpret_cast<uintptr_t>(p);
    if(address<first || address>=first+64*88 || (address-first)%88) return false;
    const auto expected=&At<char>(s,s.extra->textBuffers+uint32_t((address-first)/88)*512);
    return Field<char*>(p,72)==expected;
}
void ReleaseText123(Slot& s,void* p) {
    if(ValidText123(s,p) && (Field<uint32_t>(p,0)&1)) {
        Field<uint32_t>(p,0)&=~1u;
        auto& count=At<int16_t>(s,s.extra->textCount);if(count>0) --count;
    }
}
bool OwnText(Slot& s,int i) {
    auto* p=s.texts[i];
    return ValidText123(s,p) && (Field<uint32_t>(p,0)&1) &&
        std::strncmp(Field<char*>(p,72),s.textValues[i].data(),s.textValues[i].size())==0;
}
void ClearTexts(Slot& s) {
    if(!s.extra->textCount) return;
    for(int i=0;i<Slot::TextCount;++i) {
        if(OwnText(s,i)) ReleaseText123(s,s.texts[i]);
        s.texts[i]=nullptr;
    }
}
void WriteText123(Slot& s,int i,const char* label,int16_t y,int16_t color) {
    if(!OwnText(s,i)) s.texts[i]=nullptr;
    if(!s.texts[i]) {
        using Print=void*(*)(int16_t,int16_t,int16_t,const char*);
        auto print=reinterpret_cast<Print>(reinterpret_cast<uintptr_t>(s.module)+s.extra->text);
        s.texts[i]=print(0,y,color,label);
    } else strcpy_s(Field<char*>(s.texts[i],72),512,label);
    strcpy_s(s.textValues[i].data(),s.textValues[i].size(),label);
    if(ValidText123(s,s.texts[i])) {
        Field<uint32_t>(s.texts[i],0)|=0x30;Field<int32_t>(s.texts[i],80)=0x10000;
    } else s.texts[i]=nullptr;
}
void DrawPage123(Slot& s) {
    // T_Decode consumes <...> as an icon token (unknown tokens draw nothing).
    // Keep setting values as plain text so every choice remains visible.
    WriteText123(s,0,Label,-90,0);
    for(int row=0;row<vrmenu::SettingCount;++row) {
        char value[128]{};
        sprintf_s(value,"%s%s: %s",s.page.selected==row ? "> " : "  ",s.page.Name(row),s.page.Value(row));
        WriteText123(s,row+1,value,int16_t(-58+row*28),1);
    }
    const int16_t footer=int16_t(-58+vrmenu::SettingCount*28+12);
    WriteText123(s,vrmenu::SettingCount+1,"Up/Down: select  Left/Right: change  Back: return",footer,0);
    WriteText123(s,vrmenu::SettingCount+2,s.page.Message(),footer+28,0);
}

void ClearNativeTexts123(Slot& s) {
    // These are the native Options page's twenty owned text records. Releasing
    // them mirrors its own close path, without closing the inventory object.
    for(int i=0;i<20;++i) {
        auto& p=At<void*>(s,s.extra->nativeTexts+i*8);
        ReleaseText123(s,p);p=nullptr;
    }
}
void DrawOptionsRow123(Slot& s) {
    const bool visible=At<int32_t>(s,s.extra->firstVisible)==3;
    auto* background=At<void*>(s,s.extra->nativeTexts+13*8);
    if(ValidText123(s,background)) Field<int16_t>(background,58)=visible ? 206 : 190;
    if(!visible) { ClearTexts(s);return; }
    WriteText123(s,0,Label,90,0);
    if(s.options123.vr) {
        auto* highlight=At<void*>(s,s.extra->nativeTexts+15*8);
        if(ValidText123(s,highlight)) Field<float>(highlight,16)=90.f;
        // The original selection stays at row 12. Hide that row's value arrows
        // while the extra row is selected, and consume its change inputs.
        for(int i:{16,17}) {
            auto& p=At<void*>(s,s.extra->nativeTexts+i*8);ReleaseText123(s,p);p=nullptr;
        }
    }
}
template<int G> int Options123() {
    auto& s=g_slots[G];const auto& r=*s.row;const auto& e=*s.extra;
    auto original=s.title.Original<int(*)()>();
    if(!Available()) { ClearTexts(s);s.page={};s.options123={};return original(); }
    if(s.page.open) {
        PageInput(s,vrmenu::Options123Buttons(At<uint32_t>(s,r.dbinput)));
        At<uint32_t>(s,r.dbinput)=At<uint32_t>(s,r.input)=0;
        if(s.page.open) { DrawPage123(s);return 0; }
        // Consume the child Back. Rebuild the parent with VR Options selected;
        // a second Back uses the native parent close path.
        ClearTexts(s);
    }
    auto& input=At<uint32_t>(s,r.dbinput);
    if(s.options123.Input(At<int16_t>(s,e.selection),input)) {
        ClearTexts(s);ClearNativeTexts123(s);OpenPage(s);
        At<uint32_t>(s,r.input)=input=0;DrawPage123(s);return 0;
    }
    const int result=original();
    if(result || At<int32_t>(s,e.subpage)!=0) {
        ClearTexts(s);s.options123={};
    } else DrawOptionsRow123(s);
    return result;
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
void PreparePage6(Slot& s) {
    for(int row=0;row<vrmenu::SettingCount;++row) {
        sprintf_s(s.textValues[row].data(),s.textValues[row].size(),"%s: %s",s.page.Name(row),s.page.Value(row));
        s.controllerOptions[row]={row+1<vrmenu::SettingCount ? &s.controllerOptions[row+1] : nullptr,row ? &s.controllerOptions[row-1] : nullptr,
            0x80000000u,-1,nullptr,nullptr,s.textValues[row].data(),1,0,{}};
    }
    auto* menu=s.controllerMenu.data();
    Field<uint8_t>(menu,106)=vrmenu::SettingCount;Field<uint8_t>(menu,107)=uint8_t(s.page.selected);
    Field<Option6*>(menu,120)=s.controllerOptions.data();
}
void DrawPage6(Slot& s) {
    const auto& e=*s.extra;
    PreparePage6(s);
    auto* menu=s.controllerMenu.data();
    auto base=reinterpret_cast<uintptr_t>(s.module);
    reinterpret_cast<void(*)(void*,int)>(base+e.ringDraw)(menu,0);
    // menuRing establishes the native orthographic/font render pass. Keep its
    // row, and use the same font calls for the heading and restart notice.
    const auto oldFont=At<int32_t>(s,e.font);const auto oldColor=At<uint32_t>(s,e.color);
    auto text=reinterpret_cast<void(*)(int,int,const char*,int)>(base+e.text);
    At<int32_t>(s,e.font)=4;At<uint32_t>(s,e.color)=0xc07ebefe;
    text(At<int32_t>(s,e.left)+40,32,Label,0);
    At<int32_t>(s,e.font)=2;At<uint32_t>(s,e.color)=0xfefefefe;
    text(At<int32_t>(s,e.left)+40,400,s.page.Message(true),0);
    At<int32_t>(s,e.font)=oldFont;At<uint32_t>(s,e.color)=oldColor;
}
uint8_t Options6(void* menu,uint8_t event) {
    auto& s=g_slots[5];const auto& r=*s.row;const auto& e=*s.extra;
    auto original=s.title.Original<uint8_t(*)(void*,uint8_t)>();
    if(!Available() || menu!=At<void*>(s,e.optionsMenu) || !InsertOption6(s,menu))
        return original(menu,event);
    if(s.page.open) {
        if(event==6) PageInput(s,0x200000);
        else if(event==1) PageInput(s,0x10000000);
        else if(event==2) PageInput(s,0x20000000);
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
void UpdateMenuState(Slot& s) {
    // TR6 can show its pause Options ring without the collection inventory
    // flag. Its active-menu pointer identifies that native menu directly.
    const bool options6=s.row->game==6 && s.extra->active && s.extra->optionsMenu &&
        At<void*>(s,s.extra->active) && At<void*>(s,s.extra->active)==At<void*>(s,s.extra->optionsMenu);
    if(Available() && (InTitle() || InInventory() || options6)) return;
    Restore(s);s.selection={};s.options123={};s.page={};s.scope=false;
}
void Detach(Slot& s,bool mapped) {
    if(mapped) { Restore(s);s.draw.Remove();s.title.Remove(); }
    else { s.draw.Abandon();s.title.Abandon(); }
    s.module=nullptr;s.row=nullptr;s.extra=nullptr;s.attempted=s.scope=false;
    s.selection={};s.options123={};s.page={};s.texts={};s.menu6=nullptr;
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
    for(const auto address:{e.options,e.subpage,e.selection,e.text,e.textCount,e.ringDraw,e.color,e.font,e.left,e.active,e.optionsMenu,e.nativeTexts,e.firstVisible,e.textPool,e.textBuffers})
        if(address && address+8>=pe->OptionalHeader.SizeOfImage) { s.row=nullptr;return; }
    if(r.game<=3 && (!e.options || !e.nativeTexts || !e.textPool || !e.textBuffers ||
       uint64_t(e.nativeTexts)+160>pe->OptionalHeader.SizeOfImage ||
       uint64_t(e.textPool)+64*88>pe->OptionalHeader.SizeOfImage ||
       uint64_t(e.textBuffers)+64*512>pe->OptionalHeader.SizeOfImage)) { s.row=nullptr;return; }
    const bool hasDraw=r.game==4 || r.game==5;
    const auto target=HookRva(s);
    const auto size=s.extra->options ? 5 : r.titleSize;
    const auto* bytes=s.extra->options ? s.extra->bytes : r.titleBytes;
    if(std::memcmp(base+target,bytes,size) ||
       (hasDraw && std::memcmp(base+r.draw,r.drawBytes,r.drawSize))) {
        LogF("VR Options: native menu signature mismatch TR%d",index+1);s.row=nullptr;return;
    }
    void* titleHooks[]={reinterpret_cast<void*>(&Options123<0>),reinterpret_cast<void*>(&Options123<1>),reinterpret_cast<void*>(&Options123<2>),
        reinterpret_cast<void*>(&Options<3>),reinterpret_cast<void*>(&Options<4>),reinterpret_cast<void*>(&Options6)};
    void* drawHooks[]={nullptr,nullptr,nullptr,
        reinterpret_cast<void*>(&AddString<3>),reinterpret_cast<void*>(&AddString<4>),nullptr};
    if(hasDraw && !s.draw.Install(base+r.draw,drawHooks[index],r.drawSize,r.drawBytes,r.drawSize,"VR native menu draw")) return;
    if(!s.title.Install(base+target,titleHooks[index],size,bytes,size,"VR native options")) { s.draw.Remove();return; }
    LogF("VR Options: native Options submenu installed for TR%d",index+1);
}
}
void VRNativeMenuUpdate() {
    for(int i=0;i<6;++i) {
        auto& s=g_slots[i];const auto module=GetModuleHandleW(Modules[i]);
        if(s.module && (module!=s.module || (s.title.installed() && At<uint8_t>(s,HookRva(s))!=0xe9))) Detach(s,false);
        if(module && !s.attempted && Cfg().enabled && VR().active()) Attach(s,i,module);
        if(s.title.installed()) UpdateMenuState(s);
    }
}
void VRNativeMenuShutdown() {
    for(int i=0;i<6;++i) if(g_slots[i].module) Detach(g_slots[i],GetModuleHandleW(Modules[i])==g_slots[i].module);
}
}
