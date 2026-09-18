#pragma once
#include <cstdint>
#include "xr_math.h"
namespace firstperson {
constexpr int headJoint = 14;
inline int32_t interpolate(int32_t previous, int32_t current, int fraction) {
  return int32_t(previous + (int64_t(current) - previous) * fraction / 256);
}
// The stored joints already include Lara's body rotation, but not world translation.
inline void headPosition(const int32_t* previousBody, const int32_t* currentBody,
                         const int32_t* previousJoint, const int32_t* currentJoint,
                         int fraction, int32_t* position) {
  constexpr int local[] = {0, -32, 16}; // A point inside the head, above its neck pivot.
  for(int row=0;row<3;row++) {
    int64_t fixed=interpolate(previousJoint[row*4+3],currentJoint[row*4+3],fraction);
    for(int col=0;col<3;col++)
      fixed+=int64_t(interpolate(previousJoint[row*4+col],currentJoint[row*4+col],fraction))*local[col];
    position[row]=interpolate(previousBody[row],currentBody[row],fraction)+int32_t(fixed>>14);
  }
}
inline XrVector3f eyeOffset(XrPosef eye, const XrView* views, XrPosef origin) {
  // Cancel the tracked eye midpoint, preserving the rotated stereo baseline.
  origin.position={(views[0].pose.position.x+views[1].pose.position.x)*.5f,
                   (views[0].pose.position.y+views[1].pose.position.y)*.5f,
                   (views[0].pose.position.z+views[1].pose.position.z)*.5f};
  return vrmath::relativePosition(eye,origin);
}
}
