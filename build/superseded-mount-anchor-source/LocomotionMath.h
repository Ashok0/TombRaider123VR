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
// Collision pushback is not physical travel. Only consume a shortened or
// sliding projection of the requested step, never a correction away from it.
inline bool AcceptCollisionDragStep(Vec requested, Vec accepted) {
    const float travel=Dot(accepted,accepted), along=Dot(accepted,requested);
    return std::isfinite(travel) && std::isfinite(along) && travel>0 &&
           travel<=64.f*64.f && travel<=along+.001f;
}
inline float Wrap(float a) { return std::remainder(a, 2 * Pi); }
inline float Radians(int16_t a) { return a * (2 * Pi / 65536); }
// Ground roll start/end share states 45/23 in TR1-3. Observe the native
// turn180_effect inside one animation tick, not B or repeated render frames.
inline bool IsGroundRollState(int state) { return state==23 || state==45; }
inline float GroundRollTurn(int beforeState,int afterState,int16_t beforeYaw,int16_t afterYaw) {
    const bool roll=IsGroundRollState(beforeState) || IsGroundRollState(afterState);
    return roll && uint16_t(int(afterYaw)-int(beforeYaw))==0x8000u ? Pi : 0.f;
}
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
inline Vec NeckFloorOffset(Vec rawEyeOffset, Vec neckArc) {
    return rawEyeOffset - neckArc;
}
// The estimated neck arc belongs to body room-scale movement. Once the
// grounded camera has a stable eye anchor, removing that arc from the rendered
// HMD position would move the world whenever the player merely turns her head.
inline Vec ViewFloorOffset(Vec rawEyeOffset, Vec neckArc, bool stabilized) {
    return stabilized ? rawEyeOffset : NeckFloorOffset(rawEyeOffset, neckArc);
}
// Artificial yaw pivots translated room position around the current player,
// but the physical neck-to-eye arc belongs to the avatar's facing. Rotating
// that arc with the room offset leaves the eye orbiting Lara until a full turn.
inline Vec PivotFloorOffset(Vec rawEyeOffset, Vec neckArc, float yawDelta) {
    return Rotate(NeckFloorOffset(rawEyeOffset, neckArc), -yawDelta) + neckArc;
}
inline bool IsJumpSteeringState(int state) { return state == 15 || state == 3; }
inline bool IsGroundLocomotionState(int state) {
    switch (state) {
    case 0: case 1: case 2: case 5: case 6: case 7:
    case 16: case 20: case 21: case 22: return true;
    default: return false;
    }
}
inline bool CanHardStopState(int state, int goal, bool gravity) {
    return !gravity && IsGroundLocomotionState(state) &&
           IsGroundLocomotionState(goal);
}
inline bool IsJumpOrFallState(int state) {
    return state == 3 || state == 9 || state == 15 ||
        (state >= 25 && state <= 29);
}
inline bool IsLedgeHangState(int state) {
    // Control-table entries shared by TR1-3; 75/82/83 exist only in TR3.
    switch (state) {
    case 10: case 30: case 31: case 75: case 82: case 83: return true;
    default: return false;
    }
}
inline bool IsClimbingCameraState(int state) {
    // TR1-3 wall-climb stance, steps and transitions. Pull-up has a native
    // animated eye; sweeping it from Lara's body can
    // place the camera behind her arms and torso.
    return state >= 56 && state <= 61;
}
// These animations keep Lara tight against geometry. The normal first-person
// avatar-fit offset deliberately moves the eye forward so her torso sits below
// the player, but here that same offset can cross the ledge wall or enter the
// movable block. Values come from lara_control_routines in the TR1-3 DLLs;
// TR3 adds hang2 and the two hanging-turn states.
inline bool IsConstrainedInteractionState(int state) {
    if (IsLedgeHangState(state)) return true;
    switch (state) {
    case 36: case 37: case 38:       // push, pull, push/pull ready
    case 56: case 57: case 58:       // climb stance, left/right
    case 59: case 60: case 61:       // climb transition/down and the other side
        return true;
    default:
        return false;
    }
}
inline int FirstPersonAnchorZ(int state, int normal, int constrained) {
    // A safety value must never move the camera farther into the obstacle than
    // the user's normal anchor, including custom anchors behind the head.
    return IsConstrainedInteractionState(state) ? std::min(normal, constrained) : normal;
}
inline bool IsMountingCameraState(int state) { return state==19; }
inline Vec MountEyeSweepStart(Vec eye,Vec headForward,int normal,int constrained) {
    // Pull-ups/vaults keep the configured eye offset. Check only its extension
    // from the in-head point, not the distance from Lara's root below/behind
    // the ledge. That root sweep can pull the camera back through her torso.
    return eye-headForward*float(normal-std::min(normal,constrained));
}
inline Vec SimulationStick(Vec world, float frameYaw, float decodedMagnitude) {
    const float n = Length(world);
    return n > 0.0001f ? Rotate(world, -frameYaw) * (decodedMagnitude / n) : Vec{};
}
constexpr uint32_t Forward = 1, Back = 2, Left = 4, Right = 8;
constexpr uint32_t Walk = 0x80, StepLeft = 0x400, StepRight = 0x800;
constexpr uint32_t Directions = Forward | Back | Left | Right | StepLeft | StepRight;
inline Vec CardinalMovement(Vec stick) {
    const float magnitude = Length(stick);
    if (magnitude <= 0.0001f) return {};
    if (std::fabs(stick.x) > std::fabs(stick.z))
        return {std::copysign(magnitude, stick.x), 0};
    return {0, std::copysign(magnitude, stick.z)};
}
inline Vec MovementWorld(Vec stick, float heading) {
    return Rotate(CardinalMovement(stick), heading);
}
inline float MovementYaw(Vec stick, float heading) {
    const Vec world = MovementWorld(stick, heading);
    return std::atan2(world.x, world.z);
}
// First-person movement keeps Lara facing the HMD. On the ground, lateral
// stick input therefore has to select the game's dedicated sidestep actions;
// ordinary Left/Right would turn her and make the forward-run animation do all
// four directions. Compression uses ordinary Left/Right for native side jumps.
inline uint32_t MovementAction(Vec stick, bool preparingJump = false) {
    if (Length(stick) <= 0.0001f) return 0;
    if (std::fabs(stick.x) > std::fabs(stick.z)) {
        if (stick.x < 0) return preparingJump ? Left : StepLeft;
        return preparingJump ? Right : StepRight;
    }
    // Back alone selects the classic fast-back hop. Walk+Back is Lara's
    // continuous backward-walk state, which is the intended backpedal here.
    return stick.z < 0 ? (Back | Walk) : Forward;
}
inline int DirectionalRootScale(uint32_t action, bool preparingJump) {
    if (preparingJump) return 1;
    const uint32_t direction = action & Directions;
    return direction == Back || direction == StepLeft || direction == StepRight ? 3 : 1;
}
inline Vec DragRequest(Vec pending, float dead) {
    const float n = Length(pending);
    return n > std::max(0.0f, dead) && n > 0.0001f
        ? pending * ((n - std::max(0.0f, dead)) / n) : Vec{};
}
struct Heading {
    float base = 0; // tracking forward -> world; never follows the chase camera
    void Align(float worldYaw, float headYaw) { base = Wrap(worldYaw - headYaw); }
    void Turn(float delta) { base = Wrap(base + delta); }
    float World(float headYaw) const { return Wrap(base + headYaw); }
};
} // namespace tr::locomotion
