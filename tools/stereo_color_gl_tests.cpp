// Real-driver regression for the production eye targets; no headset required.
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <initializer_list>
#include "../src/StereoRenderer.h"
#include "../src/Config.h"
namespace tr {
Config config;
const Config& Cfg() { return config; }
}
void Log(const char* s) { std::puts(s); }
void LogF(const char* f,...) { va_list a;va_start(a,f);std::vprintf(f,a);va_end(a);std::puts(""); }
int checks=0;
void Check(bool ok,const char* why) { ++checks;if(!ok) {std::printf("FAIL: %s\n",why);std::exit(1);} }
GLuint Target(GLint format) {
    GLuint texture=0,fbo=0;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,format,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    gl::GenFramebuffers(1,&fbo);gl::BindFramebuffer(GL_FRAMEBUFFER,fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    Check(gl::CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"reference framebuffer complete");
    return fbo;
}
void Pixel(GLuint fbo,int x,unsigned char out[4]) {
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER,fbo);glReadPixels(x,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,out);
}
void Same(const unsigned char* a,const unsigned char* b,const char* why) {
    for(int i=0;i<4;++i) {
        if(std::abs(int(a[i])-int(b[i]))>1) std::printf("channel %d: actual %d expected %d\n",i,int(a[i]),int(b[i]));
        Check(std::abs(int(a[i])-int(b[i]))<=1,why);
    }
}
int main() {
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"StereoColorTest";wc.style=CS_OWNDC;
    Check(RegisterClassW(&wc)!=0,"register hidden window");
    HWND window=CreateWindowW(wc.lpszClassName,L"Color test",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    HDC dc=GetDC(window);PIXELFORMATDESCRIPTOR pf{};pf.nSize=sizeof(pf);pf.nVersion=1;
    pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;pf.iPixelType=PFD_TYPE_RGBA;pf.cColorBits=32;pf.cDepthBits=24;
    Check(SetPixelFormat(dc,ChoosePixelFormat(dc,&pf),&pf)!=0,"set pixel format");
    HGLRC legacy=wglCreateContext(dc);Check(legacy && wglMakeCurrent(dc,legacy),"legacy GL context");
    using Create=HGLRC(WINAPI*)(HDC,HGLRC,const int*);
    auto create=reinterpret_cast<Create>(wglGetProcAddress("wglCreateContextAttribsARB"));Check(create!=nullptr,"core GL available");
    const int attributes[]={0x2091,3,0x2092,2,0x9126,1,0};HGLRC core=create(dc,nullptr,attributes);
    wglMakeCurrent(nullptr,nullptr);wglDeleteContext(legacy);Check(core && wglMakeCurrent(dc,core),"core GL context");
    Check(gl::Load(),"load GL API");std::printf("GL: %s\n",glGetString(GL_VERSION));
    const char* vs="#version 150\nvoid main(){vec2 p=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);gl_Position=vec4(p,0,1);}";
    const char* fs="#version 150\nuniform vec4 sampleColor;out vec4 color;void main(){color=sampleColor;}";
    auto shader=[](GLenum type,const char* source) { GLuint id=gl::CreateShader(type);gl::ShaderSource(id,1,&source,nullptr);gl::CompileShader(id);GLint ok=0;gl::GetShaderiv(id,GL_COMPILE_STATUS,&ok);Check(ok!=0,"compile color shader");return id; };
    GLuint program=gl::CreateProgram();gl::AttachShader(program,shader(GL_VERTEX_SHADER,vs));gl::AttachShader(program,shader(GL_FRAGMENT_SHADER,fs));gl::LinkProgram(program);
    GLint linked=0;gl::GetProgramiv(program,GL_LINK_STATUS,&linked);Check(linked!=0,"link color shader");
    gl::UseProgram(program);GLuint vao=0;gl::GenVertexArrays(1,&vao);gl::BindVertexArray(vao);
    const GLint sample=gl::GetUniformLocation(program,"sampleColor");
    auto draw=[&](GLuint fbo,float grey,float alpha=1.f) {
        gl::BindFramebuffer(GL_FRAMEBUFFER,fbo);glViewport(0,0,32,16);glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);
        const float value[]={grey,grey,grey,alpha};gl::Uniform4fv(sample,1,value);glDrawArrays(GL_TRIANGLES,0,3);
    };
    const GLuint reference=Target(GL_SRGB8_ALPHA8),linear=Target(GL_RGBA8),mirror=Target(GL_SRGB8_ALPHA8),mirrorReference=Target(GL_SRGB8_ALPHA8);
    auto& stereo=tr::Stereo();Check(stereo.Create(16,16) && stereo.CreateMono(32,16),"create production stereo and scratch targets");
    const GLuint eyeTexture=stereo.texture(),eyeFbo=stereo.fbo(),monoFbo=stereo.monoFbo();
    Check(!tr::Cfg().vrBrighterColors,"Dark is the default");
    unsigned char expected[4]{},actual[4]{};
    for(bool brighter:{false,true,false,true}) {
    tr::config.vrBrighterColors=brighter;
    glBindTexture(GL_TEXTURE_2D,eyeTexture);
    stereo.BeginFrame();
    GLint binding=0,format=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
    Check(binding==GLint(eyeTexture),"format change preserves the engine texture binding");
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&format);
    Check(format==(brighter ? GL_SRGB8_ALPHA8 : GL_RGBA8),"menu selection changes actual eye texture storage");
    Check(stereo.texture()==eyeTexture && stereo.fbo()==eyeFbo && stereo.monoFbo()==monoFbo,"live color switches keep engine framebuffer references valid");
    for(bool corrected:{true,false}) for(float grey:{.01f,.04f,.18f,.5f,1.f}) {
        if(corrected) glEnable(GL_FRAMEBUFFER_SRGB);else glDisable(GL_FRAMEBUFFER_SRGB);
        const GLuint target=brighter ? reference : linear;
        draw(target,grey,.4f);Pixel(target,8,expected);
        draw(linear,grey,.4f);Pixel(linear,8,actual);
        if(corrected && grey==.18f) std::printf("18%% grey: %s=%d, original dark=%d\n",brighter ? "Brighter" : "Dark",expected[0],actual[0]);
        draw(stereo.fbo(),grey,.4f);
        for(int x:{8,24}) { Pixel(stereo.fbo(),x,actual);Same(actual,expected,"both VR eyes match native color conversion"); }
        draw(stereo.monoFbo(),grey,.4f);
        for(int eye:{0,1}) {
            stereo.BlitMonoToHalf(eye);Pixel(stereo.fbo(),eye*16+8,actual);
            Same(actual,expected,"scratch-to-eye blit retains encoded brightness");
        }
        // Compare the mirror with the same native copy from a reference target.
        // The old Dark path already encoded during its window blit, hiding the
        // missing headset correction; don't require the mirror to be dark too.
        gl::BindFramebuffer(GL_READ_FRAMEBUFFER,target);gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER,mirrorReference);
        gl::BlitFramebuffer(0,0,16,16,0,0,16,16,GL_COLOR_BUFFER_BIT,GL_LINEAR);
        unsigned char mirrorExpected[4]{};Pixel(mirrorReference,8,mirrorExpected);
        stereo.MirrorToWindow(16,16,mirror);Pixel(mirror,8,actual);
        Same(actual,mirrorExpected,"desktop mirror matches native reference copy");
        Check(bool(glIsEnabled(GL_FRAMEBUFFER_SRGB))==corrected,"copies preserve engine color-conversion state");
    }
    }
    glEnable(GL_FRAMEBUFFER_SRGB);
    for(GLuint fbo:{reference,stereo.fbo()}) {
        glDisable(GL_BLEND);draw(fbo,.04f);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw(fbo,.18f,.5f);glDisable(GL_BLEND);
    }
    Pixel(reference,8,expected);Pixel(stereo.fbo(),8,actual);Same(actual,expected,"transparency blends in the same color space as flat rendering");
    Check(glGetError()==GL_NO_ERROR,"no OpenGL errors");stereo.Destroy();
    std::printf("OK: %d stereo color checks passed.\n",checks);
    wglMakeCurrent(nullptr,nullptr);wglDeleteContext(core);ReleaseDC(window,dc);DestroyWindow(window);
}
