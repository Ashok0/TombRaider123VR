#pragma once
#include <cstdint>
#include <cmath>

namespace tr::firstperson {
constexpr uint32_t HeadMeshBit = 1u << 14;
constexpr uint32_t ArmMeshBits = 0x3f00u;
constexpr uint32_t HandMeshBits = 0x2400u;

// Hold the draw restriction through the first standing palettes after a
// mount/landing. Rendering and simulation can change state within one pose.
struct BodyVisibilityRecovery {
    const void* owner=nullptr;
    int level=-1;
    uint32_t mask=UINT32_MAX;
    double until=0,last=0;
    uint32_t Update(const void* item,int currentLevel,double now,bool armsOnly,
                    bool landing,bool recovering) {
        if(owner!=item || level!=currentLevel || now<last) *this={};
        owner=item;level=currentLevel;last=now;
        if(armsOnly || landing) {
            mask=armsOnly ? ArmMeshBits : ~(1u<<7); // Chest mesh.
            until=now+.35;
        } else if(now>=until && !recovering) mask=UINT32_MAX;
        return mask;
    }
};

inline bool UseHeadCamera(int hitPoints) { return hitPoints > 0; }

inline bool SwitchUsesThirdPerson(int game,int state) {
    if(game!=0 && game!=1) return false;
    // Native TR4/5 switch-use states: on/off (also jump/crowbar/block
    // switches), turn wheel, cog, rail lever and pulley. Underwater switches
    // already use the swimming camera gate. Crow/dove is specific to TR5.
    switch(state) {
    case 40: case 41: case 95: case 96: case 97: case 104: return true;
    case 126: return game==1;
    default: return false;
    }
}

// Unarmed ground-movement arms are revealed only by looking down. Separate
// show/hide thresholds avoid flickering at the edge of a downward glance.
struct UnarmedArmVisibility {
    bool lookingDown=false;
    bool Hide(bool eligible,float pitch) {
        if (!eligible) { lookingDown=false; return false; }
        if (!std::isfinite(pitch)) { lookingDown=false; return true; }
        constexpr float radians=3.14159265358979323846f/180.f;
        if (pitch<=-15.f*radians) lookingDown=true;
        else if (pitch>=-10.f*radians) lookingDown=false;
        return !lookingDown;
    }
};

// HD palette slots are mapped to classic mesh bits by each outfit.
inline uint64_t SkinJointMask(uint32_t meshBits, const int32_t* mapping, int count) {
    uint64_t result=0;
    for (int i=0;i<count && i<64;++i) {
        const int mesh=mapping ? mapping[i] : i;
        if (mesh>=0 && mesh<32 && (meshBits&(uint32_t(1)<<mesh)))
            result|=uint64_t(1)<<i;
    }
    return result;
}

// DrawLaraHD passes zero for an unmasked body. Its effective native mask is
// then all meshes, regardless of the persistent ITEM_INFO::mesh_bits value.
// Restrict that draw locally so a stale item mask cannot remove hanging hands.
inline uint32_t HdDrawMeshBits(uint32_t itemBits, bool nativeMaskedPass,
                               bool ledgeArmsOnly) {
    return (nativeMaskedPass ? itemBits : UINT32_MAX) &
           (ledgeArmsOnly ? ArmMeshBits : ~HeadMeshBit);
}
} // namespace tr::firstperson
