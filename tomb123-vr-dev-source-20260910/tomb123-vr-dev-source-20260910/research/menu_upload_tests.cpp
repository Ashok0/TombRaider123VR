#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <gl/GL.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include "../src/vr_menu.h"
void log(const char* fmt, ...) {va_list a;va_start(a,fmt);vprintf(fmt,a);puts("");va_end(a);}
int main(){
 WNDCLASSA wc{};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="MenuUploadTest";wc.style=CS_OWNDC;
 assert(RegisterClassA(&wc));
 HWND window=CreateWindowA(wc.lpszClassName,"",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
 HDC dc=GetDC(window);PIXELFORMATDESCRIPTOR pf{};pf.nSize=sizeof(pf);pf.nVersion=1;pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;pf.iPixelType=PFD_TYPE_RGBA;pf.cColorBits=32;
 assert(SetPixelFormat(dc,ChoosePixelFormat(dc,&pf),&pf));HGLRC ctx=wglCreateContext(dc);assert(wglMakeCurrent(dc,ctx));
 using Gen=void(APIENTRY*)(GLsizei,GLuint*);using Bind=void(APIENTRY*)(GLenum,GLuint);
 auto gen=(Gen)wglGetProcAddress("glGenBuffers");auto bind=(Bind)wglGetProcAddress("glBindBuffer");assert(gen&&bind);
 GLuint buffers[2]{},originalTexture=0;gen(2,buffers);glGenTextures(1,&originalTexture);glBindTexture(GL_TEXTURE_2D,originalTexture);
 bind(0x88ec,buffers[0]);bind(0x88eb,buffers[1]);
 const GLenum params[]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS,GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,GL_PACK_SKIP_PIXELS};
 const GLint values[]={8,777,3,5,8,999,7,9};for(int i=0;i<8;i++)glPixelStorei(params[i],values[i]);
 int setting=1;MenuItem item{"Test setting",&setting,nullptr,1,0,10,nullptr,nullptr,nullptr};VRMenu menu;menu.bind(&item,1);
 assert(menu.render());assert(!menu.dirty);assert(glGetError()==GL_NO_ERROR);
 for(int i=0;i<8;i++){GLint v;glGetIntegerv(params[i],&v);assert(v==values[i]);}
 GLint v;glGetIntegerv(GL_TEXTURE_BINDING_2D,&v);assert(v==(GLint)originalTexture);
 glGetIntegerv(0x88ef,&v);assert(v==(GLint)buffers[0]);glGetIntegerv(0x88ed,&v);assert(v==(GLint)buffers[1]);
 menu.adjust(1);assert(menu.render());assert(setting==2);assert(glGetError()==GL_NO_ERROR);
 wglMakeCurrent(nullptr,nullptr);wglDeleteContext(ctx);ReleaseDC(window,dc);DestroyWindow(window);
 puts("PASS: menu pixels survive nondefault pack/unpack state and bound PBOs; state restored and changed values reupload.");
}
