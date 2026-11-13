#include "hc/palm_decode.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "hc/anchors.hpp"
#include "hc/geometry.hpp"

using hc::Anchor;
using hc::DecodePalmDetections;
using hc::PalmDecodeDefaults;
using hc::PalmDecodeOptions;
using hc::PalmDetection;

namespace {

// Builds a flat raw_boxes array of anchors.size() * 18 floats, all zero,
// ready for individual rows to be overwritten by each test.
std::vector<float> ZeroBoxes(std::size_t n) {
  return std::vector<float>(n * 18, 0.0f);
}

// Builds a flat raw_scores array of anchors.size() floats, all at a
// strongly-below-threshold logit (-4 => sigmoid ~0.018), ready for
// individual entries to be overwritten.
std::vector<float> LowScores(std::size_t n) {
  return std::vector<float>(n, -4.0f);
}

PalmDecodeOptions DefaultsForSyntheticAnchors() {
  // models.md pins x/y/w/h scale at 192.0 regardless of anchor count, so the
  // scale factors are identical for a small synthetic anchor set.
  return PalmDecodeDefaults();
}

}  // namespace

TEST(PalmDecode, SinglePeakDecodesToExpectedBox) {
  // One anchor at (0.5, 0.5), w=h=1.0 (fixed_anchor_size). Box offsets are
  // asymmetric (x != y, w != h) so a transposed read would land elsewhere:
  // raw columns [dx=10, dy=20, dw=30, dh=40], scale 192 on all four.
  //   cx = 0.5 + 10/192 = 0.552083333
  //   cy = 0.5 + 20/192 = 0.604166667
  //   w  = 30/192       = 0.15625
  //   h  = 40/192       = 0.208333333
  //   box.x = cx - w/2 = 0.473958333
  //   box.y = cy - h/2 = 0.5
  // score logit +4 => sigmoid(4) = 0.982013790...
  std::vector<Anchor> anchors = {Anchor{0.5f, 0.5f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  boxes[0] = 10.0f;
  boxes[1] = 20.0f;
  boxes[2] = 30.0f;
  boxes[3] = 40.0f;
  std::vector<float> scores = {4.0f};

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  ASSERT_EQ(dets.size(), 1u);
  EXPECT_NEAR(dets[0].box.x, 0.473958333f, 1e-5f);
  EXPECT_NEAR(dets[0].box.y, 0.5f, 1e-5f);
  EXPECT_NEAR(dets[0].box.w, 0.15625f, 1e-5f);
  EXPECT_NEAR(dets[0].box.h, 0.208333333f, 1e-5f);
  EXPECT_NEAR(dets[0].score, 0.982013790f, 1e-5f);
}

TEST(PalmDecode, ReverseOutputOrderFalseTransposesBoxAndKeypoints) {
  // Same raw row as SinglePeakDecodesToExpectedBox, but with
  // reverse_output_order == false: columns read as
  // (y_center, x_center, h, w), i.e. transposed from the true-branch
  // reading. So raw columns [col0=10, col1=20, col2=30, col3=40] decode as
  // y_offset=10, x_offset=20, h_offset=30, w_offset=40:
  //   cx = 0.5 + 20/192 = 0.604166667
  //   cy = 0.5 + 10/192 = 0.552083333
  //   w  = 40/192       = 0.208333333
  //   h  = 30/192       = 0.15625
  //   box.x = cx - w/2 = 0.5 (exactly)
  //   box.y = cy - h/2 = 0.473958333
  // Keypoint pair i reads (y, x) instead of (x, y): keypoint 0's raw pair
  // (col4=5, col5=1) decodes to y = 0.5 + 5/192, x = 0.5 + 1/192.
  std::vector<Anchor> anchors = {Anchor{0.5f, 0.5f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  boxes[0] = 10.0f;
  boxes[1] = 20.0f;
  boxes[2] = 30.0f;
  boxes[3] = 40.0f;
  boxes[4] = 5.0f;
  boxes[5] = 1.0f;
  std::vector<float> scores = {4.0f};

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  o.reverse_output_order = false;
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  ASSERT_EQ(dets.size(), 1u);
  EXPECT_NEAR(dets[0].box.x, 0.5f, 1e-5f);
  EXPECT_NEAR(dets[0].box.y, 0.473958333f, 1e-5f);
  EXPECT_NEAR(dets[0].box.w, 0.208333333f, 1e-5f);
  EXPECT_NEAR(dets[0].box.h, 0.15625f, 1e-5f);
  EXPECT_NEAR(dets[0].keypoints[0].x, 0.5f + 1.0f / 192.0f, 1e-5f);
  EXPECT_NEAR(dets[0].keypoints[0].y, 0.5f + 5.0f / 192.0f, 1e-5f);
}

TEST(PalmDecode, BelowThresholdYieldsNothing) {
  // All logits at -4 (sigmoid ~0.018) with min_score from models.md (0.5):
  // every candidate is below threshold, so decode must return empty.
  std::vector<Anchor> anchors = {Anchor{0.3f, 0.3f, 1.0f, 1.0f},
                                  Anchor{0.7f, 0.7f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  std::vector<float> scores = LowScores(anchors.size());

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  EXPECT_TRUE(dets.empty());
}

TEST(PalmDecode, OverlappingDetectionsCollapseToOne) {
  // Two adjacent anchors, both above threshold, with heavily overlapping
  // boxes (IoU 0.908 > nms_iou 0.3):
  //   anchor A = (0.5, 0.5), raw box offsets [0,0,40,40], score logit 3.0
  //     => box A = (0.395833333, 0.395833333, 0.208333333, 0.208333333)
  //     => score A = sigmoid(3.0) = 0.952574127
  //   anchor B = (0.51, 0.5), raw box offsets [0,0,40,40], score logit 1.0
  //     => box B = (0.405833333, 0.395833333, 0.208333333, 0.208333333)
  //     => score B = sigmoid(1.0) = 0.731058579
  //   IoU(A, B) = 0.908396947 (> 0.3, so both fall in one cluster)
  // Weighted NMS keeps A's score/keypoints (the higher-scoring candidate)
  // but replaces the box with the score-weighted mean of A and B's boxes:
  //   total = scoreA + scoreB = 1.683632706
  //   avg.x = (scoreA*0.395833333 + scoreB*0.405833333) / total
  //         = 0.400175483
  //   avg.y = 0.395833333 (both inputs equal on y)
  //   avg.w = avg.h = 0.208333333 (both inputs equal)
  std::vector<Anchor> anchors = {Anchor{0.5f, 0.5f, 1.0f, 1.0f},
                                  Anchor{0.51f, 0.5f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  boxes[0 * 18 + 2] = 40.0f;  // anchor A dw
  boxes[0 * 18 + 3] = 40.0f;  // anchor A dh
  boxes[1 * 18 + 2] = 40.0f;  // anchor B dw
  boxes[1 * 18 + 3] = 40.0f;  // anchor B dh
  std::vector<float> scores = {3.0f, 1.0f};

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  ASSERT_EQ(dets.size(), 1u);
  // The score and keypoints must be the higher-scoring candidate's,
  // unaveraged.
  EXPECT_NEAR(dets[0].score, 0.952574127f, 1e-5f);
  // The box must be the score-weighted mean, NOT candidate A's own box.
  EXPECT_NEAR(dets[0].box.x, 0.400175483f, 1e-5f);
  EXPECT_NEAR(dets[0].box.y, 0.395833333f, 1e-5f);
  EXPECT_NEAR(dets[0].box.w, 0.208333333f, 1e-5f);
  EXPECT_NEAR(dets[0].box.h, 0.208333333f, 1e-5f);
  EXPECT_FALSE(std::abs(dets[0].box.x - 0.395833333f) < 1e-6f)
      << "box must be the weighted average, not candidate A's own box";
}

TEST(PalmDecode, DisjointDetectionsBothSurvive) {
  // Two above-threshold anchors far apart (IoU == 0):
  //   anchor A = (0.2, 0.2), raw box offsets [0,0,20,20], score logit 2.0
  //     => box A = (0.147916667, 0.147916667, 0.104166667, 0.104166667)
  //     => score A = sigmoid(2.0) = 0.880797078
  //   anchor B = (0.8, 0.8), raw box offsets [0,0,20,20], score logit 3.5
  //     => box B = (0.747916667, 0.747916667, 0.104166667, 0.104166667)
  //     => score B = sigmoid(3.5) = 0.970687769
  // Both must survive, highest score (B) first.
  std::vector<Anchor> anchors = {Anchor{0.2f, 0.2f, 1.0f, 1.0f},
                                  Anchor{0.8f, 0.8f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  boxes[0 * 18 + 2] = 20.0f;
  boxes[0 * 18 + 3] = 20.0f;
  boxes[1 * 18 + 2] = 20.0f;
  boxes[1 * 18 + 3] = 20.0f;
  std::vector<float> scores = {2.0f, 3.5f};

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  ASSERT_EQ(dets.size(), 2u);
  EXPECT_NEAR(dets[0].score, 0.970687769f, 1e-5f);
  EXPECT_NEAR(dets[0].box.x, 0.747916667f, 1e-5f);
  EXPECT_NEAR(dets[1].score, 0.880797078f, 1e-5f);
  EXPECT_NEAR(dets[1].box.x, 0.147916667f, 1e-5f);
}

TEST(PalmDecode, ScoreClippingPreventsSigmoidOverflow) {
  // A logit of 1e9 must clip to +score_clipping_thresh (100.0) before
  // sigmoid, producing score 1.0 (within float precision) and no NaN --
  // without clipping, 1/(1+exp(-1e9)) risks NaN or an FP exception depending
  // on the libm path.
  std::vector<Anchor> anchors = {Anchor{0.5f, 0.5f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  std::vector<float> scores = {1e9f};

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  ASSERT_EQ(dets.size(), 1u);
  EXPECT_FALSE(std::isnan(dets[0].score));
  EXPECT_NEAR(dets[0].score, 1.0f, 1e-6f);
}

TEST(PalmDecode, SevenKeypointsAreDecodedRelativeToAnchor) {
  // anchor at (0.5, 0.5); 7 keypoints, each with distinct (x_off, y_off) raw
  // pairs at columns 4..17, scale 192 on all:
  //   keypoint i -> x = 0.5 + x_off/192, y = 0.5 + y_off/192
  std::vector<Anchor> anchors = {Anchor{0.5f, 0.5f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  const float kp_offsets[7][2] = {{5, 1},  {2, 8},  {9, 3},  {4, 12},
                                   {15, 6}, {7, 18}, {11, 2}};
  for (int k = 0; k < 7; ++k) {
    boxes[4 + k * 2] = kp_offsets[k][0];
    boxes[4 + k * 2 + 1] = kp_offsets[k][1];
  }
  std::vector<float> scores = {4.0f};

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  ASSERT_EQ(dets.size(), 1u);
  for (int k = 0; k < 7; ++k) {
    const float expected_x = 0.5f + kp_offsets[k][0] / 192.0f;
    const float expected_y = 0.5f + kp_offsets[k][1] / 192.0f;
    EXPECT_NEAR(dets[0].keypoints[k].x, expected_x, 1e-5f)
        << "keypoint " << k;
    EXPECT_NEAR(dets[0].keypoints[k].y, expected_y, 1e-5f)
        << "keypoint " << k;
  }
}

TEST(PalmDecode, RespectsMaxResults) {
  // Three disjoint above-threshold anchors (far apart, no NMS suppression),
  // but max_results caps the decoder's output to 2. The cap is ours (ruling
  // T6-C), applied after NMS, and must keep the highest-scoring detections.
  std::vector<Anchor> anchors = {Anchor{0.1f, 0.1f, 1.0f, 1.0f},
                                  Anchor{0.5f, 0.5f, 1.0f, 1.0f},
                                  Anchor{0.9f, 0.9f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  for (int i = 0; i < 3; ++i) {
    boxes[i * 18 + 2] = 10.0f;
    boxes[i * 18 + 3] = 10.0f;
  }
  std::vector<float> scores = {1.0f, 3.0f, 2.0f};

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  o.max_results = 2;
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  ASSERT_EQ(dets.size(), 2u);
  // Must keep the two highest scores (logits 3.0 and 2.0), dropping 1.0.
  EXPECT_GT(dets[0].score, dets[1].score);
  EXPECT_NEAR(dets[0].score, 1.0f / (1.0f + std::exp(-3.0f)), 1e-5f);
  EXPECT_NEAR(dets[1].score, 1.0f / (1.0f + std::exp(-2.0f)), 1e-5f);
}

TEST(PalmDecode, DefaultsMatchModelsMd) {
  // Pins PalmDecodeDefaults() against the values recorded in
  // cpp/docs/models.md (lines 115-215): x/y/w/h scale 192.0, min_score 0.5,
  // score_clipping_thresh 100.0, nms_iou 0.3, reverse_output_order true
  // (ruling T6-A), and max_results -1 meaning unlimited (ruling T6-C, a
  // decoder policy, not a model parameter).
  PalmDecodeOptions o = PalmDecodeDefaults();
  EXPECT_FLOAT_EQ(o.x_scale, 192.0f);
  EXPECT_FLOAT_EQ(o.y_scale, 192.0f);
  EXPECT_FLOAT_EQ(o.w_scale, 192.0f);
  EXPECT_FLOAT_EQ(o.h_scale, 192.0f);
  EXPECT_FLOAT_EQ(o.min_score, 0.5f);
  EXPECT_FLOAT_EQ(o.score_clipping_thresh, 100.0f);
  EXPECT_FLOAT_EQ(o.nms_iou, 0.3f);
  EXPECT_TRUE(o.reverse_output_order);
  EXPECT_LE(o.max_results, 0);
}
