#pragma once

#include <cstddef>

namespace hc {

// A 2D point normalized to the full frame: (0,0) is the top-left corner and
// (1,1) is the bottom-right corner, regardless of the frame's pixel size.
struct Point2 {
  float x, y;
};

// An axis-aligned rectangle, normalized to the full frame in the same sense
// as Point2. (x, y) is the top-left corner; w and h are width and height.
struct Rect {
  float x, y, w, h;
};

// A rotated rectangle. `center` is normalized to the full frame. `w` is
// normalized to frame width and `h` is normalized to frame height, so the two
// are only comparable after accounting for the frame's aspect ratio.
// `rotation_rad` is the rotation of the rectangle's own axes about `center`,
// in radians.
struct RotatedRect {
  Point2 center;
  float w, h;
  float rotation_rad;
};

// Returns the smallest axis-aligned rectangle containing all `n` points in
// `pts`. Behavior is undefined if n == 0.
Rect BoundingBox(const Point2* pts, std::size_t n);

// Returns the intersection-over-union of two axis-aligned rectangles, in
// [0, 1]. Disjoint rectangles yield 0.
float IoU(const Rect& a, const Rect& b);

// Returns a copy of `r` with the shorter side (measured in pixels, using
// `aspect` = frame width / frame height to convert the normalized w/h of `r`
// into comparable pixel units) grown to match the longer side. The rectangle
// stays centered on `r.center` and keeps `r.rotation_rad`.
RotatedRect SquareLong(const RotatedRect& r, float aspect);

// Returns a copy of `r` scaled by (scale_x, scale_y) about its own center,
// then shifted along its own rotated axes by (shift_x, shift_y), where the
// shifts are fractions of the (post-scale) rect's own w and h. The rectangle
// keeps `r.rotation_rad`.
RotatedRect ExpandAndShift(const RotatedRect& r, float scale_x, float scale_y,
                            float shift_x, float shift_y);

}  // namespace hc
