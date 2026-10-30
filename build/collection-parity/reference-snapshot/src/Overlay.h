// Overlay.h -- switch off the engine's optic overlays: the binocular vignette,
// the scope frame, the VCI visor, the Labyrinth fisheye.
//
// WHY THEY DO NOT WORK IN VR
//
// All four are flat, full-screen artwork. DrawBinoculars scales the mesh to
// phd_scr_left..right / top..bottom and writes it into raw_vbuf with z = 0, so
// each one is a 2D quad covering the whole screen -- and the mod's 2D path puts
// that on the world-locked panel at HudDepthMetres. A vignette is meant to be
// the edge of your vision; as a rectangle floating at 4 m in the middle of a
// 94-degree field of view it is a picture of a vignette, sitting in space, with
// your actual peripheral vision wide open around it. There is no placement that
// fixes that, because the thing it imitates is the headset's own field stop.
//
// WHAT IS KEPT
//
// The aiming dot, which is NOT part of any of them. DrawBinoculars draws it
// separately after DrawGameInfo -- a DefaultSprites sprite at the centre of the
// screen rect, gated on LaserSightActive and coloured through LaserSightCol so
// it still turns green on a target and still pulses. Killing the four mesh
// overlays leaves it untouched, which is the whole reason this is worth doing at
// all rather than suppressing DrawBinoculars wholesale.
//
// DoInfraRedQuad is the fifth, and it is the one that is easy to miss. It draws
// a single untextured quad over the whole screen rect with vertex colour
// 0xFF5050FF -- a red wash, and DrawBinoculars calls it whenever LaserSight is
// set, not just in infra-red mode. That is the transparent red rectangle sitting
// behind the laser dot, and in VR it is a red pane floating at HudDepthMetres,
// so hideOpticsTint stubs it too. The same function is what tints the VCI
// headset's infra-red mode, so that tint goes with it -- the engine draws both
// through this one function, and the setting says so.
//
// HOW
//
// Not a hook. Each of the four is a void function whose return value no caller
// reads, so one byte -- 0xC3, `ret` -- at its entry is the entire change. No
// trampoline, no stolen instruction window, no detour to get right, and
// restoring it is a single byte back. The five prologue bytes are verified
// before the write exactly as InlineHook verifies its own, so a wrong address
// declines to patch and says so rather than corrupting an instruction stream.
#pragma once

namespace tr {

// Apply or drop the stubs to match the DLL currently bound and the ini. Cheap
// and idempotent; call once per frame, after GameDllUpdate().
void OverlayUpdate();

// Restore every patched byte. Called from RemoveHooks.
void OverlayShutdown();

} // namespace tr
