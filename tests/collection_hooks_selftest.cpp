// Exercises the actual background detour with fake engine calls and real
// OpenGL colour/depth buffers; no game or headset is required.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#define private public
#include "../src/InlineHook.h"
#include "../src/VRSystem.h"
#undef private
#include "../src/CollectionParity.cpp"
#include "../src/InventoryProjection.h"

namespace {
int checks=0, nativeCalls=0, roomCalls=0, flushCalls=0;
int clears=0, restores=0;
bool menuCamera=true;
using ClearFn=void (__cdecl*)(int);
alignas(16) unsigned char appMemory[0x140]{};
unsigned char* app=appMemory;
bool active=true, recurse=false, cameraReady=true;
tr::Config config;
tr::GameDllLayout dll{};
uint64_t moduleBase=0;
alignas(16) unsigned char roomMemory[168*2]{};
unsigned char* rooms=roomMemory;
int16_t roomCount=2;
alignas(16) unsigned char camera[112]{};
void Check(bool ok,const char* label) {
    ++checks;
    if(!ok) { std::printf("FAIL: %s\n",label);std::exit(1); }
}
void __cdecl Native(int) { ++nativeCalls; }
void __cdecl DrawRooms(int room) {
    ++roomCalls;
    Check(tr::InventorySceneDrawing(),"scene uses gameplay stereo camera during native draws");
    Check(!menuCamera,"background retains gameplay camera");
    Check(room==1,"native scene receives camera room");
    for(int i=0;i<2;++i) {
        auto& bits=*reinterpret_cast<uint16_t*>(roomMemory+i*168+78);
        Check(bits==0,"both native room draw bytes reset");
        bits|=3;
    }
    if(recurse) tr::collection::BufferBackground(0);
    // Native drawing fills both eye halves and leaves a restricted scissor
    // and disabled depth writes, as the game's last material can do.
    glEnable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);
    glScissor(0,0,8,8);glClearColor(1,0,0,1);glClearDepth(.25);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glScissor(8,0,8,8);glClearColor(0,1,0,1);glClearDepth(.5);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glClearDepth(1);glDepthMask(GL_FALSE);
}
void __cdecl NativeClear(int mask) {
    Check(mask==3,"native initialization unconditionally clears colour AND depth");
    ++clears;glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
}
void __cdecl Init(int clear) {
    menuCamera=!tr::InventorySceneDrawing();
    Check(clear==(menuCamera?1:0),"native menu and background initialization use correct camera and clear colour");
    if(menuCamera) ++restores;
    (*reinterpret_cast<ClearFn*>(app+0x138))(3);
}
void __cdecl Flush() {
    ++flushCalls;
    Check(tr::InventorySceneDrawing(),"queued scene draws flush before switching back to menu camera");
    glFlush();
}
uint32_t Rva(const void* p) {
    const auto delta=reinterpret_cast<uint64_t>(p)-moduleBase;
    Check(delta<=UINT32_MAX,"fixture address fits engine RVA");
    return uint32_t(delta);
}
}
void Log(const char*) {}
void LogF(const char*,...) {}
namespace hook {
InlineHook::~InlineHook() {}
bool InlineHook::Install(void*,void*,size_t,const uint8_t*,size_t,const char*,const int*,size_t) { return false; }
void InlineHook::Remove() { m_installed=false; }
}
namespace tr {
VRSystem testVR; Layout testLayout{};
VRSystem& VR() { return testVR; }
uint64_t Base() { return moduleBase; }
const Layout& L() { return testLayout; }
int CurrentGame() { return 0; }
bool followFlare=true;int32_t tip[3]={100,200,300};
bool FirstPersonFlareTip(int32_t (&out)[3]) { std::memcpy(out,tip,12);return followFlare; }
const Config& Cfg() { return config; }
const GameDllLayout* GameDllBound() { return &dll; }
uint64_t GameDllBase() { return moduleBase; }
bool WorldInventoryActive() { return active; }
bool InventorySceneCameraReady() { return cameraReady; }
}
namespace {
int32_t focused=0;int focusMessages=0;
void __cdecl MessagesMock() { Check(focused==0,"native message pump sees actual focus");++focusMessages; }
void __cdecl PollMock() {
    tr::collection::Messages();
    Check(focused==1,"VR native input poll continues after unfocused message pump");
    tr::collection::Messages();
    Check(focused==1,"repeated message pumps restore and reapply focus override");
}
int32_t dynamicsCount=0;uint8_t dynamics[64*20]{};
int32_t flareArgument[3]{};
int __cdecl FlareMock(int32_t* point,int,int) {
    std::memcpy(flareArgument,point,12);
    auto* record=dynamics+20*dynamicsCount++;
    std::memcpy(record,point,12);reinterpret_cast<int32_t*>(record)[1]-=64;
    record[12]=9;record[17]=231;record[18]=112;record[19]=30;return 7;
}
void TestFocusAndFlare(tr::collection::DllBuild& row) {
    using namespace tr::collection;
    tr::testVR.m_system=reinterpret_cast<vr::IVRSystem*>(1);
    windowActive=&focused;messagesHook.m_trampoline=reinterpret_cast<void*>(&MessagesMock);
    updateHook.m_trampoline=reinterpret_cast<void*>(&PollMock);
    InputUpdate();Check(focused==0 && focusMessages==2 && !focusScope,"focus restoration covers entire native poll");
    config.enabled=false;
    { FocusScope scope;scope.AfterMessages();Check(focused==0,"disabled VR preserves actual desktop focus"); }
    config.enabled=true;
    row.dynamics=Rva(dynamics);row.numberDynamics=Rva(&dynamicsCount);
    flareHook.m_trampoline=reinterpret_cast<void*>(&FlareMock);
    int32_t native[3]={-1,-2,-3};
    Check(Flare(native,40,1)==7 && !std::memcmp(flareArgument,tr::tip,12),"held flare uses tracked tip and native age/result");
    Check(!std::memcmp(dynamics,tr::tip,12) && dynamics[17]==231,"light stays at rendered tip with native flicker colour");
    tr::tip[0]+=50;RefreshFlareLight();
    Check(reinterpret_cast<int32_t*>(dynamics)[0]==150,"render-time light follows latest hand pose");
    dynamics[17]=99;tr::tip[0]+=50;RefreshFlareLight();
    Check(light.index==-1 && reinterpret_cast<int32_t*>(dynamics)[0]==150,"recycled dynamic-light slot is never animated");
    Flare(native,40,0);Check(!std::memcmp(flareArgument,native,12),"dropped flares retain native world light");
    tr::followFlare=false;Flare(native,40,1);
    Check(!std::memcmp(flareArgument,native,12) && light.index==-1,"tracking loss restores native held flare and invalidates cache");
    tr::followFlare=true;Flare(native,40,1);light.updated=GetTickCount64()-151;
    auto x=reinterpret_cast<int32_t*>(dynamics+20*light.index)[0];tr::tip[0]+=20;RefreshFlareLight();
    Check(reinterpret_cast<int32_t*>(dynamics+20*light.index)[0]==x,"stale flare snapshot is not refreshed");
    tr::testVR.m_system=nullptr;
}
void TestProjection() {
    using namespace tr;
    auto transform=[](const mat4& m,const float (&p)[4],float (&out)[4]) {
        for(int r=0;r<4;++r) { out[r]=0;for(int c=0;c<4;++c) out[r]+=m.m[c*4+r]*p[c]; }
    };
    auto close=[](float a,float b) { return std::abs(a-b)<0.0001f*std::max(1.f,std::max(std::abs(a),std::abs(b))); };
    for(float scale:{256.f,512.f,1024.f}) for(float aspect:{4.f/3,16.f/9,21.f/9})
    for(float flip:{-1.f,1.f}) for(float fov:{.5f,1.f,1.5f}) {
        const float z=4*scale,w=z*.52f,h=flip*w/aspect;
        mat4 native{};native.m[0]=1/fov;native.m[5]=-aspect/fov;
        native.m[8]=.13f;native.m[9]=-.07f;native.m[10]=-1.001f;native.m[11]=-1;native.m[14]=-32;
        const auto placement=inventory::MenuToAnchor(native,true,true,z,w,h,.25f*scale);
        for(float distance:{320.f,640.f,1280.f,2560.f}) {
            const float p[4]={73,-31,-distance,1};float clip[4]{},anchor[4]{};
            transform(native,p,clip);transform(placement,p,anchor);
            Check(close(anchor[0]/-anchor[2],clip[0]/clip[3]*w/z) &&
                  close(anchor[1]/-anchor[2],clip[1]/clip[3]*h/z),"perspective menu preserves native screen layout including off-axis offsets at all depths");
            Check(close(anchor[2],-z*distance/640) && anchor[3]==1,"mesh volume and front/back ordering survive spatial normalization");
        }
        mat4 ortho{};ortho.m[0]=ortho.m[5]=ortho.m[10]=ortho.m[15]=1;
        const auto label=inventory::MenuToAnchor(ortho,false,false,z,w,h,.25f*scale);
        const float p[4]={73,-31,-640,1};float clip[4]{};transform(native,p,clip);
        const float text[4]={clip[0]/clip[3],clip[1]/clip[3],.9f,1};
        for(float yaw:{-45.f,0.f,35.f}) for(float eye:{-.032f,.032f}) {
            Affine e=RotY(yaw);e.r[0][3]=(.2f+eye)*scale;e.r[1][3]=-.1f*scale;e.r[2][3]=.3f*scale;
            mat4 hmd=native;hmd.m[8]=eye>0?.15f:-.15f;
            const mat4 view=Mul4(hmd,AffineToMat4(e));
            float a[4]{},b[4]{};transform(Mul4(view,placement),p,a);transform(Mul4(view,label),text,b);
            for(int r=0;r<4;++r) Check(close(a[r],b[r]),"ring reference point and its label coincide in both eyes through head rotation and translation");
        }
        const auto mesh=inventory::MenuToAnchor(ortho,false,true,z,w,h,.25f*scale);
        Check(mesh.m[10]!=0 && label.m[10]==0,"orthographic meshes retain depth while text stays on its plane");
    }
}
}
int main() {
    TestProjection();
    WNDCLASSA wc{};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandle(nullptr);
    wc.lpszClassName="InventorySceneGLTest";wc.style=CS_OWNDC;
    Check(RegisterClassA(&wc)!=0,"register hidden GL window");
    const HWND window=CreateWindowA(wc.lpszClassName,"",WS_POPUP,0,0,16,8,nullptr,nullptr,wc.hInstance,nullptr);
    Check(window!=nullptr,"create hidden GL window");
    const HDC dc=GetDC(window);
    PIXELFORMATDESCRIPTOR pf{};pf.nSize=sizeof(pf);pf.nVersion=1;
    pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;pf.iPixelType=PFD_TYPE_RGBA;
    pf.cColorBits=24;pf.cDepthBits=24;
    const int format=ChoosePixelFormat(dc,&pf);
    Check(format && SetPixelFormat(dc,format,&pf),"create colour and depth buffers");
    const auto context=wglCreateContext(dc);
    Check(context && wglMakeCurrent(dc,context),"activate real OpenGL context");
    glViewport(0,0,16,8);
    moduleBase=reinterpret_cast<uint64_t>(GetModuleHandle(nullptr));
    dll.camera=Rva(camera);dll.room=Rva(&rooms);dll.numberRooms=Rva(&roomCount);
    *reinterpret_cast<int16_t*>(camera+12)=1;
    tr::collection::DllBuild build{};
    build.app=Rva(&app);*reinterpret_cast<ClearFn*>(app+0x138)=&NativeClear;
    build.rooms=Rva(reinterpret_cast<void*>(&DrawRooms));build.flush=Rva(reinterpret_cast<void*>(&Flush));
    build.initialize.address=Rva(reinterpret_cast<void*>(&Init));
    tr::collection::base=moduleBase;tr::collection::layout=&dll;tr::collection::build=&build;
    tr::collection::backgroundHook.m_installed=true;
    tr::collection::backgroundHook.m_trampoline=reinterpret_cast<void*>(&Native);
    Check(tr::InventorySceneReady(),"matching hooked module is ready");
    for(int i=0;i<2;++i) *reinterpret_cast<uint16_t*>(roomMemory+i*168+78)=0xFFFF;
    for(int frame=0;frame<3;++frame) {
        tr::collection::BufferBackground(0);
        Check(roomCalls==frame+1 && flushCalls==frame+1 && nativeCalls==0,"redraw every frame without native backdrop");
        Check(!tr::InventorySceneDrawing(),"menu camera restored after scene flush");
        Check(menuCamera && restores==frame+1 && clears==frame+1,
            "restore native menu state without a second clear");
        Check(*reinterpret_cast<ClearFn*>(app+0x138)==&NativeClear,"native clear callback restored before menu draws");
        for(int eye=0;eye<2;++eye) {
            unsigned char colour[4]{};float depth=0;
            glReadPixels(eye*8+4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,colour);
            glReadPixels(eye*8+4,4,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depth);
            Check(colour[eye]>240 && colour[1-eye]<10 && colour[2]<10,"world colour survives under menu in both eyes");
            Check(depth>.999f,"both eyes have fresh menu depth despite scene scissor and write mask");
        }
        GLboolean mask=GL_TRUE;glGetBooleanv(GL_DEPTH_WRITEMASK,&mask);
        GLint box[4]{};glGetIntegerv(GL_SCISSOR_BOX,box);
        Check(glIsEnabled(GL_SCISSOR_TEST) && !mask && box[0]==8 && box[2]==8,"native GL state restored");
        Check(glGetError()==GL_NO_ERROR,"render pass has no GL errors");
    }
    for(bool scissor:{false,true}) for(bool mask:{false,true}) {
        if(scissor) glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
        glDepthMask(mask?GL_TRUE:GL_FALSE);tr::collection::ClearMenuDepth();
        GLboolean actual=GL_FALSE;glGetBooleanv(GL_DEPTH_WRITEMASK,&actual);
        Check(bool(glIsEnabled(GL_SCISSOR_TEST))==scissor && bool(actual)==mask,"all incoming depth/scissor states restored");
    }
    for(int guard=0;guard<6;++guard) {
        active=guard!=0;rooms=guard==1?nullptr:roomMemory;
        roomCount=guard==2?0:guard==3?1025:2;
        cameraReady=guard!=5;
        *reinterpret_cast<int16_t*>(camera+12)=guard==4?-1:1;
        tr::collection::BufferBackground(0);
        Check(nativeCalls==guard+1 && roomCalls==3,"ordinary menus or missing level fall back without drawing rooms");
    }
    active=true;cameraReady=true;rooms=roomMemory;roomCount=2;*reinterpret_cast<int16_t*>(camera+12)=1;
    recurse=true;tr::collection::BufferBackground(0);
    Check(roomCalls==4 && nativeCalls==7 && !tr::InventorySceneDrawing(),"nested invocation uses original and restores drawing scope");
    ++moduleBase;Check(!tr::InventorySceneReady(),"old module binding cannot be used for camera preservation");--moduleBase;
    TestFocusAndFlare(build);
    tr::CollectionShutdown();
    Check(!tr::InventorySceneReady() && !tr::collection::base && !tr::collection::layout,"shutdown removes hook and scene binding");
    wglMakeCurrent(nullptr,nullptr);wglDeleteContext(context);ReleaseDC(window,dc);DestroyWindow(window);
    std::printf("Collection hooks: %d checks passed (real OpenGL colour/depth, repeated frames, fallback and lifecycle).\n",checks);
}
