#pragma once
#include "MotionGunMath.h"

namespace tr::wristcap {
constexpr float CutWeight=.05f; // HandSkin.h fragment boundary
struct Vertex {
    motiongun::Vec position{};
    int joint[3]{};
    float weight[3]{};
};
struct Edge { Vertex a,b; float t; };
inline float HandWeight(const Vertex& v,int hand) {
    float weight=0;
    for (int i=0;i<3;++i) if (v.joint[i]==hand) weight+=v.weight[i];
    return weight;
}
// Each triangle straddling the skin cutoff contributes one rim segment.
// Fan that segment to the common wrist centre to close the actual mesh cut.
inline bool CutTriangle(const Vertex (&v)[3],int hand,Edge (&out)[2]) {
    int count=0;
    for (int i=0;i<3;++i) {
        const auto& a=v[i]; const auto& b=v[(i+1)%3];
        const float wa=HandWeight(a,hand),wb=HandWeight(b,hand);
        if ((wa<CutWeight)==(wb<CutWeight)) continue;
        if (count>=2) return false;
        out[count++]={a,b,(CutWeight-wa)/(wb-wa)};
    }
    return count==2;
}
inline motiongun::Vec Skin(const Vertex& v,const float* palette) {
    motiongun::Vec p{};
    for (int i=0;i<3;++i) {
        if (v.joint[i]<0 || v.joint[i]>=32 || v.weight[i]<=0) continue;
        p=motiongun::Add(p,motiongun::Scale(motiongun::Transform(
            motiongun::ReadRows(palette+v.joint[i]*12),v.position),v.weight[i]));
    }
    return p;
}
inline motiongun::Vec Rim(const Edge& e,const float* palette) {
    const auto a=Skin(e.a,palette),b=Skin(e.b,palette);
    return motiongun::Add(a,motiongun::Scale(motiongun::Sub(b,a),e.t));
}
}
