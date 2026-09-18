#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <gl/GL.h>
#include "vr_menu.h"
extern void log(const char *, ...);
namespace {
constexpr int bgraFormat = 0x80e1; // GL_BGRA: core since GL 1.2, absent from the GL 1.1 header.
constexpr int titleY = 18, rowTop = 76, rowHeight = 34, margin = 26;
COLORREF panel = RGB(18, 20, 26), bar = RGB(52, 96, 150), label = RGB(226, 230, 238),
         valueColor = RGB(150, 205, 255), titleColor = RGB(240, 196, 96),
         hintColor = RGB(120, 126, 138);
HDC dib;
HBITMAP bitmap;
void *bits;
int dibW, dibH;
bool makeDIB(int w, int h) {
  if (dib && dibW == w && dibH == h)
    return true;
  if (bitmap)
    DeleteObject(bitmap);
  if (dib)
    DeleteDC(dib);
  bitmap = nullptr;
  dib = nullptr;
  bits = nullptr;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = h; // Positive: bottom-up rows, matching GL's origin.
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  dib = CreateCompatibleDC(nullptr);
  if (!dib)
    return false;
  bitmap = CreateDIBSection(dib, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap || !bits) {
    DeleteDC(dib);
    dib = nullptr;
    return false;
  }
  SelectObject(dib, bitmap);
  SetBkMode(dib, TRANSPARENT);
  dibW = w;
  dibH = h;
  return true;
}
void draw(const char *text, int y, COLORREF color, bool rightAlign, int width) {
  SetTextColor(dib, color);
  RECT r{margin, y, width - margin, y + rowHeight};
  DrawTextA(dib, text, -1, &r, (rightAlign ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_TOP);
}
void format(const MenuItem &item, char *out, size_t size) {
  if (item.action) {
    out[0] = 0;
    return;
  }
  float v = item.integer ? float(*item.integer) : *item.number;
  if (item.names) {
    snprintf(out, size, "%s", item.names[(int)lroundf(v)]);
    return;
  }
  if (item.step >= 1.f)
    snprintf(out, size, "%d%s", (int)lroundf(v), item.unit ? item.unit : "");
  else
    snprintf(out, size, "%.2f%s", v, item.unit ? item.unit : "");
}
} // namespace
void VRMenu::move(int delta) {
  if (count <= 0)
    return;
  selected = ((selected + delta) % count + count) % count;
  dirty = true;
}
void VRMenu::adjust(int delta) {
  if (selected < 0 || selected >= count)
    return;
  auto &item = items[selected];
  if (item.action)
    return;
  float v = item.integer ? float(*item.integer) : *item.number;
  if (item.names) {
    // Enumerated rows wrap, so one direction still reaches every option.
    int span = (int)lroundf(item.high - item.low) + 1;
    int index = (int)lroundf(v - item.low);
    v = item.low + float(((index + delta) % span + span) % span);
  } else
    v = std::clamp(v + item.step * delta, item.low, item.high);
  if (item.integer)
    *item.integer = (int)lroundf(v);
  else
    *item.number = v;
  dirty = true;
}
void VRMenu::activate() {
  if (selected < 0 || selected >= count)
    return;
  if (items[selected].action)
    items[selected].action();
  else
    adjust(1);
  dirty = true;
}
bool VRMenu::render() {
  if (texture && !dirty)
    return true;
  if (!makeDIB(width, height))
    return false;
  RECT full{0, 0, width, height};
  HBRUSH background = CreateSolidBrush(panel);
  FillRect(dib, &full, background);
  DeleteObject(background);
  HFONT title = CreateFontA(30, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
  HFONT row = CreateFontA(24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
  auto old = (HFONT)SelectObject(dib, title);
  draw("TOMB RAIDER VR", titleY, titleColor, false, width);
  SelectObject(dib, row);
  for (int i = 0; i < count; i++) {
    int y = rowTop + i * rowHeight;
    if (i == selected) {
      RECT r{margin - 10, y - 2, width - margin + 10, y + rowHeight - 4};
      HBRUSH highlight = CreateSolidBrush(bar);
      FillRect(dib, &r, highlight);
      DeleteObject(highlight);
    }
    char value[64];
    format(items[i], value, sizeof(value));
    draw(items[i].label, y, label, false, width);
    if (value[0])
      draw(value, y, i == selected ? RGB(255, 255, 255) : valueColor, true, width);
  }
  draw("Numpad 8/2 select   4/6 change   5 activate   F10 close", height - rowHeight - 12,
       hintColor, false, width);
  SelectObject(dib, old);
  DeleteObject(title);
  DeleteObject(row);
  GdiFlush();
  // BI_RGB leaves the fourth byte zero; the quad is composited opaquely.
  auto pixels = (unsigned char *)bits;
  for (int i = 3; i < width * height * 4; i += 4)
    pixels[i] = 255;
  // Explicitly isolate every unpack parameter that can reinterpret a CPU pointer.
  using BindBuffer = void(APIENTRY *)(GLenum, GLuint);
  auto bindBuffer = (BindBuffer)wglGetProcAddress("glBindBuffer");
  if (!bindBuffer) return false;
  GLint bound = 0, unpackBuffer = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  glGetIntegerv(0x88ef, &unpackBuffer); // GL_PIXEL_UNPACK_BUFFER_BINDING
  const GLenum params[] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
                          GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS};
  GLint saved[4]{};
  for (int i=0;i<4;i++) {
    glGetIntegerv(params[i], &saved[i]);
    glPixelStorei(params[i], i==0 ? 4 : 0);
  }
  bindBuffer(0x88ec, 0); // GL_PIXEL_UNPACK_BUFFER
  while (glGetError()!=GL_NO_ERROR) {} // Attribute subsequent errors to this upload.
  if (!texture) glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, bgraFormat, GL_UNSIGNED_BYTE, bits);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  GLenum error = glGetError();
  bool uploaded = error == GL_NO_ERROR;
  // Read back the first few updates: catches failed uploads and wrong texture data.
  static unsigned checks = 0;
  if (uploaded && checks < 4) {
    GLint packBuffer=0, pack[4]{};
    const GLenum packing[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
                             GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS};
    glGetIntegerv(0x88ed, &packBuffer);
    bindBuffer(0x88eb, 0);
    for(int i=0;i<4;i++) {
      glGetIntegerv(packing[i], &pack[i]);
      glPixelStorei(packing[i], i==0 ? 4 : 0);
    }
    std::vector<unsigned char> actual(size_t(width)*height*4);
    glGetTexImage(GL_TEXTURE_2D, 0, bgraFormat, GL_UNSIGNED_BYTE, actual.data());
    error = glGetError();
    uploaded = error == GL_NO_ERROR && memcmp(actual.data(),bits,actual.size())==0;
    unsigned hash=2166136261u;
    for(auto v:actual) hash=(hash^v)*16777619u;
    log("Menu upload texture=%u %dx%d CPU/GPU match=%d hash=%08x GL=%x",
        texture,width,height,uploaded,hash,error);
    for(int i=0;i<4;i++) glPixelStorei(packing[i],pack[i]);
    bindBuffer(0x88eb,packBuffer);
    ++checks;
  }
  for (int i=0;i<4;i++) glPixelStorei(params[i],saved[i]);
  bindBuffer(0x88ec,unpackBuffer);
  glBindTexture(GL_TEXTURE_2D, bound);
  if (!uploaded) {
    static unsigned failures=0;
    if(failures++<4) log("Menu upload failed GL=%x; retrying",error);
    return false;
  }
  dirty = false;
  return true;
}
