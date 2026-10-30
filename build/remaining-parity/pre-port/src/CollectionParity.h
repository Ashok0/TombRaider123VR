#pragma once
namespace tr {
void CollectionUpdate();
void CollectionShutdown();
bool InventorySceneDrawing();
bool InventorySceneReady();
// Last gameplay camera captured by the shared native camera hook, in either view mode.
bool InventorySceneCameraReady();
bool CollectionFlareReady();
}
