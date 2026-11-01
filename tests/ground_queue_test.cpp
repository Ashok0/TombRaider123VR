// Focused queued-crouch gait regression. No full suite or game DLL executes.
#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main
namespace {
void QueueSetup(int game,int required,Vec stick,bool smooth=true,bool modern=true,float head=0,int clip=103) {
    const int state=clip==103 ? 2 : clip==0 ? 1 : clip==40 || clip==41 ? 16 : clip==65 || clip==66 ? 22 : 21;
    Reset(state,stick,smooth,head);EnableAnimations(game);
    dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
    testConfigFlags=modern ? 2 : 0;testHeadYaw=0;
    g_heightCrouch={};g_physicalCrouchCamera=false;g_jumpLanding.Reset();
    Field<int16_t>(24)=int16_t(clip);Field<int16_t>(26)=animationTable[clip].first;
    Field<int16_t>(20)=2;Field<int16_t>(22)=int16_t(required);
    testConfig.firstPersonMoveWithHead=true;
    float x=stick.x,y=stick.z,turn=0;FirstPersonInput(x,y,turn,false,false);
}
}
int main() {
    QueueSetup(3,71,{0,1});Tick();
    Check(Field<int16_t>(18)==1 && Length(beforeCollision)>0,"TR3 standing with queued DUCK starts moving on first tick");
    const struct {Vec stick;int state,clip,speed;float offset;} moves[]={
        {{0,1},1,0,47,0},{{0,-1},16,41,2,Pi},{{-1,0},22,65,9,-Pi/2},{{1,0},21,67,9,Pi/2}};
    for(int game:{1,2,3}) for(int queued:{0,71}) for(bool smooth:{false,true})
    for(bool modern:{false,true}) for(float head:{0.f,.7f}) for(const auto& move:moves)
    for(int clip:{103,0,40,65,66,67,68}) {
        if(game!=3 && queued) continue;
        QueueSetup(game,queued,move.stick,smooth,modern,head,clip);Tick();
        Check(Field<int16_t>(18)==move.state && Field<int16_t>(24)==move.clip,"first-tick entry survives stops, reversals and queued DUCK");
        Check(Field<int16_t>(34)==move.speed && std::fabs(Length(beforeCollision)-move.speed*(move.state==1 ? 1 : 3))<3,"destination gait supplies bounded native speed");
        Check(Field<int16_t>(22)==queued,"native required state stays intact");
        Check(ticks==1 && collisions==1 && Field<PHD_3DPOS>(88).y_pos==7,"native animation/collision run once and preserve vertical movement");
        Check(std::fabs(beforeCollision.x*std::cos(head+move.offset)-beforeCollision.z*std::sin(head+move.offset))<2,"four directions follow first-person heading");
        const int frame=Field<int16_t>(26);Tick();
        Check(Field<int16_t>(26)==frame+(move.state==16 ? 4 : 1),"held movement continues instead of restarting gait");
        wall=true;const auto before=Field<PHD_3DPOS>(88);Tick();
        Check(Field<PHD_3DPOS>(88).x_pos==before.x_pos && Field<PHD_3DPOS>(88).z_pos==before.z_pos,"native wall collision retains final position");
    }
    for(bool modern:{false,true}) for(const auto& move:moves) {
        QueueSetup(3,71,move.stick,true,modern);floorAllowsEntry=false;Tick();
        Check(Field<int16_t>(18)==2 && Length(beforeCollision)==0,"native floor/ceiling rejection still prevents entry");
    }
    for(int game:{1,2,3}) for(int guard=0;guard<12;++guard) {
        QueueSetup(game,71,{-1,0});
        if(guard==0) Field<int16_t>(22)=19;
        if(guard==1) Field<int16_t>(20)=71;
        if(guard==2) *Ptr<uint32_t>(dll.input)=firstperson::PhysicalCrouchInputScope::Duck;
        if(guard==3) g_heightCrouch.active=true;
        if(guard==4) g_active=false;
        if(guard==5) *Ptr<uint16_t>(dll.lara+60)|=0x2000;
        if(guard==6) Field<uint16_t>(off::item_flags)|=8;
        if(guard==7) Field<int16_t>(24)=24;
        if(guard==8) *Ptr<uint32_t>(dll.input)=0x40;
        if(guard==9) Field<int16_t>(18)=Field<int16_t>(20)=80;
        if(guard==10) *Ptr<uint32_t>(dll.lara+0x40)|=firstperson::PhysicalCrouchInputScope::Latch;
        if(guard==11) *Ptr<int16_t>(dll.lara+40)=0;
        const auto saved=std::string(reinterpret_cast<char*>(item),sizeof(item));
        PrepareGroundDirection(item,StepLeft);
        Check(!std::memcmp(saved.data(),item,sizeof(item)),"active crouch/toggle, scripted states and TR1/2 queues retain native handling");
    }
    for(int game:{1,2,3}) {
        QueueSetup(game,71,{0,-1},true,true,0,41);Field<int16_t>(20)=16;
        const int first=Field<int16_t>(26);AccelerateBackpedalStart(item,Back);
        Check(Field<int16_t>(26)==first+(game==3 ? 3 : 0) && Field<int16_t>(22)==71,"queued-DUCK fast backpedal exception is TR3-only");
        QueueSetup(game,71,{-1,0});
        const auto saved=std::string(reinterpret_cast<char*>(item),sizeof(item));
        if(game!=3) {
            PrepareGroundDirection(item,StepLeft);
            Check(!std::memcmp(saved.data(),item,sizeof(item)),"TR1/2 state 71 never receives TR3 crouch semantics");
        }
    }
    std::printf("PASS: %d focused ground-queue checks (TR3 fix, TR1/2 guards)\n",checks);
}
