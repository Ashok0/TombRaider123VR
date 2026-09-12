// Gamepad.h -- present the Oculus Touch controllers to the game as an Xbox pad.
//
// The game resolves XInput dynamically:
//
//     hMod = LoadLibraryA("xinput1_3.dll");
//     if (hMod || (hMod = LoadLibraryA("xinput9_1_0.dll"), hMod)) {
//         _XInputGetState = GetProcAddress(hMod, "XInputGetState");
//         _XInputSetState = GetProcAddress(hMod, "XInputSetState");
//     }
//
// so there is no import to hook -- but there IS a function-pointer global, the
// same shape as glUniformMatrix4fv. Overwriting _XInputGetState makes the game
// call us, with no code patching and no ViGEm-style virtual pad driver.
//
// inputUpdate() polls pads 0..3 every frame and, the moment a poll returns
// ERROR_SUCCESS with any button or stick active, sets app.input_type =
// INPUT_TYPE_XB. So simply answering the poll switches the game to the Xbox
// control scheme and prompts on its own.
//
// The state we return is ordinary XInput: standard wButtons bits, sticks read
// from the high byte, triggers compared against 0x1e. Nothing bespoke.
#pragma once

#include <cstdint>

namespace tr {

// Install the pointer override. Idempotent and cheap; call once per frame so it
// survives the game re-resolving XInput, and so it self-heals if we install
// before WinMain has run.
void GamepadUpdate();

// Undo the override.
void GamepadShutdown();

} // namespace tr
