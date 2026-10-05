// Production movement hooks with synthetic engine memory and native callbacks.
// Run tests\build_locomotion_selftest.cmd; no game or headset is required.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <openvr.h>
#include "../src/StereoMath.h"
#define private public
#include "../src/VRSystem.h"
#include "../src/InlineHook.h"
#undef private
#include "../src/FirstPerson.cpp"

namespace tr {
Config testConfig;
VRSystem testVR;
int testWater=0;
int testConfigFlags=0;
bool testPoseAvailable=true;
float testViewRight=0, testViewForward=0, testViewRise=0, testHeadPitch=0;
int testPivotCount=0;
float testPivotTurn=0;
const Config& Cfg() { return testConfig; }
VRSystem& VR() { return testVR; }
int LaraWaterStatus() { return testWater; }
int32_t AppFlag(uint32_t offset) { return offset==drva::app_off::cfg_flags ? testConfigFlags : 0; }
float LiveWorldUnitsPerMetre() { return 1000; }
float VRSystem::HeadYawRadians() const { return 0; }
void VRSystem::PivotHeadFloorOffset(float turn) { ++testPivotCount; testPivotTurn=turn; }
void VRSystem::HeadFloorOffset(float& x,float& z) const { x=z=0; }
// Unrelated first-person entry points must never be reached by these tests.
float VRSystem::HeadPitchRadians() const { return testHeadPitch; }
void VRSystem::RecenterHead() { std::abort(); }
void VRSystem::FirstPersonViewOffset(float& x,float& z) const { x=testViewRight; z=testViewForward; }
float VRSystem::HeadVerticalOffset() const { return testViewRise; }
void VRSystem::ConsumeHeadFloorOffset(float,float) { std::abort(); }
bool VRSystem::ControllerPose(int,vr::HmdMatrix34_t& pose) const {
    pose={}; pose.m[0][0]=pose.m[1][1]=pose.m[2][2]=1; return testPoseAvailable;
}
bool VRSystem::FirstPersonControllerOffset(int,float& x,float& y,float& z) const {
    x=y=z=0; return testPoseAvailable;
}
const GameDllLayout* GameDllBound() { return g_boundDll; }
uint64_t GameDllBase() { return g_boundBase; }
const motiongun::Calibration& LiveMotionGunCalibration() { static motiongun::Calibration fit{}; return fit; }
void AdjustMotionGunCalibration(int) { std::abort(); }
void RestoreMotionGunCalibration() { std::abort(); }
bool SaveMotionGunCalibration() { std::abort(); }
}

