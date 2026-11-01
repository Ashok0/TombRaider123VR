// Focused actual controller/readiness integration; the full suite is unused.
#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main
int main(){
    using namespace tr;
    FlareThrowDll flare{};flare.items=640;
    auto setup=[&](int game){
        Reset(2,{0,0},true);dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
        testConfigFlags=1;testFlareReady=true;g_scenePoseValid=true;
        testConfig.firstPersonUnarmedIK=testConfig.firstPersonMotionGuns=true;
        testConfig.positionalTracking=testConfig.firstPersonHeadTranslation=true;
        g_hGetJoints.m_installed=g_hDrawCreatureHD.m_installed=g_hFireWeapon.m_installed=true;
        g_hGetTargetOnLOS.m_installed=g_hAnimatePistols.m_installed=true;
        dll.nextItemActive=dll.getSpheres=dll.findTargetPoint=dll.los=1;
        *Ptr<int16_t>(dll.lara+4)=game==2?8:9;
        *Ptr<uint8_t*>(640)=item;g_flareThrowDll=&flare;g_flareGesture={};g_flareRelease={};
        testHands[0]={};testHands[1]={};testHands[0].valid=testHands[1].valid=true;
    };
    setup(1);Check(!FirstPersonFlareGrip(true,true),"TR1 retains native controls; no flares");
    for(int game:{2,3}){
        setup(game);Check(FlareIKReady(),"actual TR2/3 tracked-flare eligibility");
        XState out{};bool shifted=false,action=false;testHands[0].grip=1;
        BuildState(out,shifted,true,action);
        Check(g_flareGesture.held && !(out.Gamepad.wButtons&XB_LEFT_SHOULDER),"flare grip suppresses Duck");
        testHands[1].grip=1;BuildState(out,shifted,true,action);
        Check(action && !g_flareGesture.held && !g_flareRelease.time,"two grips retain Action and cancel flare gesture");
        testHands[0].grip=testHands[1].grip=0;BuildState(out,shifted,true,action);
        Check(!g_flareRelease.time,"Action release cannot throw");
        for(int exclusion=0;exclusion<8;++exclusion){
            setup(game);
            switch(exclusion){case 0:g_active=false;break;case 1:testConfigFlags=0;break;
            case 2:testMissingHand=0;break;case 3:*Ptr<int16_t>(dll.lara+2)=3;break;
            case 4:*Ptr<int16_t>(dll.lara+4)=0;break;case 5:testWater=1;break;
            case 6:Field<int16_t>(18)=56;break;case 7:testConfig.firstPersonMotionGuns=false;break;}
            Check(!FirstPersonFlareGrip(true,true),"native controls retained outside physical flare eligibility");
        }
        for(int chord=0;chord<5;++chord){
            setup(game);testHands[0].grip=1;BuildState(out,shifted,true,action);
            switch(chord){case 0:testHands[0].btnUpper=true;break;case 1:testHands[0].btnLower=true;break;
            case 2:testHands[0].trigger=1;break;case 3:testHands[1].trigger=1;break;
            case 4:testHands[0].stickClick=testHands[1].stickClick=true;break;}
            BuildState(out,shifted,true,action);
            Check(!g_flareGesture.held && !g_flareRelease.time,"menu/equip/photo chord cancels prepared throw");
        }
    }
    g_hGetJoints.m_installed=g_hDrawCreatureHD.m_installed=g_hFireWeapon.m_installed=false;
    g_hGetTargetOnLOS.m_installed=g_hAnimatePistols.m_installed=false;
    std::printf("PASS: %d TR1-3 flare integration checks\n",checks);
}
