// Reuse the production-hook fixtures, but do not run the full locomotion suite.
#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main

namespace {
int transitionDraws=0;
bool expectTransition=true;
void __cdecl ObserveTransition(void* target,int32_t masked) {
    using namespace tr;
    Detour_GetJoints(static_cast<uint8_t*>(target));
    int pivot=-1;const float* palette=FirstPersonHandSkin(pivot);float centre[3]{};
    if(!expectTransition) {
        Check(!palette && !FirstPersonWristCentre(centre),"native third person and shadows never inherit transition caps");
        return;
    }
    Check(masked==1 && g_transitionHand>=0 && g_renderArm==-1,"transition solves the body underneath an isolated hand draw");
    Check(palette && FirstPersonWristCentre(centre),"transition exposes sealed-hand palette and wrist centre");
    const int joint=g_transitionHand ? 10 : 13;
    Check(Field<uint32_t>(off::item_mesh_bits)==(1u<<joint),"only one hand is visible in each split pass");
    uint32_t bits=0;Check(!FirstPersonBodySkin(bits),"rigid hand shader owns the pass without conflicting body mask");
    const auto* obj=Ptr<uint8_t>(dll.objects);const auto* geom=obj+off::object_geom;
    const int bones=*reinterpret_cast<const int*>(geom+28),count=bones>0 ? bones : 15;
    const auto* map=bones>0 ? *reinterpret_cast<const int* const*>(geom+48) : nullptr;
    const auto* poses=bones>0 ? *reinterpret_cast<const float* const*>(geom+72) : nullptr;
    motiongun::Frame wrist{},authored{},solved{};
    Check(ReadIKJoint(obj,palette,count,bones,map,poses,joint,wrist) &&
        ReadIKJoint(obj,g_bodyRenderPalette,count,bones,map,poses,joint,solved),"sealed wrist uses the solved visual pose");
    Check(firstperson::Length(motiongun::Sub(wrist.origin,solved.origin))<.01f && WristBasisError(wrist.basis,solved.basis)<.0001f,
        "visibility changes do not alter wrist position or orientation");
    if(FullBodyGripState(item)) {
        Check(ReadIKJoint(obj,ikNative,count,bones,map,poses,joint,authored),"native grip is readable");
        Check(firstperson::Length(motiongun::Sub(wrist.origin,authored.origin))<.01f && WristBasisError(wrist.basis,authored.basis)<.0001f,
            "mounting hand stays at its authored ledge contact");
    }
    Check(firstperson::Length(motiongun::Sub(wrist.origin,{centre[0],centre[1],centre[2]}))<.01f,"black cap centre matches the displayed wrist");
    float saved[384];std::memcpy(saved,palette,sizeof(saved));
    std::memset(Ptr<float>(dll.joints),0,sizeof(saved));
    Check(!std::memcmp(saved,palette,sizeof(saved)),"native bone masking cannot destroy cap palette");
    ++transitionDraws;
}
}
int main() {
    using namespace tr;
    for(int game:{1,2,3}) for(bool hd:{false,true}) {
        BodyRig(game,false);testConfig.firstPersonFullBodyIK=true;testConfig.firstPersonBodyScale=1;
        ResetBodyFollower();g_bodyVisibility={};g_transitionHand=-1;g_renderArm=-1;
        auto* obj=Ptr<uint8_t>(dll.objects);auto* geom=obj+off::object_geom;
        int mapping[32]{};float poses[32*12]{};
        motiongun::Frame native[15],bind[15],inverse[15];
        for(int j=0;j<15;++j) {
            Check(ReadIKJoint(obj,ikNative,15,0,nullptr,nullptr,j,native[j]) &&
                ReadIKBind(obj,15,0,nullptr,nullptr,j,bind[j]) && motiongun::Inverse(bind[j],inverse[j]),"fixture records original joints and bind transforms");
        }
        if(hd) {
            for(int i=0;i<32;++i) {
                const int j=mapping[i]=(i*7)%15;auto raw=inverse[j];
                for(int row=0;row<3;++row) {raw.basis.r[row][1]=inverse[j].basis.r[row][2];raw.basis.r[row][2]=-inverse[j].basis.r[row][1];}
                motiongun::WriteRows(raw,poses+i*12);
                motiongun::WriteRows(motiongun::Multiply(native[j],inverse[j]),ikNative+i*12);
            }
            *reinterpret_cast<int*>(geom+28)=32;*reinterpret_cast<int**>(geom+48)=mapping;*reinterpret_cast<float**>(geom+72)=poses;
        }
        g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&ObserveTransition);
        for(int state:{19,54,game==1 ? 58 : 55}) {
            Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(state);
            Check(FirstPersonBodyMask(item)==firstperson::HandMeshBits,"each game's mount/water-exit state shows hands only");
            for(int eye:{0,1}) for(uint32_t bits:{0x7fffu,0x3600u,0x2000u,0x400u}) {
                (void)eye;Field<uint32_t>(off::item_mesh_bits)=bits;
                uint8_t saved[sizeof(item)];std::memcpy(saved,item,sizeof(item));
                transitionDraws=0;expectTransition=true;Detour_DrawCreatureHD(item,bits==0x7fff ? 0 : 1);
                Check(transitionDraws==((bits==0x2000 || bits==0x400) ? 1 : 2),"body and attachment passes draw only their available hands");
                int pivot=-1;float centre[3]{};
                Check(g_transitionHand==-1 && !FirstPersonHandSkin(pivot) && !FirstPersonWristCentre(centre),"hand/cap scope clears after draw");
                Check(!std::memcmp(saved,item,sizeof(item)),"transition draw leaves simulation and native mesh mask intact");
            }
        }
        Field<int16_t>(off::item_anim_state)=2;g_torsoPose.recovering=true;
        Check(FirstPersonBodyMask(item)==firstperson::HandMeshBits,"post-climb recovery continues hiding arms");
        expectTransition=true;transitionDraws=0;Field<uint32_t>(off::item_mesh_bits)=0x7fff;Detour_DrawCreatureHD(item,0);
        Check(transitionDraws==2,"standing handoff retains sealed hands");
        g_torsoPose.recovering=false;g_mountBodyTransition.Reset();g_mountCamera.Reset();g_bodyVisibility.until=0;
        Check(FirstPersonBodyMask(item)==UINT32_MAX,"arms return when recovery ends");
        for(int state:{10,30,31,75,76,77,78,79,82,83}) {
            if(game!=3 && state>=75) continue;
            Field<int16_t>(off::item_anim_state)=int16_t(state);
            Check(FirstPersonBodyMask(item)==firstperson::ArmMeshBits,"ordinary hanging and TR3 monkey traversal retain arms");
        }
        expectTransition=false;g_active=false;Detour_DrawCreatureHD(item,0);
        g_active=true;*Ptr<int32_t>(dll.renderPass)=4;Detour_DrawCreatureHD(item,0);
    }
    std::printf("TR1-3 transition hands: %d checks passed\n",checks);
}
