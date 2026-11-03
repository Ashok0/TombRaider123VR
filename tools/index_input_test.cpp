#include "VRSystem.h"
#include <string>
#include <cstdlib>
static int checks=0;
static void Check(bool ok,const char* message) {
    ++checks;
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1); }
}
#include <map>
namespace {
struct IndexRuntimeFixture {
    std::map<std::string,vr::VRActionHandle_t> handles;
    std::map<vr::VRActionHandle_t,vr::InputAnalogActionData_t> analog;
    std::map<vr::VRActionHandle_t,vr::InputDigitalActionData_t> digital;
    vr::EVRInputError initError=vr::VRInputError_None,updateError=vr::VRInputError_None;
    vr::VRActionHandle_t failAction=0,lastHaptic=0;
    std::string failInitName;
    int updates=0,haptics=0;float duration=0;
    vr::EVRInputError SetActionManifestPath(const char* path) {
        Check(path && *path,"Index manifest is supplied");return initError;
    }
    vr::EVRInputError GetActionSetHandle(const char* name,vr::VRActionSetHandle_t* out) {
        Check(std::string(name)=="/actions/game","Index activates game action set");*out=100;return initError;
    }
    vr::EVRInputError GetActionHandle(const char* name,vr::VRActionHandle_t* out) {
        if(name==failInitName) return vr::VRInputError_NameNotFound;
        const auto found=handles.find(name);
        *out=found==handles.end() ? handles.emplace(name,handles.size()+1).first->second : found->second;
        return initError;
    }
    vr::EVRInputError UpdateActionState(vr::VRActiveActionSet_t* set,uint32_t size,uint32_t count) {
        Check(size==sizeof(*set) && count==1 && set->ulActionSet==100 && !set->ulRestrictedToDevice,
            "Index refreshes both hands through the active action set");++updates;return updateError;
    }
    vr::EVRInputError GetAnalogActionData(vr::VRActionHandle_t h,vr::InputAnalogActionData_t* out,uint32_t size,vr::VRInputValueHandle_t) {
        Check(size==sizeof(*out),"analog ABI size");*out=analog[h];
        return h==failAction ? vr::VRInputError_InvalidHandle : vr::VRInputError_None;
    }
    vr::EVRInputError GetDigitalActionData(vr::VRActionHandle_t h,vr::InputDigitalActionData_t* out,uint32_t size,vr::VRInputValueHandle_t) {
        Check(size==sizeof(*out),"digital ABI size");*out=digital[h];
        return h==failAction ? vr::VRInputError_InvalidHandle : vr::VRInputError_None;
    }
    vr::EVRInputError TriggerHapticVibrationAction(vr::VRActionHandle_t h,float start,float seconds,float frequency,float amplitude,vr::VRInputValueHandle_t) {
        Check(start==0 && frequency>0 && amplitude==1,"Index haptic uses immediate vibration action");
        lastHaptic=h;duration=seconds;++haptics;return vr::VRInputError_None;
    }
    vr::VRActionHandle_t Handle(int hand,const char* name) { return handles.at(std::string("/actions/game/in/")+(hand?"right_":"left_")+name); }
    void Value(int hand,const char* name,float x,float y=0,bool active=true) {
        auto& a=analog[Handle(hand,name)];a={};a.bActive=active;a.x=x;a.y=y;
    }
    void Button(int hand,const char* name,bool down,bool active=true) {
        auto& d=digital[Handle(hand,name)];d={};d.bActive=active;d.bState=down;
    }
};
void TestIndexInput() {
    using namespace tr;
    IndexInput input;IndexRuntimeFixture runtime;
    VRSystem::HandState hands[2]{};
    Check(input.Init(runtime,"C:/game/TombRaiderVRBindings/actions.json")==vr::VRInputError_None && runtime.handles.size()==14,
        "Index resolves all named inputs and per-hand haptics");
    const auto read=[&]() { return input.Read(runtime,hands,.55f,.35f); };
    for(int hand:{0,1}) {
        runtime.Value(hand,"stick",hand?.6f:-.7f,.8f);
        runtime.Value(hand,"trigger",.42f);runtime.Value(hand,"grip",0);
        runtime.Button(hand,"lower",true);runtime.Button(hand,"upper",false);
        runtime.Button(hand,"stickclick",false);
    }
    read();
    Check(hands[0].valid && hands[1].valid && hands[0].stickX==-.7f && hands[1].stickX==.6f && hands[0].trigger==.42f,
        "independent sticks and partial analog triggers survive action conversion");
    Check(hands[0].btnLower && hands[1].btnLower && !hands[0].grip && !hands[1].grip,
        "Index A buttons cannot become grip squeezes");
    for(int hand:{0,1}) {
        runtime.Button(hand,"lower",false);
        for(const auto step:{std::pair<float,bool>{.54f,false},{.55f,true},{.4f,true},{.35f,false}}) {
            runtime.Value(hand,"grip",step.first);read();
            Check((hands[hand].grip>.5f)==step.second && !hands[hand].btnLower,
                "Index squeeze has press/release hysteresis and never becomes A");
        }
        runtime.Value(hand,"grip",.8f);read();
        runtime.Value(hand,"grip",.8f,0,false);read();
        Check(!hands[hand].grip,"inactive grip releases despite stale force");
        runtime.Button(hand,"upper",true,false);runtime.Button(hand,"stickclick",true,false);read();
        Check(!hands[hand].btnUpper && !hands[hand].stickClick,"inactive buttons cannot stay latched");
        runtime.Value(hand,"grip",NAN);runtime.Value(hand,"stick",INFINITY,-2);runtime.Value(hand,"trigger",NAN);read();
        Check(!hands[hand].grip && !hands[hand].stickX && hands[hand].stickY==-1 && !hands[hand].trigger,
            "invalid analog values cannot trigger movement, fire or grip");
        runtime.Value(hand,"grip",.2f);
        input.Read(runtime,hands,.15f,.1f);
        Check(hands[hand].grip==1,"INI grip threshold supports a lighter squeeze");
        input.Haptic(runtime,hand,3000);
        Check(runtime.lastHaptic==runtime.handles.at(std::string("/actions/game/out/")+(hand?"right_haptic":"left_haptic")) &&
            std::fabs(runtime.duration-.003f)<.00001f,"haptic selects correct hand and converts microseconds");
    }
    runtime.updateError=vr::VRInputError_NoData;
    Check(read()==vr::VRInputError_NoData && !hands[0].valid && !hands[1].valid && !hands[0].grip,
        "failed action update clears both hands instead of retaining input");
    runtime.updateError=vr::VRInputError_None;runtime.failAction=runtime.Handle(0,"trigger");
    Check(read()!=vr::VRInputError_None && !hands[0].valid && hands[1].valid,
        "per-hand action error clears only affected controller");
    runtime.failAction=0;read();Check(hands[0].valid,"action polling recovers after transient error");
    runtime.initError=vr::VRInputError_InvalidParam;
    Check(input.Init(runtime,"missing.json")==vr::VRInputError_InvalidParam && read()==vr::VRInputError_InvalidHandle,
        "manifest failure cannot reuse stale handles or silently use legacy aliases");
    runtime.initError=vr::VRInputError_None;input.Init(runtime,"manifest.json");
}
}
#include "index_diagnostics_test.inl"
int main() { TestIndexInput();TestDiagnostics();std::printf("Index input and diagnostics: %d checks passed\n",checks); }
