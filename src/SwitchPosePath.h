#pragma once
#include <cstdint>

namespace tr::firstperson {
// Native animation records, used as pose data only while the hand owns a lever.
struct SwitchAnim {
    const int16_t* frames;
    int16_t interpolation,state;
    int32_t velocity,acceleration;
    int16_t first,last,jump,jumpFrame,changeCount,changeIndex,commandCount,commandIndex;
    int32_t padding;
};
static_assert(sizeof(SwitchAnim)==40);
struct SwitchChange { int16_t goal,count,index; };
struct SwitchRange { int16_t first,last,animation,frame; };
struct SwitchPose { int16_t animation,frame,state; };
struct SwitchPosePath {
    SwitchPose poses[256]{};
    int count=0;
    bool Build(const SwitchAnim* animations,const SwitchChange* changes,
               const SwitchRange* ranges,const int16_t* commands,SwitchPose pose,int goal) {
        count=0;
        if(!animations || !changes || !ranges || !commands || goal<0 || goal>1) return false;
        auto valid=[&](int index) {
            if(index<0) return false;
            const auto& a=animations[index];
            return a.frames && (a.interpolation&255)>0 && a.first>=0 && a.last>=a.first &&
                a.last-a.first<256 && a.changeCount>=0 && a.changeCount<=32 && a.changeIndex>=0 &&
                a.commandCount>=0 && a.commandCount<=32 && a.commandIndex>=0 &&
                !a.velocity && !a.acceleration;
        };
        if(!valid(pose.animation)) return false;
        const auto& initial=animations[pose.animation];
        if(pose.frame<initial.first || pose.frame>initial.last) return false;
        poses[count++]=pose;
        while(count<256) {
            ++pose.frame;
            const auto* a=&animations[pose.animation];
            if(pose.state!=goal) {
                bool changed=false;
                for(int c=0;c<a->changeCount&&!changed;++c) {
                    const auto& change=changes[a->changeIndex+c];
                    if(change.goal!=goal) continue;
                    if(change.count<0 || change.count>32 || change.index<0) return false;
                    for(int r=0;r<change.count;++r) {
                        const auto& range=ranges[change.index+r];
                        if(pose.frame<range.first || pose.frame>range.last) continue;
                        if(!valid(range.animation)) return false;
                        pose.animation=range.animation;pose.frame=range.frame;
                        a=&animations[pose.animation];pose.state=a->state;changed=true;break;
                    }
                }
            }
            bool deactivate=false;
            int command=a->commandIndex;
            for(int c=0;c<a->commandCount;++c) {
                const int op=commands[command++];
                if(op==4) deactivate=true;
                else if(op==5) command+=2; // sound: native execution occurs once, on commit
                else return false; // Root motion/effect commands belong to native special switches.
            }
            if(pose.frame>a->last) {
                if(!valid(a->jump)) return false;
                pose.animation=a->jump;pose.frame=a->jumpFrame;pose.state=animations[pose.animation].state;
                if(pose.frame<animations[pose.animation].first || pose.frame>animations[pose.animation].last) return false;
                poses[count++]=pose;
                if(deactivate) return pose.state==goal;
            } else {
                if(pose.frame<a->first) return false;
                poses[count++]=pose;
            }
        }
        count=0;return false;
    }
};
}
