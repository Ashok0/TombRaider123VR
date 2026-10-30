#pragma once
#include <cmath>
#include <cstdint>

namespace tr::firstperson {
// Tracking-space metres (Y up), independent of world scale and gun/IK offsets.
// Compare each hand both to the floor and to the headset: neither a crouch
// with the hands nor raising only the head is a two-arm pull.
struct LedgePullGesture {
    struct Sample { uint64_t time=0; float head=0, hand[2]{}; };
    Sample history[64]{}, bottom{};
    unsigned count=0, next=0;
    uint64_t until=0;
    bool latched=false;

    void Reset() { *this={}; }
    void Record(const Sample& sample) {
        history[next]=sample; next=(next+1)%64;
        if (count<64) ++count;
    }
    bool Update(bool eligible, uint64_t now, float head, float left, float right) {
        if (!eligible || !std::isfinite(head) || !std::isfinite(left) || !std::isfinite(right)) {
            Reset(); return false;
        }
        const Sample sample{now,head,{left,right}};
        if (count) {
            const auto& last=history[(next+63)%64];
            // Lost/pause/recenter samples cannot bridge into a gesture.
            if (now<last.time || now-last.time>250 ||
                std::fabs(left-last.hand[0])>.5f || std::fabs(right-last.hand[1])>.5f)
                Reset();
            else if (now-last.time<15) return latched && now<until;
        }
        if (latched) {
            if (now<until) { Record(sample); return true; }
            // A blocked ledge gets one bounded request, not endless retries.
            // Raise both hands at least 15 cm before another pull can start.
            if ((left-head)-(bottom.hand[0]-bottom.head)<.15f ||
                (right-head)-(bottom.hand[1]-bottom.head)<.15f) {
                count=0; Record(sample);
                return false;
            }
            Reset();
        }
        for (unsigned i=0;i<count;++i) {
            const auto& start=history[(next+63-i)%64];
            const auto age=now-start.time;
            if (age<100 || age>850) continue;
            bool pulled=true;
            for (int hand=0;hand<2;++hand) {
                const float down=start.hand[hand]-sample.hand[hand];
                if (down<.22f || down+head-start.head<.22f) pulled=false;
            }
            if (pulled) {
                // Cover the native 22-frame hang loop at 30 Hz.
                latched=true; until=now+900; bottom=sample;
                count=0; Record(sample);
                return true;
            }
        }
        Record(sample);
        return false;
    }
};
} // namespace tr::firstperson
