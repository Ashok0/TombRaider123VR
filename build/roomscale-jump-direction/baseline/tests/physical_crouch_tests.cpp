// Standalone math/input checks: no game hooks, DLL loading or VR runtime.
#include "BodyFollower.h"
#include <cstdio>
#include <cstdlib>
using namespace tr::firstperson;
static int checks=0;
static void Check(bool ok,const char* why) {
    ++checks;
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1); }
}
int main() {
    HeightCrouch crouch;
    Frame native[15]{};
    const Vec p[15]={{0,-500,0},{-100,-500,0},{-100,-260,70},{-100,0,0},
        {100,-500,0},{100,-260,70},{100,0,0},{0,-650,0},
        {180,-700,0},{240,-460,30},{180,-250,70},{-180,-700,0},{-240,-460,30},{-180,-250,70},{0,-850,0}};
    for(int i=0;i<15;++i) native[i]={IdentityBasis(),p[i]};
    Frame standing[15]{};
    Check(SolveFollowingBody(native,p[14],p[14],0,IdentityBasis(),{0,0,1},1000,{},0,true,0,0,standing,1),"standing solve");
    for(float drop:{.15f,.25f,.35f,.45f,.54f}) {
        Check(!crouch.Update(true,drop),"partial squat retains visible body");
        Frame pose[15]{};
        Check(SolveFollowingBody(native,p[14],Add(p[14],{0,drop*1000,0}),0,IdentityBasis(),
            {0,0,1},1000,{},0,true,0,0,pose,StandingPostureWeight(drop)),"squat solve");
        Check(pose[0].origin.y>standing[0].origin.y+50,"squat lowers pelvis");
        for(int first:{1,4}) {
            Check(KneeBendDegrees(pose,first)>KneeBendDegrees(standing,first)+15,"knees bend before native crawl");
            Check(Length(Sub(pose[first+2].origin,p[first+2]))<.05f,"feet stay planted");
            for(int j=first;j<first+2;++j)
                Check(std::fabs(Length(Sub(pose[j+1].origin,pose[j].origin))-Length(Sub(p[j+1],p[j])))<.05f,"leg lengths preserved");
        }
    }
    Check(crouch.Update(true,.56f),"deep squat requests native crouch");
    Check(crouch.Update(true,.3f),"hysteresis prevents repeated transitions");
    Check(!crouch.Update(true,.22f),"rising releases crouch");
    Check(!crouch.Update(true,0) && !crouch.Update(true,-.1f),"standing and raised HMD stay released");
    Check(crouch.Update(true,.6f) && !crouch.Update(false,.6f),"gate releases crouch");
    Check(!crouch.Update(true,NAN),"invalid height releases crouch");
    constexpr auto duck=PhysicalCrouchInputScope::Duck, latch=PhysicalCrouchInputScope::Latch;
    for(bool modern:{false,true}) for(bool held:{false,true}) for(bool manual:{false,true}) for(bool latched:{false,true}) {
        uint32_t flags=0x80000001u|(latched ? latch : 0);
        uint32_t input=0x41|(manual ? duck : 0);
        const auto before=input;
        {
            PhysicalCrouchInputScope scope(&input,&flags,modern,held);
            Check(modern ? bool(flags&latch)==(held||manual) : bool(flags&latch)==latched,
                "TR3 modern latch follows height; tank latch unchanged");
            Check(bool(input&duck)==(!modern && (held||manual)),"modern cannot retoggle; tank receives held Duck");
            Check((input&~duck)==(before&~duck),"other controls retained");
            input|=0x10; // A native handler's unrelated edit must survive.
            flags|=0x100;
        }
        Check(input==(before|0x10),"only synthetic Duck removed after native tick");
        Check((flags&~latch)==0x80000101u,"other native flag edits survive");
        Check(bool(flags&latch)==(modern && !held ? manual : latched),
            "physical hold leaves no new latch; rising clears the old latch");
    }
    {
        uint32_t flags=latch,input=0;
        { PhysicalCrouchInputScope scope(&input,&flags,true,true);flags&=~latch; }
        Check(!(flags&latch),"native fall or crawl-to-hang may clear the latch");
        flags=latch;
        { PhysicalCrouchInputScope scope(&input,&flags,true,false); }
        Check(!(flags&latch),"standing clears an already-stuck crouch toggle");
        { PhysicalCrouchInputScope scope(nullptr,nullptr,true,true); }
    }
    std::printf("Physical crouch: %d standalone checks passed.\n",checks);
}
