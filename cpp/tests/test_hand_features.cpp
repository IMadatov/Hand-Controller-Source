#include "hc/hand_features.hpp"

#include <cmath>

#include <gtest/gtest.h>

#include "hc/geometry.hpp"
#include "hc/hand_observation.hpp"

using hc::ComputeFeatures;
using hc::Handedness;
using hc::HandFeatures;
using hc::HandObservation;
using hc::Point2;

namespace {

// MediaPipe's 21-landmark indices, named so the poses below read as anatomy
// rather than as magic numbers.
enum Lm {
  kWrist = 0,
  kThumbCmc = 1, kThumbMcp = 2, kThumbIp = 3, kThumbTip = 4,
  kIndexMcp = 5, kIndexPip = 6, kIndexDip = 7, kIndexTip = 8,
  kMiddleMcp = 9, kMiddlePip = 10, kMiddleDip = 11, kMiddleTip = 12,
  kRingMcp = 13, kRingPip = 14, kRingDip = 15, kRingTip = 16,
  kPinkyMcp = 17, kPinkyPip = 18, kPinkyDip = 19, kPinkyTip = 20,
};

HandObservation Blank() {
  HandObservation o{};
  o.track_id = 1;
  o.handedness = Handedness::Right;
  o.handedness_score = 0.9f;
  o.presence = 1.0f;
  o.timestamp_us = 0;
  o.landmarks.fill(Point2{0.5f, 0.5f});
  return o;
}

// An upright open palm: wrist at the bottom, finger chains running upward so
// each tip sits well above its PIP, thumb splayed out to the side.
//
// `scale` multiplies the hand's size about the wrist, and `origin` places the
// wrist. Both exist so the scale-invariance test can build the same pose at
// two sizes without duplicating the layout.
HandObservation OpenPalm(float scale = 1.0f, Point2 origin = {0.5f, 0.8f}) {
  HandObservation o = Blank();
  const auto put = [&](int i, float dx, float dy) {
    o.landmarks[i] = Point2{origin.x + dx * scale, origin.y + dy * scale};
  };
  put(kWrist, 0.0f, 0.0f);

  // Thumb: out to the left and slightly up, i.e. across the palm's axis.
  put(kThumbCmc, -0.03f, -0.02f);
  put(kThumbMcp, -0.07f, -0.05f);
  put(kThumbIp,  -0.11f, -0.07f);
  put(kThumbTip, -0.15f, -0.09f);

  // Four fingers: columns rising from the knuckles. dy is negative upward.
  const float col[4] = {-0.05f, -0.01f, 0.03f, 0.07f};
  const int mcp[4] = {kIndexMcp, kMiddleMcp, kRingMcp, kPinkyMcp};
  const int pip[4] = {kIndexPip, kMiddlePip, kRingPip, kPinkyPip};
  const int dip[4] = {kIndexDip, kMiddleDip, kRingDip, kPinkyDip};
  const int tip[4] = {kIndexTip, kMiddleTip, kRingTip, kPinkyTip};
  for (int f = 0; f < 4; ++f) {
    put(mcp[f], col[f], -0.10f);
    put(pip[f], col[f], -0.18f);
    put(dip[f], col[f], -0.24f);
    put(tip[f], col[f], -0.30f);
  }
  return o;
}

// A closed fist: same knuckles, but every fingertip curled back down below
// its own PIP, and the thumb folded across the palm.
HandObservation Fist() {
  HandObservation o = OpenPalm();
  const int pip[4] = {kIndexPip, kMiddlePip, kRingPip, kPinkyPip};
  const int dip[4] = {kIndexDip, kMiddleDip, kRingDip, kPinkyDip};
  const int tip[4] = {kIndexTip, kMiddleTip, kRingTip, kPinkyTip};
  for (int f = 0; f < 4; ++f) {
    const Point2 p = o.landmarks[pip[f]];
    o.landmarks[dip[f]] = Point2{p.x, p.y + 0.03f};
    o.landmarks[tip[f]] = Point2{p.x, p.y + 0.06f};  // Below the PIP.
  }
  o.landmarks[kThumbTip] = Point2{0.52f, 0.72f};  // Folded across the palm.
  o.landmarks[kThumbIp] = Point2{0.50f, 0.74f};
  return o;
}

}  // namespace

TEST(HandFeatures, OpenPalmHasAllFingersExtended) {
  const HandFeatures f = ComputeFeatures(OpenPalm());
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(f.extended[i]) << "finger " << i << " should be extended";
  }
}

TEST(HandFeatures, ClosedFistHasNoneExtended) {
  const HandFeatures f = ComputeFeatures(Fist());
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(f.extended[i]) << "finger " << i << " should be curled";
  }
}

