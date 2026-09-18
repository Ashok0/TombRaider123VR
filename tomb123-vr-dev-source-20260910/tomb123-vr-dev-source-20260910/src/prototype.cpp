#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gl/GL.h>
#include <share.h>
#include "vr_menu.h"
#include "xr_bridge.h"
#include "xr_math.h"
#include "classic_math.h"
#include "firstperson_math.h"
#include "firstperson_mesh.h"
#include "native_eye.h"
#include <map>
void log(const char *, ...);
static FILE *logFile;
static HMODULE self;
static XRBridge xr;
static bool xrRequested = false, xrEyeFrame = false, worldCamera = false, inDraw = false;
static int forceCinema = 0;
enum CameraMode { CameraClassic, CameraShoulder, CameraFirstPerson, CameraModeCount };
static const char *cameraModeNames[] = {"Classic", "Over the shoulder", "First person"};
static int cameraMode = CameraClassic;
static float shoulderDistance = 512.f, shoulderRight = 160.f, shoulderUp = 64.f;
static float eyeHeight = 650.f, eyeForward = 0.f;
static VRMenu menu;
static float unitsPerMeter = 512.f;
using UniformMatrix = void(APIENTRY *)(GLint, GLsizei, GLboolean, const GLfloat *);
static UniformMatrix realUniformMatrix;
static std::map<GLuint, GLint> projectionLocations;
static unsigned char *tr;
// Per-game addresses. TR1 and TR2 are the same engine at different offsets, so
// every module-relative address the hooks touch is gathered here rather than
// spelled out at the use site. Derived and cross-checked in
// research/tr2-addresses.md; a zero disables the feature that needs it instead
// of patching an address that was never confirmed.
struct Profile {
  const wchar_t *module;
  const char *name;
  uint32_t imageSize;
  size_t drawScene, drawCall, generateW2V, cameraCall;
  size_t interp, ticks, eyeCenterX, eyeCenterY, w2v, viewScale;
  size_t laraItem, apiTable, cameraType, cameraFrames;
  size_t meshTable, objectTable;
  size_t laraDraw, laraDrawCall, meshDraw, meshDrawCall, hairDraw, hairDrawCall, skinnedDraw;
  const size_t *skinnedCalls; // Zero-terminated; head-filtered submissions.
  const size_t *faceCalls;    // Zero-terminated; dedicated face/eyewear meshes.
};
static const size_t tr1Skinned[] = {0x6deda, 0x6e04a, 0x6e160, 0x6e261,
                                    0x6e355, 0x6e426, 0x6e5d5, 0};
static const size_t tr1Face[] = {0x6e761, 0x6e842, 0};
static const size_t tr2Skinned[] = {0x9fc6a, 0x9fdda, 0x9fef0, 0x9fff1,
                                    0xa00e5, 0xa01b6, 0xa0365, 0};
static const size_t tr2Face[] = {0xa04f1, 0xa05d2, 0};
static const Profile profiles[] = {
    {L"tomb1.dll", "TR1", 0x505000,
     0xd6f0, 0x17429, 0x73a40, 0x635b3,
     0x2c9b38, 0x2c9b60, 0x2c9b64, 0x2c9b68, 0x2c9b80, 0x2c9bb0,
     0x33ccf0, 0x41efd0, 0x41eec0, 0x41eed0,
     0x33cba8, 0x41f038,
     0xfd00, 0xda5b, 0x6a300, 0xff07, 0x553e0, 0x10044, 0x6be70,
     tr1Skinned, tr1Face},
    // TR2 uses the same item/bone layout, but its classic mesh draw takes
    // a second argument. Preserve it through classicHeadHookTR2.
    {L"tomb2.dll", "TR2", 0x545000,
     0x145d0, 0x25ee8, 0xa7dc0, 0x949b3,
     0x307cd8, 0x307d08, 0x307c94, 0x307c98, 0x307ca0, 0x307cd0,
     0x37ae30, 0x433130, 0x4330c0, 0x4330d0,
     0x37ace8, 0x45d1b8,
     0x17b00, 0x14a7b, 0x9bc80, 0x17cfc, 0x87650, 0x18032, 0x9d890,
     tr2Skinned, tr2Face},
    // TR3 head hiding is likewise unmapped; its draw call site sits at a different
    // offset inside its containing function than TR1's, so it was resolved by
    // elimination among drawScene's callers rather than by a fixed offset.
    {L"tomb3.dll", "TR3", 0x5a8000,
     0x29940, 0x3d279, 0xf5d60, 0xe0c43,
     0x361b64, 0x361b54, 0x361b90, 0x361b94, 0x361bc0, 0x361b98,
     0x3d4d30, 0x494eb8, 0x494f00, 0x494f10,
     0x3d4be8, 0,
     0, 0, 0, 0, 0xd04d0, 0, 0xe99d0,
     nullptr, nullptr},
};
static const Profile *game = &profiles[0];
constexpr size_t profileCount = sizeof(profiles) / sizeof(*profiles);
// Resolved per game, because more than one game DLL can be hooked in a single
// session: the originals a hook has to call through to differ per module.
struct GameHooks {
  unsigned char *base;
  int (*draw)();
  void (*camera)(void *);
  void (*laraDraw)(unsigned char *);
  void (*meshDraw)(void *);
  void (*hairDraw)();
  void (*skinnedDraw)(unsigned char *, int);
  bool headHooks;
};
static GameHooks hooks[profileCount]{};
static bool enabled = false, inStereo = false;
static int eye = -1;
static uint64_t frames = 0, swaps = 0;
static float tx = 0, ty = 0, tz = 0, pitch = 0, yaw = 0, roll = 0, separation = 32;
static bool keyPrev[256]{};
using Swap = BOOL(WINAPI *)(HDC);
static Swap realSwap;
using Draw = int (*)();
static Draw realDraw;
using Camera = void (*)(void *);
static Camera realCamera;
using Tick = int (*)(void *);
static Tick realTick;
using Scissor=void(*)(int,int,int,int);static Scissor realScissor;
static bool classicEye=false;
static int eyeRenderWidth=0,eyeRenderHeight=0,eyeFocal=0,eyeCenterX=0,eyeCenterY=0;
static int eyeInterpolation[2]{};
static uint64_t remappedScissors=0;
static bool classicFix=true;
static bool headHooksReady=false, firstPersonEye=false, laraFirstPersonPass=false;
static uint64_t hiddenClassicHeads=0, hiddenHair=0;
using LaraDraw=void(*)(unsigned char*);
using MeshDraw=void(*)(void*);
using SkinnedDraw=void(*)(unsigned char*,int);
static LaraDraw realLaraDraw;
static MeshDraw realMeshDraw;
static SkinnedDraw realSkinnedDraw;
static void (*realHairDraw)();
void laraDrawHook(unsigned char* item) {
  bool previous=laraFirstPersonPass;
  laraFirstPersonPass=headHooksReady&&firstPersonEye&&inStereo&&worldCamera&&item==*reinterpret_cast<unsigned char**>(tr+game->laraItem);
  auto api=*reinterpret_cast<unsigned char**>(tr+game->apiTable);
  auto flags=reinterpret_cast<uint32_t*>(api+0x854);
  uint32_t proximity=*flags&2u;
  // The native close-camera check hides the entire body. Keep the body visible
  // while our dedicated mesh paths remove the head only.
  if(laraFirstPersonPass)*flags&=~2u;
  realLaraDraw(item);
  if(laraFirstPersonPass)*flags=(*flags&~2u)|proximity;
  laraFirstPersonPass=previous;
}
void classicHeadHook(void* mesh) {
  if(laraFirstPersonPass&&mesh==*reinterpret_cast<void**>(tr+game->meshTable+14*8)) {++hiddenClassicHeads;return;}
  realMeshDraw(mesh);
}
void classicHeadHookTR2(void* mesh, int lighting) {
  if(laraFirstPersonPass&&mesh==*reinterpret_cast<void**>(tr+game->meshTable+14*8)) {
    ++hiddenClassicHeads;
    return;
  }
  reinterpret_cast<void(*)(void*,int)>(realMeshDraw)(mesh,lighting);
}
void hairHook() {
  if(laraFirstPersonPass){++hiddenHair;return;}
  realHairDraw();
}
void faceOnlyHook(unsigned char* item,int masked) {
  // Dedicated animated face / eyewear meshes use their own facial joints.
  // Suppress the whole submission only during Lara's first-person eye draw.
  if(laraFirstPersonPass) {
    static unsigned logged=0;
    if(logged++<4)log("FirstPerson dedicated face/eyewear draw suppressed");
    return;
  }
  realSkinnedDraw(item,masked);
}
void skinnedHeadHook(unsigned char* item,int masked) {
  bool previous=firstpersonmesh::active;
  firstpersonmesh::active=laraFirstPersonPass;
  realSkinnedDraw(item,masked);
  firstpersonmesh::active=previous;
}


