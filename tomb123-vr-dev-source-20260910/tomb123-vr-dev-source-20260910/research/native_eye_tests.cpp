#define WIN32_LEAN_AND_MEAN
#include "../src/native_eye.h"
#include <cassert>
#include <cstdio>
#include <cstdarg>
void log(const char* f,...){va_list a;va_start(a,f);vprintf(f,a);puts("");va_end(a);}
int main(){
 WNDCLASSA wc{};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="NativeEyeTest";wc.style=CS_OWNDC;assert(RegisterClassA(&wc));
 HWND wnd=CreateWindowA(wc.lpszClassName,"",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);HDC dc=GetDC(wnd);
 PIXELFORMATDESCRIPTOR pf{};pf.nSize=sizeof(pf);pf.nVersion=1;pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;pf.iPixelType=PFD_TYPE_RGBA;pf.cColorBits=32;
 assert(SetPixelFormat(dc,ChoosePixelFormat(dc,&pf),&pf));HGLRC ctx=wglCreateContext(dc);assert(wglMakeCurrent(dc,ctx));
 GLuint tex;glGenTextures(1,&tex);int ww=64,wh=64,tw=0,th=0;GLuint fb=0;EyeHostState host{&ww,&wh,&tw,&th,&fb};NativeEye eye;
 glViewport(1,2,31,32);glScissor(3,4,21,22);
 for(int pass=0;pass<4;pass++){
  int w=pass==1?513:257,h=pass==1?389:193;
  if(pass==3){glBindTexture(GL_TEXTURE_2D,tex);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);}
  assert(eye.begin(host,tex,w,h));assert(ww==w&&wh==h&&tw==w&&th==h&&fb!=0);
  glDisable(GL_SCISSOR_TEST);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);glScissor(w-8,h-8,8,8);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  unsigned char pixel[4]{};glReadPixels(w-2,h-2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==0);
  eye.end();assert(ww==64&&wh==64&&tw==0&&th==0&&fb==0);
  GLint v[4];glGetIntegerv(GL_VIEWPORT,v);assert(v[0]==1&&v[1]==2&&v[2]==31&&v[3]==32);glGetIntegerv(GL_SCISSOR_BOX,v);assert(v[0]==3&&v[1]==4&&v[2]==21&&v[3]==22);
  glGetIntegerv(0x8ca6,v);assert(v[0]==0);glGetIntegerv(0x8caa,v);assert(v[0]==0);assert(glGetError()==GL_NO_ERROR);
 }
 assert(!eye.begin(host,tex,0,10));assert(!eye.active);puts("PASS: true rendering beyond window bounds, resize, overwritten texture recovery, host and GL state restoration.");
 wglMakeCurrent(nullptr,nullptr);wglDeleteContext(ctx);ReleaseDC(wnd,dc);DestroyWindow(wnd);
}
