#include "VRMenu.h"
#include "VRNativeMenu.h"
#include "VRMenuModel.h"
#include "Engine.h"
#include "VRSystem.h"
#include "Config.h"
#include "Log.h"
#include <windows.h>
#include <openvr.h>
#include <mutex>
#include <vector>

namespace tr {
namespace {
std::mutex g_menuMutex;
vrmenu::Model g_menu;
vr::IVROverlay* g_overlay=nullptr;
vr::IVRSystem* g_system=nullptr;
vr::VROverlayHandle_t g_handle=vr::k_ulOverlayHandleInvalid;
bool g_ready=false,g_shown=false,g_dirty=true,g_reanchor=true,g_attempted=false;
std::wstring g_message;

// TR1-3's title menu uses the inventory ring; InInventory must not exclude it.
bool Available() { return Cfg().enabled && VR().active() && InTitle(); }

// Native GDI rasterization keeps the menu independent of six different game
// fonts/renderers. OpenVR composites the same readable panel into both eyes.
std::vector<uint32_t> RenderMenu(const vrmenu::Model& model,const std::wstring& message,int& width,int& height) {
    width=900;height=720;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);
    if(!dc) return {};
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap || !pixels) { if(bitmap) DeleteObject(bitmap);DeleteDC(dc);return {}; }
    auto old=SelectObject(dc,bitmap);
    auto fill=[&](RECT rect,COLORREF color) { auto brush=CreateSolidBrush(color);FillRect(dc,&rect,brush);DeleteObject(brush); };
    fill({0,0,width,height},RGB(13,20,30));fill({0,0,width,5},RGB(210,172,103));
    SetBkMode(dc,TRANSPARENT);
    auto text=[&](const wchar_t* value,RECT rect,int size,COLORREF color,UINT format=DT_LEFT|DT_VCENTER|DT_SINGLELINE) {
        HFONT font=CreateFontW(-size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        auto previous=SelectObject(dc,font);SetTextColor(dc,color);DrawTextW(dc,value,-1,&rect,format|DT_NOPREFIX);
        SelectObject(dc,previous);DeleteObject(font);
    };
    const auto white=RGB(241,242,244),muted=RGB(171,184,200),gold=RGB(232,194,121);
    {
        text(L"VR OPTIONS",{42,20,858,83},44,white);
        wchar_t page[80]{};swprintf_s(page,L"CONTROLLERS   /   %d OF 3",model.page+1);
        text(page,{44,83,850,120},23,gold);
        for(int row=0;row<vrmenu::RowsPerPage;++row) {
            const int i=model.page*vrmenu::RowsPerPage+row;if(i>=vrmenu::SettingCount) break;
            const int top=143+row*68;
            if(model.selected==i) fill({28,top,872,top+52},RGB(42,59,77));
            text(vrmenu::Settings()[i].label,{44,top,710,top+52},26,model.selected==i ? gold : white);
            wchar_t value[64]{};
            if(i==0) wcscpy_s(value,model.values[i] ? L"INDEX" : L"TOUCH");
            else if(vrmenu::Settings()[i].toggle) wcscpy_s(value,model.values[i] ? L"ON" : L"OFF");
            else swprintf_s(value,i>=7 && i<=9 ? L"%.3f" : L"%.2f",model.values[i]);
            text(value,{710,top,850,top+52},26,white,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        }
        if(model.selected==vrmenu::SettingCount) fill({28,505,438,560},RGB(42,59,77));
        if(model.selected==vrmenu::SettingCount+1) fill({462,505,872,560},RGB(42,59,77));
        text(L"SAVE & CLOSE",{44,505,430,560},28,model.selected==vrmenu::SettingCount ? gold : white);
        text(L"CANCEL",{480,505,850,560},28,model.selected==vrmenu::SettingCount+1 ? gold : white);
        text(L"Left stick: select / adjust   A: select   Y: save   B: cancel",{44,578,865,620},23,muted);
        text(message.empty() ? L"Changes apply after restarting the game." : message.c_str(),{44,625,865,660},23,gold);
        text(L"First-person settings apply to TR1-5. Index needs its binding files.",{44,668,865,703},20,muted);
    }
    GdiFlush();
    const auto* source=static_cast<const uint32_t*>(pixels);
    std::vector<uint32_t> result(size_t(width)*height);
    for(size_t i=0;i<result.size();++i) {
        const uint32_t c=source[i]; // GDI BGR -> OpenVR RGBA, opaque panel.
        result[i]=0xff000000u | ((c&255)<<16) | (c&0xff00) | ((c>>16)&255);
    }
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    return result;
}

bool InitOverlay() {
    if(g_handle!=vr::k_ulOverlayHandleInvalid) return true;
    if(g_attempted) return false;g_attempted=true;
    auto module=GetModuleHandleW(L"openvr_api.dll");
    using GetInterface=void* (__cdecl*)(const char*,vr::EVRInitError*);
    auto get=module ? reinterpret_cast<GetInterface>(GetProcAddress(module,"VR_GetGenericInterface")) : nullptr;
    if(!get) return false;
    vr::EVRInitError error=vr::VRInitError_None;
    g_overlay=static_cast<vr::IVROverlay*>(get(vr::IVROverlay_Version,&error));
    if(error!=vr::VRInitError_None || !g_overlay) { g_overlay=nullptr;Log("VR OPTIONS: overlay interface unavailable");return false; }
    g_system=static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version,&error));
    if(error!=vr::VRInitError_None || !g_system) return false;
    char key[80]{};sprintf_s(key,"tombraider.vr.options.%lu",GetCurrentProcessId());
    if(g_overlay->CreateOverlay(key,"Tomb Raider VR Options",&g_handle)!=vr::VROverlayError_None) {
        g_handle=vr::k_ulOverlayHandleInvalid;Log("VR OPTIONS: could not create settings panel");return false;
    }
    g_overlay->SetOverlayInputMethod(g_handle,vr::VROverlayInputMethod_None);
    g_overlay->SetOverlaySortOrder(g_handle,100);
    return true;
}
bool AnchorOverlay() {
    const auto origin=Cfg().seatedOrigin ? vr::TrackingUniverseSeated : vr::TrackingUniverseStanding;
    vr::TrackedDevicePose_t head{};
    g_system->GetDeviceToAbsoluteTrackingPose(origin,0,&head,1);
    if(!head.bPoseIsValid || !head.bDeviceIsConnected) return false;
    // Use raw runtime tracking here, not the game's recentered tracking space.
    auto pose=head.mDeviceToAbsoluteTracking;
    const float distance=1.6f,down=0.f;
    for(int row=0;row<3;++row) pose.m[row][3]+=down*pose.m[row][1]-distance*pose.m[row][2];
    if(g_overlay->SetOverlayWidthInMeters(g_handle,1.3f)!=vr::VROverlayError_None) return false;
    return g_overlay->SetOverlayTransformAbsolute(g_handle,origin,&pose)==vr::VROverlayError_None;
}
}

