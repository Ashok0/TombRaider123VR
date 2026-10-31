// Real firing hooks with native callbacks, plus the production pulse scheduler.
namespace {
int hapticVolleyWeapon=4;
void __cdecl NativeHapticVolley() {
    if(hapticVolleyWeapon==4) {
        for(int pellet=0;pellet<6;++pellet) FireHand(1,4);
    } else { FireHand(0);FireHand(1); }
    Check(tr::testHapticShots[0]==0 && tr::testHapticShots[1]==0,
          "pellet/dual-gun haptics wait for the native firing operation to finish");
}
bool hapticSpawn=true;
int hapticObject=0;
uint8_t hapticMissile[3664]{};
void* __cdecl HapticFloor(int32_t,int32_t,int32_t,int16_t* room) {
    *room=-1;return nullptr; // Stop retargeting after confirmed allocation.
}
void __cdecl NativeHapticProjectile() {
    if(!hapticSpawn) return;
    *tr::Ptr<int16_t>(dll.nextItemFree)=1;
    *reinterpret_cast<int16_t*>(hapticMissile+tr::off::item_object_number)=int16_t(hapticObject);
}
void __cdecl NativeHapticGrenade(int) { NativeHapticProjectile(); }
void TestGunHaptics() {
    using namespace tr;
    for(int game:{1,2,3}) for(int result:{-1,0,1}) {
        GunSetup(game,false,true,4);
        nativeGunResult=result;
        for(int hand=0;hand<2;++hand) {
            Check(FireHand(hand)==result,"haptics retain native hit/miss/no-ammo result");
            Check(testHapticShots[hand]==int(result!=0),"misses rumble too, empty weapons do not");
            Check(testHapticShots[1-hand]==(hand ? int(result!=0) : 0),"firing hand cannot rumble the other controller");
        }
        GunSetup(game,false,true,4,4);nativeGunResult=result;
        hapticVolleyWeapon=4;
        g_hLaraGun.m_trampoline=reinterpret_cast<void*>(&NativeHapticVolley);
        Detour_LaraGun();
        Check(nativeGunShots==6 && testHapticShots[0]==0 && testHapticShots[1]==int(result!=0),
              "shotgun pellets produce one right-hand burst without altering the volley");
        Check(g_shotHapticMask==-1,"shot haptics scope restores after native gun update");
        GunSetup(game,false,true,4);nativeGunResult=result;hapticVolleyWeapon=1;
        g_hLaraGun.m_trampoline=reinterpret_cast<void*>(&NativeHapticVolley);
        Detour_LaraGun();
        Check(testHapticShots[0]==int(result!=0) && testHapticShots[1]==int(result!=0),
              "simultaneous guns each rumble their matching controller");
    }
    for(int missing=0;missing<4;++missing) {
        GunSetup(1,false,true,4);
        if(missing==0) testPoseAvailable=false;
        if(missing==1) g_active=false;
        if(missing==2) testConfig.firstPersonMotionGuns=false;
        const int16_t aim[2]={};
        FireWeaponForCaller(missing==3 ? 0x999 : dll.rightFireReturn,1,nullptr,nullptr,aim);
        Check(!testHapticShots[0] && !testHapticShots[1],"untracked or unidentified shots cannot emit tracked-gun rumble");
    }
    for(int game:{2,3}) for(int weapon=6;weapon<=(game==2 ? 7 : 8);++weapon)
    for(bool spawn:{false,true}) for(bool expectedObject:{false,true}) {
        GunSetup(game,false,true,4,weapon);
        dll.nextItemFree=212;dll.items=216;dll.numberRooms=232;dll.itemNewRoom=1;
        // Rebase data RVAs so getFloor can resolve directly to the test callback.
        const uint64_t oldBase=g_boundBase;
        g_boundBase=reinterpret_cast<uint64_t>(&HapticFloor);
        Check(oldBase>g_boundBase && oldBase-g_boundBase<UINT32_MAX,"haptic native callbacks fit synthetic module");
        for(auto member:{&GameDllLayout::laraItem,&GameDllLayout::lara,&GameDllLayout::input,
                         &GameDllLayout::analogInput,&GameDllLayout::camera,&GameDllLayout::frameFrac,
                         &GameDllLayout::nextItemActive,&GameDllLayout::nextItemFree,
                         &GameDllLayout::items,&GameDllLayout::numberRooms})
            dll.*member+=uint32_t(oldBase-g_boundBase);
        dll.getFloor=0;
        *Ptr<uint8_t*>(dll.items)=hapticMissile;
        *Ptr<int16_t>(dll.nextItemFree)=0;
        const int object=game==2 ? (weapon==6 ? 0xF8 : 0xF9) :
            weapon==6 ? 0x135 : weapon==7 ? 0x137 : 0x136;
        hapticSpawn=spawn;hapticObject=expectedObject ? object : 0;
        for(auto* hook:{&g_hFireHarpoon,&g_hFireRocket,&g_hFireGrenade,&g_hAnimateShotgun})
            hook->m_installed=true;
        g_hFireHarpoon.m_trampoline=g_hFireRocket.m_trampoline=g_hFireGrenade.m_trampoline=
            reinterpret_cast<void*>(&NativeHapticProjectile);
        g_hAnimateShotgun.m_trampoline=reinterpret_cast<void*>(&NativeHapticGrenade);
        if(game==2 && weapon==6) Detour_AnimateShotgun(6);
        else if((game==2 && weapon==7) || weapon==8) Detour_FireHarpoon();
        else if(weapon==6) Detour_FireRocket();
        else Detour_FireGrenade();
        Check(testHapticShots[0]==0 && testHapticShots[1]==int(spawn && expectedObject),
              "projectile rumble requires actual allocation of the correct missile, including TR2 grenades");
        for(auto* hook:{&g_hFireHarpoon,&g_hFireRocket,&g_hFireGrenade,&g_hAnimateShotgun})
            hook->m_installed=false;
    }
    SetMotionHooks(false);
    GunHaptics pulses;
    int count[2]{};uint64_t last[2]{};uint64_t now=100;
    auto pulse=[&](int hand,unsigned short duration) {
        Check(hand>=0 && hand<2 && duration==3999,"gun burst uses strong legacy pulses on a valid hand");
        Check(!count[hand] || now-last[hand]>=5,"legacy haptic pulses stay at least 5 ms apart");
        ++count[hand];last[hand]=now;
    };
    pulses.Shot(-1,now);pulses.Shot(2,now);pulses.Update(now,pulse);
    Check(!count[0] && !count[1],"invalid hands are ignored");
    pulses.Shot(0,now);
    for(now=100;now<180;++now) { pulses.Update(now,pulse);pulses.Update(now,pulse); }
    Check(count[0]==16 && !count[1],"left-only shot sustains an 80 ms burst without per-eye duplicates");
    pulses.Update(now,pulse);Check(count[0]==16,"burst stops at 80 ms");
    pulses.Shot(1,200);now=200;pulses.Update(now,pulse);
    pulses.Shot(1,250); // Held fire refreshes a deadline; it never queues a tail.
    now=279;pulses.Update(now,pulse);now=329;pulses.Update(now,pulse);
    const int before=count[1];now=330;pulses.Update(now,pulse);
    Check(count[1]==before && count[0]==16,"repeat fire refreshes only its hand and expires after the last shot");
    pulses.Shot(0,400);pulses.Shot(1,400);pulses.Reset();now=400;pulses.Update(now,pulse);
    Check(count[0]==16 && count[1]==before,"runtime reset cancels all pending rumble");
    pulses.Shot(0,500);now=1000;pulses.Update(now,pulse);
    Check(count[0]==16,"expired bursts are dropped after a render stall");
}
}
