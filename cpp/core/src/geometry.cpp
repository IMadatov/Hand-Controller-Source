#include "hc/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace hc {

Rect BoundingBox(const Point2* pts, std::size_t n) {
  float min_x = pts[0].x;
  float max_x = pts[0].x;
  float min_y = pts[0].y;
  float max_y = pts[0].y;
  for (std::size_t i = 1; i < n; ++i) {
    min_x = std::min(min_x, pts[i].x);
    max_x = std::max(max_x, pts[i].x);
    min_y = std::min(min_y, pts[i].y);
    max_y = std::max(max_y, pts[i].y);
  }
  return Rect{min_x, min_y, max_x - min_x, max_y - min_y};
}

float IoU(const Rect& a, const Rect& b) {
  const float ix0 = std::max(a.x, b.x);
  const float iy0 = std::max(a.y, b.y);
  const float ix1 = std::min(a.x + a.w, b.x + b.w);
  const float iy1 = std::min(a.y + a.h, b.y + b.h);

  const float iw = std::max(0.0f, ix1 - ix0);
  const float ih = std::max(0.0f, iy1 - iy0);
  const float intersection = iw * ih;
  if (intersection <= 0.0f) {
    return 0.0f;
  }

  const float area_a = a.w * a.h;
  const float area_b = b.w * b.h;
  const float union_area = area_a + area_b - intersection;
  if (union_area <= 0.0f) {
    return 0.0f;
  }
  return intersection / union_area;
}

RotatedRect SquareLong(const RotatedRect& r, float aspect) {
  // r.w is normalized to frame width, r.h to frame height; convert both to
  // the same "pixel-comparable" unit before deciding which side is longer.
  // Using frame height as the common unit: w_px = w * aspect, h_px = h.
  const float w_px = r.w * aspect;
  const float h_px = r.h;

  RotatedRect out = r;
  if (w_px >= h_px) {
    // Width is the longer side in pixels; grow h (normalized to height) to
    // match it.
    out.h = w_px;
  } else {
    // Height is the longer side in pixels; grow w (normalized to width) to
    // match it.
    out.w = h_px / aspect;
  }
  return out;
}

RotatedRect TransformRect(const RotatedRect& r, float aspect,
                          const RectTransform& t) {
  RotatedRect out = r;

  // Step 1: shift the center along the rect's own rotated axes, using the
  // *input* w/h -- before squaring or scaling, matching MediaPipe's
  // TransformNormalizedRect. Derived from its pixel-space x_shift/y_shift
  // by dividing through by image_width / image_height respectively and
  // substituting aspect = image_width / image_height, which leaves a
  // 1/aspect on the cross-axis term of dx and an aspect on that of dy.
  //
  // At rotation == 0 this degenerates exactly to MediaPipe's fast path
  // (dx = w * shift_x, dy = h * shift_y), since cos(0) and sin(0) are
  // exactly 1 and 0.
  const float cos_r = std::cos(r.rotation_rad);
  const float sin_r = std::sin(r.rotation_rad);

  const float dx = r.w * t.shift_x * cos_r - (r.h * t.shift_y * sin_r) / aspect;
  const float dy = aspect * r.w * t.shift_x * sin_r + r.h * t.shift_y * cos_r;

  out.center.x = r.center.x + dx;
  out.center.y = r.center.y + dy;

  // Step 2: square on the longer side, still before scaling.
  float w = r.w;
  float h = r.h;
  if (t.square_long) {
    const RotatedRect squared = SquareLong(r, aspect);
    w = squared.w;
    h = squared.h;
  }

  // Step 3: scale last.
  out.w = w * t.scale_x;
  out.h = h * t.scale_y;

  return out;
}

}  // namespace hc
