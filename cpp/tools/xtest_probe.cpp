// Empirical XTest probe. The plan calls this the single most dangerous fact
// in phase 2: XTest can report itself present and working, return success
// from every call, and move nothing. Asking the extension is not enough --
// the only trustworthy check reads the pointer position back.
//
// Uses dlopen rather than linking, which is also how the real sink will do
// it (the project forbids adding a build-time system dependency).
#include <dlfcn.h>
#include <cstdio>

typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID Window;

int main() {
  void* x11 = dlopen("libX11.so.6", RTLD_LAZY);
  void* xtst = dlopen("libXtst.so.6", RTLD_LAZY);
  if (!x11 || !xtst) {
    std::printf("dlopen failed: libX11=%p libXtst=%p\n", x11, xtst);
    return 2;
  }

  auto XOpenDisplay = (Display * (*)(const char*))dlsym(x11, "XOpenDisplay");
  auto XCloseDisplay = (int (*)(Display*))dlsym(x11, "XCloseDisplay");
  auto XFlush = (int (*)(Display*))dlsym(x11, "XFlush");
  auto XSync = (int (*)(Display*, int))dlsym(x11, "XSync");
  auto XDefaultRootWindow = (Window(*)(Display*))dlsym(x11, "XDefaultRootWindow");
  auto XQueryPointer =
      (int (*)(Display*, Window, Window*, Window*, int*, int*, int*, int*,
               unsigned int*))dlsym(x11, "XQueryPointer");

  auto XTestQueryExtension =
      (int (*)(Display*, int*, int*, int*, int*))dlsym(xtst, "XTestQueryExtension");
  auto XTestFakeMotionEvent =
      (int (*)(Display*, int, int, int, unsigned long))dlsym(xtst, "XTestFakeMotionEvent");

  if (!XOpenDisplay || !XQueryPointer || !XTestQueryExtension ||
      !XTestFakeMotionEvent) {
    std::printf("dlsym failed on one or more symbols\n");
    return 2;
  }

  Display* d = XOpenDisplay(nullptr);
  if (!d) {
    std::printf("XOpenDisplay(NULL) returned NULL -- no X display reachable\n");
    return 3;
  }

  int ev = 0, err = 0, major = 0, minor = 0;
  const int present = XTestQueryExtension(d, &ev, &err, &major, &minor);
  std::printf("XTestQueryExtension: present=%d version=%d.%d\n", present, major,
              minor);

  const Window root = XDefaultRootWindow(d);
  Window r = 0, c = 0;
  int rx = 0, ry = 0, wx = 0, wy = 0;
  unsigned int mask = 0;

  XQueryPointer(d, root, &r, &c, &rx, &ry, &wx, &wy, &mask);
  std::printf("pointer before: %d,%d\n", rx, ry);

  // Move somewhere unambiguous and read it back.
  const int target_x = rx + 137;
  const int target_y = ry + 91;
  const int rc = XTestFakeMotionEvent(d, -1, target_x, target_y, 0);
  XFlush(d);
  XSync(d, 0);
  std::printf("XTestFakeMotionEvent returned %d (asked for %d,%d)\n", rc,
              target_x, target_y);

  int ax = 0, ay = 0;
  XQueryPointer(d, root, &r, &c, &ax, &ay, &wx, &wy, &mask);
  std::printf("pointer after:  %d,%d\n", ax, ay);

  if (ax == target_x && ay == target_y) {
    std::printf("RESULT: XTest ACTUALLY MOVES THE CURSOR\n");
    // Put it back; a probe that leaves the cursor somewhere random is rude.
    XTestFakeMotionEvent(d, -1, rx, ry, 0);
    XFlush(d);
    XSync(d, 0);
    XCloseDisplay(d);
    return 0;
  }

  std::printf("RESULT: XTest SILENTLY DID NOTHING (returned success, no move)\n");
  XCloseDisplay(d);
  return 1;
}