using BindFB = void(APIENTRY *)(GLenum, GLuint);
static BindFB bindFB;
using GenFB = void(APIENTRY *)(GLsizei, GLuint *);
static GenFB genFB;
using Attach = void(APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
static Attach attach;
using Blit = void(APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                              GLenum);
static Blit blit;
using Check = GLenum(APIENTRY *)(GLenum);
static Check checkFB;
static GLuint textures[2]{}, cinemaTexture = 0, fb = 0;
static int width = 0, height = 0;
static bool captured[2]{};
static NativeEye nativeEyes[2];
static int captureWidths[2]{},captureHeights[2]{};
void beginNativeEye(){
  if(!xrEyeFrame)return;
  auto host=(unsigned char*)GetModuleHandleW(nullptr);
  EyeHostState state{(int*)(host+0x41e1d0),(int*)(host+0x41e1cc),
                     (int*)(host+0x269e220),(int*)(host+0x41e1ec),
                     (GLuint*)(host+0xc94d99c)};
  if(!nativeEyes[eye].begin(state,textures[eye],xr.widths[eye],xr.heights[eye])) {
    static int failures=0;if(failures++<4)log("Native eye target unavailable; falling back to window capture");
  }
}

static uint64_t cinemaFrames = 0, lastCinemaSwap = 0;
void log(const char *f, ...) {
  if (!logFile)
    return;
  va_list a;
  va_start(a, f);
  vfprintf(logFile, f, a);
  va_end(a);
  fputc('\n', logFile);
  fflush(logFile);
}
template <class T> T &at(size_t r) { return *reinterpret_cast<T *>(tr + r); }
bool edge(int k) {
  bool d = (GetAsyncKeyState(k) & 0x8000) != 0;
  bool r = d && !keyPrev[k];
  keyPrev[k] = d;
  return r;
}
float axis(int plus, int minus) {
  return float((GetAsyncKeyState(plus) & 0x8000) != 0) -
         float((GetAsyncKeyState(minus) & 0x8000) != 0);
}
void recenterAction() {
  xr.recenter();
  tx = ty = tz = pitch = yaw = roll = 0;
  log("Camera reset");
}
void closeAction() { menu.visible = false; }
static const char *const cinemaNames[] = {"Automatic", "Forced"};
static const MenuItem menuItems[] = {
    {"Camera", &cameraMode, nullptr, 1, 0, CameraModeCount - 1, cameraModeNames, nullptr, nullptr},


    {"Shoulder distance", nullptr, &shoulderDistance, 32, 0, 4096, nullptr, " u", nullptr},
    {"Shoulder right", nullptr, &shoulderRight, 16, -1024, 1024, nullptr, " u", nullptr},
    {"Shoulder up", nullptr, &shoulderUp, 16, -1024, 1024, nullptr, " u", nullptr},
    {"World scale", nullptr, &unitsPerMeter, 32, 64, 4096, nullptr, " u/m", nullptr},
    {"Screen distance", nullptr, &xr.cinemaDistance, .25f, .5f, 20, nullptr, " m", nullptr},
    {"Screen width", nullptr, &xr.cinemaWidth, .25f, .3f, 40, nullptr, " m", nullptr},
    {"Screen mode", &forceCinema, nullptr, 1, 0, 1, cinemaNames, nullptr, nullptr},
    {"Recenter view", nullptr, nullptr, 0, 0, 0, nullptr, nullptr, recenterAction},
    {"Close menu", nullptr, nullptr, 0, 0, 0, nullptr, nullptr, closeAction},
};
void controls() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  if (pid != GetCurrentProcessId())
    return;
  if (edge(VK_F6)) {
    enabled = !enabled;
    log("Stereo %s", enabled ? "ON" : "OFF");
  }
  if (edge(VK_F8)) {
    xrRequested = !xrRequested;
    if (!xrRequested)
      xr.shutdown();
    else
      xr.failed = false;
    log("OpenXR %s", xrRequested ? "requested" : "OFF");
  }
  if (edge(VK_F11)) {
    cameraMode = (cameraMode + 1) % CameraModeCount;
    log("Camera mode: %s", cameraModeNames[cameraMode]);
  }
  if (edge(VK_F9)) {
    forceCinema = !forceCinema;
    xr.screenPending = true;
    log("Cinema %s", forceCinema ? "FORCED" : "automatic");
  }
  if (edge(VK_F7))
    recenterAction();
  if (edge(VK_F10)) {
    menu.visible = !menu.visible;
    menu.dirty = true;
    if (menu.visible)
      xr.menuPending = true;
    log("Menu %s", menu.visible ? "open" : "closed");
  }
  static ULONGLONG last = GetTickCount64();
  auto now = GetTickCount64();
  float dt = std::min(float(now - last) / 1000.f, .05f);
  last = now;
  if (menu.visible) {
    if (edge(VK_NUMPAD8))
      menu.move(-1);
    if (edge(VK_NUMPAD2))
      menu.move(1);
    if (edge(VK_NUMPAD4))
      menu.adjust(-1);
    if (edge(VK_NUMPAD6))
      menu.adjust(1);
    if (edge(VK_NUMPAD5))
      menu.activate();
    return;
  }
  if (!enabled && !xrRequested)
    return;
  float speed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1024.f : 256.f;
  tx += axis(VK_NUMPAD6, VK_NUMPAD4) * speed * dt;
  ty += axis(VK_SUBTRACT, VK_ADD) * speed * dt;
  tz += axis(VK_NUMPAD8, VK_NUMPAD2) * speed * dt;
  yaw += axis(VK_NUMPAD3, VK_NUMPAD1) * 8192 * dt;
  pitch += axis(VK_NUMPAD9, VK_NUMPAD7) * 8192 * dt;
  roll += axis(VK_DIVIDE, VK_MULTIPLY) * 8192 * dt;
}
int setting(const wchar_t *path, const wchar_t *key, int fallback, int low, int high) {
  wchar_t text[32];
  // GetPrivateProfileInt is unsigned and will not parse a leading '-'.
  if (!GetPrivateProfileStringW(L"VR", key, L"", text, 32, path) || !text[0])
    return fallback;
  wchar_t *end = nullptr;
  long value = wcstol(text, &end, 10);
  return end == text ? fallback : std::clamp((int)value, low, high);
}
bool patchCall(size_t r, void *target, size_t expected) {
  auto p = tr + r;
  if (*p != 0xe8 || p + 5 + *reinterpret_cast<int32_t *>(p + 1) != tr + expected) {
    log("Call signature mismatch %zx", r);
    return false;
  }
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  unsigned char *stub = nullptr;
  uintptr_t center = (uintptr_t)p & ~(uintptr_t(si.dwAllocationGranularity) - 1);
  for (uintptr_t delta = si.dwAllocationGranularity; delta < 0x70000000 && !stub;
       delta += si.dwAllocationGranularity) {
    stub = (unsigned char *)VirtualAlloc((void *)(center + delta), si.dwAllocationGranularity,
                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!stub && center > delta)
      stub = (unsigned char *)VirtualAlloc((void *)(center - delta), si.dwAllocationGranularity,
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  }
  if (!stub)
    return false;
  stub[0] = 0xff;
  stub[1] = 0x25;
  memset(stub + 2, 0, 4);
  memcpy(stub + 6, &target, 8);
  DWORD old;
  VirtualProtect(stub, 14, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), stub, 14);
  if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
    return false;
  int32_t rel = (int32_t)(stub - (p + 5));
  memcpy(p + 1, &rel, 4);
  DWORD tmp;
  VirtualProtect(p, 5, old, &tmp);
  FlushInstructionCache(GetCurrentProcess(), p, 5);
  log("Patched call RVA %zx", r);
  return true;
}
// Lara's render pose. The engine hands the camera hook an interpolated pose, so
// Lara is interpolated with the same factor to keep the derived modes smooth.
// Her ITEM_INFO carries the current pose at +0x58 and the previous one at +0x6c.
bool laraPose(int32_t *pos, int16_t *rot) {
  auto lara = at<unsigned char *>(game->laraItem);
  if (!lara)
    return false;
  int interp = std::clamp(at<int>(game->interp), 0, 0x100);
  for (int i = 0; i < 3; i++) {
    int32_t cur, prev;
    memcpy(&cur, lara + 0x58 + 4 * i, 4);
    memcpy(&prev, lara + 0x6c + 4 * i, 4);
    pos[i] = prev + int32_t((int64_t(cur - prev) * interp) >> 8);
  }
  for (int i = 0; i < 3; i++) {
    int16_t cur, prev;
    memcpy(&cur, lara + 0x64 + 2 * i, 2);
    memcpy(&prev, lara + 0x78 + 2 * i, 2);
    int delta = (int16_t)(cur - prev); // Wraps to the shortest signed turn.
    // The previous-rotation offset is inferred from the 20-byte pose stride.
    // A real per-tick turn is small, so an implausible delta means the guess is
    // wrong for this build: fall back to the uninterpolated rotation.
    rot[i] = (delta > 0x2000 || delta < -0x2000) ? cur : (int16_t)(prev + ((delta * interp) >> 8));
  }
  return true;
}
// Rotation part of the world-to-view matrix the engine just generated. Rows are
// the view basis in world space: row 0 right, row 1 down, row 2 forward.
vrmath::M viewFrame() {
  auto m = (int32_t *)(tr + game->w2v);
  vrmath::M f;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      f.a[i][j] = float(m[i * 4 + j]) / 16384.f;
  return f;
}
void offsetPose(int32_t *pos, vrmath::M frame, XrVector3f viewSpace) {
  auto o = vrmath::mul(vrmath::transpose(frame), viewSpace);
  pos[0] += (int32_t)lroundf(o.x);
  pos[1] += (int32_t)lroundf(o.y);
  pos[2] += (int32_t)lroundf(o.z);
}
void applyCameraMode(unsigned char *pose) {
  firstPersonEye=false;
  if (cameraMode == CameraClassic)
    return;
  // CAMERA_INFO.type: 0 chase, 1 fixed, 2 look, 3 combat. Fixed and cinematic
  // cameras are placed deliberately by the level, so leave their framing alone.
  int type = at<int>(game->cameraType);
  if (type == 1 || type > 3)
    return;
  int32_t lp[3];
  int16_t lr[3];
  if (!laraPose(lp, lr))
    return;
  auto pos = (int32_t *)pose;
  auto ang = (int16_t *)(pose + 12);
  if (cameraMode == CameraFirstPerson) {
    if(!headHooksReady || !inStereo) return;
    auto lara=at<unsigned char*>(game->laraItem);
    auto api=at<unsigned char*>(game->apiTable);
    int fraction=*reinterpret_cast<int*>(api+0x374) ? 256 : std::clamp(at<int>(game->interp),0,256);
    auto previous=reinterpret_cast<int32_t*>(lara+0x490);
    auto current=reinterpret_cast<int32_t*>(lara+0xac0);
    // Refuse uninitialized bone matrices during loads; never invent a head pose.
    int64_t basis=0;for(int row=0;row<3;row++)for(int col=0;col<3;col++)basis+=std::abs(int64_t(current[row*4+col]));
    if(basis<16000 || basis>150000) return;
    firstperson::headPosition(reinterpret_cast<int32_t*>(lara+0x6c),reinterpret_cast<int32_t*>(lara+0x58),previous,current,fraction,pos);
    // Body heading provides turning; animation pitch/roll never tilt the horizon.
    ang[0]=0;ang[1]=lr[1];ang[2]=0;
    firstPersonEye=true;
    if(eye==0 && (frames<4 || frames%120==0))
      log("%s FirstPerson head=%d,%d,%d interp=%d joint=14 classicHidden=%llu hairHidden=%llu",game->name,pos[0],pos[1],pos[2],fraction,hiddenClassicHeads,hiddenHair);
    return;
  }
  realCamera(pose);
  auto frame = viewFrame();
  float dx = float(lp[0] - pos[0]), dy = float(lp[1] - pos[1]), dz = float(lp[2] - pos[2]);
  float distance = sqrtf(dx * dx + dy * dy + dz * dz);
  // Only ever move closer, along the camera's own view axis. The engine already
  // proved that ray clear of geometry, so pulling in cannot clip through walls.
  float pull = std::max(0.f, distance - shoulderDistance);
  offsetPose(pos, frame, {shoulderRight, -shoulderUp, pull});
}
// patchCall verifies that a call site really targets the function it names, so a
// wrong code address refuses to patch. Data addresses have no such protection: a
// wrong one is a silent bad write. Confirm the profile's world-to-view global
// really does hold a rotation before the rest of the data addresses are trusted.
void verifyProfileOnce() {
  static int state[profileCount]{}; // 0 unchecked, 1 plausible, 2 suspect
  size_t index = size_t(game - profiles);
  if (index >= profileCount || state[index])
    return;
  auto m = (int32_t *)(tr + game->w2v);
  double worst = 0;
  for (int i = 0; i < 3; i++) {
    double length = 0;
    for (int j = 0; j < 3; j++) {
      double v = double(m[i * 4 + j]) / 16384.0;
      length += v * v;
    }
    worst = std::max(worst, fabs(sqrt(length) - 1.0));
  }
  state[index] = worst < .05 ? 1 : 2;
  log("Profile %s world-to-view rows within %.4f of unit length: %s", game->name, worst,
      state[index] == 1 ? "OK" : "SUSPECT, data addresses for this build may be wrong");
}
void cameraHook(void *ptr) {
  alignas(4) unsigned char pose[20];
  memcpy(pose, ptr, 20);
  applyCameraMode(pose);
  if (inStereo) {
    auto angles = (int16_t *)(pose + 12);
    angles[0] = (int16_t)(angles[0] + (firstPersonEye ? 0 : int(pitch)));
    angles[1] = (int16_t)(angles[1] + (firstPersonEye ? 0 : int(yaw)));
    angles[2] = (int16_t)(angles[2] + (firstPersonEye ? 0 : int(roll)));
    realCamera(pose);
    auto m = (int32_t *)(tr + game->w2v);
    auto pos = (int32_t *)pose;
    if (xrEyeFrame) {
      vrmath::M anchor;
      for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
          anchor.a[i][j] = float(m[i * 4 + j]) / 16384.f;
      auto relative = firstPersonEye ? firstperson::eyeOffset(xr.views[eye].pose,xr.views,xr.origin) : vrmath::relativePosition(xr.views[eye].pose, xr.origin);
      relative.x = relative.x * unitsPerMeter + (firstPersonEye ? 0 : tx);
      relative.y = relative.y * unitsPerMeter + (firstPersonEye ? 0 : ty);
      relative.z = relative.z * unitsPerMeter + (firstPersonEye ? 0 : tz);
      auto offset = vrmath::mul(vrmath::transpose(anchor), relative);
      pos[0] += (int)lroundf(offset.x);
      pos[1] += (int)lroundf(offset.y);
      pos[2] += (int)lroundf(offset.z);
      vrmath::angles(vrmath::relativeView(xr.views[eye].pose, xr.origin, anchor), angles);
      // A conservative symmetric CPU frustum contains the asymmetric eye frustum.
      auto f = xr.views[eye].fov;
      float h = std::max(fabsf(tanf(f.angleLeft)), fabsf(tanf(f.angleRight)));
      float v = std::max(fabsf(tanf(f.angleUp)), fabsf(tanf(f.angleDown)));
      using Dimension = int (*)();
      auto api = at<unsigned char *>(game->apiTable);
      int w = (*(Dimension *)(api + 0x88))(), ht = (*(Dimension *)(api + 0x90))();
      at<int>(game->viewScale) = std::max(1, (int)std::min(w / (2 * h), ht / (2 * v)));
      eyeRenderWidth=w;eyeRenderHeight=ht;eyeFocal=at<int>(game->viewScale);
      eyeCenterX=at<int>(game->eyeCenterX);eyeCenterY=at<int>(game->eyeCenterY);
    } else {
      float side = (firstPersonEye ? 0 : tx) + (eye == 0 ? -.5f : .5f) * separation;
      for (int i = 0; i < 3; i++)
        pos[i] += (int32_t)std::lround((side * m[i] + (firstPersonEye ? 0 : ty) * m[4 + i] + (firstPersonEye ? 0 : tz) * m[8 + i]) / 16384.f);
    }
    if (frames < 3)
      log("eye=%d camera=%d,%d,%d angles=%d,%d,%d", eye, pos[0], pos[1], pos[2], angles[0],
          angles[1], angles[2]);
  }
  realCamera(pose);
  if(inStereo&&eye>=0)eyeInterpolation[eye]=at<int>(game->interp);
  verifyProfileOnce();
  worldCamera = true;
}
void APIENTRY uniformMatrixHook(GLint location, GLsizei count, GLboolean trans,
                                const GLfloat *value) {
  if (xrEyeFrame && worldCamera && eye >= 0 && count == 1 && !trans && fabsf(value[15]) < 0.0001f &&
      fabsf(value[11]) > .5f) {
    GLint program = 0;
    glGetIntegerv(0x8b8d, &program);
    auto it = projectionLocations.find(program);
    if (it == projectionLocations.end()) {
      using GetLocation = GLint(APIENTRY *)(GLuint, const char *);
      auto get = (GetLocation)wglGetProcAddress("glGetUniformLocation");
      it = projectionLocations.emplace(program, get(program, "uProjMatrix")).first;
    }
    if (location == it->second) {
      float modified[16];
      memcpy(modified, value, sizeof(modified));
      vrmath::projection(modified, xr.views[eye].fov);
      static int logged = 0;
      if (logged++ < 4)
        log("XR projection eye=%d original=%.3f %.3f w=%.1f new=%.3f %.3f offsets=%.3f %.3f", eye,
            value[0], value[5], value[11], modified[0], modified[5], modified[8], modified[9]);
      realUniformMatrix(location, count, trans, modified);
      return;
    }
  }
  realUniformMatrix(location, count, trans, value);
}
void scissorHook(int x,int y,int w,int h){
 // The host exposes top-left scissor coordinates; its real callback handles
 // the GL bottom-left conversion. UI and shadow projections stay unchanged.
 if(classicFix&&classicEye&&xrEyeFrame&&worldCamera&&eye>=0&&eyeFocal>0){
   auto host=(unsigned char*)GetModuleHandleW(nullptr);
   auto projection=*(const float**)(host+0xc6a2508);
   if(projection&&fabsf(projection[15])<.0001f&&fabsf(projection[11])>.5f){
     auto f=xr.views[eye].fov;
     auto r=classicvr::reproject({x,y,w,h},eyeRenderWidth,eyeRenderHeight,float(eyeFocal),float(eyeCenterX),float(eyeCenterY),tanf(f.angleLeft),tanf(f.angleRight),tanf(f.angleUp),tanf(f.angleDown));
     if(remappedScissors<4)log("Classic scissor eye=%d %d,%d %dx%d -> %d,%d %dx%d focal=%d",eye,x,y,w,h,r.x,r.y,r.w,r.h,eyeFocal);
     ++remappedScissors;realScissor(r.x,r.y,r.w,r.h);return;
   }
 }
 realScissor(x,y,w,h);
}
void renderMetrics(bool classic,int ticks){
 static ULONGLONG start=0;static unsigned count=0,interpBins=0;static int oldTicks=0;static bool previous=false;auto now=GetTickCount64();
 if(!start||previous!=classic){start=now;oldTicks=ticks;count=interpBins=0;previous=classic;}
 ++count;interpBins|=1u<<std::clamp(eyeInterpolation[0]/16,0,16);
 if(now-start>=2000){unsigned steps=0;for(unsigned bits=interpBins;bits;bits>>=1)steps+=bits&1;
 log("RenderMetrics mode=%s fix=%d fps=%.1f tickUnitsPerSec=%.1f interpBins=%u eyeInterp=%d/%d scissorRemaps=%llu",classic?"classic":"remastered",classicFix,1000.*count/(now-start),1000.*(ticks-oldTicks)/(now-start),steps,eyeInterpolation[0],eyeInterpolation[1],remappedScissors);start=now;oldTicks=ticks;count=interpBins=0;}
}
int noTick(void *) { return 0; }
// Upload in the mod context so game pixel-transfer state and binding caches
// cannot affect the panel, and panel rendering cannot disturb the game UI.
void stageMenu() {
  if(!inStereo) firstpersonmesh::clear();
  xr.menuTexture = 0;
  if (!menu.visible || !xr.context) return;
  HGLRC previous = wglGetCurrentContext();
  HDC previousDC = wglGetCurrentDC();
  if (!wglMakeCurrent(xr.dc, xr.context)) return;
  if (menu.render()) {
    xr.menuTexture = menu.texture;
    xr.menuTexW = menu.width;
    xr.menuTexH = menu.height;
    glFinish();
  }
  wglMakeCurrent(previousDC, previous);
}
bool setupGL() {
  if (bindFB)
    return true;
  bindFB = (BindFB)wglGetProcAddress("glBindFramebuffer");
  genFB = (GenFB)wglGetProcAddress("glGenFramebuffers");
  attach = (Attach)wglGetProcAddress("glFramebufferTexture2D");
  blit = (Blit)wglGetProcAddress("glBlitFramebuffer");
  checkFB = (Check)wglGetProcAddress("glCheckFramebufferStatus");
  if (!bindFB || !genFB || !attach || !blit || !checkFB) {
    bindFB = nullptr;
    return false;
  }
  glGenTextures(2, textures);
  glGenTextures(1, &cinemaTexture);
  genFB(1, &fb);
  log("GL %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
  return true;
}
bool capture(HDC dc, GLuint target) {
  if (!setupGL())
    return false;
  RECT r;
  GetClientRect(WindowFromDC(dc), &r);
  int w = r.right, h = r.bottom;
  if (w <= 0 || h <= 0)
    return false;
  GLint tex, read, draw, rb;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
  glGetIntegerv(0x8caa, &read);
  glGetIntegerv(0x8ca6, &draw);
  bindFB(0x8ca8, 0);
  glGetIntegerv(GL_READ_BUFFER, &rb);
  glReadBuffer(GL_BACK);
  glBindTexture(GL_TEXTURE_2D, target);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, w, h, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glReadBuffer(rb);
  glBindTexture(GL_TEXTURE_2D, tex);
  bindFB(0x8ca8, read);
  bindFB(0x8ca9, draw);
  width = w;
  height = h;
  return true;
}
void composite() {
  if (!captured[0] || !captured[1])
    return;
  GLint read, draw;
  glGetIntegerv(0x8caa, &read);
  glGetIntegerv(0x8ca6, &draw);
  GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), srgb = glIsEnabled(0x8db9);
  glDisable(GL_SCISSOR_TEST);
  glDisable(0x8db9);
  bindFB(0x8ca9, 0);
  glDrawBuffer(GL_BACK);
  GLfloat oldColor[4];
  glGetFloatv(GL_COLOR_CLEAR_VALUE, oldColor);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(oldColor[0], oldColor[1], oldColor[2], oldColor[3]);
  bindFB(0x8ca8, fb);
  for (int i = 0; i < 2; i++) {
    attach(0x8ca8, 0x8ce0, GL_TEXTURE_2D, textures[i], 0);
    glReadBuffer(0x8ce0);
    if (checkFB(0x8ca8) != 0x8cd5) {
      log("Incomplete eye FBO");
      continue;
    }
    RECT client{};GetClientRect(WindowFromDC(wglGetCurrentDC()),&client);
    int panelWidth=client.right/2, panelHeight=client.bottom;
    int sourceWidth=captureWidths[i], sourceHeight=captureHeights[i];
    float scale=std::min(float(panelWidth)/sourceWidth,float(panelHeight)/sourceHeight);
    int dw=int(sourceWidth*scale),dh=int(sourceHeight*scale);
    int x=i*panelWidth+(panelWidth-dw)/2,y=(panelHeight-dh)/2;
    blit(0,0,sourceWidth,sourceHeight,x,y,x+dw,y+dh,GL_COLOR_BUFFER_BIT,GL_LINEAR);
  }
  bindFB(0x8ca8, read);
  bindFB(0x8ca9, draw);
  if (sc)
    glEnable(GL_SCISSOR_TEST);
  if (srgb)
    glEnable(0x8db9);
}
struct State {
  int interp, ticks;
  int xyz[3];
  short anim, frame;
};
State state() {
  State s{};
  s.interp = at<int>(game->interp);
  s.ticks = at<int>(game->ticks);
  auto lara = at<unsigned char *>(game->laraItem);
  if (lara) {
    memcpy(s.xyz, lara + 0x58, 12);
    memcpy(&s.anim, lara + 0x14, 2);
    memcpy(&s.frame, lara + 0x16, 2);
  }
  return s;
}
int drawHook() {
  controls();
  inDraw = true;
  xrEyeFrame = xrRequested && !forceCinema && xr.begin(true);
  if ((!enabled || forceCinema) && !xrEyeFrame) {
    int r = realDraw();
    if (xr.begun)
      xr.finish(nullptr, 0, 0);
    inDraw = false;
    return r;
  }
  auto api = at<unsigned char *>(game->apiTable);
  if (!api || !setupGL()) {
    int r = realDraw();
    if (xr.begun)
      xr.finish(nullptr, 0, 0);
    inDraw = false;
    return r;
  }
  if(classicFix && !realScissor) {
  auto host = (unsigned char *)GetModuleHandleW(nullptr);auto scissor=(Scissor*)(api+0x150);
  if(*scissor==(Scissor)(host+0xfb10)){realScissor=*scissor;*scissor=scissorHook;log("Classic scissor callback hooked");}
  else {classicFix=false;log("Classic scissor callback mismatch; classic fix disabled");}
  }
  auto lara=at<unsigned char*>(game->laraItem);
  if(headHooksReady && lara) {
    int object=*reinterpret_cast<int16_t*>(lara+0x10);
    if(object>=0 && object<512) {
      auto dispatch=reinterpret_cast<LaraDraw*>(tr+game->objectTable+size_t(object)*0x900);
      if(*dispatch==realLaraDraw)*dispatch=laraDrawHook;
    }
  }
  static unsigned char* cacheLara=nullptr;
  static int cacheOutfit=-1, cacheMode=-1, cacheGraphics=-1;
  int outfit=*reinterpret_cast<int*>(api+0x348), graphics=*reinterpret_cast<uint32_t*>(api+0x854)&1;
  if(cacheLara!=lara || cacheOutfit!=outfit || cacheMode!=cameraMode || cacheGraphics!=graphics) {
    firstpersonmesh::clear();cacheLara=lara;cacheOutfit=outfit;cacheMode=cameraMode;cacheGraphics=graphics;
  }
  auto flags=(uint32_t*)(api+0x854);
  classicEye=(*flags&1)==0;
  // Reuse the shipped classic-60 interpolation gate. Do not change bit 0,
  // the graphics mode, or the simulation frequency. Restore only our bit.
  uint32_t previousSmooth=*flags&0x04000000u;
  if(classicFix&&classicEye)*flags|=0x04000000u;
  auto tick = (Tick *)(api + 0x80);
  realTick = *tick;
  auto before = state();
  int savedTime = at<int>(game->cameraFrames);
  inStereo = true;
  captured[0] = captured[1] = false;
  eye = 0;
  worldCamera = false;
  firstPersonEye = false;
  *tick = noTick;
  beginNativeEye();
  realDraw();
  nativeEyes[0].end();
  auto afterLeft = state();
  *tick = realTick;
  at<int>(game->interp) = before.interp;
  at<int>(game->cameraFrames) = savedTime;
  eye = 1;
  worldCamera = false;
  firstPersonEye = false;
  beginNativeEye();
  int result = realDraw();
  nativeEyes[1].end();
  auto afterRight = state();
  if(classicFix&&classicEye)*flags=(*flags&~0x04000000u)|previousSmooth;
  renderMetrics(classicEye,afterRight.ticks);
  stageMenu();
  if (xr.begun)
    xr.finish(captured[0] && captured[1] ? textures : nullptr, width, height, captureWidths, captureHeights);
  firstPersonEye = false;
  xrEyeFrame = false;
  classicEye = false;
  inDraw = false;
  eye = -1;
  inStereo = false;
  frames++;
  if (frames < 4 || frames % 120 == 0)
    log("Frame %llu tick=%d/%d/%d Lara=(%d,%d,%d)/(%d,%d,%d) anim=%d/%d "
        "frame=%d/%d captures=%d,%d offset=%.1f,%.1f,%.1f rot=%.1f,%.1f,%.1f",
        frames, before.ticks, afterLeft.ticks, afterRight.ticks, before.xyz[0], before.xyz[1],
        before.xyz[2], afterRight.xyz[0], afterRight.xyz[1], afterRight.xyz[2], before.anim,
        afterRight.anim, before.frame, afterRight.frame, captured[0], captured[1], tx, ty, tz,
        pitch, yaw, roll);
  return result;
}
// Each patched call site belongs to exactly one game DLL, so entry through a
// per-game thunk is what makes the active profile unambiguous. Binding once to
// whichever DLL happened to be loaded first is wrong: the trilogy loads them on
// demand, so the first one seen is not necessarily the one being played.
template <size_t N> void enterGame() {
  game = &profiles[N];
  tr = hooks[N].base;
  realDraw = hooks[N].draw;
  realCamera = hooks[N].camera;
  realLaraDraw = hooks[N].laraDraw;
  realMeshDraw = hooks[N].meshDraw;
  realHairDraw = hooks[N].hairDraw;
  realSkinnedDraw = hooks[N].skinnedDraw;
  headHooksReady = hooks[N].headHooks;
}
template <size_t N> int drawThunk() {
  enterGame<N>();
  return drawHook();
}
template <size_t N> void cameraThunk(void *pose) {
  enterGame<N>();
  cameraHook(pose);
}
static void *const drawThunks[] = {(void *)drawThunk<0>, (void *)drawThunk<1>,
                                   (void *)drawThunk<2>};
