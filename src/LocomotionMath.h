// Horizontal vectors use +x right and +z forward; positive yaw turns right.
// OpenVR tracking positions enter as (x, -z). No pitch/roll enters locomotion.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tr::locomotion {
constexpr float Pi = 3.14159265358979323846f;
struct Vec { float x = 0, z = 0; };
inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.z + b.z}; }
inline Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.z - b.z}; }
inline Vec operator*(Vec a, float s) { return {a.x * s, a.z * s}; }
inline float Dot(Vec a, Vec b) { return a.x * b.x + a.z * b.z; }
inline float Length(Vec a) { return std::sqrt(Dot(a, a)); }
inline float Wrap(float a) { return std::remainder(a, 2 * Pi); }
inline float Radians(int16_t a) { return a * (2 * Pi / 65536); }
inline int16_t Angle(float a) { return static_cast<int16_t>(static_cast<int>(Wrap(a) * (65536 / (2 * Pi)))); }
inline Vec Rotate(Vec v, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c * v.x + s * v.z, -s * v.x + c * v.z};
}
inline Vec Limit(Vec v) {
    const float n = Length(v);
    return n > 1 ? v * (1 / n) : v;
}
inline float StickTurn(float axis, float dead, float degreesPerSecond, float seconds) {
    dead = std::clamp(dead, 0.0f, 0.95f);
    const float strength = std::max(0.0f, std::min(1.0f, std::fabs(axis)) - dead) / (1 - dead);
    return std::copysign(strength, axis) * std::clamp(degreesPerSecond, 0.0f, 720.0f)
           * Pi / 180 * std::clamp(seconds, 0.0f, 0.05f);
}
inline Vec RoomInput(Vec offset, float dead, float full) {
    const float n = Length(offset);
    dead = std::max(0.0f, dead);
    if (n <= dead || n < 0.0001f) return {};
    const float strength = std::clamp((n - dead) / std::max(0.01f, full - dead), 0.0f, 1.0f);
    return offset * (strength / n);
}
// Only consume travel along the requested step, never collision slide in the
// opposite direction. Allocation excludes the manual stick's share of travel.
inline Vec Consumed(Vec pending, Vec travel, float allocation) {
    const float n = Length(pending);
    if (n < 0.0001f) return {};
    const Vec direction = pending * (1 / n);
    return direction * std::clamp(Dot(travel, direction) * allocation, 0.0f, n);
}
struct Heading {
    float base = 0; // tracking forward -> world; never follows the chase camera
    void Align(float worldYaw, float headYaw) { base = Wrap(worldYaw - headYaw); }
    void Turn(float delta) { base = Wrap(base + delta); }
    float World(float headYaw) const { return Wrap(base + headYaw); }
};
} // namespace tr::locomotion
