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

RotatedRect ExpandAndShift(const RotatedRect& r, float scale_x, float scale_y,
                            float shift_x, float shift_y) {
  RotatedRect out = r;
  out.w = r.w * scale_x;
  out.h = r.h * scale_y;

  // The rect's own rotated axes: x' points along rotation_rad, y' is x'
  // rotated -90 degrees (matching image coordinates, where y grows downward
  // and a positive rotation turns x' toward +y').
  const float cos_r = std::cos(r.rotation_rad);
  const float sin_r = std::sin(r.rotation_rad);

  const float dx_local = shift_x * out.w;
  const float dy_local = shift_y * out.h;

  out.center.x = r.center.x + dx_local * cos_r + dy_local * sin_r;
  out.center.y = r.center.y + dx_local * sin_r - dy_local * cos_r;

  return out;
}

}  // namespace hc
