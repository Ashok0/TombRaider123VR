#pragma once
#include "StereoMath.h"
namespace tr {
bool HKScopeBeginCapture();
void HKScopeEndCapture();
bool HKScopeCapturing();
void HKScopeProjection(mat4& projection,bool sky);
void HKScopeCaptureViewport();
void HKScopePresent();
void HKScopeShutdown();
}
