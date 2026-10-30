// Pure Quest/OpenVR -> Tomb Raider first-person hand maths. Kept separate from
// runtime hooks so the axis/sign conventions can be regression-tested.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace tr::motiongun {

struct Vec { float x, y, z; };
struct Basis { float r[3][3]; };
struct Frame { Basis basis; Vec origin; };
struct Calibration {
    float rightMetres=0, raiseMetres=-.06985f, gripForwardMetres=.1778f;
    float pitchDegrees=-30, yawDegrees=0, rollDegrees=0;
};

// Ctrl+F1..F7 commands; adding Shift selects angles/restore.
inline void AdjustCalibration(Calibration& c, int command) {
    constexpr float inchStep=.00635f; // quarter inch per press
    switch (command) {
    case 0: c.rightMetres-=inchStep; break;
    case 1: c.rightMetres+=inchStep; break;
    case 2: c.raiseMetres-=inchStep; break;
    case 3: c.raiseMetres+=inchStep; break;
    case 4: c.gripForwardMetres+=inchStep; break; // mesh backward
    case 5: c.gripForwardMetres-=inchStep; break; // mesh forward
    case 7: c.yawDegrees-=1; break;
    case 8: c.yawDegrees+=1; break;
    case 9: c.pitchDegrees-=1; break;
    case 10: c.pitchDegrees+=1; break;
    case 11: c.rollDegrees-=1; break;
    case 12: c.rollDegrees+=1; break;
    }
    c.rightMetres=std::fmax(-.5f,std::fmin(.5f,c.rightMetres));
    c.raiseMetres=std::fmax(-.5f,std::fmin(.5f,c.raiseMetres));
    c.gripForwardMetres=std::fmax(-.5f,std::fmin(.5f,c.gripForwardMetres));
    c.pitchDegrees=std::fmax(-90.f,std::fmin(90.f,c.pitchDegrees));
    c.yawDegrees=std::fmax(-90.f,std::fmin(90.f,c.yawDegrees));
    c.rollDegrees=std::fmax(-180.f,std::fmin(180.f,c.rollDegrees));
}

struct CalibrationKeys {
    bool captured[7]={};
    bool controlCaptured=false;
    bool ControlEvent(bool down, bool active, bool alt) {
        if (!down) {
            const bool used=controlCaptured; controlCaptured=false; return used;
        }
        if (controlCaptured || (active && !alt)) {
            controlCaptured=true; return true;
        }
        return false;
    }
    // Caller supplies actual key transitions, not render-rate polling.
    bool Event(int key, bool down, bool control, bool shift, bool alt,
               bool active, bool repeat, int& command) {
        command=-1;
        if (key<0 || key>=7) return false;
        if (!down) { const bool used=captured[key]; captured[key]=false; return used; }
        if (repeat && captured[key]) return true;
        if (!active || !control || alt || repeat) return false;
        captured[key]=true;
        command=key+(shift ? 7 : 0);
        return true;
    }
};

