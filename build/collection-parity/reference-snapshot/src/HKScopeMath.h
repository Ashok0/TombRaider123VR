#pragma once
#include "MotionGunMath.h"
#include "StereoMath.h"

namespace tr::hkscope {
// Rear aperture of shipped 4/ITEM/HAND_{BARE,GLOVES,XRAY}_HK.TRM.
// All aperture vertices are weighted entirely to joint 10. These are mesh
// coordinates, NOT native wrist coordinates or metres. Keep the disc just
// behind the rear lip (toward the eye), instead of inside the opaque tube.
inline bool MeshLens(const motiongun::Frame& wrist,const motiongun::Frame& inverseBind,
                     motiongun::Frame& lens,float& radius) {
    using namespace motiongun;
    const Frame mesh=Multiply(wrist,inverseBind);
    const Vec forward=Transform(mesh.basis,{0,.985f,.172f});
    const Vec right=Transform(mesh.basis,{1,0,0});
    const float f=std::sqrt(Dot(forward,forward)),r=std::sqrt(Dot(right,right));
    if (!std::isfinite(f) || !std::isfinite(r) || f<.001f || r<.001f) return false;
    const Vec y=Scale(forward,1/f);
    Vec z=Cross(right,y);
    const float u=std::sqrt(Dot(z,z));
    if (!std::isfinite(u) || u<.001f) return false;
    z=Scale(z,1/u);
    const Vec x=Cross(y,z);
    lens={{{{x.x,y.x,z.x},{x.y,y.y,z.y},{x.z,y.z,z.z}}},
          Transform(mesh,{63.45f,39.8f,80.5f})};
    radius=5.4f*r;
    return std::isfinite(lens.origin.x) && std::isfinite(lens.origin.y) && std::isfinite(lens.origin.z);
}
inline Affine WorldView(const float rot[3][3],const float pos[3]) {
    Affine world=Affine::Identity();
    for (int r=0;r<3;++r) for (int c=0;c<3;++c) {
        world.r[r][c]=rot[r][c]*(r==2 ? -1.f : 1.f);
        world.r[r][3]-=world.r[r][c]*pos[c];
    }
    return world;
}
// Native gun axes: +X right, +Y barrel-forward, +Z up.
inline Affine View(const motiongun::Frame& f) {
    Affine v{};
    for (int i=0;i<3;++i) {
        v.r[0][i]=f.basis.r[i][0];
        v.r[1][i]=-f.basis.r[i][2];
        v.r[2][i]=-f.basis.r[i][1];
    }
    const float p[]={f.origin.x,f.origin.y,f.origin.z};
    for (int row=0;row<3;++row) for (int i=0;i<3;++i) v.r[row][3]-=v.r[row][i]*p[i];
    return v;
}
inline bool EyeBox(const motiongun::Frame& lens, motiongun::Vec eye,float scale) {
    if (!std::isfinite(scale) || scale<=0) return false;
    const auto d=motiongun::Sub(lens.origin,eye);
    const auto axis=[&](int col) { return motiongun::Vec{lens.basis.r[0][col],lens.basis.r[1][col],lens.basis.r[2][col]}; };
    const float relief=motiongun::Dot(d,axis(1))/scale;
    const float x=motiongun::Dot(d,axis(0))/scale,z=motiongun::Dot(d,axis(2))/scale;
    return std::isfinite(relief) && relief>=0.001f && relief<=0.35f && x*x+z*z<=0.025f*0.025f;
}
inline mat4 LensModel(const motiongun::Frame& lens,float scale,float radiusMetres=.03f) {
    mat4 m{};
    for (int i=0;i<3;++i) {
        m.m[i]=lens.basis.r[i][0]*radiusMetres*scale;
        m.m[4+i]=lens.basis.r[i][2]*radiusMetres*scale;
        m.m[8+i]=-lens.basis.r[i][1];
    }
    m.m[12]=lens.origin.x;m.m[13]=lens.origin.y;m.m[14]=lens.origin.z;m.m[15]=1;
    return m;
}
// Keep the physical mesh's exact screen position, size and perspective.
// Only clip depth can change; translating the display away from the eye
// causes off-axis drift relative to the scope aperture.
inline bool LensProjection(const mat4& projection,const Affine& view,const mat4& model,
                           float scale,float nearPlane,mat4& result) {
    const mat4 mv=Mul4(AffineToMat4(view),model);
    const float closestZ=mv.m[14]+std::fabs(mv.m[2])+std::fabs(mv.m[6]);
    // No projection is meaningful once the eye crosses the lens plane.
    // Reject any corner at/behind the eye instead of moving the image.
    if (closestZ>-.0005f*scale) return false;
    const bool comfort=closestZ>-nearPlane;
    if (comfort && mv.m[10]<.25f) return false;
    result=Mul4(projection,mv);
    // Bypass near clipping only for this lens, preserving X/Y/W exactly.
    // Normal-distance lenses retain ordinary world-depth occlusion.
    if (comfort) for (int col=0;col<4;++col) result.m[col*4+2]=-result.m[col*4+3];
    return true;
}
inline mat4 Projection(float nearPlane,float farPlane,float radiusMetres=.03f) {
    mat4 p{};
    // Target 3x at reference 12 cm eye relief, independent of mesh aperture size.
    const float tangent=radiusMetres/(0.12f*3.0f);
    BuildEyeProjection(p,-tangent,tangent,-tangent,tangent,nearPlane,farPlane,true);
    return p;
}
} // namespace tr::hkscope
