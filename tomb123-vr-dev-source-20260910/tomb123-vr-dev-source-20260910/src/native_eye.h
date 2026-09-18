#pragma once
#include <windows.h>
#include <gl/GL.h>
struct EyeHostState {
  int *windowWidth, *windowHeight, *targetWidth, *targetHeight;
  GLuint *defaultFramebuffer;
};
class NativeEye {
public:
  bool begin(EyeHostState host, GLuint color, int width, int height);
  void end();
  bool active=false;
  int width=0,height=0;
private:
  EyeHostState host{};
  int saved[4]{};
  GLuint savedDefault=0, framebuffer=0, depth=0;
  GLint read=0,draw=0,viewport[4]{},scissor[4]{};
};
