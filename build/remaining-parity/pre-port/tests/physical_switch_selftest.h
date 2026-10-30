// Production lever hooks against independent native callbacks and fixed-point matrices.
alignas(16) uint8_t leverItem[3664]{},leverObjects[130*2304]{};
uint8_t* leverItems=leverItem;
int leverCount=1,leverActivations=0,leverTicks=0,leverControls=0,leverPositionCalls=0;
int16_t leverBounds[6]={-64,64,-900,0,-32,32};
int16_t leverReach[12]={-220,220,-100,100,-512,0,-1820,1820,-5460,5460,-1820,1820};
firstperson::SwitchAnim leverAnimations[4]{},*leverAnims=leverAnimations;
firstperson::SwitchChange leverChanges[2]={{0,1,0},{1,1,1}},*leverChangePtr=leverChanges;
firstperson::SwitchRange leverRanges[2]={{0,1,2,2},{1,2,3,13}},*leverRangePtr=leverRanges;
int16_t leverCommands[]={4},*leverCommandPtr=leverCommands,dummyLeverFrames[16]{};
int32_t leverStack[48]{},*leverMatrixPtr=leverStack;
template<class T> T& LeverField(size_t offset) { return *reinterpret_cast<T*>(leverItem+offset); }
void __cdecl FakeLeverMatrices(uint8_t* target) {
    const int a=*reinterpret_cast<int16_t*>(target+24),f=*reinterpret_cast<int16_t*>(target+26);
    float progress=a==0 ? 0.f : a==1 ? 1.f : a==2 ? (f-2)/10.f : 1-(f-13)/10.f;
    float rows[24]{};
    motiongun::WriteRows({firstperson::IdentityBasis(),{}},rows);
    motiongun::WriteRows({firstperson::IdentityBasis(),{0,progress*200,std::sin(progress*3.14159265f)*50}},rows+12);
    auto* fixed=reinterpret_cast<int32_t*>(target+off::item_joints_cur);
    for(int i=0;i<24;++i) fixed[i]=int32_t(std::round(rows[i]*16384));
    *reinterpret_cast<int32_t*>(target+488)=123;
    leverMatrixPtr+=12; // The native function leaves a matrix stack push to its caller.
}
void __cdecl FakeLeverAnimate(uint8_t* target) {
    ++leverTicks;
    auto& a=*reinterpret_cast<int16_t*>(target+24);auto& f=*reinterpret_cast<int16_t*>(target+26);
    auto& state=*reinterpret_cast<int16_t*>(target+18);
    if(a==0) { a=2;f=2;state=0; }
    else if(a==1) { a=3;f=13;state=1; }
    else if(++f>leverAnimations[a].last) {
        f=leverAnimations[a].jumpFrame;a=leverAnimations[a].jump;
        *reinterpret_cast<uint16_t*>(target+484)=5;
    }
}
void __cdecl FakeLeverControl(int16_t index) {
    Check(index==0,"native lever control receives grabbed index");
    ++leverControls;LeverField<int16_t>(44)|=0x3e00;FakeLeverAnimate(leverItem);
}
int16_t* __cdecl FakeLeverBounds(uint8_t*) { return leverBounds; }
int32_t __cdecl FakeLeverPosition(const void* bounds,uint8_t* target,uint8_t* lara) {
    ++leverPositionCalls;
    if(!bounds) return 1;
    const auto& a=*reinterpret_cast<PHD_3DPOS*>(target+88);
    const auto& b=*reinterpret_cast<PHD_3DPOS*>(lara+88);
    const auto local=locomotion::Rotate({float(b.x_pos-a.x_pos),float(b.z_pos-a.z_pos)},-Radians(a.y_rot));
    const float p[3]={local.x,float(b.y_pos-a.y_pos),local.z};
    const auto* box=static_cast<const int16_t*>(bounds);
    for(int i=0;i<3;++i) if(p[i]<box[2*i] || p[i]>box[2*i+1]) return 0;
    return b.y_rot==a.y_rot;
}
void __cdecl FakeLeverCollision(int16_t index,uint8_t* lara,void*) {
    if(!(*Ptr<uint32_t>(dll.input)&0x40)) return;
    Check(index==0,"native switch receives selected index");
    const auto before=leverPositionCalls;
    Check(Detour_SwitchTestPosition(leverReach,leverItem,lara)==1 &&
          Detour_SwitchMovePosition(nullptr,leverItem,lara)==1,"commit satisfies native alignment");
    Check(before==leverPositionCalls,"commit bypasses native pose movement");
    ++leverActivations;LeverField<int16_t>(20)=1-LeverField<int16_t>(18);
    LeverField<uint16_t>(484)=3;FakeLeverAnimate(leverItem);
    std::memset(lara+18,41,10);std::memset(lara+88,42,40);
    std::memset(Ptr<uint8_t>(dll.lara+256),0,12);
    *Ptr<int16_t>(dll.lara+2)=1;*Ptr<int16_t>(dll.lara+58)=23;
    *Ptr<uint16_t>(dll.lara+60)&=~0x2000;
}
void TestPhysicalSwitch() {
    for(int i=0;i<4;++i) { leverAnimations[i]={};leverAnimations[i].frames=dummyLeverFrames;leverAnimations[i].interpolation=1; }
    leverAnimations[0].state=1;leverAnimations[0].changeCount=1;
    leverAnimations[1].first=leverAnimations[1].last=leverAnimations[1].jumpFrame=1;
    leverAnimations[1].jump=1;leverAnimations[1].changeCount=1;leverAnimations[1].changeIndex=1;
    leverAnimations[2].first=2;leverAnimations[2].last=12;leverAnimations[2].jump=1;
    leverAnimations[2].jumpFrame=1;leverAnimations[2].commandCount=1;
    leverAnimations[3].state=1;leverAnimations[3].first=13;leverAnimations[3].last=23;leverAnimations[3].commandCount=1;
    for(int game:{1,2,3}) for(int initial:{0,1}) {
        Reset(2,{},true);dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
        // Resolve executable callbacks and globals through one synthetic image base.
        const auto base=std::min(reinterpret_cast<uint64_t>(module),reinterpret_cast<uint64_t>(&FakeLeverMatrices))&~0xffffull;
        const auto delta=uint32_t(reinterpret_cast<uint64_t>(module)-base);g_boundBase=base;
        for(auto* field:{&dll.laraItem,&dll.camera,&dll.analogInput,&dll.input,&dll.lara}) *field+=delta;
        const auto rva=[&](const void* p) { const auto n=reinterpret_cast<uint64_t>(p)-base;Check(n<0x100000000ull,"synthetic RVA fits");return uint32_t(n); };
        SwitchDll table{};table.items=rva(&leverItems);table.count=rva(&leverCount);
        table.bounds=rva(leverReach);table.getBounds=rva(reinterpret_cast<void*>(&FakeLeverBounds));
        table.animate=rva(reinterpret_cast<void*>(&FakeLeverAnimate));table.calcMatrices=rva(reinterpret_cast<void*>(&FakeLeverMatrices));
        table.control=rva(reinterpret_cast<void*>(&FakeLeverControl));
        table.changes=rva(&leverChangePtr);table.ranges=rva(&leverRangePtr);table.commands=rva(&leverCommandPtr);
        dll.anims=rva(&leverAnims);dll.objects=rva(leverObjects);dll.phdMxptr=rva(&leverMatrixPtr);
        auto* object=leverObjects+WallSwitchObject()*off::object_stride;
        *reinterpret_cast<int16_t*>(object)=2;
        *reinterpret_cast<Fn_SwitchControl*>(object+24)=FakeLeverControl;
        g_switchDll=&table;g_switchGrab={};g_switchCandidate={};g_switchPreview={};
        g_switchGripDown=g_switchGripClaimed=g_switchCompleted=false;
        g_hSwitchCollision.m_trampoline=reinterpret_cast<void*>(&FakeLeverCollision);
        g_hSwitchTestPosition.m_trampoline=g_hSwitchMovePosition.m_trampoline=reinterpret_cast<void*>(&FakeLeverPosition);
        Field<int16_t>(24)=11;Field<PHD_3DPOS>(88)={0,0,-150,0,0,0,0};
        g_scenePose={0,-700,-32,0,0,0,0};g_scenePoseValid=true;
        testControllerOffset[1]={};testConfigFlags=1;
        std::memset(leverItem,0,sizeof(leverItem));
        LeverField<int16_t>(16)=int16_t(WallSwitchObject());LeverField<int16_t>(18)=int16_t(initial);
        LeverField<int16_t>(24)=LeverField<int16_t>(26)=int16_t(1-initial);
        uint8_t idleLever[sizeof(leverItem)];std::memcpy(idleLever,leverItem,sizeof(leverItem));
        motiongun::Vec hand{};float distance=0;
        Check(PhysicalSwitchReady() && SwitchHand(hand),"lever has usable first-person tracking");
        Check(SwitchReach(leverItem,item,hand,distance,false),"nearby hand reaches native wall lever");
        Check(!SwitchReach(leverItem,item,Add(hand,{1000,0,0}),distance,false),"distant hand cannot grab");
        // TR4/5 ordinary-lever reach follows the mesh, not the mutable native
        // tank/modern horizontal bounds. Check actual acceptance at each edge.
        const auto savedBody=Field<PHD_3DPOS>(88);
        const auto savedTarget=LeverField<PHD_3DPOS>(88);
        int16_t savedReach[12];std::memcpy(savedReach,leverReach,sizeof(savedReach));
        for(int yaw:{0,0x4000}) {
            LeverField<PHD_3DPOS>(88)={100,0,200,0,int16_t(yaw),0,0};
            const auto world=[&](float x,float y,float z) {
                const auto rotated=locomotion::Rotate({x,z},Radians(int16_t(yaw)));
                return motiongun::Vec{100+rotated.x,y,200+rotated.z};
            };
            const auto nearHand=world(0,-700,-32);
            for(int width:{20,440}) {
                leverReach[0]=int16_t(-width);leverReach[1]=int16_t(width);
                const auto reach=[&](float x,float y,float z) {
                    const auto p=world(x,y,z);
                    auto& body=Field<PHD_3DPOS>(88);
                    body.x_pos=int32_t(std::round(p.x));body.y_pos=int32_t(std::round(p.y));body.z_pos=int32_t(std::round(p.z));
                    return SwitchReach(leverItem,item,nearHand,distance,false);
                };
                Check(reach(-320,0,-150) && reach(320,0,-150),"mesh-relative lateral limits match TR4/5 regardless of native control bounds");
                Check(!reach(-321,0,-150) && !reach(321,0,-150),"beyond mesh-relative lateral limits cannot grab");
                Check(reach(0,0,-232) && !reach(0,0,-233),"ordinary lever uses TR4/5 mesh-relative 200-unit depth");
                Check(!reach(0,101,-150) && !reach(0,0,100),"native vertical reach and backside rejection remain enforced");
                Check(leverReach[0]==-width && leverReach[1]==width && leverReach[4]==savedReach[4],"physical reach never rewrites native Action bounds");
            }
        }
        std::memcpy(leverReach,savedReach,sizeof(savedReach));
        Field<PHD_3DPOS>(88)=savedBody;LeverField<PHD_3DPOS>(88)=savedTarget;
        Field<PHD_3DPOS>(88).z_pos=100;
        Check(!SwitchReach(leverItem,item,hand,distance,false),"cannot grab through backside");
        Field<PHD_3DPOS>(88).z_pos=-150;Field<PHD_3DPOS>(88).y_rot=Angle(1.2f);
        Check(SwitchReach(leverItem,item,hand,distance,false),"hand replaces Lara facing without moving body");
        --LeverField<int16_t>(16);
        Check(!SwitchReach(leverItem,item,hand,distance,false),"special switches remain native");
        ++LeverField<int16_t>(16);
        XState pad{};bool shifted=false,chord=false;
        auto grip=[&](float amount,bool gameplay=true) { testHands[1].grip=amount;BuildState(pad,shifted,gameplay,chord); };
        auto grab=[&]() { testControllerOffset[1]={};grip(0);Detour_PhysicalSwitchCollision(0,item,nullptr);grip(1);Check(g_switchPreview.active,"fresh grip attaches lever"); };
        auto move=[&](int pose) { auto d=Sub(g_switchPreview.points[pose],hand);testControllerOffset[1]={d.x/1000,d.y/1000,d.z/1000};Detour_PhysicalSwitchCollision(0,item,nullptr); };
        grab();Check(!(pad.Gamepad.wButtons&(XB_X|XB_RIGHT_SHOULDER)),"grab consumes Walk and pitch modifier");
        Check(g_switchPreview.path.count==13 && leverMatrixPtr==leverStack,"pose sampling preserves native matrix stack");
        const int ticksBefore=leverTicks,controlsBefore=leverControls;
        move(6);Check(g_switchPreview.selected==6 && !g_switchCompleted && leverTicks==ticksBefore,"half pull has no native commands");
        const auto frame=LeverField<int16_t>(26);
        for(int i=0;i<6;++i) Detour_PhysicalSwitchCollision(0,item,nullptr);
        Check(LeverField<int16_t>(26)==frame,"stationary hand holds lever stationary");
        motiongun::Frame wrist{firstperson::IdentityBasis(),{20,30,40}};
        AttachPhysicalSwitchHand(1,wrist,Add(g_switchPreview.grip,{5,6,7}));
        Check(std::fabs(wrist.origin.x-15)<.01f && std::fabs(wrist.origin.y-24)<.01f,"IK hand attaches to displayed lever");
        move(3);Check(g_switchPreview.selected==3,"reversing hand reverses lever");
        grip(0);Check(!g_switchPreview.active && LeverField<int16_t>(24)==1-initial && leverTicks==ticksBefore && LeverField<int32_t>(488)==0,"early release restores pose without activation");
        Check(!std::memcmp(idleLever,leverItem,sizeof(leverItem)),"cancellation restores both native matrix palettes and all lever state");
        for(int cancel:{0,1,2,3,4,5,6,7,8,9,10,11}) {
            grab();move(4);
            if(cancel==0) { testMissingHand=1;Detour_PhysicalSwitchCollision(0,item,nullptr);testMissingHand=-1; }
            if(cancel==1) UpdatePhysicalSwitchGrip(true,true,g_switchPreview.lastTime+251);
            if(cancel==2) grip(1,false);
            if(cancel==3) { testHands[0].grip=1;grip(1);Check(chord,"both grips retain Action");testHands[0].grip=0; }
            if(cancel==4) { testHands[0].btnUpper=true;grip(1);testHands[0].btnUpper=false; }
            if(cancel==5) { testControllerOffset[1].x+=.6f;Detour_PhysicalSwitchCollision(0,item,nullptr); }
            if(cancel==6) ResetPhysicalSwitch();
            if(cancel==7) { g_active=false;grip(1);g_active=true; }
            if(cancel==8) { testHands[0].trigger=.2f;grip(1);testHands[0].trigger=0; }
            if(cancel==9) { testHands[1].trigger=.2f;grip(1);testHands[1].trigger=0; }
            if(cancel==10) { testHands[0].btnLower=true;grip(1);testHands[0].btnLower=false; }
            if(cancel==11) { testHands[0].stickClick=testHands[1].stickClick=true;grip(1);testHands[0].stickClick=testHands[1].stickClick=false; }
            Check(!g_switchPreview.active && !g_switchPreview.attached && leverTicks==ticksBefore,"tracking, timeout, menus and chords cancel safely");
        }
        grab();uint8_t savedItem[sizeof(item)],savedLara[432];
        std::memset(Ptr<uint8_t>(dll.lara+256),0x17,12);
        *Ptr<uint16_t>(dll.lara+60)=0x1040;
        std::memcpy(savedItem,item,sizeof(item));std::memcpy(savedLara,Ptr<uint8_t>(dll.lara),432);
        *Ptr<uint32_t>(dll.input)=0x800;*Ptr<uint32_t>(dll.input+4)=0xdeadbeef;
        const int activations=leverActivations;move(g_switchPreview.path.count-1);
        Check(g_switchCompleted && leverActivations==activations+1 && LeverField<int16_t>(20)==1-initial,"end stop activates opposite state once");
        Check((LeverField<uint16_t>(484)&6)==4 && leverTicks==ticksBefore+12 && leverControls==controlsBefore+1,"native completion and timer initialization run once");
        Check(!std::memcmp(savedItem,item,sizeof(item)) && !std::memcmp(savedLara,Ptr<uint8_t>(dll.lara),432),"commit preserves Lara position, animation, flags and head angles");
        Check(*Ptr<uint32_t>(dll.input)==0x800 && *Ptr<uint32_t>(dll.input+4)==0xdeadbeef && Gate(),"32-bit input and first-person view preserved");
        *Ptr<uint32_t>(dll.input)=0;Detour_PhysicalSwitchCollision(0,item,nullptr);
        Check(leverActivations==activations+1 && UpdatePhysicalSwitchGrip(true,true,GetTickCount64()+1000),"completed grip cannot repeat or turn into Walk");
        grip(0);g_switchCandidate={};grip(1);Check((pad.Gamepad.wButtons&XB_X)!=0,"Right Grip still walks away from levers");
        ResetPhysicalSwitch();g_switchDll=nullptr;testHands[1].grip=0;testControllerOffset[1]={};
        g_hSwitchCollision.m_trampoline=g_hSwitchTestPosition.m_trampoline=g_hSwitchMovePosition.m_trampoline=nullptr;
    }
}
