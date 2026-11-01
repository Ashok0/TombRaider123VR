// Focused production-hook regression only; no game DLL or full suite executes.
#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main
namespace {
void TransitionSetup(int state,int required,int status,bool modern=true,int game=3) {
    GunSetup(game,modern,true,status,0);
    testConfig.firstPersonFullBodyIK=testConfig.firstPersonUnarmedIK=true;
    testConfig.firstPersonPhysicalCrouch=testConfig.firstPersonMoveWithHead=true;
    Field<int16_t>(18)=Field<int16_t>(20)=int16_t(state);
    Field<int16_t>(22)=int16_t(required);Field<int16_t>(24)=258;
    g_hLaraGun.m_installed=false; // No weapon scope changes the native busy flag.
    g_heading.base=.4f;testHeadYaw=.7f;
    g_groundEye.Reset();g_renderTurn.Reset();g_mountRootHeight.Reset();g_mountBodyTransition.Reset();
    g_previousBody={};g_dragPrevious=g_dragCurrent=g_dragShown={};
    testViewRight=testViewForward=testViewRise=0;
    testControllerOffset[0]=testControllerOffset[1]={};
}
}
int main() {
    TransitionSetup(80,71,1);
    Check(CanSteerCrouch(item),"TR3 queued duck keeps crawl steering");
    Check(FullBodyIKReady() && UnarmedIKReady(),"TR3 native crawl busy flag keeps tracked hands");
    for(bool modern:{false,true}) for(int state:{71,80,81,84,85,86,89,90})
        for(int queued:{0,2,71,80,81,84,85,86,89,90}) for(int status:{0,1}) {
        TransitionSetup(state,queued,status,modern);
        Check(CanSteerCrouch(item) && FullBodyIKReady() && UnarmedIKReady(),"ordinary postures retain steering and IK");
        for(int hand:{0,1}) {
            motiongun::Frame wrist{};motiongun::Basis basis{};
            Check(BuildControllerWrist(hand,wrist,basis),"crawling wrist is available");
            const auto before=wrist.origin;testControllerOffset[hand].x+=.1f;
            Check(BuildControllerWrist(hand,wrist,basis) && std::fabs(firstperson::Length(Sub(wrist.origin,before))-100)<.01f,"both wrists follow controller movement while crawling");
        }
        float x=.6f,y=.8f,turn=.5f;FirstPersonInput(x,y,turn,false,false);
        const auto expected=Rotate(Vec{.6f,.8f},1.1f);
        Check(g_haveManualInput && turn==0 && Length(g_manualWorld-expected)<.001f,"crawl intent follows headset yaw in both control modes");
        nativeSpeed=state==86 ? -10 : 10;Tick();
        Check(std::fabs(beforeCollision.x-expected.x*10)<2 && std::fabs(beforeCollision.z-expected.z*10)<2,"production simulation sends forward and backward crawl toward headset-relative intent");
        Check(ticks==1 && collisions==1 && Field<PHD_3DPOS>(88).y_pos==7 && !g_crouchDrive,"native animation and collision run once and steering scope ends");
        Check(Field<int16_t>(22)==queued && *Ptr<int16_t>(dll.lara+2)==status,"simulation preserves queued posture and native hands-busy status");
        wall=true;const auto before=Field<PHD_3DPOS>(88);Tick();
        Check(Field<PHD_3DPOS>(88).x_pos==before.x_pos && Field<PHD_3DPOS>(88).z_pos==before.z_pos,"native wall collision remains authoritative");
        testViewRise=-.65f;Check(FirstPersonPhysicalDuck(),"low headset requests physical crouch through transition");
        testViewRise=0;Check(!FirstPersonPhysicalDuck(),"standing headset releases physical crouch request");
        Check(Field<int16_t>(22)==queued && *Ptr<int16_t>(dll.lara+2)==status,"physical posture leaves native ceiling and queued states intact");
    }
    for(int queued:{10,19,40,41,72,87,88}) {
        TransitionSetup(80,queued,1);
        Check(!CanSteerCrouch(item) && !UnarmedIKReady() && !FullBodyIKReady(),"required interaction/roll/hang retains native control");
        TransitionSetup(80,0,1);Field<int16_t>(20)=int16_t(queued);
        Check(!CanSteerCrouch(item) && !UnarmedIKReady() && !FullBodyIKReady(),"interaction/roll/hang goal retains native hands");
    }
    for(int guard=0;guard<5;++guard) {
        TransitionSetup(80,71,1);
        if(guard==0) *Ptr<uint16_t>(dll.lara+60)|=0x2000;
        if(guard==1) Field<uint16_t>(off::item_flags)|=8;
        if(guard==2) Field<int16_t>(off::item_hit_points)=0;
        if(guard==3) testWater=1;
        if(guard==4) *Ptr<int16_t>(dll.lara+40)=0;
        Check(!CanSteerCrouch(item) && !UnarmedIKReady() && !FullBodyIKReady(),"alignment, gravity, death, water and vehicles retain guards");
    }
    for(int status:{1,2,3,4}) {
        TransitionSetup(2,0,status);Check(!UnarmedIKReady(),"standing busy/equip states do not gain unarmed hands");
    }
    TransitionSetup(80,71,1);testMissingHand=0;Check(!UnarmedIKReady(),"missing controller retains tracking guard");
    TransitionSetup(80,71,1);testConfig.firstPersonUnarmedIK=false;Check(!UnarmedIKReady(),"unarmed hands respect INI switch");
    TransitionSetup(80,71,1);g_active=false;
    float x=0,y=1,turn=0;FirstPersonInput(x,y,turn,false,false);
    Check(!g_haveManualInput && !UnarmedIKReady() && !FullBodyIKReady() && !FirstPersonPhysicalDuck(),"third person keeps native posture and input");
    for(int game:{1,2}) {
        for(int state:{71,80,81,84,85,86,89,90}) {
            TransitionSetup(state,71,1,true,game);
            Check(!CanUseCrouchCamera(item) && !CanSteerCrouch(item) && !UnarmedIKReady(),"TR3-only posture IDs never enable crawl overrides in TR1/2");
        }
        TransitionSetup(2,0,0,true,game);
        Check(UnarmedIKReady() && FullBodyIKReady(),"TR1/2 standing tracked hands remain available");
        testViewRise=-.65f;
        Check(!FirstPersonPhysicalDuck() && HidePhysicalCrouchBody(item) && !PhysicalCrouchEligible(item),"TR1/2 physical squat still hides body without native crawl");
        testViewRise=0;
        Check(!FirstPersonPhysicalDuck() && !HidePhysicalCrouchBody(item),"TR1/2 standing restores physical body");
    }
    std::printf("PASS: %d focused crouch-transition checks (TR3 fix, TR1/2 guards)\n",checks);
}
