#include "HKScope.h"
#include "HKScopeMath.h"
#include "FirstPerson.h"
#include "GameDll.h"
#include "Config.h"
#include "GL.h"
#include "VRSystem.h"
#include "StereoRenderer.h"
#include "Log.h"

namespace tr {
namespace {
constexpr int kSize=512;
GLuint texture=0,depth=0,fbo=0,program=0,vao=0,vbo=0;
GLint uMvp=-1,uImage=-1,uVisible=-1;
bool checked=false,enabled=false,capturing=false,cleared=false,failed=false;
bool visible[2]{};
unsigned captured=0;
uint64_t lastReport=0;
uint64_t lastCompositeReport=0;
Affine delta{};
mat4 lensMvp[2]{};
float lensRadiusMetres=.03f;
float eyeRelief[2]{},eyeMiss[2]{};

struct TargetState {
    GLint read=0,draw=0,viewport[4]{},scissor[4]{};
    GLboolean scissorOn=false;
    void Save() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
        glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_SCISSOR_BOX,scissor);
        scissorOn=glIsEnabled(GL_SCISSOR_TEST);
    }
    void Restore() const {
        gl::BindFramebuffer(GL_READ_FRAMEBUFFER,read);gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        glScissor(scissor[0],scissor[1],scissor[2],scissor[3]);
        if (scissorOn) glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
    }
} target;

