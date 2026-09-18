#pragma once
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <windows.h>
#include <unknwn.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
struct XRBridge {
  XrInstance instance{};
  XrSystemId system{};
  XrSession session{};
  XrSpace local{}, head{};
  XrSwapchain chains[2]{};
  std::vector<XrSwapchainImageOpenGLKHR> images[2];
  // Cinema: a single mono swapchain carrying the flat game image for menus,
  // cutscenes and anything else drawn outside the hooked gameplay loop.
  XrSwapchain cinemaChain{};
  std::vector<XrSwapchainImageOpenGLKHR> cinemaImages;
  int cinemaW = 0, cinemaH = 0;
  int64_t swapFormat = 0;
  // Options panel: its own quad layer, drawn over whatever else is submitted.
  XrSwapchain menuChain{};
  std::vector<XrSwapchainImageOpenGLKHR> menuImages;
  XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  int menuW = 0, menuH = 0;
  unsigned int menuTexture = 0;
  int menuTexW = 0, menuTexH = 0;
  XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  XrPosef center{{0, 0, 0, 1}, {0, 0, 0}};
  XrPosef origin{{0, 0, 0, 1}, {0, 0, 0}};
  XrPosef screen{{0, 0, 0, 1}, {0, 0, 0}};
  XrPosef menuPose{{0, 0, 0, 1}, {0, 0, 0}};
  float cinemaDistance = 2.5f, cinemaWidth = 3.f;
  float menuDistance = 1.2f, menuWidth = .9f;
  HGLRC context{}, gameContext{};
  HDC dc{};
  bool running = false, begun = false, valid = false, recenterPending = true, failed = false;
  bool screenPending = true, menuPending = true;
  int widths[2]{}, heights[2]{};
  unsigned int readFB{}, drawFB{};
  unsigned long long submitted = 0;
  bool initialize();
  bool begin(bool render);
  void finish(const unsigned int *textures, int w, int h, const int* sourceWidths=nullptr, const int* sourceHeights=nullptr);
  void refreshResolution();
  bool present(XrSwapchain &chain, std::vector<XrSwapchainImageOpenGLKHR> &imgs, int &cw, int &ch,
               unsigned int texture, int w, int h, const char *what);
  bool stageMenu();
  void finishCinema(unsigned int texture, int w, int h);
  void shutdown();
  void recenter() { recenterPending = screenPending = menuPending = true; }
};