static void *const cameraThunks[] = {(void *)cameraThunk<0>, (void *)cameraThunk<1>,
                                     (void *)cameraThunk<2>};
static_assert(sizeof(drawThunks) / sizeof(*drawThunks) == profileCount, "thunk per profile");

void installGame(size_t index, unsigned char *base) {
  auto &p = profiles[index];
  auto &h = hooks[index];
  h = {};
  h.base = base;
  tr = base;
  game = &p;
  auto dos = (IMAGE_DOS_HEADER *)base;
  auto nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
  if (nt->OptionalHeader.SizeOfImage != p.imageSize) {
    log("Unexpected %s image size %x (expected %x); refusing hooks", p.name,
        nt->OptionalHeader.SizeOfImage, p.imageSize);
    return;
  }
  if (p.laraDraw && p.meshDraw && p.skinnedCalls && p.faceCalls) {
    realLaraDraw = h.laraDraw = (LaraDraw)(base + p.laraDraw);
    realMeshDraw = h.meshDraw = (MeshDraw)(base + p.meshDraw);
    realHairDraw = h.hairDraw = (void (*)())(base + p.hairDraw);
    realSkinnedDraw = h.skinnedDraw = (SkinnedDraw)(base + p.skinnedDraw);
    // The indexed-mesh filter patches a host import, so it is installed once.
    static int meshFilter = -1;
    if (meshFilter < 0)
      meshFilter = firstpersonmesh::install() ? 1 : 0;
    bool ready = meshFilter == 1 &&
                 patchCall(p.laraDrawCall, (void *)laraDrawHook, p.laraDraw) &&
                 patchCall(p.meshDrawCall, index == 1 ? (void *)classicHeadHookTR2 : (void *)classicHeadHook, p.meshDraw) &&
                 patchCall(p.hairDrawCall, (void *)hairHook, p.hairDraw);
    // Lara submits her base body, equipment and animated face separately.
    // Apply the same bone-based filter to every submission, retaining non-head parts.
    for (const size_t *c = p.skinnedCalls; ready && *c; c++)
      ready = patchCall(*c, (void *)skinnedHeadHook, p.skinnedDraw);
    for (const size_t *c = p.faceCalls; ready && *c; c++)
      ready = patchCall(*c, (void *)faceOnlyHook, p.skinnedDraw);
    h.headHooks = headHooksReady = ready;
    log("%s head hooks ready=%d", p.name, ready);
  } else
    log("%s first person unavailable: head-rendering hooks are not mapped", p.name);
  realDraw = h.draw = (Draw)(base + p.drawScene);
  realCamera = h.camera = (Camera)(base + p.generateW2V);
  if (!patchCall(p.cameraCall, cameraThunks[index], p.generateW2V)) {
    log("%s camera hook failed at call site %zx; staying in cinema-only mode", p.name,
        p.cameraCall);
    return;
  }
  if (!patchCall(p.drawCall, drawThunks[index], p.drawScene)) {
    log("%s draw hook failed at call site %zx; staying in cinema-only mode", p.name, p.drawCall);
    return;
  }
  log("%s hooks ready at %p. F6 stereo; F7 recenter; F9 force cinema; F10 menu; "
      "F11 camera mode; numpad camera.",
      p.name, (void *)base);
}

