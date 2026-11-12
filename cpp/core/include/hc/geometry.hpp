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

// The parameters of a rect transformation, mirroring the options of
// MediaPipe's RectTransformationCalculator.
struct RectTransform {
  float scale_x, scale_y, shift_x, shift_y;
  bool square_long;
};

// Returns `r` transformed by `t`, where `aspect` is frame width / frame
// height (the same convention SquareLong uses).
//
// Applies MediaPipe's TransformNormalizedRect order exactly:
//   1. shift the center along the rect's own rotated axes, by fractions
//      (shift_x, shift_y) of the rect's *input* w and h -- before any
//      squaring or scaling;
//   2. if `square_long`, square the rect on its longer side in pixels;
//   3. scale w and h by (scale_x, scale_y).
//
// The order matters: the shift reads the pre-scale, pre-square dimensions,
// which is why this is a single function rather than a composition of
// smaller ones -- squaring overwrites the very w/h the shift must be
// computed from. `rotation_rad` is carried through unchanged.
RotatedRect TransformRect(const RotatedRect& r, float aspect,
                          const RectTransform& t);

}  // namespace hc
