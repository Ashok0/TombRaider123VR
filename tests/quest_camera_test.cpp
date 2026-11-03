// Exercise the production camera detour with synthetic engine memory. Only
// its caller identity is substituted; no game DLL or full self-test executes.
#include <intrin.h>
static const void* questSceneCaller=nullptr;
#define _ReturnAddress() questSceneCaller
#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main
#undef _ReturnAddress
namespace {
int cameraDraws=0;
tr::PHD_3DPOS seenCamera{};
void __cdecl NativeQuestCamera(tr::PHD_3DPOS* pose) { ++cameraDraws;seenCamera=*pose; }
void QuestCameraSetup(int game,bool hd) {
    using namespace tr;
    SetMeshVisibility(false,false);UpdateControlMode(false);
    GunSetup(game,true,false,0,0);
    testConfigFlags=hd ? 3 : 2;
    testConfig.firstPersonFullBodyIK=testConfig.firstPersonUnarmedIK=false;
    testConfig.firstPersonBodyFollowsHead=testConfig.firstPersonWallCollision=false;
    testConfig.firstPersonAnchorX=testConfig.firstPersonAnchorZ=0;
    testConfig.firstPersonAnchorY=0;
    testViewRight=testViewForward=testViewRise=testFloorRight=testFloorForward=0;
    testHeadYaw=.4f;testAllowRecenter=true;testRecenterCalls=0;
    g_groundEye.Reset();g_stanceEye.Reset();g_mountCamera.Reset();g_mountRootHeight.Reset();g_blockCamera.Reset();
    g_runtimeInitialized=g_runtimeEnabled=true;g_active=g_haveHeading=g_scenePoseValid=g_neutralTaken=false;
    g_headingItem=nullptr;g_haveManualInput=false;g_shadowReceiver=nullptr;
    Field<uint32_t>(off::item_mesh_bits)=0x7fff;
    Field<int16_t>(off::item_speed)=0;
    Field<int16_t>(off::item_anim_number)=103;
    for(auto offset:{off::item_joints_cur,off::item_joints_prev}) {
        auto* head=reinterpret_cast<int32_t*>(item+offset+14*48);
        head[0]=head[5]=head[10]=16384;head[7]=-700*16384;
    }
    dll.w2vSceneReturn=4;questSceneCaller=reinterpret_cast<void*>(g_boundBase+4);
    g_hGenerateW2V.m_trampoline=reinterpret_cast<void*>(&NativeQuestCamera);
    testVR.m_rawHeadPose={{{1,0,0,0},{0,1,0,1.7f},{0,0,1,0}}};
    cameraDraws=0;
}
}
int main() {
    using namespace tr;
    const PHD_3DPOS native{100,200,300,10,20,30,0};
    for(int game:{1,2,3}) for(bool hd:{false,true}) for(int use:{42,43}) {
        QuestCameraSetup(game,hd);
        auto camera=native;Detour_GenerateW2V(&camera);
        Check(g_active && g_runtimeEnabled && g_headHidden,"standing first person starts normally");
        const auto heading=g_heading.base+testHeadYaw;
        Field<int16_t>(off::item_goal_state)=int16_t(use);
        Check(Gate(),"approaching a socket keeps FP until the actual use animation");
        Field<int16_t>(off::item_anim_state)=int16_t(use);Field<int16_t>(off::item_goal_state)=2;
        Field<int16_t>(off::item_anim_number)=int16_t(use==42 ? 131 : 134);
        auto before=std::string(reinterpret_cast<char*>(item),sizeof(item));
        *reinterpret_cast<uint32_t*>(before.data()+off::item_mesh_bits)=0x7fff;
        const auto laraBefore=std::string(reinterpret_cast<char*>(Ptr<uint8_t>(dll.lara)),432);
        g_gunTriggers.active=true;g_gunTriggers.pending[0]=true;g_gunEquip.pending=true;g_haveManualInput=true;
        for(int frame=0;frame<5;++frame) {
            camera=native;const int calls=cameraDraws;Detour_GenerateW2V(&camera);
            Check(!g_active && !g_scenePoseValid && g_runtimeEnabled,"quest animation suspends FP but retains its preference");
            Check(cameraDraws==calls+1 && !std::memcmp(&seenCamera,&native,sizeof(native)),"native third-person camera forwarded exactly once");
            Check(Field<uint32_t>(off::item_mesh_bits)==0x7fff && !g_headHidden,"complete native body and head restored");
            Check(!g_gunTriggers.WantsShot() && !g_gunEquip.pending && !g_haveManualInput,"no queued VR firing/equip/movement leaks through use");
            Check(!std::memcmp(before.data(),item,sizeof(item)) && !std::memcmp(laraBefore.data(),Ptr<uint8_t>(dll.lara),432),
                "camera does not alter native animation or inventory");
        }
        Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=2;
        Field<int16_t>(off::item_anim_number)=103;testHeadYaw=-.4f;camera=native;Detour_GenerateW2V(&camera);
        Check(g_active && g_runtimeEnabled && g_headHidden,"completed insertion resumes first person");
        Check(std::fabs(locomotion::Wrap(g_heading.base+testHeadYaw-heading))<.002f,"camera handoff preserves viewing heading");
        Field<int16_t>(off::item_anim_state)=int16_t(use);camera=native;Detour_GenerateW2V(&camera);
        FirstPersonToggle();Check(!g_runtimeEnabled,"manual third-person choice during insertion is retained");
        Field<int16_t>(off::item_anim_state)=2;camera=native;Detour_GenerateW2V(&camera);
        Check(!g_active && !g_runtimeEnabled && !std::memcmp(&camera,&native,sizeof(native)),"finishing cannot force third-person players into FP");
        // Beginning and finishing in third person must also be a pass-through.
        Field<int16_t>(off::item_anim_state)=int16_t(use);camera=native;Detour_GenerateW2V(&camera);
        Field<int16_t>(off::item_anim_state)=2;Detour_GenerateW2V(&camera);
        Check(!g_active && !std::memcmp(&camera,&native,sizeof(native)),"third-person-only use stays native");
    }
    for(int game:{1,2,3}) for(int state:{0,1,2,39,40,41,71,80,81,89,90,95,104,126})
        Check(!firstperson::QuestItemUsesThirdPerson(game,state),"other states, including TR3 crawl, do not inherit TR4/5 socket rules");
    for(int game:{0,4,5,6}) for(int state:{42,43})
        Check(!firstperson::QuestItemUsesThirdPerson(game,state),"predicate is restricted to TR1-3");
    QuestCameraSetup(3,true);Field<int16_t>(off::item_anim_state)=0;*Ptr<uint16_t>(dll.lara+60)|=4;
    Check(!Gate(),"TR3 extra animation bank retains its existing camera override");
    testVR.m_system=nullptr;
    std::printf("PASS: %d focused quest-camera checks (TR1-3); no full self-test executed.\n",checks);
}
