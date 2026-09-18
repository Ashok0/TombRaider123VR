#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "xr_bridge.h"
#include <gl/GL.h>
#include <cstring>
#include <algorithm>
#include <cmath>
extern void log(const char *, ...);
namespace {
using Bind = void(APIENTRY *)(GLenum, GLuint);
using Gen = void(APIENTRY *)(GLsizei, GLuint *);
using Attach = void(APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using Blit = void(APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                              GLenum);
using Check = GLenum(APIENTRY *)(GLenum);
using Delete = void(APIENTRY *)(GLsizei, const GLuint *);
Bind bind;
Gen gen;
Attach attach;
Blit blit;
Check check;
Delete del;
// Heading of a pose's forward (-Z) axis, projected onto the horizontal plane.
float yawOf(const XrQuaternionf &q) {
  float fwdX = -2 * (q.x * q.z + q.w * q.y), fwdZ = -(1 - 2 * (q.x * q.x + q.y * q.y));
  return atan2f(-fwdX, -fwdZ);
}
XrQuaternionf yawQuat(float angle) { return {0, sinf(angle * .5f), 0, cosf(angle * .5f)}; }
// A surface squared to the given heading, level with the eyes, that many metres ahead.
XrPosef facing(const XrPosef &head, float heading, float distance) {
  XrPosef p{};
  p.orientation = yawQuat(heading);
  p.position = {head.position.x - sinf(heading) * distance, head.position.y,
                head.position.z - cosf(heading) * distance};
  return p;
}
bool ok(XrResult r, const char *what) {
  if (XR_FAILED(r)) {
    log("XR ERROR %s: %d", what, r);
    return false;
  }
  return true;
}
} // namespace
bool XRBridge::initialize() {
  if (session)
    return true;
  if (failed)
    return false;
  gameContext = wglGetCurrentContext();
  dc = wglGetCurrentDC();
  if (!gameContext || !dc)
    return false;
  uint32_t n = 0;
  if (!ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr), "extensions"))
    return false;
  std::vector<XrExtensionProperties> ex(n, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, n, &n, ex.data());
  bool supported = false;
  for (auto &e : ex)
    if (!strcmp(e.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME))
      supported = true;
  if (!supported) {
    log("XR runtime does not support OpenGL");
    failed = true;
    return false;
  }
  const char *extension = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  strcpy_s(ci.applicationInfo.applicationName, "Tomb Raider Remastered VR");
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = &extension;
  if (!ok(xrCreateInstance(&ci, &instance), "create instance")) {
    failed = true;
    return false;
  }
  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  xrGetInstanceProperties(instance, &ip);
  log("XR runtime: %s", ip.runtimeName);
  XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
  gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if (!ok(xrGetSystem(instance, &gi, &system), "get headset")) {
    shutdown();
    failed = true;
    return false;
  }
  PFN_xrGetOpenGLGraphicsRequirementsKHR req = nullptr;
  xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req);
  XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
  if (!req || !ok(req(instance, system, &requirements), "GL requirements")) {
    shutdown();
    failed = true;
    return false;
  }
  using CreateContext = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
  auto create = (CreateContext)wglGetProcAddress("wglCreateContextAttribsARB");
  int major = std::max(4, int(XR_VERSION_MAJOR(requirements.minApiVersionSupported)));
  int minor = major == 4 ? std::max(3, int(XR_VERSION_MINOR(requirements.minApiVersionSupported)))
                         : int(XR_VERSION_MINOR(requirements.minApiVersionSupported));
  int attrs[] = {0x2091, major, 0x2092, minor, 0x9126, 1, 0};
  context = create ? create(dc, gameContext, attrs) : nullptr;
  if (!context) {
    log("XR shared context creation failed: %lu", GetLastError());
    shutdown();
    failed = true;
    return false;
  }
  if (!wglMakeCurrent(dc, context)) {
    shutdown();
    failed = true;
    return false;
  }
  log("XR shared GL context %s", glGetString(GL_VERSION));
  bind = (Bind)wglGetProcAddress("glBindFramebuffer");
  gen = (Gen)wglGetProcAddress("glGenFramebuffers");
  attach = (Attach)wglGetProcAddress("glFramebufferTexture2D");
  blit = (Blit)wglGetProcAddress("glBlitFramebuffer");
  check = (Check)wglGetProcAddress("glCheckFramebufferStatus");
  del = (Delete)wglGetProcAddress("glDeleteFramebuffers");
  XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
  binding.hDC = dc;
  binding.hGLRC = context;
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
  si.next = &binding;
  si.systemId = system;
  if (!ok(xrCreateSession(instance, &si, &session), "create session")) {
    shutdown();
    failed = true;
    return false;
  }
  XrReferenceSpaceCreateInfo sci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  sci.poseInReferenceSpace.orientation.w = 1;
  sci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  if (!ok(xrCreateReferenceSpace(session, &sci, &local), "local space")) {
    shutdown();
    failed = true;
    return false;
  }
  sci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  if (!ok(xrCreateReferenceSpace(session, &sci, &head), "head space")) {
    shutdown();
    failed = true;
    return false;
  }
  uint32_t count = 0;
  xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                    &count, nullptr);
  std::vector<XrViewConfigurationView> configs(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                    count, &count, configs.data());
  if (count != 2) {
    shutdown();
    failed = true;
    return false;
  }
  xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  xrEnumerateSwapchainFormats(session, count, &count, formats.data());
  int64_t format = 0;
  for (auto f : formats)
    if (f == GL_RGBA8)
      format = f; // Game final backbuffer contains display-encoded color: preserve byte values in
                  // sRGB images.
  for (auto f : formats)
    if (f == 0x8c43)
      format = f;
  swapFormat = format;
  if (!format) {
    log("XR no RGBA8/sRGB swapchain format");
    shutdown();
    failed = true;
    return false;
  }
  for (int i = 0; i < 2; i++) {
    widths[i] = configs[i].recommendedImageRectWidth;
    heights[i] = configs[i].recommendedImageRectHeight;
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = format;
    sc.sampleCount = 1;
    sc.width = widths[i];
    sc.height = heights[i];
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    if (!ok(xrCreateSwapchain(session, &sc, &chains[i]), "swapchain")) {
      shutdown();
      failed = true;
      return false;
    }
    xrEnumerateSwapchainImages(chains[i], 0, &count, nullptr);
    images[i].resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    xrEnumerateSwapchainImages(chains[i], count, &count,
                               (XrSwapchainImageBaseHeader *)images[i].data());
    log("XR eye %d swapchain %dx%d format %llx images %u", i, widths[i], heights[i], format, count);
  }
  gen(1, &readFB);
  gen(1, &drawFB);
  wglMakeCurrent(dc, gameContext);
  log("XR initialized; waiting for session READY");
  return true;
}
void XRBridge::refreshResolution() {
  static ULONGLONG last=0;auto now=GetTickCount64();
  if(now-last<1000 || begun || !session)return;
  last=now;
  uint32_t count=0;
  XrViewConfigurationView config[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  if(!ok(xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,config),"refresh eye resolution") || count!=2)return;
  bool changed=false;
  for(int i=0;i<2;i++)changed|=widths[i]!=int(config[i].recommendedImageRectWidth)||heights[i]!=int(config[i].recommendedImageRectHeight);
  if(!changed)return;
  HGLRC previous=wglGetCurrentContext();HDC previousDC=wglGetCurrentDC();glFinish();
  if(!wglMakeCurrent(dc,context))return;
  XrSwapchain replacement[2]{};
  std::vector<XrSwapchainImageOpenGLKHR> replacementImages[2];
  bool ready=true;
  for(int i=0;i<2&&ready;i++) {
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format=swapFormat;sc.sampleCount=1;sc.width=config[i].recommendedImageRectWidth;
    sc.height=config[i].recommendedImageRectHeight;sc.faceCount=sc.arraySize=sc.mipCount=1;
    ready=ok(xrCreateSwapchain(session,&sc,&replacement[i]),"resize eye swapchain");
    if(ready){
      uint32_t n=0;ready=ok(xrEnumerateSwapchainImages(replacement[i],0,&n,nullptr),"resized image count")&&n>0;
      if(ready){replacementImages[i].resize(n,{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});ready=ok(xrEnumerateSwapchainImages(replacement[i],n,&n,(XrSwapchainImageBaseHeader*)replacementImages[i].data()),"resized images");}
    }
  }
  if(ready)for(int i=0;i<2;i++) {
    xrDestroySwapchain(chains[i]);chains[i]=replacement[i];replacement[i]=XR_NULL_HANDLE;
    images[i]=std::move(replacementImages[i]);widths[i]=config[i].recommendedImageRectWidth;heights[i]=config[i].recommendedImageRectHeight;
    log("XR eye %d resolution changed to %dx%d",i,widths[i],heights[i]);
  }
  for(auto chain:replacement)if(chain)xrDestroySwapchain(chain);
  wglMakeCurrent(previousDC,previous);
}
bool XRBridge::begin(bool render) {
  valid = false;
  if (!initialize())
    return false;
  if(render) refreshResolution();
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      auto *s = (XrEventDataSessionStateChanged *)&ev;
      log("XR session state %d", s->state);
      if (s->state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        running = ok(xrBeginSession(session, &bi), "begin session");
      }
      if (s->state == XR_SESSION_STATE_STOPPING) {
        if (running)
          xrEndSession(session);
        running = false;
      }
      if (s->state == XR_SESSION_STATE_LOSS_PENDING || s->state == XR_SESSION_STATE_EXITING) {
        shutdown();
        failed = true;
        return false;
      }
    } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
      recenterPending = true;
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      shutdown();
      failed = true;
      return false;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  if (!running)
    return false;
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  frame = {XR_TYPE_FRAME_STATE};
  if (!ok(xrWaitFrame(session, &wi, &frame), "wait frame"))
    return false;
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  if (!ok(xrBeginFrame(session, &bi), "begin frame"))
    return false;
  begun = true;
  if (!frame.shouldRender)
    return false;
  // The head pose anchors both the gameplay tracking frame and the cinema screen.
  XrSpaceLocation h{XR_TYPE_SPACE_LOCATION};
  if (!ok(xrLocateSpace(head, local, frame.predictedDisplayTime, &h), "head pose"))
    return false;
  auto hf = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
  if ((h.locationFlags & hf) != hf)
    return false;
  center = h.pose;
  float heading = yawOf(center.orientation);
  if (recenterPending) {
    origin = center;
    origin.orientation = yawQuat(heading);
    recenterPending = false;
    log("XR recentered at %.3f %.3f %.3f", origin.position.x, origin.position.y, origin.position.z);
  }
  if (screenPending) {
    screen = facing(center, heading, cinemaDistance);
    screenPending = false;
    log("XR cinema screen at %.3f %.3f %.3f heading %.1f deg", screen.position.x, screen.position.y,
        screen.position.z, heading * 57.2958f);
  }
  if (menuPending) {
    menuPose = facing(center, heading, menuDistance);
    menuPending = false;
    log("XR menu panel at %.3f %.3f %.3f", menuPose.position.x, menuPose.position.y,
        menuPose.position.z);
  }
  if (!render)
    return true;
  XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
  li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  li.displayTime = frame.predictedDisplayTime;
  li.space = local;
  XrViewState vs{XR_TYPE_VIEW_STATE};
  uint32_t count = 0;
  if (!ok(xrLocateViews(session, &li, &vs, 2, &count, views), "locate views"))
    return false;
  auto flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
  if (count != 2 || (vs.viewStateFlags & flags) != flags)
    return false;
  valid = true;
  return true;
}
void XRBridge::finish(const unsigned int *textures, int w, int h, const int* sourceWidths, const int* sourceHeights) {
  if (!begun)
    return;
  XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  bool rendered = valid && textures && w > 0 && h > 0, menuShown = false;
  if (rendered) {
    glFinish();
    if (!wglMakeCurrent(dc, context))
      rendered = false;
    else {
      glDisable(GL_SCISSOR_TEST);
      glDisable(0x8db9);
      for (int i = 0; i < 2; i++) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!ok(xrAcquireSwapchainImage(chains[i], &ai, &index), "acquire image")) {
          rendered = false;
          break;
        }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        bool waited = ok(xrWaitSwapchainImage(chains[i], &wi), "wait image");
        if (waited) {
          bind(0x8ca8, readFB);
          attach(0x8ca8, 0x8ce0, GL_TEXTURE_2D, textures[i], 0);
          glReadBuffer(0x8ce0);
          bind(0x8ca9, drawFB);
          attach(0x8ca9, 0x8ce0, GL_TEXTURE_2D, images[i][index].image, 0);
          glDrawBuffer(0x8ce0);
          if (check(0x8ca8) != 0x8cd5 || check(0x8ca9) != 0x8cd5) {
            log("XR incomplete copy framebuffer");
            rendered = false;
          } else
            blit(0, 0, sourceWidths ? sourceWidths[i] : w, sourceHeights ? sourceHeights[i] : h, 0, 0, widths[i], heights[i], GL_COLOR_BUFFER_BIT, GL_NEAREST);
          glFinish();
          XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
          if (!ok(xrReleaseSwapchainImage(chains[i], &ri), "release image"))
            rendered = false;
        } else
          rendered = false;
        pv[i].pose = views[i].pose;
        pv[i].fov = views[i].fov;
        pv[i].subImage.swapchain = chains[i];
        pv[i].subImage.imageRect.extent = {widths[i], heights[i]};
        if (!rendered)
          break;
      }
      bind(0x8d40, 0);
      menuShown = stageMenu();
      wglMakeCurrent(dc, gameContext);
    }
  }
  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  layer.space = local;
  layer.viewCount = 2;
  layer.views = pv;
  const XrCompositionLayerBaseHeader *layers[2];
  uint32_t count = 0;
  if (rendered)
    layers[count++] = (XrCompositionLayerBaseHeader *)&layer;
  if (menuShown)
    layers[count++] = (XrCompositionLayerBaseHeader *)&menuQuad;
  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = frame.predictedDisplayTime;
  ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  ei.layerCount = count;
  ei.layers = count ? layers : nullptr;
  bool success = ok(xrEndFrame(session, &ei), "end frame");
  begun = false;
  valid = false;
  if (success && rendered) {
    submitted++;
    if (submitted < 4 || submitted % 300 == 0)
      log("XR submitted %llu head=%.3f %.3f %.3f quat=%.3f %.3f %.3f %.3f", submitted,
          center.position.x, center.position.y, center.position.z, center.orientation.x,
          center.orientation.y, center.orientation.z, center.orientation.w);
  }
}
bool XRBridge::present(XrSwapchain &chain, std::vector<XrSwapchainImageOpenGLKHR> &imgs,
                       int &cw, int &ch, unsigned int texture, int w, int h, const char *what) {
  // The XR context must be current. The swapchain is sized to the source so the
  // flat image is presented without rescaling.
  if (!texture || w <= 0 || h <= 0 || !swapFormat)
    return false;
  if (!chain || cw != w || ch != h) {
    if (chain) {
      xrDestroySwapchain(chain);
      chain = XR_NULL_HANDLE;
      imgs.clear();
    }
    cw = ch = 0;
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = swapFormat;
    sc.sampleCount = 1;
    sc.width = w;
    sc.height = h;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    if (!ok(xrCreateSwapchain(session, &sc, &chain), what)) {
      chain = XR_NULL_HANDLE;
      return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(chain, 0, &count, nullptr);
    imgs.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader *)imgs.data());
    cw = w;
    ch = h;
    log("XR %s swapchain %dx%d images %u", what, w, h, count);
  }
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (!ok(xrAcquireSwapchainImage(chain, &ai, &index), "acquire image"))
    return false;
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  if (!ok(xrWaitSwapchainImage(chain, &wi), "wait image"))
    return false;
  bool copied = true;
  glDisable(GL_SCISSOR_TEST);
  glDisable(0x8db9);
  bind(0x8ca8, readFB);
  attach(0x8ca8, 0x8ce0, GL_TEXTURE_2D, texture, 0);
  glReadBuffer(0x8ce0);
  bind(0x8ca9, drawFB);
  attach(0x8ca9, 0x8ce0, GL_TEXTURE_2D, imgs[index].image, 0);
  glDrawBuffer(0x8ce0);
  if (check(0x8ca8) != 0x8cd5 || check(0x8ca9) != 0x8cd5) {
    log("XR incomplete %s framebuffer", what);
    copied = false;
  } else {
    blit(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
      log("XR %s copy GL error %x", what, error);
      copied = false;
    }
    static unsigned menuChecks = 0;
    if (copied && strcmp(what, "menu") == 0 && menuChecks < 4) {
      // Both buffers are read in identical BGRA order in our private context.
      std::vector<unsigned char> source(size_t(w)*h*4), destination(source.size());
      glReadPixels(0,0,w,h,0x80e1,GL_UNSIGNED_BYTE,source.data());
      bind(0x8ca8,drawFB);
      glReadBuffer(0x8ce0);
      glReadPixels(0,0,w,h,0x80e1,GL_UNSIGNED_BYTE,destination.data());
      error=glGetError();
      copied=error==GL_NO_ERROR && source==destination;
      unsigned hash=2166136261u;
      for(auto v:destination) hash=(hash^v)*16777619u;
      log("XR menu copy index=%u source/destination match=%d hash=%08x GL=%x",
          index,copied,hash,error);
      ++menuChecks;
    }
  }
  glFinish();
  bind(0x8d40, 0);
  // Always release what was successfully waited on, or the swapchain wedges.
  XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  if (!ok(xrReleaseSwapchainImage(chain, &ri), "release image"))
    copied = false;
  return copied;
}
bool XRBridge::stageMenu() {
  if (!present(menuChain, menuImages, menuW, menuH, menuTexture, menuTexW, menuTexH, "menu"))
    return false;
  menuQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  menuQuad.space = local;
  menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  menuQuad.subImage.swapchain = menuChain;
  menuQuad.subImage.imageRect.extent = {menuW, menuH};
  menuQuad.pose = menuPose;
  menuQuad.size = {menuWidth, menuWidth * float(menuH) / float(menuW)};
  return true;
}
void XRBridge::finishCinema(unsigned int texture, int w, int h) {
  if (!begun)
    return;
  bool rendered = false, menu = false;
  if (frame.shouldRender) {
    glFinish();
    if (wglMakeCurrent(dc, context)) {
      rendered = present(cinemaChain, cinemaImages, cinemaW, cinemaH, texture, w, h, "cinema");
      menu = stageMenu();
      wglMakeCurrent(dc, gameContext);
    }
  }
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  quad.layerFlags = 0; // Backbuffer alpha is undefined: composite the quad opaquely.
  quad.space = local;
  quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  quad.subImage.swapchain = cinemaChain;
  quad.subImage.imageRect.extent = {cinemaW, cinemaH};
  quad.pose = screen;
  float aspect = h > 0 ? float(w) / float(h) : 16.f / 9.f;
  quad.size = {cinemaWidth, cinemaWidth / aspect};
  const XrCompositionLayerBaseHeader *layers[2];
  uint32_t count = 0;
  if (rendered)
    layers[count++] = (XrCompositionLayerBaseHeader *)&quad;
  if (menu)
    layers[count++] = (XrCompositionLayerBaseHeader *)&menuQuad;
  XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
  ei.displayTime = frame.predictedDisplayTime;
  ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  ei.layerCount = count;
  ei.layers = count ? layers : nullptr;
  bool success = ok(xrEndFrame(session, &ei), "end cinema frame");
  begun = false;
  valid = false;
  if (success && rendered) {
    submitted++;
    if (submitted < 4 || submitted % 300 == 0)
      log("XR cinema submitted %llu source %dx%d screen %.2fx%.2f m at %.2f m", submitted, w, h,
          quad.size.width, quad.size.height, cinemaDistance);
  }
}
void XRBridge::shutdown() {
  if (begun)
    finish(nullptr, 0, 0);
  if (context)
    wglMakeCurrent(dc, context);
  for (auto &s : chains)
    if (s) {
      xrDestroySwapchain(s);
      s = XR_NULL_HANDLE;
    }
  if (cinemaChain) {
    xrDestroySwapchain(cinemaChain);
    cinemaChain = XR_NULL_HANDLE;
  }
  if (menuChain) {
    xrDestroySwapchain(menuChain);
    menuChain = XR_NULL_HANDLE;
  }
  cinemaImages.clear();
  menuImages.clear();
  cinemaW = cinemaH = menuW = menuH = 0;
  if (head) {
    xrDestroySpace(head);
    head = XR_NULL_HANDLE;
  }
  if (local) {
    xrDestroySpace(local);
    local = XR_NULL_HANDLE;
  }
  if (session) {
    xrDestroySession(session);
    session = XR_NULL_HANDLE;
  }
  if (context) {
    if (del) {
      if (readFB)
        del(1, &readFB);
      if (drawFB)
        del(1, &drawFB);
    }
    readFB = drawFB = 0;
    wglMakeCurrent(dc, gameContext);
    wglDeleteContext(context);
    context = nullptr;
  }
  if (instance) {
    xrDestroyInstance(instance);
    instance = XR_NULL_HANDLE;
  }
  running = valid = false;
  submitted = 0;
  swapFormat = 0;
  recenterPending = screenPending = menuPending = true;
}