bool VRMenuOpen() {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    if(!Available() || !g_ready || g_menu.open) return false;
    g_menu.Open();g_menu.values=g_menu.original=vrmenu::ReadSettings();
    g_message.clear();g_dirty=g_reanchor=true;
    return true;
}
bool VRMenuIsOpen() {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    return g_menu.open;
}
bool VRMenuBlocksInput() {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    return g_menu.open || g_menu.drain;
}
bool VRMenuInput(uint16_t buttons,int16_t leftX,int16_t leftY) {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    const auto event=g_menu.Input(g_ready && Available(),buttons,leftX,leftY,GetTickCount64());
    if(event==vrmenu::Event::Save) {
        if(vrmenu::SaveSettings(g_menu.values,g_menu.original,g_message)) {
            g_menu.open=false;g_menu.drain=true;g_message=L"Saved. Restart the game to apply.";g_reanchor=true;
        }
    } else if(event==vrmenu::Event::Cancel) { g_message.clear();g_reanchor=true; }
    if(event!=vrmenu::Event::None) g_dirty=true;
    return g_menu.consume;
}
void VRMenuUpdate() {
    VRNativeMenuUpdate();
    std::lock_guard<std::mutex> lock(g_menuMutex);
    if(!Available()) {
        if(g_shown && g_overlay) g_overlay->HideOverlay(g_handle);
        g_menu.open=g_menu.drain=false;g_shown=g_ready=false;g_dirty=g_reanchor=true;
        return;
    }
    g_ready=InitOverlay();
    if(!g_ready) return;
    if(!g_menu.open) {
        if(g_shown) g_overlay->HideOverlay(g_handle);
        g_shown=false;return;
    }
    if(g_reanchor) { if(!AnchorOverlay()) return;g_reanchor=false; }
    if(g_dirty) {
        int width=0,height=0;auto pixels=RenderMenu(g_menu,g_message,width,height);
        if(pixels.empty() || g_overlay->SetOverlayRaw(g_handle,pixels.data(),width,height,4)!=vr::VROverlayError_None) {
            g_overlay->HideOverlay(g_handle);g_ready=g_shown=false;g_menu.open=false;return;
        }
        g_dirty=false;
    }
    if(!g_shown) g_shown=g_overlay->ShowOverlay(g_handle)==vr::VROverlayError_None;
    g_ready=g_shown;
}
void VRMenuShutdown() {
    VRNativeMenuShutdown();
    std::lock_guard<std::mutex> lock(g_menuMutex);
    if(g_overlay && g_handle!=vr::k_ulOverlayHandleInvalid) g_overlay->DestroyOverlay(g_handle);
    g_handle=vr::k_ulOverlayHandleInvalid;g_overlay=nullptr;g_system=nullptr;
    g_menu={};g_ready=g_shown=g_attempted=false;g_dirty=g_reanchor=true;g_message.clear();
}
}
