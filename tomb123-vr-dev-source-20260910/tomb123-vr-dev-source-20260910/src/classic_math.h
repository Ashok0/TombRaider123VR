#pragma once
#include <algorithm>
#include <cmath>
namespace classicvr {
struct Rect {int x,y,w,h;};
// CPU portal rectangles use a symmetric focal length in pixels. Reproject
// their edges into the GPU eye's asymmetric projection; round outward so
// integer portal clipping never erases a visible edge.
inline Rect reproject(Rect r,int width,int height,float focal,float cx,float cy,float left,float right,float up,float down){
 if(r.w<=0||r.h<=0||width<=0||height<=0||focal<=0)return {0,0,0,0};
 float sx=width/(focal*(right-left)),sy=height/(focal*(up-down));
 float ox=-width*left/(right-left)-cx*sx;
 float oy=height*up/(up-down)-cy*sy;
 int x0=std::clamp((int)floorf(r.x*sx+ox)-1,0,width),y0=std::clamp((int)floorf(r.y*sy+oy)-1,0,height);
 int x1=std::clamp((int)ceilf((r.x+r.w)*sx+ox)+1,0,width),y1=std::clamp((int)ceilf((r.y+r.h)*sy+oy)+1,0,height);
 return {x0,y0,std::max(0,x1-x0),std::max(0,y1-y0)};
}
}