namespace {
using namespace tr;
using namespace tr::locomotion;
alignas(16) uint8_t module[8192]{}, item[3664]{};
GameDllLayout dll{};
int checks=0, ticks=0, collisions=0;
int nextState=-1, nextGoal=-1, nextAnimation=-1;
bool nativeHalfTurn=false;
bool startGravity=false, inheritSpeed=false, wall=false, fall=false;
int nativeSpeed=10;
bool useAnimationTable=false, floorAllowsEntry=true;
uint32_t extraInput=0;
// Runtime layout independently transcribed from the three PDBs.
struct TestAnim {
    void* framePtr=nullptr;
    int16_t interpolation=1, state=0;
    int32_t velocity=0, acceleration=0;
    int16_t first=0,last=0,jump=0,jumpFrame=0,changes=0,changeIndex=0,commands=0,commandIndex=0;
};
static_assert(sizeof(TestAnim)==40);
TestAnim animationTable[104]{};
Vec beforeCollision{};
template<class T> T& Field(size_t offset) { return *reinterpret_cast<T*>(item+offset); }
void Check(bool ok,const char* label) {
    ++checks;
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",label); std::exit(1); }
}
void __cdecl NativeAnimate(uint8_t* target) {
    Check(target==item,"native animation receives Lara");
    ++ticks;
    if (useAnimationTable) {
        auto& animation=Field<int16_t>(24);
        auto& frame=Field<int16_t>(26);
        ++frame;
        // GetChange runs before the end-of-animation jump, even for TR1/2's
        // one-frame standing entry. Real PDP ranges are verified separately.
        if (animation==11 && floorAllowsEntry) {
            const int goal=Field<int16_t>(20);
            const int destination=goal==16 ? 41 : goal==21 ? 67 : goal==22 ? 65 : -1;
            if (destination>=0) { animation=int16_t(destination); frame=animationTable[destination].first; }
        }
        if (frame>animationTable[animation].last) {
            const auto previous=animationTable[animation];
            animation=previous.jump; frame=previous.jumpFrame;
        }
        const auto& anim=animationTable[animation];
        Field<int16_t>(18)=anim.state;
        nativeSpeed=(anim.velocity+anim.acceleration*(frame-anim.first))>>16;
    }
    if (nextAnimation>=0) Field<int16_t>(off::item_anim_number)=int16_t(nextAnimation);
    if (nextState>=0) Field<int16_t>(off::item_anim_state)=int16_t(nextState);
    if (nextGoal>=0) Field<int16_t>(off::item_goal_state)=int16_t(nextGoal);
    if (startGravity) Field<uint16_t>(off::item_flags)|=8;
    auto& speed=Field<int16_t>(off::item_speed);
    if (!inheritSpeed) speed=int16_t(nativeSpeed);
    auto& pos=Field<PHD_3DPOS>(off::item_pos);
    if (nativeHalfTurn) { pos.y_rot=int16_t(uint16_t(pos.y_rot)+0x8000u); nativeHalfTurn=false; }
    const float yaw=Radians(*Ptr<int16_t>(dll.lara+254));
    pos.x_pos+=int32_t(std::round(std::sin(yaw)*speed));
    pos.z_pos+=int32_t(std::round(std::cos(yaw)*speed));
    pos.y_pos+=7;
}
void __cdecl NativeAboveWater(uint8_t* target,void*) {
    const auto before=Field<PHD_3DPOS>(off::item_pos);
    if (useAnimationTable && Field<int16_t>(18)==2) {
        const auto action=*Ptr<uint32_t>(dll.input)&Directions;
        if (floorAllowsEntry)
            Field<int16_t>(20)=action==Back ? 16 : action==StepRight ? 21 : action==StepLeft ? 22 : 2;
    }
    Detour_AnimateLara(target);
    auto& pos=Field<PHD_3DPOS>(off::item_pos);
    beforeCollision={float(pos.x_pos-before.x_pos),float(pos.z_pos-before.z_pos)};
    ++collisions;
    if (wall) { pos.x_pos=before.x_pos; pos.z_pos=before.z_pos; }
    if (fall) {
        Field<uint16_t>(off::item_flags)|=8;
        Field<int16_t>(off::item_anim_state)=3;
        Field<int16_t>(off::item_goal_state)=3;
    }
}
void Reset(int state,Vec stick,bool smooth,float heading=0) {
    std::memset(item,0,sizeof(item));
    std::memset(module,0,sizeof(module));
    dll={}; dll.module=L"tomb1.dll"; dll.laraItem=8; dll.camera=32; dll.analogInput=160;
    dll.input=176; dll.lara=256;
    g_boundBase=reinterpret_cast<uint64_t>(module); g_boundDll=&dll;
    *Ptr<uint8_t*>(dll.laraItem)=item;
    Field<int16_t>(off::item_hit_points)=1000;
    Field<int16_t>(off::item_anim_state)=int16_t(state);
    Field<int16_t>(off::item_goal_state)=int16_t(state);
    Field<int16_t>(off::item_speed)=10;
    testConfig=Config{};
    testConfig.enabled=true;
    testConfig.firstPersonMovementStabilization=smooth;
    testConfig.firstPersonRoomscaleMove=false;
    testConfig.firstPersonDriftLog=false;
    testVR.m_system=reinterpret_cast<vr::IVRSystem*>(1);
    testVR.m_poseValid=true;
    testWater=0; testConfigFlags=0; testPoseAvailable=true;
    g_runtimeEnabled=g_active=g_haveHeading=g_haveManualInput=true;
    g_headingItem=item; g_heading.base=heading;
    g_manualLocal=stick; g_manualWorld=MovementWorld(stick,heading);
    g_shifted=g_jumpPressed=false;
    g_groundMoveAction=0; g_stabilizeRoot=g_hardStopRoot=false;
    g_rootMotion.Reset();
    g_hAnimateLara.m_trampoline=reinterpret_cast<void*>(&NativeAnimate);
    g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeAboveWater);
    ticks=collisions=0; nextState=nextGoal=nextAnimation=-1;
    nativeHalfTurn=false; testPivotCount=0; testPivotTurn=0;
    startGravity=inheritSpeed=wall=fall=false; nativeSpeed=10;
    useAnimationTable=false; floorAllowsEntry=true; extraInput=0;
}
void EnableAnimations(int game) {
    useAnimationTable=true;
    dll.anims=192;
    *Ptr<TestAnim*>(dll.anims)=animationTable;
    for (auto& anim:animationTable) anim=TestAnim{};
    const auto set=[](int index,int state,int first,int last,int speed,int jump,int jumpFrame) {
        auto& anim=animationTable[index];
        anim.state=int16_t(state); anim.first=int16_t(first); anim.last=int16_t(last);
        anim.velocity=speed<<16; anim.jump=int16_t(jump); anim.jumpFrame=int16_t(jumpFrame);
    };
    const int start=game==2 ? 597 : 598, loop=start-60;
    const int idleLoop=game==1 ? 1636 : game==2 ? 1608 : 1609;
    set(11,2,185,game==3 ? 186 : 185,0,103,idleLoop);
    set(103,2,idleLoop,idleLoop+43,0,103,idleLoop);
    set(40,16,loop,loop+59,10,40,loop);
    set(41,16,start,start+15,2,40,loop);
    set(65,22,1050+game,1074+game,9,65,1050+game);
    set(67,21,1080+game,1104+game,9,67,1080+game);
}
void Tick() {
    *Ptr<uint32_t>(dll.input)=extraInput;
    Ptr<int16_t>(dll.analogInput)[0]=10000;
    Detour_LaraAboveWater(item,nullptr);
    Check(g_groundMoveAction==0 && !g_stabilizeRoot && !g_hardStopRoot,
          "movement scope ends after native collision");
}
}

