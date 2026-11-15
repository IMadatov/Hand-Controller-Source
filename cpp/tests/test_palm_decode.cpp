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
  //
  // Keypoints are deliberately DISTINCT and NONZERO on both candidates, with
  // opposite signs, so that for every keypoint the top candidate's value,
  // the lower candidate's value, and their score-weighted mean are three
  // visibly different numbers (minimum separation 0.0136 at keypoint 0,
  // growing to 0.095 at keypoint 6 -- all far above the 1e-5 tolerance).
  // Ruling T6-B pins that MediaPipe averages ONLY the bounding box, so the
  // emitted keypoints must equal candidate A's decoded keypoints exactly:
  // not B's, and not the average. With all-zero keypoint rows this would be
  // unfalsifiable, since averaging zero with zero is zero.
  //   A keypoint k raw offsets = (+6*(k+1), +3*(k+1)) about anchor (0.5, 0.5)
  //   B keypoint k raw offsets = (-6*(k+1), -3*(k+1)) about anchor (0.51, 0.5)
  std::vector<Anchor> anchors = {Anchor{0.5f, 0.5f, 1.0f, 1.0f},
                                  Anchor{0.51f, 0.5f, 1.0f, 1.0f}};
  std::vector<float> boxes = ZeroBoxes(anchors.size());
  boxes[0 * 18 + 2] = 40.0f;  // anchor A dw
  boxes[0 * 18 + 3] = 40.0f;  // anchor A dh
  boxes[1 * 18 + 2] = 40.0f;  // anchor B dw
  boxes[1 * 18 + 3] = 40.0f;  // anchor B dh
  for (int k = 0; k < 7; ++k) {
    const float mag_x = 6.0f * static_cast<float>(k + 1);
    const float mag_y = 3.0f * static_cast<float>(k + 1);
    boxes[0 * 18 + 4 + k * 2] = mag_x;       // anchor A keypoint k dx
    boxes[0 * 18 + 4 + k * 2 + 1] = mag_y;   // anchor A keypoint k dy
    boxes[1 * 18 + 4 + k * 2] = -mag_x;      // anchor B keypoint k dx
    boxes[1 * 18 + 4 + k * 2 + 1] = -mag_y;  // anchor B keypoint k dy
  }
  std::vector<float> scores = {3.0f, 1.0f};

  PalmDecodeOptions o = DefaultsForSyntheticAnchors();
  std::vector<PalmDetection> dets =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o);

  ASSERT_EQ(dets.size(), 1u);
  // The score must be the higher-scoring candidate's, unaveraged.
  EXPECT_NEAR(dets[0].score, 0.952574127f, 1e-5f);
  // The box must be the score-weighted mean, NOT candidate A's own box.
  EXPECT_NEAR(dets[0].box.x, 0.400175483f, 1e-5f);
  EXPECT_NEAR(dets[0].box.y, 0.395833333f, 1e-5f);
  EXPECT_NEAR(dets[0].box.w, 0.208333333f, 1e-5f);
  EXPECT_NEAR(dets[0].box.h, 0.208333333f, 1e-5f);
  EXPECT_FALSE(std::abs(dets[0].box.x - 0.395833333f) < 1e-6f)
      << "box must be the weighted average, not candidate A's own box";

  // The keypoints must be candidate A's exactly -- averaging the box must
  // not have dragged the keypoints toward B.
  const float kScoreA = 0.952574127f;
  const float kScoreB = 0.731058579f;
  for (int k = 0; k < 7; ++k) {
    const float mag_x = 6.0f * static_cast<float>(k + 1);
    const float mag_y = 3.0f * static_cast<float>(k + 1);
    const float a_x = 0.5f + mag_x / 192.0f;
    const float a_y = 0.5f + mag_y / 192.0f;
    const float b_x = 0.51f - mag_x / 192.0f;
    const float b_y = 0.5f - mag_y / 192.0f;
    const float avg_x = (kScoreA * a_x + kScoreB * b_x) / (kScoreA + kScoreB);
    const float avg_y = (kScoreA * a_y + kScoreB * b_y) / (kScoreA + kScoreB);

    // Sanity-check that this keypoint actually discriminates: the three
    // candidate outcomes must be mutually distinct by well over the
    // assertion tolerance, so passing cannot be an accident of them
    // coinciding.
    ASSERT_GT(std::abs(a_x - b_x), 1e-3f) << "keypoint " << k;
    ASSERT_GT(std::abs(a_x - avg_x), 1e-3f) << "keypoint " << k;
    ASSERT_GT(std::abs(b_x - avg_x), 1e-3f) << "keypoint " << k;
    ASSERT_GT(std::abs(a_y - b_y), 1e-3f) << "keypoint " << k;
    ASSERT_GT(std::abs(a_y - avg_y), 1e-3f) << "keypoint " << k;
    ASSERT_GT(std::abs(b_y - avg_y), 1e-3f) << "keypoint " << k;

    EXPECT_NEAR(dets[0].keypoints[static_cast<std::size_t>(k)].x, a_x, 1e-5f)
        << "keypoint " << k
        << " x must stay the top candidate's, unaveraged (B's would be "
        << b_x << ", the weighted mean would be " << avg_x << ")";
    EXPECT_NEAR(dets[0].keypoints[static_cast<std::size_t>(k)].y, a_y, 1e-5f)
        << "keypoint " << k
        << " y must stay the top candidate's, unaveraged (B's would be "
        << b_y << ", the weighted mean would be " << avg_y << ")";
  }
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