GLuint Compile(GLenum kind,const char* source) {
    GLuint s=gl::CreateShader(kind);gl::ShaderSource(s,1,&source,nullptr);gl::CompileShader(s);
    GLint ok=0;gl::GetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) {
        char info[1024]{};gl::GetShaderInfoLog(s,sizeof(info)-1,nullptr,info);
        LogF("HK scope: shader failed: %s",info);gl::DeleteShader(s);return 0;
    }
    return s;
}
void DeleteResources() {
    if (vbo) gl::DeleteBuffers(1,&vbo);
    if (vao) gl::DeleteVertexArrays(1,&vao);
    if (program) gl::DeleteProgram(program);
    if (fbo) gl::DeleteFramebuffers(1,&fbo);
    if (depth) gl::DeleteRenderbuffers(1,&depth);
    if (texture) glDeleteTextures(1,&texture);
    texture=depth=fbo=program=vao=vbo=0;
}
bool Create() {
    if (fbo) return true;
    if (failed || !gl::Loaded() || !gl::LoadedShaderApi()) return false;
    TargetState oldTarget;oldTarget.Save();
    GLint oldTexture=0,oldRenderbuffer=0,oldVao=0,oldBuffer=0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
    glGetIntegerv(0x8CA7,&oldRenderbuffer); // GL_RENDERBUFFER_BINDING
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&oldVao);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&oldBuffer);
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,kSize,kSize,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    gl::GenRenderbuffers(1,&depth);gl::BindRenderbuffer(GL_RENDERBUFFER,depth);
    gl::RenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,kSize,kSize);
    gl::GenFramebuffers(1,&fbo);gl::BindFramebuffer(GL_FRAMEBUFFER,fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    gl::FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
    bool ok=gl::CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    const char* vert="#version 150\nin vec2 aPos;out vec2 uv;uniform mat4 mvp;void main(){uv=aPos;gl_Position=mvp*vec4(aPos,0,1);}";
    const char* frag="#version 150\nin vec2 uv;out vec4 color;uniform sampler2D image;uniform int visible;"
        "void main(){float r=length(uv);if(r>1.0)discard;vec3 c=vec3(0.025);"
        "float px=max(fwidth(uv.x),fwidth(uv.y));"
        "if(r<0.92&&visible!=0){c=texture(image,uv*0.5+0.5).rgb;"
        "if((abs(uv.x)<max(0.006,px*0.75)||abs(uv.y)<max(0.006,px*0.75))&&r>0.025)c=vec3(0.02);"
        "if(r<max(0.009,px*0.8))c=vec3(0.8,0.03,0.03);}color=vec4(c,1);}";
    GLuint vs=ok ? Compile(GL_VERTEX_SHADER,vert) : 0;
    GLuint fs=vs ? Compile(GL_FRAGMENT_SHADER,frag) : 0;
    if (vs && fs) {
        program=gl::CreateProgram();gl::AttachShader(program,vs);gl::AttachShader(program,fs);gl::LinkProgram(program);
        GLint linked=0;gl::GetProgramiv(program,GL_LINK_STATUS,&linked);ok=linked!=0;
        if (!ok) { char info[1024]{};gl::GetProgramInfoLog(program,sizeof(info)-1,nullptr,info);LogF("HK scope: link failed: %s",info); }
    } else ok=false;
    if (vs) gl::DeleteShader(vs);if (fs) gl::DeleteShader(fs);
    if (ok) {
        uMvp=gl::GetUniformLocation(program,"mvp");uImage=gl::GetUniformLocation(program,"image");uVisible=gl::GetUniformLocation(program,"visible");
        const float quad[]={-1,-1,1,-1,-1,1,1,-1,1,1,-1,1};
        gl::GenVertexArrays(1,&vao);gl::BindVertexArray(vao);gl::GenBuffers(1,&vbo);gl::BindBuffer(GL_ARRAY_BUFFER,vbo);
        gl::BufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
        GLint attr=gl::GetAttribLocation(program,"aPos");
        ok=attr>=0 && uMvp>=0 && uImage>=0 && uVisible>=0;
        if (ok) { gl::EnableVertexAttribArray(attr);gl::VertexAttribPointer(attr,2,GL_FLOAT,GL_FALSE,2*sizeof(float),nullptr); }
    }
    gl::BindVertexArray(oldVao);gl::BindBuffer(GL_ARRAY_BUFFER,oldBuffer);
    gl::BindRenderbuffer(GL_RENDERBUFFER,oldRenderbuffer);glBindTexture(GL_TEXTURE_2D,oldTexture);oldTarget.Restore();
    if (!ok) { DeleteResources();failed=true;Log("HK scope: unavailable; normal VR remains active"); }
    else Log("HK scope: 512px independent lens ready (3x reference magnification)");
    return ok;
}
bool Prepare() {
    if (checked) return enabled;
    checked=true;
    motiongun::Frame lens{},camera{};
    if (!FirstPersonHKScopePose(lens,camera,&lensRadiusMetres) || Cfg().monoTracking || !Cfg().duplicateDraws ||
        Cfg().eyeOffsetMode!=3 || !Cfg().perEyeView || Cfg().perEyeProjection!=1) return false;
    float rot[3][3]{},pos[3]{};
    if (!CameraViewFrame(rot,pos)) return false;
    const Affine world=hkscope::WorldView(rot,pos);
    delta=Mul(hkscope::View(camera),InvertRigid(world));
    const float scale=LiveWorldUnitsPerMetre();
    const mat4 model=hkscope::LensModel(lens,scale,lensRadiusMetres);
    for (int eye=0;eye<2;++eye) {
        const Affine view=Mul(VR().EyeView(Eye(eye)),world);
        const Affine eyeWorld=InvertRigid(view);
        const motiongun::Vec eyePosition{eyeWorld.r[0][3],eyeWorld.r[1][3],eyeWorld.r[2][3]};
        const auto distance=motiongun::Sub(lens.origin,eyePosition);
        eyeRelief[eye]=motiongun::Dot(distance,{lens.basis.r[0][1],lens.basis.r[1][1],lens.basis.r[2][1]})/scale;
        eyeMiss[eye]=std::sqrt(std::fmax(0.f,motiongun::Dot(distance,distance)/(scale*scale)-eyeRelief[eye]*eyeRelief[eye]));
        visible[eye]=hkscope::EyeBox(lens,{eyeWorld.r[0][3],eyeWorld.r[1][3],eyeWorld.r[2][3]},scale);
        mat4 projection{};float zn=16,zf=32768;ExtractNearFar(Proj()[1],zn,zf);
        if (Cfg().nearClip>0) zn=Cfg().nearClip;if (Cfg().farClip>0) zf=Cfg().farClip;
        VR().EyeProjection(Eye(eye),zn,zf,projection);
        if (!hkscope::LensProjection(projection,view,model,scale,zn,lensMvp[eye])) visible[eye]=false;
    }
    // Do not pay another scene's draw cost with the rifle lowered.
    const uint64_t now=GetTickCount64();
    if (now-lastReport>=5000) {
        LogF("HK scope: eye-box=%d/%d mesh-fit=%d radius=%.3fm relief=%.3f/%.3fm miss=%.3f/%.3fm",
            int(visible[0]),int(visible[1]),int(Cfg().firstPersonHKScopeMeshFit),lensRadiusMetres,
            eyeRelief[0],eyeRelief[1],eyeMiss[0],eyeMiss[1]);
        lastReport=now;
    }
    if (!visible[0] && !visible[1]) return false;
    enabled=Create();return enabled;
}
} // namespace