inline Vec Add(Vec a, Vec b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec Sub(Vec a, Vec b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec Scale(Vec a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline float Dot(Vec a, Vec b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec Cross(Vec a, Vec b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
// Per-hand assist, never a head/body-aim override. The caller supplies only
// a living, unobstructed target. Zero disables assistance.
inline bool AssistedDirection(Vec muzzle, Vec barrel, Vec target, Vec& result,
                              float coneDegrees=12.0f) {
    const Vec delta=Sub(target,muzzle);
    const float length2=Dot(delta,delta), barrel2=Dot(barrel,barrel);
    if (!std::isfinite(coneDegrees) || coneDegrees<=0 || coneDegrees>60 ||
        !std::isfinite(length2) || !std::isfinite(barrel2) ||
        length2<1.f || length2>8192.f*8192.f || barrel2<.0001f) return false;
    const Vec direction=Scale(delta,1.f/std::sqrt(length2));
    const float cosCone=std::cos(coneDegrees*(3.14159265358979323846f/180.0f));
    if (Dot(direction,barrel)/std::sqrt(barrel2)<cosCone) return false;
    result=direction;
    return true;
}
inline Vec CollisionEndpoint(bool confirmedHit, Vec nativeEnd, Vec muzzle, Vec ray) {
    // A confirmed sphere hit uses a SHORT segment ending before the sphere.
    // Extending it changes GetTargetOnLOS's result and bypasses HitTarget.
    return confirmedHit ? nativeEnd : Add(muzzle,Scale(ray,20480.f));
}
inline Vec Transform(const Basis& m, Vec p) {
    return {m.r[0][0]*p.x + m.r[0][1]*p.y + m.r[0][2]*p.z,
            m.r[1][0]*p.x + m.r[1][1]*p.y + m.r[1][2]*p.z,
            m.r[2][0]*p.x + m.r[2][1]*p.y + m.r[2][2]*p.z};
}
inline Basis Multiply(const Basis& a, const Basis& b) {
    Basis result{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                result.r[row][col] += a.r[row][k] * b.r[k][col];
    return result;
}
// DrawGunFlash receives Lara's interpolated wrist matrix in world axes. Move
// its origin by the same world-space correction as the HD hand palette and
// replace its axes with the controller gun axes. Native flash
// offsets and spin are still applied by DrawGunFlash itself.
inline bool RetargetFlashMatrix(int32_t* wrist, const Basis& gun, Vec handDelta) {
    constexpr float fixed=16384.f;
    const float displacement[3]={handDelta.x,handDelta.y,handDelta.z};
    int32_t result[12];
    for (int row=0; row<3; ++row) {
        for (int col=0; col<3; ++col) {
            const double value=double(gun.r[row][col])*fixed;
            if (!std::isfinite(value) ||
                value<std::numeric_limits<int32_t>::min() ||
                value>std::numeric_limits<int32_t>::max()) return false;
            result[row*4+col]=int32_t(std::llround(value));
        }
        const double value=double(wrist[row*4+3])+
                           double(displacement[row])*fixed;
        if (!std::isfinite(value) ||
            value<std::numeric_limits<int32_t>::min() ||
            value>std::numeric_limits<int32_t>::max()) return false;
        result[row*4+3]=int32_t(std::llround(value));
    }
    for (int i=0;i<12;++i) wrist[i]=result[i];
    return true;
}

inline bool IsShotgunEffectQuery(uint64_t caller, const uint32_t (&returns)[4],
                                 bool lara, int weapon, int joint) {
    if (!lara || weapon!=4 || joint!=10 || !caller) return false;
    for (uint32_t address : returns)
        if (address && caller==address) return true;
    return false;
}

// Shotguns spawn world-space sparks/smoke during FireShotgun, outside the
// DrawGunFlash path used by pistols. Transform BOTH the origin and the second
// point defining particle velocity through the same tracked wrist frame.
// Native HD barrel offsets and particle spread remain owned by FireShotgun.
inline bool RetargetEffectPoint(int32_t* point, const Basis& gun, Vec hand) {
    if (!point) return false;
    const Vec world=Add(hand,Transform(gun,
        {float(point[0]),float(point[1]),float(point[2])}));
    const double coords[3]={world.x,world.y,world.z};
    int32_t result[3];
    for (int i=0;i<3;++i) {
        if (!std::isfinite(coords[i]) ||
            coords[i]<std::numeric_limits<int32_t>::min() ||
            coords[i]>std::numeric_limits<int32_t>::max()) return false;
        result[i]=int32_t(std::llround(coords[i]));
    }
    for (int i=0;i<3;++i) point[i]=result[i];
    return true;
}

// Match FireWeapon's sphere test: centre in front by more than its radius,
// and the ray passing inside the projected circle. The returned distance is
// the same conservative near endpoint used by its native obstruction test.
inline bool ShotSphere(Vec muzzle, Vec ray, Vec centre, float radius, float& distance) {
    const Vec delta=Sub(centre,muzzle);
    const float ray2=Dot(ray,ray);
    if (!(radius>0) || !std::isfinite(ray2) || ray2<.0001f) return false;
    const float along=Dot(delta,ray)/std::sqrt(ray2);
    const float across2=Dot(delta,delta)-along*along;
    if (!std::isfinite(along) || along<=radius || across2>=radius*radius) return false;
    distance=along-radius;
    return true;
}

inline Vec Transform(const Frame& m, Vec p) {
    return Add(Transform(m.basis, p), m.origin);
}
inline Frame Multiply(const Frame& a, const Frame& b) {
    return {Multiply(a.basis, b.basis), Transform(a, b.origin)};
}
inline Frame ReadRows(const float* p) {
    return {{{{p[0],p[1],p[2]}, {p[4],p[5],p[6]}, {p[8],p[9],p[10]}}},
            {p[3],p[7],p[11]}};
}
inline void WriteRows(const Frame& m, float* p) {
    for (int row=0; row<3; ++row)
        for (int col=0; col<3; ++col) p[row*4+col]=m.basis.r[row][col];
    p[3]=m.origin.x; p[7]=m.origin.y; p[11]=m.origin.z;
}
inline bool Inverse(const Frame& m, Frame& out) {
    const auto& a=m.basis.r;
    const Vec rows[3]={{a[0][0],a[0][1],a[0][2]},
                       {a[1][0],a[1][1],a[1][2]},
                       {a[2][0],a[2][1],a[2][2]}};
    const Vec cof[3]={Cross(rows[1],rows[2]), Cross(rows[2],rows[0]),
                      Cross(rows[0],rows[1])};
    const float det=Dot(rows[0],cof[0]);
    if (!std::isfinite(det) || std::fabs(det)<1e-8f) return false;
    for (int col=0; col<3; ++col) {
        out.basis.r[0][col]=cof[col].x/det;
        out.basis.r[1][col]=cof[col].y/det;
        out.basis.r[2][col]=cof[col].z/det;
    }
    out.origin=Scale(Transform(out.basis,m.origin),-1);
    return std::isfinite(out.origin.x) && std::isfinite(out.origin.y) &&
           std::isfinite(out.origin.z);
}

// GetJoints returns skinning palettes, NOT joint frames: P = J * inverseBind.
// Recover J before computing the rigid correction for the whole masked draw.
inline bool PaletteCorrection(const Frame& palette, const Frame& inverseBind,
                              const Frame& desired, Frame& correction) {
    Frame bind{}, inverseJoint{};
    if (!Inverse(inverseBind,bind) ||
        !Inverse(Multiply(palette,bind),inverseJoint)) return false;
    correction=Multiply(desired,inverseJoint);
    return true;
}

// The HD mesh input axes are converted by GetJoints: col0, -col2, col1.
inline Frame HdInverseBind(Frame bind) {
    for (int row=0; row<3; ++row) {
        const float y=bind.basis.r[row][1];
        bind.basis.r[row][1]=-bind.basis.r[row][2];
        bind.basis.r[row][2]=y;
    }
    return bind;
}

// Native barrel axis is +Y, not the wrist-to-muzzle displacement. The latter
// includes the barrel's height/side offset and must not steer the gun.
inline Basis GunBasis(const Basis& controller) {
    const Basis barrelToController{{{1,0,0},{0,0,-1},{0,1,0}}};
    return Multiply(controller,barrelToController);
}
inline Basis CalibratedController(const Basis& controller, const Calibration& c) {
    constexpr float radians=.01745329251994329577f;
    const float p=c.pitchDegrees*radians, y=c.yawDegrees*radians, r=c.rollDegrees*radians;
    const float cp=std::cos(p),sp=std::sin(p),cy=std::cos(y),sy=std::sin(y);
    const float cr=std::cos(r),sr=std::sin(r);
    const Basis pitch{{{1,0,0},{0,cp,-sp},{0,sp,cp}}};
    const Basis yaw{{{cy,0,sy},{0,1,0},{-sy,0,cy}}};
    const Basis roll{{{cr,-sr,0},{sr,cr,0},{0,0,1}}};
    return Multiply(controller,Multiply(yaw,Multiply(pitch,roll)));
}
// Calibrate the mesh's grip point, not a post-rotation world offset.
// Native local +Y is barrel-forward. Mapping this local point to the tracked
// controller keeps the visible grip stationary while the wrist rotates.
inline Frame GripFrame(const Basis& gun, Vec controllerPosition,
                       float gripForwardUnits, float raiseUnits=0, float rightUnits=0) {
    // Native +Z maps to controller-up. Negate the local grip Z to raise the
    // mesh while keeping the calibrated grip point on the controller.
    return {gun, Sub(controllerPosition,Transform(gun,{-rightUnits,gripForwardUnits,-raiseUnits}))};
}
inline Vec MuzzleLocal(int weapon, int hand, int game=0) {
    // HD flash offsets (hand 0=left, 1=right). TR1/2 weapon 2 is dual;
    // TR3 weapon 2 is the single Desert Eagle.
    switch (weapon) {
    case 2: return game==2 ? Vec{-20,215,52} :
        Vec{hand ? -11.f : 10.f,game==1 ? 213.f : 185.f,game==1 ? 50.f : 33.f};
    case 3: return game==2 ? Vec{hand ? -16.f : 14.f,206,40} :
        Vec{hand ? -11.f : 10.f,208,38};
    case 4: return {0,228,32}; // FireShotgun's barrel/smoke origin
    case 5: return game==2 ? Vec{0,270,78} : Vec{0,335,93};
    case 6: return game==1 ? Vec{0,203,97} : Vec{0,228,72};
    case 7: return game==1 ? Vec{-2,368,92} : Vec{0,180,80};
    case 8: return {-2,593,141};
    default: return {hand ? -11.f : 10.f,game==0 ? 182.f : game==1 ? 183.f : 180.f,
                    game==0 ? 33.f : game==1 ? 35.f : 30.f};
    }
}

inline Vec HandInWorld(Vec camera, float right, float down, float forward,
                       float heading, float unitsPerMetre) {
    const float c = std::cos(heading), s = std::sin(heading);
    return {camera.x + unitsPerMetre*(c*right + s*forward),
            camera.y + unitsPerMetre*down,
            camera.z + unitsPerMetre*(-s*right + c*forward)};
}

// Third row of TR4/5 phd_GenerateW2V: this is the LOS direction used by
// FireWeapon, with game-positive pitch pointing upward (negative Y).
inline Vec ShotForward(float yaw, float pitch) {
    const float flat = std::cos(pitch);
    return {std::sin(yaw)*flat, -std::sin(pitch), std::cos(yaw)*flat};
}
inline bool DirectionAngles(Vec direction,float& yaw,float& pitch) {
    const float length2=Dot(direction,direction);
    if (!std::isfinite(length2) || length2<.0001f) return false;
    const float flat=std::hypot(direction.x,direction.z);
    // Straight up/down is a valid shot (not tracking loss).
    yaw=std::atan2(direction.x,direction.z);
    pitch=std::atan2(-direction.y,flat);
    return true;
}

// OpenVR's -Z forward/Y up becomes TR's +Z forward/Y down. The physical
// controller's +Z basis is consequently the ray Lara should fire along.
inline Basis ControllerBasis(const float (&m)[3][4], float heading) {
    Basis controller{};
    const float c = std::cos(heading), s = std::sin(heading);
    for (int j = 0; j < 3; ++j) {
        const float sign = j == 0 ? 1.0f : -1.0f;
        const float x = m[0][j]*sign, y = -m[1][j]*sign,
                    z = -m[2][j]*sign;
        controller.r[0][j] = c*x + s*z;
        controller.r[1][j] = y;
        controller.r[2][j] = -s*x + c*z;
    }
    return controller;
}

} // namespace tr::motiongun
