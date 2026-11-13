#pragma once

#include <array>

#include "hc/geometry.hpp"
#include "hc/palm_decode.hpp"

namespace hc {

// Wraps an angle in radians into [-pi, pi). Matches MediaPipe's
// NormalizeRadians exactly (used by both ROI paths' rotation computation):
//   angle - 2*pi*floor((angle + pi) / (2*pi))
// which maps the upper boundary (angle == pi) down to -pi, since floor
// rounds down exactly at that boundary.
float NormalizeRadians(float angle_rad);

// Options for deriving a ROI from a single palm detection. Mirrors
// MediaPipe's palm_detection_detection_to_roi.pbtxt (models.md:362-387).
//
// The rotation axis runs from keypoint `rotation_kp_start` to keypoint
// `rotation_kp_end`, both indices into PalmDetection::keypoints (the palm
// detector's 7 keypoints), NOT the 21-point hand landmark topology.
//
// Preconditions (undefined behavior if violated -- these are checked by
// assert in debug builds only, and are never validated at runtime in a way
// that changes the return value):
//   - rotation_kp_start and rotation_kp_end are both in [0, 7).
struct PalmRoiOptions {
  float scale_x, scale_y, shift_x, shift_y;
  bool square_long;
  float target_angle_rad;
  int rotation_kp_start, rotation_kp_end;
};

// The values from models.md:365-387: rotation_kp_start 0 (wrist center),
// rotation_kp_end 2 (middle finger MCP), target_angle_rad pi/2, scale_x/
// scale_y 2.6, shift_x 0.0, shift_y -0.5, square_long true.
PalmRoiOptions DefaultPalmRoiOptions();

// One rotation-axis target point, expressed as a weighted average of full
// 21-point landmark indices. Weights across the active entries of a
// LandmarkRoiOptions::rotation_targets list must sum to 1.0.
struct WeightedLandmark {
  int index;
  float weight;
};

// Capacities for LandmarkRoiOptions' fixed-size storage. These exist so the
// options struct is trivially copyable and allocation-free: it sits in a
// per-hand, per-frame path (Task 8 derives a ROI for every tracked hand on
// every frame), where a heap allocation would show up directly in phase 1's
// per-stage timing report.
inline constexpr int kMaxRotationTargets = 3;
inline constexpr int kMaxBboxIndices = 21;

// Options for deriving a ROI from the previous frame's 21 hand landmarks.
// Mirrors MediaPipe's hand_landmark_landmarks_to_roi.pbtxt plus
// hand_landmarks_to_rect_calculator.cc (models.md:389-490).
//
// The rotation axis runs from landmark `origin_index` to the weighted
// average of the first `rotation_target_count` entries of
// `rotation_targets`. The bounding box is computed only over the landmarks
// named by the first `bbox_index_count` entries of `bbox_indices` (the
// 12-point partial subset -- fingertips 4/8/12/16/20 are excluded), not all
// 21 points.
//
// `rotation_targets` and `bbox_indices` are fixed-capacity arrays paired
// with explicit counts rather than std::vector, so constructing, copying and
// passing this struct allocates nothing. Entries past the corresponding
// count are ignored and may hold any value.
//
// Preconditions (undefined behavior if violated -- these are checked by
// assert in debug builds only, and are never validated at runtime in a way
// that changes the return value):
//   - origin_index is in [0, 21).
//   - rotation_target_count is in [0, kMaxRotationTargets], and every active
//     entry's `index` is in [0, 21).
//   - bbox_index_count is in [1, kMaxBboxIndices] -- it must be NON-ZERO,
//     because hc::BoundingBox is undefined for n == 0 -- and every active
//     entry is in [0, 21).
struct LandmarkRoiOptions {
  float scale_x, scale_y, shift_x, shift_y;
  bool square_long;
  float target_angle_rad;
  int origin_index;
  std::array<WeightedLandmark, kMaxRotationTargets> rotation_targets;
  int rotation_target_count;
  std::array<int, kMaxBboxIndices> bbox_indices;
  int bbox_index_count;
};

// The values from models.md:420-490: origin_index 0 (wrist), rotation_targets
// {{5, 0.25f}, {9, 0.5f}, {13, 0.25f}} (index/middle/ring MCP, nested-average
// weighting), bbox_indices {0,1,2,3,5,6,9,10,13,14,17,18} (the 12-point
// partial subset), target_angle_rad pi/2, scale_x/scale_y 2.0, shift_x 0.0,
// shift_y -0.1, square_long true.
LandmarkRoiOptions DefaultLandmarkRoiOptions();

// Shared preconditions for both functions below (undefined behavior if
// violated): `aspect` is frame width / frame height and must be > 0. An
// `aspect` of 0 propagates through atan2 and the rect transform as NaN.
//
// Both functions are allocation-free.
//
// Degenerate-input properties, observed and deliberately preserved rather
// than "fixed", because MediaPipe behaves identically:
//   - If the rotation axis has zero length (origin and the weighted target
//     coincide, or the two palm keypoints coincide), atan2(0, 0) == 0, so
//     the rotation comes out as the target angle itself (pi/2 for both
//     default option sets). This is arbitrary but matches MediaPipe; do not
//     substitute an invented "undefined axis" value.
//   - If all contributing points coincide, the bounding box has zero size
//     and the resulting ROI has zero w and h.

// Derives a ROI from a single palm detection. The rect's extent comes from
// `d.box` (NOT the convex hull of its keypoints); the keypoints are used
// only to compute the rotation. `aspect` is frame width / frame height, the
// same convention as hc::SquareLong and hc::TransformRect.
RotatedRect RoiFromPalm(const PalmDetection& d, float aspect,
                        const PalmRoiOptions& o);

// Derives a ROI from the previous frame's 21 hand landmarks. The rect's
// extent comes from the axis-aligned bounding box of the `bbox_indices`
// subset of `lm`, computed in unrotated normalized frame space; rotation is
// attached to the result afterward (MediaPipe's
// NormalizedLandmarkListToRect order). `aspect` is frame width / frame
// height.
RotatedRect RoiFromLandmarks(const std::array<Point2, 21>& lm, float aspect,
                             const LandmarkRoiOptions& o);

}  // namespace hc
