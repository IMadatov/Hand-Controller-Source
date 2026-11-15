#include "hc/roi.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>

namespace hc {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kNumLandmarks = 21;
constexpr int kNumPalmKeypoints = 7;
}  // namespace

float NormalizeRadians(float angle_rad) {
  // Matches MediaPipe's NormalizeRadians: wraps into [-pi, pi). The floor
  // maps the upper boundary (angle == pi) down to -pi.
  return angle_rad - 2.0f * kPi * std::floor((angle_rad + kPi) / (2.0f * kPi));
}

PalmRoiOptions DefaultPalmRoiOptions() {
  PalmRoiOptions o{};
  o.scale_x = 2.6f;
  o.scale_y = 2.6f;
  o.shift_x = 0.0f;
  o.shift_y = -0.5f;
  o.square_long = true;
  o.target_angle_rad = kPi * 0.5f;
  o.rotation_kp_start = 0;
  o.rotation_kp_end = 2;
  return o;
}

LandmarkRoiOptions DefaultLandmarkRoiOptions() {
  LandmarkRoiOptions o{};
  o.scale_x = 2.0f;
  o.scale_y = 2.0f;
  o.shift_x = 0.0f;
  o.shift_y = -0.1f;
  o.square_long = true;
  o.target_angle_rad = kPi * 0.5f;
  o.origin_index = 0;
  o.rotation_targets = {WeightedLandmark{5, 0.25f}, WeightedLandmark{9, 0.5f},
                        WeightedLandmark{13, 0.25f}};
  o.rotation_target_count = 3;
  o.bbox_indices = {0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};
  o.bbox_index_count = 12;
  return o;
}

namespace {

// Implements MediaPipe's ComputeRotation (models.md:400-417): target angle
// minus the atan2 of the (negated-dy, dx) vector from (x0,y0) to (x1,y1),
// evaluated in pixel-comparable units via the aspect multiplier on the x
// term, then normalized into [-pi, pi).
float ComputeRotation(float x0, float y0, float x1, float y1, float aspect,
                       float target_angle_rad) {
  const float dx = (x1 - x0) * aspect;
  const float dy = -(y1 - y0);
  return NormalizeRadians(target_angle_rad - std::atan2(dy, dx));
}

}  // namespace

RotatedRect RoiFromPalm(const PalmDetection& d, float aspect,
                        const PalmRoiOptions& o) {
  assert(aspect > 0.0f);
  assert(o.rotation_kp_start >= 0 && o.rotation_kp_start < kNumPalmKeypoints);
  assert(o.rotation_kp_end >= 0 && o.rotation_kp_end < kNumPalmKeypoints);

  const Point2& p0 = d.keypoints[static_cast<std::size_t>(o.rotation_kp_start)];
  const Point2& p1 = d.keypoints[static_cast<std::size_t>(o.rotation_kp_end)];
  const float rotation =
      ComputeRotation(p0.x, p0.y, p1.x, p1.y, aspect, o.target_angle_rad);

  // Ruling T7-E: the rect's extent comes from d.box, NOT the keypoint hull.
  const RotatedRect base{
      Point2{d.box.x + d.box.w * 0.5f, d.box.y + d.box.h * 0.5f},
      d.box.w,
      d.box.h,
      rotation,
  };

  const RectTransform t{o.scale_x, o.scale_y, o.shift_x, o.shift_y,
                         o.square_long};
  return TransformRect(base, aspect, t);
}

RotatedRect RoiFromLandmarks(const std::array<Point2, 21>& lm, float aspect,
                             const LandmarkRoiOptions& o) {
  assert(aspect > 0.0f);
  assert(o.origin_index >= 0 && o.origin_index < kNumLandmarks);
  assert(o.rotation_target_count >= 0 &&
         o.rotation_target_count <= kMaxRotationTargets);
  // BoundingBox is undefined for n == 0, so the subset must be non-empty.
  assert(o.bbox_index_count >= 1 && o.bbox_index_count <= kMaxBboxIndices);

  const Point2& p0 = lm[static_cast<std::size_t>(o.origin_index)];

  float x1 = 0.0f;
  float y1 = 0.0f;
  for (int i = 0; i < o.rotation_target_count; ++i) {
    const WeightedLandmark& wl = o.rotation_targets[static_cast<std::size_t>(i)];
    assert(wl.index >= 0 && wl.index < kNumLandmarks);
    const Point2& p = lm[static_cast<std::size_t>(wl.index)];
    x1 += p.x * wl.weight;
    y1 += p.y * wl.weight;
  }

  const float rotation =
      ComputeRotation(p0.x, p0.y, x1, y1, aspect, o.target_angle_rad);

  // Ruling T7-D: bounding box in UNROTATED normalized frame space, over
  // only the 12-point partial subset (fingertips excluded), rotation
  // attached afterward. Stack storage, so this path never allocates.
  std::array<Point2, kMaxBboxIndices> subset;
  for (int i = 0; i < o.bbox_index_count; ++i) {
    const int idx = o.bbox_indices[static_cast<std::size_t>(i)];
    assert(idx >= 0 && idx < kNumLandmarks);
    subset[static_cast<std::size_t>(i)] = lm[static_cast<std::size_t>(idx)];
  }
  const Rect box =
      BoundingBox(subset.data(), static_cast<std::size_t>(o.bbox_index_count));

  const RotatedRect base{
      Point2{box.x + box.w * 0.5f, box.y + box.h * 0.5f},
      box.w,
      box.h,
      rotation,
  };

  const RectTransform t{o.scale_x, o.scale_y, o.shift_x, o.shift_y,
                         o.square_long};
  return TransformRect(base, aspect, t);
}

}  // namespace hc
