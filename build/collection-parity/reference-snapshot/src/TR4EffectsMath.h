#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tr::effects {
enum class Texture { None, Sunrays, Atlas };

// Use app.getFilePath's resolved resource directory: during a game switch
// gGame still describes the previous game, while the loader uses gNextGame.
inline Texture Identify(const char* path) {
    if (!path) return Texture::None;
    if (!_stricmp(path,"4\\TEX\\3115.DDS")) return Texture::Sunrays;
    if (!_stricmp(path,"4\\TEX\\EFFECT.DDS")) return Texture::Atlas;
    return Texture::None;
}
inline float Strength(float value) {
    return std::isfinite(value) ? std::clamp(value,0.f,2.f) : 1.f;
}
inline uint8_t Blend(uint8_t source,float enhanced,float strength) {
    return uint8_t(std::lround(std::clamp(source+(enhanced-source)*strength,0.f,255.f)));
}

// Fit to the brighter, slightly warm sunray reference. Exact black stays
// black for additive blending; alpha and the existing ray silhouettes stay
// intact. This is a transfer curve, not embedded replacement image data.
inline void Sunrays(uint8_t* rgba,size_t pixels,float strength) {
    strength=Strength(strength);
    uint8_t lut[3][256];
    const float gain[3]={2.50f,2.61f,2.23f};
    for (int c=0;c<3;++c) for (int v=0;v<256;++v) {
        const float boosted=v*std::max(1.f,gain[c]*std::pow(v/40.f,.16f));
        lut[c][v]=Blend(uint8_t(v),boosted,strength);
    }
    for (size_t i=0;i<pixels;++i) for (int c=0;c<3;++c) rgba[4*i+c]=lut[c][rgba[4*i+c]];
}

// Only the effect cells changed by the supplied TR4 pack are enhanced.
// Ropes, footsteps, bubbles, lens flares, lightning and water-strip cells
// retain their original bytes. Preserve a small cell-edge guard against
// filtering into an adjacent sprite. Redrawn artwork is not reproduced.
inline float AtlasGain(int x,int y) {
    if (x>=4 && x<252 && y>=4 && y<252) return 1.25f; // smoke
    if (x>=1060 && x<1564 && y>=4 && y<508) return 1.30f; // fire
    if (x>=1060 && x<1564 && y>=516 && y<668) return 1.50f; // spray
    if (x>=804 && x<1020 && y>=676 && y<924) return 1.85f; // splash
    return 1.f;
}
inline bool Enhance(Texture kind,std::vector<uint8_t>& rgba,int width,int height,float strength) {
    if (kind==Texture::None || width<=0 || height<=0 ||
        rgba.size()!=size_t(width)*height*4) return false;
    strength=Strength(strength);
    if (strength==0) return false;
    if (kind==Texture::Sunrays) Sunrays(rgba.data(),size_t(width)*height,strength);
    else {
        if (width!=height || width>2048 || 2048%width) return false;
        for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
            const float gain=AtlasGain((2*x+1)*1024/width,(2*y+1)*1024/height);
            if (gain==1) continue;
            auto* p=&rgba[(size_t(y)*width+x)*4];
            for (int c=0;c<3;++c) p[c]=Blend(p[c],p[c]*gain,strength);
        }
    }
    return true;
}
}
