#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cmath>
#include "../src/firstperson_math.h"
bool near(float a,float b){return std::abs(a-b)<.00001f;}
int main(){
 int32_t previousBody[]={1000,-300,500},currentBody[]={1100,-350,700};
 int32_t previous[]={16384,0,0,0,0,16384,0,-600*16384,0,0,16384,20*16384};
 int32_t current[]={16384,0,0,40*16384,0,16384,0,-640*16384,0,0,16384,60*16384};
 int32_t p[3];firstperson::headPosition(previousBody,currentBody,previous,current,0,p);
 assert(p[0]==1000&&p[1]==-932&&p[2]==536);
 firstperson::headPosition(previousBody,currentBody,previous,current,256,p);
 assert(p[0]==1140&&p[1]==-1022&&p[2]==776);
 firstperson::headPosition(previousBody,currentBody,previous,current,128,p);
 assert(p[0]==1070&&p[1]==-977&&p[2]==656);
 assert(firstperson::interpolate(10,9,128)==10); // Engine truncates negative deltas toward zero.
 for(int i=0;i<1000;i++){
  float angle=float(i)*.013f;XrPosef origin{{0,std::sin(angle/2),0,std::cos(angle/2)},{3,2,-5}};
  XrView views[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
  XrVector3f midpoint{float(i)*.001f,1.6f,-.2f};
  XrVector3f baseline{.032f*std::cos(angle),0,.032f*std::sin(angle)};
  for(int e=0;e<2;e++){float sign=e?1.f:-1.f;views[e].pose={{0,0,0,1},{midpoint.x+sign*baseline.x,midpoint.y,midpoint.z+sign*baseline.z}};}
  auto left=firstperson::eyeOffset(views[0].pose,views,origin),right=firstperson::eyeOffset(views[1].pose,views,origin);
  assert(near(left.x+right.x,0)&&near(left.y+right.y,0)&&near(left.z+right.z,0));
  float dx=left.x-right.x,dy=left.y-right.y,dz=left.z-right.z;assert(near(std::sqrt(dx*dx+dy*dy+dz*dz),.064f));
  for(auto& v:views){v.pose.position.x+=2;v.pose.position.y-=1;v.pose.position.z+=4;}
  auto moved=firstperson::eyeOffset(views[0].pose,views,origin);
  assert(near(left.x,moved.x)&&near(left.y,moved.y)&&near(left.z,moved.z));
 }
 puts("PASS: animated head interpolation, engine rounding, translation cancellation and rotated 64 mm stereo baseline across 1000 poses.");
}
