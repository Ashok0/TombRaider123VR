#pragma once
#include <cstdint>
namespace tr {
void VRMenuUpdate();
void VRMenuShutdown();
// Called before native chords/menus. True consumes the whole controller sample.
bool VRMenuInput(uint16_t buttons,int16_t leftX,int16_t leftY);
}
