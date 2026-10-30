// First-person jump/landing port; native animation commands own physics.
bool FinishFirstPersonLanding(uint8_t* item,bool landedThisTick=false) {
    if(!item || !g_active || !g_haveHeading || item!=g_headingItem || !Gate() ||
       !GroundBodyAvailable(item) || !g_boundDll->anims ||
       (BoundGame()<1 || BoundGame()>3) ||
       (*reinterpret_cast<const uint16_t*>(item+off::item_flags)&8) ||
       *reinterpret_cast<const int16_t*>(item+22)!=0) return false;
    auto& state=*reinterpret_cast<int16_t*>(item+off::item_anim_state);
    auto& goal=*reinterpret_cast<int16_t*>(item+off::item_goal_state);
    auto& clip=*reinterpret_cast<int16_t*>(item+off::item_anim_number);
    // Native collision can clear gravity and request STOP/RUN while the
    // current state still names the jump. Catch that contact before rendering
    // or input polling; otherwise even the first post-landing A tap is lost.
    const bool contact=landedThisTick && (state==3 || (state>=25 && state<=28));
    const bool recovery=locomotion::IsLandingAnimation(state,clip) &&
        !locomotion::IsHardLanding(state,clip);
    if((!contact && !recovery) || (goal!=1 && goal!=2) || clip<0) return false;
    const auto* anims=*Ptr<const uint8_t*>(g_boundDll->anims);
    if(!anims || *reinterpret_cast<const int16_t*>(anims+clip*off::anim_stride+10)!=state) return false;
    // Running landings continue into the normal run. Stationary landings go
    // straight to the stable idle loop, removing the recovery foot shuffle.
    const int target=goal==1 ? 0 : 103;
    const auto* next=anims+target*off::anim_stride;
    const int16_t first=*reinterpret_cast<const int16_t*>(next+20);
    const int16_t last=*reinterpret_cast<const int16_t*>(next+22);
    if(*reinterpret_cast<const int16_t*>(next+10)!=goal || first<0 || last<=first ||
       *reinterpret_cast<const int16_t*>(next+24)!=target ||
       *reinterpret_cast<const int32_t*>(next+16)!=0 ||
       *reinterpret_cast<const int32_t*>(next+12)!=(goal==1 ? 47<<16 : 0) ||
       *reinterpret_cast<const int16_t*>(next+32)!=(goal==1 ? BoundGame()*2 : 0)) return false;
    clip=int16_t(target);state=goal;
    *reinterpret_cast<int16_t*>(item+off::item_anim_number+2)=first;
    // Position, fall speed, damage and collision have already been resolved by
    // the native tick. Do not advance another tick or reset tracked movement.
    g_jumpLanding.Reset();g_rootMotion.Reset();g_groundGait.Reset();
    g_torsoPose.Reset();g_mountBodyTransition.Reset();g_bodyLanding=false;
    return true;
}

