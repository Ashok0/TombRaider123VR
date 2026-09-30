#pragma once
#include <cstdint>

namespace tr::firstperson {
constexpr uint32_t HeadMeshBit = 1u << 14;
constexpr uint32_t ArmMeshBits = 0x3f00u;

// DrawLaraHD passes zero for an unmasked body. Its effective native mask is
// then all meshes, regardless of the persistent ITEM_INFO::mesh_bits value.
// Restrict that draw locally so a stale item mask cannot remove hanging hands.
inline uint32_t HdDrawMeshBits(uint32_t itemBits, bool nativeMaskedPass,
                               bool ledgeArmsOnly) {
    return (nativeMaskedPass ? itemBits : UINT32_MAX) &
           (ledgeArmsOnly ? ArmMeshBits : ~HeadMeshBit);
}
} // namespace tr::firstperson
