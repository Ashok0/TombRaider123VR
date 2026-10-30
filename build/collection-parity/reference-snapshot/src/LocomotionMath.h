// Directional stick locomotion from TombRaider123VR/LocomotionMath.h.
// Horizontal +x is right, +z is forward; positive yaw turns right.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tr::locomotion {
struct Vec { float x = 0, z = 0; };
inline Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.z + b.z}; }
inline Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.z - b.z}; }
inline Vec operator*(Vec a, float s) { return {a.x * s, a.z * s}; }
inline float Length(Vec a) { return std::sqrt(a.x * a.x + a.z * a.z); }
inline float Dot(Vec a,Vec b) { return a.x*b.x+a.z*b.z; }
// Collision pushback is not physical travel. Only consume a shortened or
// sliding projection of the requested step, never a correction away from it.
inline bool AcceptCollisionDragStep(Vec requested, Vec accepted) {
    const float travel=Dot(accepted,accepted), along=Dot(accepted,requested);
    return std::isfinite(travel) && std::isfinite(along) && travel>0 &&
           travel<=64.f*64.f && travel<=along+.001f;
}
inline Vec Rotate(Vec v, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c * v.x + s * v.z, -s * v.x + c * v.z};
}
inline Vec Limit(Vec v) {
    const float n = Length(v);
    return n > 1 ? v * (1 / n) : v;
}
// Same physical neck pivot as TR1-3: Lara's animated head supplies the eye
// arc, so only genuine neck displacement belongs on top of that anchor.
inline Vec NeckToHead(float yaw, float metres) { return Rotate({0, metres}, yaw); }
inline Vec NeckFloorOffset(Vec rawEyeOffset, Vec neckArc) { return rawEyeOffset - neckArc; }
inline Vec DragRequest(Vec pending, float dead) {
    const float n = Length(pending);
    return n > std::max(0.0f, dead) && n > 0.0001f
        ? pending * ((n - std::max(0.0f, dead)) / n) : Vec{};
}
inline Vec PivotFloorOffset(Vec rawEyeOffset, Vec neckArc, float yawDelta) {
    return Rotate(NeckFloorOffset(rawEyeOffset, neckArc), -yawDelta) + neckArc;
}
inline bool IsJumpSteeringState(int state) { return state == 15 || state == 3; }
// Shared TR4/5 ground roll start/end. Observe the native animation's exact
// half-turn, not the B button or repeated rendered frames (TR1-3 behavior).
inline bool IsGroundRollState(int state) { return state==23 || state==45; }
inline bool IsHardLanding(int state,int animation) {
    // TR4/5 lara_col_fastfall uses stop (2), animation 24 after a survivable
    // hard fall, just like TR1-3. Stop alone also includes idle and vault exits.
    return state==2 && animation==24;
}
inline bool IsLandingAnimation(int game,int state,int animation) {
    // Native TR4/5 PDP jump/fall exits: hard fall, vertical/drop landing,
    // ordinary jump landing and landing into a run. Match state AND clip so
    // ordinary STOP/RUN animation keeps its stable eye. Clip 99 is a death
    // in most TR5 levels but a landing in JOBY4; its state disambiguates it.
    if (game!=0 && game!=1) return false;
    // State 7 / clip 13 is the looping left turn. Armed modern controls enter
    // it when HMD facing differs from VRIK's body yaw; treating it as a landing
    // swaps the stable eye for the animated neck and back on every head turn.
    return IsHardLanding(state,animation) ||
        (state==2 && (animation==31 || animation==82 || animation==99)) ||
        (state==1 && animation==92);
}
inline bool IsJumpRollAnimation(int state,int animation) {
    // Installed TR4/5 Lara tables: forward flips 207/210 and backflip 212
    // contain turn180_effect. Ordinary jumps and wall deflections do not.
    return (state==3 && (animation==207 || animation==210)) ||
        (state==25 && animation==212);
}
inline float NativeRollTurn(int beforeState,int afterState,int beforeAnimation,int afterAnimation,
                            int16_t beforeYaw,int16_t afterYaw) {
    return (IsGroundRollState(beforeState) || IsGroundRollState(afterState) ||
            IsJumpRollAnimation(beforeState,beforeAnimation) ||
            IsJumpRollAnimation(afterState,afterAnimation)) &&
        uint16_t(int(afterYaw)-int(beforeYaw))==0x8000u ? 3.14159265358979323846f : 0.f;
}
inline bool IsMonkeyBarState(int state) {
    // TR4/5 lara_control_routines: hang2, swing, left/right traverse,
    // native half-turn, and hanging turns. Ledges/ladders are separate.
    return (state>=75 && state<=79) || state==82 || state==83;
}
inline bool IsLedgeHangState(int state) {
    // Shared TR4/5 hang, shimmy, alternate hang/turn and stop-to-hang states.
    switch (state) {
    case 10: case 30: case 31: case 75: case 82: case 83: case 139: return true;
    default: return false;
    }
}
inline bool IsLedgeMountState(int state) {
    // LaraTestWaterClimbOut in both TR4/5 sets AS_WATEROUT (55),
    // hands busy and water_status=0 before animations 111/191/193 finish.
    return state==19 || state==54 || state==55; // vault, gymnast, water exit
}
inline bool IsLadderState(int state) {
    return state>=56 && state<=61; // stance, up, left, end, right, down
}
inline bool IsConstrainedInteractionState(int state) {
    // Classic states retained by TR4/5: hang/pull-up, push/pull, climb.
    // Do not carry over TR3-only hang-turn state numbers.
    if (IsLedgeHangState(state)) return true;
    switch (state) {
    case 10: case 19: case 30: case 31:
    case 36: case 37: case 38:
    case 54: // Gymnast pull-up, also pitches the head through the ledge.
    case 55: // Water climb-out needs the same short eye offset and native grips.
    case 56: case 57: case 58: case 59: case 60: case 61:
        return true;
    default: return false;
    }
}
inline int FirstPersonAnchorZ(int state, int normal, int constrained) {
    return IsConstrainedInteractionState(state) ? std::min(normal, constrained) : normal;
}
inline Vec SimulationStick(Vec world, float frameYaw, float magnitude) {
    const float n = Length(world);
    return n > 0.0001f ? Rotate(world, -frameYaw) * (magnitude / n) : Vec{};
}
// TR4/5 retain these classic action bits, but their full input mask is 64-bit.
constexpr uint64_t Forward = 1, Back = 2, Left = 4, Right = 8;
constexpr uint64_t Walk = 0x80, StepLeft = 0x400, StepRight = 0x800;
constexpr uint64_t Directions = Forward | Back | Left | Right | StepLeft | StepRight;
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
inline uint64_t MovementAction(Vec stick, bool preparingJump = false) {
    if (Length(stick) <= 0.0001f) return 0;
    if (std::fabs(stick.x) > std::fabs(stick.z)) {
        if (stick.x < 0) return preparingJump ? Left : StepLeft;
        return preparingJump ? Right : StepRight;
    }
    // Back alone is a hop; Walk+Back selects continuous backward walking.
    return stick.z < 0 ? (Back | Walk) : Forward;
}
inline int DirectionalRootScale(uint64_t action, bool preparingJump) {
    if (preparingJump) return 1;
    const uint64_t direction = action & Directions;
    return direction == Back || direction == StepLeft || direction == StepRight ? 3 : 1;
}
inline bool GroundGaitMatchesAction(int state, uint64_t action) {
    // Shared TR4/5 native control/collision table states. Stop/turn and the
    // outgoing gait can persist after input changes; they must finish before
    // their velocity can be used along the newly requested direction.
    switch (action & Directions) {
    case Forward: return state==0 || state==1 || state==73; // walk/run/sprint
    case Back: return state==16;              // continuous backward walk
    case StepRight: return state==21;
    case StepLeft: return state==22;
    default: return false;
    }
}
} // namespace tr::locomotion
