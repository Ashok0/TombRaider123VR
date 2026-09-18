#pragma once
// A minimal VR options panel: a GDI-drawn texture presented as its own quad
// layer. One row per setting, driven from the numeric keypad.
struct MenuItem {
  const char *label;
  int *integer;          // Bound to exactly one of integer/number, or neither
  float *number;         // for an action row.
  float step, low, high;
  const char *const *names; // When set, the value is shown as names[(int)value].
  const char *unit;
  void (*action)();
};
struct VRMenu {
  const MenuItem *items = nullptr;
  int count = 0, selected = 0;
  bool visible = false, dirty = true;
  unsigned int texture = 0;
  int width = 640, height = 512;
  void bind(const MenuItem *list, int n) {
    items = list;
    count = n;
  }
  void move(int delta);
  void adjust(int delta);
  void activate();
  bool render(); // Requires the mod GL context; returns true if the texture is ready.
};
