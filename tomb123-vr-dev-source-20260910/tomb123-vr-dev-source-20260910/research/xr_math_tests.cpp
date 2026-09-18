#include <cstdint>
#include "../src/xr_math.h"
#include <cstdio>
#include <cstdlib>
using namespace vrmath;
void near(float a,float b,float e=0.0001f){if(fabsf(a-b)>e){printf("FAIL %.6f != %.6f\n",a,b);exit(1);}}
M engine(float p,float y,float r){float sp=sinf(p),cp=cosf(p),sy=sinf(y),cy=cosf(y),sr=sinf(r),cr=cosf(r);return {{{cr*cy+sy*sp*sr,sr*cp,cy*sp*sr-cr*sy},{sy*sp*cr-sr*cy,cr*cp,cy*sp*cr+sr*sy},{sy*cp,-sp,cy*cp}}};}
int main(){XrPosef origin{{0,0,0,1},{0,1.6f,0}},eye=origin;auto identity=rotation(origin.orientation);eye.position.x=.032f;auto p=relativePosition(eye,origin);near(p.x,.032f);near(p.y,0);near(p.z,0);eye.position={0,1.7f,-.2f};p=relativePosition(eye,origin);near(p.y,-.1f);near(p.z,.2f);
 auto view=relativeView(origin,origin,identity);for(int i=0;i<3;i++)for(int j=0;j<3;j++)near(view.a[i][j],i==j?1.f:0.f);
 XrPosef turned=origin;turned.orientation={0,sinf(.5f),0,cosf(.5f)};view=relativeView(turned,origin,identity);auto forward=mul(view,XrVector3f{-sinf(1.f),0,cosf(1.f)});near(forward.x,0);near(forward.z,1);
 auto recentered=relativeView(turned,turned,identity);for(int i=0;i<3;i++)for(int j=0;j<3;j++)near(recentered.a[i][j],i==j?1.f:0.f);
 for(float p0=-1.5f;p0<=1.5f;p0+=.2f)for(float y=-3;y<=3;y+=.3f)for(float r=-3;r<=3;r+=.6f){auto m=engine(p0,y,r);int16_t a[3];angles(m,a);constexpr float rad=6.28318530718f/65536;auto round=engine(a[0]*rad,a[1]*rad,a[2]*rad);for(int i=0;i<3;i++)for(int j=0;j<3;j++)near(m.a[i][j],round.a[i][j],.0002f);}
 float proj[16]{};proj[0]=1;proj[5]=-1;proj[11]=1;proj[10]=.5f;proj[14]=-10;XrFovf f{-.8f,.9f,.85f,-.7f};projection(proj,f);near(proj[0]*tanf(f.angleLeft)+proj[8],-1);near(proj[0]*tanf(f.angleRight)+proj[8],1);near(proj[5]*(-tanf(f.angleUp))+proj[9],1);near(proj[5]*(-tanf(f.angleDown))+proj[9],-1);near(proj[10],.5f);near(proj[14],-10);

 proj[11]=-1;projection(proj,f);near(proj[0]*tanf(f.angleLeft)-proj[8],-1);near(proj[0]*tanf(f.angleRight)-proj[8],1);near(proj[5]*(-tanf(f.angleUp))-proj[9],1);near(proj[5]*(-tanf(f.angleDown))-proj[9],-1);
 puts("PASS: IPD, translation axes, yaw direction, recenter, 3000+ Euler round trips, asymmetric FOV edges and preserved depth.");}
