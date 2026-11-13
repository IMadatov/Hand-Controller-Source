#include "hc/palm_decode.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hc {

namespace {

constexpr int kNumCoords = 18;
constexpr int kNumKeypoints = 7;

float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// A decode candidate before NMS: the decoded detection plus the anchor index
// it came from, kept so sorting can tie-break deterministically.
struct Candidate {
  PalmDetection detection;
  std::size_t anchor_index;
};

}  // namespace

PalmDecodeOptions PalmDecodeDefaults() {
  PalmDecodeOptions o{};
  o.input_w = 192;
  o.input_h = 192;
  o.x_scale = 192.0f;
  o.y_scale = 192.0f;
  o.w_scale = 192.0f;
  o.h_scale = 192.0f;
  o.min_score = 0.5f;
  o.score_clipping_thresh = 100.0f;
  o.nms_iou = 0.3f;
  o.reverse_output_order = true;
  o.max_results = -1;
  return o;
}

std::vector<PalmDetection> DecodePalmDetections(const float* raw_boxes,
                                                 const float* raw_scores,
                                                 const std::vector<Anchor>& anchors,
                                                 const PalmDecodeOptions& o) {
  std::vector<Candidate> candidates;
  candidates.reserve(anchors.size());

  for (std::size_t i = 0; i < anchors.size(); ++i) {
    // Clamp the raw logit to +/-score_clipping_thresh before sigmoid, so a
    // huge magnitude (e.g. 1e9) can never reach exp() and produce a NaN or
    // an FP exception.
    const float clipped = std::max(-o.score_clipping_thresh,
                                    std::min(o.score_clipping_thresh, raw_scores[i]));
    const float score = Sigmoid(clipped);
    if (score < o.min_score) {
      continue;
    }

    const Anchor& anchor = anchors[i];
    const float* row = raw_boxes + i * kNumCoords;

    // Columns 0-3: box. true => (x, y, w, h); false => (y, x, h, w).
    float raw_x_off, raw_y_off, raw_w_off, raw_h_off;
    if (o.reverse_output_order) {
      raw_x_off = row[0];
      raw_y_off = row[1];
      raw_w_off = row[2];
      raw_h_off = row[3];
    } else {
      raw_y_off = row[0];
      raw_x_off = row[1];
      raw_h_off = row[2];
      raw_w_off = row[3];
    }

    // Multiply by the anchor's own w/h -- identity for this model
    // (fixed_anchor_size => anchor.w == anchor.h == 1.0), but written
    // explicitly so a future model without fixed_anchor_size doesn't
    // silently break.
    const float cx = anchor.x_center + (raw_x_off / o.x_scale) * anchor.w;
    const float cy = anchor.y_center + (raw_y_off / o.y_scale) * anchor.h;
    const float w = (raw_w_off / o.w_scale) * anchor.w;
    const float h = (raw_h_off / o.h_scale) * anchor.h;

    PalmDetection det{};
    det.box = Rect{cx - w / 2.0f, cy - h / 2.0f, w, h};
    det.score = score;

    for (int k = 0; k < kNumKeypoints; ++k) {
      const float a_raw = row[4 + k * 2];
      const float b_raw = row[4 + k * 2 + 1];
      float kx_off, ky_off;
      if (o.reverse_output_order) {
        kx_off = a_raw;
        ky_off = b_raw;
      } else {
        ky_off = a_raw;
        kx_off = b_raw;
      }
      const float kx = anchor.x_center + (kx_off / o.x_scale) * anchor.w;
      const float ky = anchor.y_center + (ky_off / o.y_scale) * anchor.h;
      det.keypoints[static_cast<std::size_t>(k)] = Point2{kx, ky};
    }

    candidates.push_back(Candidate{det, i});
  }

  // Deterministic descending-score sort: ties broken by ascending anchor
  // index so the order never varies between standard-library
  // implementations.
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              if (a.detection.score != b.detection.score) {
                return a.detection.score > b.detection.score;
              }
              return a.anchor_index < b.anchor_index;
            });

  // MediaPipe's WeightedNonMaxSuppression: walk remaining candidates in
  // descending score order; the cluster around the current top candidate is
  // every remaining candidate (including itself) whose IoU with it is
  // strictly greater than nms_iou. The emitted detection keeps the top
  // candidate's score and keypoints unaveraged, but its box becomes the
  // score-weighted mean of the cluster's boxes. Every cluster member is
  // removed before continuing.
  std::vector<bool> consumed(candidates.size(), false);
  std::vector<PalmDetection> results;
  results.reserve(candidates.size());

  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (consumed[i]) {
      continue;
    }
    const Candidate& top = candidates[i];

    double weighted_x = 0.0, weighted_y = 0.0, weighted_w = 0.0, weighted_h = 0.0;
    double weight_sum = 0.0;

    for (std::size_t j = i; j < candidates.size(); ++j) {
      if (consumed[j]) {
        continue;
      }
      const Candidate& cand = candidates[j];
      const float iou = IoU(top.detection.box, cand.detection.box);
      if (iou > o.nms_iou) {
        const double s = static_cast<double>(cand.detection.score);
        weighted_x += s * cand.detection.box.x;
        weighted_y += s * cand.detection.box.y;
        weighted_w += s * cand.detection.box.w;
        weighted_h += s * cand.detection.box.h;
        weight_sum += s;
        consumed[j] = true;
      }
    }

    PalmDetection emitted = top.detection;
    if (weight_sum > 0.0) {
      emitted.box.x = static_cast<float>(weighted_x / weight_sum);
      emitted.box.y = static_cast<float>(weighted_y / weight_sum);
      emitted.box.w = static_cast<float>(weighted_w / weight_sum);
      emitted.box.h = static_cast<float>(weighted_h / weight_sum);
    }
    results.push_back(emitted);

    if (o.max_results > 0 &&
        results.size() >= static_cast<std::size_t>(o.max_results)) {
      break;
    }
  }

  return results;
}

}  // namespace hc