bool HKScopeCapturing() { return capturing; }
void HKScopeCaptureViewport() { glViewport(0,0,kSize,kSize);glScissor(0,0,kSize,kSize);glEnable(GL_SCISSOR_TEST); }
bool HKScopeBeginCapture() {
    if (capturing || FirstPersonDrawingTrackedHands() || !Prepare()) return false;
    target.Save();gl::BindFramebuffer(GL_FRAMEBUFFER,fbo);HKScopeCaptureViewport();
    if (!cleared) {
        GLfloat color[4]{};GLdouble clearDepth=1;GLboolean mask[4]{},write=false;
        glGetFloatv(GL_COLOR_CLEAR_VALUE,color);glGetDoublev(GL_DEPTH_CLEAR_VALUE,&clearDepth);
        glGetBooleanv(GL_COLOR_WRITEMASK,mask);glGetBooleanv(GL_DEPTH_WRITEMASK,&write);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_TRUE);glClearColor(0,0,0,1);glClearDepth(1);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glColorMask(mask[0],mask[1],mask[2],mask[3]);glDepthMask(write);glClearColor(color[0],color[1],color[2],color[3]);glClearDepth(clearDepth);
        cleared=true;
    }
    capturing=true;++captured;return true;
}
void HKScopeEndCapture() {
    capturing=false;target.Restore();
    VidState().consts|=kProj|kView; // the next eye must not inherit lens uniforms
}
void HKScopeProjection(mat4& projection,bool sky) {
    float zn=16,zf=32768;ExtractNearFar(projection,zn,zf);
    if (Cfg().nearClip>0) zn=Cfg().nearClip;if (Cfg().farClip>0) zf=Cfg().farClip;
    Affine transform=delta;
    if (sky) transform.r[0][3]=transform.r[1][3]=transform.r[2][3]=0;
    projection=Mul4(hkscope::Projection(zn,zf,lensRadiusMetres),AffineToMat4(transform));
}
void HKScopePresent() {
    motiongun::Frame lens{},camera{};
    if (enabled && captured && Stereo().valid() && FirstPersonHKScopePose(lens,camera)) {
        const auto now=GetTickCount64();
        if (now-lastCompositeReport>=2000) {
            const int e=visible[0] ? 0 : 1;
            LogF("HK scope: composite draws=%u eye=%d clip-w=%.2f clip-z=%.2f radius=%.3fm",
                captured,e,lensMvp[e].m[15],lensMvp[e].m[14],lensRadiusMetres);
            lastCompositeReport=now;
        }
        TargetState oldTarget;oldTarget.Save();
        GLint oldProgram=0,oldVao=0,oldActive=0,oldTexture=0,oldDepthFunc=0,oldSampler=0;
        glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&oldVao);
        glGetIntegerv(GL_ACTIVE_TEXTURE,&oldActive);gl::ActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
        if (gl::BindSampler) { glGetIntegerv(0x8919,&oldSampler);gl::BindSampler(0,0); }
        glGetIntegerv(GL_DEPTH_FUNC,&oldDepthFunc);
        const GLboolean wasDepth=glIsEnabled(GL_DEPTH_TEST),wasBlend=glIsEnabled(GL_BLEND),wasCull=glIsEnabled(GL_CULL_FACE);
        GLboolean write=false,mask[4]{};GLfloat range[2]{};
        glGetBooleanv(GL_DEPTH_WRITEMASK,&write);glGetBooleanv(GL_COLOR_WRITEMASK,mask);glGetFloatv(GL_DEPTH_RANGE,range);
        gl::BindFramebuffer(GL_FRAMEBUFFER,Stereo().fbo());
        glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);glDepthRange(0,1);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
        gl::UseProgram(program);gl::BindVertexArray(vao);glBindTexture(GL_TEXTURE_2D,texture);gl::Uniform1i(uImage,0);
        for (int eye=0;eye<2;++eye) {
            // Only the aligned eye sees through the lens; avoid an opaque disc
            // masking the other eye's normal view when looking around the tube.
            if (!visible[eye]) continue;
            Stereo().SetEyeViewport(eye);gl::UniformMatrix4fv(uMvp,1,GL_FALSE,lensMvp[eye].m);
            gl::Uniform1i(uVisible,1);glDrawArrays(GL_TRIANGLES,0,6);
        }
        gl::BindVertexArray(oldVao);gl::UseProgram(oldProgram);glBindTexture(GL_TEXTURE_2D,oldTexture);
        if (gl::BindSampler) gl::BindSampler(0,oldSampler);
        gl::ActiveTexture(oldActive);
        glDepthFunc(oldDepthFunc);glDepthMask(write);glDepthRange(range[0],range[1]);glColorMask(mask[0],mask[1],mask[2],mask[3]);
        if (!wasDepth) glDisable(GL_DEPTH_TEST);if (wasBlend) glEnable(GL_BLEND);if (wasCull) glEnable(GL_CULL_FACE);
        oldTarget.Restore();
    }
    checked=enabled=cleared=capturing=false;captured=0;visible[0]=visible[1]=false;
}
void HKScopeShutdown() {
    // As with the existing stereo target, deletion requires its GL context.
    if (wglGetCurrentContext() && gl::Loaded()) DeleteResources();
    checked=enabled=cleared=capturing=failed=false;captured=0;
}
} // namespace tr
