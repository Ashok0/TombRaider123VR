#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "native_eye.h"
extern void log(const char*,...);
namespace {
using Bind=void(APIENTRY*)(GLenum,GLuint);
using Gen=void(APIENTRY*)(GLsizei,GLuint*);
using Attach=void(APIENTRY*)(GLenum,GLenum,GLenum,GLuint,GLint);
using Check=GLenum(APIENTRY*)(GLenum);
Bind bind,bindBuffer;Gen gen;Attach attach;Check check;
}
bool NativeEye::begin(EyeHostState state, GLuint color, int w, int h) {
  if(active)return false;
  if(!bind){
    bind=(Bind)wglGetProcAddress("glBindFramebuffer");
    bindBuffer=(Bind)wglGetProcAddress("glBindBuffer");
    gen=(Gen)wglGetProcAddress("glGenFramebuffers");
    attach=(Attach)wglGetProcAddress("glFramebufferTexture2D");
    check=(Check)wglGetProcAddress("glCheckFramebufferStatus");
  }
  if(!bind||!bindBuffer||!gen||!attach||!check||!color||w<=0||h<=0)return false;
  GLint limit=0,limits[2]{};glGetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);glGetIntegerv(GL_MAX_VIEWPORT_DIMS,limits);
  if(w>limit||h>limit||w>limits[0]||h>limits[1]){
    log("Native eye size %dx%d exceeds GPU limits",w,h);return false;
  }
  glGetIntegerv(0x8caa,&read);glGetIntegerv(0x8ca6,&draw);
  glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_SCISSOR_BOX,scissor);
  if(!framebuffer)gen(1,&framebuffer);
  if(!depth)glGenTextures(1,&depth);
  GLint previousTexture=0,actualWidth=0,actualHeight=0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D,&previousTexture);
  glBindTexture(GL_TEXTURE_2D,color);
  glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&actualWidth);
  glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&actualHeight);
  glBindTexture(GL_TEXTURE_2D,previousTexture);
  if(width!=w||height!=h||actualWidth!=w||actualHeight!=h){
    GLint texture=0,pbo=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegerv(0x88ef,&pbo);
    bindBuffer(0x88ec,0);
    while(glGetError()!=GL_NO_ERROR){}
    glBindTexture(GL_TEXTURE_2D,color);
    glTexImage2D(GL_TEXTURE_2D,0,0x8c43,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D,depth);
    glTexImage2D(GL_TEXTURE_2D,0,0x88f0,w,h,0,0x84f9,0x84fa,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    GLenum error=glGetError();
    glBindTexture(GL_TEXTURE_2D,texture);bindBuffer(0x88ec,pbo);
    if(error){log("Native eye allocation failed %dx%d GL=%x",w,h,error);width=height=0;return false;}
    width=w;height=h;
    log("Native eye allocated color=%u framebuffer=%u %dx%d",color,framebuffer,w,h);
  }
  bind(0x8d40,framebuffer);
  attach(0x8d40,0x8ce0,GL_TEXTURE_2D,color,0);
  attach(0x8d40,0x821a,GL_TEXTURE_2D,depth,0);
  glReadBuffer(0x8ce0);glDrawBuffer(0x8ce0);
  if(check(0x8d40)!=0x8cd5){
    log("Native eye framebuffer incomplete");bind(0x8ca8,read);bind(0x8ca9,draw);return false;
  }
  host=state;
  saved[0]=*host.windowWidth;saved[1]=*host.windowHeight;
  saved[2]=*host.targetWidth;saved[3]=*host.targetHeight;savedDefault=*host.defaultFramebuffer;
  *host.windowWidth=*host.targetWidth=w;*host.windowHeight=*host.targetHeight=h;
  *host.defaultFramebuffer=framebuffer;
  glViewport(0,0,w,h);glScissor(0,0,w,h);
  active=true;return true;
}
void NativeEye::end(){
  if(!active)return;
  *host.windowWidth=saved[0];*host.windowHeight=saved[1];
  *host.targetWidth=saved[2];*host.targetHeight=saved[3];*host.defaultFramebuffer=savedDefault;
  bind(0x8ca8,read);bind(0x8ca9,draw);
  glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
  glScissor(scissor[0],scissor[1],scissor[2],scissor[3]);
  active=false;
}
