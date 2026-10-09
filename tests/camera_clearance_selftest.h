// Included by locomotion_selftest.cpp. Native callbacks reproduce the actual
// sample writes in GetCollisionInfo: four in TR1/2, six in TR3.
namespace {
int eyeSamples=4, blockedEyeSample=-1, eyeObstacle=0, eyeQueryY=0;
void __cdecl NativeEyeCollision(tr::RoomCollision* coll,int32_t,int32_t y,
                                int32_t,int16_t,int32_t height) {
    eyeQueryY=y;
    Check(height==762,"camera query uses native capsule height");
    for (int i=0;i<eyeSamples;++i) {
        coll->floorSamples[3*i]=0;
        coll->floorSamples[3*i+1]=-2000;
        coll->floorSamples[3*i+2]=0;
    }
    // TR1/2 deliberately leave offsets 48..71 untouched.
    if (blockedEyeSample>=0) {
        if (eyeObstacle==1) coll->floorSamples[3*blockedEyeSample]=-700;
        if (eyeObstacle==2) coll->floorSamples[3*blockedEyeSample+1]=0;
        if (eyeObstacle==3) coll->floorSamples[3*blockedEyeSample]=-32512;
    }
    if (eyeObstacle==4) coll->hitStatic=1;
    if (eyeObstacle==5) coll->type=8;
    if (eyeObstacle==6) coll->shift[0]=1;
}
void TestCameraClearance() {
    using namespace tr;
    for (int game:{1,2,3}) {
        Reset(2,{},true);
        dll.module=game==1 ? L"tomb1.dll" : game==2 ? L"tomb2.dll" : L"tomb3.dll";
        // A synthetic module base lets the production callback resolve directly.
        g_boundBase=reinterpret_cast<uint64_t>(&NativeEyeCollision);
        const auto fracAddress=reinterpret_cast<uint64_t>(module+200);
        Check(fracAddress>=g_boundBase && fracAddress-g_boundBase<=UINT32_MAX,
              "synthetic collision callback and frame fraction fit in one module");
        dll.getCollisionInfo=0;
        dll.frameFrac=uint32_t(fracAddress-g_boundBase);
        *Ptr<int32_t>(dll.frameFrac)=128;
        auto& body=Field<PHD_3DPOS>(off::item_pos);
        body={75000,3072,31000,0,0,0,0};
        Field<PHD_3DPOS>(off::item_pos_prev)=body;
        testConfig.positionalTracking=testConfig.firstPersonHeadTranslation=true;
        eyeSamples=game==3 ? 6 : 4;
        blockedEyeSample=-1; eyeObstacle=0;
        // Logged standing eye is 701 units above the root. A few millimetres
        // across the false ceiling at root-762 used to snap X/Z to the root.
        for (float base:{0.f,.7f,2.8f}) for (int degree=0;degree<=720;degree+=5) {
            g_heading.base=base;
            const float a=degree*locomotion::Pi/180;
            testViewRight=.04f*std::sin(a);
            testViewForward=.04f*std::cos(a);
            testViewRise=.02f*std::sin(2*a);
            PHD_3DPOS eye{74901,2371,30921,0,0,0,0};
            ClampRenderedHeadToCollision(item,eye);
            Check(eye.x_pos==74901 && eye.y_pos==2371 && eye.z_pos==30921,
                  "looking around in an open room never retracts the standing eye");
        }
        testViewRight=testViewForward=testViewRise=0;
        for (int sample=0;sample<eyeSamples;++sample) for (int obstacle=1;obstacle<=6;++obstacle) {
            blockedEyeSample=sample; eyeObstacle=obstacle;
            PHD_3DPOS eye{74901,2371,30921,0,0,0,0};
            ClampRenderedHeadToCollision(item,eye);
            Check(eye.x_pos==body.x_pos && eye.z_pos==body.z_pos,
                  "real floor, ceiling, missing room, static and collision shifts still block the eye");
        }
    }
}
}