TEST(PalmDecode, NonPositiveMaxResultsMeansUnlimited) {
  // Ruling T6-C: any max_results <= 0 means unlimited, so the decoder never
  // silently truncates. Build 12 mutually disjoint above-threshold
  // detections -- more than any plausible cap (and far more than the
  // tracker's eventual two-hand policy) -- on a 4x3 grid at
  // x in {0.1, 0.3, 0.5, 0.7} and y in {0.15, 0.5, 0.85}, each with raw box
  // offsets [0,0,10,10] so every box is 10/192 = 0.052083333 square.
  // Spacing (0.2 in x, 0.35 in y) vastly exceeds the box size, so the
  // maximum pairwise IoU is exactly 0 -- NMS cannot merge or suppress
  // anything, which isolates the cap as the only thing under test.
  // Logits are 1.0 + 0.1*i for i in [0, 12), so the smallest is
  // sigmoid(1.0) = 0.731 (comfortably above min_score 0.5) and all 12 are
  // distinct.
  std::vector<Anchor> anchors;
  const float xs[4] = {0.1f, 0.3f, 0.5f, 0.7f};
  const float ys[3] = {0.15f, 0.5f, 0.85f};
  for (int yi = 0; yi < 3; ++yi) {
    for (int xi = 0; xi < 4; ++xi) {
      anchors.push_back(Anchor{xs[xi], ys[yi], 1.0f, 1.0f});
    }
  }
  ASSERT_EQ(anchors.size(), 12u);

  std::vector<float> boxes = ZeroBoxes(anchors.size());
  std::vector<float> scores(anchors.size());
  for (std::size_t i = 0; i < anchors.size(); ++i) {
    boxes[i * 18 + 2] = 10.0f;
    boxes[i * 18 + 3] = 10.0f;
    scores[i] = 1.0f + 0.1f * static_cast<float>(i);
  }

  // -1 (the PalmDecodeDefaults() value) must return every detection.
  PalmDecodeOptions o_neg = DefaultsForSyntheticAnchors();
  o_neg.max_results = -1;
  std::vector<PalmDetection> dets_neg =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o_neg);
  EXPECT_EQ(dets_neg.size(), anchors.size())
      << "max_results == -1 must mean unlimited, not truncated";

  // 0 must behave identically -- both non-positive values are the same rule.
  PalmDecodeOptions o_zero = DefaultsForSyntheticAnchors();
  o_zero.max_results = 0;
  std::vector<PalmDetection> dets_zero =
      DecodePalmDetections(boxes.data(), scores.data(), anchors, o_zero);
  EXPECT_EQ(dets_zero.size(), anchors.size())
      << "max_results == 0 must mean unlimited, not zero results";

  // And the defaults themselves, untouched, must also return everything --
  // pinning that the shipped default does not truncate.
  std::vector<PalmDetection> dets_default = DecodePalmDetections(
      boxes.data(), scores.data(), anchors, DefaultsForSyntheticAnchors());
  EXPECT_EQ(dets_default.size(), anchors.size())
      << "PalmDecodeDefaults() must not truncate";

  // Still sorted descending, so "unlimited" did not also mean "unordered".
  for (std::size_t i = 1; i < dets_neg.size(); ++i) {
    EXPECT_GE(dets_neg[i - 1].score, dets_neg[i].score)
        << "detection " << i << " breaks descending score order";
  }
  // The highest logit (1.0 + 0.1*11 = 2.1) must lead.
  EXPECT_NEAR(dets_neg.front().score, 1.0f / (1.0f + std::exp(-2.1f)), 1e-5f);
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
