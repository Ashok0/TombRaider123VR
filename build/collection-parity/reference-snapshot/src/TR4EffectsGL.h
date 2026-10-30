#pragma once
#include "TR4EffectsMath.h"
#include "GL.h"

namespace tr::effects {
namespace gpu {
constexpr GLenum ArrayBinding=0x8C1D,Depth=0x8071,BC7=0x8E8C,BC7srgb=0x8E8D;
constexpr GLenum PackBuffer=0x88EB,UnpackBuffer=0x88EC,PackBinding=0x88ED,UnpackBinding=0x88EF;
using BindBuffer=void (APIENTRY*)(GLenum,GLuint);
using CompressedImage=void (APIENTRY*)(GLenum,GLint,GLenum,GLsizei,GLsizei,GLint,GLsizei,const void*);
using GetCompressed=void (APIENTRY*)(GLenum,GLint,void*);
template<class T> T Proc(const char* name) {
    const auto p=wglGetProcAddress(name);
    const auto n=reinterpret_cast<intptr_t>(p);
    return n>3 && n!=-1 ? reinterpret_cast<T>(p) : nullptr;
}
struct TransferState {
    BindBuffer bind;
    GLint array=0,texture=0,pack=0,unpack=0;
    static constexpr GLenum fields[]={GL_PACK_ALIGNMENT,GL_UNPACK_ALIGNMENT,
        GL_PACK_ROW_LENGTH,GL_UNPACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,GL_UNPACK_SKIP_ROWS,
        GL_PACK_SKIP_PIXELS,GL_UNPACK_SKIP_PIXELS,0x806B,0x806E,0x806C,0x806D};
    GLint values[12]{};
    explicit TransferState(BindBuffer fn):bind(fn) {
        glGetIntegerv(ArrayBinding,&array);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        glGetIntegerv(PackBinding,&pack);glGetIntegerv(UnpackBinding,&unpack);
        for(int i=0;i<12;++i) glGetIntegerv(fields[i],&values[i]);
        bind(PackBuffer,0);bind(UnpackBuffer,0);
        for(int i=0;i<12;++i) glPixelStorei(fields[i],i<2?1:0);
    }
    ~TransferState() {
        glBindTexture(GL_TEXTURE_2D_ARRAY,GLuint(array));glBindTexture(GL_TEXTURE_2D,GLuint(texture));
        for(int i=0;i<12;++i) glPixelStorei(fields[i],values[i]);
        bind(PackBuffer,GLuint(pack));bind(UnpackBuffer,GLuint(unpack));
    }
};
struct ScratchTexture {
    GLuint id=0;
    ScratchTexture() { glGenTextures(1,&id); }
    ~ScratchTexture() { if(id) glDeleteTextures(1,&id); }
};
}

// Stage only the named layer/mip, never the entire shared texture array.
// On failure the caller forwards the original compressed buffer to the engine.
// Native texture objects, samplers, array storage and render caches stay intact.
inline bool PrepareUpload(GLuint array,int layer,int mip,Texture kind,float strength,
                          const void* source,std::vector<uint8_t>& compressed) {
    using namespace gpu;
    if(!array || !source || layer<0 || mip<0 || mip>=6 || kind==Texture::None ||
       Strength(strength)==0 || !wglGetCurrentContext()) return false;
    const auto image=Proc<CompressedImage>("glCompressedTexImage2D");
    const auto get=Proc<GetCompressed>("glGetCompressedTexImage");
    const auto bind=Proc<BindBuffer>("glBindBuffer");
    if(!image || !get || !bind || glGetError()!=GL_NO_ERROR) return false;
    TransferState state(bind);
    glBindTexture(GL_TEXTURE_2D_ARRAY,array);
    GLint w=0,h=0,d=0,format=0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY,mip,GL_TEXTURE_WIDTH,&w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY,mip,GL_TEXTURE_HEIGHT,&h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY,mip,Depth,&d);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY,mip,GL_TEXTURE_INTERNAL_FORMAT,&format);
    const int expected=(kind==Texture::Sunrays?512:2048)>>mip;
    if(w!=expected || h!=expected || layer>=d || (format!=BC7 && format!=BC7srgb)) return false;
    const size_t bytes=size_t((w+3)/4)*((h+3)/4)*16;
    ScratchTexture scratch;
    glBindTexture(GL_TEXTURE_2D,scratch.id);
    image(GL_TEXTURE_2D,0,format,w,h,0,GLsizei(bytes),source);
    std::vector<uint8_t> original(size_t(w)*h*4);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,original.data());
    if(glGetError()!=GL_NO_ERROR) return false;
    auto enhanced=original;
    if(!Enhance(kind,enhanced,w,h,strength)) return false;
    // Driver BC7 encoder avoids shipping an image decoder or replacement art.
    glTexImage2D(GL_TEXTURE_2D,0,format,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,enhanced.data());
    GLint encodedBytes=0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,0x86A0,&encodedBytes);
    if(glGetError()!=GL_NO_ERROR || encodedBytes!=GLint(bytes)) return false;
    compressed.resize(bytes);
    get(GL_TEXTURE_2D,0,compressed.data());
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,enhanced.data());
    if(glGetError()!=GL_NO_ERROR) return false;
    // BC7 is lossy. Keep the exact source block if alpha changes by >2/255,
    // lights an additive-black pixel, or touches an unselected atlas cell.
    // Neighboring sprites and fully transparent pixels remain exact.
    bool changed=false;
    for(int by=0;by<h;by+=4) for(int bx=0;bx<w;bx+=4) {
        bool keep=false;
        for(int y=by;y<std::min(by+4,h);++y) for(int x=bx;x<std::min(bx+4,w);++x) {
            const auto i=(size_t(y)*w+x)*4;
            if(std::abs(int(original[i+3])-int(enhanced[i+3]))>2 ||
                (original[i+3]==0 && enhanced[i+3]!=0)) keep=true;
            if(original[i]==0 && original[i+1]==0 && original[i+2]==0 &&
                (enhanced[i] || enhanced[i+1] || enhanced[i+2])) keep=true;
            if(kind==Texture::Atlas && AtlasGain((2*x+1)*1024/w,(2*y+1)*1024/h)==1.f) keep=true;
        }
        const auto offset=(size_t(by/4)*((w+3)/4)+bx/4)*16;
        const auto* native=static_cast<const uint8_t*>(source)+offset;
        if(keep) std::memcpy(compressed.data()+offset,native,16);
        else if(std::memcmp(compressed.data()+offset,native,16)) changed=true;
    }
    return changed;
}
}