void TestResponsiveEntry() {
    const struct { int state,animation; } ordinary[]={
        {1,0},{1,6},{1,8},{1,10},{0,1},{0,2},{0,3},{0,4},{0,5},
        {0,7},{0,9},{0,20},{0,21},{2,11},{2,103},{16,38},{16,39},
        {16,40},{16,41},{22,65},{22,66},{21,67},{21,68}};
    const struct { Vec stick; int state,animation,speed; } moves[]={
        {{-1,0},22,65,9},{{1,0},21,67,9},{{0,-1},16,41,2}};
    for (int game:{1,2,3}) for (bool smooth:{false,true}) for (float heading:{0.f,.7f,2.8f}) {
        for (const auto& move:moves) for (const auto& from:ordinary) {
            Reset(from.state,move.stick,smooth,heading); EnableAnimations(game);
            Field<int16_t>(24)=int16_t(from.animation);
            Field<int16_t>(26)=12; // Outgoing frame must be replaced, not reused.
            Field<int16_t>(20)=2; // Include release/re-press during a pending stop.
            Field<int16_t>(34)=47;
            g_rootMotion.valid=true; g_rootMotion.speed=141;
            Tick();
            Check(Field<int16_t>(18)==move.state && Field<int16_t>(24)==move.animation,
                  "side/back changes enter their own gait on the first tick");
            Check(Field<int16_t>(34)==move.speed &&
                  std::fabs(Length(beforeCollision)-3*move.speed)<3,
                  "entry uses the destination gait speed without outgoing run momentum");
            Check(Field<int16_t>(26)==animationTable[move.animation].first,
                  "entry frame comes from the current level animation table");
            Check(ticks==1 && collisions==1 && Field<PHD_3DPOS>(88).y_pos==7,
                  "instant entry advances animation and collision only once");
            const auto frame=Field<int16_t>(26);
            Tick();
            Check(Field<int16_t>(26)==frame+(move.state==16 ? 4 : 1),
                  "holding a gait continues its animation instead of restarting it");
            const auto before=Field<PHD_3DPOS>(88); wall=true; Tick();
            Check(Field<PHD_3DPOS>(88).x_pos==before.x_pos && Field<PHD_3DPOS>(88).z_pos==before.z_pos,
                  "instant entry retains native wall collision");
        }
        // Standing control must still be allowed to refuse entry at a ledge.
        for (const auto& move:moves) {
            Reset(1,move.stick,smooth,heading); EnableAnimations(game);
            floorAllowsEntry=false; Tick();
            Check(Field<int16_t>(18)==2 && Length(beforeCollision)==0,
                  "native floor/ceiling checks can reject immediate gait entry");
        }
        Reset(2,{0,-1},smooth,heading); EnableAnimations(game);
        Field<int16_t>(24)=11; Field<int16_t>(26)=185;
        Tick(); // Native standing -> backward start at its first frame.
        const int first=animationTable[41].first;
        for (int step=1;step<=4;++step) {
            Tick();
            if (step<4)
                Check(Field<int16_t>(24)==41 && Field<int16_t>(26)==first+4*step,
                      "backpedal startup advances four frames per tick");
        }
        Check(Field<int16_t>(24)==40 && Field<int16_t>(26)==animationTable[40].first && Field<int16_t>(34)==10,
              "backpedal reaches the normal loop after four startup ticks");
        for (int step=1;step<=20;++step) {
            Tick();
            Check(Field<int16_t>(26)==animationTable[40].first+step && Field<int16_t>(34)==10 && Length(beforeCollision)<33,
                  "backpedal loop cadence and maximum speed remain native");
        }
        Reset(16,{},smooth,heading); EnableAnimations(game);
        Field<int16_t>(24)=41; Field<int16_t>(26)=int16_t(first);
        Tick();
        Check(Field<int16_t>(26)==first+1 && Length(beforeCollision)==0,
              "release during backpedal startup stops movement without fast-forwarding");
    }
    // Invalid/unavailable data and special poses must keep the safe handoff.
    for (int excluded=0;excluded<18;++excluded) {
        Reset(1,{-1,0},true); EnableAnimations(1);
        Field<int16_t>(24)=0; Field<int16_t>(26)=7;
        if (excluded==0) Field<int16_t>(22)=19;
        if (excluded==1) Field<uint16_t>(484)=8;
        if (excluded==2) Field<int16_t>(20)=15;
        if (excluded==3) { Field<int16_t>(18)=2; Field<int16_t>(24)=24; } // hard landing
        if (excluded==4) { Field<int16_t>(18)=0; Field<int16_t>(24)=12; } // step up
        if (excluded==5) { Field<int16_t>(18)=19; Field<int16_t>(24)=42; }
        if (excluded==6) dll.anims=0;
        if (excluded==7) *Ptr<TestAnim*>(dll.anims)=nullptr;
        if (excluded==8) animationTable[11].state=3;
        if (excluded==9) animationTable[11].first=-1;
        if (excluded==10) animationTable[11].last=184;
        if (excluded==11) animationTable[11].commands=1;
        if (excluded>=12 && excluded<=15) *Ptr<uint32_t>(dll.input)=((excluded==12) ? 0x10u : (excluded==13) ? 0x40u : (excluded==14) ? 0x100u : 0x1000u);
        if (excluded==16) testWater=1;
        if (excluded==17) Field<int16_t>(38)=0;
        uint8_t before[sizeof(item)]; std::memcpy(before,item,sizeof(item));
        PrepareGroundDirection(item,StepLeft);
        Check(std::memcmp(before,item,sizeof(item))==0,
              "special poses, pending actions and invalid tables retain native timing");
    }
    // Do not skip frames in another animation or one with animation commands.
    for (int excluded=0;excluded<10;++excluded) {
        Reset(16,{0,-1},true); EnableAnimations(3);
        Field<int16_t>(24)=41; Field<int16_t>(26)=598;
        if (excluded==0) Field<int16_t>(22)=19;
        if (excluded==1) Field<int16_t>(20)=2;
        if (excluded==2) Field<int16_t>(24)=40;
        if (excluded==3) animationTable[41].commands=1;
        if (excluded==4) animationTable[41].state=2;
        if (excluded==5) Field<int16_t>(26)=597;
        if (excluded==6) Field<int16_t>(26)=614;
        if (excluded==7) dll.anims=0;
        if (excluded==8) *Ptr<TestAnim*>(dll.anims)=nullptr;
        const auto frame=Field<int16_t>(26);
        AccelerateBackpedalStart(item,excluded==9 ? StepLeft : Back|Walk);
        Check(Field<int16_t>(26)==frame,"backpedal fast startup excludes pending actions and invalid animations");
    }
}

