// Production simulation/animation hooks, including native turn commands.
namespace {
void TestJumpRoll() {
    using namespace tr;
    using namespace tr::locomotion;
    struct Flip { int state,animation,exitState,exitAnimation; };
    const Flip tr1Flips[]={{3,113,25,160},{3,43,25,47},{25,48,3,49}};
    const Flip flips[]={
        {3,207,25,209},{3,210,25,211},{25,212,3,213}};
    for (int game:{1,2,3}) for (bool smooth:{false,true}) for (bool modern:{false,true})
    for (float heading:{0.f,.7f,3.1f,-3.1f}) for (bool boundary:{false,true})
    for (const auto& flip:(game==1 ? tr1Flips : flips)) {
        Reset(flip.state,{0,1},smooth,heading);
        dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
        testConfigFlags=modern ? 2 : 0;
        auto& body=Field<PHD_3DPOS>(off::item_pos);
        body.y_rot=Angle(heading);
        Field<int16_t>(off::item_anim_number)=int16_t(flip.animation);
        Field<uint16_t>(off::item_flags)=8;
        *Ptr<int16_t>(dll.lara+254)=Angle(heading);
        const Vec oldManual=g_manualWorld;
        const int16_t oldYaw=body.y_rot;
        extraInput=0x1000; // Native B/roll survives input adaptation.
        Tick();
        Check(testPivotCount==0 && std::fabs(Wrap(g_heading.base-heading))<.0001f,
              "B press and pre-turn jump-roll frames do not flip VR early");
        if (boundary) { nextState=flip.exitState; nextAnimation=flip.exitAnimation; }
        nativeHalfTurn=true;
        const auto before=body;
        Tick();
        Check(std::fabs(Wrap(g_heading.base-heading-Pi))<.0001f && testPivotCount==1,
              "native TR1/2/3 midair turn rotates the VR heading exactly once");
        Check(std::fabs(testPivotTurn-Pi)<.0001f && Length(g_manualWorld+oldManual)<.0001f,
              "midair turn pivots tracking and carries held movement into the new heading");
        Check(uint16_t(int(body.y_rot)-oldYaw)==0x8000 && body.y_pos==before.y_pos+7 &&
              Length(beforeCollision)<12 && Field<int16_t>(off::item_speed)==nativeSpeed &&
              (Field<uint16_t>(off::item_flags)&8),
              "VR turn preserves native body half-turn, airborne trajectory, speed and gravity");
        Check((*Ptr<uint32_t>(dll.input)&0x1000)!=0 &&
              Ptr<int16_t>(dll.analogInput)[2]==Angle(g_lastHeadWorld) &&
              Ptr<int16_t>(dll.analogInput)[3]==Angle(g_lastHeadWorld),
              "roll button remains native and both native camera headings follow the flip");
        for (int tick=0;tick<10;++tick) Tick();
        Check(testPivotCount==1 && std::fabs(Wrap(g_heading.base-heading-Pi))<.0001f,
              "holding B and repeated animation ticks cannot double-flip the camera");
        Field<int16_t>(off::item_anim_state)=2;
        Field<uint16_t>(off::item_flags)=0;
        TurnBodyToHead(item,.05f);
        Check(std::fabs(Wrap(Radians(body.y_rot)-g_heading.base))<.001f,
              "landing body-follow does not undo the midair half-turn");
    }
    // Ordinary jump wall deflections, water rolls, wrong-game animations, third person and
    // cancelled flips must never acquire the new airborne camera behavior.
    for (int scenario=0;scenario<6;++scenario) {
        Reset(3,{},true);
        dll.module=scenario==0 ? L"tomb1.dll" : L"tomb3.dll";
        Field<int16_t>(off::item_anim_number)=scenario==1 ? 77 : 207;
        if (scenario==2) { testWater=1; Field<int16_t>(off::item_anim_state)=66; }
        if (scenario==3) g_active=false;
        if (scenario==4) g_runtimeEnabled=false;
        nativeHalfTurn=scenario!=5;
        Detour_AnimateLara(item);
        Check(testPivotCount==0 && g_heading.base==0,
              "air-roll camera excludes wrong-game animations, ordinary jumps, water, third person and cancelled turns");
    }
    for (int game:{1,2,3}) {
        Reset(45,{},true);
        dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
        nativeHalfTurn=true; nextState=23;
        Detour_AnimateLara(item);
        Check(testPivotCount==1 && std::fabs(Wrap(g_heading.base-Pi))<.0001f,
              "ground-roll heading still follows the native half-turn in all three games");
    }
}
}
