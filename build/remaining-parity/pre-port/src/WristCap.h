#pragma once
#include "MotionGunMath.h"
#include <array>
#include <map>
#include <set>
#include <vector>
#include <algorithm>

namespace tr::wristcap {
constexpr float CutWeight=.5f; // HandSkin.h fragment boundary
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
inline motiongun::Vec Rim(const Edge& e,const float* palette,int hand) {
    const auto wrist=motiongun::ReadRows(palette+hand*12);
    const auto a=motiongun::Transform(wrist,e.a.position);
    const auto b=motiongun::Transform(wrist,e.b.position);
    return motiongun::Add(a,motiongun::Scale(motiongun::Sub(b,a),e.t));
}

using Loop=std::vector<motiongun::Vec>;
// Extract the boundary of the visible surface, including pre-existing holes.
// Weld positions across duplicated texture/normal vertices. Process all of a
// mesh's materials together so material borders do not become false openings.
class Boundary {
    std::map<std::array<long long,3>,int> welded;
    std::vector<motiongun::Vec> points;
    std::map<std::pair<int,int>,unsigned> edges;
    std::set<std::vector<int>> faces;
    int Point(motiongun::Vec p) {
        const std::array<long long,3> key={std::llround(p.x*1000.),
            std::llround(p.y*1000.),std::llround(p.z*1000.)};
        auto found=welded.find(key);
        if (found!=welded.end()) return found->second;
        const int id=int(points.size()); welded.emplace(key,id); points.push_back(p);
        return id;
    }
public:
    void Add(const Vertex (&triangle)[3],int hand) {
        Loop clipped;
        for (int i=0;i<3;++i) {
            const auto& a=triangle[i]; const auto& b=triangle[(i+1)%3];
            const auto p=a.position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                std::fabs(p.x)>1e7f || std::fabs(p.y)>1e7f || std::fabs(p.z)>1e7f) return;
            const float wa=HandWeight(a,hand),wb=HandWeight(b,hand);
            if (wa>=CutWeight) clipped.push_back(p);
            if ((wa<CutWeight)!=(wb<CutWeight))
                clipped.push_back(motiongun::Add(p,motiongun::Scale(
                    motiongun::Sub(b.position,p),(CutWeight-wa)/(wb-wa))));
        }
        std::vector<int> ids;
        for (auto p:clipped) {
            const int id=Point(p);
            if (ids.empty() || ids.back()!=id) ids.push_back(id);
        }
        if (ids.size()>1 && ids.front()==ids.back()) ids.pop_back();
        if (ids.size()<3) return;
        auto key=ids; std::sort(key.begin(),key.end());
        // Some buffers repeat triangles for separate render passes.
        if (!faces.insert(key).second) return;
        for (size_t i=0;i<ids.size();++i) {
            const int a=ids[i],b=ids[(i+1)%ids.size()];
            ++edges[std::minmax(a,b)];
        }
    }
    std::vector<Loop> Loops() const {
        std::map<int,std::vector<int>> neighbours;
        for (const auto& e:edges) if (e.second==1) {
            neighbours[e.first.first].push_back(e.first.second);
            neighbours[e.first.second].push_back(e.first.first);
        }
        std::set<int> seen;
        std::vector<Loop> loops;
        for (const auto& node:neighbours) {
            if (seen.count(node.first)) continue;
            std::vector<int> pending{node.first},component;
            bool closed=true;
            while (!pending.empty()) {
                const int v=pending.back(); pending.pop_back();
                if (!seen.insert(v).second) continue;
                component.push_back(v);
                const auto& next=neighbours.at(v);
                if (next.size()!=2) closed=false;
                pending.insert(pending.end(),next.begin(),next.end());
            }
            if (!closed || component.size()<3) continue;
            Loop loop; int previous=-1,current=node.first;
            do {
                loop.push_back(points[current]);
                const auto& next=neighbours.at(current);
                const int following=next[0]==previous ? next[1] : next[0];
                previous=current; current=following;
            } while (current!=node.first && loop.size()<=component.size());
            if (current==node.first) loops.push_back(std::move(loop));
        }
        return loops;
    }
};
}
