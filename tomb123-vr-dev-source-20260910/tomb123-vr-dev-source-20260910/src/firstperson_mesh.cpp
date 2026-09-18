#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <gl/GL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>
#include "firstperson_mesh.h"
extern void log(const char*,...);
namespace firstpersonmesh {
bool active=false;
namespace {
using Draw=void(APIENTRY*)(GLenum,GLsizei,GLenum,const void*);
using Bind=void(APIENTRY*)(GLenum,GLuint);
using Gen=void(APIENTRY*)(GLsizei,GLuint*);
using Del=void(APIENTRY*)(GLsizei,const GLuint*);
using Data=void(APIENTRY*)(GLenum,ptrdiff_t,const void*,GLenum);
using Read=void(APIENTRY*)(GLenum,ptrdiff_t,ptrdiff_t,void*);
using Param=void(APIENTRY*)(GLenum,GLenum,GLint*);
using Attrib=void(APIENTRY*)(GLuint,GLenum,GLint*);
using Pointer=void(APIENTRY*)(GLuint,GLenum,void**);
using Location=GLint(APIENTRY*)(GLuint,const char*);
Draw original;
Bind bind;Gen gen;Del del;Data data;Read read;Param param;Attrib attrib;Pointer pointer;Location location;
struct Attribute {GLint buffer=0,type=0,size=0,stride=0,normalized=0;uintptr_t offset=0;};
using Key=std::tuple<GLuint,GLint,GLint,uintptr_t,GLsizei,GLenum,GLuint>;
struct Entry {GLuint buffer=0;GLsizei count=0;};
std::map<Key,Entry> cache;
int bytes(GLenum type){switch(type){case GL_UNSIGNED_BYTE:return 1;case GL_UNSIGNED_SHORT:return 2;case GL_FLOAT:case GL_UNSIGNED_INT:return 4;default:return 0;}}
bool describe(GLuint program,const char* name,Attribute& a){
 GLint index=location(program,name),enabled=0;if(index<0)return false;
 attrib(index,0x8622,&enabled);attrib(index,0x889f,&a.buffer);
 attrib(index,0x8623,&a.size);attrib(index,0x8624,&a.stride);attrib(index,0x8625,&a.type);attrib(index,0x886a,&a.normalized);
 void* p=nullptr;pointer(index,0x8645,&p);a.offset=(uintptr_t)p;
 if(!a.stride)a.stride=a.size*bytes(a.type);
 return enabled&&a.buffer&&a.size>=3&&bytes(a.type)&&a.stride>0;
}
float component(const unsigned char* p,GLenum type,bool norm){
 if(type==GL_FLOAT){float f;memcpy(&f,p,4);return f;}
 uint32_t v=0;memcpy(&v,p,bytes(type));
 if(norm){if(type==GL_UNSIGNED_BYTE)return v/255.f;if(type==GL_UNSIGNED_SHORT)return v/65535.f;return float(v)/4294967295.f;}
 return float(v);
}
bool loadAttribute(const Attribute& a,uint32_t maxIndex,std::vector<unsigned char>& out){
 bind(0x8892,a.buffer);GLint size=0;param(0x8892,0x8764,&size);
 uint64_t end=a.offset+uint64_t(maxIndex)*a.stride+3*bytes(a.type);
 if(end>uint64_t(std::max(0,size))||end-a.offset>64*1024*1024)return false;
 out.resize(size_t(end-a.offset));read(0x8892,a.offset,out.size(),out.data());return glGetError()==GL_NO_ERROR;
}
void APIENTRY draw(GLenum mode,GLsizei count,GLenum type,const void* indices){
 if(!active||mode!=GL_TRIANGLES||count<=0||count%3||count>3000000||(type!=GL_UNSIGNED_SHORT&&type!=GL_UNSIGNED_INT)){
  original(mode,count,type,indices);return;
 }
 if(!bind){
  bind=(Bind)wglGetProcAddress("glBindBuffer");gen=(Gen)wglGetProcAddress("glGenBuffers");del=(Del)wglGetProcAddress("glDeleteBuffers");
  data=(Data)wglGetProcAddress("glBufferData");read=(Read)wglGetProcAddress("glGetBufferSubData");param=(Param)wglGetProcAddress("glGetBufferParameteriv");
  attrib=(Attrib)wglGetProcAddress("glGetVertexAttribiv");pointer=(Pointer)wglGetProcAddress("glGetVertexAttribPointerv");location=(Location)wglGetProcAddress("glGetAttribLocation");
 }
 if(!bind||!gen||!del||!data||!read||!param||!attrib||!pointer||!location){original(mode,count,type,indices);return;}
 GLint program=0,ebo=0,array=0;glGetIntegerv(0x8b8d,&program);glGetIntegerv(0x8895,&ebo);glGetIntegerv(0x8894,&array);
 Attribute joints,weights;
 if(!ebo||!describe(program,"aLight",joints)||!describe(program,"aColor",weights)){original(mode,count,type,indices);return;}
 // Programs/VAOs identify attribute layouts; every first-person entry and cinema
 // transition clears this cache so reloaded/outfit meshes cannot reuse stale data.
 GLint vao=0;glGetIntegerv(0x85b5,&vao);
 Key key{GLuint(ebo),joints.buffer,vao,(uintptr_t)indices,count,type,GLuint(program)};
 auto found=cache.find(key);
 if(found==cache.end()){
  Entry entry;GLint size=0;param(0x8893,0x8764,&size);size_t indexBytes=size_t(count)*bytes(type);
  bool valid=size>=0&&(uintptr_t)indices<=uintptr_t(size)&&indexBytes<=size_t(size)-(uintptr_t)indices;
  std::vector<unsigned char> raw(indexBytes),j,w;
  uint32_t maxIndex=0;
  if(valid){read(0x8893,(ptrdiff_t)indices,indexBytes,raw.data());valid=glGetError()==GL_NO_ERROR;}
  auto indexAt=[&](int i){uint32_t v=0;memcpy(&v,raw.data()+size_t(i)*bytes(type),bytes(type));return v;};
  if(valid){for(int i=0;i<count;i++)maxIndex=std::max(maxIndex,indexAt(i));valid=loadAttribute(joints,maxIndex,j)&&loadAttribute(weights,maxIndex,w);}
  std::vector<uint32_t> kept;
  if(valid){
   auto head=[&](uint32_t vertex){float headWeight=0;
    for(int c=0;c<3;c++){
     float bone=component(j.data()+size_t(vertex)*joints.stride+c*bytes(joints.type),joints.type,joints.normalized!=0);
     float weight=component(w.data()+size_t(vertex)*weights.stride+c*bytes(weights.type),weights.type,weights.normalized!=0);
     if(std::isfinite(bone)&&std::abs(bone-14.f)<.01f)headWeight+=weight;
    }return headWeight>.05f;
   };
   kept.reserve(count);
   for(int i=0;i<count;i+=3){uint32_t a=indexAt(i),b=indexAt(i+1),c=indexAt(i+2);if(!head(a)&&!head(b)&&!head(c)){kept.push_back(a);kept.push_back(b);kept.push_back(c);}}
   gen(1,&entry.buffer);bind(0x8893,entry.buffer);data(0x8893,kept.size()*sizeof(uint32_t),kept.data(),0x88e4);
   valid=glGetError()==GL_NO_ERROR;entry.count=GLsizei(kept.size());
   log("FirstPerson mesh ebo=%d joints=%d type=%x normalized=%d weightsType=%x normalized=%d triangles=%d kept=%d removed=%d GLvalid=%d",
       ebo,joints.buffer,joints.type,joints.normalized,weights.type,weights.normalized,count/3,entry.count/3,(count-entry.count)/3,valid);
  }
  bind(0x8892,array);bind(0x8893,ebo);
  if(!valid){if(entry.buffer)del(1,&entry.buffer);static int failures=0;if(failures++<4)log("FirstPerson mesh filter rejected unsupported buffer layout");original(mode,count,type,indices);return;}
  found=cache.emplace(key,entry).first;
 }
 bind(0x8893,found->second.buffer);
 original(mode,found->second.count,GL_UNSIGNED_INT,nullptr);
 bind(0x8893,ebo);
}
}
void clear(){if(del)for(auto& e:cache)if(e.second.buffer)del(1,&e.second.buffer);cache.clear();}
bool install(){
 auto host=(unsigned char*)GetModuleHandleW(nullptr);auto slot=(Draw*)(host+0x66438);
 if(*slot!=(Draw)glDrawElements){log("FirstPerson glDrawElements import mismatch");return false;}
 DWORD old=0;if(!VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&old))return false;
 original=*slot;InterlockedExchangePointer((void**)slot,(void*)draw);DWORD ignored;VirtualProtect(slot,sizeof(*slot),old,&ignored);
 log("FirstPerson indexed mesh filter installed");return true;
}
}
