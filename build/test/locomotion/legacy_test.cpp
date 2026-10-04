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
#include "C:/dev/TombRaider123VR/src/StereoMath.h"
#define private public
#include "C:/dev/TombRaider123VR/src/VRSystem.h"
#include "C:/dev/TombRaider123VR/src/InlineHook.h"
#undef private
#include "C:/dev/TombRaider123VR/build/test/locomotion/legacy_firstperson.cpp"

namespace tr {
Config testConfig;
VRSystem testVR;
int testWater=0;
const Config& Cfg() { return testConfig; }
VRSystem& VR() { return testVR; }
int LaraWaterStatus() { return testWater; }
int32_t AppFlag(uint32_t) { return 0; }
float LiveWorldUnitsPerMetre() { return 1000; }
float VRSystem::HeadYawRadians() const { return 0; }
void VRSystem::PivotHeadFloorOffset(float) {}
void VRSystem::HeadFloorOffset(float& x,float& z) const { x=z=0; }
// Unrelated first-person entry points must never be reached by these tests.
float VRSystem::HeadPitchRadians() const { std::abort(); }
void VRSystem::RecenterHead() { std::abort(); }
void VRSystem::FirstPersonViewOffset(float&,float&) const { std::abort(); }
float VRSystem::HeadVerticalOffset() const { std::abort(); }
void VRSystem::ConsumeHeadFloorOffset(float,float) { std::abort(); }
bool VRSystem::ControllerPose(int,vr::HmdMatrix34_t&) const { std::abort(); }
bool VRSystem::FirstPersonControllerOffset(int,float&,float&,float&) const { std::abort(); }
const GameDllLayout* GameDllBound() { std::abort(); }
uint64_t GameDllBase() { std::abort(); }
const motiongun::Calibration& LiveMotionGunCalibration() { std::abort(); }
void AdjustMotionGunCalibration(int) { std::abort(); }
void RestoreMotionGunCalibration() { std::abort(); }
bool SaveMotionGunCalibration() { std::abort(); }
}

namespace {
using namespace tr;
using namespace tr::locomotion;
alignas(16) uint8_t module[1024]{}, item[3664]{};
GameDllLayout dll{};
int checks=0, ticks=0, collisions=0;
int nextState=-1, nextGoal=-1;
bool startGravity=false, inheritSpeed=false, wall=false, fall=false;
int nativeSpeed=10;
Vec beforeCollision{};
template<class T> T& Field(size_t offset) { return *reinterpret_cast<T*>(item+offset); }
void Check(bool ok,const char* label) {
    ++checks;
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",label); std::exit(1); }
}
void __cdecl NativeAnimate(uint8_t* target) {
    Check(target==item,"native animation receives Lara");
    ++ticks;
    if (nextState>=0) Field<int16_t>(off::item_anim_state)=int16_t(nextState);
    if (nextGoal>=0) Field<int16_t>(off::item_goal_state)=int16_t(nextGoal);
    if (startGravity) Field<uint16_t>(off::item_flags)|=8;
    auto& speed=Field<int16_t>(off::item_speed);
    if (!inheritSpeed) speed=int16_t(nativeSpeed);
    auto& pos=Field<PHD_3DPOS>(off::item_pos);
    const float yaw=Radians(*Ptr<int16_t>(dll.lara+254));
    pos.x_pos+=int32_t(std::round(std::sin(yaw)*speed));
    pos.z_pos+=int32_t(std::round(std::cos(yaw)*speed));
    pos.y_pos+=7;
}
void __cdecl NativeAboveWater(uint8_t* target,void*) {
    const auto before=Field<PHD_3DPOS>(off::item_pos);
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
    dll={}; dll.laraItem=8; dll.camera=32; dll.analogInput=160;
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
    testWater=0;
    g_runtimeEnabled=g_active=g_haveHeading=g_haveManualInput=true;
    g_headingItem=item; g_heading.base=heading;
    g_manualLocal=stick; g_manualWorld=MovementWorld(stick,heading);
    g_shifted=g_jumpPressed=false;
    g_groundMoveAction=0; g_stabilizeRoot=g_hardStopRoot=false;
    g_rootMotion.Reset();
    g_hAnimateLara.m_trampoline=reinterpret_cast<void*>(&NativeAnimate);
    g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeAboveWater);
    ticks=collisions=0; nextState=nextGoal=-1;
    startGravity=inheritSpeed=wall=fall=false; nativeSpeed=10;
}
void Tick() {
    *Ptr<uint32_t>(dll.input)=0;
    Ptr<int16_t>(dll.analogInput)[0]=10000;
    Detour_LaraAboveWater(item,nullptr);
    Check(g_groundMoveAction==0 && !g_stabilizeRoot && !g_hardStopRoot,
          "movement scope ends after native collision");
}
}

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
    std::printf("PASS: %d locomotion hook checks\n",checks);
    return 0;
}
