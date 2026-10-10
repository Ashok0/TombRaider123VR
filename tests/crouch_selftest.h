// TR3 crouch/crawl parity through production hooks, with native callbacks.
namespace {
void CrouchSetup(int state,int status=4,bool modern=true) {
    GunSetup(3,modern,true,status);
    Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(state);
    g_hLaraGun.m_installed=true;
    g_meshOverride=false;g_meshItem=nullptr;
    g_headHidden=g_rollHidden=g_crouchHidden=g_ledgeArmsOnly=g_unarmedArmsHidden=false;
    Field<uint32_t>(off::item_mesh_bits)=0x7fff;
    g_groundEye.Reset();g_renderTurn.Reset();g_mountRootHeight.Reset();g_mountBodyTransition.Reset();
    g_previousBody={};g_dragPrevious=g_dragCurrent=g_dragShown={};
    testViewRight=testViewForward=testViewRise=0;
}
int crouchWeaponCalls=0, crouchExit=0, expectedCrouchStatus=4;
void __cdecl NativeCrouchGun() {
    ++crouchWeaponCalls;
    Check(Field<int16_t>(18)==2,"weapon dispatcher alone sees standing during ordinary crouch");
    Check(*Ptr<int16_t>(dll.lara+2)==expectedCrouchStatus,"real weapon status restored before native gun update");
    Check((*Ptr<uint32_t>(dll.input)&0x60u)==0x60u,"draw/fire input restored at weapon consumer");
    NativeGunControl();
}
void __cdecl NativeCrouchControl(uint8_t*,void*) {
    Check(*Ptr<int16_t>(dll.lara+2)==0,"crawl movement sees free hands");
    Check((*Ptr<uint32_t>(dll.input)&0x60u)==0,"equipped draw/fire cannot become crawl interactions");
    *Ptr<uint32_t>(dll.input)|=0x100u; // Native change must survive restoring our bits.
    *Ptr<int16_t>(dll.lara+2)=1;
    if (crouchExit) {
        Field<int16_t>(20)=int16_t(crouchExit);
        return;
    }
    Detour_LaraGun();
    Check(Field<int16_t>(18)==80,"weapon dispatch restores crouch before returning to movement");
}
int crouchDrawCalls=0, crouchHandMask=0;
bool expectCrouchModern=false;
void __cdecl NativeCrouchHands(void*,int32_t masked) {
    ++crouchDrawCalls;
    Check(masked==1,"crouch hands use native masked draws");
    crouchHandMask|=Field<uint32_t>(off::item_mesh_bits);
}
void __cdecl NativeCrouchDraw(uint8_t* target) {
    Check(bool(testConfigFlags&2)==expectCrouchModern,"proximity gate disabled only for tracked crouch eye render");
    Check((testConfigFlags&1)!=0,"HD rendering remains enabled");
    const uint32_t saved=Field<uint32_t>(off::item_mesh_bits);
    Field<uint32_t>(off::item_mesh_bits)=0x3600; // Native DrawLaraHD hand geometry pass.
    Detour_DrawCreatureHD(target,1);
    Field<uint32_t>(off::item_mesh_bits)=saved;
    testConfigFlags|=0x80; // Restore only the bit owned by the scope.
}
int crouchWall=INT32_MAX, crouchQueries=0;
void __cdecl NativeCrouchCollision(RoomCollision* coll,int32_t x,int32_t,
                                  int32_t,int16_t,int32_t height) {
    ++crouchQueries;
    Check(height==400,"crouch eye queries use TR3's 400-unit native capsule");
    for (int i=0;i<6;++i) { coll->floorSamples[i*3]=0;coll->floorSamples[i*3+1]=0; }
    if (x>=crouchWall) coll->hitStatic=1;
}
void TestCrouchParity() {
    using namespace tr;
    for (int state:{71,80,81,84,85,86,89,90}) {
        for (int game:{1,2,3}) {
            CrouchSetup(state);
            dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
            Check(CanUseCrouchCamera(item)==(game==3),"crouch state IDs are restricted to TR3");
        }
        for (int excluded=0;excluded<6;++excluded) {
            CrouchSetup(state);
            if (excluded==0) Field<uint16_t>(off::item_flags)=8;
            if (excluded==1) Field<int16_t>(off::item_hit_points)=0;
            if (excluded==2) testWater=1;
            if (excluded==3) *Ptr<int16_t>(dll.lara+40)=0;
            if (excluded==4) Field<int16_t>(off::item_required_state)=1;
            if (excluded==5) Field<int16_t>(off::item_goal_state)=87; // crawl-to-hang
            Check(!CanSteerCrouch(item) && !CrouchWeaponsEligible(item),"death, water, vehicle, gravity and interaction exits retain native control");
        }
        for (bool modern:{false,true}) for (float yaw:{0.f,.7f,2.8f})
            for (Vec stick:{Vec{0,1},Vec{0,-1},Vec{1,0},Vec{-1,0},Vec{.6f,-.8f}}) {
            CrouchSetup(state,0,modern);g_heading.base=yaw;
            float x=stick.x,y=stick.z,right=.5f;
            FirstPersonInput(x,y,right,false,false);
            Check(g_haveManualInput && right==0,"crouch captures LS and artificial turning input");
            nativeSpeed=state==86 ? -10 : 10;
            Tick();
            const Vec desired=Rotate(stick,yaw);
            Check(std::fabs(beforeCollision.x-desired.x*10)<2 &&
                  std::fabs(beforeCollision.z-desired.z*10)<2,"all crawl directions follow LS without reversing backward clips");
            Check(ticks==1 && collisions==1 && Field<PHD_3DPOS>(off::item_pos).y_pos==7,
                  "crawl correction advances animation, Y and collision exactly once");
            Check(!g_crouchDrive,"crawl steering scope ends at the simulation boundary");
            wall=true;const auto before=Field<PHD_3DPOS>(off::item_pos);Tick();
            Check(Field<PHD_3DPOS>(off::item_pos).x_pos==before.x_pos &&
                  Field<PHD_3DPOS>(off::item_pos).z_pos==before.z_pos,"native wall collision remains authoritative");
        }
    }
    CrouchSetup(80,0);g_manualLocal={1,0};nativeSpeed=-8;Tick();
    Check(beforeCollision.x==8 && beforeCollision.z==0,"negative-speed all4s stopping clip still follows requested travel");
    CrouchSetup(80,0);g_manualLocal={1,0};nativeSpeed=300;Tick();
    Check(std::fabs(Length(beforeCollision)-300)<2,"large authored crawl displacement is not scaled");
    for (bool modern:{false,true}) {
        CrouchSetup(80,4,modern);expectedCrouchStatus=4;crouchWeaponCalls=crouchExit=0;
        *Ptr<uint32_t>(dll.input)=0x60u;
        g_hLaraGun.m_trampoline=reinterpret_cast<void*>(&NativeCrouchGun);
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeCrouchControl);
        Detour_LaraAboveWater(item,nullptr);
        Check(crouchWeaponCalls==1 && !g_crouchWeaponScope && (*Ptr<uint32_t>(dll.input)&0x160u)==0x160u,
              "native gun dispatch runs once and preserves unrelated native input changes");
        CrouchSetup(80,4,modern);crouchExit=87;*Ptr<uint32_t>(dll.input)=0x60u;
        g_hLaraAboveWater.m_trampoline=reinterpret_cast<void*>(&NativeCrouchControl);
        Detour_LaraAboveWater(item,nullptr);
        Check(*Ptr<int16_t>(dll.lara+2)==1 && !g_crouchWeaponScope,"crawl-to-hang keeps native hands busy on scope fallback");
        CrouchSetup(80,1,modern);
        { CrouchWeaponScope scope(item);Check(scope.active,"native crawl busy state can enter weapon scope"); }
        Check(*Ptr<int16_t>(dll.lara+2)==0,"ordinary crawling releases its busy flag for weapon equip");
        CrouchSetup(71,0,modern);uint8_t lt=255,rt=0;PollGuns(lt,rt);NativeGunTick();
        Check(*Ptr<int16_t>(dll.lara+2)==2 && Field<int16_t>(18)==71,"LT draws weapons without leaving crouch");
        *Ptr<int16_t>(dll.lara+2)=4;lt=rt=0;PollGuns(lt,rt);
        for (int hand:{0,1}) {
            lt=hand==0 ? 255 : 0;rt=hand==1 ? 255 : 0;PollGuns(lt,rt);
            Check(FireHand(hand)==1 && FireHand(hand)==0,"crouched trigger press fires once per native weapon opportunity");
            Check(testHapticShots[hand]>0,"crouched gun retains per-hand firing haptic");
        }
        lt=rt=0;PollGuns(lt,rt,true);NativeGunTick();
        Check(*Ptr<int16_t>(dll.lara+2)==3 && Field<int16_t>(18)==71,"Y holsters without leaving crouch");
    }
    CrouchSetup(80);SetMeshVisibility(true,false,true);
    Check(Field<uint32_t>(off::item_mesh_bits)==0x2400u,"crouch retains both floating hand bits at outer visibility gate");
    dll.objects=600;g_hCrouchDraw.m_trampoline=reinterpret_cast<void*>(&NativeCrouchDraw);
    g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&NativeCrouchHands);
    crouchDrawCalls=crouchHandMask=0;expectCrouchModern=false;
    Detour_CrouchDraw(item);
    Check(crouchDrawCalls==2 && crouchHandMask==0x2400 && (testConfigFlags&0x83)==0x83,
          "both floating hands draw and modern controls plus unrelated native flag changes survive");
    dll.renderPass=212;*Ptr<int32_t>(dll.renderPass)=4;expectCrouchModern=true;
    Detour_CrouchDraw(item);Check(crouchDrawCalls==3,"native shadow pass bypasses hand splitting and proximity override");
    *Ptr<int32_t>(dll.renderPass)=0;
    *Ptr<int16_t>(dll.lara+2)=0;SetMeshVisibility(true,false,true);
    Check(Field<uint32_t>(off::item_mesh_bits)==0,"unarmed crouch retains existing body hiding");
    *Ptr<int16_t>(dll.lara+2)=4;SetMeshVisibility(true,false,true);
    Check(Field<uint32_t>(off::item_mesh_bits)==0x2400u,"equipping again restores both hands from full saved mesh mask");
    SetMeshVisibility(false,false);
    Check(Field<uint32_t>(off::item_mesh_bits)==0x7fffu,"view toggle restores full mesh after crouch equip changes");
    g_hCrouchDraw.m_installed=false;g_hLaraGun.m_installed=false;

    CrouchSetup(80,0);g_groundEye.valid=true;g_groundEye.local={20,-701,90};
    const auto standing=g_groundEye.local;
    for (int frame=0;frame<100;++frame) {
        Field<PHD_3DPOS>(off::item_pos).y_rot=Angle(frame*.17f);
        PHD_3DPOS pose{frame*3,-800+frame*4,frame*5,0,0,0,0};
        UpdateLocomotion(pose);
        Check(pose.x_pos==20 && pose.y_pos==-336 && pose.z_pos==90,
              "animated crawl head and body yaw cannot orbit or bob the scene camera");
        Check(g_groundEye.local.y==standing.y,"crouch cannot replace standing eye calibration");
    }
    stabilization::StanceEye stance;
    Check(stance.Apply(-701,false,0)==-701,"stance starts at native standing height");
    Check(std::fabs(stance.Apply(-336,true,.01)+671)<.01f,"crouch entry height is rate limited");
    for (int i=2;i<=20;++i) stance.Apply(-336,true,i*.01);
    Check(stance.height==-336,"stance converges to crouch capsule eye height");
    Check(std::fabs(stance.Apply(-701,false,.21)+366)<.01f,"standing exit height is rate limited");
    stance.Reset();Check(stance.Apply(-701,false,1)==-701,"view reset discards crouch smoothing history");

    CrouchSetup(80,0);
    const auto oldBase=g_boundBase;
    g_boundBase=reinterpret_cast<uint64_t>(&NativeCrouchCollision);
    const auto rebase=[&](uint32_t& rva) { rva=uint32_t(oldBase+rva-g_boundBase); };
    rebase(dll.lara);rebase(dll.frameFrac);
    dll.getCollisionInfo=0;*Ptr<int32_t>(dll.frameFrac)=256;
    crouchWall=INT32_MAX;crouchQueries=0;
    for (float rise:{-.2f,0.f,.2f}) {
        testViewRise=rise;
        PHD_3DPOS eye{96,-701,0,0,0,0,0};
        ClampRenderedHeadToCollision(item,eye);
        Check(eye.x_pos==96 && eye.y_pos-int(std::lround(rise*1000))==-335,
              "low ceiling lowers actual tracked eye before sweep without retracting horizontal anchor");
    }
    testViewRise=0;testViewRight=.1f;crouchWall=120;
    PHD_3DPOS eye{96,-336,0,0,0,0,0};ClampRenderedHeadToCollision(item,eye);
    Check(eye.x_pos+100<120 && eye.x_pos+100>=96,"physical lean cannot place the crouched eye inside a wall");
    Check(crouchQueries>0,"crouch clearance invokes native geometry queries");
    Reset(2,{},true);g_hLaraGun.m_installed=false;SetMotionHooks(false);
}
}