void SkipFirstPersonJumpPreparation(uint8_t* item) {
    if(!item || item!=g_headingItem || !g_active || !g_haveHeading || !Gate() ||
       !Cfg().gamepadEnabled || g_shifted || !GroundBodyAvailable(item) || !GroundJumpControlsAvailable() ||
       !g_boundDll->anims || !g_nativeJumpCompression ||
       (BoundGame()<1 || BoundGame()>3) ||
       (*reinterpret_cast<const uint16_t*>(item+off::item_flags)&8) ||
       *reinterpret_cast<const int16_t*>(item+22)!=0) return;
    const auto* anims=*Ptr<const uint8_t*>(g_boundDll->anims);
    if(!anims) return;
    auto& state=*reinterpret_cast<int16_t*>(item+off::item_anim_state);
    auto& goal=*reinterpret_cast<int16_t*>(item+off::item_goal_state);
    auto& clip=*reinterpret_cast<int16_t*>(item+off::item_anim_number);
    auto& frame=*reinterpret_cast<int16_t*>(item+off::item_anim_number+2);
    // Only known takeoff clips: the native end command supplies velocity,
    // gravity and the original airborne animation. Never tick physics twice.
    const auto validTakeoff=[&](int animation,int expectedState,int next,int frames) {
        const auto* a=anims+animation*off::anim_stride;
        const auto* b=anims+next*off::anim_stride;
        const int first=*reinterpret_cast<const int16_t*>(a+20);
        return *reinterpret_cast<const int16_t*>(a+10)==expectedState && first>=0 &&
            *reinterpret_cast<const int16_t*>(a+22)==first+frames-1 &&
            *reinterpret_cast<const int16_t*>(a+24)==next &&
            *reinterpret_cast<const int16_t*>(a+26)==*reinterpret_cast<const int16_t*>(b+20) &&
            *reinterpret_cast<const int16_t*>(b+10)==expectedState &&
            *reinterpret_cast<const int16_t*>(a+32)==(frames==7 ? BoundGame()+2 : BoundGame()==1 ? 1 : 2);
    };
    const auto launch=[&](int animation) {
        const auto* a=anims+animation*off::anim_stride;
        clip=int16_t(animation);frame=*reinterpret_cast<const int16_t*>(a+22);
        state=*reinterpret_cast<const int16_t*>(a+10);
        g_groundMoveAction=0;g_stabilizeRoot=g_hardStopRoot=false;g_rootMotion.Reset();
    };
    // lara_as_run has already accepted this request (including stride and
    // clearance). Preserve its left/right leading-leg airborne animation.
    if(state==1 && goal==3 && clip==0 && frame>=0 && frame<=21) {
        const int takeoff=frame+1<=11 ? 18 : 16;
        if(validTakeoff(takeoff,3,takeoff+1,7)) launch(takeoff);
        return;
    }
    const bool entering=state==2 && goal==15 && (clip==11 || clip==103);
    const bool compressing=state==15 && clip==73;
    if(!entering && !compressing) return;
    const auto* compression=anims+73*off::anim_stride;
    const int compressionCommands=*reinterpret_cast<const int16_t*>(compression+32);
    if(*reinterpret_cast<const int16_t*>(compression+10)!=15 ||
       *reinterpret_cast<const int16_t*>(compression+22)-*reinterpret_cast<const int16_t*>(compression+20)!=10 ||
       *reinterpret_cast<const int16_t*>(compression+24)!=91 ||
       compressionCommands!=(BoundGame()==1 ? 2 : 3) ||
       !validTakeoff(74,25,75,3) || !validTakeoff(76,3,77,3) ||
       !validTakeoff(78,27,79,3) || !validTakeoff(80,26,81,3) ||
       !validTakeoff(91,28,28,3)) return;
    // Compression normally rejects low ceilings and unsupported ground before
    // takeoff. Query the same standing hull before skipping those frames;
    // uneven support stays native until its floor adjustment has settled.
    if(!g_boundDll->getCollisionInfo) return;
    const auto& pos=*reinterpret_cast<const PHD_3DPOS*>(item+off::item_pos);
    RoomCollision clearance{};
    clearance.radius=100;clearance.badPos=32512;clearance.badNeg=-32512;clearance.badCeiling=0;
    clearance.facing=*Ptr<int16_t>(g_boundDll->lara+254);
    clearance.old[0]=pos.x_pos;clearance.old[1]=pos.y_pos;clearance.old[2]=pos.z_pos;
    reinterpret_cast<Fn_GetCollisionInfo>(g_boundBase+g_boundDll->getCollisionInfo)(
        &clearance,pos.x_pos,pos.y_pos,pos.z_pos,*reinterpret_cast<const int16_t*>(item+off::item_room_number),762);
    if(clearance.floorSamples[0]!=0 || clearance.floorSamples[1]>-100) return;
    if(entering) {
        // STOP approved Jump this tick. Run the usual compression controller
        // immediately so its native direction/floor checks still select the
        // jump. All three native routines leave the COLL_INFO argument unused.
        state=15;clip=73;frame=*reinterpret_cast<const int16_t*>(compression+20);
        DirectionalJumpAxesScope axes(item);
        g_nativeJumpCompression(item,nullptr);
    }
    // A rejected direction keeps goal 15, whose native default is an up-jump.
    // Death/fall/other requests retain native dispatch instead of forcing a jump.
    const int takeoff=goal==3 ? 76 : goal==25 ? 74 : goal==27 ? 78 : goal==26 ? 80 : goal==15 ? 91 : -1;
    if(takeoff>=0) launch(takeoff);
}
