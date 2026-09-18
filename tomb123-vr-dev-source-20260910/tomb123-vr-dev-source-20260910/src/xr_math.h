#pragma once
#include <cmath>
#include <algorithm>
#include <openxr/openxr.h>
namespace vrmath {
struct M {
  float a[3][3]{};
};
inline M transpose(M m) {
  M r;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      r.a[i][j] = m.a[j][i];
  return r;
}
inline M mul(M a, M b) {
  M r;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      for (int k = 0; k < 3; k++)
        r.a[i][j] += a.a[i][k] * b.a[k][j];
  return r;
}
inline XrVector3f mul(M a, XrVector3f b) {
  float v[] = {b.x, b.y, b.z}, o[3]{};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      o[i] += a.a[i][j] * v[j];
  return {o[0], o[1], o[2]};
}
inline M rotation(XrQuaternionf q) {
  return {
      {{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
       {2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
       {2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w),
        1 - 2 * (q.x * q.x + q.y * q.y)}}};
}
inline M gameRotation(M xr) {
  float sign[] = {1, -1, -1};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      xr.a[i][j] *= sign[i] * sign[j];
  return xr;
}
inline XrVector3f relativePosition(XrPosef eye, XrPosef origin) {
  XrVector3f p{eye.position.x - origin.position.x, eye.position.y - origin.position.y,
               eye.position.z - origin.position.z};
  p = mul(transpose(rotation(origin.orientation)), p);
  return {p.x, -p.y, -p.z};
}
inline M relativeView(XrPosef eye, XrPosef origin, M anchor) {
  return mul(transpose(gameRotation(
                 mul(transpose(rotation(origin.orientation)), rotation(eye.orientation)))),
             anchor);
}
inline void angles(M m, int16_t *result) {
  constexpr float units = 65536.f / (2 * 3.14159265358979323846f);
  float p = asinf(std::clamp(-m.a[2][1], -1.f, 1.f));
  float y = atan2f(m.a[2][0], m.a[2][2]);
  float r = atan2f(m.a[0][1], m.a[1][1]);
  result[0] = (int16_t)lroundf(p * units);
  result[1] = (int16_t)lroundf(y * units);
  result[2] = (int16_t)lroundf(r * units);
}
inline void projection(float *p, XrFovf f) {
  float l = tanf(f.angleLeft), r = tanf(f.angleRight), u = tanf(f.angleUp), d = tanf(f.angleDown);
  p[0] = std::copysign(2 / (r - l), p[0]);
  p[5] = std::copysign(2 / (u - d), p[5]);
  p[8] = -p[11] * (r + l) / (r - l);
  p[9] = -p[11] * (u + d) / (u - d);
}
} // namespace vrmath
