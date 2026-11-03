void TestDiagnostics() {
    using namespace tr;
    IndexInput input;IndexRuntimeFixture runtime;
    VRSystem::HandState hands[2]{};
    Check(!input.Ready() && std::string(input.Diagnostic().initStage)=="not initialized","uninitialized input remains distinguishable");
    runtime.initError=vr::VRInputError_MismatchedActionManifest;
    input.Init(runtime,"manifest.json");
    Check(!input.Ready() && input.Diagnostic().initError==runtime.initError &&
        std::string(input.Diagnostic().initStage)=="SetActionManifestPath","manifest mismatch preserves exact failure stage and error");
    runtime.initError=vr::VRInputError_None;runtime.failInitName="/actions/game/in/right_upper";
    input.Init(runtime,"manifest.json");
    Check(!input.Ready() && std::string(input.Diagnostic().initStage)==runtime.failInitName,"failed action lookup preserves exact path");
    runtime.failInitName.clear();input.Init(runtime,"manifest.json");
    input.Capture(true);
    const auto read=[&]() { return input.Read(runtime,hands,.55f,.35f); };
    Check(read()==vr::VRInputError_None && !hands[0].valid && !hands[1].valid &&
        input.Ready() && !input.Diagnostic().activeSeen[0],"resolved handles with inactive bindings are not mistaken for live controllers");
    runtime.Value(0,"stick",-.8f,.7f);runtime.Value(0,"grip",.8f);
    runtime.Button(0,"upper",true);read();
    runtime.Value(0,"stick",0);runtime.Value(0,"grip",.4f);runtime.Button(0,"upper",false);read();
    Check(input.Diagnostic().pressedSeen[0]==(1u<<4) && input.Diagnostic().peak[0][0]==.8f &&
        input.Diagnostic().peak[0][1]==.7f && input.Diagnostic().peak[0][3]==.8f,
        "brief presses and analog peaks survive between log snapshots");
    const int updates=runtime.updates;input.Capture(false);input.Capture(true);
    Check(runtime.updates==updates && !input.Diagnostic().pressedSeen[0] && !input.Diagnostic().peak[0][3],
        "capture toggles neither poll runtime nor retain a previous capture's summary");
    read();Check(hands[0].grip==1,"starting diagnostics does not reset grip hysteresis mid-squeeze");
    runtime.failAction=runtime.Handle(0,"grip");read();
    Check(input.Diagnostic().errors[0][2]==vr::VRInputError_InvalidHandle && !hands[0].valid,
        "per-action error is visible while gameplay retains its error handling");
    runtime.failAction=0;runtime.updateError=vr::VRInputError_NoActiveActionSet;read();
    Check(input.Diagnostic().updateError==runtime.updateError && !input.Diagnostic().analog[0][0].bActive &&
        input.Diagnostic().errors[0][0]==vr::VRInputError_NoData,"failed refresh cannot report stale input as current");
    runtime.updateError=vr::VRInputError_None;read();
    Check(input.Diagnostic().updateError==vr::VRInputError_None && hands[0].valid,"diagnostics and input recover after an error");
    Check(std::string(InputErrorName(vr::VRInputError_MismatchedActionManifest))=="MismatchedActionManifest",
        "runtime binding mismatch has a readable name");
    ControllerProbe probe;
    Check(probe.Tick(0,true,false,false)==ControllerProbe::None,"unfocused F7 does not start a capture");
    Check(probe.Tick(1,true,true,false)==ControllerProbe::None,"focusing while F7 held does not trigger it");
    probe.Tick(2,false,true,false);
    Check(probe.Tick(3,true,true,true)==ControllerProbe::None,"Ctrl/Shift/Alt F7 remains reserved");
    Check(probe.Tick(4,true,true,false)==ControllerProbe::None,"releasing modifier while holding F7 does not trigger it");
    probe.Tick(5,false,true,false);
    Check(probe.Tick(100,true,true,false)==ControllerProbe::Started,"plain focused F7 starts capture");
    Check(probe.Tick(101,true,true,false)==ControllerProbe::None,"holding F7 cannot toggle repeatedly");
    Check(probe.Tick(599,false,false,false)==ControllerProbe::None,"sampling is bounded to twice per second");
    Check(probe.Tick(600,false,false,false)==ControllerProbe::Sample,"capture continues after desktop focus loss");
    Check(probe.Tick(20100,false,false,false)==ControllerProbe::Stopped && !probe.active,"capture stops after twenty seconds without focus");
    Check(probe.Tick(20200,true,true,false)==ControllerProbe::Started,"another capture can be requested");
    probe.Tick(20201,false,true,false);
    Check(probe.Tick(20202,true,true,false)==ControllerProbe::Stopped,"second press stops capture early");
    Check(probe.Tick(20203,true,true,false)==ControllerProbe::None,"held stop key cannot restart capture");
}
