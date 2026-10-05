#pragma once
#include <cstdint>
#include <cmath>

namespace tr::firstperson {
constexpr uint32_t HeadMeshBit = 1u << 14;
constexpr uint32_t ArmMeshBits = 0x3f00u;

inline bool UseHeadCamera(int hitPoints) { return hitPoints > 0; }

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
inline uint32_t SkinJointMask(uint32_t meshBits, const int32_t* mapping, int count) {
    uint32_t result=0;
    for (int i=0;i<count && i<32;++i) {
        const int mesh=mapping ? mapping[i] : i;
        if (mesh>=0 && mesh<32 && (meshBits&(uint32_t(1)<<mesh)))
            result|=uint32_t(1)<<i;
    }
    return result;
}

// DrawLaraHD passes zero for an unmasked body. Its effective native mask is
// then all meshes, regardless of the persistent ITEM_INFO::mesh_bits value.
// Restrict that draw locally so a stale item mask cannot remove hanging hands.
inline uint32_t HdDrawMeshBits(uint32_t itemBits, bool nativeMaskedPass,
                               bool ledgeArmsOnly, bool hideArms=false, bool hideHead=true) {
    return (nativeMaskedPass ? itemBits : UINT32_MAX) &
           (ledgeArmsOnly ? ArmMeshBits :
            ~(hideHead ? HeadMeshBit : 0u) & ~(hideArms ? ArmMeshBits : 0u));
}
} // namespace tr::firstperson
