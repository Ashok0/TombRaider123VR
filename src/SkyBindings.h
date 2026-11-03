#pragma once
#include <cstdint>
namespace tr::sky {
// ins_mesh called from the Classic horizon branch of DrawRooms, plus S_CmdFlush.
// RVAs verified against each supported DLL, including the exact E8 call site.
// TR1 has no separate Classic horizon mesh; its DrawSkyHD hook remains sufficient.
struct Binding { uint32_t timestamp,entry,skyReturn,flush; };
constexpr Binding Bindings[] = {
    {0x6A4B4915,0x9BE00,0x148DC,0x8F740},
    {0x6A4B490D,0xE60F0,0x29B9E,0xD3AA0},
    {0x6A4B7C3F,0x9BC80,0x148EC,0x8F210},
    {0x6A4B7C37,0xE7DC0,0x29C5E,0xD5340},
    {0x6AA15696,0x9BAE0,0x143BC,0x8F080},
    {0x6AA1568D,0xE8AA0,0x29D6E,0xD6010},
};
inline const Binding* Find(uint32_t timestamp) {
    for(const auto& binding:Bindings) if(binding.timestamp==timestamp) return &binding;
    return nullptr;
}
} // namespace tr::sky
