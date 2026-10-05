// Actual visibility and HD draw hooks, including the palette sent to shaders.
namespace {
uint32_t armDrawBits=0, armDrawSkinMask=0;
int armDrawMasked=0;
bool armDrawPalette=false;
void __cdecl NativeArmJoints(uint8_t*) {
    float* palette=tr::Ptr<float>(dll.joints);
    std::fill(palette,palette+384,0.f);
    for (int i=0;i<32;++i) {
        palette[i*12]=palette[i*12+5]=palette[i*12+10]=1;
        palette[i*12+3]=float(i)+.25f;
    }
}
void __cdecl NativeArmDraw(void* target,int32_t masked) {
    armDrawBits=*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(target)+tr::off::item_mesh_bits);
    armDrawMasked=masked;
    tr::Detour_GetJoints(static_cast<uint8_t*>(target));
    const float* palette=tr::FirstPersonBodySkin(armDrawSkinMask);
    armDrawPalette=palette!=nullptr;
    if (palette) {
        Check(palette[7*12+3]==7.25f,"body palette retains torso transform for jiggle rendering");
        Check(palette[8*12+3]==8.25f && palette[13*12+3]==13.25f,
              "hidden arm bones retain full transforms instead of collapsing skin seams");
    }
}
void ArmFrame(float pitch,bool head=true) {
    tr::testHeadPitch=pitch*(tr::locomotion::Pi/180.f);
    const bool hide=tr::HideUnarmedArms(item);
    tr::SetMeshVisibility(head,false,false,false,hide);
}
void ArmSetup(int game,int state) {
    using namespace tr;
    Reset(state,{},true);
    dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
    dll.objects=600; dll.joints=1024;
    testConfig.firstPersonMotionGuns=false;
    *Ptr<int16_t>(dll.lara+4)=1;
    g_meshOverride=false; g_meshItem=nullptr;
    g_headHidden=g_rollHidden=g_crouchHidden=g_ledgeArmsOnly=g_unarmedArmsHidden=false;
    g_unarmedArmVisibility={}; g_renderArm=-1; g_scenePoseValid=false;
    Field<uint32_t>(off::item_mesh_bits)=0x7fff;
    g_hDrawCreatureHD.m_trampoline=reinterpret_cast<void*>(&NativeArmDraw);
    g_hGetJoints.m_trampoline=reinterpret_cast<void*>(&NativeArmJoints);
}
void TestArmVisibility() {
    using namespace tr;
    for (int game:{1,2,3}) for (int state:{0,1,2,5,6,7,16,20,21,22}) for (bool head:{false,true}) {
        ArmSetup(game,state);
        const struct { float pitch; bool hidden; } sequence[]={
            {0,true},{-14,true},{-15,false},{-12,false},{-10.1f,false},
            {-10,true},{-12,true},{-15,false},{-40,false},{20,true}};
        for (const auto& step:sequence) {
            ArmFrame(step.pitch,head);
            const uint32_t expected=0x7fff & ~(head ? kHeadMeshBit : 0u) &
                ~(step.hidden ? kArmMeshBits : 0u);
            Check(Field<uint32_t>(off::item_mesh_bits)==expected,
                  "all ordinary unarmed gaits show at 15 degrees and hide at 10 without flicker");
            for (int masked:{0,1}) {
                Detour_DrawCreatureHD(item,masked);
                const bool override=head || step.hidden;
                const uint32_t drawExpected=override ?
                    firstperson::HdDrawMeshBits(expected,masked!=0,false,step.hidden,head) : expected;
                Check(armDrawBits==drawExpected && armDrawPalette==override,
                      "masked and unmasked HD passes follow the same arm visibility rule");
                Check(Field<uint32_t>(off::item_mesh_bits)==expected && !g_bodySkinScope,
                      "HD draw restores item mask and ends the palette override scope");
                if (override) Check((armDrawSkinMask&(1u<<7))!=0 &&
                                    ((armDrawSkinMask&kArmMeshBits)==0)==step.hidden,
                                    "GPU body mask hides only arms and keeps the chest visible");
            }
        }
        // Unrelated native mesh changes survive while we own head/arm bits.
        Field<uint32_t>(off::item_mesh_bits)&=~1u;
        ArmFrame(0,head); ArmFrame(-20,head);
        Check((Field<uint32_t>(off::item_mesh_bits)&(kArmMeshBits|1u))==kArmMeshBits,
              "revealing arms restores their saved bits without erasing native leg-mask changes");
        SetMeshVisibility(false,false);
        Check(Field<uint32_t>(off::item_mesh_bits)==0x7ffe,
              "leaving first person restores the complete native arm/head mask");
    }
    for (int game:{1,2,3}) {
        ArmSetup(game,2);
        for (int status:{1,2,3,4}) {
            ArmFrame(0);
            *Ptr<int16_t>(dll.lara+off::lara_gun_status)=int16_t(status);
            ArmFrame(0);
            Check(!g_unarmedArmsHidden && (Field<uint32_t>(off::item_mesh_bits)&kArmMeshBits)==kArmMeshBits,
                  "busy hands and drawing, holstering or held weapons retain their arms");
            *Ptr<int16_t>(dll.lara+off::lara_gun_status)=0;
        }
        *Ptr<int16_t>(dll.lara+4)=int16_t(game==1 ? 5 : game==2 ? 8 : 9);
        ArmFrame(0);
        Check(!g_unarmedArmsHidden,"flare/unsupported held items bypass the unarmed pitch rule");
        *Ptr<int16_t>(dll.lara+4)=1;
        for (int state:{3,10,11,19,28,30,31,54,56,57,58,59,60,61,75,82,83}) {
            Field<int16_t>(off::item_anim_state)=int16_t(state);
            ArmFrame(0);
            Check(!g_unarmedArmsHidden,"jumps, grabbing, hanging, climbing and pull-ups bypass the unarmed rule");
            if (locomotion::IsLedgeArmsOnlyState(state)) {
                SetMeshVisibility(true,false,false,true,true);
                Detour_DrawCreatureHD(item,0);
                Check(armDrawBits==kArmMeshBits && armDrawSkinMask==kArmMeshBits,
                      "ledge arm visibility takes priority even over a stale hidden-arm flag");
            }
        }
        g_active=false;
        Check(!HideUnarmedArms(item),"third person resets and bypasses the pitch gate");
        SetMeshVisibility(false,false);
    }
}
}