// The thumb folds ACROSS the palm rather than curling into it, so the
// tip-above-PIP test that works for the other four reports it wrong. If the
// thumb shares the finger rule, this fails.
TEST(HandFeatures, ThumbUsesASeparateAxisFromTheFingers) {
  HandObservation o = OpenPalm();
  // Fold only the thumb, leaving all four fingers extended. The thumb tip
  // stays ABOVE its own PIP in y -- it has moved sideways, not down.
  o.landmarks[kThumbTip] = Point2{0.52f, 0.72f};
  o.landmarks[kThumbIp] = Point2{0.50f, 0.74f};

  const HandFeatures f = ComputeFeatures(o);
  EXPECT_FALSE(f.extended[0]) << "a thumb folded across the palm is not extended";
  for (int i = 1; i < 5; ++i) EXPECT_TRUE(f.extended[i]);
}

// A raw tip-to-tip distance shrinks as the hand moves away from the camera,
// which would make every pinch threshold depth-dependent -- the resolution
// locking this rebuild exists to remove.
TEST(HandFeatures, PinchDistanceIsScaleInvariant) {
  const HandFeatures near = ComputeFeatures(OpenPalm(1.0f));
  const HandFeatures far = ComputeFeatures(OpenPalm(0.5f));
  EXPECT_NEAR(near.pinch_distance, far.pinch_distance, 0.02f);
}

TEST(HandFeatures, PinchDistanceFallsWhenTheFingersClose) {
  HandObservation o = OpenPalm();
  const HandFeatures apart = ComputeFeatures(o);

  o.landmarks[kThumbTip] = o.landmarks[kIndexTip];  // Touching.
  const HandFeatures together = ComputeFeatures(o);

  EXPECT_LT(together.pinch_distance, apart.pinch_distance);
  EXPECT_NEAR(together.pinch_distance, 0.0f, 1e-4f);
}

TEST(HandFeatures, PalmCenterLiesInsideThePalm) {
  const HandFeatures f = ComputeFeatures(OpenPalm());
  // Between the wrist and the knuckles, horizontally among the fingers.
  EXPECT_GT(f.palm_center.y, 0.60f);
  EXPECT_LT(f.palm_center.y, 0.82f);
  EXPECT_GT(f.palm_center.x, 0.40f);
  EXPECT_LT(f.palm_center.x, 0.60f);
}

TEST(HandFeatures, PalmSizeGrowsWithTheHand) {
  EXPECT_GT(ComputeFeatures(OpenPalm(1.0f)).palm_size,
            ComputeFeatures(OpenPalm(0.5f)).palm_size);
}

// All 21 landmarks identical is degenerate but reachable: it is what a
// collapsed or garbage detection looks like. palm_size is a divisor, so a
// zero here is a division by zero in every scale-invariant quantity.
TEST(HandFeatures, DegenerateHandDoesNotDivideByZero) {
  HandObservation o = Blank();  // Every landmark at the same point.
  const HandFeatures f = ComputeFeatures(o);

  EXPECT_GT(f.palm_size, 0.0f);
  EXPECT_TRUE(std::isfinite(f.pinch_distance));
  EXPECT_TRUE(std::isfinite(f.palm_center.x));
  EXPECT_TRUE(std::isfinite(f.palm_center.y));
}

// Phase 1's tracker clamps nothing: landmarks outside 0..1 reach here when a
// hand is partly out of frame.
TEST(HandFeatures, OutOfRangeLandmarksStayFinite) {
  HandObservation o = OpenPalm();
  o.landmarks[kIndexTip] = Point2{1.8f, -0.4f};

  const HandFeatures f = ComputeFeatures(o);
  EXPECT_TRUE(std::isfinite(f.pinch_distance));
  EXPECT_TRUE(std::isfinite(f.palm_size));
  EXPECT_GT(f.palm_size, 0.0f);
}

// The features describe a pose, not an identity. Phase 1 measured handedness
// confidence as low as 0.575, so nothing downstream may key off it here.
TEST(HandFeatures, HandednessDoesNotChangeTheFeatures) {
  HandObservation right = OpenPalm();
  HandObservation left = OpenPalm();
  left.handedness = Handedness::Left;
  left.track_id = 99;

  const HandFeatures a = ComputeFeatures(right);
  const HandFeatures b = ComputeFeatures(left);

  EXPECT_EQ(a.extended, b.extended);
  EXPECT_FLOAT_EQ(a.pinch_distance, b.pinch_distance);
  EXPECT_FLOAT_EQ(a.palm_size, b.palm_size);
}
