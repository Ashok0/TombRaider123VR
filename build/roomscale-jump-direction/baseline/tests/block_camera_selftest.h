// Production block observer, camera and final tracked-eye collision path.
namespace {
alignas(16) uint8_t testBlocks[2*3664]{};
int blockAcceptedState=-1,blockNativeCalls=0;
void __cdecl NativeBlockCollision(int16_t,uint8_t* lara,void*) {
    ++blockNativeCalls;
    if (blockAcceptedState>=0) {
        *reinterpret_cast<int16_t*>(lara+18)=int16_t(blockAcceptedState);
        ++*reinterpret_cast<int16_t*>(lara+26);
    }
}
void SetupBlockCamera(int game,int direction,bool smooth=true) {
    Reset(2,{},smooth);
    dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
    g_blockCamera.Reset(); g_groundEye.Reset(); g_scenePoseValid=false;
    g_groundEye.valid=true; g_groundEye.local={0,-701,126};
    std::memset(testBlocks,0,sizeof(testBlocks));
    const auto oldBase=g_boundBase;
    g_boundBase=reinterpret_cast<uint64_t>(&NativeEyeCollision);
    const auto rva=[&](uint32_t offset) { return uint32_t(oldBase+offset-g_boundBase); };
    dll.laraItem=rva(dll.laraItem); dll.items=rva(184); dll.frameFrac=rva(200);
    dll.getCollisionInfo=0;
    *Ptr<uint8_t*>(dll.items)=testBlocks;
    *Ptr<int32_t>(dll.frameFrac)=256;
    eyeSamples=game==3 ? 6 : 4; blockedEyeSample=-1; eyeObstacle=0;
    testConfig.positionalTracking=testConfig.firstPersonHeadTranslation=true;
    testViewRight=testViewForward=testViewRise=0;
    auto& body=Field<PHD_3DPOS>(88);
    body={50000,3072,50000,0,int16_t(direction*16384),0,0};
    const auto forward=Rotate({0,1},Radians(body.y_rot));
    auto& block=*reinterpret_cast<PHD_3DPOS*>(testBlocks+88);
    block=body; block.x_pos+=int32_t(std::lround(forward.x*612));
    block.z_pos+=int32_t(std::lround(forward.z*612));
    *reinterpret_cast<PHD_3DPOS*>(testBlocks+108)=block;
    Field<PHD_3DPOS>(108)=body;
    for (auto offset:{off::item_joints_prev,off::item_joints_cur}) {
        auto* torso=reinterpret_cast<int32_t*>(item+offset+7*off::joint_stride);
        torso[0]=torso[5]=torso[10]=16384;
        torso[7]=-500*16384;
    }
    g_hBlockCollision.m_trampoline=reinterpret_cast<void*>(&NativeBlockCollision);
    blockAcceptedState=-1; blockNativeCalls=0;
}
void TestBlockCamera() {
    using namespace tr;
    for (int game:{1,2,3}) for (int direction=0;direction<4;++direction) {
        SetupBlockCamera(game,direction);
        Detour_BlockCollision(0,item,nullptr);
        Check(g_blockCamera.index==-1 && blockNativeCalls==1,
              "a refused block interaction leaves native behavior and camera unchanged");
        blockAcceptedState=38;
        Detour_BlockCollision(0,item,nullptr);
        Check(g_blockCamera.index==0 && g_blockCamera.owner==item && blockNativeCalls==2,
              "native acceptance identifies the exact interacted block once");
        blockAcceptedState=-1;
        Detour_BlockCollision(1,item,nullptr);
        Check(g_blockCamera.index==0,"nearby unrelated blocks cannot steal the camera");
        g_blockCamera.Reset();
        Detour_BlockCollision(0,item,nullptr);
        Check(g_blockCamera.index==0,"saved ready stance recovers its block without an animation change");
        for (bool smooth:{false,true}) for (int state:{36,37}) {
            SetupBlockCamera(game,direction,smooth);
            blockAcceptedState=state;
            Detour_BlockCollision(0,item,nullptr);
            const auto facing=g_blockCamera.facing;
            const auto blockStart=*reinterpret_cast<PHD_3DPOS*>(testBlocks+88);
            PHD_3DPOS eye{50000,2371,50000,0,0,0,0};
            UpdateBlockCamera(item,eye,0);
            float lastAlong=0;
            for (int tick=0;tick<=64;++tick) for (int frac:{0,64,128,192,256}) {
                const float sign=state==36 ? 1.f : -1.f;
                auto* a=reinterpret_cast<int32_t*>(testBlocks+off::item_joints_prev);
                auto* b=reinterpret_cast<int32_t*>(testBlocks+off::item_joints_cur);
                const float before=sign*std::max(0,tick-1)*16, after=sign*tick*16;
                a[3]=int32_t(std::lround(facing.x*before*16384));
                a[11]=int32_t(std::lround(facing.z*before*16384));
                b[3]=int32_t(std::lround(facing.x*after*16384));
                b[11]=int32_t(std::lround(facing.z*after*16384));
                *Ptr<int32_t>(dll.frameFrac)=frac;
                const float travel=before+(after-before)*frac/256.f;
                // Kneel 240 units during the action and rise again. Both body
                // samples must contribute at intermediate render fractions.
                const int oldDrop=std::min(std::max(0,tick-1),64-std::max(0,tick-1))*15/2;
                const int newDrop=std::min(tick,64-tick)*15/2;
                auto* torsoA=reinterpret_cast<int32_t*>(item+off::item_joints_prev+7*off::joint_stride);
                auto* torsoB=reinterpret_cast<int32_t*>(item+off::item_joints_cur+7*off::joint_stride);
                torsoA[7]=(-500+oldDrop)*16384; torsoB[7]=(-500+newDrop)*16384;
                const float drop=oldDrop+(newDrop-oldDrop)*frac/256.f;
                // Deliberately severe head bob must have no influence.
                eye={50100+(tick%3)*30,2300+(tick%7)*25,50100-(tick%5)*40,0,0,0,0};
                uint8_t savedLara[sizeof(item)],savedBlock[sizeof(testBlocks)];
                std::memcpy(savedLara,item,sizeof(item));
                std::memcpy(savedBlock,testBlocks,sizeof(testBlocks));
                UpdateBlockCamera(item,eye,1+tick/30.0+frac/7680.0);
                ClampRenderedHeadToCollision(item,eye);
                const float x=blockStart.x_pos+facing.x*(travel-576);
                const float z=blockStart.z_pos+facing.z*(travel-576);
                Check(std::fabs(eye.x_pos-x)<1.1f && std::fabs(eye.z_pos-z)<1.1f && std::fabs(eye.y_pos-(2371+drop))<=.51f,
                      "push/pull follows interpolated torso kneeling vertically and block motion horizontally");
                const float along=eye.x_pos*facing.x+eye.z_pos*facing.z;
                if (tick>0) Check(std::fabs(along-lastAlong)<=4.1f,
                      "block camera advances smoothly between simulation ticks");
                lastAlong=along;
                Check(std::memcmp(savedLara,item,sizeof(item))==0 &&
                      std::memcmp(savedBlock,testBlocks,sizeof(testBlocks))==0,
                      "camera never changes native skeletons, block movement or physics inputs");
            }
            // At completion, the root advances and the joint resets. Their
            // world-space sum must retain interpolation over the last 16 units.
            auto& block=*reinterpret_cast<PHD_3DPOS*>(testBlocks+88);
            const float sign=state==36 ? 1.f : -1.f;
            block.x_pos+=int32_t(std::lround(facing.x*sign*1024));
            block.z_pos+=int32_t(std::lround(facing.z*sign*1024));
            auto* b=reinterpret_cast<int32_t*>(testBlocks+off::item_joints_cur);
            b[3]=b[11]=0;
            for (int frac=0;frac<=256;++frac) {
                *Ptr<int32_t>(dll.frameFrac)=frac;
                const auto centre=RenderedBlockCentre(testBlocks,frac);
                const float travel=sign*(1008+16*frac/256.f);
                Check(std::fabs(centre.x-(blockStart.x_pos+facing.x*travel))<1.1f &&
                      std::fabs(centre.z-(blockStart.z_pos+facing.z*travel))<1.1f,
                      "animation root reset cannot cause a tile-sized camera jump");
            }
            UpdateBlockCamera(item,eye,4);
            eyeObstacle=4;
            ClampRenderedHeadToCollision(item,eye);
            Check(std::abs(eye.x_pos-int32_t(std::lround(g_blockCamera.root.x)))<=1 &&
                  std::abs(eye.z_pos-int32_t(std::lround(g_blockCamera.root.z)))<=1,
                  "surrounding room walls still stop the eye from the moving stance");
            eyeObstacle=0;
            // Headset leaning is applied after the scene camera. Test the final
            // tracked eye, with arbitrary tracking/world yaw, against the face.
            for (float yaw:{0.f,.7f,2.8f}) for (float lean:{-.12f,0.f,.02f,.2f}) {
                g_heading.base=yaw;
                const auto local=Rotate(facing*lean,-yaw);
                testViewRight=local.x; testViewForward=local.z;
                UpdateBlockCamera(item,eye,4.1);
                ClampRenderedHeadToCollision(item,eye);
                const Vec final{eye.x_pos+facing.x*lean*1000,eye.z_pos+facing.z*lean*1000};
                const float depth=(final.x-block.x_pos)*facing.x+(final.z-block.z_pos)*facing.z;
                Check(depth<=-575 && depth>=-700,
                      "either eye and near plane stay outside a moving block while leaning");
            }
            testViewRight=testViewForward=0;
            // Start the next tick with the block settled at its destination.
            *reinterpret_cast<PHD_3DPOS*>(testBlocks+108)=block;
            auto* a=reinterpret_cast<int32_t*>(testBlocks+off::item_joints_prev);
            a[3]=a[11]=0;
            reinterpret_cast<int32_t*>(item+off::item_joints_prev+7*off::joint_stride)[7]=-500*16384;
            UpdateBlockCamera(item,eye,5); ClampRenderedHeadToCollision(item,eye);
            const auto last=eye;
            Field<int16_t>(18)=2;
            auto& body=Field<PHD_3DPOS>(88);
            body.x_pos+=int32_t(std::lround(facing.x*sign*1024));
            body.z_pos+=int32_t(std::lround(facing.z*sign*1024));
            *Ptr<int32_t>(dll.frameFrac)=0;
            eye={50000,2371,50000,0,0,0,0};
            UpdateBlockCamera(item,eye,5.01); ClampRenderedHeadToCollision(item,eye);
            Check(std::abs(eye.x_pos-last.x_pos)<=1 && std::abs(eye.z_pos-last.z_pos)<=1,
                  "return to standing preserves the last block eye despite the origin advance");
            Field<PHD_3DPOS>(108)=body;
            PHD_3DPOS previous=eye;
            for (int step=1;step<=16;++step) {
                eye={body.x_pos,2371,body.z_pos,0,0,0,0};
                UpdateBlockCamera(item,eye,5.01+step*.01);
                ClampRenderedHeadToCollision(item,eye);
                Check(std::hypot(float(eye.x_pos-previous.x_pos),float(eye.z_pos-previous.z_pos))<5,
                      "release blend settles over time without a frame-sized camera snap");
                previous=eye;
            }
            Check(!g_blockCamera.following && g_blockCamera.index==-1,
                  "completed release leaves ordinary locomotion free of block state");
            // Re-enter before testing interruption of an active interaction.
            blockAcceptedState=state; Detour_BlockCollision(0,item,nullptr);
            UpdateBlockCamera(item,eye,6);
            // Death/water/level changes must immediately stop following a block.
            Field<int16_t>(off::item_hit_points)=0;
            eye={1,2,3,0,0,0,0}; UpdateBlockCamera(item,eye,6.01);
            Check(eye.x_pos==1 && eye.y_pos==2 && eye.z_pos==3 && !g_blockCamera.collision,
                  "death releases the block camera without touching its replacement pose");
        }
    }
    for (int game:{1,2,3}) for (int state:{36,37,38}) {
        SetupBlockCamera(game,0);
        blockAcceptedState=state; Detour_BlockCollision(0,item,nullptr);
        auto* a=reinterpret_cast<int32_t*>(item+off::item_joints_prev+7*off::joint_stride);
        auto* b=reinterpret_cast<int32_t*>(item+off::item_joints_cur+7*off::joint_stride);
        a[7]=b[7]=-300*16384;
        PHD_3DPOS eye{50000,2571,50000,0,0,0,0};
        UpdateBlockCamera(item,eye,0);
        Check(g_blockCamera.torsoEyeValid && eye.y_pos==2571,
              "first-person entry during a kneel fits the current eye instead of forcing standing height");
        Field<int16_t>(18)=36;
        a[7]=-300*16384; b[7]=-200*16384;
        for (int frac=0;frac<=256;++frac) {
            *Ptr<int32_t>(dll.frameFrac)=frac;
            eye.y_pos=2200; // Independent head motion is deliberately unrelated.
            UpdateBlockCamera(item,eye,1+frac/256.0);
            Check(std::fabs(eye.y_pos-(2571+100*frac/256.f))<=.51f,
                  "ready-to-push keeps the fit and follows torso height at every render fraction");
        }
        const auto last=eye;
        Field<int16_t>(18)=2;
        eye.y_pos=2371; UpdateBlockCamera(item,eye,3);
        Check(eye.y_pos==last.y_pos,"release from a low torso pose starts without a vertical snap");
        eye.y_pos=2371; UpdateBlockCamera(item,eye,3.075);
        Check(eye.y_pos==2521,"release smoothly blends the lowered eye back to standing");
        eye.y_pos=2371; UpdateBlockCamera(item,eye,3.16);
        Check(eye.y_pos==2371 && g_groundEye.local.y==-701,
              "kneeling never overwrites the saved standing eye height");
    }
    g_blockCamera.Reset();
}
}
