#pragma once

#include <array>
#include <cstddef>
#include <vector>

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
// 21-point landmark indices. Weights across all entries in a
// LandmarkRoiOptions::rotation_targets list must sum to 1.0.
struct WeightedLandmark {
  int index;
  float weight;
};

// Options for deriving a ROI from the previous frame's 21 hand landmarks.
// Mirrors MediaPipe's hand_landmark_landmarks_to_roi.pbtxt plus
// hand_landmarks_to_rect_calculator.cc (models.md:389-490).
//
// The rotation axis runs from landmark `origin_index` to the weighted
// average of `rotation_targets`. The bounding box is computed only over the
// landmarks listed in `bbox_indices` (the 12-point partial subset --
// fingertips 4/8/12/16/20 are excluded), not all 21 points.
struct LandmarkRoiOptions {
  float scale_x, scale_y, shift_x, shift_y;
  bool square_long;
  float target_angle_rad;
  int origin_index;
  std::vector<WeightedLandmark> rotation_targets;
  std::vector<int> bbox_indices;
};

// The values from models.md:420-490: origin_index 0 (wrist), rotation_targets
// {{5, 0.25f}, {9, 0.5f}, {13, 0.25f}} (index/middle/ring MCP, nested-average
// weighting), bbox_indices {0,1,2,3,5,6,9,10,13,14,17,18} (the 12-point
// partial subset), target_angle_rad pi/2, scale_x/scale_y 2.0, shift_x 0.0,
// shift_y -0.1, square_long true.
LandmarkRoiOptions DefaultLandmarkRoiOptions();

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
