namespace {
void TestMountCameraSmoothing() {
    using namespace tr;
    using stabilization::Point;
    const auto closePoint=[](Point a,Point b) {
        return std::fabs(a.x-b.x)<.002f && std::fabs(a.y-b.y)<.002f && std::fabs(a.z-b.z)<.002f;
    };
    const Point mount{80,-600,160},standing{20,-700,90};
    for(int hz:{60,72,90,120,144}) {
        stabilization::MountCameraTransition camera;
        Check(closePoint(camera.Apply(item,1,true,false,mount,1,.16f,1000),mount),"mount animation itself is not delayed");
        Check(closePoint(camera.Apply(item,1,false,true,standing,1.01,.16f,1000),mount),"standing handoff starts at last climbing camera");
        float last=100;
        for(int frame=1;frame<=hz;++frame) {
            const double elapsed=double(frame)/hz;
            const Point moving{standing.x+float(elapsed*120),standing.y+float(elapsed*60),standing.z};
            const auto result=camera.Apply(item,1,false,true,moving,1.01+elapsed,.16f,1000);
            const float residual=result.y-moving.y;
            Check(residual>=-.002f && residual<=last+.002f,"mount offset settles monotonically without overshoot");
            last=residual;
            const float expected=float(std::clamp(elapsed/.16,0.,1.));
            const float weight=1-expected*expected*expected*(10+expected*(-15+6*expected));
            Check(closePoint(result,{moving.x+60*weight,moving.y+100*weight,moving.z+70*weight}),
                "mount recovery is frame-rate independent and follows live root travel");
            Check(closePoint(camera.Apply(item,1,false,true,moving,1.01+elapsed,.16f,1000),result),
                "repeated eye/render calls cannot advance the blend twice");
        }
        Check(!camera.active,"mount recovery finishes at the exact live standing position");
    }
    // A pull-up already moving toward the standing eye should not stop on
    // the first grounded frame. Check the curve and its measured velocity,
    // including repeated render passes and both directions on each axis.
    for(int hz:{60,72,90,120,144}) for(float speed:{-4000.f,-200.f,0.f,200.f,4000.f}) {
        stabilization::MountCameraTransition camera;
        const double dt=1.0/hz;
        for(int frame=0;frame<8;++frame) {
            const double now=1+frame*dt;
            const Point animated{float(speed*frame*dt),float(-speed*frame*dt),0};
            camera.Apply(item,1,true,false,animated,now,.16f,1000);
            camera.Presented(animated,now);
            const auto measured=camera.velocity;
            camera.Apply(item,1,true,false,animated,now,.16f,1000);
            camera.Presented(animated,now);
            Check(closePoint(measured,camera.velocity),"duplicate mount renders preserve measured velocity");
        }
        const Point from=camera.last;
        const Point destination{from.x+100,from.y-100,0};
        const double start=1+8*dt;
        const auto first=camera.Apply(item,1,false,true,destination,start,.16f,1000);
        Check(closePoint(first,from),"moving mount remains position-continuous at handoff");
        const auto next=camera.Apply(item,1,false,true,destination,start+.0001,.16f,1000);
        const float expectedSpeed=std::clamp(speed,0.f,100*2.5f/.16f);
        Check(std::fabs((next.x-first.x)/.0001f-expectedSpeed)<2 &&
              std::fabs((next.y-first.y)/.0001f+expectedSpeed)<2,
              "handoff preserves incoming velocity toward the target within monotone limits");
        Point previous=first;
        for(int frame=1;frame<=40;++frame) {
            const auto current=camera.Apply(item,1,false,true,destination,start+frame*.004,.16f,1000);
            Check(current.x>=previous.x-.002f && current.x<=destination.x+.002f &&
                  current.y<=previous.y+.002f && current.y>=destination.y-.002f,
                  "velocity-aware recovery never reverses or overshoots on any refresh rate");
            previous=current;
        }
        Check(closePoint(previous,destination),"velocity-aware blend reaches the exact destination on time");
    }
    for(float seconds:{.01f,.03f,.16f,.4f,.8f}) {
        stabilization::MountCameraTransition fixed,moving;
        for(auto* camera:{&fixed,&moving}) {
            camera->Apply(item,1,true,false,mount,1,seconds,1000);
            camera->Presented(mount,1);
            Point raised=mount;raised.y-=4;
            camera->Apply(item,1,true,false,raised,1.02,seconds,1000);
            camera->Presented(raised,1.02);
            camera->Apply(item,1,false,true,standing,1.03,seconds,1000);
        }
        const float duration=std::clamp(seconds,.03f,.4f);
        for(int frame=1;frame<=20;++frame) {
            const double now=1.03+duration*frame/20.0;
            const Point travel{float(frame*2),float(-frame*3),float(frame*4)};
            const auto a=fixed.Apply(item,1,false,true,standing,now,seconds,1000);
            const auto b=moving.Apply(item,1,false,true,
                {standing.x+travel.x,standing.y+travel.y,standing.z+travel.z},now,seconds,1000);
            Check(closePoint({b.x-a.x,b.y-a.y,b.z-a.z},travel),
                "velocity-aware mount recovery preserves immediate locomotion at all supported durations");
        }
        Check(closePoint(fixed.last,standing),"configured duration limits still end exactly at the target");
    }
    {
        stabilization::MountCameraTransition camera;
        camera.Apply(item,1,true,false,mount,1,.16f,1000);
        const Point constrained{mount.x-30,mount.y-20,mount.z};
        camera.Presented(constrained,1);
        Check(closePoint(camera.Apply(item,1,false,true,standing,1.01,.16f,1000),constrained),
            "mount recovery starts from the collision-corrected presented camera");
        const Point cut{standing.x+1500,standing.y,standing.z};
        Check(closePoint(camera.Apply(item,1,false,true,cut,1.02,.16f,1000),cut) && !camera.active,
            "relocation during recovery cancels stale camera offset");
        camera.Reset();camera.Apply(item,1,true,false,mount,2,.16f,1000);
        camera.Presented(mount,2);
        camera.Apply(item,1,true,false,standing,2.02,.16f,1000);
        camera.Presented(standing,2.02);
        camera.Apply(item,1,false,true,{standing.x,standing.y-100,standing.z},2.1,.16f,1000);
        Check(closePoint(camera.tangent,{}),"old velocity is discarded after a render stall");
    }
    for(int scenario=0;scenario<9;++scenario) {
        stabilization::MountCameraTransition camera;
        camera.Apply(item,1,true,false,mount,1,.16f,1000);
        const void* owner=item;int level=1;double now=1.01;bool ground=true;
        float seconds=.16f;Point target=standing;
        if(scenario==0) owner=module;
        if(scenario==1) level=2;
        if(scenario==2) now=.5;
        if(scenario==3) now=2;
        if(scenario==4) ground=false;
        if(scenario==5) seconds=0;
        if(scenario==6) seconds=NAN;
        if(scenario==7) target.y+=2048;
        if(scenario==8) camera.Reset();
        Check(closePoint(camera.Apply(owner,level,false,ground,target,now,seconds,1000),target) && !camera.active,
            "camera cuts, pauses, jump/fall, disabled setting and recenter do not carry a mount offset");
    }
    // Production camera order keeps the root-height latch and saved calibration
    // authoritative; only the final animation-camera handoff is eased.
    for(int game:{1,2,3}) for(int mountState:{19,54,game==1?58:55}) {
        Reset(2,{},true);dll.module=game==1?L"tomb1.dll":game==2?L"tomb2.dll":L"tomb3.dll";
        dll.frameFrac=204;*Ptr<int32_t>(dll.frameFrac)=0;
        g_groundEye.Reset();g_groundEye.Apply({0,0,0},0,{20,-718,90},0);
        const auto calibration=g_groundEye.local;
        auto& root=Field<PHD_3DPOS>(off::item_pos);
        auto& previous=Field<PHD_3DPOS>(off::item_pos_prev);
        root.y_pos=previous.y_pos=-256;
        g_previousBody={};
        Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=int16_t(mountState);
        PHD_3DPOS eye{80,-910,160};UpdateLocomotion(eye);UpdateMountCamera(item,eye,1);
        eye.x_pos-=12;eye.y_pos-=8; // Simulate the final wall/floor correction.
        g_mountCamera.Presented({float(eye.x_pos),float(eye.y_pos),float(eye.z_pos)},1);
        const auto lastMount=eye;
        Field<int16_t>(off::item_anim_state)=Field<int16_t>(off::item_goal_state)=2;
        Field<int16_t>(off::item_anim_number)=11;
        PHD_3DPOS target{20,-974,90};UpdateLocomotion(target);
        const auto standingEye=target;
        uint8_t savedItem[sizeof(item)]{};std::memcpy(savedItem,item,sizeof(item));
        UpdateMountCamera(item,target,1.01);
        Check(target.x_pos==lastMount.x_pos && target.y_pos==lastMount.y_pos && target.z_pos==lastMount.z_pos,
            "all games' pull-up and crate/water-exit mounts avoid a one-frame standing camera snap");
        Check(g_mountCamera.active && g_mountRootHeight.active,"camera smoothing retains native destination-height latch");
        target=standingEye;target.x_rot=123;target.y_rot=456;target.z_rot=789;
        testViewRight=.24f;testViewForward=-.31f;testViewRise=.18f;testHeadYaw=.4f;
        const auto head=testVR.m_rawHeadPose;
        UpdateMountCamera(item,target,1.09);
        Check(target.y_pos>standingEye.y_pos && target.y_pos<lastMount.y_pos,"production camera height eases across the handoff");
        Check(target.x_rot==123 && target.y_rot==456 && target.z_rot==789 &&
            testViewRight==.24f && testViewForward==-.31f && testViewRise==.18f && testHeadYaw==.4f &&
            !std::memcmp(&head,&testVR.m_rawHeadPose,sizeof(head)),"mount blend never filters orientation or headset tracking");
        Check(!std::memcmp(savedItem,item,sizeof(item)) && !std::memcmp(&calibration,&g_groundEye.local,sizeof(calibration)),
            "camera easing cannot modify native physics or standing-eye calibration");
        target=standingEye;UpdateMountCamera(item,target,1.18);
        Check(target.x_pos==standingEye.x_pos && target.y_pos==standingEye.y_pos && target.z_pos==standingEye.z_pos && !g_mountCamera.active,
            "production handoff ends exactly at the standing camera");
        for(int scenario=0;scenario<7;++scenario) {
            g_mountCamera.Reset();testConfig.firstPersonMovementStabilization=true;g_active=true;
            Field<uint16_t>(off::item_flags)=0;Field<int16_t>(off::item_hit_points)=1000;testInventory=0;testWater=0;
            Field<int16_t>(off::item_anim_state)=int16_t(mountState);target=lastMount;UpdateMountCamera(item,target,2);
            Field<int16_t>(off::item_anim_state)=2;
            if(scenario==0) Field<uint16_t>(off::item_flags)=8;
            if(scenario==1) Field<int16_t>(off::item_anim_state)=3;
            if(scenario==2) g_active=false;
            if(scenario==3) testInventory=1;
            if(scenario==4) testConfig.firstPersonMovementStabilization=false;
            if(scenario==5) Field<int16_t>(off::item_hit_points)=0;
            if(scenario==6) testWater=1;
            target=standingEye;UpdateMountCamera(item,target,2.01);
            Check(target.y_pos==standingEye.y_pos && !g_mountCamera.active,"unavailable gameplay and airborne states release mount camera immediately");
        }
    }
    Reset(2,{},true);testViewRight=testViewForward=testViewRise=testHeadYaw=0;
}
}
