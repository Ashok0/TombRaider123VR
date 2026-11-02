namespace {
uint8_t pianoRooms[99*168]{},pianoMeshes[40]{};
bool pianoNativeWall=false;
int pianoNativeCalls=0;
void __cdecl PianoNativeCollision(tr::RoomCollision* c,int32_t x,int32_t,int32_t z,int16_t,int32_t) {
    ++pianoNativeCalls;
    Check(*reinterpret_cast<int32_t*>(pianoMeshes+4)==32768,"only the piano is excluded during the native query");
    c->floorSamples[0]=0;c->floorSamples[1]=-2054;
    c->type=pianoNativeWall ? 1 : 0;c->hitStatic=0;
    c->shift[0]=pianoNativeWall ? c->old[0]-x : 0;c->shift[1]=17;
    c->shift[2]=pianoNativeWall ? c->old[2]-z : 0;
}
void TestManorPiano() {
    using namespace tr;using namespace tr::locomotion;using namespace tr::firstperson;
    using Vec=tr::locomotion::Vec;
    Vec result;
    const Vec observed{44520,83761};
    Check(!SweepManorPiano(observed,{44563,83779},100,result) && result.x==44563 && result.z==83779,
        "captured Manor left-side attempt clears the piano's empty rectangular corner");
    Check(!SweepManorPiano({44520,83613},{44563,83631},100,result),
        "second live capture also clears the piano's phantom corner");
    Check(SweepManorPiano(observed,{46000,83761},100,result) && result.x>44520 && result.x<46000,
        "the actual piano remains solid after entering its old phantom corner");
    for(float radius:{32.f,64.f,100.f,200.f}) for(int angle=0;angle<360;angle+=3) {
        const float a=angle*Pi/180;const Vec direction{std::cos(a),std::sin(a)};
        const Vec from=PianoWorld(direction*3000),to=PianoWorld(direction*-3000);
        Check(SweepManorPiano(from,to,radius,result),"sweeps cannot tunnel across the piano even with clear endpoints");
        Vec second;
        Check(!SweepManorPiano(result,result,radius,second),"resolved contact leaves the collision hull outside the furniture");
        Check(!SweepManorPiano(result,from,radius,second),"walking away from a piano contact is always possible");
    }
    Check(!SweepManorPiano({46019,82810},{46042,82852},100,result),
        "walking forward from the second-side stopped position clears the false lid outline");
    for(auto p:ManorPianoVertices) {
        const Vec centre=PianoWorld({p.x,p.z});
        Check(SweepManorPiano(centre,centre,100,result,p.y+10,100),"every mesh vertex remains solid at its actual height");
    }
    const Vec lid=PianoWorld({187,-1086});
    Check(!SweepManorPiano(lid,lid,100,result),"raised lid does not project a phantom wall onto clear floor");
    Check(SweepManorPiano(lid,lid,100,result,-1200,300),"raised lid remains solid when Lara actually overlaps its height");
    Check(!SweepManorPiano(observed,{46000,83761},100,result,-1400,762),"movement entirely above furniture clears it");
    Check(!SweepManorPiano(observed,{46000,83761},100,result,1000,762),"movement entirely below furniture clears it");
    Check(!SweepManorPiano({NAN,0},observed,100,result),"invalid tracking cannot produce a collision correction");
    Reset(2,{},true);dll.module=L"tomb2.dll";dll.room=200;dll.numberRooms=212;
    *Ptr<uint8_t*>(dll.room)=pianoRooms;*Ptr<int16_t>(dll.numberRooms)=99;
    std::memset(pianoRooms,0,sizeof(pianoRooms));std::memset(pianoMeshes,0,sizeof(pianoMeshes));
    auto* r=pianoRooms+8*168;*reinterpret_cast<uint8_t**>(r+32)=pianoMeshes;
    *reinterpret_cast<int32_t*>(r+40)=34816;*reinterpret_cast<int32_t*>(r+48)=75776;
    *reinterpret_cast<int16_t*>(r+60)=11;*reinterpret_cast<int16_t*>(r+62)=14;*reinterpret_cast<int16_t*>(r+72)=2;
    for(int i=0;i<2;++i) {
        auto* m=pianoMeshes+i*20;
        *reinterpret_cast<int32_t*>(m)=i ? 44544 : 45568;
        *reinterpret_cast<int32_t*>(m+8)=i ? 81408 : 83456;
        *reinterpret_cast<int16_t*>(m+12)=i ? 0 : -16384;
        *reinterpret_cast<int16_t*>(m+18)=i ? 12 : 13;
    }
    const std::string original(reinterpret_cast<char*>(pianoMeshes),sizeof(pianoMeshes));
    g_hGetCollisionInfo.m_trampoline=reinterpret_cast<void*>(&PianoNativeCollision);
    pianoNativeWall=false;pianoNativeCalls=0;
    for(bool firstPerson:{false,true}) for(bool blocked:{false,true}) for(int x:{44563,46000}) {
        g_active=g_runtimeEnabled=firstPerson;
        RoomCollision c{};c.radius=100;c.old[0]=44520;c.old[2]=83761;
        pianoNativeWall=blocked;
        Detour_GetCollisionInfo(&c,x,0,83779,8,762);
        Check(original==std::string(reinterpret_cast<char*>(pianoMeshes),sizeof(pianoMeshes)),"piano instance is restored exactly after every query");
        Check(c.floorSamples[0]==0 && c.floorSamples[1]==-2054 && c.shift[1]==17,"native floor, ceiling and vertical resolution survive piano correction");
        if(blocked) Check(c.type==1 && x+c.shift[0]==44520 && 83779+c.shift[2]==83761,"other native obstacles still block the same step");
        else Check(x==44563 ? c.type==0 && !c.hitStatic && c.shift[0]==0 : c.type==1 && c.hitStatic && c.shift[0]<0,
            "production hook clears only the phantom corner and blocks the solid piano");
    }
    Check(pianoNativeCalls==8,"both camera modes invoke native collision exactly once per query");
    pianoNativeWall=false;
    for(bool firstPerson:{false,true}) {
        g_active=g_runtimeEnabled=firstPerson;
        RoomCollision c{};c.radius=100;c.old[0]=46019;c.old[2]=82810;
        Detour_GetCollisionInfo(&c,46042,0,82852,8,762);
        Check(!c.type && !c.hitStatic && !c.shift[0] && !c.shift[2],
            "production hook clears the second-side stopped position in both camera modes");
        for(int y:{0,-1200}) {
            c={};c.radius=100;c.old[0]=int(lid.x);c.old[2]=int(lid.z);
            Detour_GetCollisionInfo(&c,int(lid.x),y,int(lid.z),8,300);
            Check(bool(c.hitStatic)==(y==-1200),"production hook passes actual query height to raised lid collision");
            Check(original==std::string(reinterpret_cast<char*>(pianoMeshes),sizeof(pianoMeshes)),
                "height-specific piano queries restore the original instance");
        }
    }
    for(int exclusion=0;exclusion<10;++exclusion) {
        RoomCollision c{};c.radius=100;int room=8,y=0;
        if(exclusion==0) testConfig.enabled=false;
        if(exclusion==1) dll.module=L"tomb1.dll";
        if(exclusion==2) dll.module=L"tomb3.dll";
        if(exclusion==3) room=7;
        if(exclusion==4) *Ptr<int16_t>(dll.numberRooms)=98;
        if(exclusion==5) *reinterpret_cast<int32_t*>(r+40)=0;
        if(exclusion==6) *reinterpret_cast<int16_t*>(pianoMeshes+18)=14;
        if(exclusion==7) c.radius=401;
        if(exclusion==8) y=-1400;
        if(exclusion==9) y=1000;
        { ManorPianoCollisionScope scope(&c,44563,y,83779,room,762);
          Check(!scope.meshY && *reinterpret_cast<int32_t*>(pianoMeshes+4)==0,"other games, levels, geometry and vertical ranges retain native collision"); }
        testConfig.enabled=true;g_active=true;dll.module=L"tomb2.dll";*Ptr<int16_t>(dll.numberRooms)=99;
        *reinterpret_cast<int32_t*>(r+40)=34816;*reinterpret_cast<int16_t*>(pianoMeshes+18)=13;
    }
    Reset(2,{},true);
}
}
