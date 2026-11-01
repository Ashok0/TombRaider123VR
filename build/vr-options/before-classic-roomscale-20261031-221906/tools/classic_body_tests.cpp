#include "../src/ClassicBodyRender.h"
#include "../src/FirstPersonStabilization.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Pose {
    int32_t x_pos,y_pos,z_pos;
    int16_t x_rot,y_rot,z_rot,padding;
};
int main() {
    using namespace tr;
    int checks=0;
    auto check=[&](bool pass,const char* message) {
        ++checks;if(!pass) { std::printf("FAIL: %s\n",message);std::exit(1); }
    };
    stabilization::GroundEye standing;
    standing.valid=true;standing.local={0,-700,90};standing.bodyLocal={0,90};
    const Pose previous{12000,3000,-15000,1,2,3,4},current{12017,3020,-14983,5,6,7,8};
    const Pose savedPrevious=previous,savedCurrent=current;
    for(int artificial:{-90,0,90,180}) for(int physical:{-180,-90,0,90,180})
    for(int fraction:{0,64,128,192,256}) {
        const float base=artificial*3.14159265358979323846f/180;
        const float yaw=(artificial+physical)*3.14159265358979323846f/180;
        const locomotion::Vec view{.17f,-.08f},floor{.11f,-.04f};
        const auto eye=locomotion::Rotate({standing.local.x,standing.local.z},base);
        const auto offset=standing.BodyOffsetAtEye(base,yaw,view,floor,1000,eye);
        Pose a{},b{};
        check(firstperson::ClassicBodyPositions(previous,current,offset,a,b),"Classic turn offset is accepted");
        const float t=fraction/256.f;
        const locomotion::Vec nativeRoot{previous.x_pos+(current.x_pos-previous.x_pos)*t,
                                        previous.z_pos+(current.z_pos-previous.z_pos)*t};
        const locomotion::Vec renderedRoot{a.x_pos+(b.x_pos-a.x_pos)*t,a.z_pos+(b.z_pos-a.z_pos)*t};
        const auto neck=renderedRoot+locomotion::Rotate(standing.bodyLocal,yaw);
        const auto expected=nativeRoot+eye+locomotion::Rotate(view-floor,base)*1000;
        check(locomotion::Length(neck-expected)<.72f,"physical and stick turns keep the neck centered at every interpolation fraction");
        check(a.y_pos==previous.y_pos && b.y_pos==current.y_pos &&
              a.x_rot==previous.x_rot && a.y_rot==previous.y_rot && a.z_rot==previous.z_rot &&
              b.x_rot==current.x_rot && b.y_rot==current.y_rot && b.z_rot==current.z_rot,
              "render correction preserves height and native rotations");
        Pose a2{},b2{};firstperson::ClassicBodyPositions(previous,current,offset,a2,b2);
        check(std::memcmp(&a,&a2,sizeof a)==0 && std::memcmp(&b,&b2,sizeof b)==0,
              "stereo eyes and repeated draws do not accumulate the correction");
    }
    check(std::memcmp(&previous,&savedPrevious,sizeof previous)==0 && std::memcmp(&current,&savedCurrent,sizeof current)==0,
          "collision and gameplay position records remain unchanged");
    Pose a{},b{};
    check(firstperson::ClassicBodyPositions(previous,current,{},a,b) &&
          std::memcmp(&previous,&a,sizeof a)==0 && std::memcmp(&current,&b,sizeof b)==0,
          "zero offset leaves the native render origin unchanged");
    check(!firstperson::ClassicBodyPositions(previous,current,{NAN,0},a,b),"invalid tracking offset is rejected");
    check(!firstperson::ClassicBodyPositions(previous,current,{0,INFINITY},a,b),"infinite tracking offset is rejected");
    check(!firstperson::ClassicBodyPositions(previous,current,{4097,0},a,b),"discontinuous offset is rejected");
    Pose edge=current;edge.x_pos=INT32_MAX;
    check(!firstperson::ClassicBodyPositions(previous,edge,{1,0},a,b),"render position overflow is rejected");
    edge=current;edge.z_pos=INT32_MIN;
    check(!firstperson::ClassicBodyPositions(edge,current,{0,-1},a,b),"render position underflow is rejected");
    std::printf("PASS: %d Classic body render checks\n",checks);
}

