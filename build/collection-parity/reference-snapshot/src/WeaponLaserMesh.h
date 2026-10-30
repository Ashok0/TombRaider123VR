#pragma once
#include "MotionGunMath.h"

namespace tr::weaponlaser {
struct Model {
    int handIndex,weapon,game; // HAND_NAMES index within each 30-entry skin bank; -1 = both games
    motiongun::Vec front,rear;
    bool HasLaser() const { return handIndex==8 || handIndex==10 || handIndex==14; }
};
// Measured bore/bolt-guide and laser tube centres in shipped TRM mesh
// coordinates, fully weighted to wrist 10. Bare weapons need their own firing
// calibration too; disabling/removing a laser must never restore generic aim.
// End-cap bevel normals are NOT necessarily the tube's forward direction.
// HK has no laser attachment; retain its muzzle calibration for firing only.
// tools/verify_weapon_laser_mesh.py checks these against all three skin banks.
inline constexpr Model models[]={
    {10,2,-1,{56.523163f,82.748322f,59.557335f},{56.523163f,42.401154f,50.810116f}}, // Revolver
    {8,2,1,{57.436356f,93.415329f,62.074982f},{59.092934f,37.299196f,54.869764f}}, // Desert Eagle
    {14,6,0,{54.392275f,231.287956f,62.011333f},{56.894369f,175.023102f,62.011326f}}, // Crossbow
    {15,5,1,{61.939919f,245.826019f,73.651966f},{61.947754f,243.269135f,73.208870f}}, // HK
    {9,2,-1,{56.518992f,131.135363f,52.376946f},{56.521458f,119.013145f,49.723663f}}, // Bare Revolver
    {7,2,1,{57.860348f,130.785316f,44.147461f},{57.860348f,129.424263f,43.970840f}}, // Bare Desert Eagle
    {13,6,0,{53.764875f,245.396378f,33.159723f},{54.441923f,230.171547f,33.159721f}}, // Bare Crossbow
};
inline const Model* Find(int handIndex,int game,int weapon) {
    if(game<0 || game>1) return nullptr;
    for(const auto& m:models)
        if(m.handIndex==handIndex && m.weapon==weapon && (m.game<0 || m.game==game)) return &m;
    return nullptr;
}
} // namespace tr::weaponlaser
