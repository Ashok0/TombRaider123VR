#pragma once
#include <cstdint>
namespace tr {
void VRMenuUpdate();
void VRMenuShutdown();
// Native title-menu adapters select this entry using the game's normal input.
bool VRMenuOpen();
bool VRMenuIsOpen();
bool VRMenuBlocksInput();
// Called before native chords/menus. True consumes the whole controller sample.
bool VRMenuInput(uint16_t buttons,int16_t leftX,int16_t leftY);
}
