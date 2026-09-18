#include "../src/classic_math.h"
#include <cstdio>
#include <cstdlib>
void require(bool c){if(!c){puts("FAIL classic scissor reprojection");exit(1);}}
int main(){using namespace classicvr;
 // Same symmetric CPU/GPU projection: rectangle only grows by rounding guard.
 auto r=reproject({100,200,300,150},1000,1000,500,500,500,-1,1,1,-1);require(r.x==99&&r.y==199&&r.w==302&&r.h==152);
 // CPU frustum encloses a wider domain; full-screen CPU bounds must still cover the eye.
 r=reproject({0,0,2560,1440},2560,1440,540,1280,720,-1.84f,.95f,1.33f,-1.33f);require(r.x==0&&r.y==0&&r.w==2560&&r.h==1440);
 // A portal centered in CPU space lands at the eye's asymmetric optical center.
 r=reproject({1280,720,1,1},2560,1440,540,1280,720,-1.84f,.95f,1.33f,-1.33f);require(r.x>1680&&r.x<1690&&r.y==719);
 r=reproject({0,0,0,10},2560,1440,540,1280,720,-1,1,1,-1);require(r.w==0&&r.h==0);
 // Any ray enclosed by a CPU rectangle remains enclosed after transformation,
 // for both eyes and every sampled position inside the headset frustum.
 for(int eye=0;eye<2;eye++){float l=eye?-.95f:-1.84f,rr=eye?1.84f:.95f;
 for(int a=0;a<100;a++)for(int b=0;b<100;b++){float ax=l+(rr-l)*(a+.5f)/100,ay=-1.33f+2.66f*(b+.5f)/100;float x=1280+540*ax,y=720+540*ay;Rect cpu{int(floorf(x))-2,int(floorf(y))-2,6,6};auto gpu=reproject(cpu,2560,1440,540,1280,720,l,rr,1.33f,-1.33f);float gx=2560*(ax-l)/(rr-l),gy=1440*(ay+1.33f)/2.66f;require(gx>=gpu.x&&gx<=gpu.x+gpu.w&&gy>=gpu.y&&gy<=gpu.y+gpu.h);}}
 puts("PASS: classic clip reprojection, optical centers, outward rounding and 20,000 visible-ray containment cases.");}
