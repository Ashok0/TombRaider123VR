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
inline Vec NeckToHead(float yaw, float metres) {
    return Rotate({0, metres}, yaw);
}
inline bool IsJumpSteeringState(int state) { return state == 15 || state == 3; }
inline Vec SimulationStick(Vec world, float frameYaw, float decodedMagnitude) {
    const float n = Length(world);
    return n > 0.0001f ? Rotate(world, -frameYaw) * (decodedMagnitude / n) : Vec{};
}
constexpr uint32_t Forward = 1, Back = 2, Left = 4, Right = 8;
constexpr uint32_t Walk = 0x80, StepLeft = 0x400, StepRight = 0x800;
constexpr uint32_t Directions = Forward | Back | Left | Right | StepLeft | StepRight;
inline Vec DragRequest(Vec pending, float dead) {
    const float n = Length(pending);
    return n > std::max(0.0f, dead) && n > 0.0001f
        ? pending * ((n - std::max(0.0f, dead)) / n) : Vec{};
}
// Modern controls turn Lara toward the requested movement vector. Body-follow
// must stand down while that happens or a lateral request is forced back to
// the HMD facing and becomes forward travel. Tank controls still need the VR
// heading to own her facing because their axes are actions rather than a
// camera-relative direction.
inline bool EngineOwnsMovingFacing(bool modern, Vec manual) {
    return modern && Length(manual) > 0.0001f;
}
struct Heading {
    float base = 0; // tracking forward -> world; never follows the chase camera
    void Align(float worldYaw, float headYaw) { base = Wrap(worldYaw - headYaw); }
    void Turn(float delta) { base = Wrap(base + delta); }
    float World(float headYaw) const { return Wrap(base + headYaw); }
};
} // namespace tr::locomotion
