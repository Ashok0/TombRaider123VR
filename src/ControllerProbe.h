#pragma once
#include <cstdint>
#include <openvr.h>

namespace tr {
// F7 starts/stops a bounded capture. Held keys and focus changes cannot retrigger
// it; Ctrl/Shift/Alt+F7 remain available to gun fitting and other native controls.
struct ControllerProbe {
    enum Event { None, Started, Sample, Stopped };
    bool active=false,held=false;
    uint64_t started=0,lastSample=0;
    Event Tick(uint64_t now,bool down,bool focused,bool modified) {
        const bool pressed=down && !held;
        held=down;
        if(active && (now-started>=20000 || (pressed && focused && !modified))) {
            active=false;return Stopped;
        }
        if(!active && pressed && focused && !modified) {
            active=true;started=lastSample=now;return Started;
        }
        if(active && now-lastSample>=500) { lastSample=now;return Sample; }
        return None;
    }
};
inline const char* InputErrorName(vr::EVRInputError error) {
    switch(error) {
    case vr::VRInputError_None:return "None";
    case vr::VRInputError_NameNotFound:return "NameNotFound";
    case vr::VRInputError_WrongType:return "WrongType";
    case vr::VRInputError_InvalidHandle:return "InvalidHandle";
    case vr::VRInputError_InvalidParam:return "InvalidParam";
    case vr::VRInputError_NoSteam:return "NoSteam";
    case vr::VRInputError_MaxCapacityReached:return "MaxCapacityReached";
    case vr::VRInputError_IPCError:return "IPCError";
    case vr::VRInputError_NoActiveActionSet:return "NoActiveActionSet";
    case vr::VRInputError_InvalidDevice:return "InvalidDevice";
    case vr::VRInputError_NoData:return "NoData";
    case vr::VRInputError_BufferTooSmall:return "BufferTooSmall";
    case vr::VRInputError_MismatchedActionManifest:return "MismatchedActionManifest";
    case vr::VRInputError_InvalidPriority:return "InvalidPriority";
    case vr::VRInputError_PermissionDenied:return "PermissionDenied";
    default:return "Other (see numeric code)";
    }
}
}
