#pragma once
#include <cstdint>

namespace tr::firstperson {
constexpr uint32_t HeadMeshBit = 1u << 14;
constexpr uint32_t ArmMeshBits = 0x3f00u;

inline bool UseHeadCamera(int hitPoints) { return hitPoints > 0; }

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
                               bool ledgeArmsOnly) {
    return (nativeMaskedPass ? itemBits : UINT32_MAX) &
           (ledgeArmsOnly ? ArmMeshBits : ~HeadMeshBit);
}
} // namespace tr::firstperson