#include "gun_input_selftest.h"
#include "camera_clearance_selftest.h"
#include "jump_roll_selftest.h"
#include "arm_visibility_selftest.h"
#include "block_camera_selftest.h"

int main() {
    const struct { Vec stick; int gait; } directions[]={
        {{0,1},1}, {{0,-1},16}, {{1,0},21}, {{-1,0},22}};
    for (bool smooth:{false,true}) for (float heading:{0.f,.7f,2.8f}) {
        // Input changes immediately, but the outgoing animation can persist
        // for several ticks before passing through stop into the new gait.
        for (const auto& from:directions) for (const auto& to:directions) {
            if (from.gait==to.gait) continue;
            Reset(from.gait,to.stick,smooth,heading);
            nativeSpeed=47;
            for (int frame=0;frame<4;++frame) {
                if (frame==2) nextState=2;
                Tick();
                Check(Length(beforeCollision)==0,"outgoing gait cannot move in the new direction");
                Check(!g_rootMotion.valid,"handoff discards outgoing speed history");
            }
            nextState=nextGoal=to.gait; nativeSpeed=10;
            Tick();
            const float expected=to.gait==1 ? 10.f : 30.f;
            Check(std::fabs(Length(beforeCollision)-expected)<3,
                  "matching gait moves on its first tick using its own speed");
            Check(Field<int16_t>(off::item_speed)==10,"stored native speed is never boosted");
            Check(ticks==5 && collisions==5 && Field<PHD_3DPOS>(off::item_pos).y_pos==35,
                  "animation, vertical movement and collision advance once per tick");
            wall=true; const auto before=Field<PHD_3DPOS>(off::item_pos); Tick();
            Check(Field<PHD_3DPOS>(off::item_pos).x_pos==before.x_pos &&
                  Field<PHD_3DPOS>(off::item_pos).z_pos==before.z_pos,
                  "root correction cannot undo native wall collision");
        }
        for (const auto& direction:directions) {
            Reset(direction.gait,direction.stick,smooth,heading);
            for (int frame=0;frame<120;++frame) {
                Tick();
                Check(Field<int16_t>(off::item_speed)==10 && Length(beforeCollision)<33,
                      "sustained movement cannot accumulate speed");
            }
            fall=true; Tick(); fall=false; inheritSpeed=true;
            for (int frame=0;frame<30;++frame) {
                Tick();
                Check(Field<int16_t>(off::item_speed)==10 && Length(beforeCollision)<12,
                      "ledge fall inherits only the unmodified native speed");
            }
            // Guard both sides of animation, including gravity set before
            // current state catches up and authored large relocations.
            for (int transition=0;transition<9;++transition) {
                Reset(direction.gait,direction.stick,smooth,heading);
                if (transition==0) Field<uint16_t>(off::item_flags)=8;
                if (transition==1) startGravity=true;
                if (transition==2) nextState=3;
                if (transition==3) nextGoal=15;
                if (transition==4) Field<int16_t>(off::item_goal_state)=56;
                if (transition==5) nextState=19;
                if (transition==6) nativeSpeed=600;
                if (transition==7) { Field<uint16_t>(off::item_flags)=8; inheritSpeed=true; }
                if (transition==8) testWater=1;
                Tick();
                Check(std::fabs(Length(beforeCollision)-nativeSpeed)<2 &&
                      Field<int16_t>(off::item_speed)==nativeSpeed,
                      "airborne, interaction and relocation transitions preserve native motion");
                Check(!g_rootMotion.valid,"excluded movement clears smoothing history");
            }
            Reset(direction.gait,{},smooth,heading); Tick();
            Check(Length(beforeCollision)==0 && Field<int16_t>(off::item_speed)==0,
                  "stick release still brakes ordinary stopping animations");
            Reset(direction.gait,direction.stick,smooth,heading);
            g_jumpPressed=true; Tick();
            Check(Length(beforeCollision)<12 && Field<int16_t>(off::item_speed)==10,
                  "preparing a jump bypasses ground scaling and handoff braking");
            Reset(direction.gait,direction.stick,smooth,heading);
            g_shifted=true; Tick();
            Check(Length(beforeCollision)<12 && Field<int16_t>(off::item_speed)==10,
                  "shifted controls retain native motion");
            if (direction.gait!=1) {
                Reset(direction.gait,direction.stick,smooth,heading);
                nativeSpeed=100; Tick();
                Check(std::fabs(Length(beforeCollision)-100)<2 && !g_rootMotion.valid,
                      "scaled displacement is checked before applying the multiplier");
            }
        }
    }
    TestResponsiveEntry();
    TestGunControls();
    TestCameraClearance();
    TestJumpRoll();
    TestArmVisibility();
    TestBlockCamera();
    std::printf("PASS: %d locomotion, gun-control and camera hook checks\n",checks);
    return 0;
}
