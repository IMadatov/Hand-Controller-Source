#pragma once

#include <array>
#include <vector>

#include "hc/anchors.hpp"
#include "hc/geometry.hpp"

namespace hc {

// A single decoded palm detection, in the detector's own input-square space:
// normalized 0..1 against the 192x192 square the palm detector ran on (NOT
// the original camera frame). Mapping this back through the letterbox that
// produced the 192x192 square is explicitly Task 8's job, not this file's --
// do not add letterbox parameters here.
struct PalmDetection {
  Rect box;
  std::array<Point2, 7> keypoints;
  float score;
};

// Mirrors MediaPipe's TensorsToDetectionsCalculatorOptions plus
// NonMaxSuppressionCalculatorOptions, values pinned in cpp/docs/models.md.
struct PalmDecodeOptions {
  int input_w, input_h;
  float x_scale, y_scale, w_scale, h_scale;
  float min_score;
  float score_clipping_thresh;
  float nms_iou;

  // true: raw box columns 0-3 read as (x_center, y_center, w, h) and each
  // keypoint pair reads as (x, y). false: both are transposed, i.e. columns
  // read as (y_center, x_center, h, w) and each keypoint pair reads as
  // (y, x). Pinned true for the palm model (models.md: reverse_output_order:
  // true). See MediaPipe's TensorsToDetectionsCalculator for the semantics.
  bool reverse_output_order;

  // Caller-supplied cap on the number of returned detections, applied AFTER
  // NMS. This is NOT a model parameter -- models.md records no
  // max_num_detections for this calculator, and the two-hand cap is the
  // tracker's policy (Tasks 9-10), not the decoder's. Any value <= 0 means
  // unlimited, so the decoder never silently truncates by default.
  int max_results;
};

// The values from models.md: x_scale/y_scale/w_scale/h_scale all 192.0,
// min_score 0.5, score_clipping_thresh 100.0, nms_iou (min_suppression_
// threshold) 0.3, reverse_output_order true, max_results -1 (unlimited;
// see PalmDecodeOptions::max_results).
PalmDecodeOptions PalmDecodeDefaults();

// Decodes the palm detector's two raw output tensors into a sorted
// (descending score, ties broken by ascending anchor index for determinism)
// list of palm detections, in the detector's 192x192 input-square space,
// normalized 0..1.
//
// Preconditions (undefined behavior if violated):
//   - raw_boxes points to anchors.size() * 18 contiguous floats: per anchor,
//     4 box values followed by 7 keypoints of 2 values each, in raw
//     (pre-scale, anchor-relative) form, per models.md.
//   - raw_scores points to anchors.size() contiguous floats: one raw
//     (pre-sigmoid) logit per anchor.
//   - raw_boxes and raw_scores are raw, unowned pointers whose valid length
//     is implied by anchors.size(); the caller owns and must keep them alive
//     for the duration of this call.
//   - anchors[i] corresponds to row i of both raw_boxes and raw_scores
//     (pinned, tested ordering from Task 5's GenerateAnchors).
//
// Decode: for anchor i, divide raw box/keypoint offsets by the x/y/w/h scale
// factors, multiply by the anchor's own w/h (identity for this model, since
// fixed_anchor_size makes every anchor's w == h == 1.0 -- done anyway so a
// future model without fixed_anchor_size doesn't silently break), and add
// the anchor center. The raw score logit is clamped to
// +/-score_clipping_thresh before applying sigmoid exactly once; the
// resulting probability is thresholded against min_score.
//
// Suppression is MediaPipe's WEIGHTED non-max suppression (NOT plain NMS):
// walking surviving candidates in descending score order, each cluster is
// every remaining candidate (including the current top one) whose IoU with
// the top candidate is strictly greater than nms_iou. The emitted detection
// keeps the top candidate's score and keypoints unaveraged, but replaces its
// box with the score-weighted mean of the cluster's boxes
// (sum(score_i * coord_i) / sum(score_i), computed independently per
// coordinate). Every cluster member is consumed before continuing.
//
// Out-param overload: writes into `out` instead of returning a fresh
// vector. `out` is cleared first, so its prior contents are irrelevant and
// only its CAPACITY is reused. This exists for the per-frame tracker path,
// where the detector may run on every single frame (hand_tracker_onnx.cpp,
// ruling T10-D) and a returned-by-value vector is a heap allocation per
// frame. The by-value overload is a thin wrapper around this one, so the
// two cannot drift.
void DecodePalmDetections(const float* raw_boxes, const float* raw_scores,
                          const std::vector<Anchor>& anchors,
                          const PalmDecodeOptions& o,
                          std::vector<PalmDetection>& out);

std::vector<PalmDetection> DecodePalmDetections(const float* raw_boxes,
                                                 const float* raw_scores,
                                                 const std::vector<Anchor>& anchors,
                                                 const PalmDecodeOptions& o);

}  // namespace hc
