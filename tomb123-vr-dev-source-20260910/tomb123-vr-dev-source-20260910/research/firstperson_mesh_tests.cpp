#include <cstddef>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include "../src/firstperson_mesh.cpp"
void log(const char* f,...){va_list a;va_start(a,f);vprintf(f,a);puts("");va_end(a);}
static int expectedCount=3,calls=0;
void APIENTRY captureDraw(GLenum mode,GLsizei count,GLenum type,const void* indices){
 assert(mode==GL_TRIANGLES&&count==expectedCount);calls++;
 if(count==3){assert(type==GL_UNSIGNED_INT&&indices==nullptr);uint32_t result[3]{};firstpersonmesh::read(0x8893,0,sizeof(result),result);assert(result[0]==0&&result[1]==1&&result[2]==2);}
}
int main(){
 WNDCLASSA wc{};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="HeadMeshTest";wc.style=CS_OWNDC;assert(RegisterClassA(&wc));
 HWND win=CreateWindowA(wc.lpszClassName,"",WS_POPUP,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);HDC dc=GetDC(win);
 PIXELFORMATDESCRIPTOR pf{};pf.nSize=sizeof(pf);pf.nVersion=1;pf.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL;pf.iPixelType=PFD_TYPE_RGBA;pf.cColorBits=32;
 assert(SetPixelFormat(dc,ChoosePixelFormat(dc,&pf),&pf));HGLRC ctx=wglCreateContext(dc);assert(wglMakeCurrent(dc,ctx));
 using Create=GLuint(APIENTRY*)(GLenum);using Source=void(APIENTRY*)(GLuint,GLsizei,const char*const*,const GLint*);using One=void(APIENTRY*)(GLuint);using Get=void(APIENTRY*)(GLuint,GLenum,GLint*);
 auto create=(Create)wglGetProcAddress("glCreateShader");auto source=(Source)wglGetProcAddress("glShaderSource");auto compile=(One)wglGetProcAddress("glCompileShader");auto get=(Get)wglGetProcAddress("glGetShaderiv");
 const char* vertex="#version 150\nin vec3 aLight;in vec3 aColor;void main(){gl_Position=vec4(aLight+aColor,1);}";
 const char* fragment="#version 150\nout vec4 c;void main(){c=vec4(1);}";
 GLuint shaders[2]{create(0x8b31),create(0x8b30)};const char* sources[]{vertex,fragment};
 for(int i=0;i<2;i++){source(shaders[i],1,&sources[i],nullptr);compile(shaders[i]);GLint status=0;get(shaders[i],0x8b81,&status);assert(status);}
 auto createProgram=(GLuint(APIENTRY*)())wglGetProcAddress("glCreateProgram");auto attachShader=(void(APIENTRY*)(GLuint,GLuint))wglGetProcAddress("glAttachShader");auto link=(One)wglGetProcAddress("glLinkProgram");auto use=(One)wglGetProcAddress("glUseProgram");auto getProgram=(Get)wglGetProcAddress("glGetProgramiv");
 GLuint program=createProgram();for(auto shader:shaders)attachShader(program,shader);link(program);GLint linked=0;getProgram(program,0x8b82,&linked);assert(linked);use(program);
 auto gen=(firstpersonmesh::Gen)wglGetProcAddress("glGenBuffers");auto bind=(firstpersonmesh::Bind)wglGetProcAddress("glBindBuffer");auto data=(firstpersonmesh::Data)wglGetProcAddress("glBufferData");
 auto genVAO=(firstpersonmesh::Gen)wglGetProcAddress("glGenVertexArrays");auto bindVAO=(One)wglGetProcAddress("glBindVertexArray");auto loc=(firstpersonmesh::Location)wglGetProcAddress("glGetAttribLocation");
 auto enable=(One)wglGetProcAddress("glEnableVertexAttribArray");auto set=(void(APIENTRY*)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*))wglGetProcAddress("glVertexAttribPointer");
 GLuint vao,buffers[3];genVAO(1,&vao);bindVAO(vao);gen(3,buffers);
 float vertices[6][6]{};for(int i=0;i<6;i++){vertices[i][0]=i<3?7.f:14.f;vertices[i][3]=1.f;}
 bind(0x8892,buffers[0]);data(0x8892,sizeof(vertices),vertices,0x88e4);
 for(int i=0;i<2;i++){GLint attr=loc(program,i?"aColor":"aLight");assert(attr>=0);enable(attr);set(attr,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(uintptr_t(i*3*sizeof(float))));}
 uint16_t indices[]={0,1,2,3,4,5};bind(0x8893,buffers[1]);data(0x8893,sizeof(indices),indices,0x88e4);bind(0x8892,buffers[2]);
 firstpersonmesh::original=captureDraw;firstpersonmesh::active=true;
 firstpersonmesh::draw(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,nullptr);
 GLint v;glGetIntegerv(0x8895,&v);assert(v==(GLint)buffers[1]);glGetIntegerv(0x8894,&v);assert(v==(GLint)buffers[2]);glGetIntegerv(0x85b5,&v);assert(v==(GLint)vao);
 firstpersonmesh::draw(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,nullptr);assert(calls==2);
 firstpersonmesh::active=false;expectedCount=6;firstpersonmesh::draw(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,nullptr);assert(calls==3);
 firstpersonmesh::clear();assert(glGetError()==GL_NO_ERROR);
 wglMakeCurrent(nullptr,nullptr);wglDeleteContext(ctx);ReleaseDC(win,dc);DestroyWindow(win);
 puts("PASS: head triangles removed, body indices retained, cache reused, GL bindings restored, other camera modes pass through.");
}
