#include "WorldLaser.h"
#include "WorldLaserNative.h"
#include "HKScopeMath.h"
#include "GameDll.h"
#include "Config.h"
#include "GL.h"
#include "VRSystem.h"
#include "StereoRenderer.h"
#include "Log.h"
#include "FirstPerson.h"

namespace tr {
namespace worldlaser {
GLuint program=0,vao=0,vbo=0;
GLint projectionUniform=-1;
bool failed=false;
uint64_t checkedBase=0;
const Build* native=nullptr;

bool ReadNativeRay(Ray& ray) {
    const auto* dll=GameDllBound();const auto base=GameDllBase();
    if(!dll || !base || dll->game!=CurrentGame() || CurrentGame()<0 || CurrentGame()>1) return false;
    if(checkedBase!=base || (native && native->stamp!=dll->timestamp)) {
        checkedBase=base;native=nullptr;
        for(const auto& b:builds) if(b.stamp==dll->timestamp && Verify(reinterpret_cast<const uint8_t*>(base),b)) native=&b;
        Log(native ? "laser: verified native world-space hit output" : "laser: unsupported native output; beam disabled");
    }
    if(!native) return false;
    if(IsOpticsZoomed()) {
        // HK native scope output is for aiming, not a projected laser beam.
        // Leave the scope itself and its collision/firing state untouched.
        if(dll->game==1 && *reinterpret_cast<const int16_t*>(base+dll->lara+4)==5) return false;
        return Read(reinterpret_cast<const uint8_t*>(base),*native,ray);
    }
    motiongun::Vec muzzle{},direction{};int16_t room=0;
    if(!dll->getFloor || !WeaponLaserAim(muzzle,direction,room)) return false;
    using Floor=void* (__cdecl*)(int32_t,int32_t,int32_t,int16_t*);
    using LOS=int32_t (__cdecl*)(Point*,Point*);
    using Objects=int32_t (__cdecl*)(Point*,Point*,Point*,void**);
    return Trace(muzzle,direction,room,ray,reinterpret_cast<Floor>(base+dll->getFloor),
        reinterpret_cast<LOS>(base+native->los),reinterpret_cast<Objects>(base+native->objects));
}
void Delete() {
    if(vbo) gl::DeleteBuffers(1,&vbo);
    if(vao) gl::DeleteVertexArrays(1,&vao);
    if(program) gl::DeleteProgram(program);
    vbo=vao=program=0;
}
GLuint Shader(GLenum kind,const char* source) {
    const auto s=gl::CreateShader(kind);gl::ShaderSource(s,1,&source,nullptr);gl::CompileShader(s);
    GLint ok=0;gl::GetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok) { char info[1024]{};gl::GetShaderInfoLog(s,sizeof(info)-1,nullptr,info);LogF("laser: shader failed: %s",info);gl::DeleteShader(s);return 0; }
    return s;
}
struct Vertex { float x,y,z,u,v,dot; };
bool Create() {
    if(program) return true;
    if(failed || !gl::LoadedShaderApi()) return false;
    const char* vs="#version 150\nin vec3 point;in vec3 shape;out vec3 mark;uniform mat4 projection;"
        "void main(){mark=shape;gl_Position=projection*vec4(point,1);}";
    const char* fs="#version 150\nin vec3 mark;out vec4 color;void main(){"
        "if(mark.z>0.5 && dot(mark.xy,mark.xy)>1.0)discard;"
        "color=mark.z>0.5?vec4(1,.09,.04,1):vec4(.9,.015,.005,1);}";
    const auto vertex=Shader(GL_VERTEX_SHADER,vs),fragment=Shader(GL_FRAGMENT_SHADER,fs);
    GLint ok=0;
    if(vertex && fragment) {
        program=gl::CreateProgram();gl::AttachShader(program,vertex);gl::AttachShader(program,fragment);gl::LinkProgram(program);
        gl::GetProgramiv(program,GL_LINK_STATUS,&ok);
    }
    if(vertex) gl::DeleteShader(vertex);if(fragment) gl::DeleteShader(fragment);
    if(ok) {
        const GLint point=gl::GetAttribLocation(program,"point"),shape=gl::GetAttribLocation(program,"shape");
        projectionUniform=gl::GetUniformLocation(program,"projection");
        ok=point>=0 && shape>=0 && projectionUniform>=0;
        if(ok) {
            gl::GenVertexArrays(1,&vao);gl::BindVertexArray(vao);gl::GenBuffers(1,&vbo);gl::BindBuffer(GL_ARRAY_BUFFER,vbo);
            gl::BufferData(GL_ARRAY_BUFFER,12*sizeof(Vertex),nullptr,GL_STATIC_DRAW);
            gl::EnableVertexAttribArray(point);gl::VertexAttribPointer(point,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),nullptr);
            gl::EnableVertexAttribArray(shape);gl::VertexAttribPointer(shape,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(3*sizeof(float)));
        }
    }
    if(!ok) { Delete();failed=true;Log("laser: renderer unavailable"); }
    else Log("laser: stereo world-space beam ready");
    return ok!=0;
}
motiongun::Vec TransformPoint(const Affine& view,motiongun::Vec p) {
    return {view.r[0][0]*p.x+view.r[0][1]*p.y+view.r[0][2]*p.z+view.r[0][3],
            view.r[1][0]*p.x+view.r[1][1]*p.y+view.r[1][2]*p.z+view.r[1][3],
            view.r[2][0]*p.x+view.r[2][1]*p.y+view.r[2][2]*p.z+view.r[2][3]};
}
int Mesh(const Ray& ray,const Affine& view,float scale,float nearPlane,Vertex (&out)[12]) {
    using namespace motiongun;
    Vec a=TransformPoint(view,ray.start),b=TransformPoint(view,ray.end);
    if(b.z>=-nearPlane) return 0;
    if(a.z>-nearPlane) a=Add(a,Scale(Sub(b,a),(-nearPlane-a.z)/(b.z-a.z)));
    const auto direction=Sub(b,a);
    Vec side=Cross(direction,a);float sideLength=std::sqrt(Dot(side,side));
    side=sideLength>1e-5f ? Scale(side,1/sideLength) : Vec{1,0,0};
    // A thin physical beam, with a subpixel minimum at long distances.
    const auto sa=Scale(side,std::max(scale*.001f,-a.z*.0005f));
    const auto sb=Scale(side,std::max(scale*.001f,-b.z*.0005f));
    const Vec corners[]={Sub(a,sa),Add(a,sa),Sub(b,sb),Add(b,sb)};
    const int indices[]={0,1,2,2,1,3};
    for(int i=0;i<6;++i) { const auto p=corners[indices[i]];out[i]={p.x,p.y,p.z,0,0,0}; }
    // Bias only the small endpoint marker toward the viewer to avoid z fighting.
    const float radius=std::max(scale*.004f,-b.z*.0015f);
    const Vec dot=Scale(b,1-std::min(scale*.001f/-b.z,.01f));
    const float uv[4][2]={{-1,-1},{1,-1},{-1,1},{1,1}};
    for(int i=0;i<6;++i) {
        const auto* p=uv[indices[i]];out[6+i]={dot.x+p[0]*radius,dot.y+p[1]*radius,dot.z,p[0],p[1],1};
    }
    return 12;
}
struct State {
    GLint read,draw,viewport[4],scissor[4],program,vao,buffer,depthFunc;
    GLboolean scissorOn,depth,blend,cull,depthWrite,mask[4];GLdouble range[2];
    State() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
        glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_SCISSOR_BOX,scissor);
        glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&buffer);
        glGetIntegerv(GL_DEPTH_FUNC,&depthFunc);glGetBooleanv(GL_DEPTH_WRITEMASK,&depthWrite);glGetBooleanv(GL_COLOR_WRITEMASK,mask);glGetDoublev(GL_DEPTH_RANGE,range);
        scissorOn=glIsEnabled(GL_SCISSOR_TEST);depth=glIsEnabled(GL_DEPTH_TEST);blend=glIsEnabled(GL_BLEND);cull=glIsEnabled(GL_CULL_FACE);
    }
    ~State() {
        gl::BindVertexArray(vao);gl::BindBuffer(GL_ARRAY_BUFFER,buffer);gl::UseProgram(program);
        gl::BindFramebuffer(GL_READ_FRAMEBUFFER,read);gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glScissor(scissor[0],scissor[1],scissor[2],scissor[3]);
        glDepthFunc(depthFunc);glDepthMask(depthWrite);glDepthRange(range[0],range[1]);glColorMask(mask[0],mask[1],mask[2],mask[3]);
        Set(GL_SCISSOR_TEST,scissorOn);Set(GL_DEPTH_TEST,depth);Set(GL_BLEND,blend);Set(GL_CULL_FACE,cull);
    }
    static void Set(GLenum key,bool value) { if(value) glEnable(key);else glDisable(key); }
};
void Draw(const Ray& ray) {
    float rot[3][3]{},pos[3]{};
    if(!CameraViewFrame(rot,pos)) return;
    const State saved;
    if(!Create()) return;
    float zn=16,zf=32768;ExtractNearFar(Proj()[1],zn,zf);
    if(Cfg().nearClip>0) zn=Cfg().nearClip;if(Cfg().farClip>0) zf=Cfg().farClip;
    const auto world=hkscope::WorldView(rot,pos);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER,Stereo().fbo());
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);glDepthRange(0,1);
    glEnable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    gl::UseProgram(program);gl::BindVertexArray(vao);gl::BindBuffer(GL_ARRAY_BUFFER,vbo);
    for(int eye=0;eye<2;++eye) {
        const auto view=Mul(VR().EyeView(Eye(eye)),world);
        Vertex vertices[12]{};const int count=Mesh(ray,view,LiveWorldUnitsPerMetre(),zn,vertices);
        if(!count) continue;
        mat4 projection{};VR().EyeProjection(Eye(eye),zn,zf,projection);
        Stereo().SetEyeViewport(eye);gl::UniformMatrix4fv(projectionUniform,1,GL_FALSE,projection.m);
        gl::BufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);glDrawArrays(GL_TRIANGLES,0,count);
    }
}
}
void WorldLaserPresent() {
    if(!Cfg().enabled || !Cfg().worldLaserSight || Cfg().monoTracking || !Cfg().duplicateDraws ||
       Cfg().eyeOffsetMode!=3 || !Cfg().perEyeView || Cfg().perEyeProjection!=1 ||
       !VR().active() || !VR().poseValid() || !Stereo().valid() || InTitle() || InInventory() || InFMV() ||
       AppState(drva::app_off::photoMode)) return;
    worldlaser::Ray ray{};
    if(worldlaser::ReadNativeRay(ray)) worldlaser::Draw(ray);
}
void WorldLaserShutdown() {
    if(wglGetCurrentContext() && gl::Loaded()) worldlaser::Delete();
    worldlaser::failed=false;worldlaser::checkedBase=0;worldlaser::native=nullptr;
}
}