void install() {
  // Re-checked every swap: a game DLL appears when its game is selected and can
  // be replaced at a new base if the player switches away and back.
  static unsigned char *attempted[profileCount]{};
  for (size_t i = 0; i < profileCount; i++) {
    auto base = (unsigned char *)GetModuleHandleW(profiles[i].module);
    if (!base) {
      attempted[i] = nullptr; // Unloaded; hook it again if it returns.
      continue;
    }
    if (attempted[i] == base)
      continue;
    attempted[i] = base;
    log("Detected %s at %p", profiles[i].name, (void *)base);
    installGame(i, base);
  }
  if (!tr)
    return;
  static bool hostHooked = false;
  if (hostHooked)
    return;
  hostHooked = true;
  // tomb123.exe is the same module for every game, so this is done once.
  auto host = (unsigned char *)GetModuleHandleW(nullptr);
  auto slot = (UniformMatrix *)(host + 0xc6aa8e0);
  if (*slot == (UniformMatrix)wglGetProcAddress("glUniformMatrix4fv")) {
    realUniformMatrix = *slot;
    *slot = uniformMatrixHook;
    log("Projection dispatch hooked");
  } else
    log("Projection dispatch mismatch; do not enable XR");
}
BOOL WINAPI swapHook(HDC dc) {
  swaps++;
  install();
  // Menus, cutscenes and load screens never reach the gameplay draw hook. Show
  // their flat image on a world-locked screen instead of submitting an empty frame.
  if (!inStereo && (!inDraw || forceCinema)) {
    if (!inDraw)
      controls();
    if (xrRequested) {
      if (swaps - lastCinemaSwap > 8)
        xr.screenPending = true; // Re-anchor whenever the cinema is (re-)entered.
      lastCinemaSwap = swaps;
      bool ready = xr.begin(false);
      if (xr.begun) {
        bool shown = ready && capture(dc, cinemaTexture);
        stageMenu();
        xr.finishCinema(shown ? cinemaTexture : 0, width, height);
        if (shown && (++cinemaFrames < 4 || cinemaFrames % 600 == 0))
          log("Cinema frame %llu source=%dx%d forced=%d", cinemaFrames, width, height, forceCinema);
      }
    }
  }
  if (inStereo) {
    if (nativeEyes[eye].active) {
      captureWidths[eye]=nativeEyes[eye].width;
      captureHeights[eye]=nativeEyes[eye].height;
      width=captureWidths[eye];height=captureHeights[eye];
      nativeEyes[eye].end();
      captured[eye]=true;
      if(frames<3 || frames%600==0)
        log("Native eye rendered eye=%d source=%dx%d runtime=%dx%d",eye,width,height,xr.widths[eye],xr.heights[eye]);
    } else if (capture(dc, textures[eye])) {
      captured[eye] = true;
      captureWidths[eye]=width;captureHeights[eye]=height;
    }
    if (eye == 0)
      return TRUE;
    composite();
  }
  if (xrRequested) {
    using Interval = BOOL(WINAPI *)(int);
    auto interval = (Interval)wglGetProcAddress("wglSwapIntervalEXT");
    if (interval)
      interval(0);
  }
  return realSwap(dc);
}
extern "C" __declspec(dllexport) DWORD WINAPI Initialize(void *) {
  if (logFile)
    return 1;
  menu.bind(menuItems, int(sizeof(menuItems) / sizeof(*menuItems)));
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(self, path, MAX_PATH);
  wchar_t *slash = wcsrchr(path, L'\\');
  if (slash)
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"prototype.log");
  logFile = _wfsopen(path, L"w", _SH_DENYNO);
  if (slash) {
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"openxr_loader.dll");
    if (!LoadLibraryW(path)) {
      log("Cannot load OpenXR loader: %lu", GetLastError());
      return 0;
    }
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"tombvr.ini");
    classicFix=setting(path,L"ClassicFix",1,0,1)!=0;
    unitsPerMeter = float(setting(path, L"UnitsPerMeter", 512, 64, 4096));
    xrRequested = setting(path, L"OpenXR", 1, 0, 1) != 0;
    xr.cinemaDistance = setting(path, L"CinemaDistanceCm", 250, 50, 2000) / 100.f;
    xr.cinemaWidth = setting(path, L"CinemaWidthCm", 300, 30, 4000) / 100.f;
    cameraMode = setting(path, L"CameraMode", CameraClassic, 0, CameraModeCount - 1);
    shoulderDistance = float(setting(path, L"ShoulderDistance", 512, 0, 4096));
    shoulderRight = float(setting(path, L"ShoulderRight", 160, -1024, 1024));
    shoulderUp = float(setting(path, L"ShoulderUp", 64, -1024, 1024));
    eyeHeight = float(setting(path, L"EyeHeight", 650, -2048, 2048));
    eyeForward = float(setting(path, L"EyeForward", 0, -1024, 1024));
    xr.menuDistance = setting(path, L"MenuDistanceCm", 120, 40, 500) / 100.f;
    xr.menuWidth = setting(path, L"MenuWidthCm", 90, 20, 400) / 100.f;
    log("VR settings: OpenXR=%d units/m=%.1f cinema=%.2f m wide at %.2f m camera=%s", xrRequested,
        unitsPerMeter, xr.cinemaWidth, xr.cinemaDistance, cameraModeNames[cameraMode]);
  }

  auto base = (unsigned char *)GetModuleHandleW(nullptr);
  auto dos = (IMAGE_DOS_HEADER *)base;
  auto nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
  auto desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader
                                                     .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                                                     .VirtualAddress);
  for (; desc->Name; desc++) {
    if (_stricmp((char *)base + desc->Name, "GDI32.dll"))
      continue;
    auto names = (IMAGE_THUNK_DATA64 *)(base + desc->OriginalFirstThunk);
    auto slots = (IMAGE_THUNK_DATA64 *)(base + desc->FirstThunk);
    for (; names->u1.AddressOfData; names++, slots++) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
        continue;
      auto name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
      if (strcmp((char *)name->Name, "SwapBuffers"))
        continue;
      DWORD old;
      if (!VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old))
        return 0;
      realSwap = (Swap)slots->u1.Function;
      InterlockedExchangePointer((void **)&slots->u1.Function, (void *)swapHook);
      DWORD tmp;
      VirtualProtect(&slots->u1.Function, 8, old, &tmp);
      log("IAT SwapBuffers hook installed");
      return 1;
    }
  }
  log("SwapBuffers import not found");
  return 0;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    self = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}
