#pragma once

namespace tr {
void InventorySceneUpdate();
void InventorySceneShutdown();
bool InventorySceneReady();
// True only while the native level is being drawn behind the menu. The level
// uses its gameplay camera; only inventory geometry uses the menu anchor.
bool InventorySceneDrawing();
} // namespace tr
