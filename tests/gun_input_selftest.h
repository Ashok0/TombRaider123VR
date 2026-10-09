// Included by locomotion_selftest.cpp to reuse the synthetic engine harness.
// Exercises the real input adapter, LaraGun injection and per-hand FireWeapon.
namespace {
int nativeGunShots=0, nativeGunHand=-1, nativeGunResult=1;
void __cdecl NativeGunControl() {
    auto& status=*tr::Ptr<int16_t>(dll.lara+2);
    const bool draw=(*tr::Ptr<uint32_t>(dll.input)&0x20)!=0;
    const bool hold=tr::NewControls();
    if (status==0 && draw) status=2;
    else if (status==4 && (hold ? !draw : draw)) status=3;
}
int32_t __cdecl NativeGunShot(int32_t,void*,void*,const int16_t*) {
    ++nativeGunShots; nativeGunHand=tr::g_firingHand; return nativeGunResult;
}
void SetMotionHooks(bool installed) {
    using namespace tr;
    for (auto* hook:{&g_hGetJoints,&g_hDrawCreatureHD,&g_hFireWeapon,
                     &g_hGetTargetOnLOS,&g_hAnimatePistols}) hook->m_installed=installed;
}
void GunSetup(int game,bool hold,bool motion=true,int status=0,int weapon=1) {
    using namespace tr;
    Reset(2,{},true);
    dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
    dll.frameFrac=200; dll.nextItemActive=210;
    dll.getSpheres=dll.findTargetPoint=dll.los=1;
    dll.rightFireReturn=0x111; dll.leftFireReturn=0x222;
    *Ptr<int16_t>(dll.lara+2)=int16_t(status);
    *Ptr<int16_t>(dll.lara+4)=int16_t(weapon);
    *Ptr<int16_t>(dll.lara+8)=int16_t(weapon);
    testConfigFlags=1|(hold ? 2 : 0);
    testConfig.firstPersonMotionGuns=motion;
    testConfig.positionalTracking=testConfig.firstPersonHeadTranslation=true;
    g_scenePoseValid=true; g_scenePose={};
    SetMotionHooks(true);
    g_hLaraGun.m_trampoline=reinterpret_cast<void*>(&NativeGunControl);
    g_hFireWeapon.m_trampoline=reinterpret_cast<void*>(&NativeGunShot);
    g_gunTriggers.Reset(); g_gunEquip.Reset(); g_triggerWeapon=0;
    g_nativeEquipRequested=false; g_nativeEquipStatus=-1;
    nativeGunShots=0; nativeGunHand=-1; nativeGunResult=1;
    testHapticShots[0]=testHapticShots[1]=0;
}
void NativeGunTick() {
    *tr::Ptr<uint32_t>(dll.input)=0;
    tr::Detour_LaraGun();
    Check(*tr::Ptr<uint32_t>(dll.input)==0,"injected draw bit is restored after native LaraGun");
}
bool PollGuns(uint8_t& left,uint8_t& right,bool y=false,bool chord=false) {
    return tr::FirstPersonGunTriggers(left,right,chord,y);
}
int FireHand(int hand,int weapon=1) {
    const int16_t aim[2]={};
    return tr::FireWeaponForCaller(hand ? dll.rightFireReturn : dll.leftFireReturn,
                                  weapon,nullptr,nullptr,aim);
}
void TestGunControls() {
    using namespace tr;
    for (int game:{1,2,3}) for (bool hold:{false,true}) for (bool motion:{false,true}) {
        GunSetup(game,hold,motion);
        uint8_t lt=255,rt=0;
        Check(!PollGuns(lt,rt) && lt==0 && g_nativeEquipRequested,
              "first LT press requests draw immediately without a hold threshold");
        lt=rt=0; PollGuns(lt,rt); // Release before any simulation acknowledges it.
        Check(g_nativeEquipRequested,"brief LT tap persists until native draw acknowledgement");
        NativeGunTick();
        Check(*Ptr<int16_t>(dll.lara+2)==2,"native LaraGun receives one equip request");
        *Ptr<int16_t>(dll.lara+2)=4;
        lt=rt=0; PollGuns(lt,rt); NativeGunTick();
        Check(*Ptr<int16_t>(dll.lara+2)==4,"guns stay drawn after LT release in both native control styles");
        for (int poll=0;poll<8;++poll) {
            lt=255;rt=0; PollGuns(lt,rt); NativeGunTick();
            Check(*Ptr<int16_t>(dll.lara+2)==4,"repeated and held LT never disarms ready guns");
        }
        lt=rt=0;
        Check(PollGuns(lt,rt,true),"Y is consumed to holster while armed");
        NativeGunTick();
        Check(*Ptr<int16_t>(dll.lara+2)==3 && !g_gunTriggers.WantsShot(),
              "Y begins native holstering and cancels queued firing");
        *Ptr<int16_t>(dll.lara+2)=0;
        Check(PollGuns(lt,rt,true),"holding Y through holstering cannot leak native Action");
        PollGuns(lt,rt,false);
        Check(!PollGuns(lt,rt,true),"fresh Y retains native Action while unarmed");

        GunSetup(game,hold,motion);
        lt=255;rt=0; PollGuns(lt,rt); NativeGunTick();
        *Ptr<int16_t>(dll.lara+2)=4;
        for (int poll=0;poll<4;++poll) {
            lt=255;rt=0; PollGuns(lt,rt);
            Check(!g_gunTriggers.WantsShot(),"the LT press used to equip cannot fire after drawing completes");
        }
        lt=rt=0; PollGuns(lt,rt);
        lt=255; PollGuns(lt,rt);
        Check(!motion || g_gunTriggers.pending[0],"fresh armed LT press fires without waiting for release");

        GunSetup(game,hold,motion,2);
        lt=rt=0; Check(PollGuns(lt,rt,true),"Y during draw queues holster");
        *Ptr<int16_t>(dll.lara+2)=4;
        Check(PollGuns(lt,rt,true),"Y remains consumed until release");
        NativeGunTick();
        Check(*Ptr<int16_t>(dll.lara+2)==3,"draw completes into requested holster");

        GunSetup(game,hold,motion,4);
        lt=150;rt=170; Check(!PollGuns(lt,rt,false,true) && lt==150 && rt==170 &&
            !g_nativeEquipRequested && !g_gunTriggers.WantsShot(),"view/graphics chords clear queued gun actions");
        g_active=false; lt=140;rt=180;
        Check(!PollGuns(lt,rt,true) && lt==140 && rt==180,"third-person controls pass through unchanged");
        g_active=true;testWater=1;lt=140;rt=180;
        Check(!PollGuns(lt,rt,true) && lt==140 && rt==180,"suspended first-person camera preserves native input");
    }
    for (int game:{1,2,3}) for (bool hold:{false,true}) for (int weapon:{1,2,3}) {
        GunSetup(game,hold,true,4,weapon);
        const bool dual=DualMotionWeapon(weapon);
        for (int hand=0;hand<2;++hand) {
            if (!dual && hand==0) continue;
            uint8_t lt=0,rt=0; PollGuns(lt,rt);
            lt=hand==0 ? 255 : 0;rt=hand==1 ? 255 : 0; PollGuns(lt,rt);
            // TR3 Desert Eagle uses the native left-arm call for the right gun.
            const int callerHand=dual ? hand : 0;
            const int before=nativeGunShots;
            const int beforeHaptics=testHapticShots[hand];
            Check(FireHand(callerHand,weapon)==1 && nativeGunHand==hand,
                  "trigger press fires its own native gun on the first eligible frame");
            Check(testHapticShots[hand]==beforeHaptics+1,"confirmed shot rumbles only its native firing hand");
            Check(FireHand(callerHand,weapon)==0,"repeated native call cannot duplicate a consumed press");
            Check(testHapticShots[hand]==beforeHaptics+1,"suppressed duplicate shot cannot duplicate rumble");
            lt=hand==0 ? 255 : 0;rt=hand==1 ? 255 : 0; PollGuns(lt,rt); // Recoil poll.
            lt=rt=0; PollGuns(lt,rt);
            Check(FireHand(callerHand,weapon)==0 && nativeGunShots==before+1,
                  "LT/RT release discards unused recoil repeat instead of firing twice");
            for (int tick=0;tick<100;++tick) {
                lt=hand==0 ? 255 : 0;rt=hand==1 ? 255 : 0; PollGuns(lt,rt);
                Check(FireHand(callerHand,weapon)==1,"either held trigger renews firing at native cadence");
            }
            Check(testHapticShots[hand]==beforeHaptics+101,"held fire produces rumble for every native shot");
            lt=rt=0; PollGuns(lt,rt);
            Check(FireHand(callerHand,weapon)==0,"release stops held auto-fire");
        }
        if (dual) {
            uint8_t lt=255,rt=255; PollGuns(lt,rt);
            Check(FireHand(0,weapon)==1 && FireHand(1,weapon)==1,"both triggers fire each hand independently");
        }
        uint8_t lt=0,rt=0; PollGuns(lt,rt);
        rt=255; PollGuns(lt,rt);
        testPoseAvailable=false;
        Check(FireHand(dual ? 1 : 0,weapon)==0,"tracking loss cannot fire from Lara's head");
        testPoseAvailable=true;
        lt=rt=0; PollGuns(lt,rt,true);
        Check(!g_gunTriggers.WantsShot(),"Y cancels pending taps before holstering");
    }
    SetMotionHooks(false); // These were synthetic hook flags, never patches.
}
}
