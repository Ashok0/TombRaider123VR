#pragma once
#include "LocomotionMath.h"
#include <algorithm>

namespace tr::firstperson {
struct PianoVertex { float x,y,z; };
// Exact classic mesh coordinates; verify_manor_piano.py checks the level asset.
#include "ManorPianoMesh.inl"
struct PianoPart { int count; int vertices[12]; };
// Three body sections, keyboard, four legs, bench and the raised lid. Keeping
// these separate preserves the concave body and the space around the bench.
inline constexpr PianoPart ManorPianoParts[]={
    {10,{0,1,2,3,4,5,16,17,20,21}},
    {8,{2,3,4,5,6,7,8,9}}, {8,{6,7,8,9,10,11,12,13}},
    {8,{14,15,16,17,18,19,20,21}},
    {8,{24,25,26,27,28,29,30,31}}, {8,{32,33,34,35,36,37,38,39}},
    {8,{40,41,42,43,44,45,46,47}}, {8,{48,49,50,51,52,53,54,55}},
    {8,{56,57,58,59,60,61,62,63}}, {4,{0,11,22,23}}
};
inline locomotion::Vec PianoLocal(locomotion::Vec world) {
    return {world.z-83456,45568-world.x};
}
inline locomotion::Vec PianoWorld(locomotion::Vec local) {
    return {45568-local.z,83456+local.x};
}
// Project the portion of each convex part overlapping Lara's vertical span.
// Intersections of every vertex pair include all real convex-hull edges;
// extra diagonal intersections lie inside the hull and do not enlarge it.
inline int PianoFootprint(const PianoPart& part,float top,float feet,locomotion::Vec* hull) {
    using locomotion::Vec;
    Vec points[144];int count=0;
    for(int i=0;i<part.count;++i) {
        const auto a=ManorPianoVertices[part.vertices[i]];
        if(a.y>=top && a.y<=feet) points[count++]={a.x,a.z};
        for(int j=0;j<i;++j) {
            const auto b=ManorPianoVertices[part.vertices[j]];
            for(float plane:{top,feet}) if((a.y<plane && b.y>plane) || (b.y<plane && a.y>plane)) {
                const float t=(plane-a.y)/(b.y-a.y);
                points[count++]={a.x+(b.x-a.x)*t,a.z+(b.z-a.z)*t};
            }
        }
    }
    if(!count) return 0;
    std::sort(points,points+count,[](Vec a,Vec b){return a.x<b.x || (a.x==b.x && a.z<b.z);});
    count=int(std::unique(points,points+count,[](Vec a,Vec b){return a.x==b.x && a.z==b.z;})-points);
    if(count==1) { hull[0]=points[0];return 1; }
    const auto cross=[](Vec a,Vec b,Vec c){return (b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x);};
    int n=0;
    for(int i=0;i<count;++i) {
        while(n>=2 && cross(hull[n-2],hull[n-1],points[i])<=0) --n;
        hull[n++]=points[i];
    }
    const int lower=n;
    for(int i=count-2;i>=0;--i) {
        while(n>lower && cross(hull[n-2],hull[n-1],points[i])<=0) --n;
        hull[n++]=points[i];
    }
    return n-1;
}
// Sweep a circular horizontal footprint against each expanded convex part.
// Edge planes plus 16 radial planes give a conservative rounded boundary,
// without the excessive diagonal padding of the original square footprint.
inline bool SweepManorPiano(locomotion::Vec oldWorld,locomotion::Vec targetWorld,
                            float radius,locomotion::Vec& resolved,float feet=0,float height=762) {
    using namespace locomotion;
    resolved=targetWorld;
    if(!std::isfinite(radius) || radius<=0 || radius>400 ||
       !std::isfinite(feet) || !std::isfinite(height) || height<=0 || height>2048 ||
       !std::isfinite(oldWorld.x) || !std::isfinite(oldWorld.z) ||
       !std::isfinite(targetWorld.x) || !std::isfinite(targetWorld.z)) return false;
    const Vec from=PianoLocal(oldWorld),to=PianoLocal(targetWorld),delta=to-from;
    float firstContact=2;
    for(const auto& part:ManorPianoParts) {
        Vec hull[288];const int count=PianoFootprint(part,feet-height,feet,hull);
        if(!count) continue;
        float enter=0,leave=1,oldOutside=-1e30f,newOutside=-1e30f;
        bool misses=false;
        for(int i=0;i<count+16;++i) {
            Vec normal;
            if(i<count) {
                const Vec edge=hull[(i+1)%count]-hull[i];const float length=Length(edge);
                if(length<.001f) continue;
                normal={edge.z/length,-edge.x/length};
            } else {
                const float angle=(i-count)*Pi/8;
                normal={std::cos(angle),std::sin(angle)};
            }
            const auto dot=[&](Vec p){return p.x*normal.x+p.z*normal.z;};
            float bound=-1e30f;
            for(int j=0;j<count;++j) bound=std::max(bound,dot(hull[j]));
            bound+=radius+8;
            const float a=dot(from)-bound,b=dot(to)-bound,d=b-a;
            oldOutside=std::max(oldOutside,a);newOutside=std::max(newOutside,b);
            if(std::fabs(d)<.00001f) { if(a>0) misses=true; }
            else if(d<0) enter=std::max(enter,-a/d);
            else leave=std::min(leave,-a/d);
        }
        if(oldOutside<=0) {
            // Permit escape from an overlapping save/spawn position. Never
            // push out of one part into another or through a native obstacle.
            if(newOutside>oldOutside+.01f) continue;
            firstContact=0;
        } else if(!misses && enter<=leave && leave>=0 && enter<=1)
            firstContact=std::min(firstContact,enter);
    }
    if(firstContact>1) return false;
    const float distance=Length(delta);
    resolved=PianoWorld(from+delta*std::max(0.f,firstContact-(distance>0 ? 1.f/distance : 0.f)));
    return true;
}
} // namespace tr::firstperson
